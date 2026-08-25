/*
 * dirarrow.h -- the DIRECTION ARROW, and the wrong-way rule behind the banner.
 *
 * The engine calls it the `DirectArrow' and it is the last piece of the in-race
 * HUD. Two chevrons at the bottom of the screen that turn toward wherever the
 * track goes next, tinted red to green by how far through the current stretch
 * the car is; and, when the car has been pointing the wrong way for long enough,
 * the game's own WRONG WAY banner over the world with the game's own sound
 * behind it.
 *
 * BOTH ARE RECOVERED WHOLE, and once again nothing had to be invented -- see
 * hud_data.h's own arrow block, which cites the address or the file behind every
 * number, and traps.md's standing rule about enumerating what ships before
 * saying a feature is not in the data. The pieces:
 *
 *   RCCarsDB/cockpit.sb          the geometry: two six-vertex chevrons under a
 *                                node called `navigation_arrows_1', named
 *                                `extru9' and `extru9_3' -- which are the two
 *                                names RCCars.exe looks up at 0x4af44c and the
 *                                two its error string "can't locate extru9 and
 *                                extru9_3 on cockpit arrow" complains about
 *   Textures.1/coc_str_2.csi     the art: a 128x128 grey chevron on alpha
 *   Settings/arrow.ini           the camera and the screen square it draws into
 *   Sound/cp_wrongway.wav        the cue, already in sfx.c and until now unraised
 *   RCCars.exe 0x4b1360          the eight sliders and their conversions
 *   RCCars.exe 0x4e8f00          the whole model: the angle, the tint, the
 *                                blink, the wrong-way timer and its suppression
 *   RCCars.exe 0x4e94e0          the five things the drawer asks for, off a
 *                                44-byte per-racer record
 *   RCCars.exe 0x4b0fa0          the draw; 0x4b024e the banner's post
 *   Config.gm  Cockpit/DirectArrow   1, i.e. on
 *
 * `Settings/arrow.ini' is the file that gives the whole thing away, and it is
 * one this port had never opened: eight sliders, `CX' 50 and `CY' 87 and `Len'
 * 17 -- a square 17% of the screen across at the bottom centre -- with a
 * `NearPlane', an `AngleVert', a `pos_y', a `pos_z' and a `dir'. Those last five
 * are a CAMERA, and the arrow is a little three-dimensional scene of its own
 * rendered into that square. See hud_data.h.
 *
 * WHAT THE MODEL IS, in one place, because it is spread over three functions in
 * the exe. Per frame, for the player:
 *
 *   - `dir' is the direction from the car to the point the arrow aims at, and
 *     `fwd' the car's own forward, both flattened to XZ.
 *   - the WRONG-WAY TIMER rises by dt while dir . fwd < 0 and falls by
 *     ARW_WRONG_DECAY * dt while it is >= 0, held inside [0, ARW_WRONG_CAP].
 *     The banner is up while the timer is over ARW_WRONG_ON -- unless the car is
 *     within ARW_WRONG_NEAR of the checkpoint it last passed, which is what
 *     stops a pass from raising it.
 *   - the ANGLE the chevrons are turned by chases the angle between `fwd' and
 *     `dir' at ARW_SLEW degrees a second (times ARW_SNAP_MUL while a snap is
 *     latched, until the two are within ARW_ANG_EPS).
 *   - the TINT is one colour on both chevrons, red at the start of a
 *     checkpoint-to-checkpoint segment through yellow to green at the end of it,
 *     off a progress fraction that chases the real one at ARW_CHASE per second.
 *     WRONG WAY forces the fraction to 0, so the arrow goes red with the banner.
 *   - the BLINK is a ARW_BLINK_CYCLE second cycle, on for ARW_BLINK_SHOW of it,
 *     and it runs while the car is going the wrong way or has come within
 *     ARW_BLINK_NEAR of the checkpoint it is heading for. Otherwise the arrow is
 *     drawn steadily. **HAS COME**, not IS: the field is a running MINIMUM, so
 *     the blink latches until the checkpoint advances -- see below.
 *
 * THE BANNER ITSELF IS NOT HERE ANY MORE. It is message slot 2 of the engine's
 * eleven-slot layer, and that layer is msg.c now -- this file decides WHETHER the
 * car is going the wrong way and main.c posts the slot. Which also fixed the
 * banner's own timing: the poster's fourth argument is a HOLD-OVER, so slot 2 is
 * 1.5 s visible and then 4.0 s of nothing, a 5.5 s pulse, where this file used to
 * re-post the moment the life ran out and flashed every 1.5 s. See msg.h.
 *
 * WHAT THE PORT'S OWN TWO THINGS ARE, and each is anchored:
 *
 *   1. THE SIGN OF THE ANGLE. `FUN_00410150' takes the acos of a normalised dot
 *      product and clamps it to 0..180, so the ENGINE'S OWN TARGET ANGLE IS
 *      UNSIGNED and its arrow leans the same way for a left turn and a right
 *      one -- the rotation `FUN_0040cc60' builds from a positive angle turns the
 *      chevron LEFT, always. That is not a defensible thing to reproduce: an
 *      arrow that cannot tell left from right is not answering the question it
 *      is on screen to answer. So this port gives the angle the sign of the
 *      cross product -- the standard companion of the dot the wrong-way test is
 *      already taking, computed from the same two vectors, not a new mechanism.
 *      DIRARROW_SIGNED 0 restores the original's one-sided arrow exactly.
 *
 *   2. HOW THE 3D ARROW IS DRAWN FLAT. The original renders two real meshes
 *      through a real camera; this file projects the two chevrons' own corners
 *      through that same camera by hand and submits QUADS through ui.c, because
 *      the whole of this port's HUD is quads through ui.c and a second 3D pass
 *      with its own viewport for an 18-triangle object is not worth what it
 *      costs. Nothing is approximated except the interpolation INSIDE each quad:
 *      the corners are exact. Each chevron is cut into DIRARROW_GRID^2 quads to
 *      bound that, and the bound is measured rather than asserted -- the worst
 *      deviation between ui.c's two-triangle map and the true projection, over
 *      both chevrons and every 5 degrees of rotation, on a 92.5 px viewport:
 *
 *          grid 1x1   3.60 px      grid 3x3   0.55 px
 *          grid 2x2   1.13 px      grid 4x4   0.32 px
 *
 *      3 is what ships: sub-pixel, and 18 quads a frame for the pair.
 *
 * THE BLINK'S SECOND TRIGGER USED TO BE THIS FILE'S THIRD INVENTION, and it is
 * not one any more. `record+0x58' is the CLOSEST the car has come to the
 * checkpoint it is heading for: FUN_004eb550 takes the straight-line distance to
 * checkpoint `record+0x4c', keeps the current value at +0x54 and folds it into
 * +0x58 with a min(), and FUN_004ea8d0 resets +0x58 to ARW_MIN_INIT when the
 * checkpoint advances. So the port's reading -- a distance -- was right in kind,
 * and wrong in detail: it used the CURRENT distance, which stops the blink as
 * soon as the car drives away, where the engine's minimum keeps it going until
 * the checkpoint changes. `cp_min' below is that minimum.
 *
 * AND THE SAME FUNCTION CARRIES A CUE NOTHING HERE HAD RAISED. Once per
 * checkpoint, when the minimum is under ARW_BESIDE_IN and the current distance
 * is back over ARW_BLINK_NEAR -- 4 m in, 5 m out, a hysteresis -- the engine
 * fires message-system sound 0x25e, and `Sound/' ships exactly one name for
 * that: **`cp_beside.wav'**. It is already in the port's packed bank (pack_snd.py
 * packs everything snd.dat names) and nothing had ever loaded it.
 *
 * The five ids the message system fires are consecutive and the bank names them
 * all, which is what makes the mapping more than a guess:
 *
 *     0x25c  the minimap module (0x4b7ac8)     0x25e  cp_beside
 *     0x25d  cp        (a checkpoint passed)   0x25f  cp_reset  (0x508b8b, the
 *     0x27c  cp_wrongway                              stuck-car reset)
 *
 * The chevrons' shape is NOT one of the port's own numbers. Each is six vertices
 * and four triangles, flat in one plane, and its UV turns out to be an exact
 * affine function of (x, z) -- gen_hud_data.py fits it and refuses a mesh where
 * it is not -- so one quad with a UV rect reproduces the mesh exactly, with the
 * V carved by coc_str_2's own alpha. The rect that falls out is precisely the
 * chevron's own ink box in the art, which is a free check on the entire reading.
 *
 * WHY IT IS ITS OWN FILE, and it is hud.c's and race_ui.c's reason: nothing
 * compiles main.c. Everything arrives as a plain number in `dirarrow_in' -- no
 * checkpoint.h, no rb.h, no ai.h, no GL headers -- so this links against ui.c
 * alone and ui_test's recorder can read back what really went on screen,
 * including which texture each quad came out of and where the apex landed.
 */

