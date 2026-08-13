/*
 * envmap.c -- the car body's plastic glance. See envmap.h for what came out of
 * the engine and what is the port's.
 */

#include "envmap.h"
#include "fx_data.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Big enough for the largest env batch on any of the three cars: the Buggy's
   body is the worst at a few hundred vertices. Sized generously because running
   out silently would drop part of the glance. */
#define ENV_MAX_VERTS 4096

float envmap_alpha(unsigned int env_class)
{
    /* The scale is the sky's share of a real environment map -- the port reflects
       a sky-only image where the engine reflects a render of everything. GRE1 is
       glass and chrome and is left alone. See ENVMAP_SKY_ONLY_FRAC. */
    switch (env_class) {
    case ENV_BODY:     return (float)ENVMAP_BODY / 255.f * ENVMAP_SKY_ONLY_FRAC;
    case ENV_GRE1:     return (float)ENVMAP_GRE1 / 255.f;
    case ENV_GRE2:     return (float)ENVMAP_GRE2 / 255.f * ENVMAP_SKY_ONLY_FRAC;
    case ENV_UPGRADES: return (float)ENVMAP_UPGRADES / 255.f * ENVMAP_SKY_ONLY_FRAC;
    default:           return 0.f;
    }
}

void envmap_init(envmap_t *e, const scene_t *track)
{
    unsigned int i;

    memset(e, 0, sizeof(*e));
    if (!track)
        return;
    /* The sky the level actually uses, taken from the batch the packer flagged
       BATCH_SKY rather than from a texture name -- the ten tracks do not agree
       on one (sky_up, sky_up_c, sky_up_c2, sky_up_u). */
    for (i = 0; i < track->n_batches; i++) {
        if ((track->batches[i].flags & BATCH_SKY) && track->batches[i].gl_tex) {
            e->tex = track->batches[i].gl_tex;
            break;
        }
    }
    e->enabled = (e->tex != 0);
}

void envmap_draw(envmap_t *e, const scene_t *car, const float n3[9])
{
    /* Heap, not BSS: a car body batch can pass the 32 KB (1,170-vertex) mark at
       which vitaGL's SAFER_DRAW_SPEEDHACK hands GXM this pointer instead of
       copying it, and only the newlib heap is mapped for the GPU. The reasoning
       in full is on the same buffer in fx.c. */
    static vtx_t *v;
    unsigned int i;

    e->n_batches = 0;
    e->n_tris = 0;
    if (!e->enabled || !car)
        return;
    if (!v)
        v = malloc(sizeof(*v) * ENV_MAX_VERTS);
    if (!v)
        return;

    glBindTexture(GL_TEXTURE_2D, e->tex);
    /* Lerp toward the reflection: that is what a material colour of white with
       an alpha means once the stage is blended, and it is why the four alphas
       differ -- glass reflects completely, paint about 44%. */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    /* Same geometry, same depth: it must pass its own depth test and must not
       write, or the parts drawn after it lose theirs. */
    glDepthMask(GL_FALSE);
    /* Off, and it makes no difference either way -- which is worth saying rather
       than leaving to be rediscovered. The cut-out regions of a body are cut out
       by the BASE texture's alpha, and the base texture is not bound in this
       pass; the sky is, and it is opaque everywhere. So the test at 0.5 would
       keep every fragment too, and the glance covers a batch's cut-out holes
       along with its solid parts. Fixing that needs the base texture bound
       alongside the sky with the alpha taken from one and the colour from the
       other, which is a texture-combiner this fixed-function path does not have.
       It shows on the Buggy's roll cage and on nothing else. */
    glDisable(GL_ALPHA_TEST);

    for (i = 0; i < car->n_batches; i++) {
        const batch_t *b = &car->batches[i];
        const float *m3 = n3;
        float part[9];
        unsigned int k;
        int rigged;
        float a;

        if (!b->env || !b->nrm || b->nidx == 0)
            continue;
        if (b->nverts > ENV_MAX_VERTS)
            continue;
        a = envmap_alpha(b->env);
        if (a <= 0.f)
            continue;

        /* A rigged part is drawn under its own matrix, so its normals need the
           same rotation on the way to view space. Springs are GRE1 and they are
           rigged, so this is not hypothetical. */
        rigged = car->has_rig && b->part > 0 && (int)b->part < car->rig.n;
        if (rigged) {
            const float *pm = car->rig.draw[b->part];
            int r, c, t;
            for (r = 0; r < 3; r++)
                for (c = 0; c < 3; c++) {
                    float s = 0.f;
                    for (t = 0; t < 3; t++)
                        s += pm[r * 4 + t] * n3[t * 3 + c];
                    part[r * 3 + c] = s;
                }
            m3 = part;
        }

        for (k = 0; k < b->nverts; k++) {
            const float *n = &b->nrm[k * 3];
            float nx = n[0] * m3[0] + n[1] * m3[3] + n[2] * m3[6];
            float ny = n[0] * m3[1] + n[1] * m3[4] + n[2] * m3[7];
            float nz = n[0] * m3[2] + n[1] * m3[5] + n[2] * m3[8];
            float len = sqrtf(nx * nx + ny * ny + nz * nz);
            if (len > 1e-9f) { nx /= len; ny /= len; nz /= len; }
            (void)nz;
            v[k] = b->verts[k];
            /* The sphere map. v is FLIPPED because the texture's own v runs down
               the image while +y in view space runs up -- the same flip ui.c's
               ortho makes, and getting it wrong slides the highlight the wrong
               way as the car pitches. */
            v[k].u = 0.5f + nx * 0.5f;
            v[k].v = 0.5f - ny * 0.5f;
        }

        if (rigged) {
            glPushMatrix();
            glMultMatrixf(car->rig.draw[b->part]);
        }
        glColor4f(1.f, 1.f, 1.f, a);
        glVertexPointer(3, GL_FLOAT, sizeof(vtx_t), &v[0].x);
        glTexCoordPointer(2, GL_FLOAT, sizeof(vtx_t), &v[0].u);
        glDrawElements(GL_TRIANGLES, b->nidx, GL_UNSIGNED_SHORT, b->idx);
        if (rigged)
            glPopMatrix();
        e->n_batches++;
        e->n_tris += (int)(b->nidx / 3);
    }

    glColor4f(1.f, 1.f, 1.f, 1.f);
    glEnable(GL_ALPHA_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}
