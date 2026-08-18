/*
 * scene.h -- the .vsc runtime scene, moved out of main.c.
 *
 * Six versions load:
 *
 *   VSC3   textures + batches                      (a plain flattened scene)
 *   VSC4   + the car's rig                         (see carani.c)
 *   VSC5   + surface kind flags and scene markers  (the animated track)
 *   VSC6   + lightmap UVs and a lightmap per batch (the level's baked lighting)
 *   VSC7   + an env-map class per batch, and vertex normals for the batches
 *            that have one                         (the body glance, envmap.c)
 *   VSC8   + a table of named models sharing one file, and a model index per
 *            batch                                 (props.vsc, see prop.c)
 *
 * The surface flags are what let the port animate water the way the game does.
 * The engine knows a mesh is coast or stream or the WaterLOD surface because of
 * its name in the scene database; pack_vsc.py resolves the same names at pack
 * time and records the answer, so nothing has to be re-derived at 60 Hz.
 */

#ifndef SCENE_H
#define SCENE_H

#include <stdio.h>

#include <vitaGL.h>

#include "carani.h"

/* lu, lv are the LIGHTMAP UVs -- the .sb's second UV layer, tagged LM_1. Zero
   on a batch with no lightmap, and on any scene older than VSC6. */
typedef struct { float x, y, z, u, v, lu, lv; } vtx_t;

/* batch flags -- must match pack_vsc.py's BATCH_* */
#define BATCH_SKY     1u
#define BATCH_WATER   2u    /* the WaterLOD surface tiles */
#define BATCH_COAST   4u    /* the foam band along the shoreline */
#define BATCH_STREAM  8u    /* the stream */
#define BATCH_FALL   16u    /* the waterfall */
/* A signed detail map: MULTIPLIED onto the ground, not drawn as a surface.
   The texture is near-greyscale with a border of neutral 128, so under the
   engine's blend mode 5 (dst' = 2*src*dst) it darkens the middle of the strip
   and fades to nothing at its own edges -- transparency with no alpha channel
   anywhere. pack_vsc.py's signed_detail_map decides it from the pixels.

   In these ten tracks it is exactly one texture, `wt_wetsand`: the sand /
   wet-sand transition strip, 151 meshes and 9,198 triangles across the set,
   authored a centimetre above the terrain and carrying no lightmap. Drawn
   opaque, which is what the port did, it is a flat grey band with two hard
   edges -- the "sharp transition" as reported. main.c draws these in their own
   pass; the blend is the one trace.c already uses for a tyre mark, which is the
   same kind of object. */
#define BATCH_MODULATE 32u

/* The level artists tagged this mesh `TRANSP<n><A|B>` -- draw it ALPHA-BLENDED
   rather than alpha-tested.

   These are the marks painted on the track: `icon_start`, `icon_chp_1/3`,
   `icon_arrow_1/2`, `icon_pacific`, `icon_shc_1`, plus glass, lamp glass and
   some foliage. Their alpha is a soft ramp -- 18% to 36% of their texels sit
   strictly between transparent and opaque -- and the app was putting them
   through `glAlphaFunc(GL_GREATER, 0.5)`, which is a HARD CUT: every texel above
   half became fully opaque and everything below vanished. A ground marking meant
   to look sprayed on came out as a solid sticker with a jagged edge.

   Blended, the ramp survives and the mark reads as paint on the surface. main.c
   draws these after the opaque world with depth writes off. See TRANSP_RE in
   pack_vsc.py for why a mesh NAME is the right key here and was the wrong one
   for the sky. */
#define BATCH_TRANSP 64u

/* A standing puddle: `pool<n>`, the third entry of the engine's own
   stream-family table at 0x575710. Kept apart from BATCH_STREAM because the
   table's flags differ in both of the two ways that matter -- the pool carries
   its own vertex alpha (110, against the stream's 120) and it does NOT scroll
   its UVs; what it does instead is jitter them by a noise field, which is the
   one part of the entry the port does not transcribe. See POOL_VERTEX_ALPHA in
   vis_data.h and water_draw.

   These are country_1's seven ponds and beach_4's two. Missing from the port's
   name rule entirely, they were drawn as opaque `sea`-textured slabs -- flat
   teal plates lying on the ground, which is exactly what was reported. */
#define BATCH_POOL  128u

#define BATCH_ANY_WATER (BATCH_WATER | BATCH_COAST | BATCH_STREAM \
                         | BATCH_FALL | BATCH_POOL)

