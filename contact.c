/*
 * contact.c -- RC Cars tire, engine and contact forces, transcribed from
 * RCCars.exe. Companion to rb.c, which holds the rigid body and the integrator.
 *
 * This is the half of the model that makes the car a car rather than a brick:
 *
 *   normal load   suspension springs        (rb.c)
 *   longitudinal  engine drive at the two rear contact points
 *   lateral       a 2x2 simultaneous friction solve at one front + one rear
 *                 contact, each clamped to the tire's grip limit
 *   environment   quadratic drag per wheel in deep sand or water
 *
 * The lateral solve is Baraff's contact-force method, reduced to two contacts:
 * build the effective-mass matrix A, set the right-hand side to the negated
 * rate of change of sliding speed, solve A f = b, and apply f along the
 * friction direction. Reducing four wheels to two is the game's own choice --
 * carGatherContacts picks one wheel from each axle based on which way the car
 * is steering.
 *
 * Operation order inside expressions follows the original x87 code. Several
 * places look wrong and are faithful anyway; each is marked QUIRK with what the
 * textbook version would have been. Do not "fix" them -- they are what the
 * game's handling feels like.
 *
 * Build with -fno-fast-math -ffp-contract=off.
 */

#include "rb.h"
#include <math.h>
#include <string.h>
#ifdef RB_DEBUG_FRICTION
#include <stdio.h>
#endif

#define EPS      1e-06f
#define DEG2RAD  0.017453292f
#define RAD2DEG  57.295776f

/* ------------------------------------------------------------------------- */
/* curves                                                                    */
/* ------------------------------------------------------------------------- */

/* 0x0040f830, type tag 0x311 -- piecewise linear with a precomputed slope per
 * segment, clamped to the endpoint value outside the range. The .bspl files
 * carry only the control points; the slope is derived at load, so deriving it
 * here from consecutive points is equivalent.
 */
float rb_curve_eval(const rb_curve *cv, float x)
{
    int i;

    if (!cv || !cv->pt || cv->n <= 0)
        return 0.0f;
    if (x < cv->pt[0].in)
        return cv->pt[0].out;
    if (x >= cv->pt[cv->n - 1].in)
        return cv->pt[cv->n - 1].out;

    for (i = 0; i < cv->n - 1; i++) {
        if (x >= cv->pt[i].in && x < cv->pt[i + 1].in) {
            double dx = (double)cv->pt[i + 1].in - cv->pt[i].in;
            double slope;
            if (fabs(dx) < EPS)
                return cv->pt[i].out;
            slope = ((double)cv->pt[i + 1].out - cv->pt[i].out) / dx;
            return (float)((double)cv->pt[i].out + ((double)x - cv->pt[i].in) * slope);
        }
    }
    return cv->pt[cv->n - 1].out;
}

/* ------------------------------------------------------------------------- */
/* tires                                                                     */
/* ------------------------------------------------------------------------- */

/* 0x004ee130 -- a drift is on when the brake/handbrake is held AND the steering
 * angle has passed angleDriftOn. That is the handbrake-turn trigger. */
int rb_tire_drifting(const rb_car *c)
{
    if (c->in.brake && fabsf(c->steer) > c->tune.angle_drift_on)
        return 1;
    return 0;
}

/* 0x004ee180 -- grip = upgrade multiplier * axle coefficient, and the rear also
 * takes coeffDriftRear while drifting, which is what makes the tail let go.
 *
 * QUIRK: the original caches the rear result by writing it back into the shared
 * per-car tuning array (tire[2]) and reading it out again. Harmless -- the value
 * is recomputed on every call -- so this version just returns it.
 */
float rb_tire_grip(const rb_car *c, int rear)
{
    float up;

    if (c->tire_upgrade < 0 || c->tire_upgrade >= 4)
        return 0.0f;
    up = c->tune.tire_upgrade[c->tire_upgrade];

    if (rear) {
        double g = (double)up * c->tune.coeff_rear_tires;
        if (rb_tire_drifting(c))
            g = (double)c->tune.coeff_drift_rear * g;
        return (float)g;
    }
    return (float)((double)up * c->tune.coeff_front_tires);
}

/* ------------------------------------------------------------------------- */
/* engine                                                                    */
/* ------------------------------------------------------------------------- */

/* 0x0050b7f0 -- the capacity of the boost meter, in meter units.
 *
 * A linear remap of this car's booster strength onto 50..100 against a fixed
 * reference range, rounded to a whole number (the original adds 0.5 and runs it
 * through _ftol, then fild's it straight back, so the value is integral but
 * travels as a float). gen_rb_data.py derives boost_ref_lo / boost_ref_hi and
 * documents which cars they come from.
 *
 * On the shipped data this is 50 / 63 / 79 / 93 for the Overkill and
 * 50 / 67 / 83 / 100 for the Buggy and the Hummer.
 */
