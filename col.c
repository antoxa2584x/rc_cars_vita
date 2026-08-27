/*
 * col.c -- .col grid loader and queries.
 *
 * col_ground_at is the port's original downward query, moved here unchanged.
 * col_sphere and col_segment are new: the transcribed physics models the car as
 * spheres and needs closest-point and swept tests, not just a height probe.
 *
 * Water is deliberately absent from the collision TRIANGLES -- the car fords the
 * river rather than driving on it -- but it is not absent from the file: COL3
 * carries the water surface height per cell, and cb_water answers the physics'
 * water probe out of it. That is the split the original has too: water is never
 * a solid, and the only thing it does to the dynamics is carSurfaceDrag's
 * quadratic drag.
 */

#include "col.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef COL_PROFILE
col_prof_t col_prof;
#endif

static int rd(FILE *f, void *p, size_t n)
{
    return fread(p, 1, n, f) == n;
}

int col_load(const char *path, col_t *c)
{
    FILE *f = fopen(path, "rb");
    char magic[4];
    unsigned int ncell, nref;
    int v2, v3, v4, v5;
    long fsize, hdr_bytes;

    memset(c, 0, sizeof(*c));
    if (!f)
        return 0;
    if (!rd(f, magic, 4)) { fclose(f); return 0; }
    /* COL2 adds the per-triangle material for the surface sounds, COL3 the water
       surface height per cell. Both older versions still load: a COL1 grid reports
       the default material everywhere (a duller-sounding car, not a broken one)
       and a COL1 or COL2 grid reports no water anywhere (no water drag, which is
       what the port did before the grid carried it). COL4 adds the ENGINE's own
       surface class per triangle; without it col_surface_at answers 0 and the
       tyre marks fall back to one flat strength, which is what they had. COL5
       adds how bright the level's own LIGHTMAP is on each triangle; without it
       col_light_at has no opinion anywhere, which leaves the car's light at 1.0
       -- exactly how it looked before carlight.c existed. */
    if (memcmp(magic, "COL1", 4) && memcmp(magic, "COL2", 4)
        && memcmp(magic, "COL3", 4) && memcmp(magic, "COL4", 4)
        && memcmp(magic, "COL5", 4))
        { fclose(f); return 0; }
    v5 = memcmp(magic, "COL5", 4) == 0;
    v4 = v5 || memcmp(magic, "COL4", 4) == 0;
    v3 = v4 || memcmp(magic, "COL3", 4) == 0;
    v2 = v3 || memcmp(magic, "COL2", 4) == 0;
    /* magic, minx/minz/cell, nx/nz/ntris, and COL2+'s default_surf */
    hdr_bytes = 4 + 12 + 12 + (v2 ? 4 : 0);
    if (!rd(f, &c->minx, 4) || !rd(f, &c->minz, 4) || !rd(f, &c->cell, 4)
        || !rd(f, &c->nx, 4) || !rd(f, &c->nz, 4) || !rd(f, &c->ntris, 4)
        || (v2 && !rd(f, &c->default_surf, 4)))
        { fclose(f); col_free(c); return 0; }

    /* EVERY COUNT ABOVE IS FILE DATA, AND THE ARRAYS BELOW ARE SIZED AND THEN
     * INDEXED ON IT. That is the same rule ai_init's record array needed, one
     * module over, and this is the loader it was missing from: none of the three
     * mandatory reads was checked and neither was any of their allocations, so a
     * .col whose header outran its own body reported SUCCESS and handed the
     * queries a grid of uninitialised heap. Measured, before: a 32-byte file cut
     * to just its header came back from col_load as 1, with ntris 35,677 and
     * 1,708 cells, every array allocated and not one of them read.
     *
     * Two ways that went wrong beyond the garbage. `nref` is read back out of
     * `start[]` -- so on a short read it is uninitialised, and the size of the
     * next allocation is then whatever was on the heap. And the vertical-bounds
     * loop below dereferences `tris[idx[k] * 9]` with `idx[k]` straight out of
     * the file: a .col whose last `start[]` entry says 0x40000000 references
     * SEGVs in col_load itself (deterministic, ASan, col.c's ref_ylo loop).
     *
     * So: bound each count by what the file can actually hold before allocating,
     * check every read and every allocation, and validate the grid references
     * once here rather than in the queries -- col_sphere walks `idx[]` tens of
     * thousands of times a tick and must not pay for this.
     *
     * The OPTIONAL arrays below keep their existing graceful degradation: a
     * missing material, water, engine-class or lightmap array is a duller car or
     * a drier track, not a broken one. It is the three the queries cannot run
     * without that become hard failures. */
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, hdr_bytes, SEEK_SET);
    if (fsize < hdr_bytes || c->nx == 0 || c->nz == 0 || c->ntris == 0
        || c->cell <= 0.f
        /* nx*nz must not wrap, and each array must fit in what is left */
        || c->nx > COL_MAX_DIM || c->nz > COL_MAX_DIM
        || (unsigned long long)c->ntris * 9u * sizeof(float)
           > (unsigned long long)(fsize - hdr_bytes)) {
        fclose(f);
        col_free(c);
        return 0;
    }

    c->tris = malloc((size_t)c->ntris * 9 * sizeof(float));
    if (!c->tris || !rd(f, c->tris, (size_t)c->ntris * 9 * sizeof(float)))
        { fclose(f); col_free(c); return 0; }
    ncell = c->nx * c->nz + 1;
    c->start = malloc((size_t)ncell * sizeof(unsigned int));
    if (!c->start || !rd(f, c->start, (size_t)ncell * sizeof(unsigned int)))
        { fclose(f); col_free(c); return 0; }
    /* Now that start[] has really been read, its last entry is the reference
       count -- and it still has to fit in the file. */
    nref = c->start[ncell - 1];
    if ((unsigned long long)nref * sizeof(unsigned int)
        > (unsigned long long)(fsize - ftell(f)))
        { fclose(f); col_free(c); return 0; }
    c->idx = malloc((size_t)(nref ? nref : 1) * sizeof(unsigned int));
    if (!c->idx || (nref && !rd(f, c->idx, (size_t)nref * sizeof(unsigned int))))
        { fclose(f); col_free(c); return 0; }
    /* The prefix offsets and the references they name, checked ONCE. A cell whose
       range runs backwards or past the end, or a reference naming a triangle that
       does not exist, is an out-of-bounds read in the hottest loop in the port. */
    {
        unsigned int k;
        for (k = 0; k < ncell; k++)
            if (c->start[k] > nref || (k && c->start[k] < c->start[k - 1]))
                { fclose(f); col_free(c); return 0; }
        for (k = 0; k < nref; k++)
            if (c->idx[k] >= c->ntris)
                { fclose(f); col_free(c); return 0; }
    }
    if (v2 && c->ntris) {
        c->surf = malloc(c->ntris);
        /* A truncated material array is not worth failing the whole track over:
           drop it and fall back to the default. */
        if (c->surf && !rd(f, c->surf, c->ntris)) {
            free(c->surf);
            c->surf = NULL;
        }
    }
    if (v3 && c->surf) {
        /* Only after the material array survived: it sits in front of the water
           grid, so a short read there has already left the cursor in the wrong
           place and anything read from here would be garbage rather than absent. */
        size_t n = (size_t)c->nx * c->nz;
        c->water_y = malloc(n * sizeof(float));
        if (c->water_y && !rd(f, c->water_y, n * sizeof(float))) {
            free(c->water_y);
            c->water_y = NULL;
        }
    }
    if (v4 && c->water_y) {
        /* Same discipline as the water grid above: only if everything in front
           of it read cleanly, because a short read has already moved the cursor
           and anything from here would be garbage rather than absent. */
        c->eng_surf = malloc(c->ntris);
        if (c->eng_surf && !rd(f, c->eng_surf, c->ntris)) {
            free(c->eng_surf);
            c->eng_surf = NULL;
        }
    }
    if (v5 && c->eng_surf) {
        /* And the same discipline again, one array further along. */
        c->light = malloc(c->ntris);
        if (c->light && !rd(f, c->light, c->ntris)) {
            free(c->light);
            c->light = NULL;
        }
    }
    fclose(f);

    /* The vertical bounds index -- see col_t. Two floats per grid REFERENCE, not
       per triangle, because that is what the query walks: 0.45 MB on beach_1's
       3 m grid. If the allocation fails the queries fall back to reading the
       vertices, which is correct and merely slower. */
    if (nref) {
        c->ref_ylo = malloc((size_t)nref * sizeof(float));
        c->ref_yhi = malloc((size_t)nref * sizeof(float));
        if (!c->ref_ylo || !c->ref_yhi) {
            free(c->ref_ylo); free(c->ref_yhi);
            c->ref_ylo = c->ref_yhi = NULL;
        } else {
            unsigned int k;
            for (k = 0; k < nref; k++) {
                const float *t = &c->tris[(size_t)c->idx[k] * 9];
                float lo = t[1], hi = t[1];
                if (t[4] < lo) lo = t[4]; else if (t[4] > hi) hi = t[4];
                if (t[7] < lo) lo = t[7]; else if (t[7] > hi) hi = t[7];
                c->ref_ylo[k] = lo;
                c->ref_yhi[k] = hi;
            }
        }
    }
    return 1;
}

