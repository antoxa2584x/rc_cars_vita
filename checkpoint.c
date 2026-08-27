/*
 * checkpoint.c -- see checkpoint.h for the originals this follows
 * (FUN_004e9560 loads them, FUN_0052abc0 draws them).
 */

#include "checkpoint.h"
#include "vis_data.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

    /* THE PAINT each checkpoint is drawn over -- `acp_N`, the mean of the ACP
       mesh's vertices, which pack_vsc.py works out because a .vsc drops mesh
       names. This is the engine's own anchor for the animated arrow and it is
       NOT cp_N; see cp_t.paint. A scene packed before the packer grew these
       has none, and then the anchor is cp_N as it was. */
    for (k = 0; k < c->n; k++) {
        char want[16];
        c->cp[k].paint[0] = c->cp[k].p[0][0];
        c->cp[k].paint[1] = c->cp[k].p[0][1];
        c->cp[k].paint[2] = c->cp[k].p[0][2];
        c->cp[k].has_paint = 0;
        sprintf(want, "acp_%d", k + 1);
        for (i = 0; i < scene->n_markers; i++) {
            if (!strcmp(scene->markers[i].name, want)) {
                c->cp[k].paint[0] = scene->markers[i].x;
                c->cp[k].paint[1] = scene->markers[i].y;
                c->cp[k].paint[2] = scene->markers[i].z;
                c->cp[k].has_paint = 1;
                break;
            }
        }
    }

    /* The terrain under each checkpoint, so the marker can stand ON the ground
       rather than at whatever height the marker node happens to float at.
     *
       CEILED at CP_GROUND_CEIL, and that is the whole of a reported bug: at the
       5 m this used, nine of the fifty checkpoints found the ROOF over a tunnel
       or a bridge instead of the floor under it, and cp_t.ground is what BOTH the
       respawn point and the animated marker are placed at -- so a car sent back
       to a tunnel checkpoint arrived on top of the tunnel, with its marker up
       there too and nowhere near the graffiti on the road. */
    for (k = 0; k < c->n; k++) {
        float gy, nx, ny, nz;
        const float *p = c->cp[k].p[0];
        c->cp[k].ground = p[1];
        if (col && col_ground_at(col, p[0], p[2], p[1] + CP_GROUND_CEIL,
                                 &gy, &nx, &ny, &nz))
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
    /* THE LINE ITSELF until cp_restart latches a real one, which makes
       cp_lap_origin 0 and leaves the progress measure with its origin on the
       start/finish -- the answer a track that is loaded but not raced should
       give, and the one a harness that never calls cp_restart gets. */
    c->grid_arc = c->spine_len;
    c->prog = c->spine_len;
    c->prog_odo = 0.f;
    c->prog_ok = 0;
    /* The memset above already dropped any road -- cp_free is the caller's job
       and this is why the header says so. -1 is "the stretch's own start". */
    c->line_prev = -1.f;
    /* THE ARC STATIONS UNTIL A RECORDING SAYS OTHERWISE. cp_set_stations
       replaces them with where the checkpoints really fall round a lap; a track
       with no usable recording keeps these, which is what shipped before the
       distinction was measured. road_len = spine_len makes the exchange rate 1,
       so cp_restart behaves as it did. */
    for (k = 0; k < c->n; k++) {
        c->station[k] = c->cum[k][0];
        c->seg_road[k] = 0.f;           /* unfitted: normalise instead */
    }
    c->road_len = c->spine_len;
}

int cp_set_stations(checkpoints_t *c, const float *frac, int n, float lap_len)
{
    int k;

    if (!c || !frac || n != c->n || n <= 0 || n > CP_MAX)
        return 0;
    if (!(lap_len > 1e-3f) || !(c->spine_len > 0.f))
        return 0;
    if (frac[0] != 0.f)
        return 0;
    for (k = 1; k < n; k++)
        if (!(frac[k] > frac[k-1]) || !(frac[k] < 1.f))
            return 0;

    for (k = 0; k < n; k++) {
        float hi = (k + 1 < n) ? frac[k+1] : 1.f;
        c->station[k] = frac[k] * c->spine_len;
        c->seg_road[k] = (hi - frac[k]) * lap_len;
    }
    c->road_len = lap_len;
    return 1;
}

