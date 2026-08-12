/*
 * See physics.h: the constants and curves are the game's, the model is not.
 *
 * Intermediates are deliberately double and state is float, which is what the
 * original does (x87 at PC=53 with float32 state). Build this file with
 * -fno-fast-math -ffp-contract=off so the compiler cannot reassociate.
 */

#include "physics.h"
#include "physics_data.h"

#include <math.h>

#define DEG (M_PI / 180.0)

/* Measured off the Car1 mesh once node transforms were applied:
   wheels sit at z = +/-0.147 and x = +/-0.15. */
#define WHEELBASE 0.294f
#define TRACK_W   0.300f
#define GRAVITY   9.81f

static double curve_eval(const curve_pt_t *c, int n, double x)
{
    if (n <= 0) return 0.0;
    if (x <= c[0].in) return c[0].out;
    if (x >= c[n - 1].in) return c[n - 1].out;
    for (int i = 1; i < n; i++) {
        if (x <= c[i].in) {
            double span = (double)c[i].in - (double)c[i - 1].in;
            if (span <= 0.0) return c[i].out;
            double t = (x - (double)c[i - 1].in) / span;
            return (double)c[i - 1].out + t * ((double)c[i].out - (double)c[i - 1].out);
        }
    }
    return c[n - 1].out;
}

void vehicle_init(vehicle_t *v, int car, float x, float y, float z, float yaw)
{
    for (unsigned i = 0; i < sizeof(*v) / sizeof(float); i++) ((float *)v)[i] = 0.f;
    v->car = (car < 0) ? 0 : (car > 2 ? 2 : car);
    v->x = x; v->y = y; v->z = z; v->yaw = yaw;
}

const char *vehicle_car_name(int car)
{
    return CARS[(car < 0) ? 0 : (car > 2 ? 2 : car)].name;
}

float vehicle_speed(const vehicle_t *v)
{
    return (float)sqrt((double)v->vlong * v->vlong + (double)v->vlat * v->vlat);
}

