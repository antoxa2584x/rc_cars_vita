/*
 * antenna.h -- the whip antenna.
 *
 * The retail engine simulates it with a general chain module that the PS2
 * *Smash Cars* ELF names in full: `carANTENNA_NEW` drives `_cdyCalcDynFrame`,
 * which is
 *
 *     weight  ->  _cdyAddWindForce  ->  the user-force callback  ->
 *     _cdyAddLocalStiffness (or _cdyAddGlobalStiffness)  ->  _cdyAddDamping  ->
 *     dynChainCalcAccelerations  ->  integrate  ->  _cdyTightenPoints  ->
 *     _cdyCalcPointWCS
 *
 * and every one of those is transcribed here. The numbers are the PC build's,
 * out of Settings/Antenna_Car<n>.crs (FUN_00500230):
 *
 *     nPoints       4         masses in the chain
 *     chainLength   0.25 m    TOTAL, and the loader divides by nPoints-1 to get
 *                             the segment length -- 0.0833 m for the Overkill
 *     mass          0.25      per point
 *     stiffness     10.0      raw 2000. The PC loader compares every scalar
 *                             against 1000.0 and saturates at 10.0, so 2000
 *                             lands on 10. (The PS2 build reads each key's
 *                             ceiling off its own slider range and gets 20 for
 *                             this one; the port ships PC data and takes 10.)
 *     damping       1.16
 *     windFriction  4.60      recovered and NOT used -- see below
 *     volume        0.03
 *
 * WHAT IS THE GAME'S, now:
 *
 *   - THE CHAIN IS IN WORLD SPACE with its first TWO points held fixed.
 *     `initInPos` sets `flags |= 1` on points 0 and 1, and a fixed point's
 *     velocity and acceleration are finite-differenced from the pose it is
 *     driven to rather than integrated. Two fixed points is what clamps the
 *     base DIRECTION, and it is why a retail antenna stands up off the boot
 *     instead of pivoting at its root.
 *   - BENDING IS AN ANGLE, not a displacement. `_cdyAddLocalStiffness` takes
 *     the angle between a segment and the previous segment's direction and
 *     applies `stiffness * angle / segmentLength` at right angles to the
 *     segment, equal and opposite on the two ends. A displacement spring --
 *     which is what this port had, with an invented gain of 75 to make the
 *     numbers work -- is a different law with a different stiffness dimension.
 *   - DAMPING IS RELATIVE TO THE PREVIOUS POINT'S FRAME, and divided by
 *     `dt * segmentLength`. `_cdyAddDamping` carries each point through
 *     inverse(previous frame of point i-1) and back out through the current
 *     one, and damps whatever is left over. At the engine's own 7 ms step that
 *     coefficient is ~2000 per metre of deviation, which is why a retail whip
 *     is crisp; a plain `v -= v*damping*dt` is 1.16 per second, which is a
 *     rope.
 *   - INEXTENSIBILITY IS A CONSTRAINT SOLVE, not a projection.
 *     `dynChainCalcAccelerations` assembles the tridiagonal system
 *         M[k][k]   = (w[k]+w[k+1]) |u[k]|^2
 *         M[k][k-1] = -w[k] dot(u[k-1],u[k])
 *         M[k][k+1] = -w[k+1] dot(u[k],u[k+1])
 *         R[k]      = dot(u[k], a[k+1]-a[k]) - |v[k]-v[k+1]|^2
 *     over the links (u[k] = p[k] - p[k+1], w = 1/mass and 0 where fixed),
 *     solves it with `_dynSolveSystem` (Thomas), and turns the multipliers
 *     into forces. `_cdyTightenPoints` then re-projects the positions onto the
 *     previous step's lengths and replaces each point's ALONG-SEGMENT velocity
 *     with its predecessor's.
 *   - THE STEP IS FIXED AT 7 ms, with a leftover accumulator and a remainder
 *     under 3 ms folded into the step, and the anchor pose SLERPED across the
 *     frame (`cgmSlerpRotMatrContinuous`). An anchor that jumps more than
 *     2.5 m in a frame re-plants the chain (`initInPos`).
 *   - THERE IS NO WIND. `getUserForceCB` writes the zero vector for every point
 *     but the tip, and the tip's is `_carAntennaDirZ` scaled by a literal 0.0f;
 *     `_cdyAddWindForce` is called with speed 0. The port used to push the whip
 *     back with `windFriction * speed`, which at 30 m/s is 55 times gravity on
 *     the tip -- that is the "rope in the air" this file was rewritten for.
 *
 * WHAT IS STILL THE PORT'S:
 *
 *   - The anchor points. The engine hangs an antenna INSTANCE off a mount
 *     offset in the car's frame; the port has the whip as geometry, so the two
 *     fixed points are the mesh's own base and one segment up its own axis
 *     (+Y). Same clamp, measured off the shipped model instead of a config.
 *   - The rotation interpolation is an nlerp on quaternions where the engine
 *     slerps. Over a 7 ms step the two differ by less than a thousandth of a
 *     degree.
 *   - A fixed point's velocity and acceleration are FORWARD differences here.
 *     The engine's two lines difference the other way round; the velocity only
 *     ever enters the solve squared, but the acceleration does not, and a whip
 *     that leans into the car's acceleration is not what a whip does.
 *   - `Height2Stiffness`, the spline that shapes stiffness along the whip,
 *     ships with no data in any of the three files and is not applied.
 *
 * The mesh is deformed rather than rotated. ANTENNA is a 26-triangle tapered
 * tube 0.267 m tall, and a whip that only pivots at its base reads as a rigid
 * stick on a hinge. pack_vsc.py gives it its own part (CAR_PARTS_EXTRA) so it
 * lands in its own batch and this file can reach the vertices: each one is
 * placed by its height fraction along the simulated curve.
 */

