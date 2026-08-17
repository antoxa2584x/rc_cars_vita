/*
 * ai.c -- the AI opponents. See ai.h for the model and for what is NOT here.
 *
 * Every function below names the address it came from. Only four things are the
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
 *     nothing on this path integrates;
 *   - THE BUMP OFFSET and the contact solve over it, which is the whole of the
 *     "ai_bump_" and "ai_pair_" block below. The original has no such mechanism:
 *     it either replays a car or simulates one, and this is neither. See ai.h.
 *
 * The replay itself is untouched by all of that, and deliberately so: the offset
 * is composed onto the recorded pose at the last moment (ai_bump_apply) and the
 * recording is what everything upstream -- the cursor, the lead, the speed the
 * rubber band chases -- continues to be measured on.
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

static void ai_bump_derive(ai_car *a);
static void ai_bump_clamp(ai_car *a);
static void ai_bump_apply(ai_car *a);
static void ai_collide_field(ai_t *ai);

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
        /* After rbcar_init, because it measures the proxy this car ended up
           with rather than reading a table of it. */
        ai_bump_derive(a);
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
static void ai_pose_rec(ai_car *a)
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

/* ------------------------------------------------------------ the bump offset
 *
 * THE PORT'S, all of it -- ai.h has the model and every constant's anchor.
 * Nothing here is a transcription; the original either replays a car or
 * simulates one, and an opponent that is knocked off a replayed line and steers
 * back onto it is neither.
 */

/* The four per-car numbers, built once at load out of the car's OWN data:
 *
 *   reach   the furthest its collision proxy gets from its centre of mass. Not
 *           tabled -- measured off rb_gather_spheres, so a Hummer gets more room
 *           than a Buggy because it IS bigger, and a change to the proxy (the
 *           roof stations were added to it recently) carries through by itself.
 *           Rotation-invariant, so the pose it is measured at does not matter.
 *   limit   AI_BUMP_LIMIT_REACH times that.
 *   accel   the car's grip times gravity -- what its tyres can pull with.
 *   w       fixed BY the other two: sqrt(accel / limit) is the frequency at
 *           which a fully displaced car returns at exactly that limit.
 */
static void ai_bump_derive(ai_car *a)
{
    float s[RB_MAX_SPHERES][4];
    const rb_car_data *d = &RB_CARS[a->car];
    double reach = 0.0;
    int n, i;

    n = rb_gather_spheres(&a->rb, s);
    for (i = 0; i < n; i++) {
        double dx = (double)s[i][0] - a->rb.body.x[0];
        double dy = (double)s[i][1] - a->rb.body.x[1];
        double dz = (double)s[i][2] - a->rb.body.x[2];
        double r  = sqrt(dx * dx + dy * dy + dz * dz) + s[i][3];
        if (r > reach)
            reach = r;
    }
    /* A proxy that gathered nothing would give a zero limit, which is a car
       that cannot be bumped at all -- fall back to a wheel radius rather than
       to a silently rigid opponent. */
    if (reach < 1e-3)
        reach = (double)d->radius;

    a->bump_limit     = (float)(AI_BUMP_LIMIT_REACH * reach);
    a->bump_accel     = d->tune.coeff_rear_tires * RB_GRAVITY;
    a->bump_w         = (float)sqrt((double)a->bump_accel / a->bump_limit);
    a->bump_yaw_limit = (float)(AI_BUMP_YAW_LOCKS * d->steer_max_deg
                                * (3.14159265358979 / 180.0));
}

/* THE TERRAIN FOLLOW, which is a SEPARATE term from the offset and not part of
 * it: the ground under the displaced car minus the ground under the recorded
 * one. A car shoved sideways up a slope has to climb it, or it ends up buried on
 * the high side and hanging on the low one -- the recorded height is the height
 * of the line it is no longer on.
 *
 * Kept out of `off` deliberately. `off` is what the contact solve writes and the
 * spring pulls back to zero, and this is neither: it is a function of where the
 * car has been put, so feeding it back into the spring would have the terrain
 * pushing a car along its own line.
 *
 * Two probes, and none at all for a car on its line -- or for one that has not
 * moved AI_BUMP_PROBE_STEP sideways since the last pair, which is what keeps a
 * car in contact from paying for them once per depenetration pass. */
