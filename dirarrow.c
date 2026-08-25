/*
 * dirarrow.c -- see dirarrow.h. The model is FUN_004e8f00's, the camera and the
 * screen square are Settings/arrow.ini's and the chevrons are cockpit.sb's; the
 * sign of the angle and the flat projection are the port's, and dirarrow.h says
 * why for each. The WRONG WAY banner is message slot 2 and lives in msg.c.
 */

#include "dirarrow.h"
#include "ui.h"

#include <math.h>
#include <string.h>

#define DEG2RAD 0.017453292519943295f
#define RAD2DEG 57.29577951308232f

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Into (-180, 180]. The engine's FUN_0040c6b0 normalises into [0, 360) because
   its angle is unsigned; a signed one wants the symmetric range, and the two are
   the same set of directions. */
static float wrap180(float a)
{
    while (a > 180.f)
        a -= 360.f;
    while (a <= -180.f)
        a += 360.f;
    return a;
}

/* FUN_0040c580: move an angle toward another by at most `step', the short way
   round -- a wrap-aware move-towards on degrees with a 360 modulus. */
static float ang_towards(float cur, float tgt, float step)
{
    const float d = wrap180(tgt - cur);
    if (step <= 0.f)
        return wrap180(cur);
    if (d >= -step && d <= step)
        return wrap180(tgt);
    return wrap180(cur + (d > 0.f ? step : -step));
}

/* ------------------------------------------------------------------ the state */

void dirarrow_init(dirarrow_t *d, const dirarrow_tex *tex)
{
    if (!d)
        return;
    memset(d, 0, sizeof(*d));
    if (tex)
        d->tex = *tex;
    d->enabled = 1;
    dirarrow_reset(d);
}

void dirarrow_reset(dirarrow_t *d)
{
    if (!d)
        return;
    /* FUN_004e8e50's own initial record, field for field: the angle and its
       target at 0, the two progress fractions at 1 -- so a car that has just
       been put on the grid shows a GREEN arrow rather than flashing red for the
       second it takes the chase to catch up -- the timer and the blink at 0, and
       FUN_004afdb0(idx, 0) clearing the flag. NOT a memset: the textures and
       `enabled' outlive a respawn, the way hud_reset keeps its binding. */
    d->ang = 0.f;
    d->ang_tgt = 0.f;
    d->prog = 1.f;
    d->prog_tgt = 1.f;
    d->wrong_t = 0.f;
    d->blink = 0.f;
    d->snap = 0;
    d->wrong = 0;
    /* FUN_004ea8d0's own reset of the approach: the minimum back to its sentinel
       and the beside latch cleared, which is what a new checkpoint means. -1 is
       an index no caller can pass, so the first step always re-arms. */
    d->cp_min = ARW_MIN_INIT;
    d->cp_seen = -1;
    d->beside = 0;
    d->beside_cue = 0;
    /* AND ASK FOR THE SNAP. Record +0x08 is the engine's own request flag and
       +0x0c the latch it sets; what raises it in the original is not recovered,
       but a car that has just been PUT somewhere is exactly the case the fast
       slew exists for -- otherwise the arrow spends the first second of every
       race sweeping up to 180 degrees at ARW_SLEW while the player is already
       driving. */
    d->snap_req = 1;
}

/* -------------------------------------------------------------------- the step */

