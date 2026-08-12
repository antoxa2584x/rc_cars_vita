/*
 * scene.c -- .vsc loading and drawing.
 *
 * Lifted out of main.c unchanged except for the version handling (VSC5 adds
 * markers), the texture-name table, and scene_draw's flag filter. See scene.h
 * for the format and pack_vsc.py for the writer.
 */

#include "scene.h"

#include "rlog.h"

#include <stddef.h>      /* offsetof, for the vertex-buffer attribute offsets */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }

/* How many mip levels to drop off the top of every texture -- see scene.h. */
static int tex_skip;

void scene_set_tex_quality(int skip_levels)
{
    if (skip_levels < 0)
        skip_levels = 0;
    if (skip_levels > SCENE_TEX_QUALITY_LEVELS - 1)
        skip_levels = SCENE_TEX_QUALITY_LEVELS - 1;
    tex_skip = skip_levels;
}

int scene_tex_quality(void) { return tex_skip; }

/* Exchange the two 5-bit fields of every 565 texel at upload -- see scene.h. */
static int tex_swap_rb;

void scene_set_tex_swap_rb(int on) { tex_swap_rb = on ? 1 : 0; }
int  scene_tex_swap_rb(void)       { return tex_swap_rb; }

static void swap_rb565(void *p, unsigned int n)
{
    unsigned short *v = (unsigned short *)p;
    for (unsigned int i = 0; i < n; i++)
        v[i] = (unsigned short)(((v[i] & 0x001Fu) << 11)
                                | (v[i] & 0x07E0u)
                                | (v[i] >> 11));
}

/* Names in the file are length-prefixed and unterminated; the fixed slots here
   are short on purpose. Copy what fits and terminate -- strncpy would warn, and
   would not terminate on a name exactly `cap` long. */
static void copy_name(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n > cap - 1)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

