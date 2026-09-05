/*
 * awards.h -- THE PORT'S OWN AWARD BOOK, and every word of it is invented.
 *
 * There are no achievements in RC Cars. `RCCars.exe' has no trophy table, no
 * unlock list and no notion of one: what it keeps per player is cash, scores,
 * a rank off those two (player.h) and 22 race slots per track. So unlike every
 * other file in this port, NOTHING HERE IS RECOVERED -- the twenty-five awards
 * below, their names, their wording and their thresholds are this project's,
 * and they are written down as such so no later reader mistakes them for the
 * game's. What they are anchored to is not invented: every one of them fires
 * off an event some other module already raises, and the threshold of each is
 * either 1, a round count, or a number out of the shipped data.
 *
 * WHY NOT PSN TROPHIES, which is the first thing to ask on this machine. The
 * Vita has a trophy service and the SDK stubs for it are right there --
 * `libSceNpTrophy_stub.a' exports sceNpTrophyCreateContext,
 * sceNpTrophyUnlockTrophy and thirteen more -- and it is still not available to
 * this port, for two reasons that are not about effort:
 *
 *   - a context is opened against an NP COMMUNICATION ID (`NPWRxxxxx_00'),
 *     which Sony ISSUES to a title. Homebrew has none, and this VPK's id is
 *     RCCV00001, which is a title id and not an NP one
 *   - the trophy set itself is a TROPHY.TRP in the app's own `sce_sys/trophy/',
 *     and the service checks its signature before it will register it. That
 *     signature comes out of the publishing tools, so an unsigned pack made
 *     here is rejected rather than merely untrusted
 *
 * -- and vitasdk ships no `psp2/np/trophy.h' at all, so the structs those
 * fifteen stubs take are not even declared. The book therefore lives in this
 * port's own file, exactly as the record book does (records.h), and the ONE
 * system service that is available to a self-signed app is used for what it is
 * good for: `sceNotificationUtil' puts a line in the machine's own notification
 * list, the same drawer a download or a friend request lands in. See awards.c's
 * `notify' -- it is best-effort by design, its return is logged, and the
 * in-app toast is what the player actually sees.
 *
 * WHAT AN AWARD IS. One row of the table: a KEY (the word in the file, which
 * never changes once shipped), a name, one line saying how it is earned, and a
 * goal of one of two shapes --
 *
 *     AW_K_COUNT  progress is a tally; the award lands when it reaches `goal'
 *     AW_K_BITS   progress is a BITMASK and `goal' is how many bits must be
 *                 set. That is what "on all ten tracks" and "with all three
 *                 cars" are, and a bitmask is the only form of those that
 *                 cannot be satisfied by doing the same track ten times
 *
 * THE BOOK IS PER PROFILE, keyed by the profile's NAME, and it is its own file
 * -- `ux0:data/rccars/awards.txt', beside records.txt and settings.txt. NOT in
 * the `.scp': that format is the engine's and this port writes it back byte for
 * byte, portrait and all (player.h), so a chunk invented here would be a chunk
 * the original does not know in a file whose whole value is that it can be
 * carried to a PC install and opened there. Text, key per line, unknown keys
 * skipped -- settings.h's argument, and for the same three reasons.
 *
 * WHAT IS DELIBERATELY NOT IN THE LIST, and this is the half of the design that
 * took the reading: THE PORT HAD NO INCOME when this book was written.
 * `championship.ini's prize money belonged to a championship that was not
 * built, so nothing in the build ever added to a profile's cash or scores --
 * garage.c only spends and refunds -- and a fresh profile had $100
 * (CHAMP_DEFAULT_CASH) forever. So "own all three cars" ($2000 and $1250),
 * "fit a level-3 part" ($450 upwards) and every rank above the bottom rung were
 * UNREACHABLE and were not offered: an award nobody can earn is not an award,
 * it is a bug with a caption. What IS offered off the shop is the one purchase
 * $100 covers -- a level-1 part, from $50 -- and the paint, which is free.
 *
 * THE CHAMPIONSHIP IS BUILT NOW (champ.h) AND THE PREMISE HAS EXPIRED. A round
 * on its ladder pays prize money into both cash and scores, so all five of
 * those are earnable. They are still absent, and now for a different reason:
 * five entries, five anchors and an awards_test pass of its own is a job, and
 * it was left rather than done half. THESE ARE THE FIVE:
 *
 *     own all three cars          Car2 $2000, Car3 $1250
 *     fit a level-3 part          $450 upwards
 *     three ranks above the bottom rung, off FUN_004e85d0's own ladder
 *
 * docs/known-issues.md carries the same note as the one thing the championship
 * unblocked and did not do.
 *
 * WHY IT IS ITS OWN FILE. records.h's reason and hud.h's: the state is a table
 * and a tally, nothing compiles main.c, and the whole of it -- the rules, the
 * queue, the file -- is checkable on the host. `awards_test' does that.
 */