static float ai_bump_ground_dy(ai_car *a)
{
    const rb_world *w = a->rb.world;
    float y0, y1, n[3], ceil_y, dy;

    if (!w || !w->ground)
        return 0.0f;
    if (fabs((double)a->off[0]) + fabs((double)a->off[2]) < 1e-4) {
        /* straight up or nowhere: same column, same ground */
        a->off_gnd_at[0] = a->off_gnd_at[1] = 0.0f;
        return 0.0f;
    }
    if (fabs((double)a->off[0] - a->off_gnd_at[0])
        + fabs((double)a->off[2] - a->off_gnd_at[1]) < AI_BUMP_PROBE_STEP)
        return a->off_gnd;                      /* near enough; reuse it */
    a->off_gnd_at[0] = a->off[0];
    a->off_gnd_at[1] = a->off[2];
    ceil_y = a->rec_x[1] + AI_BUMP_CEIL;
    if (!w->ground(w->ctx, a->rec_x[0], a->rec_x[2], ceil_y, &y0, n))
        return 0.0f;
    if (!w->ground(w->ctx, a->rec_x[0] + a->off[0], a->rec_x[2] + a->off[2],
                   ceil_y, &y1, n))
        return 0.0f;
    dy = y1 - y0;
    if (dy >  AI_BUMP_MAX_LIFT) dy =  AI_BUMP_MAX_LIFT;
    if (dy < -AI_BUMP_MAX_LIFT) dy = -AI_BUMP_MAX_LIFT;
    return dy;
}

/* Compose the offset onto the recorded pose. IDEMPOTENT -- it always rebuilds
 * from (rec_x, rec_q), never from wherever the body happens to be -- which is
 * what lets the contact solve push a car and re-pose it several times inside one
 * tick without the pushes compounding.
 *
 * The zero cases are handled by not doing the arithmetic rather than by doing it
 * with zeroes: an untouched opponent must be BIT-IDENTICAL to one from before
 * any of this existed, and a quaternion multiply by identity followed by a
 * renormalise is not bit-identical, it is within an ulp. Every aitest
 * measurement of the replay depends on that. */
static void ai_bump_apply(ai_car *a)
{
    rb_body *b = &a->rb.body;
    int moved = (a->off[0] != 0.0f || a->off[2] != 0.0f);

    a->off_gnd = moved ? ai_bump_ground_dy(a) : 0.0f;
    b->x[0] = a->rec_x[0] + a->off[0];
    b->x[1] = a->rec_x[1] + a->off[1] + a->off_gnd;
    b->x[2] = a->rec_x[2] + a->off[2];

    if (a->off_yaw != 0.0f) {
        /* A world-space rotation about the car's own centre of mass, so it
           PRE-multiplies: the port's convention is dq/dt = 0.5 (0,w) (x) q with
           w in world space (PHYSICS.md, and see ai_diff_velocity). */
        float qy[4], q[4];
        qy[0] = (float)cos((double)a->off_yaw * 0.5);
        qy[1] = 0.0f;
        qy[2] = (float)sin((double)a->off_yaw * 0.5);
        qy[3] = 0.0f;
        rb_quat_mul(qy, a->rec_q, q);
        rb_quat_normalize(q);
        memcpy(b->q, q, sizeof(b->q));
        rb_update_inv_inertia_world(b);
    } else {
        memcpy(b->q, a->rec_q, sizeof(b->q));
    }
    rb_car_update_matrix(&a->rb);

    a->bump = (float)sqrt((double)a->off[0] * a->off[0]
                          + (double)a->off[1] * a->off[1]
                          + (double)a->off[2] * a->off[2]);
}

/* Write the pose for the current (cursor, u), then put the bump back on top of
   it. Everything upstream keeps reading the recording out of rec_x / rec_q. */
static void ai_pose(ai_car *a)
{
    ai_pose_rec(a);
    memcpy(a->rec_x, a->rb.body.x, sizeof(a->rec_x));
    memcpy(a->rec_q, a->rb.body.q, sizeof(a->rec_q));
    ai_bump_apply(a);
}

/* Hold the offset inside its limits, and KILL THE OUTWARD VELOCITY WHERE IT
 * DOES. Both halves are load-bearing and the second is the subtle one: an offset
 * velocity that keeps growing against a position that cannot move is a car
 * reporting that it is getting out of the way while standing still, and the
 * contact solve believes it -- vrel reads as separating, no impulse is applied,
 * and the player drives on into a car it is already inside. A clamped opponent
 * has to be RIGID, not merely stationary.
 *
 * Called after every push and every impulse as well as from the relax, because
 * the ten Gauss-Seidel passes can put a car on its limit mid-tick. */
