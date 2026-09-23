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
#include "scene.h"      /* ASSET_IOBUF */
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

/* HOW FAR UP A CONTACT MAY EVER PUT THIS CAR: one car height, off its own
   proxy, so a Hummer may be lifted further than a Buggy because it IS taller
   and a change to the proxy carries through by itself. See ai_bump_clamp for
   why the bound exists and why it is not in ai_pair_resolve. */
static void ai_bump_up_derive(ai_car *a)
{
    float s[RB_MAX_SPHERES][4];
    double lo = 1e30, hi = -1e30;
    int n, i;

    n = rb_gather_spheres(&a->rb, s);
    for (i = 0; i < n; i++) {
        if ((double)s[i][1] - s[i][3] < lo) lo = (double)s[i][1] - s[i][3];
        if ((double)s[i][1] + s[i][3] > hi) hi = (double)s[i][1] + s[i][3];
    }
    /* A proxy that gathered nothing leaves the bound where it was, which for a
       car that has never been posed is `bump_reach' -- generous, and the old
       behaviour, rather than a car that cannot be lifted at all. */
    if (n > 0 && hi > lo)
        a->bump_up = (float)(hi - lo);
    else if (a->bump_up <= 0.0f)
        a->bump_up = a->bump_reach;
}

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

/* WHERE A LAP RESUMES, and it is ONE SAMPLE further on than this used to be.
 *
 * The original carries its cursor as (index, TIME) and the rewind sets both:
 * `*piVar1 = phys+0x43b8` (the cycle start) and `piVar1[1] = t[cycle_start]`, so
 * the next FUN_00502ea0 reproduces `s[cycle_start]` exactly. This carries
 * (cursor, u) with the convention `cursor = k, u = 0` -> `s[k-1]` (ai_pose_rec
 * interpolates s[cursor-1] .. s[cursor]), so the equivalent cursor is
 * `cycle_start + 1` and not `cycle_start`.
 *
 * It was `cycle_start`, which resumed every lap on `s[cycle_start - 1]`: one
 * segment -- 0.030 to 0.383 m over the 50 shipped profiles -- short, on every
 * lap after the first. The loader already disagreed with it, which is what makes
 * this an internal inconsistency and not only a divergence: ai_car.lead_in is
 * the arc `s[0..cycle_start]` and ai_car.lap_len is what is left of path_len, so
 * both of them say the loop begins AT `s[cycle_start]`.
 *
 * One function because two places need the same number and they have to agree --
 * ai_step's wrap and ai_path_ahead's, whose own comment says so. */
static int ai_cycle_cursor(const ai_car *a)
{
    int c = a->cycle_start > 0 ? a->cycle_start + 1 : 1;
    /* The loader rejects cycle_start >= n, so this cannot exceed n; the clamp is
       for the one value it can reach, n, which would index s[n]. */
    if (c > a->n - 1)
        c = a->n - 1;
    if (c < 1)
        c = 1;
    return c;
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

/* See ai.h. A file static rather than an ai_t field because it has to be known
   while the roster is being BUILT, which is before there is an ai_t to ask. */
static int g_skill_field;

void ai_set_skill_field(int on)
{
    g_skill_field = on ? 1 : 0;
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
    /* See ASSET_IOBUF in scene.h: newlib's fread never reads past the FILE
       buffer, so an unset one turns the load into 1 KB syscalls. */
    setvbuf(f, NULL, _IOFBF, ASSET_IOBUF);
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "AIP1", 4) != 0) {
        rlog("ai: %s is not an AIP1 file\n", path);
        fclose(f);
        return 0;
    }
    memcpy(&n_file, hdr + 4, 4);

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    /* The header's opponent count is FILE DATA and the record array below is
     * indexed on it, so the file has to be big enough to hold that many
     * records -- not merely one. The sample BLOCKS were bounded in the loop and
     * the record array was not, which is the same rule missed one level up:
     * a .aip claiming five opponents with two records' worth of bytes read past
     * the blob at the memcpy below (ASan heap-buffer-overflow, ai.c:188). The
     * n_file bound is tested first so the multiply cannot overflow. */
    if (n_file == 0 || n_file > AI_MAX_OPPONENTS
        || size < (long)(12 + (long)n_file * AI_RECORD_BYTES)) {
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

        if ((ai->championship || g_skill_field) && !(u16[2] & mask))
            continue;
        /* The array bound, and nothing else: the FIELD SIZE is the difficulty
           mask's, above -- three at easy, five at hard -- and ai.h says why
           this stopped being a cap of its own. Tested here rather than at the
           array bound so the entries that do start keep their own FILE slot,
           and therefore their own spline. */
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
        /* THE LEAD-IN, walked once: the polyline from sample 0 to the sample the
           replay rejoins at. What is left of `path_len` is one lap. See
           ai_car.lap_len for the measurements and for the bug it fixes. */
        {
            int j;
            double run = 0.0;
            for (j = 1; j <= a->cycle_start && j < a->n; j++) {
                double dx = a->s[j].p[0] - a->s[j-1].p[0];
                double dy = a->s[j].p[1] - a->s[j-1].p[1];
                double dz = a->s[j].p[2] - a->s[j-1].p[2];
                run += sqrt(dx * dx + dy * dy + dz * dz);
            }
            a->lead_in = (float)run;
            a->lap_len = a->path_len - a->lead_in;
            /* A profile whose cycle starts at 0, or a hand-edited one whose
               lead-in swallows the path: the whole polyline is the lap, which is
               what this did before the distinction existed. */
            if (!(a->lap_len > 1e-3f)) {
                a->lead_in = 0.0f;
                a->lap_len = a->path_len;
            }
        }
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
             " (lead %.1f + lap %.0f)  %.1f s  b%d r%d t%d\n",
             a->slot, a->name, a->path, a->car, a->n, a->cycle_start,
             a->path_len, a->lead_in, a->lap_len,
             a->duration, a->boost, a->reson, a->tires);
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

    a->bump_reach     = (float)reach;
    a->bump_ref       = (float)(AI_BUMP_REF_REACH * reach);
    a->bump_accel     = d->tune.coeff_rear_tires * RB_GRAVITY;
    /* THE SPRING is set by the car's SIZE and the LIMIT by its GRIP, and they
     * are different questions -- see ai.h. `ref` is the displacement at which
     * the return wants the whole of the car's grip; `limit` is how far that
     * grip could ever let it slide, from the car's own recovered top speed.
     * speed_boost_max is km/h (rb.h), hence the /3.6. */
    {
        double vtop = (double)d->tune.speed_boost_max / 3.6;
        a->bump_limit = (float)(vtop * vtop / (2.0 * a->bump_accel));
    }
    a->bump_w         = (float)sqrt((double)a->bump_accel / a->bump_ref);
    a->bump_yaw_limit = (float)(AI_BUMP_YAW_LOCKS * d->steer_max_deg
                                * (3.14159265358979 / 180.0));
    a->bump_wall      = -1.0f;         /* not measured */
    a->bump_up        = 0.0f;
    ai_bump_up_derive(a);
}

/* HOW FAR THE LEVEL LETS THIS SHOVE GO, along the offset's own horizontal
 * direction. See ai.h, "the wall stop". -> a horizontal distance, or a negative
 * number when the world cannot be asked, which every caller reads as "no limit".
 *
 * Amortised on AI_WALL_STEP for the reason AI_BUMP_PROBE_STEP gives: this runs
 * from ai_bump_clamp, which the contact solve calls after every push.
 */
static float ai_bump_wall(ai_car *a)
{
    const rb_world *w = a->rb.world;
    float clear, y0, y1, n[3], ceil_y, ux, uz;
    float A[3], B[3], lo, hi;
    double d;
    int i;

    if (!w || !w->segment || !w->ground)
        return -1.0f;
    d = sqrt((double)a->off[0] * a->off[0] + (double)a->off[2] * a->off[2]);
    if (d < 1e-4)
        return -1.0f;                  /* straight up or nowhere: no wall to hit */
    /* AND NOT FOR AN ORDINARY KNOCK. Inside the spring's own reference
     * displacement the car is where the whole model used to allow it to be, and
     * the ten-track survey behind ai.h's note found something in the way there
     * 1.3% of the time -- against 31% at 2.5 m and 64% at 5. Paying six ground
     * probes and five segment queries for that is the wrong trade, and this is
     * where the cost of the feature is kept off every tap. */
    if (d <= (double)a->bump_ref)
        return -1.0f;
    /* THE CACHE IS KEYED ON WHERE THE CAR IS, not just on how far off it is.
     * The offset is measured from a recorded pose that is MOVING -- 12 cm a tick
     * at racing speed -- so the same offset vector points at different geometry
     * every tick, and a key on `off` alone goes on answering with a wall the car
     * has already driven past. What the amortisation is really for is the
     * eight depenetration passes inside ONE tick, and those move `off` by
     * millimetres and `rec_x` not at all. */
    if (a->bump_wall >= 0.0f
        && fabs((double)a->off[0] + a->rec_x[0] - a->bump_wall_at[0])
         + fabs((double)a->off[2] + a->rec_x[2] - a->bump_wall_at[1])
           < AI_WALL_STEP)
        return a->bump_wall;           /* near enough; reuse it */
    a->bump_wall_at[0] = a->off[0] + a->rec_x[0];
    a->bump_wall_at[1] = a->off[2] + a->rec_x[2];

    ux = (float)(a->off[0] / d);
    uz = (float)(a->off[2] / d);
    clear = AI_WALL_CLEAR_EXTENT * 0.5f * RB_CARS[a->car].extent[1];
    ceil_y = a->rec_x[1] + AI_BUMP_CEIL;
    if (!w->ground(w->ctx, a->rec_x[0], a->rec_x[2], ceil_y, &y0, n)) {
        a->bump_wall = -1.0f;
        return -1.0f;
    }
    A[0] = a->rec_x[0]; A[1] = y0 + clear; A[2] = a->rec_x[2];

    /* the far end first: clear all the way and there is nothing to bisect */
    lo = 0.0f;
    hi = (float)d;
    B[0] = a->rec_x[0] + ux * hi;
    B[2] = a->rec_x[2] + uz * hi;
    if (!w->ground(w->ctx, B[0], B[2], ceil_y, &y1, n))
        y1 = y0;
    B[1] = y1 + clear;
    if (!w->segment(w->ctx, A, B)) {
        a->bump_wall = -1.0f;          /* nothing in the way at this reach */
        return -1.0f;
    }
    /* BISECT ON THE REACH, not on the segment: each probe is its own ground
       query at its own point, so the test walks the terrain instead of cutting
       across it -- which is the whole reason a slope does not read as a wall. */
    for (i = 0; i < AI_WALL_BISECT; i++) {
        float mid = 0.5f * (lo + hi);
        B[0] = a->rec_x[0] + ux * mid;
        B[2] = a->rec_x[2] + uz * mid;
        if (!w->ground(w->ctx, B[0], B[2], ceil_y, &y1, n))
            y1 = y0;
        B[1] = y1 + clear;
        if (w->segment(w->ctx, A, B)) hi = mid; else lo = mid;
    }
    a->bump_wall = lo;
    return lo;
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
        /* straight up or nowhere: same column, same ground. The key is
           poisoned rather than zeroed -- zero is a position like any other now
           that rec_x is in it. */
        a->off_gnd_at[0] = a->off_gnd_at[1] = 1e30f;
        return 0.0f;
    }
    /* KEYED ON WHERE THE CAR IS, not on how far off its line it is. The offset
     * is measured from a recorded pose travelling 12 cm a tick, so an unchanged
     * `off` is a DIFFERENT piece of ground every tick and a key on `off` alone
     * answers with the height difference from somewhere the car has left. That
     * cost nothing while the offset was capped at 0.63 m and the difference was
     * millimetres; at the metres a shove now reaches, the ground it describes
     * can be a different piece of hillside. What the amortisation is for is the
     * eight depenetration passes
     * inside ONE tick, and those move `off` by millimetres and `rec_x` not at
     * all -- so both terms belong in the key and the saving is kept. */
    if (fabs((double)a->off[0] + a->rec_x[0] - a->off_gnd_at[0])
        + fabs((double)a->off[2] + a->rec_x[2] - a->off_gnd_at[1])
          < AI_BUMP_PROBE_STEP)
        return a->off_gnd;                      /* near enough; reuse it */
    a->off_gnd_at[0] = a->off[0] + a->rec_x[0];
    a->off_gnd_at[1] = a->off[2] + a->rec_x[2];
    ceil_y = a->rec_x[1] + AI_BUMP_CEIL;
    if (!w->ground(w->ctx, a->rec_x[0], a->rec_x[2], ceil_y, &y0, n))
        return 0.0f;
    if (!w->ground(w->ctx, a->rec_x[0] + a->off[0], a->rec_x[2] + a->off[2],
                   ceil_y, &y1, n))
        return 0.0f;
    dy = y1 - y0;
    /* HOW FAR THE FOLLOW MAY CLIMB, and it is now the DISTANCE that says so.
     *
     * A flat AI_BUMP_MAX_LIFT was right while a shove could carry a car 0.63 m,
     * where 0.35 m of height change is a probe that has landed on something it
     * should not have. Over the metres a shove reaches now it is the binding
     * failure instead: on a bank, ground that legitimately rises a metre over
     * two is clamped to 0.35 and the car is left buried in it. Measured with the
     * clamp flat, 20 of 480 hard shoves ended deeper in the level than an
     * unshoved twin; with the bound below, 17. It is not the whole of that
     * residual -- see known-issues.md -- but a car standing on a bank it climbed
     * is the case this one is for.
     *
     * So the bound is what a DRIVABLE slope could do over the distance
     * travelled: the engine's own 46-degree floor cone -- AI_TOP_COS, the angle
     * carDriveForce calls the difference between a floor and a wall -- times the
     * horizontal displacement, and never less than the old constant. Anything
     * steeper than that is not ground the car could be standing on, and is left
     * clamped for exactly the reason the constant was introduced. */
    {
        float lift = AI_BUMP_MAX_LIFT;
        float horiz = (float)sqrt((double)a->off[0] * a->off[0]
                                  + (double)a->off[2] * a->off[2]);
        float tan_cone = (float)(sqrt(1.0 - (double)AI_TOP_COS * AI_TOP_COS)
                                 / (double)AI_TOP_COS);
        if (horiz * tan_cone > lift)
            lift = horiz * tan_cone;
        if (dy >  lift) dy =  lift;
        if (dy < -lift) dy = -lift;
    }
    return dy;
}

/* HOLD THE OFFSET INSIDE WHAT THE LEVEL ALLOWS -- by refusing to let it GROW
 * past a wall, and by nothing else.
 *
 * TWO VERSIONS OF THIS WERE WRONG BEFORE THIS ONE, both in the same way: they
 * treated ai_bump_wall's answer as somewhere the car had to BE, and moved it
 * there. The offset is measured from a pose that is DRIVING, so geometry arrives
 * beside a held-out car without the car moving at all and the allowed distance
 * collapses between one tick and the next --
 *
 *   - applied at once, that is a teleport. All 60 sustained holds across the ten
 *     tracks did it, worst 1.95 m gone in a single tick, from 1.95: straight
 *     home. Reported as "ai car spawns on its way if long push it to side".
 *   - applied as a grip-limited slide, it is a dart: the correction runs at
 *     sqrt(2 * accel * excess), which is 4.4 m/s at a metre and a half and moves
 *     the offset's POSITION, so nothing else in the model sees it as speed.
 *
 * A wall arriving next to a car is not a reason to pull the car in. It is a
 * reason to stop it going further out, and the spring is ALREADY pulling it home
 * at the car's own grip -- which is the rate a car recovers at and the rate
 * everything else here runs on. So this refuses growth and nothing more: the
 * offset may keep whatever it had when the tick began, and may not add to it in
 * a direction the level has closed. A car caught inside a fence is out of it in
 * about the second the spring takes, at a speed that reads as a car sliding.
 *
 * `h0` is the horizontal offset at the top of the relax, before the spring
 * integrated -- so "what it had" means before this tick's growth, not after. */
static void ai_bump_wall_relax(ai_car *a, float h0)
{
    float wall = ai_bump_wall(a);
    double h, allowed, ux, uz, radial;

    if (wall < 0.0f)
        return;                        /* nothing in the way, or nothing to ask */
    allowed = (double)wall;
    if ((double)h0 > allowed)
        allowed = (double)h0;          /* never take away what it already had */
    h = sqrt((double)a->off[0] * a->off[0] + (double)a->off[2] * a->off[2]);
    if (h <= allowed || h < 1e-9)
        return;
    ux = a->off[0] / h;
    uz = a->off[2] / h;
    a->off[0] = (float)(ux * allowed);
    a->off[2] = (float)(uz * allowed);
    /* and the outward velocity goes with it, for the reason ai_bump_clamp's
       header gives: a car reporting that it is getting out of the way while a
       wall holds it still is one the contact solve will drive straight into. */
    radial = (double)a->offv[0] * ux + (double)a->offv[2] * uz;
    if (radial > 0.0) {
        a->offv[0] = (float)((double)a->offv[0] - radial * ux);
        a->offv[2] = (float)((double)a->offv[2] - radial * uz);
    }
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
/* HOW FAR THE CAR IS FROM ITS OWN RECORDED LINE, and it has ONE meaning.
 *
 * It used to be `|off|`, which was the same number while the offset was the only
 * thing that could move a car off its recording. It is not any more: a simulated
 * car is moved by the integrator and the ease home is a pose blend, so `|off|`
 * reads ZERO for a car that is metres away. Measured as `bump` alternating
 * 0.000 <-> 0.238 tick by tick across the mode switch while the car's actual
 * motion was a smooth 0.10 m -- a quantity that flips between two definitions
 * looks exactly like a teleport to anything watching it, which is what
 * `traps.md` means by TELEMETRY IS NOT THE THING.
 *
 * `body - rec` is true under every mechanism, and on the plain replay path it is
 * `|off|` exactly, because there the body IS `rec + off`. */
static void ai_bump_measure(ai_car *a)
{
    a->bump = (float)sqrt(
        (double)(a->rb.body.x[0] - a->rec_x[0]) * (a->rb.body.x[0] - a->rec_x[0])
      + (double)(a->rb.body.x[1] - a->rec_x[1]) * (a->rb.body.x[1] - a->rec_x[1])
      + (double)(a->rb.body.x[2] - a->rec_x[2]) * (a->rb.body.x[2] - a->rec_x[2]));
}

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

    ai_bump_measure(a);
}

