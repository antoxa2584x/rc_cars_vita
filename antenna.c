/*
 * antenna.c -- see antenna.h for what here is the game's and what is the port's.
 */

#include "antenna.h"
#include "vis_data.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Gravity is 10.0 in this engine, not 9.81 -- see CLAUDE.md. */
#define ANT_GRAVITY 10.0f

/* How hard the whip wants to be straight, per unit of the config's `stiffness`.
 *
 * NOT a recovered value. `stiffness` clamps to 10.0 and the spline that shapes
 * it along the whip -- Antenna_Car<n>.ini's "Height2Stiffness", a Mask spline
 * with no data in the shipped files -- is not recovered. This gain is chosen so
 * the static droop under gravity is a few percent of the length: a real whip
 * antenna stands up. At stiffness 10 that is 750/m against gravity's 10 m/s^2,
 * so the tip hangs about 13 mm off vertical out of 267 mm.
 *
 * The first version had NO bending term at all, only gravity and the segment
 * constraints, which is the definition of a hanging chain. It lay down like
 * wire, which is exactly what it was told to do. */
#define ANT_STIFF_GAIN 75.0f

/* The chain is stiff and short, so it is integrated faster than the frame. */
#define ANT_SUBSTEP (1.f / 480.f)
#define ANT_MAX_SUBSTEPS 16

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
    if (a->n < 2)
        a->n = 2;
}

void antenna_init(antenna_t *a, scene_t *car, int car_index)
{
    int part = -1, i, k;
    float lo = 1e30f, hi = -1e30f, sx = 0.f, sz = 0.f;
    batch_t *b = NULL;

    memset(a, 0, sizeof(*a));
    if (!car || !car->has_rig)
        return;
    for (i = 0; i < car->rig.n; i++)
        if (!strcmp(car->rig.part[i].name, "ANTENNA")) { part = i; break; }
    if (part < 0)
        return;
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

    /* start straight up from the anchor */
    for (i = 0; i < a->n; i++) {
        a->p[i][0] = a->base[0];
        a->p[i][1] = a->base[1] + a->seg * (float)i;
        a->p[i][2] = a->base[2];
        a->v[i][0] = a->v[i][1] = a->v[i][2] = 0.f;
    }
    a->ready = 1;
}

