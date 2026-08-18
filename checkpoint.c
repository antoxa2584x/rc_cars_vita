/*
 * checkpoint.c -- see checkpoint.h for the originals this follows
 * (FUN_004e9560 loads them, FUN_0052abc0 draws them).
 */

#include "checkpoint.h"
#include "vis_data.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Spelt out rather than taken from M_PI, which is not in ANSI C's math.h and is
   how cam.c does it too. */
#define CP_RAD2DEG 57.29577951308232

static void cp_add_point(cp_t *c, const marker_t *m)
{
    if (c->n >= CP_MAX_POINTS)
        return;                            /* "Too many edges in checkpoint" */
    c->p[c->n][0] = m->x;
    c->p[c->n][1] = m->y;
    c->p[c->n][2] = m->z;
    c->n++;
}

void cp_init(checkpoints_t *c, const scene_t *scene, const col_t *col)
{
    unsigned int i;
    int k, f;

    memset(c, 0, sizeof(*c));

    /* cp_N is the checkpoint, cp_N_M its refining points -- exactly the two
       name patterns FUN_004e9560 builds with "cp_%d" and "%s_%d". The base
       point goes in first so the chain starts at the checkpoint itself. */
    for (k = 1; k <= CP_MAX; k++) {
        char want[16];
        int found = 0;
        sprintf(want, "cp_%d", k);
        for (i = 0; i < scene->n_markers; i++) {
            if (!strcmp(scene->markers[i].name, want)) {
                cp_add_point(&c->cp[k - 1], &scene->markers[i]);
                found = 1;
                break;
            }
        }
        if (!found)
            break;                          /* the numbering is contiguous */
        for (f = 1; f < CP_MAX_POINTS; f++) {
            char sub[24];
            int hit = 0;
            sprintf(sub, "cp_%d_%d", k, f);
            for (i = 0; i < scene->n_markers; i++) {
                if (!strcmp(scene->markers[i].name, sub)) {
                    cp_add_point(&c->cp[k - 1], &scene->markers[i]);
                    hit = 1;
                    break;
                }
            }
            if (!hit)
                break;
        }
        c->n = k;
    }

    /* The terrain under each checkpoint, so the marker can stand ON the ground
       rather than at whatever height the marker node happens to float at. */
    for (k = 0; k < c->n; k++) {
        float gy, nx, ny, nz;
        const float *p = c->cp[k].p[0];
        c->cp[k].ground = p[1];
        if (col && col_ground_at(col, p[0], p[2], p[1] + 5.f, &gy, &nx, &ny, &nz))
            c->cp[k].ground = gy;
    }

    /* The spine's cumulative arc length, in the loader's stitching order, and
       closed: the last leg runs from the final point back to cp_0. */
    {
        double run = 0.0;
        const float *prev = NULL;
        for (k = 0; k < c->n; k++) {
            int j;
            for (j = 0; j < c->cp[k].n; j++) {
                const float *p = c->cp[k].p[j];
                if (prev)
                    run += sqrt((double)(p[0] - prev[0]) * (p[0] - prev[0])
                                + (double)(p[1] - prev[1]) * (p[1] - prev[1])
                                + (double)(p[2] - prev[2]) * (p[2] - prev[2]));
                c->cum[k][j] = (float)run;
                prev = p;
            }
        }
        if (prev && c->n > 0) {
            const float *p = c->cp[0].p[0];
            run += sqrt((double)(p[0] - prev[0]) * (p[0] - prev[0])
                        + (double)(p[1] - prev[1]) * (p[1] - prev[1])
                        + (double)(p[2] - prev[2]) * (p[2] - prev[2]));
        }
        c->spine_len = (float)run;
    }

    /* FUN_0052a9b0 loads both three-frame sets by name. Its own error strings
       call cp_ar_2 the "common" arrow and cp_ar_3 the "custom" one. */
    for (k = 0; k < 3; k++) {
        char nm[24];
        sprintf(nm, "cp_ar_2_f%d", k + 1);
        c->tex_common[k] = scene_tex(scene, nm);
        sprintf(nm, "cp_ar_3_f%d", k + 1);
        c->tex_custom[k] = scene_tex(scene, nm);
    }
    c->enabled = (c->n > 0 && c->tex_common[0] != 0 && c->tex_custom[0] != 0);
    c->next = 0;
    /* -1, not the 0 the memset left: 0 is a real checkpoint index, and a host
       that reads `passed` before the first cp_step would hear the start line.
       `last` for the same reason and a sharper one -- 0 would tell the respawn
       path that the start/finish line has been crossed before the race began. */
    c->passed = -1;
    c->last = -1;
    c->in_zone = 0;
}

