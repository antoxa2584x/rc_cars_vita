/*
 * carani.c -- carAniProc1/2/3 (0x00504820 / 0x00505780 / 0x005068e0), transcribed.
 *
 * See carani.h for what it drives and why. Two facts do most of the work here:
 *
 * - The engine's rotate helpers PRE-multiply. FUN_0040c940 with mode 1 computes
 *   mat4Mul(rot, m) and stores it back, i.e. m = rot * m, so a rotation acts in
 *   the node's OWN frame before its rest transform. Getting this backwards
 *   rotates each part about the model origin instead of its own pivot.
 *
 * - The rotate helpers are, in the engine's numbering,
 *       0x0040ca30 = about X    0x0040cab0 = about Y    0x0040cb30 = about Z
 *   read straight off the matrix elements each one writes. The wheels turn about
 *   Z because their node frames are rolled: WHEEL_FRONT_LEFT's local +Z maps to
 *   model +X, which is the axle direction.
 *
 * Build with -fno-fast-math -ffp-contract=off, like the rest of the model.
 */

#include "carani.h"
#include <math.h>
#include <string.h>

#define EPS 1e-06f

/* --- row-vector 4x4 ------------------------------------------------------- */

static void mat_identity(float m[16])
{
    memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* out = a * b, row-vector: (p*a)*b == p*(a*b). out must not alias a or b. */
static void mat_mul(const float a[16], const float b[16], float out[16])
{
    int r, c;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++)
            out[r * 4 + c] = (float)((double)a[r * 4 + 0] * b[0 * 4 + c]
                                   + (double)a[r * 4 + 1] * b[1 * 4 + c]
                                   + (double)a[r * 4 + 2] * b[2 * 4 + c]
                                   + (double)a[r * 4 + 3] * b[3 * 4 + c]);
}

/* m = rot * m -- FUN_0040c940 mode 1, the only mode the animation procs use. */
static void mat_premul(float m[16], const float rot[16])
{
    float t[16];
    mat_mul(rot, m, t);
    memcpy(m, t, sizeof(t));
}

/* The engine builds these in degrees (0x0040ca30 / 0x0040cab0 / 0x0040cb30 all
   scale by 0.017453292 before the sin/cos), so these take degrees too. */
static void rot_x(float m[16], float deg)
{
    double a = (double)deg * 0.017453292;
    float s = (float)sin(a), c = (float)cos(a);
    mat_identity(m);
    m[5] = c; m[6] = s; m[9] = -s; m[10] = c;
}

static void rot_y(float m[16], float deg)
{
    double a = (double)deg * 0.017453292;
    float s = (float)sin(a), c = (float)cos(a);
    mat_identity(m);
    m[0] = c; m[2] = -s; m[8] = s; m[10] = c;
}

static void rot_z(float m[16], float deg)
{
    double a = (double)deg * 0.017453292;
    float s = (float)sin(a), c = (float)cos(a);
    mat_identity(m);
    m[0] = c; m[1] = s; m[4] = -s; m[5] = c;
}

/* Rotation carrying unit `a` onto unit `b`, row-vector, so a*R == b.
   0x00407620 in the original; the springs move by fractions of a degree per
   frame, so only the near-parallel case needs guarding. */
static void rot_align(const float a[3], const float b[3], float m[16])
{
    double ax = (double)a[1] * b[2] - (double)a[2] * b[1];
    double ay = (double)a[2] * b[0] - (double)a[0] * b[2];
    double az = (double)a[0] * b[1] - (double)a[1] * b[0];
    double s = sqrt(ax * ax + ay * ay + az * az);
    double c = (double)a[0] * b[0] + (double)a[1] * b[1] + (double)a[2] * b[2];
    double t, x, y, z;

    mat_identity(m);
    if (s < (double)EPS) {
        if (c >= 0.0)
            return;                       /* already aligned */
        /* antiparallel: a half turn about any perpendicular axis */
        ax = fabs((double)a[0]) < 0.9 ? 1.0 : 0.0;
        ay = ax == 0.0 ? 1.0 : 0.0;
        az = 0.0;
        x = (double)a[1] * az - (double)a[2] * ay;
        y = (double)a[2] * ax - (double)a[0] * az;
        z = (double)a[0] * ay - (double)a[1] * ax;
        s = sqrt(x * x + y * y + z * z);
        if (s < (double)EPS)
            return;
        x /= s; y /= s; z /= s;
        s = 0.0; c = -1.0;
    } else {
        x = ax / s; y = ay / s; z = az / s;
    }
    t = 1.0 - c;
    /* Rodrigues, TRANSPOSED into the row-vector convention. */
    m[0]  = (float)(t * x * x + c);
    m[1]  = (float)(t * x * y + s * z);
    m[2]  = (float)(t * x * z - s * y);
    m[4]  = (float)(t * x * y - s * z);
    m[5]  = (float)(t * y * y + c);
    m[6]  = (float)(t * y * z + s * x);
    m[8]  = (float)(t * x * z + s * y);
    m[9]  = (float)(t * y * z - s * x);
    m[10] = (float)(t * z * z + c);
}

