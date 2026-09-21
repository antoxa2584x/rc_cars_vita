/*
 * opts.h -- THE OPTIONS SCREEN'S STATE: the game's own sound page, and a
 * control map for a machine the game never ran on.
 *
 * Two of the original's dialogs, and both are shipped data rather than
 * invention:
 *
 *   dlgSOUND          Use sound / Sound quality / Sound volume / Background
 *                     sound / Background volume / Music style, a rule, and
 *                     Master volume. `Settings/dlgSOUND.ini' carries exactly
 *                     ONE rectangle -- the rule, (95,357) to (542,357) -- and
 *                     the rows are measured off the game's own screenshot of
 *                     the page against it; see mainmenu.c, MM_SND_*.
 *   dlgCONTROL_PLAYER Show controls for / Layout / the eight-action table /
 *                     the conflict line. Every rectangle of that one IS
 *                     shipped (dlg_data.h, DLG_CONTROL_PLAYER_*).
 *
 * THE EIGHT ACTIONS ARE THE ENGINE'S OWN EIGHT. `RCCars.exe' registers them
 * against the `[Player1]' config section at 0x004e4182 and the names are in
 * the binary: Boost, Left, Right, Forw, Back, Jump, Brake, Reset. The screen
 * draws them in the artists' order -- Gear, Reverse, Turn Left, Turn Right,
 * Boost, Jump, Stop, Reset (string table 10300..10307, with 10305 `Stop' and
 * 10306 `Jump' drawn the other way round, which is what the game's own
 * screenshot of the page has) -- and OPT_ACT_* below is that order.
 *
 * WHAT EACH ONE DRIVES IN THIS PORT, because the model has two pedals and not
 * four (physics.md, "The brake IS reverse"):
 *
 *   Gear        vehicle_input_t::throttle
 *   Reverse     vehicle_input_t::brake, which IS the reverse throttle -- the
 *               model has no separate reverse control and nothing here invents
 *               one
 *   Turn Left   steer -1  } only when the stick is off, or as well as it: a
 *   Turn Right  steer +1  } bound button is always honoured
 *   Boost       vehicle_input_t::boost
 *   Jump        rbcar_jump -- hop, or right an overturned car
 *   Stop        rbcar_hold, the engine's own DRIVE INHIBIT (phys+0x5784): no
 *               engine force either way and the wheels locked. This is the one
 *               reading under which Stop and Reverse are different actions in
 *               this model, and it is the mechanism the end of a race already
 *               uses -- see main.c. Unbound by default, because the port has
 *               never had a handbrake and a default would move a button a
 *               player already knows
 *   Reset       respawn_checkpoint() -- back to the last checkpoint crossed,
 *               which is what the port already does to a drowned car. Unbound
 *               by default for the same reason
 *
 * CONFLICTS ARE IMPOSSIBLE BY CONSTRUCTION rather than reported: binding a
 * button that another slot holds CLEARS it there (opts_bind). So the page's
 * `staticExplain' says the game's own "No conflicts" (10330) and always tells
 * the truth. The two strings beside it -- 10331/10332, "Conflicts with player
 * #1, key %s" -- are about the OTHER player's map, and this machine has one
 * pad and one player.
 *
 * START AND SELECT ARE NOT BINDABLE. START is the in-race menu and SELECT the
 * free-fly camera; a player who bound Boost to START would lose the only way
 * to pause, on a handheld with no keyboard to undo it with. That leaves the
 * ten in OPT_BTN, which is enough for eight actions with two slots each.
 *
 * DELIBERATELY FREE OF THE GL, AUDIO AND psp2 LAYERS, exactly as menu.c is:
 * this is a state machine over button bits and it is worth checking on the
 * host, where a binding that steals the wrong slot is a two-second test rather
 * than a redeploy. menu.h supplies the SCE_CTRL_* bits off-target.
 */
#ifndef OPTS_H
#define OPTS_H

#include "menu.h"       /* the SCE_CTRL_* bits, host-safe */

/* ------------------------------------------------------------ the actions */

