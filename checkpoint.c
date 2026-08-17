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
    c->have_s = 0;
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

/* Every checkpoint's arc-length STATION: where on the spine it sits. cp_0's is
   0, which is also spine_len -- the start line is the seam. */
static float cp_station(const checkpoints_t *c, int k)
{
    if (k <= 0 || k >= c->n)
        return 0.f;
    return c->cum[k][0];
}

/*
 * The first checkpoint whose station is strictly ahead of `s`, wrapping to 0 --
 * what the arrow should point at for a car that has just been put down.
 *
 * THE BOUNDARY, because the death respawn stands the car EXACTLY on a station and
 * that is the one input where `>` could go either way. It does not, and the reason
 * is exact rather than lucky: cp_progress builds its answer as
 * `arc_a + (arc_b - arc_a) * u`, a marker is always a segment ENDPOINT, so u comes
 * out 0 or 1 and `s` is bit-identical to the stored cum value. `station > s` is
 * then false for the station the car is standing on, and the next one is returned.
 *
 * The case that would cost something is checkpoint 0, whose station is BOTH 0 and
 * spine_len: if the projection preferred the closing leg (u = 1, s = spine_len)
 * over the outgoing one (u = 0, s = 0), no station would be greater and this would
 * hand back 0 -- the line the car has just crossed -- so the next metre driven
 * would cross it again and count a phantom LAP, which is a whole spine of error in
 * the opponents' lead. It cannot: cp_progress keeps a STRICTLY better d2 and visits
 * the outgoing leg first, so the tie goes to u = 0. Measured over every checkpoint
 * of all ten shipped tracks -- 0 of 50 aim back at themselves.
 *
 * There WAS a guard here forcing `next != last`, and it came out again. It is
 * unnecessary given the above, and it is actively WRONG for the other way of
 * arriving on that arc: a car teleported to just before the line, having crossed
 * it a lap ago, genuinely is heading for checkpoint 0, and the guard would send it
 * to 1. The property is asserted in vis_test against the real spines instead, so a
 * change to cp_progress's tie-break shows up as a failure rather than being
 * papered over here.
 */
static int cp_ahead(const checkpoints_t *c, float s)
{
    int k;
    for (k = 1; k < c->n; k++)
        if (cp_station(c, k) > s)
            return k;
    return 0;
}

void cp_resync(checkpoints_t *c, float x, float y, float z)
{
    float s;

    (void)y;
    if (!c)
        return;
    c->passed = -1;
    c->have_s = 0;
    if (c->n <= 0 || !cp_progress(c, x, z, &s))
        return;
    c->s = s;
    c->have_s = 1;
    c->next = cp_ahead(c, s);
}

void cp_restart(checkpoints_t *c, float x, float y, float z)
{
    if (!c)
        return;
    /* BEFORE the resync, not after: cp_resync recomputes `next` and must be the
       last word on the cursor. Neither of these is derived from a position. */
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
    float s, ds, cur, len;
    int guard;

    (void)y;
    c->t += dt;
    c->passed = -1;                    /* written every call: it is an EDGE */
    if (!c->enabled)
        return;
    if (!cp_progress(c, x, z, &s))
        return;

    /* First step after a load: nothing to compare against, so sync and aim the
       arrow rather than inventing a crossing at the start line. */
    if (!c->have_s) {
        c->s = s;
        c->have_s = 1;
        c->next = cp_ahead(c, s);
        return;
    }

    len = c->spine_len;
    ds = s - c->s;
    cur = c->s;
    c->s = s;
    if (len <= 1e-6f)
        return;

    /* Forward travel, taken the short way round the loop so crossing the seam at
       the start line reads as a small step forward rather than a lap backwards. */
    while (ds < -0.5f * len) ds += len;
    while (ds >  0.5f * len) ds -= len;

    if (ds < 0.f) {
        /* Driving backwards. Nothing is un-passed -- the original's own distance
           is monotonic across laps and a race module does not hand a checkpoint
           back -- so the arrow holds and re-crossing the station fires again.
           Deleting this changes NOTHING today, and it is written out anyway: the
           loop below would break on `ds < gap` for any negative ds, so the
           statement is here to say what the file means to do rather than to
           carry the arithmetic. */
        return;
    }
    if (ds > CP_MAX_STEP) {
        cp_resync(c, x, y, z);         /* a teleport, or a leg swap: see the .h */
        return;
    }

    /* Spend the travel along the spine, firing at each station it sweeps past.
       Bounded by the checkpoint count: at 30 m between stations and a 10 m cap
       this can only ever be one, and the loop is here so it cannot silently drop
       the second if a track ever packs two close together. */
    for (guard = 0; guard < c->n; guard++) {
        float gap = cp_station(c, c->next) - cur;
        while (gap < 0.f) gap += len;
        if (ds < gap)
            break;
        ds -= gap;
        cur = cp_station(c, c->next);
        c->passed = c->next;
        /* Latched, for the respawn point. See checkpoints_t.last. */
        c->last = c->next;
        /* Checkpoint 0 IS the start/finish line, so crossing its station is the
           lap. Not "next wrapped to 0", which is the same event only while the
           two rules agree about where the wrap happens. */
        if (c->next == 0)
            c->lap++;
        c->next = (c->next + 1) % c->n;
    }
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