void dirarrow_step(dirarrow_t *d, const dirarrow_in *in, float dt)
{
    float dx, dz, len, fx, fz, dot, side, tgt;
    float rate, step;

    if (!d)
        return;

    /* FUN_004e8f00 with no follower resets the record and returns; the drawer's
       own gate (FUN_004e94e0's fifth output) then draws nothing. */
    if (!in || !in->valid) {
        dirarrow_reset(d);
        return;
    }

    /* --- where the track goes next, and where the car is facing.
       Both flattened to XZ, which is what FUN_00410150 does to its two
       arguments before it takes their dot. */
    dx = in->aim_x - in->car_x;
    dz = in->aim_z - in->car_z;
    len = sqrtf(dx * dx + dz * dz);
    if (len < 1e-4f) {
        /* The engine's own degenerate answer, DAT_0055e9b0 = (0, 0, 1). */
        dx = 0.f;
        dz = 1.f;
        len = 1.f;
    }
    dx /= len;
    dz /= len;

    fx = sinf(in->car_yaw);
    fz = cosf(in->car_yaw);

    dot = dx * fx + dz * fz;
    /* THE SIDE, which is the port's one addition to the angle -- see dirarrow.h,
       point 1 -- and it is the RENDERER's side, constructed and not assumed.
       main.c lays the view down as Rx(vpitch).Ry(-vyaw).T(-eye), so row 0 of
       that product is the camera's right in world, (cos vyaw, 0, -sin vyaw);
       the chase camera's vyaw is the car's view yaw, which is the rig yaw
       `in->car_yaw' minus 180, so screen-right in world is

           right = (-cos psi, 0, sin psi) = (-fz, 0, fx)

       -- world +x is screen LEFT for a car facing +z, not right. `side' is the
       target's component along that, so side > 0 is the car's RIGHT, and the
       chevron turns right on a NEGATIVE angle (FUN_0040cc60's rotation carries
       the apex toward -x for a positive one). It is the cross product either
       way, the companion of the dot the wrong-way test already takes; only its
       sense had been taken on paper, and on paper it came out mirrored. Same
       class of bug as the minimap arrow's `theta = pi - yaw' -- see
       traps.md. */
    side = fx * dz - fz * dx;

    tgt = acosf(clampf(dot, -1.f, 1.f)) * RAD2DEG;   /* FUN_00410150, 0..180 */
#if DIRARROW_SIGNED
    if (side > 0.f)
        tgt = -tgt;
#endif
    d->ang_tgt = wrap180(tgt);

    /* --- the wrong-way timer. Up at 1x while the car faces away from the aim
       point, down at ARW_WRONG_DECAY while it faces it, and the gate is on the
       value BEFORE the step in both directions -- so it can sit a single step
       past either end, which is the engine's own arithmetic and is harmless. */
    if (dot < 0.f) {
        if (d->wrong_t < ARW_WRONG_CAP)
            d->wrong_t += dt;
    } else if (d->wrong_t > 0.f) {
        d->wrong_t -= ARW_WRONG_DECAY * dt;
    }

    d->wrong = (d->wrong_t > ARW_WRONG_ON);
    if (d->wrong) {
        /* SUPPRESSED NEAR THE CHECKPOINT JUST PASSED. Right at a marker the car
           really is pointing away from something, and the engine will not call
           that going the wrong way. */
        const float px = in->prev_x - in->car_x;
        const float pz = in->prev_z - in->car_z;
        if (sqrtf(px * px + pz * pz) < ARW_WRONG_NEAR)
            d->wrong = 0;
    }

    /* --- THE APPROACH TO THE CHECKPOINT, which is what the blink reads and
       which is a running MINIMUM rather than the current distance. FUN_004eb550
       folds the distance in with a min(); FUN_004ea8d0 resets it to the sentinel
       when the index advances, and the index changing is how this file sees
       that. */
    if (in->cp_index != d->cp_seen) {
        d->cp_seen = in->cp_index;
        d->cp_min = ARW_MIN_INIT;
        d->beside = 0;
    }
    if (in->cp_dist < d->cp_min)
        d->cp_min = in->cp_dist;

    /* `cp_beside': in ARW_BESIDE_IN, out ARW_BLINK_NEAR, once per checkpoint.
       The hysteresis is the engine's -- 4 m to arm, 5 m to fire -- so it lands
       as the car LEAVES a marker it really reached, not as it arrives. */
    if (!d->beside && d->cp_min < ARW_BESIDE_IN
        && in->cp_dist > ARW_BLINK_NEAR) {
        d->beside = 1;
        d->beside_cue = 1;
    }

    /* --- the blink, and its order is the engine's: the flag is settled first,
       then the counter reads it. Negative is STEADY and stays negative, so the
       arrow is on for good until something restarts the cycle. */
    d->blink -= dt;
    if (d->blink < 0.f) {
        d->blink = ARW_STEADY;
        if (d->wrong || d->cp_min < ARW_BLINK_NEAR)
            d->blink = ARW_BLINK_CYCLE;
    }

    /* --- the snap latch. +0x08 asks, +0x0c holds, and the hold clears when the
       shown angle has caught its target to within ARW_ANG_EPS. */
    if (d->snap_req) {
        d->snap = 1;
        d->snap_req = 0;
    }

    if (d->snap) {
        step = ARW_SLEW * ARW_SNAP_MUL * dt;
        d->ang = ang_towards(d->ang, d->ang_tgt, step);
        /* The engine compares the two raw normalised angles, which is 358 apart
           either side of its own wrap; the shortest arc is the same test
           everywhere else and right there too. */
        if (fabsf(wrap180(d->ang - d->ang_tgt)) < ARW_ANG_EPS)
            d->snap = 0;
        rate = ARW_CHASE * ARW_SNAP_MUL;
    } else {
        step = ARW_SLEW * dt;
        d->ang = ang_towards(d->ang, d->ang_tgt, step);
        rate = ARW_CHASE;
    }

    /* --- the tint's own quantity. WRONG WAY forces it to 0, which is what puts
       the arrow at the red end of the ramp the moment the banner comes up. */
    d->prog_tgt = d->wrong ? 0.f : clampf(in->seg_frac, 0.f, 1.f);
    if (d->prog_tgt > d->prog) {
        d->prog += rate * dt;
        if (d->prog > d->prog_tgt)
            d->prog = d->prog_tgt;
    } else if (d->prog_tgt < d->prog) {
        d->prog -= rate * dt;
        if (d->prog < d->prog_tgt)
            d->prog = d->prog_tgt;
    }

    /* The banner is not this file's any more: `wrong' is the whole output, and
       main.c posts message slot 2 off it. The poster's own "drop a post of the
       slot already showing" rule is what makes it pulse, so nothing here has to
       keep a life or a latch -- and getting that rule from the layer is what
       fixed the pulse from 1.5 s to the recovered 5.5. See msg.h. */
}

