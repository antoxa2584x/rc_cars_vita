/*
 * net.h -- MULTIPLAYER, on the machine's own Wi-Fi.
 *
 * Two to four Vitas on one network race each other. This file is the whole
 * transport and the whole protocol; `mainmenu.c' draws the two screens over it
 * (dlgMULTIPLAYER and the dlgWAITPLAYERS_* lobby) and `main.c' hands the race
 * state in and out. Nothing above it knows a socket exists.
 *
 * WHY UDP OVER INFRASTRUCTURE WI-FI AND NOT AD HOC. The Vita has both:
 * `sceNetAdhoc*' (PSP-style peer-to-peer, no access point) and `sceNet', which
 * is a BSD socket API over whatever the console is associated with. This port
 * uses the second, for three reasons and one of them is decisive:
 *
 *   - it is the one the EMULATOR implements. Vita3K maps sceNet onto the host's
 *     own sockets, so a build can actually be tested -- two instances on one PC,
 *     or an instance and a real Vita on the same Wi-Fi. `sceNetAdhoc' is not
 *     emulated at all, so an ad-hoc port would be code nobody could run;
 *   - it is what the ORIGINAL does. `dlgNETCREATE_OPTS' has `Protocol:',
 *     `Port nmb:', `Machine IP address:' and `Max lag:' -- it is an IP game with
 *     a server list, not a link-layer one;
 *   - and one socket serves both jobs. The same UDP port carries the host's
 *     broadcast announce (which is the server list) and the unicast game
 *     traffic.
 *
 * Ad hoc is therefore a gap and not a decision against it; `known-issues.md'.
 *
 * THE SHAPE. One host, up to NET_MAX peers including it. The host owns the
 * SETTINGS (track, laps, car restrictions) and the roster; each peer owns its
 * OWN car, parts, paint and ready flag. Nobody simulates anybody else:
 *
 *   discovery   the host broadcasts NET_ANNOUNCE twice a second. A peer sitting
 *               on the Join page collects those into a server list. Broadcast is
 *               the only packet that is not addressed to somebody.
 *   lobby       a peer sends NET_JOIN; the host answers NET_WELCOME with its
 *               slot and then sends the whole NET_LOBBY -- settings, roster and
 *               every peer's ADDRESS -- four times a second. A peer sends
 *               NET_PEER when its own car, parts, paint or ready flag moves.
 *   the start   the host sends NET_START; everyone loads the track and the car
 *               and drives. The grid slot IS the roster slot, so two peers
 *               cannot be handed the same one -- and the app makes that mean
 *               something: `ai_grid' reads the track's own authored starting
 *               grid out of its `.aip' first samples and slot k is seated on
 *               it. Before that every peer was placed on the track's single
 *               player marker, i.e. inside each other.
 *   the race    every peer sends its own NET_STATE to every other peer directly,
 *               `sendFrameRate' times a second (network.ini's own key, floored
 *               at NET_STATE_FLOOR). No relay: the addresses came out of
 *               NET_LOBBY, and a relay through the host doubles the latency of
 *               the half of the traffic that matters most. A receiver draws the
 *               car BETWEEN the last two states rather than on the last one --
 *               NET_INTERP_MIN, and it is the whole of why a remote car no
 *               longer moves in steps.
 *   the end     the host sends NET_FINISH with the finishing order.
 *
 * AND THE RACE PACKET IS `ai_sample', WHICH IS THE ONE THING HERE THAT IS NOT
 * INVENTED. The AI opponents are recorded laps replayed through
 * `rb_car_set_state' (ai.h), and a recorded sample is exactly what a remote
 * car's state is: position, orientation, speed, six suspension lengths and the
 * steer angle, quantised at scales that were measured against the 151 shipped
 * recordings. So a remote player is REPLAYED the way an opponent is -- same
 * struct, same unpack, same rig -- and its springs compress because that path
 * already does all of it. 36 bytes a car a packet.
 *
 * ITS WHEELS DO NOT TURN BY THEMSELVES, and this file said for a while that
 * they did. The rolling angle is NOT in the 32-float ODE state and therefore
 * not in the sample: `rb_wheel.spin' is integrated every frame by
 * `rb_wheel_spin_update', which a recorded opponent reaches through `ai_step'
 * and a remote player -- for which `ai_step' is not called at all -- reached
 * through nothing at all. `ai_remote_spin' is that missing call; see ai.h.
 *
 * NOBODY IS AUTHORITATIVE OVER PHYSICS, and that is deliberate. This is a 2003
 * arcade racer with a `Max lag' setting and a `sendFrameRate' of 5 -- a state
 * broadcast with dead reckoning, not lockstep. Each peer simulates its own car
 * and displays everyone else's; a collision between two remote cars is seen
 * differently on the two machines and nothing tries to reconcile it. Rollback
 * would mean determinism this port does not have (`traps.md' on the physics
 * order-preservation rule) and is not what the original did either.
 *
 * THE HOST HAS A SOCKET TOO. `net.c' has one backend for the Vita (`sceNet')
 * and one for POSIX, so `net_test' runs two REAL peers over the loopback and
 * exercises the protocol rather than a mock of it.
 */