void col_free(col_t *c)
{
    free(c->tris);
    free(c->start);
    free(c->idx);
    free(c->surf);
    free(c->water_y);
    free(c->eng_surf);
    free(c->light);
    free(c->ref_ylo);
    free(c->ref_yhi);
    memset(c, 0, sizeof(*c));
}

int col_water_at(const col_t *c, float x, float z, float *y)
{
    int cx, cz;
    float w;

    if (!c->water_y)
        return 0;
    cx = (int)((x - c->minx) / c->cell);
    cz = (int)((z - c->minz) / c->cell);
    if (cx < 0 || cz < 0 || (unsigned)cx >= c->nx || (unsigned)cz >= c->nz)
        return 0;
    w = c->water_y[(size_t)(unsigned)cz * c->nx + (unsigned)cx];
    if (w <= COL_NO_WATER)
        return 0;
    if (y)
        *y = w;
    return 1;
}

int col_material_at(const col_t *c, float x, float y, float z)
{
    int cx, cz;
    unsigned int cell, k;
    float best = -1e30f;
    int mat;

    if (!c->surf || !c->ntris) return (int)c->default_surf;
    mat = (int)c->default_surf;
    cx = (int)((x - c->minx) / c->cell);
    cz = (int)((z - c->minz) / c->cell);
    if (cx < 0 || cz < 0 || (unsigned)cx >= c->nx || (unsigned)cz >= c->nz)
        return mat;

    cell = (unsigned)cz * c->nx + (unsigned)cx;
    for (k = c->start[cell]; k < c->start[cell + 1]; k++) {
        unsigned int ti = c->idx[k];
        const float *t = &c->tris[(size_t)ti * 9];
        float ax = t[0], az = t[2];
        float bx = t[3], bz = t[5];
        float cx2 = t[6], cz2 = t[8];
        float d, w0, w1, w2, ty;

        d = (bz - cz2) * (ax - cx2) + (cx2 - bx) * (az - cz2);
        if (d > -1e-9f && d < 1e-9f) continue;
        w0 = ((bz - cz2) * (x - cx2) + (cx2 - bx) * (z - cz2)) / d;
        w1 = ((cz2 - az) * (x - cx2) + (ax - cx2) * (z - cz2)) / d;
        w2 = 1.f - w0 - w1;
        if (w0 < -0.001f || w1 < -0.001f || w2 < -0.001f) continue;
        ty = w0 * t[1] + w1 * t[4] + w2 * t[7];
        /* The query point is a contact patch, so the face it belongs to is at
           or just below it. Taking the highest face outright would pick a
           bridge deck while the car is on the road underneath. */
        if (ty <= y + 0.25f && ty > best) {
            best = ty;
            mat = c->surf[ti];
        }
    }
    return mat;
}

