/*
 * carlight.h -- the car's own lighting: one directional sun, one ambient, and
 * the LEVEL LIGHTMAP SAMPLED UNDER THE WHEELS.
 *
 * Recovered whole, and none of it was in this port. Three engine subsystems and
 * two files of shipped data that nothing here had opened:
 *
 *   Settings/carLight.ini    AmbientI 138, DirectI 128, AngleAzimuth 0,
 *                            AngleElevation 75 -- and six R/G/B keys, all 255,
 *                            that the loader at 0x004f9aa0 does not read. So
 *                            both terms are GREY.
 *   Settings/car_lmshd.ini   alphavel 127, deltaalpha 5, maxdist 25, and the
 *                            name of a spline: `light_car.bspl'
 *   Splines/light_car.bspl   two control points, 112.6 -> 158.9 and
 *                            200.9 -> 260.0
 *   Settings/LOD.ini + .crs  DistLOD1SwitchOn / Delta12 -> 2.00 m and 4.30 m
 *
 *   carVisualBody 0x005077e0   builds the light and the ambient and submits them
 *   FUN_00531ff0               drives the light LEVEL off the lightmap
 *   FUN_005323a0               loads car_lmshd and the spline
 *   FUN_004572c0               samples a lightmap texel at a point on a face
 *
 * THE MODEL, in the order the frame runs it.
 *
 * 1. THE LEVEL. `FUN_00531ff0', called per car per frame off the car's visual
 *    step (0x00531a20), and it is the whole of "the car goes dark under the
 *    bridge":
 *
 *      - if no camera is within LIGHT_LM_MAXDIST (25 m) of the car, the level is
 *        1.0 and the entry latch is cleared. This is an LOD gate and nothing else
 *      - otherwise, for each of the FOUR WHEELS that has ground contact, sample
 *        the level's LIGHTMAP at the contact point and average the wheels that
 *        answered. The engine's sampler is `FUN_004572c0': it barycentrically
 *        interpolates the face's LM UVs at the point, reads the texel out of the
 *        atlas with `csiGetImPixel', and returns **the mean of its R, G and B**.
 *        Gated on Video/VIDEO_LightMaps
 *      - if no wheel answered -- airborne, or on a face with no lightmap -- the
 *        engine casts a 1.5 m sphere query at the car's own position, 1 cm up,
 *        and samples whatever that hits. If that fails too it leaves the level
 *        alone. AND THAT FALLBACK IS FOR PLAYERS 0 AND 1 ONLY (0x005321b1); an
 *        opponent in the air keeps the level it had
 *      - the average goes through `light_car.bspl' at 0..255 and comes back
 *        divided by 255 again, so the CURVE IS THE WHOLE FEEL OF IT: 0.623 at or
 *        below brightness 112.6, 1.019 at or above 200.9, a line between. A
 *        tunnel darkens the car by 38% and never more
 *      - on the frame the car ENTERS range the level snaps. After that it chases
 *        at LIGHT_LM_ALPHAVEL (1.27 per second) with a LIGHT_LM_DELTA (0.05) dead
 *        band, so driving into shadow takes about a third of a second
 *
 * 2. THE TWO INTENSITIES. `carVisualBody' scales both bytes by the level, greys
 *    them and submits them -- the ambient as the ambient RENDER STATE and the
 *    direct as one DIRECTIONAL light's diffuse colour:
 *
 *      ambient = min(255, AmbientI * level)     138 -> 0.541 at level 1
 *      diffuse = min(255, DirectI  * level)     128 -> 0.502 at level 1
 *
 *    So a face square to the sun renders at 1.04x its texture and a face turned
 *    away at 0.54x. That two-to-one range across one moulded shell IS the effect,
 *    and it is why the car reads as plastic rather than as a decal.
 *
 * 3. THE LOD FADE. Called with a camera matrix, `carVisualBody' ramps the pair
 *    toward FLAT over LIGHT_FADE_NEAR..LIGHT_FADE_FAR -- ambient to 255 and
 *    direct to 0, i.e. the unlit texture. LOD.crs overrides Delta12 from the
 *    .ini's 60 to 4, which puts the far end at 4.30 m rather than 9.96, and the
 *    chase camera sits 0.87 m back (RB_CAMERA dist_xz 0.79, dist_y 0.36). So the
 *    PLAYER'S car is always at full strength and an opponent more than 4.3 m away
 *    is flat. The feature really is about the car the camera follows.
 *
 * 4. THE DIRECTION. `Ry(azimuth)' applied to (0,0,1) -- the engine's own
 *    DAT_0055e9b0 -- then rotated toward +Y by the elevation with `FUN_0040b4c0',
 *    which is a spherical move-towards: it turns by `min(angle between, param)'.
 *    The base vector is horizontal, so the angle between it and +Y is always 90
 *    and the slider's range is 0..90 -- the min() can never bite, and the whole
 *    thing collapses to
 *
 *        L = (sin(az) cos(el),  sin(el),  cos(az) cos(el))
 *
 *    which at the shipped 0 and 75 is (0, 0.966, 0.259): nearly overhead, tipped
 *    a little toward world +Z. `carlight_dir' is that closed form and
 *    `carlight_test' checks it against a transcription of FUN_0040b4c0 rather
 *    than against itself.
 *
 * WHAT IS THE PORT'S, and each is anchored:
 *
 *   1. THE SENSE OF THE DIRECTION. The engine submits that vector to its light
 *      unnegated, and Direct3D's own `D3DLIGHT8.Direction' points the way the
 *      light TRAVELS -- which would put the sun under the track and light the
 *      car's floor. Two things settle it the other way: the key is called
 *      AngleELEVATION with a 0..90 slider, and the engine's D3D wrapper is a
 *      function pointer this port cannot read. So `carlight_dir' returns the
 *      direction TOWARD the light, which is also what GL's GL_POSITION with
 *      w = 0 wants. If it is ever shown to be the other way, negate here.
 *
 *   2. WHERE THE BRIGHTNESS UNDER A WHEEL COMES FROM. The engine collides
 *      against its RENDER geometry, so the face under a wheel carries its own LM
 *      UVs and it can sample the atlas per contact. This port's collision is a
 *      separate grid (`col.c'), so `pack_col.py' bakes the answer per collision
 *      triangle instead -- COL5, one byte, the mean lightmap RGB at the
 *      triangle's own LM-UV centroid, 0xFF where the face has no lightmap. Same
 *      quantity, sampled once per triangle rather than per contact point: a
 *      lightmap texel covers a good fraction of a metre on these tracks and the
 *      curve is flat outside 113..201, so the difference does not survive the
 *      spline. `col_light_at' is the query.
 *
 *   3. THE GLANCE IS DIMMED BY THE LEVEL TOO, which is the engine's -- the env
 *      pass `FUN_00507ba0' reads the same +0xe824 and builds a colour out of it
 *      (0x00507bcc) -- but the port applies it as a grey on `envmap_draw's
 *      vertex colour, keeping its alpha. A sky reflection has no business being
 *      bright inside a tunnel.
 *
 * THE STATE IS PER CAR, and that is not an assumption: the level lives at
 * `*(car+0xe8) + 0xe824', and car+0xe8 is the car's OWN physics record -- ani.c
 * reads its wheel count at +0x5928 and its spring params at +0x54 out of the
 * same pointer.
 *
 * WHY IT IS ITS OWN FILE: the model is pure arithmetic over plain numbers, so
 * `carlight_test' can drive a car into a tunnel and back out on the host. The GL
 * calls are the last three functions and nothing else in here touches the API.
 */