int scene_load(const char *path, scene_t *s)
{
    FILE *f = fopen(path, "rb");
    if (!f) { rlog("[rccars] cannot open %s\n", path); return 0; }

    char magic[4];
    unsigned int n_parts = 0;
    int ver;

    memset(s, 0, sizeof(*s));
    if (!rd(f, magic, 4) || memcmp(magic, "VSC", 3)
        || magic[3] < '3' || magic[3] > '8') {
        rlog("[rccars] %s: bad magic\n", path);
        fclose(f);
        return 0;
    }
    ver = magic[3] - '0';
    rd(f, &s->n_tex, 4);
    rd(f, &s->n_batches, 4);
    if (ver >= 4)
        rd(f, &n_parts, 4);
    if (ver >= 5) {
        rd(f, &s->n_markers, 4);
        rd(f, &s->shadow_radius, 4);
    }
    /* VSC8: a table of named models sharing one file, and a model index per
       batch. Only props.vsc has any -- a track or a car writes 0 here and no
       per-batch field, which is what keeps every existing asset loading. */
    if (ver >= 8) {
        rd(f, &s->n_models, 4);
        if (s->n_models) {
            s->model_names = calloc(s->n_models, SCENE_TEX_NAME);
            for (unsigned int i = 0; i < s->n_models; i++) {
                unsigned short nlen;
                char name[256];
                rd(f, &nlen, 2);
                if (nlen > 255) nlen = 255;
                rd(f, name, nlen);
                name[nlen] = 0;
                copy_name(s->model_names[i], SCENE_TEX_NAME, name);
            }
        }
    }
    s->has_rig = (n_parts > 0);

    size_t up_bytes = 0;         /* texture memory actually uploaded */
    size_t buf_bytes = 0;        /* vertex + index memory put on the GPU */
    unsigned int n_buffered = 0; /* batches that got one */
    /* Per texture: would the alpha test reject any of its texels? Drives
       BATCH_ALPHA_KEYED -- see scene.h for why this is worth knowing. */
    unsigned char *keyed = calloc(s->n_tex ? s->n_tex : 1, 1);
    /* And did it upload anything at all? A declared texture whose image is
       missing gets an id from glGenTextures and never a glTexImage2D, which
       leaves the sampler undefined -- see BATCH_NO_TEXTURE. */
    unsigned char *has_px = calloc(s->n_tex ? s->n_tex : 1, 1);
    s->tex_ids = calloc(s->n_tex, sizeof(GLuint));
    s->tex_names = calloc(s->n_tex ? s->n_tex : 1, SCENE_TEX_NAME);
    glGenTextures(s->n_tex, s->tex_ids);

    for (unsigned int i = 0; i < s->n_tex; i++) {
        unsigned short nlen;
        rd(f, &nlen, 2);
        char name[256];
        if (nlen > 255) nlen = 255;
        rd(f, name, nlen);
        name[nlen] = 0;
        copy_name(s->tex_names[i], SCENE_TEX_NAME, name);

        unsigned short w, h;
        unsigned char fmt, mips;
        rd(f, &w, 2); rd(f, &h, 2); rd(f, &fmt, 1); rd(f, &mips, 1);

        unsigned int bpp = (fmt == 1) ? 4 : 2;
        void *px = malloc((size_t)w * h * bpp);      /* level 0 is largest */
        glBindTexture(GL_TEXTURE_2D, s->tex_ids[i]);

        /* Texture quality: drop this many levels off the TOP of the chain, so
           the 256 px or 128 px copy becomes GL level 0. See scene.h. Never drop
           the whole chain -- a texture with fewer levels keeps its smallest. */
        unsigned int skip = (unsigned int)tex_skip;
        if (skip > mips - 1u)
            skip = mips - 1u;

        /*
         * Ship the .csi's own mip chain; without it distant terrain aliases.
         *
         * ONLY LEVEL `skip` IS ACTUALLY UPLOADED, and that is not a shortcut --
         * it is what vitaGL does with the rest. glTexImage2D with level != 0 on
         * an uncompressed format ignores its pixel pointer entirely and calls
         * gpu_alloc_mipmaps(level), which box-downscales the chain out of level 0
         * with sceGxmTransferDownscale (textures.c:922). So the authored mips
         * were never reaching the GPU on hardware. Worse, that call regenerates
         * levels 0..n EVERY time, so handing it ten levels one at a time is ~45
         * downscales per texture instead of 9 -- pure load time, paid again on
         * every track change and every quality change.
         *
         * glGenerateMipmap does the same generation in one pass, so the result is
         * byte-identical and the load is not quadratic. The remaining levels are
         * still READ, because the file offsets depend on it.
         */
        for (unsigned int lvl = 0; lvl < mips; lvl++) {
            unsigned int lw = w >> lvl, lh = h >> lvl;
            if (!lw) lw = 1;
            if (!lh) lh = 1;
            /* The skipped levels still have to be READ, or the stream goes out
               of step and every batch after this texture is garbage. */
            rd(f, px, (size_t)lw * lh * bpp);
            /* Judged on level 0, which is the one the alpha-keyed art is authored
               at; a mip of a cut-out keeps its transparent region. */
            if (lvl == 0 && fmt == 1) {
                const unsigned char *a = (const unsigned char *)px;
                for (unsigned int t = 0; t < lw * lh; t++)
                    if (a[t * 4 + 3] <= 127) { keyed[i] = 1; break; }
            }
            if (lvl != skip)
                continue;
            up_bytes += (size_t)lw * lh * bpp;
            has_px[i] = 1;
            /* Only the 565 path: the RGBA one uploads byte-order r,g,b,a, which
               GXM's U8U8U8U8_ABGR reads correctly on both hardware and Vita3K. */
            if (fmt != 1 && tex_swap_rb)
                swap_rb565(px, lw * lh);
            if (fmt == 1)
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, lw, lh, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, px);
            else
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, lw, lh, 0,
                             GL_RGB, GL_UNSIGNED_SHORT_5_6_5, px);
        }
        /* The chain, in one call -- see the comment on the loop. Same condition
           as the filter choice below, so a single-level texture (the lightmap
           atlases ship that way) still gets neither. */
        if ((mips - skip) > 1)
            glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        (mips - skip) > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        /* The sprite textures (shadow, foam, checkpoint arrows) are drawn well
           outside [0,1] by design, and their border is transparent -- REPEAT
           would tile a second car shadow next to the first. */
        int wrap = (name[0] == '_' || !strncmp(name, "cp_ar", 5)
                    || !strncmp(name, "water_wave", 10))
                   ? GL_CLAMP_TO_EDGE : GL_REPEAT;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
        free(px);
    }

    /* The rig sits between the textures and the batches. Reading it here keeps
       the stream in step even if there are more parts than carani can hold. */
    if (ver >= 4)
        carani_read_parts(&s->rig, f, n_parts);

    if (s->n_markers) {
        s->markers = calloc(s->n_markers, sizeof(marker_t));
        for (unsigned int i = 0; i < s->n_markers; i++) {
            unsigned short nlen;
            char name[256];
            rd(f, &nlen, 2);
            if (nlen > 255) nlen = 255;
            rd(f, name, nlen);
            name[nlen] = 0;
            copy_name(s->markers[i].name, SCENE_MARKER_NAME, name);
            rd(f, &s->markers[i].x, 4);
            rd(f, &s->markers[i].y, 4);
            rd(f, &s->markers[i].z, 4);
            rd(f, &s->markers[i].yaw, 4);
        }
    }

    s->batches = calloc(s->n_batches, sizeof(batch_t));
    for (unsigned int i = 0; i < s->n_batches; i++) {
        batch_t *b = &s->batches[i];
        rd(f, &b->tex, 4);
        rd(f, &b->flags, 4);
        if (ver >= 4)
            rd(f, &b->part, 4);
        b->lm_tex = 0xFFFFFFFFu;
        if (ver >= 6)
            rd(f, &b->lm_tex, 4);
        if (ver >= 7)
            rd(f, &b->env, 4);
        if (ver >= 8)
            rd(f, &b->model, 4);
        rd(f, &b->nverts, 4);
        rd(f, &b->nidx, 4);
        b->verts = malloc(sizeof(vtx_t) * b->nverts);
        b->idx = malloc(sizeof(unsigned short) * b->nidx);
        if (ver >= 6) {
            rd(f, b->verts, sizeof(vtx_t) * b->nverts);
        } else {
            /* older files carry 5 floats per vertex, not 7 */
            unsigned int v;
            for (v = 0; v < b->nverts; v++) {
                rd(f, &b->verts[v], 5 * sizeof(float));
                b->verts[v].lu = b->verts[v].lv = 0.f;
            }
        }
        /* Normals sit between the vertices and the indices, and ONLY on an
           env-mapped batch. Reading them unconditionally would put the stream
           out of step on every other batch in the file. */
        if (ver >= 7 && b->env) {
            b->nrm = malloc(sizeof(float) * 3 * b->nverts);
            if (b->nrm)
                rd(f, b->nrm, sizeof(float) * 3 * b->nverts);
            else
                fseek(f, (long)(sizeof(float) * 3 * b->nverts), SEEK_CUR);
        }
        rd(f, b->idx, sizeof(unsigned short) * b->nidx);
        b->gl_tex = (b->tex < s->n_tex) ? s->tex_ids[b->tex] : 0;
        b->gl_lm = (b->lm_tex < s->n_tex) ? s->tex_ids[b->lm_tex] : 0;
        /* Only this batch's own texture decides it. The lightmap cannot: it is
           opaque everywhere, and on unit 1 the alpha comes from unit 0 anyway --
           the same fact that makes the one-pass lightmap correct.
         *
         * FAIL SAFE. A batch is put in the cheap no-alpha-test pass only when its
         * texture is present AND proven to have no rejectable texel. Anything
         * else keeps the test, because the cost of being wrong in that direction
         * is a lost cut-out and the cost the other way is a little speed. */
        if (b->tex >= s->n_tex || !has_px[b->tex]) {
            b->flags |= BATCH_NO_TEXTURE | BATCH_ALPHA_KEYED;
        } else if (keyed[b->tex]) {
            b->flags |= BATCH_ALPHA_KEYED;
        }

        /* The culling box. Padded here rather than at the test so the slack is
           part of the batch's own description -- water animates its vertices in
           place and the box has to cover the swell. */
        {
            unsigned int v;
            b->bmin[0] = b->bmin[1] = b->bmin[2] = 1e30f;
            b->bmax[0] = b->bmax[1] = b->bmax[2] = -1e30f;
            for (v = 0; v < b->nverts; v++) {
                const float p[3] = { b->verts[v].x, b->verts[v].y, b->verts[v].z };
                int k;
                for (k = 0; k < 3; k++) {
                    if (p[k] < b->bmin[k]) b->bmin[k] = p[k];
                    if (p[k] > b->bmax[k]) b->bmax[k] = p[k];
                }
            }
            if (b->nverts) {
                int k;
                for (k = 0; k < 3; k++) {
                    b->bmin[k] -= SCENE_CULL_PAD;
                    b->bmax[k] += SCENE_CULL_PAD;
                }
            }
        }

        /* Onto the GPU, unless water animates it -- see SCENE VERTEX BUFFERS in
           scene.h for the measurement that made this worth doing and for why the
           water batches are the exception. A failed glGenBuffers leaves the ids
           at 0, which draw_pass reads as "client pointers", so running out of
           buffer memory costs speed and not the frame. */
        if (b->nverts && b->nidx && !(b->flags & BATCH_ANY_WATER)) {
            glGenBuffers(1, &b->gl_vbo);
            glGenBuffers(1, &b->gl_ibo);
            if (b->gl_vbo && b->gl_ibo) {
                glBindBuffer(GL_ARRAY_BUFFER, b->gl_vbo);
                glBufferData(GL_ARRAY_BUFFER,
                             (GLsizeiptr)(sizeof(vtx_t) * b->nverts),
                             b->verts, GL_STATIC_DRAW);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b->gl_ibo);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                             (GLsizeiptr)(sizeof(unsigned short) * b->nidx),
                             b->idx, GL_STATIC_DRAW);
                buf_bytes += sizeof(vtx_t) * b->nverts
                           + sizeof(unsigned short) * b->nidx;
                n_buffered++;
            }
        }
    }
    /* Never leave one bound: every other module in the port draws from client
       pointers, which a bound buffer silently reinterprets as offsets. */
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    fclose(f);
    unsigned int n_env = 0;
    for (unsigned int i = 0; i < s->n_batches; i++)
        if (s->batches[i].env)
            n_env++;
    rlog("[rccars] %s: VSC%d  %u textures, %u batches, %u rig parts, "
                  "%u markers, %u glance\n", path, ver, s->n_tex, s->n_batches,
                  s->has_rig ? (unsigned)s->rig.n : 0u, s->n_markers, n_env);
    /* up_bytes is the base levels only; the generated chain adds about a third
       on top of that. It is the number that moves with the quality setting,
       which is what this line is for. */
    rlog("[rccars]   texture quality %d (top %d mip level(s) skipped), "
                  "%u KB of base levels uploaded (+~33%% for the generated chain)\n",
                  tex_skip, tex_skip, (unsigned)(up_bytes / 1024));
    {
        unsigned int nk = 0, tk = 0, to = 0, nn = 0, tn = 0;
        for (unsigned int i = 0; i < s->n_batches; i++) {
            unsigned int tris = s->batches[i].nidx / 3;
            if (s->batches[i].flags & BATCH_NO_TEXTURE) {
                nn++;
                tn += tris;
            } else if (s->batches[i].flags & BATCH_ALPHA_KEYED) {
                nk++;
                tk += tris;
            } else {
                to += tris;
            }
        }
        rlog("[rccars]   alpha test: %u/%u batches need it (%u tris); "
                      "%u tris draw opaque\n", nk, s->n_batches, tk, to);
        if (nn)
            rlog("[rccars]   %u batch(es), %u tris have NO TEXTURE and "
                          "are not drawn -- a packing gap, see BATCH_NO_TEXTURE\n",
                          nn, tn);
    }
    /* The count that matters is how many did NOT get a buffer: those still cost
       a full copy of their vertices per attribute per frame. */
    rlog("[rccars]   vertex buffers: %u/%u batches on the GPU (%u KB), "
                  "%u drawing from main memory\n",
                  n_buffered, s->n_batches, (unsigned)(buf_bytes / 1024),
                  s->n_batches - n_buffered);
    free(keyed);
    free(has_px);
    return 1;
}