/* Broad phase, defined further down with the sphere/segment queries. */
static int tri_y_reach(const col_t *c, unsigned int k, float y, float r);

/* Height of a triangle's plane over (x, z), or 0 if (x, z) is outside it. */
static int tri_y_at(const float *t, float x, float z, float *out)
{
    float ax = t[0], az = t[2], bx = t[3], bz = t[5], cx = t[6], cz = t[8];
    float d, w0, w1, w2;

    d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
    if (d > -1e-9f && d < 1e-9f)
        return 0;
    w0 = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / d;
    w1 = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / d;
    w2 = 1.f - w0 - w1;
    if (w0 < -0.001f || w1 < -0.001f || w2 < -0.001f)
        return 0;
    *out = w0 * t[1] + w1 * t[4] + w2 * t[7];
    return 1;
}

int col_surface_at(const col_t *c, float x, float y, float z)
{
    int cx, cz;
    unsigned int cell, k;
    float best = -1e30f;
    int cls = 0;

    if (!c->eng_surf || !c->ntris)
        return 0;
    cx = (int)((x - c->minx) / c->cell);
    cz = (int)((z - c->minz) / c->cell);
    if (cx < 0 || cz < 0 || (unsigned)cx >= c->nx || (unsigned)cz >= c->nz)
        return 0;
    cell = (unsigned)cz * c->nx + (unsigned)cx;

    /* Two sweeps, because the engine's rule is NOT "the topmost face wins".
       FUN_00534fc0 takes the MINIMUM class over the faces at the contact,
       skipping 0 -- and 0 means "no opinion", which is exactly what a decal
       carries. `wt_wetsand` sits a centimetre above the sand it modulates, so
       picking the topmost face would have every transition strip in the game
       report 0 and mark nothing; taking the min over the band lets the sand
       underneath decide. That is the whole reason the original loops rather
       than picks.

       THE PORT'S: the 5 cm band. The engine has the contact's own face list and
       needs no geometric window at all. This one is set above every decal lift
       measured in the shipped tracks (5-21 mm, median 1 cm) and far below any
       genuine floor-over-floor gap in them. */
    for (k = c->start[cell]; k < c->start[cell + 1]; k++) {
        const float *t = &c->tris[(size_t)c->idx[k] * 9];
        float ty;
        if (tri_y_at(t, x, z, &ty) && ty <= y + 0.25f && ty > best)
            best = ty;
    }
    if (best < -1e29f)
        return 0;
    for (k = c->start[cell]; k < c->start[cell + 1]; k++) {
        unsigned int ti = c->idx[k];
        const float *t = &c->tris[(size_t)ti * 9];
        float ty;
        int e;
        /* Y-first reject, the same one the sphere and segment queries use, and
           for the same reason: a cell is a 3 m column over the WHOLE height of
           the level, so most of what is in it is a roof or a seabed. Valid only
           on THIS sweep -- `best` is by construction the highest face at or
           below y + 0.25, so the band is [best - 0.05, best] and a triangle
           whose Y extent misses it cannot land in it. The first sweep has no
           lower bound at all and so cannot be rejected this way, which is why
           col_material_at above has no reject either. */
        if (!tri_y_reach(c, k, best - 0.025f, 0.035f))
            continue;
        if (!tri_y_at(t, x, z, &ty))
            continue;
        if (ty > y + 0.25f || ty < best - 0.05f)
            continue;
        e = c->eng_surf[ti];
        if (e > 0 && (cls == 0 || e < cls))
            cls = e;
    }
    return cls;
}