void cp_free(checkpoints_t *c)
{
    if (!c)
        return;
    if (c->line_pt)  free(c->line_pt);
    if (c->line_cum) free(c->line_cum);
    c->line_pt = NULL;
    c->line_cum = NULL;
    c->line_n = 0;
    c->line_ok = 0;
}

int cp_set_line(checkpoints_t *c, const float (*pt)[2], const float *cum,
                int n, float len, const float *at, int n_cp)
{
    int i;

    if (!c)
        return 0;
    cp_free(c);
    c->line_prev = -1.f;
    if (!pt || !cum || !at || n < 2 || n_cp != c->n || n_cp <= 0
        || n_cp > CP_MAX || !(len > 1e-3f))
        return 0;
    /* THE ARCS HAVE TO BE ON THE LOOP and in the spine's own order. A fit that
       came back with two checkpoints at the same arc, or with one outside the
       loop it was measured on, describes a road this cannot window on -- and the
       odometer, whatever else is wrong with it, is never ambiguous.
     *
       IN ORDER FROM cp_0, NOT FROM ZERO. The loop begins where the replay
       rejoins, which is a few metres PAST the start/finish -- so cp_0 sits near
       the loop's end (428 m of 443 on beach_1) and the raw arcs wrap. What has to
       increase is the forward distance from cp_0, which is what ai_fit_one's own
       fractions are and what the windowing below measures in. */
    for (i = 0; i < n_cp; i++)
        if (!(at[i] >= 0.f) || !(at[i] < len))
            return 0;
    {
        float prev = 0.f;
        for (i = 1; i < n_cp; i++) {
            float f = at[i] - at[0];
            while (f < 0.f) f += len;
            while (f >= len) f -= len;
            if (!(f > prev))
                return 0;
            prev = f;
        }
    }

    c->line_pt  = (float (*)[2])malloc(sizeof(float) * 2 * (size_t)n);
    c->line_cum = (float *)malloc(sizeof(float) * (size_t)n);
    if (!c->line_pt || !c->line_cum) {
        cp_free(c);
        return 0;
    }
    memcpy(c->line_pt, pt, sizeof(float) * 2 * (size_t)n);
    memcpy(c->line_cum, cum, sizeof(float) * (size_t)n);
    c->line_n = n;
    c->line_len = len;
    for (i = 0; i < n_cp; i++)
        c->line_at[i] = at[i];
    c->line_ok = 1;
    return 1;
}

/* Forward along the loop from `a` to `b`, 0 .. line_len. */
static float cp_line_fwd(const checkpoints_t *c, float a, float b)
{
    float d = b - a;
    while (d < 0.f) d += c->line_len;
    while (d >= c->line_len) d -= c->line_len;
    return d;
}

/* The largest i with line_cum[i] <= arc. Binary search, because the window is
   re-derived from the LATCH each frame rather than walked forward from the last
   answer -- there is no cursor to carry and none to get out of step. */
static int cp_line_index(const checkpoints_t *c, float arc)
{
    int lo = 0, hi = c->line_n - 1;
    if (arc <= c->line_cum[0])
        return 0;
    if (arc >= c->line_cum[hi])
        return hi;
    while (hi - lo > 1) {
        int mid = (lo + hi) >> 1;
        if (c->line_cum[mid] <= arc) lo = mid; else hi = mid;
    }
    return lo;
}

/* The nearest point on the loop to (x, z), searching arc `lo` forward for `span`
   metres. -> its arc. The projection is onto SEGMENTS and clamped to each, so
   the answer is continuous as the car drives -- the same rule cp_spine_dist_near
   uses, on a polyline that is actually the road. */
