/*
 * checkpoint.c -- see checkpoint.h for the originals this follows
 * (FUN_004e9560 loads them, FUN_0052abc0 draws them).
 */

#include "checkpoint.h"
#include "vis_data.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

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
}

static float dist2_xz(const float *p, float x, float z)
{
    float dx = p[0] - x, dz = p[2] - z;
    return dx * dx + dz * dz;
}

void cp_step(checkpoints_t *c, float x, float y, float z, float dt)
{
    float near2 = 1e30f;
    int owner = -1;
    int k, j, prev;

    (void)y;
    c->t += dt;
    if (!c->enabled)
        return;

    /* NOT the game's rule -- see checkpoint.h. But it is the game's DATA used
       the way the game uses it: the checkpoints stitch into one closed spine,
       and the race module measures progress as distance along it. So find the
       spine point the car is nearest to; the checkpoint that owns it is the one
       just passed, and the next one is what the arrow marks.
     *
     * Proximity to the checkpoint itself would be the obvious rule and is
     * wrong: the waypoints sit 30 to 90 m apart on these tracks, and a car that
     * takes a wide line past one never comes inside any sensible radius of it,
     * so the arrow sticks on a checkpoint already behind the player. */
    for (k = 0; k < c->n; k++) {
        for (j = 0; j < c->cp[k].n; j++) {
            float d2 = dist2_xz(c->cp[k].p[j], x, z);
            if (d2 < near2) {
                near2 = d2;
                owner = k;
            }
        }
    }
    if (owner < 0)
        return;

    prev = c->next;
    c->next = (owner + 1) % c->n;
    /* one lap per wrap past the last checkpoint back to the start line */
    if (prev == c->n - 1 && c->next == 0)
        c->lap++;
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
