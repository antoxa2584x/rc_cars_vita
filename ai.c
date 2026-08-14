/*
 * ai.c -- the AI opponents. See ai.h for the model and for what is NOT here.
 *
 * Every function below names the address it came from. Only three things are the
 * port's and each is marked THE PORT'S at the point of use:
 *
 *   - the wheel-contact flags fed to rb_wheel_spin_update, because the recorded
 *     state carries no contact bits;
 *   - adding the lap count to the spine distance. That is PLUMBING, not a new
 *     idea: the original's own distance is already cumulative
 *     (FUN_004eb630 = spine_len * (lap - 1) + distance into the lap), and
 *     checkpoint.c's query answers within a lap, so the lift rebuilds the
 *     quantity the original compares;
 *   - leaving the angular MOMENTUM inconsistent with the angular velocity, which
 *     nothing on this path integrates.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "rb_data.h"
#include "rbcar.h"
#include "rlog.h"

#define AI_EPS 1e-06f

/* The packed sample is read by pointing straight into the loaded file, so its C
 * layout has to be the packer's byte layout with no padding anywhere. Every
 * field is naturally aligned at the offset pack_ai.py writes it to and 36 is a
 * multiple of the struct's own alignment, so no compiler is entitled to insert
 * any -- but if one did, every geometry read below would be off by a growing
 * offset and the failure would look like a corrupt path rather than a build
 * problem. Fail at compile time instead. */
typedef char ai_sample_size_check[(sizeof(ai_sample) == AI_SAMPLE_BYTES)
                                  ? 1 : -1];

/* ------------------------------------------------------------------ the file */

static float seg_len(const ai_car *a, int i)
{
    /* |pos[i] - pos[i-1]|, the metric FUN_00503440 walks. */
    double dx, dy, dz;
    if (i < 1 || i >= a->n)
        return 0.0f;
    dx = (double)a->s[i].p[0] - a->s[i - 1].p[0];
    dy = (double)a->s[i].p[1] - a->s[i - 1].p[1];
    dz = (double)a->s[i].p[2] - a->s[i - 1].p[2];
    return (float)sqrt(dx * dx + dy * dy + dz * dz);
}

/* The speed the recording was driven at, at sample i -- the quantity
 * FUN_00503880 scales by the rubber-band coefficient.
 *
 * NOT DIVIDED BY THE MASS. The stored field is a speed in m/s, which is measured
 * and not assumed: see the note on ai_sample.mom. Dividing it by the port's
 * 2.0 kg ran every opponent at exactly half speed -- a whole lap in 160 s against
 * the recording's own 68.2 s over the track's own coefficient of 0.85, which is
 * 80.2 s. */
static float sample_speed(const ai_car *a, int i)
{
    double vx, vy, vz;
    if (i < 0 || i >= a->n)
        return 0.0f;
    vx = (double)a->s[i].mom[0] / AI_VEL_SCALE;
    vy = (double)a->s[i].mom[1] / AI_VEL_SCALE;
    vz = (double)a->s[i].mom[2] / AI_VEL_SCALE;
    return (float)sqrt(vx * vx + vy * vy + vz * vz);
}

void ai_free(ai_t *ai)
{
    if (!ai)
        return;
    if (ai->blob)
        free(ai->blob);
    memset(ai, 0, sizeof(*ai));
}