#ifndef DIRARROW_H
#define DIRARROW_H

#include "hud_data.h"

/* ------------------------------------------------------------ the port's own */

/* THE SIGN. 1 gives the arrow the cross product's sign, so it leans toward the
   turn; 0 is the engine's own unsigned angle, which leans left whichever way the
   track goes. See the header comment, point 1. */
#define DIRARROW_SIGNED 1

/* How many quads each chevron is cut into per axis, to bound the error of
   ui.c's two-triangle interpolation against the true perspective projection.
   Measured, not guessed -- the table in the header comment. */
#define DIRARROW_GRID 3

/* ------------------------------------------------------------------ the state */

typedef struct {
    unsigned int arrow;    /* coc_str_2, out of the load-once props scene */
} dirarrow_tex;

/* WHAT TO SHOW, filled by the caller from the checkpoint layer and the physics.
   Nothing in here is a pointer into either. */
typedef struct {
    /* 0 turns the whole thing off for this frame without disturbing its state:
       there is no spine, or the race has not started. The engine's own gate is
       `FUN_004e94e0' returning 0 and the follower being absent. */
    int   valid;

    float car_x, car_z;      /* world metres */
    float car_yaw;           /* radians, 0 faces +Z -- race_ui.c's convention */

    /* WHERE THE ARROW POINTS: the marker of the checkpoint being headed for.
       This is the engine's own fallback branch (0x4e90bb), which takes the
       vector from the car to checkpoint `follower + 0x4c' when the path follower
       has no waypoint to offer -- and the port's player has no follower, so the
       fallback is the whole of it rather than a substitute for it. */
    float aim_x, aim_z;

    /* The marker of the checkpoint last passed, for the ARW_WRONG_NEAR
       suppression. The engine takes checkpoint (next - 1), wrapping to the last
       one when next is 0, which is what the caller should pass. */
    float prev_x, prev_z;

    /* How far through the current checkpoint-to-checkpoint segment, 0 at the one
       just passed and 1 at the one ahead. Clamped here, so a caller with a
       ragged measure cannot push the tint off its ramp. */
    float seg_frac;

    /* Metres to the checkpoint being headed for -- the CURRENT distance, in 3D,
       which is what FUN_004eb550 measures. This file keeps the running minimum
       itself; the caller must not pre-minimise it or the beside cue can never
       see the car leave. */
    float cp_dist;

    /* WHICH checkpoint that is. The engine resets its minimum when the index
       advances (FUN_004ea8d0), and an index is the only way this file can know
       that happened -- it has no checkpoint.h. Any value works as long as it
       changes on a pass; the port passes `cps.next'. */
    int   cp_index;
} dirarrow_in;