/*
 * Set at RUNTIME, never packed -- deliberately well clear of pack_vsc.py's bit
 * range so the two cannot collide as either side grows. carparts.c raises it on
 * the exhaust groups; main.c draws those batches in their own pass with the
 * alpha-test reference dropped to 0, and OPAQUE. See carparts.h.
 *
 * THE EXHAUST IS NOT TRANSLUCENT, AND THIS FLAG USED TO BE NAMED AS THOUGH IT
 * WERE. Every <prefix>turbo_<n> is ARGB4444 whose alpha sits on a plateau at
 * nibble 7 -- 119 as a byte, 0.467 -- over 1.3 to 22.4% of the image, and the
 * world's cut-out test is glAlphaFunc(GL_GREATER, 0.5), just above it. So those
 * texels were being discarded and the pipes and boosters had holes punched
 * through them. The fix for THAT was to draw the batches BLENDED, which traded
 * the holes for 47%-opaque bodywork -- reported as "car boosters and exhaust
 * looks semi transparent", and it was.
 *
 * What the plateau actually covers is SOLID METAL, checked in the artwork on all
 * three cars: the Overkill's brass velocity stacks and their flanges and nut,
 * the Buggy's magenta muffler barrel and pipe elbow, the Hummer's blue barrel
 * and cone insert. THE ARTISTS' OWN SHOP ICONS SETTLE IT -- upgr_boost<1..3>_<n>
 * are renders of these very parts, and in all nine the stacks, barrels and scoop
 * fronts are fully opaque, with the car's bodywork completely hidden behind
 * them. (Same class of evidence as upgr_tires* under "A fitted tyre is wider".)
 * Alpha 119 is not an opacity the engine reads: the exhausts are simply the only
 * 12 ARGB textures on a car whose other 14 are RGB with no alpha channel at all,
 * so there is nothing else on the car for the engine to have read alpha from.
 *
 * The engine's own blend selector agrees that opaque is expressible and is the
 * default. FUN_0045c3c0 tests the material flags byte and calls FUN_0045c6e0,
 * whose mode 0 -- taken whenever bit 0x1 is CLEAR -- is
 * SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE) at 0x0045c6f7. Its jump table at
 * 0x0045c964 puts plain SRCALPHA/INVSRCALPHA on mode 2 (bit 0x2, 0x0045c7e5) and
 * the documented 2*src*dst on mode 5 (bit 0x20, 0x0045c911) -- the tyre marks'
 * own mode, which is what confirms the decode. Car.sb declares no MOD_MATERIAL
 * node at all and every car texture's 0x3408 words 3 and 4 are 0, so nothing in
 * the car's data asks for a blend mode.
 *
 * So the THRESHOLD is the whole point of this flag and the blend was the bug.
 * GREATER 0 rather than no test at all, because the alpha-0 region -- the dark
 * bores looking into the stacks -- stays a real cut-out. That half was already
 * reasoned through and is not what was reported wrong.
 */
#define BATCH_ALPHA_LOWREF 0x1000u

/*
 * Set at RUNTIME by scene_load: this batch's texture has texels the alpha test
 * would actually reject, so it needs GL_ALPHA_TEST and the rest of the scene
 * does not.
 *
 * This matters far more on hardware than it looks. vitaGL compiles the alpha test
 * into the generated fragment shader as a `discard` (source/shaders/ffp_f.h), and
 * on the Vita's PowerVR SGX a shader that can discard makes its geometry
 * PUNCH-THROUGH: it can no longer take part in the tile-based hidden-surface
 * removal that is the whole reason that GPU is fast. main.c used to enable the
 * test once, globally, for the alpha-keyed foliage and signage -- correct as
 * output, and it put EVERY triangle in the world on the slow path. Measured on
 * beach_1: 14 batches and 1,278 triangles genuinely need the test, against 94
 * batches and 47,514 triangles that do not. 97.4% of the scene was punch-through
 * for the benefit of the other 2.6%.
 *
 * Decided from the pixels rather than from the format, because "has an alpha
 * channel" is not the question -- the RGBA decoder fills opaque textures with
 * alpha 255 and every texel passes. The question is whether any texel would be
 * rejected, i.e. whether any alpha is at or below the 0.5 threshold.
 */
#define BATCH_ALPHA_KEYED 0x2000u