int ai_init(ai_t *ai, int track, const char *asset_dir, const rb_world *w,
            int difficulty, int championship)
{
    char path[256];
    FILE *f;
    unsigned char hdr[12];
    unsigned int n_file, i;
    long size;
    unsigned char *blob;
    const ai_race *race;
    int mask;

    if (!ai)
        return 0;
    ai_free(ai);
    if (track < 0 || track >= AI_N_RACES)
        return 0;
    race = &AI_RACES[track];
    ai->track = track;
    ai->difficulty = difficulty < 0 ? 0 : (difficulty > 3 ? 3 : difficulty);
    ai->championship = championship ? 1 : 0;

    /* FUN_004fd4c0: the product of the difficulty coefficient and the track's
     * own CoeffCommonOpponents, clamped, with a log line when it has to clamp --
     * the original's is "CAR: coeff for AI is unacceptable".
     *
     * FUN_004f11b0 only substitutes the difficulty coefficient when the game
     * mode is 1, the championship. Outside it the field keeps whatever the AI
     * descriptor carried, which for a single race is 1.0. */
    ai->coeff_static = race->coeff_common
                     * (ai->championship ? AI_DIFFICULTY[ai->difficulty] : 1.0f);
    if (ai->coeff_static < AI_COEFF_MIN || ai->coeff_static > AI_COEFF_MAX) {
        rlog("ai: coeff for AI is unacceptable (%.3f), using 1.0\n",
             ai->coeff_static);
        ai->coeff_static = 1.0f;
    }

    snprintf(path, sizeof(path), "%s/%s.aip", asset_dir ? asset_dir : ".",
             race->track);
    f = fopen(path, "rb");
    if (!f) {
        rlog("ai: no %s -- racing alone\n", path);
        return 0;
    }
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "AIP1", 4) != 0) {
        rlog("ai: %s is not an AIP1 file\n", path);
        fclose(f);
        return 0;
    }
    memcpy(&n_file, hdr + 4, 4);

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= (long)(12 + AI_RECORD_BYTES) || n_file == 0
        || n_file > AI_MAX_OPPONENTS) {
        rlog("ai: %s has %u opponents in %ld bytes -- ignoring\n",
             path, n_file, size);
        fclose(f);
        return 0;
    }
    blob = (unsigned char *)malloc((size_t)size);
    if (!blob) {
        rlog("ai: %ld bytes for %s failed\n", size, path);
        fclose(f);
        return 0;
    }
    if (fread(blob, 1, (size_t)size, f) != (size_t)size) {
        rlog("ai: %s is short\n", path);
        free(blob);
        fclose(f);
        return 0;
    }
    fclose(f);
    ai->blob = blob;

    /* Which opponents start. AI<n>Races is a mask of the race types that entry
     * appears in, so at EASY only two or three of the five turn up. It is a
     * CHAMPIONSHIP notion: outside it every opponent races, which is what the
     * original's single race does. Ultra rides with hard -- ailayouts.ini
     * declares only three bits. */
    mask = 1 << ai->difficulty;
    if (mask > AI_RACE_HARD)
        mask = AI_RACE_HARD;

    for (i = 0; i < n_file; i++) {
        const unsigned char *r = blob + 12 + i * AI_RECORD_BYTES;
        unsigned short u16[8];
        unsigned int u32[3];
        float f32[6];
        ai_car *a;
        int slot_in_file = (int)i;

        memcpy(u16, r + 48, sizeof(u16));
        memcpy(u32, r + 64, sizeof(u32));
        memcpy(f32, r + 76, sizeof(f32));

        if (ai->championship && !(u16[2] & mask))
            continue;
        /* The field is AI_MAX_FIELD, not the layout's five -- see ai.h. Capped
           here rather than at the array bound so the entries that do start keep
           their own FILE slot, and therefore their own spline. */
        if (ai->n >= AI_MAX_FIELD)
            break;

        /* The block must be inside the file, hold at least one segment, and
           have its cycle start inside it. A truncated or hand-edited .aip is the
           one way this hands out a pointer past the end. */
        if (u32[0] < 2 || u32[1] >= u32[0]
            || (unsigned long long)u32[2]
               + (unsigned long long)u32[0] * AI_SAMPLE_BYTES
               > (unsigned long long)size) {
            rlog("ai: %s slot %d out of range (%u samples at %u, cycle %u,"
                 " file %ld) -- skipped\n",
                 path, slot_in_file, u32[0], u32[2], u32[1], size);
            continue;
        }

        a = &ai->car[ai->n];
        memset(a, 0, sizeof(*a));
        memcpy(a->name, r, 16);
        a->name[15] = 0;
        memcpy(a->path, r + 16, 32);
        a->path[31] = 0;
        a->car   = u16[0] < 3 ? (int)u16[0] : 0;
        a->ref   = (int)u16[1];
        a->races = (int)u16[2];
        a->boost = (int)u16[3];
        a->reson = (int)u16[4];
        a->tires = (int)u16[5];
        a->n           = (int)u32[0];
        a->cycle_start = (int)u32[1];
        a->path_len    = f32[0];
        a->duration    = f32[1];
        a->body_dy     = f32[2];
        a->s = (const ai_sample *)(blob + u32[2]);
        /* FUN_004fd6b0 indexes the spline family by the opponent's own slot and
           falls back to slot 0 outside 0..4. Keep the FILE's slot, not the
           surviving-car index: the curves get progressively stronger down the
           list, so dropping an entry at EASY must not promote the rest. */
        a->slot = (slot_in_file >= 0 && slot_in_file <= 4) ? slot_in_file : 0;

        /* An AI car is a real rb_car; see ai.h. NULL world so rbcar_init skips
         * its ground probe -- the pose comes from the profile, not a probe -- and
         * the upgrade levels go in before rb_boost_reset reads the capacity
         * again, which is the order load_car uses for the player. */
        rbcar_init(&a->rb, a->car, NULL, a->s[0].p[0], a->s[0].p[1],
                   a->s[0].p[2], 0.0f);
        a->rb.world = w;
        a->rb.tire_upgrade = a->tires;
        a->rb.reso_upgrade = a->reson;
        a->rb.boost_upgrade = a->boost;
        rb_boost_reset(&a->rb);
        ai->n++;
    }

    ai_reset(ai);
    rlog("ai: %s -> %d opponent(s), coeff %.3f (%s, difficulty %d)\n",
         path, ai->n, ai->coeff_static,
         ai->championship ? "championship" : "single race", ai->difficulty);
    for (i = 0; i < (unsigned)ai->n; i++) {
        const ai_car *a = &ai->car[i];
        rlog("  slot %d %-10s %-16s car %d  %d samples  cycle %d  %.0f m"
             "  %.1f s  b%d r%d t%d\n",
             a->slot, a->name, a->path, a->car, a->n, a->cycle_start,
             a->path_len, a->duration, a->boost, a->reson, a->tires);
    }
    return ai->n;
}