static float cp_line_arc(const checkpoints_t *c, float x, float z,
                         float lo, float span)
{
    float best = 1e30f, arc = lo;
    int i, i0 = cp_line_index(c, lo), n = c->line_n;

    if (span > c->line_len) span = c->line_len;
    for (i = 0; i < n; i++) {
        int j = i0 + i, j2;
        float ca, cb, seg, dx, dz, len2, t, px, pz, d2;
        if (j >= n) j -= n;
        j2 = (j + 1 == n) ? 0 : j + 1;
        ca = c->line_cum[j];
        cb = c->line_cum[j2];
        seg = cp_line_fwd(c, ca, cb);
        /* The first segment is the one `lo` falls in, so its own start is BEHIND
           the window -- included whatever its offset says. Every later one is
           ahead, and the walk stops at the first past the span. */
        if (i > 0 && cp_line_fwd(c, lo, ca) > span)
            break;
        if (!(seg > 1e-6f))
            continue;
        dx = c->line_pt[j2][0] - c->line_pt[j][0];
        dz = c->line_pt[j2][1] - c->line_pt[j][1];
        len2 = dx * dx + dz * dz;
        t = (len2 > 1e-9f)
            ? ((x - c->line_pt[j][0]) * dx + (z - c->line_pt[j][1]) * dz) / len2
            : 0.f;
        if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
        px = c->line_pt[j][0] + dx * t;
        pz = c->line_pt[j][1] + dz * t;
        d2 = (x - px) * (x - px) + (z - pz) * (z - pz);
        if (d2 < best) {
            best = d2;
            arc = ca + seg * t;
            if (arc >= c->line_len) arc -= c->line_len;
        }
    }
    return arc;
}

static float dist2_xz(const float *p, float x, float z)
{
    float dx = p[0] - x, dz = p[2] - z;
    return dx * dx + dz * dz;
}

/* Defined down with the other progress code; declared here because cp_step, up
   above it, is what advances it. */
static void cp_prog_step(checkpoints_t *c, float x, float z);

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
    /* AND THE PROGRESS, SAID rather than projected. Every caller has just put the
       car on the marker of `last` (cp_respawn_pose) or on the grid, and those are
       exact answers where a projection is not -- see cp_prog_step. The anchor is
       dropped so the teleport itself is not dead-reckoned as a lap of driving. */
    c->prog = (c->last < 0) ? c->grid_arc : c->station[c->last];
    c->prog_odo = 0.f;
    c->prog_ok = 0;
    /* And the window's anchor with it: a teleport is not driving, so the next
       frame starts the projection at the stretch's own beginning, which is where
       both callers have just put the car. */
    c->line_prev = -1.f;
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
    /* And the race is not under way, so the crossing of the line that a race
       start makes within its first second counts no lap. cp_resync deliberately
       does NOT clear this, for the same reason it keeps the lap. */
    c->started = 0;
    cp_resync(c, x, y, z);
    /* THE GRID, and this is the one call that knows where a race begins, which is
       why the latch lives here and not in cp_init. A STRAIGHT LINE back from the
       start/finish marker, not a projection: the grid is 2.3 to 20.5 m from that
       marker on all ten tracks, so over that distance the chord IS the road, and
       a projection onto the spine would answer with a point hundreds of metres
       away on five of them -- a start line being exactly where a track passes
       nearest to itself. cp_resync does NOT redo this: a drowning does not move
       the grid. */
    if (c->n > 0 && c->spine_len > 0.f) {
        float dx = c->cp[0].p[0][0] - x, dz = c->cp[0].p[0][2] - z;
        float d = (float)sqrt((double)dx * dx + (double)dz * dz);
        /* d is ROAD metres and grid_arc is the placing's, in which one lap is
           spine_len however long the road is -- so it is converted, not used
           raw. The rate is 1 until a recording has been fitted. */
        if (c->road_len > 1e-3f)
            d *= c->spine_len / c->road_len;
        if (d > c->spine_len) d = c->spine_len;
        c->grid_arc = c->spine_len - d;
    }
    /* AFTER the latch, because cp_resync puts the progress ON the grid and reads
       it from here. cp_restart's own cp_resync above ran before there was one. */
    c->prog = c->grid_arc;
    c->prog_odo = 0.f;
    c->prog_ok = 0;
    c->line_prev = -1.f;
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