float rb_boost_capacity(const rb_tuning *tune, int lvl)
{
    double t, lo = tune->boost_ref_lo, hi = tune->boost_ref_hi;

    if (lvl < 0 || lvl >= 4)
        lvl = 0;
    t = (double)tune->boost_up * tune->booster_upgrade[lvl];

    if (t < lo)
        return RB_BOOST_CAP_MIN;
    if (t > hi)
        return RB_BOOST_CAP_MAX;
    if (fabs(hi - lo) < 1e-6)
        return 0.0f;
    return (float)floor((t - lo) * (double)RB_BOOST_CAP_MIN / (hi - lo)
                        + RB_BOOST_CAP_MIN + 0.5);
}

/* Fill the meter. The original's car-reset path does exactly this and no more --
 * 0x004f27e6 calls FUN_0050b7f0 and stores the result straight into phys+0x5740
 * -- so the arm timer keeps running across a respawn and the lockout is left to
 * clear itself, which it does on the next update now that the meter is full.
 * Call it AFTER setting boost_upgrade: the capacity depends on the level. */
void rb_boost_reset(rb_car *c)
{
    c->boost_tank = rb_boost_capacity(&c->tune, c->boost_upgrade);
}

/* 0x004f3800 -- the boost meter. Sets c->in.boost, which is what the engine
 * reads; see the header for the shape and for why the button is not it.
 *
 * Called once per FRAME, not per substep -- the original's call site is the
 * per-car frame loop at 0x004f7051, alongside the suspension ramp and the jump,
 * and it is passed the frame's dt.
 *
 * Two pieces of the original are transcribed as reached rather than as written:
 *
 *  - releasing the button clears the lockout (0x4f38bf) even though holding it
 *    through an empty meter does not. So tapping does get you a sliver of boost
 *    the moment the arm timer expires. It is worth about 27% duty against the
 *    21% the fill rate allows outright, so it is not an exploit, and it is what
 *    the machine code does.
 *  - the button-released path's "if the latch was set, reset the arm timer"
 *    (0x4f38cb) is UNREACHABLE: 0x4f3847, four instructions earlier on the same
 *    path, has already zeroed the latch. Written out here the same way, so the
 *    timer keeps running across a release exactly as it does in the original.
 */
void rb_boost_update(rb_car *c, int button, float dt)
{
    float cap = rb_boost_capacity(&c->tune, c->boost_upgrade);
    double rate;
    int lvl = c->boost_upgrade, engaged = 0;

    if (lvl < 0 || lvl >= 4)
        lvl = 0;

    /* 0x4f3831: this advances every frame, whatever the button is doing. */
    c->boost_arm_t += dt;

    if (!button) {
        c->boost_latch = 0;                       /* 0x4f3847 */
        c->boost_lock  = 0;                       /* 0x4f38bf */
    } else if (c->boost_arm_t > RB_BOOST_ARM_TIME) {   /* 0x4f3854, strict */
        if (c->boost_lock) {
            if (c->boost_tank > RB_BOOST_REARM) {      /* 0x4f3871, strict */
                engaged = 1;
                c->boost_lock = 0;
            }
            c->boost_latch = 1;
        } else if (c->boost_tank >= RB_BOOST_EMPTY) {  /* 0x4f388e */
            engaged = 1;
            c->boost_latch = 1;
        } else {
            /* Ran dry: lock out and restart the arm timer. */
            c->boost_lock  = 1;
            c->boost_arm_t = 0.0f;
            c->boost_latch = 1;
        }
    }

    /* 0x4f3926: the fill. This runs unconditionally -- while a burn is on, the
       drain below is applied on top of it, so the two rates subtract. The sign
       test is the original's own: the rate is a product of shipped positives,
       but a negative one drains toward empty instead. */
    rate = (double)c->tune.boost_up * c->tune.booster_upgrade[lvl]
           * RB_BOOST_FILL_SCALE;
    if (rate > -1e-6)
        c->boost_tank = rb_move_towards(c->boost_tank, cap, (float)rate, dt);
    else
        c->boost_tank = rb_move_towards(c->boost_tank, 0.0f,
                                        (float)fabs(rate), dt);

    if (engaged) {                                /* 0x4f398a */
        c->boost_time += dt;
        c->boost_tank  = rb_move_towards(c->boost_tank, 0.0f,
                                         c->tune.boost_down, dt);
        c->in.boost = 1;
    } else {                                      /* 0x4f3979 */
        c->boost_time = 0.0f;
        c->in.boost = 0;
    }
}

/* 0x004eea50 -- returns the commanded acceleration in m/s^2, and through
 * restrict_out the engine-braking magnitude from the <CAR>_RESTRICT curve.
 *
 * Reverse is a flat -brake*4 (or *6 with boost), tapered to nothing between
 * 2.8333 and 3.3333 m/s so the car cannot reverse indefinitely.
 *
 * Forward reads the <CAR> curve. The x axis is km/h scaled by the resonator
 * upgrade, and the y axis is m/s^2. Above speed_base_max * upgrade the limiter
 * returns zero outright.
 *
 * NOTE the last line: the positive drag magnitude is ADDED back. carDragForce
 * subtracts drag at the centre of mass every step, so the curve represents NET
 * acceleration at a given speed -- which is why the curves cross zero exactly
 * at each car's top speed.
 */