static void ai_bump_clamp(ai_car *a)
{
    double mag;

    /* DOWN IS THE ONE ASYMMETRIC BOUND, and it is the whole of what stops "an
     * opponent can be lifted" turning into "an opponent can be driven into the
     * ground". Upward it is bounded only by the offset limit below -- a car hit
     * hard enough to ride over another one should, and the spring brings it back
     * -- but there is ground under it, and nothing on this path models the ground
     * holding it up. */
    if (a->off[1] < -AI_BUMP_MAX_SINK) {
        a->off[1] = -AI_BUMP_MAX_SINK;
        if (a->offv[1] < 0.0f) a->offv[1] = 0.0f;
    }
    /* Then ONE budget over all three axes, so `bump_limit` means what it says --
       how far from its line the car can be, full stop. Bounding the horizontal
       and the vertical separately would have let the two combine to 1.41 times
       it, which the ten-track survey duly caught at 0.627 m against 0.617. */
    mag = sqrt((double)a->off[0] * a->off[0] + (double)a->off[1] * a->off[1]
               + (double)a->off[2] * a->off[2]);
    if (mag > (double)a->bump_limit && mag > 1e-9) {
        double ux = a->off[0] / mag, uy = a->off[1] / mag, uz = a->off[2] / mag;
        double radial;
        a->off[0] = (float)(ux * a->bump_limit);
        a->off[1] = (float)(uy * a->bump_limit);
        a->off[2] = (float)(uz * a->bump_limit);
        radial = (double)a->offv[0] * ux + (double)a->offv[1] * uy
               + (double)a->offv[2] * uz;
        if (radial > 0.0) {
            a->offv[0] = (float)((double)a->offv[0] - radial * ux);
            a->offv[1] = (float)((double)a->offv[1] - radial * uy);
            a->offv[2] = (float)((double)a->offv[2] - radial * uz);
        }
    }
    if (a->off_yaw > a->bump_yaw_limit) {
        a->off_yaw = a->bump_yaw_limit;
        if (a->off_yawv > 0.0f) a->off_yawv = 0.0f;
    } else if (a->off_yaw < -a->bump_yaw_limit) {
        a->off_yaw = -a->bump_yaw_limit;
        if (a->off_yawv < 0.0f) a->off_yawv = 0.0f;
    }
}

/* The return to the line: a critically damped spring on the horizontal offset
 * and on the yaw, capped at what the car's tyres could actually pull with.
 *
 * The cap is not decoration. The spring's own peak is at the grip limit only
 * when the car is at full displacement and stationary relative to its line; a
 * bump that arrives while it is already moving back can ask for several times
 * that, and a car that recovers harder than it could corner is the thing that
 * would read as a rubber band rather than as driving. */