#ifndef NET_H
#define NET_H

#include "ai.h"          /* ai_sample -- the race packet's own payload */
#include "player.h"      /* PL_NAME -- a peer's name is a profile's name */

/* HOW MANY CARS ON THE GRID. The engine's own `Max players:' enum
   (dlgNETCREATE_OPTS) is what this stands in for, and AI_MAX_OPPONENTS is the
   ceiling that matters: a remote peer occupies one of the opponent slots the
   field would otherwise fill, so four peers means the host plus three remotes
   and AI_MAX_OPPONENTS is five. */
#define NET_MAX 4

/* THE PORT. 3658 is the first of the range Sony documents for user UDP on the
   Vita and is what most homebrew uses; the announce carries the sender's own
   port, so a second instance on one machine binds 3659 and is still found. */
#define NET_PORT       3658
#define NET_PORT_TRIES 4

/* How many servers the Join page will list. */
#define NET_MAX_SERVERS 8

/* Seconds. A peer unheard from for this long is dropped -- the engine's own
   `Max lag' is the same idea, and its own enum offers 1..5 seconds. */
#define NET_TIMEOUT 5.0f

/* HOW FAR A REMOTE CAR MAY BE CARRIED FORWARD on its own reported velocity,
   seconds. At `sendFrameRate' 5 a packet is up to 200 ms old, so the cap has to
   be at least that; 0.5 s is two and a half missed packets, past which a peer
   that has gone quiet freezes rather than flying off the level -- and
   NET_TIMEOUT then takes its slot away. */
#define NET_DR_MAX 0.5f

/* THE RENDER DELAY, and it is the other half of NET_DR_MAX.
 *
 * Dead reckoning alone can only carry the POSITION -- there is no angular
 * velocity in `ai_sample' to extrapolate an orientation from, and none in the
 * suspension either -- so with the state rate at `sendFrameRate' every remote
 * car SNAPPED its yaw, its springs and its steer once per packet and held them
 * still in between. That is what "the other car runs at low fps" is: not a
 * frame rate at all, a pose updated five times a second and drawn sixty.
 *
 * So a remote car is drawn one packet BEHIND the newest state and interpolated
 * between the two that bracket it -- position, orientation, suspension, steer
 * and velocity, all of them, with nothing guessed. The delay is the sender's
 * OWN cadence, which it reports in `ai_sample.dt' (the field the recordings use
 * for exactly this and which the wire left at zero until now), so the receiver
 * does not have to measure it against its own jitter.
 *
 * WHEN THE BUFFER RUNS DRY -- a packet late or lost -- the newest sample is
 * dead-reckoned forward as before and capped at NET_DR_MAX, which is continuous
 * with the interpolation at the moment it takes over. So a clean link is smooth
 * and a lossy one degrades into what this file did before.
 *
 * The clamps: never delay by less than one send at the fastest rate the slider
 * offers, and never by more than the dead-reckoning cap, past which a peer is
 * being carried by guesswork anyway. */
#define NET_INTERP_MIN (1.0f / 50.0f)
#define NET_INTERP_MAX NET_DR_MAX

/* THE FLOOR UNDER `sendFrameRate'.
 *
 * network.ini ships `Values 1 50 5' -- a slider from 1 to 50 with 5 as the
 * default, and 5 is a 2003 number: a 36-byte car state five times a second was
 * sized for a modem. On the console's own Wi-Fi one state packet is 48 bytes of
 * payload and 76 on the wire, so three peers at 20 Hz is 4.6 KB/s each way --
 * which is nothing, and it is what makes the render delay above cheap (50 ms,
 * not 200). The KEY still wins when it asks for more; this only stops it asking
 * for less than a modern link can trivially carry. */
#define NET_STATE_FLOOR 20.0f

/* The host's announce, and the lobby's own refresh. */
#define NET_ANNOUNCE_HZ 2.0f
#define NET_LOBBY_HZ    4.0f

/* THE PROTOCOL'S OWN VERSION, in every header. Two builds that disagree about
   the packet layout must not half-work: a peer whose version does not match is
   ignored, so an old build simply does not see a new one's game. */
#define NET_VERSION 2

/* The message log's own depth -- the panel on the game's own lobby screenshot
   is about four lines of the wide font. */