/* ------------------------------------------------------------------ the pose */

/* 0x00407aa0 -- the engine's quaternion interpolation, shortest arc.
 *
 * Whether the original is a true slerp or a normalised lerp is NOT settled: the
 * function sits in the quaternion block next to quatMul and quatNormalize and
 * Ghidra never decompiled it. It does not matter, and that is measurable rather
 * than assumed: the profiles are sampled at 90 to 125 Hz, so the angle between
 * two consecutive recorded orientations is a fraction of a degree, and slerp and
 * nlerp differ by O(theta^3). Slerp is used because it is the one that stays
 * right at any angle, which keeps the teleport guard below the only thing
 * standing between this and a discontinuity in the recording. */
static void quat_slerp(const float a[4], const float b[4], float t,
                       float out[4])
{
    double d = (double)a[0] * b[0] + (double)a[1] * b[1]
             + (double)a[2] * b[2] + (double)a[3] * b[3];
    double s = 1.0, ka, kb, th, sth;
    int i;

    if (d < 0.0) {                    /* shortest arc */
        d = -d;
        s = -1.0;
    }
    if (d > 0.9995) {                 /* colinear: lerp and renormalise */
        for (i = 0; i < 4; i++)
            out[i] = (float)((double)a[i] + (double)t * (s * b[i] - a[i]));
        rb_quat_normalize(out);
        return;
    }
    if (d > 1.0)
        d = 1.0;
    th = acos(d);
    sth = sin(th);
    ka = sin((1.0 - (double)t) * th) / sth;
    kb = sin((double)t * th) / sth;
    for (i = 0; i < 4; i++)
        out[i] = (float)(ka * a[i] + kb * s * b[i]);
    rb_quat_normalize(out);
}

/* One packed sample -> the 32-float ODE state carSetState takes. */
static void unpack_state(const ai_car *a, const ai_sample *s,
                         float y[RB_STATE_N])
{
    int i;

    memset(y, 0, sizeof(float) * RB_STATE_N);
    y[0] = s->p[0];
    y[1] = s->p[1];
    y[2] = s->p[2];
    for (i = 0; i < 4; i++)
        y[3 + i] = (float)s->q[i] / AI_Q_SCALE;
    /* The state slot IS a momentum (carSetState divides it by the mass to get
       the velocity), and the field is a speed, so it has to be multiplied back
       up. ai_pose replaces both with the finite difference immediately
       afterwards, so this only matters to whatever reads the body between the
       two -- but a state that is silently a factor of the mass out is exactly
       the bug this line used to be. */
    for (i = 0; i < 3; i++)
        y[7 + i] = (float)((double)s->mom[i] / AI_VEL_SCALE
                           * (double)a->rb.body.mass);
    /* 10..12 is L, which the .aip does not carry -- see pack_ai.py. Left zero;
       ai_pose replaces the angular VELOCITY afterwards with the playback's own,
       the way FUN_005037f0 does, so nothing reads this. */
    for (i = 0; i < 6; i++)
        y[13 + i] = (float)s->susp[i] * (AI_SUSP_FULL / 255.0f);
    /* 19..24 is dlen, the suspension's rate of change. Left zero rather than
       differenced from the neighbours: its only consumer is
       rb_susp_spring_damper's damping term, which nothing on this path
       evaluates, and a differenced value would be a force input derived from a
       pose that is not being integrated. */
    y[25] = (float)s->steer / AI_STEER_SCALE;
    /* 26..31 is len_extra, a constant the original re-asserts every substep:
       FUN_004fbe60 is `len_extra[i] = radius[i] * 0.02`. The spring force does
       not run here but rb_wheel_frame's use_extra path does, and it is what the
       car rig draws through, so 1.4 mm of it is worth being right. */
    for (i = 0; i < RB_MAX_WHEELS; i++)
        y[26 + i] = (i < a->rb.nwheels)
                  ? a->rb.wheel[i].radius * 0.02f : 0.0f;
}

