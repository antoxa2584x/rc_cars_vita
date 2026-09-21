/*
 * water.h -- animated water: the sea surface, the shoreline foam, the stream,
 * the waterfall, and the breaking-wave sprites.
 *
 * The game's water is the WaterLOD module (wlodInit and friends, 0x00521540
 * for its config, 0x00525000-0x0052a600 for the rest). Its parts map cleanly
 * onto a flattened scene:
 *
 *   surface   the `sea`-textured LOD tiles, displaced and UV-scrolled
 *   coast     the foam band, scrolled, with alpha driven by the water height
 *             (WaterLOD_Coast's HeightOn / HeightOff are exactly that: the
 *             surface heights at which the foam is fully on and fully off)
 *   stream    scrolled at WaterLOD_Stream's ScrollVel
 *   fall      scrolled downward, and the only one of the family with a sound
 *   pool      a standing puddle: no scroll, a noise jitter instead. country_1's
 *             seven ponds and beach_4's two, and the port had no such kind at
 *             all until they were reported showing as opaque teal plates
 *   waves     sprites spawned at the water_wave_N markers, one on a TimeLong
 *             timer and one on a TimeShort timer, exactly as FUN_00525700
 *             steps its two per-marker counters
 *
 * stream, fall and pool are ONE scanner and one three-entry table in the exe
 * (0x575710, walked by 0x523760); the coast is a second scanner of the same
 * shape (0x51c950). Both test the node name with strstr -- a SUBSTRING -- which
 * is what pack_vsc.py's SURFACE_NEEDLES now mirrors.
 *
 * What is transcribed and what is not:
 *
 *   TRANSCRIBED  every constant (vis_data.h cites the loader each came from),
 *                the two-timer wave spawner, the wave sprite's geometry and
 *                life envelope (FUN_0052a030 / FUN_00529c60), the coast's
 *                height-driven alpha, and -- since the sea was found to be
 *                displaced by a tenth of what the engine does it by -- the
 *                surface displacement FUNCTION itself, FUN_005240c0. The engine
 *                tessellates its own grid over the water_box volume and the
 *                port has authored tiles, so the port folds FUN_0051c000's
 *                per-vertex precompute back into the evaluator; the arithmetic
 *                is the same. See vis_data.h's WSURF block for the formula and
 *                for the four config conversions that are not raw*0.01.
 *   NOT          the per-vertex UV orbit's DRAW. FUN_005240c0 reads a radius
 *                and a rate out of each vertex's own record; texRadMin/Max and
 *                texSpeedMin/Max being pairs is what says the record was filled
 *                by drawing from the range, but the draw itself is at
 *                tessellation time and is not transcribed. water.c hashes the
 *                vertex's own POSITION, through the weld map below, which needs
 *                no storage and survives a reload. It used to hash the vertex
 *                INDEX, and that is one half of the seam this port had: the
 *                engine's grid shares its vertices and the authored tiles
 *                duplicate theirs, so the two sides of every tile edge drew
 *                their own independent shimmer and the texture tore.
 *   NOT          the pool's UV noise. Its table entry sets the noise flag at
 *                +0x24 and clears the scroll flag at +0x20, so a pool shimmers
 *                in place rather than flowing: 0x523555 jitters each U by
 *                noise * STREAM_POOL_NOISE_LEN (0.01 UV) off a 256-entry noise
 *                object. The amplitude is recovered, the noise function is not,
 *                so water_draw holds a pool still and lifts it as a decal.
 *   TRANSCRIBED  the shallow-water damping, which used to be in this list as a
 *                guess. It is magnetOffset / magnetRadius, and the reason it
 *                read as unrecoverable is that neither converts as raw*0.01:
 *                magnetOffset is raw*0.01 - 0.5, which makes the shipped raw 50
 *                exactly ZERO on every track, and magnetRadius is raw*0.05,
 *                which makes the shipped raw 51 into 2.55 m. So the swell fades
 *                out linearly over the last 2.55 m of DEPTH and the surface is
 *                not lifted at the shore at all. The port had guessed 0.5 m and
 *                a smoothstep. clampYImmediate / clampYScale are still not read
 *                out; they are zero on every track that has a sea.
 *
 *                Depth, and not distance to the `coast` meshes. Four of the
 *                six tracks with any water at all ship no coast mesh --
 *                country_1 (pools only), and beach_1's stream and country_3's
 *                stream and falls all run inland -- and the water surface ships
 *                as one merged batch whose per-tile vertices are not shared, so
 *                its own mesh boundary is every tile edge, not the shoreline.
 *
 *                (The count used to read "five of the ten tracks have water and
 *                no coast mesh". That was measured through the packer's old
 *                `^coast\d*$` rule, which missed beach_2's `LM0_NOSHDW_coast1`
 *                -- the only coast mesh on that track. Every track with a SEA
 *                surface does in fact have a coast band.)
 */