int dirarrow_wrong(const dirarrow_t *d)
{
    return d && d->wrong;
}

int dirarrow_beside_cue(dirarrow_t *d)
{
    int c;
    if (!d)
        return 0;
    c = d->beside_cue;
    d->beside_cue = 0;
    return c;
}

int dirarrow_visible(const dirarrow_t *d)
{
    /* 0x4b1025: the drawer clears the arrow's viewport whatever happens and
       only puts the chevrons in it when FUN_004e94e0's fifth output is set,
       which is `the blink counter is at or under ARW_BLINK_SHOW'. */
    return d && d->enabled && d->blink <= ARW_BLINK_SHOW;
}

void dirarrow_tint(const dirarrow_t *d, float *r, float *g, float *b)
{
    const float p = d ? clampf(d->prog, 0.f, 1.f) : 1.f;
    float cr = ARW_TINT_FULL * (1.f - p);
    float cg = ARW_TINT_FULL * p;

    /* 0x4b10d4's two clamps, and the two ceilings really are different -- 255
       for red and 235 for green, the exe's own `cmp' immediates. Kept in the
       engine's bytes and divided at the end so the numbers here are the numbers
       in the disassembly. */
    if (cr > (float)ARW_TINT_R_CAP)
        cr = (float)ARW_TINT_R_CAP;
    if (cg > (float)ARW_TINT_G_CAP)
        cg = (float)ARW_TINT_G_CAP;

    if (r) *r = cr * (1.f / 255.f);
    if (g) *g = cg * (1.f / 255.f);
    if (b) *b = 0.f;                       /* blue is 0 at every reading */
}

/* -------------------------------------------------------------- the projection */

