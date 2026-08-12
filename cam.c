#include "cam.h"
#include "rb_data.h"
#include <math.h>
#include <string.h>

#define DEG2RAD 0.017453292f
#define RAD2DEG 57.295776f

/* 0x0049d740 -- the easing curve camFollowStep applies to the pull-back
 * acceleration when the descriptor's exponent is non-zero. Normalised distance
 * from rest toward the slack edge, raised to `ease`. */
static double ease_curve(double rest, double edge, double ease, double v)
{
    double t;
    if (fabs(edge) < 1e-06)
        return 1.0;
    t = fabs(v - rest) / fabs(edge);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return pow(t, ease);
}

/* 0x004722c0 -- one follower step. Returns the OFFSET to add to `current`.
 *
 *   within the deadzone            -> no offset at all
 *   otherwise                      -> clamp into [rest-slack_lo, rest+slack_hi],
 *                                     approach at max_speed, then pull back
 *                                     toward rest at (eased) accel
 */
static float follow_step(const cam_follow *f, float current, float dt)
{
    double lo = (double)f->rest - f->slack_lo;
    double hi = (double)f->rest + f->slack_hi;
    double target, v;

    if (fabs((double)current - f->rest) < (double)f->dead + 1e-06)
        return 0.0f;

    target = current;
    if (target < lo) target = lo;
    if (target > hi) target = hi;

    v = rb_move_towards(current, (float)target, f->max_speed, dt);

    if (v < hi + 1e-06 && v > lo - 1e-06) {
        double accel = f->accel;
        double rate;
        if (fabs(f->ease) >= 1e-06) {
            double edge = ((double)f->rest <= current) ? f->slack_hi : f->slack_lo;
            accel = ease_curve(f->rest, edge, f->ease, v) * f->accel;
        }
        rate = accel * dt - fabs(v - (double)current);
        if (rate < 0.0)   rate = 0.0;
        if (rate > 2e+06) rate = 2e+06;
        v = rb_move_towards((float)v, f->rest, (float)rate, 1.0f);
    }
    return (float)(v - (double)current);
}

/* THE RENDERER'S convention, which is the only one that matters here.
 *
 * The view matrix is Rx(pitch) . Ry(-yaw) . T(-eye), and a camera looks along its
 * own -Z, so for a view yaw v the world-space direction the camera faces is
 *
 *     F(v) = (-sin v, 0, -cos v)
 *
 * NOT (+sin v, 0, -cos v), which is what the port's old vehicle_t used and what
 * this file used to assume. The two agree only at yaw 0 and 180, which is exactly
 * why the view looked right straight ahead and fell apart in a turn: measured with
 * the real GL matrices, the car passed BEHIND the camera and its right side landed
 * on screen-left.
 */
static void yaw_forward(float yaw_deg, float out[3])
{
    double r = (double)yaw_deg * DEG2RAD;
    out[0] = (float)-sin(r);
    out[1] = 0.0f;
    out[2] = (float)-cos(r);
}

/* The view yaw whose F(v) is the car's forward (body local +Z). */
static float car_yaw_deg(const rb_car *c)
{
    return atan2f(-c->m[8], -c->m[10]) * RAD2DEG;
}

void cam_init(cam_t *cam, const rb_car *c)
{
    memset(cam, 0, sizeof(*cam));

    /* Block A, 0x014c4898: rest = defDistXZ, no deadzone, unbounded slack,
       effectively infinite accel and rate -- so this one snaps to rest. */
    cam->f_dist.rest      = RB_CAMERA.dist_xz;
    cam->f_dist.dead      = 0.0f;
    cam->f_dist.slack_lo  = 2e+06f;
    cam->f_dist.slack_hi  = 2e+06f;
    cam->f_dist.accel     = 2e+06f;
    cam->f_dist.max_speed = 2e+06f;
    cam->f_dist.ease      = 0.0f;

    /* Block B, 0x014c48b4: rest = defDistY, slack +-0.75*defDistY, pulled back
       at accDistY. This is the vertical give. */
    cam->f_height.rest      = RB_CAMERA.dist_y;
    cam->f_height.dead      = 0.0f;
    cam->f_height.slack_lo  = RB_CAMERA.dist_y * 0.75f;
    cam->f_height.slack_hi  = RB_CAMERA.dist_y * 0.75f;
    cam->f_height.accel     = RB_CAMERA.acc_dist_y;
    cam->f_height.max_speed = 2e+06f;
    cam->f_height.ease      = 0.0f;

    /* Block C, 0x014c48d0, with camSetupTargets' overrides: rest 0, slack +-20
       degrees, ease exponent 2.0. accel and rate are set per frame. */
    cam->f_yaw.rest      = 0.0f;
    cam->f_yaw.dead      = 0.0f;
    cam->f_yaw.slack_lo  = 20.0f;
    cam->f_yaw.slack_hi  = 20.0f;
    cam->f_yaw.accel     = RB_CAMERA.acc_alpha * 200.0f;
    cam->f_yaw.max_speed = RB_CAMERA.acc_alpha * 200.0f;
    cam->f_yaw.ease      = 2.0f;

    cam->yaw    = car_yaw_deg(c);
    cam->dist   = RB_CAMERA.dist_xz;
    cam->height = RB_CAMERA.dist_y;
    {
        float f[3];
        yaw_forward(cam->yaw, f);
        cam->pos[0] = c->body.x[0] - f[0] * cam->dist;
        cam->pos[1] = c->body.x[1] + cam->height;
        cam->pos[2] = c->body.x[2] - f[2] * cam->dist;
    }
    cam->valid = 1;
}

