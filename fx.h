/*
 * fx.h -- the wheel dust and the exhaust smoke.
 *
 * Two particle systems, both the game's own, both drawing the same `dust`
 * sprite. Each is created by a short function that fills in a system descriptor
 * and hangs four callbacks off it:
 *
 *              dust                     exhaust gas
 *   create     FUN_0052e000             FUN_00530180
 *   config     FUN_0052ea60 car_dustx   FUN_005308f0 exhausted_gas
 *   emit       FUN_0052e320             FUN_005303c0     -- how many, where
 *   spawn      FUN_0052e810             FUN_005306d0     -- one new particle
 *   update     FUN_0052e960             FUN_00530800     -- move it
 *   scale      FUN_0052e180             FUN_00530330     -- size over life
 *
 * and the emit driver both share is FUN_00530b70: `n = rate*dt + carry`, spawn
 * n particles at the position the emit callback wrote, keep the fraction. Both
 * are gated by RCCars.crs's VIDEO_DustEff / VIDEO_ExhaustGas, which ship at 1.
 *
 * The constants are in fx_data.h, generated from the game's settings. What is
 * NOT recovered, and is marked at the point of use in fx.c:
 *
 *   - the base sprite size FUN_00477940 sets, which ScaleX/ScaleY multiply;
 *   - the billboard construction (the engine hands the sprite to its own
 *     particle renderer, which is not transcribed);
 * Which way the DynamicScale ramp runs used to be on that list. It is read off
 * the two scale callbacks now -- t is the remaining life in SECONDS and a
 * particle SHRINKS; see fx_scale.
 */

#ifndef FX_H
#define FX_H

#include "col.h"
#include "rb.h"
#include "scene.h"

/* local_1a0[2] = 0x800 in both FUN_0052e000 and FUN_00530180: 2048 particles
   per system. The port shares one pool between the two, so it is sized once. */
#define FX_MAX_PARTICLES 2048

/* Which system emitted a particle. The engine keeps the two pools apart
   entirely; the port shares one, so a particle has to say where it came from.
   Only ONE thing reads it -- the ZIgnoreRad hide test, which belongs to the
   dust system and to nothing else. See fx_draw. */
#define FX_SYS_DUST  0
#define FX_SYS_GAS   1

typedef struct {
    float x, y, z;          /* +0x10  position, world */
    float vx, vy, vz;       /* +0x28  velocity */
    float life, age;        /* +0x08 / +0x0c */
    float angle;            /* +0x4c  sprite roll, degrees */
    float spin;             /* the sign TwistSpeed is applied with */
    float sx, sy;           /* sprite half-size at birth, metres */
    float grow;             /* DynamicScale for this particle */
    unsigned char r, g, b, a;
    unsigned char used;
    unsigned char sys;      /* FX_SYS_*, the port's: the engine has two pools */
} fx_particle;

typedef struct {
    fx_particle p[FX_MAX_PARTICLES];
    int n_live;             /* for telemetry */
    GLuint tex;
    int enabled;

    /* One carry per emitter, exactly as FUN_00530b70 keeps one per system
       object: dropping it makes any rate below 1/dt emit nothing at all. */
    float carry_dust[RB_MAX_WHEELS];
    float carry_gas;

    /* the exhaust's own state, from the emitter object FUN_005303c0 walks */
    float explode_t;        /* +0x34  backfire hold, counts down */
    float gas_alpha;        /* +0x20  0..255, ramps at 256/s */
    int prev_jump;          /* +0x28  last frame's Jump bit, for the edge */
    unsigned int seed;

    /* Where the pipe is, in body space. Taken from the car's own
       `booster_<n>_end` node -- see fx_set_pipe. */
    float pipe[3];
    /* And which way it POINTS, body space, unit. The node's own +Z: every one
       of the 12 booster_<n>_end nodes across the three cars has its local +Z
       aimed out of the car, and not one points forward -- see
       fx_pipe_from_rig. Defaults to body -Z, which is what fx_set_pipe leaves
       and what a car with no rig gets. */
    float pipe_dir[3];
    int have_pipe;
} fx_t;

/* `src` is the scene the `dust` texture was packed into (the car's). */
void fx_init(fx_t *fx, const scene_t *src);

/* The exhaust tip, in BODY space. Without it the smoke comes out of the body
   origin, which is the centre of mass. */
void fx_set_pipe(fx_t *fx, const float p[3]);

/* The same, taken from the fitted booster's own `booster_<n>_end` node --
   pack_vsc.py names those so they survive the flattening. `booster` is 0..3, the
   menu's level. Returns 0 if this car has no such node, leaving the pipe where
   it was. Converts model space to body space on the way, which is NOT a no-op:
   see the note in fx.c. */
int fx_pipe_from_rig(fx_t *fx, const carani_t *rig, int booster);

/* One frame. Emits from every wheel in contact and from the pipe, then moves
   and ages everything. `col` supplies the surface class under each wheel;
   `eye` is the camera, for the 12 m emit cull. */
void fx_step(fx_t *fx, const rb_car *c, const col_t *col,
             const float eye[3], float dt);

/* Billboards, camera-facing. `right` and `up` are the view basis. */
void fx_draw(const fx_t *fx, const float eye[3],
             const float right[3], const float up[3]);

/* Emission rate in particles/second for one wheel, split out so the test can
   read it without a GL context. Returns 0 when the wheel raises nothing. */
float fx_dust_rate(const fx_t *fx, const rb_car *c, int wheel, int surface,
                   float kmh);

#endif
