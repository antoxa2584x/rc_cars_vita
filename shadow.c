/*
 * shadow.c -- the car's projected shadow. See shadow.h for the original it
 * follows (shdAddSource type 3, driven by FUN_005072f0).
 *
 * The projection is straight down. That is not an assumption: the source's
 * matrix is a TEXTURE matrix, and FUN_005072f0 builds it as a rotation about
 * (0.5, 0.5) by the car's heading -- a 2D spin of the image in the ground
 * plane, with no perspective and no light direction anywhere in it. The
 * CarLight azimuth/elevation feed the body's vertex lighting (carVisualBody
 * 0x005077e0), not the shadow.
 */

#include "shadow.h"
#include "vis_data.h"

#include <math.h>
#include <string.h>

/* The shadow only ever covers a 2*ShadowSize square, which is 0.72 m for the
   Overkill -- a couple of collision cells. 64 receivers is far more than that
   ever produces; the cap is a backstop, not a budget. */
#define SHADOW_MAX_TRIS 64

/* Surfaces steeper than this are walls, not floors: projecting a top-down
   image onto them smears it into a vertical streak. */
#define SHADOW_MIN_UP 0.30f

/* Lift off the receiver so the decal wins the depth test against the surface
   it is lying on. Along the surface normal, not along world up -- on the 23
   degree spawn slope those differ by 8%, and the shallow end z-fights. */
#define SHADOW_LIFT 0.012f

/* How far below the car to look for a receiver. Bounded so the shadow does not
   fall through the pier onto the seabed while the car is airborne. */
#define SHADOW_DEPTH 1.5f

void shadow_init(shadow_t *sh, const scene_t *src, int car)
{
    static const float size[3] = SHADOW_SIZE;
    static const float shift[3] = SHADOW_SHIFT;

    memset(sh, 0, sizeof(*sh));
    if (car < 0 || car > 2)
        car = 0;
    sh->tex = src ? scene_tex(src, "__shadow") : 0;
    sh->size = size[car];
    if (src && src->shadow_radius > sh->size)
        sh->size = src->shadow_radius;
    sh->shift = shift[car];
    sh->density = (float)SHADOW_DENSITY / 255.f;
    sh->enabled = (sh->tex != 0);
}

/*
 * cx, cz, cy: the shadow centre, already shifted along the body's forward.
 * fx, fz:     the body's forward, flattened to the ground plane and unit.
 */
static void shadow_project(shadow_t *sh, const col_t *col,
                           float cx, float cy, float cz, float fx, float fz)
{
    static float tris[SHADOW_MAX_TRIS * 9];
    static vtx_t verts[SHADOW_MAX_TRIS * 3];
    /* model +X is LEFT and +Z is forward (see CLAUDE.md, "Car axes"), so the
       left direction in the ground plane is forward turned a quarter turn this
       way. Baked and sampled with the same frame, which is what makes the
       silhouette line up with the car above it. */
    const float lx = fz, lz = -fx;
    const float inv = 1.f / (2.f * sh->size);
    int n, i, k, nv = 0;

    n = col_gather(col, cx, cy, cz, sh->size, SHADOW_DEPTH,
                   tris, SHADOW_MAX_TRIS);
    sh->n_tris = 0;
    if (!n)
        return;

    for (i = 0; i < n; i++) {
        const float *t = &tris[i * 9];
        float ux = t[3] - t[0], uy = t[4] - t[1], uz = t[5] - t[2];
        float vx = t[6] - t[0], vy = t[7] - t[1], vz = t[8] - t[2];
        float nx = uy * vz - uz * vy;
        float ny = uz * vx - ux * vz;
        float nz = ux * vy - uy * vx;
        float len = sqrtf(nx * nx + ny * ny + nz * nz);

        if (len < 1e-9f)
            continue;
        nx /= len; ny /= len; nz /= len;
        if (ny < 0.f) { nx = -nx; ny = -ny; nz = -nz; }
        if (ny < SHADOW_MIN_UP)
            continue;

        for (k = 0; k < 3; k++) {
            float px = t[k * 3], py = t[k * 3 + 1], pz = t[k * 3 + 2];
            float dx = px - cx, dz = pz - cz;
            vtx_t *o = &verts[nv++];
            o->x = px + nx * SHADOW_LIFT;
            o->y = py + ny * SHADOW_LIFT;
            o->z = pz + nz * SHADOW_LIFT;
            o->u = 0.5f + (dx * lx + dz * lz) * inv;
            o->v = 0.5f + (dx * fx + dz * fz) * inv;
        }
    }
    if (!nv)
        return;
    sh->n_tris = nv / 3;

    glBindTexture(GL_TEXTURE_2D, sh->tex);
    /* The world draws with GL_ALPHA_TEST at 0.5; the shadow's peak alpha is
       ShadowDensity/255 = 0.13, so leaving it on discards every fragment. */
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    /* Collision triangles carry no consistent winding -- half of any patch is
       backfacing and would vanish. */
    glDisable(GL_CULL_FACE);
    /* Black at ShadowDensity: with GL_MODULATE the texture's RGB (also black)
       leaves the colour alone and its alpha carries the silhouette. */
    glColor4f(0.f, 0.f, 0.f, sh->density);

    glVertexPointer(3, GL_FLOAT, sizeof(vtx_t), &verts[0].x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(vtx_t), &verts[0].u);
    glDrawArrays(GL_TRIANGLES, 0, nv);

    glColor4f(1.f, 1.f, 1.f, 1.f);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);
}

void shadow_draw(shadow_t *sh, const col_t *col, const float *m)
{
    float fx, fz, len;

    if (!sh->enabled)
        return;
    /* row-major row-vector: row 2 is the body's +Z, row 3 the translation */
    fx = m[8];
    fz = m[10];
    len = sqrtf(fx * fx + fz * fz);
    if (len < 1e-4f) {
        /* nose straight up or down; fall back to the body's +X so the frame
           stays defined rather than exploding */
        fx = m[2];
        fz = -m[0];
        len = sqrtf(fx * fx + fz * fz);
        if (len < 1e-4f) { fx = 0.f; fz = 1.f; len = 1.f; }
    }
    fx /= len;
    fz /= len;
    shadow_project(sh, col,
                   m[12] + m[8] * sh->shift,
                   m[13] + m[9] * sh->shift,
                   m[14] + m[10] * sh->shift,
                   fx, fz);
}

void shadow_draw_yaw(shadow_t *sh, const col_t *col,
                     float x, float y, float z, float yaw_deg)
{
    /* the placeholder model's convention: forward = (sin yaw, 0, -cos yaw) */
    float r = yaw_deg * (float)(M_PI / 180.0);
    float fx = sinf(r), fz = -cosf(r);

    if (!sh->enabled)
        return;
    shadow_project(sh, col, x + fx * sh->shift, y, z + fz * sh->shift, fx, fz);
}