/* In the shipped screen's own row order -- see the header comment. */
enum {
    OPT_GEAR = 0,       /* 10300 -- throttle */
    OPT_REVERSE,        /* 10301 -- brake, which is reverse */
    OPT_TURN_L,         /* 10302 */
    OPT_TURN_R,         /* 10303 */
    OPT_BOOST,          /* 10304 */
    OPT_JUMP,           /* 10306 */
    OPT_STOP,           /* 10305 -- the drive inhibit */
    OPT_RESET,          /* 10307 -- back to the last checkpoint */
    OPT_N_ACT
};

/* Two binding slots per action, which is the two columns the game's own page
   draws -- the second is `None' on every row of its screenshot. */
#define OPT_N_SLOT 2

/* THE BINDABLE BUTTONS. Ten of the Vita's sixteen bits: START and SELECT are
   the app's own (see the header), and L3/R3/L1/R1 do not exist on the machine
   this runs on. The ORDER is the order the bind list walks, so it is the order
   a player sees. */
#define OPT_N_BTN 10
extern const unsigned int OPT_BTN[OPT_N_BTN];
/* The button's own name, for the table cell. Not from the string table: the
   game names keyboard keys and this machine has none of them. */
const char *opts_btn_name(unsigned int bits);
/* Whether `bits' is one this page will accept, i.e. one of OPT_BTN. */
int opts_btn_ok(unsigned int bits);

/* --------------------------------------------------------------- the sound */

/* Every volume on the page is an integer notch, for the reason menu.h gives
   its own two: the state has to stay exactly comparable across a save and a
   restore, and MENU_VOL_STEPS is that count. Mirrored here rather than
   redefined, so the Sound volume row on THIS page and the one in the START
   menu cannot drift apart -- they write the same field. */
#define OPT_VOL_STEPS MENU_VOL_STEPS

/* 10205 `Sound quality', whose three values are the string table's own
   Low/Medium/High (25/26/27). WHAT IT CHANGES IS THE VOICE CAP, and that is
   the port's reading rather than a recovered one: the original picks a mixing
   rate and a DirectSound buffer count, neither of which exists here. mix.c
   runs 24 voices at one rate; capping them is the one knob in that mixer that
   costs measurably less work and is audible as fewer simultaneous sounds,
   which is what a quality setting is. Named at the point of use --
   opts_voices() -- and nowhere else. */
enum { OPT_Q_LOW = 0, OPT_Q_MEDIUM, OPT_Q_HIGH, OPT_N_QUALITY };

/* 10209 `Music style' -- 10210 Rock, 10211 Techno, 10212 Both, in that order.
 *
 * WHAT THE TWO STYLES ARE is the two playlists `Autoexec.gm' ships, and the
 * game's own file names them: `Deaduski' (cycle 1, the seven long tracks) and
 * `Master' (cycle 0, the eleven short ones).
 *
 * WHICH OF THE TWO IS WHICH IS NOT IN THE DATA -- nothing in `Autoexec.gm', the
 * file names, the track counts or the durations says so, and no tool here can
 * listen to an mp3. It was settled the only way it can be: the port shipped one
 * reading, somebody played both and reported them the wrong way round. So ROCK
 * is cycle 0 (`Mast_Track01..11') and TECHNO is cycle 1 (`Dead_Track01..07'),
 * which is the opposite of what this file first assumed. Recorded as evidence
 * and not as a guess, because the next person to read it will otherwise
 * re-derive the guess.
 *
 * Both is what the app has always done: the menu bank in the front end, the
 * race bank in a race. */
enum { OPT_MUSIC_ROCK = 0, OPT_MUSIC_TECHNO, OPT_MUSIC_BOTH, OPT_N_MUSIC };

/* MIRRORED FROM audio.h, not included -- opts.c is kept free of the audio
   layer the same way menu.c is, so the host harnesses link it without a mixer.
   main.c includes both and carries the compile-time check that they agree. */
#define OPT_GROUP_MENU 0
#define OPT_GROUP_RACE 1

/* ------------------------------------------------------------ the steering */