/* --------------------------------------------------------------- the lap seam
 *
 * THE RECORDING DOES NOT CLOSE, AND THE ORIGINAL CARRIES THE CAR ACROSS THE GAP
 * RATHER THAN MOVING IT.
 *
 * A profile's loop is `s[cycle_start .. n-1]` and it very nearly closes on
 * itself, but only nearly: measured over the 50 shipped profiles the gap from
 * `s[n-1]` to `s[cycle_start]` is 0.003 to 0.217 m across and 0 to 0.076 m
 * vertically, with up to 12.22 degrees between the two recorded orientations.
 * So a replay that rewinds its cursor and writes the new lap's pose MOVES the
 * car by that much in one tick, and -- because the pose it moved from is a tick
 * of ordinary travel away -- a finite difference taken across the rewind reads
 * up to 17.7 m/s, which is under AI_TELEPORT_SPEED and so caught by nothing.
 * That velocity then becomes `a->speed`, which is what next tick's
 * rb_move_towards accelerates FROM, so the seam left the car running 4.6 m/s
 * slow to 4.2 m/s fast (worst beach_1/Johny, 9.30 against 5.12 -- +82%) for as
 * long as AI_ACCEL_LIMIT took to bleed it off, up to about 0.9 s, once per lap
 * per opponent. It also reached ai_actor_point_vel, so the contact solve saw a
 * car doing 17 m/s for a tick, and rb_wheel_spin_update and the engine voice
 * with it.
 *
 * FUN_00503880 does none of that, and the way it does not is exact:
 *
 *     if (FUN_00503440(...) != 0) {              // the path ran out
 *         piVar1[1] = t[n-1];                    // clamp the cursor's time
 *         FUN_00502ea0(car, slot, local_80);
 *         FUN_004fda10(car, phys+0x4390);        // ARM THE BLEND
 *         FUN_00502c70(car);
 *         *piVar1   = phys+0x43b8;               // cursor = the cycle start
 *         piVar1[1] = t[cycle_start];
 *         return;                                // <-- BEFORE both of these:
 *     }                                          //   FUN_005037f0, the finite
 *     ...                                        //   difference velocity, and
 *     FUN_00503190(...)                          //   the pose write
 *
 * so on the seam frame the body keeps the velocity it had and is not re-posed,
 * and the next frame's difference is taken wholly inside the new lap. And
 * `FUN_004fda10` is the other half: it copies `car+0xf8`, THE CAR'S LIVE WORLD
 * MATRIX, into `actor+0xcc` = `phys+0x445c` and sets `actor+0xac` =
 * `phys+0x443c` = 1.0f, which is the countdown FUN_00503190's blend runs on. So
 * the next second is spent dead-reckoning from where the car ACTUALLY WAS,
 * eased onto the new lap as the countdown expires. The seam is the only place a
 * retail race arms that blend at all: FUN_004fda10's three other call sites are
 * inside FUN_004fdb50, which needs `phys+0x4398`, and that flag is 0 for the
 * whole race (ai-opponents.md, "so which of the two runs when").
 *
 * THE PORT ALREADY HAS SOMEWHERE TO PUT IT. `off` is a displacement from the
 * recording that the pose is composed with and a spring bleeds out, which is
 * the same shape as the blend, so the seam residue goes there: the car's
 * composed pose comes out of the wrap UNCHANGED, and the existing relax carries
 * it onto the new lap. Nothing new is introduced and no constant moves.
 *
 * TWO PARTS OF THE SEAM ARE NOT EXPRESSIBLE IN `off` AND STILL STEP, both
 * measured rather than waved at:
 *
 *   - the VERTICAL. ai_bump_clamp bounds `off[1]` below at AI_BUMP_MAX_SINK,
 *     1 cm, because the ground is there and nothing here models it holding the
 *     car up. A seam whose new sample is higher than the old wants a negative
 *     offset of up to 7.6 cm and keeps 1 cm of it, so up to 6.6 cm of the
 *     vertical gap still arrives in one tick. Widening a safety bound to hide a
 *     7 cm step is the wrong trade and is deliberately not taken.
 *   - the ATTITUDE. `off_yaw` is a rotation about the world Y and the seam's
 *     orientation gap is mostly NOT yaw: of the 12.22 degrees at the worst seam
 *     at most 6.75 is yaw and the residual -- the roll and pitch the car's
 *     suspension was carrying -- reaches 10.40. The yaw is latched and bled out
 *     on the spring it already has; the rest steps. Expressing it needs a
 *     quaternion residue on `ai_car` and a second decay beside the one that is
 *     already there, which is a new mechanism and wants its own pass.
 *
 * What IS fixed is the whole of the velocity glitch and the whole of the
 * horizontal hop, which is where the 0.217 m and the +82% were.
 */

/* The yaw of a quaternion about the world Y, in radians -- the one component of
   an orientation `off_yaw` can hold. Same extraction ai_bump_apply's inverse
   would be: off_yaw pre-multiplies a Y rotation onto rec_q. */
static double ai_quat_yaw(const float q[4])
{
    return atan2(2.0 * ((double)q[0] * q[2] + (double)q[1] * q[3]),
                 1.0 - 2.0 * ((double)q[2] * q[2] + (double)q[3] * q[3]));
}

/* Hold the composed pose across the wrap: whatever the rewind moved the
 * RECORDING by, move the offset by the same amount the other way.
 *
 * `x0`/`q0` are the recorded pose from BEFORE the advance, which ai_step has
 * already captured for ai_diff_velocity -- so this needs nothing the tick did
 * not already have. Called AFTER ai_pose, so rec_x/rec_q are the new lap's.
 *
 * It may touch the offset and nothing else. The cursor, the lap, the distance
 * walked, both speeds and the rubber-band coefficient are the replay's and this
 * does not reach them, which is the same guarantee the bump and the steering
 * decision have.
 */
static void ai_seam_latch(ai_car *a, const float x0[3], const float q0[4])
{
    double dyaw;
    int k;

    for (k = 0; k < 3; k++)
        a->off[k] = (float)((double)a->off[k] + (double)x0[k] - a->rec_x[k]);

    dyaw = ai_quat_yaw(q0) - ai_quat_yaw(a->rec_q);
    /* The shortest way round, so a seam that happens to straddle +-pi does not
       latch a whole turn. */
    if (dyaw >  3.14159265358979) dyaw -= 2.0 * 3.14159265358979;
    if (dyaw < -3.14159265358979) dyaw += 2.0 * 3.14159265358979;
    a->off_yaw = (float)((double)a->off_yaw + dyaw);

    /* Through the SAME clamp every other path is funnelled through -- see
       ai_bump_clamp, which is where the vertical gives way -- and then recompose
       so the pose the rest of the tick sees is the one the car is standing in.
       No velocity is written: `offv` and `off_yawv` keep whatever the last tick
       left, exactly as the body's own velocity does. */
    ai_bump_clamp(a);
    ai_bump_apply(a);
}

/* DID THIS SHOVE KILL IT? The player's own two tests, on an opponent -- see
 * ai.h, "dying". -> nonzero if the car was put back on its line.
 *
 * Runs on the composed pose, so it sees where the car actually IS, and only on a
 * car a shove has actually moved: an opponent on its line is its recording, and
 * a recording is a lap that was really driven, so there is nowhere better to
 * send it and declaring it dead would fire every tick for the rest of the race.
 */
static void ai_pose(ai_car *a);        /* defined just below; a respawn poses */

static int ai_bump_death(ai_car *a, float dt)
{
    const rb_world *w = a->rb.world;
    const rb_body *b = &a->rb.body;
    float gap;
    int dead = 0;

    /* ONLY A SIMULATED CAR CAN GET ANYWHERE THAT NEEDS THIS. A car walking its
       recording is on a lap somebody really drove, so there is nowhere better to
       send it and the test would fire every tick on a recording that fords a
       stream. The gate used to be the bump offset; it is the mode now, because
       the offset is no longer where a shove lands. */
    if (!a->phys_mode) {
        a->buried_for = 0.0f;
        return 0;
    }

    /* FELL OUT OF THE WORLD, measured below its own recorded height -- the
       track's own reference, and the counterpart of the player's spawn height. */
    if (b->x[1] < a->rec_x[1] - AI_FELL_BELOW) {
        rlog("[rccars] ai %s: fell out of the world, back on its line\n",
             a->name);
        dead = 1;
    }
    /* DROWNED. The engine's own answer here is a `car_reset` script volume over
       the water (0x004f27a0), which the port has no data for; this is main.c's
       stand-in for it, with main.c's number. */
    if (!dead && w && w->water
        && w->water(w->ctx, 0, b->x, &gap) && gap < -AI_DROWN_DEPTH) {
        rlog("[rccars] ai %s: drowned (%d cm under), back on its line\n",
             a->name, (int)(-gap * 100.0f));
        dead = 1;
    }
    /* BURIED -- the ground is ABOVE the car. This is the engine's own
     * `gmIsPointInCDT(pos)`, the first arm of `carCheckAIResetInPhysMode`:
     * inside geometry, reset the car and put it back on its path. That function's
     * other two arms are the drowning above and a scripted `RESET_VOL`, which
     * the port has no data for.
     *
     * WHAT CHANGED IS WHO IT CAN HAPPEN TO, and that is the reported teleport.
     * It used to run on a KINEMATIC car placed by an offset and a terrain
     * follow, for which being inside a bank was a placement the model could not
     * undo -- so a graze against one respawned the car, 130 of 640 hard shoves,
     * reported as "they could teleport if player push them into obstacles". It
     * runs on a SIMULATED car now: one with contacts, grip and a controller
     * driving it, for which being inside geometry means what the engine means by
     * it. The car can drive out, and the settle below is what gives it the
     * chance to.
     *
     * The window is AI_PHYS_SETTLE_T rather than three time constants of the
     * return spring, because the spring is retired -- what the car does instead
     * is DRIVE out, and that constant is the engine's own "has this recovery
     * worked yet". */
    if (w && w->ground) {
        float gy, n[3];
        float half = 0.5f * RB_CARS[a->car].extent[1];
        if (w->ground(w->ctx, b->x[0], b->x[2], b->x[1] + AI_BUMP_CEIL, &gy, n)
            && gy > b->x[1] + half) {
            a->buried_for += dt;
            if (!dead && a->buried_for > AI_PHYS_SETTLE_T) {
                rlog("[rccars] ai %s: buried for %.1f s (%d cm of ground"
                     " overhead), back on its line\n", a->name,
                     (double)a->buried_for, (int)((gy - b->x[1]) * 100.0f));
                dead = 1;
            }
        } else {
            a->buried_for = 0.0f;
        }
    }
    if (!dead)
        return 0;

    /* BACK ON THE LINE, and that is the whole of it: the cursor, the lap, the
     * distance walked and the rubber-band coefficient are untouched, exactly as
     * respawn_checkpoint leaves the player's lap alone. What goes is the shove
     * and everything it was carrying -- including the steering decision, which
     * was taken about a piece of road the car is no longer beside. */
    a->off[0] = a->off[1] = a->off[2] = 0.0f;
    a->offv[0] = a->offv[1] = a->offv[2] = 0.0f;
    a->off_yaw = a->off_yawv = 0.0f;
    a->off_gnd = 0.0f;
    a->off_gnd_at[0] = a->off_gnd_at[1] = 1e30f;
    a->bump_wall = -1.0f;
    a->steer_want = a->steer_cmd = a->steer_hold = 0.0f;
    a->steer_side = 0;
    a->buried_for = 0.0f;
    a->respawns++;
    /* AND OUT OF PHYSICS MODE, with NO ease home: this is a genuine respawn --
       the car drowned or fell out of the world -- and the player's own
       respawn_checkpoint is an instant move for the same reason. Everything
       else that ends the mode goes through ai_phys_end and its blend. */
    a->phys_mode = 0;
    a->phys_t = 0.0f;
    a->ctrl_steer = 0.0f;
    a->blend_t = 0.0f;
    ai_pose(a);                        /* rebuild the pose from the recording */
    return 1;
}

/* Write the pose for the current (cursor, u), then put the bump back on top of
   it. Everything upstream keeps reading the recording out of rec_x / rec_q. */
