/*
 * water.c -- see water.h for what here is the game's and what is the port's.
 */

#include "water.h"
#include "vis_data.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TWO_PI 6.2831853f
#define DEG (float)(M_PI / 180.0)

/* Everything the sea surface is shaped by now comes out of WSURF[track] in
   vis_data.h, which is generated from the track's OWN config section and cites
   FUN_00521540 for every conversion. Three constants used to live here instead:

     WATER_DEPTH_FADE 0.5f    the depth the swell fades out over. It is
                              cfg->magnet_radius, and the shipped value is
                              2.55 m -- magnetRadius converts as raw*0.05.
     WSURF_OFFSET             the surface's vertical offset. It is cfg->offset,
                              it is per track, and the port compiled beach_1's
                              -0.37 m into all ten. It is now also what
                              pack_col.py bakes into the .col water grid, so the
                              waterline the car feels is the one it can see.
     WATER_SWELL_AMP 0.02f    the swell height. See water.h. */

/* ------------------------------------------------------------------ sine LUT
 *
 * Two sines per surface vertex per frame over several thousand vertices is
 * real work on a 444 MHz Cortex-A9, and water has no accuracy requirement
 * whatsoever. 1024 entries is 0.35 degrees of step, which the linear
 * interpolation below smooths out entirely.
 */
#define SIN_BITS 10
#define SIN_N (1 << SIN_BITS)
static float sin_lut[SIN_N + 1];
static int sin_ready;

static void sin_init(void)
{
    int i;
    if (sin_ready)
        return;
    for (i = 0; i <= SIN_N; i++)
        sin_lut[i] = sinf((float)i * (TWO_PI / (float)SIN_N));
    sin_ready = 1;
}

/* phase in turns, not radians */
static float fsin(float turns)
{
    float f = (turns - floorf(turns)) * (float)SIN_N;
    int i = (int)f;
    float frac = f - (float)i;
    return sin_lut[i] + (sin_lut[i + 1] - sin_lut[i]) * frac;
}

static unsigned int rnd(water_t *w)
{
    w->rng ^= w->rng << 13;
    w->rng ^= w->rng >> 17;
    w->rng ^= w->rng << 5;
    return w->rng;
}

/* 0..1 */
static float rnd01(water_t *w) { return (float)(rnd(w) & 0xffffff) / 16777216.f; }

/* ------------------------------------------------------------ the two waves
 *
 * FUN_005240c0, the sea's own vertex animator, with the projections
 * FUN_0051c000 precomputes at load folded back in. Two trains:
 *
 *   1  directional, along (cos angle, sin angle), wavelength 2*pi/period and
 *      phase speed `speed`; its amplitude term is 0.25 + 0.75*sin, which spans
 *      -0.5..+1.0, so crests stand twice as far above the mean as troughs fall
 *      below it. That asymmetry is the engine's, not a simplification.
 *   2  RADIAL about (posX, posZ), wavelength 2*pi*length2, angular rate period2.
 *      angle2 and speed2 are loaded and this function never reads them.
 *
 * The first version of this file summed two DIRECTIONAL sines at 1.24 m and
 * 1.61 m and scaled them by 0.02 m, having read `period` as a time and `amp` as
 * a key the loader ignores. It reads it: the string at 0x5756e0 is "amp" and it
 * lands in param_2[8].
 */

/* fsin takes turns; the engine's phases are radians. */
static float fsinr(float rad) { return fsin(rad * (1.f / TWO_PI)); }

/* The radial train's phase, radians. Shared with the shoreline foam, which
   FUN_0051c690 drives off this same sine and nothing else. Zero length2 means
   the section shipped none and there is no wave. */
static float radial_phase(const wsurf_t *c, float x, float z, float t)
{
    float dx = x - c->pos_x, dz = z - c->pos_z;
    if (c->length2 < 1e-4f)
        return 0.f;
    return t * c->period2 + sqrtf(dx * dx + dz * dz) / c->length2;
}

/* The sea's vertical displacement at (x, z), metres, BEFORE the depth damping
   and before the track's own vertical offset. */
static float surf_disp(const water_t *w, float x, float z, float t)
{
    const wsurf_t *c = w->cfg;
    float s1 = fsinr((t * c->speed + x * w->d1x + z * w->d1z) * c->period);
    float s2 = fsinr(radial_phase(c, x, z, t));
    return c->amp * (0.25f + 0.75f * s1) + c->amp2 * s2;
}

/*
 * The height the SHORELINE FOAM keys on. FUN_0051c690 reads the RADIAL train
 * alone, at unit amplitude -- 0x51c71d multiplies the sine by amp2 and then by
 * the 1/amp2 the loader precomputed beside it, which is what that reciprocal
 * was for. WaterLOD_Coast's HeightOn and HeightOff are the surface heights at
 * which the foam is fully on and fully off, so a signal spanning exactly that
 * range is the one the two constants were written for.
 *
 * The engine's remap of the signal to an alpha is asymmetric and is not
 * transcribed -- see water.h.
 */
static float shore_height(const water_t *w, float x, float z, float t)
{
    const float mid = 0.5f * (COAST_HEIGHT_ON + COAST_HEIGHT_OFF);
    const float half = 0.5f * (COAST_HEIGHT_ON - COAST_HEIGHT_OFF);
    return mid + half * fsinr(radial_phase(w->cfg, x, z, t));
}

float water_height(const water_t *w, float x, float z)
{
    return surf_disp(w, x, z, w->t);
}

/* ------------------------------------------------------------------- setup */

/* How deep the water is under a surface vertex, or a large number where there
   is no ground below it at all (open ocean, or off the collision grid). */
static float depth_at(const col_t *col, const vtx_t *v)
{
    float gy, nx, ny, nz;
    if (!col || !col_ground_at(col, v->x, v->z, v->y, &gy, &nx, &ny, &nz))
        return 1e9f;
    return v->y - gy;
}

/*
 * THE SEAM TABLES -- see water.h. Both are built once, off the rest positions,
 * and both exist only because the port displaces authored tiles where the
 * engine tessellates its own grid.
 */

/* A uniform XZ cell grid over one batch's rest vertices, used by both builders.
   `cell` is the bucket size; buckets are singly-linked through `next`. */
typedef struct {
    int   *head;            /* per bucket */
    int   *next;            /* per vertex */
    int    nx, nz;
    float  x0, z0, cell;
} wgrid_t;