/* THE CURSOR: has the car passed `next`, and if so latch it. Split out of
   cp_step only so that cp_step can advance the PROGRESS on every call whatever
   this answers -- when the two shared a body, the early returns for "still
   closing" and "nowhere near it" took the progress with them. */
static void cp_cursor_step(checkpoints_t *c, float x, float z)
{
    float d;

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
    /* Checkpoint 0 IS the start/finish line, so passing it is the lap -- from the
       SECOND time. It is also the first thing a race passes, on every track,
       because cp_restart aims here and the grid is short of it; that opening
       crossing completes no lap. See checkpoints_t.started. */
    if (c->next == 0) {
        if (c->started)
            c->lap++;
        else
            c->started = 1;
    }
    c->next = (c->next + 1) % c->n;
    c->in_zone = 0;
    c->zone_min = 0.f;
    /* AND THE RE-ANCHOR. The odometer measures from the checkpoint last passed,
       so passing one starts it again -- which is what makes cp_prog_step answer
       with the new stretch's own station on this very frame, whatever the last
       stretch had accumulated. See there. */
    c->prog_odo = 0.f;
    /* The window is re-anchored the same way and for the same reason: the stretch
       it is clamped to is the one the latch names, and the car is AT the marker
       that begins it. -1 says exactly that without needing the arc. */
    c->line_prev = -1.f;
}

void cp_step(checkpoints_t *c, float x, float y, float z, float dt)
{
    (void)y;
    c->t += dt;
    c->passed = -1;                    /* written every call: it is an EDGE */
    if (!c->enabled || c->n <= 0)
        return;

    cp_cursor_step(c, x, z);
    /* AFTER the cursor, and unconditionally: the stretch the progress is clamped
       to is the one the latch names, so on the frame a checkpoint is passed the
       progress must see the NEW stretch -- that is what re-anchors it. */
    cp_prog_step(c, x, z);
}

/* The stitched polyline, as SEGMENTS. `i` walks 0 .. cp_spine_n(c)-1 and the
 * segment is point[i] -> point[i+1], wrapping to point[0] -- the loader's own
 * order, cp_0, cp_0_1..., cp_1, ..., which is what cp_init accumulated `cum`
 * over. Written as a flat index so the query below can be one loop. */
static int cp_spine_n(const checkpoints_t *c)
{
    int k, n = 0;
    for (k = 0; k < c->n; k++)
        n += c->cp[k].n;
    return n;
}

static void cp_spine_pt(const checkpoints_t *c, int i, const float **p,
                        float *cum)
{
    int k;
    for (k = 0; k < c->n; k++) {
        if (i < c->cp[k].n) {
            *p = c->cp[k].p[i];
            *cum = c->cum[k][i];
            return;
        }
        i -= c->cp[k].n;
    }
    *p = c->cp[0].p[0];
    *cum = c->cum[0][0];
}

