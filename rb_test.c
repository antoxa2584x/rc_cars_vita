/* Host harness for the transcribed RC Cars physics core.
 *
 * Part 1 checks the rigid-body core in isolation against closed-form answers.
 * Part 2 checks the tire/engine/contact layer piece by piece.
 * Part 3 drives a car on flat ground and checks the emergent behaviour.
 *
 * Part 3 runs the REAL transcribed stack: rb_car_update_suspension solves each
 * spring length against the world and rb_collide produces the per-wheel
 * contacts, exactly as carPhysTick does. The only scaffolding left is the world
 * itself -- a flat one-sided plane at y=0 standing in for the level geometry.
 *
 *   gcc -I. -O2 -fno-fast-math -ffp-contract=off \
 *       rb_test.c rb.c contact.c collide.c -lm
 */
#include "rb.h"
#include "cam.h"
#include "rb_data.h"
#include "rbcar.h"
#include "carani.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
static const rb_world FLAT_WORLD;   /* defined below */

static void ck(int ok, const char *what)
{
    if (!ok) { printf("  FAIL: %s\n", what); fails++; }
}

/* Overkill, from the recovered constants -- see rccars_re/PHYSICS.md */
static const rb_curve_pt OVERKILL_ACCEL[] = {
    {  0.0f, 6.0f }, { 16.0f, 3.0f }, { 32.0f, 0.0f },
};
static const rb_curve_pt OVERKILL_RESTRICT[] = {
    {  0.0f, 2.0f }, { 15.0f, 0.0f },
};

static void tune_overkill(rb_tuning *t)
{
    int i_;
    memset(t, 0, sizeof(*t));
    t->coeff_moment_ox         = 1.045f;
    t->coeff_moment_oz         = 1.869f;
    t->coeff_friction_bearings = 0.202f;
    t->coeff_air_resistance    = 0.0323f;
    t->coeff_front_tires       = 0.70f;
    t->coeff_rear_tires        = 0.70f;
    t->angle_drift_on          = 1.0f;    /* angleDriftOn, raw 1 */
    t->speed_drift_on          = 1.0f;
    t->coeff_drift_rear        = 0.218f;
    t->coeff_deep_sand         = 0.0606f;
    t->coeff_water             = 0.192f;
    /* UPGRADES.ini [TIRES] OverkillUpgrades */
    t->tire_upgrade[0] = 0.90f; t->tire_upgrade[1] = 1.02f;
    t->tire_upgrade[2] = 1.10f; t->tire_upgrade[3] = 1.20f;
    t->speed_base_max  = 27.0f;           /* km/h */
    t->speed_boost_max = 35.0f;
    t->boost_ratio     = 35.0f / 27.0f;
    /* UPGRADES.ini [RESONATORS] OverkillUpgrades / _SPL_OY */
    t->resonator_speed[0] = 0.92f; t->resonator_speed[1] = 0.97f;
    t->resonator_speed[2] = 1.07f; t->resonator_speed[3] = 1.12f;
    t->resonator_accel[0] = 0.70f; t->resonator_accel[1] = 0.80f;
    t->resonator_accel[2] = 0.90f; t->resonator_accel[3] = 1.00f;
    /* cdt, Overkill. The loader scales the CdtDelta/CdtRadUp keys by 0.1. */
    t->cdt_rad_wheel = 0.07182f;   /* CdtRadWheel raw 69 */
    t->cdt_rad_back  = 0.07182f;   /* coeffRadBackWheels 1.0 */
    t->cdt_front_x   = 0.0f;
    t->cdt_side_x    = 0.05f;      /* shiftRoofBaseLefter */
    for (i_ = 0; i_ < 3; i_++) {
        t->body_sphere[i_].offset[0] = 0.0f;
        t->body_sphere[i_].offset[1] = 0.3879f;   /* CdtDeltaY * 0.1 */
        t->body_sphere[i_].offset[2] = 0.3758f * (float)(1 - i_); /* front/mid/rear */
        t->body_sphere[i_].radius    = 0.0510f;   /* CdtRadUp * 0.1 */
        t->body_sphere[i_].central_z = 0.0f;
    }
    t->accel.pt = OVERKILL_ACCEL;
    t->accel.n  = (int)(sizeof(OVERKILL_ACCEL) / sizeof(rb_curve_pt));
    t->restrict_.pt = OVERKILL_RESTRICT;
    t->restrict_.n  = (int)(sizeof(OVERKILL_RESTRICT) / sizeof(rb_curve_pt));
}

static void body_init(rb_car *c, float mass, float ex, float ey, float ez)
{
    int i;
    memset(c, 0, sizeof(*c));
    c->body.mass = mass;
    c->body.inv_mass = 1.0f / mass;
    {
        float ix = mass * (ey * ey + ez * ez) / 12.0f;
        float iy = mass * (ex * ex + ez * ez) / 12.0f;
        float iz = mass * (ex * ex + ey * ey) / 12.0f;
        for (i = 0; i < 16; i++) c->body.ibody_inv[i] = 0.0f;
        c->body.ibody_inv[0]  = 1.0f / ix;
        c->body.ibody_inv[5]  = 1.0f / iy;
        c->body.ibody_inv[10] = 1.0f / iz;
        c->body.ibody_inv[15] = 1.0f;
    }
    c->body.q[0] = 1.0f;
    c->max_contacts = 4;
    rb_car_update_matrix(c);
    rb_update_inv_inertia_world(&c->body);
}

/* The car exactly as the app builds it: rbcar_init from rb_data.h. Keeping one
   source of truth matters -- an earlier version of this harness built its own
   car with invented geometry, and every grip result it produced was wrong. */
static void car_init(rb_car *c)
{
    rbcar_init(c, 0, &FLAT_WORLD, 0.0f, 0.0f, 0.0f, 0.0f);
}

/* ---- the world: a flat plane at y = 0 ------------------------------------ */
/* Two-sided, because col_sphere is: closest-point-on-triangle finds the surface
   from either side, and the substep search depends on that -- a probe that has
   already stepped past the surface must still report a hit or the bisection has
   nothing to bracket. */
/* A plane through the origin. TILT_N is its normal, world up unless a test
   changes it. */
static float TILT_N[3] = { 0.0f, 1.0f, 0.0f };

/* An optional second surface: a vertical wall whose solid side is x >= WALL_X.
   Disabled at the sentinel. It exists so the reset's clearance loop can be tested
   somewhere one pass is NOT enough -- pushed clear of the floor, the car is still
   inside the wall. On a bare plane the loop finishes in one pass and its iteration
   bound is unobservable, which let a 10 -> 1 mutant survive. */
static float WALL_X = 1e30f;

static int w_sphere(void *ctx, const float centre[3], float radius,
                    rb_world_hit *hits, int max_hits, int *n_hits)
{
    double d, dw;
    (void)ctx; (void)max_hits;
    *n_hits = 0;
    d = (double)centre[0] * TILT_N[0] + (double)centre[1] * TILT_N[1]
        + (double)centre[2] * TILT_N[2];
    dw = (WALL_X < 1e29f) ? ((double)WALL_X - centre[0]) : 1e30;
    /* whichever of the two is nearer, and only if it is within reach */
    if (fabs(dw) < fabs(d) && fabs(dw) <= radius) {
        hits[0].point[0] = WALL_X;
        hits[0].point[1] = centre[1];
        hits[0].point[2] = centre[2];
        hits[0].surface  = 0;
        *n_hits = 1;
        return 1;
    }
    if (fabs(d) > radius)
        return 0;
    hits[0].point[0] = (float)(centre[0] - d * TILT_N[0]);
    hits[0].point[1] = (float)(centre[1] - d * TILT_N[1]);
    hits[0].point[2] = (float)(centre[2] - d * TILT_N[2]);
    hits[0].surface  = 0;
    *n_hits = 1;
    return 1;
}

static int w_segment(void *ctx, const float a[3], const float b[3])
{
    double da = (double)a[0]*TILT_N[0] + (double)a[1]*TILT_N[1] + (double)a[2]*TILT_N[2];
    double db = (double)b[0]*TILT_N[0] + (double)b[1]*TILT_N[1] + (double)b[2]*TILT_N[2];
    (void)ctx;
    return (da > 0.0) != (db > 0.0);
}

static int w_ground(void *ctx, float x, float z, float ceil_y,
                    float *y, float n[3])
{
    /* The harness plane is single-valued, so the ceiling can only reject -- it
       never picks between surfaces the way the real .col grid does. */
    (void)ctx; (void)ceil_y;
    if (fabsf(TILT_N[1]) < 1e-6f) return 0;
    *y = -(TILT_N[0] * x + TILT_N[2] * z) / TILT_N[1];
    n[0] = TILT_N[0]; n[1] = TILT_N[1]; n[2] = TILT_N[2];
    return 1;
}

/* A water plane at WATER_Y, or no water at all while it sits at the sentinel.
   Same answer shape as col.c's cb_water: `gap` is how far the wheel CENTRE is
   above the surface, so the submerged depth is `radius - gap`. */
static float WATER_Y = -1e30f;
/* Tilt on the water surface, dy/dx. Physically silly for water; the point is that
   the real grid's height varies with position, so the probe's answer must depend
   on WHERE it was asked -- see the per-wheel check in part 6b. */
static float WATER_SLOPE_X = 0.0f;

static int w_water(void *ctx, int wheel, const float centre[3], float *gap)
{
    (void)ctx; (void)wheel;
    if (WATER_Y <= -1e29f)
        return 0;
    if (gap)
        *gap = (float)((double)centre[1] - WATER_Y
                       - (double)WATER_SLOPE_X * centre[0]);
    return 1;
}

static const rb_world FLAT_WORLD = { w_sphere, w_segment, w_water, w_ground, 0 };

/* rb_car_tick now owns the whole per-substep sequence -- suspension solve,
   contact gather, integrate -- so the host only has to supply the world. */
static void step_car(rb_car *c, float dt)
{
    rb_car_tick(c, dt);
}


/* ---- the REAL GL view matrix, built exactly as main.c does ---------------
 *
 * An earlier version of this test computed the view direction from an assumed
 * formula and reported a 0.028 degree aim error for a camera that actually put
 * the car BEHIND itself in a turn. Never assume the renderer's convention --
 * construct Rx(pitch) . Ry(-yaw) . T(-eye) and measure. */
static void gl_ident(float *m) { memset(m, 0, 16 * sizeof(float));
                                 m[0] = m[5] = m[10] = m[15] = 1.f; }
static void gl_mul(float *o, const float *a, const float *b)
{
    float t[16];
    int r, c2, k;
    for (c2 = 0; c2 < 4; c2++) for (r = 0; r < 4; r++) {
        float sum = 0.f;
        for (k = 0; k < 4; k++) sum += a[k*4+r] * b[c2*4+k];
        t[c2*4+r] = sum;
    }
    memcpy(o, t, 16 * sizeof(float));
}
static void gl_view(float *V, float yaw_deg, float pitch_deg,
                    float ex, float ey, float ez)
{
    const float D = 0.017453292f;
    float A[16], B[16], C[16];
    float cp = cosf(pitch_deg * D), sp = sinf(pitch_deg * D);
    float cy = cosf(-yaw_deg * D), sy = sinf(-yaw_deg * D);
    gl_ident(A); A[5] = cp; A[9] = -sp; A[6] = sp; A[10] = cp;   /* glRotatef X */
    gl_ident(B); B[0] = cy; B[8] = sy;  B[2] = -sy; B[10] = cy;  /* glRotatef Y */
    gl_ident(C); C[12] = -ex; C[13] = -ey; C[14] = -ez;          /* glTranslatef */
    gl_ident(V); gl_mul(V, A, B); gl_mul(V, V, C);
}
static void gl_xform(float *o, const float *m, const float *p)
{
    int r;
    for (r = 0; r < 3; r++)
        o[r] = m[0*4+r]*p[0] + m[1*4+r]*p[1] + m[2*4+r]*p[2] + m[3*4+r];
}

static float ride_height(const rb_car *c)
{
    return c->wheel[0].radius + c->wheel[0].len - c->wheel[0].mount[1];
}

static float speed_of(const rb_car *c)
{
    return sqrtf(c->body.v[0] * c->body.v[0] + c->body.v[1] * c->body.v[1]
                 + c->body.v[2] * c->body.v[2]);
}

/* --------------------------------------------------------------------------
 * The car rig (carani.c / carAniProc1 0x00504820).
 *
 * Driven off the REAL exported rig in assets/car1.vsc, not a synthetic one --
 * every interesting property here (which local axis is the axle, which way the
 * axles are mirrored, the 1.05 scale baked into the node chain) lives in that
 * data and a hand-built rig would prove nothing. Skipped with a note if the
 * asset is not there, so the harness still runs from anywhere.
 * -------------------------------------------------------------------------- */

/* model-space image of a point given in a part's own rest frame, after animation */
static float dist3(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* The length of basis row `row` of wheel `w`'s REST matrix -- the car's own
   model scale, which lives on an ancestor and which nothing in the rig scales. */
static float rest_row(const carani_t *r, int w, int row)
{
    const float *m = r->part[r->wheel[w]].rest + row * 4;
    return sqrtf(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
}

static void rig_pt(const carani_t *r, int p, float x, float y, float z, float o[3])
{
    const float *m = r->world[p];
    o[0] = x*m[0] + y*m[4] + z*m[8]  + m[12];
    o[1] = x*m[1] + y*m[5] + z*m[9]  + m[13];
    o[2] = x*m[2] + y*m[6] + z*m[10] + m[14];
}

/* Accepts VSC4 and up, not VSC4 alone.
 *
 * This used to demand the magic be exactly "VSC4", and the cars have been packed
 * as VSC5 and then VSC6 since markers and lightmaps were added -- so every check
 * below it has been quietly SKIPPING, printing its "not VSC4" note into a wall of
 * passing output, for as long as the format has been current. The rig has had a
 * real bug the whole time (the left-hand springs aimed up into the body, from the
 * 0x540B Euler order) and nothing here could see it.
 *
 * A skip that reads like a pass is the same failure as the no-op stubs in
 * vis_test: the harness has to be able to tell you it did nothing. So the version
 * fields are parsed properly, and rig_checks now treats a load failure as a
 * FAILED check rather than a quiet return. */
static int rig_load(carani_t *r, const char *path)
{
    FILE *f = fopen(path, "rb");
    char magic[4];
    unsigned int ntex, nb, npart, nmark, i;
    int ver;
    if (!f)
        return 0;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "VSC", 3)) {
        fclose(f);
        return 0;
    }
    ver = magic[3] - '0';
    if (ver < 4) {                     /* no rig in VSC3 */
        fclose(f);
        return 0;
    }
    if (fread(&ntex, 4, 1, f) != 1 || fread(&nb, 4, 1, f) != 1
        || fread(&npart, 4, 1, f) != 1) {
        fclose(f);
        return 0;
    }
    if (ver >= 5) {                    /* marker count and the shadow radius */
        float shadow_r;
        if (fread(&nmark, 4, 1, f) != 1 || fread(&shadow_r, 4, 1, f) != 1) {
            fclose(f);
            return 0;
        }
    }
    for (i = 0; i < ntex; i++) {          /* skip the pixel data */
        unsigned short nlen, w, h;
        unsigned char fmt, mips, l;
        if (fread(&nlen, 2, 1, f) != 1) { fclose(f); return 0; }
        fseek(f, nlen, SEEK_CUR);
        if (fread(&w, 2, 1, f) != 1 || fread(&h, 2, 1, f) != 1
            || fread(&fmt, 1, 1, f) != 1 || fread(&mips, 1, 1, f) != 1) {
            fclose(f); return 0;
        }
        for (l = 0; l < mips; l++) {
            unsigned int lw = w >> l, lh = h >> l;
            if (!lw) lw = 1;
            if (!lh) lh = 1;
            fseek(f, (long)lw * lh * (fmt == 1 ? 4 : 2), SEEK_CUR);
        }
    }
    carani_read_parts(r, f, npart);
    fclose(f);
    return r->n > 0;
}

