/*
 * antenna.c -- the engine's own chain, transcribed. See antenna.h for what
 * here is the game's and what is the port's, and for where each piece lives in
 * the unstripped PS2 image.
 */

#include "antenna.h"
#include "vis_data.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Gravity is 10.0 in this engine, not 9.81 -- see CLAUDE.md. _cdyCalcDynFrame
   builds its weight vector as r3dVUnitY * -10.0 and multiplies by each point's
   own mass, so it is a force and not an acceleration. */
#define ANT_GRAVITY 10.0f

#define ANT_EPS 1e-6f

/* ---------------------------------------------------------------- vectors */

static void v3sub(const float *a, const float *b, float *o)
{
    o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2];
}
static float v3dot(const float *a, const float *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
static float v3len(const float *a) { return sqrtf(v3dot(a, a)); }
static int v3norm(float *a)
{
    float l = v3len(a);
    if (l < ANT_EPS)
        return 0;
    a[0] /= l; a[1] /= l; a[2] /= l;
    return 1;
}

/* A point and a direction through a row-major ROW-VECTOR 4x4: the rows are the
   body axes in world and the last row is the translation. */
static void xform_pt(const float *m, const float *p, float *o)
{
    int j;
    for (j = 0; j < 3; j++)
        o[j] = p[0] * m[0 * 4 + j] + p[1] * m[1 * 4 + j]
             + p[2] * m[2 * 4 + j] + m[3 * 4 + j];
}
static void xform_inv_pt(const float *m, const float *w, float *o)
{
    float d[3];
    int i;
    d[0] = w[0] - m[12]; d[1] = w[1] - m[13]; d[2] = w[2] - m[14];
    for (i = 0; i < 3; i++)
        o[i] = d[0] * m[i * 4 + 0] + d[1] * m[i * 4 + 1] + d[2] * m[i * 4 + 2];
}

/* ------------------------------------------------------------ the config */

static void ant_params(antenna_t *a, int car)
{
    switch (car) {
    case 1:
        a->n = ANT2_POINTS; a->seg = ANT2_SEG; a->mass = ANT2_MASS;
        a->stiffness = ANT2_STIFFNESS; a->damping = ANT2_DAMPING;
        a->wind = ANT2_WIND;
        break;
    case 2:
        a->n = ANT3_POINTS; a->seg = ANT3_SEG; a->mass = ANT3_MASS;
        a->stiffness = ANT3_STIFFNESS; a->damping = ANT3_DAMPING;
        a->wind = ANT3_WIND;
        break;
    default:
        a->n = ANT1_POINTS; a->seg = ANT1_SEG; a->mass = ANT1_MASS;
        a->stiffness = ANT1_STIFFNESS; a->damping = ANT1_DAMPING;
        a->wind = ANT1_WIND;
        break;
    }
    if (a->n > ANT_MAX_POINTS)
        a->n = ANT_MAX_POINTS;
    /* Two of them are the CLAMP, not a minimum for its own sake: both are
       fixed, so a chain of two has nothing to simulate. */
    if (a->n < 3)
        a->n = 3;
    if (a->mass < 1e-4f)
        a->mass = 1e-4f;
}

/* ------------------------------------------------------------- the frames */

/*
 * _cdyCalcPointWCS / _cdyCalcNextPointWCS: point 0 carries the base frame, and
 * every other point's is its predecessor's turned by the MINIMAL rotation that
 * takes the predecessor's X axis onto the direction of the segment between
 * them. The engine spells that as inverse-transform, r3dAngleVector, then
 * cgmCalcRotMatr; it is the rotation-minimising frame, and reproducing it is
 * what lets the damping term below measure a deviation and not a twist.
 *
 * The rest matrices it multiplies in between are IDENTITY on every point --
 * initInPos calls sceVu0UnitMatrix on all of them -- so they drop out.
 */
static void frame_from_pose(const float *m, float *fr)
{
    /* The whip's axis is the model's +Y, so that is the frame's X, which is the
       convention _cdyCalcNextPointWCS propagates. The other two are the model's
       Z and X, in that order, so the frame stays right-handed. */
    fr[0] = m[4]; fr[1] = m[5]; fr[2] = m[6];       /* X <- model +Y */
    fr[3] = m[8]; fr[4] = m[9]; fr[5] = m[10];      /* Y <- model +Z */
    fr[6] = m[0]; fr[7] = m[1]; fr[8] = m[2];       /* Z <- model +X */
}

static void frames_build(antenna_t *a, int st, const float *pose)
{
    int i, k, j;

    frame_from_pose(pose, a->fr[st][0]);
    for (i = 1; i < a->n; i++) {
        const float *pf = a->fr[st][i - 1];
        float *nf = a->fr[st][i];
        float d[3], loc[3], R[9], c, kk;

        v3sub(a->p[st][i], a->p[st][i - 1], d);
        if (!v3norm(d)) {
            memcpy(nf, pf, 9 * sizeof(float));
            continue;
        }
        /* the direction in the predecessor's own frame */
        for (j = 0; j < 3; j++)
            loc[j] = v3dot(d, pf + j * 3);
        c = loc[0];
        if (c <= -1.f + 1e-6f) {
            /* antiparallel: the engine takes cgmRotationAroundRay 180 degrees
               about any axis square to X. Half a turn about Z is one. */
            R[0] = -1.f; R[1] = 0.f;  R[2] = 0.f;
            R[3] = 0.f;  R[4] = -1.f; R[5] = 0.f;
            R[6] = 0.f;  R[7] = 0.f;  R[8] = 1.f;
        } else {
            /* Rodrigues from (1,0,0) to loc: column 0 comes out as loc. */
            kk = 1.f / (1.f + c);
            R[0] = c;                       R[1] = -loc[1];
            R[2] = -loc[2];
            R[3] = loc[1];                  R[4] = 1.f - kk * loc[1] * loc[1];
            R[5] = -kk * loc[1] * loc[2];
            R[6] = loc[2];                  R[7] = -kk * loc[1] * loc[2];
            R[8] = 1.f - kk * loc[2] * loc[2];
        }
        /* The new frame's axes are R's COLUMNS read in the old frame. */
        for (k = 0; k < 3; k++)
            for (j = 0; j < 3; j++)
                nf[k * 3 + j] = R[0 * 3 + k] * pf[0 * 3 + j]
                              + R[1 * 3 + k] * pf[1 * 3 + j]
                              + R[2 * 3 + k] * pf[2 * 3 + j];
    }
}

/* ------------------------------------------------------------ the anchors */

/* Model-space position of fixed point k (0 = the mesh's base, 1 one segment up
   its own axis), through a pose. */
static void anchor_at(const antenna_t *a, const float *pose, int k, float *o)
{
    float mp[3];
    mp[0] = a->base[0];
    mp[1] = a->base_y + a->seg * (float)k;
    mp[2] = a->base[2];
    xform_pt(pose, mp, o);
}

/* ------------------------------------------------------------- the forces */

static void ant_forces(antenna_t *a, int cur, int prev, float h)
{
    int i, j;

    /* weight, per point, as a FORCE */
    for (i = 0; i < a->n; i++) {
        a->f[i][0] = 0.f;
        a->f[i][1] = -ANT_GRAVITY * a->mass;
        a->f[i][2] = 0.f;
    }

    /* There is no wind term. getUserForceCB scales _carAntennaDirZ by 0.0f and
       hands every other point the zero vector; `wind` is read out of the config
       and deliberately unused. */
    (void)a->wind;

    /* _cdyAddLocalStiffness, over the free points only (0 and 1 are fixed). */
    for (i = 2; i < a->n; i++) {
        float seg[3], s[3], rest[3], d[3], ang, mag, c;
        float L;

        v3sub(a->p[cur][i], a->p[cur][i - 1], seg);
        L = v3len(seg);
        if (L < ANT_EPS)
            continue;
        for (j = 0; j < 3; j++)
            s[j] = seg[j] / L;
        /* the rest direction is the PREVIOUS segment's -- the X axis of point
           i-1's frame, which _cdyCalcNextPointWCS makes exactly that */
        memcpy(rest, a->fr[cur][i - 1], 3 * sizeof(float));

        c = v3dot(rest, s);
        if (c > 1.f) c = 1.f;
        if (c < -1.f) c = -1.f;
        ang = acosf(c);
        mag = a->stiffness * ang / L;

        v3sub(rest, s, d);
        c = v3dot(s, d);
        for (j = 0; j < 3; j++)
            d[j] -= s[j] * c;
        if (!v3norm(d))
            continue;
        for (j = 0; j < 3; j++) {
            a->f[i][j]     += d[j] * mag;
            a->f[i - 1][j] -= d[j] * mag;
        }
    }

    /* _cdyAddDamping: carry the previous step's point through the previous
       step's frame of its predecessor and out through this step's, and damp
       whatever it did NOT follow. Divided by dt and by the segment length, so
       the coefficient the config calls `damping` is dimensionless. */
    if (h > ANT_EPS) {
        for (i = 2; i < a->n; i++) {
            float seg[3], rel[3], loc[3], q[3], d[3];
            float L;
            const float *pf = a->fr[prev][i - 1];
            const float *cf = a->fr[cur][i - 1];

            v3sub(a->p[cur][i], a->p[cur][i - 1], seg);
            L = v3len(seg);
            if (L < ANT_EPS)
                continue;
            v3sub(a->p[prev][i], a->p[prev][i - 1], rel);
            for (j = 0; j < 3; j++)
                loc[j] = v3dot(rel, pf + j * 3);
            for (j = 0; j < 3; j++)
                q[j] = a->p[cur][i - 1][j] + loc[0] * cf[0 * 3 + j]
                     + loc[1] * cf[1 * 3 + j] + loc[2] * cf[2 * 3 + j];
            v3sub(a->p[cur][i], q, d);
            for (j = 0; j < 3; j++)
                d[j] /= h * L;
            for (j = 0; j < 3; j++) {
                a->f[i][j]     -= a->damping * d[j];
                a->f[i - 1][j] += a->damping * d[j];
            }
        }
    }
}

/* ------------------------------------------------------- the tension solve */

/*
 * dynChainCalcAccelerations, on the sub-chain that starts at the LAST fixed
 * point. The wrapper splits the chain at every fixed point and skips a link
 * with both ends held -- which link 0 is here, points 0 and 1 both being fixed
 * -- so the system is assembled over points 1..n-1 and its first diagonal is
 * non-zero. Leaving point 0 in would put a zero pivot in row 0 and
 * _dynSolveSystem would refuse the whole frame.
 */
static int solve_tridiag(int m, float *A, float *B, float *C, float *R)
{
    int i;
    for (i = 0; i < m - 1; i++) {
        if (fabsf(B[i]) < ANT_EPS)
            return 0;
        B[i + 1] -= C[i] * A[i + 1] / B[i];
        R[i + 1] -= R[i] * A[i + 1] / B[i];
    }
    for (i = m - 1; i > 0; i--) {
        if (fabsf(B[i]) < ANT_EPS)
            return 0;
        R[i - 1] -= R[i] * C[i - 1] / B[i];
    }
    for (i = 0; i < m; i++) {
        if (fabsf(B[i]) < ANT_EPS)
            return 0;
        R[i] /= B[i];
    }
    return 1;
}

static void ant_accel(antenna_t *a, int cur)
{
    const int s = 1;                /* the last fixed point starts the system */
    const int np = a->n - s;        /* points in it */
    const int nl = np - 1;          /* links, and unknowns */
    float u[ANT_MAX_POINTS][3];
    float w[ANT_MAX_POINTS];
    float ex[ANT_MAX_POINTS][3];
    float A[ANT_MAX_POINTS], B[ANT_MAX_POINTS];
    float C[ANT_MAX_POINTS], R[ANT_MAX_POINTS];
    int i, j;

    for (i = 0; i < np; i++) {
        /* point s+i: fixed ones weigh nothing and carry the acceleration they
           were driven to; free ones carry F/m */
        w[i] = (s + i < 2) ? 0.f : 1.f / a->mass;
        for (j = 0; j < 3; j++)
            ex[i][j] = (s + i < 2) ? a->a[s + i][j] : a->f[s + i][j] * w[i];
    }
    for (i = 0; i < nl; i++)
        v3sub(a->p[cur][s + i], a->p[cur][s + i + 1], u[i]);

    for (i = 0; i < nl; i++) {
        B[i] = (w[i] + w[i + 1]) * v3dot(u[i], u[i]);
        A[i] = (i > 0) ? -w[i] * v3dot(u[i - 1], u[i]) : 0.f;
        C[i] = (i < nl - 1) ? -w[i + 1] * v3dot(u[i], u[i + 1]) : 0.f;
        {
            float da[3], dv[3];
            v3sub(ex[i + 1], ex[i], da);
            v3sub(a->v[cur][s + i], a->v[cur][s + i + 1], dv);
            R[i] = v3dot(u[i], da) - v3dot(dv, dv);
        }
    }

    if (!solve_tridiag(nl, A, B, C, R)) {
        /* The engine abandons the whole frame here. The port keeps the free
           fall and lets _cdyTightenPoints put the lengths back -- a whip that
           stops for a frame is a worse answer than one that is briefly soft,
           and this only happens on a degenerate chain. */
        for (i = 2; i < a->n; i++)
            for (j = 0; j < 3; j++)
                a->a[i][j] = a->f[i][j] / a->mass;
        return;
    }

    for (i = 0; i < np; i++) {
        float acc[3];
        if (s + i < 2)
            continue;               /* fixed: its acceleration is prescribed */
        memcpy(acc, a->f[s + i], sizeof(acc));
        if (i > 0)
            for (j = 0; j < 3; j++)
                acc[j] -= R[i - 1] * u[i - 1][j];
        if (i < nl)
            for (j = 0; j < 3; j++)
                acc[j] += R[i] * u[i][j];
        for (j = 0; j < 3; j++)
            a->a[s + i][j] = acc[j] * w[i];
    }
}

/* ------------------------------------------------------------ the tighten */

/* _cdyTightenPoints: put each free point back on its rod, and give it its
   predecessor's along-segment velocity, so nothing moves along a rod.
 *
 * The engine re-projects onto the PREVIOUS STATE's measured length, which for a
 * chain that started at `seg` and has been tightened every step since is `seg`
 * -- the two are the same number and the loop is drift-free either way. This
 * uses `seg` because it is also self-CORRECTING: a chain that got stretched
 * once, by a pose that jumped further in a step than a segment is long, comes
 * back. Measuring the previous state instead locks that stretch in for good,
 * which is a real failure mode here and is not one in the engine, where the
 * anchor is an instance that never teleports without initInPos.
 */
static void ant_tighten(antenna_t *a, int cur, int nw)
{
    int i, j;

    (void)cur;
    for (i = 2; i < a->n; i++) {
        float d[3], L, vp, vn;

        v3sub(a->p[nw][i], a->p[nw][i - 1], d);
        if (!v3norm(d))
            continue;
        L = a->seg;
        for (j = 0; j < 3; j++)
            a->p[nw][i][j] = a->p[nw][i - 1][j] + d[j] * L;
        vp = v3dot(d, a->v[nw][i - 1]);
        vn = v3dot(d, a->v[nw][i]);
        for (j = 0; j < 3; j++)
            a->v[nw][i][j] += d[j] * (vp - vn);
    }
}

/* ---------------------------------------------------------------- the pose */

static void mat_to_quat(const float *m, float *q)
{
    /* rows are the axes; the trace form, with the largest-diagonal branch */
    float t = m[0] + m[5] + m[10], r;
    if (t > 0.f) {
        r = sqrtf(t + 1.f);
        q[3] = 0.5f * r;
        r = 0.5f / r;
        q[0] = (m[6] - m[9]) * r;
        q[1] = (m[8] - m[2]) * r;
        q[2] = (m[1] - m[4]) * r;
    } else {
        int i = 0, j, k;
        if (m[5] > m[0]) i = 1;
        if (m[10] > m[i * 4 + i]) i = 2;
        j = (i + 1) % 3; k = (j + 1) % 3;
        r = sqrtf(m[i * 4 + i] - m[j * 4 + j] - m[k * 4 + k] + 1.f);
        q[i] = 0.5f * r;
        r = 0.5f / r;
        q[3] = (m[j * 4 + k] - m[k * 4 + j]) * r;
        q[j] = (m[i * 4 + j] + m[j * 4 + i]) * r;
        q[k] = (m[i * 4 + k] + m[k * 4 + i]) * r;
    }
}

static void quat_to_mat(const float *q, float *m)
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0]  = 1.f - 2.f * (y * y + z * z);
    m[1]  =       2.f * (x * y + z * w);
    m[2]  =       2.f * (x * z - y * w);
    m[4]  =       2.f * (x * y - z * w);
    m[5]  = 1.f - 2.f * (x * x + z * z);
    m[6]  =       2.f * (y * z + x * w);
    m[8]  =       2.f * (x * z + y * w);
    m[9]  =       2.f * (y * z - x * w);
    m[10] = 1.f - 2.f * (x * x + y * y);
    m[3] = m[7] = m[11] = 0.f;
    m[15] = 1.f;
}