static void ai_pose(ai_car *a)
{
    ai_pose_rec(a);
    memcpy(a->rec_x, a->rb.body.x, sizeof(a->rec_x));
    memcpy(a->rec_q, a->rb.body.q, sizeof(a->rec_q));
    /* THE LIFT BOUND FOLLOWS THE POSE. `bump_up' is one car height and a car's
       proxy height is not a constant -- it is thirteen spheres on a rig whose
       wheels move, and it is measured at whatever attitude the recording has
       the car in this tick. Derived once at load it was a centimetre or two out
       of step with the same measurement taken live, which is a bound that is
       occasionally the wrong side of the invariant it exists to keep. One
       gather per car per tick, on a rig the contact solve gathers anyway. */
    ai_bump_up_derive(a);
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
    /* AND UP IS BOUNDED TOO NOW, AT ONE CAR HEIGHT -- which the paragraph above
     * used to say it was not ("upward it is bounded only by the offset limit
     * below"). That was true and it was not enough: `bump_limit` is what the
     * car's GRIP could slide it, 1.4 m on the Overkill, and a lift exists only
     * to get one car up over another. The most that can ever honestly take is
     * the height of a car; past it the opponent is simply in the air.
     *
     * WHY IT IS HERE and not in ai_pair_resolve, where the lift is applied:
     * because the lift is not the only thing that raises off[1]. The positional
     * branch's give-back makes a PAIR zero-sum, and RAISING AI_MAX_FIELD TO THE
     * LAYOUT'S FIVE (ai.h) showed that a pair is the wrong unit -- with ten
     * pairs a tick instead of three a car is the upper one in four of them at
     * once, and what it cannot take positionally it takes through offv[1] out
     * of the impulse half instead. Capping the positional share alone made the
     * survey WORSE (0.744 m to 1.195 m), because the pair then stays inside
     * itself for longer. This is the one place every path -- push, impulse and
     * relax -- is funnelled through, which is what the function's own header
     * comment says it is for.
     *
     * The outward velocity goes with it, for ai_bump_clamp's own reason: an
     * offset velocity climbing against a position that cannot move is a car
     * that reports it is getting out of the way while standing still. */
    if (a->bump_up > 0.0f && a->off[1] > a->bump_up) {
        a->off[1] = a->bump_up;
        if (a->offv[1] > 0.0f) a->offv[1] = 0.0f;
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

/* ------------------------------------------------------- the steering decision
 *
 * See ai.h, "the steering decision", for what this is and what it deliberately
 * is not. Everything here writes `steer_want`, `steer_cmd` and their hysteresis
 * and NOTHING ELSE -- the replay, the cursor, the lap and the rubber band are
 * out of reach from this file section by construction.
 */

/* FUN_00410150: the signed angle between two vectors, in DEGREES, both flattened
 * into XZ. acos of the normalised dot, negated when cross(a, b).y < 0.
 *
 * With the port's own convention (+X is LEFT) a POSITIVE angle means `b` is to
 * the left of `a`, and a positive steer angle points the wheels left, so the
 * sign crosses into the controller unchanged. */
static float ai_signed_angle(const float a[3], const float b[3])
{
    double ax = a[0], az = a[2], bx = b[0], bz = b[2];
    double la = sqrt(ax * ax + az * az), lb = sqrt(bx * bx + bz * bz);
    double d, ang;

    if (la < 1e-6 || lb < 1e-6)
        return 0.0f;
    d = (ax * bx + az * bz) / (la * lb);
    if (d > 1.0) d = 1.0;
    if (d < -1.0) d = -1.0;
    ang = acos(d) * (180.0 / 3.14159265358979);
    if (az * bx - ax * bz < 0.0)          /* cross(a, b).y */
        ang = -ang;
    return (float)ang;
}

/* The point `m` metres further along the recorded polyline than the cursor is --
 * FUN_004fda90's lookahead, walked on the same metric ai_advance walks and
 * wrapping to the same cursor ai_step's wrap wraps to -- ai_cycle_cursor, which
 * is the one place that number lives -- so the point ahead of a car about to
 * close its lap is on the lap and not off the end of the array.
 *
 * READ ONLY: it takes a copy of (cursor, u) and never writes them back. -> 0 on
 * a profile too short to have a segment. */
static int ai_path_ahead(const ai_car *a, float m, float out[3])
{
    int c = a->cursor, guard = 0;
    float u = a->u;

    if (!a->s || a->n < 2)
        return 0;
    if (c < 1)
        c = 1;
    for (;;) {
        float seg = seg_len(a, c);
        float rem = (1.0f - u) * seg;

        if (m <= rem || guard > a->n) {
            float t = (seg > AI_EPS) ? u + m / seg : 1.0f;
            const float *p0 = a->s[c - 1].p, *p1 = a->s[c].p;
            int k;
            if (t > 1.0f) t = 1.0f;
            for (k = 0; k < 3; k++)
                out[k] = p0[k] + (p1[k] - p0[k]) * t;
            return 1;
        }
        m -= rem;
        u = 0.0f;
        c++;
        if (c >= a->n)
            c = ai_cycle_cursor(a);
        guard++;
    }
}

/* One car's proxy reach, measured at load by ai_bump_derive. It used to be
 * recovered by dividing bump_limit back down, which was right only while the
 * limit was two reaches; the limit is a slide distance now, so the reach is
 * stored instead of inferred. */
static float ai_reach(const ai_car *a)
{
    return a->bump_reach;
}

/* How much room this opponent wants between itself and `other`, centre to
 * centre: both proxies, plus one of its own WHEELS of daylight.
 *
 * The margin is THE PORT'S and the wheel radius is the car's own recovered
 * number rather than a typed-in gap. Proxies exactly touching is not a pass, it
 * is a scrape -- the contact solve would fire on it every tick of the overtake
 * -- and a wheel's width is the smallest real dimension the car carries. */
static float ai_steer_clearance(const ai_car *a, float other_reach)
{
    return ai_reach(a) + other_reach + RB_CARS[a->car].radius;
}

/* Nothing decided, and exactly nothing -- an opponent with an empty corridor has
   to be bit-identical to one from before this existed. */
static void ai_steer_clear(ai_car *a)
{
    a->steer_want = a->steer_cmd = a->steer_hold = 0.0f;
    a->steer_side = 0;
    a->steer_left[0] = a->steer_left[1] = a->steer_left[2] = 0.0f;
}

/* THE DECISION. Once per tick per car, before the spring, after the pose.
 *
 * `px/py/pz` is the player, which ai_step is handed as a position; the other
 * opponents are read off `ai` directly. */
static void ai_steer_decide(ai_t *ai, int idx, float px, float py, float pz,
                            float dt)
{
    ai_car *a = &ai->car[idx];
    (void)py;

    if (ai->steer_off) {
        ai_steer_clear(a);
        return;
    }
    float look[3], fwd[3], left[3], fl;
    float best_lat = 0.0f, best_need = 0.0f, clear = 0.0f;
    float want, tgt[3], dir[3], ang, step, horizon;
    int blocked = 0, side, j, k;

    if (!ai_path_ahead(a, AI_STEER_LOOKAHEAD, look)) {
        ai_steer_clear(a);
        return;
    }

    /* The recorded frame: forward is the chord to the lookahead point, left is
       its XZ perpendicular. +X is LEFT, so for fwd = (0,0,1) this is (1,0,0). */
    fwd[0] = look[0] - a->rec_x[0];
    fwd[1] = 0.0f;
    fwd[2] = look[2] - a->rec_x[2];
    fl = (float)sqrt((double)fwd[0] * fwd[0] + (double)fwd[2] * fwd[2]);
    if (fl < 1e-4f) {
        ai_steer_clear(a);
        return;
    }
    fwd[0] /= fl;
    fwd[2] /= fl;
    left[0] =  fwd[2];
    left[1] =  0.0f;
    left[2] = -fwd[0];
    memcpy(a->steer_left, left, sizeof(left));

    /* WHAT IS IN THE NEXT 2.7 METRES OF ROAD. Measured from the car's own
     * RECORDED position, so the corridor is the line it is going to drive and
     * not the one a shove has put it on -- a car knocked wide must still avoid
     * what is on its line, and must not invent an obstacle out of the shove.
     *
     * ENGAGING AND RELEASING ARE DIFFERENT TESTS, and making them the same one
     * is the whole of a weave. A car engages when something is within `c` of its
     * line; if it then released on the same test it would release the instant it
     * had moved far enough -- which is to say the instant the avoidance WORKED --
     * swing back onto the line, find the obstacle there again, and re-engage,
     * possibly on the other side. Measured before this: five changes of side and
     * ELEVEN contact ticks passing a parked player, against one for a car that
     * did not steer at all. So a car that has committed keeps its hazard until
     * the thing is BEHIND it, whatever its lateral distance has become. */
    /* THE HORIZON: 2.7 m of road at the speed this car is being played back at.
     *
     * NO SPEED FLOOR, and that is not a division guard, it is the rule. A car
     * that is not moving has no line to change and nothing it is about to reach;
     * with a floor of 1 m/s the horizon at a standing start comes out at 2.7
     * SECONDS, which is long enough for the grid -- three cars 0.74 m apart,
     * closing at 6 cm/s because their three recordings drift together -- to
     * predict a collision and swerve apart before the lights go out. At racing
     * speed it is 0.45 s, which is what "about to hit it" should mean. */
    if (a->speed < AI_STEER_MIN_SPEED) {
        ai_steer_clear(a);
        return;
    }
    horizon = AI_STEER_LOOKAHEAD / a->speed;
    /* AND NEVER FURTHER AHEAD THAN THE CAR COULD ACT. 2.7 m of road is 0.45 s at
     * racing speed and the whole of a lap at walking pace, and a horizon that
     * long turns the 6 cm/s with which two nose-to-tail recordings drift
     * together into a predicted collision. AI_STEER_SETTLE is the offset
     * spring's own settling time -- three time constants of `bump_w`, 0.92 s on
     * an Overkill -- which is how long a lane change actually takes: looking
     * further ahead than you could finish acting on is not foresight. */
    {
        float settle = AI_STEER_SETTLE / a->bump_w;
        if (horizon > settle)
            horizon = settle;
    }
    for (j = -1; j < ai->n; j++) {
        const float *ov;
        float rel[3], vel[3], ahead, lat, need, c;
        double rr, rl, vr, vl, vv, tca, sa, sl;
        float other_reach;

        if (j == idx)
            continue;
        if (j < 0) {
            other_reach = ai->player_reach > 1e-3f ? ai->player_reach
                                                   : ai_reach(a);
            c = ai_steer_clearance(a, other_reach);
            rel[0] = px - a->rec_x[0];
            rel[2] = pz - a->rec_x[2];
            ov = ai->player_v;
        } else {
            /* THE OTHER OPPONENT'S ACTUAL POSE, offset and all: where it IS is
               what has to be driven round, and it is the one place in this
               decision that reads anything but a recording. */
            const float *op = ai->car[j].rb.body.x;
            other_reach = ai_reach(&ai->car[j]);
            c = ai_steer_clearance(a, other_reach);
            rel[0] = op[0] - a->rec_x[0];
            rel[2] = op[2] - a->rec_x[2];
            ov = ai->car[j].rb.body.v;
        }
        rel[1] = 0.0f;
        /* Relative velocity in the recorded frame. This car's own is its
           commanded speed straight down the path -- which is what the recording
           is about to do, and the only prediction of it there is. */
        vel[0] = ov[0] - fwd[0] * a->speed;
        vel[2] = ov[2] - fwd[2] * a->speed;

        ahead = rel[0] * fwd[0]  + rel[2] * fwd[2];
        lat   = rel[0] * left[0] + rel[2] * left[2];
        if (ahead > AI_STEER_LOOKAHEAD + c)
            continue;                        /* further up the road than this */

        if (a->steer_side != 0) {
            /* COMMITTED: it stays the hazard until it is properly BEHIND, one
               clearance back -- when the centres are level the two cars are
               still alongside, and one that comes back onto the line there
               sideswipes what it has just passed. Lateral distance is not
               consulted at all: it is small BECAUSE the avoidance is working. */
            if (ahead < -c)
                continue;
            need = c - (lat < 0.0f ? -lat : lat);
            if (need <= 0.0f)
                need = 1e-4f;
            if (need > best_need) {
                best_need = need;
                best_lat = lat;
                clear = c;
                blocked = 1;
            }
            continue;
        }

        /* ENGAGING: not "is it near my line" but "AM I GOING TO HIT IT" -- the
         * closest approach of the two, over the lookahead's own span of time.
         *
         * The difference is the whole start of a race. The field lines up on a
         * grid 0.74 m apart and the three recordings then run nose to tail for a
         * whole lap, because three humans drove them on three different
         * afternoons and never met. Anything that engages on proximity has every
         * follower swerving to its full 0.66 m budget for the entire race, and
         * swerving at a car it was never going to reach. Two cars holding
         * station never close, so their closest approach is where they already
         * are, and nothing fires.
         *
         * AI_STEER_LOOKAHEAD is the horizon, in the units it was recovered in:
         * 2.7 m of road at the speed this car is being played back at. */
        rr = ahead;   rl = lat;
        vr = vel[0] * fwd[0]  + vel[2] * fwd[2];
        vl = vel[0] * left[0] + vel[2] * left[2];
        vv = vr * vr + vl * vl;
        tca = vv > 1e-9 ? -(rr * vr + rl * vl) / vv : 0.0;
        if (tca < 0.0) tca = 0.0;
        if (tca > horizon) tca = horizon;
        sa = rr + vr * tca;
        sl = rl + vl * tca;
        if (tca <= 1e-6) {
            /* NOT CLOSING -- the closest they get is where they already are. Two
             * cars HOLDING STATION are not a hazard however close the station
             * is, and that case is the whole race: three recordings driven on
             * three different afternoons run nose to tail 0.74 m apart for a
             * lap, which is 0.13 m of daylight between two proxies and well
             * inside `c`. Engaging on it puts every follower at its full 0.66 m
             * of offset from the lights to the flag. So the only thing that
             * counts here is proxies ACTUALLY touching, with no margin. */
            double touch = ai_reach(a) + other_reach;
            if (rr * rr + rl * rl >= touch * touch)
                continue;
        } else if (sa * sa + sl * sl >= (double)c * c) {
            continue;                    /* they never get inside each other */
        }
        /* And it has to be IN FRONT when they meet: a car closing from behind is
           overtaking THIS one, and getting out of its way is its own business. */
        if (sa < 0.0)
            continue;
        need = c - (float)sqrt(rl * rl);
        if (need <= 0.0f)
            need = (float)(c - sqrt(sl * sl));
        if (need <= 0.0f)
            need = 1e-4f;
        if (need > best_need) {
            best_need = need;
            best_lat = (float)(sl != 0.0 ? sl : rl);
            clear = c;
            blocked = 1;
        }
    }

    /* THE COMMITMENT. AI_STEER_HOLD is how long the last decision outlives the
       last tick that still saw the obstacle at all. */
    if (blocked) {
        side = a->steer_side;
        if (side == 0)
            side = best_lat >= 0.0f ? -1 : 1;   /* go where it is not */
        want = best_lat + (float)side * clear;
        /* If that side cannot be reached inside the STEERING budget, try the
         * other one before settling for a pass that does not clear.
         *
         * `bump_ref`, NOT `bump_limit`. This is how far the car will steer
         * across its own line to get round something, and that is a car-sized
         * quantity -- two proxy reaches, 0.63 m, which is what it always was.
         * bump_limit is now how far a SHOVE can carry it, 6.75 m of grip-limited
         * slide (ai.h), and reading it here let a car commit to a six-metre
         * swerve to pass another: measured over a minute on each of the ten
         * tracks with no player, that turned 54 contact car-ticks between
         * opponents into 111, i.e. deciding made the field touch MORE. */
        if ((want < 0.0f ? -want : want) > a->bump_ref) {
            float alt = best_lat - (float)side * clear;
            if ((alt < 0.0f ? -alt : alt) <= a->bump_ref) {
                side = -side;
                want = alt;
            } else {
                want = want < 0.0f ? -a->bump_ref : a->bump_ref;
            }
        }
        a->steer_side = side;
        a->steer_hold = AI_STEER_HOLD;
        a->steer_want = want;
    } else if (a->steer_hold > 0.0f) {
        a->steer_hold -= dt;
        if (a->steer_hold <= 0.0f) {
            a->steer_hold = 0.0f;
            a->steer_side = 0;
            a->steer_want = 0.0f;
        }
        /* else: hold `steer_want`, which is a LATERAL SCALAR and so stays
           correct as the frame turns through a corner. Storing the world vector
           instead would hold a line that is right only where it was decided. */
    } else {
        a->steer_side = 0;
        a->steer_want = 0.0f;
    }

    /* IS THE MOVE WORTH MAKING? The recovered controller's +-0.5 degree deadband,
     * asked of the same quantity it is a deadband on: the signed angle from the
     * recorded heading to the target point, FUN_00410150 over FUN_004fda90's own
     * 2.7 m lookahead. Inside it the target is straight ahead and there is
     * nothing to steer for. */
    for (k = 0; k < 3; k++)
        tgt[k] = look[k] + left[k] * a->steer_want;
    dir[0] = tgt[0] - a->rec_x[0];
    dir[1] = 0.0f;
    dir[2] = tgt[2] - a->rec_x[2];
    ang = ai_signed_angle(fwd, dir);
    if ((ang < 0.0f ? -ang : ang) < AI_STEER_DEADBAND) {
        a->steer_want = 0.0f;
        a->steer_side = 0;
    }

    /* WHICH WAY THE CAR POINTS, and it is a MEASUREMENT rather than a servo.
     *
     * FUN_004fddd0's steer is the output of a loop closed on a body it
     * integrates; there is no body to close a loop on here, and the first
     * attempt -- hold the heading offset AT the angle to the target, measured
     * from the recorded forward -- was wrong in both the ways that shape can be.
     * It never zeroed, because a car that has finished crossing and is running
     * parallel on the new line still reads a constant angle to a target beside
     * its recorded position; and it wound up, because the yaw spring's
     * acceleration cap clips the damping term along with the restoring one, so a
     * large standing error drives it bang-bang and it overshot to the full
     * 30-degree stop while the command said 13.8 the other way.
     *
     * What a car's heading actually is, with no slip, is the direction it is
     * travelling: the lateral rate the spring is producing over the forward
     * speed the recording is being played at. It is zero when the car is on a
     * line and zero again when it has reached a new one, it cannot wind up
     * because nothing integrates it, and it costs an atan2.
     *
     * The recovered lock and rate limit still do their own work on top: 35
     * degrees is as far over as the controller may ask, and 90 deg/s is how fast
     * the nose may swing -- which is what stops a car snapping straight when the
     * spring's velocity changes sign. */
    {
        float vlat = a->offv[0] * left[0] + a->offv[2] * left[2];
        float v = a->speed > 1.0f ? a->speed : 1.0f;
        ang = (float)(atan2((double)vlat, (double)v)
                      * (180.0 / 3.14159265358979));
    }
    if ((ang < 0.0f ? -ang : ang) < AI_STEER_DEADBAND)
        ang = 0.0f;
    if (ang >  AI_STEER_LOCK) ang =  AI_STEER_LOCK;
    if (ang < -AI_STEER_LOCK) ang = -AI_STEER_LOCK;
    step = AI_STEER_RATE * dt;
    if (ang > a->steer_cmd + step)      a->steer_cmd += step;
    else if (ang < a->steer_cmd - step) a->steer_cmd -= step;
    else                                a->steer_cmd = ang;
}

/* The return to the line: a critically damped spring on the horizontal offset
 * and on the yaw, capped at what the car's tyres could actually pull with.
 *
 * The cap is not decoration. The spring's own peak is at the grip limit only
 * when the car is at full displacement and stationary relative to its line; a
 * bump that arrives while it is already moving back can ask for several times
 * that, and a car that recovers harder than it could corner is the thing that
 * would read as a rubber band rather than as driving. */
static void ai_bump_relax(ai_car *a, float dt, const float want[3],
                          float yaw_want)
{
    double k, c, ax, ay, az, mag, aw, cap_w;
    double ex, ey, ez, eyaw;
    float h0;

    /* THE SPRING'S TARGET IS THE DECISION, and zero when there is none.
     *
     * `want` is what ai_steer_decide chose -- a lateral offset that clears
     * whatever is in the next 2.7 m of road -- and `yaw_want` the heading that
     * goes with it. NOT A SECOND MECHANISM: the frequency, the damping, the grip
     * cap and the limits below are the bump's own, every constant unchanged, and
     * all that moves is where the spring is pulling to. An opponent moves over
     * no harder than `coeff_rear_tires * RB_GRAVITY` lets it, which is the whole
     * of why a decision it makes is one it could actually execute.
     *
     * With nothing in the way `want` and `yaw_want` are exactly zero and every
     * line below is the arithmetic it was before this existed -- including the
     * early return, which is what keeps an unobstructed opponent bit-identical.
     */
    if (a->off[0] == 0.0f && a->off[1] == 0.0f && a->off[2] == 0.0f
        && a->offv[0] == 0.0f && a->offv[1] == 0.0f && a->offv[2] == 0.0f
        && a->off_yaw == 0.0f && a->off_yawv == 0.0f
        && want[0] == 0.0f && want[1] == 0.0f && want[2] == 0.0f
        && yaw_want == 0.0f)
        return;                        /* on its line: exactly the recording */

    /* what the horizontal offset was before this tick's spring ran -- the wall
       bound below refuses GROWTH against it and never takes anything away */
    h0 = (float)sqrt((double)a->off[0] * a->off[0]
                     + (double)a->off[2] * a->off[2]);

    k = (double)a->bump_w * a->bump_w;
    c = 2.0 * a->bump_w;

    ex = (double)a->off[0] - want[0];
    ey = (double)a->off[1] - want[1];
    ez = (double)a->off[2] - want[2];
    eyaw = (double)a->off_yaw - yaw_want;

    ax = -(k * ex + c * a->offv[0]);
    ay = -(k * ey + c * a->offv[1]);
    az = -(k * ez + c * a->offv[2]);
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
    /* and then the level, which is the one bound that has to know what the
       offset looked like before the tick */
    ai_bump_wall_relax(a, h0);
    mag = sqrt((double)a->off[0] * a->off[0] + (double)a->off[1] * a->off[1]
               + (double)a->off[2] * a->off[2]);

    /* The yaw, on the same spring. Its acceleration budget is the linear one
       over the proxy's REACH -- the same tyre force, applied at the end of the
       same lever, and the lever is the car's own size.
       This used to read bump_limit * 0.5, which was the reach only because the
       limit was two of them; it is now a slide distance in metres and the two
       have nothing to do with each other. */
    cap_w = (double)a->bump_accel / (double)a->bump_reach;
    aw = -(k * eyaw + c * a->off_yawv);
    if (aw >  cap_w) aw =  cap_w;
    if (aw < -cap_w) aw = -cap_w;
    a->off_yawv = (float)((double)a->off_yawv + aw * dt);
    a->off_yaw  = (float)((double)a->off_yaw + (double)a->off_yawv * dt);
    ai_bump_clamp(a);

    /* SNAP TO EXACTLY ZERO. See AI_BUMP_SNAP: a recovered opponent has to become
       the same car it was before it was touched, or the pose carries a
       micrometre of displacement for the rest of the race and ai_bump_apply
       keeps probing the ground for it.

       NOT WHILE A DECISION IS STANDING. Snapping to zero against a non-zero
       target is the spring being told to go somewhere and then teleported home,
       once a tick, for as long as the car is holding a line round something. */
    if (want[0] == 0.0f && want[1] == 0.0f && want[2] == 0.0f
        && mag < AI_BUMP_SNAP
        && fabs((double)a->offv[0]) < AI_BUMP_SNAP_V
        && fabs((double)a->offv[1]) < AI_BUMP_SNAP_V
        && fabs((double)a->offv[2]) < AI_BUMP_SNAP_V) {
        a->off[0] = a->off[1] = a->off[2] = 0.0f;
        a->offv[0] = a->offv[1] = a->offv[2] = 0.0f;
    }
    if (yaw_want == 0.0f
        && fabs((double)a->off_yaw) < AI_BUMP_SNAP_YAW
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
/* ==================================================== the engine's own dispatch
 *
 * TRANSCRIBED. ai.h has the model and every constant's address; the short form
 * is that a retail opponent stops being a replay the moment anything touches it
 * and becomes a real car that FUN_004fddd0 steers back to its recorded path.
 * This file believed for a long time that no such thing existed.
 */

static int ai_advance(ai_car *a, float target, float dt);  /* below */

/* HOW FAR THE CAR IS FROM ITS OWN RECORDED PATH, read-only.
 *
 * THE CURSOR IS NOT MOVED. Progress and position are two different questions
 * and this file answered them with one number for a while, which broke both:
 * moving the cursor to where the car IS made an opponent's race progress
 * depend on where a shove had put it (`wideline` 5.4% -> 20.6% of frames with
 * the wrong place), and leaving the cursor on the RECORDING made the exit test
 * measure how far the schedule had run away rather than how far the car was off
 * its line, so a shoved car never came home at all (`progchk` -257 m/lap).
 *
 * So the cursor stays on the schedule -- `ai_phys_step` advances it exactly as
 * the replay does -- and this answers the other question by looking, without
 * writing anything down. A short window either side of the cursor, because the
 * car cannot be far from it in the moment a shove lasts. */
static double ai_path_gap(const ai_car *a)
{
    double best = -1.0;
    int c, k;

    if (!a->s || a->n < 2)
        return 0.0;
    c = a->cursor < 1 ? 1 : a->cursor;
    /* start a little behind, then sweep forward over AI_CTRL_BACK_M + FWD_M */
    {
        double back = 0.0;
        int in_loop = a->cursor > a->cycle_start;
        for (k = 0; k < AI_CTRL_SEARCH && back < AI_CTRL_BACK_M; k++) {
            if (c <= 1) {
                if (!in_loop) break;
                c = a->n - 1;
            } else {
                c--;
            }
            back += seg_len(a, c);
        }
        if (c < 1) c = 1;
    }
    {
        double span = 0.0;
        for (k = 0; k < AI_CTRL_SEARCH
                    && span < AI_CTRL_BACK_M + AI_CTRL_FWD_M; k++) {
            const float *p0 = a->s[c - 1].p, *p1 = a->s[c].p;
            double ex = (double)p1[0] - p0[0], ez = (double)p1[2] - p0[2];
            double ll = ex * ex + ez * ez, t = 0.0, dx, dz, d2;
            span += seg_len(a, c);
            if (ll > 1e-12) {
                t = ((double)(a->rb.body.x[0] - p0[0]) * ex
                   + (double)(a->rb.body.x[2] - p0[2]) * ez) / ll;
                if (t < 0.0) t = 0.0;
                if (t > 1.0) t = 1.0;
            }
            dx = (double)a->rb.body.x[0] - (p0[0] + ex * t);
            dz = (double)a->rb.body.x[2] - (p0[2] + ez * t);
            d2 = dx * dx + dz * dz;
            if (best < 0.0 || d2 < best)
                best = d2;
            c++;
            if (c >= a->n)
                c = ai_cycle_cursor(a);
        }
    }
    return best < 0.0 ? 0.0 : sqrt(best);
}

/* FUN_004fd9d0 / carAiStartPhysicsMode -- arm it.
 *
 * IDEMPOTENT, and that is the engine's own `if (actor+0x08 == 0)` guard: a car
 * already simulated does not restart its clock because a second sphere pair
 * touched it in the same tick. Both cars of a pair are armed, which is what
 * FUN_00533990 does at 0x533a82 and 0x533a9b.
 */
static void ai_phys_start(ai_car *a)
{
    if (!a || a->remote)
        return;
    /* AND ONLY WHERE THERE IS A WORLD TO DRIVE IN. Simulating a car needs
       ground, contacts and a collision grid; REPLAYING one needs none of them,
       which is why several fixtures here bind no world at all and why a car
       handed a NULL one has to stay on its recording. Without this guard a
       graze in such a fixture handed the car to rb_car_tick with nothing under
       it and it free-fell -- measured as part 2's lap covering 623 m through
       space against the recording's own 460. The engine has no such case: a
       race always has a level. */
    if (!a->rb.world)
        return;
    a->phys_hold = AI_PHYS_HOME_MIN;
    if (a->phys_mode)
        return;
    a->phys_mode = 1;
    a->phys_t = 0.0f;
    a->ctrl_steer = 0.0f;
    a->blend_t = 0.0f;

    /* NOTHING IS SEEDED INTO THE BODY, because the body is already right:
       ai_pose wrote this tick's pose and ai_diff_velocity wrote the replay's own
       velocity and momentum into it (FUN_00503880 writes both for exactly this
       reason), so the car enters the simulation moving as it was seen to move.
       What goes is the OFFSET -- a displacement composed onto a recording is
       meaningless for a car that has stopped following one, and leaving it set
       would have ai_bump_apply drag the body back the moment the mode ends.
       Zeroing it moves nothing: the body is at `rec + off` already and only
       ai_bump_apply ever re-derives that, which the simulated path never calls. */

    /* BUT THE HIT THAT ARMED THE MODE IS NOT THROWN AWAY, and this is what
     * "when i hit ai car, it just stops and not bumps" was.
     *
     * The contact that arms the mode is resolved BEFORE the flag is set, so its
     * impulse goes where an un-armed car's goes -- into `offv`, the offset
     * velocity. Zeroing that here discarded the whole of the first and hardest
     * hit, and the first hit is the one the player sees; everything after it
     * lands on a car that is already moving away. So the car took a blow that
     * moved the PLAYER (measured: a parked Hummer knocked from rest to 5.4 m/s)
     * and showed nothing at all on its own side -- `bump` 0.000 for the whole
     * encounter.
     *
     * `offv` is a genuine velocity delta (`ai_take_impulse` divides the impulse
     * by the mass to build it), so handing it to the body is exact. The YAW rate
     * is dropped rather than converted: `off_yawv` would need the forward
     * inertia tensor to become angular momentum, and the engine's own resolver
     * discards an opponent's roll and pitch for the same kind of reason. One
     * tick of yaw is small against the linear kick. */
    {
        int k;
        for (k = 0; k < 3; k++) {
            a->rb.body.v[k] += a->offv[k];
            a->rb.body.P[k] = a->rb.body.v[k] * a->rb.body.mass;
        }
    }

    a->off[0] = a->off[1] = a->off[2] = 0.0f;
    a->offv[0] = a->offv[1] = a->offv[2] = 0.0f;
    a->off_yaw = a->off_yawv = 0.0f;
    a->off_gnd = 0.0f;
    a->bump = 0.0f;
    a->buried_for = 0.0f;
    ai_steer_clear(a);
}

/* FUN_004fda10 / carAiEndPhysicsMode -- leave it, snapshot where the car
 * actually ended up, and arm the one-second ease back onto the line.
 *
 * The snapshot is the function's own: it copies `phys+0x5884` (the body
 * position) and `phys+0x5890` (its orientation) into the actor and sets the
 * `+0x443c` countdown to 1.0. */
static void ai_phys_end(ai_car *a)
{
    a->phys_mode = 0;
    a->phys_t = 0.0f;
    a->phys_hold = 0.0f;
    a->ctrl_steer = 0.0f;
    memcpy(a->blend_x, a->rb.body.x, sizeof(a->blend_x));
    memcpy(a->blend_q, a->rb.body.q, sizeof(a->blend_q));
    a->blend_t = AI_BLEND_T;
}

/* STILL TOUCHING -- hold the mode open without arming it. See AI_PHYS_MIN_HIT:
   arming asks "was this a hit", this asks "are we still against each other". */
static void ai_phys_touch(ai_car *a)
{
    if (a && !a->remote && a->phys_mode)
        a->phys_hold = AI_PHYS_HOME_MIN;
}

void ai_phys_bump(ai_t *ai, int i)
{
    if (!ai || i < 0 || i >= ai->n)
        return;
    ai_phys_start(&ai->car[i]);
}

int ai_phys_active(const ai_t *ai, int i)
{
    if (!ai || i < 0 || i >= ai->n)
        return 0;
    return ai->car[i].phys_mode;
}

/* RETIRED: `ai_ctrl_resync`, the nearest-point cursor search. It existed to put
   the cursor where the CAR is; the cursor now stays where the RECORDING is and
   only the body is simulated (see ai_phys_step). That removes the whole class
   of bug it kept producing -- a cursor that could run backwards and turn the car
   round, a window sized in samples that aliased where a track passes near
   itself, and a wrap out of the lead-in -- and it restores the premise the
   placing layer is built on. */

/* FUN_004fddd0 -- the recovered controller, and the only new arithmetic here.
 * Every constant is in ai.h with the address it came from.
 *
 * WHAT IT IS NOT: `ai_steer_decide` uses the same lookahead and the same signed
 * angle to move a bump OFFSET, because it has no body to drive. This has one.
 */
static void ai_ctrl_command(ai_car *a, const float look[3],
                            float *throttle, float *brake, float *steer,
                            float dt)
{
    const float *m = rbcar_matrix(&a->rb);
    float fwd[3], dir[3];
    double d, want, ang, cmd, lim;

    fwd[0] = m[8]; fwd[1] = m[9]; fwd[2] = m[10];   /* body +Z, row 2 */
    dir[0] = look[0] - a->rb.body.x[0];
    dir[1] = 0.0f;
    dir[2] = look[2] - a->rb.body.x[2];
    d = sqrt((double)dir[0] * dir[0] + (double)dir[2] * dir[2]);

    /* THE STEER: the signed angle to the point, deadbanded, clamped to the
       controller's own lock and RATE LIMITED -- which is what stops a car that
       has just been spun round sawing at the wheel. */
    ang = ai_signed_angle(fwd, dir);
    /* AND THE COMMAND IS THE NEGATED ANGLE, WHICH IS MEASURED AND NOT ASSUMED.
     *
     * `docs/ai-opponents.md` records that `FUN_00410150`'s sign "crosses
     * unchanged" into the steer, and a rig test seemed to agree -- drive a car
     * with steer +1 for two seconds and `ai_signed_angle(start_fwd, end_fwd)`
     * comes out POSITIVE. That test is worthless: over two seconds the car turns
     * most of a circle, and the sign of a large rotation says nothing about the
     * local response.
     *
     * The transfer function does. Logging (steer applied, angle next tick) over
     * 14,437 ticks of real recovery on all ten tracks: a POSITIVE steer made the
     * angle GROW 2283 times against 1788, and a negative steer shrank it 2055
     * against 1444 -- both halves agreeing, which is the check that it is a
     * signal and not noise. So `steer = +ang` is POSITIVE feedback, and every
     * symptom follows from it: a car that left its line drove further from it,
     * never satisfied the exit, and stayed simulated until the five-second
     * timeout. What it was worth, over ten tracks of 120 s with five opponents
     * and no player:
     *
     *     worst distance off its own line   38.420 m  ->  1.431 m
     *     respawns (drowned/fell/buried)           7  ->  0
     *     share of ticks spent simulated       4.04%  ->  1.26%
     *
     * This is the fourth convention in this port to be settled by constructing
     * the measurement rather than reading the note (`traps.md`), and the note it
     * overturns is one this project wrote itself. */
    ang = -ang;
    cmd = (fabs(ang) < AI_CTRL_DEADBAND) ? 0.0 : ang;
    if (cmd >  AI_CTRL_LOCK) cmd =  AI_CTRL_LOCK;
    if (cmd < -AI_CTRL_LOCK) cmd = -AI_CTRL_LOCK;
    /* "FULL LOCK FOR THE FIRST SECOND" (`phys+0x439c < 1.0`) IS THE RATE LIMIT
     * BEING LIFTED, NOT THE COMMAND BEING SLAMMED TO THE STOP -- i.e. the whole
     * of the lock is AVAILABLE immediately, rather than ramped at AI_CTRL_RATE.
     *
     * Read the other way first, as `steer = ±AI_CTRL_LOCK whenever the command
     * is nonzero`, and it wrecks the thing it is for: a car grazed while sitting
     * ON its own line has a command of a fraction of a degree, and got full
     * opposite lock for a second. Measured -- cars entered the mode at
     * `dn = 0.000` and were driven to a median 2.03 m off their line, and the
     * mode then could not exit because the car was never back on it. A car that
     * has just been hit needs its steering to RESPOND at once; it does not need
     * to be told to turn as hard as it can. */
    lim = (a->phys_t < AI_CTRL_FULL_T) ? (double)AI_CTRL_LOCK * 2.0
                                       : (double)AI_CTRL_RATE * dt;
    if (cmd > a->ctrl_steer + lim) cmd = a->ctrl_steer + lim;
    if (cmd < a->ctrl_steer - lim) cmd = a->ctrl_steer - lim;
    a->ctrl_steer = (float)cmd;

    /* THE TARGET SPEED: 4 m/s under a metre, ramping to 10 at ten, scaled down
       by how far off the heading is and floored at half past 20 degrees. */
    if (d <= AI_CTRL_D_NEAR)
        want = AI_CTRL_V_NEAR;
    else if (d >= AI_CTRL_D_FAR)
        want = AI_CTRL_V_FAR;
    else
        want = AI_CTRL_V_NEAR + (AI_CTRL_V_FAR - AI_CTRL_V_NEAR)
                              * (d - AI_CTRL_D_NEAR)
                              / (AI_CTRL_D_FAR - AI_CTRL_D_NEAR);
    {
        double k = 1.0 - AI_CTRL_ANG_K * fabs(ang);
        if (k < AI_CTRL_ANG_FLOOR) k = AI_CTRL_ANG_FLOOR;
        want *= k;
    }
    /* AND NEVER SLOWER THAN THE RACE PACE. THE PORT'S, and the second half of
     * "it just stops".
     *
     * FUN_004fddd0's speed law tops out at AI_CTRL_V_FAR and is scaled down by
     * the heading error, which for a car sitting ON its line with the target
     * 2.7 m ahead asks for about 5 m/s. The recordings run at 6 to 7. So every
     * tick a car spent simulated it was being told to slow down, and a graze
     * that kept re-arming the mode bled it off a metre per second at a time --
     * measured 6.35 -> 4.57 m/s over three seconds with the car never once
     * leaving its own line.
     *
     * The engine's law is for a car that is LOST: stopped, spun, off the track,
     * where 4 m/s is an approach speed and not a race pace. A car that has been
     * nudged mid-lap already knows how fast it should be going -- it is the
     * rubber-banded speed the replay was playing at, which this port computes
     * every tick anyway. That is the floor. */
    {
        double race = (double)sample_speed(a, a->cursor > 0 ? a->cursor - 1 : 0)
                      * (double)a->coeff;
        if (want < race)
            want = race;
    }

    /* THROTTLE 1.0 below half the target, ramping to 0 at it. */
    {
        double v = rbcar_speed(&a->rb);
        double half = want * 0.5;
        if (v <= half)
            *throttle = 1.0f;
        else if (v >= want)
            *throttle = 0.0f;
        else
            *throttle = (float)((want - v) / (want - half));
    }
    *brake = 0.0f;
    /* The port's steer is -1..1 over the CAR's own lock; the controller's number
       is degrees over its own 35. Scaled here rather than in ai.h so the
       recovered constant stays the recovered constant. */
    *steer = (float)(a->ctrl_steer / AI_CTRL_LOCK);
}

/* One tick of a simulated opponent -- FUN_004fdb50's body, in its own order.
 * -> nonzero if the car went home this tick. */
static int ai_phys_step(ai_t *ai, ai_car *a, float dt)
{
    float near_pt[3], look[3], throttle = 0.0f, brake = 0.0f, steer = 0.0f;
    double dn;

    a->phys_t += dt;
    if (a->phys_hold > 0.0f)
        a->phys_hold -= dt;

    /* THE TWO DEATHS THE PLAYER ALSO HAS -- drowned, or below its own recorded
       height by AI_FELL_BELOW. Only reachable from this mode, which is the only
       mode that can drive a car anywhere like that. */
    if (ai_bump_death(a, dt))
        return 1;

    /* ===== PROGRESS IS THE RECORDING'S; ONLY THE BODY IS THE PHYSICS' =====
     *
     * The cursor, the lap and `dist` advance here EXACTLY as they do on the
     * replay path -- same `rb_move_towards` against the same rubber-banded
     * speed, same `ai_advance`, same wrap. What is simulated is where the CAR
     * is, not where it is up to in the race.
     *
     * This replaced a nearest-point re-sync that drove the cursor off the car's
     * actual position, and the re-sync was wrong twice over. It let the cursor
     * run backwards (a positive feedback loop that turned the car round), and --
     * the reason it is gone rather than patched -- it made an opponent's RACE
     * PROGRESS depend on where a shove had put it. The whole placing layer is
     * built on "an opponent's progress is its own recording walked"
     * (`ai-opponents.md`), so a car that stops walking it stops having a place:
     * measured, `wideline` went from 5.4% of frames with the wrong place to
     * 20.6%, and `progchk`'s per-lap drift to -30.91 m.
     *
     * It is also what the engine does. `FUN_004ea7b0` reads a lap and a distance
     * STORED on each racer's record rather than re-deriving them from a
     * position, which is the same separation.
     *
     * `dn` below is then the honest quantity it always should have been: how far
     * the car is from where its own schedule says it should be. */
    a->speed_rec = sample_speed(a, a->cursor > 0 ? a->cursor - 1 : 0);
    {
        /* AT THE CAR'S OWN SPEED, NOT THE SCHEDULE'S -- and that distinction is
         * the whole of "they immediately go off course".
         *
         * Walking the cursor at the rubber-banded RECORDED speed while the car
         * is being held up, shoved or steering itself home means the schedule
         * runs away from the car. `dn` -- how far the car is from where its
         * cursor says it should be -- then grows for a reason that has nothing
         * to do with the car being off its line, the exit test never fires, the
         * car stays simulated, and it drifts further. Positive feedback.
         * Measured: an opponent the player never even touched (hardest hit
         * 0.00 m/s, armed by a graze from another opponent) stayed simulated for
         * all 192 ticks of a run and ended **17.5 m** from its schedule, and
         * `progchk`'s per-lap drift hit -255 m.
         *
         * The car's own speed keeps the cursor WITH the car, which is what makes
         * `dn` mean "off the line" again. It also keeps `dist` a genuine walk of
         * the recorded polyline -- metres actually travelled along it -- which is
         * what the placing layer is built on. A car held stationary by the player
         * advances neither, which is right: it is not making progress. */
        float target = rbcar_speed(&a->rb);
        if (ai_advance(a, target, dt)) {
            a->lap++;
            a->cursor = ai_cycle_cursor(a);
            a->u = 0.0f;
        }
        a->speed = target;
    }
    /* The recorded pose at the new cursor, written WITHOUT touching the body --
       the body belongs to the integrator while this mode is running. */
    {
        const ai_sample *A = &a->s[a->cursor > 0 ? a->cursor - 1 : 0];
        const ai_sample *B = &a->s[a->cursor > 0 ? a->cursor : 0];
        float qa[4], qb[4];
        int k;
        for (k = 0; k < 3; k++)
            a->rec_x[k] = A->p[k] + (B->p[k] - A->p[k]) * a->u;
        for (k = 0; k < 4; k++) {
            qa[k] = (float)A->q[k] / AI_Q_SCALE;
            qb[k] = (float)B->q[k] / AI_Q_SCALE;
        }
        quat_slerp(qa, qb, a->u, a->rec_q);
        near_pt[0] = a->rec_x[0];
        near_pt[1] = a->rec_x[1];
        near_pt[2] = a->rec_x[2];
    }
    /* THE EXIT'S QUANTITY IS "AM I BACK ON MY LINE", which is the distance to
       the PATH and not to the schedule's current point -- a car that has been
       held up is behind its schedule and still perfectly on its racing line. */
    dn = ai_path_gap(a);
    (void)near_pt;

    /* EXIT 1 -- LOST AND UNWATCHED. See AI_PHYS_LOST_T. */
    if (a->phys_t > AI_PHYS_LOST_T) {
        double dx = (double)ai->player_prev[0] - a->rb.body.x[0];
        double dz = (double)ai->player_prev[2] - a->rb.body.x[2];
        int seen = sqrt(dx * dx + dz * dz) < (double)AI_PHYS_SEE_FAR;
        if (!seen) {
            ai_pose(a);
            ai_phys_end(a);
            a->blend_t = 0.0f;
            return 1;
        }
    }

    /* EXIT 2 -- back where its schedule says it should be. */
    if (a->phys_hold <= 0.0f && dn < AI_PHYS_HOME_DIST) {
        /* THE ENGINE'S OWN TEST, restored. It was loosened to the car's own
         * `bump_ref` while `FUN_004fddd0`'s loop was still running with its
         * steering sign inverted -- a car could not be held to 0.1 m of its line
         * because the controller was driving it away from it, so the blend had
         * to do the rejoining. With the sign measured and corrected the
         * controller converges, and the engine's threshold costs nothing:
         * `aiphys`, `aitest`, `progchk` and `wideline` are identical on both,
         * and the ten-track survey keeps its zero respawns.
         *
         * What it buys is the FEEL. At `bump_ref` the car stopped being
         * simulated while still two thirds of a metre off its line and was
         * GLIDED back by the blend; at 0.1 m it stays a real car until it has
         * genuinely driven home, which is what the engine does and what a bump
         * is supposed to look like afterwards. The survey's worst displacement
         * rises 1.431 -> 3.460 m accordingly -- a car that has been hit now
         * visibly recovers instead of being put back. */
        const float *m2 = rbcar_matrix(&a->rb);
        float fw[3];
        fw[0] = m2[8]; fw[1] = m2[9]; fw[2] = m2[10];
        if (fabs(ai_signed_angle(fw, a->rb.body.v)) < AI_PHYS_HOME_ANG) {
            ai_phys_end(a);
            return 1;
        }
    }

    if (!ai_path_ahead(a, AI_CTRL_LOOKAHEAD, look)) {
        ai_phys_end(a);
        return 1;
    }

    /* EXIT 2 -- BACK ON THE LINE. Horizontal distance to the nearest point on
       the path, and the car's heading against its own VELOCITY, both inside
       their thresholds, in the first AI_PHYS_SETTLE_T seconds. */
    {
        double dx = (double)near_pt[0] - a->rb.body.x[0];
        double dz = (double)near_pt[2] - a->rb.body.x[2];
        dn = sqrt(dx * dx + dz * dz);
    }
    /* THE 2 s GATE THE ENGINE PUTS ON THIS IS DELIBERATELY NOT REPRODUCED, and
     * it is the one divergence in this transcription. FUN_004fdb50 gates the
     * test on `phys+0x439c <= 2.0` (`0x5543c8`, read at `0x4fdcbd`) and relies
     * on its THIRD test for anything longer. That third test compares the body
     * against the point AI_CTRL_LOOKAHEAD metres up the path from the cursor --
     * and here the cursor is re-synced to the nearest sample every tick, so that
     * distance is 2.7 m by construction and the test can never fire. The
     * engine's cursor must therefore lag its car in a way `FUN_004fd8b0` has not
     * been read closely enough to reproduce.
     *
     * Rather than ship a mode with no working exit -- measured: cars sat
     * simulated for the whole run, median 2.07 m off their own line, and an
     * opponent that should have driven into a parked player missed it entirely
     * -- the semantic test runs every tick. "Back on the line and pointing
     * along it" is what both of the engine's tests are asking; only its
     * bookkeeping is unrecovered. */
    /* HOME IS WITHIN THE CAR'S OWN REFERENCE DISPLACEMENT, and then the BLEND
     * carries it the rest of the way.
     *
     * The engine's test is AI_PHYS_HOME_DIST -- 0.1 m, a quarter of a car -- and
     * this port cannot hold a car to it: `FUN_004fddd0`'s loop is transcribed
     * but not yet STABLE here, and a car asked to thread that needle instead
     * wanders off (measured: 0.13 m off its line at the knock, 4.4 m two and a
     * half seconds later, recovered only by the 5 s lost-exit). Simulating a car
     * badly for five seconds is worse than the bug this replaces.
     *
     * So the two recovered mechanisms are used for what each is good at: the
     * controller keeps a shoved car sane for the moment it is genuinely
     * disturbed, and FUN_00503190's one-second ease -- which is bounded,
     * monotone and needs no controller at all -- does the rejoining. The
     * threshold is the car's OWN `bump_ref`, two proxy reaches (0.63 m on the
     * Overkill), which is the same number every other return in this file is
     * scaled by; it is not a typed constant.
     *
     * The engine's own 0.1 m test is kept as the tighter of the two, so a car
     * that IS threaded home exits on the engine's terms. */
    if (a->phys_hold <= 0.0f) {
        float home = a->bump_ref > 1e-3f ? a->bump_ref : AI_PHYS_HOME_DIST;
        if (dn < AI_PHYS_HOME_DIST || dn < home) {
            ai_phys_end(a);
            return 1;
        }
    }

    /* STILL OUT THERE: drive. */
    ai_ctrl_command(a, look, &throttle, &brake, &steer, dt);
    rbcar_step(&a->rb, throttle, brake, steer, 0, dt);
    a->speed = rbcar_speed(&a->rb);
    /* `rec_x`/`rec_q` STAY THE RECORDING'S, which is the whole reason the
     * displacement is measurable at all: `rec` is where the recording says the
     * car is, `body` is where it actually is, and `bump` is the distance between
     * them whichever mechanism put it there.
     *
     * Writing the BODY into `rec` here (which this function did for one
     * revision) makes the two identical, so every measure of "how far off its
     * line is it" reads zero on a car that has been thrown across the track --
     * and twelve of aitest part 9's checks are exactly that measure. It is the
     * same shape as `traps.md`'s "TELEMETRY IS NOT THE THING". */
    {
        const ai_sample *A = &a->s[a->cursor - 1];
        const ai_sample *B = &a->s[a->cursor];
        float qa[4], qb[4];
        int k;
        for (k = 0; k < 3; k++)
            a->rec_x[k] = A->p[k] + (B->p[k] - A->p[k]) * a->u;
        for (k = 0; k < 4; k++) {
            qa[k] = (float)A->q[k] / AI_Q_SCALE;
            qb[k] = (float)B->q[k] / AI_Q_SCALE;
        }
        quat_slerp(qa, qb, a->u, a->rec_q);
    }
    ai_bump_measure(a);
    return 0;
}

/* FUN_00503190 -- THE EASE HOME, and the reason leaving the mode is continuous.
 *
 * The car is dead-reckoned from where it actually ended up (`blend_x`/`blend_q`,
 * FUN_004fda10's snapshot), carried forward by the step the RECORDING took this
 * tick, and lerped toward the recording by `1 - countdown`. Past AI_BLEND_SNAP
 * the engine abandons the blend and takes the recording outright -- the only
 * snap in its whole opponent model.
 *
 * `x0`/`q0` are the recorded pose BEFORE this tick's advance, which ai_step
 * already keeps for ai_diff_velocity.
 */
static void ai_blend_pose(ai_car *a, const float x0[3], const float q0[4],
                          float dt)
{
    float d[3], inv[4], rel[4], mq[4], mx[3], res_x[3], res_q[4];
    double gap;
    float t, k;
    int i;

    if (a->blend_t <= 1e-6f)
        return;

    for (i = 0; i < 3; i++)
        d[i] = a->rec_x[i] - x0[i];

    inv[0] = q0[0]; inv[1] = -q0[1]; inv[2] = -q0[2]; inv[3] = -q0[3];
    rb_quat_mul(inv, a->rec_q, rel);
    rb_quat_mul(a->blend_q, rel, mq);
    rb_quat_normalize(mq);
    for (i = 0; i < 3; i++)
        mx[i] = a->blend_x[i] + d[i];

    gap = sqrt((double)(mx[0] - a->rec_x[0]) * (mx[0] - a->rec_x[0])
             + (double)(mx[1] - a->rec_x[1]) * (mx[1] - a->rec_x[1])
             + (double)(mx[2] - a->rec_x[2]) * (mx[2] - a->rec_x[2]));

    if (gap <= AI_BLEND_SNAP) {
        t = a->blend_t;
        k = (t < 0.0f) ? 1.0f : (t <= 1.0f ? 1.0f - t : 0.0f);
        for (i = 0; i < 3; i++)
            res_x[i] = mx[i] + (a->rec_x[i] - mx[i]) * k;
        quat_slerp(mq, a->rec_q, k, res_q);
    } else {
        /* PAST AI_BLEND_SNAP the engine abandons the blend and takes the
           recording outright -- the only snap in its opponent model. */
        memcpy(res_x, a->rec_x, sizeof(res_x));
        memcpy(res_q, a->rec_q, sizeof(res_q));
    }

    memcpy(a->blend_x, res_x, sizeof(res_x));
    memcpy(a->blend_q, res_q, sizeof(res_q));
    memcpy(a->rb.body.x, res_x, sizeof(res_x));
    memcpy(a->rb.body.q, res_q, sizeof(res_q));
    rb_car_update_matrix(&a->rb);

    /* FUN_00503880 decrements it, not FUN_00503190. ON THE GUARD'S OWN
       EPSILON: AI_BLEND_T less sixty ticks of 1/60 leaves 2.8e-7 in float, not
       0, and the guard above takes that as finished -- so the countdown used to
       stop one hair short of zero for ever and the hand-back below never ran. */
    a->blend_t -= dt;
    if (a->blend_t <= 1e-6f)
        a->blend_t = 0.0f;

    /* AND THE HAND-BACK HAS TO BE WHERE THE EASE LEFT THE CAR. From the next
     * tick the pose is ai_bump_apply's again, `rec + off`, and nothing above
     * reads `off` -- but the impulse half of every contact during the ease still
     * lands in `offv`, and ai_bump_relax integrates it, so the offset the ease
     * hands back is one that was never on screen. `aiphys` measured it as the
     * rest of the unattributed kinematic steps: a car standing exactly on its
     * recording with 0.27 to 0.52 m of offset nobody had seen, realised in one
     * tick when the countdown ran out. The offset is re-based on the body instead
     * -- about zero at the end of an ease, by construction -- so the hand-back is
     * continuous and the spring takes it from there. */
    if (a->blend_t <= 0.0f) {
        for (i = 0; i < 3; i++) {
            a->off[i] = a->rb.body.x[i] - a->rec_x[i];
            a->offv[i] = 0.0f;
        }
        a->off_yaw = a->off_yawv = 0.0f;
        a->off_gnd = 0.0f;
    }
}

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

/* ------------------------------------------------------- a remote player
 *
 * See ai.h. Nothing here walks a recording, because there is none.
 */
int ai_remote_init(ai_t *ai, int track, const rb_world *w,
                   const unsigned char *car, int n)
{
    int i;

    if (!ai)
        return 0;
    /* THE WHOLE ai_t, so the recorded-lap machinery cannot be half-present:
       a slot with a `remote' flag and a stale sample array behind it is
       exactly the state a later reader would trust. `blob' is NULL after
       this, so ai_free stays correct. */
    ai_free(ai);
    memset(ai, 0, sizeof *ai);
    ai->track = track;
    if (n < 0) n = 0;
    if (n > AI_MAX_OPPONENTS) n = AI_MAX_OPPONENTS;
    for (i = 0; i < n; i++) {
        ai_car *a = &ai->car[i];
        a->car = (car && car[i] <= 2) ? (int)car[i] : 0;
        a->remote = 1;
        a->slot = i;
        snprintf(a->name, sizeof a->name, "P%d", i + 1);
        /* NULL world to rbcar_init, exactly as a recorded opponent gets: the
           pose comes from outside and not from a ground probe. */
        rbcar_init(&a->rb, a->car, NULL, 0.f, 0.f, 0.f, 0.f);
        a->rb.world = w;
        rb_boost_reset(&a->rb);
        ai_bump_derive(a);
        ai->n++;
    }
    rlog("ai: %d remote player(s) on track %d\n", ai->n, track);
    return ai->n;
}

int ai_grid(int track, const char *asset_dir, float out[][3], int max)
{
    char path[256];
    unsigned char hdr[12];
    FILE *f;
    long size;
    unsigned int n_file, i;
    int n = 0;

    if (track < 0 || track >= AI_N_RACES || !out || max <= 0)
        return 0;
    snprintf(path, sizeof(path), "%s/%s.aip", asset_dir ? asset_dir : ".",
             AI_RACES[track].track);
    f = fopen(path, "rb");
    if (!f) {
        rlog("ai: no %s -- no authored grid\n", path);
        return 0;
    }
    /* See ASSET_IOBUF in scene.h: newlib's fread never reads past the FILE
       buffer, so an unset one turns the load into 1 KB syscalls. */
    setvbuf(f, NULL, _IOFBF, ASSET_IOBUF);
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "AIP1", 4) != 0) {
        fclose(f);
        return 0;
    }
    memcpy(&n_file, hdr + 4, 4);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    /* THE SAME BOUND ai_init TAKES, and for the same reason: the opponent count
       is file data and the record array is indexed on it. Tested before the
       multiply so it cannot overflow. */
    if (n_file == 0 || n_file > AI_MAX_OPPONENTS
        || size < (long)(12 + (long)n_file * AI_RECORD_BYTES)) {
        fclose(f);
        return 0;
    }
    for (i = 0; i < n_file && n < max; i++) {
        unsigned int u32[3];
        float p[3];
        /* the record's sample-block fields, at +64: count, cycle start, offset */
        if (fseek(f, 12 + (long)i * AI_RECORD_BYTES + 64, SEEK_SET) != 0
            || fread(u32, 1, sizeof u32, f) != sizeof u32)
            break;
        if (u32[0] < 1
            || (unsigned long long)u32[2]
               + (unsigned long long)u32[0] * AI_SAMPLE_BYTES
               > (unsigned long long)size)
            continue;
        /* The first sample's own p[3], which is the first twelve bytes of an
           ai_sample -- and the field this port has already proved is a position
           in world metres (ai.h). */
        if (fseek(f, (long)u32[2], SEEK_SET) != 0
            || fread(p, 1, sizeof p, f) != sizeof p)
            break;
        out[n][0] = p[0];
        out[n][1] = p[1];
        out[n][2] = p[2];
        n++;
    }
    fclose(f);
    return n;
}

void ai_remote_park(ai_t *ai, int i, float x, float y, float z, float yaw)
{
    ai_car *a;
    const rb_world *w;

    if (!ai || i < 0 || i >= ai->n)
        return;
    a = &ai->car[i];
    if (!a->remote)
        return;
    /* KEPT ACROSS THE RE-INIT, because rbcar_init overwrites the whole rb_car
       and the world was handed over once, by ai_remote_init. */
    w = a->rb.world;
    /* A FULL rbcar_init, not a poke at body.x: the pose is a position AND an
       orientation AND a suspension state, and half of one is the state a later
       reader would trust. NULL world, as ai_remote_init hands it -- the caller
       has already probed the ground, because the world lives over there. */
    rbcar_init(&a->rb, a->car, NULL, x, y, z, yaw);
    a->rb.world = w;
    rb_boost_reset(&a->rb);
}

void ai_remote_look(ai_t *ai, int i, const unsigned char up[3], int skin)
{
    ai_car *a;

    if (!ai || i < 0 || i >= ai->n || !up)
        return;
    a = &ai->car[i];
    if (!a->remote)
        return;
    /* CLAMPED HERE, not by the drawer: these three came off the wire and the
       peer that sent them is not this build. carparts_apply clamps too, but the
       booster level is also read by fx_pipe_from_rig, which indexes a table. */
    a->boost = up[0] <= 3 ? (int)up[0] : 3;
    a->reson = up[1] <= 3 ? (int)up[1] : 3;
    a->tires = up[2] <= 3 ? (int)up[2] : 3;
    a->skin  = (skin >= 0 && skin < 4) ? skin : 0;
}

void ai_remote_pose(ai_t *ai, int i, const ai_sample *s)
{
    float y[RB_STATE_N];
    ai_car *a;
    int k;

    if (!ai || i < 0 || i >= ai->n || !s)
        return;
    a = &ai->car[i];
    if (!a->remote)
        return;
    /* THE RECORDED-LAP UNPACK, verbatim -- one sample in, one rb state out.
       unpack_state needs the car for its wheel radii (len_extra) and reads
       nothing else off it, which is why a remote slot needs no recording. */
    unpack_state(a, s, y);
    rb_car_set_state(&a->rb, y);
    /* AND THE VELOCITY IS THE SENDER'S, not a difference of two poses. ai_pose
       finite-differences a replayed car because a recording's samples are
       milliseconds apart; these are up to 200 ms apart (net.h) and the sender
       measured its own velocity, so differencing would be a worse number
       computed from a longer baseline. The state slot is a MOMENTUM, which is
       why unpack_state multiplied it back up by the mass. */
    for (k = 0; k < 3; k++)
        a->rb.body.v[k] = (float)s->mom[k] / AI_VEL_SCALE;
    a->rb.body.w[0] = a->rb.body.w[1] = a->rb.body.w[2] = 0.f;
    /* AND `speed' WITH IT, or the car drives past in silence. That field is
     * filled by ai_step -- which a remote slot never enters -- so it stayed 0
     * for the whole race while main.c pitched the opponent's motor voice by
     * `speed / top'. A car that sounds stopped while it overtakes you was the
     * report; the number was on the wire the whole time.
     *
     * THE SAME EXPRESSION ai_step ENDS ON, all three axes and not the two a
     * ground speed would use -- so a remote car's `speed' means exactly what a
     * replayed one's does and the two can be compared. Grepped before writing,
     * per traps.md: the only other reader that can see a remote slot is
     * TRIANGLE's inventory dump, which was printing the same 0 and is now
     * honest too. ai.c's own readers are all inside the step and decide paths,
     * and neither runs for a remote car. */
    a->speed = (float)sqrt((double)a->rb.body.v[0] * a->rb.body.v[0]
                           + (double)a->rb.body.v[1] * a->rb.body.v[1]
                           + (double)a->rb.body.v[2] * a->rb.body.v[2]);
}

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
        /* and no hint: a reset car has not been found on the spine yet, so its
           first query searches the whole of it. 0 is a VALID arc position (the
           start line), which is why this is negative and not zero. */
        a->spine_at = -1.0f;
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
        /* And its decision with it: a car re-gridded still holding a side would
           set off round an obstacle that is no longer there. */
        a->buried_for = 0.0f;
        a->respawns = 0;
        /* AND OUT OF PHYSICS MODE. A restart with a car still recovering from a
           shove would grid it and then go on driving it back to a line it is
           already standing on. No ai_phys_end here: that arms the ease home,
           and a re-gridded car has nothing to ease from. */
        a->phys_mode = 0;
        a->phys_t = 0.0f;
        a->phys_hold = 0.0f;
        a->ctrl_steer = 0.0f;
        a->blend_t = 0.0f;
        ai_steer_clear(a);
        rb_boost_reset(&a->rb);
        /* AND THAT IS AS FAR AS A REMOTE SLOT GOES. Everything above is state
           this file owns and can safely zero; everything below reads the
           RECORDING -- and a remote slot has none, so `ai_pose' unpacked
           `a->s[0]' through a null pointer and `sample_speed' indexed it. A
           latent null dereference on every restart of a network race.
           Where the car goes instead is the app's business: it holds the world,
           the grid and the wire (`ai_remote_park'). */
        if (a->remote)
            continue;
        ai_pose(a);
        a->speed_rec = sample_speed(a, 0);
    }
    ai->player_reach = 0.0f;
    ai->player_dist = 0.0f;
    ai->player_at = -1.0f;
    ai->player_at_proj = -1.0f;
    ai->player_lap_seam = 0;
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
    const rb_world *w = a->rb.world;
    int i, air = 1;

    for (i = 0; i < a->rb.nwheels; i++) {
        rb_wheel *wh = &a->rb.wheel[i];
        int loaded = wh->len < wh->len_free - AI_DROOP_TOL;
        float centre[3], radius = wh->radius;

        memset(&a->rb.hit[i], 0, sizeof(a->rb.hit[i]));
        a->rb.hit[i].active = loaded;
        if (loaded)
            air = 0;

        /* WHERE THE PATCH IS, and it is filled whether the wheel is loaded or
         * not so that a caller which ignores `active` still gets a sane point.
         *
         * `active` alone was all this ever wrote, because rb_wheel_spin_update
         * -- the only reader for as long as it was the only reader -- asks
         * nothing else. fx.c asks where: it emits dust AT hit[].point, so with
         * the field left at the memset's zero every opponent raised its dust at
         * the world origin. Nothing complained, because nothing emitted.
         *
         * The centre comes from the recorded suspension through the car's own
         * rb_wheel_frame -- `use_extra` = 1, the animation proc's variant, since
         * the recording carries len_extra and this is a VISUAL contact point --
         * and the patch is one radius down the body's up axis from it.
         *
         * DOWN THE STRUT, NOT DOWN THE SURFACE NORMAL, and that is a knowing
         * approximation rather than an oversight: the exact patch is where
         * col_sphere puts it, and asking would be a collision query per wheel
         * per opponent per tick -- the cost the whole replay exists to avoid.
         * On the flat the two agree exactly; on a slope of t they differ by
         * radius*(1-cos t), which at the Overkill's 70 mm wheel is 1 mm at 10
         * degrees and 4 mm at 20. A dust puff is 0.42 m across. */
        rb_wheel_frame(&a->rb, i, 1, centre, &radius, NULL, NULL);
        a->rb.hit[i].point[0] = centre[0] - a->rb.m[4] * radius;
        a->rb.hit[i].point[1] = centre[1] - a->rb.m[5] * radius;
        a->rb.hit[i].point[2] = centre[2] - a->rb.m[6] * radius;

        /* WET, on the host's own water probe and behind the SAME gate rb_collide
           applies (collide.c: `water_gap < radius`). Without it an opponent
           fording beach_1's river throws dust off a submerged tyre. The probe is
           a per-column grid read, so this is O(1) per wheel and not a query into
           the triangles. */
        if (w && w->water
            && w->water(w->ctx, i, centre, &a->rb.hit[i].water_gap))
            a->rb.hit[i].in_water = a->rb.hit[i].water_gap < radius;
    }
    a->airborne = air;
}

