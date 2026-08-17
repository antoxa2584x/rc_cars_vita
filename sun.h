/*
 * sun.h -- the sun disc and its lens flare.
 *
 * A registered engine subsystem in the original, named `SUN_AF`, dispatched by
 * FUN_00478cf0:
 *
 *   msg 1    FUN_00478ea0  create; calls the config loader
 *   msg 0x69 FUN_00478f60  page in the six textures
 *   msg 0x6b FUN_00478fd0  ENABLE and PLACE, per track
 *   msg 0x6c FUN_00479050  disable
 *   msg 0x68 FUN_00479070  build the disc quad
 *            FUN_004791e0  draw the disc
 *            FUN_00479250  the ghost chain, and the line-of-sight cast
 *            FUN_004797c0  draw all five sprites
 *   msg 0x85 FUN_00479b70  reload the config
 *
 * WHERE THE SUN IS. FUN_00478fd0 asks modFindName for a node of type 'MARK'
 * (0x4d41524b) whose name is the subsystem's own -- `SUN_AF` -- and takes its
 * translation (+0x90/+0x94/+0x98) as the sun's WORLD position. So the sun is
 * per-track data in the track's own .sb, and pack_vsc.py exports it as an
 * ordinary marker beside the checkpoint spine.
 *
 * It is a real world point, not a direction: 25 to 130 m out and 8 to 84 m up,
 * an elevation of 17 to 44 degrees depending on the track. It is inside the
 * world, so the disc is depth-tested against the scenery and really can go
 * behind a building.
 *
 * EIGHT of the ten tracks carry a SUN_AF. urban_1 and urban_2 do NOT, and that
 * is the original's own answer for them, not an omission: no node, so
 * FUN_00478fd0 returns with the subsystem left disabled. `sun_init` does the
 * same and `sun_enabled` reports it.
 *
 * WHAT IS DRAWN. Six sprites off five textures (FUN_00478f60's own list and
 * order -- note lens_flare_3 twice):
 *
 *   the disc    sun_disc      at the sun, world space, SizeSun 0.05
 *   ghost 1     lens_flare_1  Dist1 -0.05  Size1 0.45  200,255,201
 *   ghost 2     lens_flare_3  Dist2 -0.02  Size2 0.03  white
 *   ghost 3     lens_flare_2  Dist3 +0.04  Size3 0.10  171,255,170
 *   ghost 4     lens_flare_3  Dist4 +0.11  Size4 0.02  white
 *   the shine   shine         on the sun   Size5 0.50  white
 *
 * A ghost sits `Dist` metres from the point directly ahead of the camera
 * (eye-space (0,0,1) * (focal + CamOffset)) along the line to the sun, at that
 * depth, then projected. Dist1 and Dist2 are negated by the loader, so two
 * ghosts fall on the far side of the view centre from the sun -- which is what a
 * lens ghost does. `line_flare` is NOT part of this: it belongs to the
 * four-entry table at 0x005705d8 and this subsystem never loads it.
 *
 * THE TWO BLEND MODES ARE THE ART'S OWN, and the flags words agree with it.
 * FUN_0045c3c0 decodes a material's flags byte: bit 0x2 -> mode 2, bit 0x4 ->
 * mode 3.
 *
 *   disc,   flags 0xa0000003 -> mode 2 = SRCALPHA / INVSRCALPHA
 *   flares, flags 0xa0200005 -> mode 3 = SRCCOLOR / ONE
 *
 * and mode 3's factors are read straight off 0x0045c81e (DESTBLEND <- 2 = ONE)
 * with the src factor still holding the mode number, 3 = SRCCOLOR. The pixels
 * say the same thing independently: `sun_disc` keeps RGB at (255,255,181) all
 * the way out and ramps its ALPHA to 0, so it can only be alpha-blended; the
 * other five have alpha 255 EVERYWHERE and ramp their RGB to black, so they can
 * only be added -- alpha-blended they would paint black squares. Same reasoning
 * as the tyre marks, from the other end.
 *
 * `dst' = src*src + dst` also explains why the art is so bright: squaring a
 * glow crushes the faint halo and keeps the core, and adding it can never blow
 * out past white.
 *
 * THE LIGHT-OF-SIGHT TEST is what makes the flare an effect rather than a
 * decal. FUN_00479250 counts `TimeShootRay` (0.5 s) down and, when it expires,
 * casts a ray from the camera to the sun (FUN_00456260, flags 0x1804) and keeps
 * the answer. A four-state machine then fades the whole flare in and out over
 * `FadeTime` (0.25 s):
 *
 *   0 off        blocked -> 1
 *   1 fading in  timer out -> 3;  clear -> 2 with the timer mirrored
 *   3 on         clear -> 2
 *   2 fading out timer out -> 0;  blocked -> 1 with the timer mirrored
 *
 * State 0 returns before drawing anything, so the flare is genuinely absent
 * when the sun is behind scenery, and the mirror on a reversal means a flare
 * caught half way in fades out from where it got to rather than snapping.
 *
 * THE DISC IS NOT GATED BY IT. The original builds the disc's quad in screen
 * space but writes the sun's own projected depth into all four vertices, so the
 * depth buffer occludes it; only the flare uses the ray. This port keeps both
 * halves of that.
 *
 * THE PORT'S, and there are three:
 *
 *  - the disc is a WORLD-space camera-facing billboard rather than a screen
 *    quad carrying a hand-written depth. `SizeSun` is a screen fraction, so the
 *    world half-extent is derived from it and the distance --
 *    `half = SUN_SIZE * dist * tan(fovy/2) * aspect` -- which subtends the same
 *    angle and gets depth testing from the pipeline instead of from a float
 *    written into a vertex. Exact for a perspective projection.
 *  - the ray is `col_segment` against the collision grid, not the engine's own
 *    query over render geometry. The grid is what this port has, and it is the
 *    same trade `shadow.c` documents for its receivers.
 *  - the fade FRACTION, and where it is applied. The state machine and both its
 *    timers are recovered; the alpha the original derives from them went through
 *    a `__ftol` the decompiler lost its argument to, so linear in the timer is
 *    the port's reading -- the only shape under which the mirrored reversal
 *    above is continuous. And it scales the COLOUR rather than the alpha,
 *    because it has to: FUN_004797c0 packs the byte into the D3DCOLOR's alpha
 *    (CONCAT31/21/11 of fade,R,G,B = 0xAARRGGBB) and mode 3 is SRCCOLOR / ONE,
 *    in which the source alpha takes no part. A vertex alpha would fade nothing.
 *  - `focal`, the ghost chain's layout depth: the original's cam+0x158, which is
 *    not recovered. It is the ONE unrecovered scale here, and the caller anchors
 *    it (main.c passes 1.0 m). `Dist` is used verbatim against it because Dist
 *    already IS eye-space metres; `Size` is a screen fraction (the original
 *    multiplies it by cam+0x220) and is converted at each sprite's own depth by
 *    the same formula the disc uses, so every sprite subtends the authored angle
 *    whatever the resolution. The RATIOS among the four Dists and the five Sizes
 *    are exact either way -- only their common scale rides on `focal`.
 *
 * One thing deliberately NOT reproduced: the original's disc is square in PIXELS
 * on a 4:3 screen (half_y = half_x / (aspect * 0.75), so the 0.75 is a 4:3
 * reference) and therefore stretches horizontally on anything wider. A world
 * billboard is round at every aspect, which is what the art is.
 */