static int wgrid_build(wgrid_t *g, const batch_t *b, float cell)
{
    float x1, z1;
    double ax, az, want;
    unsigned int j, cells;

    if (!b->nverts || !b->rest)
        return 0;
    g->x0 = x1 = b->rest[0].x;
    g->z0 = z1 = b->rest[0].z;
    for (j = 1; j < b->nverts; j++) {
        if (b->rest[j].x < g->x0) g->x0 = b->rest[j].x;
        if (b->rest[j].x > x1)    x1    = b->rest[j].x;
        if (b->rest[j].z < g->z0) g->z0 = b->rest[j].z;
        if (b->rest[j].z > z1)    z1    = b->rest[j].z;
    }
    ax = (double)x1 - (double)g->x0;
    az = (double)z1 - (double)g->z0;
    if (cell <= 0.f)
        cell = 1.f;

    /* SIZE THE TABLE IN DOUBLE, AND SIZE IT UP FRONT.
     *
     * A 4 mm weld cell over beach_3's 216 x 197 m sea is 54,002 x 49,252
     * buckets -- 2.66e9, which does not fit in the 32-bit `long` the Vita has
     * and does fit in the 64-bit one the host has. The first version of this
     * computed that product as a `long` and halved the cell until it was under
     * a ceiling: on the host the loop ran, on the device it never ran at all,
     * the bucket count wrapped, and the fill below indexed a table a fraction
     * of the size it thought it had. beach_3 and beach_4 are the two tracks
     * whose seas are big enough to do it, and they are the two that crashed.
     *
     * A coarser cell is only ever slower here -- both callers scan a
     * neighbourhood and compare real distances -- so this picks the cell from
     * the extent instead of discovering it. */
    want = (ax / (double)cell + 2.0) * (az / (double)cell + 2.0);
    if (want > (double)WATER_GRID_MAX_CELLS)
        cell *= (float)sqrt(want / (double)WATER_GRID_MAX_CELLS);

    /* That scaling is only approximate -- the +2 margins do not scale with the
       cell -- so settle it by measuring, not by trusting the estimate. A
       handful of steps at worst, and the loop is bounded so a NaN cannot spin
       it. The first try at this REFUSED the grid when the estimate came out a
       few buckets over, which silently turned the weld off on every track:
       build_weld returns with w->site[bi] still NULL, animate_surface falls
       back to the raw vertex index, and build_stitch is never called at all. */
    for (j = 0; j < 40u; j++) {
        g->nx = (int)(ax / (double)cell) + 2;
        g->nz = (int)(az / (double)cell) + 2;
        if (g->nx < 1) g->nx = 1;
        if (g->nz < 1) g->nz = 1;
        if ((double)g->nx * (double)g->nz <= (double)WATER_GRID_MAX_CELLS)
            break;
        cell *= 1.25f;
    }
    if ((double)g->nx * (double)g->nz > (double)WATER_GRID_MAX_CELLS)
        return 0;
    g->cell = cell;
    cells = (unsigned int)g->nx * (unsigned int)g->nz;

    g->head = malloc((size_t)cells * sizeof(int));
    g->next = malloc((size_t)b->nverts * sizeof(int));
    if (!g->head || !g->next) {
        free(g->head); free(g->next);
        g->head = NULL; g->next = NULL;
        return 0;
    }
    for (j = 0; j < cells; j++)
        g->head[j] = -1;
    for (j = 0; j < b->nverts; j++) {
        int cx = (int)((b->rest[j].x - g->x0) / g->cell);
        int cz = (int)((b->rest[j].z - g->z0) / g->cell);
        int c;
        if (cx < 0) cx = 0;
        if (cx >= g->nx) cx = g->nx - 1;
        if (cz < 0) cz = 0;
        if (cz >= g->nz) cz = g->nz - 1;
        c = cz * g->nx + cx;
        g->next[j] = g->head[c];
        g->head[c] = (int)j;
    }
    return 1;
}

static void wgrid_free(wgrid_t *g)
{
    free(g->head); free(g->next);
    g->head = NULL; g->next = NULL;
}

/* site[j] = the lowest-numbered vertex within WATER_WELD_TOL of j in XZ. */
static void build_weld(water_t *w, unsigned int bi)
{
    batch_t *b = &w->scene->batches[bi];
    wgrid_t g;
    int *site;
    unsigned int j;

    site = malloc((size_t)b->nverts * sizeof(int));
    if (!site)
        return;
    for (j = 0; j < b->nverts; j++)
        site[j] = (int)j;
    if (!wgrid_build(&g, b, WATER_WELD_TOL)) {
        free(site);
        return;
    }
    for (j = 0; j < b->nverts; j++) {
        int cx = (int)((b->rest[j].x - g.x0) / g.cell);
        int cz = (int)((b->rest[j].z - g.z0) / g.cell);
        int dx, dz;
        for (dz = -1; dz <= 1 && site[j] == (int)j; dz++)
            for (dx = -1; dx <= 1 && site[j] == (int)j; dx++) {
                int ux = cx + dx, uz = cz + dz, k;
                if (ux < 0 || uz < 0 || ux >= g.nx || uz >= g.nz)
                    continue;
                for (k = g.head[uz * g.nx + ux]; k >= 0; k = g.next[k]) {
                    float ex, ez;
                    if (k >= (int)j || site[k] != k)
                        continue;   /* only ever point at an earlier ORIGINAL */
                    ex = b->rest[k].x - b->rest[j].x;
                    ez = b->rest[k].z - b->rest[j].z;
                    if (ex * ex + ez * ez <= WATER_WELD_TOL * WATER_WELD_TOL) {
                        site[j] = k;
                        break;
                    }
                }
            }
    }
    wgrid_free(&g);
    w->site[bi] = site;
}

/* Exactly the height animate_surface writes, for one vertex at one time. The
   stitch's own bound is measured with it, so the bound is measured against the
   thing that will actually be drawn -- including the SHALLOW-WATER DAMPING,
   which is where the last of the big corrections came from: a vertex in the
   shallows, whose swell is damped almost flat, pinned to a chord between two
   deep-water vertices carrying the full wave. Near the beach, which is where
   "the waves look cut from one side" was reported. */
static float stitch_h(const water_t *w, unsigned int bi, int j, float t)
{
    const batch_t *b = &w->scene->batches[bi];
    const wsurf_t *c = w->cfg;
    float k = (w->damp && w->damp[bi]) ? w->damp[bi][j] : 1.f;
    return b->rest[j].y + c->offset + (1.f - k) * c->magnet_offset
         + k * surf_disp(w, b->rest[j].x, b->rest[j].z, t);
}

/* Every vertex that lands on the interior of some triangle edge it is not an
   endpoint of. Built off the index buffer, deduplicated by the (lo,hi) pair. */