/* A REMOTE CAR'S WHEELS. See ai.h -- everything about why this exists is there.
   Placed here rather than beside ai_remote_pose because it is the one caller of
   ai_fake_contacts outside ai_step and that function is static and above. */
void ai_remote_spin(ai_t *ai, float dt)
{
    int i;

    if (!ai || dt <= 0.f)
        return;
    for (i = 0; i < ai->n; i++) {
        ai_car *a = &ai->car[i];
        if (!a->remote)
            continue;
        /* WHICH WHEELS ARE ON THE GROUND, off the suspension that arrived --
           the recorded path's own test, unchanged, because the signal is the
           same one: a strut extended to its free length is hanging. */
        ai_fake_contacts(a);
        /* AND THE INPUTS ARE LEFT ALONE. `ai_throttle' has no meaning here --
           it reads the RECORDING's commanded speed against the car's own, and a
           remote slot has no recording -- and the only thing in.accel reaches on
           this path is the wheelspin branch below, whose amplitude is
           SpeedAngMaxREL, 0 for all three retail cars (rb.h). So a remote car's
           wheels roll at the patch speed and never spin up, which is what the
           retail game does for every car including the player's. */
        rb_wheel_spin_update(&a->rb, dt);
    }
}

/*
 * THE PORT'S, and the derivation is stated here because the QUANTITY is the
 * engine's and only the SOURCE is invented.
 *
 * The exhaust reads one bit: FUN_005303c0 asks the car whether the throttle is
 * down (phys+0x576c, rb_input.accel) and emits nothing at all when it is not --
 * `gas_rate` returns 0 through its last branch. So a car with no driver makes no
 * smoke, which is exactly what an opponent is: ai_step writes its pose and never
 * touches its inputs, so `in` stays as ai_reset left it, all zero, forever.
 * That is the whole of "AI cars have no exhaust smoke" -- not a missing emitter,
 * a missing throttle.
 *
 * The engine has the same seam and fills it from the other mode: FUN_004fddd0,
 * the steering controller, writes action bit 3 (Forw) into that very field, so
 * an opponent's smoke there comes from its own throttle. The port does not run
 * that controller (see ai.h), so the bit has to come off the replay instead.
 *
 * A REPLAY DOES NOT RECORD THE THROTTLE. The 32-float ODE state is (x, q, P, L)
 * plus the suspension and the steer angle; there is no driver input in it, and
 * .aip packs less than that again. What the replay does carry is the speed the
 * recording is being asked for -- `speed_rec * coeff`, the rubber-banded command
 * ai_step already computes -- against the speed the car is actually doing. A
 * command at or above the current speed is a car being driven; a command below
 * it is a car being slowed. That is the same shape as the controller's own rule
 * (throttle 1.0 below the target, ramping to 0 at it), read off the one signal
 * this path has.
 *
 * The tolerance is what keeps a CRUISING car smoking. In the steady state
 * rb_move_towards returns the command exactly and the car converges onto it, so
 * command and speed sit on top of each other and a strict test flickers on float
 * noise. AI_THROTTLE_COAST is the margin below which "not being slowed" still
 * counts as throttle -- a real car holding a speed is holding it ON the engine.
 *
 * Deliberately NOT set: in.brake, which would need a second invented threshold
 * and whose only effect here would be to let a decelerating opponent count as
 * "spinning" in gas_rate and fx_dust_rate. An opponent's wheels never spin: the
 * recorded suspension drives rb_wheel_spin_update and the slip it produces is
 * the road's, not a driver's.
 *
 * INERT FOR THE REPLAY, and that is checkable rather than argued. The only other
 * reader of in.accel on this path is rb_wheel_spin_update's wheelspin branch,
 * which multiplies tune.speed_ang_max_rel -- SpeedAngMaxREL, 0 in the retail
 * game for all three cars (rb.h). aitest part 4 compares a driven lap against a
 * reference build with this whole function removed and reports it bit-identical.
 */