static void rig_checks(void)
{
    static carani_t rig;
    rb_car c;
    rb_world w;
    int i, L, R;

    puts("\n-- car rig --");
    if (!rig_load(&rig, "assets/car1.vsc")) {
        ck(0, "assets/car1.vsc loads a rig (run from rccars_vita/, and repack "
              "with pack_vsc.py --rig)");
        return;
    }
    memset(&w, 0, sizeof w);
    rbcar_init(&c, 0, &w, 0.f, 0.f, 0.f, 0.f);
    carani_bind(&rig, &c);

    printf("%d parts; wheels -> %s %s %s %s\n", rig.n,
           rig.wheel[0] >= 0 ? rig.part[rig.wheel[0]].name : "?",
           rig.wheel[1] >= 0 ? rig.part[rig.wheel[1]].name : "?",
           rig.wheel[2] >= 0 ? rig.part[rig.wheel[2]].name : "?",
           rig.wheel[3] >= 0 ? rig.part[rig.wheel[3]].name : "?");
    ck(rig.wheel[0] >= 0 && rig.wheel[1] >= 0 && rig.wheel[2] >= 0
       && rig.wheel[3] >= 0, "all four wheels bound");
    ck(rig.support[0] >= 0 && rig.support[1] >= 0, "both steering knuckles bound");
    ck(rig.axle_front >= 0 && rig.axle_rear >= 0, "both axles bound");
    ck(rig.n_springs == 8, "eight springs bound");

    L = rig.pair_front[0];
    R = rig.pair_front[1];
    ck(L >= 0 && R >= 0 && c.wheel[L].mount[0] > 0.f && c.wheel[R].mount[0] < 0.f,
       "front pair resolved left(+X)/right(-X) by geometry, not by index");

    /* 1. everything at rest draws exactly where it was baked -- everything
          except the WHEELS, which carry the tyre table's own axle scale from
          the moment the upgrades are applied and are 0.95 wide at rest, not
          1.0. That is checked here rather than excused: a wheel's draw matrix
          at rest has to be a pure scale by carani_tire_width along its own
          axle, and the identity across it. */
    {
        float worst = 0.f, w_worst = 0.f;
        int wi = 0, k;
        c.steer = 0.f;
        c.tire_upgrade = 0;
        for (i = 0; i < 4; i++) c.wheel[i].spin = 0.f;
        carani_update(&rig, &c);
        for (i = 0; i < rig.n; i++) {
            int is_wheel = 0;
            for (k = 0; k < 4; k++)
                if (rig.wheel[k] == i) is_wheel = 1;
            if (is_wheel)
                continue;
            for (k = 0; k < 16; k++) {
                float e = fabsf(rig.draw[i][k] - ((k % 5 == 0) ? 1.f : 0.f));
                if (e > worst) { worst = e; wi = i; }
            }
        }
        printf("at rest: worst draw matrix deviation from identity %.2e (%s)\n",
               worst, rig.part[wi].name);
        ck(worst < 1e-5f,
           "an unmoved rig draws as the identity, the tyres apart");
        {
            float want = carani_tire_width(&c);
            for (i = 0; i < 4; i++) {
                float hub[3], ax[3], tr[3], e;
                if (rig.wheel[i] < 0) continue;
                rig_pt(&rig, rig.wheel[i], 0.f, 0.f, 0.f, hub);
                rig_pt(&rig, rig.wheel[i], 0.f, 0.f, 1.f, ax);
                rig_pt(&rig, rig.wheel[i], 0.07f, 0.f, 0.f, tr);
                /* world[] carries the car's own model scale as well -- 1.05
                   on the Overkill, and on an ANCESTOR, which is exactly what
                   the engine does not touch -- so each probe is measured
                   against that node's rest row, not against 1. */
                e = fabsf(dist3(ax, hub) - want * rest_row(&rig, i, 2));
                if (e > w_worst) w_worst = e;
                e = fabsf(dist3(tr, hub) - 0.07f * rest_row(&rig, i, 0));
                if (e > w_worst) w_worst = e;
            }
            printf("at rest: tyre axle span %.4f (want %.4f), worst error "
                   "%.2e\n", (double)want, (double)want, (double)w_worst);
            ck(w_worst < 1e-5f,
               "and a resting tyre is exactly the tuning's width across its "
               "axle, and unchanged around it");
        }
    }

    /* 2. steering: the drawn wheel must turn by exactly the body steer angle,
          and only the front pair may move at all */
    {
        float ax0[3], hub[3], tip[3], d[3], prev[3];
        float n, ang, worst_err = 0.f, rear_move = 0.f;
        float st;
        c.steer = 0.f;
        carani_update(&rig, &c);
        rig_pt(&rig, rig.wheel[L], 0.f, 0.f, 0.f, hub);
        rig_pt(&rig, rig.wheel[L], 0.f, 0.f, 1.f, tip);
        prev[0] = tip[0]-hub[0]; prev[1] = tip[1]-hub[1]; prev[2] = tip[2]-hub[2];
        n = sqrtf(prev[0]*prev[0]+prev[1]*prev[1]+prev[2]*prev[2]);
        prev[0]/=n; prev[1]/=n; prev[2]/=n;
        rig_pt(&rig, rig.wheel[2], 0.f, 0.f, 1.f, ax0);

        for (st = -30.f; st <= 30.5f; st += 10.f) {
            float got;
            c.steer = st;
            carani_update(&rig, &c);
            rig_pt(&rig, rig.wheel[L], 0.f, 0.f, 0.f, hub);
            rig_pt(&rig, rig.wheel[L], 0.f, 0.f, 1.f, tip);
            d[0] = tip[0]-hub[0]; d[1] = tip[1]-hub[1]; d[2] = tip[2]-hub[2];
            n = sqrtf(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
            d[0]/=n; d[1]/=n; d[2]/=n;
            /* signed angle about +Y between the rest axle and this one */
            ang = atan2f(prev[2]*d[0] - prev[0]*d[2],
                         prev[0]*d[0] + prev[2]*d[2]) * RB_RAD2DEG;
            got = fabsf(fabsf(ang) - fabsf(st));
            if (got > worst_err) worst_err = got;
            {
                float t[3];
                rig_pt(&rig, rig.wheel[2], 0.f, 0.f, 1.f, t);
                n = fabsf(t[0]-ax0[0]) + fabsf(t[1]-ax0[1]) + fabsf(t[2]-ax0[2]);
                if (n > rear_move) rear_move = n;
            }
        }
        printf("steer -30..+30: worst |drawn angle| - |body angle| = %.3f deg; "
               "rear axle moved %.2e m\n", worst_err, rear_move);
        ck(worst_err < 0.01f, "drawn steer angle equals the body steer angle");
        ck(rear_move < 1e-6f, "the rear wheels do not steer");
        c.steer = 0.f;
    }

    /* 3. rolling: a tread point orbits the hub, the axle direction does not move */
    {
        float hub[3], p0[3], p1[3], ax0[3], ax1[3];
        float r0, r1, dot;
        for (i = 0; i < 4; i++) c.wheel[i].spin = 0.f;
        carani_update(&rig, &c);
        rig_pt(&rig, rig.wheel[0], 0.f, 0.f, 0.f, hub);
        rig_pt(&rig, rig.wheel[0], 0.07f, 0.f, 0.f, p0);
        rig_pt(&rig, rig.wheel[0], 0.f, 0.f, 1.f, ax0);
        for (i = 0; i < 4; i++) c.wheel[i].spin = 1.5707963f;
        carani_update(&rig, &c);
        rig_pt(&rig, rig.wheel[0], 0.07f, 0.f, 0.f, p1);
        rig_pt(&rig, rig.wheel[0], 0.f, 0.f, 1.f, ax1);
        r0 = sqrtf((p0[0]-hub[0])*(p0[0]-hub[0]) + (p0[1]-hub[1])*(p0[1]-hub[1])
                 + (p0[2]-hub[2])*(p0[2]-hub[2]));
        r1 = sqrtf((p1[0]-hub[0])*(p1[0]-hub[0]) + (p1[1]-hub[1])*(p1[1]-hub[1])
                 + (p1[2]-hub[2])*(p1[2]-hub[2]));
        dot = ((p0[0]-hub[0])*(p1[0]-hub[0]) + (p0[1]-hub[1])*(p1[1]-hub[1])
             + (p0[2]-hub[2])*(p1[2]-hub[2])) / (r0 * r1);
        if (dot < -1.f) dot = -1.f;
        if (dot > 1.f) dot = 1.f;
        printf("spin 90 deg: tread radius %.4f -> %.4f, swept %.2f deg, "
               "axle moved %.2e m\n", r0, r1, acosf(dot) * RB_RAD2DEG,
               fabsf(ax0[0]-ax1[0]) + fabsf(ax0[1]-ax1[1]) + fabsf(ax0[2]-ax1[2]));
        ck(fabsf(r1 - r0) < 1e-5f, "spinning preserves the tread radius");
        ck(fabsf(acosf(dot) * RB_RAD2DEG - 90.f) < 0.01f,
           "a quarter turn of spin sweeps 90 degrees");
        ck(fabsf(ax0[0]-ax1[0]) + fabsf(ax0[1]-ax1[1]) + fabsf(ax0[2]-ax1[2])
           < 1e-6f, "spinning does not move the axle direction");
        for (i = 0; i < 4; i++) c.wheel[i].spin = 0.f;
    }

    /* 4. suspension: the axle must roll TOWARDS the wheel that dropped.
          Getting this backwards is the whole reason the left/right pairing is
          derived from the mounts instead of taken from the wheel index. */
    {
        float lo[3], ro[3], l1[3], r1v[3], dl, dr;
        for (i = 0; i < 4; i++)
            c.wheel[i].len = c.wheel[i].len_free - c.wheel[i].sag;
        carani_update(&rig, &c);
        /* probe the two ends of the front axle, in MODEL space */
        rig_pt(&rig, rig.axle_front, 0.f, 0.f,  0.10f, lo);
        rig_pt(&rig, rig.axle_front, 0.f, 0.f, -0.10f, ro);
        c.wheel[L].len += 0.05f;                       /* drop the LEFT wheel */
        carani_update(&rig, &c);
        rig_pt(&rig, rig.axle_front, 0.f, 0.f,  0.10f, l1);
        rig_pt(&rig, rig.axle_front, 0.f, 0.f, -0.10f, r1v);
        dl = (lo[0] > 0.f) ? l1[1]-lo[1] : r1v[1]-ro[1];    /* +X end = LEFT */
        dr = (lo[0] > 0.f) ? r1v[1]-ro[1] : l1[1]-lo[1];
        printf("front axle, left wheel +50 mm droop: +X end dy=%+.4f, "
               "-X end dy=%+.4f\n", dl, dr);
        ck(dl < -0.005f && dr > 0.005f,
           "the axle rolls towards the wheel that dropped");
        c.wheel[L].len -= 0.05f;
    }

    /* 5. both wheels together is pure pitch: the axle rotates about its own
          line, so nothing moves along that line */
    {
        float p0[3], p1[3];
        for (i = 0; i < 4; i++)
            c.wheel[i].len = c.wheel[i].len_free - c.wheel[i].sag;
        carani_update(&rig, &c);
        rig_pt(&rig, rig.axle_front, 0.10f, 0.f, 0.f, p0);
        c.wheel[L].len += 0.05f;
        c.wheel[R].len += 0.05f;
        carani_update(&rig, &c);
        rig_pt(&rig, rig.axle_front, 0.10f, 0.f, 0.f, p1);
        printf("front axle, both wheels +50 mm droop: arm end moved "
               "(%+.4f %+.4f %+.4f)\n", p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2]);
        ck(fabsf(p1[0]-p0[0]) < 1e-5f, "even droop pitches the axle, no roll");
        ck(fabsf(p1[1]-p0[1]) > 0.01f, "even droop actually moves the axle");
        c.wheel[L].len -= 0.05f;
        c.wheel[R].len -= 0.05f;
    }

    /* 6. springs stretch to reach their axle and keep their body-side origin */
    {
        int s = rig.spring[0];
        float o0[3], e0[3], o1[3], e1[3], l0, l1;
        for (i = 0; i < 4; i++)
            c.wheel[i].len = c.wheel[i].len_free - c.wheel[i].sag;
        carani_update(&rig, &c);
        rig_pt(&rig, s, 0.f, 0.f, 0.f, o0);
        rig_pt(&rig, s, 0.f, 0.f, 1.f, e0);
        l0 = sqrtf((e0[0]-o0[0])*(e0[0]-o0[0]) + (e0[1]-o0[1])*(e0[1]-o0[1])
                 + (e0[2]-o0[2])*(e0[2]-o0[2]));
        for (i = 0; i < 4; i++) c.wheel[i].len += 0.06f;
        carani_update(&rig, &c);
        rig_pt(&rig, s, 0.f, 0.f, 0.f, o1);
        rig_pt(&rig, s, 0.f, 0.f, 1.f, e1);
        l1 = sqrtf((e1[0]-o1[0])*(e1[0]-o1[0]) + (e1[1]-o1[1])*(e1[1]-o1[1])
                 + (e1[2]-o1[2])*(e1[2]-o1[2]));
        printf("%s: length %.4f -> %.4f (%.0f%%), origin moved %.2e m\n",
               rig.part[s].name, l0, l1, 100.f * l1 / l0,
               fabsf(o1[0]-o0[0]) + fabsf(o1[1]-o0[1]) + fabsf(o1[2]-o0[2]));
        ck(l1 > l0 * 1.02f, "the springs stretch as the suspension droops");
        ck(fabsf(o1[0]-o0[0]) + fabsf(o1[1]-o0[1]) + fabsf(o1[2]-o0[2]) < 1e-6f,
           "the springs stay anchored to the body");
        for (i = 0; i < 4; i++) c.wheel[i].len -= 0.06f;
    }

    /* 7. the spin integrator: a wheel rolling on the ground at v must turn at
          v/r, and it must keep turning through a jump */
    {
        rb_car t;
        float want;
        rbcar_init(&t, 0, &w, 0.f, 0.f, 0.f, 0.f);
        t.body.v[2] = 3.0f;                       /* 3 m/s along body +Z */
        for (i = 0; i < 4; i++) t.hit[i].active = 1;
        for (i = 0; i < 120; i++) rb_wheel_spin_update(&t, 1.f / 60.f);
        want = 3.0f / t.wheel[0].radius;
        printf("rolling at 3 m/s: wheel rate %.2f rad/s (v/r = %.2f)\n",
               t.wheel[0].spin_w, want);
        ck(fabsf(t.wheel[0].spin_w - want) < 0.5f, "wheel rate settles at v/r");
        ck(t.wheel[0].spin >= -RB_TWO_PI && t.wheel[0].spin <= RB_TWO_PI,
           "the spin angle stays wrapped");

        for (i = 0; i < 4; i++) t.hit[i].active = 0;
        for (i = 0; i < 30; i++) rb_wheel_spin_update(&t, 1.f / 60.f);
        printf("airborne for 0.5 s: wheel rate %.2f rad/s (was %.2f)\n",
               t.wheel[0].spin_w, want);
        ck(t.wheel[0].spin_w > want * 0.8f,
           "an airborne wheel keeps spinning (5 rad/s^2, not 200)");

        /* and reverse must wrap the other way rather than sticking */
        t.body.v[2] = -3.0f;
        for (i = 0; i < 4; i++) t.hit[i].active = 1;
        for (i = 0; i < 600; i++) rb_wheel_spin_update(&t, 1.f / 60.f);
        printf("reversing for 10 s: angle %.3f rad, rate %.2f rad/s\n",
               t.wheel[0].spin, t.wheel[0].spin_w);
        ck(t.wheel[0].spin_w < 0.f, "the wheels turn backwards in reverse");
        ck(t.wheel[0].spin >= -RB_TWO_PI && t.wheel[0].spin <= RB_TWO_PI,
           "reverse wraps instead of sticking at the low end");
    }

    /* 8. the tuning's tyre width (carani_tire_width; see carani.h -- the
          engine's own table at 0x005738c8, applied to the wheel nodes by
          FUN_0050be40).

          The four numbers are NOT asserted against themselves here. What is
          asserted is what they cannot move -- that the scale reaches the wheel
          node along its AXLE and nowhere else, that the tread radius is
          untouched, and that no part which is not a wheel feels it at all
          (which is also what pins the wheel nodes as leaves) -- plus the one
          consequence that separates the table from what the port used to
          derive: a STOCK tyre is drawn narrower than it was modelled. */
    {
        float wid[4], hub0[3], hub1[3], ax0[3], ax1[3], tr0[3], tr1[3];
        float axle0[3], spr0[3], kn0[3], axle1[3], spr1[3], kn1[3];
        float z0, z1, r0, r1, still, mono = 1.f;
        int lv;

        for (i = 0; i < 4; i++)
            c.wheel[i].len = c.wheel[i].len_free - c.wheel[i].sag;
        c.steer = 0.f;
        for (i = 0; i < 4; i++) c.wheel[i].spin = 0.f;

        printf("tyre width by tuning level:");
        for (lv = 0; lv < 4; lv++) {
            c.tire_upgrade = lv;
            wid[lv] = carani_tire_width(&c);
            printf(" %.4f", (double)wid[lv]);
            if (lv && wid[lv] <= wid[lv - 1])
                mono = 0.f;
        }
        putchar('\n');
        /* 0.95, not 1.0. The engine sets the node's axle row to an absolute
           length and stock is the shortest of the four, so a stock tyre is
           drawn NARROWER than the mesh was modelled -- which is what the port
           got wrong, and it got it wrong in the direction that made the mark
           under it look narrow too. */
        ck(wid[0] < 1.f && wid[0] > 0.5f,
           "a stock tyre is drawn narrower than it was modelled");
        ck(mono != 0.f, "each tuning level fits a wider tyre than the last");
        /* A band, not the value: under 1.05 of stock nobody could see the
           progression, and at 2.0 the Overkill's 78 mm tyre would be wider than
           its own 140 mm diameter. */
        ck(wid[3] / wid[0] > 1.05f && wid[3] / wid[0] < 2.0f,
           "the top tyre is visibly but not absurdly wider than stock");

        c.tire_upgrade = 0;
        carani_update(&rig, &c);
        rig_pt(&rig, rig.wheel[0], 0.f,   0.f, 0.f, hub0);
        rig_pt(&rig, rig.wheel[0], 0.f,   0.f, 1.f, ax0);
        rig_pt(&rig, rig.wheel[0], 0.07f, 0.f, 0.f, tr0);
        /* OFF the origin, on all three axes. A scale premultiplied the way this
           one is leaves a node's origin exactly where it was -- that is the
           point of scaling the basis rows -- so probing origins cannot see one
           at all, and a mutant that leaked the width onto the front axle
           survived the whole battery until these three probes moved off it. */
        rig_pt(&rig, rig.axle_front, 0.05f, 0.05f, 0.10f, axle0);
        rig_pt(&rig, rig.spring[0],  0.05f, 0.05f, 0.10f, spr0);
        rig_pt(&rig, rig.support[0], 0.05f, 0.05f, 0.10f, kn0);

        c.tire_upgrade = 3;
        carani_update(&rig, &c);
        rig_pt(&rig, rig.wheel[0], 0.f,   0.f, 0.f, hub1);
        rig_pt(&rig, rig.wheel[0], 0.f,   0.f, 1.f, ax1);
        rig_pt(&rig, rig.wheel[0], 0.07f, 0.f, 0.f, tr1);
        rig_pt(&rig, rig.axle_front, 0.05f, 0.05f, 0.10f, axle1);
        rig_pt(&rig, rig.spring[0],  0.05f, 0.05f, 0.10f, spr1);
        rig_pt(&rig, rig.support[0], 0.05f, 0.05f, 0.10f, kn1);

        z0 = dist3(ax0, hub0);
        z1 = dist3(ax1, hub1);
        r0 = dist3(tr0, hub0);
        r1 = dist3(tr1, hub1);
        still = dist3(axle1, axle0) + dist3(spr1, spr0) + dist3(kn1, kn0)
                + dist3(hub1, hub0);
        printf("level 0 -> 3: axle span x%.4f (want x%.4f), tread radius "
               "%.4f -> %.4f, rest of the rig moved %.2e m\n",
               (double)(z1 / z0), (double)(wid[3] / wid[0]), r0, r1, still);
        ck(fabsf(z1 / z0 - wid[3] / wid[0]) < 1e-4f,
           "the drawn wheel widens along its axle by exactly that factor");
        ck(fabsf(r1 - r0) < 1e-5f,
           "a wider tyre is not a bigger one -- the tread radius is unchanged");
        ck(still < 1e-6f,
           "the width reaches the wheel node and nothing else in the rig");

        c.tire_upgrade = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* part 5b: the other two procs, against the REAL packed cars                 */
/* ------------------------------------------------------------------------- */

/* Rotation about Y that carries unit `a` onto unit `b`, in degrees, signed.
   Negating BOTH vectors leaves it alone, so it does not care which way along
   its own axle a wheel node happens to point -- which the three cars disagree
   about. */
static float yaw_between(const float a[3], const float b[3])
{
    return (float)(atan2((double)a[2] * b[0] - (double)a[0] * b[2],
                         (double)a[0] * b[0] + (double)a[2] * b[2])
                   * 57.2957795);
}

/* Unit direction of a part's own +Z in model space -- for a wheel node, the
   axle it spins about. */
static void rig_axle(const carani_t *r, int p, float o[3])
{
    const float *m = r->world[p];
    double n = sqrt((double)m[8] * m[8] + (double)m[9] * m[9]
                    + (double)m[10] * m[10]);
    if (n < 1e-9) n = 1.0;
    o[0] = (float)(m[8] / n); o[1] = (float)(m[9] / n); o[2] = (float)(m[10] / n);
}

static void rig_checks_23(void)
{
    static carani_t rig;
    rb_car c;
    rb_world w;
    int car, i;

    puts("\n-- part 5b: the Buggy's wishbones and the Hummer's third axle --");

    /* 1. WHICH WAY A WHEEL ROLLS, on all three cars. The Overkill's proc has
          been right since the rig was written, so running the same probe over
          all three makes the other two answerable to it rather than to
          themselves -- and the Hummer's wheel nodes are modelled along -X where
          the other two are along +X, which is why its own proc spins them by
          -spin. A wheel rolling backwards is not something a check on the
          ANGLE would notice; this asks where the tread at the bottom of the
          wheel goes, and forward travel is +Z. */
    for (car = 0; car < 3; car++) {
        char path[32];
        float p0[RB_MAX_WHEELS][3], p1[3];
        int back = 0, seen = 0;

        sprintf(path, "assets/car%d.vsc", car + 1);
        if (!rig_load(&rig, path)) {
            ck(0, "assets/carN.vsc loads a rig (repack with pack_vsc.py --rig)");
            continue;
        }
        memset(&w, 0, sizeof w);
        rbcar_init(&c, car, &w, 0.f, 0.f, 0.f, 0.f);
        carani_bind(&rig, &c);

        for (i = 0; i < c.nwheels; i++) c.wheel[i].spin = 0.f;
        carani_update(&rig, &c);
        for (i = 0; i < c.nwheels; i++)
            if (rig.wheel[i] >= 0)
                rig_pt(&rig, rig.wheel[i], 0.f, -1.f, 0.f, p0[i]);
        for (i = 0; i < c.nwheels; i++) c.wheel[i].spin = 0.10f;
        carani_update(&rig, &c);
        for (i = 0; i < c.nwheels; i++) {
            if (rig.wheel[i] < 0)
                continue;
            rig_pt(&rig, rig.wheel[i], 0.f, -1.f, 0.f, p1);
            seen++;
            if (p1[2] - p0[i][2] < -1e-4f)
                back++;
        }
        printf("car%d (proc %d): %d of %d wheels put the tread at the bottom "
               "BACKWARDS as spin grows\n", car + 1, rig.proc, back, seen);
        ck(seen == c.nwheels && back == seen,
           "every wheel rolls forwards when the car drives forwards");
    }

    /* ---- 2. the Buggy ---- */
    if (!rig_load(&rig, "assets/car2.vsc")) {
        ck(0, "assets/car2.vsc loads a rig");
        return;
    }
    memset(&w, 0, sizeof w);
    rbcar_init(&c, 1, &w, 0.f, 0.f, 0.f, 0.f);
    carani_bind(&rig, &c);

    ck(rig.proc == 2, "the Buggy takes carAniProc2");
    {
        int armed = 0, pairs = rig.n_pairs;
        for (i = 0; i < c.nwheels; i++)
            if (rig.wheel[i] >= 0 && rig.arm_up[i] >= 0 && rig.arm_down[i] >= 0
                && rig.arm_knuckle[i] >= 0)
                armed++;
        printf("Buggy: %d parts, %d corners with a full wishbone, %d spring "
               "pairs\n", rig.n, armed, pairs);
        ck(armed == 4, "all four wishbones bound");
        ck(pairs == 4, "all four spring pairs bound");
        ck(rig.support[0] < 0 && rig.support[1] < 0,
           "and no steering knuckle, which is why the wheel itself must turn");
    }

    /* Steering has to reach the WHEELS -- the report was that it does not. */
    {
        float a0[RB_MAX_WHEELS][3], a1[3], fy = 0.f, ry = 0.f;
        c.steer = 0.f;
        carani_update(&rig, &c);
        for (i = 0; i < c.nwheels; i++)
            rig_axle(&rig, rig.wheel[i], a0[i]);
        c.steer = 25.f;
        carani_update(&rig, &c);
        for (i = 0; i < c.nwheels; i++) {
            rig_axle(&rig, rig.wheel[i], a1);
            if (c.wheel[i].mount[2] > 0.f) fy = yaw_between(a0[i], a1);
            else                           ry = yaw_between(a0[i], a1);
        }
        printf("Buggy steer 25: front wheel yaw %+.2f deg, rear %+.2f\n",
               (double)fy, (double)ry);
        ck(fabsf(fy - c.steer) < 0.01f,
           "the Buggy's front wheels turn by the body's steer angle");
        ck(fabsf(ry) < 0.01f, "and its rear wheels do not turn");
        c.steer = 0.f;
    }

    /* The wishbone: each corner moves on its own, the wheel keeps its camber,
       and the spring halves keep pointing at each other while they do. */
    {
        float y0[RB_MAX_WHEELS], y1[RB_MAX_WHEELS], p[3];
        float ax0[3], ax1[3], camber;
        int drop = 0;                       /* the corner that is commanded */
        for (i = 0; i < c.nwheels; i++)
            c.wheel[i].len = c.wheel[i].len_free - c.wheel[i].sag;
        carani_update(&rig, &c);
        for (i = 0; i < c.nwheels; i++) {
            rig_pt(&rig, rig.wheel[i], 0.f, 0.f, 0.f, p);
            y0[i] = p[1];
        }
        rig_axle(&rig, rig.wheel[drop], ax0);

        c.wheel[drop].len += 0.030f;        /* 30 mm of droop on one corner */
        carani_update(&rig, &c);
        for (i = 0; i < c.nwheels; i++) {
            rig_pt(&rig, rig.wheel[i], 0.f, 0.f, 0.f, p);
            y1[i] = p[1];
        }
        rig_axle(&rig, rig.wheel[drop], ax1);
        camber = (float)(acos(ax0[0] * ax1[0] + ax0[1] * ax1[1]
                              + ax0[2] * ax1[2] > 1.0 ? 1.0
                              : ax0[0] * ax1[0] + ax0[1] * ax1[1]
                                + ax0[2] * ax1[2]) * 57.2957795);
        printf("Buggy 30 mm droop on corner 0: that node %+.4f m, the other "
               "three %+.4f %+.4f %+.4f, camber %.3f deg\n",
               (double)(y1[0] - y0[0]), (double)(y1[1] - y0[1]),
               (double)(y1[2] - y0[2]), (double)(y1[3] - y0[3]), (double)camber);
        /* A BAND, not a number: the arms carry the wheel down and the upright's
           counter-rotation carries a little of it back, which is the original's
           own approximation -- its sweep tables are built with the upright at
           rest. What must be true is that the wheel goes DOWN and goes most of
           the way, so both "nothing happens" and "the sign is inverted" die. */
        ck(y1[0] - y0[0] < -0.015f && y1[0] - y0[0] > -0.035f,
           "a drooped corner carries its own wheel down with it");
        for (i = 1; i < c.nwheels; i++)
            ck(fabsf(y1[i] - y0[i]) < 1e-5f,
               "and leaves the other three alone -- these corners are independent");
        /* The upright exists to keep the wheel upright. Delete its opposite
           rotation and this is what moves. */
        ck(camber < 0.5f, "the upright cancels the arms and the wheel keeps its camber");

        {
            int k, moved = 0;
            float worst = 1.f;
            for (k = 0; k < rig.n_pairs; k++) {
                int lo = rig.spair[k][0], hi = rig.spair[k][1];
                float o[3], e[3], d[3], ax[3], n, dot, len;
                rig_pt(&rig, lo, 0.f, 0.f, 0.f, o);
                rig_pt(&rig, hi, 0.f, 0.f, 0.f, e);
                d[0] = e[0]-o[0]; d[1] = e[1]-o[1]; d[2] = e[2]-o[2];
                len = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
                d[0] /= len; d[1] /= len; d[2] /= len;
                n = (float)sqrt((double)rig.world[lo][4]*rig.world[lo][4]
                                + (double)rig.world[lo][5]*rig.world[lo][5]
                                + (double)rig.world[lo][6]*rig.world[lo][6]);
                ax[0] = rig.world[lo][4]/n;
                ax[1] = rig.world[lo][5]/n;
                ax[2] = rig.world[lo][6]/n;
                dot = ax[0]*d[0] + ax[1]*d[1] + ax[2]*d[2];
                if (dot < worst) worst = dot;
                if (fabsf(len - 0.1016f) > 0.002f && fabsf(len - 0.0909f) > 0.002f)
                    moved++;
            }
            printf("Buggy springs after the droop: worst aim %.5f, %d of %d "
                   "pairs at a new length\n", (double)worst, moved, rig.n_pairs);
            ck(worst > 0.999f,
               "every spring half still points at the other one");
            ck(moved >= 1, "and the drooped corner's spring has actually moved");
        }
        c.wheel[drop].len -= 0.030f;
    }

    /* The arm angle is clamped by springs[12]/springs[13], the two keys only the
       Buggy ships. Measured on the ARM, not on the wheel: the upright takes some
       of the wheel's travel back, so how far the wheel fell says little about how
       far the arm swung. And bound to the linkage folding rather than to the
       clamp's own 35.45 degrees -- with nothing stopping it the solve runs to the
       end of the arm's circle, which is 79 degrees over on this corner. */
    {
        float d0[3], d1[3], swing;
        int drop = 0, arm = rig.arm_up[0];
        for (i = 0; i < c.nwheels; i++)
            c.wheel[i].len = c.wheel[i].len_free - c.wheel[i].sag;
        carani_update(&rig, &c);
        d0[0] = rig.world[arm][4]; d0[1] = rig.world[arm][5];
        d0[2] = rig.world[arm][6];
        c.wheel[drop].len += 0.200f;        /* far past anything the car can do */
        carani_update(&rig, &c);
        d1[0] = rig.world[arm][4]; d1[1] = rig.world[arm][5];
        d1[2] = rig.world[arm][6];
        {
            double n0 = sqrt((double)d0[0]*d0[0] + (double)d0[1]*d0[1]
                             + (double)d0[2]*d0[2]);
            double n1 = sqrt((double)d1[0]*d1[0] + (double)d1[1]*d1[1]
                             + (double)d1[2]*d1[2]);
            double dp = ((double)d0[0]*d1[0] + (double)d0[1]*d1[1]
                         + (double)d0[2]*d1[2]) / (n0 * n1);
            if (dp > 1.0) dp = 1.0;
            else if (dp < -1.0) dp = -1.0;
            swing = (float)(acos(dp) * 57.2957795);
        }
        printf("Buggy 200 mm of droop: the wishbone swung %.2f deg\n",
               (double)swing);
        ck(swing > 1.f && swing < 60.f,
           "the wishbone swings, and its clamp stops it short of folding up");
        c.wheel[drop].len -= 0.200f;
    }

    /* ---- 3. the Hummer ---- */
    if (!rig_load(&rig, "assets/car3.vsc")) {
        ck(0, "assets/car3.vsc loads a rig");
        return;
    }
    memset(&w, 0, sizeof w);
    rbcar_init(&c, 2, &w, 0.f, 0.f, 0.f, 0.f);
    carani_bind(&rig, &c);

    ck(rig.proc == 3, "the Hummer takes carAniProc3");
    {
        int bound = 0;
        for (i = 0; i < c.nwheels; i++)
            if (rig.wheel[i] >= 0)
                bound++;
        printf("Hummer: %d parts, %d of %d wheels bound, axles %d/%d/%d, "
               "%d spring pairs\n", rig.n, bound, c.nwheels, rig.axle_front,
               rig.axle_rear, rig.axle_middle, rig.n_pairs);
        ck(c.nwheels == 6 && bound == 6, "all six wheels bound");
        ck(rig.axle_middle >= 0, "the middle axle is bound");
        ck(rig.pair_middle[0] >= 0 && rig.pair_middle[1] >= 0,
           "and both of its wheels found it");
        ck(rig.n_pairs == 6, "all six spring pairs bound");
    }

    /* THE REPORTED BUG: the middle pair rode the body instead of the ground.
       The check is not "the middle axle moved" -- it is that the middle pair
       answers the SAME command with the SAME travel as the other two, because
       what was wrong was that it did not behave like them. */
    {
        float y0[RB_MAX_WHEELS], y1[RB_MAX_WHEELS], p[3];
        float dfront, dmid, drear;
        for (i = 0; i < c.nwheels; i++)
            c.wheel[i].len = c.wheel[i].len_free - c.wheel[i].sag;
        carani_update(&rig, &c);
        for (i = 0; i < c.nwheels; i++) {
            rig_pt(&rig, rig.wheel[i], 0.f, 0.f, 0.f, p);
            y0[i] = p[1];
        }
        for (i = 0; i < c.nwheels; i++) c.wheel[i].len += 0.030f;
        carani_update(&rig, &c);
        for (i = 0; i < c.nwheels; i++) {
            rig_pt(&rig, rig.wheel[i], 0.f, 0.f, 0.f, p);
            y1[i] = p[1];
        }
        dfront = y1[rig.pair_front[0]]  - y0[rig.pair_front[0]];
        drear  = y1[rig.pair_rear[0]]   - y0[rig.pair_rear[0]];
        dmid   = y1[rig.pair_middle[0]] - y0[rig.pair_middle[0]];
        printf("Hummer 30 mm of even droop: front %+.4f m, middle %+.4f, "
               "rear %+.4f\n", (double)dfront, (double)dmid, (double)drear);
        ck(dmid < -0.025f && dmid > -0.035f,
           "the middle pair follows the ground rather than the body");
        ck(fabsf(dmid - dfront) < 0.003f && fabsf(dmid - drear) < 0.003f,
           "and follows it by as much as the front and rear pairs do");
        for (i = 0; i < c.nwheels; i++) c.wheel[i].len -= 0.030f;
    }

    /* One middle wheel down and the other up is a roll, not a slide: the axle
       is solid, so its two wheels must go opposite ways. */
    {
        float y0[2], y1[2], p[3];
        int L = rig.pair_middle[0], R = rig.pair_middle[1];
        for (i = 0; i < c.nwheels; i++)
            c.wheel[i].len = c.wheel[i].len_free - c.wheel[i].sag;
        carani_update(&rig, &c);
        rig_pt(&rig, rig.wheel[L], 0.f, 0.f, 0.f, p); y0[0] = p[1];
        rig_pt(&rig, rig.wheel[R], 0.f, 0.f, 0.f, p); y0[1] = p[1];
        c.wheel[L].len += 0.030f;
        c.wheel[R].len -= 0.030f;
        carani_update(&rig, &c);
        rig_pt(&rig, rig.wheel[L], 0.f, 0.f, 0.f, p); y1[0] = p[1];
        rig_pt(&rig, rig.wheel[R], 0.f, 0.f, 0.f, p); y1[1] = p[1];
        printf("Hummer middle axle, left down / right up: left %+.4f m, "
               "right %+.4f\n", (double)(y1[0]-y0[0]), (double)(y1[1]-y0[1]));
        ck(y1[0] - y0[0] < -0.025f && y1[1] - y0[1] > 0.025f,
           "the middle axle rolls, each wheel to its own height");
        c.wheel[L].len -= 0.030f;
        c.wheel[R].len += 0.030f;
    }

    /* The middle axle SLIDES, and a slide with nothing bounding it would take
       the axle through the floor of the car. 100 mm is the bound here: not the
       clamp itself (45 mm), just far enough under the 200 mm commanded that only
       something stopping it can pass. */
    {
        float p[3], y0, y1;
        int L = rig.pair_middle[0];
        for (i = 0; i < c.nwheels; i++)
            c.wheel[i].len = c.wheel[i].len_free - c.wheel[i].sag;
        carani_update(&rig, &c);
        rig_pt(&rig, rig.wheel[L], 0.f, 0.f, 0.f, p);
        y0 = p[1];
        for (i = 0; i < c.nwheels; i++) c.wheel[i].len += 0.200f;
        carani_update(&rig, &c);
        rig_pt(&rig, rig.wheel[L], 0.f, 0.f, 0.f, p);
        y1 = p[1];
        printf("Hummer 200 mm of droop: the middle axle slid %.4f m\n",
               (double)(y0 - y1));
        ck(y0 - y1 < 0.100f, "the middle axle's slide is bounded");
        for (i = 0; i < c.nwheels; i++) c.wheel[i].len -= 0.200f;
    }

    /* The middle pair steers WITH the front, and by less. The fraction itself is
       0.3 in the image; asserting that number here would only be asserting the
       constant against itself, so this asserts the shape -- same way, smaller,
       and the rear not at all. */
    {
        float a0[RB_MAX_WHEELS][3], a1[3];
        float fy = 0.f, my = 0.f, ry = 0.f;
        c.steer = 0.f;
        carani_update(&rig, &c);
        for (i = 0; i < c.nwheels; i++)
            rig_axle(&rig, rig.wheel[i], a0[i]);
        c.steer = 25.f;
        carani_update(&rig, &c);
        for (i = 0; i < c.nwheels; i++) {
            rig_axle(&rig, rig.wheel[i], a1);
            if (c.wheel[i].mount[2] > 0.02f)       fy = yaw_between(a0[i], a1);
            else if (c.wheel[i].mount[2] < -0.02f) ry = yaw_between(a0[i], a1);
            else                                   my = yaw_between(a0[i], a1);
        }
        printf("Hummer steer 25: front %+.2f deg, middle %+.2f, rear %+.2f\n",
               (double)fy, (double)my, (double)ry);
        ck(fabsf(fy - c.steer) < 0.01f, "the Hummer's front pair takes the lock");
        ck(fabsf(ry) < 0.01f, "its rear pair does not steer");
        ck(my * fy > 0.f && fabsf(my) > 0.5f && fabsf(my) < fabsf(fy),
           "and its middle pair steers the same way as the front, by less");
        c.steer = 0.f;
    }
}

/* ------------------------------------------------------------------------- */
/* part 6: the Jump action, and water                                        */
/* ------------------------------------------------------------------------- */

/* Settle a car of the given index on the flat plane, at rest. */
static void settle(rb_car *c, int car)
{
    int i;
    rbcar_init(c, car, &FLAT_WORLD, 0.0f, 0.0f, 0.0f, 0.0f);
    for (i = 0; i < 120; i++)
        rbcar_step(c, 0.0f, 0.0f, 0.0f, 0, 1.0f / 60.0f);
}

static float tilt_deg(const rb_car *c)
{
    double u = c->m[5];
    if (u > 1.0) u = 1.0;
    if (u < -1.0) u = -1.0;
    return (float)(acos(u) * RB_RAD2DEG);
}

/* Hold a car at exactly `deg` of roll about its own forward axis, lowered until
 * rb_collide in `mode` reports contact (0 wheels, 1 body, 2 either).
 *
 * Dropping it and letting it settle -- which is what this did first -- does not
 * work for a threshold test: a car released at 100 degrees rolls the rest of the
 * way onto its roof and arrives at 137. Every tilt between the threshold and 137
 * then goes untested, and a mutant that moved RB_JUMP_FLIP_DEG from 80 to 120
 * survived the whole suite. Hold the attitude instead and only find the height.
 *
 * `mode` is a parameter for a second reason found the same way. Between about 80
 * and 100 degrees this car balances on the OUTER EDGES OF ITS WHEELS, which stick
 * out past the body -- so no body sphere touches, and the reset (which gates on
 * mode 1, the body spheres, exactly as carJump does) correctly declines. That is
 * the transcription behaving, not failing, but it means a threshold sweep has to
 * lower to mode 1 so the gate is satisfied at every tilt and the ANGLE is the
 * only thing varying. */
static void tilt_and_rest(rb_car *c, int car, float deg, int mode)
{
    double half = (double)deg * 0.017453292 * 0.5;
    int k;

    rbcar_init(c, car, &FLAT_WORLD, 0.0f, 0.0f, 0.0f, 0.0f);
    c->body.q[0] = (float)cos(half);    /* about body +Z, the forward axis */
    c->body.q[1] = 0.0f;
    c->body.q[2] = 0.0f;
    c->body.q[3] = (float)sin(half);
    rb_quat_normalize(c->body.q);
    rb_update_inv_inertia_world(&c->body);

    for (k = 0; k < 1500; k++) {
        c->body.x[1] = 0.60f - (float)k * 0.001f;
        rb_car_update_matrix(c);
        if (rb_collide(c, 0.0f, RB_CONTACT_TOL, mode, -1, 0))
            return;
    }
    ck(0, "tilt_and_rest never found contact -- the fixture is broken");
}

/* Put a FRESH contact gather into c->hit.
 *
 * This exists because of a bug it caught in this very harness. A car the rest
 * clamp has parked skips its entire tick -- hit[] included -- so changing the
 * world and stepping once reads back the PREVIOUS answer. The water sweep below
 * did exactly that and reported every level's result one case late, which looked
 * like a plausible set of numbers and was not. Zero the timers so the tick runs. */
static void refresh_contacts(rb_car *c)
{
    c->rest_slow_t = c->rest_spin_t = c->rest_ground_t = 0.0f;
    rbcar_step(c, 0.0f, 0.0f, 0.0f, 0, 1.0f / 60.0f);
    if (c->asleep)
        ck(0, "refresh_contacts failed to wake the car -- results are stale");
}

/* Hold Jump for `frames`, counting what it did. */
static void hold_jump(rb_car *c, int frames, int *hops, int *resets)
{
    int i;
    *hops = *resets = 0;
    for (i = 0; i < frames; i++) {
        int r = rbcar_jump(c, 1, 1.0f / 60.0f);
        if (r == RB_JUMP_HOP)   (*hops)++;
        if (r == RB_JUMP_RESET) (*resets)++;
        rbcar_step(c, 0.0f, 0.0f, 0.0f, 0, 1.0f / 60.0f);
    }
}

static void jump_water_checks(void)
{
    rb_car c;
    int i, hops, resets;

    puts("\n-- part 6: the Jump action (0x004f3b80) --");

    /* The cooldown gate. jump_t starts at 0, so the button does nothing at all
       for the first RB_JUMP_COOLDOWN of the car's life. */
    {
        int early = 0;
        settle(&c, 0);
        c.jump_t = 0.0f;
        for (i = 0; i < 29; i++) {                 /* 29/60 s < 0.5 s */
            if (rbcar_jump(&c, 1, 1.0f / 60.0f) != RB_JUMP_NONE) early++;
            rbcar_jump(&c, 0, 0.0f);               /* release, clear the latch */
            rbcar_step(&c, 0.0f, 0.0f, 0.0f, 0, 1.0f / 60.0f);
        }
        printf("cooldown: %d hops in the first %.2f s (limit %.2f)\n",
               early, 29.0 / 60.0, (double)RB_JUMP_COOLDOWN);
        ck(early == 0, "Jump does nothing before the cooldown elapses");
    }

    /* One hop per press, not one per frame. */
    {
        settle(&c, 0);
        c.jump_t = RB_JUMP_COOLDOWN;
        hold_jump(&c, 180, &hops, &resets);
        printf("held for 3 s: %d hops, %d resets\n", hops, resets);
        ck(hops == 1, "holding Jump hops exactly once");
        ck(resets == 0, "an upright car is never reset");
    }

    /* Tapping it again after the cooldown hops again. */
    {
        int total = 0;
        settle(&c, 0);
        c.jump_t = RB_JUMP_COOLDOWN;
        for (i = 0; i < 240; i++) {
            /* press for one frame in ten, so every press is a fresh edge */
            int held = (i % 10) == 0;
            if (rbcar_jump(&c, held, 1.0f / 60.0f) == RB_JUMP_HOP) total++;
            rbcar_step(&c, 0.0f, 0.0f, 0.0f, 0, 1.0f / 60.0f);
        }
        /* 0.5 s between hops (0x554384) allows 8 in four seconds; the literal,
           so raising the constant cannot raise the bound with it. */
        printf("tapping for 4 s: %d hops (0.5 s cooldown allows at most 8)\n",
               total);
        ck(total >= 2, "releasing and pressing again hops again");
        ck(total <= 9, "the 0.5 s cooldown still limits the rate");
    }

    /* The cooldown has an UPPER bound too. `total >= 2` above is satisfied by a
       two-second cooldown, so on its own it lets the constant grow without limit;
       a 0.5 s cooldown must allow a second hop 0.6 s later. */
    {
        int second;
        settle(&c, 0);
        c.jump_t = RB_JUMP_COOLDOWN;
        rbcar_jump(&c, 1, 0.0f);                     /* first hop, cooldown reset */
        for (i = 0; i < 36; i++) {                   /* 0.6 s, button released */
            rbcar_jump(&c, 0, 1.0f / 60.0f);
            rbcar_step(&c, 0.0f, 0.0f, 0.0f, 0, 1.0f / 60.0f);
        }
        second = rbcar_jump(&c, 1, 0.0f);
        printf("second press 0.6 s later: %d\n", second);
        ck(second == RB_JUMP_HOP, "0.6 s after a hop the car can hop again");
    }

    /* The impulse itself: RB_JUMP_SPEED along the car's own up axis, times mass.
       Read off P directly, before anything integrates it. */
    {
        float p0[3], dp[3], along;
        settle(&c, 0);
        c.jump_t = RB_JUMP_COOLDOWN;
        memcpy(p0, c.body.P, sizeof(p0));
        ck(rbcar_jump(&c, 1, 0.0f) == RB_JUMP_HOP, "a settled car can hop");
        for (i = 0; i < 3; i++) dp[i] = c.body.P[i] - p0[i];
        along = (float)((double)dp[0]*c.m[4] + (double)dp[1]*c.m[5]
                        + (double)dp[2]*c.m[6]);
        printf("hop impulse: |dP|=%.4f along up=%.4f  (mass*%.1f = %.4f)\n",
               sqrtf(dp[0]*dp[0] + dp[1]*dp[1] + dp[2]*dp[2]), along,
               (double)RB_JUMP_SPEED, c.body.mass * RB_JUMP_SPEED);
        /* 3.0, the literal at 0x5543f4 -- NOT RB_JUMP_SPEED. Comparing against
           the constant the code uses makes the check move with it, which is how
           four assertions in this port ended up as decoration; see CLAUDE.md on
           self-referential assertions. */
        ck(fabsf(along - c.body.mass * 3.0f) < 1e-3f,
           "the hop adds 3.0 m/s * mass along the body up axis (0x5543f4)");
        ck(fabsf(sqrtf(dp[0]*dp[0] + dp[1]*dp[1] + dp[2]*dp[2]) - along) < 1e-3f,
           "the impulse is purely along the up axis");
        ck(c.jump_t == 0.0f, "a successful hop restarts the cooldown");
    }

    /* What 3 m/s actually buys, which no constant in the code can fake: a car
       thrown upward at v against gravity 10 rises v^2/20 = 0.45 m. This is the
       check that says the hop is a JUMP and not a twitch. */
    {
        float y0, peak;
        settle(&c, 0);
        y0 = c.body.x[1];
        peak = y0;
        c.jump_t = RB_JUMP_COOLDOWN;
        rbcar_jump(&c, 1, 0.0f);
        for (i = 0; i < 120; i++) {
            rbcar_step(&c, 0.0f, 0.0f, 0.0f, 0, 1.0f / 60.0f);
            if (c.body.x[1] > peak) peak = c.body.x[1];
        }
        printf("hop apex: +%.3f m above the resting %.4f (3^2/20 = 0.450)\n",
               peak - y0, y0);
        ck(peak - y0 > 0.35f, "the hop clears the ground by a third of a metre");
        ck(peak - y0 < 0.55f, "and not by more than half a metre");
        ck(fabsf(c.body.x[1] - y0) < 0.01f, "and it comes back down to rest");
    }

    /* Yaw momentum is multiplied, and only yaw. */
    {
        float l0[3];
        settle(&c, 0);
        c.jump_t = RB_JUMP_COOLDOWN;
        c.body.L[0] = 0.02f; c.body.L[1] = 0.05f; c.body.L[2] = -0.03f;
        memcpy(l0, c.body.L, sizeof(l0));
        rbcar_jump(&c, 1, 0.0f);
        printf("hop spin: L.y %.4f -> %.4f (x%.2f), L.x/L.z unchanged=%s\n",
               l0[1], c.body.L[1], (double)RB_JUMP_SPIN_MULT,
               (c.body.L[0] == l0[0] && c.body.L[2] == l0[2]) ? "yes" : "NO");
        ck(fabsf(c.body.L[1] - l0[1] * 1.5f) < 1e-6f,
           "the hop multiplies yaw momentum by 1.5 (0x554824)");
        ck(c.body.L[0] == l0[0] && c.body.L[2] == l0[2],
           "roll and pitch momentum are left alone");
    }

    /* The Buggy hops harder -- car index 1 is the one special case in the
       original (0x5549ac against 0x5543f4). */
    {
        float along[3];
        int k;
        for (k = 0; k < 3; k++) {
            float p0[3], dp[3];
            settle(&c, k);
            c.jump_t = RB_JUMP_COOLDOWN;
            memcpy(p0, c.body.P, sizeof(p0));
            rbcar_jump(&c, 1, 0.0f);
            for (i = 0; i < 3; i++) dp[i] = c.body.P[i] - p0[i];
            along[k] = (float)(((double)dp[0]*c.m[4] + (double)dp[1]*c.m[5]
                                + (double)dp[2]*c.m[6]) / c.body.mass);
        }
        printf("hop speed by car: %.4f / %.4f / %.4f m/s\n",
               along[0], along[1], along[2]);
        /* Literals again: 0x5543f4 is 3.0 and 0x5549ac is 3.3000002. */
        ck(fabsf(along[0] - 3.0f) < 1e-3f, "Overkill hops at 3.0 m/s");
        ck(fabsf(along[1] - 3.3000002f) < 1e-3f, "the Buggy hops at 3.3 m/s");
        ck(fabsf(along[2] - 3.0f) < 1e-3f, "the Hummer hops at 3.0 m/s");
        ck(along[1] > along[0], "the Buggy is the one that hops higher");
    }

    /* No hop in mid-air: the wheel spheres have to be touching. */
    {
        int r;
        settle(&c, 0);
        c.body.x[1] += 2.0f;
        rb_car_update_matrix(&c);
        c.jump_t = RB_JUMP_COOLDOWN;
        r = rbcar_jump(&c, 1, 0.0f);
        printf("airborne (2 m up): jump returned %d\n", r);
        ck(r == RB_JUMP_NONE, "a car in mid-air cannot hop");
    }

    /* The 80-degree threshold: what separates a hop from a reset. Swept, and from
       both sides -- a threshold tested at one tilt is not tested. 90 degrees is
       the case that actually matters: a car lying on its SIDE, which is what the
       reset is for as much as one on its roof. */
    {
        const float tilts[] = { 0.0f, 40.0f, 70.0f, 79.0f,
                                81.0f, 90.0f, 120.0f, 170.0f };
        unsigned k;
        /* Only the RESET side is asserted here. Resting on the body at a shallow
           tilt lifts the wheels clear, so most of the sub-80 rows come out NONE
           rather than HOP -- that is the hop's own contact gate, tested above. */
        printf("threshold sweep (RESET=%d, HOP=%d, NONE=%d):\n",
               RB_JUMP_RESET, RB_JUMP_HOP, RB_JUMP_NONE);
        for (k = 0; k < sizeof(tilts)/sizeof(tilts[0]); k++) {
            /* 80.0, the literal at 0x554958, not the macro. */
            int r, want_reset = tilts[k] > 80.0f;
            float held;
            /* mode 1: rest it on its BODY, so the reset's contact gate is
               satisfied at every tilt and only the angle is in play. */
            tilt_and_rest(&c, 0, tilts[k], 1);
            held = tilt_deg(&c);
            c.jump_t = RB_JUMP_COOLDOWN;
            r = rbcar_jump(&c, 1, 0.0f);
            printf("  held at %5.1f deg (measured %5.1f) -> %d\n",
                   tilts[k], held, r);
            ck(fabsf(held - tilts[k]) < 0.5f, "the fixture held the tilt it asked for");
            if (want_reset)
                ck(r == RB_JUMP_RESET, "beyond 80 degrees of tilt Jump resets");
            else
                ck(r != RB_JUMP_RESET,
                   "inside 80 degrees of tilt Jump never resets");
        }
    }

    /* And the contact gate on the reset path, in isolation: between about 80 and
       100 degrees this car balances on the outer edges of its wheels with no body
       sphere touching, and carJump's mode-1 test then declines to reset it. Worth
       pinning as behaviour rather than leaving it to be rediscovered. */
    {
        int r_wheels, r_body;
        tilt_and_rest(&c, 0, 90.0f, 0);            /* down to the wheels only */
        c.jump_t = RB_JUMP_COOLDOWN;
        r_wheels = rbcar_jump(&c, 1, 0.0f);
        tilt_and_rest(&c, 0, 90.0f, 1);            /* down to the body */
        c.jump_t = RB_JUMP_COOLDOWN;
        r_body = rbcar_jump(&c, 1, 0.0f);
        printf("on its side at 90 deg: on wheel edges -> %d, on its body -> %d\n",
               r_wheels, r_body);
        ck(r_wheels == RB_JUMP_NONE,
           "the reset declines while only the wheels are touching");
        ck(r_body == RB_JUMP_RESET, "the reset fires once the body is down");
    }

    /* A hop follows the car's OWN up axis, not the world's -- so a hop off a
       banked surface throws the car sideways, which is the whole point of the
       original reading matrix row 1. Checked on a 40-degree slope, held. */
    {
        float p0[3], dp[3], along, vertical;
        tilt_and_rest(&c, 0, 40.0f, 0);
        c.jump_t = RB_JUMP_COOLDOWN;
        memcpy(p0, c.body.P, sizeof(p0));
        ck(rbcar_jump(&c, 1, 0.0f) == RB_JUMP_HOP, "a car on its side-ish hops");
        for (i = 0; i < 3; i++) dp[i] = c.body.P[i] - p0[i];
        along = (float)((double)dp[0]*c.m[4] + (double)dp[1]*c.m[5]
                        + (double)dp[2]*c.m[6]);
        vertical = dp[1];
        printf("hop at 40 deg of roll: along body up %.4f, vertical %.4f, "
               "lateral %.4f\n", along, vertical, dp[0]);
        ck(fabsf(along - c.body.mass * RB_JUMP_SPEED) < 1e-3f,
           "the tilted hop is still RB_JUMP_SPEED * mass along the BODY up axis");
        ck(fabsf(dp[0]) > 0.5f,
           "a tilted hop throws the car sideways, not straight up");
        ck(vertical < along - 0.5f,
           "the vertical part of a tilted hop is less than the total");
    }

    /* WHAT rbcar_yaw_deg ACTUALLY RETURNS, asserted rather than commented.
     *
     * It is the RENDERER'S VIEW YAW -- the angle a camera needs to look at the
     * car's forward -- and so it is 180 degrees from where the car POINTS. Three
     * consumers depend on that (cam.c, the chase camera, shadow_draw_yaw) and
     * main.c draws the car model with `veh.yaw + 180` because of it. It is also a
     * trap: it cost the minimap its arrow, which came out 180 degrees round on
     * every track and every opponent because a fourth caller read it as a heading.
     *
     * So this pins BOTH halves: rbcar_init's yaw really does put local +Z on
     * (sin y, 0, cos y), and rbcar_yaw_deg really is that plus 180. A "fix" to
     * either one fails here and says which. */
    {
        static const float YW[6] = { 0.f, 45.f, 90.f, 135.f, 180.f, -90.f };
        int q, bad_fwd = 0, bad_view = 0;
        for (q = 0; q < 6; q++) {
            rb_car y;
            float r = YW[q] * 0.017453292519943295f, got, want, d;
            rbcar_init(&y, 0, &FLAT_WORLD, 0.f, 0.f, 0.f, YW[q]);
            /* column 2 of the column-major matrix is local +Z in world space */
            if (fabs(y.m[8] - sinf(r)) > 1e-4 || fabs(y.m[10] - cosf(r)) > 1e-4
                || fabs(y.m[9]) > 1e-4)
                bad_fwd++;
            got = rbcar_yaw_deg(&y);
            want = YW[q] + 180.f;
            d = got - want;
            while (d > 180.f) d -= 360.f;
            while (d < -180.f) d += 360.f;
            if (fabs(d) > 0.05) bad_view++;
        }
        ck(bad_fwd == 0,
           "rbcar_init's yaw puts local +Z on (sin y, 0, cos y) -- the rig's "
           "own convention, over six headings");
        ck(bad_view == 0,
           "and rbcar_yaw_deg returns that plus 180 -- the VIEW yaw, not the "
           "heading. Anything wanting the heading must add 180 back");
    }

    /* The reset itself. Roll the car right over and settle it on its roof. */
    {
        float tilt_before, tilt_after, yaw_before, yaw_after, y_after;
        int r;
        rbcar_init(&c, 0, &FLAT_WORLD, 0.0f, 0.0f, 0.0f, 40.0f);
        yaw_before = rbcar_yaw_deg(&c);
        /* 180 degrees about the body forward axis, then let it come to rest */
        {
            double half = 3.14159265358979 * 0.5;
            float qr[4], qc[4];
            qr[0] = (float)cos(half); qr[1] = 0.0f; qr[2] = 0.0f;
            qr[3] = (float)sin(half);
            rb_quat_mul(qr, c.body.q, qc);
            memcpy(c.body.q, qc, sizeof(qc));
            rb_quat_normalize(c.body.q);
            c.body.x[1] = 0.5f;
            rb_car_update_matrix(&c);
            rb_update_inv_inertia_world(&c.body);
        }
        for (i = 0; i < 180; i++) rbcar_step(&c, 0.0f, 0.0f, 0.0f, 0, 1.0f/60.0f);
        tilt_before = (float)(acos(c.m[5] < -1.0f ? -1.0f : c.m[5]) * RB_RAD2DEG);
        /* Read the heading HERE, not before the tumble: settling on its roof
           yaws the car as well as rolling it, and the reset can only preserve
           the heading it is handed. */
        yaw_before = rbcar_yaw_deg(&c);
        /* Give it real momentum first. A car that has finished settling is already
           at rest, so "the reset zeroes P and L" passed against code that zeroed
           neither -- both mutants survived until this was here. */
        c.body.P[0] = 4.0f; c.body.P[1] = -1.0f; c.body.P[2] = 2.0f;
        c.body.L[0] = 0.3f; c.body.L[1] = -0.2f; c.body.L[2] = 0.15f;
        c.body.v[0] = 2.0f; c.body.v[1] = -0.5f; c.body.v[2] = 1.0f;
        c.body.w[0] = 1.5f; c.body.w[1] = -0.8f; c.body.w[2] = 0.6f;
        c.jump_t = RB_JUMP_COOLDOWN;
        r = rbcar_jump(&c, 1, 0.0f);
        tilt_after = (float)(acos(c.m[5] > 1.0f ? 1.0f : c.m[5]) * RB_RAD2DEG);
        yaw_after = rbcar_yaw_deg(&c);
        y_after = c.body.x[1];
        printf("reset: tilt %.1f -> %.1f deg,  yaw %+.1f -> %+.1f deg,  "
               "y=%.3f,  |P|=%.5f |L|=%.5f\n",
               tilt_before, tilt_after, yaw_before, yaw_after, y_after,
               sqrtf(c.body.P[0]*c.body.P[0] + c.body.P[1]*c.body.P[1]
                     + c.body.P[2]*c.body.P[2]),
               sqrtf(c.body.L[0]*c.body.L[0] + c.body.L[1]*c.body.L[1]
                     + c.body.L[2]*c.body.L[2]));
        /* Not 180, historically: a car dropped upside down did not stay balanced
           there, because the proxy was one trio of body spheres at a single Z
           station -- body_sphere[1] and [2] absent -- so an inverted car had
           nothing under its roof and rolled onto a corner of the one cluster it
           did have. It settled around 153 degrees.
           With the roof stations in (gen_rb_data.py) it lies flat on its roof at
           178.9 and the bound has a lot more room than it used to. Left at 120
           anyway: what this check exists to say is that the FIXTURE really
           inverted the car, comfortably past the 80 degrees the jump tests, and
           tightening it to the new resting attitude would turn a fixture guard
           into a second copy of the roof-station measurement. flipped.c owns
           that measurement, against the drawn mesh. */
        ck(tilt_before > 120.0f, "the fixture really did put the car on its roof");
        ck(r == RB_JUMP_RESET, "Jump on an inverted car resets it");
        ck(tilt_after < 0.5f, "the reset leaves the car level");
        /* The heading survives. This is the reason the reset flattens the forward
           vector instead of just loading the identity: a reset that faced the car
           down the track's +Z would be useless on a corner. */
        ck(fabsf(yaw_after - yaw_before) < 1.0f,
           "the reset keeps the car's heading");
        ck(sqrtf(c.body.P[0]*c.body.P[0] + c.body.P[1]*c.body.P[1]
                 + c.body.P[2]*c.body.P[2]) == 0.0f, "the reset zeroes P");
        ck(sqrtf(c.body.L[0]*c.body.L[0] + c.body.L[1]*c.body.L[1]
                 + c.body.L[2]*c.body.L[2]) == 0.0f, "the reset zeroes L");
        /* The derived velocities too. Leaving these stale is invisible in practice
           -- the next tick rederives them from P and L -- but the reset's contract
           is that the car is dead still afterwards, and stating it here is what
           stops a mutation that drops the line from passing silently. */
        ck(c.body.v[0] == 0.0f && c.body.v[1] == 0.0f && c.body.v[2] == 0.0f,
           "the reset zeroes the derived linear velocity");
        ck(c.body.w[0] == 0.0f && c.body.w[1] == 0.0f && c.body.w[2] == 0.0f,
           "the reset zeroes the derived angular velocity");
        /* The clearance sphere is 0.5 m and much bigger than the car, so on open
           ground the reset lifts the origin to 0.5 m and lets it drop. */
        ck(fabsf(y_after - 0.5f) < 0.01f,
           "the reset lifts the body clear by 0.5 m (0x3f000000)");
        /* And two things the constant cannot move: the lift has to be big enough
           to be a lift at all -- more than the car's own 71 mm ride height -- and
           small enough not to be a launch. */
        ck(y_after > 0.15f, "the reset actually picks the car up off the ground");
        ck(y_after < 1.0f, "the reset does not fling the car into the air");

        /* The clearance loop against TWO surfaces. Reset an inverted car wedged
           against a wall: one pass clears the floor and leaves it inside the
           wall, so the loop has to go round again. On a bare plane it never does,
           and a mutant cutting RB_RESET_CLEAR_PASSES from 10 to 1 survived. */
        {
            float dx;
            tilt_and_rest(&c, 0, 180.0f, 1);            /* on its roof, touching */
            WALL_X = c.body.x[0] + 0.10f;               /* a wall 10 cm to its right */
            c.jump_t = RB_JUMP_COOLDOWN;
            ck(rbcar_jump(&c, 1, 0.0f) == RB_JUMP_RESET,
               "an inverted car wedged at a wall still resets");
            dx = WALL_X - c.body.x[0];
            printf("reset in a corner: %.3f m from the floor, %.3f m from the "
                   "wall\n", c.body.x[1], dx);
            WALL_X = 1e30f;
            ck(c.body.x[1] > 0.49f, "cleared the floor");
            ck(dx > 0.49f, "and the wall too -- the loop ran more than once");
        }

        /* And it lands back on its wheels rather than bouncing off again. */
        for (i = 0; i < 300; i++) rbcar_step(&c, 0.0f, 0.0f, 0.0f, 0, 1.0f/60.0f);
        {
            float t2 = (float)(acos(c.m[5] > 1.0f ? 1.0f : c.m[5]) * RB_RAD2DEG);
            printf("       5 s later: tilt %.2f deg, y=%.4f\n", t2, c.body.x[1]);
            ck(t2 < 5.0f, "the car is still on its wheels five seconds later");
        }
    }

    /* An inverted car in mid-air is not reset -- the body spheres must touch. */
    {
        int r;
        rbcar_init(&c, 0, &FLAT_WORLD, 0.0f, 0.0f, 0.0f, 0.0f);
        c.body.q[0] = 0.0f; c.body.q[1] = 0.0f;
        c.body.q[2] = 0.0f; c.body.q[3] = 1.0f;
        c.body.x[1] = 5.0f;
        rb_car_update_matrix(&c);
        rb_update_inv_inertia_world(&c.body);
        c.jump_t = RB_JUMP_COOLDOWN;
        r = rbcar_jump(&c, 1, 0.0f);
        printf("inverted 5 m up: jump returned %d\n", r);
        ck(r == RB_JUMP_NONE, "a tumbling car cannot be reset mid-flight");
    }

    /* Jump wakes a car the rest clamp has put to sleep. Without this the clamp
       zeroes P before anything integrates the impulse and the car never leaves
       the ground -- which is why 0x577c is in FUN_004f59a0's input test. */
    {
        float y0;
        settle(&c, 0);
        for (i = 0; i < 400; i++) rbcar_step(&c, 0.0f, 0.0f, 0.0f, 0, 1.0f/60.0f);
        printf("after 6.7 s parked: asleep=%d\n", c.asleep);
        ck(c.asleep, "the rest clamp does put a parked car to sleep");
        y0 = c.body.x[1];
        c.jump_t = RB_JUMP_COOLDOWN;
        rbcar_jump(&c, 1, 1.0f / 60.0f);
        rbcar_step(&c, 0.0f, 0.0f, 0.0f, 0, 1.0f / 60.0f);
        for (i = 0; i < 8; i++) {
            rbcar_jump(&c, 1, 1.0f / 60.0f);
            rbcar_step(&c, 0.0f, 0.0f, 0.0f, 0, 1.0f / 60.0f);
        }
        printf("       hopped from sleep: y %.4f -> %.4f (+%.1f mm)\n",
               y0, c.body.x[1], (c.body.x[1] - y0) * 1000.0f);
        ck(c.body.x[1] > y0 + 0.05f, "Jump wakes a sleeping car and lifts it");

        /* The hop's own impulse is enough to wake the car by itself -- |P| = 6
           trips the speed timer -- so the check above passes with the jump bit
           missing from rb_car_rest_update's input test. What the bit is actually
           for is the frames where Jump does NOT produce an impulse: the cooldown,
           the latch, mid-air. Hold the button on a parked car and the original
           keeps all four timers at zero the whole time, so it cannot fall asleep
           under the driver's hand. That is what 0x577c is doing in FUN_004f59a0,
           and this is the check that sees it. */
        settle(&c, 0);
        {
            int slept = 0;
            for (i = 0; i < 600; i++) {           /* 10 s, button held */
                rbcar_jump(&c, 1, 1.0f / 60.0f);
                rbcar_step(&c, 0.0f, 0.0f, 0.0f, 0, 1.0f / 60.0f);
                if (c.asleep) slept++;
            }
            printf("       Jump held for 10 s parked: %d asleep frames\n", slept);
            ck(slept == 0, "a car cannot fall asleep while Jump is held");
        }
    }

    /* ---- water ---------------------------------------------------------- */
    puts("\n-- part 6b: water (carSurfaceDrag 0x004eeea0) --");

    /* The probe reaches the contact record, and the drag it produces. Driven
       through rb_surface_drag directly so the force is readable. */
    {
        float pts[RB_MAX_FORCES][3], f[RB_MAX_FORCES][3];
        float radius;
        double coeff;
        int n;

        settle(&c, 0);
        radius = c.wheel[0].radius;
        coeff = c.tune.coeff_water;
        printf("wheel radius %.4f m, coeffWater %.4f, coeffDeepSand %.4f\n",
               radius, coeff, c.tune.coeff_deep_sand);
        ck(coeff > 0.0f, "the Overkill has a coeffWater to apply");

        /* dry: no water probe, no drag (the plane is not deep sand either) */
        WATER_Y = -1e30f;
        for (i = 0; i < 20; i++) rbcar_step(&c, 0.0f, 0.0f, 0.0f, 0, 1.0f/60.0f);
        ck(!c.hit[0].in_water, "no water reported when the world has none");
        c.body.v[0] = 5.0f; c.body.P[0] = 5.0f * c.body.mass;
        n = 0;
        rb_surface_drag(&c, &n, pts, f);
        printf("dry: %d drag forces\n", n);
        ck(n == 0, "dry ground makes no surface drag");

        /* Water level sweep. `depth = radius - gap`, and the ramp runs from 0.3
         * to 0.9 of the radius, so these are the thresholds the original's
         * arithmetic pins.
         *
         * The expectation is derived from the gap the model ACTUALLY saw, not
         * from the water level this asked for. A first version compared against
         * the level it set, and the car had drifted a couple of centimetres in
         * the frames between the two -- enough to move a case across the ramp and
         * report a 30% mutant as passing. Read the state back; do not assume it.
         */
        {
            /* Negative means the surface is BELOW the wheel entirely -- water in a
               ditch the car is straddling, which must drag nothing at all. */
            const float fracs[] = { -1.00f, 0.10f, 0.29f, 0.35f,
                                     0.60f, 1.00f, 1.60f };
            float prev_depth = -1e30f, prev_mag = 0.0f;
            unsigned k;
            for (k = 0; k < sizeof(fracs)/sizeof(fracs[0]); k++) {
                float centre[3], mag, gap, depth, want;
                double cf;

                int submerged;
                settle(&c, 0);
                rb_wheel_frame(&c, 0, 0, centre, NULL, NULL, NULL);
                WATER_Y = centre[1] - radius * (1.0f - fracs[k]);
                refresh_contacts(&c);
                c.body.v[0] = 5.0f; c.body.v[1] = 0.0f; c.body.v[2] = 0.0f;
                c.body.P[0] = 5.0f * c.body.mass;
                c.body.P[1] = c.body.P[2] = 0.0f;
                n = 0;
                rb_surface_drag(&c, &n, pts, f);
                mag = 0.0f;
                for (i = 0; i < n; i++)
                    mag += sqrtf(f[i][0]*f[i][0] + f[i][1]*f[i][1]
                                 + f[i][2]*f[i][2]);

                gap   = c.hit[0].water_gap;
                depth = radius - gap;
                /* What carSurfaceDrag should make of that gap. A wheel whose
                   centre is at or above the surface takes the DRY branch and is
                   skipped entirely; anything below it is appended even when the
                   ramp puts the coefficient at zero, which is the original's own
                   behaviour and the reason `n` and `mag` say different things. */
                submerged = c.hit[0].in_water && gap < radius;
                if (!submerged)                     cf = 0.0;
                else if (depth < radius * 0.3f)     cf = 0.0;
                else if (depth <= radius * 0.9f)
                    cf = coeff * (depth - radius * 0.3) / (radius * 0.6);
                else                                cf = coeff;
                want = (float)(4.0 * cf * 25.0);

                printf("  asked %4.0f%%  gap=%+.4f depth/r=%5.2f  in_water=%d  "
                       "%d forces  %6.3f N (want %6.3f)\n",
                       fracs[k] * 100.0f, gap, depth / radius,
                       c.hit[0].in_water, n, mag, want);

                /* in_water is "the wheel is WET", not "the column has water".
                   The -100% case is the one with teeth: the surface is a whole
                   radius below the wheel, which is what a pier deck over the sea
                   looks like, and it must read dry. Asserted against the gap the
                   model actually saw, for the drift reason above. */
                ck(c.hit[0].in_water == (gap < radius),
                   "in_water says wet, not merely that the column has water");
                if (fracs[k] < 0.0f)
                    ck(!c.hit[0].in_water,
                       "a surface a full radius below the wheel is not water "
                       "the wheel is in");
                ck(fabsf(mag - want) < 0.02f * (want > 1.0f ? want : 1.0f),
                   "the drag matches coeffWater ramped over 30%..90% of the radius");
                ck(n == (submerged ? c.nwheels : 0),
                   "only a wheel whose centre is under the surface is considered");
                /* Monotone in depth: the ramp must not run backwards. */
                if (prev_depth > -1.0f && depth > prev_depth)
                    ck(mag >= prev_mag - 1e-4f,
                       "deeper water never drags less");
                prev_depth = depth; prev_mag = mag;
            }
            /* And it opposes the motion rather than helping it. */
            ck(f[0][0] < 0.0f, "the drag opposes a car moving along +X");
        }

        /* The probe is answered PER WHEEL, at that wheel's own sphere centre.
         *
         * Invisible on level ground over level water, and that is the trap: all
         * four wheels sit at the same height as each other, so a mutant handing
         * the probe c->body.x instead of the wheel centre survived every other
         * check in this file. It used to be worse -- gen_rb_data parked the com
         * ON the wheel-centre plane, so body origin and wheel centres were at the
         * same height too; CenterMassOY puts the com 57 / 29 / 39 mm lower, which
         * separates them by a constant. This check does not rely on that.
         *
         * A surface that varies across the car separates them. The real grid's
         * does -- that is what a shoreline is. */
        {
            float g[4], centre[3];
            int k, spread_ok = 1;
            settle(&c, 0);
            /* At a WHEEL CENTRE, not at c.body.x[1]. Those were the same height
               while the com was parked on the wheel-centre plane; CenterMassOY
               puts the body origin 57 mm lower, so using it left every wheel
               0.086 m clear of the surface, i.e. dry (in_water needs the gap
               under one radius, 0.072) -- the check then failed on its own
               precondition rather than on what it is about. */
            rb_wheel_frame(&c, 0, 0, centre, NULL, NULL, NULL);
            WATER_Y = centre[1];
            WATER_SLOPE_X = 0.2f;
            refresh_contacts(&c);
            for (k = 0; k < 4; k++) g[k] = c.hit[k].water_gap;
            printf("water sloped 0.2 dy/dx: gaps %+.4f %+.4f %+.4f %+.4f\n",
                   g[0], g[1], g[2], g[3]);
            for (k = 0; k < 4; k++) {
                rb_wheel_frame(&c, k, 0, centre, NULL, NULL, NULL);
                if (!c.hit[k].in_water) spread_ok = 0;
                /* each gap must match that wheel's own x, not the body's */
                if (fabsf(g[k] - (centre[1] - WATER_Y - 0.2f * centre[0]))
                    > 1e-4f)
                    spread_ok = 0;
            }
            ck(spread_ok, "each wheel's gap is measured at that wheel's centre");
            ck(fabsf(g[0] - g[1]) > 0.02f,
               "wheels on opposite sides see different water depths");
            WATER_SLOPE_X = 0.0f;
        }
        WATER_Y = -1e30f;
    }

    /* End to end: the same throttle, in and out of water. The point of the whole
       exercise is that water is SLOW. */
    {
        float dry_v, wet_v, centre_y;
        settle(&c, 0);
        for (i = 0; i < 600; i++) rbcar_step(&c, 1.0f, 0.0f, 0.0f, 0, 1.0f/60.0f);
        dry_v = rbcar_speed(&c);

        settle(&c, 0);
        centre_y = c.wheel[0].mount[1] - c.wheel[0].len + c.body.x[1];
        WATER_Y = centre_y;                 /* wheels submerged to the centre */
        for (i = 0; i < 600; i++) rbcar_step(&c, 1.0f, 0.0f, 0.0f, 0, 1.0f/60.0f);
        wet_v = rbcar_speed(&c);
        WATER_Y = -1e30f;
        printf("10 s at full throttle: %.3f m/s dry, %.3f m/s wading (%.0f%%)\n",
               dry_v, wet_v, 100.0 * wet_v / (dry_v > 0.0f ? dry_v : 1.0f));
        ck(dry_v > 6.0f, "the dry run still reaches the documented top speed");
        ck(wet_v < dry_v * 0.5f, "water more than halves the top speed");
    }
}

/* ================= part 7: the fixed timestep ========================= */
/*
 * The app used to hand rb_car_tick a MEASURED frame time. rb_car_tick cannot
 * always consume one: its substeps are capped at RB_MAX_SUBSTEP with a budget of
 * RB_MAX_SUBSTEPS, so 8/240 = 33.3 ms is the most world-time one call can
 * advance, and it returns how much it managed -- which the caller discarded. So
 * below 30 fps the car went into slow motion in proportion to the frame time.
 *
 * These checks pin the mechanism as well as the fix, because the fix is only
 * interesting if the starvation is real. The payoff check is bound to an
 * OBSERVABLE -- how far the car actually gets in a second of wall time -- rather
 * than to RBCAR_TICK_DT, so moving the constant cannot move the expectation.
 */

/* Drive `frames` frames of `frame_dt` through the fixed-step driver, counting
   ticks. Returns the tick total. */
static int drive_frames(rb_car *c, rbcar_clock *k, float frame_dt, int frames,
                        float throttle)
{
    int i, total = 0;
    for (i = 0; i < frames; i++) {
        int n = rbcar_step_frame(c, k, throttle, 0.0f, 0.0f, 0, frame_dt);
        total += (n > 0) ? n : 0;
    }
    return total;
}

/* Sum of |dx| and |dv| between two cars -- 0 when they are bit-identical. */
static double state_diff(const rb_car *a, const rb_car *b)
{
    double worst = 0.0, d;
    int i;
    for (i = 0; i < 3; i++) {
        d = fabs((double)a->body.x[i] - b->body.x[i]);
        if (d > worst) worst = d;
        d = fabs((double)a->body.v[i] - b->body.v[i]);
        if (d > worst) worst = d;
    }
    for (i = 0; i < 4; i++) {
        d = fabs((double)a->body.q[i] - b->body.q[i]);
        if (d > worst) worst = d;
    }
    return worst;
}

static void timestep_checks(void)
{
    rb_car a, b;
    rbcar_clock ka, kb;
    int i;

    puts("\n-- part 7: the fixed timestep (rbcar_step_frame) --");

    /* (1) THE MECHANISM. A single rb_car_tick cannot consume a 50 ms frame. */
    {
        float consumed, budget = RB_MAX_SUBSTEPS * RB_MAX_SUBSTEP;
        settle(&a, 0);
        for (i = 0; i < 60; i++)          /* get it rolling, so it is not asleep */
            rbcar_step(&a, 1.0f, 0.0f, 0.0f, 0, 1.0f / 60.0f);
        consumed = rb_car_tick(&a, 1.0f / 20.0f);
        printf("rb_car_tick(50 ms): simulated %.2f ms (budget %.2f ms)\n",
               (double)consumed * 1000.0, (double)budget * 1000.0);
        ck(!a.asleep, "the fixture is awake, so 'consumed' means what it says");
        ck(consumed < 1.0f / 20.0f - 1e-4f,
           "rb_car_tick really does starve on a 50 ms frame");
        ck(fabs(consumed - budget) < 1e-4f,
           "and it starves at exactly RB_MAX_SUBSTEPS * RB_MAX_SUBSTEP");

        /* At 60 fps it does NOT starve -- which is why this went unnoticed. */
        consumed = rb_car_tick(&a, 1.0f / 60.0f);
        ck(fabs(consumed - 1.0f / 60.0f) < 1e-5f,
           "a 60 fps frame is consumed in full");
    }

    /* (2) THE DRIVER'S CONTRACT, exactly: running a frame through
       rbcar_step_frame is indistinguishable from applying rbcar_step at
       RBCAR_TICK_DT the number of times it reports. Bit-exact, so it holds the
       driver to being pure bookkeeping and nothing else.

       Compared at equal TICK COUNTS, not equal wall time. 1/60 is not a binary
       fraction, so a 20 fps frame's accumulator carries residue and the two
       pacings can land one tick apart over a second -- which is quantisation,
       not frame dependence, and is measured as such in (3). */
    {
        int n = 0;
        settle(&b, 0); rbcar_clock_reset(&kb);
        for (i = 0; i < 40; i++) {         /* 40 frames at 20 fps */
            int t = rbcar_step_frame(&b, &kb, 1.0f, 0.0f, 0.0f, 0, 1.0f / 20.0f);
            n += (t > 0) ? t : 0;
        }
        settle(&a, 0);
        for (i = 0; i < n; i++)            /* the same tick count, by hand */
            rbcar_step(&a, 1.0f, 0.0f, 0.0f, 0, RBCAR_TICK_DT);
        printf("40 frames at 20 fps ran %d ticks; vs %d hand-run ticks, worst "
               "state difference %.3g\n", n, n, state_diff(&a, &b));
        ck(n > 0, "the 20 fps run actually ran some ticks");
        ck(state_diff(&a, &b) == 0.0,
           "rbcar_step_frame is exactly N applications of rbcar_step(TICK_DT)");
    }

    /* (3) THE PAYOFF, in observable terms: a second of wall time gets the car
       the same distance down the track at 20 fps as at 60.
     *
     * The residual is expressed in TICKS OF TRAVEL rather than as a percentage,
     * because one tick is the real and irreducible quantisation -- you cannot
     * run a fraction of one. Anything inside a tick or two is that; the old
     * path missed by seventeen. */
    {
        float d60, d20, d_old, tick_m;
        int n60, n20;
        settle(&a, 0); rbcar_clock_reset(&ka);
        n60 = drive_frames(&a, &ka, 1.0f / 60.0f, 60, 1.0f);
        d60 = a.body.x[2];
        tick_m = rbcar_speed(&a) * RBCAR_TICK_DT;   /* one tick, at the end */

        settle(&b, 0); rbcar_clock_reset(&kb);
        n20 = drive_frames(&b, &kb, 1.0f / 20.0f, 20, 1.0f);
        d20 = b.body.x[2];

        /* the old path, for comparison: rbcar_step with the raw frame time */
        settle(&b, 0);
        for (i = 0; i < 20; i++)
            rbcar_step(&b, 1.0f, 0.0f, 0.0f, 0, 1.0f / 20.0f);
        d_old = b.body.x[2];

        printf("1 s of wall time: 60 fps %.4f m (%d ticks), "
               "20 fps %.4f m (%d ticks), old 20 fps path %.4f m\n",
               (double)d60, n60, (double)d20, n20, (double)d_old);
        printf("            one tick of travel is %.4f m; 20 fps is %.1f ticks "
               "short, the old path %.1f\n", (double)tick_m,
               (double)(fabs(d60 - d20) / tick_m),
               (double)(fabs(d60 - d_old) / tick_m));
        ck(fabs(d20 - d60) <= 2.0f * tick_m,
           "20 fps lands within a tick or two of 60 fps");
        ck(fabs(d60 - d_old) > 10.0f * tick_m,
           "the old path really did fall far behind -- the bug was real");
    }

    /* (4) A frame shorter than a tick BANKS its time rather than losing it.
       A driver that dropped the remainder would pass check (2) and quietly run
       the world slow at any frame rate above 60. */
    {
        int n, first = -1, total = 0;
        settle(&a, 0); rbcar_clock_reset(&ka);
        for (i = 0; i < 8; i++) {          /* eight 240 Hz frames = two ticks */
            n = rbcar_step_frame(&a, &ka, 1.0f, 0.0f, 0.0f, 0, 1.0f / 240.0f);
            if (n > 0 && first < 0) first = i;
            total += (n > 0) ? n : 0;
        }
        printf("eight 1/240 frames: %d ticks, first at frame %d\n", total, first);
        ck(first == 3, "sub-tick frames bank their time and fire on the fourth");
        ck(total == 2, "eight 1/240 frames are worth exactly two ticks");
    }

    /* (5) The catch-up cap holds, and does not leave a backlog to burst on the
       next frame. */
    {
        int n, next;
        settle(&a, 0); rbcar_clock_reset(&ka);
        n = rbcar_step_frame(&a, &ka, 1.0f, 0.0f, 0.0f, 0, 0.5f);
        next = rbcar_step_frame(&a, &ka, 1.0f, 0.0f, 0.0f, 0, 1.0f / 60.0f);
        printf("a 500 ms frame: returned %d, the frame after it ran %d tick(s)\n",
               n, next);
        ck(n < 0, "an over-long frame reports that it was clipped");
        ck(next == 1, "and leaves no banked backlog to burst with");
    }

    /* (6) A zero dt runs nothing -- this is how the menu freezes the world. */
    {
        float y0;
        settle(&a, 0); rbcar_clock_reset(&ka);
        y0 = a.body.x[2];
        for (i = 0; i < 30; i++)
            ck(rbcar_step_frame(&a, &ka, 1.0f, 0.0f, 0.0f, 0, 0.0f) == 0,
               "dt = 0 runs no ticks");
        ck(a.body.x[2] == y0, "and moves the car not at all");
    }

    /* (7) rbcar_clock_reset really does drop the banked time.
       Added because a mutation stubbing it out survived everything above: the
       reset is what stops a track load's stall being spent as a burst of ticks
       at the new spawn, and nothing was holding it to that. Bank three quarters
       of a tick, reset, and the next quarter-tick frame must NOT fire. */
    {
        int fired = 0;
        settle(&a, 0); rbcar_clock_reset(&ka);
        for (i = 0; i < 3; i++)
            rbcar_step_frame(&a, &ka, 1.0f, 0.0f, 0.0f, 0, RBCAR_TICK_DT / 4.0f);
        rbcar_clock_reset(&ka);
        for (i = 0; i < 3; i++)
            if (rbcar_step_frame(&a, &ka, 1.0f, 0.0f, 0.0f, 0,
                                 RBCAR_TICK_DT / 4.0f) > 0)
                fired = 1;
        printf("banked 3/4 of a tick, reset, then 3/4 again: ticks fired = %d\n",
               fired);
        ck(!fired, "rbcar_clock_reset drops the banked time");
    }
}

/* ------------------------------------------------------------------------- */
/* part 8: the boost meter (FUN_004f3800, capacity FUN_0050b7f0)             */
/* ------------------------------------------------------------------------- */
/*
 * `in.boost` used to be the button wired straight through, so boost was
 * unlimited and the [BOOSTERS] upgrade reached nothing at all. What the meter
 * has to be is FINITE and UPGRADEABLE, so these checks are bound to what the
 * driver gets out of it -- how long one burn lasts, how much of a minute is
 * spent boosting, how far the car travels -- and not to the meter's own
 * constants. Asserting `tank == capacity` would pass against any capacity, and
 * this suite has shipped that mistake six times; see CLAUDE.md.
 */

/* Drive flat out for `secs` with the boost button in `button`. Returns the
 * number of ticks the ENGINE was boosting -- c->in.boost, which is what
 * carEngineAccel reads -- and reports the first continuous burn, the distance
 * covered, and the boosting ticks in the SECOND HALF of the window.
 *
 * That last one is the refill rate on its own. Once the meter the car started
 * with has been spent, every later burn begins from the re-arm threshold, so
 * what is left is a duty cycle set by how fast the meter fills -- and a mutant
 * that scales only the CAPACITY by the booster level survives every check that
 * looks at the first burn or at the whole window. */
static int boost_run(int car, int level, int button, float secs,
                     float *first_out, float *dist_out, int *tail_out)
{
    rb_car c;
    int i, n = (int)(secs * 60.0f + 0.5f), on = 0, first = 0, ended = 0, tail = 0;
    float x0, z0;

    settle(&c, car);
    c.boost_upgrade = level;
    rb_boost_reset(&c);
    x0 = c.body.x[0];
    z0 = c.body.x[2];

    for (i = 0; i < n; i++) {
        rbcar_step(&c, 1.0f, 0.0f, 0.0f, button, 1.0f / 60.0f);
        if (c.in.boost) {
            on++;
            if (i >= n / 2) tail++;
            if (!ended) first++;
        } else if (first) {
            ended = 1;
        }
    }
    if (tail_out)
        *tail_out = tail;
    if (first_out)
        *first_out = first / 60.0f;
    if (dist_out) {
        double dx = (double)c.body.x[0] - x0, dz = (double)c.body.x[2] - z0;
        *dist_out = (float)sqrt(dx * dx + dz * dz);
    }
    return on;
}

static void boost_checks(void)
{
    rb_car c;
    int lvl, on[4], tail[4], rising;
    float first[4], dist_boost, dist_plain, junk;

    puts("\n-- part 8: the boost meter (0x004f3800) --");

    /* (1) THE CAPACITY ENDPOINTS. FUN_0050b7f0 clamps below its reference range
       and at the top of it, and the shipped data lands exactly on both ends --
       which is the evidence that the remap was read the right way round. Those
       two values are the ones interpolation cannot produce. */
    printf("capacity  Overkill %.0f %.0f %.0f %.0f   Buggy %.0f %.0f %.0f %.0f\n",
           (double)rb_boost_capacity(&RB_CARS[0].tune, 0),
           (double)rb_boost_capacity(&RB_CARS[0].tune, 1),
           (double)rb_boost_capacity(&RB_CARS[0].tune, 2),
           (double)rb_boost_capacity(&RB_CARS[0].tune, 3),
           (double)rb_boost_capacity(&RB_CARS[1].tune, 0),
           (double)rb_boost_capacity(&RB_CARS[1].tune, 1),
           (double)rb_boost_capacity(&RB_CARS[1].tune, 2),
           (double)rb_boost_capacity(&RB_CARS[1].tune, 3));
    ck(rb_boost_capacity(&RB_CARS[1].tune, 0) == RB_BOOST_CAP_MIN,
       "the weakest booster in the game sits on the bottom of the range");
    ck(rb_boost_capacity(&RB_CARS[1].tune, 3) == RB_BOOST_CAP_MAX,
       "the strongest sits on the top of it");
    ck(rb_boost_capacity(&RB_CARS[0].tune, 0) == RB_BOOST_CAP_MIN,
       "and a booster below the range is clamped, not extrapolated");
    for (lvl = 1; lvl < 4; lvl++)
        ck(rb_boost_capacity(&RB_CARS[0].tune, lvl)
           > rb_boost_capacity(&RB_CARS[0].tune, lvl - 1),
           "capacity rises with the booster level");

    /* (2) A car arrives with the meter its booster buys, not the level-0 meter
       it was constructed with. */
    settle(&c, 0);
    c.boost_upgrade = 3;
    rb_boost_reset(&c);
    ck(c.boost_tank == rb_boost_capacity(&RB_CARS[0].tune, 3),
       "rb_boost_reset fills to the level actually fitted");

    /* (3) THE GATE. The button is not boost: the original ANDs it with the
       throttle at its own call site (0x004f7032), so a coasting car cannot
       boost however hard the button is held. */
    ck(boost_run(0, 3, 1, 3.0f, NULL, NULL, NULL) > 0,
       "throttle + button does boost");
    {
        rb_car d;
        int i, idle = 0;
        settle(&d, 0);
        d.boost_upgrade = 3;
        rb_boost_reset(&d);
        for (i = 0; i < 180; i++) {
            rbcar_step(&d, 0.0f, 0.0f, 0.0f, 1, 1.0f / 60.0f);
            idle += d.in.boost ? 1 : 0;
        }
        printf("button held 3 s with no throttle: %d boosting ticks, "
               "meter %.1f\n", idle, (double)d.boost_tank);
        ck(idle == 0, "the button alone does nothing -- boost needs the throttle");
        ck(d.in.boost_button == 1,
           "but the raw action stays visible to the rest clamp");
        ck(d.boost_tank > rb_boost_capacity(&RB_CARS[0].tune, 3) - 0.001f,
           "and a meter that never fired is still full");
    }

    /* (4) IT IS FINITE -- the whole of the bug. Held down, the burn must stop.
       Bound to "less than the window", which no choice of constants satisfies
       if the drain is disconnected. */
    for (lvl = 0; lvl < 4; lvl++)
        on[lvl] = boost_run(0, lvl, 1, 120.0f, &first[lvl], NULL, &tail[lvl]);
    puts("Overkill, 120 s flat out with boost held:");
    for (lvl = 0; lvl < 4; lvl++)
        printf("   level %d: first burn %5.2f s, %6.2f s boosting of 120, "
               "%5.2f s in the last 60 (steady state)\n",
               lvl + 1, (double)first[lvl], on[lvl] / 60.0, tail[lvl] / 60.0);
    for (lvl = 0; lvl < 4; lvl++) {
        ck(on[lvl] < 120 * 60, "the meter runs out -- boost is not unlimited");
        ck(first[lvl] > 0.5f && first[lvl] < 30.0f,
           "and one burn is a usable length, not a frame and not forever");
    }

    /* (5) THE PAYOFF, and the check that dies if the upgrade is unwired: a
       better booster buys strictly more boost, both in one burn (a bigger
       meter) and over a minute (a faster refill). Ordering, not magnitudes --
       nothing here is satisfied by copying a constant out of rb_data.h. */
    rising = 1;
    for (lvl = 1; lvl < 4; lvl++)
        if (!(first[lvl] > first[lvl - 1]) || !(on[lvl] > on[lvl - 1]))
            rising = 0;
    ck(rising, "every booster level buys a longer burn AND more boost overall");
    ck(on[3] * 4 > on[0] * 5,
       "and the range is worth having -- level 4 beats level 1 by over 25%");

    /* The upgrade has TWO halves -- a bigger meter and a faster refill -- and
       everything above sees only the first, because all of it is dominated by
       the tank the car starts with. Scaling the capacity alone and leaving the
       fill rate on level 1 passes every one of those checks; it was a live
       mutant. This is the other half on its own: once the starting tank is
       spent, every later burn begins from the same re-arm threshold, so what is
       left is a duty cycle, and a duty cycle can only rise if the RATE does. */
    rising = 1;
    for (lvl = 1; lvl < 4; lvl++)
        if (!(tail[lvl] > tail[lvl - 1]))
            rising = 0;
    ck(rising, "and it refills faster too -- the steady-state duty cycle rises");
    ck(tail[3] * 5 > tail[0] * 6,
       "measurably so, not by a tick or two");

    /* (6) IT REACHES THE ENGINE. carEngineAccel is the only consumer, so the
       proof is on the ground: the same car over the same window gets further
       with the button held. */
    (void)boost_run(0, 3, 1, 10.0f, &junk, &dist_boost, NULL);
    (void)boost_run(0, 3, 0, 10.0f, &junk, &dist_plain, NULL);
    printf("10 s flat out: %.2f m boosting, %.2f m plain\n",
           (double)dist_boost, (double)dist_plain);
    ck(dist_boost > dist_plain + 0.5f,
       "boost actually moves the car -- in.boost reaches the engine");

    /* (7) IT COMES BACK. Run it dry, let go, and the meter refills -- one burn
       a race is the failure mode on the other side of (4). */
    {
        rb_car d;
        int i;
        float low;
        settle(&d, 0);
        d.boost_upgrade = 3;
        rb_boost_reset(&d);
        for (i = 0; i < 60 * 60; i++)          /* burn it down */
            rbcar_step(&d, 1.0f, 0.0f, 0.0f, 1, 1.0f / 60.0f);
        low = d.boost_tank;
        for (i = 0; i < 30 * 60; i++)          /* then coast for 30 s */
            rbcar_step(&d, 0.0f, 0.0f, 0.0f, 0, 1.0f / 60.0f);
        printf("after a minute of burning: %.1f units, +30 s idle: %.1f\n",
               (double)low, (double)d.boost_tank);
        ck(d.boost_tank > low + 1.0f, "the meter refills when it is left alone");
        ck(d.boost_tank <= rb_boost_capacity(&RB_CARS[0].tune, 3) + 0.001f,
           "and never past its capacity");
    }
}

int main(void)
{
    rb_car c;
    float y[RB_STATE_N];
    int i;
    float ride;

    /* ================= part 1: rigid-body core ======================== */
    puts("-- rigid body core --");

    body_init(&c, 2.0f, 0.4f, 0.2f, 0.8f);
    for (i = 0; i < 600; i++) rb_car_tick(&c, 1.0f / 600.0f);
    printf("free fall  1s: y=%+.4f  vy=%+.4f\n", c.body.x[1], c.body.v[1]);
    ck(fabs(c.body.v[1] + 10.0f) < 1e-3, "gravity = 10 m/s^2");

    body_init(&c, 2.0f, 0.4f, 0.2f, 0.8f);
    c.tune.coeff_air_resistance = 0.0323f;
    c.tune.coeff_friction_bearings = 0.202f;
    for (i = 0; i < 60 * 600; i++) rb_car_tick(&c, 1.0f / 600.0f);
    {
        double want = sqrt((2.0 * 10.0 - 0.202) / 0.0323);
        printf("drag term. vel: |vy|=%.4f (analytic %.4f)\n",
               fabs(c.body.v[1]), want);
        ck(fabs(fabs(c.body.v[1]) - want) < 0.05, "drag terminal velocity");
    }

    body_init(&c, 2.0f, 0.4f, 0.2f, 0.8f);
    rb_car_get_state(&c, y);
    y[10] = 0.30f; y[11] = 0.10f; y[12] = -0.20f;
    rb_car_set_state(&c, y);
    for (i = 0; i < 3 * 600; i++) {
        c.body.mass = 0.0f;
        rb_car_tick(&c, 1.0f / 600.0f);
        c.body.mass = 2.0f;
    }
    {
        double qn = sqrt((double)c.body.q[0]*c.body.q[0] + (double)c.body.q[1]*c.body.q[1]
                       + (double)c.body.q[2]*c.body.q[2] + (double)c.body.q[3]*c.body.q[3]);
        double ln = sqrt((double)c.body.L[0]*c.body.L[0] + (double)c.body.L[1]*c.body.L[1]
                       + (double)c.body.L[2]*c.body.L[2]);
        printf("spin 3s: |q|=%.7f |L|=%.6f\n", qn, ln);
        ck(fabs(qn - 1.0) < 1e-5, "quaternion stays unit");
        ck(fabs(ln - 0.3741657) < 1e-5, "angular momentum conserved");
    }

    body_init(&c, 2.0f, 0.4f, 0.2f, 0.8f);
    {
        float tau[3] = { 0.7f, -1.3f, 0.45f };
        float f[2][3], p[2][3];
        rb_torque_to_couple(&c.body, tau, f, p);
        rb_sum_forces_torques(&c.body, (const float (*)[3])p, 2,
                              (const float (*)[3])f);
        printf("couple: in (%.3f %.3f %.3f) out (%.3f %.3f %.3f) F=(%.1g %.1g %.1g)\n",
               tau[0], tau[1], tau[2], c.body.torque[0], c.body.torque[1],
               c.body.torque[2], c.body.force[0], c.body.force[1], c.body.force[2]);
        for (i = 0; i < 3; i++) ck(fabs(c.body.torque[i] - tau[i]) < 1e-4, "couple torque");
        for (i = 0; i < 3; i++) ck(fabs(c.body.force[i]) < 1e-5, "couple net force");
    }

    /* ================= part 2: tires, engine, contacts ================ */
    puts("\n-- tires / engine / contacts --");

    car_init(&c);
    {
        rb_curve cv; cv.pt = OVERKILL_ACCEL; cv.n = 3;
        printf("curve: x=-5 -> %.3f  x=0 -> %.3f  x=8 -> %.3f  x=24 -> %.3f  x=40 -> %.3f\n",
               rb_curve_eval(&cv, -5), rb_curve_eval(&cv, 0), rb_curve_eval(&cv, 8),
               rb_curve_eval(&cv, 24), rb_curve_eval(&cv, 40));
        ck(fabs(rb_curve_eval(&cv, -5) - 6.0f) < 1e-5, "curve clamps below");
        ck(fabs(rb_curve_eval(&cv,  8) - 4.5f) < 1e-5, "curve interpolates");
        ck(fabs(rb_curve_eval(&cv, 24) - 1.5f) < 1e-5, "curve interpolates 2");
        ck(fabs(rb_curve_eval(&cv, 40) - 0.0f) < 1e-5, "curve clamps above");
    }

    printf("grip: front=%.4f rear=%.4f  (0.90 * 0.70 = 0.6300)\n",
           rb_tire_grip(&c, 0), rb_tire_grip(&c, 1));
    ck(fabs(rb_tire_grip(&c, 0) - 0.63f) < 1e-4, "front grip");
    ck(fabs(rb_tire_grip(&c, 1) - 0.63f) < 1e-4, "rear grip");
    c.in.brake = 1; c.steer = 20.0f;
    printf("drifting: %d  rear grip drops to %.5f\n",
           rb_tire_drifting(&c), rb_tire_grip(&c, 1));
    ck(rb_tire_drifting(&c) == 1, "handbrake + steer starts a drift");
    ck(fabs(rb_tire_grip(&c, 1) - 0.63f * c.tune.coeff_drift_rear) < 1e-4,
       "drift cuts rear grip");
    c.in.brake = 0; c.steer = 0.0f;

    /* engine: standing start, full throttle */
    c.in.accel = 1; c.in.throttle = 1.0f;
    {
        float rst = 0.0f;
        float a = rb_engine_accel(&c, &rst);
        /* curve at x=0 is 6.0, * resonator_accel[0]=0.70 -> 4.20, plus the
           bearing term the body will subtract (0.202) */
        /* curve(0)=6.0 * resonator_accel[0]=0.70 -> 4.20. No drag is added
           back at zero speed: carDragForce's magnitude is only computed above
           0.0001 m/s. */
        printf("engine at rest: a=%.4f restrict=%.4f  (want 4.20)\n", a, rst);
        ck(fabs(a - 4.20f) < 2e-3, "engine accel at rest");
        ck(fabs(rst - 2.0f) < 1e-4, "restrict curve at rest");
    }
    /* engine: above the limiter */
    c.body.v[2] = 20.0f;    /* forward is local +Z; identity pose */
    rb_car_set_state(&c, (rb_car_get_state(&c, y), y));
    c.body.v[2] = 20.0f;
    printf("engine at 20 m/s (limiter is %.2f km/h -> %.2f m/s): a=%.4f\n",
           27.0f * 0.92f, 27.0f * 0.2777778f * 0.92f, rb_engine_accel(&c, NULL));
    ck(rb_engine_accel(&c, NULL) == 0.0f, "speed limiter cuts drive");
    c.body.v[2] = 0.0f;

    /* reverse taper */
    c.gear = -1; c.in.accel = 0; c.in.brake = 1; c.in.brake_amount = 1.0f;
    {
        float r0, r3, r4;
        c.body.v[1] = 0.0f;  r0 = rb_engine_accel(&c, NULL);
        c.body.v[1] = -3.0f; r3 = rb_engine_accel(&c, NULL);
        c.body.v[1] = -4.0f; r4 = rb_engine_accel(&c, NULL);
        c.body.v[1] = 0.0f;
        printf("reverse: 0 m/s a=%.3f, 3.0 m/s a=%.3f, 4 m/s a=%.3f\n", r0, r3, r4);
        ck(fabs(r0 + 4.0f) < 1e-4, "reverse accel at rest");
        ck(fabs(r3 + 2.6667f) < 1e-3, "reverse tapers");
        ck(r4 == 0.0f, "reverse cuts out above 3.33 m/s");
    }
    c.gear = 0; c.in.brake = 0;

    /* contact frame orthonormality, and steering turning it */
    car_init(&c);
    c.world = &FLAT_WORLD;
    rb_car_update_matrix(&c);
    for (i = 0; i < 600; i++) step_car(&c, 1.0f / 600.0f);
    {
        rb_contact r0, r1;
        double d;
        ck(c.hit[0].active != 0, "wheel 0 is in contact after settling");
        rb_contact_record(&c, 0, 0.0f, &r0);
        rb_contact_record(&c, 0, 30.0f, &r1);
        d = r0.lat[0]*r0.normal[0] + r0.lat[1]*r0.normal[1] + r0.lat[2]*r0.normal[2];
        ck(fabs(d) < 1e-5, "lat perpendicular to normal");
        d = r0.fwd[0]*r0.normal[0] + r0.fwd[1]*r0.normal[1] + r0.fwd[2]*r0.normal[2];
        ck(fabs(d) < 1e-5, "fwd perpendicular to normal");
        d = r0.fwd[0]*r0.lat[0] + r0.fwd[1]*r0.lat[1] + r0.fwd[2]*r0.lat[2];
        ck(fabs(d) < 1e-5, "fwd perpendicular to lat");
        printf("frame straight: fwd=(%.3f %.3f %.3f) lat=(%.3f %.3f %.3f)\n",
               r0.fwd[0], r0.fwd[1], r0.fwd[2], r0.lat[0], r0.lat[1], r0.lat[2]);
        printf("frame 30 deg  : fwd=(%.3f %.3f %.3f)\n",
               r1.fwd[0], r1.fwd[1], r1.fwd[2]);
        ck(fabs(r0.fwd[2] - 1.0f) < 1e-4, "straight ahead is +Z");
        ck(fabs(r1.fwd[0] - 0.5f) < 1e-3, "30 deg steer rotates fwd by 30 deg");
    }

    {
        float A[2][2] = { { 4.0f, 1.0f }, { 2.0f, 3.0f } };
        float b[2] = { 9.0f, 11.0f }, f[2];
        int ok = rb_solve2((const float (*)[2])A, b, f);
        printf("solve2: f=(%.4f %.4f) (want 1.6 2.6)\n", f[0], f[1]);
        ck(ok && fabs(f[0] - 1.6f) < 1e-4 && fabs(f[1] - 2.6f) < 1e-4, "2x2 solve");
    }

    /* ================= part 3: driving on flat ground ================= */
    puts("\n-- driving --");

    car_init(&c);
    c.world = &FLAT_WORLD;
    rb_car_update_matrix(&c);
    for (i = 0; i < 3 * 600; i++) step_car(&c, 1.0f / 600.0f);
    ride = ride_height(&c);
    printf("settle 3s: ride height y=%.4f  spring len=%.4f (free %.4f, sag %.4f)"
           "  k_pos=%.3f  speed=%.5f\n",
           c.body.x[1], c.wheel[0].len, c.wheel[0].len_free, c.wheel[0].sag,
           c.wheel[0].k_pos, speed_of(&c));
    /* 2 mm, not 20. The spring's own force balance puts equilibrium at
       len = len_free - sag exactly, so a settled car IS the check on everything
       that shifts the length away from it -- including len_extra, which
       0x004fbe60 re-asserts every substep as radius * 0.02 (1.4 mm on the
       Overkill) and which nothing in this port wrote at all until it was
       recovered. A 20 mm window was wide enough to hide it fourteen times over. */
    printf("   compression %.5f vs configured sag %.5f  (len_extra %.5f)\n",
           c.wheel[0].len_free - c.wheel[0].len, c.wheel[0].sag,
           c.wheel[0].len_extra);
    ck(fabs(c.wheel[0].len_free - c.wheel[0].len - c.wheel[0].sag) < 0.002f,
       "static compression equals the configured sag");
    /* This one guards the WRITE, not the value, and that is deliberate. The 0.02
       is a recovered constant and comparing against it proves nothing about
       whether it is right -- but the bug it stands guard over was that NOTHING in
       this port wrote len_extra at any point, so every spring read 1.4 mm of
       slack that the original does not have. It is a collision result with a zero
       derivative in the ODE state, so an unwritten field is invisible: it just
       sits at whatever init left. 0x004fbe60 re-asserts it every substep. */
    ck(c.wheel[0].len_extra > 0.0f
       && fabs((double)c.wheel[0].len_extra - c.wheel[0].radius * 0.02) < 1e-6,
       "len_extra is being written each substep (0x004fbe60)");
    ck(fabs(c.body.x[1] - ride) < 0.01f, "car rests on its springs at the geometric ride height");
    ck(speed_of(&c) < 0.05f, "car at rest stays at rest");

    c.in.accel = 1; c.in.throttle = 1.0f;
    for (i = 0; i < 10 * 600; i++) step_car(&c, 1.0f / 600.0f);
    printf("10s full throttle: speed=%.3f m/s (%.1f km/h), pos=(%.2f %.2f %.2f)\n",
           speed_of(&c), speed_of(&c) * 3.6f, c.body.x[0], c.body.x[1], c.body.x[2]);
    printf("   limiter is %.2f km/h = %.3f m/s\n",
           27.0f * 0.92f, 27.0f * 0.2777778f * 0.92f);
    ck(speed_of(&c) > 1.0f, "throttle accelerates the car");
    ck(speed_of(&c) < 8.0f, "speed settles at the limiter, not beyond");
    ck(c.body.x[1] > 0.0f, "car stays on the ground under power");

    {
        float straight = speed_of(&c);
        float yaw_before = c.body.w[1];
        c.steer = 25.0f;
        for (i = 0; i < 2 * 600; i++) step_car(&c, 1.0f / 600.0f);
        printf("2s at 25 deg steer: yaw rate %+.4f -> %+.4f rad/s, speed %.3f -> %.3f\n",
               yaw_before, c.body.w[1], straight, speed_of(&c));
        ck(fabs(c.body.w[1]) > 0.05f, "steering produces yaw");
    }

    /* ---- landing: conservative advancement, then settling --------------
     *
     * From 0.4 m the impact speed is sqrt(2*10*0.4) = 2.8 m/s, which at dt = 1/60
     * moves 0.047 m per frame -- about two thirds of a wheel radius, squarely in
     * the regime where a plain Euler step walks through the ground. This is what
     * the 0.9-radius conservative-advancement cap is for.
     */
    car_init(&c);
    c.world = &FLAT_WORLD;
    c.body.x[1] = ride + 0.30f;
    rb_car_update_matrix(&c);
    {
        float lowest = 1e30f;
        for (i = 0; i < 300; i++) {          /* 5 s at 60 Hz */
            step_car(&c, 1.0f / 60.0f);
            if (c.body.x[1] < lowest) lowest = c.body.x[1];
        }
        printf("drop 0.40 m, dt=1/60: lowest y=%.4f  after 5 s y=%.4f  speed=%.4f\n"
               "   (static ride height is %.4f)\n",
               lowest, c.body.x[1], speed_of(&c), ride);
        ck(lowest > ride - 0.20f, "conservative advancement stops the car tunnelling");
        /* Was ride - 0.12: the landing used to recover to ~0.11 against a 0.17
           static height and creep there, because the port had no normal impulse
           of its own. With rb_coll_resolve transcribed it comes back to within a
           millimetre, so this asks for a millimetre. */
        ck(c.body.x[1] > ride - 0.005f, "car returns to its ride height after landing");
        ck(speed_of(&c) < 1.0f, "car is not still bouncing after landing");
    }

    /* A harder drop bottoms the struts out. The car must still not pass through
     * the floor -- that is the `stuck` path refusing the substep -- and it must
     * come back UP to its ride height rather than creeping there off the bump
     * stops, which is what it did while the normal impulse was invented.
     */
    car_init(&c);
    c.world = &FLAT_WORLD;
    c.body.x[1] = ride + 1.40f;
    rb_car_update_matrix(&c);
    {
        float lowest = 1e30f;
        for (i = 0; i < 600; i++) {
            step_car(&c, 1.0f / 60.0f);
            if (c.body.x[1] < lowest) lowest = c.body.x[1];
        }
        printf("drop 1.50 m (bottoms out): lowest y=%.4f  after 10 s y=%.4f\n",
               lowest, c.body.x[1]);
        ck(lowest > ride - 0.25f, "a hard landing does not fall through the floor");
        ck(c.body.x[1] > lowest + 0.01f, "car climbs back off the bump stops");
        ck(c.body.x[1] > ride - 0.005f, "and all the way back to its ride height");
    }

    /* ---- lateral grip actually bites ----------------------------------
     *
     * A car shoved sideways at 3 m/s must be stopped by the tires, not coast.
     * This is the check that catches the geometry going wrong: the vertical lever
     * arm from the centre of mass to the contact patch enters the friction
     * matrix as ry^2 * Izz_inv and dominates it, so a wrong COM height or a
     * body-box-only inertia tensor makes the solve stiff and the grip feeble.
     * With the mesh geometry this decelerates at about 5 m/s^2, in line with a
     * friction coefficient of 0.63.
     */
    car_init(&c);
    c.world = &FLAT_WORLD;
    rb_car_update_matrix(&c);
    for (i = 0; i < 120; i++) step_car(&c, 1.0f / 600.0f);
    {
        float y[RB_STATE_N], v0;
        rb_car_get_state(&c, y);
        y[7] = 3.0f * c.body.mass;          /* 3 m/s sideways */
        rb_car_set_state(&c, y);
        v0 = c.body.v[0];
        for (i = 0; i < 30; i++) step_car(&c, 1.0f / 60.0f);
        printf("shoved sideways at %.2f m/s: after 0.5 s vx=%.3f, slid %.3f m\n",
               v0, c.body.v[0], c.body.x[0]);
        ck(c.body.v[0] < 1.0f, "lateral grip stops a sideways slide");
        ck(c.body.x[0] < 1.5f, "it does not slide far");
    }

    /* ---- slope stability: the substep rate ------------------------------
     *
     * The roll mode is unstable at one Euler step per 60 Hz frame. Measured on a
     * perfect plane, a 2 degree slope used to diverge to 31 degrees of tilt error
     * and 12 rad/s; with the substep held at RB_MAX_SUBSTEP it settles. Only
     * exactly-flat ground was ever stable before, because nothing excited roll.
     *
     * The mean tilt error and the whole-run worst |w| are NOT enough to say the
     * car is still: a steady oscillation about the right attitude passes both.
     * This bug shipped through exactly that gap -- the suspension ramp collapse
     * (see rb_susp_ramp_reset in collide.c) left a parked car swinging 5 degrees
     * peak to peak forever while this test reported a 3 degree mean and passed.
     * So the tail window also bounds the PEAK-TO-PEAK tilt and the |w| still
     * present after the transient has died.
     */
    {
        const float DEGS[4] = { 0.5f, 2.0f, 5.0f, 10.0f };
        int t;
        for (t = 0; t < 4; t++) {
            double r = (double)DEGS[t] * 0.017453292;
            float worst_w = 0.f, tail_e = 0.f;
            float tail_lo = 1e30f, tail_hi = -1e30f, tail_w = 0.f;
            int n = 0;
            TILT_N[0] = (float)sin(r);
            TILT_N[1] = (float)cos(r);
            TILT_N[2] = 0.0f;
            car_init(&c);
            for (i = 0; i < 600; i++) {
                step_car(&c, 1.0f / 60.0f);
                {
                    const float *up = &c.m[4];
                    double dn = (double)up[0]*TILT_N[0] + (double)up[1]*TILT_N[1]
                              + (double)up[2]*TILT_N[2];
                    double e = acos(dn > 1.0 ? 1.0 : (dn < -1.0 ? -1.0 : dn)) * 57.295776;
                    double w = sqrt((double)c.body.w[0]*c.body.w[0]
                                  + (double)c.body.w[1]*c.body.w[1]
                                  + (double)c.body.w[2]*c.body.w[2]);
                    if (w > worst_w) worst_w = (float)w;
                    if (i > 480) {
                        tail_e += (float)e; n++;
                        if (e < tail_lo) tail_lo = (float)e;
                        if (e > tail_hi) tail_hi = (float)e;
                        if (w > tail_w) tail_w = (float)w;
                    }
                }
            }
            if (n) tail_e /= (float)n;
            printf("slope %5.2f deg: worst |w|=%5.2f  settled tilt error=%5.2f deg"
                   "  tail tilt p2p=%5.2f deg  tail |w|=%5.2f\n",
                   DEGS[t], worst_w, tail_e, tail_hi - tail_lo, tail_w);
            ck(worst_w < 6.0f, "slope does not spin the car up");
            ck(tail_e < 8.0f, "car settles near the surface attitude");
            ck(tail_hi - tail_lo < 1.0f, "a parked car does not keep swinging");
            ck(tail_w < 0.5f, "a parked car's angular velocity dies away");
        }
        TILT_N[0] = 0.0f; TILT_N[1] = 1.0f; TILT_N[2] = 0.0f;
    }

    /* ---- a DRIVING car must not porpoise -------------------------------
     *
     * Flat out on flat ground there is nothing to bounce off, so the ride height
     * should be near constant. For a long time it was not: the car ran a
     * sustained 1.5 Hz limit cycle, 131 mm peak to peak, the front strut swinging
     * from near-droop to bottomed on len_min every cycle. The parked checks above
     * all passed throughout -- a car can be perfectly still standing and unusable
     * moving, so this has to be measured separately.
     *
     * FIXED, by putting the advance back where carPhysTick has it. The damping
     * term is k_speed * dlen / dt, and `dlen` is whatever the last suspension pass
     * moved the length by; solving the suspension BEFORE the step, as this port
     * used to, meant the pose had not changed since the previous substep already
     * solved it, so `dlen` was not the body's motion at all but just the extension
     * rate limit -- pinned at 10*dt*radius = 2.992 mm frame after frame. A
     * suspension with a paper damping ratio of 0.46 therefore ran undamped. It now
     * measures 0.5 mm here, and 1.46 Hz is this spring's own natural frequency, so
     * a return to tens of mm means the damper has been disconnected again.
     */
    {
        float lo = 1e30f, hi = -1e30f, worst_tilt = 0.f, spd;
        car_init(&c);
        for (i = 0; i < 900; i++) {
            c.in.throttle = 1.0f;
            c.in.accel = 1;
            step_car(&c, 1.0f / 60.0f);
            if (i > 420) {                       /* well past the launch */
                const float *up = &c.m[4];
                float t = acosf(up[1] > 1.f ? 1.f : (up[1] < -1.f ? -1.f : up[1]))
                          * 57.295776f;
                if (c.body.x[1] < lo) lo = c.body.x[1];
                if (c.body.x[1] > hi) hi = c.body.x[1];
                if (t > worst_tilt) worst_tilt = t;
            }
        }
        spd = sqrtf(c.body.v[0]*c.body.v[0] + c.body.v[2]*c.body.v[2]);
        printf("driving flat out on flat ground: ride p2p=%.1f mm  worst tilt=%.1f"
               " deg  speed=%.3f m/s\n",
               (hi - lo) * 1000.f, worst_tilt, spd);
        /* 10 mm, not 150: the bound is now set where a damped suspension belongs
           rather than where the limit cycle used to sit. It measures 0.5 mm. */
        ck((hi - lo) < 0.010f, "a driving car does not porpoise on flat ground");
        ck(worst_tilt < 4.0f, "and does not pitch itself about either");
        ck(spd > 6.5f && spd < 7.3f, "top speed is still the limiter's");
    }

    /* ---- a rolled car stays in the world -------------------------------
     *
     * The suspension acts along the body Y axis through the WHEEL spheres, so
     * once the car is far enough over for the wheels not to reach, the model has
     * no upward force anywhere. Measured before rb_body_contact_stop existed: a
     * modest angular kick put the car 270+ metres under the terrain on every
     * track tried, 12 out of 12 -- including rolls that ended nearly upright,
     * because the car went under DURING the roll.
     */
    {
        int flipped;
        for (flipped = 0; flipped < 2; flipped++) {
            float worst = 0.f;
            car_init(&c);
            if (flipped) {
                /* upside down: 180 degrees about Z */
                c.body.q[0] = 0.f; c.body.q[1] = 0.f;
                c.body.q[2] = 0.f; c.body.q[3] = 1.f;
                c.body.x[1] = 0.40f;
            } else {
                c.body.L[2] = 0.6f;              /* kick it into a roll */
            }
            {
                float y[RB_STATE_N];
                rb_car_get_state(&c, y);
                rb_car_set_state(&c, y);
            }
            for (i = 0; i < 900; i++) {
                step_car(&c, 1.0f / 60.0f);
                if (-c.body.x[1] > worst) worst = -c.body.x[1];
            }
            printf("%s: deepest below the surface %.3f m, final y=%.3f\n",
                   flipped ? "dropped upside down" : "kicked into a roll",
                   worst, c.body.x[1]);
            ck(worst < 0.5f, "a rolled car does not sink through the ground");
            ck(c.body.x[1] > -1.0f, "and is still in the world afterwards");

            /* ...but "still in the world" was ALL this asked, and that let a
             * plainly wrong answer through for a long time: dropped from y = 0.40
             * onto flat ground, an inverted car did not fall at all. It ended at
             * 0.441 -- 41 mm HIGHER than it started, and about 0.39 m above the
             * roof it should be lying on, with a body half-height of 0.053 -- and
             * was still drifting at 0.042 m/s after fifteen seconds. The Buggy
             * settled at 0.303 and the Hummer at -0.061, under the ground.
             *
             * That was the reported "if it flips it starts to float and moves
             * randomly", and the cause was the invented physics rather than the
             * model: inverted, the wheels point at the sky, the suspension (the
             * only upward force in the transcribed model) can never engage, and
             * all that was left was a positional projection that deleted inward
             * momentum. It had no friction, so tangential velocity never decayed,
             * and it never touched ANGULAR momentum, so the car kept turning.
             *
             * The real solve is transcribed now -- rb_coll_friction (0x004f0560)
             * and rb_coll_resolve (0x004f0750), applying proper impulses at the
             * contact points through 0x004754a0's denominator, which is what lets
             * it arrest rotation. So this asks for the real thing: the car must
             * come down, and it must STOP. rccars_re/rockroll.c measures all
             * three cars. */
            /* WHAT "STOP" CAN MEAN HERE CHANGED when the roof stations went into
             * the proxy, and the old form of this check was passing for the
             * wrong reason. With no sphere anywhere near the roof, an inverted
             * car sank until its WHEEL spheres caught it -- 97 mm of bodywork
             * underground -- and a car buried to its wheel arches is
             * geometrically wedged, so its instantaneous speed really was
             * 0.0000 and `resid < 0.01f` sailed through. It was measuring a car
             * stuck in the terrain, not a car at rest on it.
             *
             * Resting properly, on its roof, the car is NOT perfectly still and
             * cannot be: the port has no carSubstepContact bisection, so a body
             * sphere ends each substep slightly inside the surface,
             * rb_body_depenetrate lifts it back out and rb_coll_resolve leaves
             * the contact separating at +0.05 m/s. The car falls the 1.0 mm of
             * RB_PENETRATION_SLACK plus the 0.51 mm depenetration margin before
             * the gate re-fires -- about four substeps, in which gravity builds
             * 0.17 m/s -- and is caught again. That is a bounded buzz in place,
             * measured at 0.17 m/s peak, and it is a known consequence of the
             * documented structural divergence rather than drifting.
             *
             * So ask the question that separates the two, which the instantaneous
             * speed never could: does the car GO anywhere? Anchored to the car's
             * own length, not to the buzz. */
            if (flipped) {
                float resid = sqrtf(c.body.v[0]*c.body.v[0]
                                    + c.body.v[1]*c.body.v[1]
                                    + c.body.v[2]*c.body.v[2]);
                float travel, len = RB_CARS[0].extent[2];
                float x0 = c.body.x[0], z0 = c.body.x[2];

                for (i = 0; i < 300; i++)                  /* five more seconds */
                    step_car(&c, 1.0f / 60.0f);
                travel = sqrtf((c.body.x[0] - x0) * (c.body.x[0] - x0)
                               + (c.body.x[2] - z0) * (c.body.x[2] - z0));
                printf("   inverted: residual speed %.4f m/s, travelled %.4f m "
                       "in the next 5 s (car is %.3f m long)\n",
                       resid, travel, len);
                ck(c.body.x[1] < 0.25f,
                   "an inverted car comes down instead of hovering");
                ck(c.body.x[1] > 0.0f, "and does not sink through its own roof");
                ck(travel < 0.1f * len, "and stays put instead of drifting");
            }
        }
    }

    /* ---- the contact solve, one mechanism at a time ---------------------
     *
     * The block above only drops an inverted car straight down with no velocity
     * and no spin, and that turned out to exercise almost none of the solve. With
     * rb_coll_friction stubbed out, with rb_apply_impulse's angular term deleted,
     * with the below-surface normal guard removed, and with the depenetration
     * lifting once per penetrating sphere instead of once for the deepest, it
     * still passed -- a mutation run put seven deliberate breakages through this
     * file and only one check between them noticed.
     *
     * So give each mechanism something only it can answer for, and run all three
     * cars: the Hummer is the one the normal guard was found on, and rb_test has
     * historically only ever exercised car 0.
     */
    {
        int carn;
        printf("\n-- the contact solve, per mechanism --\n");
        for (carn = 0; carn < 3; carn++) {
            float y_rest, spin_end, slide_end;
            float y[RB_STATE_N];

            /* (a) inverted and SLIDING. Only rb_coll_friction can stop this; the
                   normal impulse is normal, by construction. */
            rbcar_init(&c, carn, &FLAT_WORLD, 0.f, 0.f, 0.f, 0.f);
            c.body.q[0] = 0.f; c.body.q[1] = 0.f; c.body.q[2] = 0.f;
            c.body.q[3] = 1.f;
            /* 0.60, not 0.40: the body sphere sits 0.39-0.41 m from the centre
               of mass on these three cars (CdtDeltaY, and that distance is itself
               suspect on a car 0.42 m long -- see PHYSICS.md), so a lower release
               starts the car with its roof sphere already under the terrain and
               the test would be scoring the recovery from that instead. */
            c.body.x[1] = 0.60f;
            c.body.P[0] = 3.0f * c.body.mass;      /* 3 m/s sideways */
            rb_car_get_state(&c, y); rb_car_set_state(&c, y);
            for (i = 0; i < 900; i++) step_car(&c, 1.0f / 60.0f);
            slide_end = sqrtf(c.body.v[0]*c.body.v[0] + c.body.v[2]*c.body.v[2]);
            y_rest = c.body.x[1];

            /* (b) inverted and SPINNING. Only the r x j term in rb_apply_impulse
                   takes angular momentum out -- an impulse along the normal
                   applied at the centre of mass could not. */
            rbcar_init(&c, carn, &FLAT_WORLD, 0.f, 0.f, 0.f, 0.f);
            c.body.q[0] = 0.f; c.body.q[1] = 0.f; c.body.q[2] = 0.f;
            c.body.q[3] = 1.f;
            c.body.x[1] = 0.60f;
            c.body.L[1] = 0.5f;                    /* spinning about world up */
            rb_car_get_state(&c, y); rb_car_set_state(&c, y);
            for (i = 0; i < 900; i++) step_car(&c, 1.0f / 60.0f);
            /* The SEEDED axis, averaged over a second -- not the magnitude at one
               frame, which is what this used to take.
               |w| conflates two different things now that the car rests on its
               roof instead of being wedged in the ground to its wheel arches. The
               contact is live (see the buzz note above) and it rocks the car
               about its FORWARD axis: measured on flat ground, mean |wz| is
               0.218 / 0.164 / 0.069 while the yaw this test actually seeded with
               L[1] = 0.5 is down to 0.169 / 0.076 / 0.084. Scoring the magnitude
               therefore reports the rocking and calls it an unarrested spin.
               The mechanism under test is the r x j term removing the angular
               momentum that was put in, so measure the axis it was put in on. */
            spin_end = 0.0f;
            for (i = 0; i < 60; i++) {
                step_car(&c, 1.0f / 60.0f);
                spin_end += fabsf(c.body.w[1]);
            }
            spin_end /= 60.0f;

            printf("car %d inverted: rests at y=%.3f | 3 m/s slide stops to"
                   " %.4f m/s | seeded yaw stops to %.4f rad/s\n",
                   carn + 1, y_rest, slide_end, spin_end);

            /* Every car must end up ON the surface -- not above it and not below
               it. The upper bound catches a depenetration that lifts once per
               penetrating sphere; the lower bound catches a normal that points
               into the ground. */
            ck(y_rest > 0.0f && y_rest < 0.25f,
               "an inverted car rests on the surface, all three cars");
            ck(slide_end < 0.05f, "and body friction brings a slide to a stop");
            ck(spin_end < 0.20f, "and the contact impulse arrests its spin");
        }
    }

    /* ---- the momentum clamps (0x004f5770, MAX_IMPULSE) ------------------
     *
     * maxImpLinear caps |v| at 20 m/s and maxImpAng caps |L| at 1.0. Neither is
     * reachable by driving, so nothing else in this file can see them: with the
     * whole clamp stubbed out every other check still passed.
     *
     * The linear bound is pinned against an INDEPENDENT number. The drag check at
     * the top of this file measures this body's terminal velocity at 24.76 m/s, so
     * a long fall has to be stopped by the clamp before drag stops it; delete the
     * clamp and the fall runs on to 24-something and this fails.
     */
    {
        float vmax = 0.f, wend;
        /* High above the plane, so it falls freely -- but with the world still
           ATTACHED, because rb_car_tick short-circuits to a single unclamped Euler
           step when there is no world and would skip the clamp entirely. */
        car_init(&c);
        c.body.x[1] = 300.0f;
        rb_car_update_matrix(&c);
        for (i = 0; i < 600; i++) {
            step_car(&c, 1.0f / 60.0f);
            if (speed_of(&c) > vmax) vmax = speed_of(&c);
        }
        car_init(&c);
        c.body.x[1] = 300.0f;
        rb_car_update_matrix(&c);
        c.body.L[0] = 5.0f;                 /* five times the cap */
        {
            float y[RB_STATE_N];
            rb_car_get_state(&c, y); rb_car_set_state(&c, y);
        }
        for (i = 0; i < 10; i++) step_car(&c, 1.0f / 60.0f);
        wend = sqrtf(c.body.L[0]*c.body.L[0] + c.body.L[1]*c.body.L[1]
                     + c.body.L[2]*c.body.L[2]);
        printf("MAX_IMPULSE: 10 s of free fall peaks at %.2f m/s (drag alone"
               " reaches 24.76), |L| 5.0 -> %.3f\n", vmax, wend);
        ck(vmax < 20.5f, "maxImpLinear caps the fall before drag does");
        ck(wend < 1.05f, "maxImpAng caps angular momentum at 1.0");
    }

    /* ---- the rest clamp (0x004f6610) -----------------------------------
     *
     * carPhysTick's first act is to ask whether the car has been slower than
     * 0.3611 m/s for 2 s, slower than 1.0 rad/s for 2 s, and in contact for 1 s --
     * and if so it zeroes P, L, v and w and skips the ENTIRE physics step. The
     * engine puts a settled car to sleep; nothing in this port did.
     *
     * It is worth its own check because it is not observable anywhere else: with
     * the clamp stubbed out, every other check in this file still passed. Assert
     * the mechanism directly -- the flag, and the exact zero that goes with it.
     * "Approximately still" is what a damped car does on its own; only the clamp
     * produces a hard zero and an `asleep` frame.
     */
    {
        int slept_at = -1;
        car_init(&c);
        for (i = 0; i < 600; i++) {
            step_car(&c, 1.0f / 60.0f);
            if (c.asleep && slept_at < 0) slept_at = i;
        }
        printf("rest clamp: asleep from frame %d (%.2f s), final |v|=%.9f"
               " |w|=%.9f\n", slept_at, slept_at / 60.0f, speed_of(&c),
               sqrtf(c.body.w[0]*c.body.w[0] + c.body.w[1]*c.body.w[1]
                     + c.body.w[2]*c.body.w[2]));
        ck(slept_at >= 0, "a settled car reaches the rest clamp");
        ck(slept_at > 60 && slept_at < 400,
           "and not before its timers can possibly have run");
        ck(c.asleep, "and stays asleep");
        ck(speed_of(&c) == 0.0f, "the clamp zeroes velocity exactly");
    }

    /* ---- forward convention -------------------------------------------
     *
     * The rigid body travels along its local +Z, and the car mesh's nose is +Z
     * too (front wheels at z = +0.149). They agree, so the renderer must NOT add
     * a half turn -- doing so drew the car facing backwards, which looked correct
     * only in reverse. Guard the convention here so a future change to the wheel
     * layout cannot silently flip it.
     */
    car_init(&c);
    c.world = &FLAT_WORLD;
    rb_car_update_matrix(&c);
    for (i = 0; i < 180; i++) { c.in.accel = 1; c.in.throttle = 1.0f;
                                step_car(&c, 1.0f / 60.0f); }
    {
        const float *bz = &c.m[8];
        float sp = speed_of(&c);
        double dot = sp > 1e-6f
            ? ((double)c.body.v[0]*bz[0] + (double)c.body.v[1]*bz[1]
               + (double)c.body.v[2]*bz[2]) / sp
            : 0.0;
        printf("forward convention: dot(velocity, body +Z) = %+.4f  "
               "front wheel z=%+.3f rear z=%+.3f\n",
               dot, c.wheel[0].mount[2], c.wheel[2].mount[2]);
        ck(dot > 0.9, "car drives along its local +Z");
        ck(c.wheel[0].mount[2] > c.wheel[2].mount[2],
           "front wheels are the +Z pair, matching the mesh nose");
    }

    /* ---- steering handedness -------------------------------------------
     *
     * Stick right must turn the car right. The body's steer angle has the
     * OPPOSITE sense to the stick: carWheelFrame rotates the wheel's local +Z
     * toward +X for a positive angle, and +X is the car's left (the mesh puts
     * WHEEL_FRONT_LEFT at x = +0.141). Getting this backwards makes the car steer
     * away from the stick, which reads as left and right being swapped relative
     * to the camera.
     *
     * Yaw here is the RENDERER's view yaw: a camera at yaw v faces
     * F(v) = (-sin v, 0, -cos v), so v = 0 is -Z and v = 90 is -X. Seen from above
     * with +X to the right, increasing v swings up -> left, so a right turn
     * DECREASES yaw. (The port's old vehicle_t used the mirrored convention; see
     * cam.c.)
     */
    {
        int t;
        for (t = 0; t < 2; t++) {
            float st = t ? -1.0f : 1.0f;
            float prev = 0.f, d = 0.f;
            car_init(&c);
            c.world = &FLAT_WORLD;
            rb_car_update_matrix(&c);
            /* ACCUMULATE the per-step deltas. Taking the endpoint difference and
               wrapping it to +-180, which is what this used to do, silently
               ALIASES: at full lock the car now turns about 350 degrees in these
               three seconds, and the wrap reported that as +7 -- the wrong sign,
               on a car that was turning correctly the whole way round. The test
               was reading a full revolution as a small turn the other way. It only
               ever passed because cornering used to be too weak to complete one.*/
            for (i = 0; i < 240; i++) {
                float yaw, step;
                rbcar_step(&c, 0.6f, 0.0f, st, 0, 1.0f / 60.0f);
                yaw = rbcar_yaw_deg(&c);
                if (i == 0) { prev = yaw; continue; }
                step = yaw - prev;
                while (step > 180.f)  step -= 360.f;
                while (step < -180.f) step += 360.f;
                if (i > 60) d += step;             /* skip the launch */
                prev = yaw;
            }
            printf("steer %+.1f -> yaw %+.1f deg accumulated, turns %-5s"
                   " (body angle %+.1f deg)\n",
                   st, d, d < 0 ? "RIGHT" : "LEFT", c.steer);
            if (st > 0) ck(d < -2.0f, "stick right turns the car right");
            else        ck(d > 2.0f,  "stick left turns the car left");
            ck(st * c.steer < 0.0f,
               "body steer angle has the opposite sign to the stick");
        }
    }

    /* ================= part 4: the follow camera ====================== */
    puts("\n-- follow camera --");

    car_init(&c);
    c.world = &FLAT_WORLD;
    c.body.x[1] = c.wheel[0].radius + c.wheel[0].len;
    rb_car_update_matrix(&c);
    for (i = 0; i < 120; i++) step_car(&c, 1.0f / 600.0f);
    {
        cam_t cm;
        cam_init(&cm, &c);
        for (i = 0; i < 120; i++) cam_update(&cm, &c, 0.0f, 1.0f / 60.0f);
        printf("at rest: dist=%.4f height=%.4f yaw=%+.2f  (config %.4f / %.4f)\n",
               cm.dist, cm.height, cm.yaw, RB_CAMERA.dist_xz, RB_CAMERA.dist_y);
        ck(fabs(cm.dist - RB_CAMERA.dist_xz) < 0.02f, "at rest the camera sits at defDistXZ");
        ck(fabs(cm.height - RB_CAMERA.dist_y) < 0.05f, "at rest it sits at defDistY");

        /* behind the car: the eye must be on the negative-forward side */
        {
            double dot = (double)(cm.pos[0] - c.body.x[0]) * c.m[8]
                       + (double)(cm.pos[2] - c.body.x[2]) * c.m[10];
            printf("   eye is %s the car (dot=%.4f)\n", dot < 0 ? "behind" : "AHEAD OF", dot);
            ck(dot < 0.0, "camera is behind the car");
        }

        /* pull-back with speed: above 8 m/s it should reach 1.5x the base */
        c.in.accel = 1; c.in.throttle = 1.0f;
        c.body.v[2] = 9.0f;
        { float y[RB_STATE_N]; rb_car_get_state(&c, y); y[9] = 9.0f * c.body.mass;
          rb_car_set_state(&c, y); }
        for (i = 0; i < 300; i++) cam_update(&cm, &c, 0.0f, 1.0f / 60.0f);
        printf("at 9 m/s on throttle: dist=%.4f  (base %.4f, +50%% speed, +20%% throttle"
               " -> %.4f)\n", cm.dist, RB_CAMERA.dist_xz,
               RB_CAMERA.dist_xz * 1.7f);
        ck(cm.dist > RB_CAMERA.dist_xz * 1.4f, "camera pulls back with speed");
        ck(cm.dist < RB_CAMERA.dist_xz * 2.0f, "pull-back stays bounded");

        /* Yaw lag at a rate the follower can hold. The car is turned at
           0.02 rad/frame = 68.8 deg/s, inside camFollowStep's 99 deg/s, so the
           trail settles where the eased pull-back matches it:
           68.8 = 99 * (lag/20)^2 -> 16.7 degrees, just inside the window. */
        {
            float worst = 0.f, yq[RB_STATE_N];
            int k;
            for (k = 0; k < 240; k++) {
                double a = (double)k * 0.02;      /* yaw the car steadily */
                rb_car_get_state(&c, yq);
                yq[3] = (float)cos(a * 0.5); yq[4] = 0.f;
                yq[5] = (float)sin(a * 0.5); yq[6] = 0.f;
                rb_car_set_state(&c, yq);
                cam_update(&cm, &c, 0.0f, 1.0f / 60.0f);
                {
                    double e = (double)cm.yaw - rbcar_yaw_deg(&c);
                    while (e > 180.0) e -= 360.0;
                    while (e < -180.0) e += 360.0;
                    if (fabs(e) > worst) worst = (float)fabs(e);
                }
            }
            printf("car yawed at 68.8 deg/s: worst camera lag = %.2f deg"
                   " (eased equilibrium is 16.7)\n", worst);
            ck(worst > 0.5f, "camera yaw actually lags the car");
            ck(worst > 10.0f && worst < 21.0f,
               "at 68.8 deg/s the trail settles near the eased equilibrium");
        }

        /* Sustained turn, measured through the real GL view matrix.
         *
         * Two things must hold every frame: the car stays in front of the camera
         * and centred, and the car's right side appears on screen-right. Both used
         * to fail as soon as the car turned -- the eye was placed with
         * (+sin, 0, -cos) while glRotatef(-yaw, Y) makes the camera face
         * (-sin, 0, -cos), so they agreed only at yaw 0 and 180.
         */
        {
            rb_car t;
            cam_t  tc;
            float worst_off = 0.f, worst_lag = 0.f, peak_w = 0.f;
            int behind = 0, mirrored = 0;
            car_init(&t);
            t.world = &FLAT_WORLD;
            rb_car_update_matrix(&t);
            cam_init(&tc, &t);
            for (i = 0; i < 420; i++) {
                float st = (i < 60) ? 0.0f : 1.0f;
                rbcar_step(&t, 0.6f, 0.0f, st, 0, 1.0f / 60.0f);
                cam_update(&tc, &t, st, 1.0f / 60.0f);
                {
                    float wd = fabsf(t.body.w[1]) * 57.295776f;
                    if (wd > peak_w) peak_w = wd;
                }
                {
                    const float D = 0.017453292f;
                    float h = tc.yaw * D;
                    float ex = t.body.x[0] + sinf(h) * tc.dist;
                    float ez = t.body.x[2] + cosf(h) * tc.dist;
                    float ey = t.body.x[1] + tc.height;
                    float V[16], pc[3], rm[3], ec[3], er[3];
                    double lag;
                    int k;
                    gl_view(V, tc.yaw, cam_pitch_deg(&tc), ex, ey, ez);
                    pc[0] = t.body.x[0]; pc[1] = t.body.x[1]; pc[2] = t.body.x[2];
                    for (k = 0; k < 3; k++) rm[k] = pc[k] - t.m[0 + k];  /* -X = right */
                    gl_xform(ec, V, pc);
                    gl_xform(er, V, rm);
                    lag = (double)tc.yaw - rbcar_yaw_deg(&t);
                    while (lag > 180.0)  lag -= 360.0;
                    while (lag < -180.0) lag += 360.0;
                    if (fabs(lag) > worst_lag) worst_lag = (float)fabs(lag);
                    /* These three are the mirrored-yaw-convention checks, and
                       they only mean anything while the camera is still behind
                       the car. Past 60 degrees of trail it is looking at the
                       flank, so of course the right side crosses the centre --
                       that is the camera working, not the convention failing.
                       The convention bug showed up on the first frame of the
                       turn, long inside this gate. */
                    if (fabs(lag) < 60.0) {
                        if (ec[2] >= 0.f) behind = 1;
                        if (er[0] <= ec[0]) mirrored = 1;
                        if (fabsf(ec[0]) > worst_off) worst_off = fabsf(ec[0]);
                    }
                }
            }
            printf("7 s full-lock turn: worst |eye-space x|=%.4f, peak car yaw rate"
                   " %.0f deg/s, trail=%.2f deg, car behind camera=%s,"
                   " right side mirrored=%s\n",
                   worst_off, peak_w, worst_lag, behind ? "YES" : "no",
                   mirrored ? "YES" : "no");
            ck(!behind,   "car stays in front of the camera through a turn");
            ck(!mirrored, "car's right side stays on screen-right");
            ck(worst_off < 0.05f, "car stays horizontally centred in the view");
            ck(worst_lag > 90.0f,
               "a 176 deg/s car does out-turn the follower's 99 deg/s");

            /* The trail is NOT clamped to the slack window -- that was the port's
               own invention and it welded the view to the tail. camFollowStep
               closes at 99 deg/s (30 while the car spins past 180 deg/s) and
               nothing else bounds it, so a car turning faster than that does get
               away from the view. What must hold is that it comes BACK: release
               the steering and the trail has to unwind. */
            {
                float after;
                int k;
                for (k = 0; k < 180; k++) {          /* 3 s running straight */
                    rbcar_step(&t, 0.6f, 0.0f, 0.0f, 0, 1.0f / 60.0f);
                    cam_update(&tc, &t, 0.0f, 1.0f / 60.0f);
                }
                {
                    double e = (double)tc.yaw - rbcar_yaw_deg(&t);
                    while (e > 180.0)  e -= 360.0;
                    while (e < -180.0) e += 360.0;
                    after = (float)fabs(e);
                }
                printf("   3 s after the stick is released: trail=%.2f deg\n", after);
                ck(after < 2.0f, "the trail unwinds once the car stops turning");
            }
        }

        /* VisTurn: NOT a look-into-turn. 0x00501180 rotates the aim about the
           camera's right axis by a constant 11.82 degrees, every frame, with the
           stick nowhere in it. So the stick must not move the camera, and the aim
           must sit VisTurn degrees above the line to the car. */
        {
            cam_t c2, c3;
            float aimed;
            cam_init(&c2, &c); cam_init(&c3, &c);
            for (i = 0; i < 240; i++) cam_update(&c2, &c, 0.0f, 1.0f / 60.0f);
            for (i = 0; i < 240; i++) cam_update(&c3, &c, 1.0f, 1.0f / 60.0f);
            printf("VisTurn: yaw %+.2f straight vs %+.2f at full steer"
                   " -- the stick must not appear here\n", c2.yaw, c3.yaw);
            ck(fabs(c3.yaw - c2.yaw) < 1e-3f,
               "the steering stick does not move the camera round the car");

            aimed = atan2f(c2.height, c2.dist) * 57.295776f;
            printf("   aim: %.2f deg down at the car, %.2f deg after VisTurn"
                   " (config %.2f)\n", aimed, cam_pitch_deg(&c2), RB_CAMERA.vis_turn);
            ck(fabs((aimed - cam_pitch_deg(&c2)) - RB_CAMERA.vis_turn) < 0.01f,
               "the aim is tilted UP by exactly VisTurn");
            ck(cam_pitch_deg(&c2) < aimed - 5.0f,
               "the car therefore sits below the centre of the frame");
        }

        /* The obstacle lift, 0x00500e60: block the line from the car to the eye
           and the eye has to climb over it. The harness plane is the only
           geometry there is, so tip it up behind the car to make the segment
           cross it. */
        {
            rb_car g;
            cam_t  gc;
            float lifted, level;
            car_init(&g);
            g.world = &FLAT_WORLD;
            g.body.x[1] = g.wheel[0].radius + g.wheel[0].len;
            rb_car_update_matrix(&g);
            cam_init(&gc, &g);
            for (i = 0; i < 60; i++) cam_update(&gc, &g, 0.0f, 1.0f / 60.0f);
            level = gc.height;
            /* A vertical plane through the car, normal along -Z. The car sits
               exactly on it and the eye is 0.79 m behind, so the sight line
               crosses it -- and with no Y component w_ground declines to answer,
               which keeps the ground clamp out of this measurement. */
            TILT_N[0] = 0.0f; TILT_N[1] = 0.0f; TILT_N[2] = -1.0f;
            for (i = 0; i < 60; i++) cam_update(&gc, &g, 0.0f, 1.0f / 60.0f);
            lifted = gc.height;
            printf("obstacle lift: height %.4f clear -> %.4f blocked"
                   " (CDT_AngleUp %.1f deg at %.1f deg/s)\n",
                   level, lifted, RB_CAMERA.cdt_angle_up,
                   RB_CAMERA.cdt_angle_up_speed);
            ck(lifted > level + 0.05f, "a blocked sight line lifts the eye");
            ck(gc.cdt_dir_add > 1.0f, "and carries its own extra aim tilt");
            TILT_N[0] = 0.0f; TILT_N[1] = 1.0f; TILT_N[2] = 0.0f;
        }
    }

    /* ================= part 5: the car rig ============================ */
    rig_checks();
    rig_checks_23();

    /* ================= part 6: Jump, and water ======================== */
    jump_water_checks();

    /* ================= part 7: the fixed timestep ===================== */
    timestep_checks();

    /* ================= part 8: the boost meter ======================== */
    boost_checks();

    printf("\n%s\n", fails ? "SOME CHECKS FAILED" : "all checks passed");
    return fails != 0;
}
