/*
 * carani.h -- the car's rig. All three animation procs, transcribed:
 *
 *   carAniProc1  0x00504820   Car1, Overkill   solid axles on a pivot
 *   carAniProc2  0x00505780   Car2, Buggy      double wishbone per corner
 *   carAniProc3  0x005068e0   Car3, Hummer     proc1 plus a third, sliding axle
 *
 * The body matrix alone puts a rigid lump on the track. The game articulates the
 * suspension on top of it, and every input comes out of state the transcribed
 * physics already has -- the steer angle at phys+0x5c6c, the wheel spin at the
 * wheel record's +0x04 and the per-wheel spring length.
 *
 * proc1:
 *   AXLE_FRONT_*_SUPPORT   rotY(steer)
 *   WHEEL_*                rotZ(spin)
 *   FRONT_AXLE/REAR_AXLE   rotZ(pitch) then rotX(tilt)   from wheel-centre heights
 *   SPRING_*               aim at a point on its axle, and stretch to reach it
 *
 * proc2 (no knuckle node at all -- the Buggy steers the wheel itself):
 *   WHEEL_FRONT_*          rotY(steer), then rotZ(spin)
 *   AXLE_*_UP / _DOWN      rotX(arm), the two wishbones swinging together
 *   AXLE_*_WHEEL           rotX(-arm), the upright cancelling them so the wheel
 *                          rides up and down without pitching
 *   Spring<n>_1 / _2       the two halves aim at each other
 *
 * proc3 (six wheels):
 *   AXLE_FRONT_*_SUPPORT   rotY(steer)
 *   WHEEL_MIDDLE_*         rotY(steer * 0.3), the middle pair steering with the
 *                          front at a third of the angle, and again with no
 *                          knuckle node to carry it
 *   WHEEL_*                rotZ(-spin) -- see spin_sign below
 *   FRONT_AXLE/REAR_AXLE   as proc1
 *   MIDDLE_AXLE            TRANSLATES vertically, then rotZ(roll)
 *   Spring<n>_1 / _2       as proc2, about a different axis
 *
 * The middle axle is the one that cannot pivot: its node origin sits 3.5 mm from
 * its own wheel line, so no rotation about it would raise those wheels at all --
 * which is exactly why the engine moves it and pivots the other two.
 *
 * The geometry side is `pack_vsc.py --rig`, which keeps these nodes addressable
 * after the scene is flattened (VSC4 parts). Vertices stay baked in model space,
 * so a part that is not animated draws exactly where it always did; a part that
 * IS animated draws under `rest_inv * animated_world`, which is what `draw` here
 * holds -- ready for glMultMatrixf after the body matrix.
 *
 * Everything is the engine's row-major ROW-vector 4x4: p' = p * M, translation
 * in row 3. That is the same memory layout as an OpenGL column-major
 * column-vector matrix, so nothing here is ever transposed.
 *
 * HOW AN ANGLE IS FOUND. The engine does not solve its linkages; at load it
 * SWEEPS each one from -90 to +89.9 degrees in 0.1 steps, records the wheel
 * node's height at each step (1800 floats per corner, FUN_005055b0 for proc2 and
 * FUN_005065c0 for proc3), and at runtime binary-searches that table for the
 * angle that puts the wheel where the physics says. The port solves the same
 * relation in closed form -- see carani_arc -- because the sweep is exactly a
 * point on a circle and the closed form is both shorter and free of the 0.1
 * degree quantisation. The engine's tables are built in each linkage's own
 * frame; the port's arcs are built in MODEL space, which is the frame it draws
 * in, so the sign of every angle falls out of the geometry instead of out of a
 * convention. (Checked against the engine anyway: its table for a left-hand
 * wheel decreases with the angle and for a right-hand one increases, and so do
 * these arcs.)
 */

#ifndef CARANI_H
#define CARANI_H

#include "rb.h"
#include <stdio.h>