static void ai_throttle(ai_car *a)
{
    float command = a->speed_rec * a->coeff;

    a->rb.in.accel = (command >= a->speed - AI_THROTTLE_COAST);
    /* The analogue value beside the bit, as FUN_004fddd0 writes phys+0x5770
       beside phys+0x576c. Nothing on the replay path reads it -- it is here so
       that a car handed to a consumer expecting both is not half-driven. */
    a->rb.in.throttle = a->rb.in.accel ? 1.0f : 0.0f;
}

/* A FORWARD CROSSING OF THE SPINE'S SEAM, from the previous within-lap arc
 * position to this one. -> 1 to add a lap, -1 to take one off, 0 otherwise.
 *
 * "Wrapped" is a jump of more than half the spine, which is safe because
 * cp_spine_dist_near is windowed to CP_SPINE_WINDOW (15 m) and cannot jump
 * further than that any other way -- the two facts hold each other up. A
 * negative `prev` is the first query after a reset and counts nothing. */
static int ai_seam_cross(float prev, float now, float spine_len)
{
    if (prev < 0.f || spine_len <= 0.f)
        return 0;
    if (now < prev - spine_len * 0.5f)
        return 1;                       /* over the line, forward */
    if (now > prev + spine_len * 0.5f)
        return -1;                      /* back over it */
    return 0;
}