float rb_engine_accel(rb_car *c, float *restrict_out)
{
    const float *fwd = &c->m[8];      /* matrix row 2 = local Z */
    double throttle = 0.0, brake = 0.0;
    int accel = 0, braking = 0;
    double vfwd, speed, a, vmax, mult, denom, sscale, ascale;

    if (restrict_out)
        *restrict_out = 0.0f;

    if (c->in.accel) { accel = 1; throttle = c->in.throttle; }
    if (c->in.brake) { braking = 1; brake = c->in.brake_amount; }

    vfwd = (double)fwd[0] * c->body.v[0]
           + (double)fwd[1] * c->body.v[1]
           + (double)fwd[2] * c->body.v[2];

    if (c->gear < 0) {
        double k;
        if (!braking)
            return 0.0f;
        k = c->in.boost ? 6.0 : 4.0;
        speed = sqrt((double)c->body.v[0] * c->body.v[0]
                     + (double)c->body.v[1] * c->body.v[1]
                     + (double)c->body.v[2] * c->body.v[2]);
        a = -(fabs(brake) * k);
        if (speed < 2.8333335)
            return (float)a;
        if (speed <= 3.3333335)
            return (float)(-a * (speed - 2.8333335) * 2.0000002 + a);
        return 0.0f;
    }

    if (!accel)
        return 0.0f;

    if (c->reso_upgrade < 0 || c->reso_upgrade >= 4)
        return 0.0f;
    sscale = c->tune.resonator_speed[c->reso_upgrade];
    ascale = c->tune.resonator_accel[c->reso_upgrade];

    if (!c->in.boost) {
        vmax = c->tune.speed_base_max;
        mult = 1.0;
    } else {
        vmax = c->tune.speed_boost_max;
        mult = c->tune.boost_ratio;
    }

    denom = fabs(throttle) * mult * sscale;

    if (vmax * 0.2777778 * sscale <= vfwd) {
        a = 0.0;                      /* speed limiter */
    } else {
        if (fabs(denom) < EPS)
            return 0.0f;
        a = rb_curve_eval(&c->tune.accel, (float)((vfwd * 3.6) / denom));
        if (c->in.boost)
            a = a * 1.2;
        if (a < 0.0)
            a = 0.0;
        a = a * ascale;
    }

    if (restrict_out && c->tune.restrict_.pt) {
        double d = denom;
        if (c->in.boost && fabs(c->tune.boost_ratio) > EPS)
            d = d / c->tune.boost_ratio;
        if (fabs(d) >= EPS)
            *restrict_out = rb_curve_eval(&c->tune.restrict_,
                                          (float)((vfwd * 3.6) / d));
    }

    if (a <= 0.001)
        return (float)a;

    /* add back the drag the body will subtract this step (see note above) */
    {
        double sp = sqrt((double)c->body.v[0] * c->body.v[0]
                         + (double)c->body.v[1] * c->body.v[1]
                         + (double)c->body.v[2] * c->body.v[2]);
        double dragmag = 0.0;
        if (sp > 0.0001)
            dragmag = sp * c->tune.coeff_air_resistance * sp
                      + c->tune.coeff_friction_bearings;
        return (float)(dragmag + a);
    }
}

/* ------------------------------------------------------------------------- */
/* rigid-body derivatives used by the friction solve                         */
/* ------------------------------------------------------------------------- */

/* 0x00476ba0 -- alpha = Iinv * (tau - w x L). */
void rb_angular_accel(rb_body *b, const float pts[][3], int n,
                      const float f[][3], float out[3])
{
    float t[3];

    rb_update_inv_inertia_world(b);
    rb_sum_forces_torques(b, pts, n, f);

    t[0] = (float)(((double)b->w[2] * b->L[1] - (double)b->w[1] * b->L[2])
                   + b->torque[0]);
    t[1] = (float)(((double)b->L[2] * b->w[0] - (double)b->w[2] * b->L[0])
                   + b->torque[1]);
    t[2] = (float)(((double)b->w[1] * b->L[0] - (double)b->L[1] * b->w[0])
                   + b->torque[2]);

    rb_mat3_mul_vec3(b->iinv, t, out);
}

/* 0x00476c50 -- with dir given, returns dir * d/dt(v_point . dir), the rate at
 * which the sliding speed along dir is changing. That is the right-hand side
 * quantity a contact solve needs.
 *
 * QUIRK: the centripetal term is built from r x w and then crossed with w,
 * giving out += w x (r x w) = +w^2 * r_perp. The textbook point acceleration is
 * a + alpha x r + w x (w x r), i.e. the opposite sign. Reproduced as-is; it
 * only perturbs the friction RHS and only at high yaw rate.
 */