/* p' = p * m */
static void xform_point(const float m[16], const float p[3], float out[3])
{
    int i;
    for (i = 0; i < 3; i++)
        out[i] = (float)((double)p[0] * m[0 * 4 + i] + (double)p[1] * m[1 * 4 + i]
                       + (double)p[2] * m[2 * 4 + i] + m[3 * 4 + i]);
}

static float normalize3(float v[3])
{
    double n = sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]);
    if (n < (double)EPS)
        return (float)n;
    v[0] = (float)(v[0] / n);
    v[1] = (float)(v[1] / n);
    v[2] = (float)(v[2] / n);
    return (float)n;
}

/* --- the arc a rotating linkage carries a point along --------------------- */

/*
 * Everything proc2 and proc3 have to invert is the same question: the part P is
 * premultiplied by a rotation about one of its OWN axes, so a point that rides
 * it traces
 *
 *      p(t) = q * Rot(t) * P.rest ,   q = p_rest * P.rest_inv
 *
 * and only the model-space Y of that is ever asked for. Writing Rot out and
 * collecting the terms in cos and sin gives y(t) = c + a*cos t + b*sin t --
 * a circle seen edge on, three constants, exact.
 *
 * The engine tabulates this by brute force at load (1800 samples from -90 to
 * +89.9 degrees, FUN_005055b0 / FUN_005065c0) and binary-searches the table at
 * runtime. Same relation, and the table is the reason the original's angles come
 * in 0.1 degree steps.
 */
static void arc_build(const carani_part *p, const float pt[3], int axis,
                      carani_arc *out)
{
    const float *R = p->rest;
    float q[3];

    xform_point(p->rest_inv, pt, q);
    if (axis == 0) {            /* rot_x: q*Rx = (qx, qy*c - qz*s, qy*s + qz*c) */
        out->c = (float)((double)q[0] * R[1] + R[13]);
        out->a = (float)((double)q[1] * R[5] + (double)q[2] * R[9]);
        out->b = (float)((double)q[1] * R[9] - (double)q[2] * R[5]);
    } else {                    /* rot_z: q*Rz = (qx*c - qy*s, qx*s + qy*c, qz) */
        out->c = (float)((double)q[2] * R[9] + R[13]);
        out->a = (float)((double)q[0] * R[1] + (double)q[1] * R[5]);
        out->b = (float)((double)q[0] * R[5] - (double)q[1] * R[1]);
    }
}

static float arc_rest(const carani_arc *arc)
{
    return (float)((double)arc->c + arc->a);   /* y(0) */
}

float carani_arc_solve(const carani_arc *arc, float y)
{
    double a = arc->a, b = arc->b;
    double r = sqrt(a * a + b * b), d, phi;

    if (r < (double)EPS)
        return 0.0f;                     /* the point is on the axis; no lever */
    d = ((double)y - arc->c) / r;
    if (d > 1.0) d = 1.0;
    else if (d < -1.0) d = -1.0;         /* out of the circle's reach: saturate */
    /* r*cos(t - phi) = y - c has two roots; take the branch through t = 0, which
       is the rest pose the arc was built from and the one the engine's search --
       walking a table that is monotonic over the working range -- also lands on. */
    phi = atan2(b, a);
    return (float)((phi < 0.0 ? phi + acos(d) : phi - acos(d)) * RB_RAD2DEG);
}

/* --- loading -------------------------------------------------------------- */

int carani_read_parts(carani_t *r, FILE *f, unsigned int n)
{
    unsigned int i;
    r->n = 0;
    for (i = 0; i < n; i++) {
        unsigned short nlen = 0;
        char nm[256];
        int parent = -1;
        float mats[48];
        if (fread(&nlen, 2, 1, f) != 1)
            break;
        if (nlen > 255) nlen = 255;
        if (fread(nm, 1, nlen, f) != nlen)
            break;
        nm[nlen] = 0;
        if (fread(&parent, 4, 1, f) != 1)
            break;
        if (fread(mats, sizeof(float), 48, f) != 48)
            break;
        if (r->n < CARANI_MAX_PARTS) {
            carani_part *p = &r->part[r->n++];
            if (nlen > (int)sizeof(p->name) - 1) nlen = sizeof(p->name) - 1;
            memcpy(p->name, nm, nlen);
            p->name[nlen] = 0;
            p->parent = parent;
            memcpy(p->rest,     mats,      sizeof(float) * 16);
            memcpy(p->rest_inv, mats + 16, sizeof(float) * 16);
            memcpy(p->local,    mats + 32, sizeof(float) * 16);
        }
    }
    carani_rest(r);
    return r->n;
}

