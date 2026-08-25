/*
 * msg.h -- the engine's own ON-SCREEN MESSAGE LAYER.
 *
 * `RCCars.exe' has one, it holds ELEVEN slots over SIX textures, and this port
 * had been drawing four of those slots out of three separate files with nothing
 * arbitrating between them. That was defensible while there were two -- hud.c
 * and countdown.c each say so, in the same words: "two messages that cannot
 * overlap need neither [a priority table nor a queue]". The fourth arrived with
 * the WRONG WAY banner and the argument died with it.
 *
 * WHAT IS RECOVERED, and it is the whole layer. hud_data.h's MSG_SLOT table
 * carries it; the addresses behind each column are in the generator.
 *
 *   0x4af195     loads the six textures BY NAME into a handle table, and the
 *                order it loads them in IS the texture index the slot table
 *                uses: msg_low_signal, msg_pause, msg_wrong_way, msg_hits,
 *                msg_321_s_f, msg_bestlap
 *   0x56d2d0     texture index per slot
 *   0x56d278     size per slot, a (w, h) pair of SCREEN FRACTIONS
 *   0x56d328     UV rect per slot -- four of the six textures are atlases
 *   0x56d2fc     PRIORITY per slot
 *   FUN_004afbb0 the poster, i.e. the arbitration -- three rules, below
 *   0x4af28e     the retire step: `life -= dt', drop at `life < -hold'
 *   FUN_004b11e0 the drawer: centred across, one of three vertical BANDS
 *   FUN_004b12b0 the grow-in, about the rect's own centre
 *
 * THE ARBITRATION IS THREE RULES and they are worth stating plainly, because
 * two of them have surprising consequences:
 *
 *   1. A post of the slot ALREADY SHOWING is dropped (0x4afbde). So a message
 *      re-posted every frame does not sit pinned at the start of its own
 *      grow-in -- it runs its life out and is posted again. That is what makes
 *      the WRONG WAY banner PULSE, and it is why nothing needs a "have I posted
 *      this yet" flag of its own.
 *   2. A post is dropped when the showing slot's priority is strictly HIGHER
 *      (0x4afbfc). Equal priorities REPLACE each other -- which matters, since
 *      seven of the eleven slots share priority 0.
 *   3. Otherwise it takes the screen.
 *
 * AND THE FOURTH POSTER ARGUMENT IS A HOLD-OVER. It is not read by the drawer,
 * which is why this project recorded it as unread; the RETIRE step reads it. A
 * slot is SELECTED for `life + hold' seconds and VISIBLE for the first `life' of
 * them, so a message with a hold flashes, goes quiet, and comes back:
 *
 *     WRONG WAY   life 1.5   hold 4.0   ->  1.5 s on, 4.0 s off, every 5.5 s
 *     3, 2, 1, GO life 1.0   hold 0     ->  on for its whole life, then gone
 *
 * The port had the wrong-way banner re-posting the moment its life ran out, so
 * it flashed -- and beeped -- every 1.5 s instead of every 5.5. See ui.md.
 *
 * WHAT THIS FILE OWNS, and what it does not
 * -----------------------------------------
 * It owns the TABLE, the ARBITRATION and the DRAW for every slot, and the state
 * machine for the two slots nothing else had: `msg_pause' and `msg_bestlap'.
 *
 * It does NOT yet own hud.c's two hit banners or countdown.c's five countdown
 * cells. Those two files still draw their own geometry, and are held off the
 * layer for one reason each: hud.c's timing is knowingly not the original's
 * (recovered life 1.0 s and animate 0 against its own 0.85 s with an overshoot
 * and a fade -- see ui.md, which argues for leaving the feel alone), and
 * countdown.c's five cells carry per-slot checks that would move with them. Both
 * are arbitrated ANYWAY, through msg_arbitrate: main.c asks which of the four
 * contenders wins the screen and draws only that one, so the engine's priority
 * table decides what the player sees even where the geometry has not moved yet.
 * Folding those two in is a clean follow-up and nothing here has to change for
 * it.
 *
 * WHY IT IS ITS OWN FILE: the same reason as hud.c, race_ui.c and dirarrow.c --
 * nothing compiles main.c. This links against ui.c alone, so ui_test's recorder
 * can read back which cell of which atlas went where, in which band, at what
 * size, and which of two contending slots actually reached the screen.
 */

#ifndef MSG_H
#define MSG_H

#include "hud_data.h"

/* ------------------------------------------------------------ the port's own */

/* The 4:3 frame the eleven size pairs were authored against. Every slot's `w'
   and `h' are fractions of it, so the pair encodes the ART'S ASPECT and msg.c
   keeps the HEIGHT and derives the width -- the same rule and the same reason as
   the rest of this HUD. On a 4:3 panel it is bit-identical to using both. */
#define MSG_REF_ASPECT (4.f / 3.f)

/* HOW LONG `PAUSE' AND `BEST LAP' LIVE, and both are the port's: the engine's
   own posts of slots 1 and 10 have not been found, so the two lives here are
   chosen and said to be chosen.
 *
 * PAUSE is posted with a life longer than any frame and RE-POSTED while the menu
 * is open -- but rule 1 above drops a post of the showing slot, so it would
 * pulse. It gets msg_hold() instead, which refreshes the life of the slot
 * already up without re-posting it: the banner sits still for as long as the
 * menu is open, which is what a pause banner is. Slot 1's priority is 9, the
 * highest in the table by a wide margin, so nothing takes the screen off it.
 *
 * BEST LAP is a one-shot on a lap time, and 2.0 s is the countdown's own `GO'
 * life -- the longest life any recovered post uses -- because a lap time is
 * something the player wants to read rather than glimpse. Its hold is 0, so it
 * appears once and goes. */