/*
 * The lightmap goes on texture unit 1, modulated with the base texture, in ONE
 * pass.
 *
 * Not two passes with a DST_COLOR blend, which is the other obvious way to do
 * this on fixed function. The alpha test is on globally for the alpha-keyed
 * foliage and signage, and in a second pass the alpha comes from whatever is
 * bound -- the lightmap atlas, which is opaque everywhere. Every cut-out region
 * would pass the test and multiply the BACKGROUND behind the leaves by the
 * lightmap, ringing every tree with a darkened rectangle. On unit 1 the alpha
 * still comes from unit 0 and the cut-outs stay cut out.
 */
static int lm_on;

/* Where a vertex component lives, as the argument gl*Pointer wants: a real
   address when the batch draws from main memory, a byte offset into the bound
   buffer when it does not. GL uses the same parameter for both. */
#define VTX_AT(b, field) \
    ((b)->gl_vbo ? (const void *)(size_t)offsetof(vtx_t, field) \
                 : (const void *)&(b)->verts[0].field)

static void lm_bind(GLuint tex, const batch_t *b)
{
    if (tex) {
        glActiveTexture(GL_TEXTURE1);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glBindTexture(GL_TEXTURE_2D, tex);
        glClientActiveTexture(GL_TEXTURE1);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, sizeof(vtx_t), VTX_AT(b, lu));
        glClientActiveTexture(GL_TEXTURE0);
        glActiveTexture(GL_TEXTURE0);
        lm_on = 1;
    } else if (lm_on) {
        glActiveTexture(GL_TEXTURE1);
        glDisable(GL_TEXTURE_2D);
        glClientActiveTexture(GL_TEXTURE1);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glClientActiveTexture(GL_TEXTURE0);
        glActiveTexture(GL_TEXTURE0);
        lm_on = 0;
    }
}