static float dist2_xz(const float *p, float x, float z)
{
    float dx = p[0] - x, dz = p[2] - z;
    return dx * dx + dz * dz;
}

int cp_progress(const checkpoints_t *c, float x, float z, float *out_s)
{
    float best2 = 1e30f, best_s = 0.f;
    const float *a = NULL;
    float arc_a = 0.f;
    int k, j, found = 0;

    if (!c || c->n <= 0)
        return 0;

    /* Walk the stitched polyline in the loader's own order and one leg further,
       back to cp_0: the spine is CLOSED, and the leg that carries the start line
       is exactly the one a lap has to be able to cross. */
    for (k = 0; k <= c->n; k++) {
        int kk = (k == c->n) ? 0 : k;
        int nj = (k == c->n) ? 1 : c->cp[kk].n;
        for (j = 0; j < nj; j++) {
            const float *b = c->cp[kk].p[j];
            float arc_b = (k == c->n) ? c->spine_len : c->cum[kk][j];
            if (a) {
                /* Nearest point on the segment, in XZ -- the same plane
                   cp_step and cp_spine_dist measure in, because a car that is a
                   metre above or below the marker line is still abreast of it. */
                float dx = b[0] - a[0], dz = b[2] - a[2];
                float l2 = dx * dx + dz * dz;
                float u = 0.f, px, pz, d2;
                if (l2 > 1e-12f) {
                    u = ((x - a[0]) * dx + (z - a[2]) * dz) / l2;
                    if (u < 0.f) u = 0.f;
                    else if (u > 1.f) u = 1.f;
                }
                px = a[0] + dx * u;
                pz = a[2] + dz * u;
                d2 = (x - px) * (x - px) + (z - pz) * (z - pz);
                if (d2 < best2) {
                    best2 = d2;
                    /* The arc runs along the 3D leg while u was solved in XZ, so
                       a steep leg is interpolated slightly short. Immaterial
                       here -- these are near-flat waypoint legs, and the
                       quantity is only ever differenced against itself. */
                    best_s = arc_a + (arc_b - arc_a) * u;
                    found = 1;
                }
            }
            a = b;
            arc_a = arc_b;
        }
    }
    if (!found) {                       /* one point, so no leg at all */
        best_s = 0.f;
        found = 1;
    }
    if (out_s)
        *out_s = best_s;
    return found;
}

void cp_resync(checkpoints_t *c, float x, float y, float z)
{
    /* No projection, and no use for the position: `next` is (last + 1) and every
       caller has just put the car either on the grid or on `last`'s own marker.
       See checkpoints_t.last for the five tracks the projection got wrong. */
    (void)x; (void)y; (void)z;
    if (!c)
        return;
    c->passed = -1;
    c->in_zone = 0;
    c->zone_min = 0.f;
    if (c->n <= 0)
        return;
    c->next = (c->last < 0) ? 0 : (c->last + 1) % c->n;
}

void cp_restart(checkpoints_t *c, float x, float y, float z)
{
    if (!c)
        return;
    /* BEFORE the resync, not after: cp_resync reads `last` to aim the cursor and
       must be the last word on it. `last` = -1 is what makes the car head for
       CHECKPOINT 0 -- the start/finish -- on every track, which is the whole of
       what a race start is. */
    c->lap = 0;
    c->last = -1;
    cp_resync(c, x, y, z);
}