int dirarrow_project(const dirarrow_t *d, int chev, float fu, float fv,
                     int screen_w, int screen_h, float *out_x, float *out_y)
{
    float x, z, rx, rz, c, s, dy, dz, depth, t, side;

    if (!d || chev < 0 || chev >= ARW_CHEV_N)
        return 0;

    /* The point, in the chevron's own plan footprint. fv 0 is the APEX, which is
       the most negative z -- and -z is away from the camera, which is what says
       the chevron points forward. */
    x = ARW_CHEV_X0 + (ARW_CHEV_X1 - ARW_CHEV_X0) * fu;
    z = ARW_CHEV_Z[chev][0] + (ARW_CHEV_Z[chev][1] - ARW_CHEV_Z[chev][0]) * fv;

    /* FUN_0040cc60's rotation about Y, applied the way the engine applies it --
       a row vector against a matrix whose [0][2] is -sin and [2][0] is +sin. So
       a positive angle carries the apex toward negative x, i.e. to the LEFT. */
    c = cosf(d->ang * DEG2RAD);
    s = sinf(d->ang * DEG2RAD);
    rx = x * c + z * s;
    rz = -x * s + z * c;

    /* The camera out of arrow.ini: at (0, ARW_CAM_Y, ARW_CAM_Z), looking back
       along -(0, cos dir, sin dir), so its own axes are
           forward = (0, -cos dir, -sin dir)
           up      = (0,  sin dir, -cos dir)
           right   = (1, 0, 0)
       -- one orthonormal frame, and the only one with x to the right and the
       camera's axis where arrow.ini puts it. */
    {
        const float cd = cosf(ARW_CAM_DIR * DEG2RAD);
        const float sd = sinf(ARW_CAM_DIR * DEG2RAD);
        float ndx, ndy;

        dy = ARW_CHEV_Y - ARW_CAM_Y;
        dz = rz - ARW_CAM_Z;

        depth = -cd * dy - sd * dz;
        if (depth < ARW_NEAR)
            return 0;

        t = tanf(ARW_FOV_Y * 0.5f * DEG2RAD);   /* AngleVert, aspect exactly 1 */
        ndx = (rx / depth) / t;
        ndy = ((sd * dy - cd * dz) / depth) / t;

        /* The viewport, UNIFORM OFF THE HEIGHT -- arrow.ini's Len is a fraction
           of both axes and on a 4:3 screen that is a square, so on 960x544 the
           port takes the height and keeps it square rather than stretching the
           arrow 4/3 wider. Same rule and same reason as the rest of the HUD;
           see hud_data.h's HUD_REF_W block. */
        side = ARW_LEN * (float)screen_h;
        if (out_x)
            *out_x = ARW_CX * (float)screen_w + ndx * side * 0.5f;
        if (out_y)
            *out_y = ARW_CY * (float)screen_h - ndy * side * 0.5f;
    }
    return 1;
}

/* -------------------------------------------------------------------- the draw */

static void draw_arrow(const dirarrow_t *d, int sw, int sh)
{
    float r, g, b;
    int chev, i, j;

    if (!d->tex.arrow)
        return;      /* nothing to fall back to: a chevron is not a word */

    dirarrow_tint(d, &r, &g, &b);

    /* DIRARROW_GRID^2 quads a chevron, and the far one first so the pair blends
       back to front -- they overlap by 0.095 in z. The corners are exact; the
       grid is only there to bound ui.c's two-triangle interpolation against the
       real projection, and dirarrow.h has the measured table. */
    for (chev = 0; chev < ARW_CHEV_N; chev++) {
        for (j = 0; j < DIRARROW_GRID; j++) {
            for (i = 0; i < DIRARROW_GRID; i++) {
                const float fu0 = (float)i / (float)DIRARROW_GRID;
                const float fu1 = (float)(i + 1) / (float)DIRARROW_GRID;
                const float fv0 = (float)j / (float)DIRARROW_GRID;
                const float fv1 = (float)(j + 1) / (float)DIRARROW_GRID;
                const float pu[4] = { fu0, fu1, fu1, fu0 };
                const float pv[4] = { fv0, fv0, fv1, fv1 };
                float qx[4], qy[4], qu[4], qv[4];
                int k, ok = 1;

                for (k = 0; k < 4; k++) {
                    if (!dirarrow_project(d, chev, pu[k], pv[k], sw, sh,
                                          &qx[k], &qy[k])) {
                        ok = 0;
                        break;
                    }
                    qu[k] = ARW_CHEV_U0
                          + (ARW_CHEV_U1 - ARW_CHEV_U0) * pu[k];
                    qv[k] = ARW_CHEV_V0
                          + (ARW_CHEV_V1 - ARW_CHEV_V0) * pv[k];
                }
                if (ok)
                    ui_image_quad(qx, qy, qu, qv, d->tex.arrow, r, g, b, 1.f);
            }
        }
    }
}

void dirarrow_draw(const dirarrow_t *d, int screen_w, int screen_h)
{
    if (!d || screen_w <= 0 || screen_h <= 0)
        return;
    /* An off half of the blink costs no ortho pass at all, the way hud_draw
       skips an idle pop. */
    if (!dirarrow_visible(d))
        return;

    ui_begin(screen_w, screen_h);
    draw_arrow(d, screen_w, screen_h);
    ui_end();
}