void rb_point_accel_along(rb_body *b, const float p[3], const float dir[3],
                          const float pts[][3], int n, const float f[][3],
                          float out[3])
{
    double rx, ry, rz;
    float alpha[3], u[3], vp[3];
    double ax, ay, az;

    out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f;

    rx = (double)p[0] - b->x[0];
    ry = (double)p[1] - b->x[1];
    rz = (double)p[2] - b->x[2];

    /* linear part: a = F/m */
    rb_sum_forces_torques(b, pts, n, f);
    ax = (double)b->inv_mass * b->force[0];
    ay = (double)b->inv_mass * b->force[1];
    az = (double)b->inv_mass * b->force[2];

    /* angular part: alpha x r  (this also refreshes force/torque) */
    rb_angular_accel(b, pts, n, f, alpha);
    ax += (double)alpha[1] * rz - (double)alpha[2] * ry;
    ay += (double)alpha[2] * rx - (double)rz * alpha[0];
    az += (double)alpha[0] * ry - (double)alpha[1] * rx;

    /* u = r x w, then out += w x u   (see QUIRK above) */
    u[0] = (float)(rz * b->w[1] - ry * b->w[2]);
    u[1] = (float)(rx * b->w[2] - rz * b->w[0]);
    u[2] = (float)(ry * b->w[0] - rx * b->w[1]);

    ax += (double)u[2] * b->w[1] - (double)u[1] * b->w[2];
    ay += (double)u[0] * b->w[2] - (double)u[2] * b->w[0];
    az += (double)u[1] * b->w[0] - (double)u[0] * b->w[1];

    out[0] = (float)ax; out[1] = (float)ay; out[2] = (float)az;

    if (!dir)
        return;

    /* s = a_point . dir + v_point . (w x dir)  =  d/dt (v_point . dir) */
    rb_point_velocity(b, p, vp);
    {
        double wd0 = (double)b->w[1] * dir[2] - (double)b->w[2] * dir[1];
        double wd1 = (double)b->w[2] * dir[0] - (double)dir[2] * b->w[0];
        double wd2 = (double)dir[1] * b->w[0] - (double)b->w[1] * dir[0];
        double s = (double)vp[0] * wd0 + (double)vp[1] * wd1 + (double)vp[2] * wd2
                   + ax * dir[0] + ay * dir[1] + az * dir[2];
        out[0] = (float)(s * dir[0]);
        out[1] = (float)(s * dir[1]);
        out[2] = (float)(s * dir[2]);
    }
}

/* 0x00408c20 plus the solve in carContactSolve -- 2x2 solve by explicit
 * inverse. Returns 0 if the system is singular. */
int rb_solve2(const float A[2][2], const float b[2], float f[2])
{
    double det = (double)A[1][1] * A[0][0] - (double)A[1][0] * A[0][1];
    double i0, i1, i3, i4;

    if (fabs(det) < EPS)
        return 0;

    det = 1.0 / det;
    i0 =  (double)A[1][1] * det;
    i4 =  det * A[0][0];
    i1 = -((double)A[0][1] * det);
    i3 = -((double)A[1][0] * det);

    f[0] = (float)((double)b[1] * i1 + i0 * b[0]);
    f[1] = (float)(i4 * b[1] + i3 * b[0]);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* contact frames                                                            */
/* ------------------------------------------------------------------------- */

static void vnorm3(float v[3], const float fallback[3])
{
    double n = sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]);
    if (n < EPS) {
        v[0] = fallback[0]; v[1] = fallback[1]; v[2] = fallback[2];
        return;
    }
    if (fabs(n - 1.0) >= EPS) {
        double inv = 1.0 / n;
        v[0] = (float)(v[0] * inv);
        v[1] = (float)(v[1] * inv);
        v[2] = (float)(v[2] * inv);
    }
}

/* 0x004ed6a0 -- build the contact frame for one wheel.
 *
 * The wheel's rolling direction is local +Z rotated about local Y by the steer
 * angle, then taken to world space. From that and the contact normal:
 *
 *   lat = normalise(dir x normal)      the friction direction
 *   fwd = normalise(normal x lat)      dir projected onto the contact plane
 *
 * so `lat` is purely lateral -- the friction solve resists cornering slip, and
 * the drive force pushes along `fwd`.
 */