#ifndef CARLIGHT_H
#define CARLIGHT_H

#include "vis_data.h"

/* Per car. Two fields of the engine's own record and one bookkeeping flag. */
typedef struct {
    float level;      /* +0xe824: the light level, 0.623 .. 1.019 in practice */
    int   in_range;   /* +0xe820: a camera was within LIGHT_LM_MAXDIST */
    /* Not the engine's: what the last step actually did, for the log and for
       carlight_test. The engine has no need to say. */
    int   n_samples;
    float raw;        /* the averaged lightmap brightness, 0..1, -1 = none */
} carlight_t;

/* The engine's own initial state: level 1.0, latch clear. FUN_005320de. */
void carlight_reset(carlight_t *cl);

/* One step of FUN_00531ff0.
 *
 *   cam_dist  metres from the nearest camera to the car (the maxdist gate)
 *   samples   the lightmap brightness under each wheel that has contact, 0..1
 *   n         how many of them there are, 0..4 -- 0 means nothing sampled
 *   fallback  the brightness under the car's own position, or a negative number
 *             if there is none. The engine's sphere-query branch, and like the
 *             engine it is only consulted when n == 0
 *   dt        seconds
 */
void carlight_step(carlight_t *cl, float cam_dist,
                   const float *samples, int n, float fallback, float dt);