float col_light_at(const col_t *c, float x, float y, float z)
{
    int cx, cz;
    unsigned int cell, k;
    float best = -1e30f, top;
    float lit = -1.f;

    if (!c->light || !c->ntris)
        return -1.f;
    cx = (int)((x - c->minx) / c->cell);
    cz = (int)((z - c->minz) / c->cell);
    if (cx < 0 || cz < 0 || (unsigned)cx >= c->nx || (unsigned)cz >= c->nz)
        return -1.f;
    cell = (unsigned)cz * c->nx + (unsigned)cx;

    /* THE TOPMOST FACE THAT HAS AN OPINION, which is not the rule col_surface_at
       above uses and the difference is the point. The engine samples the ONE face
       the contact is on, so what this has to answer is "the face you can see" --
       the highest one at or below the query. Where that face carries no lightmap
       (a decal: the sand transitions and the checkpoint markings are 21% of
       beach_1's triangles and none of them is lit) the search continues DOWN
       through the 5 cm band to the surface underneath, which is the face whose
       lighting the decal is painted on top of. Taking the minimum over the band
       the way the surface class does would let one unlit sliver hold the car
       bright over a shadowed road. */
    for (k = c->start[cell]; k < c->start[cell + 1]; k++) {
        const float *t = &c->tris[(size_t)c->idx[k] * 9];
        float ty;
        if (tri_y_at(t, x, z, &ty) && ty <= y + 0.25f && ty > best)
            best = ty;
    }
    if (best < -1e29f)
        return -1.f;
    top = -1e30f;
    for (k = c->start[cell]; k < c->start[cell + 1]; k++) {
        unsigned int ti = c->idx[k];
        const float *t = &c->tris[(size_t)ti * 9];
        float ty;
        /* The same Y-first reject and the same band as col_surface_at, valid for
           the same reason: `best' is the highest face at or below y + 0.25. */
        if (!tri_y_reach(c, k, best - 0.025f, 0.035f))
            continue;
        if (!tri_y_at(t, x, z, &ty))
            continue;
        if (ty > y + 0.25f || ty < best - 0.05f)
            continue;
        if (c->light[ti] == COL_LIGHT_NONE)
            continue;
        if (ty > top) {
            top = ty;
            lit = (float)c->light[ti] * (1.f / 255.f);
        }
    }
    return lit;
}

int col_ground_at(const col_t *c, float x, float z, float ceil_y,
                  float *out_y, float *nx_, float *ny_, float *nz_)
{
    int cx, cz;
    unsigned int cell, k;
    float best = -1e30f;
    int found = 0;

    if (!c->ntris) return 0;
    cx = (int)((x - c->minx) / c->cell);
    cz = (int)((z - c->minz) / c->cell);
    if (cx < 0 || cz < 0 || (unsigned)cx >= c->nx || (unsigned)cz >= c->nz)
        return 0;

    cell = (unsigned)cz * c->nx + (unsigned)cx;
    COL_PROF(ground, 1);
    COL_PROF(cells, 1);
    COL_PROF(tris, c->start[cell + 1] - c->start[cell]);
    for (k = c->start[cell]; k < c->start[cell + 1]; k++) {
        const float *t;
        float ax, ay, az, bx, by, bz, cx2, cy2, cz2;
        float d, w0, w1, w2, y, ylo, yhi;

        /* Two exact rejects before the barycentric solve, for the same reason
           col_sphere has a broad phase: a cell is a column over the whole level.
           A triangle entirely above the ceiling cannot be reported, and one
           entirely below the best found so far cannot beat it -- the interpolated
           y is inside [ylo, yhi] either way. This is where the shadow, the water
           probe and the prop placement spend their time.

           Out of ref_ylo/ref_yhi when they were built, so a rejected triangle is
           never touched in `tris` at all. */
        if (c->ref_ylo) {
            if (c->ref_ylo[k] > ceil_y || c->ref_yhi[k] <= best)
                continue;
        }
        t = &c->tris[(size_t)c->idx[k] * 9];
        ax = t[0]; ay = t[1]; az = t[2];
        bx = t[3]; by = t[4]; bz = t[5];
        cx2 = t[6]; cy2 = t[7]; cz2 = t[8];
        if (!c->ref_ylo) {
            ylo = yhi = ay;
            if (by < ylo) ylo = by; else if (by > yhi) yhi = by;
            if (cy2 < ylo) ylo = cy2; else if (cy2 > yhi) yhi = cy2;
            if (ylo > ceil_y || yhi <= best)
                continue;
        }

        d = (bz - cz2) * (ax - cx2) + (cx2 - bx) * (az - cz2);
        if (d > -1e-9f && d < 1e-9f) continue;
        w0 = ((bz - cz2) * (x - cx2) + (cx2 - bx) * (z - cz2)) / d;
        w1 = ((cz2 - az) * (x - cx2) + (ax - cx2) * (z - cz2)) / d;
        w2 = 1.f - w0 - w1;
        if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;

        y = w0 * ay + w1 * by + w2 * cy2;
        if (y <= ceil_y && y > best) {
            best = y;
            found = 1;
            if (nx_) {
                float ux = bx - ax, uy = by - ay, uz = bz - az;
                float vx = cx2 - ax, vy = cy2 - ay, vz = cz2 - az;
                float n0 = uy * vz - uz * vy;
                float n1 = uz * vx - ux * vz;
                float n2 = ux * vy - uy * vx;
                float len = sqrtf(n0 * n0 + n1 * n1 + n2 * n2);
                if (len > 1e-6f) {
                    if (n1 < 0.f) { n0 = -n0; n1 = -n1; n2 = -n2; }
                    *nx_ = n0 / len; *ny_ = n1 / len; *nz_ = n2 / len;
                }
            }
        }
    }
    if (found) *out_y = best;
    return found;
}