/* 56 rather than 40. The Buggy packs 38 parts (4 wheels, 12 wishbone nodes, 8
   spring halves, the antenna, the root, four booster tips, and the four
   booster_<n> / compressor_<n> pairs that name the exhaust LEVELS), the Hummer
   37 and the Overkill 30 -- and 40 left the Buggy two spare, which is no margin
   for a rig that has now grown twice.

   The cost of getting this wrong also got worse when the exhausts became parts.
   carani_read_parts drops the overflow and keeps the stream in sync, so it used
   to cost a batch that silently stopped articulating; now a dropped part also
   costs carparts_bind the level it reads off that part's NAME, and leaves the
   batch pointing past rig.n at a draw matrix nothing initialised. */
#define CARANI_MAX_PARTS 56
#define CARANI_MAX_SPRINGS 8        /* proc1: SPRING_{FRONT,REAR}_1..4 */
#define CARANI_MAX_SPRING_PAIRS 6   /* proc2: 4 pairs, proc3: 6 */

/* SpringLength, Settings/WheelsVisual.crs raw 77, converted by carInitAniProc1
   as (raw-1)*0.0009090909 + 0.01. It is where the spring's aim point sits along
   its own +Z at rest, and the length the stretch is measured against. */
#define CARANI_SPRING_LEN 0.0790909f

/* Half the front angle is what the middle axle's wheels are steered by in the
   PHYSICS (carSetupSteering gives them a third); the ANIMATION uses this, read
   off the immediate at 0x005068f6. Kept separate because they are separate
   numbers in the original and only one of them is a third. */
#define CARANI_MIDDLE_STEER 0.3f

typedef struct {
    char  name[32];
    int   parent;           /* -1 for the root, which is always part 0 */
    float rest[16];         /* node -> model at rest */
    float rest_inv[16];
    float local[16];        /* rest relative to the parent part */
} carani_part;

/* The model-space height of one point as one part rotates about one of its own
   axes: y(t) = c + a*cos(t) + b*sin(t), which is what a point on a circle is.
   `carani_arc_solve` inverts it. See the header comment on how the angle is
   found, and arc_build in carani.c for the derivation. */
typedef struct { float a, b, c; } carani_arc;