void vehicle_step(vehicle_t *v, const vehicle_input_t *in, float dt, ground_fn ground)
{
    const car_phys_t *C = &CARS[v->car];
    if (dt <= 0.f) return;
    if (dt > 0.05f) dt = 0.05f;          /* never integrate through a hitch */

    /* ---- boost ramp: the game has separate spool-up and spool-down rates ---- */
    double boost = v->boost;
    boost += (in->boost ? (double)C->boost_up : -(double)C->boost_down) * dt;
    if (boost < 0.0) boost = 0.0;
    if (boost > 1.0) boost = 1.0;
    v->boost = (float)boost;

    double vmax = (double)C->speed_base_max
                + boost * ((double)C->speed_boost_max - (double)C->speed_base_max);

    /* ---- engine: acceleration comes from the car's own curve ---- */
    double spd = fabs((double)v->vlong);
    double acc = curve_eval(C->accel, C->accel_n, spd);
    if (v->vlong > vmax) acc = 0.0;

    double drive = acc * (double)in->throttle;
    double brake = 14.0 * (double)in->brake;      /* no braking constant recovered */

    /*
     * The acceleration curve is NET: splAccelBase falls to zero at roughly
     * speedBaseMax, so the losses at full throttle are already inside it.
     * Applying air drag on top double-counts them -- doing that pinned Overkill
     * at 9.5 m/s against its real 27. So resistance only acts on the part of
     * the throttle that is NOT applied, i.e. it dominates when coasting.
     */
    double coast = 1.0 - (double)in->throttle;
    double drag = coast * ((double)C->air_drag * spd * spd
                           + (double)C->bearing_friction * spd);

    double vlong = (double)v->vlong;
    double sgn = (vlong >= 0.0) ? 1.0 : -1.0;
    vlong += (drive - sgn * drag - sgn * brake) * dt;
    if (in->brake > 0.f && sgn > 0.0 && vlong < 0.0) vlong = 0.0;
    if (vlong > vmax) vlong = vmax;
    if (vlong < -0.35 * vmax) vlong = -0.35 * vmax;

    /* ---- steering: max lock from AngleSteer, tightened off at speed ---- */
    double steer = (double)in->steer * (double)C->steer_max_deg;
    double fade = 1.0 / (1.0 + 0.045 * spd * spd / 10.0);
    steer *= fade;

    /* Bicycle-model yaw rate, then clamp by the grip the tyres can supply:
       a_lat = v * yawrate must stay under mu * g. */
    double yaw_rate = 0.0;
    if (fabs(vlong) > 0.05)
        yaw_rate = vlong * tan(steer * DEG) / (double)WHEELBASE;

    double mu = ((double)C->grip_front + (double)C->grip_rear) * 0.5;
    double a_lat_max = mu * GRAVITY;
    double a_lat = fabs(vlong * yaw_rate);

    v->drifting = 0;
    if (a_lat > a_lat_max && a_lat > 1e-6) {
        double scale = a_lat_max / a_lat;
        /* Past the limit the rear steps out: coeffDriftRear says how much. */
        if (spd > (double)C->drift_speed_on
            && fabs(steer) > (double)C->drift_angle_on) {
            scale += (1.0 - scale) * (double)C->drift_rear;
            v->drifting = 1;
        }
        yaw_rate *= scale;
    }

    /* lateral velocity: builds while sliding, scrubbed off by rear grip */
    double vlat = (double)v->vlat;
    vlat += (vlong * yaw_rate - vlat * (double)C->grip_rear * 9.0) * dt;
    if (!v->drifting) vlat *= 0.86;

    double yaw = (double)v->yaw + yaw_rate / DEG * dt;

    /* ---- integrate position in world space ---- */
    double r = yaw * DEG;
    double fx = sin(r), fz = -cos(r);
    double sx = cos(r), sz = sin(r);
    double nx = (double)v->x + (fx * vlong + sx * vlat) * dt;
    double nz = (double)v->z + (fz * vlong + sz * vlat) * dt;

    v->yaw = (float)yaw;
    v->yaw_rate = (float)(yaw_rate / DEG);
    v->vlat = (float)vlat;

    /* ---- suspension and terrain ---- */
    double ny = (double)v->y;
    double ceil_y = (double)v->y + 1.5;
    float gy, gnx, gny, gnz;
    int hit = ground ? ground((float)nx, (float)nz, (float)ceil_y,
                              &gy, &gnx, &gny, &gnz) : 0;
    if (hit) {
        v->x = (float)nx;
        v->z = (float)nz;

        /* Spring/damper about the rest length, with kPos and kSpeed from the
           game's springs<n>.crs, clamped to lenMin..lenMax travel. */
        double rest = (double)C->susp_length;
        double target = (double)gy + rest;
        double err = target - ny;
        double vy = (double)v->vy;
        vy += ((double)C->susp_k_pos * err * 40.0
               - (double)C->susp_k_speed * vy - GRAVITY) * dt;
        ny += vy * dt;

        double lo = (double)gy + (double)C->susp_len_min * rest;
        double hi = (double)gy + (double)C->susp_len_max * rest;
        if (ny < lo) { ny = lo; if (vy < 0.0) vy = 0.0; }
        if (ny > hi) { ny = hi; if (vy > 0.0) vy = 0.0; }
        v->vy = (float)vy;
        v->on_ground = 1;

        /* Body attitude: terrain slope under the axles, plus load transfer
           weighted by the recovered moment coefficients. */
        double yf, yb, yl, yr, d0, d1, d2;
        float t;
        double half = WHEELBASE * 0.5, halfw = TRACK_W * 0.5;
        double pitch = (double)v->pitch, roll = (double)v->roll;
        if (ground((float)(nx + fx * half), (float)(nz + fz * half), (float)ceil_y, &t, &gnx, &gny, &gnz)) {
            yf = t;
            if (ground((float)(nx - fx * half), (float)(nz - fz * half), (float)ceil_y, &t, &gnx, &gny, &gnz)) {
                yb = t;
                double accel_pitch = -(drive - sgn * drag) * (double)C->moment_ox * 0.20;
                pitch = -atan2(yf - yb, WHEELBASE) / DEG + accel_pitch;
            }
        }
        if (ground((float)(nx + sx * halfw), (float)(nz + sz * halfw), (float)ceil_y, &t, &gnx, &gny, &gnz)) {
            yr = t;
            if (ground((float)(nx - sx * halfw), (float)(nz - sz * halfw), (float)ceil_y, &t, &gnx, &gny, &gnz)) {
                yl = t;
                double lean = vlong * yaw_rate * (double)C->moment_oz * 0.06;
                roll = atan2(yr - yl, TRACK_W) / DEG + lean;
            }
        }
        /* ease toward the target attitude so kerbs do not snap the body */
        v->pitch = (float)((double)v->pitch + (pitch - (double)v->pitch) * 10.0 * dt);
        v->roll  = (float)((double)v->roll  + (roll  - (double)v->roll)  * 10.0 * dt);
        (void)yl; (void)yr; (void)d0; (void)d1; (void)d2;
    } else {
        /* off the collision mesh: fall, and scrub speed so we cannot run away */
        double vy = (double)v->vy - GRAVITY * dt;
        ny += vy * dt;
        v->vy = (float)vy;
        v->on_ground = 0;
        vlong *= 0.5;
    }

    v->y = (float)ny;
    v->vlong = (float)vlong;
}