static void ai_bump_relax(ai_car *a, float dt)
{
    double k, c, ax, ay, az, mag, aw, cap_w;

    if (a->off[0] == 0.0f && a->off[1] == 0.0f && a->off[2] == 0.0f
        && a->offv[0] == 0.0f && a->offv[1] == 0.0f && a->offv[2] == 0.0f
        && a->off_yaw == 0.0f && a->off_yawv == 0.0f)
        return;                        /* on its line: exactly the recording */

    k = (double)a->bump_w * a->bump_w;
    c = 2.0 * a->bump_w;

    ax = -(k * a->off[0] + c * a->offv[0]);
    ay = -(k * a->off[1] + c * a->offv[1]);
    az = -(k * a->off[2] + c * a->offv[2]);
    mag = sqrt(ax * ax + ay * ay + az * az);
    if (mag > (double)a->bump_accel && mag > 1e-9) {
        double s = (double)a->bump_accel / mag;
        ax *= s;
        ay *= s;
        az *= s;
    }
    a->offv[0] = (float)((double)a->offv[0] + ax * dt);
    a->offv[1] = (float)((double)a->offv[1] + ay * dt);
    a->offv[2] = (float)((double)a->offv[2] + az * dt);
    a->off[0]  = (float)((double)a->off[0] + (double)a->offv[0] * dt);
    a->off[1]  = (float)((double)a->off[1] + (double)a->offv[1] * dt);
    a->off[2]  = (float)((double)a->off[2] + (double)a->offv[2] * dt);

    /* ai_bump_clamp holds both limits and, where it has to, kills the outward
       velocity with them -- leaving that in would store up a shove that is not
       going anywhere and spend it the moment the car came off the limit, which
       reads as a car spat sideways a second after the hit. */
    ai_bump_clamp(a);
    mag = sqrt((double)a->off[0] * a->off[0] + (double)a->off[1] * a->off[1]
               + (double)a->off[2] * a->off[2]);

    /* The yaw, on the same spring. Its acceleration budget is the linear one
       over the proxy's reach -- the same tyre force, applied at the end of the
       same lever. */
    cap_w = (double)a->bump_accel / ((double)a->bump_limit * 0.5);
    aw = -(k * a->off_yaw + c * a->off_yawv);
    if (aw >  cap_w) aw =  cap_w;
    if (aw < -cap_w) aw = -cap_w;
    a->off_yawv = (float)((double)a->off_yawv + aw * dt);
    a->off_yaw  = (float)((double)a->off_yaw + (double)a->off_yawv * dt);
    ai_bump_clamp(a);

    /* SNAP TO EXACTLY ZERO. See AI_BUMP_SNAP: a recovered opponent has to become
       the same car it was before it was touched, or the pose carries a
       micrometre of displacement for the rest of the race and ai_bump_apply
       keeps probing the ground for it. */
    if (mag < AI_BUMP_SNAP
        && fabs((double)a->offv[0]) < AI_BUMP_SNAP_V
        && fabs((double)a->offv[1]) < AI_BUMP_SNAP_V
        && fabs((double)a->offv[2]) < AI_BUMP_SNAP_V) {
        a->off[0] = a->off[1] = a->off[2] = 0.0f;
        a->offv[0] = a->offv[1] = a->offv[2] = 0.0f;
    }
    if (fabs((double)a->off_yaw) < AI_BUMP_SNAP_YAW
        && fabs((double)a->off_yawv) < AI_BUMP_SNAP_YAWV)
        a->off_yaw = a->off_yawv = 0.0f;
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
 * nothing because nothing integrates this body.
 *
 * IT DIFFERENCES THE RECORDING, not the body -- rec_x and rec_q, not body.x and
 * body.q, which since the bump offset exists are not the same thing. The speed
 * this produces is what the rubber band's moveTowards chases, and that has to be
 * the rate the RECORDING is being played at: fold a shove into it and a bumped
 * opponent reads as going faster than it is and slows itself down for it. The
 * bump's own contribution to a contact point's velocity is added where it
 * belongs, in ai_actor_point_vel. */
static void ai_diff_velocity(ai_car *a, const float x0[3], const float q0[4],
                             float dt)
{
    rb_body *b = &a->rb.body;
    const float *qn = a->rec_q;
    double inv = (dt > AI_EPS) ? 1.0 / (double)dt : 0.0;
    double dq[4], n, ang, k;
    int i;

    for (i = 0; i < 3; i++) {
        b->v[i] = (float)(((double)a->rec_x[i] - x0[i]) * inv);
        b->P[i] = (float)((double)b->mass * b->v[i]);
    }

    /* dq = q_new (x) conj(q_old); the port's convention is
       dq/dt = 0.5 * (0, w) (x) q with w in WORLD space (PHYSICS.md), so
       w = 2 * axis * (angle / dt) read off that delta. */
    dq[0] =  (double)qn[0] * q0[0] + (double)qn[1] * q0[1]
           + (double)qn[2] * q0[2] + (double)qn[3] * q0[3];
    dq[1] = -(double)qn[0] * q0[1] + (double)qn[1] * q0[0]
           - (double)qn[2] * q0[3] + (double)qn[3] * q0[2];
    dq[2] = -(double)qn[0] * q0[2] + (double)qn[2] * q0[0]
           - (double)qn[3] * q0[1] + (double)qn[1] * q0[3];
    dq[3] = -(double)qn[0] * q0[3] + (double)qn[3] * q0[0]
           - (double)qn[1] * q0[2] + (double)qn[2] * q0[1];
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
        /* Straight back onto its line. A restart with a car still leaning on an
           opponent would otherwise put it on the grid carrying the shove. */
        a->off[0] = a->off[1] = a->off[2] = 0.0f;
        a->offv[0] = a->offv[1] = a->offv[2] = 0.0f;
        a->off_yaw = a->off_yawv = 0.0f;
        a->off_gnd = a->off_gnd_at[0] = a->off_gnd_at[1] = 0.0f;
        a->bump = 0.0f;
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
        /* The RECORDED pose, not the bumped one -- see ai_diff_velocity. */
        memcpy(x0, a->rec_x, sizeof(x0));
        memcpy(q0, a->rec_q, sizeof(q0));
        a->speed_rec = sample_speed(a, a->cursor > 0 ? a->cursor - 1 : 0);

        /* FUN_004fd5e0: the lead, measured on the same spine as the player.
         * With no spine bound the lead is 0 and the coefficient is the static
         * product alone -- which is also what the original does, because
         * FUN_004fd5e0 returns 0 until the checkpoint list is up and
         * FUN_004fd4c0 then returns local_98 by itself. */
        /* ON THE RECORDED POSITION, not the bumped one. A shove sideways is not
         * race progress, and measuring the lead where the car has been knocked to
         * would let a contact reach the rubber band -- which would make "a bump
         * cannot touch the replay" false by a hair instead of exactly true, and
         * aitest part 9 checks it exactly. */
        if (have_spine
            && tr->spine(tr->ctx, a->rec_x[0], a->rec_x[1], a->rec_x[2],
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

        /* THE PORT'S: work off whatever the last tick's contacts left in the
         * offset and put the car back where that says it is. Last, so
         * everything above -- the cursor, the lead, the recorded speed, the
         * wheel spin -- is measured on the recording and not on the shove. */
        ai_bump_relax(a, dt);
        ai_bump_apply(a);
    }

    /* THE PORT'S, and the field's own business rather than the player's:
     * opponents are solid to each other too. Same routine, both sides bumpable.
     * After every car has been posed, because a pair solved against a pose that
     * is about to be overwritten does nothing at all. */
    ai_collide_field(ai);
}

/* ------------------------------------------------------------------ contact */

/* One overlapping sphere pair. `normal` points OUT of body B and toward body A,
   which is rb_coll_contact's own convention -- so an A approaching B has a
   negative relative normal velocity. */
typedef struct {
    float point[3];
    float normal[3];
    float depth;
} ai_touch;

#define AI_MAX_TOUCH 12

/* One side of a contact pair. `ai` is NULL for a body that takes the reaction in
   its own rigid state -- the player -- and non-NULL for one that takes it in a
   bump offset. Everything below is written against this so that player-against-
   opponent and opponent-against-opponent are the same solve. */
typedef struct {
    rb_car *car;
    ai_car *ai;
} ai_actor;

/* The overlapping pairs between two gathered proxies. */
static int ai_touch_list(const float as[][4], int na,
                         const float bs[][4], int nb,
                         ai_touch *t, int max)
{
    int p, o, nt = 0;

    for (p = 0; p < na && nt < max; p++) {
        for (o = 0; o < nb && nt < max; o++) {
            double ex = (double)as[p][0] - bs[o][0];
            double ey = (double)as[p][1] - bs[o][1];
            double ez = (double)as[p][2] - bs[o][2];
            double d2 = ex * ex + ey * ey + ez * ez;
            double sum = (double)as[p][3] + bs[o][3];
            double len, inv;

            if (d2 >= sum * sum || d2 < 1e-12)
                continue;
            len = sqrt(d2);
            inv = 1.0 / len;
            t[nt].normal[0] = (float)(ex * inv);
            t[nt].normal[1] = (float)(ey * inv);
            t[nt].normal[2] = (float)(ez * inv);
            /* The contact point on B's surface, which is what both lever arms
               are measured from. */
            t[nt].point[0] = (float)(bs[o][0] + t[nt].normal[0] * bs[o][3]);
            t[nt].point[1] = (float)(bs[o][1] + t[nt].normal[1] * bs[o][3]);
            t[nt].point[2] = (float)(bs[o][2] + t[nt].normal[2] * bs[o][3]);
            t[nt].depth = (float)(sum - len);
            nt++;
        }
    }
    return nt;
}

/* An opponent's velocity at a world point: the replay's own, plus what the bump
   offset is doing. rb_point_velocity cannot know about the second -- the offset
   is not in the body's state, it is composed onto its pose. */
static void ai_actor_point_vel(const ai_actor *b, const float p[3], float o[3])
{
    const ai_car *a = b->ai;
    float r[3];

    rb_point_velocity(&b->car->body, p, o);
    if (!a)
        return;
    r[0] = p[0] - b->car->body.x[0];
    r[2] = p[2] - b->car->body.x[2];
    /* w x r with w = (0, off_yawv, 0) is (w*rz, 0, -w*rx). */
    o[0] += a->offv[0] + a->off_yawv * r[2];
    o[1] += a->offv[1];
    o[2] += a->offv[2] - a->off_yawv * r[0];
}

/* The contact denominator: the change in the point's velocity along `n` per unit
 * impulse along `n`. For the player this is the engine's own rb_impulse_denom
 * (0x004754a0); for an opponent it is THE PORT'S counterpart of it, and it has to
 * agree with what ai_take_impulse actually does or the solve over- or
 * under-corrects on every pass. The linear halves are identical; the angular one
 * keeps only the YAW component of the response, because that is the only one
 * applied. */
static double ai_actor_denom(const ai_actor *b, const float point[3],
                             const float n[3])
{
    const rb_body *bd;
    float r[3], rn[3], t[3];
    double lin, ang;

    if (!b->ai)
        return rb_impulse_denom(b->car, point, n);

    bd = &b->car->body;
    r[0] = point[0] - bd->x[0];
    r[1] = point[1] - bd->x[1];
    r[2] = point[2] - bd->x[2];
    rn[0] = r[1] * n[2] - r[2] * n[1];
    rn[1] = r[2] * n[0] - r[0] * n[2];
    rn[2] = r[0] * n[1] - r[1] * n[0];
    rb_mat3_mul_vec3(bd->iinv, rn, t);
    /* ((0, t1, 0) x r) . n, the yaw-only form of rb_impulse_denom's tail. */
    ang = (double)t[1] * r[2] * n[0] - (double)t[1] * r[0] * n[2];
    lin = bd->inv_mass;
    return lin + ang;
}

/* The reaction an opponent takes: into the OFFSET, never into the replay.
 *
 * Linear in all three axes -- what bounds the vertical is ai_bump_clamp's
 * one-centimetre floor and the shared offset budget, not a dropped term -- and
 * YAW ONLY of the angular, which ai_actor_denom is built to match. See ai.h for
 * why roll and pitch are dropped. */
static void ai_take_impulse(ai_car *a, const float point[3], const float j[3])
{
    rb_body *bd = &a->rb.body;
    float r[3], rj[3], dw[3];

    a->offv[0] = (float)((double)a->offv[0] + (double)j[0] * bd->inv_mass);
    a->offv[1] = (float)((double)a->offv[1] + (double)j[1] * bd->inv_mass);
    a->offv[2] = (float)((double)a->offv[2] + (double)j[2] * bd->inv_mass);

    r[0] = point[0] - bd->x[0];
    r[1] = point[1] - bd->x[1];
    r[2] = point[2] - bd->x[2];
    rj[0] = r[1] * j[2] - r[2] * j[1];
    rj[1] = r[2] * j[0] - r[0] * j[2];
    rj[2] = r[0] * j[1] - r[1] * j[0];
    rb_mat3_mul_vec3(bd->iinv, rj, dw);
    a->off_yawv = (float)((double)a->off_yawv + dw[1]);

    /* A car already at its limit must not accumulate velocity it cannot spend --
       see ai_bump_clamp, and the 11 cm of burial that finding it cost. */
    ai_bump_clamp(a);
}

static void ai_actor_impulse(ai_actor *b, const float point[3],
                             const float j[3])
{
    if (b->ai)
        ai_take_impulse(b->ai, point, j);
    else
        rb_apply_impulse(b->car, point, j);
}

/* Move a body by `dv`, and report in `taken` how much of that it ACTUALLY did.
 * An opponent moves its offset and re-poses; a real body moves itself and always
 * takes the lot.
 *
 * `taken` is the whole point. An opponent can REFUSE part of a move -- it will
 * not pass its limit, and it will not be driven into the ground -- and the caller
 * has to know, because a separation neither body performs is a pair left inside
 * each other. That one was measured before it was reasoned about: with the
 * refused share silently dropped, a car ramming an opponent already at its limit
 * settled 11 cm INSIDE it, which is exactly how far the player travels in one
 * tick at 6.7 m/s. */
static void ai_actor_move(ai_actor *b, const float dv[3], float taken[3])
{
    ai_car *a = b->ai;
    float before[3];
    int k;

    if (!a) {
        for (k = 0; k < 3; k++) {
            b->car->body.x[k] += dv[k];
            taken[k] = dv[k];
        }
        rb_car_update_matrix(b->car);
        return;
    }
    memcpy(before, a->off, sizeof(before));
    for (k = 0; k < 3; k++)
        a->off[k] += dv[k];
    ai_bump_clamp(a);
    ai_bump_apply(a);
    for (k = 0; k < 3; k++)
        taken[k] = a->off[k] - before[k];
}

/* One pair of proxies, already gathered. -> the number of overlapping pairs, and
 * `impact` (may be NULL) gets the hardest closing speed BEFORE any impulse, which
 * is what main.c raises car_cdt_car off.
 *
 * The law is rb_coll_resolve's (0x004f0750) over two moving bodies -- see ai.h.
 */
static int ai_pair_resolve(ai_actor *A, ai_actor *B,
                           const float as[][4], int na,
                           const float bs[][4], int nb,
                           float *impact)
{
    ai_touch t[AI_MAX_TOUCH];
    int nt, i, pass, deep = 0;
    double wa, wb, ima, imb;

    nt = ai_touch_list(as, na, bs, nb, t, AI_MAX_TOUCH);
    if (nt <= 0)
        return 0;
    for (i = 1; i < nt; i++)
        if (t[i].depth > t[deep].depth)
            deep = i;

    /* THE POSITIONAL HALF: THE DEEPEST PAIR, RE-MEASURED, SPLIT BY MASS.
     *
     * There is no carSubstepContact bisection on this path to stop the proxies
     * overlapping in the first place, so something has to undo it, and
     * RB_PENETRATION_SLACK is the same margin the world contact leaves.
     *
     * SPLITTING it is what the one-way version could not do, and it is the whole
     * of the reported "the Buggy gets stuck in the Hummer": with the opponent
     * immovable the player was pushed out of one sphere pair per tick while the
     * opponent's next pose put it straight back in. Now each body carries away
     * its share -- and the opponent's share PERSISTS, because the offset is state
     * and the return spring takes a second to spend it, so the pair has time to
     * come apart.
     *
     * ITERATING it is the other half, and that one is measured. A proxy is 13
     * spheres and they wedge: resolving the deepest pair pushes the car into a
     * different pair, and with one pass per tick a car bulldozing an opponent
     * that has reached its limit sat 7.4 cm inside it, alternating between which
     * pair was worst. Each pass RE-GATHERS both proxies, so it always works on
     * the overlap that is actually there and cannot over-correct the way pushing
     * out of every pair in one list does -- which is the rule ai.c used to have a
     * note against, and the note stands: this is not that.
     *
     * AI_DEPEN_PASSES carries the measured table and the reason the window it was
     * measured over is the arrival rather than a ten-second grind. */
    ima = A->car->body.inv_mass;
    imb = B->ai ? B->car->body.inv_mass : 0.0;
    if (ima + imb < 1e-12) {
        wa = 1.0;
        wb = 0.0;
    } else {
        wa = ima / (ima + imb);
        wb = 1.0 - wa;
    }
    {
        float as2[RB_MAX_SPHERES][4], bs2[RB_MAX_SPHERES][4];
        ai_touch t2[AI_MAX_TOUCH];
        const ai_touch *cur = t;
        int n2 = nt, k, deep2 = deep;

        for (k = 0; k < AI_DEPEN_PASSES; k++) {
            float d, sep[3], mv[3], took[3];
            int c;

            if (k > 0) {
                int ga = rb_gather_spheres(A->car, as2);
                int gb = rb_gather_spheres(B->car, bs2);
                n2 = ai_touch_list(as2, ga, bs2, gb, t2, AI_MAX_TOUCH);
                if (n2 <= 0)
                    break;
                cur = t2;
                deep2 = 0;
                for (c = 1; c < n2; c++)
                    if (t2[c].depth > t2[deep2].depth)
                        deep2 = c;
            }
            if (cur[deep2].depth <= 0.0f)
                break;
            d = cur[deep2].depth + RB_PENETRATION_SLACK;
            for (c = 0; c < 3; c++)
                sep[c] = cur[deep2].normal[c] * d;   /* A relative to B */

            /* HORIZONTALLY, split by mass: B takes its share and A covers
               whatever B refused, so the pair separates by the whole of it
               however much of the way B could come. */
            mv[0] = -sep[0] * (float)wb;
            mv[1] = 0.0f;
            mv[2] = -sep[2] * (float)wb;
            ai_actor_move(B, mv, took);
            mv[0] = sep[0] + took[0];
            mv[1] = 0.0f;
            mv[2] = sep[2] + took[2];
            ai_actor_move(A, mv, took);

            /* VERTICALLY, THE WHOLE OF IT GOES TO WHICHEVER CAR IS ON TOP, and
             * none of it to the one underneath.
             *
             * The reason is that there is GROUND under these cars and this solve
             * cannot see it. Pushing the lower one down is asking the world to
             * absorb a separation it was never told about, and if it absorbs
             * enough of one the car ends up inside terrain -- which col_sphere
             * cannot report at all once a sphere is buried deeper than its own
             * radius (rb.h). The car above has open air over it and no such
             * problem.
             *
             * HONESTLY: that failure has not been observed. Splitting the
             * vertical by mass instead passes every check in aitest, and the
             * Buggy's ride height after a Hummer drives over it is 0.145 m
             * against a not-run-over control's 0.146. So this is a guard rather
             * than a fix, and what it is measurably worth is separation: two
             * opponents over a minute on each of the ten tracks come 0.070 m
             * inside each other under the mass split and 0.031 m under this,
             * and the head-on Buggy-into-Hummer ram goes from 44 contact ticks
             * to 9 -- the pair comes apart instead of grinding along. */
            if (sep[1] > 0.0f) {
                mv[0] = mv[2] = 0.0f;
                mv[1] = sep[1];
                ai_actor_move(A, mv, took);
            } else if (sep[1] < 0.0f) {
                mv[0] = mv[2] = 0.0f;
                mv[1] = -sep[1];
                ai_actor_move(B, mv, took);
            }
        }
    }

    /* THE VELOCITY HALF. Same ten passes, same 0.02 gate, same 0.05 m/s target
     * as rb_coll_resolve; the denominator is the PAIR's, so the impulse delivers
     * its dv across both bodies rather than all of it into one. */
    for (pass = 0; pass < AI_CONTACT_PASSES; pass++) {
        int any = 0;
        for (i = 0; i < nt; i++) {
            float va[3], vb[3], j[3];
            double vrel, dv, k;

            ai_actor_point_vel(A, t[i].point, va);
            ai_actor_point_vel(B, t[i].point, vb);
            vrel = (double)(va[0] - vb[0]) * t[i].normal[0]
                 + (double)(va[1] - vb[1]) * t[i].normal[1]
                 + (double)(va[2] - vb[2]) * t[i].normal[2];
            if (vrel > AI_CONTACT_VREL)
                continue;
            if (pass == 0 && impact && -vrel > *impact)
                *impact = (float)-vrel;   /* the sound, before any impulse */
            any = 1;
            dv = AI_CONTACT_SEP - vrel;
            if (dv < 0.0)
                dv = 0.0;
            k = ai_actor_denom(A, t[i].point, t[i].normal)
              + ai_actor_denom(B, t[i].point, t[i].normal);
            if (k < 1e-09)
                continue;
            j[0] = (float)(t[i].normal[0] * (dv / k));
            j[1] = (float)(t[i].normal[1] * (dv / k));
            j[2] = (float)(t[i].normal[2] * (dv / k));
            ai_actor_impulse(A, t[i].point, j);
            j[0] = -j[0]; j[1] = -j[1]; j[2] = -j[2];
            ai_actor_impulse(B, t[i].point, j);
        }
        if (!any)
            break;
    }
    return nt;
}

/* Are these two centres close enough that their proxies could be touching? */
static int ai_near(const rb_car *a, const rb_car *b)
{
    double dx = (double)a->body.x[0] - b->body.x[0];
    double dy = (double)a->body.x[1] - b->body.x[1];
    double dz = (double)a->body.x[2] - b->body.x[2];
    return dx * dx + dy * dy + dz * dz
           <= (double)AI_COLLIDE_RANGE * AI_COLLIDE_RANGE;
}

void ai_bump_impulse(ai_t *ai, int i, const float point[3], const float j[3])
{
    if (!ai || i < 0 || i >= ai->n)
        return;
    ai_take_impulse(&ai->car[i], point, j);
}

/* THE PORT'S: opponent against opponent, run from ai_step. With both of them
 * bumpable, two cars sharing a corner passing through each other is the one
 * remaining way for the field to look like a recording. Same solve, both sides
 * taking their share into their own offset.
 *
 * No sound is raised: car_cdt_car is the PLAYER's collision cue and there is
 * nothing in the bank for two opponents touching each other out of sight. */
static void ai_collide_field(ai_t *ai)
{
    float as[RB_MAX_SPHERES][4], bs[RB_MAX_SPHERES][4];
    int i, j;

    for (i = 0; i < ai->n; i++) {
        for (j = i + 1; j < ai->n; j++) {
            ai_actor A, B;
            int na, nb;

            if (!ai_near(&ai->car[i].rb, &ai->car[j].rb))
                continue;
            na = rb_gather_spheres(&ai->car[i].rb, as);
            nb = rb_gather_spheres(&ai->car[j].rb, bs);
            if (na <= 0 || nb <= 0)
                continue;
            A.car = &ai->car[i].rb; A.ai = &ai->car[i];
            B.car = &ai->car[j].rb; B.ai = &ai->car[j];
            ai_pair_resolve(&A, &B, as, na, bs, nb, NULL);
        }
    }
}

float ai_collide_player(ai_t *ai, rb_car *player, float dt)
{
    float ps[RB_MAX_SPHERES][4], os[RB_MAX_SPHERES][4];
    int np, i;
    float impact = 0.0f;

    (void)dt;
    if (!ai || !player || ai->n <= 0)
        return 0.0f;
    np = rb_gather_spheres(player, ps);
    if (np <= 0)
        return 0.0f;

    for (i = 0; i < ai->n; i++) {
        ai_car *a = &ai->car[i];
        ai_actor A, B;
        int no;

        if (!ai_near(player, &a->rb))
            continue;                       /* broad phase, see ai.h */
        no = rb_gather_spheres(&a->rb, os);
        if (no <= 0)
            continue;
        A.car = player;  A.ai = NULL;
        B.car = &a->rb;  B.ai = a;
        if (ai_pair_resolve(&A, &B, ps, np, os, no, &impact) > 0) {
            /* The player moved, so its proxy is stale for the next opponent --
               and being between two of them is exactly when that matters. */
            np = rb_gather_spheres(player, ps);
        }
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