/* THE PLAYER'S OWN PROGRESS, on its own -- see ai.h.
 *
 * LIFTED OUT OF `ai_step' RATHER THAN COPIED, because a network race needs this
 * and nothing else out of the step: there is no field to walk, no rubber band
 * to apply and no recording to read, and `main.c' does not call `ai_step' at
 * all in one. It used to have no second implementation either, which is the bug
 * -- `player_dist' sat at 0 for the whole of every network race, every remote
 * car ranked ahead of it, and both players read 2nd of two from the grid to the
 * flag. One copy, so the two callers cannot measure the player differently from
 * each other or from the field they are ranked against. */
int ai_player_progress(ai_t *ai, const ai_track *tr,
                       float px, float py, float pz)
{
    float pdist = 0.0f;
    int pcp = 0;
    int have_spine = 0;

    if (!ai || !tr)
        return 0;
    if (tr->spine)
        have_spine = tr->spine(tr->ctx, px, py, pz, ai->player_at,
                               &pdist, &pcp);
    /* THE PLAYER'S PROGRESS, off the LATCHED checkpoint index rather than off a
     * projection -- see ai_track.lap_progress. The projection stays bound for
     * `player_cp`, which is the gap term the rubber band's own curve indexes and
     * which a few metres of ambiguity does not disturb. */
    if (tr->lap_progress) {
        float lp = 0.f;
        if (tr->lap_progress(tr->ctx, px, py, pz, &lp)) {
            ai->player_lap_seam += ai_seam_cross(ai->player_at, lp,
                                                 tr->spine_len);
            ai->player_at = lp;
            ai->player_dist = lp
                              + (float)ai->player_lap_seam * tr->spine_len;
        }
    }
    if (have_spine) {
        ai->player_at_proj = pdist;
        ai->player_cp = pcp;
    }
    return have_spine;
}

void ai_step(ai_t *ai, const ai_track *tr, float px, float py, float pz,
             int player_lap, float dt)
{
    int i;
    int have_spine = 0;

    if (!ai || ai->n <= 0 || dt <= 0.0f)
        return;

    /* THE PLAYER'S VELOCITY, for the steering decision's closing test -- see
       ai_t.player_v. First tick is a standing start by definition. */
    if (ai->player_seen && dt > 1e-6f) {
        ai->player_v[0] = (px - ai->player_prev[0]) / dt;
        ai->player_v[1] = (py - ai->player_prev[1]) / dt;
        ai->player_v[2] = (pz - ai->player_prev[2]) / dt;
    } else {
        ai->player_v[0] = ai->player_v[1] = ai->player_v[2] = 0.0f;
        ai->player_seen = 1;
    }
    ai->player_prev[0] = px;
    ai->player_prev[1] = py;
    ai->player_prev[2] = pz;

    have_spine = ai_player_progress(ai, tr, px, py, pz);
    (void)player_lap;

    for (i = 0; i < ai->n; i++) {
        ai_car *a = &ai->car[i];
        float target, adist = 0.0f;
        float x0[3], q0[4];
        int acp = 0, gap = 0, wrapped = 0;

        /* A REMOTE PLAYER IS POSED, NEVER STEPPED. It has no recording to walk,
           no spine progress of its own worth rubber-banding and nothing this
           machine is entitled to decide about it -- see ai.h. Belt and braces:
           a network race builds the field with ai_remote_init and the app does
           not call this at all, so this is the second of two gates and exists
           so that adding a third caller cannot start simulating somebody
           else's car. */
        if (a->remote)
            continue;

        /* THE PUSH BUDGET'S TICK STARTS HERE, for either mode -- see
           ai_car.sim_push. A simulated car used to reset it at the top of
           ai_phys_step, which is this same instant. */
        a->sim_push = 0.0f;

        /* ===== THE DISPATCH, and it is FUN_004f72f0's own =====
         *
         * A car something has touched is not a replay: it is a real rb_car that
         * the recovered controller is steering back to its line, and none of the
         * replay below applies to it. `FUN_004f72f0` branches on exactly this
         * flag; `ai_phys_step` is the `else` -- carPhysTick plus FUN_004fdb50.
         *
         * The rubber band, the lead and the coefficient are deliberately NOT
         * evaluated while it is out there. The engine does not evaluate them
         * either (the whole of FUN_00503880 is on the other branch), and they
         * are quantities about a car walking a recording. `ai_ctrl_resync`
         * keeps `dist` and the cursor honest so the placing survives the trip. */
        if (a->phys_mode) {
            /* NO ai_fake_contacts AND NO rb_wheel_spin_update HERE. Both exist
               because a replayed car has no collision of its own; a simulated
               one does -- rbcar_step fills `hit[]` from the real query and calls
               the spin integrator itself (rbcar.c). Calling the fakes here would
               overwrite the real contact set with one derived from a recording
               the car is not currently following, which is also where its dust
               and its lightmap sample come from. */
            ai_phys_step(ai, a, dt);
            /* AND THE PLACING'S RULER GOES WITH IT. `spine_dist` is what
             * `ai_player_place` and the HUD compare cars on, and it is DERIVED
             * from `dist` -- which `ai_phys_step` advances. Leaving the
             * derivation to the replay path meant it FROZE for every tick a car
             * spent simulated while `dist` went on without it, so the two drifted
             * apart by exactly the ground covered in the mode.
             *
             * Measured: `progchk`'s "an opponent covers exactly one spine length
             * per lap of road" at **-253 m/lap** on three tracks, and `wideline`
             * down from 4/1 to 1/4 -- while `dist` itself was correct to within
             * 5 m on every car of every track at every difficulty, which is what
             * made it look like a cursor bug for three rounds. It was not the
             * cursor; it was the one line downstream of it that never ran. */
            if (tr && tr->spine_len > 1e-3f && a->lap_len > 1e-3f)
                a->spine_dist = a->dist * (tr->spine_len / a->lap_len);
            continue;
        }

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
                         a->spine_at, &adist, &acp)) {
            a->spine_at = adist;
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
            /* AN OPPONENT'S PROGRESS IS ITS OWN RECORDING, not a projection:
             * `dist` is metres walked along a polyline of two to eleven thousand
             * samples, exact and monotonic by construction, and `path_len` is
             * that polyline's length. Scaled to the spine so the two sides are in
             * the same metres.
             *
             * This is what the engine does -- FUN_004ea7b0 reads a lap and a
             * distance STORED on each racer's record rather than re-deriving them
             * from a position -- and it is why the projection's 4-to-16 jumps a
             * lap never reach the placing. */
            /* BY THE LAP, NOT BY THE POLYLINE. `path_len` is the lead-in plus
               the lap (ai_car.lap_len), so dividing by it made every lap after
               the first 1.8% to 3.8% short and compounding. */
            a->spine_dist = (a->lap_len > 1e-3f)
                            ? a->dist * (tr->spine_len / a->lap_len)
                            : adist;
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
            a->cursor = ai_cycle_cursor(a);
            a->u = 0.0f;
            wrapped = 1;
        }

        ai_pose(a);
        if (wrapped) {
            /* THE SEAM, and FUN_00503880's own answer to it: the recording does
             * not close, so hold the car where it is and let the offset spring
             * carry it onto the new lap -- and do NOT difference the velocity
             * across the gap, because the original returns before the function
             * that would. See ai_seam_latch, which has the measurements. */
            ai_seam_latch(a, x0, q0);
        } else {
            ai_diff_velocity(a, x0, q0, dt);
            a->speed = (float)sqrt((double)a->rb.body.v[0] * a->rb.body.v[0]
                                   + (double)a->rb.body.v[1] * a->rb.body.v[1]
                                   + (double)a->rb.body.v[2] * a->rb.body.v[2]);
        }
        ai_fake_contacts(a);
        /* After a->speed, because the throttle is a comparison against it, and
           before rb_wheel_spin_update, which is the one transcribed reader of
           in.accel -- so an opponent reaches it in the same state a driven car
           would. See ai_throttle for why that read is inert here. */
        ai_throttle(a);
        rb_wheel_spin_update(&a->rb, dt);

        /* THE PORT'S: decide, then work off whatever the last tick's contacts
         * left in the offset and put the car back where that says it is. Last,
         * so everything above -- the cursor, the lead, the recorded speed, the
         * wheel spin -- is measured on the recording and not on the shove or on
         * the line the car has chosen. */
        {
            float want[3];
            /* THE DECISION, on the pose ai_pose has just written -- so the
               lookahead is off this tick's cursor and the obstacles are where
               they are now. See ai.h, "the steering decision". */
            ai_steer_decide(ai, i, px, py, pz, dt);
            /* The lateral scalar becomes a world vector against the SAME frame
               the decision was taken in -- ai_car.steer_left, which the decision
               left there. Kept as a scalar plus an axis rather than as a world
               vector so a held decision stays ACROSS the line as the line turns
               through a corner. */
            want[0] = a->steer_left[0] * a->steer_want;
            want[1] = 0.0f;
            want[2] = a->steer_left[2] * a->steer_want;
            ai_bump_relax(a, dt, want,
                          (float)(a->steer_cmd * (3.14159265358979 / 180.0)));
        }
        ai_bump_apply(a);
        /* AND LAST, THE EASE HOME -- FUN_00503190, which has the final word on
         * the pose because in the engine it IS the pose writer. Inert unless the
         * car has just come out of physics mode (`blend_t` > 0), so an opponent
         * nothing has touched is bit-identical to one from before this existed.
         *
         * RETIRED HERE: `ai_bump_death`. Its burial test set `off` to zero on the
         * spot, which is a jump of up to the whole offset and is the reported
         * "opponents teleport when you push them into things". There is no
         * counterpart to it anywhere in the engine -- a car that has been shoved
         * into a bank is SIMULATED now, and drives out under the controller. The
         * function is kept, unreferenced from the step, because its drowning and
         * fell-out-of-the-world tests are still the right answer for a car that
         * ends up somewhere no amount of driving recovers; see ai.h. */
        ai_blend_pose(a, x0, q0, dt);
        /* AFTER THE BLEND, because the blend is the last thing that moves the
           body and `bump` has to describe where the car ended up. */
        ai_bump_measure(a);
    }

    /* THE PORT'S, and the field's own business rather than the player's:
     * opponents are solid to each other too. Same routine, both sides bumpable.
     * After every car has been posed, because a pair solved against a pose that
     * is about to be overwritten does nothing at all. */
    ai_collide_field(ai);
}

/* ------------------------------------------------------------------ contact */

/* RETIRED WITH THE OLD DIRECTION: `ai_touch`, AI_MAX_TOUCH and `ai_touch_list`,
   which built the deepest-twelve list of overlapping sphere pairs so the
   response could be solved along one pair's own normal. The engine does not
   resolve car-vs-car that way -- see ai_pair_resolve -- so all the proxy is
   asked now is whether anything overlaps and by how much, which is
   ai_deepest_pair and needs no list, no ordering and no cap. */

/* One side of a contact pair. `ai` is NULL for a body that takes the reaction in
   its own rigid state -- the player -- and non-NULL for one that takes it in a
   bump offset. Everything below is written against this so that player-against-
   opponent and opponent-against-opponent are the same solve. */