/* --- binding -------------------------------------------------------------- */

static int find_part(const carani_t *r, const char *name)
{
    int i;
    for (i = 0; i < r->n; i++)
        if (!strcmp(r->part[i].name, name))
            return i;
    return -1;
}

/* "Spring<n>_<half>", or the same with the env-map class suffix the Buggy's
   lower halves carry (Spring1_1_GRE1). The engine's own table names them without
   it and its node lookup evidently tolerates that; here both are simply tried. */
static int find_spring_half(const carani_t *r, int n, int half)
{
    char nm[32];
    int p;

    sprintf(nm, "Spring%d_%d", n, half);
    p = find_part(r, nm);
    if (p < 0) {
        strcat(nm, "_GRE1");
        p = find_part(r, nm);
    }
    return p;
}

void carani_bind(carani_t *r, const rb_car *c)
{
    static const char *const SIDE[2] = { "LEFT", "RIGHT" };
    static const char *const ROW[3]  = { "FRONT", "REAR", "MIDDLE" };
    int i, s;

    for (i = 0; i < RB_MAX_WHEELS; i++) {
        r->wheel[i] = -1;
        r->spin_sign[i] = 1;
        r->arm_up[i] = r->arm_down[i] = r->arm_knuckle[i] = -1;
        r->arm_rest_y[i] = 0.0f;
        memset(&r->arm[i], 0, sizeof(r->arm[i]));
    }
    r->support[0] = r->support[1] = -1;
    r->pair_front[0] = r->pair_front[1] = -1;
    r->pair_rear[0] = r->pair_rear[1] = -1;
    r->pair_middle[0] = r->pair_middle[1] = -1;
    r->axle_front  = find_part(r, "FRONT_AXLE");
    r->axle_rear   = find_part(r, "REAR_AXLE");
    r->axle_middle = find_part(r, "MIDDLE_AXLE");
    r->n_springs  = 0;
    r->n_pairs    = 0;
    memset(r->axle_pitch, 0, sizeof(r->axle_pitch));
    memset(r->axle_tilt, 0, sizeof(r->axle_tilt));
    memset(r->axle_rest_y, 0, sizeof(r->axle_rest_y));
    r->mid_heave  = 0.0f;

    /* Which proc, by which nodes the model HAS. The Buggy is the one with
       wishbones and the Hummer the one with a third axle; the Overkill has
       neither, and no model has both. Deriving it beats a car index: carani
       never sees one, and a mesh that lacks its own proc's nodes would animate
       wrongly rather than not at all. */
    r->proc = (find_part(r, "AXLE_FRONT_LEFT_UP") >= 0) ? 2
            : (r->axle_middle >= 0) ? 3 : 1;
    r->spring_axis = (r->proc == 2) ? 1 : 0;

    /* Match each rb_car wheel to a mesh node by where it sits, not by index.
       +X is the car's LEFT (WHEEL_FRONT_LEFT is at x = +0.15 in Car1) and +Z is
       forward, so the mount's two signs name the node outright. rbcar_init and
       the original disagree on which index is the +X wheel, and this sidesteps
       that entirely -- see the note in carani.h. */
    for (i = 0; i < c->nwheels && i < RB_MAX_WHEELS; i++) {
        char nm[40];
        const float *mt = c->wheel[i].mount;
        int row = (mt[2] > 0.02f) ? 0 : (mt[2] < -0.02f ? 1 : 2);
        int side = (mt[0] >= 0.0f) ? 0 : 1;
        strcpy(nm, "WHEEL_");
        strcat(nm, ROW[row]);
        strcat(nm, "_");
        strcat(nm, SIDE[side]);
        r->wheel[i] = find_part(r, nm);
        /* The axle tilt is (left height - right height), so remember which
           rb_car index is which side. carAniProc1 gets away with hardcoding
           wheels 0/1 and 2/3 because in the original wheel 0 IS the +X one;
           rbcar_init lays them out mirrored, so the pairing has to be derived. */
        if (row == 0)      r->pair_front[side] = i;
        else if (row == 1) r->pair_rear[side]  = i;
        else               r->pair_middle[side] = i;
        /* Which way this wheel's axle points in model space. See carani.h. */
        if (r->wheel[i] >= 0 && r->part[r->wheel[i]].rest[8] < 0.0f)
            r->spin_sign[i] = -1;
        /* Only the front pair steers, and its knuckle is the wheel's parent --
           on the cars that HAVE a knuckle. The Buggy's front wheels are carried
           by an upright that stays put and are turned in their own node. */
        if (row == 0 && r->wheel[i] >= 0) {
            strcpy(nm, "AXLE_FRONT_");
            strcat(nm, SIDE[side]);
            strcat(nm, "_SUPPORT");
            r->support[side] = find_part(r, nm);
        }
        /* proc2's wishbone: the two arms and the upright between them, and the
           arc the wheel node rides as the UP arm swings about its own X. */
        if (r->proc == 2 && row < 2 && r->wheel[i] >= 0) {
            char base[40];
            strcpy(base, "AXLE_");
            strcat(base, ROW[row]);
            strcat(base, "_");
            strcat(base, SIDE[side]);
            strcpy(nm, base); strcat(nm, "_UP");
            r->arm_up[i] = find_part(r, nm);
            strcpy(nm, base); strcat(nm, "_DOWN");
            r->arm_down[i] = find_part(r, nm);
            strcpy(nm, base); strcat(nm, "_WHEEL");
            r->arm_knuckle[i] = find_part(r, nm);
            if (r->arm_up[i] >= 0) {
                arc_build(&r->part[r->arm_up[i]],
                          r->part[r->wheel[i]].rest + 12, 0, &r->arm[i]);
                r->arm_rest_y[i] = arc_rest(&r->arm[i]);
            }
        }
    }

    /* proc3's three axles. Each one's arcs are traced by its LEFT wheel node --
       the engine sweeps a wheel of its own choosing per table (FUN_00506800 /
       FUN_00506820 pick 0, 3 and 4) and measures it in the axle's own frame;
       measuring the same wheel in MODEL space for both arcs is the same relation
       with the sign settled by the geometry rather than by that frame.
       The third axle CANNOT pivot its wheels up and down -- its node origin is
       3.5 mm from its own wheel line against 132 mm on the other two -- so the
       engine slides it and rotates it only for the roll: FUN_0040c880(node, 0,
       dy, 0, 1) then rotZ, at 0x005069d0. Hence pitch-about-Z for all three, and
       a tilt-about-X for only the two that pivot. */
    if (r->proc == 3) {
        int k;
        for (k = 0; k < 3; k++) {
            int part = (k == 0) ? r->axle_front
                     : (k == 1) ? r->axle_rear : r->axle_middle;
            int lw = (k == 0) ? r->pair_front[0]
                   : (k == 1) ? r->pair_rear[0] : r->pair_middle[0];
            if (part < 0 || lw < 0 || r->wheel[lw] < 0)
                continue;
            arc_build(&r->part[part], r->part[r->wheel[lw]].rest + 12, 2,
                      &r->axle_pitch[k]);
            if (k != 2)
                arc_build(&r->part[part], r->part[r->wheel[lw]].rest + 12, 0,
                          &r->axle_tilt[k]);
            r->axle_rest_y[k] = arc_rest(&r->axle_pitch[k]);
        }
        /* How far the middle may slide. The engine's clamp is a by-product of
           the same sweeps -- DAT_014edd4c, written at 0x00506772 whenever a
           sweep passes 20 degrees, so the FRONT axle's pass, the last of the
           three, is the one that stands: the vertical travel a 20 degree pitch
           gives that axle's own wheel. 45 mm on the Hummer. Derived here off the
           same geometry rather than tabled, so it cannot drift from the mesh. */
        if (r->axle_front >= 0 && r->pair_front[0] >= 0
            && r->wheel[r->pair_front[0]] >= 0) {
            const carani_arc *pa = &r->axle_pitch[0];
            double t = 20.0 * 0.017453292;
            r->mid_heave = (float)fabs((double)pa->c + pa->a * cos(t)
                                       + pa->b * sin(t) - arc_rest(pa));
        }
    }

    /* proc2 and proc3 spring halves, which aim at each other instead of at an
       axle and never stretch. Six pairs is the Hummer; the Buggy has four. */
    if (r->proc != 1) {
        for (s = 1; s <= CARANI_MAX_SPRING_PAIRS; s++) {
            int lo = find_spring_half(r, s, 1), hi = find_spring_half(r, s, 2);
            if (lo < 0 || hi < 0)
                continue;
            r->spair[r->n_pairs][0] = lo;
            r->spair[r->n_pairs][1] = hi;
            r->n_pairs++;
        }
    }

    /* Springs, in the original's order: SPRING_REAR_1..4 hang off the rear axle
       and SPRING_FRONT_1..4 off the front (carAniProc1 splits them at index 4).
       Binding by name rather than by that index keeps it right whatever order
       the exporter happened to walk the tree in. */
    for (s = 0; s < CARANI_MAX_SPRINGS; s++) {
        char nm[32];
        int rear = (s < 4);
        int axle = rear ? r->axle_rear : r->axle_front;
        int p;
        size_t len;
        strcpy(nm, rear ? "SPRING_REAR_" : "SPRING_FRONT_");
        len = strlen(nm);
        nm[len] = (char)('1' + (s & 3));
        nm[len + 1] = 0;
        p = find_part(r, nm);
        if (p < 0 || axle < 0)
            continue;
        r->spring[r->n_springs] = p;
        r->spring_axle[r->n_springs] = axle;
        {
            /* At rest the spring aims one SpringLength along its own +Z. Store
               that point in the AXLE's frame so it rides the axle from then on. */
            const float *rest = r->part[p].rest;
            float z[3], w[3];
            z[0] = rest[8]; z[1] = rest[9]; z[2] = rest[10];
            normalize3(z);
            w[0] = (float)((double)rest[12] + (double)z[0] * CARANI_SPRING_LEN);
            w[1] = (float)((double)rest[13] + (double)z[1] * CARANI_SPRING_LEN);
            w[2] = (float)((double)rest[14] + (double)z[2] * CARANI_SPRING_LEN);
            xform_point(r->part[axle].rest_inv, w, r->spring_aim[r->n_springs]);
        }
        r->n_springs++;
    }

    carani_rest(r);
}