int cp_spine_dist_near(const checkpoints_t *c, float x, float y, float z,
                       float hint, float *dist, int *cp)
{
    float near2 = 1e30f;
    float best_arc = 0.f;
    int i, n, best_cp = -1;

    (void)y;
    if (!c || c->n <= 0)
        return 0;
    n = cp_spine_n(c);
    if (n < 2)
        return 0;

    (void)hint;
    {
        for (i = 0; i < n; i++) {
            const float *a, *b;
            float ca, cb, seg, dx, dz, t, px, pz, d2, arc;
            int ka;

            cp_spine_pt(c, i, &a, &ca);
            cp_spine_pt(c, (i + 1) % n, &b, &cb);
            /* The closing segment wraps, so its length comes off the total. */
            seg = (i + 1 == n) ? (c->spine_len - ca) : (cb - ca);
            if (!(seg > 1e-6f))
                continue;

            /* Project onto the segment in XZ and clamp to it -- the nearest point
               on the nearest SEGMENT, which is what this file's header has always
               claimed and what the old sample-snapping version did not do. It is
               also what makes the answer CONTINUOUS as the car drives, which the
               window and the seam detection both need. */
            dx = b[0] - a[0];
            dz = b[2] - a[2];
            {
                const float len2 = dx * dx + dz * dz;
                t = (len2 > 1e-9f)
                    ? ((x - a[0]) * dx + (z - a[2]) * dz) / len2 : 0.f;
                if (t < 0.f) t = 0.f;
                else if (t > 1.f) t = 1.f;
            }
            px = a[0] + dx * t;
            pz = a[2] + dz * t;
            d2 = (x - px) * (x - px) + (z - pz) * (z - pz);
            if (d2 >= near2)
                continue;
            near2 = d2;
            arc = ca + seg * t;
            if (arc >= c->spine_len)
                arc -= c->spine_len;
            best_arc = arc;
            /* which checkpoint owns this segment's START point */
            {
                int j = i;
                best_cp = 0;
                for (ka = 0; ka < c->n; ka++) {
                    if (j < c->cp[ka].n) { best_cp = ka; break; }
                    j -= c->cp[ka].n;
                }
            }
        }
    }
    if (best_cp < 0)
        return 0;
    if (dist) *dist = best_arc;
    if (cp) *cp = best_cp;
    return 1;
}

int cp_spine_dist(const checkpoints_t *c, float x, float y, float z,
                  float *dist, int *cp)
{
    return cp_spine_dist_near(c, x, y, z, -1.f, dist, cp);
}

/* THE ARC OF (x, z) ON ONE CHECKPOINT-TO-CHECKPOINT STRETCH, `a` to `b`, where
 * `b` is (a + 1) % n. Nearest point on the nearest segment OF THAT RUN ONLY.
 *
 * See cp_lap_progress in the header for why the run is restricted; briefly, it is
 * the only way to get an answer that both moves every frame and cannot teleport.
 *
 * The result runs from cum[a][0] to cum[b][0], or to spine_len for the closing
 * stretch -- NOT wrapped back to 0 the way cp_spine_dist_near wraps it, because a
 * lap has to be able to end at its own length. Clamped to the stretch, so a car
 * that has run past `b` without triggering it reports the end of the stretch and
 * a car that has spun back behind `a` reports its start. */