/* Write the pose for the current (cursor, u). FUN_00502ea0. */
static void ai_pose(ai_car *a)
{
    float y[RB_STATE_N];
    int i;

    if (a->cursor <= 0) {
        unpack_state(a, &a->s[0], y);
        rb_car_set_state(&a->rb, y);
        return;
    }

    {
        const ai_sample *A = &a->s[a->cursor - 1];
        const ai_sample *B = &a->s[a->cursor];
        float ya[RB_STATE_N], yb[RB_STATE_N], q[4];
        float dt = (float)B->dt / AI_DT_SCALE;
        double dx = (double)B->p[0] - A->p[0];
        double dz = (double)B->p[2] - A->p[2];

        unpack_state(a, A, ya);

        /* The teleport guard, on the HORIZONTAL speed the segment implies: take
           the earlier sample whole rather than sliding through a discontinuity.
           It never fires on the shipped profiles -- see AI_TELEPORT_SPEED -- which
           is the point of it. */
        if (dt <= AI_EPS
            || (float)(sqrt(dx * dx + dz * dz) / (double)dt)
               > AI_TELEPORT_SPEED) {
            rb_car_set_state(&a->rb, ya);
            return;
        }

        unpack_state(a, B, yb);
        for (i = 0; i < RB_STATE_N; i++)
            y[i] = (float)((double)ya[i] + ((double)yb[i] - ya[i]) * a->u);
        /* P and L come from the EARLIER sample UNLERPED -- FUN_00502ea0 copies
           local_100[7..12] back over the interpolated ones after its loop. */
        for (i = 7; i <= 12; i++)
            y[i] = ya[i];
        quat_slerp(&ya[3], &yb[3], a->u, q);
        for (i = 0; i < 4; i++)
            y[3 + i] = q[i];
        rb_car_set_state(&a->rb, y);
    }
}

/* FUN_005037f0 -> FUN_00474700: the body's velocity on this path is NOT the
 * recorded momentum, it is the FINITE DIFFERENCE of the two poses over the
 * frame -- and then P is rebuilt from it as `mass * v`. That is what makes
 * FUN_00503880's `moveTowards(|v|, recorded * coeff, 5, dt)` a first-order lag
 * on the speed the car is actually being moved at, rather than a comparison
 * between two recorded numbers.
 *
 * The angular half is the same difference taken on the orientation. L is left
 * where rb_car_set_state put it (zero) rather than rebuilt from w, which would
 * need the world inertia rather than its inverse: THE PORT'S, and it costs
 * nothing because nothing integrates this body. */
static void ai_diff_velocity(ai_car *a, const float x0[3], const float q0[4],
                             float dt)
{
    rb_body *b = &a->rb.body;
    double inv = (dt > AI_EPS) ? 1.0 / (double)dt : 0.0;
    double dq[4], n, ang, k;
    int i;

    for (i = 0; i < 3; i++) {
        b->v[i] = (float)(((double)b->x[i] - x0[i]) * inv);
        b->P[i] = (float)((double)b->mass * b->v[i]);
    }

    /* dq = q_new (x) conj(q_old); the port's convention is
       dq/dt = 0.5 * (0, w) (x) q with w in WORLD space (PHYSICS.md), so
       w = 2 * axis * (angle / dt) read off that delta. */
    dq[0] =  (double)b->q[0] * q0[0] + (double)b->q[1] * q0[1]
           + (double)b->q[2] * q0[2] + (double)b->q[3] * q0[3];
    dq[1] = -(double)b->q[0] * q0[1] + (double)b->q[1] * q0[0]
           - (double)b->q[2] * q0[3] + (double)b->q[3] * q0[2];
    dq[2] = -(double)b->q[0] * q0[2] + (double)b->q[2] * q0[0]
           - (double)b->q[3] * q0[1] + (double)b->q[1] * q0[3];
    dq[3] = -(double)b->q[0] * q0[3] + (double)b->q[3] * q0[0]
           - (double)b->q[1] * q0[2] + (double)b->q[2] * q0[1];
    if (dq[0] < 0.0) {                 /* shortest arc */
        dq[0] = -dq[0];
        dq[1] = -dq[1];
        dq[2] = -dq[2];
        dq[3] = -dq[3];
    }
    n = sqrt(dq[1] * dq[1] + dq[2] * dq[2] + dq[3] * dq[3]);
    if (n < AI_EPS || dt <= AI_EPS) {
        b->w[0] = b->w[1] = b->w[2] = 0.0f;
        return;
    }
    if (dq[0] > 1.0)
        dq[0] = 1.0;
    ang = 2.0 * atan2(n, dq[0]);
    k = ang * inv / n;
    b->w[0] = (float)(dq[1] * k);
    b->w[1] = (float)(dq[2] * k);
    b->w[2] = (float)(dq[3] * k);
}