/* --- update --------------------------------------------------------------- */

void carani_rest(carani_t *r)
{
    int i;
    for (i = 0; i < r->n; i++) {
        memcpy(r->world[i], r->part[i].rest, sizeof(float) * 16);
        mat_identity(r->draw[i]);
    }
}

float carani_wheel_plane_y(const carani_t *r)
{
    double sum = 0.0;
    int i, n = 0;

    for (i = 0; i < RB_MAX_WHEELS; i++) {
        int p = r->wheel[i];
        if (p < 0 || p >= r->n)
            continue;
        sum += r->part[p].rest[13];
        n++;
    }
    return n ? (float)(sum / (double)n) : 0.0f;
}

/* The tuning's tyre width -- FUN_0050bde0's own table, indexed by the tyre
   level. See carani.h for where the engine applies it and for what makes an
   absolute node scale safe to use as a multiplier here.

   No "nothing loaded" guard: this reads no tuning at all, so a car with an
   empty .crs gets the same 0.95 as one with a full one. The level is clamped
   rather than passed through, because the engine's own callers bounds-check it
   before the lookup and never reach the 100.0 the lookup returns otherwise. */
float carani_tire_width(const rb_car *c)
{
    static const float tab[4] = CARANI_TIRE_WIDTH_TABLE;
    int lvl;

    if (!c)
        return tab[0];
    /* tire_upgrade is phys+0xe45c, 0..3 -- the same field the mark reads. */
    lvl = c->tire_upgrade;
    if (lvl < 0) lvl = 0;
    if (lvl > 3) lvl = 3;
    return tab[lvl];
}