/* 10318 `Joystick sensitivity' and 10319 `Joystick deadzone', both 0..10.
 *
 * The defaults are the numbers the port already had hard-coded, so a fresh
 * settings file drives exactly as this app always has: OPT_SENS_DEF gives a
 * gain of 1.0 and OPT_DEAD_DEF gives main.c's own DEADZONE of 24 out of 128.
 * Both are named here rather than in main.c because the file that WRITES a
 * default and the file that reads it have to agree about what it means. */
#define OPT_SENS_STEPS 10
#define OPT_SENS_DEF    5       /* gain 0.5 + 0.1*s -> 1.0 */
#define OPT_DEAD_STEPS 10
#define OPT_DEAD_DEF    3       /* raw 8*d -> 24, main.c's old DEADZONE */

typedef struct {
    /* ---- dlgSOUND */
    int use_sound;      /* 10201 -- the master switch; 0 silences everything */
    int quality;        /* 10205 -- OPT_Q_* */
    int bg_sound;       /* 10207 -- the music on or off */
    int music_style;    /* 10209 -- OPT_MUSIC_* */
    int vol_master;     /* 10202 -- 0..OPT_VOL_STEPS, over both groups */

    /* ---- dlgCONTROL_PLAYER
       `stick' is 10317 `Use joystick': with it on the left stick steers, with
       it off only the two bound buttons do. The bound buttons are honoured
       EITHER WAY -- a player who turns the stick off must be able to steer,
       and a player who leaves it on loses nothing by the D-pad also working. */
    int stick;
    int sens;           /* 0..OPT_SENS_STEPS */
    int dead;           /* 0..OPT_DEAD_STEPS */
    unsigned int bind[OPT_N_ACT][OPT_N_SLOT];   /* SCE_CTRL_* bits, 0 = None */
} opts_t;

/* The defaults, which are the map this app shipped with: R throttle, L brake,
   the D-pad steering, CROSS boost, CIRCLE jump -- and Stop and Reset unbound,
   because the port had no button for either and inventing one here would move
   a control a player already knows. */
void opts_init(opts_t *o);
/* Just the map, for the Layout row's `Gamepad' value. */
void opts_default_binds(opts_t *o);
/* Whether the map has been changed away from the defaults, which is what the
   Layout row shows as `Custom' (10311) rather than `Gamepad' (10315). */
int  opts_is_custom(const opts_t *o);

/* Bind `bits' (one of OPT_BTN, or 0 for None) to a slot, CLEARING it wherever
   else it is held. Returns 1 if anything changed. Out-of-range is a no-op. */
int  opts_bind(opts_t *o, int act, int slot, unsigned int bits);

/* Is this action held, given a frame's SceCtrlData::buttons? */
int  opts_held(const opts_t *o, int act, unsigned int buttons);

/* The steering axis, -1..1: the stick through the deadzone and the
   sensitivity when `stick' is on, plus the two bound buttons, clamped. `raw'
   is SceCtrlData::lx, 0..255. */
float opts_steer(const opts_t *o, unsigned int buttons, unsigned char raw);

/* The two gains main.c hands sfx_volumes(), in 0..1. `sfx' and `music' are the
   START menu's own notches (menu_t::vol_sfx / vol_music), which is where those
   two rows' storage lives -- there is one Sound volume in this app and two
   screens that edit it. Use sound off is silence; Background sound off is the
   music alone. */
float opts_sfx_gain(const opts_t *o, int sfx_notches);
float opts_music_gain(const opts_t *o, int music_notches);

/* How many mixer voices the quality row allows -- see OPT_Q_*. */
int   opts_voices(const opts_t *o);

/* Which playlist group should be playing. `in_menu' is the app's own mode bit
   (front end or START menu up). -1 when there is to be no music at all, which
   is what Background sound off and Use sound off both mean. */
int   opts_music_group(const opts_t *o, int in_menu);

/* Clamp every field into range. Called by opts_init and by settings.c on the
   way in AND on the way out, for the reason settings.h gives: the file is what
   the NEXT launch takes as the range, and a hand-edited one must not be able
   to index a table out of bounds before there is a menu to fix it with. */
void opts_clamp(opts_t *o);

#endif /* OPTS_H */
