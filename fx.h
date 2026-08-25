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

/*
 * How many frames of DRAW buffer to keep, and why there has to be more than one.
 *
 * The pool's draw is one glDrawArrays over up to FX_MAX_PARTICLES * 6 vertices,
 * and the custom vitaGL (SAFER_DRAW_SPEEDHACK) stops copying a draw's vertices
 * into its own mapped temp once the draw passes 32 KB: past that line GXM is
 * handed the app's pointer and reads it AT FLUSH, which is after the swap. So a
 * single buffer refilled the next frame is read while the GPU is still drawing
 * the last one -- the exact mechanism that drew the big characters as exploding
 * spikes before char.c got its skin ring (see CHR_SKIN_RINGS, and known-issues).
 *
 * MEASURED, not assumed: the line is 32768 / 28 = 1170 vertices = 195 live
 * particles. With the app's own arrangement -- ONE shared pool and one emitter
 * per car, the player plus three opponents -- three seconds off beach_1's grid
 * peaks at 242 live particles, 40,656 B in one draw, over the line on 46 frames
 * of 480. The other nine tracks stay under (beach_2 22 particles, country_4 67),
 * so it is beach_1's sand that does it, and beach_1 is the first track.
 *
 * Three, because vitaGL's own circular vertex pool is one arena per display
 * buffer (gxm_display_buffer_count, 3 by default) reset at swap for the buffer
 * coming round again, so three is what says "the GPU is done with it".
 */
#define FX_DRAW_RINGS 3

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

/*
 * ONE EMITTER'S OWN STATE -- everything in the engine's system descriptor that
 * belongs to the CAR rather than to the pool of particles.
 *
 * It is split out of fx_t so that SEVERAL CARS can throw dust and smoke into one
 * shared pool. The engine has no such problem: it creates a whole particle system
 * per car, each with its own 2048 particles. Doing that here would be 123 KB and
 * a second draw call for every opponent, for a field of at most four cars whose
 * plumes composite into the same frame anyway -- so the port keeps ONE pool and N
 * emitters, which is the same trade it already makes between the dust and the
 * exhaust (see FX_SYS_*).
 *
 * Nothing in here may be shared between cars, and that is the whole point: the
 * carry fractions, the backfire hold and the smoke's colour ramp are per-system
 * in the original too. Sharing them makes two cars' emission rates beat against
 * each other and lets one car's backfire darken another car's smoke.
 */
typedef struct {
    /* One carry per emitter, exactly as FUN_00530b70 keeps one per system
       object: dropping it makes any rate below 1/dt emit nothing at all. */
    float carry_dust[RB_MAX_WHEELS];
    float carry_gas;

    /* the exhaust's own state, from the emitter object FUN_005303c0 walks */
    float explode_t;        /* +0x34  backfire hold, counts down */
    float gas_alpha;        /* +0x20  0..255, ramps at 256/s */
    int prev_jump;          /* +0x28  last frame's Jump bit, for the edge */

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
} fx_emitter;

typedef struct {
    fx_particle p[FX_MAX_PARTICLES];
    int n_live;             /* for telemetry */
    GLuint tex;
    int enabled;
    unsigned int seed;

    /* THE PLAYER'S emitter, so the common case needs no second object and every
       caller that had one fx_t per car is unchanged. An opponent brings its own
       and hands it to fx_emit. */
    fx_emitter em;
} fx_t;

/* FUN_0052e320 walks the cameras and gives up if none is within 12 m of the
   wheel. Public because main.c applies the same radius to a whole OPPONENT
   before it looks at that car's wheels at all -- the per-wheel test inside
   fx_emit would reject them one by one, having already posed the pose. */
#define FX_EMIT_RANGE 12.0f

/* `src` is the scene the `dust` texture was packed into (the car's). Also
   resets the built-in player emitter. */
void fx_init(fx_t *fx, const scene_t *src);

/* A fresh emitter: no carry, no backfire running, smoke at full white, pipe at
   the body origin pointing out the back. fx_init does this for fx->em; an
   opponent's emitter is initialised with it directly. */
void fx_emitter_init(fx_emitter *em);

/* The exhaust tip, in BODY space. Without it the smoke comes out of the body
   origin, which is the centre of mass. */
void fx_set_pipe(fx_emitter *em, const float p[3]);

/* The same, taken from the fitted booster's own `booster_<n>_end` node --
   pack_vsc.py names those so they survive the flattening. `booster` is 0..3, the
   menu's level. Returns 0 if this car has no such node, leaving the pipe where
   it was. Converts model space to body space on the way, which is NOT a no-op:
   `com_oy` is rbcar_com_oy(car), the recovered CenterMassOY. See the note in
   fx.c for why it is a parameter rather than something read off the rig. */
int fx_pipe_from_rig(fx_emitter *em, const carani_t *rig, int booster,
                     float com_oy);

/* SPAWN ONLY, for one car. `col` supplies the surface class under each wheel;
   `eye` is the camera, for the 12 m emit cull.
 *
 * Split from the ageing half BECAUSE THE POOL IS SHARED: ageing it once per
 * emitter would run every particle's life at N times the rate, so a field of
 * four cars would give the player a quarter of the dust it had. Call this for
 * every car that emits, then fx_age exactly once. */
void fx_emit(fx_t *fx, fx_emitter *em, const rb_car *c, const col_t *col,
             const float eye[3], float dt);

/* Move and age the whole pool. ONCE per frame, whatever the field size. */
void fx_age(fx_t *fx, float dt);

/* One frame for the player: fx_emit through the built-in emitter, then fx_age.
   Opponents are emitted BEFORE this call, so their particles age on the same
   tick the player's do. */
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