#ifndef WATER_H
#define WATER_H

#include "col.h"
#include "scene.h"
#include "vis_data.h"

/* There is no swell-amplitude constant here any more. The height is
   WSURF[track].amp and .amp2 out of vis_data.h, straight from the track's own
   config section, and it runs from 0.42 m peak to peak on beach_4 to 1.12 m on
   beach_2 -- not the 0.02 m that used to live here.

   That 0.02 m was chosen because the first build's 0.41 m "read as a storm next
   to a 0.42 m car". It did, but the fault was never the height: the same build
   had the wavelength at 1.24 m, because it read `period` as a time when the
   engine uses it as a spatial frequency. 0.41 m of swell on a 1.24 m wave is
   60% steepness, which no water can hold. On the engine's own 10.5 m it is 2%.
   Steepness is what the packer prints for each track now, and it is the number
   that says whether the units have been read right -- a height on its own says
   nothing at all. */

/* THE HORIZON RING. Sized against the frustum, not against a track: the far
   plane is 4000 m (main.c's perspective call), the widest sky dome spans about
   220 m and is drawn camera-locked, and the authored sea reaches at most ~230 m
   from the map centre. An inner radius past the map and an outer one short of
   the far plane puts the ring entirely in the band the tiles cannot reach.
   32 segments is an 11.25-degree facet, which at these radii is well under a
   pixel of silhouette error. */
#define WATER_HORIZON_SEGMENTS 32
#define WATER_HORIZON_MID  200.0f
#define WATER_HORIZON_OUT 3000.0f
/* World metres to UV. The sea texture is a few metres of water, so this is a
   tile every 8 m -- the same order as the authored tiles carry. */
#define WATER_HORIZON_UV  0.125f
/* How many of the surface's outermost vertices the plane's height is averaged
   over. The swell is what is being averaged away -- a sample of one carries the
   whole wave -- and the radius band widens until this many are in it. */
#define WATER_HORIZON_RIM_MIN 64u

/* How close two sea vertices have to be to be the SAME point. The shipped
   tiles duplicate their shared edges and the duplicates agree only to about a
   millimetre, so this is sized an order of magnitude above that disagreement
   and three below the coarsest grid step on any track. */
/* The ceiling on either seam grid's bucket table, in entries (1 MB of int).
   The cell size is derived from the sea's extent and this, in double, because
   the naive 4 mm cell over a 200 m sea is billions of buckets and the product
   does not fit in a 32-bit long -- which the Vita has and the host does not. */
#define WATER_GRID_MAX_CELLS 262144u

#define WATER_WELD_TOL 0.004f

/* How far off an edge a vertex may sit and still count as lying ON it. Same
   reasoning; the T-junction vertices in the shipped meshes are exact to well
   inside this. */
#define WATER_STITCH_TOL 0.004f

/* How far the chord may be from the surface before closing the crack costs
   more than it buys. Above this the vertex is left where the swell puts it and
   the hairline stays open -- see build_stitch for why the geometric fix is not
   available here. 5 cm is a tenth of the smallest track's swell and about a
   pixel at the distance these edges are seen from. The worst case is measured
   over a phase sweep long enough to cover a full period of both wave trains on
   every track (the slowest is beach_1's 3.5 s). */
#define WATER_STITCH_MAX_SAG 0.05f
#define WATER_STITCH_SWEEP_T 8.0f
#define WATER_STITCH_SWEEP_N 24

/* One T-junction: vertex `v` sits at fraction `t` along the edge a..b of a
   neighbouring tile, so after the swell has moved a and b its height has to be
   put back on the chord or the surface splits along that edge. */
typedef struct {
    unsigned short v, a, b;
    float t;
} wstitch_t;

