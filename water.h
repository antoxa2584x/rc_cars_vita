/*
 * water.h -- animated water: the sea surface, the shoreline foam, the stream,
 * the waterfall, and the breaking-wave sprites.
 *
 * The game's water is the WaterLOD module (wlodInit and friends, 0x00521540
 * for its config, 0x00525000-0x0052a600 for the rest). Four of its five parts
 * map cleanly onto a flattened scene:
 *
 *   surface   the `sea`-textured LOD tiles, displaced and UV-scrolled
 *   coast     the foam band, scrolled, with alpha driven by the water height
 *             (WaterLOD_Coast's HeightOn / HeightOff are exactly that: the
 *             surface heights at which the foam is fully on and fully off)
 *   stream    scrolled at WaterLOD_Stream's ScrollVel
 *   fall      scrolled downward
 *   waves     sprites spawned at the water_wave_N markers, one on a TimeLong
 *             timer and one on a TimeShort timer, exactly as FUN_00525700
 *             steps its two per-marker counters
 *
 * What is transcribed and what is not:
 *
 *   TRANSCRIBED  every constant (vis_data.h cites the loader each came from),
 *                the two-timer wave spawner, the wave sprite's geometry and
 *                life envelope (FUN_0052a030 / FUN_00529c60), the coast's
 *                height-driven alpha.
 *   NOT          the surface displacement FUNCTION. The engine tessellates its
 *                own grid over the water_box volume and evaluates it there;
 *                the port has authored tiles and sums two directional sines
 *                from the recovered angle/speed/period/amp pairs. The numbers
 *                are the game's, the curve is ours.
 *   NOT          the shallow-water damping. The engine has magnetOffset /
 *                magnetRadius / clampYImmediate / clampYScale for it; those are
 *                not read out yet, so water.c fades the displacement out where
 *                the seabed comes up to meet the surface. Without SOMETHING
 *                there a 0.41 m swell tears open the seam where the sea meets
 *                the sand.
 *
 *                Depth, and not distance to the `coast` meshes, because five of
 *                the ten tracks have water and no coast mesh at all, and
 *                because the water surface ships as one merged batch whose
 *                per-tile vertices are not shared -- so its own mesh boundary
 *                is every tile edge, not the shoreline.
 */

#ifndef WATER_H
#define WATER_H

#include "col.h"
#include "scene.h"

/* How far the sea surface itself moves, metres. NOT a recovered value -- see
   water.c's surf_signal. Here rather than in the .c so vis_test can bound
   against it: the first build displaced by 0.41 m, built half out of a key the
   engine never reads. */
#define WATER_SWELL_AMP 0.02f

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

/* The stream and the waterfall are translucent too, at a flat value: they are
   uniformly shallow (beach_1's stream runs 2 to 13 cm over its bed), so the
   depth ramp the sea uses would take them to nearly clear. NOT recovered --
   WaterLOD_Stream ships ScrollVel and noise terms and no alpha at all. */
#define WATER_STREAM_ALPHA 0.55f

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

/* --- how far the foam's on/off signal is stretched -------------------------
 *
 * shore_height reads the two recovered wave trains, whose wavelengths work out
 * at 1.24 m (WSURF_SPEED * WSURF_PERIOD) and 1.61 m (WSURF_LENGTH2). The coast
 * band's own median triangle edge is 1.77 m on beach_1 (p90 2.60 m), so
 * evaluating that signal per vertex samples it BELOW Nyquist: neighbouring
 * vertices land on unrelated phases and the band flashes at the faster train's
 * 1.67 Hz instead of breathing with a swell. vis_test already had to work
 * around it -- its alpha check samples over time because "a single frame is a
 * snapshot of a 1.2 m wave read on a grid whose spacing is several metres".
 *
 * Dividing the whole PHASE by one factor stretches wavelength and period
 * together, so WSURF_SPEED comes out unchanged at 2.06 m/s: it is the same wave
 * train read at swell scale rather than at ripple scale. 12x puts the two
 * trains at 14.8 m / 7.2 s and 19.3 m / 21.6 s -- about seven band vertices per
 * wavelength, which is a curve rather than a sampling artefact, and shore-swell
 * timing rather than a strobe.
 *
 * The value is set by the coarsest thing that samples it, so anything under
 * about 9 is visibly aliased and vis_test bounds it from below on both counts.
 *
 * The port's number. The engine never had this to solve -- it tessellates its
 * own surface grid far finer than the authored coast strips and reads the foam
 * height off that. */
#define COAST_WAVE_STRETCH 12.0f

#define WATER_MAX_WAVES 32
#define WATER_MAX_SPAWN 8

typedef struct {
    float x, y, z;          /* current position, world */
    float dx, dz;           /* unit travel direction, ground plane */
    float life, age;        /* seconds */
    float u0;               /* texture scroll offset at birth */
    int   active;
} wave_t;

typedef struct {
    float x, y, z;
    float dx, dz;           /* the marker's facing */
    float t_long, t_short;  /* the two spawn timers */
} wave_spawn_t;

typedef struct {
    scene_t *scene;
    float t;                /* seconds since the level loaded */
    unsigned int rng;

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

   Accepts an UNINITIALISED struct -- it memsets first. So call water_free
   yourself before re-initialising one that has already been built. */
void water_init(water_t *w, scene_t *scene, const col_t *col);

/* Release everything water_init allocated and zero the struct. Safe on a
   zeroed or an already-freed one; not safe on an uninitialised one. */
void water_free(water_t *w);

/* Advance the wave sprites and the scroll clock. */
void water_step(water_t *w, float dt);

/* Displace and scroll the surfaces, then draw them. Call after the opaque
   world; leaves GL state as it found it. `eye` is the camera position, which
   the wave sprites billboard about their crest towards. */
void water_draw(water_t *w, const float eye[3]);

/* The surface height at (x, z) -- the same sum the surface batches use. Handy
   for anything that wants to sit on the water. */
float water_height(const water_t *w, float x, float z);

#endif