/* ---------------------------------------------------------------- culling */

/* Six clip planes, inward-facing, in model space. Valid only while cull_on. */
static float cull_plane[6][4];
static int cull_on;
static scene_stats_t stats;

void scene_cull_off(void) { cull_on = 0; }
int  scene_cull_is_on(void) { return cull_on; }

void scene_set_frustum(const float m[16])
{
    /* clip = M * v with M in GL column-major order, so element (row r, col c) is
       m[c*4 + r] and each plane is a sum or difference of two ROWS. Left is
       w + x >= 0, right w - x, bottom w + y, top w - y, near w + z, far w - z. */
    static const int SGN[6] = { +1, -1, +1, -1, +1, -1 };
    static const int ROW[6] = {  0,  0,  1,  1,  2,  2 };
    int p, c;

    for (p = 0; p < 6; p++) {
        for (c = 0; c < 4; c++)
            cull_plane[p][c] = m[c * 4 + 3] + (float)SGN[p] * m[c * 4 + ROW[p]];
    }
    cull_on = 1;
}

void scene_frustum_from_gl(void)
{
    float proj[16], mv[16], vp[16];
    int r, c, k;

    glGetFloatv(GL_PROJECTION_MATRIX, proj);
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    /* vp = proj * mv, both column-major: vp(r,c) = sum_k proj(r,k) * mv(k,c) */
    for (c = 0; c < 4; c++)
        for (r = 0; r < 4; r++) {
            float sum = 0.f;
            for (k = 0; k < 4; k++)
                sum += proj[k * 4 + r] * mv[c * 4 + k];
            vp[c * 4 + r] = sum;
        }
    scene_set_frustum(vp);
}