void rb_contact_record(const rb_car *c, int wheel, float steer_deg,
                       rb_contact *out)
{
    static const float AXIS_X[3] = { 1.0f, 0.0f, 0.0f };
    const rb_wheel_contact *h = &c->hit[wheel];
    float local[3], dir[3];
    double s, k;
    int j;

    out->wheel = wheel;
    memcpy(out->point,  h->point,  3 * sizeof(float));
    memcpy(out->normal, h->normal, 3 * sizeof(float));

    for (j = 0; j < 3; j++)
        out->r[j] = (float)((double)out->point[j] - c->body.x[j]);

    /* local +Z (0,0,1) rotated about Y by steer_deg -> (sin, 0, cos) */
    s = sin((double)steer_deg * DEG2RAD);
    k = cos((double)steer_deg * DEG2RAD);
    local[0] = (float)s;
    local[1] = 0.0f;
    local[2] = (float)k;

    /* to world: row-vector multiply against the rotation rows of c->m */
    for (j = 0; j < 3; j++) {
        dir[j] = (float)((double)local[0] * c->m[0 + j]
                         + (double)local[1] * c->m[4 + j]
                         + (double)local[2] * c->m[8 + j]);
    }

    out->lat[0] = (float)((double)dir[1] * out->normal[2]
                          - (double)dir[2] * out->normal[1]);
    out->lat[1] = (float)((double)dir[2] * out->normal[0]
                          - (double)dir[0] * out->normal[2]);
    out->lat[2] = (float)((double)dir[0] * out->normal[1]
                          - (double)dir[1] * out->normal[0]);
    vnorm3(out->lat, AXIS_X);

    /* fwd = normal x lat */
    out->fwd[0] = (float)((double)out->lat[2] * out->normal[1]
                          - (double)out->normal[2] * out->lat[1]);
    out->fwd[1] = (float)((double)out->normal[2] * out->lat[0]
                          - (double)out->normal[0] * out->lat[2]);
    out->fwd[2] = (float)((double)out->normal[0] * out->lat[1]
                          - (double)out->lat[0] * out->normal[1]);
    vnorm3(out->fwd, AXIS_X);
}

/* ------------------------------------------------------------------------- */
/* environment drag                                                          */
/* ------------------------------------------------------------------------- */

/* 0x004eeea0 -- per-wheel quadratic drag from deep sand or standing water,
 * opposing the contact point's velocity. Water ramps in linearly between 30%
 * and 90% of the wheel radius submerged.
 */
void rb_surface_drag(rb_car *c, int *n, float pts[][3], float f[][3])
{
    int i;

    for (i = 0; i < c->nwheels && *n < RB_MAX_FORCES; i++) {
        const rb_wheel_contact *h = &c->hit[i];
        float p[3], vp[3];
        double sp, coeff, mag;

        if (!h->active)
            continue;

        memcpy(p, h->point, 3 * sizeof(float));
        rb_point_velocity(&c->body, p, vp);
        sp = sqrt((double)vp[0] * vp[0] + (double)vp[1] * vp[1]
                  + (double)vp[2] * vp[2]);
        if (sp >= EPS && fabs(sp - 1.0) >= EPS) {
            double inv = 1.0 / sp;
            vp[0] = (float)(inv * vp[0]);
            vp[1] = (float)(vp[1] * inv);
            vp[2] = (float)(vp[2] * inv);
        }

        if (!h->in_water || c->wheel[i].radius <= h->water_gap) {
            /* dry: only deep sand resists */
            if (h->surface != 3)
                continue;
            coeff = c->tune.coeff_deep_sand;
        } else {
            double depth = (double)c->wheel[i].radius - h->water_gap;
            double hi = (double)c->wheel[i].radius * 0.9;
            double lo = (double)c->wheel[i].radius * 0.3;
            coeff = c->tune.coeff_water;
            if (depth < lo) {
                coeff = 0.0;
            } else if (depth <= hi) {
                double span = hi - lo;
                coeff = (fabs(span) >= EPS) ? (coeff * (depth - lo)) / span : 0.0;
            }
        }

        {
            double v2 = (double)c->body.v[0] * c->body.v[0]
                        + (double)c->body.v[1] * c->body.v[1]
                        + (double)c->body.v[2] * c->body.v[2];
            mag = -(coeff * v2);
        }

        memcpy(pts[*n], p, 3 * sizeof(float));
        f[*n][0] = (float)(mag * vp[0]);
        f[*n][1] = (float)(vp[1] * mag);
        f[*n][2] = (float)(vp[2] * mag);
        (*n)++;
    }
}

/* ------------------------------------------------------------------------- */
/* drive                                                                     */
/* ------------------------------------------------------------------------- */

/* 0x004ee5e0 -- gear/direction logic, then the engine force at the rear
 * contacts. Returns nonzero when drive is inhibited, which carGatherContacts
 * uses to switch the friction direction from "lateral only" to "full slide".
 *
 * The car is REAR WHEEL DRIVE: the loop runs over wheels 2 and 3 only, and the
 * force is halved so the two corners share it.
 */