/*
 * Set at RUNTIME by scene_load: this batch's texture does not resolve -- the index
 * is out of range, or the entry it names carries no pixels. Such a batch is NOT
 * DRAWN.
 *
 * Every one of the ten tracks has exactly one, 870 to 4,291 triangles (beach_1's
 * is batch 24, tex 0xFFFFFFFF, 974 tris). They were invisible for a long time, and
 * FOR A REASON NOBODY CHOSE: `gl_tex` fell back to 0, which is never uploaded, and
 * with GL_ALPHA_TEST enabled globally that undefined sampler returned alpha 0, so
 * every fragment was discarded. The batch was hidden by an accident of the alpha
 * test, not by any decision.
 *
 * Confining the alpha test to the batches that need it (BATCH_ALPHA_KEYED above)
 * removed that accident and 974 triangles of black, depth-writing geometry
 * appeared on beach_1 -- occluding scenery from some angles and not others, which
 * is what "black textures on transparent objects" was.
 *
 * So it is explicit now. A batch with no texture has no defined appearance on
 * EITHER target -- an unbound sampler is undefined, and it only happened to read
 * as transparent black in Vita3K -- so drawing it is not a thing to preserve. The
 * count is logged at load, because 974 triangles of geometry with no texture is a
 * PACKING problem (see "5 texture names resolve to no file" in FORMAT_NOTES) and
 * silently dropping it should not make that easier to forget.
 */
#define BATCH_NO_TEXTURE 0x4000u

/* Which CarEnvMap alpha this batch's glance is drawn at -- must match
   pack_vsc.py's ENV_*. 0 means the batch has no glance and carries no normals. */
#define ENV_NONE      0
#define ENV_BODY      1
#define ENV_GRE1      2
#define ENV_GRE2      3
#define ENV_UPGRADES  4

typedef struct {
    unsigned int tex, flags, part, nverts, nidx;
    unsigned int lm_tex;            /* index into tex_ids, or 0xFFFFFFFF */
    GLuint gl_lm;                   /* resolved, 0 = unlit */
    unsigned int env;               /* ENV_*, 0 on a scene older than VSC7 */
    unsigned int model;             /* VSC8: which prop model, 0 otherwise */
    /* Model-space vertex normals, 3 floats each, NULL unless env != 0. The
       glance pass turns these into sphere-map UVs; nothing else reads them. */
    float *nrm;
    vtx_t *verts;
    unsigned short *idx;
    GLuint gl_tex;
    /* Animated batches keep an untouched copy of the packed vertices. Waves are
       a displacement from rest, not an accumulation -- integrating in place
       drifts, and a scroll that never resets eventually loses float precision
       in the UVs. */
    vtx_t *rest;
    /* Model-space AABB over `verts`, computed once at load. What frustum culling
       tests -- see scene_set_frustum. An empty batch gets an inverted box, which
       the culler treats as "nothing to draw". */
    float bmin[3], bmax[3];
    /*
     * The batch's vertices and indices resident on the GPU, or 0 if it draws
     * from `verts`/`idx` in main memory instead. See SCENE VERTEX BUFFERS below.
     */
    GLuint gl_vbo, gl_ibo;
} batch_t;

#define SCENE_MARKER_NAME 32

typedef struct {
    char name[SCENE_MARKER_NAME];
    float x, y, z;
    float yaw;              /* degrees; the marker faces (sin yaw, 0, cos yaw) */
} marker_t;

#define SCENE_TEX_NAME 32

typedef struct {
    batch_t *batches;
    unsigned int n_batches;
    GLuint *tex_ids;
    char (*tex_names)[SCENE_TEX_NAME];
    unsigned int n_tex;
    marker_t *markers;
    unsigned int n_markers;
    /* VSC5: the world half-extent the '__shadow' texture covers, 0 if the
       scene has none. Fitted to the car at pack time -- see pack_vsc.py's
       shadow_fit_radius for why ShadowSize is the wrong number for this. */
    float shadow_radius;
    /* VSC4 and up: the car's rig. `has_rig` stays 0 for a scene with no parts,
       and then every batch is part 0 and draws with no extra matrix. */
    int has_rig;
    carani_t rig;
    /* VSC8: several named models sharing one file. props.vsc holds all 13 props;
       everything else has none and every batch is model 0. See scene_draw_model
       and PROP MODELS in prop.h. */
    unsigned int n_models;
    char (*model_names)[SCENE_TEX_NAME];
} scene_t;

