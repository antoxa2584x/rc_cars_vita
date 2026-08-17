/*
 * countdown.h -- the race start: 3, 2, 1, GO!, in the game's own artwork.
 *
 * The second entry in the port's on-screen message layer, after hud.c's !HIT!
 * banner, and a much better recovered one: where the two hit slots had to be
 * given a trigger and a timing by hand, this whole message -- the art, its
 * layout, its per-message life, its animation and its SOUND -- is read out of
 * the retail image, and two of those sources are independent of each other.
 *
 * THE ARTWORK, and the notes had two of its cells wrong
 * ----------------------------------------------------
 * `FUN_004af195` loads six message textures by name into a handle table at
 * `0x149dc20`; the fifth is `msg_321_s_f`, a 512x256 ARGB8888 atlas whose name
 * reads "3 2 1 START FINISH". It is not quite that. `FUN_004b11e0` draws one of
 * eleven message SLOTS, each carrying a texture index (`0x56d2d0`), a size as a
 * (w, h) fraction pair (`0x56d278`) and a UV rect (`0x56d328`), and five of the
 * eleven point at this texture:
 *
 *     slot  what     w        h        uv
 *      5    3        0.16000  0.21333  0.000,0.000 -> 0.250,0.500
 *      6    2        0.16000  0.21333  0.000,0.500 -> 0.250,1.000
 *      7    1        0.09250  0.21333  0.250,0.500 -> 0.395,1.000
 *      8    FiNiSH   0.38750  0.21333  0.395,0.500 -> 1.000,1.000
 *      9    !!Go!!   0.48000  0.21333  0.250,0.000 -> 1.000,0.500
 *
 * -- so the fourth word is **GO!**, not START, and it is slot NINE while FINISH
 * is slot eight. Settled by opening the PNG and cropping it on those five rects,
 * which is what this project does with a texture whose name is a claim (see the
 * six audio materials that had to be reclassified by looking at the artwork).
 *
 * Every cell is drawn at exactly 0.8 texels to the pixel on the 640x480 frame
 * these fractions were authored for, which is the cross-check that the rects and
 * the sizes belong together: slot 5's u extent is 128 texels against
 * 0.16 * 640 = 102.4 px, slot 7's is 74.24 against 59.2, slot 9's 384 against
 * 307.2. Three ratios, one number.
 *
 * FINISH is packed and addressable and nothing raises it, because there is no
 * race to finish yet -- see "What is left" in CLAUDE.md. It is here as
 * CD_CELL_FINISH so the day there is one, the geometry is already read.
 *
 * THE TIMING, recovered TWICE
 * ---------------------------
 * The countdown is posted by the game-message handler at `0x4e2060` onward, one
 * `FUN_004afbb0(player, slot, life, _, animate)` per step:
 *
 *     0x4e2079  slot 5   life 1.0   animate 1
 *     0x4e209a  slot 6   life 1.0   animate 1
 *     0x4e20b9  slot 7   life 1.0   animate 1
 *     0x4e20e8  slot 9   life 2.0   animate 1
 *
 * -- three digits at a second each and GO! held for two, so the light goes green
 * at t = 3.0 and the banner outlives it by two seconds.
 *
 * AND THE SOUND SAYS THE SAME THING, off data that has nothing to do with the
 * exe: `Sound/cp_start.wav` is 4.44 s holding FOUR beeps whose onsets are at
 * 0.0005, 1.0005, 2.0005 and 3.0005 s -- 1.0000 s apart to the sample. One wav,
 * played once at t = 0, lands a beep on each digit and the fourth on GO. Two
 * independent recoveries of the same four numbers, which is the strongest kind
 * of confirmation this project gets.
 *
 * THE ANIMATION, from the drawer
 * ------------------------------
 * `0x4b028d`, the per-frame message update, computes the scale the drawer is then
 * handed, and only when the poster's fifth argument was 1 -- which the countdown
 * passes and the two !HIT! slots do not:
 *
 *     d = min(life, 0.8)                     0x5544a0
 *     k = (elapsed >= d) ? 1.0 : elapsed / d
 *
 * so a message GROWS from nothing to full size over 0.8 s and then holds, about
 * its own rect's centre (`FUN_004b12b0`, which also declines to draw at all below
 * k = 1e-6). There is no fade anywhere in this layer: the colour argument to
 * `FUN_00471610` is a flat 0xffffffff, and a message ends by its life running out.
 * hud.c's own overshoot-and-fade is the port's; this one is the engine's.
 *
 * THE PLACEMENT
 * -------------
 *     x0 = (1 - w) / 2                       always centred across the screen
 *     y0 = (base - h) / 2 + yoff[band]       base 1.0, 0.5 or 1.5
 *
 * and the band is the message CHANNEL -- there are three of them (`0x56d3d8`),
 * three yoff entries (`0x149dd44`, in the zero-filled part of .data so all 0.0),
 * and the poster hands channels 0 and 1 to player 0 and channel 2 to player 1.
 * Channel 0 is the one whose viewport is the whole screen in a single-player
 * game, so band 0 -- dead centre -- is what this port draws in. hud.c takes
 * band 1 for its own stated reason, that a banner over the middle of the frame
 * covers the car it is about; a start countdown has no such problem, because the
 * car is not going anywhere yet.
 *
 * WHAT IS THE PORT'S
 * ------------------
 * Only the state machine, and only because there is no race module here to own
 * one: the original's countdown arrives as network messages from its game
 * manager, one per second, and the port has to keep its own clock. Specifically
 *
 *   - that the car and the field are HELD until GO. main.c does this by not
 *     spending physics ticks at all while countdown_holding() is true, which
 *     freezes the player, the opponents and the props together and by
 *     construction rather than by three separate gates -- they already all
 *     advance on the same banked ticks. The controls are zeroed as well, so a
 *     throttle held through the countdown is not a jump start it is simply
 *     ignored until the tick loop runs again.
 *   - that it runs on a race START and not on a checkpoint respawn. Dying is not
 *     the race beginning again, which is the whole point of the respawn change
 *     it shipped beside.
 *
 * It draws through ui.c the way hud.c does, brackets its own ui_begin/ui_end, and
 * is its own file for the same reason hud.c is: nothing compiles main.c, and
 * ui_test's recorder can read back which cell of the atlas went on screen.
 */