/* --------------------------------------------------------------- the advance
 *
 * FUN_00503440, the target-speed branch (param_3 >= 0). The original carries the
 * cursor as (index, TIME) and every use of that time is of the form
 * (t - tA) / (tB - tA), so this carries (index, u) and the two are identical:
 *
 *     the original                        here
 *     frac = 1 - (t-tA)/(tB-tA)           1 - u
 *     rem  = frac * seg                   (1-u) * seg
 *     stepping into a new segment:
 *       t += (tB-tA) * step/seg           u  = step/seg
 *     staying in this one:
 *       t += (tB - t) * step/rem
 *          = (1-u)(tB-tA)*step
 *            / ((1-u)*seg)                u += step/seg
 *
 * -- the (tB - t)/rem form cancels its own (1-u), so the advance is `step` metres
 * over the segment's length either way. Writing it in u drops absolute time out
 * of the runtime entirely, which is why the .aip stores a per-sample dt for the
 * teleport guard and no timestamps at all.
 *
 * Two guards in the original are unreachable and are not transcribed: `step < 0`
 * after `step -= rem` cannot hold, because that branch is entered on rem < step.
 *
 * -> 1 when the path has run out, which is the caller's lap boundary.
 */
static int ai_advance(ai_car *a, float target, float dt)
{
    float step, seg, rem;

    if (a->cursor == 0) {                      /* 0x503468 */
        a->cursor = 1;
        a->u = 0.0f;
    }
    if (target < AI_SPEED_FLOOR)               /* 0x50347e */
        target = AI_SPEED_FLOOR;
    step = target * dt;
    a->dist += step;

    seg = seg_len(a, a->cursor);
    rem = (1.0f - a->u) * seg;

    if (rem < step) {
        step -= rem;
        a->u = 1.0f;
        if (a->cursor >= a->n - 1)
            return 1;
        for (;;) {
            a->cursor++;
            seg = seg_len(a, a->cursor);
            if (step < seg)
                break;
            if (a->cursor >= a->n - 1) {
                a->u = 1.0f;
                return 1;
            }
            step -= seg;
        }
        a->u = (seg > AI_EPS) ? step / seg : 0.0f;
    } else if (seg > AI_EPS) {
        a->u += step / seg;
        if (a->u > 1.0f)
            a->u = 1.0f;
    }
    return 0;
}

/* ------------------------------------------------------------ the rubber band
 *
 * FUN_004fd4c0 with FUN_004fd5e0 and FUN_00471a70 folded in, because between
 * them they compute one expression. FUN_004fd6b0 hands FUN_00471a70 a 33-entry
 * table and then overwrites the only three entries it reads with 1.0, 1.0 and
 * 10.0, which makes its ramp return exactly 1.0 for any input -- so the spline
 * multiply is the whole of it, and the nine OpponentBehav constants that fill
 * the rest of that table never reach a behaviour. See gen_ai_data.py.
 */
float ai_coeff(const ai_t *ai, int slot, float lead, int gap)
{
    const rb_curve *cv;

    if (!ai)
        return 1.0f;
    if (slot < 0 || slot > 4)
        slot = 0;
    /* FUN_004fd6b0: the plain family when the game mode is 5, the _A family
       otherwise -- and the championship is mode 1, so it races against _A. */
    cv = ai->championship ? &AI_FWA_A[slot] : &AI_FWA[slot];

    /* FUN_004fd5e0's shift, and it is hedged about with three conditions:
     *
     *   FUN_004a4ca0() == 5     the same mode test that picks the spline family,
     *                           so the CHAMPIONSHIP never sees this at all
     *   1 < iVar2               the player's checkpoint index is two or more
     *                           past the opponent's
     *   0.0 < param_3[2]        and the lead is positive
     *
     * The last two only hold together when the OPPONENT is a lap or more up on
     * the player while sitting at a lower checkpoint index within the lap --
     * because the distance both are measured on is cumulative
     * (FUN_004eb630 = spine_len * (lap - 1) + distance into the lap), so a
     * positive lead means genuinely further round the race. Subtracting 5 m
     * (gaps 2..7) or 60 to 120 m (gaps 8 and up) from a lead that is already at
     * least a lap then changes nothing the spline can see: it clamps at +45 m on
     * slot 0 and +20 on slot 4.
     *
     * So this is a REACHABLE mechanism that is very nearly inert, and the shipped
     * data says the authors knew: aiShiftsFuncWaitAccel.ini carries a
     * commented-out `frontShifts = 17, 25, 35, 45, 50, 55, 60, ...` above a live
     * line whose first six entries have all been flattened to 5. Transcribed as
     * written, gate and all. */
    if (!ai->championship && gap > 1 && lead > 0.0f)
        lead -= AI_FRONT_SHIFTS[gap < 10 ? gap - 2 : 9];

    return rb_curve_eval(cv, lead) * ai->coeff_static;
}