/* The direction TOWARD the light, world space, unit length. See point 1. */
void carlight_dir(float out[3]);

/* light_car.bspl at x (0..255), returning 0..255. The engine's own clamped
   piecewise linear (0x0040f830), which for two control points is a line and a
   clamp. Exposed for the test. */
float carlight_curve(float x);

/* carVisualBody's two greys, 0..1, after the level and the LOD fade. */
void carlight_terms(const carlight_t *cl, float cam_dist,
                    float *ambient, float *direct);

/*
 * The sun in a CAR'S OWN MODEL SPACE. `m' is a body-to-world row-vector 4x4 --
 * rbcar_matrix's, or ai_matrix's -- so its first three rows are the model axes in
 * world space and the component of the world light along each row is the light in
 * model space. No inverse and no transpose: for a rotation they are the same
 * thing, which is the sort of sentence this project has been wrong about four
 * times, so carlight_test checks the INVARIANT instead of the arithmetic --
 * shading a model normal with this must equal shading the same normal rotated
 * into the world with the world light, over a set of real rotations.
 *
 * It lives here rather than in main.c so that it can be checked at all: nothing
 * compiles main.c.
 */
void carlight_model_dir(const float m[16], float out[3]);

/* --------------------------------------------------------------- the shading */

/*
 * The lighting equation, per vertex, into RGBA bytes. `L' is the direction
 * TOWARD the light in the SAME SPACE as the normals -- the caller rotates it,
 * because only the caller knows whether these normals belong to the body or to a
 * wheel under its own matrix.
 *
 *     c = clamp(ambient + direct * max(0, N . L)) * 255,  a = 255
 *
 * which is D3D's fixed-function ambient-plus-one-directional-light with a white
 * material, and GL's, and what carVisualBody submits.
 *
 * IT IS THE CPU THAT DOES THIS, and that is the port's, for a reason worth
 * writing down: the same arithmetic through GL's own fixed-function lighting --
 * one directional light, LIGHT_MODEL_AMBIENT, a white material, normals in a
 * vertex array -- is what this port shipped first, and on the target it drew the
 * whole car in fine-grained RAINBOW NOISE. In the custom vitaGL the four
 * material attributes are only reconfigured when the COLOR attribute goes dirty
 * (`setup_lighting_attributes' is inside `if (ffp_dirty_vert_attr & (1 <<
 * FFP_ATTRIB_COLOR))'), and an app that lights without ever touching a colour
 * array never sets that bit; what those attributes read instead is whatever the
 * previous draw left in the slot. A vertex colour array is a path this port
 * already drives every frame in `ui.c', `fx.c', `trace.c' and `water.c', and it
 * is one a host harness can read back. So: same numbers, computed here.
 */
void carlight_shade(float ambient, float direct, const float L[3],
                    const float *nrm, unsigned int n, unsigned char *out);

/* Off restores exactly the port's old appearance: nothing is shaded and the car
   draws at full texture brightness. On the menu as "Car lighting". */
void carlight_set_enabled(int on);
int  carlight_enabled(void);

#endif