/* The anchor pose at fraction `t` of the frame. The engine slerps
   (cgmSlerpRotMatrContinuous) and lerps the origin; over 7 ms an nlerp is the
   same answer to four more digits than anything downstream can see. */
static void ant_pose(const antenna_t *a, float t, float *out)
{
    float q0[4], q1[4], q[4], l;
    int i;

    mat_to_quat(a->m_prev, q0);
    mat_to_quat(a->m_cur, q1);
    if (q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3] < 0.f)
        for (i = 0; i < 4; i++) q1[i] = -q1[i];
    for (i = 0; i < 4; i++)
        q[i] = q0[i] + (q1[i] - q0[i]) * t;
    l = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (l < ANT_EPS) {
        memcpy(out, a->m_cur, 16 * sizeof(float));
        return;
    }
    for (i = 0; i < 4; i++) q[i] /= l;
    quat_to_mat(q, out);
    for (i = 0; i < 3; i++)
        out[12 + i] = a->m_prev[12 + i]
                    + (a->m_cur[12 + i] - a->m_prev[12 + i]) * t;
}

/* --------------------------------------------------------------- planting */

/* initInPos: the chain straight up its own axis, at rest, in the current pose.
   All three ring slots, so the damping term has a sane predecessor state. */
static void ant_plant(antenna_t *a, const float *pose)
{
    int st, i, j;

    for (i = 0; i < a->n; i++) {
        float mp[3], w[3];
        mp[0] = a->base[0];
        mp[1] = a->base_y + a->seg * (float)i;
        mp[2] = a->base[2];
        xform_pt(pose, mp, w);
        for (st = 0; st < 3; st++)
            for (j = 0; j < 3; j++) {
                a->p[st][i][j] = w[j];
                a->v[st][i][j] = 0.f;
            }
        for (j = 0; j < 3; j++)
            a->a[i][j] = 0.f;
    }
    for (st = 0; st < 3; st++)
        frames_build(a, st, pose);
    a->cur = 0;
    a->accum = 0.f;
}