/* THE PLAYER'S PROGRESS, CARRIED FORWARD ONE FRAME. Called by cp_step, which is
 * the once-a-frame call; cp_lap_progress only reads what this leaves. Doing it
 * here rather than in the query is not tidiness -- main.c asks the question twice
 * a frame (the placing and the direction arrow) and a query that advanced state
 * would count the frame twice.
 *
 * IT USES THE CHECKPOINT MARKERS, THE CAR'S OWN MOTION, AND THE ROAD LENGTHS
 * FITTED OFF THE RECORDINGS. Specifically it does NOT project onto the spine's
 * polyline, which was the obvious fix and
 * is unsound here: the "spine" is not a centreline. Measured against the first
 * opponent's recorded lap on each of the ten tracks, its points sit 9 to 27 m
 * from the racing line on average and up to 84 m from it at worst, and the
 * polyline is 1.4 to 2.1 times LONGER than the lap it is supposed to describe
 * (beach_1: a 460 m lap, a 643 m spine). A nearest-point-on-the-spine answer is
 * therefore not a position on the track, and a step along its tangent is not a
 * step along the road -- on beach_1 the tangent at the start line points SOUTH
 * down a 71 m detour the car never drives, so dead reckoning ran backwards and
 * pinned at zero for the whole stretch.
 *
 * WHAT IS SOUND is the pair of checkpoint markers bounding the stretch and the
 * car's own displacement, so the fraction of the stretch is built from those:
 *
 *     odo  the road distance the car has driven since `last` was latched --
 *          its own displacement, summed. It needs no idea of how long the road
 *          between the two checkpoints is, which is exactly what makes it usable
 *          on a track whose spine lies about that length.
 *     d    the straight-line distance to `next` right now.
 *     road this stretch's own length on the ground, out of the same fit that
 *          placed the stations -- 0 when nothing has been fitted.
 *     t    odo / max(road, odo + d). 0 at the checkpoint just passed, 1 at the
 *          one being driven to, and see the code for which term owns which
 *          regime: normally `road' wins and t is EXACTLY LINEAR in distance
 *          driven, which is exactly what an opponent's progress is.
 *
 * ODO IS THE UNSIGNED DISTANCE and the signed one -- the displacement resolved
 * along the bearing to `next` -- was tried first and is worse, which is worth
 * recording because it is the more obviously correct of the two. It reads zero
 * while the road leads AWAY from the next checkpoint before turning back to it,
 * and that is not a rare shape: measured over the ten tracks it froze the answer
 * for 34.5% of country_1's frames, 21.9% of beach_1's and 9.3% of beach_3's, and
 * a frozen progress measure is the very bug being fixed. Unsigned freezes on none
 * of the ten and was the better mean on nine of them.
 *
 * WHAT UNSIGNED COSTS is a car going the WRONG WAY: driving back down the stretch
 * grows odo, so t creeps up instead of falling to zero rather than reading the
 * retreat. Bounded by the stretch -- t cannot exceed 1, and the station beyond it
 * cannot be reached without passing the checkpoint that ends it -- and the same
 * bound the rule this replaced had. The wrong way has its own indicator
 * (dirarrow.c) and this is not it.
 *
 * `t` then maps onto the stretch's own two STATIONS -- where the checkpoints
 * really fall round a lap, fitted off the recordings, NOT the spine's cum[] arc
 * lengths, which are 38.6 m out on average. See checkpoints_t.station; that
 * distinction is worth more than everything above it.
 *
 * THE LATCH RE-ANCHORS IT EXACTLY at every crossing: cp_cursor_step zeroes the
 * odometer on a pass, so t is 0 and the answer is the new stretch's own station,
 * whatever the previous stretch had accumulated. Error is bounded by one stretch
 * and reset 4 to 7 times a lap; it cannot accumulate over a race.
 */