/* Compose `local` down the part tree and derive the draw matrices. Parents
   always come before their children -- the exporter appends a part when it
   enters the node -- so one forward pass is enough. */
static void compose(carani_t *r, const float local[][16])
{
    int i;
    for (i = 0; i < r->n; i++) {
        int p = r->part[i].parent;
        if (p < 0 || p >= i)
            memcpy(r->world[i], local[i], sizeof(float) * 16);
        else
            mat_mul(local[i], r->world[p], r->world[i]);
        mat_mul(r->part[i].rest_inv, r->world[i], r->draw[i]);
    }
}

/* The springs run after the compose: each one needs its axle's FINAL world
   matrix, and no spring is a parent of anything. */
static void update_springs(carani_t *r)
{
    int s;
    for (s = 0; s < r->n_springs; s++) {
        int p = r->spring[s], ax = r->spring_axle[s];
        const float *rest = r->part[p].rest;
        float z[3], aim[3], dir[3], rot[16], w[16], len;

        z[0] = rest[8]; z[1] = rest[9]; z[2] = rest[10];
        normalize3(z);
        xform_point(r->world[ax], r->spring_aim[s], aim);
        dir[0] = (float)((double)aim[0] - rest[12]);
        dir[1] = (float)((double)aim[1] - rest[13]);
        dir[2] = (float)((double)aim[2] - rest[14]);
        len = normalize3(dir);
        if (len < EPS) {
            dir[0] = z[0]; dir[1] = z[1]; dir[2] = z[2];
            len = CARANI_SPRING_LEN;
        }

        rot_align(z, dir, rot);
        mat_mul(rest, rot, w);
        /* The rotation moved the origin too; put it back. 0x0040c880 mode 0. */
        w[12] = rest[12]; w[13] = rest[13]; w[14] = rest[14];
        /* Stretch along the spring's own +Z to actually reach the aim point.
           0x0040c9a0 mode 1 pre-multiplies, i.e. it scales the basis rows, so
           the translation is untouched. */
        {
            float sc[16];
            mat_identity(sc);
            sc[10] = (float)((double)len / CARANI_SPRING_LEN);
            mat_premul(w, sc);
        }
        memcpy(r->world[p], w, sizeof(w));
        mat_mul(r->part[p].rest_inv, w, r->draw[p]);
    }
}

