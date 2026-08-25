/*
 * hud.h -- the on-screen feedback the port owes the player, over the world.
 *
 * One thing so far, and it is the game's OWN artwork: `msg_hits`, shown when the
 * car knocks one of the 129 knockable props. Those objects arrived with a sound
 * each (stone.sb names a wav per model -- see prop_data.h and sfx_prop_hit) and
 * with nothing to SEE, so on a track where a can is behind the camera by the
 * time it moves, hitting one was a noise with no acknowledgement.
 *
 * WHAT IS RECOVERED, and it is more than expected
 * ----------------------------------------------
 * `RCCars.exe` has a whole on-screen message layer, and this is one of its
 * entries. `FUN_004af195` loads SIX message textures by name into a handle table
 * at `0x149dc20`, in this order:
 *
 *     0 msg_low_signal   1 msg_pause   2 msg_wrong_way
 *     3 msg_hits         4 msg_321_s_f 5 msg_bestlap
 *
 * and `FUN_004b11e0` draws one of ELEVEN message SLOTS over those six textures.
 * Each slot carries a texture index (`0x56d2d0`), a size in normalised screen
 * units (`0x56d278`, a (w, h) pair) and a UV rect (`0x56d328`, four floats):
 *
 *     slot  tex  what              w        h        uv
 *      3     3   !HIT!             0.320    0.21333  0,0    -> 1,0.5
 *      4     3   GREAT !HIT!       0.320    0.21333  0,0.5  -> 1,1
 *
 * so **`msg_hits` is an ATLAS OF TWO MESSAGES**, the ordinary hit in its top half
 * and a GREAT HIT in its bottom half, and the port draws the very same halves at
 * the very same size. The drawer places them:
 *
 *     x0 = (1 - w) / 2                      always centred across the screen
 *     y0 = (base - h) / 2 + yoff[anchor]    base 1.0, 0.5 or 1.5 by anchor
 *
 * -- three vertical bands (whole screen, top half, bottom half), and `yoff`
 * (`0x149dd44`) lands in the zero-filled part of `.data`, so it is 0.0 and the
 * bands are exactly those three expressions. Then
 * `FUN_00471610(rect, tex, 0xffffffff, 4, uv)` draws the quad -- colour always
 * white, so the original never fades a message; what it animates is the SCALE,
 * about the rect's own centre (`FUN_004b12b0`).
 *
 * THIS FILE USED TO SAY "NOTHING IN THE RETAIL EXE EVER POSTS SLOT 3 OR 4", AND
 * THAT WAS WRONG. The enumeration behind it was right -- ten `E8` rel32 calls to
 * the poster `FUN_004afbb0` -- and the conclusion was not: ONE of those ten call
 * sites has its arguments set up in FOUR different branches, and `0x4b025d`
 * posts slot 0, slot 4, slot 3 or slot 2 depending on which. Reading one branch
 * per call site attributes one slot per call site.
 *
 *     0x4b022e  slot 4  GREAT !HIT!  life 1.0  animate 0  gated on phys+0x56ec
 *     0x4b023c  slot 3  !HIT!        life 1.0  animate 0  gated on phys+0x56e8
 *     0x4b024e  slot 2  wrong way    life 1.5  animate 1  -- see dirarrow.h
 *
 * So the two hit messages are NOT cut content. See ui.md, "The hit banners ARE
 * posted, and this file said they were not", for the writers of both gates and
 * for the recovered tier test; and traps.md, "A DOCUMENTED NEGATIVE IS STILL A
 * CLAIM".
 *
 * WHAT IS STILL THE PORT'S is the TRIGGER, because the recovered one runs on an
 * accumulator and a mean impact vector that are not transcribed. The recovered
 * LIFE is 1.0 s against HUD_HIT_TIME's 0.85, the hit slots pass `animate 0` so
 * the original does not scale them (the overshoot below is FUN_004b12b0's idea,
 * which is true of the mechanism and not of these two slots) and there is no
 * fade. Moving all three is its own pass with ui_test part 8 as the evidence;
 * ui.md says why it has not been taken.
 *
 * WHAT IS THE PORT'S, therefore
 * -----------------------------
 *   - the trigger: a prop-hit edge, above the same speed floor sfx.c uses
 *   - which of the two halves (HUD_GREAT_SPEED / HUD_GREAT_COUNT below)
 *   - the timing, and a fade rather than the original's scale animation. The
 *     PRIORITY TABLE is transcribed now -- msg.c owns it, and these two slots are
 *     arbitrated through msg_arbitrate like everything else, so "there is nothing
 *     here for a priority to arbitrate" is no longer the reason. The reason is
 *     that the recovered post is life 1.0 with `animate 0' and no fade, against
 *     this file's 0.85 with an overshoot: moving all three is a visible change to
 *     something nobody has complained about, and it wants its own pass with
 *     ui_test part 8 as the evidence. See ui.md and msg.h
 *   - the vertical band: the top-half anchor rather than the whole-screen one,
 *     which is a choice between two of the drawer's own three. See HUD_MSG_ANCHOR
 *
 * WHY IT IS ITS OWN FILE. The state is four numbers and a decay, which is exactly
 * the sort of thing that ends up as three statics in main.c and is then
 * untestable, because nothing compiles main.c. Here it links against ui.c and
 * ui_test's recorder can read back what was actually drawn -- including which
 * half of the atlas.
 *
 * The draw brackets itself with ui_begin/ui_end, the way menu_draw does, so the
 * caller does not have to know it is a 2D pass.
 */

#ifndef HUD_H
#define HUD_H

/* ---------------------------------------------------- the recovered geometry */

