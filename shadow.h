/*
 * shadow.h -- the car's projected shadow.
 *
 * The game's shadow module is shdInit/shdInitMap/shdAddSource (0x0044c410..).
 * A source is a position, a radius, a type and, for type 3, a texture plus a
 * 4x4 TEXTURE matrix; shdRender (0x0044c750) walks the map's face lists, marks
 * every face inside the radius, and re-draws those faces with the source's
 * texture projected onto them.
 *
 * FUN_005072f0 is the car's own call into it, and it is short enough to quote:
 *
 *     if (!Video/VIDEO_CarShadow)             return;
 *     if (car is not a player car)            return;
 *     fwd  = row2(carMatrix)                  the body's +Z
 *     ang  = angle(ref, cross(K, row0))       the heading, from the body's +X
 *     tex  = rotate about (0.5, 0.5) by base[car] - ang
 *     pos  = row3(carMatrix) + fwd * ShadowShift[car]
 *     shdAddSource(0, pos, ShadowSize[car], 0xffffffff, 3, 0, tex[car], texmat)
 *
 * so: a disc of radius ShadowSize centred ShadowShift ahead of the body origin,
 * with the car's own top-down image spun to the car's heading. That is what
 * this file draws.
 *
 * Two divergences, both marked at the point of use:
 *   - the receiver faces come from the COLLISION mesh, because the port has no
 *     per-object render-face list to mark;
 *   - the projected texture is baked once by pack_vsc.py instead of being
 *     re-rendered from above every FrameSkipNmb+1 frames.
 */

#ifndef SHADOW_H
#define SHADOW_H

#include "col.h"
#include "scene.h"

typedef struct {
    GLuint tex;
    /* max(ShadowSize[car], the radius the texture was baked over).
     *
     * ShadowSize alone is not enough. shdAddSource's radius is how far out the
     * source marks receiving faces, and the texture's own scale lives in the
     * 4x4 the engine passes alongside it -- which is not recovered. Fitting the
     * texture to the car and taking the larger of the two keeps the shadow the
     * size of the car it belongs to, which ShadowSize does not do for the
     * Buggy: its 0.29 is shorter than the car's own 0.298 half-length. */
    float size;
    float shift;        /* ShadowShift[car], metres along the body +Z */
    float density;      /* ShadowDensity / 255 */
    int enabled;
    int n_tris;         /* receivers gathered last frame, for telemetry */
} shadow_t;

/* `car` is 0-based (0 = Overkill), matching rb_car.car_index. `src` is the
   scene the '__shadow' texture was packed into -- the car's. */
void shadow_init(shadow_t *sh, const scene_t *src, int car);

/* Draw the shadow under a body whose world matrix is `m` (the engine's
   row-major row-vector layout, exactly what rbcar_matrix returns). */
void shadow_draw(shadow_t *sh, const col_t *col, const float *m);

/* Same, for the placeholder model, which has a position and a yaw but no
   matrix. `yaw` is in the renderer's convention: forward = (sin y, 0, -cos y). */
void shadow_draw_yaw(shadow_t *sh, const col_t *col,
                     float x, float y, float z, float yaw_deg);

#endif