static void cp_prog_step(checkpoints_t *c, float x, float z)
{
    const float *m;
    float dx, dz, d, t = 0.f, from, to;
    int a, b;

    if (c->n <= 0 || !(c->spine_len > 0.f))
        return;

    /* `next` IS (last + 1) % n by the state machine's own identity, so this is
       one stretch and not a guess; with nothing latched the car is on the grid,
       heading for the line, and the stretch it is on begins at the grid. */
    b = (c->next >= 0 && c->next < c->n) ? c->next : 0;
    a = (c->last >= 0 && c->last < c->n) ? c->last : -1;
    from = (a < 0) ? c->grid_arc : c->station[a];
    to = (b == 0) ? c->spine_len : c->station[b];
    if (to < from)
        to = from;

    /* THE ROAD, when there is one: WHERE THE CAR IS on it, not how far it has
     * driven. Everything below this block is the odometer, which is the fallback
     * for a track whose recordings would not fit -- see checkpoints_t.line_pt for
     * why it cannot be the answer.
     *
     * The stretch is the latch's, exactly as below, and the window is
     * CP_LINE_BACK/CP_LINE_FWD around the last answer intersected with it. An
     * anchor that has fallen off the stretch -- a reset, or the frame after a
     * pass -- starts at the stretch's own beginning, which is where the car is.
     *
     * `t` comes out EXACTLY LINEAR IN ROAD DISTANCE by construction, because the
     * line's arc IS road distance; that is the same quantity an opponent's
     * progress is, which is the whole point. */
    if (c->line_ok) {
        float lo, hi, span, p0, w0, w1, arc, tt;
        if (a < 0) {
            /* THE GRID. The loop has no run-up on it -- the replay rejoins at
               cycle_start and never drives the approach again -- so the opening
               stretch is the road immediately BEFORE cp_0, taken back from it by
               the grid's own distance to the line. In road metres, because that
               is what the line measures in and grid_arc is in the placing's. */
            float run = c->spine_len - c->grid_arc;
            if (c->spine_len > 1e-3f)
                run *= c->road_len / c->spine_len;
            if (run > c->line_len) run = c->line_len;
            lo = c->line_at[0] - run;
            while (lo < 0.f) lo += c->line_len;
            hi = c->line_at[0];
        } else {
            lo = c->line_at[a];
            hi = c->line_at[b];
        }
        span = cp_line_fwd(c, lo, hi);
        if (!(span > 1e-3f))
            span = c->line_len;
        /* WHERE THE WINDOW SITS. -1 is a reset or the frame after a pass, and
           both mean the stretch's own start -- there is no other way for the
           anchor to be off this stretch, because the search below can never
           answer outside it. A p0 past the far end is the last frame's answer
           having clamped AT that end, and it CLAMPS FORWARD: reading it as "not
           on this stretch" and starting the window over at the beginning threw
           the projection 174 m back up beach_3's own lap, once per checkpoint
           per lap, on all ten tracks. */
        p0 = (c->line_prev < 0.f) ? 0.f : cp_line_fwd(c, lo, c->line_prev);
        if (p0 > span)
            p0 = span;
        w0 = p0 - CP_LINE_BACK;
        if (w0 < 0.f) w0 = 0.f;
        w1 = p0 + CP_LINE_FWD;
        if (w1 > span) w1 = span;
        if (!(w1 > w0)) w1 = span;
        arc = cp_line_arc(c, x, z, lo + w0 >= c->line_len
                                   ? lo + w0 - c->line_len : lo + w0,
                          w1 - w0);
        c->line_prev = arc;
        /* The dead-reckoning anchor is kept CURRENT but not accumulated: nothing
           on this path reads the odometer, and a stale position left behind here
           would be differenced against a teleport if anything ever did. */
        c->prog_odo = 0.f;
        c->prog_ok = 1;
        c->prog_x = x;
        c->prog_z = z;
        tt = cp_line_fwd(c, lo, arc) / span;
        if (tt < 0.f) tt = 0.f; else if (tt > 1.f) tt = 1.f;
        c->prog = from + (to - from) * tt;
        return;
    }

    m = c->cp[b].p[0];
    dx = m[0] - x;
    dz = m[2] - z;
    d = (float)sqrt((double)dx * dx + (double)dz * dz);

    /* The first step after a reset only seeds the anchor: cp_restart and
       cp_resync have already said where the progress is, and a frame that
       differenced against a stale position would count the teleport itself as
       driving. */
    if (!c->prog_ok) {
        c->prog_odo = 0.f;
        c->prog_ok = 1;
    } else {
        float mx = x - c->prog_x, mz = z - c->prog_z;
        c->prog_odo += (float)sqrt((double)mx * mx + (double)mz * mz);
    }
    c->prog_x = x;
    c->prog_z = z;

    if (c->prog_odo > 0.f) {
        /* THE STRETCH'S OWN ROAD LENGTH against the odometer, with the straight
         * line to `next` as the floor under the denominator. One expression, and
         * each term owns a regime:
         *
         *   NORMALLY `road' wins, because a car on the road has driven `odo' of
         *   it and the chord `d' cuts the rest short, so `odo + d' stays under
         *   `road'. Then t is odo/road -- EXACTLY LINEAR IN DISTANCE DRIVEN,
         *   which is exactly what an opponent's progress is, and the whole point
         *   of measuring both sides the same way. Mean error against the road
         *   fell from 27.5 m to 15.9 m when this replaced the self-normalising
         *   form, and the worst case from 120 m to 45 m.
         *
         *   ON AN OVERSHOOT `odo + d' wins and t stays under 1 and keeps moving.
         *   Without that floor the fraction clamps at 1 the moment a car drives
         *   further than the fitted stretch -- a wide line, or a lap that is not
         *   the one the recording drove -- and STALLS there until the checkpoint
         *   triggers: 205 frames on country_3, 3.4 s of a frozen place, which is
         *   the very bug this all fixes.
         *
         *   UNFITTED, `road' is 0 and the floor is all there is, which is the
         *   self-normalising rule a track with no usable recording gets.
         */
        float road = (a < 0)
                     ? (c->spine_len - c->grid_arc) * (c->road_len / c->spine_len)
                     : c->seg_road[a];
        float den = c->prog_odo + d;
        if (road > den) den = road;
        if (den > 1e-3f) {
            t = c->prog_odo / den;
            if (t > 1.f) t = 1.f;
        } else {
            t = 1.f;
        }
    }

    c->prog = from + (to - from) * t;
}