/* Depth over which the surface ramps from alphaMin to alphaMax.
 *
 * NOT recovered -- the engine's own controls are alphaRadius, alphaPow and
 * distAlphaClampMin/Max, and only alphaPow's conversion is known. It is separate
 * from WATER_DEPTH_FADE on purpose: that one is about the swell reaching the
 * seabed and is measured in centimetres, this one is about seeing THROUGH the
 * water and has to span the whole shelf.
 *
 * Sized against the map: beach_1's sea bottoms out at 2.22 m, so at 6 m the
 * deepest water reaches about 60% opacity and the shallows stay near alphaMin.
 * The first version reused WATER_DEPTH_FADE's 0.5 m, which put everything past
 * knee depth at fully opaque -- a solid teal slab with a translucent hairline at
 * the edge, which is not what water looks like. */
#define WATER_ALPHA_DEPTH 6.0f

/* The exponent on that ramp. ALSO the port's, and it used to be spelled
   WSURF_ALPHA_POW as though it were the game's. The engine does ship an
   `alphaPow` and its conversion is now read out -- raw*0.2 - 10, stored as
   -1/that, so the shipped raw 60 is -0.5 -- but -0.5 is not an exponent this
   ramp could use, and the code that consumes it is not transcribed. The 0.6
   here is the number the port has always used; only its NAME was wrong. */
#define WATER_ALPHA_POW 0.6f

/* The stream, the waterfall and the pools are translucent too, at a flat value
   each -- RECOVERED, and they do not agree: STREAM_VERTEX_ALPHA 120/255,
   FALL_VERTEX_ALPHA 140/255, POOL_VERTEX_ALPHA 110/255, in vis_data.h. They come
   off +0x34 of the engine's own node table, which 0x523866 writes into every
   vertex colour of the matched surface. Flat rather than depth-ramped like the
   sea, which is also what the engine does and suits surfaces this shallow
   (beach_1's stream runs 2 to 13 cm over its bed).

   This used to be one invented 0.55 for the whole family. That is the
   WATERFALL'S value -- and the pools never saw it, because `pool` was not in the
   packer's name rule at all. */

/* --- the foam band is a DECAL, and the art puts it EXACTLY on the sand -----
 *
 * Measured against each track's own collision grid: 89% of beach_1's coast
 * vertices sit within 1 cm of the terrain under them, with a median difference
 * of 0.0000 m. beach_3 is 69%, beach_4 85%, country_3 87%; the stream is 26-30%
 * on the two tracks that have one. The level artists laid these bands on the
 * terrain and let D3D8's ZBIAS separate them.
 *
 * Drawn with no bias at all, two coplanar surfaces z-fight over the whole band
 * and it strobes per pixel as the camera moves. That is what "the white texture
 * flickers a lot" was, and it is geometry, not the foam's alpha.
 *
 * The lift is the port's. 2 cm is 5% of the car's own length -- invisible at
 * any angle you can see a shoreline from -- and enough depth separation out to
 * roughly the 100 m the game's own COAST_ACTIVE_RAD would have culled at.
 * water_draw ALSO asks for a polygon offset, which keeps scaling with the depth
 * slope where a fixed world-space lift runs out; the lift is the half that
 * works whatever the driver does with GXM's bias units. */
#define WATER_DECAL_LIFT 0.02f

/* The depth bias that goes with it. vitaGL casts both straight to int on the
   way to sceGxmSetFrontDepthBias, so fractional values would quietly become
   zero -- these have to stay whole numbers. Negative is toward the camera. The
   slope term is what earns its place: it is the far end of a shoreline seen at
   a grazing angle where a fixed 2 cm has stopped separating anything. */
#define WATER_DECAL_OFFSET_FACTOR (-1.f)
#define WATER_DECAL_OFFSET_UNITS  (-16.f)

/* --- the foam's on/off signal ----------------------------------------------
 *
 * There is no stretch factor here any more either. It existed because
 * shore_height read the two wave trains at 1.24 m and 1.61 m -- ripples, on a
 * coast band whose median triangle edge is 1.77 m, which is below Nyquist -- and
 * dividing the phase by 12 was the only thing keeping the band from strobing.
 * The wavelengths were the units bug; the engine's own are 10.5 m and 10.1 m and
 * the band samples them at about six vertices a wavelength with nothing added.
 *
 * And the signal is not a sum of the two trains at all. FUN_0051c690 reads only
 * the SECOND, radial one -- sin(period2*t + r/length2) -- at unit amplitude,
 * which is what the loader's 1/amp2 was precomputed for: 0x51c71d multiplies by
 * amp2 and then straight back by 1/amp2.
 *
 * NOT transcribed: the engine's remap of that signal to an alpha is asymmetric.
 * It branches on the sign of the cosine, so the foam comes ON over a shorter
 * span of phase than it goes OFF over, and the three constants it interpolates
 * between are runtime globals at 0x14ef3d4/e0/e4 rather than anything in a
 * config file. water.c keeps the port's symmetric HeightOn..HeightOff map. */