typedef struct {
    dirarrow_tex tex;

    /* The engine's 44-byte record, by the offsets hud_data.h's comments name.
       Kept in the engine's units -- degrees, seconds, a 0..1 fraction -- so a
       number in a log can be compared with a number in the disassembly. */
    float ang;        /* +0x14: the angle the chevrons are drawn at, degrees */
    float ang_tgt;    /* +0x18: the angle they are turning toward */
    float prog;       /* +0x20: the smoothed segment fraction, 0..1 */
    float prog_tgt;   /* +0x24: the raw one */
    float wrong_t;    /* +0x28: the wrong-way timer, seconds */
    float cp_min;     /* +0x58: the closest the car has come, metres */
    int   cp_seen;    /* the index cp_min belongs to, so a pass can reset it */
    int   beside;     /* +0x5c: the beside cue has fired for this checkpoint */
    int   beside_cue; /* ... and the edge the host hangs cp_beside on */
    float blink;      /* +0x10: the blink's countdown; negative means steady */
    int   snap;       /* +0x0c: the fast slew is latched */
    int   snap_req;   /* +0x08: ... and something has asked for it */

    int   wrong;      /* what FUN_004afdb0 posts: the flag itself */

    /* Config.gm's `Cockpit/DirectArrow', which ships at 1. NOT wired to the
       port's SQUARE toggle: that button turns the port's own rendering
       experiments off, and this is the race telling the player where the track
       goes -- the same argument hud.c, countdown.c and race_ui.c are all off
       show_vis for. Here so that the config key has somewhere to land. */
    int   enabled;
} dirarrow_t;