static void build_stitch(water_t *w, unsigned int bi)
{
    batch_t *b = &w->scene->batches[bi];
    const int *site = w->site[bi];
    wstitch_t *list = NULL;
    unsigned int n = 0, cap = 0;
    wgrid_t g;
    unsigned int e;

    if (!b->idx || !b->nidx || b->nverts > 0xffffu)
        return;
    /* A metre of cell is enough: the scan walks the edge's whole bounding box
       and the sea's own grid step is a metre or more everywhere. */
    if (!wgrid_build(&g, b, 1.f))
        return;

    for (e = 0; e + 2 < b->nidx; e += 3) {
        int tri[3];
        int q;
        tri[0] = b->idx[e]; tri[1] = b->idx[e + 1]; tri[2] = b->idx[e + 2];
        for (q = 0; q < 3; q++) {
            int ia = tri[q], ib = tri[(q + 1) % 3];
            float ax, az, ex, ez, len2, x0, x1, z0, z1;
            int cx0, cx1, cz0, cz1, cx, cz;

            /* Both directions. A tile's OUTER edge belongs to one triangle
               only -- the neighbouring tile has its own duplicated copy -- so
               a canonical-direction filter drops every edge whose single
               winding happens to run the wrong way, which is most of the
               seams that matter. Walking both is a few hundred duplicate
               entries and the fixup is idempotent. */
            if (site[ia] == site[ib])
                continue;
            ax = b->rest[ia].x; az = b->rest[ia].z;
            ex = b->rest[ib].x - ax; ez = b->rest[ib].z - az;
            len2 = ex * ex + ez * ez;
            if (len2 < 1e-8f)
                continue;
            x0 = ax < ax + ex ? ax : ax + ex; x1 = ax + ex > ax ? ax + ex : ax;
            z0 = az < az + ez ? az : az + ez; z1 = az + ez > az ? az + ez : az;
            cx0 = (int)((x0 - g.x0) / g.cell) - 1;
            cx1 = (int)((x1 - g.x0) / g.cell) + 1;
            cz0 = (int)((z0 - g.z0) / g.cell) - 1;
            cz1 = (int)((z1 - g.z0) / g.cell) + 1;
            if (cx0 < 0) cx0 = 0;
            if (cz0 < 0) cz0 = 0;
            if (cx1 >= g.nx) cx1 = g.nx - 1;
            if (cz1 >= g.nz) cz1 = g.nz - 1;
            for (cz = cz0; cz <= cz1; cz++)
                for (cx = cx0; cx <= cx1; cx++) {
                    int k;
                    for (k = g.head[cz * g.nx + cx]; k >= 0; k = g.next[k]) {
                        float px, pz, t, perp;
                        /* EVERY vertex on the edge, not just the site's
                           representative. Narrowing this to representatives
                           passes the whole suite on the ten shipped tracks --
                           no T-junction vertex in any of them is itself a
                           duplicate -- but that is a property of this
                           tessellation, not of the rule: a duplicated one would
                           have its representative put back on the chord and its
                           twin left up on the swell. A few dozen more entries in
                           a list of a few hundred buys not having to rely on it. */
                        if (site[k] == site[ia] || site[k] == site[ib])
                            continue;
                        px = b->rest[k].x - ax; pz = b->rest[k].z - az;
                        t = (px * ex + pz * ez) / len2;
                        if (t <= 1e-3f || t >= 1.f - 1e-3f)
                            continue;
                        perp = (px * ez - pz * ex);
                        if (perp * perp > WATER_STITCH_TOL * WATER_STITCH_TOL
                                          * len2)
                            continue;
                        /* IS THE CHORD ACTUALLY THE SURFACE HERE?
                         *
                         * Pinning the vertex to the chord closes the hole, and
                         * it closes it by DELETING whatever the swell was doing
                         * between the two endpoints. Over a short edge that is
                         * a hairline either way. Over a long one it is a
                         * straight flat scar across the water -- measured on
                         * beach_2, a 9.1 m edge whose vertex was dropped 0.92 m,
                         * and on beach_3 nine edges over 8 m and one of 23.5 m.
                         * That was reported as "the waves near the beach look
                         * cut from one side", and it is worse than the crack.
                         *
                         * The geometric fix -- splitting the coarse triangle at
                         * the vertex -- is NOT available: the tiles carry their
                         * own UV parameterisation and a seam pair's UVs differ
                         * by up to 43 units, so splicing the neighbour's vertex
                         * into the coarse triangle would tear the texture far
                         * worse than the crack it closed.
                         *
                         * So close the ones that are hairlines and leave the
                         * rest, on the worst case over a phase sweep of the
                         * track's own displacement rather than on a rule of
                         * thumb about length. Undamped, i.e. full amplitude,
                         * because build_damping has not run yet and because
                         * that can only ever exclude more. */
                        {
                            float worst = 0.f;
                            int q2;
                            for (q2 = 0; q2 < WATER_STITCH_SWEEP_N; q2++) {
                                float tt = WATER_STITCH_SWEEP_T
                                    * (float)q2 / (float)WATER_STITCH_SWEEP_N;
                                float hv = stitch_h(w, bi, k, tt);
                                float ha = stitch_h(w, bi, site[ia], tt);
                                float hb = stitch_h(w, bi, site[ib], tt);
                                float e = fabsf(hv - (ha + (hb - ha) * t));
                                if (e > worst) worst = e;
                            }
                            if (worst > WATER_STITCH_MAX_SAG)
                                continue;
                        }
                        if (n == cap) {
                            unsigned int nc = cap ? cap * 2u : 32u;
                            wstitch_t *nl = realloc(list,
                                                    nc * sizeof(wstitch_t));
                            if (!nl)
                                goto done;
                            list = nl; cap = nc;
                        }
                        list[n].v = (unsigned short)k;
                        list[n].a = (unsigned short)site[ia];
                        list[n].b = (unsigned short)site[ib];
                        list[n].t = t;
                        n++;
                    }
                }
        }
    }
done:
    wgrid_free(&g);
    w->stitch[bi] = list;
    w->n_stitch[bi] = n;

}

static void build_damping(water_t *w, const col_t *col)
{
    scene_t *s = w->scene;
    const wsurf_t *c = w->cfg;
    unsigned int i, j;

    for (i = 0; i < s->n_batches; i++) {
        batch_t *b = &s->batches[i];
        if (!(b->flags & BATCH_WATER))
            continue;
        w->damp[i] = malloc(b->nverts * sizeof(float));
        if (!w->damp[i])
            continue;
        for (j = 0; j < b->nverts; j++) {
            /* LINEAR, and over cfg->magnet_radius. Both used to be guesses --
               a smoothstep over 0.5 m -- and both are FUN_0051c000's: it
               divides by magnetRadius and clamps, with no shaping. The 2.55 m
               it clamps over is five times the guess, so the swell now settles
               out over a real shelf rather than snapping flat at the last
               half-metre.

               Sampled at the WELD REPRESENTATIVE, not at this vertex: two sides
               of a tile seam sit up to a millimetre apart and the seabed under
               them is not the same triangle, so a per-vertex query gives the
               duplicates different damping and the seam parts. */
            unsigned int r = w->site[i] ? (unsigned int)w->site[i][j] : j;
            float d = depth_at(col, &b->verts[r]) / c->magnet_radius;
            if (d < 0.f) d = 0.f;
            if (d > 1.f) d = 1.f;
            w->damp[i][j] = d;
        }
    }
}