/* ------------------------------------------------------------------ init */

void antenna_init(antenna_t *a, scene_t *car, int car_index)
{
    int part = -1, i, k;
    float lo = 1e30f, hi = -1e30f, sx = 0.f, sz = 0.f;
    batch_t *b = NULL;
    static const float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    memset(a, 0, sizeof(*a));
    a->part = -1;
    if (!car || !car->has_rig)
        return;
    for (i = 0; i < car->rig.n; i++)
        if (!strcmp(car->rig.part[i].name, "ANTENNA")) { part = i; break; }
    if (part < 0)
        return;
    /* Recorded before the early returns below: the menu's framing wants to know
       which part the whip is even on a scene whose chain cannot be bound. */
    a->part = part;
    for (i = 0; i < (int)car->n_batches; i++)
        if ((int)car->batches[i].part == part) { b = &car->batches[i]; break; }
    if (!b || !b->nverts || !scene_keep_rest(b))
        return;

    ant_params(a, car_index);

    /* The mesh's own extent along its axis is what the chain is mapped onto.
       Reading it beats hardcoding: the three cars' antennae differ, and the
       Buggy's chainLength is 0.35 m against the Overkill's 0.25. */
    for (k = 0; k < (int)b->nverts; k++) {
        if (b->rest[k].y < lo) lo = b->rest[k].y;
        if (b->rest[k].y > hi) hi = b->rest[k].y;
        sx += b->rest[k].x;
        sz += b->rest[k].z;
    }
    a->batch = b;
    a->base_y = lo;
    a->tip_y = hi;
    a->base[0] = sx / (float)b->nverts;
    a->base[1] = lo;
    a->base[2] = sz / (float)b->nverts;

    memcpy(a->m_prev, ident, sizeof(ident));
    memcpy(a->m_cur, ident, sizeof(ident));
    ant_plant(a, ident);
    a->primed = 0;
    a->ready = 1;
}