int cp_respawn_pose(const checkpoints_t *c, float pos[3], float *yaw_deg)
{
    const cp_t *k;
    const float *a, *b = NULL;
    float dx, dz, len;
    int j, step, at;

    if (!c || c->n <= 0 || c->last < 0 || c->last >= c->n)
        return 0;

    k = &c->cp[c->last];
    if (k->n <= 0)
        return 0;
    a = k->p[0];

    /* Aim along the spine, which means the NEXT point in the loader's own
     * stitching order -- this checkpoint's first refining point if it has one,
     * otherwise the next checkpoint. Walked rather than read directly, because a
     * marker duplicated at the same coordinates (or a checkpoint whose refining
     * point sits on top of it) gives no direction at all, and the answer is then
     * the point after that rather than a yaw of zero. Bounded by the whole spine,
     * so a track of coincident markers falls out with 0 and the caller uses the
     * grid. */
    at = c->last;
    j = 1;
    for (step = 0; step < c->n * CP_MAX_POINTS; step++) {
        const float *p;
        if (j >= c->cp[at].n) {
            at = (at + 1) % c->n;
            j = 0;
            if (at == c->last)                 /* all the way round */
                break;
        }
        p = c->cp[at].p[j];
        j++;
        dx = p[0] - a[0];
        dz = p[2] - a[2];
        if (dx * dx + dz * dz > 1e-4f) {       /* 1 cm of separation is plenty */
            b = p;
            break;
        }
    }
    if (!b)
        return 0;

    dx = b[0] - a[0];
    dz = b[2] - a[2];
    len = (float)sqrt((double)dx * dx + (double)dz * dz);
    if (!(len > 1e-6f))
        return 0;

    if (pos) {
        pos[0] = a[0];
        /* cp_t.ground, not the marker's own y: the markers float. The caller
           re-probes anyway, and this is the fallback if that probe misses. */
        pos[1] = k->ground;
        pos[2] = a[2];
    }
    if (yaw_deg) {
        /* rbcar_init's convention: local +Z on (sin yaw, 0, cos yaw), so the yaw
           of a direction is atan2(x, z). Checked against tracks.h's own race
           start headings, which cross into rbcar_init unchanged. */
        *yaw_deg = (float)(atan2((double)dx, (double)dz) * CP_RAD2DEG);
    }
    return 1;
}

void cp_step(checkpoints_t *c, float x, float y, float z, float dt)
{
    float d;

    (void)y;
    c->t += dt;
    c->passed = -1;                    /* written every call: it is an EDGE */
    if (!c->enabled || c->n <= 0)
        return;
    if (c->next < 0 || c->next >= c->n)
        c->next = 0;

    /* THE APPROACH TO `next`, in XZ. Height is left out for the same reason
       cp_progress leaves it out and a stronger one here: urban_1 and urban_2 both
       run a deck 8 to 9 m over another part of their own road, and a car under one
       is not near the checkpoint above it -- but it is also never within 5 m of it
       in XZ, because CP_TRIGGER_RAD is under half the closest two markers on any
       track. Nothing in the ten needs the third axis to disambiguate. */
    d = sqrtf(dist2_xz(c->cp[c->next].p[0], x, z));

    if (d <= CP_TRIGGER_RAD) {
        if (!c->in_zone || d < c->zone_min) {
            c->in_zone = 1;
            c->zone_min = d;
            return;                    /* still closing */
        }
        if (d < c->zone_min + CP_PASS_EPS)
            return;                    /* not yet clear of the minimum */
    } else if (!c->in_zone) {
        return;                        /* nowhere near it */
    }

    /* PASSED: either the distance has climbed CP_PASS_EPS off its minimum, or the
       car has left the radius having been inside it. The event is AT the marker --
       the closest approach -- rather than at the edge of the circle, so the cue
       lands where the checkpoint is however fast the car is going. */
    c->passed = c->next;
    c->last = c->next;                 /* latched, for the respawn point */
    /* Checkpoint 0 IS the start/finish line, so passing it is the lap. It is also
       the FIRST thing a race passes, on every track -- cp_restart aims here. */
    if (c->next == 0)
        c->lap++;
    c->next = (c->next + 1) % c->n;
    c->in_zone = 0;
    c->zone_min = 0.f;
}

int cp_spine_dist(const checkpoints_t *c, float x, float y, float z,
                  float *dist, int *cp)
{
    float near2 = 1e30f;
    int k, j, bk = -1, bj = 0;

    (void)y;
    if (!c || c->n <= 0)
        return 0;
    for (k = 0; k < c->n; k++) {
        for (j = 0; j < c->cp[k].n; j++) {
            float d2 = dist2_xz(c->cp[k].p[j], x, z);
            if (d2 < near2) {
                near2 = d2;
                bk = k;
                bj = j;
            }
        }
    }
    if (bk < 0)
        return 0;
    if (dist) *dist = c->cum[bk][bj];
    if (cp) *cp = bk;
    return 1;
}