#ifndef ANTENNA_H
#define ANTENNA_H

#include "scene.h"
#include "rb.h"

#define ANT_MAX_POINTS 8

/* The engine's own step and its two thresholds -- process__14carANTENNA_NEW. */
#define ANT_STEP       0.007f   /* seconds, fixed */
#define ANT_STEP_FOLD  0.003f   /* a remainder shorter than this joins the step */
#define ANT_MAX_STEPS  8        /* the port's: a stalled frame must not spiral */
#define ANT_RESET_JUMP 2.5f     /* metres of anchor travel in one frame -> replant */

typedef struct {
    /* Three states in a ring: the step before last, the current one, and the
       one being written. `_cdyAddDamping` reads the previous state's frames
       against the current state's, so two have to survive a step. */
    float p[3][ANT_MAX_POINTS][3];   /* WORLD position */
    float v[3][ANT_MAX_POINTS][3];   /* WORLD velocity */
    float fr[3][ANT_MAX_POINTS][9];  /* the point's frame: rows X, Y, Z in world.
                                        X is the direction of the segment that
                                        arrives at it -- _cdyCalcNextPointWCS */
    int   cur;                       /* index of the current state */

    float f[ANT_MAX_POINTS][3];      /* force accumulator, one step's worth */
    float a[ANT_MAX_POINTS][3];      /* acceleration out of the tension solve */

    int   n;
    float seg;              /* segment length, metres */
    float stiffness, damping, wind, mass;

    float accum;            /* time not yet stepped */
    float m_prev[16], m_cur[16];  /* model->world, previous frame and this one */
    int   primed;

    /* the batch this bends, and the mesh's own extent along its axis */
    batch_t *batch;
    /* WHICH RIG PART it is, or -1 when the scene was packed without one. Kept
       because the main menu's car viewport has to frame the car WITHOUT the
       whip -- 0.38 m of wire over a 0.42 m truck, which owns the top half of
       the car's bounding box (scene_bounds, menu_car_draw). This file is the
       one place that knows how to find the part, so it is the one place that
       should say which it is. */
    int part;
    float base_y, tip_y;    /* model-space y of the mesh's bottom and top */
    float base[3];          /* model-space anchor (x, base_y, z) */
    int   ready;
} antenna_t;

/* Bind to the car scene's ANTENNA part. Does nothing if the scene was packed
   without it, in which case the antenna keeps drawing at rest. */
void antenna_init(antenna_t *a, scene_t *car, int car_index);

/* Advance the chain. `m` is the MODEL-to-WORLD matrix the car's geometry will
   be drawn under -- row-major row-vector, the engine's own layout -- and the
   only thing the chain is driven by: the anchor's motion through the world is
   where every inertial effect comes from. */
void antenna_step(antenna_t *a, const float *m, float dt);

/* Rewrite the batch's vertices to follow the chain. Call once per frame after
   antenna_step, before the car is drawn. */
void antenna_apply(antenna_t *a);

#endif