#ifndef AWARDS_H
#define AWARDS_H

#include "player.h"     /* PL_NAME and PL_MAX -- the book is keyed by profile */

/* THE TWENTY-FIVE. The order is the order the page lists them in, grouped the
   way a player meets them: the race, the driving, the damage, the shop.
 *
   NEVER RENUMBER a shipped id -- but nothing is stored by number: the file
   carries each award's own `key' string, so inserting one here costs nothing
   and a build that drops one leaves an unknown key in the file rather than
   shifting everybody's progress along by one. That is the whole reason the key
   exists. */
enum {
    AW_FIRST_RACE = 0,  /* finish one                                   */
    AW_FIRST_WIN,       /* win one                                      */
    AW_FIVE_WINS,       /* win five                                     */
    AW_ALL_TRACKS,      /* finish on all ten -- bits                    */
    AW_WIN_TRACKS,      /* win on all ten -- bits                       */
    AW_WIN_CARS,        /* win with all three cars -- bits              */
    AW_SEVEN_LAPS,      /* finish a seven-lap race                      */
    AW_NO_DEATHS,       /* win without one reset                        */
    AW_HOME_ALONE,      /* win with nobody else home                    */
    AW_NET_WIN,         /* win over the wire                            */

    AW_PEGGED,          /* the needle at the top of its own dial        */
    AW_AIR,             /* AW_AIR_TIME seconds with no contact          */
    AW_TANK_DRY,        /* empty the boost meter                        */
    AW_MARATHON,        /* AW_MARATHON_M metres, all races              */
    AW_DROWNED,         /* die under water                              */
    AW_WRONG_WAY,       /* five WRONG WAY banners                       */

    AW_PROPS_50,        /* fifty props knocked                          */
    AW_PROPS_500,       /* five hundred                                 */
    AW_GREAT_HIT,       /* the atlas's other half                       */
    AW_RUN_OVER,        /* ten of the locals                            */
    AW_SHOT_AT,         /* a guard's burst                              */
    AW_THROWN,          /* picked up and thrown                         */

    AW_UPGRADE,         /* fit a part                                   */
    AW_PAINTS,          /* all four paints -- bits                      */
    AW_HOUR,            /* an hour of racing                            */
    AW_N
};

/* The got-mask is a u32, so this is a real ceiling and not a style rule. */
#if AW_N > 32
#error "awards.h: more awards than the got-mask has bits"
#endif

typedef enum {
    AW_K_COUNT = 0,     /* prog >= goal            */
    AW_K_BITS           /* popcount(prog) >= goal  */
} aw_kind;

typedef struct {
    const char *key;    /* the word in awards.txt -- stable forever */
    const char *name;   /* what the toast and the page show */
    const char *what;   /* one line: how it is earned */
    unsigned char kind; /* aw_kind */
    int goal;
} aw_def;

/* ------------------------------------------------------ the port's thresholds

   Every one of these is this project's choice. The three that could have been
   arbitrary are not: AW_PEGGED is a FRACTION of the car's own boost top speed
   (rb_data.h's speedBoostMax through the resonator upgrade, which is what the
   HUD's dial is scaled to) rather than a km/h figure, because 35 km/h is the
   fastest thing in the shipped data and a fixed number would be reachable in
   one car and not another; AW_AIR_TIME is longer than any landing this port has
   logged off a kerb and shorter than the Mines' rail jump; AW_MARATHON_M is a
   marathon. */
#define AW_PEG_FRAC     0.97f
#define AW_PEG_HOLD     0.50f    /* seconds it must be held, so a spike is not it */
#define AW_AIR_TIME     2.00f
#define AW_MARATHON_M   42195
#define AW_HOUR_S       3600

/* HOW LONG A TOAST IS UP, and how long between two of them. The first is
   msg.h's MSG_BEST_LIFE doubled -- an award has a name AND a line to read,
   which is more than a lap time -- and the gap keeps two awards landing in one
   frame (which happens: a first race is also a first finish) from reading as
   one flicker. */
#define AW_TOAST_LIFE   4.00f
#define AW_TOAST_GAP    0.25f
#define AW_TOAST_FADE   0.50f    /* of the life, at each end */

/* Bumped only if an existing key changes MEANING. A file from a LATER version
   is left alone entirely -- records.h's rule, and for its reason. */
#define AWARDS_VERSION 1
#define AWARDS_TMP_SUFFIX ".new"

/* The whole file, formatted: 25 awards over up to PL_MAX books, plus the
   header. Sized like records.h's, i.e. generously and once. */
#define AWARDS_TEXT_MAX 16384