int cp_lap_progress(const checkpoints_t *c, float x, float y, float z,
                    float *out)
{
    float arc;

    /* The position is not read: cp_step advanced the progress with it already,
       once, this frame. The arguments stay so the call reads at the site as what
       it is -- and so that a caller which has not been moved onto cp_step still
       compiles rather than silently asking a different question. cp_resync's
       do the same. */
    (void)x; (void)y; (void)z;
    if (!c || c->n <= 0 || c->spine_len <= 0.f)
        return 0;

    arc = c->prog;

    /* THE ORIGIN MOVED TO THE GRID, which is where every opponent's own measure
     * starts -- an opponent's is metres walked from its recording's first sample,
     * and that is its grid slot, not the line.
     *
     * Before the first crossing the car is on a lap it has not begun, so the same
     * offset carries a spine length off it. The two meet exactly at the line and
     * the first crossing costs nothing: `prog' runs grid_arc -> spine_len over
     * the opening stretch, so the answer runs 0 -> origin; the crossing puts
     * `prog' back to 0 on the new stretch and the answer stays at origin. */
    arc += c->spine_len - c->grid_arc;
    if (c->last < 0)
        arc -= c->spine_len;

    if (out)
        *out = arc;
    return 1;
}

/* The offset cp_lap_progress applies, on its own: `spine_len - grid_arc`, and 0
   before cp_restart has latched a grid, because cp_init leaves grid_arc at
   spine_len. See the header -- main.c's direction arrow divides the progress
   back into a fraction of its own stretch and has to take this off first. */
float cp_lap_origin(const checkpoints_t *c)
{
    if (!c || c->n <= 0 || c->spine_len <= 0.f)
        return 0.f;
    return c->spine_len - c->grid_arc;
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
        /* THE PAINT, not the waypoint -- FUN_0052abc0 draws at the mean of the
           ACP object's vertices and that is a mean 0.27 m (worst 0.79 m) from
           cp_N. See cp_t.paint. */
        const float *p = c->cp[k].paint;
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
            /* The paint's own mean height IS the ground under it plus the
               centimetre the decal is draped by, so it needs no probe; the
               probe under cp_N is the fallback for a scene with no `acp_N`. */
            float base = CP_GROUND
                         ? (c->cp[k].has_paint ? p[1] : c->cp[k].ground)
                         : p[1];
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