int rb_drive_forces(rb_car *c, float dt, int *n, float pts[][3], float f[][3])
{
    static const float UP[3] = { 0.0f, 1.0f, 0.0f };
    float grip_rear, engine_restrict = 0.0f;
    double a, drive, speed;
    int i;

    for (i = 0; i < c->nwheels; i++) {
        c->wheel[i].drive[0] = 0.0f;
        c->wheel[i].drive[1] = 0.0f;
        c->wheel[i].drive[2] = 0.0f;
    }

    grip_rear = rb_tire_grip(c, 1);

    speed = sqrt((double)c->body.v[0] * c->body.v[0]
                 + (double)c->body.v[1] * c->body.v[1]
                 + (double)c->body.v[2] * c->body.v[2]);

    /* direction change: you must be nearly stopped to swap gear, and while you
       are still fast the drive is blocked so the friction goes full-slide. */
    if (c->in.blocked) {
        c->drive_blocked = 1;
    } else {
        c->drive_blocked = 0;
        if (c->gear < 0) {
            if (c->in.accel) {
                if (speed <= 2.0) c->gear = 0;
                else              c->drive_blocked = 1;
            }
        } else if (c->in.brake) {
            if (speed <= 2.0) c->gear = -1;
            else              c->drive_blocked = 1;
        }
    }

    a = rb_engine_accel(c, &engine_restrict);

    /* traction limit: the tire can only put down so much */
    {
        double hi = (double)grip_rear * 20.0;
        double lo = (double)grip_rear * -20.0;
        if (a < lo)      a = lo;
        else if (a > hi) a = hi;
    }
    drive = a * c->body.mass;

    rb_surface_drag(c, n, pts, f);

    if (c->drive_blocked)
        return c->drive_blocked;
    if (fabs(drive) < 0.001)
        return 0;

    drive = drive * 0.5;
    engine_restrict = (float)((double)engine_restrict * 0.5);

    for (i = 2; i < 4 && i < c->nwheels && *n < RB_MAX_FORCES; i++) {
        const rb_wheel_contact *h = &c->hit[i];
        rb_contact rec;
        float fv[3];
        double ang, proj;

        if (!h->active)
            continue;

        /* skip contacts on steep faces: the normal must be within 46 deg of up */
        {
            double d = (double)UP[0] * h->normal[0] + (double)UP[1] * h->normal[1]
                       + (double)UP[2] * h->normal[2];
            if (d > 1.0) d = 1.0;
            if (d < -1.0) d = -1.0;
            ang = acos(d) * RAD2DEG;
            if (ang > 46.0)
                continue;
        }

        {
            float fsusp[3];
            rb_car_susp_force(c, dt, fsusp);
            rb_contact_record(c, i, 0.0f, &rec);

            fv[0] = (float)((double)rec.fwd[0] * drive);
            fv[1] = (float)((double)rec.fwd[1] * drive);
            fv[2] = (float)((double)rec.fwd[2] * drive);

            /* QUIRK: the original's dot product here omits the Y term. It is a
               two-term, horizontal-only projection of the suspension force onto
               the rolling direction. Verified in the disassembly at 0x4ee8fc. */
            proj = (double)fsusp[2] * rec.fwd[2] + (double)fsusp[0] * rec.fwd[0];
        }

        /* engine braking, applied only when rolling against the drive */
        {
            double d = (double)UP[0] * rec.fwd[0] + (double)UP[1] * rec.fwd[1]
                       + (double)UP[2] * rec.fwd[2];
            if (d > 1.0) d = 1.0;
            if (d < -1.0) d = -1.0;
            if (acos(d) * RAD2DEG < 80.0 && proj < 0.0) {
                double m = fabs(proj);
                double use = (engine_restrict <= 0.0f || engine_restrict <= m)
                             ? (double)engine_restrict : m;
                fv[0] = (float)((double)rec.fwd[0] * use + fv[0]);
                fv[1] = (float)((double)rec.fwd[1] * use + fv[1]);
                fv[2] = (float)((double)rec.fwd[2] * use + fv[2]);
            }
        }

        memcpy(c->wheel[i].drive, fv, 3 * sizeof(float));
        memcpy(pts[*n], rec.point, 3 * sizeof(float));
        memcpy(f[*n], fv, 3 * sizeof(float));
        (*n)++;
    }

    return c->drive_blocked;
}

/* ------------------------------------------------------------------------- */
/* contact selection                                                         */
/* ------------------------------------------------------------------------- */

/* 0x004ee280 -- apply the drive forces, then choose at most TWO contacts for
 * the lateral solve.
 *
 * With one or two wheels down, take them in order. With three or four down,
 * take one wheel from each axle, chosen by which way the car is steering -- so
 * the solve always sees one front and one rear contact, which is what makes a
 * 2x2 system enough to model cornering.
 *
 * Only the front wheels are given the steering angle; the rears run straight.
 */