/* ----------------------------------------------------------------- the tick */

void ai_reset(ai_t *ai)
{
    int i;
    if (!ai)
        return;
    for (i = 0; i < ai->n; i++) {
        ai_car *a = &ai->car[i];
        a->cursor = 0;
        a->u = 0.0f;
        a->lap = 0;
        a->dist = 0.0f;
        a->lead = 0.0f;
        a->spine_dist = 0.0f;
        a->cp = 0;
        a->coeff = ai->coeff_static;
        a->speed = 0.0f;
        ai_pose(a);
        a->speed_rec = sample_speed(a, 0);
        rb_boost_reset(&a->rb);
    }
    ai->player_dist = 0.0f;
    ai->player_cp = 0;
}

/* THE PORT'S. The recorded state carries no contact flags, and
 * rb_wheel_spin_update needs to know whether the car is on the ground: an
 * airborne wheel gets a zero patch velocity and a 5 rad/s^2 chase instead of
 * 200, so without this the wheels either stop turning through a jump or keep
 * being driven by a patch that is not touching anything. A wheel whose recorded
 * suspension has extended to its FREE length is hanging; anything shorter is
 * carrying load. That is the same signal the spring force reads, so it is not a
 * guess about the recording -- it IS the recording.
 *
 * AGAINST len_free, NOT len_max, and that distinction is the whole check. This
 * first compared against `len_max`, which is the hard clamp the geometric solve
 * refuses to pass and which a car in the air never reaches: `suspExtend` extends
 * toward len_free. Measured over beach_1's five recordings, the longest strut
 * anyone reached is 0.2247 m against an Overkill len_max of 0.2397 -- so the
 * test never fired, `airborne` was permanently 0, and a mutant that hardcoded
 * every wheel to "grounded" was indistinguishable from the real thing. Against
 * len_free (0.2179) the same five recordings report every wheel hanging for
 * 0.6% to 2.8% of their samples and at least one hanging for 2.4% to 5.7%,
 * which is what a lap with jumps in it should look like. */
static void ai_fake_contacts(ai_car *a)
{
    int i, air = 1;
    for (i = 0; i < a->rb.nwheels; i++) {
        rb_wheel *wh = &a->rb.wheel[i];
        int loaded = wh->len < wh->len_free - AI_DROOP_TOL;
        memset(&a->rb.hit[i], 0, sizeof(a->rb.hit[i]));
        a->rb.hit[i].active = loaded;
        if (loaded)
            air = 0;
    }
    a->airborne = air;
}