/* Body-space height of a wheel centre. carAniProc1 gets this by calling
   carWheelFrame (through the 0x004ef9b0 thunk, so WITH len_extra) and pushing
   the world point back through the car's inverse matrix, which is an exact round
   trip to the local point the frame was built from. */
static float wheel_y(const rb_car *c, int i)
{
    return (float)((double)c->wheel[i].mount[1] - c->wheel[i].len
                   - c->wheel[i].len_extra);
}

/* Reference height the axle angle is measured against: phys+0x5d48, which is the
 * ani context's first entry -- WHEEL_FRONT_LEFT's rest position in body space.
 * In this port a wheel's rest centre is mount_y - (len_free - sag), which
 * gen_rb_data.py makes exactly 0 by putting the centre of mass on the
 * wheel-centre plane. Written out rather than hardcoded so it survives a change
 * of that convention. */
static float rest_y(const rb_car *c)
{
    const rb_wheel *w = &c->wheel[0];
    return (float)((double)w->mount[1] - ((double)w->len_free - w->sag));
}

/* One axle. `a` is its LEFT wheel and `b` its right; the front pair and the rear
 * pair take opposite tilt signs because their node frames are mirrored.
 *
 * The original clamps the two ends differently -- the rear clamps -pitch to
 * [-90, 30] and the front clamps +pitch to [-30, 90] -- which is the same window
 * on the raw angle, so one clamp covers both.
 */
static void axle_local(const carani_t *r, const rb_car *c, int part,
                       int a, int b, float len_axe, int front, float out[16])
{
    double ya = wheel_y(c, a), yb = wheel_y(c, b);
    double ref = rest_y(c);
    float tilt = (float)(atan2((ya - yb) * 4.0, 1.0) * RB_RAD2DEG);
    float pitch = (float)(atan2(((ya + yb) * 0.5 - ref) / len_axe, 1.0)
                          * RB_RAD2DEG);
    float rot[16];

    if (pitch < -30.0f) pitch = -30.0f;
    else if (pitch > 90.0f) pitch = 90.0f;

    memcpy(out, r->part[part].local, sizeof(float) * 16);
    rot_z(rot, -pitch);
    mat_premul(out, rot);
    rot_x(rot, front ? -tilt : tilt);
    mat_premul(out, rot);
}

/* The two spring halves of proc2 and proc3 aim at EACH OTHER -- there is no aim
 * point on an axle and no stretch, only the rotation that puts each half's long
 * axis on the line between them (0x005058c0 and 0x00506a20, both ending in
 * mat4Mul(world, align) with the origin written back).
 *
 * Runs after the compose, on world matrices: the lower half rides a wishbone or
 * an axle and its position is not known until then. The origins never move, so
 * updating one half in place cannot disturb the other. */
static void update_spring_pairs(carani_t *r)
{
    int k, h, row = r->spring_axis * 4;

    for (k = 0; k < r->n_pairs; k++)
        for (h = 0; h < 2; h++) {
            int a = r->spair[k][h], b = r->spair[k][h ^ 1];
            float *wa = r->world[a];
            const float *wb = r->world[b];
            float axis[3], dir[3], rot[16], w[16];

            axis[0] = wa[row]; axis[1] = wa[row + 1]; axis[2] = wa[row + 2];
            if (normalize3(axis) < EPS)
                continue;
            dir[0] = (float)((double)wb[12] - wa[12]);
            dir[1] = (float)((double)wb[13] - wa[13]);
            dir[2] = (float)((double)wb[14] - wa[14]);
            if (normalize3(dir) < EPS)
                continue;                    /* coincident: leave it as it is */

            rot_align(axis, dir, rot);
            mat_mul(wa, rot, w);
            w[12] = wa[12]; w[13] = wa[13]; w[14] = wa[14];
            memcpy(wa, w, sizeof(w));
            mat_mul(r->part[a].rest_inv, w, r->draw[a]);
        }
}

/* Steering and rolling, which every proc does and only the Buggy and the Hummer
 * do to the wheel nodes themselves. `steer_only` skips the spin, for the flat
 * path below. */
