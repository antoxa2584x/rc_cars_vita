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
 *
 * Note what the second line does NOT say: outside the window the step is only
 * the approach, limited by max_speed, and the pull-back block is skipped
 * entirely. On the yaw follower that is 99 deg/s and no more, which is the whole
 * reason the view can be left behind by a car that turns faster than that.
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

/* camRefFrame, 0x00500840. The frame the whole camera is expressed in is the
 * car's position with WORLD up and the car's forward FLATTENED to horizontal --
 * which is why the view never rolls or pitches with the body.
 *
 * The special case is the one line of it that matters: when the car's forward is
 * within 30 degrees of world up -- nose in the air off a jump, or standing on its
 * tail -- the flattened forward is numerical noise and the yaw would spin. The
 * engine then takes the body-space direction 0.5*(local +Z) - 0.5*(local +Y),
 * i.e. 45 degrees down from the nose, through the car's matrix instead, and
 * flattens THAT. `m` is row-vector row-major, so row 1 is the body up and row 2
 * the body forward.
 */
static float car_yaw_deg(const rb_car *c)
{
    float fx = c->m[8], fy = c->m[9], fz = c->m[10];

    if (fy > 0.8660254f) {            /* cos 30 deg, against world up (0,1,0) */
        fx = 0.5f * c->m[8]  - 0.5f * c->m[4];
        fy = 0.5f * c->m[9]  - 0.5f * c->m[5];
        fz = 0.5f * c->m[10] - 0.5f * c->m[6];
        if (fx * fx + fz * fz < 1e-12f)   /* still degenerate: keep the old yaw */
            return atan2f(-c->m[8], -c->m[10]) * RAD2DEG;
    }
    (void)fy;
    return atan2f(-fx, -fz) * RAD2DEG;
}

/* The port's own two bounds on the ground clamp below, and the reason they
 * exist: the original probes a 10 metre COLUMN (0x00535130) and asks how far the
 * point is below whatever it lands on. rb_world.ground cannot say that -- it
 * takes a ceiling, and an unbounded ceiling resolves to an overpass, which is the
 * trap rb.h already documents for the race starts. So the probe is allowed to
 * look a little above the eye, and the surface it finds is rejected outright if
 * it is high enough above the car to be a roof rather than the slope the camera
 * is sitting on. These are 1:10 models: a bridge with four metres of real
 * clearance is 0.4 m here, so the margins have to be small. */