#define NET_LOG_LINES 6
#define NET_LOG_CHARS 64

/* What this machine is doing. */
typedef enum {
    NET_OFF = 0,        /* no socket, nothing running */
    NET_BROWSING,       /* listening for announces -- the Join page */
    NET_HOSTING,        /* announcing, and the lobby is ours */
    NET_JOINING,        /* a NET_JOIN is out, waiting for NET_WELCOME */
    NET_JOINED,         /* in somebody's lobby */
    NET_RACING          /* the race is on */
} net_mode;

/* Why the session ended, for the dialog that says so. */
typedef enum {
    NET_ERR_NONE = 0,
    NET_ERR_NO_SOCKET,  /* sceNet would not give us one */
    NET_ERR_TIMEOUT,    /* the host went away -- 20233 */
    NET_ERR_KICKED,     /* 20236 */
    NET_ERR_FULL,       /* the game was full when we asked */
    NET_ERR_VERSION     /* their build is not ours */
} net_err;

/* One entry in the Join page's server list. */
typedef struct {
    char          name[PL_NAME]; /* the host's own player name */
    unsigned int  addr;          /* network byte order, as it arrived */
    unsigned short port;
    unsigned char players, maxplayers;
    unsigned char track, laps;
    float         seen;          /* net_time() when the last announce arrived */
} net_server;

/* One car in the game, host included. Slot 0 is always the host. */
typedef struct {
    int            used;
    char           name[PL_NAME];
    unsigned int   addr;         /* 0 for our own slot */
    unsigned short port;
    unsigned char  car;          /* 0..2 */
    unsigned char  up[3];        /* booster, engine, tyres -- garage.h's order */
    unsigned char  skin;
    unsigned char  ready;
    unsigned char  face;         /* which FacesSys portrait, for the table */
    /* THE RACE. TWO states, not one: `s' is the newest that arrived and `s0'
       the one before it, with `s_at'/`s0_at' saying when. `have' counts them,
       0, 1 or 2 -- at 2 the remote car is drawn BETWEEN them (net_remote_pose
       and NET_INTERP_MIN above), at 1 it is dead-reckoned off the only one
       there is, and at 0 there is nothing to draw.

       `seq' is the newest sample's own wrapping sequence, out of the header
       byte that used to be a pad. UDP may deliver two states out of order and
       the interpolator would then walk the car BACKWARDS for a packet; the
       compare in `take_state' is what stops it. */
    ai_sample      s, s0;
    float          s_at, s0_at;
    int            have;
    unsigned char  seq;
    unsigned char  lap, place, finished;
    float          best_lap, total;
    float          heard;        /* net_time() of the last packet from it */
} net_peer;

/* THE CAR RESTRICTIONS, dlgWAITPLAYERS_CARRESTR's own three enums. 0 is the
   engine's `Disable' (43206) and 1..4 are Default and Level 1..3 (43207..43210)
   -- so the value is "the highest upgrade level this car may wear, plus one",
   and 0 means the car is not allowed at all. */
#define NET_RESTR_OFF   0
#define NET_RESTR_N     5

typedef struct {
    unsigned char track;
    unsigned char laps;
    unsigned char restr[3];      /* per car, NET_RESTR_* */
} net_settings;

/* --------------------------------------------------------------- the module */

/* Bring the stack up and take a socket. Idempotent; -> 1 on success. Called the
   first time a multiplayer page is opened rather than at boot, because on real
   hardware `sceNetInit' wants a megabyte of pool and a race has better uses
   for it. */
int  net_open(void);

/* Give it all back. Safe with nothing open. */
void net_close(void);

net_mode net_mode_now(void);
net_err  net_error(void);
void     net_clear_error(void);

/* Seconds since net_open, which is the clock every timestamp here is in. */
float net_time(void);

/* Pump the socket and the timers. Once a frame, from wherever the app is --
   the lobby, the race, or the front end with a browse running. */
void net_step(float dt);

/* ------------------------------------------------------------- the lobby */

/* Start listening for announces (the Join page) or start announcing (Create).
   `me' names this machine and supplies its car; both take a copy. */
int  net_browse(void);
int  net_host(const char *name, int car, const unsigned char up[3], int skin,
              int face);

/* The server list, newest announce first. `n' is how many are live. */
int  net_n_servers(void);
const net_server *net_server_at(int i);

/* Ask to join the `i'th server. -> 1 if the request went out. */
int  net_join(int i, const char *name, int car, const unsigned char up[3],
              int skin, int face);

/* Leave, whichever end we are. The host tells everyone first. */
void net_leave(void);