/* ------------------------------------------------------------------------- */
/* closest point on a triangle                                               */
/* ------------------------------------------------------------------------- */

/* The triangle's outward normal, straight off its winding.
 *
 * The winding IS meaningful in these grids -- measured over beach_1, 99.4% of
 * the faces you see first looking straight down have a +Y normal, and the
 * undersides of slabs and roofs correctly carry -Y. col_ground_at flips its
 * normal up unconditionally, which is right for a query that only ever reports
 * something to stand ON; a sphere test also touches ceilings, so it must not.
 *
 * Zero-length (a degenerate triangle) is left as all-zero, which is the "cannot
 * say" value rb_world_hit documents. */
static void tri_normal(const float *t, float out[3])
{
    float u[3], v[3], n[3], len;
    int k;

    for (k = 0; k < 3; k++) {
        u[k] = t[3 + k] - t[k];
        v[k] = t[6 + k] - t[k];
    }
    n[0] = u[1]*v[2] - u[2]*v[1];
    n[1] = u[2]*v[0] - u[0]*v[2];
    n[2] = u[0]*v[1] - u[1]*v[0];
    len = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
    if (len < 1e-12f) {
        out[0] = out[1] = out[2] = 0.f;
        return;
    }
    for (k = 0; k < 3; k++)
        out[k] = n[k] / len;
}

/* Can a sphere of `r` about `p` reach this triangle at all?
 *
 * THE BROAD PHASE, and it is the difference between a playable frame and a 111 ms
 * one. A cell is a 3 m column over the WHOLE height of the level, so the pier's
 * deck, its piles, its roof and the terrain underneath are all in the same cell,
 * and col_sphere used to run the full closest-point test on every one of them for
 * a 0.07 m wheel: measured on the hardware log's own worst position, 62,318
 * triangles visited per 1/60 tick and 62,318 closest-point tests done. The car
 * issues ~110 sphere queries a tick (the suspension solve alone can spend ten per
 * wheel), and a 4-tick catch-up frame multiplies that by four -- which is exactly
 * the 111 ms `sim` in the log, against a `draw` that never moved off 5.7 ms.
 *
 * This is an EXACT reject, not an approximation: if the triangle's own AABB does
 * not meet the sphere's, no point of the triangle is within r, so tri_closest
 * cannot have produced a hit. Y is tested first because vertical stacking is where
 * the geometry actually is -- a column holds a building, not a fan of floors.
 *
 * The triangle's 9 floats are read either way, so the bounds cost no extra memory
 * traffic; col_gather has always done the same thing for the shadow receivers. */
static int tri_y_reach(const col_t *c, unsigned int k, float y, float r)
{
    float lo, hi;

    if (c->ref_ylo) {
        lo = c->ref_ylo[k];
        hi = c->ref_yhi[k];
    } else {
        const float *t = &c->tris[(size_t)c->idx[k] * 9];
        lo = hi = t[1];
        if (t[4] < lo) lo = t[4]; else if (t[4] > hi) hi = t[4];
        if (t[7] < lo) lo = t[7]; else if (t[7] > hi) hi = t[7];
    }
    return !(hi < y - r || lo > y + r);
}

static int tri_reach_xz(const float *t, const float p[3], float r)
{
    float lo, hi;

    lo = hi = t[0];
    if (t[3] < lo) lo = t[3]; else if (t[3] > hi) hi = t[3];
    if (t[6] < lo) lo = t[6]; else if (t[6] > hi) hi = t[6];
    if (hi < p[0] - r || lo > p[0] + r) return 0;

    lo = hi = t[2];
    if (t[5] < lo) lo = t[5]; else if (t[5] > hi) hi = t[5];
    if (t[8] < lo) lo = t[8]; else if (t[8] > hi) hi = t[8];
    if (hi < p[2] - r || lo > p[2] + r) return 0;

    return 1;
}