/* ------------------------------------------------------------- the step */

static void ant_substep(antenna_t *a, const float *pose, float h)
{
    const int cur = a->cur;
    const int prev = (cur + 2) % 3;
    const int nw = (cur + 1) % 3;
    float at[2][3], av[2][3];
    int i, j;

    /* WHERE THE ANCHORS WILL BE at the end of this step, and the velocity and
       acceleration that takes -- which is all cdyCalcDynFrame does with a point
       whose flags carry bit 0, and the ONLY route the car's motion has into the
       chain. The positions are not applied yet: the forces and the constraint
       solve below run on a state whose two ends are where the free points last
       saw them, or the solve is handed a rod that is already stretched by a
       step of the car's travel and answers with nonsense. */
    for (i = 0; i < 2 && i < a->n; i++) {
        anchor_at(a, pose, i, at[i]);
        for (j = 0; j < 3; j++) {
            av[i][j] = (at[i][j] - a->p[cur][i][j]) / h;
            a->a[i][j] = (av[i][j] - a->v[cur][i][j]) / h;
        }
    }

    ant_forces(a, cur, prev, h);
    ant_accel(a, cur);

    for (i = 0; i < a->n; i++) {
        if (i < 2) {
            /* the anchors land on the pose */
            memcpy(a->p[nw][i], at[i], 3 * sizeof(float));
            memcpy(a->v[nw][i], av[i], 3 * sizeof(float));
            continue;
        }
        /* v += a*h/2 then p += v*h -- the engine's own two lines, and the half
           step is its own, not a transcription slip. */
        for (j = 0; j < 3; j++) {
            a->v[nw][i][j] = a->v[cur][i][j] + a->a[i][j] * (h * 0.5f);
            a->p[nw][i][j] = a->p[cur][i][j] + a->v[nw][i][j] * h;
        }
    }

    ant_tighten(a, cur, nw);
    /* _cdyCalcPointWCS, once per step, on the state just written */
    frames_build(a, nw, pose);
    a->cur = nw;
}