static void wheels_local(carani_t *r, const rb_car *c, float steer, float width,
                         float local[][16])
{
    float rot[16];
    int i;

    /* Steering. On the Overkill and the Hummer it turns the front knuckles;
       proc2 has none, so the Buggy's front WHEEL nodes take it directly
       (0x005057e6 hands the same angle to the first two entries of the node
       table, which are the wheels). Either way carWheelFrame reads the same
       phys+0x5c6c for the tire forces, so what is drawn and what grips cannot
       drift apart. */
    rot_y(rot, steer);
    for (i = 0; i < 2; i++)
        if (r->support[i] >= 0)
            mat_premul(local[r->support[i]], rot);
    if (r->proc == 2) {
        for (i = 0; i < 2; i++)
            if (r->pair_front[i] >= 0 && r->wheel[r->pair_front[i]] >= 0)
                mat_premul(local[r->wheel[r->pair_front[i]]], rot);
    }
    /* The Hummer's middle pair steers with the front at 0.3x, again in the wheel
       node -- 0x005068f6, and the physics gives that axle a third. */
    if (r->proc == 3) {
        rot_y(rot, (float)((double)steer * CARANI_MIDDLE_STEER));
        for (i = 0; i < 2; i++)
            if (r->pair_middle[i] >= 0 && r->wheel[r->pair_middle[i]] >= 0)
                mat_premul(local[r->wheel[r->pair_middle[i]]], rot);
    }

    /* Rolling, and the tuning's tyre width -- both about the wheel node's local
       +Z, which is the axle direction. spin is in radians; the helper takes
       degrees. spin_sign is which way that axle points in model space; see
       carani.h.

       The width is a scale premultiplied the way 0x0040c9a0 mode 1 does it,
       because that is what FUN_0050be40 calls -- it scales the node's basis rows
       and leaves its origin alone, so the tyre grows SYMMETRICALLY about the
       wheel node, which is where the contact patch and therefore the mark is.
       The engine sets that row to an absolute length; premultiplying is the same
       thing only because every shipped wheel node's local scale is 1.0 (see
       carani.h). It is applied to `local` so it composes normally, which is safe
       because the wheel nodes are leaves -- none of the three shipped cars packs
       anything with a WHEEL_* parent, and rb_test pins that by checking the
       axles and springs do not move when the width does. */
    for (i = 0; i < c->nwheels && i < RB_MAX_WHEELS; i++) {
        if (r->wheel[i] < 0)
            continue;
        rot_z(rot, (float)((double)c->wheel[i].spin * RB_RAD2DEG
                           * r->spin_sign[i]));
        mat_premul(local[r->wheel[i]], rot);
        if (width != 1.0f) {
            float sc[16];
            mat_identity(sc);
            sc[10] = width;
            mat_premul(local[r->wheel[i]], sc);
        }
    }
}

/* proc2's wishbone, one corner. Both arms swing by the angle that puts the wheel
 * node at the height the physics gives it, and the upright between them takes
 * the opposite angle so the wheel rides up and down without pitching
 * (0x00505ab6 negates the third of the three, which is AXLE_*_WHEEL).
 *
 * The clamp is springs[12]/springs[13] -- 35.45 and 26.36 degrees on the Buggy,
 * and the only car that ships them -- swapped for the right-hand pair, because
 * the two sides' arms are mirrored and so are their arcs. */
static void wishbone_local(carani_t *r, const rb_car *c, int i, int side,
                           float local[][16])
{
    float a, up, down, rot[16];

    if (r->arm_up[i] < 0)
        return;
    a = carani_arc_solve(&r->arm[i],
                         (float)((double)r->arm_rest_y[i]
                                 + wheel_y(c, i) - rest_y(c)));
    up   = c->tune.angle_proc_up;
    down = c->tune.angle_proc_down;
    if (up > EPS || down > EPS) {           /* a car without the keys: no clamp */
        float lo = side ? -up : -down, hi = side ? down : up;
        if (a < lo) a = lo;
        else if (a > hi) a = hi;
    }
    rot_x(rot, a);
    mat_premul(local[r->arm_up[i]], rot);
    if (r->arm_down[i] >= 0)
        mat_premul(local[r->arm_down[i]], rot);
    if (r->arm_knuckle[i] >= 0) {
        rot_x(rot, -a);
        mat_premul(local[r->arm_knuckle[i]], rot);
    }
}

/* One proc3 axle, k = 0 front, 1 rear, 2 middle (0x00506960..0x00506a06).
 *
 * The first two PIVOT: rotZ by the angle that puts the mean of their two wheels
 * at the right height, then rotX by the angle that splits them. The third
 * SLIDES, because nothing about its own origin would raise its wheels, and then
 * rotZ for the split -- its node carries no 90 degree yaw, so what is a pitch on
 * the other two is a roll on it. All three angles come out of the arcs, so the
 * drawn wheel lands where the physics put it whichever it is.
 *
 * The slide is the one thing not taken verbatim: the engine premultiplies its
 * translation in the NODE's frame, where the model's own 1.17 scale then
 * multiplies it, so its axle travels 17% further than its wheels do. This adds
 * the offset in the parent's frame -- the frame the drop was measured in.
 */