/*
 * SCENE VERTEX BUFFERS -- why the static geometry lives on the GPU.
 *
 * This was measured, not guessed. A 545-frame log off a real Vita (rccars.log,
 * clocks already raised to arm 444 / gpu 222) put the mean frame at 37.2 ms:
 * draw 25.7 ms, sim 11.2 ms, and swap 185 US. Swap is where the CPU waits for
 * the GPU, and only 1 frame in 545 spent more than a millisecond there -- the
 * GPU was idle the entire time and the frame was CPU-bound end to end.
 *
 * Regressing draw time on what was submitted gives
 *
 *     draw_us = 0.670 * tris + 6910          R^2 = 0.708
 *
 * and adding batch count to the model does not improve R^2 at all. So the cost
 * was per VERTEX, not per draw call, which points at exactly one thing.
 *
 * vitaGL's fixed-function path, for every attribute NOT backed by a VBO, scans
 * the whole index buffer for its highest index (ffp.c:1472) and then memcpy's
 * `top_idx * stride` bytes into a freshly allocated GPU-mapped temp buffer
 * (ffp.c:1587) -- once per attribute, once per draw call, every frame. `stride`
 * is the full sizeof(vtx_t) = 28, and draw_pass enables THREE attributes on a
 * lightmapped batch (position, base UV, lightmap UV). That is 84 bytes copied
 * per visible vertex per frame.
 *
 * The numbers close: beach_1 packs 2.22 verts per triangle, the mean frame drew
 * 28,057 tris ~= 62,250 verts, so 5.2 MB was being copied per frame; against the
 * measured 0.670 us/tri that is 278 MB/s, which is what an uncached streaming
 * memcpy costs on this bus. The draw time WAS the memcpy, very nearly in full.
 *
 * A VBO removes all of it. When every enabled attribute is buffer-backed
 * vitaGL's is_full_vbo stays true (ffp.c:1437), the index scan is skipped and
 * each stream becomes a pointer into the buffer rather than a copy of it.
 *
 * WHICH BATCHES GET ONE: those whose vertices never change. Two things take a
 * batch back off the GPU, and between them they are the whole exception list:
 *
 *   - BATCH_ANY_WATER never gets one. water.c rewrites `verts` from `rest`
 *     every frame for the swell and the UV scroll, and draws them through its
 *     own path rather than scene_draw. gl_vbo stays 0 and draw_pass falls back
 *     to client pointers, so a water batch handed to scene_draw would still be
 *     correct, just slow.
 *   - scene_keep_rest DELETES one. Asking for a rest copy is the declaration
 *     that an animator is about to rewrite the vertices, so the buffer would go
 *     stale the moment it was asked for. antenna.c is the caller that needs
 *     this -- it bends the ANTENNA part's mesh in main memory, which a live VBO
 *     silently ignores, and the whip drew as a welded stick for as long as it
 *     was buffered.
 *
 * Nothing else mutates vertices: carparts.c hides a batch by zeroing nidx, the
 * rig moves parts by matrix, and envmap.c writes into a buffer of its own. A
 * new animator does not have to be added to a list here -- it has to call
 * scene_keep_rest, which it needs anyway to have a rest pose to work from.
 *
 * AND THE TWO VERTEX ARRAYS ARE APP-WIDE: main.c enables GL_VERTEX_ARRAY and
 * GL_TEXTURE_COORD_ARRAY once before the frame loop and NOTHING may disable
 * them. scene.c, water.c, trace.c, fx.c and ui.c only ever toggle
 * GL_COLOR_ARRAY, and lm_bind only touches unit 1's copy. char.c disabled both
 * on the way out once, and everything drawn after it -- the water, the effects,
 * the sun, the arrows and the whole menu -- silently stopped submitting
 * geometry. A new drawing module inherits this contract; it is written here
 * because it was not written anywhere.
 *
 * THE UNBIND AT THE END OF scene_draw IS LOAD-BEARING. In GL a bound
 * GL_ARRAY_BUFFER turns every subsequent gl*Pointer argument into an OFFSET, and
 * fx.c, trace.c, ui.c, shadow.c, water.c and envmap.c all still pass real
 * pointers. Leaving one bound reinterprets those addresses as offsets into the
 * track's vertex buffer -- not a subtle wrong colour, a wild read. vis_test
 * asserts nothing is left bound.
 *
 * Cost: one extra copy of the vertex and index data, ~3.3 MB on beach_1, held on
 * the GPU alongside the main-memory copy. The main-memory copy has to stay --
 * culling boxes are built from it at load, water keeps `rest` from it, envmap
 * reads it per frame, and trace_fit_tyres and carani measure the packed meshes
 * through it.
 */