void cam_update(cam_t *cam, const rb_car *c, float steer, float dt)
{
    float cyaw, want_dist, target, fwd[3];
    double speed, wsp, extra, d_dist, d_height, d_yaw;
    double dx, dy, dz, horiz, yaw_err;

    if (!cam->valid) {
        cam_init(cam, c);
        return;
    }

    cyaw = car_yaw_deg(c);

    speed = sqrt((double)c->body.v[0] * c->body.v[0]
                 + (double)c->body.v[1] * c->body.v[1]
                 + (double)c->body.v[2] * c->body.v[2]);

    /* camSetupTargets: pull the camera back with speed. Half the base distance,
       ramped in between 4 and 8 m/s, nothing below 4. Smoothed at 1.0/s. */
    extra = (double)RB_CAMERA.dist_xz * 0.5;
    if (speed < 4.0)
        extra = 0.0;
    else if (speed <= 8.0)
        extra = extra * (speed - 4.0) * 0.25;
    cam->boost_speed = rb_move_towards(cam->boost_speed, (float)extra, 1.0f, dt);

    /* ...and a little more while the throttle is down: target 0.2 of the base
       distance, approached at 0.5/s, released at 0.3/s. */
    if (c->in.accel) {
        target = (float)((double)RB_CAMERA.dist_xz * 0.2);
        cam->boost_input = rb_move_towards(cam->boost_input, target, 0.5f, dt);
    } else {
        cam->boost_input = rb_move_towards(cam->boost_input, 0.0f, 0.3f, dt);
    }

    want_dist = (float)((double)RB_CAMERA.dist_xz + cam->boost_speed
                        + cam->boost_input);
    cam->f_dist.rest = want_dist;

    /* camSetupTargets again: a car spinning faster than 180 deg/s gets a hard
       30/s yaw rate, otherwise accAlpha * 200. */
    wsp = sqrt((double)c->body.w[0] * c->body.w[0]
               + (double)c->body.w[1] * c->body.w[1]
               + (double)c->body.w[2] * c->body.w[2]) * RAD2DEG;
    cam->f_yaw.accel     = (wsp >= 180.0) ? 30.0f
                                          : (float)((double)RB_CAMERA.acc_alpha * 200.0);
    cam->f_yaw.max_speed = cam->f_yaw.accel;

    /* current geometry: where the camera actually is relative to the car */
    dx = (double)c->body.x[0] - cam->pos[0];
    dy = (double)cam->pos[1] - c->body.x[1];
    dz = (double)c->body.x[2] - cam->pos[2];
    horiz = sqrt(dx * dx + dz * dz);

    /* yaw error, wrapped to +-180: how far the camera has fallen behind the
       heading it should be looking along. VisTurn leans it into the corner. */
    /* VisTurn leans the view INTO the corner, i.e. toward where the car is
       going, which REDUCES the trail rather than adding to it.
       `steer` is the stick, positive = right. In the renderer's yaw convention a
       right turn DECREASES yaw (F(v) = (-sin v, 0, -cos v): v=0 is -Z, v=90 is -X,
       so increasing v swings left), so the lean subtracts. Get this backwards and
       the lean and the 20 degree slack land on the same side of the car: 31.8
       degrees off the tail instead of 8.18. */
    yaw_err = (double)cam->yaw
              - (double)(cyaw - steer * RB_CAMERA.vis_turn);
    while (yaw_err > 180.0)  yaw_err -= 360.0;
    while (yaw_err < -180.0) yaw_err += 360.0;

    d_dist   = follow_step(&cam->f_dist,   (float)horiz,   dt);
    d_height = follow_step(&cam->f_height, (float)dy,      dt);
    d_yaw    = follow_step(&cam->f_yaw,    (float)yaw_err, dt);

    cam->dist   = (float)(horiz + d_dist);
    cam->height = (float)(dy + d_height);

    /* Yaw, and the one place the car-frame formulation needs a hand.
     *
     * camFollowStep only ever returns an offset limited by its own max_speed --
     * accAlpha * 200, i.e. about 99 deg/s. In the original that is enough, because
     * camFollow rebuilds the camera relative to its PREVIOUS frame, so the
     * geometry is already converging on the car and the follower merely damps it.
     * Working in the car's frame instead makes the follower the only thing that
     * tracks at all, and a car at full lock yaws far faster than 99 deg/s -- a
     * 0.3 m wheelbase at 6 m/s gives several hundred. The lag then grows without
     * bound: measured at 30 deg after one second of turning, 94 deg after five,
     * by which point the camera is looking at the car from in front.
     *
     * The slack window is the original's own statement of how far the view may
     * trail, so enforce it as a hard bound. Inside +-20 degrees the follower's
     * easing is what shapes the motion, exactly as before.
     */
    {
        double lag = yaw_err + d_yaw;
        double lim = cam->f_yaw.slack_hi;
        if (lag >  lim) lag =  lim;
        if (lag < -lim) lag = -lim;
        cam->yaw = (float)((double)cyaw - steer * RB_CAMERA.vis_turn + lag);
    }

    if (cam->dist < 0.05f)
        cam->dist = 0.05f;

    yaw_forward(cam->yaw, fwd);
    cam->pos[0] = (float)((double)c->body.x[0] - (double)fwd[0] * cam->dist);
    cam->pos[1] = (float)((double)c->body.x[1] + cam->height);
    cam->pos[2] = (float)((double)c->body.x[2] - (double)fwd[2] * cam->dist);
}

float cam_pitch_deg(const cam_t *cam)
{
    /* look down at the car */
    return atan2f(cam->height, cam->dist) * RAD2DEG;
}
