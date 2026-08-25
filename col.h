/*
 * col.h -- the .col collision grid, and its binding to the transcribed physics.
 *
 * The .col file is a uniform XZ grid over the level's collision triangles (see
 * rccars_re/FORMAT_NOTES.md). It was written for downward ground queries only;
 * the transcribed physics needs sphere and segment tests as well, because
 * RC Cars models the car as a set of spheres and solves the suspension
 * geometrically. Those are here.
 */

#ifndef COL_H
#define COL_H

#include "rb.h"

typedef struct {
    float minx, minz, cell;
    unsigned int nx, nz, ntris;
    float *tris;             /* 9 floats per triangle, world space */
    unsigned int *start;     /* nx*nz + 1 prefix offsets */
    unsigned int *idx;
    /* COL2: the material under each triangle, for the surface sounds. NULL on a
       COL1 grid, and then everything reads as `default_surf`. Audio only -- see
       pack_col.py on why this is not rb_wheel_contact.surface. */
    unsigned char *surf;
    unsigned int default_surf;
    /* COL4: the ENGINE's own surface class per triangle, 0 = "no opinion".
       This is FUN_00534fc0's data, recovered -- see pack_col.eng_surface_class.
       NULL on any older grid, and then col_surface_at answers 0 everywhere.
       Separate from `surf` above on purpose: that one is a keyword guess at the
       same idea and drives AUDIO, this one is the game's own answer and drives
       the tyre marks. Still not plumbed into rb_wheel_contact.surface -- doing
       that turns on carSurfaceDrag's deep-sand branch and is a handling change,
       not a visual one. */
    unsigned char *eng_surf;
    /* COL5: how bright the LEVEL'S OWN LIGHTMAP is on each triangle, 0..254,
       COL_LIGHT_NONE where that face has no lightmap layer. This is the quantity
       FUN_004572c0 samples per contact and FUN_00531ff0 averages over the four
       wheels to darken the car -- baked per triangle here because this port's
       collision is not its render mesh. NULL on any pre-COL5 grid, and then
       col_light_at has no opinion and the car's light sits at 1.0. See
       carlight.h and pack_col.py's LIGHTMAP note. */
    unsigned char *light;
    /* COL3: the water surface height per cell, COL_NO_WATER where the cell has
       none. NULL on an older grid, and then there is no water anywhere -- which
       is exactly what this port did before the grid carried it. See pack_col.py's
       WATER note for how it is built and what its 3 m fuzz costs. */
    float *water_y;
    /* The Y bounds of each entry in `idx`, in the same order, built at load and
       not stored in the file. A cell is a column over the WHOLE height of the
       level -- beach_1's worst holds 286 triangles spanning 4.9 m of a building --
       so the vertical test rejects almost everything, and doing it out of here
       reads 8 contiguous bytes per triangle instead of chasing a 36-byte vertex
       record through `tris`. Same order as `idx`, deliberately: col_sphere keeps
       the first hit it finds per sphere, so re-sorting these would change which
       contact a wheel reports. NULL is legal and simply means "test the vertices
       directly", which is what this did before. */
    float *ref_ylo, *ref_yhi;
} col_t;

/* pack_col.py's NO_WATER sentinel. */
#define COL_NO_WATER  (-1.0e30f)

/* pack_col.py's LIGHT_NONE: this triangle's face carries no lightmap. */
#define COL_LIGHT_NONE 255

/* Query counters, for colprof.c only, and OFF unless COL_PROFILE is defined:
   the loops they would sit in are the hottest code in the sim, which is the
   whole reason colprof exists. `tris` counts triangles VISITED by a query and
   `narrow` the ones that survived the cheap rejects and reached the
   closest-point test -- their ratio is what the broad phase is worth. */
#ifdef COL_PROFILE
typedef struct {
    unsigned long sphere, segment, ground, cells, tris, narrow;
} col_prof_t;
extern col_prof_t col_prof;
#define COL_PROF(f, n)  (col_prof.f += (n))
#else
#define COL_PROF(f, n)  ((void)0)
#endif

int  col_load(const char *path, col_t *c);

/* Material of the surface under (x,z) nearest at or below y. Falls back to the
   track's default, so this always returns a usable class and never fails. */
int  col_material_at(const col_t *c, float x, float y, float z);

/* The engine's surface class under (x,z) at about y: 0 none, 1 sand, 2 wetsand,
   3 dunesand, 4 grass, 5 gravel, 6 metal, 7 wood, 8 stone. 0 on a pre-COL4
   grid. Takes the MINIMUM positive class over the faces in a band at the
   contact, which is what FUN_00534fc0 does and is what lets a decal defer to
   the floor it is laid on. */
int  col_surface_at(const col_t *c, float x, float y, float z);

/* How bright the level's lightmap is on the surface under (x, z) at about y,
   0..1, or a NEGATIVE number for "no opinion" -- no grid, no face there, or the
   face has no lightmap. Feeds carlight_step, one call per wheel that is touching
   the ground, which is where the engine's own four samples come from. */
float col_light_at(const col_t *c, float x, float y, float z);

/* Water surface height at (x, z). Returns 0 and leaves *y untouched where there
   is none. Backs rb_world.water -- which is what makes carSurfaceDrag's water
   term fire -- and main.c's drowned test. */
int  col_water_at(const col_t *c, float x, float z, float *y);

/* Release a loaded grid, leaving the struct safe to load into again. The track
   can be switched at runtime from the menu, so this has to exist. */
void col_free(col_t *c);

/* Highest surface at (x,z) not above ceil_y. The original downward query, kept
   for spawn placement and the camera. */
int  col_ground_at(const col_t *c, float x, float z, float ceil_y,
                   float *out_y, float *nx_, float *ny_, float *nz_);

/* Closest point on the collision mesh to `centre`, if within `radius`.
   Fills up to max_hits results, deepest first. Backs rb_world.sphere. */
int  col_sphere(const col_t *c, const float centre[3], float radius,
                rb_world_hit *hits, int max_hits, int *n_hits);

/* Does the segment a->b cross the mesh? Backs rb_world.segment. */
int  col_segment(const col_t *c, const float a[3], const float b[3]);

/* Triangles whose XZ footprint meets the square [x-r, x+r] x [z-r, z+r] and
   that lie within `depth` below `y`. Writes 9 floats per triangle into `out`
   and returns how many it wrote (at most max_tris).

   This is the shadow receiver set. The engine keeps a per-object face list
   (shdInit's two OFLs) and marks the faces a source touches; the port has no
   render-face list, so it uses the collision mesh instead -- the same surfaces,
   at collision resolution. */
int  col_gather(const col_t *c, float x, float y, float z, float r, float depth,
                float *out, int max_tris);

/* An rb_world bound to this grid. The pointer must outlive the world. */
const rb_world *col_rb_world(col_t *c);

#endif