static void tri_closest(const float *t, const float p[3], float out[3])
{
    const float *a = t, *b = t + 3, *c = t + 6;
    float ab[3], ac[3], ap[3], bp[3], cp[3];
    float d1, d2, d3, d4, d5, d6, va, vb, vc, denom, v, w;
    int k;

    for (k = 0; k < 3; k++) {
        ab[k] = b[k] - a[k];
        ac[k] = c[k] - a[k];
        ap[k] = p[k] - a[k];
    }
    d1 = ab[0]*ap[0] + ab[1]*ap[1] + ab[2]*ap[2];
    d2 = ac[0]*ap[0] + ac[1]*ap[1] + ac[2]*ap[2];
    if (d1 <= 0.f && d2 <= 0.f) { for (k=0;k<3;k++) out[k]=a[k]; return; }

    for (k = 0; k < 3; k++) bp[k] = p[k] - b[k];
    d3 = ab[0]*bp[0] + ab[1]*bp[1] + ab[2]*bp[2];
    d4 = ac[0]*bp[0] + ac[1]*bp[1] + ac[2]*bp[2];
    if (d3 >= 0.f && d4 <= d3) { for (k=0;k<3;k++) out[k]=b[k]; return; }

    vc = d1*d4 - d3*d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) {
        v = (d1 - d3 != 0.f) ? d1 / (d1 - d3) : 0.f;
        for (k=0;k<3;k++) out[k] = a[k] + v * ab[k];
        return;
    }

    for (k = 0; k < 3; k++) cp[k] = p[k] - c[k];
    d5 = ab[0]*cp[0] + ab[1]*cp[1] + ab[2]*cp[2];
    d6 = ac[0]*cp[0] + ac[1]*cp[1] + ac[2]*cp[2];
    if (d6 >= 0.f && d5 <= d6) { for (k=0;k<3;k++) out[k]=c[k]; return; }

    vb = d5*d2 - d1*d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f) {
        w = (d2 - d6 != 0.f) ? d2 / (d2 - d6) : 0.f;
        for (k=0;k<3;k++) out[k] = a[k] + w * ac[k];
        return;
    }

    va = d3*d6 - d5*d4;
    if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f) {
        float dd = (d4 - d3) + (d5 - d6);
        w = (dd != 0.f) ? (d4 - d3) / dd : 0.f;
        for (k=0;k<3;k++) out[k] = b[k] + w * (c[k] - b[k]);
        return;
    }

    denom = va + vb + vc;
    if (denom == 0.f) { for (k=0;k<3;k++) out[k]=a[k]; return; }
    v = vb / denom;
    w = vc / denom;
    for (k = 0; k < 3; k++) out[k] = a[k] + ab[k] * v + ac[k] * w;
}

int col_sphere(const col_t *c, const float centre[3], float radius,
               rb_world_hit *hits, int max_hits, int *n_hits)
{
    int cx0, cz0, cx1, cz1, ix, iz;
    float best_d2[8];
    int n = 0, i, j;

    if (n_hits) *n_hits = 0;
    if (!c->ntris || max_hits <= 0 || !hits)
        return 0;
    if (max_hits > 8) max_hits = 8;
    COL_PROF(sphere, 1);

    cx0 = (int)((centre[0] - radius - c->minx) / c->cell);
    cx1 = (int)((centre[0] + radius - c->minx) / c->cell);
    cz0 = (int)((centre[2] - radius - c->minz) / c->cell);
    cz1 = (int)((centre[2] + radius - c->minz) / c->cell);
    if (cx0 < 0) cx0 = 0;
    if (cz0 < 0) cz0 = 0;
    if ((unsigned)cx1 >= c->nx) cx1 = (int)c->nx - 1;
    if ((unsigned)cz1 >= c->nz) cz1 = (int)c->nz - 1;

    for (iz = cz0; iz <= cz1; iz++) {
        for (ix = cx0; ix <= cx1; ix++) {
            unsigned int cell = (unsigned)iz * c->nx + (unsigned)ix;
            unsigned int k;
            COL_PROF(cells, 1);
            COL_PROF(tris, c->start[cell + 1] - c->start[cell]);
            for (k = c->start[cell]; k < c->start[cell + 1]; k++) {
                const float *t;
                float q[3], dx, dy, dz, d2;

                /* Vertical first, and out of the contiguous bounds index: that is
                   the test that rejects a whole building for a 0.07 m wheel, and
                   doing it here means `tris` is never touched for a triangle that
                   cannot be reached. */
                if (!tri_y_reach(c, k, centre[1], radius))
                    continue;
                t = &c->tris[(size_t)c->idx[k] * 9];
                if (!tri_reach_xz(t, centre, radius))
                    continue;
                COL_PROF(narrow, 1);
                tri_closest(t, centre, q);
                dx = centre[0] - q[0];
                dy = centre[1] - q[1];
                dz = centre[2] - q[2];
                d2 = dx*dx + dy*dy + dz*dz;
                if (d2 > radius * radius)
                    continue;

                /* keep the max_hits deepest overlaps, nearest first */
                if (n < max_hits) {
                    i = n++;
                } else {
                    int worst = 0;
                    for (j = 1; j < n; j++)
                        if (best_d2[j] > best_d2[worst]) worst = j;
                    if (best_d2[worst] <= d2)
                        continue;
                    i = worst;
                }
                best_d2[i] = d2;
                hits[i].point[0] = q[0];
                hits[i].point[1] = q[1];
                hits[i].point[2] = q[2];
                hits[i].surface = 0;
                tri_normal(t, hits[i].normal);
            }
        }
    }
    if (n_hits) *n_hits = n;
    return n > 0;
}

