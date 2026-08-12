/*
 * checkpoint.h -- the track's checkpoints and the arrow that marks the next one.
 *
 * A checkpoint in RC Cars is NOT a gate. FUN_004e9560 loads the folder
 * "Checkpoints" and reads cp_1, cp_2, ... ; each cp_N owns a chain of refining
 * points cp_N_1, cp_N_2, ... and the loader stitches
 *
 *     cp_N -> cp_N_1 -> ... -> cp_N_M -> cp_(N+1)
 *
 * into one closed polyline, accumulating a running length as it goes (the
 * DISTTOCP / DISTALL the network code prints). So the checkpoints are the
 * track's spine: waypoints, with intermediate points to bend the line between
 * them. beach_1 has four of them and 15 refining points.
 *
 * EVERY checkpoint is marked. FUN_0052b170 walks the registered list and tints
 * each one, and FUN_0052b1d0 hands back a flat alpha 50 for any checkpoint that
 * is not the current one; only the current one gets the breathing 50..220.
 * FUN_0052abc0 then draws the billboard for the current one over the top.
 *
 *     centre = mean of that checkpoint object's vertices -- which for a gate
 *              box centred on the marker is the marker's own position, and the
 *              marker is what the port has
 *     up     = world up, right = perpendicular to (centre - camera)
 *     quad   = centre + (0, Shift, 0) +/- Size on both axes
 *     texture= cp_ar_3_fN when the index is 0, cp_ar_2_fN otherwise --
 *              FUN_0052a9b0 calls them the "custom" and "common" arrows, and
 *              index 0 is the start/finish, so: red over the finish line, green
 *              over the rest. N = time/BlinkDelta % 3.
 *     alpha  = blink * k, where k ramps 0 -> 1 between MinDist and MaxDist and
 *              STAYS 1 beyond -- the marker is invisible up close and fully on
 *              far away, and is never culled for distance
 *
 * Every constant comes from anim_cp via vis_data.h.
 *
 * Two things are NOT transcribed:
 *
 *   - the progression rule. The game tracks laps, wrong way and the rest
 *     through its race module; here "which checkpoint is next" is simply
 *     proximity to the current one, advanced forward. Marked where it happens.
 *   - FUN_0052b170, which tints the checkpoint GATE OBJECTS by the same
 *     distance fade before the arrow is drawn. The port has no gate objects --
 *     the tracks carry the checkpoints as markers only -- so there is nothing
 *     to tint.
 */

#ifndef CHECKPOINT_H
#define CHECKPOINT_H

#include "col.h"
#include "scene.h"

/* The pulse half-period, from FUN_0052b1d0: the phase runs 0 -> 0.4 and the
   alpha 50 -> 250 with it. */
#define CP_PULSE_TIME 0.4f

/* Flat alpha for a checkpoint that is NOT the one being headed for.
   FUN_0052b1d0 opens with `cmp` on its two index arguments and `mov $0x32,%al`
   when they differ -- 0x32 is 50 -- and FUN_0052b170 applies it across the whole
   registered list. Every checkpoint is marked; only the current one breathes. */
#define CP_ALPHA_OTHER 140.f

/* Two PORT ADJUSTMENTS to the marker's placement, both driven by how it looked
   on screen rather than by anything recovered. Stated here so they are easy to
   find and easy to argue with.
 *
 * CP_SIZE_SCALE: FUN_0052abc0 builds the quad as centre +/- Size on both axes,
 * so Size 1.46 is a HALF-extent and the sprite is 2.92 m tall. That is seven
 * car lengths on a 0.42 m car and it towered. Halving it gives a 1.46 m marker.
 * The original's anchor is the mean of the checkpoint GATE OBJECT's vertices --
 * an object these tracks do not carry, since they store checkpoints as markers
 * only -- so the reference this was tuned against does not exist here.
 *
 * CP_GROUND: anchor the quad's BOTTOM on the terrain under the marker instead
 * of centring it at marker + Shift. The markers themselves float 0.18 to 0.49 m
 * above the ground, and Shift then lifted the whole sprite again.
 *
 * The recovered constants are untouched in vis_data.h; this is where they get
 * bent. */
#define CP_SIZE_SCALE 0.5f
#define CP_GROUND 1

#define CP_MAX 8
#define CP_MAX_POINTS 33          /* cp_N plus up to 32 edges, per the loader */

typedef struct {
    /* p[0] is cp_N itself -- the checkpoint, and where the arrow goes. p[1..]
       are its cp_N_M refining points, which bend the spine on the way to the
       next checkpoint and are only used for progression and distance. */
    float p[CP_MAX_POINTS][3];
    int n;
    float ground;                 /* terrain height under p[0], for the marker */
} cp_t;

typedef struct {
    cp_t cp[CP_MAX];
    int n;
    int next;                     /* the checkpoint being headed for */
    int lap;
    float t;
    GLuint tex_common[3];         /* cp_ar_2_f1..3, green, ordinary checkpoints */
    GLuint tex_custom[3];         /* cp_ar_3_f1..3, red, the start/finish line */
    int enabled;
} checkpoints_t;

/* Read the cp_* markers out of the scene and pick up the arrow textures. `col`
   may be NULL, in which case the marker's own y is used instead of the ground
   under it. */
void cp_init(checkpoints_t *c, const scene_t *scene, const col_t *col);

/* Advance the progression from the car's position. */
void cp_step(checkpoints_t *c, float x, float y, float z, float dt);

/* Draw the arrows. `eye` is the camera; the quad turns to face it. Call last,
   after everything else, and it leaves GL state as it found it. */
void cp_draw(checkpoints_t *c, const float eye[3]);

/* Metres along the spine from the car to the next checkpoint's centre. */
float cp_dist_to_next(const checkpoints_t *c, float x, float y, float z);

#endif