void antenna_step(antenna_t *a, const float *m, const float accel[3],
                  float speed, float dt)
{
    float la[3], up[3];
    int i, k, pass, steps;
    float h;

    if (!a->ready)
        return;

    /* World -> body. The matrix is row-major row-vector, so its rows are the
       body axes in world and a world vector projects onto them by dot. */
    for (i = 0; i < 3; i++)
        la[i] = accel[0] * m[i * 4 + 0] + accel[1] * m[i * 4 + 1]
              + accel[2] * m[i * 4 + 2];
    /* world up in body space, so gravity pulls the right way when the car rolls */
    up[0] = m[1]; up[1] = m[5]; up[2] = m[9];

    steps = (int)(dt / ANT_SUBSTEP) + 1;
    if (steps > ANT_MAX_SUBSTEPS)
        steps = ANT_MAX_SUBSTEPS;
    h = dt / (float)steps;

    for (k = 0; k < steps; k++) {
        for (i = 1; i < a->n; i++) {
            float t = (float)i / (float)(a->n - 1);
            float tgt[3], ext[3], d[3] = {0.f, 0.f, 0.f}, len = 0.f;
            int j;

            /* BENDING, cantilever: each joint springs toward the straight
               continuation of the segment before it. That is what makes the
               whip CURVE -- every joint contributes, so deflection accumulates
               toward the tip. A spring toward the whole whip's rest pose, which
               is what this did before, deflects every point by the same amount:
               the chain stays straight and pivots at the base, which is the
               "bounces at the bottom but not the top" that was reported.
             *
             * The buckling that made a cantilever unusable earlier is handled
             * below, by taking the axial component out of the external forces
             * rather than by abandoning the model. */
            if (i == 1) {
                d[0] = 0.f; d[1] = 1.f; d[2] = 0.f;
            } else {
                for (j = 0; j < 3; j++)
                    d[j] = a->p[i - 1][j] - a->p[i - 2][j];
                len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (len < 1e-6f) {
                    d[0] = 0.f; d[1] = 1.f; d[2] = 0.f;
                } else {
                    for (j = 0; j < 3; j++)
                        d[j] /= len;
                }
            }
            for (j = 0; j < 3; j++)
                tgt[j] = a->p[i - 1][j] + d[j] * a->seg;

            /* External forces: gravity, the body's acceleration felt as an
               inertial force, and drag from the airstream along -Z. */
            for (j = 0; j < 3; j++)
                ext[j] = -up[j] * ANT_GRAVITY - la[j];
            ext[2] -= a->wind * speed * t;

            /* Take out the component ALONG the whip. An inextensible rod
               carries axial load as tension or compression, not as bending --
               and leaving it in is what buckled the cantilever: gravity
               compresses the column, the length constraint refuses to let it
               shorten, and it folds sideways into a stable wrong answer.
               Removing it costs nothing physical and removes the buckling mode
               with it. */
            {
                float u[3], ulen, ax;
                for (j = 0; j < 3; j++)
                    u[j] = a->p[i][j] - a->p[i - 1][j];
                ulen = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
                if (ulen > 1e-6f) {
                    for (j = 0; j < 3; j++)
                        u[j] /= ulen;
                    ax = ext[0] * u[0] + ext[1] * u[1] + ext[2] * u[2];
                    for (j = 0; j < 3; j++)
                        ext[j] -= u[j] * ax;
                }
            }

            for (j = 0; j < 3; j++) {
                float f = (tgt[j] - a->p[i][j]) * a->stiffness * ANT_STIFF_GAIN;
                f += ext[j];
                a->v[i][j] += f * h;
                a->v[i][j] -= a->v[i][j] * a->damping * h;
                a->p[i][j] += a->v[i][j] * h;
            }
        }

        /* Enforce the segment lengths and the anchor. Two passes is enough for
           a four-link chain and keeps it stiff without a stiff force term --
           `stiffness` scales how hard the correction pulls. */
        for (pass = 0; pass < 2; pass++) {
            /* p[0] is the anchor and nothing ever writes it -- the force
               loop starts at i = 1 and the constraint only moves p[i], i >= 1.
               There used to be a re-anchor here; a mutation deleting it changed
               nothing, which is how dead code announces itself. */
            /* full projection: inextensibility is not a stiffness, it is a
               constraint. `stiffness` drives the BENDING term above. */
            const float s = 1.f;
            for (i = 1; i < a->n; i++) {
                float d[3], len, corr;
                int j;
                for (j = 0; j < 3; j++)
                    d[j] = a->p[i][j] - a->p[i - 1][j];
                len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (len < 1e-6f) {
                    a->p[i][1] += a->seg;
                    continue;
                }
                corr = (len - a->seg) / len * s;
                for (j = 0; j < 3; j++)
                    a->p[i][j] -= d[j] * corr;
                /* No velocity feedback here. The constraint is a positional
                   projection along the segment; the bending spring and the
                   damping own the dynamics. Feeding `move / h` back into the
                   velocity -- which the first version did, when there was no
                   spring for it to fight -- injects an impulse that scales with
                   1/h, and at h = 1/480 it kept the whip ringing instead of
                   settling. */
            }
        }
    }
}

void antenna_apply(antenna_t *a)
{
    batch_t *b;
    unsigned int k;
    float span;

    if (!a->ready)
        return;
    b = a->batch;
    span = a->tip_y - a->base_y;
    if (span < 1e-5f)
        return;

    for (k = 0; k < b->nverts; k++) {
        const vtx_t *r = &b->rest[k];
        /* where this vertex sits along the whip, 0 at the base and 1 at the tip */
        float t = (r->y - a->base_y) / span;
        float f, cur[3];
        int i;
        int j;

        if (t < 0.f) t = 0.f;
        if (t > 1.f) t = 1.f;
        /* sample the chain: segment i, fraction f within it */
        f = t * (float)(a->n - 1);
        i = (int)f;
        if (i > a->n - 2) i = a->n - 2;
        f -= (float)i;
        for (j = 0; j < 3; j++)
            cur[j] = a->p[i][j] + (a->p[i + 1][j] - a->p[i][j]) * f;

        /* Keep the vertex's offset from the mesh's own axis -- that is what
           gives the tube its thickness. Only the axis itself is bent. */
        b->verts[k].x = cur[0] + (r->x - a->base[0]);
        b->verts[k].y = cur[1];
        b->verts[k].z = cur[2] + (r->z - a->base[2]);
    }
}