/* Moller-Trumbore, segment form. */
static int seg_tri(const float *t, const float a[3], const float d[3])
{
    const float *v0 = t, *v1 = t + 3, *v2 = t + 6;
    float e1[3], e2[3], pv[3], qv[3], tv[3];
    float det, inv, u, v, s;
    int k;

    for (k = 0; k < 3; k++) { e1[k] = v1[k] - v0[k]; e2[k] = v2[k] - v0[k]; }
    pv[0] = d[1]*e2[2] - d[2]*e2[1];
    pv[1] = d[2]*e2[0] - d[0]*e2[2];
    pv[2] = d[0]*e2[1] - d[1]*e2[0];
    det = e1[0]*pv[0] + e1[1]*pv[1] + e1[2]*pv[2];
    if (det > -1e-9f && det < 1e-9f) return 0;
    inv = 1.f / det;
    for (k = 0; k < 3; k++) tv[k] = a[k] - v0[k];
    u = (tv[0]*pv[0] + tv[1]*pv[1] + tv[2]*pv[2]) * inv;
    if (u < 0.f || u > 1.f) return 0;
    qv[0] = tv[1]*e1[2] - tv[2]*e1[1];
    qv[1] = tv[2]*e1[0] - tv[0]*e1[2];
    qv[2] = tv[0]*e1[1] - tv[1]*e1[0];
    v = (d[0]*qv[0] + d[1]*qv[1] + d[2]*qv[2]) * inv;
    if (v < 0.f || u + v > 1.f) return 0;
    s = (e2[0]*qv[0] + e2[1]*qv[1] + e2[2]*qv[2]) * inv;
    return (s >= 0.f && s <= 1.f);
}