/*
 * TEXTURE QUALITY -- the game's own RenderQual / VIDEO_TexQual, both of which are
 * keys in the Video section of RCCars.crs.
 *
 * The original ships three complete texture sets and picks one at runtime:
 * Textures.1 at 512 px, Textures.2 at 256, Textures.3 at 128 -- the same images
 * at three sizes. The port packs one set (the 512 one; indexing them by
 * directory order and silently getting the 128 px copy is a bug this project
 * already shipped once) and does not need the other two, because every .csi
 * carries a full mip chain: a 512 texture's level 1 IS 256 px and its level 2 IS
 * 128 px. Measured on the packed tracks -- 73 of beach_1's 84 textures chain all
 * the way down to 1x1.
 *
 * So a quality level is just "skip this many top mip levels at upload". It costs
 * nothing in the asset and lands on exactly the resolutions the original's three
 * sets have.
 *
 * WHAT IT DOES *NOT* BUY IS FRAME RATE, and the comment here used to claim it
 * did. It divides texture MEMORY by four per step; it does not divide sampling
 * bandwidth, because mipmapping already governs that. With a correct mip chain
 * the hardware picks its level from screen-space density, so distant terrain was
 * already sampling a 32-px level whether the chain starts at 512 or at 128 --
 * dropping the top of the chain only takes away detail up close, where the
 * sampler is magnifying and costs the same either way. Expect this setting to
 * move memory and looks, and to leave the frame time where it was.
 *
 * Applies to whatever loads NEXT, since the levels are picked at upload time;
 * the menu reloads the track and the car when it changes. A texture with fewer
 * levels than this asks to skip keeps its smallest one rather than vanishing.
 */
#define SCENE_TEX_QUALITY_LEVELS 3
void scene_set_tex_quality(int skip_levels);
int  scene_tex_quality(void);

/*
 * RGB565 CHANNEL ORDER -- and why this is a setting rather than a constant.
 *
 * The assets are packed with RED in the high bits: plain, standard 565. That is
 * what the hardware wants. vitaGL memcpy's GL_UNSIGNED_SHORT_5_6_5 straight into
 * GXM's SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB, and the SDK spells that same enum
 * SCE_GXM_COLOR_FORMAT_R5G6B5 (gxm.h:343) -- red high, no ambiguity.
 *
 * VITA3K DISAGREES WITH THE HARDWARE HERE. It reads U5U6U5_RGB as though blue
 * were in the high bits, so the same file that is correct on a Vita comes out
 * with red and blue exchanged in the emulator -- blue sand, gold water, orange
 * sky. This port spent weeks packed the other way round for exactly that reason:
 * it was developed against the emulator, looked right there, and was wrong on
 * every real machine.
 *
 * There is no reliable way to ask at runtime which one you are on, and probing it
 * by drawing a known texel and reading it back needs a completed frame and the
 * right buffer index before anything else has loaded -- fragile machinery on the
 * one path where a mistake means the app never starts. So it is a switch, with
 * the hardware-correct default, and swapping it costs a reload of the scene like
 * a quality change does. On real hardware you should never touch it.
 */
void scene_set_tex_swap_rb(int on);
int  scene_tex_swap_rb(void);

int scene_load(const char *path, scene_t *s);

/*
 * Read one packed texture off an open file and upload it into `id`.
 *
 * VSC and CHR2 encode a texture identically -- u16 name length, name, u16 w,
 * u16 h, u8 format (1 = RGBA8888, else RGB565), u8 mip count, then the levels
 * largest first -- and everything that has ever gone wrong in here is a
 * property of the UPLOADER, not the format: the 565 byte order, the quality
 * skip, and the one-upload-plus-glGenerateMipmap rule that stopped the load
 * being quadratic. char.c calls this rather than keeping a second copy, so the
 * two formats cannot drift apart.
 *
 * `keyed` reports whether any level-0 texel would fail the alpha test and
 * `has_px` whether anything was uploaded at all; either may be NULL.
 */
void scene_read_texture(FILE *f, GLuint id, char *name_out, size_t cap,
                        int *keyed, int *has_px);

/* Free everything scene_load allocated, and the GL textures it made. */
void scene_release(scene_t *s);