void antenna_step(antenna_t *a, const float *m, float dt)
{
    int steps;

    if (!a->ready || !m)
        return;

    memcpy(a->m_prev, a->m_cur, 16 * sizeof(float));
    memcpy(a->m_cur, m, 16 * sizeof(float));

    if (!a->primed) {
        memcpy(a->m_prev, m, 16 * sizeof(float));
        ant_plant(a, m);
        a->primed = 1;
    } else {
        /* A teleport re-plants the chain rather than whipping it across the
           map -- the engine's own 2.5 m test at the top of process(). */
        float w0[3], w1[3], d[3];
        anchor_at(a, a->m_prev, 0, w0);
        anchor_at(a, a->m_cur, 0, w1);
        v3sub(w1, w0, d);
        if (v3len(d) > ANT_RESET_JUMP) {
            memcpy(a->m_prev, m, 16 * sizeof(float));
            ant_plant(a, m);
        }
    }

    if (dt <= 0.f)
        return;
    a->accum += dt;
    if (a->accum > ANT_STEP * (float)ANT_MAX_STEPS)
        a->accum = ANT_STEP * (float)ANT_MAX_STEPS;

    for (steps = 0; steps < ANT_MAX_STEPS && a->accum > ANT_EPS; steps++) {
        float total = a->accum;
        float h = total > ANT_STEP ? ANT_STEP : total;
        float rem = total - h;
        float pose[16], t;

        if (rem < ANT_STEP_FOLD) { h += rem; rem = 0.f; }
        a->accum = rem;
        t = (dt > ANT_EPS) ? (dt - rem) / dt : 1.f;
        if (t < 0.f) t = 0.f;
        if (t > 1.f) t = 1.f;
        ant_pose(a, t, pose);
        ant_substep(a, pose, h);
    }
}