/* Outside if the box's most-positive corner along a plane's normal is still on
   the negative side of it. One-sided: a "visible" answer may be a false
   positive at the corners, which costs a draw and never a missing triangle. */
static int culled(const batch_t *b)
{
    int p;
    for (p = 0; p < 6; p++) {
        const float *n = cull_plane[p];
        float d = n[0] * (n[0] > 0.f ? b->bmax[0] : b->bmin[0])
                + n[1] * (n[1] > 0.f ? b->bmax[1] : b->bmin[1])
                + n[2] * (n[2] > 0.f ? b->bmax[2] : b->bmin[2])
                + n[3];
        if (d < 0.f)
            return 1;
    }
    return 0;
}

void scene_stats_reset(void) { memset(&stats, 0, sizeof(stats)); }
void scene_stats_get(scene_stats_t *out) { *out = stats; }

/* One pass over the batches, drawing only those whose keyed-ness matches. */
static void draw_pass(const scene_t *s, unsigned int mask, unsigned int match,
                      int want_keyed, int only_model)
{
    for (unsigned int i = 0; i < s->n_batches; i++) {
        batch_t *b = &s->batches[i];
        int rigged;
        if ((b->flags & mask) != match)
            continue;
        if (only_model >= 0 && (int)b->model != only_model)
            continue;
        /* No texture, no defined appearance -- see BATCH_NO_TEXTURE in scene.h. */
        if (b->flags & BATCH_NO_TEXTURE)
            continue;
        if (!!(b->flags & BATCH_ALPHA_KEYED) != want_keyed)
            continue;
        /* Never the sky (drawn under a camera-locked matrix the frustum was not
           built from) and never a rigged scene (its parts move under rig.draw[],
           so a model-space box is not where the batch is). See scene.h. */
        if (cull_on && !s->has_rig && !(b->flags & BATCH_SKY) && culled(b)) {
            stats.batches_culled++;
            stats.tris_culled += b->nidx / 3;
            continue;
        }
        stats.batches++;
        stats.tris += b->nidx / 3;
        if (want_keyed)
            stats.tris_keyed += b->nidx / 3;
        /* Part 0 is the unarticulated body, whose draw matrix is always the
           identity -- skip the push/pop for it, which is most of the geometry. */
        rigged = s->has_rig && b->part > 0 && (int)b->part < s->rig.n;
        if (rigged) {
            glPushMatrix();
            glMultMatrixf(s->rig.draw[b->part]);
        }
        /* Bind BEFORE the pointer calls: GL captures the buffer that is bound at
           the moment gl*Pointer is called, not at the draw (vitaGL does the same,
           ffp.c stores ffp_vertex_attrib_vbo there). Binding after would leave the
           attributes pointing at whatever was bound previously. */
        glBindBuffer(GL_ARRAY_BUFFER, b->gl_vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b->gl_ibo);
        lm_bind(b->gl_lm, b);
        glBindTexture(GL_TEXTURE_2D, b->gl_tex);
        glVertexPointer(3, GL_FLOAT, sizeof(vtx_t), VTX_AT(b, x));
        glTexCoordPointer(2, GL_FLOAT, sizeof(vtx_t), VTX_AT(b, u));
        glDrawElements(GL_TRIANGLES, b->nidx, GL_UNSIGNED_SHORT,
                       b->gl_ibo ? (const void *)0 : (const void *)b->idx);
        if (rigged)
            glPopMatrix();
    }
}

