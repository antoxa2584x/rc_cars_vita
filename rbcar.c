#include "rbcar.h"
#include "rb_data.h"
#include <math.h>
#include <string.h>

const char *rbcar_name(int car)
{
    if (car < 0 || car > 2) car = 0;
    return RB_CARS[car].name;
}

float rbcar_speed(const rb_car *c)
{
    return sqrtf(c->body.v[0] * c->body.v[0] + c->body.v[1] * c->body.v[1]
                 + c->body.v[2] * c->body.v[2]);
}

const float *rbcar_matrix(const rb_car *c)
{
    return c->m;
}

float rbcar_yaw_deg(const rb_car *c)
{
    /* The RENDERER's view-yaw convention: the view matrix's Ry(-yaw) makes a
       camera at yaw v face (-sin v, 0, -cos v), so the yaw that faces the car's
       forward (body local +Z, matrix row 2) is atan2(-m[8], -m[10]).
       See the note in cam.c -- getting this wrong put the car behind the camera
       as soon as it turned. */
    return atan2f(-c->m[8], -c->m[10]) * 57.295776f;
}

void rbcar_init(rb_car *c, int car, const rb_world *w,
                float x, float y, float z, float yaw_deg)
{
    const rb_car_data *d;
    int i;

    if (car < 0 || car > 2) car = 0;
    d = &RB_CARS[car];

    memset(c, 0, sizeof(*c));
    c->car_index = car;
    c->world = w;
    c->nwheels = d->nwheels;
    c->susp_enabled = 1;
    c->rest_damp = 1;
    c->max_contacts = 4;
    c->tune = d->tune;

    /* phys+0x08, the suspension extension-rate ramp. It is UNRECOVERED: only the
     * resets are transcribed (see rb_susp_ramp_reset), the writer is not, so this
     * value is the port's stand-in and it is a free parameter. It is used as
     * `rate = dt*radius * (1 + 18*ramp)`, so 0 is 1x and 0.5 is the full 10x.
     *
     * MEASURED, not chosen. It used to sit at 0.5 -- the top of the range, picked
     * on the reasoning that a moving car wants the fast extension. That turned
     * out to be the worst value available: driving flat out on FLAT ground, the
     * car porpoised in a sustained 1.5 Hz limit cycle, 131 mm of ride height peak
     * to peak, with the front strut swinging from near-droop to bottomed on
     * `len_min` every cycle. Sweeping the parameter:
     *
     *      ramp   drive p2p   parked tilt p2p (1 / 5 / 10 deg)
     *      0.07     43 mm      8.98  10.94   9.12      <- unstable parked
     *      0.08      7 mm      0.02   0.01   0.07
     *      0.10     10 mm      0.02   0.01   0.04      <- here
     *      0.12     16 mm      0.02   0.01   0.05
     *      0.50    131 mm      0.03   0.01   0.05      <- was here
     *
     * There is a cliff just below 0.08 where the PARKED behaviour goes unstable,
     * and a monotonic worsening of the DRIVING bounce above it, so the usable
     * window is narrow. 0.10 sits clear of the cliff rather than on the minimum.
     * All three cars improve: Overkill 131 -> 10 mm, Buggy 104 -> 30, Hummer
     * 50 -> 29, with parked stability unchanged for each.
     *
     * AND YET IT STAYS AT 0.5, because a third criterion kills it. Lowering the
     * ramp also collapses what little yaw response the model has, and at 0.10 it
     * flips the SIGN: `steer +1.0` went from -6.3 deg of yaw over three seconds
     * to +0.7, i.e. the car turns the wrong way. rb_test's steering check caught
     * it -- that check guards a convention bug this port has had four times, and
     * trading it for a smoother ride is not a trade worth making.
     *
     *      ramp   drive p2p   yaw over 3 s at full lock
     *      0.08      7 mm      +1.1   <- wrong way
     *      0.10     10 mm      +0.7   <- wrong way
     *      0.15     26 mm      -0.5   <- wrong way
     *      0.25     67 mm      -2.9
     *      0.50    131 mm      -6.3   <- here
     *
     * So the bounce is DIAGNOSED, NOT FIXED, and this parameter is not where the
     * fix lives. Two things are worth knowing for whoever picks it up:
     *
     *  - The loop is timestep-independent (131 mm at both 1/60 and 1/480), it is
     *    unaffected by drag, the contact solve, the suspension torque and the
     *    rest damper, and `carUpdateSuspension`'s branch condition and
     *    `suspExtend` were both checked against the disassembly and MATCH. It is
     *    not an integration artefact and not a transcription error in those.
     *  - `len_extra` is read by the spring force and NOTHING in this port ever
     *    writes it. In the original it is written by the untranscribed contact
     *    code. That, and the body contact solve, are the outstanding candidates.
     *
     *  - Separately: -6.3 deg over three seconds is itself far less yaw than the
     *    0.93 rad/s this model is documented as producing. Cornering strength is
     *    a second open question; rb_test only ever checked the SIGN. */
    c->susp_ramp = 0.5f;

    c->body.mass = d->mass;
    c->body.inv_mass = 1.0f / d->mass;

    /* Wheel order is FL, FR, RL, RR (carDriveForces drives indices 2 and 3), so
       +Z must be forward and the rear pair must be the negative-Z pair. A
       six-wheeler gets its middle axle between the two. */
    for (i = 0; i < c->nwheels; i++) {
        float sx = (i & 1) ? d->half_track : -d->half_track;
        float sz;
        if (c->nwheels <= 4)
            sz = (i < 2) ? d->half_base : -d->half_base;
        else
            sz = (i < 2) ? d->half_base : (i < 4 ? -d->half_base : 0.0f);
        c->wheel[i].mount[0] = sx;
        c->wheel[i].mount[1] = d->mount_y;
        c->wheel[i].mount[2] = sz;
        c->wheel[i].radius   = (i >= 2) ? d->tune.cdt_rad_back : d->tune.cdt_rad_wheel;
        c->wheel[i].k_speed  = d->k_speed;
        c->wheel[i].len_free = d->len_free;
        c->wheel[i].len_min  = d->len_min;
        c->wheel[i].len_max  = d->len_max;
        c->wheel[i].sag      = d->sag;
        /* start at the static equilibrium length: starting fully compressed is an
           invalid configuration that suspRetract reports as stuck */
        c->wheel[i].len      = d->len_free - d->sag;
    }
    rb_car_setup_springs(c);

    /* Inertia. The original's source for this is not recovered, so build it from
     * the geometry that IS: half the mass as a box over the body mesh extents,
     * half as point masses at the wheel centres.
     *
     * Body-box-only is what this used to do and it is badly wrong for roll: the
     * wheels sit at +-half_track, and leaving them out gives about a quarter of
     * the real roll inertia, which makes the car flip on a 10-degree slope. The
     * suspension roll torque is amplified by coeffMomentOZ (1.87) on top.
     */
    {
        double ex = d->extent[0], ey = d->extent[1], ez = d->extent[2];
        double mb = (double)d->mass * 0.5;          /* body share */
        double mw = ((double)d->mass * 0.5) / (double)c->nwheels;
        double ix = mb * (ey * ey + ez * ez) / 12.0;
        double iy = mb * (ex * ex + ez * ez) / 12.0;
        double iz = mb * (ex * ex + ey * ey) / 12.0;
        for (i = 0; i < c->nwheels; i++) {
            double wx = c->wheel[i].mount[0];
            double wy = (double)c->wheel[i].mount[1] - c->wheel[i].len;
            double wz = c->wheel[i].mount[2];
            ix += mw * (wy * wy + wz * wz);
            iy += mw * (wx * wx + wz * wz);
            iz += mw * (wx * wx + wy * wy);
        }
        memset(c->body.ibody_inv, 0, sizeof(c->body.ibody_inv));
        c->body.ibody_inv[0]  = (float)(1.0 / ix);
        c->body.ibody_inv[5]  = (float)(1.0 / iy);
        c->body.ibody_inv[10] = (float)(1.0 / iz);
        c->body.ibody_inv[15] = 1.0f;
    }

    /* Pose: yaw about Y, with the wheels resting on the ground at `y`.
       The wheel centre sits at mount_y - len in body space, so for it to be one
       radius above the ground the body origin must be at
       ground + radius + len - mount_y. Getting this wrong drops the car onto its
       own suspension at spawn, which on a slope is enough to tip it.
       If the world offers a ground probe, the block further down then aligns the
       body to the surface and lifts it clear. */
    c->body.x[0] = x;
    c->body.x[1] = y + c->wheel[0].radius + c->wheel[0].len - c->wheel[0].mount[1];
    c->body.x[2] = z;
    {
        double h = (double)yaw_deg * 0.017453292 * 0.5;
        c->body.q[0] = (float)cos(h);
        c->body.q[1] = 0.0f;
        c->body.q[2] = (float)sin(h);
        c->body.q[3] = 0.0f;
    }
    rb_car_update_matrix(c);

    /* Rest it ON the surface, aligned to it.
     *
     * Placing a car flat at one ground height is fine on a plane and wrong on a
     * slope: the uphill wheels end up buried and the downhill ones floating. On
     * the beach track the spawn is a 23-degree slope, and a car started that way
     * tumbles before the suspension can sort it out.
     *
     * Tilt the body to the surface normal, then raise it until no wheel is
     * inside the ground.
     */
    if (w && w->ground) {
        float gy, gn[3];
        /* Probe only just above the height the caller placed us at. `y` IS the
           ground it chose; anything within PLACE_CEIL of it is the same surface,
           and anything higher is a bridge or roof the car is meant to be under.
           See the note on rb_world.ground. */
        if (w->ground(w->ctx, x, z, y + RBCAR_PLACE_CEIL, &gy, gn)) {
            double nl = sqrt((double)gn[0] * gn[0] + (double)gn[1] * gn[1]
                             + (double)gn[2] * gn[2]);
            if (nl > 1e-06) {
                double ax[3], s2, cth, half;
                gn[0] = (float)(gn[0] / nl);
                gn[1] = (float)(gn[1] / nl);
                gn[2] = (float)(gn[2] / nl);
                /* shortest rotation from world up to the surface normal */
                ax[0] = -(double)gn[2];   /* cross((0,1,0), n) */
                ax[1] = 0.0;
                ax[2] =  (double)gn[0];
                s2 = sqrt(ax[0] * ax[0] + ax[2] * ax[2]);
                cth = gn[1];
                if (cth > 1.0) cth = 1.0;
                if (cth < -1.0) cth = -1.0;
                if (s2 > 1e-06) {
                    float qt[4], qy[4], qr[4];
                    half = acos(cth) * 0.5;
                    qt[0] = (float)cos(half);
                    qt[1] = (float)(ax[0] / s2 * sin(half));
                    qt[2] = 0.0f;
                    qt[3] = (float)(ax[2] / s2 * sin(half));
                    memcpy(qy, c->body.q, sizeof(qy));
                    rb_quat_mul(qt, qy, qr);      /* tilt after yaw */
                    memcpy(c->body.q, qr, sizeof(qr));
                    rb_quat_normalize(c->body.q);
                    rb_car_update_matrix(c);
                }
            }
            /* Sit the body at the average ground height under the four wheels,
             * with every strut at its static rest length.
             *
             * Two earlier attempts here were worse. Lifting until no wheel is
             * inside the ground leaves one wheel touching and three hanging --
             * that gives a quarter of the support force and a torque about the
             * centre of mass that never reverses (measured: a constant 1.53 Nm,
             * 62 rad/s^2, yaw rate 0 -> 7.9 rad/s in twelve frames). Setting each
             * strut from its own patch of ground is worse still where the ground
             * under the four wheels varies by more than the 0.098 m of travel,
             * which it does at this track's spawn: the struts came out at
             * 83/31/218/61 mm, one of them jammed at lenMax.
             *
             * So: average, symmetric, all four loaded equally. It is not exact on
             * uneven ground, and the suspension solve is left to sort out the
             * residual, which is what it is for.
             */
            {
                double sum = 0.0;
                int nw = 0;
                for (i = 0; i < c->nwheels; i++) {
                    float mw[3], wy, wn[3];
                    int k;
                    for (k = 0; k < 3; k++)
                        mw[k] = (float)((double)c->m[0 + k] * c->wheel[i].mount[0]
                                        + (double)c->m[4 + k] * c->wheel[i].mount[1]
                                        + (double)c->m[8 + k] * c->wheel[i].mount[2]
                                        + c->m[12 + k]);
                    if (w->ground(w->ctx, mw[0], mw[2],
                                  y + RBCAR_PLACE_CEIL, &wy, wn)) {
                        sum += wy;
                        nw++;
                    }
                }
                if (nw) {
                    double rest = (double)c->wheel[0].len_free - c->wheel[0].sag;
                    double gavg = sum / (double)nw;
                    for (i = 0; i < c->nwheels; i++)
                        c->wheel[i].len = (float)rest;
                    c->body.x[1] = (float)(gavg + (double)c->wheel[0].radius
                                           + rest - c->wheel[0].mount[1]);
                    rb_car_update_matrix(c);
                }
            }
        }
    }

    rb_update_inv_inertia_world(&c->body);
}