/*
 * water_init is reached again on every track change AND on every texture-quality
 * change (which reloads both scenes). It memsets over three pointer arrays and
 * everything they hold, so without this every reload leaked the damping and the
 * per-vertex colour buffers -- a slow bite out of the same newlib heap vitaGL's
 * RAM pool draws from.
 *
 * It is the CALLER's job, not water_init's, because water_init has always
 * accepted an uninitialised struct and vis_test still hands it a stack local;
 * freeing from in there would free garbage on the first call.
 */
void water_free(water_t *w)
{
    unsigned int i;

    /* n_alloc, not scene->n_batches: by the time a reload gets here the caller
       has usually already released the scene these arrays were sized against,
       and reading the new batch count would walk off the end of the old ones. */
    for (i = 0; i < w->n_alloc; i++) {
        int r;
        if (w->damp)       free(w->damp[i]);
        if (w->coast_rgba) free(w->coast_rgba[i]);
        if (w->surf_rgba)  free(w->surf_rgba[i]);
        if (w->site)       free(w->site[i]);
        if (w->stitch)     free(w->stitch[i]);
        for (r = 0; r < WATER_DRAW_RINGS; r++)
            if (w->vring[r]) free(w->vring[r][i]);
    }
    free(w->damp);
    free(w->coast_rgba);
    free(w->surf_rgba);
    free(w->site);
    free(w->stitch);
    free(w->n_stitch);
    {
        int r;
        for (r = 0; r < WATER_DRAW_RINGS; r++)
            free(w->vring[r]);
    }
    memset(w, 0, sizeof(*w));
}