/*
 * Draw just the batches belonging to one VSC8 model, under `m`.
 *
 * props.vsc holds all 13 knockable props in one scene and a track places up to
 * 17 instances of them, so the unit of drawing is "this model, at this matrix",
 * which scene_draw cannot express -- it draws whole scenes and filters on flags.
 *
 * Runs the same two passes as scene_draw (opaque, then alpha-keyed) and leaves
 * the same state behind, buffers unbound included. Frustum culling does NOT
 * apply: a batch's model-space AABB says nothing about where an instance of it
 * has been placed, which is the same reason a rigged scene is never culled.
 * prop.c culls per instance instead, where the world position is known.
 */
void scene_draw_model(const scene_t *s, unsigned int model, const float m[16]);

/* Index of the model with this name, or -1. props.vsc names its models so
   prop.c can bind by name rather than by position -- see pack_props.py. */
int scene_model_index(const scene_t *s, const char *name);

/* Draw every batch whose flags satisfy `(flags & mask) == match`. The world is
   `scene_draw(s, BATCH_SKY | BATCH_ANY_WATER, 0)`, the sky is
   `scene_draw(s, BATCH_SKY, BATCH_SKY)`, and water.c asks for its own. */
void scene_draw(const scene_t *s, unsigned int mask, unsigned int match);

/* GL texture id by packed name, or 0. Used for the textures no geometry
   references: the shore-wave sprite, the checkpoint arrows, the car's
   projected shadow. */
GLuint scene_tex(const scene_t *s, const char *name);

/* Keep an untouched copy of a batch's vertices so an animator can work from
   rest each frame, and drop the batch's vertex buffers so that what the
   animator writes is what draws -- see SCENE VERTEX BUFFERS above. Needs a GL
   context. Idempotent; returns 0 if out of memory. */
int scene_keep_rest(batch_t *b);

/*
 * FRUSTUM CULLING.
 *
 * The port has no VBOs: every batch's vertices stream out of main memory each
 * frame, 3.03 MB of them on beach_1, and until now the whole track was submitted
 * whatever the camera could see. That is a fixed cost per frame paid over a 65
 * degree view, and it is why the worst spots are the ones with the most geometry
 * within a few metres -- the pier end of beach_1 above all, where the deck, the
 * piles, the boats and the foam band all sit inside one small volume and every
 * one of them is submitted from every other corner of the map too.
 *
 * scene_set_frustum takes the same view-projection the draw is about to use and
 * scene_draw then skips any batch whose model-space AABB is wholly outside it.
 * Three things are deliberately never culled:
 *
 *   - BATCH_SKY, which is drawn under a camera-locked matrix that is not the one
 *     the frustum was built from;
 *   - any batch of a scene with a rig (the car), whose parts move under
 *     rig.draw[] and whose AABB is therefore not where the box says it is;
 *   - anything at all, if scene_set_frustum has not been called since the last
 *     scene_cull_off() -- culling is opt-in per pass, so a caller that has not
 *     thought about which matrix is loaded gets the old behaviour.
 *
 * Animated water displaces its vertices from rest by a few centimetres of swell;
 * SCENE_CULL_PAD covers that (and any other in-place animation) so a wave crest
 * cannot poke through a plane the box said was clear.
 */
#define SCENE_CULL_PAD 2.0f     /* metres of slack on every AABB */

/* `viewproj` is projection * modelview in GL's column-major order -- exactly what
   scene_frustum_from_gl() reads back out of the matrix stacks. */
void scene_set_frustum(const float viewproj[16]);

/* Read the CURRENTLY LOADED projection and modelview and use their product.
   Preferred over building the matrix by hand: a frustum that disagrees with the
   draw by even a little culls geometry that is on screen. */
void scene_frustum_from_gl(void);

/* Draw everything again, until the next scene_set_frustum. */
void scene_cull_off(void);
/* Whether a frustum is currently set. scene_draw_model has to suspend and
   restore culling, and a test cannot see that it restored without this. */
int  scene_cull_is_on(void);

/* Per-frame draw accounting, so the frame log can say WHERE the triangles went.
   scene_draw adds to these; the caller zeroes them once a frame. */
typedef struct {
    unsigned batches;           /* submitted */
    unsigned batches_culled;
    unsigned tris;              /* submitted */
    unsigned tris_culled;
    unsigned tris_keyed;        /* of `tris`, the punch-through ones */
} scene_stats_t;

void scene_stats_reset(void);
void scene_stats_get(scene_stats_t *out);

#endif