/* One draw buffer per display buffer, because vitaGL's own circular vertex pool
   is one arena per display buffer (gxm_display_buffer_count, 3 by default) reset
   at swap for the buffer coming round again -- so three is what says "the GPU is
   done with it". Same constant and same reason as CHR_SKIN_RINGS. */
#define WATER_DRAW_RINGS 3

/* The draw size past which vitaGL hands GXM the client pointer instead of
   copying the vertices (SAFER_DRAW_SPEEDHACK). Above it a buffer may not be
   rewritten until the GPU has been round the display buffers. */
#define WATER_CLIENT_PTR_LIMIT (32u * 1024u)

#define WATER_MAX_WAVES 32
#define WATER_MAX_SPAWN 8

typedef struct {
    float x, y, z;          /* current position, world; `y` is the marker's own
                               and is no longer what the sprite is drawn at --
                               see wave_base_y */
    float sea_y;            /* rest height of the sea surface under the spawner */
    float dx, dz;           /* unit travel direction, ground plane */
    float life, age;        /* seconds */
    float u0;               /* texture scroll offset at birth */
    int   active;
} wave_t;

typedef struct {
    float x, y, z;
    float sea_y;            /* the sea's rest height at this marker */
    float dx, dz;           /* the marker's facing */
    float t_long, t_short;  /* the two spawn timers */
} wave_spawn_t;

typedef struct {
    scene_t *scene;
    float t;                /* seconds since the level loaded */
    unsigned int rng;

    /* This track's row of vis_data.h's WSURF[]. Never NULL: water_init clamps
       the index, so a caller with no track (vis_test's fixture) gets beach_1's.
       The surface config is PER TRACK and the port used to compile beach_1's
       into every one of the ten. */
    const wsurf_t *cfg;

    /* cos/sin of cfg->angle_deg, so the directional train's projection is a
       dot product rather than two trig calls per vertex per frame. */
    float d1x, d1z;

    /* per-vertex swell damping for the surface batches, 0 where the seabed
       reaches the surface and 1 in deep water; parallel to each batch's vertex
       array */
    float **damp;

    /* per-vertex colours for the coast batches AND the sea surface (RGBA
       bytes). The surface needs them because it is translucent in the shallows:
       WaterLOD's alphaMin/alphaMax are exactly that, and drawing the sea opaque
       is what put a hard teal edge where the reference has sand showing through. */
    unsigned char **coast_rgba;
    unsigned char **surf_rgba;

    /* THE DRAW RING, and only for the batches that need one.
     *
     * water.c rewrites an animated batch's vertices every frame -- that is why
     * BATCH_ANY_WATER never gets a VBO -- and hands glDrawElements the batch's
     * own array. Past 32 KB in one draw the custom vitaGL
     * (SAFER_DRAW_SPEEDHACK) stops copying those vertices into its own mapped
     * temp and gives GXM the client pointer, which it reads AT FLUSH, after the
     * swap. So the next frame's rewrite lands in memory the GPU is still
     * reading: a torn or one-frame-stale swell. Same mechanism as the exploding
     * characters before char.c's skin ring (CHR_SKIN_RINGS) and as FX_DRAW_RINGS.
     *
     * MEASURED over the ten shipped tracks, so this is confined to what actually
     * crosses rather than applied to every animated batch: exactly THREE do, and
     * all three are the sea surface itself -- beach_1 batch 43 at 7,007 verts =
     * 196,196 B (six times the line), country_3 batch 39 at 51,492 B and
     * beach_4 batch 27 at 47,012 B. Every other animated batch on every track is
     * under (beach_3's biggest is 19,880 B), and trace.c's worst draw is 16.1 KB
     * and envmap.c's 12.9 KB, which is why neither of those is changed.
     *
     * The gate is a PROPERTY -- the batch's own vertex bytes against the
     * library's threshold -- not a list of batch names to keep in step.
     *
     * NULL per batch where none is needed, and then the draw reads b->verts
     * exactly as before. water-owned, so water_free needs nothing from the
     * scene: main.c releases the scene BEFORE calling water_free, which is why
     * b->verts is animated in place and copied out rather than repointed. */
    vtx_t **vring[WATER_DRAW_RINGS];
    unsigned int ring;

    /* THE SEAM TABLES. Both exist because the port displaces AUTHORED tiles
     * where the engine displaces a grid it tessellates itself -- see
     * WATER_WELD_TOL and wstitch_t above.
     *
     *   site[bi][j]  the vertex whose rest position speaks for j. Coincident
     *                vertices at a tile seam are DUPLICATED in the shipped
     *                mesh -- 646 of beach_1's 7,007 -- and only about half of
     *                them agree bit for bit, so keying anything per-vertex
     *                makes the two sides of a seam disagree. Everything
     *                position-derived is read through this: the height, the
     *                depth damping and the UV orbit's own hash.
     *   stitch[bi]   the T-JUNCTIONS. A vertex of a finely tessellated tile
     *                that lands on the INTERIOR of a coarser neighbour's edge
     *                is not coincident with anything, so welding cannot reach
     *                it: the fine side follows the swell and the coarse side
     *                stays a chord, and the surface opens. 269 on beach_1,
     *                the longest edge 17.4 m against a 10.5 m wavelength, so
     *                the crack is the full swell: 0.34 m there and 0.60 m on
     *                beach_3.
     *                Each entry pins one vertex's height to the lerp along the
     *                edge it sits on. */
    int **site;
    wstitch_t **stitch;
    unsigned int *n_stitch;

    /* THE HORIZON. The engine's water is a LOD grid it lays out around the
       camera (_wlodQual, _wlodRRProcessStripe, the lod0..lod4 sliders); the
       port's is the authored tiles, which stop at the edge of the map. Stand
       near that edge and the sea ends short of the sky dome -- which is drawn
       camera-locked, i.e. at infinity -- and the band between them is a hole.
       This is one flat plane at the sea's own mean level, drawn with the sky
       and before anything else, so every pixel the world covers is still the
       world's. Zero on a track with no sea surface at all. */
    int   horizon;          /* 1 when this track has a sea to continue */
    float horizon_y;        /* world height of the mean sea surface */
    GLuint horizon_tex;     /* the sea batch's own texture */
    float horizon_alpha;    /* the deep-water alpha, WSURF alphaMax */

    GLuint wave_tex;
    wave_t waves[WATER_MAX_WAVES];
    wave_spawn_t spawn[WATER_MAX_SPAWN];
    int n_spawn;
    int n_live;             /* telemetry */

    /* How many entries the three arrays above were sized for. water_free needs
       it: on a reload the scene they were built against is already gone. */
    unsigned int n_alloc;
} water_t;

