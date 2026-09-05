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

/* DID THIS SHOVE KILL IT? The player's own two tests, on an opponent -- see
 * ai.h, "dying". -> nonzero if the car was put back on its line.
 *
 * Runs on the composed pose, so it sees where the car actually IS, and only on a
 * car a shove has actually moved: an opponent on its line is its recording, and
 * a recording is a lap that was really driven, so there is nowhere better to
 * send it and declaring it dead would fire every tick for the rest of the race.
 */
static int ai_bump_death(ai_car *a, float dt)
{
    const rb_world *w = a->rb.world;
    const rb_body *b = &a->rb.body;
    float gap;
    int dead = 0;

    if (a->bump < AI_DEATH_MIN_OFF) {
        a->buried_for = 0.0f;
        return 0;                      /* on its line: nothing put it anywhere */
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
    /* BURIED -- the ground is ABOVE the car. The player has no counterpart to
     * this because a player drives on the surface and cannot be pushed into it;
     * an opponent is placed by an offset and a terrain follow, and where the
     * follow cannot rescue it the car ends up inside a bank. aitest part 9 case
     * 6 counts them: 14 of 480 hard shoves leave a car deeper in the level than
     * an unshoved twin, and nothing else in the model ever gets them out.
     *
     * One ground probe, not a proxy sweep: the question is only whether the
     * drivable surface at the car's own column is over its head, and the bound
     * is the car's own body half-height -- the same extent the wall clearance
     * uses. It is gated behind the offset test above, so a car on its line never
     * pays for it. */
    if (w && w->ground) {
        float gy, n[3];
        float half = 0.5f * RB_CARS[a->car].extent[1];
        if (w->ground(w->ctx, b->x[0], b->x[2], b->x[1] + AI_BUMP_CEIL, &gy, n)
            && gy > b->x[1] + half) {
            a->buried_for += dt;
            /* AND IT HAS TO STICK. A graze against a bank is not being stuck --
               see ai.h, and the one-in-five respawn rate that testing this on
               the instant produced. */
            if (!dead && a->bump_w > 1e-4f
                && a->buried_for > AI_BURIED_SETTLE / a->bump_w) {
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
    ai_bump_apply(a);                  /* rebuild the pose from the recording */
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
 * wrapping at the same `cycle_start` ai_advance wraps at, so the point ahead of
 * a car about to close its lap is on the lap and not off the end of the array.
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
            c = a->cycle_start > 0 ? a->cycle_start : 1;
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

void ai_step(ai_t *ai, const ai_track *tr, float px, float py, float pz,
             int player_lap, float dt)
{
    float pdist = 0.0f;
    int pcp = 0, i;
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

    if (tr && tr->spine)
        have_spine = tr->spine(tr->ctx, px, py, pz, ai->player_at, &pdist, &pcp);
    /* THE PLAYER'S PROGRESS, off the LATCHED checkpoint index rather than off a
     * projection -- see ai_track.lap_progress. The projection stays bound for
     * `player_cp`, which is the gap term the rubber band's own curve indexes and
     * which a few metres of ambiguity does not disturb. */
    if (tr && tr->lap_progress) {
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
    (void)player_lap;

    for (i = 0; i < ai->n; i++) {
        ai_car *a = &ai->car[i];
        float target, adist = 0.0f;
        float x0[3], q0[4];
        int acp = 0, gap = 0;

        /* A REMOTE PLAYER IS POSED, NEVER STEPPED. It has no recording to walk,
           no spine progress of its own worth rubber-banding and nothing this
           machine is entitled to decide about it -- see ai.h. Belt and braces:
           a network race builds the field with ai_remote_init and the app does
           not call this at all, so this is the second of two gates and exists
           so that adding a third caller cannot start simulating somebody
           else's car. */
        if (a->remote)
            continue;

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
            a->cursor = a->cycle_start > 0 ? a->cycle_start : 1;
            a->u = 0.0f;
        }

        ai_pose(a);
        ai_diff_velocity(a, x0, q0, dt);
        a->speed = (float)sqrt((double)a->rb.body.v[0] * a->rb.body.v[0]
                               + (double)a->rb.body.v[1] * a->rb.body.v[1]
                               + (double)a->rb.body.v[2] * a->rb.body.v[2]);
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
        /* AND LAST, THE SAME QUESTION THE PLAYER IS ASKED: did that leave the car
           somewhere it would have died? On the composed pose, because that is
           where the car is. See ai_bump_death. */
        ai_bump_death(a, dt);
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
    int p, o, nt = 0, shallow = 0;

    /* THE LIST IS THE DEEPEST `max` PAIRS, NOT THE FIRST `max`. Two proxies are
     * 13 spheres each -- 15 on the Hummer -- so up to 195 pairs can overlap at
     * once, and the loop below enumerates them wheels-first. Stopping at the
     * first `max` therefore filled the list with WHEEL pairs and dropped the
     * body ones, and since the positional half works on the deepest entry it
     * was depenetrating a pair that was not the worst one. Measured on
     * country_1's field: the truncated list understated the true overlap by up
     * to 8 mm on the ticks where more than `max` pairs were touching.
     *
     * `shallow` tracks the current weakest entry so a full list costs one
     * comparison per new pair rather than a rescan. */
    for (p = 0; p < na; p++) {
        for (o = 0; o < nb; o++) {
            double ex = (double)as[p][0] - bs[o][0];
            double ey = (double)as[p][1] - bs[o][1];
            double ez = (double)as[p][2] - bs[o][2];
            double d2 = ex * ex + ey * ey + ez * ez;
            double sum = (double)as[p][3] + bs[o][3];
            double len, inv, depth;
            int at;

            if (d2 >= sum * sum || d2 < 1e-12)
                continue;
            len = sqrt(d2);
            depth = sum - len;
            if (nt < max) {
                at = nt++;
            } else if ((double)t[shallow].depth < depth) {
                at = shallow;
            } else {
                continue;
            }
            inv = 1.0 / len;
            t[at].normal[0] = (float)(ex * inv);
            t[at].normal[1] = (float)(ey * inv);
            t[at].normal[2] = (float)(ez * inv);
            /* The contact point on B's surface, which is what both lever arms
               are measured from. */
            t[at].point[0] = (float)(bs[o][0] + t[at].normal[0] * bs[o][3]);
            t[at].point[1] = (float)(bs[o][1] + t[at].normal[1] * bs[o][3]);
            t[at].point[2] = (float)(bs[o][2] + t[at].normal[2] * bs[o][3]);
            t[at].depth = (float)depth;
            if (nt == max) {
                int c;
                shallow = 0;
                for (c = 1; c < nt; c++)
                    if (t[c].depth < t[shallow].depth)
                        shallow = c;
            }
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
        for (k = 0; k < 3; k++) {
            b->car->body.x[k] += dv[k];
            taken[k] = dv[k];
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

    /* THE VELOCITY HALF RUNS FIRST, ON THE CONTACTS THAT ARE ACTUALLY THERE.
     *
     * It used to run last, over the SAME `t` the positional half had just spent
     * up to AI_DEPEN_PASSES moving both bodies out of: by the time the impulses
     * were applied their points and normals described geometry that no longer
     * existed, and the solve drove stale contacts to +0.05 m/s of separation
     * anyway. That is energy injected along a direction nothing is touching in,
     * which is the "jelly" half of the reported feel.
     *
     * Re-gathering after the push instead of reordering does not work, and the
     * reason is RB_PENETRATION_SLACK: the positional half leaves the pair one
     * millimetre APART, so a re-gather finds no contact at all and the pair
     * would exchange no impulse whatever -- two cars passing through each other
     * with a shove that never happened. Velocities are what the moment of
     * contact is about and positions are the cleanup, so the order is the one
     * that keeps both honest.
     *
     * `impact` is the sound's, and this is now genuinely the pre-solve closing
     * speed rather than the speed left after eight positional passes. */
    /* THE VELOCITY HALF. Same ten passes, same 0.02 gate, same 0.05 m/s target
     * as rb_coll_resolve; the denominator is the PAIR's, so the impulse delivers
     * its dv across both bodies rather than all of it into one. */
    for (pass = 0; pass < AI_CONTACT_PASSES; pass++) {
        int any = 0;
        for (i = 0; i < nt; i++) {
            float va[3], vb[3], j[3], sn[3];
            double vrel, dv, k;

            /* THE DIRECTION THE CONTACT IS SOLVED ALONG, which is the contact
             * normal for a ride-over and the normal FLATTENED for a graze --
             * AI_TOP_COS, the same 46-degree floor cone the positional half
             * asks the same question with.
             *
             * This one is a momentum LEAK and not merely a lift. An impulse is
             * equal and opposite, so the vertical shares cancel between the two
             * cars -- but ai_take_impulse ends in ai_bump_clamp, and a car
             * already on AI_BUMP_MAX_SINK's one-centimetre floor has its
             * downward share DELETED there. Ten Gauss-Seidel passes a tick,
             * each throwing away one side of a cancelling pair, and what is left
             * is a pair of cars with net upward momentum that neither of them
             * was given. Measured on country_1 before this: BOTH cars of a
             * grinding pair went from 0.06 m to 0.27 m of lift in three ticks,
             * together, which the positional half cannot do at all -- it lifts
             * one car, never two.
             *
             * Flattening costs nothing a graze should have: the pair is beside
             * itself on the ground, the ground is what holds both cars up, and
             * this solve cannot see it. A real ride-over keeps the full normal
             * and its full vertical response. */
            if (fabs((double)t[i].normal[1]) >= AI_TOP_COS) {
                sn[0] = t[i].normal[0];
                sn[1] = t[i].normal[1];
                sn[2] = t[i].normal[2];
            } else {
                double nh = sqrt((double)t[i].normal[0] * t[i].normal[0]
                               + (double)t[i].normal[2] * t[i].normal[2]);
                if (nh < 1e-4)
                    continue;
                sn[0] = (float)((double)t[i].normal[0] / nh);
                sn[1] = 0.0f;
                sn[2] = (float)((double)t[i].normal[2] / nh);
            }

            ai_actor_point_vel(A, t[i].point, va);
            ai_actor_point_vel(B, t[i].point, vb);
            vrel = (double)(va[0] - vb[0]) * sn[0]
                 + (double)(va[1] - vb[1]) * sn[1]
                 + (double)(va[2] - vb[2]) * sn[2];
            if (vrel > AI_CONTACT_VREL)
                continue;
            if (pass == 0 && impact && -vrel > *impact)
                *impact = (float)-vrel;   /* the sound, before any impulse */
            any = 1;
            dv = AI_CONTACT_SEP - vrel;
            if (dv < 0.0)
                dv = 0.0;
            k = ai_actor_denom(A, t[i].point, sn)
              + ai_actor_denom(B, t[i].point, sn);
            if (k < 1e-09)
                continue;
            j[0] = (float)(sn[0] * (dv / k));
            j[1] = (float)(sn[1] * (dv / k));
            j[2] = (float)(sn[2] * (dv / k));
            ai_actor_impulse(A, t[i].point, j);
            j[0] = -j[0]; j[1] = -j[1]; j[2] = -j[2];
            ai_actor_impulse(B, t[i].point, j);
        }
        if (!any)
            break;
    }

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
        const ai_touch *cur;
        int n2, k, deep2, ga, gb;

        (void)deep;
        for (k = 0; k < AI_DEPEN_PASSES; k++) {
            float d, sep[3], mv[3], took[3], a_bot, b_bot, a_top, b_top;
            int c;

            /* Re-gathered on EVERY pass, the first one included. It used to
               reuse the caller's arrays for k == 0, which saved one gather and
               cost the pass its sphere EXTENTS -- and the clearance test below
               needs them on the pass that does most of the work. */
            ga = rb_gather_spheres(A->car, as2);
            gb = rb_gather_spheres(B->car, bs2);
            n2 = ai_touch_list(as2, ga, bs2, gb, t2, AI_MAX_TOUCH);
            if (n2 <= 0)
                break;
            cur = t2;
            deep2 = 0;
            for (c = 1; c < n2; c++)
                if (t2[c].depth > t2[deep2].depth)
                    deep2 = c;
            if (cur[deep2].depth <= 0.0f)
                break;
            d = cur[deep2].depth + RB_PENETRATION_SLACK;
            for (c = 0; c < 3; c++)
                sep[c] = cur[deep2].normal[c] * d;   /* A relative to B */

            /* The two proxies' vertical extents, for the clearance test the
               lift is bounded by. */
            a_bot = b_bot = 1e30f;
            a_top = b_top = -1e30f;
            for (c = 0; c < ga; c++) {
                if (as2[c][1] - as2[c][3] < a_bot) a_bot = as2[c][1] - as2[c][3];
                if (as2[c][1] + as2[c][3] > a_top) a_top = as2[c][1] + as2[c][3];
            }
            for (c = 0; c < gb; c++) {
                if (bs2[c][1] - bs2[c][3] < b_bot) b_bot = bs2[c][1] - bs2[c][3];
                if (bs2[c][1] + bs2[c][3] > b_top) b_top = bs2[c][1] + bs2[c][3];
            }

            /* IS ONE OF THEM ACTUALLY ON TOP OF THE OTHER? Only then is any of
             * this separation vertical.
             *
             * The threshold is the engine's own and not a new number: 46 degrees
             * from up is what carDriveForce (0x4ee8fc, contact.c) uses to decide
             * a face is something a car stands on rather than something it is
             * up against. Same question here -- a normal inside that cone is a
             * car riding over another car, and one outside it is two cars beside
             * each other whose contact happens to have a little Y in it.
             *
             * WHY IT MATTERS, and it is the whole of the reported "opponents
             * climb": the vertical push is one-sided. It goes entirely to the
             * upper car and AI_BUMP_MAX_SINK caps the lower one at a centimetre,
             * so nothing ever pushes a car back DOWN, while the return spring
             * only pulls at bump_accel (7 m/s^2 -- 0.35 mm a tick against a push
             * of 5 to 10 mm). Two opponents grinding along nearly-parallel
             * recorded lines therefore RATCHET upward, a fraction of a
             * millimetre of Y per pass, eight passes a tick, for as long as the
             * lines overlap. country_1's field flew: two cars at off[1] = 0.596
             * and 0.618 m at t = 8.5 s, the whole of their bump budget spent
             * straight up, on cars 0.25 m tall.
             *
             * A grazing pair is separated ALONG THE GROUND instead, in the
             * normal's own horizontal direction, and nothing is lifted. That is
             * also what should happen physically: two cars side by side on the
             * dirt slide apart, they do not climb. */
            {
                float nh = (float)sqrt((double)cur[deep2].normal[0]
                                           * cur[deep2].normal[0]
                                     + (double)cur[deep2].normal[2]
                                           * cur[deep2].normal[2]);
                if (fabs((double)cur[deep2].normal[1]) < AI_TOP_COS
                    && nh > 1e-4f) {
                    /* Re-aim the whole of `d` into the horizontal plane. nh is
                       at least sqrt(1 - AI_TOP_COS^2) = 0.72 here, so the
                       rescale is bounded by 1.39 and a pass still removes most
                       of the depth; what is left is what the next pass is for. */
                    sep[0] = cur[deep2].normal[0] * (d / nh);
                    sep[1] = 0.0f;
                    sep[2] = cur[deep2].normal[2] * (d / nh);
                }
            }

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
            /* AND IT STOPS THE MOMENT THE CAR IS CLEAR. A lift is for getting
             * one car up off another; once the lifted car's lowest sphere is
             * above the other car's highest one there is nothing left under it
             * to climb, and every further millimetre is a car in the air.
             *
             * Without this bound the lift does not converge, because the pair is
             * in SUSTAINED contact: the return spring pulls the car back down
             * into the other one, the next tick's eight passes lift it again,
             * and each pass lifts by the deepest pair's full share while the
             * thirteen-sphere proxies keep offering a different deepest pair.
             * country_1's field reached 0.389 m of lift on cars 0.25 m tall,
             * both of a pair at once. Bounded, the same fixture peaks at the
             * clearance and comes back down.
             *
             * Cutting the vertical out altogether is NOT the fix and the harness
             * says so: aitest's Buggy-into-Hummer ram fails three ways without
             * it (0.084 m of overlap left standing, and the pair still inside
             * each other when it ends) -- see ai.h on the vertical. */
            if (sep[1] != 0.0f) {
                ai_actor *up   = sep[1] > 0.0f ? A : B;
                ai_actor *down = sep[1] > 0.0f ? B : A;
                float room = sep[1] > 0.0f ? b_top - a_bot + RB_PENETRATION_SLACK
                                           : a_top - b_bot + RB_PENETRATION_SLACK;
                float need = sep[1] > 0.0f ? sep[1] : -sep[1];

                if (need > room)
                    need = room;
                /* IT COMES OUT OF THE LOWER CAR'S OWN LIFT FIRST, and that is
                 * what makes the vertical zero-sum while either car has any to
                 * give back.
                 *
                 * The branch only ever moves a car UP -- whichever of the two is
                 * on top, by however much the deepest pair is inside. Over a
                 * sustained graze the deepest pair's normal flips between the
                 * two cars from tick to tick, so BOTH of them collect upward
                 * pushes and neither ever collects a downward one, and the pair
                 * rises TOGETHER. Rising together also defeats the clearance
                 * bound above, since a_bot and b_top climb at the same rate.
                 * Measured on country_1's field: +1.187 m of net upward push in
                 * twelve seconds, against an impulse half that nets to exactly
                 * zero -- so this branch was the whole of it.
                 *
                 * Taking it from the lower car's own offset first means a pair
                 * that is merely grinding trades height instead of gaining it,
                 * and only a car standing on one that is ITSELF down on the
                 * ground -- a real ride-over, which is what the vertical is for
                 * -- adds any. */
                if (need > 0.0f && down->ai && down->ai->off[1] > 0.0f) {
                    float give = down->ai->off[1] < need ? down->ai->off[1]
                                                         : need;
                    mv[0] = mv[2] = 0.0f;
                    mv[1] = -give;
                    ai_actor_move(down, mv, took);
                    need += took[1];              /* took[1] <= 0 */
                    if (need < 0.0f)
                        need = 0.0f;
                }
                if (need > 0.0f) {
                    mv[0] = mv[2] = 0.0f;
                    mv[1] = need;
                    ai_actor_move(up, mv, took);
                }
            }
        }
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