typedef struct {
    carani_part part[CARANI_MAX_PARTS];
    int   n;

    /* 1, 2 or 3 -- which of the game's three animation procs this model wants.
       Chosen by the nodes the model actually HAS rather than by a car index,
       because that is the only difference between the three: the Buggy is the
       one with wishbones, the Hummer the one with a third axle. */
    int   proc;

    /* resolved by carani_bind; -1 when this model has no such node */
    int   wheel[RB_MAX_WHEELS];   /* indexed the way rb_car indexes its wheels */
    /* Which way that wheel node's own axle points, +1 or -1, taken from the sign
       of its rest row 2 . model X. The Overkill's and the Buggy's wheels are
       modelled with local +Z on model +X and the Hummer's on model -X, which is
       the whole reason proc3 spins them by -spin where proc1 and proc2 use
       +spin. Reading it off the mesh reproduces all three without a car table. */
    int   spin_sign[RB_MAX_WHEELS];
    int   support[2];             /* the front knuckles, [0] left [1] right */
    int   axle_front, axle_rear, axle_middle;
    /* rb_car wheel indices, ordered LEFT then RIGHT -- which is the order the
       axle tilt is computed in, and it is NOT the rb_car index order. */
    int   pair_front[2], pair_rear[2], pair_middle[2];

    /* proc2: the double wishbone, per rb_car wheel index. `arm` is the arc of
       the wheel node as the UP arm swings, and arm_rest_y its value at rest. */
    int   arm_up[RB_MAX_WHEELS], arm_down[RB_MAX_WHEELS], arm_knuckle[RB_MAX_WHEELS];
    carani_arc arm[RB_MAX_WHEELS];
    float arm_rest_y[RB_MAX_WHEELS];

    /* proc3, per axle: [0] front, [1] rear, [2] middle. Each arc is traced by
       that axle's LEFT wheel node -- `pitch` as the axle swings about its own Z
       and `tilt` about its own X -- and axle_rest_y is where that node sits at
       rest. proc3 inverts real geometry for all three axles (three tables per
       axle at 0x00506360) where proc1 divides the wheel drop by lenAxe and takes
       an arctangent; on this car lenAxe is 0.0894 against a real 0.132 m lever,
       so the arctangent overshoots the drop by 40%. That is invisible on the
       Overkill, whose proc really does use it, and glaring here, where it would
       make the middle pair -- the one axle that CAN be solved exactly -- move
       differently from the other two over the same bump.

       The middle axle has no usable pitch arc (see mid_heave) and rolls about
       its own Z, so index 2 uses `pitch` for the roll and leaves `tilt` unset. */
    carani_arc axle_pitch[3], axle_tilt[3];
    float axle_rest_y[3];
    /* How far the middle axle may slide, either way. */
    float mid_heave;

    int   spring[CARANI_MAX_SPRINGS];
    int   spring_axle[CARANI_MAX_SPRINGS];
    int   n_springs;
    float spring_aim[CARANI_MAX_SPRINGS][3];  /* in that axle's local frame */

    /* proc2/proc3: spring halves that aim at each other rather than at an axle.
       spring_axis is which row of the node's own matrix is its long axis --
       row 1 on the Buggy (mat4GetRow1 at 0x005058ec), row 0 on the Hummer
       (mat4GetRow0 at 0x00506a4c). Both confirmed against the meshes: each
       half's axis and the direction to its partner agree to a degree. */
    int   spair[CARANI_MAX_SPRING_PAIRS][2];
    int   n_pairs, spring_axis;

    float world[CARANI_MAX_PARTS][16];
    float draw[CARANI_MAX_PARTS][16];
} carani_t;

/* The angle, in DEGREES, that puts `y` on the arc. 0 at the arc's rest value,
   and saturated (not wrapped) once `y` is out of the circle's reach. */
float carani_arc_solve(const carani_arc *arc, float y);

/* Read `n` VSC4 part records from an already-positioned stream. Returns the
 * number stored (parts past CARANI_MAX_PARTS are read and dropped, so the stream
 * stays in sync for the batches that follow). */
int carani_read_parts(carani_t *r, FILE *f, unsigned int n);

/* Resolve the rig by NAME and by GEOMETRY, and precompute the spring aim points.
 * `c` supplies the wheel mounts, which is how each rb_car wheel index is matched
 * to a mesh node: sign of mount X picks the side, sign of mount Z the axle. Doing
 * it by index instead would be a coin flip -- rbcar_init lays wheel 0 out at
 * -half_track while the original's wheel 0 is the +X one. Safe to call again if
 * the car changes. */
void carani_bind(carani_t *r, const rb_car *c);

/* One frame. Reads steer, per-wheel spin and per-wheel spring length off the
 * car; writes `world` and `draw`. */
void carani_update(carani_t *r, const rb_car *c);

/* The same for the physics.c placeholder, which has no per-wheel suspension:
 * steers the knuckles and spins every wheel at one rate, axles and springs left
 * at rest. `steer_deg` follows the BODY convention (positive points the wheels
 * left), the same as rb_car.steer. */
void carani_update_flat(carani_t *r, float steer_deg, float spin_rad);

/* Everything at rest -- what to draw before anything has been bound. */
void carani_rest(carani_t *r);

