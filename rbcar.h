/*
 * rbcar.h -- glue between the transcribed physics and the app.
 *
 * Builds an rb_car from the recovered per-car data (rb_data.h), drives it, and
 * exposes the pose. Everything here is app-side convenience; the model itself is
 * rb.c / contact.c / collide.c.
 */

#ifndef RBCAR_H
#define RBCAR_H

#include "rb.h"

/* How far above the caller's `y` rbcar_init will look for the surface to rest
   on. Wide enough to absorb float slop and a wheel probed a few centimetres off
   the body point, tight enough to exclude an overpass -- see the note on
   rb_world.ground, and beach_2, whose race start is under one. */
#define RBCAR_PLACE_CEIL 0.25f

/* car: 0 Overkill, 1 Buggy, 2 Hummer. `w` must outlive the car.
   `y` is the GROUND height at (x,z); the car is placed on top of it. */
void rbcar_init(rb_car *c, int car, const rb_world *w,
                float x, float y, float z, float yaw_deg);

/* One tick. throttle/brake in 0..1, steer in -1..1.
 *
 * Prefer rbcar_step_frame below: calling this with a measured frame time makes
 * the handling depend on the frame rate. See the comment there. */
void rbcar_step(rb_car *c, float throttle, float brake, float steer,
                int boost, float dt);

/*
 * The FIXED-TIMESTEP driver, and the only thing the app should call.
 *
 * rbcar_step passes its dt straight to rb_car_tick, and rb_car_tick cannot
 * always consume it. Its substeps are capped at RB_MAX_SUBSTEP (1/240) with a
 * budget of RB_MAX_SUBSTEPS (8), so it simulates at most 8/240 = 33.3 ms of
 * world per call however much wall time it is handed -- and it RETURNS how much
 * it managed, which rbcar_step threw away. Feed it a measured frame time and:
 *
 *   60 fps  16.7 ms  -> 4 substeps, fully consumed          real time
 *   30 fps  33.3 ms  -> 8 substeps, exactly at the budget    real time
 *   20 fps  50.0 ms  -> 8 substeps, 33.3 ms consumed         67% speed
 *   15 fps  66.7 ms  -> 8 substeps, 33.3 ms consumed         50% speed
 *
 * So below 30 fps the car went into slow motion in proportion to the frame
 * time, and above it the per-frame dt jitter changed the substep pattern from
 * frame to frame -- which is the reported "physics relies on fps".
 *
 * The fix is the standard accumulator: bank the real elapsed time and spend it
 * in whole RBCAR_TICK_DT ticks, so the model always sees the timestep the
 * harnesses measured it at, whatever the renderer is doing. One 1/60 tick is
 * 4 substeps, comfortably inside the budget of 8, so a tick is never starved.
 *
 * Over-long frames are CAPPED at RBCAR_MAX_CATCHUP ticks and the surplus is
 * dropped rather than banked. Banking it is the death-spiral: a frame that took
 * too long queues extra ticks, which make the next frame take longer still.
 * Dropping it means that below RBCAR_TICK_HZ / RBCAR_MAX_CATCHUP = 15 fps the
 * world does slow down -- but it slows down smoothly instead of exploding, and
 * every tick above that rate is bit-identical regardless of frame pacing.
 *
 * Returns the number of ticks run (0 when the frame was shorter than a tick and
 * the time was merely banked), or -1 if the catch-up cap clipped the frame,
 * which main.c logs.
 */
#define RBCAR_TICK_HZ      60.0f
#define RBCAR_TICK_DT      (1.0f / RBCAR_TICK_HZ)
#define RBCAR_MAX_CATCHUP  4

typedef struct { float acc; } rbcar_clock;

int rbcar_step_frame(rb_car *c, rbcar_clock *k, float throttle, float brake,
                     float steer, int boost, float frame_dt);

/* Drop any banked time. Call after anything that stalls the frame for reasons
   the world should not experience -- a track or car load, a respawn. */
void rbcar_clock_reset(rbcar_clock *k);

/* The Jump action, `held` being the raw button state. Call ONCE PER FRAME and
 * BEFORE rbcar_step -- that is where the original has it (FUN_004f6b20, which
 * the per-frame car loop runs ahead of the physics tick at 0x004f72f0), and it
 * has to be before the tick for a second reason: the impulse Jump adds to P is
 * only integrated if the tick that follows actually runs, and the rest clamp
 * skips the whole step unless an input bit says the driver did something.
 * Setting c->in.jump here is what tells it.
 *
 * Returns RB_JUMP_NONE / RB_JUMP_HOP / RB_JUMP_RESET. A RESET has moved the car,
 * so the caller should snap its camera -- the original does exactly that
 * (FUN_00508b50 calls camUpdate with a 2e6 dt). */
int rbcar_jump(rb_car *c, int held, float dt);

const char *rbcar_name(int car);
float rbcar_speed(const rb_car *c);

/* MODEL SPACE -> BODY SPACE, in one number: the rigid body's origin (its centre
 * of mass) sits `rbcar_com_oy(car)` metres ABOVE the model origin, so anything
 * holding a model-space Y converts by subtracting it and a renderer that has
 * already multiplied by rbcar_matrix() draws the model at
 * glTranslatef(0, -rbcar_com_oy(car), 0).
 *
 * This is the game's own CenterMassOY (cdt[42]) -- 0.0000 m on the Overkill,
 * 0.0323 on the Buggy and Hummer. See rb_data.h for the recovery. It replaces
 * carani_wheel_plane_y(), which measured the MESH's wheel-centre plane and was
 * the right answer only while gen_rb_data.py parked the com on that plane. */
float rbcar_com_oy(int car);

/* Body-to-world matrix, ready for glMultMatrixf: the engine's row-major
   row-vector layout is bit-identical to OpenGL's column-major column-vector
   layout, so no transpose is needed. */
const float *rbcar_matrix(const rb_car *c);

/* Heading in degrees for the chase camera, measured the way the app's old
   vehicle_t did: forward = (sin yaw, 0, -cos yaw). */
float rbcar_yaw_deg(const rb_car *c);

#endif