/* Deliberately NOT folded into rbcar_step, for two reasons. The original keeps
   them apart the same way -- carJump is called from the frame loop, not from
   whatever sets the control bits -- and rbcar_step already has seventeen call
   sites across the harnesses in rccars_re, none of which has a jump to pass. */
int rbcar_jump(rb_car *c, int held, float dt)
{
    c->in.jump = held ? 1 : 0;
    return rb_car_jump(c, dt);
}

void rbcar_step(rb_car *c, float throttle, float brake, float steer,
                int boost, float dt)
{
    const rb_car_data *d = &RB_CARS[c->car_index];
    float target, rate, delta;

    c->in.accel        = (throttle > 0.01f);
    c->in.throttle     = throttle;
    c->in.brake        = (brake > 0.01f);
    c->in.brake_amount = brake;
    c->in.boost        = boost;
    c->in.blocked      = 0;

    /* Steering. The real thing is carSteering (0x004f2d60), which drives this
       through a per-car spline and is not transcribed; this is a rate-limited
       approach to the stick position, clamped to AngleSteer.
     *
     * NOTE THE SIGN. `steer` is the stick: positive means RIGHT. The body's steer
     * angle has the opposite sense, because carWheelFrame rotates the wheel's
     * local +Z toward +X for a positive angle, and +X is the car's LEFT -- the
     * mesh puts WHEEL_FRONT_LEFT at x = +0.141 and WHEEL_FRONT_RIGHT at -0.141.
     * So a positive stick has to become a negative body angle. Without this the
     * car steers away from the stick, which reads as the car's left and right
     * being swapped relative to the camera. */
    target = -steer * d->steer_max_deg;
    rate   = d->steer_max_deg * 4.0f;          /* full lock in a quarter second */
    delta  = target - c->steer;
    if (delta > rate * dt)       c->steer += rate * dt;
    else if (delta < -rate * dt) c->steer -= rate * dt;
    else                         c->steer = target;

    /* The suspension extension-rate ramp, from the same per-frame car loop
       (0x004f6ea0, store at 0x004f6fd8) and for the same reason it lives there:
       it is a frame timer, not a substep one. carUpdateSuspension resets it. */
    rb_susp_ramp_advance(c, dt);

    rb_car_tick(c, dt);

    /* Roll the wheels for the renderer. The original does this from the car's
       frame update (0x004f6ea0) AFTER the physics, once per frame rather than
       once per substep, and so does this. */
    rb_wheel_spin_update(c, dt);
}

void rbcar_clock_reset(rbcar_clock *k)
{
    k->acc = 0.f;
}

/* See rbcar.h for why this exists and what it is protecting against. */
int rbcar_step_frame(rb_car *c, rbcar_clock *k, float throttle, float brake,
                     float steer, int boost, float frame_dt)
{
    int ticks = 0, clipped = 0;

    if (frame_dt > 0.f)
        k->acc += frame_dt;

    /* Everything rbcar_step does per call -- the steering rate limit, the
       suspension ramp, the wheel spin -- is a rate times dt, so N ticks of
       RBCAR_TICK_DT come to the same total as one tick of N * RBCAR_TICK_DT.
       That is what makes it safe to spend a frame as several ticks. */
    while (k->acc >= RBCAR_TICK_DT) {
        if (ticks >= RBCAR_MAX_CATCHUP) {
            /* Drop the surplus rather than bank it -- see rbcar.h. */
            k->acc = 0.f;
            clipped = 1;
            break;
        }
        k->acc -= RBCAR_TICK_DT;
        rbcar_step(c, throttle, brake, steer, boost, RBCAR_TICK_DT);
        ticks++;
    }
    return clipped ? -1 : ticks;
}