typedef struct {
    rb_car *car;
    ai_car *ai;    /* NULL for a body that takes the reaction in its own state */
    ai_car *sim;   /* set when that body is a SIMULATED opponent -- see sim_push */
} ai_actor;

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

    /* A REMOTE CAR TAKES NOTHING. It is not simulated here at all: its pose
       arrives from the machine that owns it and is written straight into the
       body, so an offset velocity accumulated locally is a number the next
       packet contradicts -- and `ai_bump_apply' would spend it by moving the
       body to `rec_x + off', with `rec_x' the origin, because only the recorded
       path ever fills that. A player nudging an opponent teleported it to the
       middle of the map. See ai_actor_move, which refuses the positional half
       for the same reason and by the same rule. */
    if (a->remote)
        return;

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
        float use[3];
        memcpy(use, dv, sizeof(use));
        /* A SIMULATED OPPONENT HAS A PER-TICK PUSH BUDGET -- see ai_car.sim_push.
           The player has none and keeps the old behaviour byte for byte. */
        if (b->sim) {
            double mag = sqrt((double)dv[0] * dv[0] + (double)dv[1] * dv[1]
                            + (double)dv[2] * dv[2]);
            double room = (double)RB_CARS[b->sim->car].tune.speed_boost_max
                          / 3.6 / 60.0 - b->sim->sim_push;
            if (room < 0.0) room = 0.0;
            if (mag > room) {
                double k2 = (mag > 1e-9) ? room / mag : 0.0;
                use[0] = (float)(dv[0] * k2);
                use[1] = (float)(dv[1] * k2);
                use[2] = (float)(dv[2] * k2);
                mag = room;
            }
            b->sim->sim_push += (float)mag;
        }
        for (k = 0; k < 3; k++) {
            b->car->body.x[k] += use[k];
            taken[k] = use[k];
        }
        rb_car_update_matrix(b->car);
        return;
    }
    /* A REMOTE CAR REFUSES THE WHOLE MOVE -- which is not a special case but the
       strongest form of the refusal this function already reports: it is a car
       that will not budge, so the caller hands the whole separation to the other
       body, which is the player and really is simulated here. See
       ai_take_impulse. */
    if (a->remote) {
        taken[0] = taken[1] = taken[2] = 0.f;
        return;
    }
    memcpy(before, a->off, sizeof(before));
    {
        /* AND A KINEMATIC ONE HAS THE SAME BUDGET, which `ai_bump_clamp` never
         * was: that bounds how FAR off its line the car is, not how fast it gets
         * there. Two replays whose recordings run through each other overlap by
         * up to half a car in one tick, and the positional half cleared all of it
         * at once -- measured by `aiphys` as a car stepping 0.40 to 0.57 m in a
         * tick at 3 to 6 m/s, every one of them a field pair and never the
         * player. What is refused here is handed to the other body exactly as a
         * simulated car's refusal is, and what neither takes is left for the next
         * tick. */
        double mag = sqrt((double)dv[0] * dv[0] + (double)dv[1] * dv[1]
                        + (double)dv[2] * dv[2]);
        double room = (double)RB_CARS[a->car].tune.speed_boost_max
                      / 3.6 / 60.0 - a->sim_push;
        double k2 = 1.0;
        if (room < 0.0) room = 0.0;
        if (mag > room)
            k2 = (mag > 1e-9) ? room / mag : 0.0;
        /* A CAR UNDER THE EASE HOME IS NOT POSED FROM ITS OFFSET, and that was
         * the whole of `aiphys`'s "unattributed" sub-metre jumps. FUN_00503190
         * owns the pose while `blend_t` runs -- it is `blend_x` carried along the
         * recording and eased onto it, and `off` plays no part -- so re-posing
         * through ai_bump_apply here put the body back on `rec + off`, which is
         * the recording itself: every one of the four was a car 0.40 to 0.57 m
         * out on its ease, pushed by a field pair and landing on its line in one
         * tick. The push moves what the ease will read next instead, so the car
         * is displaced by the push and nothing else and the ease goes on from
         * there. DOWN IS REFUSED, for ai_bump_clamp's own reason: the ground is
         * under it and nothing on this path can see it. */
        if (a->blend_t > 1e-6f) {
            float mv[3];
            mv[0] = (float)(dv[0] * k2);
            mv[1] = (float)(dv[1] * k2);
            mv[2] = (float)(dv[2] * k2);
            if (mv[1] < 0.0f) mv[1] = 0.0f;
            for (k = 0; k < 3; k++) {
                a->blend_x[k] += mv[k];
                a->rb.body.x[k] += mv[k];
                taken[k] = mv[k];
            }
            rb_car_update_matrix(&a->rb);
            a->sim_push += (float)sqrt((double)mv[0] * mv[0]
                                     + (double)mv[1] * mv[1]
                                     + (double)mv[2] * mv[2]);
            return;
        }
        for (k = 0; k < 3; k++)
            a->off[k] += (float)(dv[k] * k2);
    }
    ai_bump_clamp(a);
    ai_bump_apply(a);
    {
        double mag = 0.0;
        for (k = 0; k < 3; k++) {
            taken[k] = a->off[k] - before[k];
            mag += (double)taken[k] * taken[k];
        }
        a->sim_push += (float)sqrt(mag);
    }
}

/* One pair of proxies, already gathered. -> nonzero if they were touching, and
 * `impact` (may be NULL) gets the hardest closing speed BEFORE any impulse,
 * which is what main.c raises car_cdt_car off.
 *
 * ==========================================================================
 * THE SPHERES DECIDE *WHETHER*. THE LINE OF CENTRES DECIDES *WHICH WAY*.
 * ==========================================================================
 *
 * This used to solve the deepest of up to 195 sphere pairs along THAT PAIR's own
 * normal, iterate a positional push over it eight times, and carry a cone test,
 * a clearance test, a lift bound and a vertical give-back to keep the result
 * physical. All of that was invented, and all of it was the wrong mechanism:
 * the engine does not resolve car-vs-car on the sphere set at all.
 *
 * The engine's collision registry (FUN_00534d00) registers two separate things
 * per actor, and the distinction is the whole of this:
 *
 *   FUN_00533130('$CAR', 0x4ef9e0)      the SPHERE PROVIDER -- carGatherCollSpheres.
 *                                       This is what answers "do they touch".
 *   FUN_00533170('$CAR', '$CAR',        the PAIRING: a filter (neither car is
 *                0x533940, 0x533990)    type 2, i.e. neither is the ghost) and a
 *                                       RESOLVER.
 *
 * and the resolver's geometry, FUN_00534be0(posA, posB, contact), is three lines:
 *
 *     contact.normal = normalise(posA - posB)        // the line between the two
 *                                                    // centres of mass
 *     contact.pointA =
 *     contact.pointB = (posA + posB) * 0.5           // the MIDPOINT, for both
 *
 * -- no radii, no penetration depth, no per-pair normals, and no positional
 * depenetration anywhere in FUN_00533990. It builds that ONE contact and hands
 * it to FUN_004f0730, the impulse solve. A retail car-vs-car contact is a single
 * push directly apart along the line of centres, so TWO CARS CANNOT INTERLOCK:
 * the only direction the response can ever have is the one that increases the
 * distance between their centres.
 *
 * WHY IT MATTERED HERE, measured rather than argued. Reported as "buggy still
 * have strange collision, player could stuck in it easily". The proxy is four
 * wheels and 3x3 body spheres of r 0.051 over a car up to 0.53 m long, so it is
 * mostly holes -- traps.md already records that a RAY passes between all
 * thirteen, and the contact solve has the same problem from the other side. The
 * largest sphere that fits inside a car's own extent box touching no proxy
 * sphere is 0.135 m on the Overkill, 0.150 on the Hummer and 0.184 ON THE BUGGY,
 * which is the biggest car carrying the smallest wheels (r 0.049). Swept over
 * 14,641 relative placements of two cars on flat ground, counting placements
 * where the two HULLS overlap and not one sphere pair touches:
 *
 *     Overkill vs Overkill   2465 overlapping     0 with no contact
 *     Buggy    vs Buggy      4601 overlapping  1072 with no contact (23%),
 *                                              worst 0.216 m of free interpenetration
 *     Hummer   vs Buggy      5329 overlapping   184 with no contact, worst 0.244 m
 *
 * With the deepest pair's normal as the response direction, a pair that IS
 * touching is being pushed along whichever small sphere happened to be deepest,
 * which on a proxy full of holes is frequently not a direction that separates
 * anything -- hence the cone test, the clearance test and the eight passes, each
 * of which exists to patch the consequences of the previous one. Along the line
 * of centres none of that arises.
 *
 * WHAT IS STILL THE PORT'S, and it is one thing rather than six: the positional
 * push. The engine has none because carSubstepCCD and carSubstepContact cap the
 * advance so the proxies barely overlap in the first place; this path has no
 * bisection, so something has to undo the overlap. It runs along the SAME line
 * of centres, which makes it monotone -- every pass strictly increases the
 * distance between the two centres -- so it converges instead of chasing a
 * different deepest pair each time, and AI_DEPEN_PASSES is measured again below.
 *
 * RETIRED WITH THE OLD DIRECTION, because each one existed only to manage it:
 * AI_TOP_COS's 46-degree cone in both halves, the proxies' vertical extents and
 * the clearance bound, the one-sided "the whole of the vertical goes to whichever
 * car is on top", the lower car's give-back, and AI_MAX_TOUCH's twelve-deep list.
 * THE RATCHET THEY WERE FOR CANNOT HAPPEN NOW: the line of centres is
 * ANTISYMMETRIC, so if A is above B then A is pushed up and B down by their mass
 * shares, and a grazing pair whose centres are level gets a push that is level
 * too. The one asymmetry left is that ai_bump_clamp will not drive a car more
 * than AI_BUMP_MAX_SINK into the ground, and the hand-over below refuses to turn
 * that into lift -- see the note there, which is the only place the vertical is
 * treated differently from the horizontal.
 */

/* The deepest overlapping sphere pair, and nothing else about it: the amount,
   not the direction. -> 0.0 when the two proxies are not touching at all. */
static float ai_deepest_pair(const float as[][4], int na,
                             const float bs[][4], int nb)
{
    double best = 0.0;
    int p, o;

    for (p = 0; p < na; p++)
        for (o = 0; o < nb; o++) {
            double ex = (double)as[p][0] - bs[o][0];
            double ey = (double)as[p][1] - bs[o][1];
            double ez = (double)as[p][2] - bs[o][2];
            double d2 = ex * ex + ey * ey + ez * ez;
            double sum = (double)as[p][3] + bs[o][3];
            double depth;
            if (d2 >= sum * sum || d2 < 1e-12)
                continue;
            depth = sum - sqrt(d2);
            if (depth > best)
                best = depth;
        }
    return (float)best;
}

/* HOW FAR THIS PROXY REACHES FROM ITS OWN CENTRE IN DIRECTION `d` -- the support
 * function of the sphere set, projected on one axis.
 *
 * THE GATE AND THE AMOUNT WANT DIFFERENT PROXIES, and that is traps.md's own
 * lesson arriving from the other side. "A proxy is fitted to the QUERY it was
 * built for": the sphere set is built for overlap, where every sphere is tested
 * against something with a radius of its own, and it is MOSTLY HOLES -- the
 * largest sphere that fits inside a car touching none of its own proxy is
 * 0.135 m on the Overkill, 0.150 on the Hummer and 0.184 on the Buggy, so every
 * wheel and body sphere in the game fits inside any of the three. The ray query
 * hit the same wall and traps.md records the answer: the enclosing sphere.
 *
 * So the fine set answers WHETHER the two cars are touching, which is what the
 * engine registers it for (FUN_00533130), and something coarser has to answer
 * HOW FAR IN THEY ARE -- which the deepest pair's depth cannot, because the pair
 * that happens to be deepest may be a wheel clipping a corner while the two
 * centres are 15 mm apart. Measured: part 8's 12 m run-up left the two centres
 * 0.015 m apart with 0.000 m of overlap to show for it, which is one car driven
 * clean through another.
 *
 * IT IS A PROJECTION AND NOT AN ENCLOSING SPHERE, and the difference is measured
 * too. An enclosing radius is the half-diagonal, so it reads two cars sitting
 * SIDE BY SIDE and not touching at all as a quarter of a metre overlapped, and
 * using it flung every brush apart -- a player driving at the field went from
 * 1.4 s of contact to 0.2 s. Projected on the contact normal there is no such
 * error: the extent along the normal is the car's width when they are abreast
 * and its length when they are nose to tail, which is what the depth along that
 * normal actually is. A projection of a set also has no holes, which is the
 * whole point.
 *
 * Nothing here fires unless the FINE set reports a touch and the loop stops the
 * moment it stops reporting one, so this only ever says how far to go next. */
static float ai_extent_along(const rb_car *c, const float s[][4], int n,
                             const float d[3])
{
    const float *x = c->body.x;
    double best = -1e30;
    int i;

    for (i = 0; i < n; i++) {
        double e = ((double)s[i][0] - x[0]) * d[0]
                 + ((double)s[i][1] - x[1]) * d[1]
                 + ((double)s[i][2] - x[2]) * d[2]
                 + s[i][3];
        if (e > best)
            best = e;
    }
    return best > 0.0 ? (float)best : 0.0f;
}

/* FUN_00534be0: the normal is the line between the two centres of mass, pointing
   out of B and toward A -- rb_coll_contact's own convention, so an A approaching
   B has a negative relative normal velocity -- and the point is the midpoint.
   -> 0 if the two centres coincide, which nothing downstream can be asked about.
   (The engine's own fallback there is a canned normal out of .data; this pair
   simply exchanges nothing, because a pair with no line between them has no
   direction the port could claim to have recovered.) */
static int ai_centre_contact(const ai_actor *A, const ai_actor *B,
                             float n[3], float p[3])
{
    const float *xa = A->car->body.x, *xb = B->car->body.x;
    double dx = (double)xa[0] - xb[0];
    double dy = (double)xa[1] - xb[1];
    double dz = (double)xa[2] - xb[2];
    double len = sqrt(dx * dx + dy * dy + dz * dz), inv;

    if (len < 1e-6)
        return 0;
    inv = 1.0 / len;
    n[0] = (float)(dx * inv);
    n[1] = (float)(dy * inv);
    n[2] = (float)(dz * inv);
    p[0] = (float)(((double)xa[0] + xb[0]) * 0.5);
    p[1] = (float)(((double)xa[1] + xb[1]) * 0.5);
    p[2] = (float)(((double)xa[2] + xb[2]) * 0.5);
    return 1;
}

/* ARE THESE TWO TOUCHING, and how far in are they? Fills `n` with the contact
 * normal (the line of centres) and `depth` with the amount to separate by.
 * -> 0 if they are apart.
 *
 * TWO QUESTIONS AND TWO PROXIES, because the fine set cannot answer the second
 * one and -- at speed -- cannot be trusted with the first either.
 *
 *  - A CONTACT is what the engine's own sphere set reports: any overlapping
 *    pair (FUN_005335a0 stops at the FIRST one it finds and never looks at the
 *    depth). Separated by that pair's own depth, which is small and gentle and
 *    is the ordinary case.
 *
 *  - AN INTERPENETRATION is one car's centre being inside the other's reach,
 *    and it is a different fault with a different amount. The proxy is mostly
 *    holes -- the largest sphere that fits inside a car touching none of its own
 *    thirteen is 0.135 m on the Overkill, 0.150 on the Hummer and 0.184 on the
 *    Buggy -- so a car arriving fast can put its nose in the middle of another
 *    one with NO sphere pair overlapping at all, and be neither detected nor
 *    pushed out. Measured: part 8's 12 m run-up at 6.74 m/s brought the two
 *    centres to 0.020 m with 0.000 m of overlap to show for it, which is one car
 *    driven clean through another. A car whose centre is inside another car IS
 *    touching it, whatever the sphere set says, and the separation it needs is
 *    the projected depth rather than some wheel's clipped corner.
 *
 * The engine has the same hole and does not need this, because carSubstepCCD
 * caps a car's advance at 0.9 of a sphere radius per substep so the fast case
 * never arises. There is no CCD between cars on this path.
 *
 * THE TEST IS THE CARS' OWN and not a tuned threshold: `ai_extent_along` is how
 * far each proxy reaches toward the other along the normal, so two cars merely
 * abreast sit at about ea + eb apart -- comfortably outside max(ea, eb) -- and
 * one that has driven into the middle of another is inside it. Using the
 * projected depth in BOTH regimes was measured and is wrong in the ordinary one:
 * it separates a brush to full projection clearance, and the ten-track survey
 * with no player went from 4.04 m of worst offset to 3.96 with every pair flung
 * apart before it could touch at all. */
static int ai_pair_touch(const ai_actor *A, const ai_actor *B,
                         const float as[][4], int na,
                         const float bs[][4], int nb,
                         float n[3], float *depth)
{
    float p[3], na_[3], ea, eb;
    double sep, dx, dy, dz;
    float fine;

    if (!ai_centre_contact(A, B, n, p))
        return 0;
    fine = ai_deepest_pair(as, na, bs, nb);

    na_[0] = -n[0]; na_[1] = -n[1]; na_[2] = -n[2];
    ea = ai_extent_along(A->car, as, na, na_);    /* A toward B */
    eb = ai_extent_along(B->car, bs, nb, n);      /* B toward A */
    dx = (double)A->car->body.x[0] - B->car->body.x[0];
    dy = (double)A->car->body.x[1] - B->car->body.x[1];
    dz = (double)A->car->body.x[2] - B->car->body.x[2];
    sep = sqrt(dx * dx + dy * dy + dz * dz);

    if (sep < (double)(ea > eb ? ea : eb)) {
        float gap;
        /* AND IN THIS REGIME THE NORMAL IS FLATTENED, which is the one place the
         * port departs from FUN_00534be0's full 3D line of centres -- and it is
         * this regime, which the engine does not have, that forces it.
         *
         * The line between two centres is a well-conditioned direction while the
         * cars are properly apart. Once one is INSIDE the other it is not: at
         * 0.10 m of separation a 0.05 m difference in ride height is 45% of the
         * normal, so the direction is dominated by whatever small residual is
         * left and the ejection goes UP rather than back. Measured on part 8's
         * 12 m run-up: the pair closed 0.196 -> 0.026 m over four ticks while the
         * push it was getting pointed increasingly out of the ground plane.
         *
         * Two cars that have driven into each other are separated ALONG THE
         * GROUND, because that is where they both are and what they both drive
         * on. A genuine vertical stack -- one car actually on top of another,
         * where the horizontal residual is degenerate -- keeps the 3D normal and
         * is pushed apart vertically, which is the only case the vertical is for.
         * The IMPULSE above is untouched by this and stays the engine's own. */
        double nh = sqrt((double)n[0] * n[0] + (double)n[2] * n[2]);
        if (nh > 1e-3) {
            n[0] = (float)((double)n[0] / nh);
            n[1] = 0.0f;
            n[2] = (float)((double)n[2] / nh);
            /* re-project the reaches onto the direction actually being used */
            na_[0] = -n[0]; na_[1] = 0.0f; na_[2] = -n[2];
            ea = ai_extent_along(A->car, as, na, na_);
            eb = ai_extent_along(B->car, bs, nb, n);
            sep = sqrt(dx * dx + dz * dz);
        }
        gap = (float)((double)ea + eb - sep);
        *depth = gap > fine ? gap : fine;
        return 1;                                  /* inside: eject */
    }
    if (fine > 0.0f) {
        *depth = fine;
        return 1;                                  /* a contact: its own depth */
    }
    return 0;
}