int rb_gather_contacts(rb_car *c, float dt, rb_contact *rec, int *nrec,
                       int *n, float pts[][3], float f[][3])
{
    int active = 0, i, blocked;

    *nrec = 0;
    for (i = 0; i < 4 && i < c->nwheels; i++)
        if (c->hit[i].active)
            active++;
    if (c->max_contacts < active)
        active = c->max_contacts;
    if (active <= 0)
        return 0;

    blocked = rb_drive_forces(c, dt, n, pts, f);

    if (active < 3) {
        for (i = 0; i < 4 && i < c->nwheels; i++) {
            if (!c->hit[i].active)
                continue;
            if (*nrec == 2)
                break;
            rb_contact_record(c, i, (i == 0 || i == 1) ? c->steer : 0.0f,
                              &rec[*nrec]);
            rec[*nrec].wheel = i;
            (*nrec)++;
        }
    } else {
        int kr, kf;
        if (c->steer >= 0.01f) kr = 2 + (c->hit[3].active != 0);
        else                   kr = 3 - (c->hit[1].active != 0);
        rb_contact_record(c, kr, 0.0f, &rec[*nrec]);
        rec[*nrec].wheel = kr;
        (*nrec)++;

        if (c->steer >= 0.01f) kf = (c->hit[0].active == 0);
        else                   kf = (c->hit[1].active != 0);
        rb_contact_record(c, kf, c->steer, &rec[*nrec]);
        rec[*nrec].wheel = kf;
        (*nrec)++;
    }

    /* When drive is inhibited the wheels are effectively locked, so friction
       should oppose the whole slide rather than only the lateral component. */
    if (blocked) {
        for (i = 0; i < *nrec; i++) {
            float vp[3], t[3];
            double sp, d;
            rb_point_velocity(&c->body, rec[i].point, vp);
            sp = sqrt((double)vp[0] * vp[0] + (double)vp[1] * vp[1]
                      + (double)vp[2] * vp[2]);
            if (sp < EPS)
                continue;
            if (fabs(sp - 1.0) >= EPS) {
                double inv = 1.0 / sp;
                vp[0] = (float)(vp[0] * inv);
                vp[1] = (float)(vp[1] * inv);
                vp[2] = (float)(vp[2] * inv);
            }
            d = (double)vp[1] * rec[i].normal[1] + (double)vp[0] * rec[i].normal[0]
                + (double)vp[2] * rec[i].normal[2];
            t[0] = (float)((double)vp[0] - d * rec[i].normal[0]);
            t[1] = (float)((double)vp[1] - d * rec[i].normal[1]);
            t[2] = (float)((double)vp[2] - d * rec[i].normal[2]);
            sp = sqrt((double)t[0] * t[0] + (double)t[1] * t[1] + (double)t[2] * t[2]);
            if (sp < EPS)
                continue;
            if (fabs(sp - 1.0) >= EPS) {
                double inv = 1.0 / sp;
                t[0] = (float)(t[0] * inv);
                t[1] = (float)(t[1] * inv);
                t[2] = (float)(t[2] * inv);
            }
            memcpy(rec[i].lat, t, 3 * sizeof(float));
        }
    }
    return blocked;
}

/* ------------------------------------------------------------------------- */
/* the lateral friction solve                                                */
/* ------------------------------------------------------------------------- */

/* 0x004edac0 -- Baraff's simultaneous contact-force method at two contacts.
 *
 *   A[i][j] = t_i . ( t_j / m  +  (Iinv (r_j x t_j)) x r_i )
 *   b[i]    = -d/dt( v_point_i . t_i )
 *
 * solve A f = b, clamp each f to the tire's grip limit, and apply f_i along
 * t_i at contact i. The clamp is what lets the car slide: once the required
 * force exceeds grip * 10 (rear) or grip * 5 (front), the tire gives up.
 *
 * Returns nonzero if forces were produced.
 */