/*
 * Two passes: everything opaque with the alpha test OFF, then the alpha-keyed
 * batches with it back ON. See BATCH_ALPHA_KEYED in scene.h -- on PowerVR the
 * test is a shader `discard` and it costs the whole scene its hidden-surface
 * removal, so it is worth two batch loops and one state change to confine it to
 * the 2.6% of triangles that need it.
 *
 * glAlphaFunc is deliberately NOT touched. main.c sets GREATER 0.5 for the world
 * and GREATER 0 around the car's translucent exhaust, and both callers expect
 * their own threshold to survive this. The test is left ENABLED on the way out,
 * which is the state main.c establishes at init and assumes everywhere else.
 */
void scene_draw(const scene_t *s, unsigned int mask, unsigned int match)
{
    glDisable(GL_ALPHA_TEST);
    draw_pass(s, mask, match, 0, -1);
    glEnable(GL_ALPHA_TEST);
    draw_pass(s, mask, match, 1, -1);
    /* never leave unit 1 enabled for whatever draws next */
    lm_bind(0, NULL);
    /* And never leave a buffer bound -- see SCENE VERTEX BUFFERS in scene.h.
       Every other module here draws from client pointers, which a bound
       GL_ARRAY_BUFFER turns into offsets. */
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

int scene_model_index(const scene_t *s, const char *name)
{
    unsigned int i;
    if (!s->model_names)
        return -1;
    for (i = 0; i < s->n_models; i++)
        if (!strcmp(s->model_names[i], name))
            return (int)i;
    return -1;
}

void scene_draw_model(const scene_t *s, unsigned int model, const float m[16])
{
    /* Culling is the CALLER's here -- see scene.h. cull_on is global and a prop
       instance is nowhere near its batch's model-space box, so the filtered
       passes below must not consult it. Saving and clearing it is cheaper than
       threading a flag through draw_pass, and it restores what the track's own
       pass established. */
    int was_on = cull_on;
    cull_on = 0;
    glPushMatrix();
    glMultMatrixf(m);
    glDisable(GL_ALPHA_TEST);
    draw_pass(s, 0, 0, 0, (int)model);
    glEnable(GL_ALPHA_TEST);
    draw_pass(s, 0, 0, 1, (int)model);
    glPopMatrix();
    lm_bind(0, NULL);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    cull_on = was_on;
}

GLuint scene_tex(const scene_t *s, const char *name)
{
    for (unsigned int i = 0; i < s->n_tex; i++)
        if (!strcmp(s->tex_names[i], name))
            return s->tex_ids[i];
    return 0;
}

int scene_keep_rest(batch_t *b)
{
    /* Asking for a rest copy IS the declaration that this batch's vertices are
       about to be rewritten every frame, so it cannot keep the static VBO
       scene_load gave it: a VBO holds the vertices as they were PACKED, and the
       animation would go on happening in main memory where the GPU never looks.
       Dropping the buffers puts the batch back on client pointers, which is
       where water.c has always been -- water is excluded by flag at load, so
       this is a no-op there and the rule now covers both animators with one
       mechanism instead of a second list to keep in step.

       That is not hypothetical: the antenna was buffered when the vertex
       buffers went in, and drew as a welded stick for as long as it was. The
       chain kept simulating and every check in vis_test part 5 kept passing,
       because all of them read b->verts.

       Before the early return, not after. The rest copy is what is idempotent
       here; the unbuffering has to hold on every call, or a second animator
       binding to an already-kept batch would inherit a live VBO. */
    if (b->gl_vbo) {
        glDeleteBuffers(1, &b->gl_vbo);
        b->gl_vbo = 0;
    }
    if (b->gl_ibo) {
        glDeleteBuffers(1, &b->gl_ibo);
        b->gl_ibo = 0;
    }
    if (b->rest)
        return 1;
    b->rest = malloc(sizeof(vtx_t) * b->nverts);
    if (!b->rest)
        return 0;
    memcpy(b->rest, b->verts, sizeof(vtx_t) * b->nverts);
    return 1;
}

/* Release a scene. This lived in main.c, which meant vis_test could not free a
   scene it had loaded without keeping its own copy of the list -- and a copy of a
   free function is a leak waiting for the next field to be added to batch_t. A
   track is ~7 MB of pixels plus its batches and the menu can switch tracks all
   day, so it has to stay complete. */
void scene_release(scene_t *s)
{
    unsigned int i;

    if (s->tex_ids && s->n_tex)
        glDeleteTextures((GLsizei)s->n_tex, s->tex_ids);
    free(s->tex_ids);
    free(s->tex_names);
    free(s->markers);
    free(s->model_names);
    for (i = 0; i < s->n_batches; i++) {
        /* The GPU copies go too. The menu can change track and car all day and
           each one is a few MB -- see SCENE VERTEX BUFFERS in scene.h. */
        if (s->batches[i].gl_vbo)
            glDeleteBuffers(1, &s->batches[i].gl_vbo);
        if (s->batches[i].gl_ibo)
            glDeleteBuffers(1, &s->batches[i].gl_ibo);
        free(s->batches[i].verts);
        free(s->batches[i].idx);
        free(s->batches[i].rest);
        free(s->batches[i].nrm);
    }
    free(s->batches);
    memset(s, 0, sizeof(*s));
}