/* The roster. `net_slot()' is our own, -1 when we are not in a game. */
int  net_n_peers(void);
const net_peer *net_peer_at(int i);
int  net_slot(void);
int  net_is_host(void);

/* The settings. Writing them is the HOST's alone -- the setters are no-ops on a
   client, which is what greys the two enums on its lobby page. */
const net_settings *net_settings_now(void);
void net_set_track(int track);
void net_set_laps(int laps);
void net_set_restr(int car, int value);

/* OUR OWN car, parts, paint and ready flag. Any of them moving is sent at once:
   a lobby that lags behind what its own screen says is worse than a packet. */
void net_set_me(int car, const unsigned char up[3], int skin);
void net_set_ready(int ready);

/* Whether `car' may be raced under the current restrictions, and the highest
   upgrade level it may wear (-1 for a car that is not allowed at all). */
int  net_car_allowed(int car);
int  net_max_upgrade(int car);

/* The host only: drop a peer. 0 on success, -1 for a bad slot, -2 for our own
   (42907, "You can't kick yourself"). */
int  net_kick(int slot);

/* Everyone ready and at least two cars? The host's Race button asks. */
int  net_can_start(void);

/* The host only: send NET_START and go to NET_RACING. */
void net_start(void);

/* Set by net_step when a NET_START arrives, cleared by reading it. The caller
   loads the track and the car and calls net_race_begin(). */
int  net_take_start(void);

/* ------------------------------------------------------------- the race */

/* The app has finished loading and the race is running. */
void net_race_begin(void);

/* Our own car's state, once a frame. `net_step' decides when to send it -- at
   `sendFrameRate' and not per frame. */
void net_send_state(const ai_sample *s, int lap, int place, float best_lap);

/* We crossed the line. Tells everybody once. */
void net_send_finished(float total, float best_lap);

/* Pose the remote car in `slot' into `out', INTERPOLATED to one send interval
   behind now -- or dead-reckoned forward from the newest sample when the buffer
   has run dry. Every field of the sample is carried, not just the position; see
   NET_INTERP_MIN. -> 0 when that slot has nothing to draw (empty, ourselves, or
   never heard from). */
int  net_remote_pose(int slot, ai_sample *out);

/* The race ended for everyone. */
int  net_race_over(void);

/* HOW LONG THE QUIETEST LIVE PEER HAS BEEN QUIET, seconds; 0 when we are not in
 * a game or every peer is current.
 *
 * WHAT IT IS FOR: the engine's own `msg_low_signal', message slot 0, which has
 * been packed and addressable since the message layer was read and had nothing
 * to raise it -- `known-issues.md' said so on the grounds that "this port has no
 * network", which stopped being true when net.c was written.
 *
 * THE THRESHOLD IS NOT A NEW NUMBER: past NET_DR_MAX the newest sample can no
 * longer be carried forward and a quiet peer FREEZES rather than flying off the
 * level (see NET_DR_MAX above). That is the moment the player can actually SEE
 * the link degrade -- a car standing still in the middle of the track -- so it
 * is the moment to say so, and NET_TIMEOUT takes the slot away 4.5 s later. */
float net_link_silence(void);

/* ------------------------------------------------------------- the log */

/* The lobby's message panel: the game's own 20200..20216 lines. `net.c' writes
   them; the caller draws them oldest first. */
int  net_n_log(void);
const char *net_log_at(int i);
void net_log_clear(void);

/* --------------------------------------------------- for the harness only */

/* Which port we actually bound, so a second peer in one process can be told
   where the first one is. */
int  net_port(void);

/* Join by address rather than out of the browse list -- the loopback path
   `net_test' uses, and the one an `editIPaddr' would drive if this port drew
   dlgNETJOIN_OPTS. `addr' is a dotted quad. */
int  net_join_addr(const char *addr, int port, const char *name, int car,
                   const unsigned char up[3], int skin, int face);

/* Force the clock forward without sleeping, so a harness can age a timeout. */
void net_test_advance(float seconds);

/* SEND THE ANNOUNCE HERE instead of to the broadcast address. Broadcast on
   127.0.0.1 reaches nobody, so this is the only way the DISCOVERY path can be
   driven on a host -- and it is what an `editIPaddr' direct connect would use
   if this port drew dlgNETJOIN_OPTS. `addr' is in network byte order; 0 puts
   the announce back on broadcast. */
void net_test_peer_hint(unsigned int addr, int port);

/* Which car a slot is driving, or -1. `main.c' needs it to pick the model for
   a remote car and nothing else on the roster does. */
int  net_slot_car(int slot);

/* How many packets have gone out and come in, by kind. For the log and for the
   harness; nothing in the model reads them. */
extern unsigned int net_sent, net_recv, net_dropped;

#endif /* NET_H */