int rb_contact_solve(rb_car *c, float dt, int *n, float pts[][3], float f[][3])
{
    rb_contact rec[2];
    float A[2][2], b[2], fmag[2];
    int nrec = 0, i, j;

    rb_gather_contacts(c, dt, rec, &nrec, n, pts, f);
    if (nrec <= 0 || nrec >= 3)
        return 0;

    rb_update_inv_inertia_world(&c->body);

    /* --- build A and b ------------------------------------------------- */
    for (i = 0; i < nrec; i++) {
        float acc[3];

        rb_point_accel_along(&c->body, rec[i].point, rec[i].lat,
                             (const float (*)[3])pts, *n,
                             (const float (*)[3])f, acc);
        b[i] = (float)-((double)acc[0] * rec[i].lat[0]
                        + (double)acc[1] * rec[i].lat[1]
                        + (double)acc[2] * rec[i].lat[2]);

        for (j = 0; j < nrec; j++) {
            float cr[3], wj[3];
            double v;

            v = ((double)rec[j].lat[0] * rec[i].lat[0]
                 + (double)rec[i].lat[1] * rec[j].lat[1]
                 + (double)rec[i].lat[2] * rec[j].lat[2]) * c->body.inv_mass;

            cr[0] = (float)((double)rec[j].r[1] * rec[j].lat[2]
                            - (double)rec[j].r[2] * rec[j].lat[1]);
            cr[1] = (float)((double)rec[j].r[2] * rec[j].lat[0]
                            - (double)rec[j].r[0] * rec[j].lat[2]);
            cr[2] = (float)((double)rec[j].r[0] * rec[j].lat[1]
                            - (double)rec[j].r[1] * rec[j].lat[0]);
            rb_mat3_mul_vec3(c->body.iinv, cr, wj);

            v += ((double)wj[1] * rec[i].r[2] - (double)wj[2] * rec[i].r[1])
                     * rec[i].lat[0]
                 + ((double)wj[2] * rec[i].r[0] - (double)wj[0] * rec[i].r[2])
                     * rec[i].lat[1]
                 + ((double)wj[0] * rec[i].r[1] - (double)wj[1] * rec[i].r[0])
                     * rec[i].lat[2];

            A[i][j] = (float)v;
        }
    }

    /* --- clamp the demanded slide correction to what the tire can hold -- */
    for (i = 0; i < nrec; i++) {
        int rear = (rec[i].wheel == 2 || rec[i].wheel == 3);
        float grip = rb_tire_grip(c, rear ? 1 : 0);
        double hi, lo, s;
        float vp[3];

        /* While drifting, both axles get the wide +-10 window; otherwise the
           front is held to +-5, which is what stops it from over-biting. */
        if (rb_tire_drifting(c)) {
            hi = (double)grip * 10.0;
        } else {
            hi = rear ? (double)grip * 10.0 : (double)grip * 5.0;
        }
        lo = -hi;
        if (b[i] < lo)      b[i] = (float)lo;
        else if (b[i] > hi) b[i] = (float)hi;

        /* also cancel the slide already present, over one step */
        rb_point_velocity(&c->body, rec[i].point, vp);
        s = (double)vp[1] * rec[i].lat[1] + (double)vp[2] * rec[i].lat[2]
            + (double)vp[0] * rec[i].lat[0];
#ifdef RB_DEBUG_FRICTION
        printf("    [w%d grip=%.3f hi=%.3f b_pre=%.4f vp=(%.3f %.3f %.3f) s=%.4f]\n",
               rec[i].wheel, grip, hi, b[i], vp[0], vp[1], vp[2], s);
#endif
        if (fabs(s) > 0.0005) {
            double g10hi = (double)grip * 10.0, g10lo = (double)grip * -10.0;
            s = s / dt;
            if (s < g10lo)      s = g10lo;
            else if (s > g10hi) s = g10hi;
            b[i] = (float)((double)b[i] - s);
        }
    }

    /* --- solve --------------------------------------------------------- */
    if (nrec == 1) {
        fmag[0] = (fabsf(A[0][0]) <= EPS) ? 0.0f
                                          : (float)((double)b[0] / A[0][0]);
        fmag[1] = 0.0f;
    } else if (!rb_solve2((const float (*)[2])A, b, fmag)) {
        /* Singular: fall back to a one-unknown solve and sanity-check it.
         *
         * QUIRK: the consistency test compares against b[0] in both branches,
         * where the algebra wants b[1]. Faithful to 0x4edfea / 0x4ee02f.
         */
        double chk;
        if (fabsf(A[0][1]) < fabsf(A[0][0])) {
            fmag[1] = 0.0f;
            fmag[0] = (float)((double)b[0] / A[0][0]);
            chk = (double)A[1][0] * fmag[0];
        } else {
            fmag[0] = 0.0f;
            fmag[1] = (float)((double)b[0] / A[0][1]);
            chk = (double)A[1][1] * fmag[1];
        }
        if (fabs((double)b[0] - chk) > 0.0001) {
            fmag[0] = 0.0f;
            fmag[1] = 0.0f;
        }
    }

#ifdef RB_DEBUG_FRICTION
    {
        int q;
        printf("  [fric n=%d", nrec);
        for (q = 0; q < nrec; q++)
            printf(" | w%d lat=(%.2f %.2f %.2f) r=(%.2f %.2f %.2f) b=%.3f f=%.3f",
                   rec[q].wheel, rec[q].lat[0], rec[q].lat[1], rec[q].lat[2],
                   rec[q].r[0], rec[q].r[1], rec[q].r[2], b[q], fmag[q]);
        printf(" | A=%.3f %.3f %.3f %.3f]\n", A[0][0], A[0][1], A[1][0], A[1][1]);
    }
#endif

    /* --- emit ---------------------------------------------------------- */
    for (i = 0; i < nrec && *n < RB_MAX_FORCES; i++) {
        memcpy(pts[*n], rec[i].point, 3 * sizeof(float));
        f[*n][0] = (float)((double)fmag[i] * rec[i].lat[0]);
        f[*n][1] = (float)((double)fmag[i] * rec[i].lat[1]);
        f[*n][2] = (float)((double)fmag[i] * rec[i].lat[2]);
        (*n)++;
    }
    return 1;
}