/* Build the damping table and pick up the wave spawn markers. `col` may be
   NULL, in which case the whole surface heaves at full amplitude -- fine on a
   test fixture, visibly wrong on a real beach. One ground query per surface
   vertex, at load.

   `track` indexes vis_data.h's WSURF[] -- and tracks.h's TRACKS[], which is
   the same order; out of range clamps to 0. The sea's height, wavelength and
   vertical offset all come out of that row and all three differ per track.

   Accepts an UNINITIALISED struct -- it memsets first. So call water_free
   yourself before re-initialising one that has already been built. */
void water_init(water_t *w, scene_t *scene, const col_t *col, int track);

/* Release everything water_init allocated and zero the struct. Safe on a
   zeroed or an already-freed one; not safe on an uninitialised one. */
void water_free(water_t *w);

/* Advance the wave sprites and the scroll clock. */
void water_step(water_t *w, float dt);

/* Displace and scroll the surfaces, then draw them. Call after the opaque
   world; leaves GL state as it found it. `eye` is the camera position, which
   the wave sprites billboard about their crest towards. */
void water_draw(water_t *w, const float eye[3]);

/* The horizon sea: one camera-centred plane at the track's own mean sea level,
   reaching past the sky dome. Draw it WITH THE SKY -- after the dome, before
   the world, depth test and depth write both off -- so it fills only what
   nothing else covers. Does nothing on a track with no sea, and nothing when
   the eye is at or below the surface (there is no horizon to fill from under
   the water). `eye` is the camera position. */
void water_draw_horizon(const water_t *w, const float eye[3]);

/* The surface's displacement at (x, z) -- the same sum the surface batches
   use, before the depth damping and before the track's vertical offset. Handy
   for anything that wants to sit on the water.

   NOT what the physics reads. rb_world.water answers out of the .col grid,
   which pack_col.py bakes at rest height PLUS the same cfg->offset the surface
   is drawn with, so the two agree on where the waterline is; feeding this in
   as well would modulate the drag with a wave the engine's probe never had. */
float water_height(const water_t *w, float x, float z);

#endif