/* How far ABOVE the model origin the mesh's wheel-centre plane sits: the mean
 * rest Y of the bound wheel nodes. 0 if nothing is bound.
 * 56.7 mm on the Overkill, 50-72 mm on the Buggy (unevenly, front to rear, so it
 * also sits nose-down) and 71.4 mm on the Hummer.
 *
 * IT IS NOT THE MODEL-SPACE -> BODY-SPACE SHIFT. It used to be: gen_rb_data.py
 * parked the centre of mass on this plane because CenterMassOY was unrecovered,
 * so a renderer that had multiplied by rbcar_matrix() drew the model at
 * -carani_wheel_plane_y(). CenterMassOY IS recovered now -- the body origin sits
 * `com_oy` above the MODEL origin, 0.0000 / 0.0323 / 0.0323 m -- so that shift is
 * rbcar_com_oy(car), a property of the car and not of the rig. See rb_data.h.
 *
 * What this is still good for is being the OTHER measurement: mount_y is built as
 * `wheel_y - com_oy + (len_free - sag)`, which puts a resting wheel centre in body
 * space exactly on the mesh's own wheel node, so this and com_oy must differ by
 * whatever the suspension is doing and nothing else. rccars_re/meshalign.c checks
 * that -- it settles each car on a flat plane and compares the drawn wheel centre
 * with the physics one through the very matrix main.c multiplies by -- and it is
 * the only thing tying rb_data.h's mount_y to main.c's draw offset. */
float carani_wheel_plane_y(const carani_t *r);

/* The four tyre widths, at 0x005738c8, read by FUN_0050bde0(level) -- which
   returns 100.0 for a level outside 0..3, a guard both its callers make
   unreachable by bounds-checking the level first. */
#define CARANI_TIRE_WIDTH_TABLE { 0.950000f, 1.070000f, 1.250000f, 1.350000f }

/* How wide the tyre is at the car's tuning level, as the SCALE the engine puts
 * on the wheel node's own axle row -- 0.95 at level 0, not 1.0.
 *
 * RECOVERED, and this file used to open by saying the original does not vary
 * tyre width at all. It does, from a typed table, and the same table sets the
 * width of the mark the tyre leaves:
 *
 *   - FUN_0050be40(model, modelIndex, tireLevel) looks the wheel nodes up BY
 *     NAME out of three tables -- PTR_s_WHEEL_FRONT_LEFT_00572fcc, _005731b8
 *     and _005733f0, four entries, four, and SIX for the Hummer -- reads each
 *     node's local matrix with FUN_00407200, and calls
 *     FUN_0040c9a0(m, 1, 1, (1/sz) * table[level], 1). Mode 1 multiplies the
 *     three basis ROWS, so that sets the node's Z row length to table[level]
 *     exactly and leaves its origin, its X and its Y alone;
 *   - FUN_0050bb60 is what calls it, and the level it passes is the argument
 *     its own error string calls "Car: wrong tire number", stored at
 *     phys+0xe454+model*0xb0 +0x8 -- rb_car.tire_upgrade, phys+0xe45c;
 *   - the mark reads the same field through FUN_0050bb30 and hands the same
 *     table entry to FUN_0052f990 as `param_9`, whose half-width is
 *     `param_9 * 0.05`. See trace.c.
 *
 * The SCALE IS ABSOLUTE, not a multiplier -- the engine divides out whatever
 * the node already had. It is applied here as a multiplier only because every
 * wheel node in all three shipped cars has a local scale of exactly 1.0 (the
 * 1.05 / 0.90 / 1.17 each car carries is on an ANCESTOR, and the engine never
 * touches that), so the two coincide. carani_bind's fixture asserts the 1.0.
 *
 * The three pieces of art this file used to cite as proof that no width varies
 * were read correctly and prove nothing about it: the tyre atlases really do
 * share their sidewall columns, the mark textures really do have ink spans of
 * 0.78 / 0.69 / 0.75 / 0.75, and the shop icons really do show one carcass. A
 * tyre upgrade is one mesh drawn at four widths, so no artwork could have
 * carried it. The grip ratio the width used to be derived from is still real
 * and still what rb_tire_grip reads; it just is not this.
 *
 * This is the ONE place a level becomes a width: carani_update scales the drawn
 * wheel by it and trace.c multiplies the mark's 0.05 m by it, exactly as the
 * engine's two callers of FUN_0050bde0 do.
 */
float carani_tire_width(const rb_car *c);

#endif /* CARANI_H */