void ai_step(ai_t *ai, const ai_track *tr, float px, float py, float pz,
             int player_lap, float dt)
{
    float pdist = 0.0f;
    int pcp = 0, i;
    int have_spine = 0;

    if (!ai || ai->n <= 0 || dt <= 0.0f)
        return;

    if (tr && tr->spine)
        have_spine = tr->spine(tr->ctx, px, py, pz, &pdist, &pcp);
    if (have_spine) {
        /* Cumulative, the same way FUN_004eb630 builds it. */
        ai->player_dist = pdist + (float)player_lap * tr->spine_len;
        ai->player_cp = pcp;
    }

    for (i = 0; i < ai->n; i++) {
        ai_car *a = &ai->car[i];
        float target, adist = 0.0f;
        float x0[3], q0[4];
        int acp = 0, gap = 0;

        /* FUN_00503880's order, and it is load-bearing. The pose is written
         * FIRST, so the recorded speed the target is built from is the one at
         * the cursor the car is on now; only then is the cursor moved and the
         * pose written again. Reversing it commands this frame's speed off next
         * frame's sample. */
        ai_pose(a);
        memcpy(x0, a->rb.body.x, sizeof(x0));
        memcpy(q0, a->rb.body.q, sizeof(q0));
        a->speed_rec = sample_speed(a, a->cursor > 0 ? a->cursor - 1 : 0);

        /* FUN_004fd5e0: the lead, measured on the same spine as the player.
         * With no spine bound the lead is 0 and the coefficient is the static
         * product alone -- which is also what the original does, because
         * FUN_004fd5e0 returns 0 until the checkpoint list is up and
         * FUN_004fd4c0 then returns local_98 by itself. */
        if (have_spine
            && tr->spine(tr->ctx, a->rb.body.x[0], a->rb.body.x[1], a->rb.body.x[2],
                         &adist, &acp)) {
            a->cp = acp;
            /* The distance the original compares is CUMULATIVE across laps:
             * FUN_004ea120 hands its raw pair to FUN_004eb630, which returns
             * `spine_len * (lap - 1) + distance into the lap`, and FUN_004e9860
             * is the total the checkpoint loader accumulated. So a lead is
             * monotonic and a car a lap up reads hundreds of metres ahead rather
             * than wrapping to nothing.
             *
             * THE PORT'S, but only as plumbing: checkpoint.c's spine query
             * answers WITHIN a lap, so the lap count is added here to rebuild the
             * original's own quantity. The lap comes from the path running out,
             * below, which is the only lap signal ai.c has. */
            a->spine_dist = adist + (float)a->lap * tr->spine_len;
            a->lead = a->spine_dist - ai->player_dist;
            gap = ai->player_cp - acp;    /* FUN_004fd5e0's iVar2 */
        } else {
            a->lead = 0.0f;
            gap = 0;
        }

        a->coeff = ai_coeff(ai, a->slot, a->lead, gap);

        /* FUN_00503880: moveTowards(|v|, recorded * coeff, 5.0, dt), where |v|
         * is the FINITE-DIFFERENCE speed the last frame achieved -- see
         * ai_diff_velocity. a->speed carries it forward, and it agrees with the
         * original's by construction: ai_advance moves the car exactly
         * `speed * dt` metres along the polyline, so the chord over dt IS that
         * speed to within the polyline's own curvature. */
        target = rb_move_towards(a->speed, a->speed_rec * a->coeff,
                                 AI_ACCEL_LIMIT, dt);

        if (ai_advance(a, target, dt)) {
            /* The path ran out. FUN_00503880 sends the cursor back to
             * phys+0x43b8, which is the profile's CYCLE START and not 0 -- so the
             * approach to the grid is driven once and never again. */
            a->lap++;
            a->cursor = a->cycle_start > 0 ? a->cycle_start : 1;
            a->u = 0.0f;
        }

        ai_pose(a);
        ai_diff_velocity(a, x0, q0, dt);
        a->speed = (float)sqrt((double)a->rb.body.v[0] * a->rb.body.v[0]
                               + (double)a->rb.body.v[1] * a->rb.body.v[1]
                               + (double)a->rb.body.v[2] * a->rb.body.v[2]);
        ai_fake_contacts(a);
        rb_wheel_spin_update(&a->rb, dt);
    }
}

/* ------------------------------------------------------------------ contact */

/* One overlapping sphere pair. `normal` points OUT of the opponent and toward
   the player, which is rb_coll_contact's own convention -- so an approaching
   player has a negative relative normal velocity. */
typedef struct {
    float point[3];
    float normal[3];
    float depth;
    const rb_car *other;
} ai_touch;

#define AI_MAX_TOUCH 12