#define MSG_PAUSE_LIFE  0.50f
#define MSG_BEST_LIFE   2.00f

/* Font scale for the fallback words, used only where a texture is not in the
   packed assets -- the degrade-rather-than-vanish rule every other element in
   this HUD follows. ui.c's cell is 10 x 19 px. */
#define MSG_FALLBACK_SCALE 2.6f

/* ------------------------------------------------------------------ the state */

typedef struct {
    /* The six, by the exe's own index -- MSG_TEX_NAME's order. A 0 draws that
       slot's fallback word instead, or nothing where there is no word for it. */
    unsigned int tex[MSG_N_TEX];

    int   slot;      /* 0x56d3d8: what holds the screen, or -1 */
    float life;      /* 0x149dd5c: seconds of VISIBILITY left; drawn while > 0 */
    float total;     /* 0x149dd74: the life it was posted with, for the grow-in */

    /* SECONDS SINCE THE POST, which is what the grow-in is a function of. The
       engine has no such field and does not need one: it computes the elapsed as
       `total - life', and since its life only ever falls the two are the same
       number. The port needs it separately because msg_hold REFRESHES a life --
       and `total - life' then stops rising, which froze the PAUSE banner at
       scale zero for as long as the menu was open. Equal to `total - life' after
       every engine-shaped post, which ui_test asserts. */
    float age;
    float hold;      /* 0x149dd68: retire at life < -hold */
    int   animate;   /* 0x149dd50: 1 runs the grow-in, 0 draws at full size */
    int   cue;       /* 0x149dd80: the one-shot latch, as a SLOT or -1 */

    /* WHICH BAND, i.e. the drawer's own third argument, which is the player
       index: 0 the whole screen, 1 the top half, 2 the bottom half. A single
       player is 0 and that is what this port sets. */
    int   anchor;
} msg_t;

/* -------------------------------------------------------------------- the API */

/* `tex' is MSG_N_TEX handles in MSG_TEX_NAME's order; NULL binds none. */
void msg_init(msg_t *m, const unsigned int *tex);

/* Clear the screen without disturbing the bindings -- a restart, a track change,
   a respawn. The engine's own `post slot -1'. */
void msg_reset(msg_t *m);

/* POST. -> 1 if it took the screen, 0 if the arbitration dropped it. `life' is
   how long it is visible and `hold' how long it stays selected past that;
   `animate' 1 runs the grow-in. The three rules are in the header comment. */
int msg_post(msg_t *m, int slot, float life, float hold, int animate);

/* REFRESH the life of the slot already showing, without re-posting it, and
   WITHOUT restarting its grow-in. -> 1 if that slot was up. This is not the engine's -- the engine has no message that
   needs to stay up indefinitely, because its pause is a state and not a banner
   -- and it exists so PAUSE can hold rather than pulse. Named here because it is
   the port's, not recovered. */
int msg_hold(msg_t *m, int slot, float life);

/* Once a frame with that frame's dt. `life' falls; the slot is released at
   `life < -hold'. A dt of 0 freezes it, which is what the menu does to
   everything else -- and is why PAUSE refreshes rather than relying on this. */
void msg_step(msg_t *m, float dt);

/* What holds the screen, or -1. */
int msg_slot(const msg_t *m);

/* Is `slot' both selected AND inside its visible life? */
int msg_visible(const msg_t *m, int slot);

/* THE SLOT THIS LAYER WANTS THE SCREEN FOR, or -1 -- which is what it is
 * SHOWING, not what it holds. A slot in its hold-over phase is selected and
 * invisible, and the engine lets an equal-priority post replace it there (rule
 * 2 drops only a STRICTLY higher one), so an invisible hold must not out-vote a
 * visible contender. Every slot this port raises has priority 0 except PAUSE,
 * which has no hold, so this is equivalent to the engine's own behaviour for all
 * of them -- said out loud because it would not be if a HIGH-priority slot ever
 * gained a hold. */
int msg_contender(const msg_t *m);

/* Consume the post latch: the slot just posted, or -1. An EDGE, so a host that
   calls this once a frame gets exactly one cue per post. The engine hangs
   exactly one sound off this, on slot 2 (`0x4b0301' compares the slot with 2
   before playing `cp_wrongway'); returning the slot rather than a bare flag
   keeps that mapping at the call site, where the port's sound names live. */
int msg_cue(msg_t *m);

/* WHICH OF SEVERAL CONTENDING SLOTS WINS THE SCREEN, by the recovered priority
 * table: the highest priority, and the FIRST of a tie, which is rule 2's "equal
 * replaces" seen from the caller's side. `slots' may hold -1 for "nothing to
 * show". -> the winning slot, or -1.
 *
 * Public and pure because it is the arbitration itself, and because hud.c and
 * countdown.c still draw their own geometry: main.c hands their slots in
 * alongside this layer's and draws only the winner. ui_test measures it
 * directly. */
int msg_arbitrate(const int *slots, int n);

void msg_draw(const msg_t *m, int screen_w, int screen_h);

/* THE GEOMETRY OF ONE SLOT, in the pixels ui.c draws in: the quad's centre and
 * its size, with `k' the grow-in scale. Public because hud.c and countdown.c
 * are not on this layer yet and should not carry a third and fourth copy of the
 * same arithmetic when they come across; and because ui_test would otherwise
 * have to re-derive it to check it. */
void msg_slot_rect(int slot, int anchor, float k, int screen_w, int screen_h,
                   float *cx, float *cy, float *w, float *h);

#endif /* MSG_H */