/* ------------------------------------------------------------- the draw */

void antenna_apply(antenna_t *a)
{
    batch_t *b;
    unsigned int k;
    float span;
    float mp[ANT_MAX_POINTS][3];
    int i;

    if (!a->ready)
        return;
    b = a->batch;
    span = a->tip_y - a->base_y;
    if (span < 1e-5f)
        return;

    /* The chain is simulated in world; the batch is drawn under the car's own
       matrix, so it comes back through the inverse of the matrix the last step
       ran against. */
    for (i = 0; i < a->n; i++)
        xform_inv_pt(a->m_cur, a->p[a->cur][i], mp[i]);

    for (k = 0; k < b->nverts; k++) {
        const vtx_t *r = &b->rest[k];
        /* where this vertex sits along the whip, 0 at the base and 1 at the tip */
        float t = (r->y - a->base_y) / span;
        float f, cur[3];
        int j;

        if (t < 0.f) t = 0.f;
        if (t > 1.f) t = 1.f;
        /* sample the chain: segment i, fraction f within it */
        f = t * (float)(a->n - 1);
        i = (int)f;
        if (i > a->n - 2) i = a->n - 2;
        f -= (float)i;
        for (j = 0; j < 3; j++)
            cur[j] = mp[i][j] + (mp[i + 1][j] - mp[i][j]) * f;

        /* Keep the vertex's offset from the mesh's own axis -- that is what
           gives the tube its thickness. Only the axis itself is bent. */
        b->verts[k].x = cur[0] + (r->x - a->base[0]);
        b->verts[k].y = cur[1];
        b->verts[k].z = cur[2] + (r->z - a->base[2]);
    }
}