#define CAM_GROUND_LOOKUP 0.30f     /* how far above the eye the probe may see */
#define CAM_GROUND_MAX_UP 0.75f     /* above the car, past which it is a roof */

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
       degrees, ease exponent 2.0. accel and rate are set per frame. (The loaded
       block says +-90 and 1.2; camSetupTargets overwrites [2],[3] and [6] every
       frame, so the loaded values are never the ones in force.) */
    cam->f_yaw.rest      = 0.0f;
    cam->f_yaw.dead      = 0.0f;
    cam->f_yaw.slack_lo  = 20.0f;
    cam->f_yaw.slack_hi  = 20.0f;
    cam->f_yaw.accel     = RB_CAMERA.acc_alpha * 200.0f;
    cam->f_yaw.max_speed = RB_CAMERA.acc_alpha * 200.0f;
    cam->f_yaw.ease      = 2.0f;

    /* Block D, the aim, is the engine's default descriptor DAT_00564810 =
       {0, 0, 2e6, 2e6, 2e6, 2e6, 0}: rest 0 with an infinite rate, so it snaps.
       There is no state to keep for it -- the camera simply points at the car. */

    cam->yaw    = car_yaw_deg(c);
    cam->dist   = RB_CAMERA.dist_xz;
    cam->height = RB_CAMERA.dist_y;
    cam->pitch  = atan2f(cam->height, cam->dist) * RAD2DEG - RB_CAMERA.vis_turn;
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
    const rb_world *w;

    /* VisTurn is not a steering term -- see the note over the aim below -- so
       nothing here reads the stick any more. The parameter stays for the
       harnesses and for main.c. */
    (void)steer;

    if (!cam->valid) {
        cam_init(cam, c);
        return;
    }

    w = c->world;
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
       30/s yaw rate, otherwise accAlpha * 200. Note which way that cuts -- in a
       spin the camera is told to follow LESS, so the car whirls in front of a
       view that stays roughly where it was. That is deliberate. */
    wsp = sqrt((double)c->body.w[0] * c->body.w[0]
               + (double)c->body.w[1] * c->body.w[1]
               + (double)c->body.w[2] * c->body.w[2]) * RAD2DEG;
    cam->f_yaw.accel     = (wsp >= 180.0) ? 30.0f
                                          : (float)((double)RB_CAMERA.acc_alpha * 200.0);
    cam->f_yaw.max_speed = cam->f_yaw.accel;

    /* current geometry: where the camera actually is relative to the car.
       This is camFollow's own opening move -- express the eye in the reference
       frame and run the followers on the three components of it. */
    dx = (double)c->body.x[0] - cam->pos[0];
    dy = (double)cam->pos[1] - c->body.x[1];
    dz = (double)c->body.x[2] - cam->pos[2];
    horiz = sqrt(dx * dx + dz * dz);

    /* The trail: how far round the car the eye has been left, wrapped to +-180.
       Purely geometric, with nothing in it from the controls. */
    yaw_err = (double)cam->yaw - (double)cyaw;
    while (yaw_err > 180.0)  yaw_err -= 360.0;
    while (yaw_err < -180.0) yaw_err += 360.0;

    d_dist   = follow_step(&cam->f_dist,   (float)horiz,   dt);
    d_height = follow_step(&cam->f_height, (float)dy,      dt);
    d_yaw    = follow_step(&cam->f_yaw,    (float)yaw_err, dt);

    cam->dist   = (float)(horiz + d_dist);
    cam->height = (float)(dy + d_height);

    /* The trail is NOT clamped to the slack window, and this file used to clamp
       it. camFollowStep's window is a window on the PULL-BACK, not a bound on
       the value: outside it the follower still only closes at max_speed, so a
       car that turns faster than 99 deg/s does leave the view behind, and a car
       spinning past 180 deg/s leaves it behind at 30 deg/s. Clamping made the
       camera rigidly welded to the tail through exactly the manoeuvres -- hard
       corners, spins, a landing that snaps the nose round -- where the original
       swings wide and takes a second to come back. */
    {
        double lag = yaw_err + d_yaw;
        double v = (double)cyaw + lag;
        while (v > 180.0)  v -= 360.0;
        while (v < -180.0) v += 360.0;
        cam->yaw = (float)v;
    }

    if (cam->dist < 0.05f)
        cam->dist = 0.05f;

    yaw_forward(cam->yaw, fwd);
    cam->pos[0] = (float)((double)c->body.x[0] - (double)fwd[0] * cam->dist);
    cam->pos[1] = (float)((double)c->body.x[1] + cam->height);
    cam->pos[2] = (float)((double)c->body.x[2] - (double)fwd[2] * cam->dist);

    /* ------------------------------------------------------------------------
     * camPost, 0x00501180 -- the half of the chain these notes said was "not
     * transcribed". It is registered as the camera's per-frame hook by
     * 0x00500760 (which also sets the near and far planes, 0.1 and 500), and it
     * runs on the eye and basis camFollow just produced, in this order.
     * ------------------------------------------------------------------------ */

    /* 1. The obstacle lift, 0x00500e60 over camSightBlocked 0x00500d60. If the
     *    segment from 0.15 m above the car to 0.06 m below the eye crosses
     *    geometry, the eye ORBITS UP around the car -- over the wall, the kerb,
     *    the bank behind it -- toward CDT_AngleUp (29.1 deg) at CDT_AngleUpSpeed
     *    (29.4 deg/s), and releases the same way when the view clears.
     *
     *    The original rotates the eye->car direction about the camera's right
     *    axis and re-places the eye at car - dist*dir, which leaves the radius
     *    alone. In the yaw-only frame this file works in, that is exactly raising
     *    the eye's elevation angle at constant radius. */
    {
        int blocked = 0;
        if (w && w->segment) {
            float a[3], b[3];
            a[0] = c->body.x[0];
            a[1] = c->body.x[1] + 0.15f;
            a[2] = c->body.x[2];
            b[0] = cam->pos[0];
            b[1] = cam->pos[1] - 0.06f;
            b[2] = cam->pos[2];
            blocked = w->segment(w->ctx, a, b);
        }
        cam->cdt_angle = rb_move_towards(cam->cdt_angle,
                                         blocked ? RB_CAMERA.cdt_angle_up : 0.0f,
                                         RB_CAMERA.cdt_angle_up_speed, dt);
        if (cam->cdt_angle > 1e-04f) {
            double r = sqrt((double)cam->dist * cam->dist
                            + (double)cam->height * cam->height);
            double e = atan2((double)cam->height, (double)cam->dist)
                       + (double)cam->cdt_angle * DEG2RAD;
            if (e > 1.55334306) e = 1.55334306;   /* 89 deg, so dist stays real */
            cam->dist   = (float)(r * cos(e));
            cam->height = (float)(r * sin(e));
            if (cam->dist < 0.05f) cam->dist = 0.05f;
            cam->pos[0] = (float)((double)c->body.x[0] - (double)fwd[0] * cam->dist);
            cam->pos[1] = (float)((double)c->body.x[1] + cam->height);
            cam->pos[2] = (float)((double)c->body.x[2] - (double)fwd[2] * cam->dist);
        }
    }

    /* 2. THE AIM, and the one thing in this file that was outright wrong.
     *
     *    VisTurn is not a look-into-turn and never was. camPost rotates the view
     *    direction -- and the up vector with it -- about the camera's RIGHT axis
     *    by `_DAT_014c48ec + <the lift's addition>` every single frame, with no
     *    reference to the steering, the stick, or the body's yaw rate. Rotating
     *    about the right axis is a PITCH, so VisTurn is a constant 11.82 degrees
     *    of aim tilted UP, and the direction is fixed by the other user of the
     *    same axis: the obstacle lift rotates by -CDT_AngleUp to raise the eye,
     *    so +VisTurn raises the aim.
     *
     *    That is where the framing comes from. Aimed dead at the car the view is
     *    24.6 degrees down at rest and 15.1 flat out; tilted up by VisTurn it is
     *    12.8 and 3.3, so the car sits low in the frame and the track ahead fills
     *    it. Pointing at the car instead -- which is what this file did -- puts
     *    the car in the centre and the sky where the corner should be.
     *
     *    The aim is taken BEFORE the ground clamp below, because camPost is:
     *    0x00500e60 re-derives the direction from the lifted eye, the clamp then
     *    moves the eye and does NOT re-derive it. */
    {
        float a = cam->cdt_angle, add;
        if (a <= 0.0f || RB_CAMERA.cdt_angle_up < 1e-06f)
            add = 0.0f;
        else if (a <= RB_CAMERA.cdt_angle_up)
            add = RB_CAMERA.cdt_dir_add * a / RB_CAMERA.cdt_angle_up;
        else
            add = RB_CAMERA.cdt_dir_add;
        cam->cdt_dir_add = rb_move_towards(cam->cdt_dir_add, add,
                                           RB_CAMERA.cdt_dir_add_speed, dt);
        cam->pitch = atan2f(cam->height, cam->dist) * RAD2DEG
                     - RB_CAMERA.vis_turn - cam->cdt_dir_add;
    }

    /* 3. The ground clamp. The original probes 0.07 m under the eye and, if that
     *    point is inside the surface, lifts the eye clear of it -- which is what
     *    keeps the view out of the hill on a crest and off the inside of a bank.
     *    See the two margins over CAM_GROUND_LOOKUP for what the port has to do
     *    differently and why. */
    if (w && w->ground) {
        float gy, n[3];
        if (w->ground(w->ctx, cam->pos[0], cam->pos[2],
                      cam->pos[1] + CAM_GROUND_LOOKUP, &gy, n)
            && gy <= c->body.x[1] + CAM_GROUND_MAX_UP
            && cam->pos[1] - 0.07f < gy) {
            cam->pos[1] = gy + 0.14f;
            cam->height = (float)((double)cam->pos[1] - c->body.x[1]);
        }
    }
}

float cam_pitch_deg(const cam_t *cam)
{
    return cam->pitch;
}