/* -------------------------------------------------------------------- the API */

/* `tex' may carry zeroes; each element falls back to the compiled-in font or is
   skipped, the way hud.c and race_ui.c do, so a props.vsc packed without the new
   art degrades rather than crashing. */
void dirarrow_init(dirarrow_t *d, const dirarrow_tex *tex);

/* THE CAR HAS BEEN PUT SOMEWHERE rather than driven there: a restart, a
   respawn, a track change. Clears the timer, the banner and the blink and asks
   for a SNAP, so the arrow arrives pointing the right way instead of sweeping
   90 degrees at ARW_SLEW across the first second of the race. Keeps the
   textures, the way hud_reset does. */
void dirarrow_reset(dirarrow_t *d);

/* Once a frame, with that frame's dt -- so a dt of 0 with the menu open freezes
   the timer, the blink and the banner along with everything else. */
void dirarrow_step(dirarrow_t *d, const dirarrow_in *in, float dt);

/* Is the car going the wrong way? The flag itself, not whether the banner
   happens to be on screen this instant. */
int dirarrow_wrong(const dirarrow_t *d);

/* Likewise for `cp_beside': the car came within ARW_BESIDE_IN of the checkpoint
   it was heading for and has now got back outside ARW_BLINK_NEAR. Once per
   checkpoint, and an edge for the same reason. */
int dirarrow_beside_cue(dirarrow_t *d);

/* Is a chevron on screen this frame? The blink, and `enabled'. */
int dirarrow_visible(const dirarrow_t *d);

/* THE TINT both chevrons take, 0..1 per channel, from `prog'. Public because it
   is arithmetic worth measuring directly: ui_test reads it at the two ends and
   the middle of the ramp. */
void dirarrow_tint(const dirarrow_t *d, float *r, float *g, float *b);

/* WHERE ONE OF THE CHEVRON'S CORNERS LANDS, in the pixels ui.c draws in.
   `chev' is 0 or 1 (ARW_CHEV_Z's two rows), `fu' and `fv' run 0..1 across the
   chevron's own plan footprint -- fu 0 at ARW_CHEV_X0, fv 0 at the APEX. The
   projection is the engine's camera, by hand; this is the one piece of real
   arithmetic in the file and ui_test measures points through it rather than
   re-deriving the matrix. -> 0 and touches nothing if `chev' is out of range or
   the point falls behind the near plane. */
int dirarrow_project(const dirarrow_t *d, int chev, float fu, float fv,
                     int screen_w, int screen_h, float *out_x, float *out_y);

/* Both passes. Its own ortho pass, bracketed here, the way hud_draw and
   race_ui_draw bracket theirs. */
void dirarrow_draw(const dirarrow_t *d, int screen_w, int screen_h);

#endif /* DIRARROW_H */