/* ------------------------------------------------------------------ the table */

/* Row `id', or NULL. */
const aw_def *award_def(int id);

/* ------------------------------------------------------------------ the book */

/* Whose book is live. NULL or an empty name selects nothing, which is the state
   the app is in before a profile exists -- and then every event below is
   dropped, because there is nobody to credit it to. A name that has no book yet
   gets a blank one. Case-insensitive, the way player_index_of is. */
void award_select(const char *player);
const char *award_player(void);

/* The live book. `progress' is the raw tally or bitmask. */
int  award_have(int id);
int  award_progress(int id);
int  award_n_have(void);

/* How far along, 0..1, for the page's bar. 1 whenever the award is held. */
float award_frac(int id);

/* ---------------------------------------------------------------- the events

   All of these are no-ops with no book selected, and all of them take PLAIN
   NUMBERS -- no module below this one appears in this header, which is the rule
   hud.h and dirarrow.h are written to and the reason the harness can drive the
   whole file with nothing else linked in. */

/* One finished race. `place' is 1-based, `deaths' is how many times the player
   was put back on a checkpoint during it, `n_finished' counts the racers who
   crossed for the last time before the race ended (the player included), and
   `net' says it was a race against other machines. */
typedef struct {
    int   track;        /* the port's own track index */
    int   car;          /* 0..2 */
    int   laps;         /* the limit the race was run to */
    int   place;        /* 1-based */
    int   n_racers;
    int   n_finished;
    int   net;
    int   deaths;
    float best_lap;     /* seconds, 0 for none turned */
} aw_race;
void award_race(const aw_race *r);

/* One frame of driving, and the six things that are watched over it. Called
   once a frame from the race path with that frame's dt -- 0 under the menu,
   which freezes all six the way it freezes everything else.
 *
   `speed' and `speed_max' are m/s (rbcar_speed and the dial's own full scale);
   `air' is the car's continuous no-contact time in seconds; `boost_dry' is the
   meter's own emptied latch; `wrong_way' and `great_hit' are the two FLAGS, not
   edges -- this file keeps the previous value of each, because an award counts
   the edge and a caller should not have to remember that. */
void award_frame(float dt, float speed, float speed_max, float air,
                 int boost_dry, int wrong_way, int great_hit);

/* One knocked prop, one local run over, one burst fired at the car, one throw,
   one drowning. Each is an EDGE at the call site -- prop_t.hit, chr_inst_t.event
   and the two death branches all already are. */
void award_prop(void);
void award_run_over(void);
void award_shot_at(void);
void award_thrown(void);
void award_drowned(void);

/* THE PROFILE'S OWN FACTS, on the one event that changes them -- MM_ACT_GARAGE,
   a finished race, and the way out of the app. Plain numbers again:
   `best_part_level' is the highest of the nine levels fitted across the
   profile's cars, `skin' the paint the current car is wearing, `play_time' the
   profile's own clock in seconds. */
void award_shop(int best_part_level, int skin, float play_time);

/* ------------------------------------------------------------------ the toast

   A QUEUE, not a slot: two awards can land in one frame and the second must not
   be dropped -- which is the one thing msg.c deliberately does with its own
   slots, and the difference is that a message re-posts itself while an award
   happens once and is gone. Nothing arbitrates against the message layer: the
   toast sits in the free band down the left of the HUD (awards.c says which
   pixels and why), so there is nothing for a priority to resolve. */

/* THE PLATE IS THE PORT'S OWN and there is no texture field for it, which is a
   change of mind worth recording: `messagebox_empty' is the plate the finish
   screen stands on and it is a PORTRAIT message box -- drawn 300 x 56 it
   collapses to its own left cap and two hairlines, which is what `hudshot'
   showed the first time this was rendered over the real art. So the toast
   draws a dark plate and a rule, the way hud.c falls back rather than
   vanishing, and takes only the two FONTS off the front end. */
typedef struct {
    unsigned int font_big;      /* Smash26 */
    unsigned int font_small;    /* Smash20 */
} award_tex;

void award_step(float dt);
int  award_showing(void);       /* the id on screen, or -1 */
int  award_pending(void);       /* how many are still queued */
void award_draw(const award_tex *t, int screen_w, int screen_h);

/* ---------------------------------------------------------------------- i/o */

void award_reset(void);         /* forget every book */
int  award_load(void);
int  award_save(void);
int  award_save_if_changed(void);
int  award_dirty(void);

const char *award_path(void);
void        award_set_path(const char *path);

/* The pure halves, for the harness: no file either way. `parse' starts from
   whatever the books already hold and returns 0 if the text declares a version
   this build will not read. */
int  award_parse(const char *text);
void award_format(char *out, int n);

#endif /* AWARDS_H */