float cp_dist_to_next(const checkpoints_t *c, float x, float y, float z)
{
    const float *p;
    float dx, dy, dz;

    if (!c->enabled)
        return 0.f;
    p = c->cp[c->next].p[0];
    dx = p[0] - x; dy = p[1] - y; dz = p[2] - z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

void cp_draw(checkpoints_t *c, const float eye[3])
{
    float pulse;
    int frame, k;

    if (!c->enabled)
        return;

    /*
     * The pulse, from FUN_0052b1d0: a phase ramps 0 -> 0.4 and back, mapping to
     * alpha 50 -> 250 (`x*500 + 50` one way, `250 - x*500` the other), and the
     * caller at 0x0052afe0 clamps it at 220. Out of 255, so the marker breathes
     * between about 20% and 86% rather than blinking on and off.
     *
     * That is for the checkpoint being headed for. FUN_0052b1d0's FIRST act is
     * `cmp` on its two index arguments and `mov $0x32, %al` when they differ:
     * every OTHER checkpoint gets a flat alpha 50. FUN_0052b170 then walks the
     * whole registered list applying it, so the game marks EVERY checkpoint --
     * the others dim, the current one breathing.
     *
     * Drawing only the current one, which this file did first, is why just one
     * marker was ever visible.
     */
    {
        float ph = fmodf(c->t, 2.f * CP_PULSE_TIME);
        float x = (ph < CP_PULSE_TIME) ? ph : (2.f * CP_PULSE_TIME - ph);
        pulse = 50.f + x * 500.f;
        if (pulse > 220.f)
            pulse = 220.f;
    }

    /* time / BlinkDelta modulo 3, the same index FUN_0052abc0 takes */
    frame = (int)(c->t / CP_BLINK_DELTA) % 3;
    if (frame < 0)
        frame = 0;

    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);

    for (k = 0; k < c->n; k++) {
        const float *p = c->cp[k].p[0];
        vtx_t q[4];
        float dx = p[0] - eye[0];
        float dz = p[2] - eye[2];
        float rx, rz, len, dist, alpha, cy;
        GLuint tex;

        /*
         * The distance ramp, read off the disassembly at 0x0052b002 because the
         * decompiler had lost it:
         *
         *     d = sqrt(dx*dx + dz*dz)          HORIZONTAL only, no y term
         *     if (d < MinDist)        k = 0
         *     else if (d >= MaxDist)  k = 1
         *     else                    k = (d - MinDist) / (MaxDist - MinDist)
         *     alpha = blink * k
         *
         * So a marker is INVISIBLE up close and fully on far away, and there is
         * no far cull at all -- the right way round for a navigation marker, and
         * the exact opposite of what this file did first.
         */
        dist = sqrtf(dx * dx + dz * dz);
        if (dist < CP_MIN_DIST)
            continue;
        alpha = (dist >= CP_MAX_DIST)
            ? 1.f
            : (dist - CP_MIN_DIST) / (CP_MAX_DIST - CP_MIN_DIST);
        alpha *= ((k == c->next) ? pulse : CP_ALPHA_OTHER) / 255.f;
        if (alpha <= 0.f)
            continue;

        /* FUN_0052abc0: right = cross(worldUp, centre - camera), normalised, so
           the quad faces the camera while staying upright. */
        rx = -dz;
        rz = dx;
        len = sqrtf(rx * rx + rz * rz);
        if (len < 1e-6f)
            continue;
        rx /= len; rz /= len;

        /* index 0 is the start/finish line, and it gets the "custom" red arrow */
        tex = (k == 0) ? c->tex_custom[frame] : c->tex_common[frame];
        if (!tex)
            continue;

        /* half-extent, and the quad's BOTTOM on the ground -- see checkpoint.h
           for why both of these bend the recovered numbers */
        {
            const float half = CP_SIZE * CP_SIZE_SCALE;
            float base = CP_GROUND ? c->cp[k].ground : p[1];
            cy = base + half;
            q[0].x = p[0] - rx * half; q[0].y = cy - half; q[0].z = p[2] - rz * half;
            q[1].x = p[0] + rx * half; q[1].y = cy - half; q[1].z = p[2] + rz * half;
            q[2].x = p[0] + rx * half; q[2].y = cy + half; q[2].z = p[2] + rz * half;
            q[3].x = p[0] - rx * half; q[3].y = cy + half; q[3].z = p[2] - rz * half;
        }
        q[0].u = 0.f; q[0].v = 1.f;
        q[1].u = 1.f; q[1].v = 1.f;
        q[2].u = 1.f; q[2].v = 0.f;
        q[3].u = 0.f; q[3].v = 0.f;

        glBindTexture(GL_TEXTURE_2D, tex);
        glColor4f(1.f, 1.f, 1.f, alpha);
        glVertexPointer(3, GL_FLOAT, sizeof(vtx_t), &q[0].x);
        glTexCoordPointer(2, GL_FLOAT, sizeof(vtx_t), &q[0].u);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    }

    glColor4f(1.f, 1.f, 1.f, 1.f);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);
}