int col_segment(const col_t *c, const float a[3], const float b[3])
{
    float d[3], mid[3], half[3], hr;
    int cx0, cz0, cx1, cz1, ix, iz, k;

    if (!c->ntris) return 0;
    COL_PROF(segment, 1);
    for (k = 0; k < 3; k++) d[k] = b[k] - a[k];

    /* The same broad phase as col_sphere, over the segment's own box: the
       suspension's tunnel test is a few centimetres long (rb_car_update_suspension
       runs one per wheel per pass) and the cell it lands in is a 3 m column of the
       whole level. Y goes in exactly, since that is the test that does the work;
       X and Z share the largest half-extent, which is looser than the true box on
       a diagonal segment but still only ever rejects what cannot be hit. */
    for (k = 0; k < 3; k++) {
        mid[k] = (a[k] + b[k]) * 0.5f;
        half[k] = d[k] < 0.f ? -d[k] * 0.5f : d[k] * 0.5f;
    }
    hr = half[0] > half[1] ? half[0] : half[1];
    if (half[2] > hr) hr = half[2];

    cx0 = (int)(((a[0] < b[0] ? a[0] : b[0]) - c->minx) / c->cell);
    cx1 = (int)(((a[0] > b[0] ? a[0] : b[0]) - c->minx) / c->cell);
    cz0 = (int)(((a[2] < b[2] ? a[2] : b[2]) - c->minz) / c->cell);
    cz1 = (int)(((a[2] > b[2] ? a[2] : b[2]) - c->minz) / c->cell);
    if (cx0 < 0) cx0 = 0;
    if (cz0 < 0) cz0 = 0;
    if ((unsigned)cx1 >= c->nx) cx1 = (int)c->nx - 1;
    if ((unsigned)cz1 >= c->nz) cz1 = (int)c->nz - 1;

    for (iz = cz0; iz <= cz1; iz++) {
        for (ix = cx0; ix <= cx1; ix++) {
            unsigned int cell = (unsigned)iz * c->nx + (unsigned)ix;
            unsigned int j;
            COL_PROF(cells, 1);
            COL_PROF(tris, c->start[cell + 1] - c->start[cell]);
            for (j = c->start[cell]; j < c->start[cell + 1]; j++) {
                const float *t;
                if (!tri_y_reach(c, j, mid[1], half[1]))
                    continue;
                t = &c->tris[(size_t)c->idx[j] * 9];
                if (!tri_reach_xz(t, mid, hr))
                    continue;
                COL_PROF(narrow, 1);
                if (seg_tri(t, a, d))
                    return 1;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* rb_world binding                                                          */
/* ------------------------------------------------------------------------- */

static int cb_sphere(void *ctx, const float centre[3], float radius,
                     rb_world_hit *hits, int max_hits, int *n_hits)
{
    return col_sphere((const col_t *)ctx, centre, radius, hits, max_hits, n_hits);
}

static int cb_segment(void *ctx, const float a[3], const float b[3])
{
    return col_segment((const col_t *)ctx, a, b);
}

/* rb_world.water, standing in for the engine's probe at 0x00531b10.
 *
 * The original reads a per-wheel record that the WaterLOD module fills as it
 * animates the surface; here the answer comes straight out of the .col grid's
 * water heights, at the wheel's own sphere centre. `gap` is what carSurfaceDrag
 * wants: how far the wheel CENTRE is above the surface, so the submerged depth is
 * `radius - gap` and a fully drowned wheel reports a negative gap.
 *
 * The surface used is the still height the level was authored at, not the animated
 * one. water.c's displacement is the port's own curve over the game's constants
 * (see water.h) and it is centimetres on a car with a 0.07 m wheel, so feeding it
 * in would modulate the drag with a wave the engine never had. */
static int cb_water(void *ctx, int wheel, const float centre[3], float *gap)
{
    const col_t *c = (const col_t *)ctx;
    float wy;

    (void)wheel;
    if (!col_water_at(c, centre[0], centre[2], &wy))
        return 0;
    if (gap)
        *gap = (float)((double)centre[1] - wy);
    return 1;
}

int col_gather(const col_t *c, float x, float y, float z, float r, float depth,
               float *out, int max_tris)
{
    int cx0, cz0, cx1, cz1, ix, iz, n = 0;

    if (!c->ntris || max_tris <= 0)
        return 0;

    cx0 = (int)((x - r - c->minx) / c->cell);
    cx1 = (int)((x + r - c->minx) / c->cell);
    cz0 = (int)((z - r - c->minz) / c->cell);
    cz1 = (int)((z + r - c->minz) / c->cell);
    if (cx0 < 0) cx0 = 0;
    if (cz0 < 0) cz0 = 0;
    if ((unsigned)cx1 >= c->nx) cx1 = (int)c->nx - 1;
    if ((unsigned)cz1 >= c->nz) cz1 = (int)c->nz - 1;

    for (iz = cz0; iz <= cz1; iz++) {
        for (ix = cx0; ix <= cx1; ix++) {
            unsigned int cell = (unsigned)iz * c->nx + (unsigned)ix;
            unsigned int k;
            for (k = c->start[cell]; k < c->start[cell + 1]; k++) {
                const float *t = &c->tris[(size_t)c->idx[k] * 9];
                float lox, hix, loz, hiz, loy, hiy;
                int j, dup = 0;

                /* A triangle spans several cells, so the same one comes back
                   more than once over the 2..9 cells this square covers.
                   Drawing it twice doubles the shadow's alpha there, which
                   shows up as a dark cross under the car. */
                for (j = 0; j < n; j++) {
                    if (out[j * 9] == t[0] && out[j * 9 + 1] == t[1]
                        && out[j * 9 + 2] == t[2]) { dup = 1; break; }
                }
                if (dup)
                    continue;

                lox = hix = t[0]; loy = hiy = t[1]; loz = hiz = t[2];
                for (j = 1; j < 3; j++) {
                    if (t[j*3+0] < lox) lox = t[j*3+0];
                    if (t[j*3+0] > hix) hix = t[j*3+0];
                    if (t[j*3+1] < loy) loy = t[j*3+1];
                    if (t[j*3+1] > hiy) hiy = t[j*3+1];
                    if (t[j*3+2] < loz) loz = t[j*3+2];
                    if (t[j*3+2] > hiz) hiz = t[j*3+2];
                }
                if (hix < x - r || lox > x + r || hiz < z - r || loz > z + r)
                    continue;
                /* Only surfaces UNDER the car: without this the shadow lands on
                   the underside of the pier the car is driving on top of. */
                if (loy > y || hiy < y - depth)
                    continue;

                memcpy(out + n * 9, t, 9 * sizeof(float));
                if (++n >= max_tris)
                    return n;
            }
        }
    }
    return n;
}

static int cb_ground(void *ctx, float x, float z, float ceil_y,
                     float *y, float n[3])
{
    n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f;
    return col_ground_at((const col_t *)ctx, x, z, ceil_y, y,
                         &n[0], &n[1], &n[2]);
}

static rb_world g_world;

const rb_world *col_rb_world(col_t *c)
{
    g_world.sphere  = cb_sphere;
    g_world.segment = cb_segment;
    g_world.water   = cb_water;
    g_world.ground  = cb_ground;
    g_world.ctx     = c;
    return &g_world;
}