/* Slot 3 and slot 4 both, from 0x56d278: the message's size as a fraction of the
   screen's width and height. */
#define HUD_MSG_W 0.320000f
#define HUD_MSG_H 0.213333f

/* The UV split, from 0x56d328: slot 3 is the top half, slot 4 the bottom. */
#define HUD_MSG_V_SPLIT 0.5f

/* The frame those fractions were authored against. RC Cars is a 2003 PC game and
   its menu art is 4:3; on that screen the recovered pair puts a half of the
   256x256 atlas at 204.8 x 102.4 px, which is exactly the 2:1 of a 256x128 cell.
 *
 * So the two fractions ENCODE the art's aspect, and hud.c keeps the HEIGHT
 * fraction and derives the width from the pair. On a 4:3 panel that is
 * bit-identical to using both fractions directly; on the Vita's 960x544 it is the
 * difference between the art and the art stretched 4/3 wider. */
#define HUD_MSG_REF_ASPECT (4.f / 3.f)

/* Which of FUN_004b11e0's three vertical bands. 0 = the whole screen, so
   `y0 = (1 - h)/2` and the message sits dead centre; 1 = the top half,
   `y0 = (0.5 - h)/2`; 2 = the bottom half, `y0 = (1.5 - h)/2`.
 *
 * THE PORT TAKES 1, and 0 is what a single-screen message would have used. The
 * reason is this port's camera: it holds the car in the middle of the frame, and
 * a banner 21% of the screen tall centred on it covers the thing the banner is
 * about. Band 1 puts it in the upper third and is still one of the drawer's own
 * three rather than a number invented here. Set it to 0 for the centred original. */
#define HUD_MSG_ANCHOR 1

/* ------------------------------------------------------------- the port's own */

/* How long a pop lives, and how much of that is spent at full alpha before it
   fades. Sized against the thing it is reporting: a can knocked at walking pace
   is out of shot in well under a second, so a pop that outlives the object by
   much is describing something the player can no longer see. */
#define HUD_HIT_TIME  0.85f
#define HUD_HIT_HOLD  0.45f

/* The banner snaps in bigger than it settles, about its own centre -- which is
   the one thing here that IS the original's idea: FUN_004b12b0 scales a message's
   rect about its centre over the message's life, and never touches its alpha.
   HUD_HIT_POP is how long the overshoot takes to decay. */
#define HUD_HIT_POP       0.10f
#define HUD_HIT_OVERSHOOT 1.30f

/* Closing speed below which a brush against a prop raises nothing.
 *
 * THE SAME NUMBER sfx.c's PROP_MIN_SPEED uses, so the pop and the knock agree:
 * a hit you can hear is a hit you can see, and one you cannot hear leaves the
 * screen alone. It is a second copy rather than a shared constant on purpose --
 * the pop must still appear on a machine where the audio layer failed to come
 * up, so this file does not depend on that one. If either moves, move both. */
#define HUD_HIT_MIN_SPEED 0.35f

/* And the top of that ramp, likewise sfx.c's PROP_FULL_SPEED. */
#define HUD_HIT_FULL_SPEED 4.0f

/* WHICH HALF OF THE ATLAS -- the port's rule, since the retail exe posts neither
   slot and there is nothing to read. A hit is GREAT when it is as hard as the
   game's own gain ramp goes, or when one pass took out three objects. Both halves
   are anchored to something real rather than picked: the top of the recovered
   speed ramp, and a count the pop is already keeping. */
#define HUD_GREAT_SPEED HUD_HIT_FULL_SPEED
#define HUD_GREAT_COUNT 3

/* Font scale for the fallback word, used only when msg_hits is not in the packed
   assets. ui.c's cell is 10 x 19 px, so 3.0 puts "HIT" at 90 x 57 px on a
   960 x 544 screen. */
#define HUD_HIT_SCALE 3.0f

typedef struct {
    /* msg_hits, bound once from the shared props scene -- see hud_init. 0 means
       it was not packed, and hud_draw falls back to the compiled-in font. */
    unsigned int tex;

    float t;        /* seconds of pop left; 0 means nothing is on screen */
    int   count;    /* props collected by THIS pop -- drives the GREAT half */
    float speed;    /* the hardest knock in it, m/s -- likewise */
} hud_t;

/* `tex` is the GLuint for `msg_hits`, or 0 to run on the font fallback. */
void hud_init(hud_t *h, unsigned int tex);

/* Wipe whatever is on screen, keeping the texture binding. For a teleport: the
   collision it refers to has stopped being where the player is. */
void hud_reset(hud_t *h);

/* The car just started touching a prop, at `speed` m/s of closing speed. Raise
   the pop, or extend and count up the one already showing. Below
   HUD_HIT_MIN_SPEED this does nothing at all. */
void hud_hit(hud_t *h, float speed);

/* Decay. Call once a frame with that frame's dt -- dt 0 while the menu is up
   freezes the pop, which is what the rest of the world does there. */
void hud_step(hud_t *h, float dt);

/* Is there anything to draw? Lets the caller skip the whole 2D pass. */
int hud_active(const hud_t *h);

/* Is the pop currently the GREAT !HIT! half of the atlas? */
int hud_is_great(const hud_t *h);

/* WHICH MESSAGE SLOT the pop is, in the engine's own numbering, or -1 for none.
   This file still draws its own geometry -- see msg.h for why -- but the
   arbitration is the layer's, and a slot number is what it arbitrates over. */
int hud_slot(const hud_t *h);

void hud_draw(const hud_t *h, int screen_w, int screen_h);

#endif /* HUD_H */