static void axle3_local(carani_t *r, const rb_car *c, int k, int part,
                        int a, int b, float local[16])
{
    double ya, yb, mean;
    float ang, rot[16];

    if (part < 0 || a < 0 || b < 0)
        return;
    ya = wheel_y(c, a);                  /* the LEFT wheel: the arcs' own one */
    yb = wheel_y(c, b);
    mean = (ya + yb) * 0.5;

    if (k == 2) {
        /* Slide by the mean drop, clamped, and take the split as a roll. */
        double dy = mean - rest_y(c);
        if (r->mid_heave > EPS) {
            if (dy >  (double)r->mid_heave) dy =  (double)r->mid_heave;
            else if (dy < -(double)r->mid_heave) dy = -(double)r->mid_heave;
        }
        ang = carani_arc_solve(&r->axle_pitch[k],
                               (float)((double)r->axle_rest_y[k] + (ya - mean)));
        rot_z(rot, ang);
        mat_premul(local, rot);
        local[13] = (float)((double)local[13] + dy);
        return;
    }

    /* Pitch on the mean, clamped the way proc3 clamps it (0x005069a0). */
    ang = carani_arc_solve(&r->axle_pitch[k],
                           (float)((double)r->axle_rest_y[k]
                                   + (mean - rest_y(c))));
    if (ang < -90.0f) ang = -90.0f;
    else if (ang > 20.0f) ang = 20.0f;
    rot_z(rot, ang);
    mat_premul(local, rot);

    /* Tilt on what is left over. Measured about the unpitched rest, as the
       engine's own tables are. */
    ang = carani_arc_solve(&r->axle_tilt[k],
                           (float)((double)r->axle_rest_y[k] + (ya - mean)));
    rot_x(rot, ang);
    mat_premul(local, rot);
}

void carani_update(carani_t *r, const rb_car *c)
{
    float local[CARANI_MAX_PARTS][16];
    float len_axe;
    int i;

    if (r->n <= 0)
        return;
    for (i = 0; i < r->n; i++)
        memcpy(local[i], r->part[i].local, sizeof(float) * 16);

    wheels_local(r, c, c->steer, carani_tire_width(c), local);

    if (r->proc == 2) {
        for (i = 0; i < c->nwheels && i < RB_MAX_WHEELS; i++)
            wishbone_local(r, c, i, (c->wheel[i].mount[0] >= 0.0f) ? 0 : 1,
                           local);
    } else if (r->proc == 3) {
        axle3_local(r, c, 0, r->axle_front, r->pair_front[0], r->pair_front[1],
                    r->axle_front >= 0 ? local[r->axle_front] : NULL);
        axle3_local(r, c, 1, r->axle_rear, r->pair_rear[0], r->pair_rear[1],
                    r->axle_rear >= 0 ? local[r->axle_rear] : NULL);
        axle3_local(r, c, 2, r->axle_middle, r->pair_middle[0],
                    r->pair_middle[1],
                    r->axle_middle >= 0 ? local[r->axle_middle] : NULL);
    } else {
        /* Axles. lenAxe (springs[11], 0.089 m on the Overkill) is the lever the
           mean wheel drop is measured against; the 4.0 on the difference is the
           original's own constant, close to 1/(2*half_track) for this car. */
        len_axe = c->tune.len_axe > EPS ? c->tune.len_axe : 0.089f;
        if (r->axle_front >= 0 && r->pair_front[0] >= 0 && r->pair_front[1] >= 0)
            axle_local(r, c, r->axle_front, r->pair_front[0], r->pair_front[1],
                       len_axe, 1, local[r->axle_front]);
        if (r->axle_rear >= 0 && r->pair_rear[0] >= 0 && r->pair_rear[1] >= 0)
            axle_local(r, c, r->axle_rear, r->pair_rear[0], r->pair_rear[1],
                       len_axe, 0, local[r->axle_rear]);
    }

    compose(r, (const float (*)[16])local);
    if (r->proc == 1)
        update_springs(r);
    else
        update_spring_pairs(r);
}

void carani_update_flat(carani_t *r, float steer_deg, float spin_rad)
{
    float local[CARANI_MAX_PARTS][16];
    float rot[16];
    int i;

    if (r->n <= 0)
        return;
    for (i = 0; i < r->n; i++)
        memcpy(local[i], r->part[i].local, sizeof(float) * 16);

    rot_y(rot, steer_deg);
    for (i = 0; i < 2; i++) {
        if (r->support[i] >= 0)
            mat_premul(local[r->support[i]], rot);
        if (r->proc == 2 && r->pair_front[i] >= 0
            && r->wheel[r->pair_front[i]] >= 0)
            mat_premul(local[r->wheel[r->pair_front[i]]], rot);
    }

    for (i = 0; i < RB_MAX_WHEELS; i++)
        if (r->wheel[i] >= 0) {
            rot_z(rot, (float)((double)spin_rad * RB_RAD2DEG * r->spin_sign[i]));
            mat_premul(local[r->wheel[i]], rot);
        }

    compose(r, (const float (*)[16])local);
    if (r->proc == 1)
        update_springs(r);
    else
        update_spring_pairs(r);
}