#ifndef COUNTDOWN_H
#define COUNTDOWN_H

/* --------------------------------------------------- the recovered geometry */

/* The slot table's five cells, from 0x56d278 (the size pair) and 0x56d328 (the
   UV rect). Indices here are this file's own 0..4; the engine's slot number is
   in the comment, because that is what the tables are indexed by. */
typedef struct {
    float w, h;                   /* fractions of a 4:3 screen -- see below */
    float u0, v0, u1, v1;
    int   slot;                   /* the engine's message slot */
} cd_cell;

#define CD_CELL_3      0
#define CD_CELL_2      1
#define CD_CELL_1      2
#define CD_CELL_GO     3
#define CD_CELL_FINISH 4          /* recovered, packed, and nothing raises it */
#define CD_N_CELLS     5

extern const cd_cell CD_CELLS[CD_N_CELLS];

/* The frame the (w, h) fractions were authored against, exactly as hud.h keeps
   it and for the same reason: the pair encodes the art's own aspect, so the
   HEIGHT fraction is used as given and the width is derived from the pair. On a
   4:3 panel that is bit-identical to using both; on 960x544 it is the difference
   between GO! and GO! stretched 4/3 wider. */
#define CD_REF_ASPECT (4.f / 3.f)

/* Which of FUN_004b11e0's three vertical bands. 0 = the whole screen, i.e.
   y0 = (1 - h)/2. See the header comment for why this is 0 where
   HUD_MSG_ANCHOR is 1. */
#define CD_BAND 0

/* One second per digit and two for GO!, from the four poster calls at
   0x4e2079 / 0x4e209a / 0x4e20b9 / 0x4e20e8 -- and independently from the four
   1.0000 s beep onsets in cp_start.wav. */
#define CD_STEP_TIME 1.0f
#define CD_GO_TIME   2.0f

/* The grow-in, from 0x5544a0: a message reaches full size after this many
   seconds of its life, or at the end of its life if that is sooner. */
#define CD_GROW_TIME 0.8f

/* The drawer refuses a scale below this (FUN_004b12b0 against 0x554380), which
   is what makes the first frame of a message draw nothing at all rather than a
   degenerate quad. */
#define CD_MIN_SCALE 1e-6f

/* ------------------------------------------------------------- the port's own */

/* Font scale for the fallback words, used only when msg_321_s_f is not in the
   packed assets. ui.c's cell is 10 x 19 px. Bigger than hud.c's HIT because a
   start light is the only thing on screen when it is up. */
#define CD_TEXT_SCALE 5.0f

typedef struct {
    /* msg_321_s_f, bound once from the shared props scene -- see countdown_init.
       0 means it was not packed, and countdown_draw falls back to the
       compiled-in font. */
    unsigned int tex;

    /* Seconds since the countdown started, or < 0 when it is not running.
       Counting UP rather than down, because the recovered scale ramp is a
       function of a message's ELAPSED time and the port has been bitten once by
       reading an elapsed counter as a countdown (fx.c's DynamicScale). */
    float t;
    int   running;

    /* Raised on the tick the light goes green, so the host can start its clock
       or fire a cue. An EDGE, the way cp_t.passed and prop_t.hit are. */
    int   go;
} countdown_t;

/* `tex` is the GLuint for msg_321_s_f, or 0 to run on the font fallback. Leaves
   the countdown NOT running -- countdown_start begins one. */
void countdown_init(countdown_t *c, unsigned int tex);

/* Begin, or begin again from the top. */
void countdown_start(countdown_t *c);

/* Abandon it, with no `go` edge: whatever was about to start is not starting.
 *
 * NOTHING IN main.c CALLS THIS TODAY and that is not an oversight -- every path
 * that ends a race in this port (a track change, a car change, a reload, the
 * Restart row) goes on to respawn(), which starts a fresh countdown in the same
 * frame, so there is no window in which a stale one could be left running. It
 * exists because a race module that aborts a start needs it, and because the one
 * property that has to hold -- that stopping raises no `go`, i.e. the car is
 * never released by giving up -- can only be checked if the call exists. ui_test
 * checks exactly that. */
void countdown_stop(countdown_t *c);

/* Advance. Call once a frame with that frame's dt -- dt 0 while the menu is up
   holds the countdown where it is, which is what the rest of the world does
   there. Raises `go` on the one call that crosses CD_STEP_TIME * 3. */
void countdown_step(countdown_t *c, float dt);

/* Is the car still on the line? True from the start until GO, and it is what
   main.c gates the physics ticks on. False when nothing is running at all, so a
   host that never calls countdown_start is never held. */
int countdown_holding(const countdown_t *c);

/* Is there anything to draw? Lets the caller skip the whole 2D pass. */
int countdown_active(const countdown_t *c);

/* Which cell of the atlas is up, or -1 for none. CD_CELL_* above. */
int countdown_cell(const countdown_t *c);

/* The recovered scale for whatever is up: 0 -> 1 over CD_GROW_TIME of the
   current message's life, then flat. 0 when nothing is up. */
float countdown_scale(const countdown_t *c);

void countdown_draw(const countdown_t *c, int screen_w, int screen_h);

#endif /* COUNTDOWN_H */