void water_init(water_t *w, scene_t *scene, const col_t *col, int track)
{
    unsigned int i;

    /* A PLAIN memset, not water_free: this has always accepted an uninitialised
       struct (vis_test hands it a stack local) and it must keep doing so. The
       caller owns the free -- see water_free's comment for why that is not an
       oversight. */
    memset(w, 0, sizeof(*w));
    w->scene = scene;
    w->n_alloc = scene->n_batches;
    w->rng = 0x1234567u;
    sin_init();

    /* The track's own water surface section. Clamped rather than asserted: a
       fixture with no track of its own gets beach_1's, which is what the whole
       port used to get. */
    if (track < 0 || track >= WSURF_N_TRACKS)
        track = 0;
    w->cfg = &WSURF[track];
    w->d1x = cosf(w->cfg->angle_deg * DEG);
    w->d1z = sinf(w->cfg->angle_deg * DEG);

    w->damp = calloc(scene->n_batches, sizeof(float *));
    w->coast_rgba = calloc(scene->n_batches, sizeof(unsigned char *));
    w->surf_rgba = calloc(scene->n_batches, sizeof(unsigned char *));
    w->site = calloc(scene->n_batches, sizeof(int *));
    w->stitch = calloc(scene->n_batches, sizeof(wstitch_t *));
    w->n_stitch = calloc(scene->n_batches, sizeof(unsigned int));

    for (i = 0; i < scene->n_batches; i++) {
        batch_t *b = &scene->batches[i];
        if (!(b->flags & BATCH_ANY_WATER))
            continue;
        scene_keep_rest(b);
        if (b->flags & BATCH_COAST)
            w->coast_rgba[i] = malloc((size_t)b->nverts * 4);
        if (b->flags & BATCH_WATER) {
            w->surf_rgba[i] = malloc((size_t)b->nverts * 4);
            /* The weld only. The stitch is built after build_damping below:
               it reads the weld map AND the damping, because its own bound is
               measured against the height that will be drawn. */
            if (w->site)
                build_weld(w, i);
        }
        /* A draw ring only where the draw is big enough for vitaGL to hand GXM
           this array instead of copying it -- three batches over the ten tracks.
           See water_t.vring. The gate is the batch's own size, so nothing has to
           be kept in step with it. `surf_rgba` needs no ring: it is written once,
           here, and never again; `coast_rgba` IS rewritten every frame but no
           coast batch on any track comes near the line. */
        if ((size_t)b->nverts * sizeof(vtx_t) > WATER_CLIENT_PTR_LIMIT) {
            int r;
            for (r = 0; r < WATER_DRAW_RINGS; r++) {
                if (!w->vring[r])
                    w->vring[r] = calloc(scene->n_batches, sizeof(vtx_t *));
                if (w->vring[r])
                    w->vring[r][i] = malloc((size_t)b->nverts * sizeof(vtx_t));
            }
        }
    }
    build_damping(w, col);

    /* and now the T-junctions, which need the damping to judge themselves */
    for (i = 0; i < scene->n_batches; i++) {
        batch_t *b = &scene->batches[i];
        if (!(b->flags & BATCH_WATER))
            continue;
        if (w->site && w->site[i] && w->stitch && w->n_stitch)
            build_stitch(w, i);
    }

    /* The surface's own alpha, from the depth measure build_damping just made:
       alphaMin in the shallows so the wet sand reads through, alphaMax out at
       sea. alphaPow shapes the ramp. These are all recovered (FUN_00521540) and
       went unused in the first build, which is why the sea met the sand at a
       hard edge and read as a step. */
    for (i = 0; i < scene->n_batches; i++) {
        batch_t *b = &scene->batches[i];
        unsigned int j;
        if (!(b->flags & BATCH_WATER) || !w->surf_rgba[i])
            continue;
        for (j = 0; j < b->nverts; j++) {
            /* Its OWN depth ramp, not the swell damping's. Sharing the latter
               put every vertex past 0.5 m of depth at full opacity, which is
               most of the sea. */
            float d = depth_at(col, &b->verts[j]) / WATER_ALPHA_DEPTH;
            float k, av;
            unsigned char *p;
            if (d < 0.f) d = 0.f;
            if (d > 1.f) d = 1.f;
            k = powf(d, WATER_ALPHA_POW);
            av = w->cfg->alpha_min
               + (w->cfg->alpha_max - w->cfg->alpha_min) * k;
            p = &w->surf_rgba[i][j * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = (unsigned char)(av * 255.f + 0.5f);
        }
    }

    /* ------------------------------------------------------------ the horizon
     *
     * Where the sea CONTINUES once the authored tiles run out. The engine has
     * no such problem: its water is a LOD grid laid out around the camera, so
     * it always reaches the far plane. The port has the tiles the artists drew
     * over the map, and the sky dome is drawn camera-locked -- at infinity --
     * so from the edge of the water the band between the tiles' outer rim and
     * the dome's lower edge is a hole. Reported as "the ocean ends before the
     * skydome".
     *
     * The plane's height is taken from the OUTER rim of the authored surface,
     * not from its mean: that rim is where the horizon has to carry on from,
     * and on beach_1 the sea is not flat. The swell's own mean, amp*0.25 (the
     * directional train's sine averages zero and the 0.25 bias does not), plus
     * the track's offset, puts it level with the tiles at rest.
     */
    {
        double cx = 0.0, cz = 0.0, sy = 0.0;
        unsigned int nv = 0, nout = 0;
        float r2max = 0.f, rcut;

        for (i = 0; i < scene->n_batches; i++) {
            batch_t *b = &scene->batches[i];
            unsigned int j;
            if (!(b->flags & BATCH_WATER) || !b->rest)
                continue;
            for (j = 0; j < b->nverts; j++) {
                cx += b->rest[j].x; cz += b->rest[j].z; nv++;
            }
        }
        if (nv) {
            cx /= (double)nv; cz /= (double)nv;
            for (i = 0; i < scene->n_batches; i++) {
                batch_t *b = &scene->batches[i];
                unsigned int j;
                if (!(b->flags & BATCH_WATER) || !b->rest)
                    continue;
                for (j = 0; j < b->nverts; j++) {
                    float dx = b->rest[j].x - (float)cx;
                    float dz = b->rest[j].z - (float)cz;
                    float r2 = dx * dx + dz * dz;
                    if (r2 > r2max) r2max = r2;
                }
            }
            /* The outer tenth by radius -- far enough out to be the rim the
               horizon carries on from -- but widened until the sample is big
               enough to mean the SWELL away as well. beach_2's sea is a long
               thin strip and its outermost decile is a handful of vertices,
               each of them at whatever phase of the wave it happens to sit;
               one of those is not a sea level. */
            rcut = 0.81f * r2max;
            for (;;) {
                unsigned int c2 = 0;
                for (i = 0; i < scene->n_batches; i++) {
                    batch_t *b = &scene->batches[i];
                    unsigned int j;
                    if (!(b->flags & BATCH_WATER) || !b->rest)
                        continue;
                    for (j = 0; j < b->nverts; j++) {
                        float dx = b->rest[j].x - (float)cx;
                        float dz = b->rest[j].z - (float)cz;
                        if (dx * dx + dz * dz >= rcut)
                            c2++;
                    }
                }
                if (c2 >= WATER_HORIZON_RIM_MIN || rcut <= 0.f)
                    break;
                rcut *= 0.5f;
            }
            for (i = 0; i < scene->n_batches; i++) {
                batch_t *b = &scene->batches[i];
                unsigned int j;
                if (!(b->flags & BATCH_WATER) || !b->rest)
                    continue;
                for (j = 0; j < b->nverts; j++) {
                    float dx = b->rest[j].x - (float)cx;
                    float dz = b->rest[j].z - (float)cz;
                    if (dx * dx + dz * dz < rcut)
                        continue;
                    sy += b->rest[j].y; nout++;
                }
                if (!w->horizon_tex)
                    w->horizon_tex = b->gl_tex;
            }
            if (nout) {
                w->horizon = 1;
                w->horizon_y = (float)(sy / (double)nout) + w->cfg->offset
                             + 0.25f * w->cfg->amp;
                w->horizon_alpha = w->cfg->alpha_max;
            }
        }
    }

    w->wave_tex = scene_tex(scene, "water_wave");

    /* One spawner per water_wave_N marker, each with the two independent
       timers FUN_00525700 keeps: a long one and a short one. */
    for (i = 0; i < scene->n_markers && w->n_spawn < WATER_MAX_SPAWN; i++) {
        marker_t *m = &scene->markers[i];
        wave_spawn_t *sp;
        if (strncmp(m->name, "water_wave_", 11))
            continue;
        sp = &w->spawn[w->n_spawn++];
        sp->x = m->x;
        sp->y = m->y;
        sp->z = m->z;
        /* The sea's REST height under this marker, from the nearest surface
           vertex -- the sprite stands on the water and the tiles are not all at
           one height (beach_1's run from -0.22 to 0.00). Falls back to the
           marker's own y where the track has no sea at all. */
        {
            float best = 1e30f;
            unsigned int bi2, j2;
            sp->sea_y = m->y;
            for (bi2 = 0; bi2 < scene->n_batches; bi2++) {
                batch_t *wb = &scene->batches[bi2];
                if (!(wb->flags & BATCH_WATER) || !wb->rest)
                    continue;
                for (j2 = 0; j2 < wb->nverts; j2++) {
                    float dx = wb->rest[j2].x - m->x;
                    float dz = wb->rest[j2].z - m->z;
                    float d2 = dx * dx + dz * dz;
                    if (d2 < best) { best = d2; sp->sea_y = wb->rest[j2].y; }
                }
            }
        }
        sp->dx = sinf(m->yaw * DEG);
        sp->dz = cosf(m->yaw * DEG);
        /* stagger the first firing so all five markers do not break together */
        sp->t_long = WAVE_TIME_LONG * rnd01(w);
        sp->t_short = WAVE_TIME_SHORT * rnd01(w);
    }
}

/* --------------------------------------------------------------- the waves */

static void wave_spawn(water_t *w, const wave_spawn_t *sp, int is_long)
{
    int i;
    float a, c, s;

    for (i = 0; i < WATER_MAX_WAVES; i++)
        if (!w->waves[i].active)
            break;
    if (i == WATER_MAX_WAVES)
        return;

    /* WaterLOD_Wave: Angle is the spread around the marker's facing, Len the
       spread along the crest. */
    a = (rnd01(w) * 2.f - 1.f) * WAVE_SPAWN_ANGLE * DEG;
    c = cosf(a);
    s = sinf(a);
    w->waves[i].dx = sp->dx * c + sp->dz * s;
    w->waves[i].dz = -sp->dx * s + sp->dz * c;
    /* offset along the crest, which is the travel direction turned 90 degrees */
    a = (rnd01(w) * 2.f - 1.f) * WAVE_SPAWN_LEN;
    w->waves[i].x = sp->x + w->waves[i].dz * a;
    w->waves[i].y = sp->y;
    w->waves[i].sea_y = sp->sea_y;
    w->waves[i].z = sp->z - w->waves[i].dx * a;
    /* FUN_00529c60: a long wave takes TimeLifeLong flat, a short one takes
       TimeLifeShort with +/- TimeLifeShortDisp of spread. */
    w->waves[i].life = is_long
        ? WAVE_LIFE_LONG
        : WAVE_LIFE_SHORT + (rnd01(w) * 2.f - 1.f) * WAVE_LIFE_SHORT_DISP;
    w->waves[i].age = 0.f;
    w->waves[i].u0 = rnd01(w);
    w->waves[i].active = 1;
}

/* The crest's height envelope over its life: up over IncTime, flat, down over
   what is left after DecTime. FUN_0052a030 reads it out of the record at +0x48
   and multiplies it by Height. */
static float wave_env(const wave_t *v)
{
    float a = (v->life > 1e-4f) ? v->age / v->life : 1.f;
    if (a < WAVE_INC_TIME)
        return (WAVE_INC_TIME > 1e-4f) ? a / WAVE_INC_TIME : 1.f;
    if (a > WAVE_DEC_TIME) {
        float d = 1.f - WAVE_DEC_TIME;
        return (d > 1e-4f) ? (1.f - a) / d : 0.f;
    }
    return 1.f;
}

void water_step(water_t *w, float dt)
{
    int i;

    w->t += dt;
    w->n_live = 0;

    for (i = 0; i < w->n_spawn; i++) {
        wave_spawn_t *sp = &w->spawn[i];
        sp->t_long -= dt;
        if (sp->t_long < 0.f) {
            wave_spawn(w, sp, 1);
            sp->t_long = WAVE_TIME_LONG;
        }
        sp->t_short -= dt;
        if (sp->t_short < 0.f) {
            wave_spawn(w, sp, 0);
            sp->t_short = WAVE_TIME_SHORT;
        }
    }

    for (i = 0; i < WATER_MAX_WAVES; i++) {
        wave_t *v = &w->waves[i];
        if (!v->active)
            continue;
        v->age += dt;
        if (v->age >= v->life) {
            v->active = 0;
            continue;
        }
        v->x += v->dx * WAVE_SPEED * dt;
        v->z += v->dz * WAVE_SPEED * dt;
        w->n_live++;
    }
}

/*
 * One wave sprite. FUN_0052a030's geometry, in its own terms:
 *
 *   axis    the crest direction; the quad runs +/- Len along it
 *   up      (camera - position), with its component along the crest removed,
 *           normalised -- so the quad is a billboard hinged on its crest
 *   bottom  position +/- Len*axis            + DHeight*up
 *   top     bottom + Height*envelope*up
 *
 * u sweeps one full turn of the texture, offset by the record's scroll value.
 */
/* A BREAKING WAVE STANDS ON THE WATER, not at the height its marker was
 * authored at.
 *
 * wave_spawn takes the sprite's y straight from the `water_wave_N` marker, and
 * measured against the surface the port actually draws those markers sit BELOW
 * it: 0.8 m under on beach_1, 0.2 m on beach_2, level to a couple of
 * centimetres on beach_4. A 0.3 m crest 0.8 m down is never seen at all, and
 * one a fifth of a metre down is sliced by the waterline as the swell rolls
 * over it -- which is what "the waves near the beach look cut from one side"
 * is.
 *
 * The markers are authored in the ENGINE's frame, and the port's surface is the
 * authored tiles plus WSURF's own offset, so the two do not have to agree and
 * on four of the five tracks they do not. The surface is the thing that can be
 * measured, so the sprite is put on it: the swell at the sprite's own position,
 * which also makes the crest rise and fall with the water under it instead of
 * hanging at a fixed height while the sea moves through it.
 *
 * Kept: the marker's y decides NOTHING here any more, which is a divergence and
 * is named as one. What is not recovered is where the engine puts the sprite;
 * FUN_0052a030 reads the record's own position and the record is filled by the
 * spawner, so the answer is in FUN_00525700's write and not yet read out.
 */
static float wave_base_y(const water_t *w, const wave_t *v)
{
    return v->sea_y + w->cfg->offset + surf_disp(w, v->x, v->z, w->t);
}

static void wave_draw(water_t *w, const wave_t *v, const float eye[3])
{
    vtx_t q[4];
    float ax = v->dz, az = -v->dx;          /* the crest, across the travel */
    float ex = eye[0] - v->x, ey = eye[1] - v->y, ez = eye[2] - v->z;
    float d = ex * ax + ez * az;
    float ux, uy, uz, len;
    float env = wave_env(v);
    float h = WAVE_HEIGHT * env;
    float u0 = v->u0 + w->t * WAVE_ANIM_SPEED;
    float base = wave_base_y(w, v);

    ux = ex - ax * d;
    uy = ey;
    uz = ez - az * d;
    len = sqrtf(ux * ux + uy * uy + uz * uz);
    if (len < 1e-4f)
        return;
    ux /= len; uy /= len; uz /= len;

    q[0].x = v->x - ax * WAVE_LEN + ux * WAVE_DHEIGHT;
    q[0].y = base + uy * WAVE_DHEIGHT;
    q[0].z = v->z - az * WAVE_LEN + uz * WAVE_DHEIGHT;
    q[1].x = v->x + ax * WAVE_LEN + ux * WAVE_DHEIGHT;
    q[1].y = q[0].y;
    q[1].z = v->z + az * WAVE_LEN + uz * WAVE_DHEIGHT;
    q[2].x = q[1].x + ux * h; q[2].y = q[1].y + uy * h; q[2].z = q[1].z + uz * h;
    q[3].x = q[0].x + ux * h; q[3].y = q[0].y + uy * h; q[3].z = q[0].z + uz * h;

    q[0].u = u0;        q[0].v = 1.f;
    q[1].u = u0 + 1.f;  q[1].v = 1.f;
    q[2].u = u0 + 1.f;  q[2].v = 0.f;
    q[3].u = u0;        q[3].v = 0.f;

    glColor4f(1.f, 1.f, 1.f, env);
    glVertexPointer(3, GL_FLOAT, sizeof(vtx_t), &q[0].x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(vtx_t), &q[0].u);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

/* ------------------------------------------------------------------- draw  */

/* A vertex's three draws out of the texRad/texSpeed ranges, from its index.
   Cheap, stateless and stable across a reload -- see water.h for why the draw
   is the port's and the ranges are the game's. Three 8-bit slices of one
   xorshift-mixed index, so radius, rate and starting phase are independent. */
static unsigned int vhash(unsigned int i)
{
    i ^= i << 13; i ^= i >> 17; i ^= i << 5;
    return i * 2654435761u;
}
static float slice(unsigned int h, int n) { return (float)((h >> (n * 8)) & 0xff) / 255.f; }

static void animate_surface(water_t *w, unsigned int bi)
{
    batch_t *b = &w->scene->batches[bi];
    const wsurf_t *c = w->cfg;
    const float *damp = w->damp[bi];
    const int *site = w->site ? w->site[bi] : NULL;
    unsigned int j;

    if (!b->rest)
        return;
    for (j = 0; j < b->nverts; j++) {
        /* THROUGH THE WELD MAP. `rest` for the height, and the vertex INDEX for
           the UV orbit's hash, both come from the site rather than from j: the
           shipped tiles duplicate their shared edges, so keying either on j
           gives the two sides of a seam a different swell and a different
           shimmer, and the texture tears along every tile boundary. The engine
           has neither problem because it tessellates one shared grid. */
        unsigned int sj = site ? (unsigned int)site[j] : j;
        const vtx_t *r = &b->rest[j];
        const vtx_t *sr = &b->rest[sj];
        float k = damp ? damp[j] : 1.f;
        unsigned int h = vhash(sj + 1u);
        /* FUN_005240c0 does NOT scroll the sea's UVs along a line. It advances
           a phase per vertex by rate*dt and puts the UV on a circle of radius
           texRad about its rest value -- a shimmer, not a current. The port had
           a linear scroll built on texScaleX/Z, whose real conversion is raw*0.1
           (not raw*0.01) and which is the world-to-UV rate for the grid the
           engine tessellates itself; the port's tiles carry authored UVs, so it
           has nothing to scale. */
        float rad  = c->tex_rad_min
                   + (c->tex_rad_max - c->tex_rad_min) * slice(h, 0);
        float rate = c->tex_speed_min
                   + (c->tex_speed_max - c->tex_speed_min) * slice(h, 1);
        float ph   = slice(h, 2) + w->t * rate * (1.f / 360.f);  /* turns */
        b->verts[j].y = sr->y + c->offset + (1.f - k) * c->magnet_offset
                      + k * surf_disp(w, sr->x, sr->z, w->t);
        b->verts[j].u = r->u + rad * fsin(ph + 0.25f);
        b->verts[j].v = r->v + rad * fsin(ph);
    }

    /* THE T-JUNCTIONS. A vertex sitting part-way along a coarser tile's edge
       has to end up on that edge, or the swell opens the two apart -- 269 of
       them on beach_1, and the worst of them opens 0.34 m (0.60 m on
       beach_3), because the edge under it is 17.4 m and the wavelength is
       10.5 m. Endpoints are ordinary grid vertices and were written above,
       so one pass is enough. */
    if (w->stitch && w->stitch[bi]) {
        const wstitch_t *st = w->stitch[bi];
        unsigned int q, ns = w->n_stitch[bi];
        /* ONE pass. A second was written for the case where a T-junction's
           own chord endpoint is itself a T-junction vertex of a third tile; a
           mutation that removed it changed nothing on any of the ten shipped
           tracks, so that case does not occur in this data and the pass was
           dead. The endpoints are ordinary grid vertices, written above. */
        for (q = 0; q < ns; q++)
            b->verts[st[q].v].y = b->verts[st[q].a].y
                + (b->verts[st[q].b].y - b->verts[st[q].a].y) * st[q].t;
    }
}

/* The coast band, the stream and the waterfall: scroll the UVs, and hold the
   band WATER_DECAL_LIFT off its rest height. These three are the surfaces the
   art laid on solid geometry -- see water.h for the measurement -- so without
   the lift they z-fight the ground they sit on. Written from rest every frame
   rather than once at load, so it cannot drift and so a test can see it. */
static void animate_scroll(water_t *w, unsigned int bi, float du, float dv)
{
    batch_t *b = &w->scene->batches[bi];
    unsigned int j;

    if (!b->rest)
        return;
    for (j = 0; j < b->nverts; j++) {
        b->verts[j].y = b->rest[j].y + WATER_DECAL_LIFT;
        b->verts[j].u = b->rest[j].u + du;
        b->verts[j].v = b->rest[j].v + dv;
    }
}

static void animate_coast(water_t *w, unsigned int bi)
{
    batch_t *b = &w->scene->batches[bi];
    unsigned char *rgba = w->coast_rgba[bi];
    float span = COAST_HEIGHT_ON - COAST_HEIGHT_OFF;
    unsigned int j;

    animate_scroll(w, bi, 0.f, w->t * COAST_SCROLL_VEL);
    if (!rgba)
        return;
    for (j = 0; j < b->nverts; j++) {
        /* WaterLOD_Coast's two heights, used as what they say they are: the
           surface height at which the foam is fully on, and the one at which
           it is fully off. */
        float h = shore_height(w, b->rest[j].x, b->rest[j].z, w->t);
        float k = (span > 1e-4f) ? (h - COAST_HEIGHT_OFF) / span : 1.f;
        float a;
        if (k < 0.f) k = 0.f;
        if (k > 1.f) k = 1.f;
        a = COAST_ALPHA_MIN + (COAST_ALPHA_MAX - COAST_ALPHA_MIN) * k;
        rgba[j * 4] = rgba[j * 4 + 1] = rgba[j * 4 + 2] = 255;
        rgba[j * 4 + 3] = (unsigned char)(a * 255.f);
    }
}

/* The vertices to hand GXM for batch `bi`: this frame's ring slice where the
 * batch has one, and the batch's own array where it does not. The animation
 * still runs in place into b->verts -- that keeps the array every host harness
 * reads back the animated one, and keeps the scene's ownership of it intact
 * (main.c releases the scene before water_free) -- so the ring costs one copy
 * per frame on the three batches that need it, against the per-vertex sine
 * animate_surface is already paying on the same vertices. */
static const vtx_t *draw_verts(water_t *w, unsigned int bi)
{
    const batch_t *b = &w->scene->batches[bi];
    vtx_t *slice = w->vring[w->ring] ? w->vring[w->ring][bi] : NULL;

    if (!slice)
        return b->verts;
    memcpy(slice, b->verts, (size_t)b->nverts * sizeof(vtx_t));
    return slice;
}

static void draw_batch_v(const batch_t *b, const vtx_t *v)
{
    glBindTexture(GL_TEXTURE_2D, b->gl_tex);
    glVertexPointer(3, GL_FLOAT, sizeof(vtx_t), &v[0].x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(vtx_t), &v[0].u);
    glDrawElements(GL_TRIANGLES, b->nidx, GL_UNSIGNED_SHORT, b->idx);
}

static void draw_batch(const batch_t *b)
{
    draw_batch_v(b, b->verts);
}

void water_draw_horizon(const water_t *w, const float eye[3])
{
    /* A flat DISC centred on the eye, in two radius bands. It goes down with
       the sky -- depth test and depth write both off, before any world geometry
       -- so every pixel the world owns is painted over it and this is left only
       where nothing else reached: the band between the authored tiles' outer
       edge and the dome's lower rim. A disc rather than a ring because the
       tiles stop at a different distance in every direction and a ring with an
       inner radius would leave the shortfall showing; covering the middle costs
       nothing, since the seabed is drawn over it and the translucent sea then
       blends against the seabed exactly as before.

       A plane at or below eye level never projects above the horizon line, so
       it cannot bleed into the sky. */
    static const float band[3] = { 0.f, WATER_HORIZON_MID, WATER_HORIZON_OUT };
    const int N = WATER_HORIZON_SEGMENTS;
    /* STATIC, not stack. These are handed to GL as client pointers and this
       function returns immediately afterwards; vitaGL copies a draw this small
       into its own buffer today, but that is a property of the speed hack's
       threshold and not a promise, and a client pointer into a dead frame is
       the kind of bug that shows up as one corrupt draw a minute. */
    static float verts[WATER_HORIZON_SEGMENTS * 6][3];
    static float uv[WATER_HORIZON_SEGMENTS * 6][2];
    int i, r;

    if (!w->horizon || !w->horizon_tex)
        return;
    /* Under the surface there is no horizon to continue, and the plane would
       fill the screen. */
    if (eye[1] <= w->horizon_y + 0.05f)
        return;

    glDisable(GL_ALPHA_TEST);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    /* glColor4f is ignored while a colour array is bound, and scene_draw only
       turns one off again when it had lighting on -- which the sky pass, the
       thing that draws immediately before this, does not. */
    glDisableClientState(GL_COLOR_ARRAY);
    glBindTexture(GL_TEXTURE_2D, w->horizon_tex);
    glColor4f(1.f, 1.f, 1.f, w->horizon_alpha);

    for (r = 0; r < 2; r++) {
        /* Plain triangles rather than a strip: the port's GL surface is the one
           both the device and testgl/ implement, and a strip is not in it. */
        for (i = 0; i < N; i++) {
            const float a0 = (float)i * (TWO_PI / (float)N);
            const float a1 = (float)(i + 1) * (TWO_PI / (float)N);
            float quad[4][2];
            /* WOUND FOR AN UPWARD NORMAL. The obvious 0,1,2 / 0,2,3 over a
               ring laid out by increasing angle comes out CLOCKWISE seen from
               above, i.e. facing DOWN, and the race frame draws with
               GL_CULL_FACE on (main.c) -- so the first version of this plane
               was submitted in full and culled in full, and the gap it was
               written to close stayed exactly as it was. testgl records draws
               and does not cull, which is why nothing here could see it; part 3
               now checks the winding itself. */
            static const int order[6] = { 0, 2, 1, 0, 3, 2 };
            int q;
            quad[0][0] = cosf(a0) * band[r];     quad[0][1] = sinf(a0) * band[r];
            quad[1][0] = cosf(a0) * band[r + 1]; quad[1][1] = sinf(a0) * band[r + 1];
            quad[2][0] = cosf(a1) * band[r + 1]; quad[2][1] = sinf(a1) * band[r + 1];
            quad[3][0] = cosf(a1) * band[r];     quad[3][1] = sinf(a1) * band[r];
            for (q = 0; q < 6; q++) {
                int k = i * 6 + q;
                verts[k][0] = eye[0] + quad[order[q]][0];
                verts[k][1] = w->horizon_y;
                verts[k][2] = eye[2] + quad[order[q]][1];
                uv[k][0] = verts[k][0] * WATER_HORIZON_UV;
                uv[k][1] = verts[k][2] * WATER_HORIZON_UV;
            }
        }
        glVertexPointer(3, GL_FLOAT, sizeof(verts[0]), &verts[0][0]);
        glTexCoordPointer(2, GL_FLOAT, sizeof(uv[0]), &uv[0][0]);
        glDrawArrays(GL_TRIANGLES, 0, N * 6);
    }

    glColor4f(1.f, 1.f, 1.f, 1.f);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_ALPHA_TEST);
}

void water_draw(water_t *w, const float eye[3])
{
    scene_t *s = w->scene;
    unsigned int i;
    int j;

    /* Next slice, so this frame does not write over the one the GPU may still be
       reading two frames back. See water_t.vring. */
    w->ring = (w->ring + 1u) % (unsigned)WATER_DRAW_RINGS;

    /* --- the sea surface: BLENDED, with the depth-driven alpha above ------
       Depth writes stay ON: it is a single layer with no self-overlap, and the
       foam and the wave sprites have to depth-test against it. */
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnableClientState(GL_COLOR_ARRAY);
    for (i = 0; i < s->n_batches; i++) {
        batch_t *b = &s->batches[i];
        if (!(b->flags & BATCH_WATER) || !w->surf_rgba[i])
            continue;
        animate_surface(w, i);
        glColorPointer(4, GL_UNSIGNED_BYTE, 0, w->surf_rgba[i]);
        draw_batch_v(b, draw_verts(w, i));
    }
    glDisableClientState(GL_COLOR_ARRAY);
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);

    /* --- the stream and the waterfall, also blended ----------------------
       From here down every surface is a band the art laid ON solid geometry, so
       everything is drawn biased toward the camera. WATER_DECAL_LIFT does the
       same job in world space and the two are deliberately both on: the lift is
       a fixed distance and stops being enough far out, the bias scales with the
       depth slope but depends on what the driver makes of GXM's bias units. */
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(WATER_DECAL_OFFSET_FACTOR, WATER_DECAL_OFFSET_UNITS);

    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (i = 0; i < s->n_batches; i++) {
        batch_t *b = &s->batches[i];
        /* Each kind's OWN alpha and its own scroll flag, both straight out of
           the engine's node table -- see the 0x575710 block in vis_data.h. The
           three differ, and one number for all of them was a guess.

           A pool does not scroll (POOL_SCROLLS is 0): its table entry asks for a
           noise jitter of the U coordinate instead, amplitude
           STREAM_POOL_NOISE_LEN = 0.01 UV, and that noise field is the one part
           of the entry NOT transcribed. animate_scroll with a zero delta is
           still the right call -- it is what applies WATER_DECAL_LIFT, and a
           pool needs it: measured against each track's own geometry the puddles
           are flat plates 0-23 cm above the pit floor whose RIM is coplanar with
           the sand to 0.00 cm, so without a bias the edge z-fights. */
        float alpha;
        int scrolls;
        if (b->flags & BATCH_STREAM) {
            alpha = STREAM_VERTEX_ALPHA;
            scrolls = STREAM_SCROLLS;
        } else if (b->flags & BATCH_FALL) {
            alpha = FALL_VERTEX_ALPHA;
            scrolls = FALL_SCROLLS;
        } else if (b->flags & BATCH_POOL) {
            alpha = POOL_VERTEX_ALPHA;
            scrolls = POOL_SCROLLS;
        } else {
            continue;
        }
        glColor4f(1.f, 1.f, 1.f, alpha);
        animate_scroll(w, i, 0.f, scrolls ? w->t * STREAM_SCROLL_VEL : 0.f);
        draw_batch(b);
    }
    glColor4f(1.f, 1.f, 1.f, 1.f);
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);

    /* --- blended: the foam band ------------------------------------------ */
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    glEnableClientState(GL_COLOR_ARRAY);
    for (i = 0; i < s->n_batches; i++) {
        batch_t *b = &s->batches[i];
        if (!(b->flags & BATCH_COAST) || !w->coast_rgba[i])
            continue;
        animate_coast(w, i);
        glColorPointer(4, GL_UNSIGNED_BYTE, 0, w->coast_rgba[i]);
        draw_batch(b);
    }
    glDisableClientState(GL_COLOR_ARRAY);

    /* --- blended: the breaking waves ------------------------------------- */
    if (w->wave_tex) {
        glBindTexture(GL_TEXTURE_2D, w->wave_tex);
        glDisable(GL_CULL_FACE);
        for (j = 0; j < WATER_MAX_WAVES; j++)
            if (w->waves[j].active)
                wave_draw(w, &w->waves[j], eye);
        glEnable(GL_CULL_FACE);
        glColor4f(1.f, 1.f, 1.f, 1.f);
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(0.f, 0.f);
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);
}
