/*
 * antenna.h -- the whip antenna, which in the retail game is a simulated chain
 * and in this port used to be a rigid stick welded to the body.
 *
 * The game gives it a whole settings file per car, Settings/Antenna_Car<n>.crs,
 * loaded by FUN_00500230:
 *
 *     nPoints       4         masses in the chain
 *     chainLength   0.25 m    TOTAL, and the loader divides by nPoints-1 to get
 *                             the segment length -- 0.0833 m for the Overkill
 *     mass          0.25
 *     stiffness     10.0      raw 2000, clamped: every scalar is raw*0.01 with
 *                             a ceiling of 10, so 2000 lands on 10, not 20
 *     damping       1.16
 *     windFriction  4.60
 *     volume        0.03
 *
 * What is transcribed: the constants and their clamps, and the chain's shape --
 * nPoints masses, fixed segment length, anchored at the base.
 *
 * What is NOT: the engine's own integrator for it. That lives past the loader
 * and has not been read out. This is a position-based chain -- accelerate,
 * integrate, then enforce the segment lengths -- which is the standard way to
 * get a whip to behave and is stable at any timestep. The MOTION is the port's;
 * the numbers driving it are the game's.
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

typedef struct {
    /* chain state, in the car's LOCAL frame -- so the body's own motion enters
       only as the acceleration term, and a parked car's antenna is still */
    float p[ANT_MAX_POINTS][3];
    float v[ANT_MAX_POINTS][3];
    int   n;
    float seg;              /* segment length, metres */
    float stiffness, damping, wind, mass;

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

/* Advance the chain. `accel` is the body's linear acceleration in WORLD space
   and `m` its world matrix -- both are rotated into the car's frame here. */
void antenna_step(antenna_t *a, const float *m, const float accel[3],
                  float speed, float dt);

/* Rewrite the batch's vertices to follow the chain. Call once per frame after
   antenna_step, before the car is drawn. */
void antenna_apply(antenna_t *a);

#endif