float ai_collide_player(ai_t *ai, rb_car *player, float dt)
{
    float ps[RB_MAX_SPHERES][4], os[RB_MAX_SPHERES][4];
    ai_touch t[AI_MAX_TOUCH];
    int nt = 0, np, i, pass;
    int deep = -1;
    float impact = 0.0f;

    (void)dt;
    if (!ai || !player || ai->n <= 0)
        return 0.0f;
    np = rb_gather_spheres(player, ps);
    if (np <= 0)
        return 0.0f;

    for (i = 0; i < ai->n; i++) {
        ai_car *a = &ai->car[i];
        int no, p, o;
        double dx = (double)a->rb.body.x[0] - player->body.x[0];
        double dy = (double)a->rb.body.x[1] - player->body.x[1];
        double dz = (double)a->rb.body.x[2] - player->body.x[2];

        if (dx * dx + dy * dy + dz * dz
            > (double)AI_COLLIDE_RANGE * AI_COLLIDE_RANGE)
            continue;                       /* broad phase, see ai.h */
        no = rb_gather_spheres(&a->rb, os);
        for (p = 0; p < np; p++) {
            for (o = 0; o < no; o++) {
                double ex = (double)ps[p][0] - os[o][0];
                double ey = (double)ps[p][1] - os[o][1];
                double ez = (double)ps[p][2] - os[o][2];
                double d2 = ex * ex + ey * ey + ez * ez;
                double sum = (double)ps[p][3] + os[o][3];
                double len, inv;

                if (d2 >= sum * sum || d2 < 1e-12)
                    continue;
                len = sqrt(d2);
                inv = 1.0 / len;
                if (nt >= AI_MAX_TOUCH)
                    break;
                t[nt].normal[0] = (float)(ex * inv);
                t[nt].normal[1] = (float)(ey * inv);
                t[nt].normal[2] = (float)(ez * inv);
                /* The contact point on the OPPONENT's surface, which is what the
                   lever arm has to be measured from. */
                t[nt].point[0] = (float)(os[o][0] + t[nt].normal[0] * os[o][3]);
                t[nt].point[1] = (float)(os[o][1] + t[nt].normal[1] * os[o][3]);
                t[nt].point[2] = (float)(os[o][2] + t[nt].normal[2] * os[o][3]);
                t[nt].depth = (float)(sum - len);
                t[nt].other = &a->rb;
                if (deep < 0 || t[nt].depth > t[deep].depth)
                    deep = nt;
                nt++;
            }
            if (nt >= AI_MAX_TOUCH)
                break;
        }
    }
    if (nt <= 0)
        return 0.0f;

    /* THE POSITIONAL HALF, the DEEPEST PAIR ONLY. There is no
     * carSubstepContact bisection on this path to stop the two proxies
     * overlapping in the first place, so something has to undo it, and
     * RB_PENETRATION_SLACK is the same margin the world contact leaves.
     *
     * Deepest-only mirrors rb_body_depenetrate, whose note says pushing out of
     * every overlapping sphere accumulates and walks a wedged car out of the
     * world. **Between two cars that is NOT what happens, and this comment used
     * to claim it was.** Measured against a build that pushes out of every pair:
     * the worst overlap left standing goes 0.008 -> 0.009 m, the peak speed does
     * not move at all, and the air under a glancing hit goes 0.11 -> 0.17 m. What
     * does change is that a car alongside stays in contact for 373 ticks instead
     * of 3 -- it is held out rather than glancing off. So this is the
     * conservative choice and the cheaper one, not a safety property: at most a
     * handful of pairs overlap at once here, where terrain offers dozens every
     * tick for as long as the car leans on it. */
    player->body.x[0] += t[deep].normal[0]
                       * (t[deep].depth + RB_PENETRATION_SLACK);
    player->body.x[1] += t[deep].normal[1]
                       * (t[deep].depth + RB_PENETRATION_SLACK);
    player->body.x[2] += t[deep].normal[2]
                       * (t[deep].depth + RB_PENETRATION_SLACK);
    rb_car_update_matrix(player);

    /* THE VELOCITY HALF -- rb_coll_resolve's law, over a moving second body.
     *
     * Same passes, same 0.02 gate and same 0.05 m/s target, same
     * rb_impulse_denom and rb_apply_impulse. The one extension is `vo`: the
     * opponent's own velocity AT THE CONTACT POINT is subtracted, because
     * rb_coll_resolve solves against a world that does not move and this one
     * does. Its mass does not appear at all -- the opponent is on rails, so it
     * takes the whole of the impulse's reaction and none of its effect. */
    for (pass = 0; pass < AI_CONTACT_PASSES; pass++) {
        int any = 0;
        for (i = 0; i < nt; i++) {
            float vp[3], vo[3], j[3];
            double vrel, dv, k;

            rb_point_velocity(&player->body, t[i].point, vp);
            rb_point_velocity(&t[i].other->body, t[i].point, vo);
            vrel = (double)(vp[0] - vo[0]) * t[i].normal[0]
                 + (double)(vp[1] - vo[1]) * t[i].normal[1]
                 + (double)(vp[2] - vo[2]) * t[i].normal[2];
            if (vrel > AI_CONTACT_VREL)
                continue;
            if (pass == 0 && -vrel > impact)
                impact = (float)-vrel;      /* for the sound, before any impulse */
            any = 1;
            dv = AI_CONTACT_SEP - vrel;
            if (dv < 0.0)
                dv = 0.0;
            k = rb_impulse_denom(player, t[i].point, t[i].normal);
            if (k < 1e-09)
                continue;
            j[0] = (float)(t[i].normal[0] * (dv / k));
            j[1] = (float)(t[i].normal[1] * (dv / k));
            j[2] = (float)(t[i].normal[2] * (dv / k));
            rb_apply_impulse(player, t[i].point, j);
        }
        if (!any)
            break;
    }
    return impact;
}

/* ---------------------------------------------------------------- accessors */

const float *ai_matrix(const ai_t *ai, int i)
{
    static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0,
                                     0, 0, 1, 0, 0, 0, 0, 1 };
    if (!ai || i < 0 || i >= ai->n)
        return ident;
    return ai->car[i].rb.m;
}

int ai_within(const ai_t *ai, int i, float x, float y, float z, float d)
{
    double dx, dy, dz;
    if (!ai || i < 0 || i >= ai->n)
        return 0;
    dx = (double)ai->car[i].rb.body.x[0] - x;
    dy = (double)ai->car[i].rb.body.x[1] - y;
    dz = (double)ai->car[i].rb.body.x[2] - z;
    return dx * dx + dy * dy + dz * dz <= (double)d * d;
}

int ai_player_place(const ai_t *ai)
{
    int i, place = 1;
    if (!ai)
        return 1;
    for (i = 0; i < ai->n; i++)
        if (ai->car[i].spine_dist > ai->player_dist)
            place++;
    return place;
}