static int ai_pair_resolve(ai_actor *A, ai_actor *B,
                           const float as[][4], int na,
                           const float bs[][4], int nb,
                           float *impact)
{
    float n[3], p[3];
    int pass, k;
    double wa, wb, ima, imb;

    /* WHETHER, and WHICH WAY: ai_pair_touch carries both, and a pair that is not
       touching exchanges nothing -- which is what keeps this a byte-identical
       no-op at range. */
    {
        float d0;
        if (!ai_pair_touch(A, B, as, na, bs, nb, n, &d0))
            return 0;
    }

    /* THE VELOCITY HALF, and it still runs FIRST. Same ten passes, same 0.02
     * gate, same 0.05 m/s separation target as rb_coll_resolve, and the
     * denominator is still the PAIR's so the impulse delivers its dv across both
     * bodies rather than all of it into one. What is gone is the loop over
     * contacts: there is one contact now, which is what FUN_00533990 builds.
     *
     * The order still matters for the reason it always did -- `impact` is the
     * sound's, and it has to be the closing speed before anything has been
     * applied -- and it matters less than it did, because the positional half no
     * longer moves the bodies out from under a list of stale normals. There is
     * no list. */
    for (pass = 0; pass < AI_CONTACT_PASSES; pass++) {
        float va[3], vb[3], j[3];
        double vrel, dv, kd;

        /* Re-measured every pass: the impulses move both bodies, so the line
           between their centres is not the line it was. Cheap, and it is the
           only thing this solve is about. */
        if (!ai_centre_contact(A, B, n, p))
            break;
        ai_actor_point_vel(A, p, va);
        ai_actor_point_vel(B, p, vb);
        vrel = (double)(va[0] - vb[0]) * n[0]
             + (double)(va[1] - vb[1]) * n[1]
             + (double)(va[2] - vb[2]) * n[2];
        if (vrel > AI_CONTACT_VREL)
            break;
        if (pass == 0 && impact && -vrel > *impact)
            *impact = (float)-vrel;   /* the sound, before any impulse */
        dv = AI_CONTACT_SEP - vrel;
        if (dv < 0.0)
            dv = 0.0;
        kd = ai_actor_denom(A, p, n) + ai_actor_denom(B, p, n);
        if (kd < 1e-09)
            break;
        j[0] = (float)(n[0] * (dv / kd));
        j[1] = (float)(n[1] * (dv / kd));
        j[2] = (float)(n[2] * (dv / kd));
        ai_actor_impulse(A, p, j);
        j[0] = -j[0]; j[1] = -j[1]; j[2] = -j[2];
        ai_actor_impulse(B, p, j);
    }

    /* THE POSITIONAL HALF -- THE PORT'S, and the only part of this function that
     * the engine has no counterpart for. See the header: carSubstepCCD and
     * carSubstepContact keep the retail proxies from overlapping and there is no
     * bisection here, so the overlap the sphere set reports has to be undone.
     *
     * The AMOUNT is the deepest sphere pair's depth, re-measured each pass, which
     * is the only depth the proxy can report. The DIRECTION is the line of
     * centres, which is what makes this converge: each pass strictly increases
     * the distance between the two centres, so there is no different deepest pair
     * for the next pass to chase and no lift for a sustained graze to accumulate.
     * The old version needed eight passes and a cone, a clearance bound and a
     * give-back to stay physical; this needs the passes only because the depth
     * along a pair normal is not the depth along the line of centres, so one pass
     * removes most of it and the next re-measures. */
    ima = A->car->body.inv_mass;
    imb = B->ai ? B->car->body.inv_mass : 0.0;
    if (ima + imb < 1e-12) {
        wa = 1.0;
        wb = 0.0;
    } else {
        wa = ima / (ima + imb);
        wb = 1.0 - wa;
    }
    (void)wa;
    for (k = 0; k < AI_DEPEN_PASSES; k++) {
        float as2[RB_MAX_SPHERES][4], bs2[RB_MAX_SPHERES][4];
        float depth, sep[3], mv[3], took[3];
        int ga, gb, c;

        ga = rb_gather_spheres(A->car, as2);
        gb = rb_gather_spheres(B->car, bs2);
        /* Re-measured every pass, gate and amount together, so the loop stops
           the moment the pair is apart and never pushes on a pair that is. */
        if (!ai_pair_touch(A, B, as2, ga, bs2, gb, n, &depth))
            break;
        depth += RB_PENETRATION_SLACK;   /* the margin the world contact leaves */
        for (c = 0; c < 3; c++)
            sep[c] = n[c] * depth;       /* A relative to B */

        /* B's share, by mass. */
        for (c = 0; c < 3; c++)
            mv[c] = -sep[c] * (float)wb;
        ai_actor_move(B, mv, took);

        /* A's share, plus whatever B REFUSED -- horizontally. `took` is what B
         * actually did, so `sep + took` is A's own share when B came all the way
         * and the whole separation when B would not budge at all, which is the
         * rule that stopped a rammed opponent settling 11 cm inside the player.
         *
         * THE VERTICAL REFUSAL IS NOT HANDED OVER, and this is the one place the
         * two axes differ. ai_bump_clamp will not drive a car further than
         * AI_BUMP_MAX_SINK into the ground, because the ground is there and this
         * solve cannot see it -- so a downward share is routinely refused. Adding
         * that refusal to the other car's upward move is precisely the one-sided
         * vertical the old code had, and over a sustained graze it was a ratchet:
         * country_1's field reached +1.187 m of net upward push in twelve seconds
         * on cars a quarter of a metre tall. A car that cannot sink because it is
         * already on the ground is not a reason to put the other one in the air.
         * The horizontal has no such floor and hands over in full. */
        mv[0] = sep[0] + took[0];
        mv[1] = sep[1] * (float)wa;
        mv[2] = sep[2] + took[2];
        ai_actor_move(A, mv, took);
    }

    return 1;
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
    ai_car *a;
    if (!ai || i < 0 || i >= ai->n)
        return;
    a = &ai->car[i];
    /* THIS IS A CONTACT, so it arms the mode -- which is nine of FUN_004fd9d0's
       eleven call sites: `$CAR` against stone, people, the guard and the dog all
       reach it through the same `gmcdtOnCollision` switch the car-vs-car pairing
       does. An opponent knocked by a prop is as much off its line as one knocked
       by the player, and it recovers the same way. */
    ai_phys_start(a);
    if (a->phys_mode)
        rb_apply_impulse(&a->rb, point, j);   /* a real body takes it for real */
    else
        ai_take_impulse(a, point, j);
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
    int i, j, sweep;
    /* HOW MANY TIMES THE PAIR LIST IS SWEPT, and it is not a tuning number: the
     * pairs are solved one at a time, in place, so this is Gauss-Seidel over a
     * contact GRAPH, and Gauss-Seidel needs about as many sweeps as the graph is
     * wide. With three opponents the graph is three pairs and one sweep reaches
     * everything; with five it is ten pairs and a correction made on the first
     * pair is undone by the last one before the tick ends.
     *
     * Raising AI_MAX_FIELD to the layout's five (ai.h) is what showed it: the
     * ten-track survey's worst overlap went from 0.037 m to 0.061 m, past a
     * Buggy's own wheel sphere (0.049 m), which is the anchor aitest's five-
     * centimetre bound is set from.
     *
     * `n - 1' is the DIAMETER of the worst graph n cars can form -- a chain --
     * and therefore how many sweeps it takes a correction at one end to reach
     * the other. Measured on the ten-track survey at five opponents: one sweep
     * leaves 0.061 m, three leave 0.037, four (n-1) leave 0.036, and ten leave
     * 0.042 while distorting the deciding-against-not comparison, so more is
     * not better. It costs nothing on the field this had before -- three
     * opponents is two sweeps of a three-pair list. */
    int sweeps = ai->n - 1;
    if (sweeps < 1) sweeps = 1;
    if (sweeps > AI_FIELD_SWEEPS_MAX) sweeps = AI_FIELD_SWEEPS_MAX;

    for (sweep = 0; sweep < sweeps; sweep++)
    for (i = 0; i < ai->n; i++) {
        for (j = i + 1; j < ai->n; j++) {
            ai_actor A, B;
            int na, nb;

            /* NOT BETWEEN TWO REMOTE CARS. Each of them is authoritative on
               its own machine and is posed here from the wire, so a contact
               solved locally is a nudge the next packet will contradict --
               and the far side never saw it, so the two machines disagree
               about where both cars are. The PLAYER against a remote car is a
               different question and ai_collide_player still asks it: that
               contact has to move the player, who really is simulated here. */
            if (ai->car[i].remote && ai->car[j].remote)
                continue;
            if (!ai_near(&ai->car[i].rb, &ai->car[j].rb))
                continue;
            na = rb_gather_spheres(&ai->car[i].rb, as);
            nb = rb_gather_spheres(&ai->car[j].rb, bs);
            if (na <= 0 || nb <= 0)
                continue;
            /* A SIMULATED CAR IS HANDED THROUGH AS A PLAIN BODY (`ai = NULL`),
               which is not a special case but the literal truth: in physics mode
               it is an rb_car the integrator owns, so a contact belongs in its
               own momentum and its own position exactly as the player's does.
               The offset path is for a car that is still walking a recording. */
            A.car = &ai->car[i].rb;
            A.ai  = ai->car[i].phys_mode ? NULL : &ai->car[i];
            A.sim = ai->car[i].phys_mode ? &ai->car[i] : NULL;
            B.car = &ai->car[j].rb;
            B.ai  = ai->car[j].phys_mode ? NULL : &ai->car[j];
            B.sim = ai->car[j].phys_mode ? &ai->car[j] : NULL;
            {
                float hit = 0.0f;
                if (ai_pair_resolve(&A, &B, as, na, bs, nb, &hit) > 0) {
                    /* FUN_00533990 arms BOTH cars of the pair -- 0x533a82 and
                       0x533a9b, two calls, one per side -- but only a real
                       IMPACT arms; a brush between two cars holding station
                       merely holds. See AI_PHYS_MIN_HIT. */
                    if (hit > AI_PHYS_MIN_HIT) {
                        ai_phys_start(&ai->car[i]);
                        ai_phys_start(&ai->car[j]);
                    } else {
                        ai_phys_touch(&ai->car[i]);
                        ai_phys_touch(&ai->car[j]);
                    }
                }
            }
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
    /* THE PLAYER'S PROXY REACH, cached for ai_steer_decide -- which is handed a
       position rather than a car and would otherwise have to guess how much room
       a pass round the player wants. Free here: the spheres are already
       gathered. See ai_t.player_reach. */
    {
        double best = 0.0;
        for (i = 0; i < np; i++) {
            double dx = (double)ps[i][0] - player->body.x[0];
            double dy = (double)ps[i][1] - player->body.x[1];
            double dz = (double)ps[i][2] - player->body.x[2];
            double r = sqrt(dx * dx + dy * dy + dz * dz) + ps[i][3];
            if (r > best) best = r;
        }
        ai->player_reach = (float)best;
    }

    for (i = 0; i < ai->n; i++) {
        ai_car *a = &ai->car[i];
        ai_actor A, B;
        int no;

        if (!ai_near(player, &a->rb))
            continue;                       /* broad phase, see ai.h */
        no = rb_gather_spheres(&a->rb, os);
        if (no <= 0)
            continue;
        A.car = player;  A.ai = NULL;  A.sim = NULL;   /* the player has no budget */
        B.car = &a->rb;  B.ai = a;  B.sim = NULL;
        /* ARM BEFORE RESOLVING, WHICH IS THE ENGINE'S OWN ORDER. `FUN_00533990`
         * calls `FUN_004fd9d0` on both cars at 0x533a82 and 0x533a9b, gets the
         * line of centres at 0x533c83, and only then hands the contact to
         * `FUN_004f0730` at 0x533d55 -- so by the time the impulse is computed
         * BOTH bodies are real.
         *
         * The port armed afterwards, so the FIRST contact -- the hardest one,
         * and the one the player feels -- was solved with the opponent still
         * kinematic: its share went into the offset and through
         * `ai_actor_denom`'s yaw-only form instead of the full rigid-body
         * denominator. Handing `offv` to the body afterwards recovers the
         * momentum but not the solve. One extra touch test per pair per tick
         * buys the engine's ordering. */
        {
            float n0[3], d0;
            if (ai_pair_touch(&A, &B, ps, np, os, no, n0, &d0)) {
                ai_phys_start(a);
                if (a->phys_mode) { B.ai = NULL; B.sim = a; }
            }
        }
        if (ai_pair_resolve(&A, &B, ps, np, os, no, &impact) > 0) {
            /* THE CONTACT ARMS THE ENGINE'S PHYSICS MODE -- FUN_004fd9d0, the
               writer of phys+0x4398 this project spent a year believing did not
               exist. From the next tick the opponent is a real car steering
               itself back to its line, and it is why nothing here has to invent
               a way for a replay to be pushed. */
            /* A PLAYER CONTACT ALWAYS ARMS. The impact gate is for
             * opponent-vs-opponent brushing, where two cars holding station
             * touch all lap at no closing speed; it has no business filtering
             * the player, and it was doing exactly that.
             *
             * The reason is the proxy: `ai-opponents.md` measures the largest
             * sphere that fits INSIDE a car touching none of its own thirteen at
             * 0.135 m on the Overkill, **0.150 on the Hummer and 0.184 on the
             * Buggy** -- so a real hit on the two biggest cars routinely reports
             * a depth and a closing speed far under any threshold, or none at
             * all. Gating on it is why "hummer and buggy not react on touch at
             * all". If the player touched it, that is a hit. */
            ai_phys_start(a);
            (void)ai_phys_touch;
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

/* Fit one car's loop. -> 1 and fills `g` (n_mk fractions, re-based on marker 0,
 * strictly increasing) when the recording really does pass every marker.
 *
 * ONE PASS over the loop, carrying the running distance rather than an array of
 * them: the longest profile is 11 081 samples and the alternative is 88 KB of
 * scratch on a machine with none to spare. */
static int ai_fit_one(const ai_car *a, const float (*mk)[3], int n_mk,
                      float *g, float *at_out)
{
    double best2[AI_MAX_CP], at[AI_MAX_CP];
    double run = 0.0;
    int i, k, first;

    if (n_mk <= 0 || n_mk > AI_MAX_CP || a->n <= 1 || !(a->lap_len > 1e-3f))
        return 0;
    for (k = 0; k < n_mk; k++) { best2[k] = 1e30; at[k] = 0.0; }

    first = a->cycle_start > 0 ? a->cycle_start : 0;
    for (i = first; i < a->n; i++) {
        if (i > first) {
            double dx = a->s[i].p[0] - a->s[i-1].p[0];
            double dy = a->s[i].p[1] - a->s[i-1].p[1];
            double dz = a->s[i].p[2] - a->s[i-1].p[2];
            run += sqrt(dx * dx + dy * dy + dz * dz);
        }
        for (k = 0; k < n_mk; k++) {
            /* In XZ, for the reason every other query on this road is: urban_1
               and urban_2 run a deck over another part of their own road, and a
               car under one is not at the checkpoint above it -- but nor is it
               ever within a few metres of it in the plane. */
            double dx = a->s[i].p[0] - mk[k][0];
            double dz = a->s[i].p[2] - mk[k][2];
            double d2 = dx * dx + dz * dz;
            if (d2 < best2[k]) { best2[k] = d2; at[k] = run; }
        }
    }

    for (k = 0; k < n_mk; k++)
        if (best2[k] > (double)AI_CP_FIT_NEAR * AI_CP_FIT_NEAR)
            return 0;                   /* the lap does not go through it */

    for (k = 0; k < n_mk; k++) {
        double f = (at[k] - at[0]) / (double)a->lap_len;
        while (f < 0.0) f += 1.0;
        while (f >= 1.0) f -= 1.0;
        g[k] = (float)f;
    }
    g[0] = 0.f;
    /* STRICTLY INCREASING, which is the check that the markers were met in the
       spine's own order and that none of them landed on the wrong side of the
       seam. A fit that fails it is not repaired, it is dropped. */
    for (k = 1; k < n_mk; k++)
        if (!(g[k] > g[k-1]))
            return 0;
    /* AND THE ARCS THEMSELVES, unwrapped and unaveraged, for ai_fit_line: `run`
       started at the loop's first sample, so `at` is already this loop's own arc
       and needs no conversion. */
    if (at_out)
        for (k = 0; k < n_mk; k++)
            at_out[k] = (float)at[k];
    return 1;
}

int ai_cp_fractions(const ai_t *ai, const float (*mk)[3], int n_mk,
                    float *frac, float *lap_len_out)
{
    double sum[AI_MAX_CP], laps = 0.0;
    int i, k, used = 0;

    if (!ai || !mk || !frac || n_mk <= 0 || n_mk > AI_MAX_CP)
        return 0;
    for (k = 0; k < n_mk; k++) sum[k] = 0.0;

    for (i = 0; i < ai->n; i++) {
        float g[AI_MAX_CP];
        if (!ai_fit_one(&ai->car[i], mk, n_mk, g, NULL))
            continue;
        for (k = 0; k < n_mk; k++) sum[k] += g[k];
        laps += ai->car[i].lap_len;
        used++;
    }
    if (used <= 0)
        return 0;

    for (k = 0; k < n_mk; k++) frac[k] = (float)(sum[k] / used);
    frac[0] = 0.f;
    /* The MEAN of separate drives can only break the ordering if two stations are
       closer together than the drives disagree, and then the table is worthless
       anyway -- so it is checked again rather than assumed. */
    for (k = 1; k < n_mk; k++)
        if (!(frac[k] > frac[k-1]))
            return 0;

    if (lap_len_out) *lap_len_out = (float)(laps / used);
    rlog("ai: checkpoint stations fitted off %d of %d recording(s), lap %.1f m\n",
         used, ai->n, (float)(laps / used));
    return 1;
}

/* THE ROAD, off the first recording that fits. See ai.h at ai_line.
 *
 * THE FIRST, not the best or the mean: the window this feeds has to be laid out
 * on ONE car's line, and the three drives of a track disagree with each other by
 * 6.5 m on where the checkpoints fall (ai.h). Averaging the stations is right --
 * they are the answer's units -- and averaging the LINE would be a line nobody
 * drove. The stations stay ai_cp_fractions', the window is this car's.
 */
int ai_fit_line(const ai_t *ai, const float (*mk)[3], int n_mk, ai_line *L)
{
    float g[AI_MAX_CP], at[AI_MAX_CP];
    const ai_car *a = NULL;
    int i, k, first, n;
    double run;

    if (!ai || !mk || !L || n_mk <= 0 || n_mk > AI_MAX_CP)
        return 0;
    memset(L, 0, sizeof(*L));
    for (i = 0; i < ai->n; i++)
        if (ai_fit_one(&ai->car[i], mk, n_mk, g, at)) {
            a = &ai->car[i];
            L->from = i;
            break;
        }
    if (!a)
        return 0;

    first = a->cycle_start > 0 ? a->cycle_start : 0;
    n = a->n - first;
    if (n < 2)
        return 0;
    L->pt  = (float (*)[2])malloc(sizeof(float) * 2 * (size_t)n);
    L->cum = (float *)malloc(sizeof(float) * (size_t)n);
    if (!L->pt || !L->cum) {
        ai_line_free(L);
        return 0;
    }
    L->n = n;
    run = 0.0;
    for (i = 0; i < n; i++) {
        const float *p = a->s[first + i].p;
        if (i > 0) {
            const float *q = a->s[first + i - 1].p;
            run += sqrt((double)(p[0]-q[0]) * (p[0]-q[0])
                      + (double)(p[1]-q[1]) * (p[1]-q[1])
                      + (double)(p[2]-q[2]) * (p[2]-q[2]));
        }
        L->pt[i][0] = p[0];
        L->pt[i][1] = p[2];
        L->cum[i] = (float)run;
    }
    /* THE CLOSING LEG, so the loop is a loop. The replay rejoins at cycle_start
       and the last sample is within 0.3 m of it on all thirty recordings, so
       this is a rounding error and not a jump -- but it is measured rather than
       assumed, because `len` is what one lap of the answer divides by. */
    {
        float dx = L->pt[0][0] - L->pt[n-1][0];
        float dz = L->pt[0][1] - L->pt[n-1][1];
        L->len = L->cum[n-1] + (float)sqrt((double)dx * dx + (double)dz * dz);
    }
    if (!(L->len > 1e-3f)) {
        ai_line_free(L);
        return 0;
    }
    L->n_cp = n_mk;
    for (k = 0; k < n_mk; k++) {
        float v = at[k];
        while (v < 0.f) v += L->len;
        while (v >= L->len) v -= L->len;
        L->at[k] = v;
    }
    rlog("ai: the road is %s's lap -- %d samples, %.1f m, %.2f m apart\n",
         a->name, L->n, L->len, L->len / (float)L->n);
    return 1;
}

void ai_line_free(ai_line *L)
{
    if (!L)
        return;
    if (L->pt)  free(L->pt);
    if (L->cum) free(L->cum);
    memset(L, 0, sizeof(*L));
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