#ifndef SUN_H
#define SUN_H

#include "scene.h"
#include "col.h"

/* the marker the engine looks up, and the subsystem's own registered name */
#define SUN_MARKER_NAME "SUN_AF"

/* FUN_00479250's four fade states, in its own numbering */
enum { SUN_OFF = 0, SUN_FADE_IN = 1, SUN_FADE_OUT = 2, SUN_ON = 3 };

typedef struct {
    int   enabled;          /* 0 when the track carries no SUN_AF marker */
    float pos[3];           /* the marker's world position */

    int   state;            /* SUN_OFF .. SUN_ON */
    float timer;            /* the fade timer, seconds */
    float ray_timer;        /* counts down to the next line-of-sight cast */
    int   clear;            /* last cast: nonzero = the sun is in view */

    GLuint tex_disc;
    GLuint tex_flare[5];    /* lens_flare_1, _3, _2, _3, shine */

    /* telemetry, for the harnesses and the log */
    int   n_casts;
    float alpha;            /* the fade fraction the last step produced */
} sun_t;

/* Bind to a track. Finds SUN_AF among the scene's markers and resolves the five
   textures out of `src` (the scene they were packed into, i.e. the track's).
   Leaves the struct disabled and drawing nothing if either is missing. */
void sun_init(sun_t *s, const scene_t *src);

/* One frame. `eye` is the camera position; `col` may be NULL, in which case the
   sun counts as always in view (which is what a harness with no world gets). */
void sun_step(sun_t *s, const col_t *col, const float eye[3], float dt);

/* Draw. `right`/`up` are the view basis, the same pair fx_draw takes.
 * `fovy_deg` and `aspect` turn a screen fraction into a world extent for every
 * sprite. `focal` is the eye-space depth the ghost chain is laid out at -- the
 * original's cam+0x158, the one scale here that is NOT recovered.
 *
 * The disc first (depth-tested, alpha-blended), then the ghosts (additive, depth
 * test off). All of them world-space camera-facing billboards, which is why no
 * screen-space pass and no matrix juggling appear: FUN_00479250 places each ghost
 * at an EYE-SPACE point and hands it to the ordinary projection, so a billboard
 * at that same point projects to the same pixels. Restores everything it
 * touches. */
void sun_draw(const sun_t *s, const float eye[3],
              const float right[3], const float up[3],
              float fovy_deg, float aspect, float focal);

#endif /* SUN_H */
