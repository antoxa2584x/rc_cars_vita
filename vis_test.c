/* Host harness for the port's three visual subsystems.
 *
 * Part 1 checks the projected car shadow: that it finds receivers, that its UVs
 * are the same mapping pack_vsc.py baked the silhouette with, and that the
 * mapping follows the body through a turn.
 * Part 2 checks the checkpoint arrows: placement, progression along the spine,
 * the distance fade, the three-frame blink and which arrow is which.
 * Part 3 checks the water: the surface displaces, the shoreline damping holds
 * the seam still, and the wave sprites spawn, travel and expire.
 *
 * These are geometry tests, and this port's history says geometry is where it
 * gets hurt: four separate bugs in CLAUDE.md are a convention inherited without
 * measuring anything through it. So the rule from "The renderer's yaw
 * convention is MIRRORED" applies here too -- where a convention matters,
 * construct the actual transform and measure a point through it. testgl/ is a
 * recording stand-in for vitaGL so the real modules run unmodified and the
 * vertices they submit can be read back.
 *
 *   gcc -I. -Itestgl -O2 vis_test.c scene.c shadow.c water.c checkpoint.c \
 *       col.c carani.c rb.c contact.c collide.c -lm -o vis_test
 */

#include "scene.h"
#include "shadow.h"
#include "water.h"
#include "checkpoint.h"
#include "antenna.h"
#include "col.h"
#include "vis_data.h"
#include "fx.h"
#include "fx_data.h"      /* fx_surf[] -- the dust rows the engine's class picks */
#include "trace.h"
#include "carani.h"       /* carani_tire_width -- the mark follows the tyre */
#include "rb_data.h"      /* RB_CARS[].tune, the game's own upgrades.ini rows */
#include "rbcar.h"        /* rbcar_init, to bind a real car's rig to its mesh */
#include "envmap.h"
#include "sfx.h"          /* SURF_*, the material classes the grid carries */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ------------------------------------------------------- the GL recorder */

glcap_t glcap;

static const float *cap_pos;
static const float *cap_uv;
static const unsigned char *cap_rgba;
static int cap_pos_stride, cap_uv_stride, cap_rgba_stride;
static GLuint cur_tex;
static float cur_color[4] = {1.f, 1.f, 1.f, 1.f};
static int st_depth_mask = 1, st_blend = 0, st_alpha = 1, st_cull = 1;
static int st_pol_offset = 0;
static float st_pol_factor = 0.f, st_pol_units = 0.f;
static GLenum st_blend_src = GL_SRC_ALPHA, st_blend_dst = GL_ONE_MINUS_SRC_ALPHA;
static int st_color_array = 0;
static int st_lm_array = 0;
static GLuint next_tex_id = 1;

/* --- texture units, for scene.c's lightmap pass ------------------------- */
GLuint glcap_unit_tex[2];
int    glcap_unit_enabled[2];
static int cur_unit, cur_client_unit;
static const float *cap_lm_uv;
static int cap_lm_stride;

void glActiveTexture(GLenum unit)       { cur_unit = (unit == GL_TEXTURE1); }
void glClientActiveTexture(GLenum unit) { cur_client_unit = (unit == GL_TEXTURE1); }
/* Unit 0's env mode, and the constant colour the combiner interpolates toward.
   This used to discard everything it was handed, which is precisely why the
   tyre marks could be blended the wrong way for as long as they were: the mode
   IS the bug, and it never reached the recording. */
float glcap_env_color[4] = {0.f, 0.f, 0.f, 1.f};
static GLenum st_env_mode = GL_MODULATE;
void glTexEnvi(GLenum t, GLenum p, GLint v)
{
    (void)t;
    if (p == GL_TEXTURE_ENV_MODE && !cur_unit)
        st_env_mode = (GLenum)v;
}
void glTexEnvfv(GLenum t, GLenum p, GLfloat *v)
{
    (void)t;
    if (p == GL_TEXTURE_ENV_COLOR && !cur_unit)
        memcpy(glcap_env_color, v, sizeof(float) * 4);
}


/* Which draws went out under a node matrix. scene.c's documented invariant is
   that part 0 -- the unarticulated body, and most of the geometry -- draws
   under the identity and skips the push/pop entirely; only a rigged part gets a
   matrix. A no-op glMultMatrixf cannot see that, and a mutation widening
   `part > 0` to `part >= 0` survived the whole battery because of it. */
static int pending_matrix;
static int draw_had_matrix[GLCAP_MAX_DRAWS];
static GLuint draw_lm_tex[GLCAP_MAX_DRAWS];

void gl_cap_reset(void)
{
    glcap.n_verts = 0;
    glcap.n_draws = 0;
    glcap.overflow = 0;
    pending_matrix = 0;
    memset(draw_had_matrix, 0, sizeof(draw_had_matrix));
    memset(draw_lm_tex, 0, sizeof(draw_lm_tex));
}

void glGenTextures(GLsizei n, GLuint *out)
{
    for (GLsizei i = 0; i < n; i++)
        out[i] = next_tex_id++;
}
void glDeleteTextures(GLsizei n, const GLuint *ids) { (void)n; (void)ids; }
void glBindTexture(GLenum t, GLuint id)
{
    (void)t;
    glcap_unit_tex[cur_unit] = id;
    if (!cur_unit)
        cur_tex = id;
}
/* RECORD the uploads. This was a no-op stub, and so everything scene_load does to
   a texture was invisible: which mip levels it picks, what sizes it hands GL,
   whether it uploaded at all. Same blind spot ui_test's font atlas already caught
   once -- a stub that discards its arguments cannot fail. */
#define GLCAP_MAX_UPLOADS 512
typedef struct {
    GLuint tex; int level, w, h; GLenum type;
    /* The first texel, so a test can see WHAT was uploaded and not merely that
       something was. The 565 channel-order switch is invisible without it. */
    unsigned int texel0;
} glcap_upload;
static glcap_upload uploads[GLCAP_MAX_UPLOADS];
static int n_uploads;
void glTexImage2D(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b,
                  GLenum f, GLenum ty, const void *p)
{
    (void)t;(void)i;(void)b;(void)f;(void)p;
    if (n_uploads < GLCAP_MAX_UPLOADS) {
        uploads[n_uploads].tex = cur_tex;
        uploads[n_uploads].level = l;
        uploads[n_uploads].w = w;
        uploads[n_uploads].h = h;
        uploads[n_uploads].type = ty;
        uploads[n_uploads].texel0 = 0;
        if (p) {
            if (ty == GL_UNSIGNED_SHORT_5_6_5)
                uploads[n_uploads].texel0 = *(const unsigned short *)p;
            else
                uploads[n_uploads].texel0 = *(const unsigned int *)p;
        }
        n_uploads++;
    }
}
void glTexParameteri(GLenum t, GLenum p, GLint v) { (void)t;(void)p;(void)v; }

/* RECORD the chain generation. scene_load used to hand GL every mip level of
   every texture; it now uploads level `skip` and asks for one generation, because
   vitaGL discards the pixels of any level but 0 and regenerates anyway. That
   moved the whole "the chain reaches the GPU" question out of the upload record
   and into this call, so this one is recorded rather than stubbed. */
#define GLCAP_MAX_MIPGEN 512
static struct { GLuint tex; int n; } mipgen[GLCAP_MAX_MIPGEN];
static int n_mipgen;
void glGenerateMipmap(GLenum target)
{
    int i;
    (void)target;
    for (i = 0; i < n_mipgen; i++)
        if (mipgen[i].tex == cur_tex) { mipgen[i].n++; return; }
    if (n_mipgen < GLCAP_MAX_MIPGEN) {
        mipgen[n_mipgen].tex = cur_tex;
        mipgen[n_mipgen].n = 1;
        n_mipgen++;
    }
}
int glcap_mipgen_count(GLuint tex)
{
    int i;
    for (i = 0; i < n_mipgen; i++)
        if (mipgen[i].tex == tex) return mipgen[i].n;
    return 0;
}
void glcap_mipgen_reset(void) { n_mipgen = 0; }

/* Identity, deliberately -- see testgl/vitaGL.h. scene_frustum_from_gl exists to
   read GL's live stacks and there is no live stack here to read; the culler is
   tested through scene_set_frustum instead. */
void glGetFloatv(GLenum pname, GLfloat *out)
{
    int i;
    if (pname != GL_MODELVIEW_MATRIX && pname != GL_PROJECTION_MATRIX)
        return;
    for (i = 0; i < 16; i++)
        out[i] = (i % 5) ? 0.f : 1.f;
}

/*
 * Buffer objects, emulated for real -- see the block on these in testgl/vitaGL.h.
 * A buffered batch hands gl*Pointer an OFFSET, so the recorder has to hold the
 * bytes and resolve base + offset the way GL does, or every geometry check in
 * this file would quietly start reading address 0x0000000c.
 */
#define GLCAP_MAX_BUFFERS 1024
static struct { unsigned char *data; size_t size; int alive; }
       gl_buf[GLCAP_MAX_BUFFERS];
GLuint glcap_buf_bound[2];

static int buf_slot(GLenum target)
{ return target == GL_ELEMENT_ARRAY_BUFFER ? 1 : 0; }

void glGenBuffers(GLsizei n, GLuint *out)
{
    GLsizei i;
    for (i = 0; i < n; i++) {
        int s;
        out[i] = 0;
        for (s = 0; s < GLCAP_MAX_BUFFERS; s++)
            if (!gl_buf[s].alive) {
                gl_buf[s].alive = 1;
                gl_buf[s].data = NULL;
                gl_buf[s].size = 0;
                out[i] = (GLuint)(s + 1);   /* 0 is "no buffer" in GL */
                break;
            }
    }
}

void glBindBuffer(GLenum target, GLuint id)
{ glcap_buf_bound[buf_slot(target)] = id; }

void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    GLuint id = glcap_buf_bound[buf_slot(target)];
    (void)usage;
    if (!id || id > GLCAP_MAX_BUFFERS || !gl_buf[id - 1].alive)
        return;
    free(gl_buf[id - 1].data);
    gl_buf[id - 1].data = malloc((size_t)size ? (size_t)size : 1);
    gl_buf[id - 1].size = (size_t)size;
    if (gl_buf[id - 1].data && data)
        memcpy(gl_buf[id - 1].data, data, (size_t)size);
}

void glDeleteBuffers(GLsizei n, const GLuint *ids)
{
    GLsizei i;
    for (i = 0; i < n; i++) {
        GLuint id = ids[i];
        if (!id || id > GLCAP_MAX_BUFFERS) continue;
        free(gl_buf[id - 1].data);
        gl_buf[id - 1].data = NULL;
        gl_buf[id - 1].size = 0;
        gl_buf[id - 1].alive = 0;
    }
}

int glcap_buffers_live(void)
{
    int s, n = 0;
    for (s = 0; s < GLCAP_MAX_BUFFERS; s++)
        if (gl_buf[s].alive) n++;
    return n;
}

/* GL captures the binding at the gl*Pointer call, not at the draw -- so this
   runs there, exactly where the real thing resolves it. */
static const void *buf_resolve(GLenum target, const void *p)
{
    GLuint id = glcap_buf_bound[buf_slot(target)];
    if (!id || id > GLCAP_MAX_BUFFERS || !gl_buf[id - 1].data)
        return p;
    return gl_buf[id - 1].data + (size_t)p;
}

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *p)
{
    (void)size; (void)type;
    cap_pos = buf_resolve(GL_ARRAY_BUFFER, p);
    cap_pos_stride = stride;
}
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *p)
{
    const void *r = buf_resolve(GL_ARRAY_BUFFER, p);
    (void)size; (void)type;
    if (cur_client_unit) { cap_lm_uv = r; cap_lm_stride = stride; }
    else                 { cap_uv = r; cap_uv_stride = stride; }
}
void glColorPointer(GLint size, GLenum type, GLsizei stride, const void *p)
{
    (void)size; (void)type;
    cap_rgba = buf_resolve(GL_ARRAY_BUFFER, p);
    cap_rgba_stride = stride ? stride : 4;
}

static void cap_vertex(int i)
{
    int k = glcap.n_verts;
    const char *pp = (const char *)cap_pos + (size_t)i * cap_pos_stride;
    const char *uu = (const char *)cap_uv + (size_t)i * cap_uv_stride;
    if (k >= GLCAP_MAX_VERTS) { glcap.overflow = 1; return; }
    memcpy(glcap.pos[k], pp, 3 * sizeof(float));
    memcpy(glcap.uv[k], uu, 2 * sizeof(float));
    if (st_color_array && cap_rgba) {
        memcpy(glcap.rgba[k],
               cap_rgba + (size_t)i * cap_rgba_stride, 4);
        glcap.has_color[k] = 1;
    } else {
        glcap.has_color[k] = 0;
    }
    glcap.n_verts++;
}

static void cap_begin(GLenum mode, int n)
{
    glcap_draw *d;
    if (glcap.n_draws >= GLCAP_MAX_DRAWS) { glcap.overflow = 1; return; }
    d = &glcap.draws[glcap.n_draws++];
    d->mode = mode;
    d->first = glcap.n_verts;
    d->count = n;
    d->tex = cur_tex;
    memcpy(d->color, cur_color, sizeof(cur_color));
    d->depth_mask = st_depth_mask;
    d->blend = st_blend;
    d->alpha_test = st_alpha;
    d->cull = st_cull;
    d->pol_factor = st_pol_offset ? st_pol_factor : 0.f;
    d->pol_units = st_pol_offset ? st_pol_units : 0.f;
    d->blend_src = st_blend_src;
    d->blend_dst = st_blend_dst;
    d->env_mode = st_env_mode;
    draw_had_matrix[glcap.n_draws - 1] = pending_matrix;
    draw_lm_tex[glcap.n_draws - 1] =
        (glcap_unit_enabled[1] && st_lm_array) ? glcap_unit_tex[1] : 0;
    pending_matrix = 0;
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    cap_begin(mode, count);
    for (GLsizei i = 0; i < count; i++)
        cap_vertex(first + i);
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx)
{
    /* The element buffer resolves HERE, not at a pointer call -- GL_ELEMENT_ARRAY_BUFFER
       is read at the draw. Same rule, different moment, and getting the two the
       wrong way round is a bug the recorder would otherwise hide. */
    const unsigned short *ix =
        (const unsigned short *)buf_resolve(GL_ELEMENT_ARRAY_BUFFER, idx);
    (void)type;
    cap_begin(mode, count);
    for (GLsizei i = 0; i < count; i++)
        cap_vertex(ix[i]);
}

void glEnable(GLenum c)
{
    if (c == GL_TEXTURE_2D) { glcap_unit_enabled[cur_unit] = 1; return; }
    if (c == GL_BLEND) st_blend = 1;
    else if (c == GL_ALPHA_TEST) st_alpha = 1;
    else if (c == GL_CULL_FACE) st_cull = 1;
    else if (c == GL_POLYGON_OFFSET_FILL) st_pol_offset = 1;
}
void glDisable(GLenum c)
{
    if (c == GL_TEXTURE_2D) { glcap_unit_enabled[cur_unit] = 0; return; }
    if (c == GL_BLEND) st_blend = 0;
    else if (c == GL_ALPHA_TEST) st_alpha = 0;
    else if (c == GL_CULL_FACE) st_cull = 0;
    else if (c == GL_POLYGON_OFFSET_FILL) st_pol_offset = 0;
}
void glEnableClientState(GLenum c)
{
    if (c == GL_COLOR_ARRAY) st_color_array = 1;
    else if (c == GL_TEXTURE_COORD_ARRAY && cur_client_unit) st_lm_array = 1;
}
void glDisableClientState(GLenum c)
{
    if (c == GL_COLOR_ARRAY) st_color_array = 0;
    else if (c == GL_TEXTURE_COORD_ARRAY && cur_client_unit) st_lm_array = 0;
}
void glBlendFunc(GLenum a, GLenum b) { st_blend_src = a; st_blend_dst = b; }
void glPolygonOffset(GLfloat factor, GLfloat units)
{ st_pol_factor = factor; st_pol_units = units; }
void glDepthMask(GLboolean b) { st_depth_mask = b; }
void glColor4f(float r, float g, float b, float a)
{ cur_color[0]=r; cur_color[1]=g; cur_color[2]=b; cur_color[3]=a; }
/* scene_draw is the only thing in these files that touches the matrix stack,
   and it pushes CONDITIONALLY (only for a rigged part). A condition that drifts
   apart between the push and the pop leaks a matrix per frame and overflows a
   real 32-deep stack seconds later, looking fine in the frame that causes it. */
static int mat_depth, mat_max_depth, mat_underflow;
void glPushMatrix(void)
{
    mat_depth++;
    if (mat_depth > mat_max_depth) mat_max_depth = mat_depth;
}
void glPopMatrix(void)
{
    if (--mat_depth < 0) { mat_underflow = 1; mat_depth = 0; }
}
void glMultMatrixf(const float *m) { (void)m; pending_matrix = 1; }

/* ------------------------------------------------------------ test plumbing */

static int fails, checks;

static void ck(int cond, const char *what, const char *fmt, ...)
{
    va_list ap;
    checks++;
    if (cond) {
        printf("  ok   %-46s ", what);
    } else {
        fails++;
        printf("  FAIL %-46s ", what);
    }
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static int near(float a, float b, float tol) { return fabsf(a - b) <= tol; }

/*
 * What a draw does to white ground, evaluated from the state it was RECORDED
 * with -- the fixed-function pipeline for one texel, by hand.
 *
 * `vcol` is the vertex colour channel (which for the tyre marks is the strength
 * f, in both RGB and alpha) and `texel` the texture value under the fragment.
 * The return is the factor the framebuffer is multiplied by, so 1.0 is "leaves
 * the ground alone" and 0.5 is "halves it".
 *
 * This is the same rule as constructing the real view matrix and putting a point
 * through it: where a convention matters, evaluate it rather than assert the
 * intention. It reads the recorded enums, so a mutation to the blend mode or the
 * env mode moves the answer.
 */
static float trace_ground(const glcap_draw *d, float vcol, float texel)
{
    float src;

    if (d->env_mode == GL_COMBINE)          /* INTERPOLATE(tex, constant, vcol) */
        src = texel * vcol + glcap_env_color[0] * (1.f - vcol);
    else                                    /* GL_MODULATE */
        src = texel * vcol;

    if (d->blend_src == GL_DST_COLOR && d->blend_dst == GL_SRC_COLOR)
        return 2.f * src;                   /* src*dst + dst*src, dst = 1 */
    if (d->blend_src == GL_SRC_ALPHA && d->blend_dst == GL_ONE_MINUS_SRC_ALPHA)
        return src * vcol + (1.f - vcol);   /* alpha is the same channel */
    return src;                             /* no blending: it replaces */
}

/*
 * The world draws with the alpha test on, blending off, culling on and depth
 * writes on. Every one of these modules turns some of that off and has to put
 * it back: a shadow that leaves GL_ALPHA_TEST disabled makes the next frame's
 * foliage draw as opaque rectangles, and one that leaves depth writes off
 * breaks everything after it. Nothing in the captured DRAWS can see that --
 * the damage is to the state after the call returns -- so it gets its own
 * check, run from a known starting state.
 *
 * Found by mutation: deleting the restores at the end of shadow_project left
 * all 51 checks green.
 */
static void set_world_state(void)
{
    st_alpha = 1; st_blend = 0; st_cull = 1; st_depth_mask = 1;
    st_color_array = 0; st_pol_offset = 0;
    st_pol_factor = st_pol_units = 0.f;
    st_env_mode = GL_MODULATE;
}

static void ck_state_restored(const char *who)
{
    /* The depth bias joined this list with the foam-band decal fix: left on, it
       pulls the WHOLE world toward the camera on the next frame, which is a
       subtler wrong than a missing alpha test and just as global. */
    /* The texture env mode joined it with the tyre marks: trace.c is the one
       module here that leaves GL_MODULATE, and a combiner left set makes every
       batch after it sample a constant instead of its own texture. */
    ck(st_alpha == 1 && st_blend == 0 && st_cull == 1 && st_depth_mask == 1
       && st_color_array == 0 && st_pol_offset == 0
       && st_pol_factor == 0.f && st_pol_units == 0.f
       && st_env_mode == GL_MODULATE,
       "GL state restored afterwards", "%s left atest=%d blend=%d cull=%d "
       "zwrite=%d colorarray=%d zbias=%d/%.0f,%.0f env=0x%x", who, st_alpha,
       st_blend, st_cull, st_depth_mask, st_color_array, st_pol_offset,
       st_pol_factor, st_pol_units, st_env_mode);
}

/* ------------------------------------------------------------ fixtures ---- */

/* A seabed shelving up to the shore: y = y_at_z0 at z = 0, falling to y_deep at
   z = zmax. Backs the water's depth-based swell damping. */
static void make_shelf(col_t *c, float half, int n, float y_shore, float y_deep)
{
    int cells = n * n, i, j, t = 0;
    float step = 2.f * half / n;
    memset(c, 0, sizeof(*c));
    c->minx = c->minz = -half;
    c->cell = step;
    c->nx = c->nz = (unsigned)n;
    c->ntris = (unsigned)(cells * 2);
    c->tris = malloc((size_t)c->ntris * 9 * sizeof(float));
    c->start = malloc((size_t)(cells + 1) * sizeof(unsigned int));
    c->idx = malloc((size_t)c->ntris * sizeof(unsigned int));
    for (j = 0; j < n; j++) {
        for (i = 0; i < n; i++) {
            float x0 = -half + i * step, x1 = x0 + step;
            float z0 = -half + j * step, z1 = z0 + step;
            float f0 = (z0 + half) / (2.f * half), f1 = (z1 + half) / (2.f * half);
            float ya = y_shore + (y_deep - y_shore) * f0;
            float yb = y_shore + (y_deep - y_shore) * f1;
            float *a = &c->tris[(size_t)t * 9];
            a[0]=x0; a[1]=ya; a[2]=z0;  a[3]=x1; a[4]=ya; a[5]=z0;  a[6]=x1; a[7]=yb; a[8]=z1;
            a += 9;
            a[0]=x0; a[1]=ya; a[2]=z0;  a[3]=x1; a[4]=yb; a[5]=z1;  a[6]=x0; a[7]=yb; a[8]=z1;
            c->start[j * n + i] = (unsigned)t;
            c->idx[t] = (unsigned)t;
            c->idx[t + 1] = (unsigned)(t + 1);
            t += 2;
        }
    }
    c->start[cells] = (unsigned)t;
}

/* A flat collision plane covering [-half, half]^2 as a grid of quads. */
static void make_plane(col_t *c, float half, int n, float y)
{
    int cells = n * n, i, j, t = 0;
    float step = 2.f * half / n;

    memset(c, 0, sizeof(*c));
    c->minx = c->minz = -half;
    c->cell = step;
    c->nx = c->nz = (unsigned)n;
    c->ntris = (unsigned)(cells * 2);
    c->tris = malloc((size_t)c->ntris * 9 * sizeof(float));
    c->start = malloc((size_t)(cells + 1) * sizeof(unsigned int));
    c->idx = malloc((size_t)c->ntris * sizeof(unsigned int));

    for (j = 0; j < n; j++) {
        for (i = 0; i < n; i++) {
            float x0 = -half + i * step, x1 = x0 + step;
            float z0 = -half + j * step, z1 = z0 + step;
            float *a = &c->tris[(size_t)t * 9];
            a[0]=x0; a[1]=y; a[2]=z0;  a[3]=x1; a[4]=y; a[5]=z0;  a[6]=x1; a[7]=y; a[8]=z1;
            a += 9;
            a[0]=x0; a[1]=y; a[2]=z0;  a[3]=x1; a[4]=y; a[5]=z1;  a[6]=x0; a[7]=y; a[8]=z1;
            c->start[j * n + i] = (unsigned)t;
            c->idx[t] = (unsigned)t;
            c->idx[t + 1] = (unsigned)(t + 1);
            t += 2;
        }
    }
    c->start[cells] = (unsigned)t;
}

static scene_t *make_scene(const char *const *tex, int n_tex)
{
    scene_t *s = calloc(1, sizeof(scene_t));
    s->n_tex = (unsigned)n_tex;
    s->tex_ids = calloc(n_tex, sizeof(GLuint));
    s->tex_names = calloc(n_tex, SCENE_TEX_NAME);
    for (int i = 0; i < n_tex; i++) {
        s->tex_ids[i] = next_tex_id++;
        snprintf(s->tex_names[i], SCENE_TEX_NAME, "%s", tex[i]);
    }
    return s;
}

static void add_marker(scene_t *s, const char *nm, float x, float y, float z,
                       float yaw)
{
    unsigned int k = s->n_markers++;
    s->markers = realloc(s->markers, s->n_markers * sizeof(marker_t));
    snprintf(s->markers[k].name, SCENE_MARKER_NAME, "%s", nm);
    s->markers[k].x = x;
    s->markers[k].y = y;
    s->markers[k].z = z;
    s->markers[k].yaw = yaw;
}

/* A flat grid batch, so water.c has something to displace. spanx/spanz are
   separate because the coast is a STRIP along the shoreline, not a field -- a
   square "coast" puts a shoreline vertex next to every sea vertex and the
   damping test then measures nothing. */
static batch_t *add_grid2(scene_t *s, unsigned int flags, float x0, float z0,
                          float spanx, float spanz, int n, float y)
{
    unsigned int k = s->n_batches++;
    batch_t *b;
    int i, j, v = 0, q = 0;

    s->batches = realloc(s->batches, s->n_batches * sizeof(batch_t));
    b = &s->batches[k];
    memset(b, 0, sizeof(*b));
    b->flags = flags;
    b->tex = 0;
    b->gl_tex = s->n_tex ? s->tex_ids[0] : 0;
    b->nverts = (unsigned)((n + 1) * (n + 1));
    b->nidx = (unsigned)(n * n * 6);
    b->verts = malloc(sizeof(vtx_t) * b->nverts);
    b->idx = malloc(sizeof(unsigned short) * b->nidx);
    for (j = 0; j <= n; j++) {
        for (i = 0; i <= n; i++) {
            b->verts[v].x = x0 + spanx * i / n;
            b->verts[v].y = y;
            b->verts[v].z = z0 + spanz * j / n;
            b->verts[v].u = (float)i / n;
            b->verts[v].v = (float)j / n;
            v++;
        }
    }
    for (j = 0; j < n; j++) {
        for (i = 0; i < n; i++) {
            unsigned short a = (unsigned short)(j * (n + 1) + i);
            b->idx[q++] = a;
            b->idx[q++] = (unsigned short)(a + 1);
            b->idx[q++] = (unsigned short)(a + n + 1);
            b->idx[q++] = (unsigned short)(a + 1);
            b->idx[q++] = (unsigned short)(a + n + 2);
            b->idx[q++] = (unsigned short)(a + n + 1);
        }
    }
    return b;
}

/* the engine's row-major row-vector body matrix for a yaw about world Y:
   rows 0..2 are the body X, Y, Z axes, row 3 the translation */
static void body_matrix(float *m, float yaw_deg, float x, float y, float z)
{
    float a = yaw_deg * (float)(M_PI / 180.0);
    float c = cosf(a), s = sinf(a);
    memset(m, 0, 16 * sizeof(float));
    m[0] = c;  m[1] = 0.f; m[2] = -s;      /* body +X */
    m[4] = 0.f; m[5] = 1.f; m[6] = 0.f;    /* body +Y */
    m[8] = s;  m[9] = 0.f; m[10] = c;      /* body +Z, the nose */
    m[12] = x; m[13] = y; m[14] = z; m[15] = 1.f;
}

/* ============================================================== part 1 ==== */

static void part1_shadow(void)
{
    static const char *tex[] = {"__shadow"};
    col_t col;
    scene_t *car = make_scene(tex, 1);
    shadow_t sh;
    float m[16];
    int i, inside = 0;
    float umin = 1e9f, umax = -1e9f, vmin = 1e9f, vmax = -1e9f;
    const float R = 0.284f;                 /* what pack_vsc.py fits for Car1 */

    printf("\n-- part 1: the projected car shadow --\n");
    make_plane(&col, 4.f, 16, 0.f);
    car->shadow_radius = R;
    shadow_init(&sh, car, 0);

    ck(sh.enabled, "shadow enabled", "tex=%u", sh.tex);
    ck(near(sh.size, R > 0.36f ? R : 0.36f, 1e-5f), "radius = max(ShadowSize, baked)",
       "%.3f m (ShadowSize 0.360, baked %.3f)", sh.size, R);
    ck(near(sh.density, 34.f / 255.f, 1e-5f), "density = ShadowDensity/255",
       "%.4f", sh.density);

    /* --- upright, facing +Z: the identity case ------------------------- */
    gl_cap_reset();
    set_world_state();
    body_matrix(m, 0.f, 0.f, 0.10f, 0.f);
    shadow_draw(&sh, &col, m);
    ck(glcap.n_draws == 1 && sh.n_tris > 0, "receivers found on flat ground",
       "%d draws, %d tris", glcap.n_draws, sh.n_tris);

    for (i = 0; i < glcap.n_verts; i++) {
        float du = 0.5f + glcap.pos[i][0] / (2.f * sh.size);
        float dv = 0.5f + glcap.pos[i][2] / (2.f * sh.size);
        if (!near(glcap.uv[i][0], du, 1e-4f) || !near(glcap.uv[i][1], dv, 1e-4f))
            break;
    }
    ck(i == glcap.n_verts,
       "UV = 0.5 + offset/(2R), same as the bake",
       "%d of %d vertices", i, glcap.n_verts);

    for (i = 0; i < glcap.n_verts; i++) {
        if (glcap.uv[i][0] > umax) umax = glcap.uv[i][0];
        if (glcap.uv[i][0] < umin) umin = glcap.uv[i][0];
        if (glcap.uv[i][1] > vmax) vmax = glcap.uv[i][1];
        if (glcap.uv[i][1] < vmin) vmin = glcap.uv[i][1];
        if (glcap.uv[i][0] >= 0.f && glcap.uv[i][0] <= 1.f
            && glcap.uv[i][1] >= 0.f && glcap.uv[i][1] <= 1.f)
            inside++;
    }
    ck(umin <= 0.f && umax >= 1.f && vmin <= 0.f && vmax >= 1.f,
       "decal spans the whole texture",
       "u [%.2f, %.2f]  v [%.2f, %.2f]", umin, umax, vmin, vmax);
    ck(inside > 0, "some of the decal lands inside the silhouette",
       "%d of %d vertices", inside, glcap.n_verts);

    ck(glcap.draws[0].blend && !glcap.draws[0].depth_mask
       && !glcap.draws[0].alpha_test && !glcap.draws[0].cull,
       "state: blend on, depth write off, alpha test off",
       "blend=%d zwrite=%d atest=%d cull=%d", glcap.draws[0].blend,
       glcap.draws[0].depth_mask, glcap.draws[0].alpha_test,
       glcap.draws[0].cull);
    ck_state_restored("shadow_draw");
    ck(near(glcap.draws[0].color[3], sh.density, 1e-5f)
       && glcap.draws[0].color[0] == 0.f,
       "drawn black at ShadowDensity", "rgba %.2f %.2f %.2f %.3f",
       glcap.draws[0].color[0], glcap.draws[0].color[1],
       glcap.draws[0].color[2], glcap.draws[0].color[3]);

    /* --- yawed a quarter turn: the nose now points along world +X -------
     *
     * This is the check that matters. The silhouette was baked in MODEL space
     * (u from model x, v from model z), so a car turned to face world +X must
     * map world +X onto +v and world -Z -- its left -- onto +u. Get the sign
     * wrong and the shadow is a car lying across its own body. */
    {
        int i_fwd = 0, i_left = 0;
        gl_cap_reset();
        body_matrix(m, 90.f, 0.f, 0.10f, 0.f);
        shadow_draw(&sh, &col, m);

        /* At yaw 90 the body's +Z is world +X and its +X -- its LEFT -- is
           world -Z, so the mapping has to become u = 0.5 - dz/2R,
           v = 0.5 + dx/2R. Assert that on every vertex rather than probing near
           a point: on a coarse receiver mesh the nearest vertex to a probe can
           be the centre one, where both coordinates are 0.5 and any sign
           convention "passes". */
        for (i = 0; i < glcap.n_verts; i++) {
            float du = 0.5f - glcap.pos[i][2] / (2.f * sh.size);
            float dv = 0.5f + glcap.pos[i][0] / (2.f * sh.size);
            if (!near(glcap.uv[i][0], du, 1e-4f) || !near(glcap.uv[i][1], dv, 1e-4f))
                break;
            if (glcap.pos[i][0] > glcap.pos[i_fwd][0]) i_fwd = i;
            if (glcap.pos[i][2] < glcap.pos[i_left][2]) i_left = i;
        }
        ck(i == glcap.n_verts, "yawed 90: the mapping turns with the body",
           "%d of %d vertices match u = 0.5 - dz/2R, v = 0.5 + dx/2R",
           i, glcap.n_verts);
        ck(glcap.uv[i_fwd][1] > 0.5f,
           "yawed 90: world +X (the nose) maps to +v",
           "furthest +X vertex at uv (%.3f, %.3f)",
           glcap.uv[i_fwd][0], glcap.uv[i_fwd][1]);
        ck(glcap.uv[i_left][0] > 0.5f,
           "yawed 90: world -Z (the left side) maps to +u",
           "furthest -Z vertex at uv (%.3f, %.3f)",
           glcap.uv[i_left][0], glcap.uv[i_left][1]);
    }

    /* --- the shift is along the body's forward, not world Z ------------- */
    {
        shadow_t s2 = sh;
        float cx, cz;
        s2.shift = 0.20f;
        gl_cap_reset();
        body_matrix(m, 90.f, 0.f, 0.10f, 0.f);
        shadow_draw(&s2, &col, m);
        /* Invert the yaw-90 mapping to recover the centre the decal was built
           around: u carries -dz and v carries +dx. */
        cz = glcap.pos[0][2] + (glcap.uv[0][0] - 0.5f) * 2.f * s2.size;
        cx = glcap.pos[0][0] - (glcap.uv[0][1] - 0.5f) * 2.f * s2.size;
        /* The body's +Z is world +X here, so the shift has to move the centre
           along X and leave Z alone. */
        ck(near(cx, s2.shift, 1e-3f) && near(cz, 0.f, 1e-3f),
           "ShadowShift follows the body, not world Z",
           "centre (%.4f, %.4f) with shift %.2f at yaw 90", cx, cz, s2.shift);
    }

    /* --- a wall is not a shadow receiver -------------------------------- */
    {
        col_t wall;
        memset(&wall, 0, sizeof(wall));
        wall.minx = wall.minz = -1.f;
        wall.cell = 2.f;
        wall.nx = wall.nz = 1;
        wall.ntris = 1;
        wall.tris = malloc(9 * sizeof(float));
        wall.start = malloc(2 * sizeof(unsigned int));
        wall.idx = malloc(sizeof(unsigned int));
        /* vertical triangle in the XY plane */
        wall.tris[0]=-0.5f; wall.tris[1]=0.f;  wall.tris[2]=0.f;
        wall.tris[3]= 0.5f; wall.tris[4]=0.f;  wall.tris[5]=0.f;
        wall.tris[6]= 0.5f; wall.tris[7]=-1.f; wall.tris[8]=0.f;
        wall.start[0] = 0; wall.start[1] = 1; wall.idx[0] = 0;
        gl_cap_reset();
        body_matrix(m, 0.f, 0.f, 0.10f, 0.f);
        shadow_draw(&sh, &col, m);
        i = sh.n_tris;
        gl_cap_reset();
        shadow_draw(&sh, &wall, m);
        ck(sh.n_tris == 0, "vertical faces are rejected as receivers",
           "%d tris on a wall, %d on the plane", sh.n_tris, i);
        free(wall.tris); free(wall.start); free(wall.idx);
    }

    free(col.tris); free(col.start); free(col.idx);
}

/* Which captured draw is the billboard for the checkpoint at (cx, cz)? Every
   checkpoint is marked now, so a check has to name the one it means. */
static int cp_find_draw(float cx, float cz)
{
    for (int d = 0; d < glcap.n_draws; d++) {
        float sx = 0.f, sz = 0.f;
        if (glcap.draws[d].count != 4)
            continue;
        for (int i = 0; i < 4; i++) {
            sx += glcap.pos[glcap.draws[d].first + i][0] * 0.25f;
            sz += glcap.pos[glcap.draws[d].first + i][2] * 0.25f;
        }
        if (fabsf(sx - cx) < 1e-3f && fabsf(sz - cz) < 1e-3f)
            return d;
    }
    return -1;
}

/* --- the checkpoint spine, driven -----------------------------------------
 *
 * Flatten the stitched CLOSED spine into a polyline, then drive along it. Shared
 * by the synthetic fixture below and by the ten real tracks after it, so the two
 * measure the same thing and cannot drift apart.
 */
static int cp_flatten(const checkpoints_t *c, float path[][3])
{
    int np = 0, k, j;
    for (k = 0; k < c->n; k++)
        for (j = 0; j < c->cp[k].n; j++, np++)
            memcpy(path[np], c->cp[k].p[j], sizeof(path[0]));
    memcpy(path[np++], c->cp[0].p[0], sizeof(path[0]));   /* close it */
    return np;
}

/* Two laps of it, in `step` metre increments. Reports what the crossings did:
 * the count, the furthest any of them landed from the marker it claimed to have
 * passed, whether they arrived in the spine's own order, and whether the lap
 * counter ever moved on anything but checkpoint 0.
 *
 * Driven rather than jumped between waypoints on purpose: a crossing is an event
 * in a continuous quantity, and teleporting from one waypoint to the next cannot
 * see WHERE it happened -- which is the entire property under test.
 */
static int cp_drive(checkpoints_t *c, const float path[][3], int np, int laps,
                    float step, float *worst, int *order, int *lap_bad)
{
    int fires = 0, seg, lap;

    *worst = -1.f;
    *order = 1;
    *lap_bad = 0;

    cp_step(c, path[0][0], path[0][1], path[0][2], 0.016f);
    {
        int want = c->next;
        for (lap = 0; lap < laps; lap++)
        for (seg = 0; seg + 1 < np; seg++) {
            float ax = path[seg][0], az = path[seg][2];
            float bx = path[seg + 1][0], bz = path[seg + 1][2];
            float len = sqrtf((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
            int n = (int)(len / step) + 1, i;
            for (i = 1; i <= n; i++) {
                float u = (float)i / (float)n;
                float x = ax + (bx - ax) * u, z = az + (bz - az) * u;
                int lap_was = c->lap;
                cp_step(c, x, 0.f, z, 0.016f);
                /* THE LAP HAS TO TICK OVER ON THE START LINE and nowhere else.
                   Counting it one station early passes any check on the TOTAL --
                   there is still exactly one per lap -- and it is a real error,
                   because the opponents' lead is spine_len*(lap - 1) + distance
                   along the spine, so a lap counted at the last checkpoint puts a
                   whole lap of it into the stretch before the line. A mutant that
                   did exactly that survived everything here until this line. */
                if ((c->lap != lap_was) != (c->passed == 0))
                    *lap_bad = 1;
                if (c->passed >= 0) {
                    const float *m = c->cp[c->passed].p[0];
                    float e = sqrtf((x - m[0]) * (x - m[0])
                                    + (z - m[2]) * (z - m[2]));
                    if (e > *worst) *worst = e;
                    if (c->passed != want) *order = 0;
                    want = (want + 1) % c->n;
                    fires++;
                }
            }
        }
    }
    return fires;
}

/* ============================================================== part 2 ==== */

static void part2_checkpoints(void)
{
    static const char *tex[] = {
        "cp_ar_2_f1", "cp_ar_2_f2", "cp_ar_2_f3",
        "cp_ar_3_f1", "cp_ar_3_f2", "cp_ar_3_f3"
    };
    scene_t *s = make_scene(tex, 6);
    checkpoints_t c;
    float eye[3];

    printf("\n-- part 2: the checkpoint arrows --\n");

    /* three checkpoints round a square, with one refining point each */
    add_marker(s, "cp_1", 0.f, 0.f, 0.f, 0.f);
    add_marker(s, "cp_1_1", 10.f, 0.f, 0.f, 0.f);
    add_marker(s, "cp_2", 20.f, 1.f, 0.f, 0.f);
    add_marker(s, "cp_2_1", 20.f, 1.f, 10.f, 0.f);
    add_marker(s, "cp_3", 20.f, 2.f, 20.f, 0.f);
    /* a decoy the loader must ignore: the numbering has to be contiguous */
    add_marker(s, "cp_9", 99.f, 0.f, 99.f, 0.f);

    cp_init(&c, s, NULL);
    ck(c.n == 3, "cp_N found, stopping at the gap in the numbering",
       "n = %d (cp_9 must not be picked up)", c.n);
    ck(c.cp[0].n == 2 && c.cp[1].n == 2 && c.cp[2].n == 1,
       "each checkpoint owns cp_N plus its cp_N_M points",
       "%d, %d, %d", c.cp[0].n, c.cp[1].n, c.cp[2].n);
    ck(c.cp[1].p[0][0] == 20.f && c.cp[1].p[0][1] == 1.f,
       "p[0] is cp_N itself -- the arrow's anchor",
       "cp_2 at (%.1f, %.1f, %.1f)", c.cp[1].p[0][0], c.cp[1].p[0][1],
       c.cp[1].p[0][2]);
    ck(c.enabled, "enabled once both arrow sets resolve", "tex %u / %u",
       c.tex_common[0], c.tex_custom[0]);

    /* --- progression: a checkpoint is passed AT the checkpoint -----------
     *
     * The property, and the one this used to get wrong: the crossing has to
     * happen where the checkpoint IS. `next` used to advance the moment the
     * nearest spine SAMPLE changed owner, which on this fixture is the midpoint
     * between cp_1_1 and cp_2 -- five metres early -- and on a real track is
     * wherever the artist stopped adding refining points. That is the reported
     * "the sound triggers somewhere in different places", and every check here
     * before this one asked only which checkpoint the arrow had moved to, never
     * WHERE the move happened. So the measurement is the distance from the car
     * to the marker it just claimed to pass. Two laps, so the seam at the start
     * line is crossed by driving and not by cp_init. */
    {
        float path[CP_MAX * CP_MAX_POINTS + 1][3];
        int np = cp_flatten(&c, path);

        /* Walk it at 0.25 m, which is twice the 0.125 m a car at top speed
           covers in a 1/60 frame, so a check that bounds the error by the step
           is not being generous to itself. */
        {
            const float STEP = 0.25f;
            float worst;
            int fires, order_ok, lap_bad;

            cp_step(&c, path[0][0], path[0][1], path[0][2], 0.016f);
            ck(c.next == 1 && c.passed < 0,
               "the first step aims the arrow and passes nothing",
               "next = %d, passed = %d", c.next, c.passed);

            fires = cp_drive(&c, path, np, 2, STEP, &worst, &order_ok, &lap_bad);
            ck(fires == 2 * c.n, "one crossing per checkpoint per lap, no more",
               "%d crossings over 2 laps of %d checkpoints", fires, c.n);
            ck(order_ok, "and they arrive in the spine's own order", "");
            ck(!lap_bad, "the lap ticks over on the START LINE, and only there",
               "%d crossings checked", fires);
            /* Bound it by the STEP, not by a round number and not by anything
               checkpoint.c owns: the car cannot be told it passed a marker it is
               still driving at. Two steps of slack for the corner cases where a
               station sits on a bend. The old rule fires 5 m out here and dies
               on this. */
            ck(worst >= 0.f && worst < 2.f * STEP,
               "and each fires AT its own marker, not between two of them",
               "worst %.3f m from the marker (step %.2f m)", worst, STEP);
            ck(c.lap == 2, "and the start line is what counts a lap",
               "lap = %d after 2 laps", c.lap);
        }

        /* A WIDE LINE. The rule that would be obvious here -- a radius around
           the marker -- fails exactly this case, which is why checkpoint.h
           argues against it: these waypoints are 30 to 90 m apart on a real
           track, and a car cutting a corner never comes inside any sensible
           radius of one. Same lap, driven 3 m OUTSIDE every leg -- outside
           because the offset has to stay on one side of the loop, and a fixed
           vector on this fixture would put part of the path straight down the
           middle of the closing leg. */
        {
            float cx = 0.f, cz = 0.f;
            int fires = 0, seg, i3;
            for (i3 = 0; i3 + 1 < np; i3++) {  /* centroid, minus the repeat */
                cx += path[i3][0] / (float)(np - 1);
                cz += path[i3][2] / (float)(np - 1);
            }
            cp_resync(&c, path[0][0], path[0][1], path[0][2] - 3.f);
            for (seg = 0; seg + 1 < np; seg++) {
                float ax = path[seg][0], az = path[seg][2];
                float bx = path[seg + 1][0], bz = path[seg + 1][2];
                float dx = bx - ax, dz = bz - az;
                float len = sqrtf(dx * dx + dz * dz);
                float nx, nz, i2, n;
                if (len < 1e-6f)
                    continue;
                nx = -dz / len; nz = dx / len;         /* a leg normal */
                if (nx * (0.5f * (ax + bx) - cx)        /* the OUTWARD one */
                    + nz * (0.5f * (az + bz) - cz) < 0.f) {
                    nx = -nx; nz = -nz;
                }
                n = (float)((int)(len / 0.25f) + 1);
                for (i2 = 1.f; i2 <= n; i2 += 1.f) {
                    float u = i2 / n;
                    cp_step(&c, ax + dx * u + nx * 3.f, 0.f,
                            az + dz * u + nz * 3.f, 0.016f);
                    if (c.passed >= 0)
                        fires++;
                }
            }
            ck(fires == c.n, "a wide line still passes every checkpoint",
               "%d of %d, 3 m off the line", fires, c.n);
        }

        /* A TELEPORT is not a lap. Respawning across the far side of the spine
           sweeps past every station in between, and cp_resync is what stops that
           being a burst of cues. */
        {
            int lap0 = c.lap;
            cp_resync(&c, c.cp[2].p[0][0], c.cp[2].p[0][1], c.cp[2].p[0][2]);
            /* Aim the arrow at a checkpoint already behind the car, on purpose.
               Without this the previous lap has ALREADY left it on 1 and the
               re-aim check passes on the value it happened to hold -- which is
               how a mutant that dropped the re-aim entirely survived it. */
            c.next = 2;
            cp_step(&c, c.cp[0].p[0][0], 0.f, c.cp[0].p[0][2], 0.016f);
            ck(c.passed < 0 && c.lap == lap0,
               "a teleport back to the start line passes nothing",
               "passed = %d, lap %d -> %d", c.passed, lap0, c.lap);
            ck(c.next == 1, "and re-aims the arrow at the checkpoint ahead of it",
               "next = %d (was pointed at 2 before the teleport)", c.next);
        }

        /* And DRIVING BACKWARDS does not hand a checkpoint back. */
        {
            int lap0 = c.lap;
            cp_resync(&c, 5.f, 0.f, 0.f);
            cp_step(&c, 4.f, 0.f, 0.f, 0.016f);
            cp_step(&c, 3.f, 0.f, 0.f, 0.016f);
            ck(c.passed < 0 && c.next == 1 && c.lap == lap0,
               "driving backwards passes nothing and holds the arrow",
               "passed = %d, next = %d, lap %d", c.passed, c.next, c.lap);
        }

        /* --- WHERE A DEAD CAR GOES BACK TO -----------------------------------
         *
         * cp_respawn_pose, and the three things around it. This is what stops a
         * drowning being a race restart: main.c used to call respawn(), which puts
         * the whole FIELD back on its own grid, so going in the sea on the last
         * corner un-drove the race for everybody.
         *
         * The pose comes off `last` -- the crossing the car really made -- and NOT
         * off `next - 1`, and the fixture drives to it rather than assigning it,
         * because `last` being latched by a real crossing is half the property.
         */
        {
            checkpoints_t r;
            float pos[3], yaw;

            /* NOTHING CROSSED YET: no pose, so main.c falls back to the grid. A
               car that drowns before its first checkpoint has nowhere else to go,
               and the alternative -- deriving the answer from `next` -- sends it
               HALF A LAP BACKWARDS at the start line, because cp_0's station is
               both 0 and spine_len and which side the grid falls on is an accident
               of the track. */
            cp_init(&r, s, NULL);
            ck(r.last < 0, "a fresh spine has crossed nothing",
               "last = %d", r.last);
            ck(!cp_respawn_pose(&r, pos, &yaw),
               "so there is no respawn point and the caller uses the grid", "");

            /* DRIVE a full closed lap. The flattened path ends back at cp_0, so the
               last crossing it makes is the start line -- which is the case that
               can cost a lap, and the one worth landing on. Driven, not assigned:
               `last` being latched by a real crossing is half the property. */
            {
                int np2 = cp_flatten(&r, path);
                float w2; int o2, lb2;
                r.enabled = 1;
                cp_drive(&r, path, np2, 1, 0.25f, &w2, &o2, &lb2);
                ck(r.last == 0,
                   "driving a closed lap latches the crossing of the start line",
                   "last = %d, lap %d", r.last, r.lap);
            }

            if (r.last == 0) {
                int lap0 = r.lap;
                ck(cp_respawn_pose(&r, pos, &yaw),
                   "after crossing the start line there IS a respawn point", "");
                /* AT the marker, in XZ. The y is cp_t.ground rather than the
                   marker's own height -- the markers float 0.18 to 0.49 m -- and
                   main.c re-probes it anyway. */
                ck(near(pos[0], c.cp[0].p[0][0], 1e-3f)
                   && near(pos[2], c.cp[0].p[0][2], 1e-3f),
                   "and it is the checkpoint's own position",
                   "(%.2f, %.2f) vs marker (%.2f, %.2f)", pos[0], pos[2],
                   c.cp[0].p[0][0], c.cp[0].p[0][2]);
                /* ALONG THE SPINE. The fixture's first leg runs from the origin
                   10 m along +X, and rbcar_init's convention puts local +Z on
                   (sin yaw, 0, cos yaw), so a heading down +X is yaw +90 degrees.
                   Spelled out from the geometry, not from what checkpoint.c
                   computed -- a car dropped back facing the way it came is worse
                   than not respawning it. */
                ck(near(sinf(yaw * (float)(M_PI / 180.0)), 1.f, 1e-3f)
                   && near(cosf(yaw * (float)(M_PI / 180.0)), 0.f, 1e-3f),
                   "aimed along the spine -- +X here, i.e. yaw +90",
                   "yaw = %.2f deg", yaw);

                /* THE BOUNDARY. Standing the car exactly ON the station it just
                   crossed puts cp_progress ON that station, and `station > s`
                   landing on the wrong side would aim the arrow back at the
                   checkpoint the car is sitting on. At checkpoint 0 that costs a
                   LAP, because the start line's station is both 0 and spine_len, so
                   the very next metre driven sweeps past it again -- and the
                   opponents' lead is spine_len*(lap - 1) + distance, so a phantom
                   lap is a whole spine of error in the rubber band.
                 *
                 * It comes out right from cp_progress's own tie-break rather than
                 * from a guard in cp_ahead (there was one; it came out again -- see
                 * the comment there). Which is exactly why this has to be asserted:
                 * it is a property of another function, and nothing in the respawn
                 * path enforces it. */
                cp_resync(&r, pos[0], pos[1], pos[2]);
                ck(r.next != r.last,
                   "respawning ON a station does not aim the arrow back at it",
                   "next = %d, last = %d", r.next, r.last);
                ck(r.lap == lap0, "and the respawn itself counts no lap",
                   "lap %d -> %d", lap0, r.lap);
                /* Then DRIVE away from it, which is where the phantom lap would
                   actually land. */
                {
                    int i2, phantom = 0;
                    for (i2 = 1; i2 <= 40; i2++) {
                        cp_step(&r, pos[0] + (float)i2 * 0.25f, 0.f, pos[2],
                                0.016f);
                        if (r.passed == 0) phantom = 1;
                    }
                    ck(!phantom && r.lap == lap0,
                       "and driving off it does not cross the line a second time",
                       "lap %d -> %d", lap0, r.lap);
                }
            } else {
                ck(0, "the fixture reached a crossing of checkpoint 0", "");
            }

            /* cp_resync KEEPS what a death must not undo; cp_restart clears it.
               These are the two calls main.c's two respawn paths take, and getting
               them the wrong way round is the whole bug in either direction: a
               death that restarts the race, or a restart that inherits the
               previous run's lap and checkpoint. */
            r.lap = 3;
            r.last = 1;
            cp_resync(&r, c.cp[1].p[0][0], c.cp[1].p[0][1], c.cp[1].p[0][2]);
            ck(r.lap == 3 && r.last == 1,
               "a DEATH resync keeps the lap and the last checkpoint",
               "lap %d, last %d", r.lap, r.last);
            cp_restart(&r, path[0][0], path[0][1], path[0][2]);
            ck(r.lap == 0 && r.last == -1,
               "and a RESTART clears both, so the next death goes to the grid",
               "lap %d, last %d", r.lap, r.last);
            ck(!cp_respawn_pose(&r, pos, &yaw),
               "which is exactly what cp_respawn_pose then reports", "");
        }

        /* A CHECKPOINT WITH ITS FIRST REFINING POINT ON TOP OF IT gives no
         * direction at all, so cp_respawn_pose walks on down the spine instead of
         * handing back a yaw of zero. None of the ten shipped tracks does this --
         * which is precisely why it needs a fixture: a defensive branch no data
         * reaches is a branch nobody has ever run, and a mutant that deleted the
         * separation test survived every check above.
         *
         * Its own scene, because the markers are the scene's and this one is
         * deliberately malformed. */
        {
            static const char *tx[] = {
                "cp_ar_2_f1", "cp_ar_2_f2", "cp_ar_2_f3",
                "cp_ar_3_f1", "cp_ar_3_f2", "cp_ar_3_f3"
            };
            scene_t *s2 = make_scene(tx, 6);
            checkpoints_t d;
            float pos[3], yaw;

            add_marker(s2, "cp_1", 0.f, 0.f, 0.f, 0.f);
            add_marker(s2, "cp_1_1", 0.f, 0.f, 0.f, 0.f);      /* on top of it */
            add_marker(s2, "cp_1_2", 0.f, 0.f, -10.f, 0.f);    /* the real way on */
            add_marker(s2, "cp_2", 10.f, 0.f, -10.f, 0.f);

            cp_init(&d, s2, NULL);
            d.last = 0;
            ck(cp_respawn_pose(&d, pos, &yaw),
               "a coincident refining point does not defeat the respawn pose", "");
            /* cp_1_2 is 10 m along -Z, and local +Z on (sin yaw, 0, cos yaw) puts
               a heading down -Z at yaw 180. Read off the fixture's geometry. */
            ck(near(sinf(yaw * (float)(M_PI / 180.0)), 0.f, 1e-3f)
               && near(cosf(yaw * (float)(M_PI / 180.0)), -1.f, 1e-3f),
               "it walks past it to the next real point -- yaw 180, down -Z",
               "yaw = %.2f deg", yaw);
        }
    }

    /* cp_progress is continuous where cp_spine_dist snaps to a sample -- that is
       the whole reason the crossing can be located to a frame of travel. Measured
       against the fixture's own geometry: the first leg runs 10 m along +X from
       the origin, so 4 m along it is 4 m of arc, and the nearest spine SAMPLE is
       still cp_1 at the origin. */
    {
        float s4 = -1.f, s_snap = -1.f;
        cp_progress(&c, 4.f, 0.f, &s4);
        cp_spine_dist(&c, 4.f, 0.f, 0.f, &s_snap, NULL);
        ck(near(s4, 4.f, 1e-3f), "cp_progress interpolates along a leg",
           "%.3f m of arc, 4 m along a 10 m leg", s4);
        ck(near(s_snap, 0.f, 1e-3f),
           "while cp_spine_dist still snaps -- the AI's measure, left alone",
           "%.3f m", s_snap);
    }

    /* The lateral offset must not move the arc position: a car three metres wide
       of a straight leg is exactly as far along it. */
    {
        float on = -1.f, off = -1.f;
        cp_progress(&c, 4.f, 0.f, &on);
        cp_progress(&c, 4.f, -3.f, &off);
        ck(near(on, off, 1e-3f), "and a lateral offset does not move it",
           "%.3f vs %.3f m", on, off);
    }

    /* Reset what the driving left behind: the placement checks below pick their
       own `next` and their own clock. */
    c.lap = 0;
    c.passed = -1;

    /* --- placement and the distance fade -------------------------------- */
    c.next = 1;                              /* cp_2, at (20, 1, 0) */
    c.t = CP_PULSE_TIME;                     /* peak of the pulse */

    /* EVERY checkpoint is marked, so a check has to say WHICH one it means.
       Match a draw to a checkpoint by its quad's centre. */

    /* Standing on cp_2: it is inside MinDist and must not be drawn, while the
       other two are far away and must be. The ramp runs the other way from what
       this file first assumed -- getting it backwards is why no marker was ever
       visible on a track whose checkpoints are 30 to 90 m apart. */
    eye[0] = 20.f; eye[1] = 1.f; eye[2] = -2.f;
    gl_cap_reset();
    cp_draw(&c, eye);
    ck(cp_find_draw(20.f, 0.f) < 0, "the checkpoint you are standing on is NOT drawn",
       "%d draws, cp_2 at 2.0 m (MinDist %.2f)", glcap.n_draws, CP_MIN_DIST);
    ck(cp_find_draw(0.f, 0.f) >= 0 && cp_find_draw(20.f, 20.f) >= 0,
       "and the other two are", "%d draws total", glcap.n_draws);

    /* Back off so all three are beyond MaxDist. */
    eye[0] = 20.f; eye[1] = 1.f; eye[2] = -60.f;
    gl_cap_reset();
    cp_draw(&c, eye);
    ck(glcap.n_draws == c.n, "ALL checkpoints are marked, not just the next",
       "%d draws for %d checkpoints", glcap.n_draws, c.n);
    {
        int inext = cp_find_draw(20.f, 0.f);     /* cp_2, the current one */
        int iother = cp_find_draw(0.f, 0.f);     /* cp_1 */
        ck(inext >= 0 && near(glcap.draws[inext].color[3], 220.f / 255.f, 1e-3f),
           "the one being headed for breathes up to the clamped 220",
           "alpha %.3f", inext >= 0 ? glcap.draws[inext].color[3] : -1.f);
        ck(iother >= 0
           && near(glcap.draws[iother].color[3], CP_ALPHA_OTHER / 255.f, 1e-3f),
           "and the rest sit at the flat non-current alpha",
           "alpha %.3f", iother >= 0 ? glcap.draws[iother].color[3] : -1.f);
        ck(iother >= 0 && glcap.draws[iother].count == 4,
           "each is one quad", "%d verts", iother >= 0 ? glcap.draws[iother].count : -1);
        /* And READABLE. The engine's recovered 50/255 is what it tints solid
           GATE OBJECTS with; the port has no gates, and 50 on a sprite over
           textured sand is invisible -- which is the reported symptom. This
           floor is the judgement, stated as a number a regression will trip. */
        ck(iother >= 0 && glcap.draws[iother].color[3] > 0.3f,
           "and are opaque enough to actually read on sand",
           "alpha %.3f", iother >= 0 ? glcap.draws[iother].color[3] : -1.f);
    }

    {   /* halfway up the ramp, measured on the current checkpoint */
        float mid = 0.5f * (CP_MIN_DIST + CP_MAX_DIST);
        int i2;
        eye[2] = -mid;   /* the checkpoint is at z = 0, so this IS the distance */
        gl_cap_reset();
        cp_draw(&c, eye);
        i2 = cp_find_draw(20.f, 0.f);
        ck(i2 >= 0 && near(glcap.draws[i2].color[3], 0.5f * 220.f / 255.f, 2e-2f),
           "half the ramp halfway between MinDist and MaxDist",
           "alpha %.3f at %.2f m", i2 >= 0 ? glcap.draws[i2].color[3] : -1.f, mid);
    }

    {   /* the pulse: 50 -> 250 over 0.4 s, clamped at 220 */
        float lo, hi;
        int i2;
        eye[2] = -60.f;
        c.t = 0.f;
        gl_cap_reset();
        cp_draw(&c, eye);
        i2 = cp_find_draw(20.f, 0.f);
        lo = i2 >= 0 ? glcap.draws[i2].color[3] : -1.f;
        c.t = CP_PULSE_TIME;
        gl_cap_reset();
        cp_draw(&c, eye);
        i2 = cp_find_draw(20.f, 0.f);
        hi = i2 >= 0 ? glcap.draws[i2].color[3] : -1.f;
        ck(near(lo, 50.f / 255.f, 1e-3f) && near(hi, 220.f / 255.f, 1e-3f),
           "the current marker pulses between 50 and the clamped 220",
           "%.3f -> %.3f over %.1f s", lo, hi, CP_PULSE_TIME);
    }

    eye[2] = -60.f;
    c.t = CP_PULSE_TIME;
    gl_cap_reset();
    cp_draw(&c, eye);
    {
        float lo = 1e9f, hi = -1e9f, cx = 0.f, cz = 0.f;
        int d = cp_find_draw(20.f, 0.f);
        for (int i = 0; i < 4; i++) {
            int v = glcap.draws[d].first + i;
            if (glcap.pos[v][1] < lo) lo = glcap.pos[v][1];
            if (glcap.pos[v][1] > hi) hi = glcap.pos[v][1];
            cx += glcap.pos[v][0] * 0.25f;
            cz += glcap.pos[v][2] * 0.25f;
        }
        ck(near(cx, 20.f, 1e-3f) && near(cz, 0.f, 1e-3f),
           "anchored on the checkpoint marker", "centre (%.3f, %.3f)", cx, cz);
        {
            float h = 2.f * CP_SIZE * CP_SIZE_SCALE;
            /* no col in this fixture, so the "ground" is the marker's own y */
            ck(near(hi - lo, h, 1e-3f) && near(lo, 1.f, 1e-3f),
               "scaled Size tall, standing ON its anchor",
               "%.2f m tall, bottom at y = %.2f (anchor y = 1.00)", hi - lo, lo);
            /* Bound the HEIGHT against the car, not against CP_SIZE_SCALE:
               checking the code's own scale factor is self-referential, and a
               mutation putting the 2.92 m sprite back survived exactly that
               way. Five car lengths is generous and still catches it. */
            ck(hi - lo < 5.f * 0.42f,
               "and is not taller than five car lengths",
               "%.2f m against %.2f m", hi - lo, 5.f * 0.42f);
        }
    }

    /* --- the blink, and which arrow is which ---------------------------- */
    {
        GLuint seen[3];
        int distinct = 1;
        eye[0] = 20.f; eye[1] = 1.f; eye[2] = -60.f;
        for (int f = 0; f < 3; f++) {
            c.t = (f + 0.5f) * CP_BLINK_DELTA;
            gl_cap_reset();
            cp_draw(&c, eye);
            {
                int d = cp_find_draw(20.f, 0.f);
                seen[f] = d >= 0 ? glcap.draws[d].tex : 0;
            }
        }
        for (int f = 1; f < 3; f++)
            if (seen[f] == seen[f - 1])
                distinct = 0;
        ck(distinct, "three frames cycle at BlinkDelta",
           "tex %u %u %u over %.2f s", seen[0], seen[1], seen[2],
           3.f * CP_BLINK_DELTA);

        ck(seen[0] == c.tex_common[0],
           "an ordinary checkpoint gets the green (common) arrow",
           "tex %u, cp_ar_2_f1 is %u", seen[0], c.tex_common[0]);

        c.t = 0.05f;
        gl_cap_reset();
        cp_draw(&c, eye);
        {
            int d = cp_find_draw(0.f, 0.f);      /* cp_1 -- index 0 */
            ck(d >= 0 && glcap.draws[d].tex == c.tex_custom[0],
               "checkpoint 0 gets the red (custom) arrow",
               "tex %u, cp_ar_3_f1 is %u",
               d >= 0 ? glcap.draws[d].tex : 0, c.tex_custom[0]);
        }
    }

    /* --- the billboard turns to face the camera ------------------------- */
    {
        float rx, rz, len, dot;
        c.next = 1;
        eye[0] = 28.f; eye[1] = 1.f; eye[2] = 8.f;   /* 8,8 from cp_2 */
        gl_cap_reset();
        cp_draw(&c, eye);
        /* the quad's horizontal axis must be perpendicular to the view ray */
        {
            int d = cp_find_draw(20.f, 0.f);
            rx = glcap.pos[glcap.draws[d].first + 1][0]
               - glcap.pos[glcap.draws[d].first][0];
            rz = glcap.pos[glcap.draws[d].first + 1][2]
               - glcap.pos[glcap.draws[d].first][2];
        }
        len = sqrtf(rx * rx + rz * rz);
        dot = (rx * (20.f - eye[0]) + rz * (0.f - eye[2])) / len
              / sqrtf(64.f + 64.f);
        ck(near(dot, 0.f, 1e-4f), "quad axis is perpendicular to the view ray",
           "cos = %.6f", dot);

        set_world_state();
        gl_cap_reset();
        cp_draw(&c, eye);
        ck_state_restored("cp_draw");
    }

    /* --- and now the SAME drive on all TEN REAL SPINES -------------------
     *
     * A synthetic square and a real track find different bugs, and this is the
     * bit the square cannot speak for: the real spines carry 14 to 39 points over
     * 640 to 955 m, they bend, and several of them run back close alongside
     * themselves -- which is exactly where a nearest-segment projection can jump
     * legs and lose a crossing. Cheap enough to be unconditional: about a second
     * for all ten.
     *
     * `enabled` is forced, because it gates on the two arrow textures resolving
     * and that is a question about the ATLAS, not about the spine. Everything
     * else here is the shipped data.
     *
     * The number this replaces: measured over these same ten files, the old rule
     * -- advance when the nearest spine SAMPLE changes owner -- fires between
     * 7.3 m and 75.6 m before the checkpoint, mean 24 to 45 m per track. That is
     * the reported "the sound triggers somewhere in different places", quantified.
     */
    {
        static const char *const TRK[10] = {
            "beach_1", "beach_2", "beach_3", "beach_4", "country_1",
            "country_2", "country_3", "country_4", "urban_1", "urban_2"
        };
        int t, bad_fires = 0, bad_order = 0, bad_lap = 0, loaded = 0;
        float worst_all = -1.f;
        /* The respawn point, on the real spines: how many checkpoints across all
           ten tracks failed to give one, landed away from their own marker, or were
           aimed anywhere but forward along the spine. */
        int rsp_total = 0, rsp_none = 0, rsp_far = 0, rsp_backwards = 0;
        int rsp_aim_back = 0, rsp_refire = 0, rsp_phantom = 0;
        float rsp_worst_dot = 2.f;

        for (t = 0; t < 10; t++) {
            char p[64];
            scene_t ts;
            col_t tc;
            checkpoints_t rc;
            float path[CP_MAX * CP_MAX_POINTS + 1][3];
            float worst;
            int np, fires, order_ok, lap_bad;

            snprintf(p, sizeof(p), "assets/%s.vsc", TRK[t]);
            if (!scene_load(p, &ts))
                continue;                    /* counted below, not skipped */
            loaded++;
            snprintf(p, sizeof(p), "assets/%s.col", TRK[t]);
            memset(&tc, 0, sizeof(tc));
            col_load(p, &tc);
            cp_init(&rc, &ts, &tc);
            rc.enabled = (rc.n > 0);

            np = cp_flatten(&rc, path);
            /* 0.125 m: one 1/60 frame at this car's 7.5 m/s top speed, so the
               error bound below is bounded by a real frame and not by a step
               chosen to make it pass. */
            fires = cp_drive(&rc, path, np, 2, 0.125f, &worst, &order_ok,
                             &lap_bad);
            if (fires != 2 * rc.n || rc.lap != 2) bad_fires++;
            if (!order_ok) bad_order++;
            if (lap_bad) bad_lap++;
            if (worst > worst_all) worst_all = worst;

            /* THE RESPAWN POINT, for EVERY checkpoint on every real track. The
             * synthetic square has one leg shape; the real spines bend, and the
             * point after a checkpoint can be a refining point 2 m away or the next
             * checkpoint 90 m away, which is precisely what the walk in
             * cp_respawn_pose is for.
             *
             * `last` is set directly here rather than driven to, because driving to
             * each of up to eight checkpoints on each of ten tracks is the same
             * crossing test cp_drive already ran; what is under test here is the
             * POSE, and part 2's synthetic block above is where the latch is
             * checked against a real drive.
             */
            {
                int k;
                for (k = 0; k < rc.n; k++) {
                    float pos[3], yaw, fx, fz, dx, dz, len, dot;
                    const float *m = rc.cp[k].p[0];
                    rc.last = k;
                    rsp_total++;
                    if (!cp_respawn_pose(&rc, pos, &yaw)) { rsp_none++; continue; }
                    /* On its own marker. */
                    if (fabsf(pos[0] - m[0]) > 1e-3f
                        || fabsf(pos[2] - m[2]) > 1e-3f) rsp_far++;
                    /* AIMED FORWARD. The heading rbcar_init would build from this
                       yaw, dotted with the direction to the next point on the
                       spine -- which for a bend is not the same vector, so the
                       bound is "the right side of sideways" and not "identical".
                       A negative dot is a car dropped back facing the way it
                       came. */
                    fx = sinf(yaw * (float)(M_PI / 180.0));
                    fz = cosf(yaw * (float)(M_PI / 180.0));
                    if (rc.cp[k].n > 1) {
                        dx = rc.cp[k].p[1][0] - m[0];
                        dz = rc.cp[k].p[1][2] - m[2];
                    } else {
                        const float *nx2 = rc.cp[(k + 1) % rc.n].p[0];
                        dx = nx2[0] - m[0];
                        dz = nx2[2] - m[2];
                    }
                    len = sqrtf(dx * dx + dz * dz);
                    if (len < 1e-4f) continue;
                    dot = (fx * dx + fz * dz) / len;
                    if (dot < 0.99f) rsp_backwards++;
                    if (dot < rsp_worst_dot) rsp_worst_dot = dot;

                    /* AND THE BOUNDARY, on real geometry: standing the car on the
                       station it just crossed must not aim the arrow back at it,
                       and driving off it must not re-cross it. cp_ahead has no
                       guard for this -- it falls out of cp_progress putting a
                       marker exactly on its own stored arc length -- so this is the
                       only thing holding the property, and a phantom lap at
                       checkpoint 0 is the expensive failure. */
                    {
                        int lap0 = rc.lap, i2;
                        cp_resync(&rc, pos[0], pos[1], pos[2]);
                        if (rc.next == k) rsp_aim_back++;
                        for (i2 = 1; i2 <= 24; i2++) {
                            cp_step(&rc, pos[0] + fx * (float)i2 * 0.125f, 0.f,
                                    pos[2] + fz * (float)i2 * 0.125f, 0.016f);
                            if (rc.passed == k) rsp_refire++;
                        }
                        if (rc.lap != lap0) rsp_phantom++;
                        rc.lap = lap0;
                    }
                }
            }

            free(tc.tris); free(tc.start); free(tc.idx);
            scene_release(&ts);
        }

        ck(loaded == 10, "all ten packed tracks load (run from rccars_vita/)",
           "%d of 10", loaded);
        ck(loaded == 10 && bad_fires == 0,
           "every real spine passes each checkpoint once a lap, and counts the lap",
           "%d of %d tracks disagree", bad_fires, loaded);
        ck(bad_order == 0, "in the spine's own order on every one of them",
           "%d tracks out of order", bad_order);
        ck(bad_lap == 0, "and the lap only ever ticks over on the start line",
           "%d tracks tick it elsewhere", bad_lap);
        ck(loaded == 10 && worst_all >= 0.f && worst_all < 0.25f,
           "and no crossing on any real track fires away from its own marker",
           "worst %.3f m over ten tracks (the OLD rule: 7.3 to 75.6 m early)",
           worst_all);

        /* The respawn point, over every checkpoint the ten shipped tracks carry. */
        ck(loaded == 10 && rsp_total >= 30,
           "every real track's checkpoints are available as respawn points",
           "%d checkpoints over %d tracks", rsp_total, loaded);
        ck(rsp_none == 0, "and each one yields a respawn pose",
           "%d of %d declined", rsp_none, rsp_total);
        ck(rsp_far == 0, "on its own marker",
           "%d of %d landed elsewhere", rsp_far, rsp_total);
        ck(rsp_backwards == 0,
           "and aimed FORWARD along the spine, not back the way the car came",
           "%d of %d wrong-way; worst heading dot %.4f", rsp_backwards,
           rsp_total, rsp_worst_dot);
        ck(rsp_aim_back == 0,
           "respawning ON a station never aims the arrow back at it",
           "%d of %d on the real spines", rsp_aim_back, rsp_total);
        ck(rsp_refire == 0 && rsp_phantom == 0,
           "and driving off it re-crosses nothing -- no phantom lap at the line",
           "%d re-fires, %d phantom laps of %d", rsp_refire, rsp_phantom,
           rsp_total);
    }
}

/* ============================================================== part 3 ==== */

static void part3_water(void)
{
    static const char *tex[] = {"water_wave", "water_wave_alpha"};
    scene_t *s = make_scene(tex, 2);
    water_t w;
    batch_t *sea, *coast, *stream;
    col_t bed;
    float eye[3] = {0.f, 5.f, -20.f};
    unsigned int i;

    printf("\n-- part 3: the animated water --\n");

    /* Sea surface at y = 0 from z = 0 out to z = 40, a foam strip at the
       waterline, and a seabed that shelves from y = +0.2 (dry sand, above the
       surface) at z = -60 down to y = -3.75 out at z = +60, crossing y = 0 --
       the waterline -- at z = 0, and reaching 2.5 m under the far edge of the
       sea, which is about what beach_1's shelf does. */
    /* add_grid2 REALLOCS s->batches, so its return value is only good until the
       next call. Add them all, THEN take the pointers -- holding the first two
       across the third left them dangling and the harness died on the first
       water_draw with no output at all. */
    add_grid2(s, BATCH_WATER, -20.f, 0.f, 40.f, 40.f, 20, 0.f);
    /* 22 subdivisions across 40 m, so the foam band's triangles are 1.82 m --
       beach_1's real coast band has a 1.77 m median edge (p90 2.60 m). The
       fixture used to be 8, i.e. 5 m, and that is a fixture coarser than the
       thing it stands in for: the aliasing check below could not tell a signal
       the band CAN represent from one it cannot. Same lesson as the 6 m test
       seabed against the map's real 2.2 m. */
    add_grid2(s, BATCH_COAST, -20.f, -0.5f, 40.f, 0.5f, 22, -0.2f);
    add_grid2(s, BATCH_STREAM, -30.f, -10.f, 4.f, 20.f, 4, -0.1f);
    sea = &s->batches[0];
    coast = &s->batches[1];
    stream = &s->batches[2];
    /* distinct textures so the recorder can tell the three apart: they all
       submit per-vertex colour or their own alpha, and a check that cannot
       separate them ends up asserting the loosest bound on all of them */
    sea->gl_tex = s->tex_ids[0];
    coast->gl_tex = s->tex_ids[1];
    stream->gl_tex = next_tex_id++;
    add_marker(s, "water_wave_1", 0.f, 0.f, 30.f, 180.f);
    (void)coast;
    /* Depth matched to the real map: beach_1's sea bottoms out at 2.22 m. A
       6 m fixture seabed only ever exercises the fully-opaque end of the alpha
       ramp, and a mutation putting the ramp back to 0.5 m survived because of
       it. Make the fixture as shallow as the thing it stands in for. */
    /* The seabed is LARGER than the sea on purpose: a vertex sitting exactly on
       the collision grid's last cell edge finds no ground under it, reads as
       infinitely deep, and comes out fully opaque -- which is what the z = 40
       row did when the two were the same size. */
    make_shelf(&bed, 60.f, 30, 3.75f, -3.75f);

    water_init(&w, s, &bed);
    ck(w.n_spawn == 1, "one spawner per water_wave_N marker", "%d", w.n_spawn);
    ck(sea->rest != NULL, "the surface keeps a rest copy", "%p",
       (void *)sea->rest);

    /* --- shallow-water damping ------------------------------------------ */
    {
        float near_shore = -1.f, far_shore = -1.f;
        for (i = 0; i < sea->nverts; i++) {
            if (near(sea->verts[i].x, 0.f, 0.6f)) {
                if (near(sea->verts[i].z, 0.f, 0.6f)) near_shore = w.damp[0][i];
                if (near(sea->verts[i].z, 40.f, 0.6f)) far_shore = w.damp[0][i];
            }
        }
        ck(near(near_shore, 0.f, 0.05f),
           "damping is 0 where the seabed reaches the surface",
           "%.3f at the waterline", near_shore);
        ck(near(far_shore, 1.f, 1e-4f), "and 1 in deep water", "%.3f", far_shore);
    }

    /* --- the surface moves, and moves less near the shore ---------------- */
    {
        /* PEAK TO PEAK over time, not distance from rest. The surface carries a
           constant vertical offset (WSURF_OFFSET), and measuring against rest
           reports that offset as if it were wave height -- which it did, and
           read 0.39 m for a 0.02 m swell. Amplitude is immune to any static
           shift; "distance from rest" is not. */
        static float lo[8192], hi[8192];
        float amp_near = 0.f, amp_far = 0.f;
        unsigned int nv = sea->nverts < 8192 ? sea->nverts : 8192;
        for (i = 0; i < nv; i++) { lo[i] = 1e30f; hi[i] = -1e30f; }
        for (int step = 0; step < 120; step++) {
            water_step(&w, 1.f / 60.f);
            gl_cap_reset();
            water_draw(&w, eye);
            for (i = 0; i < nv; i++) {
                float y = sea->verts[i].y;
                if (y < lo[i]) lo[i] = y;
                if (y > hi[i]) hi[i] = y;
            }
        }
        for (i = 0; i < nv; i++) {
            float d = 0.5f * (hi[i] - lo[i]);
            if (sea->rest[i].z < 0.5f && d > amp_near) amp_near = d;
            if (sea->rest[i].z > 30.f && d > amp_far) amp_far = d;
        }
        ck(amp_far > 0.4f * WATER_SWELL_AMP, "the open sea moves", "peak %.4f m "
           "over 2 s (swell amp %.3f)", amp_far, WATER_SWELL_AMP);
        ck(amp_near < amp_far * 0.25f, "the shoreline barely moves",
           "%.4f m against %.3f m", amp_near, amp_far);
        /* The first build summed `amp` + `amp2` here for 0.41 m of swell on a
           beach whose car is 0.42 m long, and `amp` is a key FUN_00521540 does
           not even read.
         *
         * Bound it against the CAR, not against WATER_SWELL_AMP. Checking the
           code's own constant is self-referential -- raising the constant raises
           the bound, and a mutation putting 0.41 m back survived exactly that
           way. A swell taller than a tenth of the car is wrong however the
           constant is spelled. */
        ck(amp_far <= 0.1f * 0.42f,
           "and never by more than a tenth of the car's length",
           "%.4f m <= %.4f m", amp_far, 0.1f * 0.42f);
    }

    /* --- the UVs scroll and never leave the rest values behind ----------- */
    {
        float u0 = sea->verts[0].u;
        water_step(&w, 1.0f);
        gl_cap_reset();
        water_draw(&w, eye);
        ck(sea->verts[0].u > u0, "the surface texture scrolls",
           "u %.4f -> %.4f", u0, sea->verts[0].u);
        ck(near(sea->verts[0].u - sea->rest[0].u,
                w.t * WSURF_TEX_SPEED_MIN * WSURF_TEX_SCALE_X, 1e-3f),
           "scroll is computed from rest, not accumulated",
           "%.4f", sea->verts[0].u - sea->rest[0].u);
    }

    /* --- the scroll on the COAST path, which is a different function from the
           surface's and was untested: animate_scroll vs animate_surface ------ */
    {
        float du0 = coast->verts[0].v - coast->rest[0].v;
        float t0 = w.t;
        water_step(&w, 0.5f);
        gl_cap_reset();
        water_draw(&w, eye);
        ck(near(coast->verts[0].v - coast->rest[0].v,
                w.t * COAST_SCROLL_VEL, 1e-3f),
           "coast scroll is computed from rest too",
           "%.4f, expected %.4f", coast->verts[0].v - coast->rest[0].v,
           w.t * COAST_SCROLL_VEL);
        ck(coast->verts[0].v - coast->rest[0].v > du0,
           "and advances with the clock", "%.4f -> %.4f over %.2f s", du0,
           coast->verts[0].v - coast->rest[0].v, w.t - t0);
    }

    /* --- the foam band and the stream are DECALS and must clear the ground ---
     *
     * The level art lays them exactly on the terrain: measured against each
     * track's own collision grid, 89% of beach_1's coast vertices sit within
     * 1 cm of the surface under them, median difference 0.0000 m (beach_3 69%,
     * beach_4 85%, country_3 87%). D3D8 had ZBIAS; drawn flat against the sand
     * with none, the two z-fight and the white band strobes per pixel.
     *
     * Bound to the CAR's 0.42 m length, not to WATER_DECAL_LIFT -- an
     * assertion that reads the constant it is guarding moves with it and tests
     * nothing (this file has been caught doing that four times). What matters
     * is that the lift is real, and that it is small enough not to read as foam
     * hovering over the beach.
     */
    {
        const float car_len = 0.42f;
        float lift_min = 1e9f, lift_max = -1e9f;
        float bias_f = 0.f, bias_u = 0.f;
        int biased = 1, seen = 0, d;
        unsigned int k;

        for (k = 0; k < coast->nverts; k++) {
            float dy = coast->verts[k].y - coast->rest[k].y;
            if (dy < lift_min) lift_min = dy;
            if (dy > lift_max) lift_max = dy;
        }
        ck(lift_min > 0.f && lift_max <= 0.25f * car_len,
           "the foam band is lifted clear of the terrain",
           "%.1f..%.1f mm, and the car is %.0f mm long",
           lift_min * 1000.f, lift_max * 1000.f, car_len * 1000.f);
        ck(near(lift_min, lift_max, 1e-6f),
           "by the same amount everywhere, so the band keeps its shape",
           "%.4f..%.4f m", lift_min, lift_max);
        ck(near(stream->verts[0].y - stream->rest[0].y, lift_min, 1e-6f),
           "and the stream, which lies on its bed, gets the same treatment",
           "%.4f m vs the band's %.4f m",
           stream->verts[0].y - stream->rest[0].y, lift_min);

        /* The other half: a depth bias toward the camera, which keeps working
           at the far end of a shoreline where a fixed 2 cm no longer separates
           anything. Nothing in the submitted vertices can show this. */
        for (d = 0; d < glcap.n_draws; d++) {
            glcap_draw *dr = &glcap.draws[d];
            if (dr->tex != coast->gl_tex)
                continue;
            if (!seen++) { bias_f = dr->pol_factor; bias_u = dr->pol_units; }
            if (dr->pol_factor >= 0.f || dr->pol_units >= 0.f)
                biased = 0;
        }
        ck(seen > 0 && biased,
           "and drawn with a depth bias toward the camera",
           "%d coast draw(s), factor %.0f units %.0f", seen, bias_f, bias_u);
    }

    /* --- GL state has to be handed back the way water_draw found it -------- */
    set_world_state();
    gl_cap_reset();
    water_draw(&w, eye);
    ck_state_restored("water_draw");

    /* --- the coast band gets a per-vertex alpha from the water height ---- */
    {
        /* Compare as int, not as the stored unsigned char: with AlphaMax at 1.0
           the upper bound is 255 and "byte > 255" is vacuously false, which the
           compiler is right to flag and which would silently stop testing
           anything if AlphaMax were ever lowered. */
        const int lo = (int)(COAST_ALPHA_MIN * 255.f) - 1;
        const int hi = (int)(COAST_ALPHA_MAX * 255.f) + 1;
        int found = 0, in_range = 1, seen = 255, peak = 0;
        /* Sample over TIME, not over one frame's vertices: a single frame is a
           snapshot of a travelling wave read at a handful of positions, and the
           phases it happens to catch need not include a trough.
           The window is 30 s, and FIXED. It was two seconds, which was long
           enough for the 0.6 s ripple the foam used to run at and stopped being
           long enough the moment it became a 7.2 s swell. Scaling the window by
           COAST_WAVE_STRETCH would have fixed that and quietly given up the
           other half of the check: with a fixed window this also fails if the
           foam is slowed down so far it stops moving at all. */
        for (int step = 0; step < 30 * 60; step++) {
            water_step(&w, 1.f / 60.f);
            gl_cap_reset();
            water_draw(&w, eye);
            for (int d = 0; d < glcap.n_draws; d++) {
                glcap_draw *dr = &glcap.draws[d];
                    if (dr->tex != coast->gl_tex)
                    continue;
                for (int k = 0; k < dr->count; k++) {
                    int v = dr->first + k;
                    int a;
                    if (!glcap.has_color[v])
                        continue;
                    a = glcap.rgba[v][3];
                    found++;
                    if (a < lo || a > hi)
                        in_range = 0;
                    if (a < seen) seen = a;
                    if (a > peak) peak = a;
                }
            }
        }
        ck(found > 0 && in_range,
           "coast alpha stays within AlphaMin..AlphaMax",
           "%d samples in %d..%d, bounds %d..%d", found, seen, peak,
           lo + 1, hi - 1);
        /* and the water height actually drives it -- a constant alpha would
           pass the bounds check above on its own */
        ck(seen <= lo + 2 && peak >= hi - 2,
           "and the foam swings the full range as the water rises and falls",
           "%d..%d over 30 s (HeightOff %.2f m -> HeightOn %.2f m)",
           seen, peak, COAST_HEIGHT_OFF, COAST_HEIGHT_ON);
    }

    /* --- and it has to swing SLOWLY, and coherently along the band ---------
     *
     * The foam alpha is shore_height sampled once per band vertex. The two
     * recovered wave trains work out at 1.24 m / 0.60 s and 1.61 m / 1.80 s --
     * ripples, not a swell -- and read on a band whose triangles are 1.8 m that
     * is below Nyquist in space and three full cycles a second in time. The
     * result is a white band that strobes, which is what "the foam flickers a
     * lot" was, along with the z-fighting above.
     *
     * Both bounds are written against things COAST_WAVE_STRETCH cannot move:
     * how long a human reads as a pulse rather than a flash, and how much of
     * the band's own alpha range one triangle edge is allowed to cross. A check
     * that compares against the constant it guards passes any value of it.
     *
     * The first version of the second check compared neighbouring vertices with
     * pairs 20 m apart, on the theory that an aliased signal makes the two
     * equal. It does -- but 20 m is a fixed distance, and at stretch 1 and 2 it
     * lands near a whole number of wavelengths and the "unrelated" baseline
     * came back as 4 and 2 out of 219. A control that resonates with the thing
     * it is controlling for is not a control. An absolute bound cannot do that.
     */
    {
        const int row = 23;             /* the fixture band is 22 wide + 1 */
        const float range = (COAST_ALPHA_MAX - COAST_ALPHA_MIN) * 255.f;
        unsigned char *rgba = w.coast_rgba[1];
        float dt = 1.f / 60.f;
        float worst_rate = 0.f;         /* alpha per second at one vertex */
        float sum_nb = 0.f;
        int n_nb = 0, step, i;
        int prev = -1;

        for (step = 0; step < 600; step++) {
            water_step(&w, dt);
            gl_cap_reset();
            water_draw(&w, eye);
            /* how fast one vertex moves */
            {
                int a = rgba[0 * 4 + 3];
                if (prev >= 0) {
                    float r = fabsf((float)(a - prev)) / dt;
                    if (r > worst_rate) worst_rate = r;
                }
                prev = a;
            }
            /* and how far the pattern moves between two vertices of the band */
            for (i = 0; i + 1 < row; i++) {
                sum_nb += fabsf((float)(rgba[(i + 1) * 4 + 3] - rgba[i * 4 + 3]));
                n_nb++;
            }
        }
        {
            /* the time a full AlphaMin->AlphaMax swing would take at the
               fastest rate the foam ever moves */
            float swing = (worst_rate > 1e-3f) ? range / worst_rate : 1e9f;
            float nb = sum_nb / (float)n_nb;
            ck(swing >= 0.5f,
               "the foam pulses rather than strobes",
               "a full swing takes at least %.2f s (want >= 0.50)", swing);
            ck(nb < range / 6.f,
               "and one triangle of band crosses a small part of the range",
               "%.0f of %.0f over 1.8 m (want < %.0f)", nb, range, range / 6.f);
        }
    }

    /* --- the sea surface is translucent in the shallows ------------------
     *
     * This is the fix for "there is space between sea and ground": drawn opaque,
     * the sea meets the sand at a hard teal edge that reads as a step. WaterLOD
     * ships alphaMin/alphaMax/alphaPow for exactly this and the first build
     * extracted them and then never used them. */
    {
        int shallow = -1, deep = -1;
        unsigned int j;
        gl_cap_reset();
        water_draw(&w, eye);
        for (int d = 0; d < glcap.n_draws; d++) {
            glcap_draw *dr = &glcap.draws[d];
            if (dr->tex != sea->gl_tex)
                continue;
            for (int k = 0; k < dr->count; k++) {
                int v = dr->first + k;
                if (!glcap.has_color[v])
                    continue;
                if (near(glcap.pos[v][2], 0.f, 0.6f)) shallow = glcap.rgba[v][3];
                if (glcap.pos[v][2] > 30.f) deep = glcap.rgba[v][3];
            }
        }
        ck(shallow >= 0 && deep >= 0, "the sea submits a per-vertex alpha",
           "shallow %d, deep %d", shallow, deep);
        ck(shallow >= 0 && near((float)shallow / 255.f, WSURF_ALPHA_MIN, 0.02f),
           "transparent at the waterline, so the sand reads through",
           "alpha %d against alphaMin %.0f", shallow, WSURF_ALPHA_MIN * 255.f);
        /* Deeper water is more opaque -- but it is still WATER. The whole
           point of the reported bug is that you can see through it; a surface
           that reaches alphaMax over the deepest part of a 2.2 m shelf is a
           painted slab. Bound it against being see-through, not against the
           code's own ramp constant, which a regression would move with it. */
        ck(deep > shallow, "and deeper water is more opaque",
           "%d in the shallows, %d at %.1f m", shallow, deep, 2.5f);
        ck(deep >= 0 && (float)deep / 255.f < 0.85f,
           "but the deepest water is still see-through",
           "alpha %d of 255 at the map's own maximum depth", deep);
        (void)j;
    }

    /* --- the stream is see-through too ----------------------------------- */
    {
        int found = -1;
        gl_cap_reset();
        water_draw(&w, eye);
        for (int d = 0; d < glcap.n_draws; d++)
            if (glcap.draws[d].tex == stream->gl_tex
                && glcap.draws[d].mode == GL_TRIANGLES)
                found = d;
        ck(found >= 0, "the stream draws", "draw %d", found);
        /* Blended and see-through -- the property, not the constant. The river
           runs 2 to 13 cm over its bed, so an opaque sheet reads as tarmac. */
        ck(found >= 0 && glcap.draws[found].blend
           && glcap.draws[found].color[3] < 0.95f,
           "and is blended and see-through",
           "blend=%d alpha=%.2f", found >= 0 ? glcap.draws[found].blend : -1,
           found >= 0 ? glcap.draws[found].color[3] : -1.f);
    }

    /* --- waves spawn, travel toward shore, and expire -------------------- */
    {
        int peak = 0, tracked = -1, steps = 0;
        float first_z = 0.f, last_z = 0.f;
        for (int step = 0; step < 60 * 8; step++) {
            water_step(&w, 1.f / 60.f);
            if (w.n_live > peak) peak = w.n_live;
        }
        ck(peak > 0, "waves spawn on the TimeLong/TimeShort timers",
           "%d live at peak (long %.2f s, short %.2f s)", peak,
           WAVE_TIME_LONG, WAVE_TIME_SHORT);
        ck(peak <= WATER_MAX_WAVES, "and never exceed the pool", "%d <= %d",
           peak, WATER_MAX_WAVES);

        /* Follow ONE wave from birth to death. Sampling "whichever wave is live
           now", or even a fixed slot, compares different instances: they are
           born at random offsets along the crest and the slot is recycled the
           moment one expires, so the reading is noise rather than travel. */
        for (int k = 0; k < WATER_MAX_WAVES; k++)
            w.waves[k].active = 0;
        for (int step = 0; step < 60 * 8 && tracked < 0; step++) {
            water_step(&w, 1.f / 60.f);
            for (int k = 0; k < WATER_MAX_WAVES; k++)
                if (w.waves[k].active) { tracked = k; break; }
        }
        if (tracked >= 0) {
            first_z = w.waves[tracked].z;
            while (w.waves[tracked].active && steps < 60 * 10) {
                last_z = w.waves[tracked].z;
                water_step(&w, 1.f / 60.f);
                steps++;
            }
        }
        ck(steps > 0 && last_z < first_z,
           "and travel the way the marker faces (yaw 180 = toward -Z)",
           "slot %d: z %.2f -> %.2f over %d frames", tracked, first_z, last_z,
           steps);

        /* let every one of them die */
        for (int k = 0; k < WATER_MAX_WAVES; k++)
            w.waves[k].active = 0;
        for (int k = 0; k < w.n_spawn; k++) {
            w.spawn[k].t_long = 1e9f;
            w.spawn[k].t_short = 1e9f;
        }
        water_step(&w, 1.f / 60.f);
        ck(w.n_live == 0, "and expire when their life runs out", "%d live",
           w.n_live);
    }

    /* --- a wave sprite is a quad hinged on its crest, facing the camera -- */
    {
        int found = -1;
        w.waves[0].active = 1;
        w.waves[0].x = 0.f; w.waves[0].y = 0.f; w.waves[0].z = 10.f;
        w.waves[0].dx = 0.f; w.waves[0].dz = -1.f;
        w.waves[0].life = 2.f;
        w.waves[0].age = 1.f;                /* mid-life, full height */
        gl_cap_reset();
        water_draw(&w, eye);
        for (int d = 0; d < glcap.n_draws; d++)
            if (glcap.draws[d].mode == GL_TRIANGLE_FAN
                && glcap.draws[d].count == 4)
                found = d;
        ck(found >= 0, "the wave draws as one quad", "draw %d", found);
        if (found >= 0) {
            int f = glcap.draws[found].first;
            float ax = glcap.pos[f + 1][0] - glcap.pos[f][0];
            float az = glcap.pos[f + 1][2] - glcap.pos[f][2];
            float len = sqrtf(ax * ax + az * az);
            float rise = glcap.pos[f + 3][1] - glcap.pos[f][1];
            ck(near(len, 2.f * WAVE_LEN, 1e-3f),
               "crest is 2*Len across", "%.3f m (Len %.2f)", len, WAVE_LEN);
            ck(rise > 0.f && rise <= WAVE_HEIGHT + 1e-4f,
               "and rises by at most Height", "%.3f m (Height %.2f)", rise,
               WAVE_HEIGHT);
            ck(near(ax * w.waves[0].dx + az * w.waves[0].dz, 0.f, 1e-4f),
               "crest is perpendicular to the travel direction",
               "dot = %.6f", ax * w.waves[0].dx + az * w.waves[0].dz);
        }

        /* The life envelope: up over IncTime, flat, down over what is left
           after DecTime. Read off the drawn quad's alpha, because that is what
           a viewer sees -- and untested until a mutation that pinned the
           envelope at 1.0 survived the whole battery. */
        {
            float a_birth = -1.f, a_mid = -1.f, a_death = -1.f;
            const float when[3] = {0.001f, 0.5f, 0.999f};
            float *out[3] = {&a_birth, &a_mid, &a_death};
            for (int p = 0; p < 3; p++) {
                w.waves[0].active = 1;
                w.waves[0].life = 2.f;
                w.waves[0].age = when[p] * 2.f;
                gl_cap_reset();
                water_draw(&w, eye);
                for (int d = 0; d < glcap.n_draws; d++)
                    if (glcap.draws[d].mode == GL_TRIANGLE_FAN
                        && glcap.draws[d].count == 4)
                        *out[p] = glcap.draws[d].color[3];
            }
            ck(a_birth >= 0.f && a_birth < 0.02f,
               "wave fades in from nothing at birth", "alpha %.4f", a_birth);
            ck(near(a_mid, 1.f, 1e-3f), "is full through the middle of its life",
               "alpha %.4f (IncTime %.2f, DecTime %.2f)", a_mid,
               WAVE_INC_TIME, WAVE_DEC_TIME);
            ck(a_death >= 0.f && a_death < 0.02f, "and fades back out at the end",
               "alpha %.4f", a_death);
        }
    }
}

/* ============================================================== part 4 ==== */

static void part4_scene(void)
{
    static const char *tex[] = {"t"};
    scene_t *s = make_scene(tex, 1);
    int i;

    printf("\n-- part 4: scene_draw's matrix stack --\n");

    /* one unrigged batch and one rigged, which is the case that pushes */
    add_grid2(s, 0, 0.f, 0.f, 1.f, 1.f, 1, 0.f);
    add_grid2(s, 0, 0.f, 0.f, 1.f, 1.f, 1, 0.f);
    s->batches[1].part = 1;
    s->has_rig = 1;
    s->rig.n = 2;
    memset(s->rig.draw[1], 0, sizeof(s->rig.draw[1]));
    s->rig.draw[1][0] = s->rig.draw[1][5] = s->rig.draw[1][10] = 1.f;
    s->rig.draw[1][15] = 1.f;

    mat_depth = mat_max_depth = mat_underflow = 0;
    gl_cap_reset();
    scene_draw(s, 0, 0);
    ck(glcap.n_draws == 2, "both batches drawn", "%d draws", glcap.n_draws);
    ck(mat_max_depth == 1, "a rigged part pushes exactly one matrix",
       "peak depth %d", mat_max_depth);
    ck(mat_depth == 0 && !mat_underflow,
       "and the stack comes back to zero",
       "depth %d, underflow %d", mat_depth, mat_underflow);
    ck(!draw_had_matrix[0] && draw_had_matrix[1],
       "part 0 draws under the identity, only a rigged part gets a matrix",
       "batch 0 matrix=%d, batch 1 matrix=%d", draw_had_matrix[0],
       draw_had_matrix[1]);

    /* 100 frames, because an imbalance of one is invisible in the frame that
       causes it and only overflows a real 32-deep stack later */
    for (i = 0; i < 100; i++) {
        gl_cap_reset();
        scene_draw(s, 0, 0);
    }
    ck(mat_depth == 0 && !mat_underflow, "and does not drift over 100 frames",
       "depth %d after 100, underflow %d", mat_depth, mat_underflow);

    /* --- the lightmap goes on unit 1 and is turned off again ------------- */
    s->batches[1].gl_lm = 4242;
    gl_cap_reset();
    scene_draw(s, 0, 0);
    ck(draw_lm_tex[0] == 0 && draw_lm_tex[1] == 4242,
       "a lit batch binds its atlas on unit 1, an unlit one does not",
       "batch 0 lm=%u, batch 1 lm=%u", draw_lm_tex[0], draw_lm_tex[1]);
    ck(!glcap_unit_enabled[1] && !st_lm_array,
       "and unit 1 is left disabled for whatever draws next",
       "enabled=%d array=%d", glcap_unit_enabled[1], st_lm_array);
    ck(cur_unit == 0 && cur_client_unit == 0,
       "with the active unit handed back as unit 0",
       "active=%d client=%d", cur_unit, cur_client_unit);
    /* the second UV set has to be the LIGHTMAP one, not the base UVs again */
    ck(cap_lm_uv == (const float *)&s->batches[1].verts[0].lu,
       "and the second UV set points at lu/lv, not u/v",
       "offset %ld", (long)((const char *)cap_lm_uv
                            - (const char *)&s->batches[1].verts[0]));
    s->batches[1].gl_lm = 0;
}

/* ============================================================== part 5 ==== */

static void part5_antenna(void)
{
    static const char *tex[] = {"t"};
    scene_t *s = make_scene(tex, 1);
    antenna_t a;
    batch_t *b;
    float m[16], acc[3] = {0.f, 0.f, 0.f};
    float rest_tip, tip0, tipL, tipR, base_moved;
    int i, k;

    printf("\n-- part 5: the whip antenna --\n");

    /* a stand-in for ANTENNA: a thin column from y = 0.19 to y = 0.46, the
       real mesh's extent, on its own part so it gets its own batch */
    b = add_grid2(s, 0, -0.004f, -0.004f, 0.008f, 0.008f, 1, 0.f);
    for (k = 0; k < (int)b->nverts; k++)
        b->verts[k].y = 0.192f + 0.267f * (float)(k / 2);
    b->part = 1;
    s->has_rig = 1;
    s->rig.n = 2;
    snprintf(s->rig.part[1].name, sizeof(s->rig.part[1].name), "ANTENNA");

    antenna_init(&a, s, 0);
    ck(a.ready && a.batch == b, "binds to the ANTENNA part's batch",
       "ready=%d n=%d seg=%.4f", a.ready, a.n, a.seg);
    ck(a.n == ANT1_POINTS && near(a.seg, ANT1_SEG, 1e-5f),
       "chain is nPoints masses of chainLength/(nPoints-1)",
       "%d points, %.4f m each (total %.3f)", a.n, a.seg, ANT1_LENGTH);
    ck(near(a.base_y, 0.192f, 1e-4f) && near(a.tip_y, 0.459f, 1e-3f),
       "reads the mesh's own extent rather than assuming one",
       "base %.3f, tip %.3f", a.base_y, a.tip_y);

    body_matrix(m, 0.f, 0.f, 0.f, 0.f);
    rest_tip = b->rest[b->nverts - 1].x;

    /* --- level and still: the whip stands up ----------------------------
     *
     * Nudge it first. A perfectly symmetric chain with no bending force sits in
     * UNSTABLE equilibrium -- gravity is along its own axis and the length
     * constraint holds it -- so it stays vertical and this check passes for
     * entirely the wrong reason. The nudge makes it a stability test. */
    acc[0] = 4.f;
    for (i = 0; i < 20; i++)
        antenna_step(&a, m, acc, 0.f, 1.f / 60.f);
    acc[0] = 0.f;
    for (i = 0; i < 600; i++)
        antenna_step(&a, m, acc, 0.f, 1.f / 60.f);
    antenna_apply(&a);
    tip0 = b->verts[b->nverts - 1].x;
    /* HEIGHT, not just x. The first version of this checked the tip's x alone,
       and gravity pulls along -y: a whip that had collapsed into a hanging wire
       -- which is exactly what the no-bending-force version did -- kept x = 0
       and sailed through. It stands up or it does not. */
    ck(b->verts[b->nverts - 1].y > a.base_y + 0.85f * (a.tip_y - a.base_y),
       "a parked car's antenna STANDS UP",
       "tip y %.4f, base %.3f, rest tip %.3f",
       b->verts[b->nverts - 1].y, a.base_y, a.tip_y);
    ck(fabsf(tip0 - rest_tip) < 0.02f,
       "and is straight in plan", "tip x %.4f against rest %.4f",
       tip0, rest_tip);
    ck(near(b->verts[0].x, b->rest[0].x, 1e-5f)
       && near(b->verts[0].y, b->rest[0].y, 1e-3f),
       "and the base stays welded to the car",
       "base moved %.5f m",
       fabsf(b->verts[0].y - b->rest[0].y));

    /* --- accelerate one way, then the other ----------------------------- */
    /* Hold each acceleration long enough to SETTLE before reading. The spring
       rings at about 4 Hz and damps with a ~0.9 s time constant, so a 1-second
       sample lands mid-oscillation and its sign is a coin toss -- which is how
       an earlier version of this check read the deflection backwards. */
    acc[0] = 6.f;
    for (i = 0; i < 300; i++)
        antenna_step(&a, m, acc, 0.f, 1.f / 60.f);
    antenna_apply(&a);
    tipR = b->verts[b->nverts - 1].x;
    base_moved = fabsf(b->verts[0].x - b->rest[0].x);

    acc[0] = -6.f;
    for (i = 0; i < 300; i++)
        antenna_step(&a, m, acc, 0.f, 1.f / 60.f);
    antenna_apply(&a);
    tipL = b->verts[b->nverts - 1].x;

    ck(tipR < tip0 && tipL > tip0,
       "the tip lags the car's acceleration, both ways",
       "still %.4f, accel +x %.4f, accel -x %.4f", tip0, tipR, tipL);
    ck(base_moved < 1e-5f, "while the base never moves at all",
       "%.7f m", base_moved);

    /* --- the chain keeps its length ------------------------------------- */
    {
        float worst = 0.f;
        for (i = 1; i < a.n; i++) {
            float dx = a.p[i][0] - a.p[i-1][0];
            float dy = a.p[i][1] - a.p[i-1][1];
            float dz = a.p[i][2] - a.p[i-1][2];
            float len = sqrtf(dx*dx + dy*dy + dz*dz);
            float err = fabsf(len - a.seg);
            if (err > worst) worst = err;
        }
        ck(worst < 0.1f * a.seg, "segments hold their length under load",
           "worst error %.4f m on %.4f m segments", worst, a.seg);
    }

    /* --- and it settles back -------------------------------------------- */
    acc[0] = 0.f;
    for (i = 0; i < 900; i++)
        antenna_step(&a, m, acc, 0.f, 1.f / 60.f);
    antenna_apply(&a);
    /* Tolerances sized to the FAILURE, not to the noise. A whip under its own
       weight really does bow: the tip sits about 27 mm below vertical and a few
       mm off plan, and that is an equilibrium, not a failure to settle. What
       this has to catch is the bug it was written for -- a chain with no
       bending force lies down, putting the tip near 0.25 rather than 0.43 and
       leaving it wherever the last shove left it. 85% of span and 20 mm of plan
       separate those two by a wide margin; the mutation battery confirms it. */
    ck(fabsf(b->verts[b->nverts - 1].x - rest_tip) < 0.02f
       && b->verts[b->nverts - 1].y > a.base_y + 0.85f * (a.tip_y - a.base_y),
       "and settles back upright once the car stops",
       "tip (%.4f, %.4f) against rest (%.4f, %.3f)",
       b->verts[b->nverts - 1].x, b->verts[b->nverts - 1].y, rest_tip, a.tip_y);

    /* --- and the deformation has to REACH THE SCREEN ---------------------
     *
     * Every check above reads b->verts, and scene_load puts a car's batches in
     * a static VBO. So a buffered antenna simulates perfectly, passes all of
     * them, and draws the pose it was PACKED with -- which is what happened
     * when the vertex buffers went in, and what got reported as "the antenna is
     * static". No amount of asserting harder on the CPU vertices could see it:
     * the gap was that this fixture could not express the state. Buffer the
     * batch exactly as scene_load would, then read the geometry back through
     * the recorder, which resolves an offset against the buffer's own contents.
     */
    {
        GLuint vbo = 0, ibo = 0;
        antenna_t a3;
        float cpu_max = 0.f, drawn_max = 0.f;
        int d;

        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ibo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(vtx_t) * b->nverts),
                     b->verts, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizeiptr)(sizeof(unsigned short) * b->nidx),
                     b->idx, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        b->gl_vbo = vbo;
        b->gl_ibo = ibo;

        /* Re-binding a batch that already HAS a rest copy is the path that
           matters: scene_keep_rest returns early on the copy, so the
           unbuffering has to happen before that return or the second track
           load of a session would ship the bug. */
        antenna_init(&a3, s, 0);
        ck(a3.ready && !b->gl_vbo && !b->gl_ibo,
           "binding the antenna takes its batch back off the GPU",
           "ready=%d vbo=%u ibo=%u", a3.ready,
           (unsigned)b->gl_vbo, (unsigned)b->gl_ibo);

        acc[0] = 6.f;
        for (i = 0; i < 300; i++)
            antenna_step(&a3, m, acc, 0.f, 1.f / 60.f);
        acc[0] = 0.f;
        antenna_apply(&a3);

        /* The mesh is a 8 mm column about x = 0, so |x| out at the tip IS the
           deflection. Compare what was drawn against what was computed rather
           than against a number: a stale buffer draws the packed +-0.004 and
           cannot reach anywhere near it. */
        for (k = 0; k < (int)b->nverts; k++)
            if (fabsf(b->verts[k].x) > cpu_max) cpu_max = fabsf(b->verts[k].x);
        gl_cap_reset();
        scene_draw(s, 0, 0);
        for (d = 0; d < glcap.n_draws; d++) {
            int q;
            for (q = 0; q < glcap.draws[d].count; q++) {
                float x = fabsf(glcap.pos[glcap.draws[d].first + q][0]);
                if (x > drawn_max) drawn_max = x;
            }
        }
        ck(cpu_max > 0.01f && near(drawn_max, cpu_max, 1e-5f),
           "and the bent whip is the geometry that actually draws",
           "drawn %.4f m off axis, simulated %.4f, packed %.4f",
           drawn_max, cpu_max, 0.004f);
    }

    /* --- and on the REAL packed cars -------------------------------------
     *
     * The fixture above is a four-vertex column with a part table written by
     * hand, so it cannot say whether car<n>.vsc carries an ANTENNA part at all
     * -- and a car packed without one binds nothing, simulates nothing, and
     * draws a welded stick. Same symptom, different cause, and only the real
     * asset can tell them apart. scene_load is also the only thing that
     * buffers a batch for real. A missing car is a FAILED check, not a skip.
     */
    {
        static const char *files[3] = {
            "assets/car1.vsc", "assets/car2.vsc", "assets/car3.vsc"
        };
        static const char *names[3] = { "Overkill", "Buggy", "Hummer" };
        int ci;

        for (ci = 0; ci < 3; ci++) {
            scene_t cs;
            antenna_t ac;
            float bend = 0.f;

            if (!scene_load(files[ci], &cs)) {
                ck(0, "the packed car loads (run from rccars_vita/)",
                   "%s", files[ci]);
                continue;
            }
            antenna_init(&ac, &cs, ci);
            ck(ac.ready && ac.batch && !ac.batch->gl_vbo && !ac.batch->gl_ibo,
               "the packed car carries an ANTENNA part, and binding it takes "
               "that batch off the GPU",
               "%s: ready=%d, %u verts, vbo %u", names[ci], ac.ready,
               ac.batch ? ac.batch->nverts : 0u,
               ac.batch ? (unsigned)ac.batch->gl_vbo : 0u);
            if (ac.ready) {
                acc[0] = 6.f;
                for (i = 0; i < 300; i++)
                    antenna_step(&ac, m, acc, 0.f, 1.f / 60.f);
                acc[0] = 0.f;
                antenna_apply(&ac);
                for (k = 0; k < (int)ac.batch->nverts; k++) {
                    float dx = fabsf(ac.batch->verts[k].x
                                     - ac.batch->rest[k].x);
                    if (dx > bend) bend = dx;
                }
                /* Against the whip's own length, not a constant: the three
                   cars' antennae differ in height and in chainLength. */
                ck(bend > 0.05f * (ac.tip_y - ac.base_y),
                   "and the real mesh bends when the car accelerates",
                   "%s: %.1f mm on a %.0f mm whip", names[ci], bend * 1000.f,
                   (ac.tip_y - ac.base_y) * 1000.f);
            }
            scene_release(&cs);
        }
    }

    /* --- a scene without the part is simply inert ------------------------ */
    {
        scene_t *bare = make_scene(tex, 1);
        antenna_t a2;
        add_grid2(bare, 0, 0.f, 0.f, 1.f, 1.f, 1, 0.f);
        antenna_init(&a2, bare, 0);
        ck(!a2.ready, "no ANTENNA part means no antenna, not a crash",
           "ready=%d", a2.ready);
        antenna_step(&a2, m, acc, 5.f, 1.f / 60.f);
        antenna_apply(&a2);
        ck(1, "and stepping it anyway is safe", "no fault");
    }
}

/* ====================================================== parts 6, 7, 8 ==== */
/*
 * The three effects the car throws off: wheel dust and exhaust smoke (fx.c),
 * tyre marks (trace.c) and the body's env-map glance (envmap.c).
 *
 * All three read an rb_car, and none of them reads it through the physics -- they
 * want a pose, wheel contacts and the driver's inputs. So the car here is built
 * by hand rather than simulated: that makes "reversing" or "wheel 2 spinning" a
 * single assignment instead of something to be driven into existence, and it is
 * the only way to test the branches that need a state the model reaches rarely.
 */

/* A grid whose per-triangle material is NOT the fallback, because
 * col_material_at returns the fallback when there is no surf array at all -- and
 * then a test that "the surface decides the dust" passes against code that never
 * looks at the grid. That exact hole is recorded in CLAUDE.md for audio_test.
 */
static void plane_material(col_t *c, int mat, int fallback)
{
    unsigned int i;
    free(c->surf);
    c->surf = malloc(c->ntris);
    for (i = 0; i < c->ntris; i++)
        c->surf[i] = (unsigned char)mat;
    c->default_surf = (unsigned)fallback;
}

/* COL4: the ENGINE's own class per triangle, which is a different array and a
   different question from plane_material's keyword guess above. Pass cls < 0 to
   take the array away again, i.e. to be a pre-COL4 grid. */
static void plane_eng_surface(col_t *c, int cls)
{
    unsigned int i;
    free(c->eng_surf);
    c->eng_surf = NULL;
    if (cls < 0)
        return;
    c->eng_surf = malloc(c->ntris);
    for (i = 0; i < c->ntris; i++)
        c->eng_surf[i] = (unsigned char)cls;
}

/* A car standing on the plane at y = 0, nose along +Z, all four wheels down. */
static void fake_car(rb_car *c, float speed, float yaw_deg)
{
    int w;

    memset(c, 0, sizeof(*c));
    c->nwheels = 4;
    body_matrix(c->m, yaw_deg, 0.f, 0.f, 0.f);
    c->body.v[0] = c->m[8] * speed;
    c->body.v[1] = c->m[9] * speed;
    c->body.v[2] = c->m[10] * speed;
    for (w = 0; w < 4; w++) {
        /* wheel 0/1 front, 2/3 rear -- the indexing rb_car uses and therefore
           the indexing fx.c's front/rear gate uses */
        float lx = (w & 1) ? -0.141f : 0.141f;
        float lz = (w < 2) ? 0.149f : -0.149f;
        c->hit[w].active = 1;
        c->hit[w].point[0] = c->m[0] * lx + c->m[8] * lz;
        c->hit[w].point[1] = 0.f;
        c->hit[w].point[2] = c->m[2] * lx + c->m[10] * lz;
        c->hit[w].normal[0] = 0.f;
        c->hit[w].normal[1] = 1.f;
        c->hit[w].normal[2] = 0.f;
        c->wheel[w].radius = 0.0718f;
        /* rolling, not slipping: the rate the ground implies */
        c->wheel[w].spin_target = speed / 0.0718f;
        c->wheel[w].spin_w = c->wheel[w].spin_target;
    }
}

/* How many particles fx has alive, and the mean drawn quad size and alpha. */
static int fx_probe(fx_t *fx, float *size_out, float *alpha_out, float *rgb_out)
{
    static const float right[3] = {1.f, 0.f, 0.f};
    static const float up[3] = {0.f, 1.f, 0.f};
    /* An eye far enough away that ZIgnoreRad cannot hide anything, so a probe
       measures the plume rather than the cull. */
    static const float eye[3] = {0.f, 0.f, -30.f};
    float sz = 0.f, al = 0.f, rgb[3] = {0.f, 0.f, 0.f};
    int d, n = 0;

    gl_cap_reset();
    fx_draw(fx, eye, right, up);
    for (d = 0; d < glcap.n_draws; d++) {
        int i;
        for (i = glcap.draws[d].first; i + 5 < glcap.draws[d].first
                                           + glcap.draws[d].count; i += 6) {
            float dx = glcap.pos[i + 1][0] - glcap.pos[i][0];
            float dy = glcap.pos[i + 1][1] - glcap.pos[i][1];
            float dz = glcap.pos[i + 1][2] - glcap.pos[i][2];
            sz += sqrtf(dx * dx + dy * dy + dz * dz);
            al += (float)glcap.rgba[i][3];
            rgb[0] += (float)glcap.rgba[i][0];
            rgb[1] += (float)glcap.rgba[i][1];
            rgb[2] += (float)glcap.rgba[i][2];
            n++;
        }
    }
    if (n) {
        sz /= n; al /= n;
        rgb[0] /= n; rgb[1] /= n; rgb[2] /= n;
    }
    if (size_out) *size_out = sz;
    if (alpha_out) *alpha_out = al;
    if (rgb_out) { rgb_out[0] = rgb[0]; rgb_out[1] = rgb[1]; rgb_out[2] = rgb[2]; }
    return n;
}

/* Run the emitter for one frame on a given surface and report the particle
   count, having cleared the pool first. */
static int emit_once(fx_t *fx, rb_car *c, col_t *col, float dt, int frames)
{
    static const float eye[3] = {0.f, 0.f, 0.f};
    int i;

    for (i = 0; i < FX_MAX_PARTICLES; i++)
        fx->p[i].used = 0;
    memset(fx->carry_dust, 0, sizeof(fx->carry_dust));
    fx->carry_gas = 0.f;
    for (i = 0; i < frames; i++)
        fx_step(fx, c, col, eye, dt);
    return fx->n_live;
}

static void part6_fx(void)
{
    static const char *tex[] = {"dust"};
    scene_t *s = make_scene(tex, 1);
    col_t plane;
    fx_t fx;
    rb_car c;
    int n_asphalt, n_gravel, n_front, n_rear, n;
    float sz0, szL, al0, alL, rgb_wet[3], rgb_sand[3];

    printf("\n-- part 6: wheel dust and exhaust smoke --\n");

    make_plane(&plane, 8.f, 8, 0.f);
    fx_init(&fx, s);
    ck(fx.enabled && fx.tex == s->tex_ids[0],
       "binds the game's own 'dust' sprite", "enabled=%d tex=%u",
       fx.enabled, fx.tex);

    /* ---- the surface decides whether there is dust at all --------------
     *
     * surf_default's DustX_IntScale is 0 and FUN_0052e320 returns the moment it
     * reads a scale below 1e-06, so a road raises nothing. That is not a
     * simplification: it is why the original's dust only appears off-road.
     */
    fake_car(&c, 5.f, 0.f);
    plane_material(&plane, SURF_ASPHALT, SURF_GRAVEL);
    n_asphalt = emit_once(&fx, &c, &plane, 1.f / 60.f, 30);
    plane_material(&plane, SURF_GRAVEL, SURF_ASPHALT);
    n_gravel = emit_once(&fx, &c, &plane, 1.f / 60.f, 30);
    ck(n_asphalt == 0 && n_gravel > 0,
       "dust on the stone road, none on asphalt",
       "asphalt %d particles, gravel %d", n_asphalt, n_gravel);
    /* The fallback is the OTHER material in each case, so a lookup that ignores
       the grid gets the answer exactly backwards rather than accidentally right. */

    /* ---- and which end of the car raises it ---------------------------- */
    {
        int w;
        plane_material(&plane, SURF_GRAVEL, SURF_ASPHALT);
        n_front = n_rear = 0;
        for (w = 0; w < 4; w++) {
            float r = fx_dust_rate(&fx, &c, w, 5, 18.f);
            if (w < 2) n_front += (r > 0.f);
            else n_rear += (r > 0.f);
        }
        ck(n_front == 0 && n_rear == 2,
           "driving forward, only the REAR wheels raise dust",
           "%d front, %d rear", n_front, n_rear);

        /* Reversing: the same car with its velocity behind it. */
        c.body.v[0] = -c.m[8] * 5.f;
        c.body.v[1] = -c.m[9] * 5.f;
        c.body.v[2] = -c.m[10] * 5.f;
        n_front = n_rear = 0;
        for (w = 0; w < 4; w++) {
            float r = fx_dust_rate(&fx, &c, w, 5, 18.f);
            if (w < 2) n_front += (r > 0.f);
            else n_rear += (r > 0.f);
        }
        ck(n_front == 2 && n_rear == 0,
           "reversing, only the FRONT wheels raise dust",
           "%d front, %d rear", n_front, n_rear);
        fake_car(&c, 5.f, 0.f);
    }

    /* ---- dune sand is the exception: every wheel ----------------------- */
    ck(fx_dust_rate(&fx, &c, 0, 3, 18.f) > 0.f
       && fx_dust_rate(&fx, &c, 3, 3, 18.f) > 0.f,
       "on dune sand every wheel raises dust",
       "front %.1f rear %.1f /s", fx_dust_rate(&fx, &c, 0, 3, 18.f),
       fx_dust_rate(&fx, &c, 3, 3, 18.f));

    /* ---- the emission carry -------------------------------------------
     *
     * FUN_00530b70 keeps the fractional part of rate*dt. Without it every rate
     * below one particle per frame -- 60/s at this timestep, which is most of
     * the dust the curve ever asks for -- truncates to zero and nothing is ever
     * emitted at all.
     */
    {
        float rate = fx_dust_rate(&fx, &c, 2, 5, 4.f);
        int frames = 60;
        n = emit_once(&fx, &c, &plane, 1.f / 60.f, frames);
        ck(rate > 0.f && rate < 60.f && n > 0,
           "a rate under one per frame still emits",
           "%.2f particles/s over %d frames -> %d alive", rate, frames, n);
    }

    /* ---- the colour comes from the surface table ----------------------- */
    plane_material(&plane, SURF_WETSAND, SURF_ASPHALT);
    emit_once(&fx, &c, &plane, 1.f / 60.f, 20);
    fx_probe(&fx, NULL, NULL, rgb_wet);
    plane_material(&plane, SURF_SAND, SURF_ASPHALT);
    emit_once(&fx, &c, &plane, 1.f / 60.f, 20);
    fx_probe(&fx, NULL, NULL, rgb_sand);
    /* Bound to a PROPERTY of the recovered tables rather than to the tables:
       wet sand is a darker, browner dust than dry sand, and neither is the white
       the default table carries. A lookup that fell back to the default, or that
       used one table for everything, breaks both halves. */
    ck(rgb_wet[2] < rgb_wet[0] && rgb_sand[2] < rgb_sand[0]
       && rgb_wet[0] < rgb_sand[0] && rgb_wet[0] > 1.f,
       "wet sand throws darker, browner dust than dry sand",
       "wet %.0f,%.0f,%.0f  dry %.0f,%.0f,%.0f",
       rgb_wet[0], rgb_wet[1], rgb_wet[2],
       rgb_sand[0], rgb_sand[1], rgb_sand[2]);

    /* ---- a puff shrinks and fades over its life ------------------------ */
    plane_material(&plane, SURF_SAND, SURF_ASPHALT);
    plane_eng_surface(&plane, 1);                     /* engine: sand, 1 s */
    emit_once(&fx, &c, &plane, 1.f / 60.f, 2);
    n = fx_probe(&fx, &sz0, &al0, NULL);
    {
        /* Stop emitting and age what is there. surf_sand lives 1 s +- 0.25. */
        static const float eye[3] = {0.f, 0.f, 0.f};
        int i;
        c.in.accel = 0;
        c.body.v[0] = c.body.v[1] = c.body.v[2] = 0.f;
        for (i = 0; i < 40; i++)
            fx_step(&fx, &c, &plane, eye, 1.f / 60.f);
    }
    fx_probe(&fx, &szL, &alL, NULL);
    /* DynamicScale multiplies the REMAINING life in seconds (fx_scale), so a
       particle is largest at birth and settles to the base sprite size. This
       check used to assert the opposite and passed for as long as the ramp ran
       backwards; the exhaust is where that showed, because at a life of 0.06 s
       the two readings differ by 11x. */
    ck(n > 0 && szL < sz0 * 0.5f,
       "a puff is biggest at birth and shrinks", "%.4f m -> %.4f m across",
       sz0, szL);
    ck(alL < al0 * 0.7f,
       "and fades", "alpha %.0f -> %.0f", al0, alL);
    /* AND HOW BIG IT GETS, bound to the car rather than to the constant that
       sets it -- FX_DUST_SPRITE_SCALE could otherwise move and drag this check
       along with it. A puff at its largest should be the sort of size the car
       is; it was 2.3x the car's LENGTH, which is what "tyre dust too strong"
       was. The floor stops the opposite mistake of tuning the dust to nothing. */
    ck(sz0 > RB_CARS[0].extent[2] * 0.2f && sz0 < RB_CARS[0].extent[2] * 1.3f,
       "and it is about the size of the car, not bigger than it",
       "%.3f m across at birth, car is %.3f m long (%.2fx)",
       sz0, RB_CARS[0].extent[2], sz0 / RB_CARS[0].extent[2]);

    /* ---- the near-camera radius --------------------------------------- */
    {
        static const float right[3] = {1.f, 0.f, 0.f};
        static const float up[3] = {0.f, 1.f, 0.f};
        float eye_near[3];
        int drawn_far, drawn_near;

        fake_car(&c, 5.f, 0.f);
        emit_once(&fx, &c, &plane, 1.f / 60.f, 10);
        drawn_far = fx_probe(&fx, NULL, NULL, NULL);
        /* Put the camera ON the plume. ZIgnoreRad is 2 m and this chase camera
           sits 0.79 m behind the car, so this is the normal case, not an edge. */
        eye_near[0] = 0.f; eye_near[1] = 0.f; eye_near[2] = 0.f;
        gl_cap_reset();
        fx_draw(&fx, eye_near, right, up);
        drawn_near = glcap.n_draws;
        ck(drawn_far > 0 && drawn_near == 0,
           "dust inside ZIgnoreRad is not drawn",
           "%d quads at 30 m, %d draws at 0 m", drawn_far, drawn_near);
    }

    /* ---- and the EXHAUST is not subject to it -------------------------
     *
     * ZIgnoreRad is the dust system's callback and the exhaust registers none:
     * both create functions zero-fill their descriptor and only the dust writes
     * the slot (0x0052e07e against 0x005301f7). Sharing one pool in the port let
     * the dust's radius hide the smoke, and EG_LIFE is 0.06 s at EG_SPEED
     * 0.98 m/s -- 6 cm of travel against a chase camera 0.79-1.35 m away -- so
     * the exhaust could never be drawn at ANY speed, on any track.
     *
     * Nothing here could have caught that: every other exhaust check goes
     * through fx_probe, whose eye is 30 m away precisely so the cull cannot
     * interfere. The state the bug lives in is a camera at CHASE distance, so
     * that is the fixture, and both systems are measured through the same eye
     * -- the claim is about which system, not about which distance. */
    {
        static const float right[3] = {1.f, 0.f, 0.f};
        static const float up[3] = {0.f, 1.f, 0.f};
        float pipe[3] = {0.05f, 0.06f, -0.20f};
        /* ON the car, so both systems are well inside FX_ZIGNORE_RAD. The claim
           is about which SYSTEM the radius applies to, so one eye measures
           both. */
        float eye_on[3] = {0.f, 0.f, 0.f};
        /* cam.c's recovered rest pose: defDistXZ 0.7929 m behind, defDistY
           0.3636 m above. This is where the camera actually is at its CLOSEST. */
        float eye_chase[3] = {0.f, 0.3636f, -0.7929f};
        int dust_near, gas_near, dust_chase;

        plane_material(&plane, SURF_SAND, SURF_SAND);
        plane_eng_surface(&plane, -1);
        fake_car(&c, 5.f, 0.f);
        emit_once(&fx, &c, &plane, 1.f / 60.f, 10);
        gl_cap_reset();
        fx_draw(&fx, eye_on, right, up);
        dust_near = glcap.n_draws;
        /* And the same dust from where the chase camera really sits: this is
           the reported bug. A rear contact patch is 0.776 m from that eye, so
           at the recovered 2 m radius the plume could not start until it had
           fallen ~1.2 m behind the tyre -- three car lengths. */
        gl_cap_reset();
        fx_draw(&fx, eye_chase, right, up);
        dust_chase = glcap.n_draws;

        plane_material(&plane, SURF_ASPHALT, SURF_ASPHALT);   /* no dust */
        fx_set_pipe(&fx, pipe);
        fake_car(&c, 5.f, 0.f);
        c.in.accel = 1;
        c.in.throttle = 1.f;
        emit_once(&fx, &c, &plane, 1.f / 60.f, 10);
        gl_cap_reset();
        fx_draw(&fx, eye_on, right, up);
        gas_near = glcap.n_draws;

        ck(gas_near > 0 && dust_near == 0,
           "the near-camera radius is the dust's rule and not the exhaust's",
           "on the car: %d smoke draws, %d dust draws", gas_near, dust_near);
        ck(dust_chase > 0,
           "and dust raised at the tyres is visible from the chase camera",
           "%d draws at the recovered rest pose %.3f m back, %.3f m up",
           dust_chase, 0.7929f, 0.3636f);
    }

    /* ---- the dust table is indexed by the ENGINE's class, not the keyword ----
     *
     * fx_surf[] is indexed by the id FUN_0052ee10 switches on, and COL4 carries
     * exactly that id. It used to be indexed with col_material_at's keyword
     * guess instead -- pack_col.py's own docstring says that array is audio only
     * -- and the two disagree on the surface a beach is made of: `sand_halfdry`
     * is in SURF_RE's `wetsand` pattern while the engine's own data gives it
     * class 1, SAND. DustX_TimeLife is 1.0 s on sand and 0.05 s on wet sand, and
     * a 0.05 s particle cannot cross ZIgnoreRad before it dies, so that one
     * disagreement was the whole of "no dust on the beach".
     *
     * Set the two arrays AGAINST each other, both ways round, so the check can
     * only pass if the engine's class is what is read. */
    {
        float life_eng_sand, life_eng_wet;
        int k;

        plane_material(&plane, SURF_WETSAND, SURF_WETSAND);
        plane_eng_surface(&plane, 1);                 /* engine says SAND */
        fake_car(&c, 5.f, 0.f);
        emit_once(&fx, &c, &plane, 1.f / 60.f, 4);
        life_eng_sand = 0.f;
        for (k = 0; k < FX_MAX_PARTICLES; k++)
            if (fx.p[k].used && fx.p[k].life > life_eng_sand)
                life_eng_sand = fx.p[k].life;

        plane_material(&plane, SURF_SAND, SURF_SAND);
        plane_eng_surface(&plane, 2);                 /* engine says WETSAND */
        emit_once(&fx, &c, &plane, 1.f / 60.f, 4);
        life_eng_wet = 0.f;
        for (k = 0; k < FX_MAX_PARTICLES; k++)
            if (fx.p[k].used && fx.p[k].life > life_eng_wet)
                life_eng_wet = fx.p[k].life;

        ck(life_eng_sand >= fx_surf[1].life && life_eng_wet <= fx_surf[2].life,
           "the engine's own surface class picks the dust row",
           "eng sand %.3f s (row 1 is %.3f), eng wetsand %.3f s (row 2 is %.3f)",
           life_eng_sand, fx_surf[1].life, life_eng_wet, fx_surf[2].life);

        /* And a pre-COL4 grid still raises dust rather than going silent. */
        plane_material(&plane, SURF_SAND, SURF_SAND);
        plane_eng_surface(&plane, -1);
        ck(emit_once(&fx, &c, &plane, 1.f / 60.f, 4) > 0,
           "a pre-COL4 grid falls back to the keyword map", "%d particles",
           fx.n_live);
    }

    /* ---- the exhaust -------------------------------------------------- */
    {
        static const float eye[3] = {0.f, 0.f, 0.f};
        float pipe[3] = {0.05f, 0.06f, -0.20f};
        float rgb_idle[3], rgb_bang[3];
        int i, n_off, n_on;

        /* asphalt, so nothing that follows can be dust */
        plane_material(&plane, SURF_ASPHALT, SURF_ASPHALT);
        fx_set_pipe(&fx, pipe);
        fake_car(&c, 5.f, 0.f);
        n_off = emit_once(&fx, &c, &plane, 1.f / 60.f, 30);
        c.in.accel = 1;
        c.in.throttle = 1.f;
        n_on = emit_once(&fx, &c, &plane, 1.f / 60.f, 30);
        ck(n_off == 0 && n_on > 0, "the pipe smokes on the throttle and not off it",
           "%d particles off, %d on", n_off, n_on);

        /* And HOW BIG it is, bound to the car rather than to any constant that
           could move it. DynamicScale multiplies the remaining life in seconds
           and the exhaust lives 0.06 s, so the peak is 29.3*0.06 + 1 = 2.76x the
           base sprite -- a puff a fifth of a car long. Read as an age fraction
           it was the full 30.3x, 0.91 m on a 0.42 m car: bigger than the thing
           emitting it, which is what was reported. Half a car length is an order
           of magnitude clear of both. */
        {
            float sz_gas = 0.f;
            fx_probe(&fx, &sz_gas, NULL, NULL);
            ck(sz_gas > 0.f && sz_gas < RB_CARS[0].extent[2] * 0.5f,
               "and the puff is smaller than the car it comes out of",
               "%.3f m across, car is %.3f m long",
               sz_gas, RB_CARS[0].extent[2]);
        }

        /* Where it comes out. The pipe is in BODY space and the emitter has to
           put it through the car matrix -- so turn the car and the plume has to
           follow. Constructed and measured, not assumed: that rule has caught
           four bugs in this port. */
        fake_car(&c, 5.f, 90.f);
        c.in.accel = 1;
        c.in.throttle = 1.f;
        emit_once(&fx, &c, &plane, 1.f / 60.f, 1);
        {
            float want[3], best = 1e9f, tol = 0.f;
            int k;
            want[0] = pipe[0] * c.m[0] + pipe[1] * c.m[4] + pipe[2] * c.m[8];
            want[1] = pipe[0] * c.m[1] + pipe[1] * c.m[5] + pipe[2] * c.m[9];
            want[2] = pipe[0] * c.m[2] + pipe[1] * c.m[6] + pipe[2] * c.m[10];
            for (k = 0; k < FX_MAX_PARTICLES; k++) {
                float dx, dy, dz, d, sp;
                if (!fx.p[k].used)
                    continue;
                dx = fx.p[k].x - want[0];
                dy = fx.p[k].y - want[1];
                dz = fx.p[k].z - want[2];
                d = sqrtf(dx * dx + dy * dy + dz * dz);
                sp = sqrtf(fx.p[k].vx * fx.p[k].vx + fx.p[k].vy * fx.p[k].vy
                           + fx.p[k].vz * fx.p[k].vz);
                if (d < best) {
                    best = d;
                    /* A particle is emitted and then moved in the SAME step, so
                       it is already one frame downstream of the pipe. The bound
                       is that frame's travel plus a centimetre -- which still
                       separates a transformed tip from an untransformed one,
                       because at yaw 90 those are 0.25 m apart. */
                    tol = sp / 60.f + 0.01f;
                }
            }
            ck(best < tol,
               "the smoke leaves the pipe, through the body matrix",
               "nearest particle %.1f mm from the transformed tip, one frame of "
               "travel is %.1f mm", best * 1000.f, (tol - 0.01f) * 1000.f);
        }

        /* AND ALONG THE PIPE, not along the car.
         *
         * On the REAL cars, because a synthetic fixture has no rig and takes
         * fx_set_pipe's body -Z fallback -- which is exactly the assumption
         * under test, so it could only ever agree with itself. The Buggy's
         * level 1 pipe exits at (-0.565, 0, -0.825) in body space, 34.4 deg off
         * the back of the car, so the plume direction can tell the two apart;
         * the old code sent it straight backwards out of a pipe pointing down
         * the left flank. Bound to the ANGLE between the plume and the node's
         * own axis, which is a property of the car, not of any constant here. */
        {
            const char *files[3] = {"assets/car1.vsc", "assets/car2.vsc",
                                    "assets/car3.vsc"};
            const char *cname[3] = {"Overkill", "Buggy", "Hummer"};
            int ci, worst_ci = -1;
            float worst_deg = 0.f, spread = 0.f;

            for (ci = 0; ci < 3; ci++) {
                scene_t cs;
                float ang, back;
                if (!scene_load(files[ci], &cs))
                    continue;
                fx_init(&fx, &cs);
                fx.tex = 1;            /* the fixture scene has no dust sprite */
                fx.enabled = 1;
                /* `ci` indexes the three real cars in order, so its com_oy is
                   the right model->body shift for this scene. */
                if (fx_pipe_from_rig(&fx, &cs.rig, 0, rbcar_com_oy(ci))) {
                    /* The car at identity, so body space IS world space. It
                       has to be MOVING -- the rate curve gives nothing at rest
                       -- and spawn_gas adds the car's velocity to the ejection,
                       which move() then decays on vx/vz but not vy. Subtracting
                       the raw car velocity therefore leaves a few degrees of
                       residue that is the damping, not the pipe. */
                    fake_car(&c, 5.f, 0.f);
                    c.in.accel = 1;
                    c.in.throttle = 1.f;
                    plane_material(&plane, SURF_ASPHALT, SURF_ASPHALT);
                    emit_once(&fx, &c, &plane, 1.f / 60.f, 1);
                    {
                        int k, found = 0;
                        float d[3] = {0.f, 0.f, 0.f}, n;
                        /* spawn_gas writes EG_SPEED*dir + the CAR's velocity,
                           and at 5 m/s the car term is 5x the ejection -- so
                           the ejection has to be isolated or this measures the
                           car's heading. */
                        for (k = 0; k < FX_MAX_PARTICLES; k++)
                            if (fx.p[k].used) {
                                d[0] = fx.p[k].vx - c.body.v[0];
                                d[1] = fx.p[k].vy - c.body.v[1];
                                d[2] = fx.p[k].vz - c.body.v[2];
                                found = 1; break;
                            }
                        n = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
                        if (found && n > 1e-6f) {
                            float dot = (d[0]*fx.pipe_dir[0] + d[1]*fx.pipe_dir[1]
                                         + d[2]*fx.pipe_dir[2]) / n;
                            if (dot > 1.f) dot = 1.f;
                            if (dot < -1.f) dot = -1.f;
                            ang = acosf(dot) * 57.29578f;
                            back = acosf(-fx.pipe_dir[2] > 1.f ? 1.f
                                         : -fx.pipe_dir[2]) * 57.29578f;
                            if (ang > worst_deg) { worst_deg = ang; worst_ci = ci; }
                            if (back > spread) spread = back;
                        }
                    }
                }
                scene_release(&cs);
            }
            ck(worst_ci >= 0 && worst_deg < 15.f && spread > 20.f,
               "the smoke leaves along the PIPE, not along the car",
               "worst %.2f deg off its own pipe axis (%s), against pipes up to "
               "%.1f deg off the back of the car -- the old body -Z would score "
               "that second number",
               worst_deg, worst_ci >= 0 ? cname[worst_ci] : "-", spread);
        }

        /* The backfire. A rising edge of the Jump action above 10 km/h holds
           ExplodeTime and darkens the smoke toward ExplodeColor*255. Bound to
           "much darker than white", not to the constant. */
        fake_car(&c, 5.f, 0.f);
        c.in.accel = 1;
        c.in.throttle = 1.f;
        emit_once(&fx, &c, &plane, 1.f / 60.f, 10);
        fx_probe(&fx, NULL, NULL, rgb_idle);
        c.in.jump = 1;
        for (i = 0; i < FX_MAX_PARTICLES; i++)
            fx.p[i].used = 0;
        for (i = 0; i < 20; i++)
            fx_step(&fx, &c, &plane, eye, 1.f / 60.f);
        fx_probe(&fx, NULL, NULL, rgb_bang);
        ck(rgb_idle[0] > 200.f && rgb_bang[0] < rgb_idle[0] * 0.6f,
           "a backfire darkens the smoke", "grey %.0f -> %.0f",
           rgb_idle[0], rgb_bang[0]);
    }

    /* ---- and it puts the world back ----------------------------------- */
    {
        static const float right[3] = {1.f, 0.f, 0.f};
        static const float up[3] = {0.f, 1.f, 0.f};
        static const float eye[3] = {0.f, 0.f, -30.f};
        set_world_state();
        fx_draw(&fx, eye, right, up);
        ck_state_restored("fx_draw");
    }
}

/* ---------------------------------------------------------------- part 7 -- */

/* Drive the car along a path and feed trace_step, returning how many ring slots
   the wheel ended up using.
 *
 * `keep` continues from wherever the car is instead of wiping the ring. It exists
 * because the version without it hid a real hole: the lifted-wheel check drove,
 * lifted, then drove again -- and the second drive CLEARED the marks the first
 * had laid, so there was no break in the ring to bridge and a mutation that
 * joined strips across breaks survived the whole battery.
 */
static float drive_x, drive_z, drive_yaw;

static int drive_trace2(trace_t *tr, rb_car *c, int steps, float step_m,
                        float curve_deg, int keep)
{
    float x = keep ? drive_x : 0.f;
    float z = keep ? drive_z : 0.f;
    float yaw = keep ? drive_yaw : 0.f;
    int i, k, used = 0;

    if (!keep)
        trace_clear(tr);
    for (i = 0; i < steps; i++) {
        yaw += curve_deg;
        body_matrix(c->m, yaw, x, 0.f, z);
        for (k = 0; k < 4; k++) {
            float lx = (k & 1) ? -0.141f : 0.141f;
            float lz = (k < 2) ? 0.149f : -0.149f;
            c->hit[k].point[0] = x + c->m[0] * lx + c->m[8] * lz;
            c->hit[k].point[1] = 0.f;
            c->hit[k].point[2] = z + c->m[2] * lx + c->m[10] * lz;
        }
        trace_step(tr, c, NULL, 1.f / 60.f);
        x += c->m[8] * step_m;
        z += c->m[10] * step_m;
    }
    drive_x = x;
    drive_z = z;
    drive_yaw = yaw;
    for (k = 0; k < TRACE_RING; k++)
        used += tr->w[2].pt[k].used ? 1 : 0;
    return used;
}

static int drive_trace(trace_t *tr, rb_car *c, int steps, float step_m,
                       float curve_deg)
{
    return drive_trace2(tr, c, steps, step_m, curve_deg, 0);
}

/* Half a wheel batch's width, measured INDEPENDENTLY of mesh_half_width: half
   its plain model-space X extent, with no node origin and no dot product at all.
   Every wheel node on all three cars has a rest Z row of (+-k, 0, 0) -- the axle
   really is model +-X -- so this has to agree, and it agrees by a different
   route, which is the point of having it here.

   It also has to be an EXTENT and not a distance from the node: the Buggy's
   front wheel mesh is 6.7 mm off its own node, and a version of this written
   as max|x - node_x| would have agreed with the same mistake in trace.c. */
static float batch_half_x(const scene_t *s, int part)
{
    float lo = 1e30f, hi = -1e30f;
    unsigned int b, v;

    if (part < 0 || part >= s->rig.n)
        return 0.f;
    for (b = 0; b < s->n_batches; b++) {
        if ((int)s->batches[b].part != part || !s->batches[b].verts)
            continue;
        for (v = 0; v < s->batches[b].nverts; v++) {
            float x = s->batches[b].verts[v].x;
            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
    }
    return hi > lo ? (hi - lo) * 0.5f : 0.f;
}

/* The widest quad in the last capture. Vertices 0 and 1 of a quad's first
   triangle are the two edges of the older sample, so their distance is the
   mark's width -- the same reading the block below takes inline. */
static float mark_width_max(void)
{
    float w, best = 0.f;
    int d, i;

    for (d = 0; d < glcap.n_draws; d++)
        for (i = glcap.draws[d].first;
             i + 5 < glcap.draws[d].first + glcap.draws[d].count; i += 6) {
            float dx = glcap.pos[i + 1][0] - glcap.pos[i][0];
            float dy = glcap.pos[i + 1][1] - glcap.pos[i][1];
            float dz = glcap.pos[i + 1][2] - glcap.pos[i][2];
            w = sqrtf(dx * dx + dy * dy + dz * dz);
            if (w > best) best = w;
        }
    return best;
}

static void part7_trace(void)
{
    static const char *tex[] = {"t_halfdry_tire2_1", "t_halfdry_tire2_2",
                                "t_halfdry_tire2_3", "t_halfdry_tire2_4"};
    scene_t *s = make_scene(tex, 4);
    static const float eye[3] = {0.f, 0.f, 0.f};
    trace_t tr;
    rb_car c;
    int straight, turning, d, i;
    float wmin = 1e9f, wmax = 0.f, ymin = 1e9f, ymax = -1e9f;

    printf("\n-- part 7: the tyre marks --\n");

    trace_init(&tr, s);
    ck(tr.enabled && tr.n_tex == 4,
       "loads the game's four t_halfdry_tire2_<n> marks",
       "enabled=%d n_tex=%d", tr.enabled, tr.n_tex);
    ck(tr.w[0].cap == TRACE_RING_FRONT && tr.w[2].cap == TRACE_RING_REAR,
       "32 slots at the front, 48 at the back", "%d and %d",
       tr.w[0].cap, tr.w[2].cap);

    /* ---- the collinear merge ------------------------------------------
     *
     * THE mechanism, and the numbers below are what it actually costs rather
     * than what it looked like it should cost.
     *
     * FUN_0052f700 rewrites the head instead of taking a slot when the new point
     * is within maxHeight of the line through the last two, so a straight trail
     * slides its head along and spends a slot only when maxLen is exceeded.
     * 240 samples 5 cm apart is 12 m of trail; at maxLen = 1 m that is about a
     * dozen slots, not 240 -- and 240 is what a ring with no merge would try to
     * take, filling and recycling all 48 in four seconds.
     *
     * A GENTLE turn merges just as happily: at 2 degrees a sample the arc is a
     * 1.9 m radius and the sagitta over a metre is 66 mm, still inside maxHeight
     * = 90 mm. It takes a sharp turn to break collinearity before maxLen does,
     * which is why this drives one.
     */
    fake_car(&c, 3.f, 0.f);
    straight = drive_trace(&tr, &c, 240, 0.05f, 0.f);
    turning = drive_trace(&tr, &c, 240, 0.05f, 12.f);
    ck(straight > 0 && straight <= 20 && turning > straight,
       "a straight trail merges, a sharp turn spends slots",
       "240 samples -> %d slots straight, %d turning", straight, turning);

    /* ---- the break test's own codes ---------------------------------- */
    {
        float a[3] = {0.f, 0.f, 0.f};
        float b[3] = {0.f, 0.f, 0.2f};
        float on[3] = {0.f, 0.f, 0.4f};
        float off[3] = {0.5f, 0.f, 0.4f};
        float far_[3] = {0.f, 0.f, 3.0f};
        float back[3] = {0.f, 0.f, 0.1f};
        ck(trace_break_test(a, b, on) == 0
           && trace_break_test(a, b, off) == 1
           && trace_break_test(a, b, far_) == 4
           && trace_break_test(a, b, back) == 3,
           "the break test agrees with FUN_0052fb60 on all four outcomes",
           "collinear %d, off-line %d, too far %d, behind %d",
           trace_break_test(a, b, on), trace_break_test(a, b, off),
           trace_break_test(a, b, far_), trace_break_test(a, b, back));
    }

    /* ---- the geometry it submits ------------------------------------- */
    drive_trace(&tr, &c, 240, 0.05f, 2.f);
    gl_cap_reset();
    trace_draw(&tr, eye);
    ck(glcap.n_draws > 0 && tr.n_quads > 0, "marks are drawn",
       "%d draws, %d quads", glcap.n_draws, tr.n_quads);

    for (d = 0; d < glcap.n_draws; d++) {
        for (i = glcap.draws[d].first;
             i + 5 < glcap.draws[d].first + glcap.draws[d].count; i += 6) {
            /* vertex 0 and 1 of the first triangle are the two edges of the
               OLDER sample: ax then ay, so their distance is the mark's width */
            float dx = glcap.pos[i + 1][0] - glcap.pos[i][0];
            float dy = glcap.pos[i + 1][1] - glcap.pos[i][1];
            float dz = glcap.pos[i + 1][2] - glcap.pos[i][2];
            float w = sqrtf(dx * dx + dy * dy + dz * dz);
            int k;
            if (w < wmin) wmin = w;
            if (w > wmax) wmax = w;
            for (k = 0; k < 6; k++) {
                float y = glcap.pos[i + k][1];
                if (y < ymin) ymin = y;
                if (y > ymax) ymax = y;
            }
        }
    }
    /* As wide as the tyre that made it: between a fifth and one whole wheel
       DIAMETER. A mark the width of the car, or a hairline, both fail. */
    ck(wmin > 0.0718f * 2.f * 0.2f && wmax < 0.0718f * 2.f * 1.2f,
       "the mark is about as wide as the tyre",
       "%.1f to %.1f mm against a %.0f mm wheel",
       wmin * 1000.f, wmax * 1000.f, 0.0718f * 2000.f);
    /* Off the surface, but not floating. A decal drawn exactly on the plane
       z-fights per pixel, which no vertex capture can see -- so this checks the
       lift, and the depth bias is checked separately below. */
    ck(ymin > 0.001f && ymax < 0.0718f,
       "and lifted clear of the ground it lies on, but under a wheel radius",
       "y from %.1f to %.1f mm", ymin * 1000.f, ymax * 1000.f);
    ck(glcap.draws[0].pol_factor < 0.f && glcap.draws[0].pol_units < 0.f,
       "and drawn with a depth bias toward the camera",
       "polygonOffset %.0f, %.0f", glcap.draws[0].pol_factor,
       glcap.draws[0].pol_units);

    /* ---- and as wide as the tyre the TUNING fitted --------------------
     *
     * The port's own, not the original's -- carani.h carries the argument and is
     * the single place a tyre upgrade becomes a width, precisely so the drawn
     * tyre and the mark under it cannot drift apart.
     *
     * Which is why this does NOT check the mark against carani_tire_width. That
     * is the function's own definition, and a check against the thing it guards
     * has passed everything four times in this port already. It checks against
     * the game's DATA instead: RB_CARS[0].tune.tire_upgrade is the Overkill's
     * upgrades.ini [TIRES] row, and each level's mark must be wider than stock
     * by that level's own grip ratio. Change the mapping and every one of the
     * three ratios moves off its table entry.
     *
     * `fake_car` memsets the car, so the loop above ran with no tuning at all
     * and carani_tire_width's "nothing loaded" guard returned 1 -- that is the
     * stock width every check above was written against, and level 0 with a real
     * table has to reproduce it exactly.
     */
    {
        float wid[4];
        int lv, ok_ratio = 1, ok_mono = 1;

        for (lv = 0; lv < 4; lv++) {
            fake_car(&c, 3.f, 0.f);
            memcpy(&c.tune, &RB_CARS[0].tune, sizeof c.tune);
            c.tire_upgrade = lv;
            drive_trace(&tr, &c, 240, 0.05f, 2.f);
            gl_cap_reset();
            trace_draw(&tr, eye);
            wid[lv] = mark_width_max();
        }
        printf("mark width by tuning level: %.1f %.1f %.1f %.1f mm\n",
               wid[0] * 1000.f, wid[1] * 1000.f, wid[2] * 1000.f,
               wid[3] * 1000.f);
        for (lv = 1; lv < 4; lv++) {
            float want = RB_CARS[0].tune.tire_upgrade[lv]
                         / RB_CARS[0].tune.tire_upgrade[0];
            if (fabsf(wid[lv] / wid[0] - want) > 1e-3f)
                ok_ratio = 0;
            if (wid[lv] <= wid[lv - 1])
                ok_mono = 0;
        }
        ck(fabsf(wid[0] - wmax) < 1e-5f,
           "a stock tyre marks exactly as wide as it always did",
           "%.2f mm against %.2f mm", wid[0] * 1000.f, wmax * 1000.f);
        ck(ok_mono, "every tuning level leaves a wider mark than the last",
           "%.2f %.2f %.2f %.2f mm", wid[0] * 1000.f, wid[1] * 1000.f,
           wid[2] * 1000.f, wid[3] * 1000.f);
        ck(ok_ratio,
           "each mark is wider by that level's own grip ratio from "
           "upgrades.ini [TIRES]",
           "x%.4f x%.4f x%.4f against x%.4f x%.4f x%.4f",
           wid[1] / wid[0], wid[2] / wid[0], wid[3] / wid[0],
           RB_CARS[0].tune.tire_upgrade[1] / RB_CARS[0].tune.tire_upgrade[0],
           RB_CARS[0].tune.tire_upgrade[2] / RB_CARS[0].tune.tire_upgrade[0],
           RB_CARS[0].tune.tire_upgrade[3] / RB_CARS[0].tune.tire_upgrade[0]);
        /* Still a tyre mark and not a paint roller: the widest one is under a
           whole wheel diameter, the same bound the stock check uses. */
        ck(wid[3] < 0.0718f * 2.f,
           "even the widest mark is narrower than the wheel is tall",
           "%.1f mm against a %.0f mm wheel", wid[3] * 1000.f, 0.0718f * 2000.f);

        /* leave the fixture as the rest of part 7 expects it */
        fake_car(&c, 3.f, 0.f);
    }

    /* ---- and as wide as the tyre, on the REAL packed cars -------------
     *
     * The width used to be TRACE_WIDTH_FRAC of the wheel's PHYSICS radius,
     * which is not the tyre's width and is not in any fixed proportion to it.
     * Measured against the drawn wheels that came out at 92% of the Overkill's
     * tyre, 76% of the Buggy's rear and 107% of its front -- a mark WIDER than
     * the tyre that made it -- and 74% of the Hummer's, which is what "the marks
     * are about three quarters of the tyre" was. No single fraction fixes three
     * cars that disagree in both directions, so trace_init measures each wheel
     * off its own mesh.
     *
     * The fixtures above cannot see any of this: they carry no rig, so they take
     * the fallback. This needs the real cars, and a missing one is a FAILED
     * check rather than a quiet skip -- rb_test's rig section spent months
     * printing "SKIPPED" into a wall of passing output.
     */
    {
        static const char *files[3] = {
            "assets/car1.vsc", "assets/car2.vsc", "assets/car3.vsc"
        };
        static const char *names[3] = { "Overkill", "Buggy", "Hummer" };
        int ci;

        for (ci = 0; ci < 3; ci++) {
            scene_t cs;
            rb_car rc;
            rb_world cw;
            trace_t t2;
            int wi, ok_fit = 1, ok_wide = 1, n = 0;
            float worst = 0.f;

            if (!scene_load(files[ci], &cs)) {
                ck(0, "the packed car loads (run from rccars_vita/)",
                   "%s", files[ci]);
                continue;
            }
            memset(&cw, 0, sizeof cw);
            rbcar_init(&rc, ci, &cw, 0.f, 0.f, 0.f, 0.f);
            carani_bind(&cs.rig, &rc);
            trace_init(&t2, &cs);

            printf("%-9s mark half-width per wheel:", names[ci]);
            for (wi = 0; wi < rc.nwheels && wi < RB_MAX_WHEELS; wi++) {
                float want = batch_half_x(&cs, cs.rig.wheel[wi]);
                float old = rc.wheel[wi].radius * 0.5f;
                if (want <= 0.f)
                    continue;
                n++;
                printf(" %.4f", t2.half_w[wi]);
                /* 1e-5, not 1e-6: the two routes normalise differently and the
                   node scales are 1.05, 0.90 and -1.17, so they land ~2e-6
                   apart on values of 0.03 to 0.04. Anything a wrong measure
                   could do here is three orders of magnitude bigger. */
                if (fabsf(t2.half_w[wi] - want) > 1e-5f)
                    ok_fit = 0;
                if (fabsf(old / want - 1.f) > worst)
                    worst = fabsf(old / want - 1.f);
                /* a mark is a tyre's width, not a car's */
                if (t2.half_w[wi] > rc.wheel[wi].radius * 1.5f)
                    ok_wide = 0;
            }
            printf("  (the old radius rule was off by up to %.0f%%)\n",
                   worst * 100.f);
            ck(n == rc.nwheels, "every wheel found the tyre that makes its mark",
               "%d of %d", n, rc.nwheels);
            ck(ok_fit, "and is fitted to that tyre's own width, off the mesh",
               "%s", names[ci]);
            ck(ok_wide, "and is a tyre's width, not a body's", "%s", names[ci]);

            /* End to end: the fitted width has to reach the drawn vertex, and
               the fixtures above cannot show that because they have no rig and
               take the fallback.

               Bound to a consequence rather than to half_w: the BUGGY's front
               tyre is genuinely narrower than its rear (0.0230 against 0.0306 in
               the mesh), so it must leave TWO mark widths in that ratio, while
               the other two cars leave one. A version that fits per car instead
               of per wheel passes every check above and dies here. */
            {
                float lo = 1e9f, hi = 0.f, want_lo = 1e9f, want_hi = 0.f;
                int d, i2;
                /* A car fresh out of rbcar_init is in the air, and trace_step
                   correctly ignores a wheel that is not touching. Put the first
                   four down -- those are the ones drive_trace2 moves, so the
                   Hummer's middle pair stays up and out of this. */
                for (wi = 0; wi < RB_MAX_WHEELS; wi++) {
                    rc.hit[wi].active = (wi < 4);
                    rc.hit[wi].in_water = 0;
                    rc.hit[wi].normal[0] = 0.f;
                    rc.hit[wi].normal[1] = 1.f;
                    rc.hit[wi].normal[2] = 0.f;
                }
                drive_trace(&t2, &rc, 240, 0.05f, 2.f);
                gl_cap_reset();
                trace_draw(&t2, eye);
                for (d = 0; d < glcap.n_draws; d++)
                    for (i2 = glcap.draws[d].first;
                         i2 + 5 < glcap.draws[d].first + glcap.draws[d].count;
                         i2 += 6) {
                        float dx = glcap.pos[i2 + 1][0] - glcap.pos[i2][0];
                        float dy = glcap.pos[i2 + 1][1] - glcap.pos[i2][1];
                        float dz = glcap.pos[i2 + 1][2] - glcap.pos[i2][2];
                        float ww = sqrtf(dx * dx + dy * dy + dz * dz);
                        if (ww < lo) lo = ww;
                        if (ww > hi) hi = ww;
                    }
                for (wi = 0; wi < 4 && wi < rc.nwheels; wi++) {
                    if (t2.half_w[wi] <= 0.f) continue;
                    if (t2.half_w[wi] * 2.f < want_lo) want_lo = t2.half_w[wi] * 2.f;
                    if (t2.half_w[wi] * 2.f > want_hi) want_hi = t2.half_w[wi] * 2.f;
                }
                printf("%-9s draws marks %.1f to %.1f mm; its tyres are "
                       "%.1f to %.1f mm\n", names[ci], lo * 1000.f, hi * 1000.f,
                       want_lo * 1000.f, want_hi * 1000.f);
                ck(fabsf(lo - want_lo) < 1e-5f && fabsf(hi - want_hi) < 1e-5f,
                   "and every drawn quad is the width of the tyre above it",
                   "%s: %.4f-%.4f against %.4f-%.4f", names[ci], lo, hi,
                   want_lo, want_hi);
                ck((ci == 1) ? hi > lo * 1.2f : fabsf(hi - lo) < 1e-5f,
                   ci == 1 ? "the Buggy's narrower front tyre leaves a narrower "
                             "mark than its rear"
                           : "a car with one tyre width leaves one mark width",
                   "%.4f and %.4f", lo, hi);
            }

            /* An UNBOUND rig points every wheel at part 0 -- __root__, the whole
               car -- and measuring that would give the mark the half-width of
               the model. main.c reaches exactly that state: load_car builds the
               trace before respawn() binds the rig. */
            if (ci == 0) {
                trace_t t3;
                int all_zero = 1;
                memset(cs.rig.wheel, 0, sizeof cs.rig.wheel);
                trace_init(&t3, &cs);
                for (wi = 0; wi < RB_MAX_WHEELS; wi++)
                    if (t3.half_w[wi] != 0.f)
                        all_zero = 0;
                ck(all_zero,
                   "an unbound rig measures nothing rather than the car body",
                   "half_w[0] = %.4f", t3.half_w[0]);
            }
            scene_release(&cs);
        }
    }

    /* ---- the tread runs ALONG the trail, not across it ---------------
     *
     * Two statements in one, and the second is the one that needs a check.
     * 240 samples 5 cm apart is 12 m of trail; at scaleCoeff repeats per metre
     * that is a range of tens, and the wrap keeps it under 1000. But which
     * coordinate carries it is not free: t_halfdry_tire2_<n> is 64 x 256 with
     * its tread down the long axis, so the repeat has to be V and the 0..1
     * across-the-mark coordinate has to be U. Swapped, the tread is drawn
     * rotated a quarter turn -- which is a thing no arithmetic check on a
     * "range" would notice, and which the recorded UVs show plainly.
     */
    {
        float vmin = 1e9f, vmax = -1e9f, umin = 1e9f, umax = -1e9f;
        for (d = 0; d < glcap.n_draws; d++)
            for (i = glcap.draws[d].first;
                 i < glcap.draws[d].first + glcap.draws[d].count; i++) {
                if (glcap.uv[i][0] < umin) umin = glcap.uv[i][0];
                if (glcap.uv[i][0] > umax) umax = glcap.uv[i][0];
                if (glcap.uv[i][1] < vmin) vmin = glcap.uv[i][1];
                if (glcap.uv[i][1] > vmax) vmax = glcap.uv[i][1];
            }
        ck(vmax - vmin > 1.f && vmax < 1000.f
           && umin == 0.f && umax == 1.f,
           "the tread repeats ALONG the trail (V) and spans it once across (U)",
           "V spans %.1f to %.1f, U %.1f to %.1f", vmin, vmax, umin, umax);
    }

    /* ---- a break really breaks ------------------------------------- */
    {
        float longest = 0.f;
        /* Lift a wheel mid-trail, then drive on from where it landed: the strip
           must not bridge the gap. `keep` on the second leg, or it wipes the
           marks the first leg laid and there is no gap left to bridge. */
        int k;
        fake_car(&c, 3.f, 0.f);
        drive_trace(&tr, &c, 60, 0.05f, 0.f);
        /* All four wheels: a jump lifts the car. Lifting one and leaving the
           others in contact is not a jump, and the three that stayed down bridge
           the gap on their own -- which is how the first version of this check
           managed to fail for a reason it was not testing. */
        for (i = 0; i < 30; i++) {
            for (k = 0; k < 4; k++)
                c.hit[k].active = 0;
            trace_step(&tr, &c, NULL, 1.f / 60.f);
        }
        for (k = 0; k < 4; k++)
            c.hit[k].active = 1;
        drive_z += 9.f;                     /* it flew 9 m while off the ground */
        drive_trace2(&tr, &c, 20, 0.05f, 0.f, 1);
        gl_cap_reset();
        trace_draw(&tr, eye);
        for (d = 0; d < glcap.n_draws; d++)
            for (i = glcap.draws[d].first;
                 i + 5 < glcap.draws[d].first + glcap.draws[d].count; i += 6) {
                float dx = glcap.pos[i + 5][0] - glcap.pos[i][0];
                float dz = glcap.pos[i + 5][2] - glcap.pos[i][2];
                float L = sqrtf(dx * dx + dz * dz);
                if (L > longest) longest = L;
            }
        ck(longest < TRACE_MAX_LEN * 2.f,
           "no quad bridges a lifted wheel",
           "longest quad %.2f m against maxLen %.2f", longest, TRACE_MAX_LEN);
    }

    /* ---- a mark MODULATES the ground; it is not a sprite over it ------
     *
     * The bug this replaced: the marks were drawn white, opaque and
     * alpha-blended, which paints a solid grey stripe over the sand. The engine
     * draws them with SRCBLEND = D3DBLEND_DESTCOLOR / DESTBLEND =
     * D3DBLEND_SRCCOLOR (0x0045c911, reached from the material flags 0x80000021
     * through FUN_0045c3c0) over a stage op of D3DTOP_MODULATEALPHA_ADDCOLOR
     * (0x0045cda6). Both are recovered facts about the original, so both are
     * asserted as themselves.
     *
     * None of it was visible to this harness until now, because glBlendFunc and
     * glTexEnvi were no-op stubs -- see testgl/vitaGL.h.
     */
    {
        const glcap_draw *d0;
        drive_trace(&tr, &c, 240, 0.05f, 2.f);
        gl_cap_reset();
        trace_draw(&tr, eye);
        d0 = &glcap.draws[0];
        ck(d0->blend_src == GL_DST_COLOR && d0->blend_dst == GL_SRC_COLOR,
           "the mark blends 2 * src * dst, as FUN_0045c6e0 mode 5 does",
           "src 0x%x dst 0x%x", d0->blend_src, d0->blend_dst);
        ck(d0->env_mode == GL_COMBINE
           && near(glcap_env_color[0], 128.f / 255.f, 0.005f),
           "and interpolates toward the texture's own neutral level",
           "env 0x%x, constant %.3f against 128/255 = %.3f", d0->env_mode,
           glcap_env_color[0], 128.f / 255.f);

        /* The steered pair mark at HALF strength (FUN_0052f310, 0x0052f537).
           Every sample here is fresh, so the drawn strengths are exactly the two
           the surface gave them -- and there must be two, in a 2:1 ratio. A
           version that ignores the per-sample strength draws one. */
        {
            int lo = 255, hi = 0, distinct = 0, seen[256];
            memset(seen, 0, sizeof(seen));
            for (i = d0->first; i < d0->first + d0->count; i++) {
                int v = glcap.rgba[i][0];
                if (v < lo) lo = v;
                if (v > hi) hi = v;
                if (!seen[v]) { seen[v] = 1; distinct++; }
            }
            ck(distinct == 2 && hi > 250 && abs(lo * 2 - hi) <= 2,
               "the front wheels mark at half the strength of the rear",
               "%d distinct strengths, %d and %d", distinct, lo, hi);
        }
    }

    /* ---- and what that does to the ground, which is the whole point ---
     *
     * Bound to the RESULT rather than to any constant in trace.c, because a
     * check that compares against the #define it is guarding passes whatever
     * the #define says -- this project has been caught by that six times.
     * `ground()` evaluates the recorded GL state on a chosen texel with the
     * ground at full white, so a wrong blend, a wrong env or a wrong constant
     * all move the number.
     *
     * The property that makes the mark texture make sense: its flat border is
     * exactly 128, and 128 must leave the ground ALONE at every age. If it does
     * not, the mark is a stripe.
     */
    {
        const float neutral = 128.f / 255.f;
        const float tread = 96.f / 255.f;      /* the darkest texel it carries */
        float fresh, old_, k;
        int j;

        /* the strongest sample drawn, i.e. a rear wheel's */
        fresh = 0.f;
        for (i = glcap.draws[0].first;
             i < glcap.draws[0].first + glcap.draws[0].count; i++)
            if (glcap.rgba[i][0] / 255.f > fresh)
                fresh = glcap.rgba[i][0] / 255.f;

        ck(near(trace_ground(&glcap.draws[0], fresh, neutral), 1.f, 0.02f),
           "a neutral texel leaves the ground untouched",
           "x%.3f at full strength", trace_ground(&glcap.draws[0], fresh,
                                                  neutral));
        k = trace_ground(&glcap.draws[0], fresh, tread);
        ck(k > 0.55f && k < 0.9f, "and the tread darkens it, but does not black it out",
           "x%.3f", k);

        /* The fade is a PLATEAU then a ramp: FUN_0052fd00 holds full strength
           until timeLife/4 is left. Half-worn is still full strength, and only
           the last quarter goes. */
        for (j = 0; j < RB_MAX_WHEELS; j++)
            for (i = 0; i < TRACE_RING; i++)
                if (tr.w[j].pt[i].used)
                    tr.w[j].pt[i].life = TRACE_LIFE * 0.5f;
        gl_cap_reset();
        trace_draw(&tr, eye);
        old_ = 0.f;
        for (i = glcap.draws[0].first;
             i < glcap.draws[0].first + glcap.draws[0].count; i++)
            if (glcap.rgba[i][0] / 255.f > old_)
                old_ = glcap.rgba[i][0] / 255.f;
        ck(near(old_, fresh, 0.01f),
           "a half-worn mark has not faded at all",
           "%.2f against a fresh %.2f", old_, fresh);

        for (j = 0; j < RB_MAX_WHEELS; j++)
            for (i = 0; i < TRACE_RING; i++)
                if (tr.w[j].pt[i].used)
                    tr.w[j].pt[i].life = TRACE_LIFE * 0.02f;
        gl_cap_reset();
        trace_draw(&tr, eye);
        old_ = 0.f;
        for (i = glcap.draws[0].first;
             i < glcap.draws[0].first + glcap.draws[0].count; i++)
            if (glcap.rgba[i][0] / 255.f > old_)
                old_ = glcap.rgba[i][0] / 255.f;
        k = trace_ground(&glcap.draws[0], old_, tread);
        ck(k > 0.95f && k < 1.05f,
           "and one at the end of its life leaves NO mark, not a black one",
           "x%.3f on the darkest texel", k);
    }

    /* ---- the 20 m cull -------------------------------------------- */
    {
        float far_eye[3] = {0.f, 0.f, 400.f};
        drive_trace(&tr, &c, 240, 0.05f, 2.f);
        gl_cap_reset();
        trace_draw(&tr, far_eye);
        ck(tr.n_quads == 0, "marks beyond 20 m are not drawn",
           "%d quads from 400 m away", tr.n_quads);
    }

    set_world_state();
    drive_trace(&tr, &c, 60, 0.05f, 2.f);
    trace_draw(&tr, eye);
    ck_state_restored("trace_draw");
}

/* ---------------------------------------------------------------- part 8 -- */

/* One env-mapped batch: a single quad whose four normals all point the same way,
   so the sphere-map UV it gets can be predicted exactly. */
static batch_t *add_env_quad(scene_t *s, unsigned int env, const float n[3])
{
    batch_t *b = add_grid2(s, 0, -0.1f, -0.1f, 0.2f, 0.2f, 1, 0.f);
    unsigned int k;

    b->env = env;
    b->nrm = malloc(sizeof(float) * 3 * b->nverts);
    for (k = 0; k < b->nverts; k++) {
        b->nrm[k * 3 + 0] = n[0];
        b->nrm[k * 3 + 1] = n[1];
        b->nrm[k * 3 + 2] = n[2];
    }
    return b;
}

static void part8_envmap(void)
{
    static const char *tex[] = {"sky_up"};
    scene_t *sky = make_scene(tex, 1);
    scene_t *car = make_scene(tex, 1);
    envmap_t e;
    float ident[9] = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
    const float toward[3] = {0.f, 0.f, 1.f};    /* at a camera looking down -Z */
    const float rightn[3] = {1.f, 0.f, 0.f};
    const float upn[3] = {0.f, 1.f, 0.f};
    batch_t *body;
    int d;

    printf("\n-- part 8: the body's env-map glance --\n");

    /* the sky the glance samples is taken off the BATCH the packer flagged */
    add_grid2(sky, BATCH_SKY, -1.f, -1.f, 2.f, 2.f, 1, 0.f);
    envmap_init(&e, sky);
    ck(e.enabled && e.tex == sky->tex_ids[0],
       "takes its source from the track's sky batch", "enabled=%d tex=%u",
       e.enabled, e.tex);

    /* ---- the four alphas ----------------------------------------------
     *
     * Bound to their ORDER, not to their values -- comparing each against the
     * #define it came from would pass whatever the #define said, which is the
     * self-referential trap this port has fallen into four times. The order is a
     * statement about the car: glass reflects completely, the bolt-on exhaust
     * nearly so, the painted shell least.
     */
    ck(envmap_alpha(ENV_NONE) == 0.f
       && envmap_alpha(ENV_BODY) > 0.f
       && envmap_alpha(ENV_BODY) < envmap_alpha(ENV_GRE2)
       && envmap_alpha(ENV_GRE2) < envmap_alpha(ENV_UPGRADES)
       && envmap_alpha(ENV_UPGRADES) < envmap_alpha(ENV_GRE1)
       && envmap_alpha(ENV_GRE1) <= 1.f,
       "paint reflects least, glass most, and nothing reflects nothing",
       "body %.2f, GRE2 %.2f, upgrades %.2f, GRE1 %.2f",
       envmap_alpha(ENV_BODY), envmap_alpha(ENV_GRE2),
       envmap_alpha(ENV_UPGRADES), envmap_alpha(ENV_GRE1));

    /* ---- the sphere map, measured through the actual transform -------- */
    body = add_env_quad(car, ENV_BODY, toward);
    gl_cap_reset();
    envmap_draw(&e, car, ident);
    ck(glcap.n_draws == 1 && near(glcap.draws[0].color[3],
                                  envmap_alpha(ENV_BODY), 1e-4f),
       "an env batch draws once, at its class's alpha",
       "%d draws, alpha %.3f", glcap.n_draws, glcap.draws[0].color[3]);
    ck(near(glcap.uv[glcap.draws[0].first][0], 0.5f, 1e-4f)
       && near(glcap.uv[glcap.draws[0].first][1], 0.5f, 1e-4f),
       "a surface facing the camera samples the middle of the sphere map",
       "uv %.3f, %.3f", glcap.uv[glcap.draws[0].first][0],
       glcap.uv[glcap.draws[0].first][1]);

    /* Right in view space must land on the RIGHT of the map, and up must land
       UP -- which in a texture means a SMALLER v. Getting either backwards
       slides the highlight the wrong way as the car turns or pitches, and is
       exactly the class of bug the mirrored-yaw note in CLAUDE.md is about. */
    memcpy(body->nrm, rightn, sizeof(float) * 3);
    gl_cap_reset();
    envmap_draw(&e, car, ident);
    ck(glcap.uv[glcap.draws[0].first][0] > 0.9f,
       "a surface facing view right samples the right of the map",
       "u %.3f", glcap.uv[glcap.draws[0].first][0]);
    memcpy(body->nrm, upn, sizeof(float) * 3);
    gl_cap_reset();
    envmap_draw(&e, car, ident);
    ck(glcap.uv[glcap.draws[0].first][1] < 0.1f,
       "a surface facing view up samples the top of the map",
       "v %.3f", glcap.uv[glcap.draws[0].first][1]);

    /* ---- and it slides when the car turns ----------------------------- */
    {
        float m[16], n3[9], u0, u1;
        int r, ci;
        memcpy(body->nrm, toward, sizeof(float) * 3);
        body_matrix(m, 0.f, 0.f, 0.f, 0.f);
        for (r = 0; r < 3; r++)
            for (ci = 0; ci < 3; ci++)
                n3[r * 3 + ci] = m[r * 4 + ci];
        gl_cap_reset();
        envmap_draw(&e, car, n3);
        u0 = glcap.uv[glcap.draws[0].first][0];
        body_matrix(m, 90.f, 0.f, 0.f, 0.f);
        for (r = 0; r < 3; r++)
            for (ci = 0; ci < 3; ci++)
                n3[r * 3 + ci] = m[r * 4 + ci];
        gl_cap_reset();
        envmap_draw(&e, car, n3);
        u1 = glcap.uv[glcap.draws[0].first][0];
        ck(near(u0, 0.5f, 1e-4f) && fabsf(u1 - u0) > 0.4f,
           "the highlight slides across the body as the car turns",
           "u %.3f at yaw 0, %.3f at yaw 90", u0, u1);
    }

    /* ---- a batch with no class, and one with no normals, are skipped --
     *
     * The unclassified batch is given NORMALS on purpose. Without them it was
     * being skipped for the wrong reason and the check was not testing the
     * sentence it claims -- pack_vsc.py only writes normals for a classified
     * batch, so env == 0 and nrm == NULL always travel together in real data and
     * the fixture has to break that pairing deliberately.
     */
    {
        const float any[3] = {0.f, 1.f, 0.f};
        batch_t *plain = add_env_quad(car, ENV_NONE, any);
        (void)plain;
    }
    {
        batch_t *b = add_env_quad(car, ENV_GRE1, toward);
        free(b->nrm);
        b->nrm = NULL;                                       /* packed pre-VSC7 */
    }
    gl_cap_reset();
    envmap_draw(&e, car, ident);
    ck(glcap.n_draws == 1 && e.n_batches == 1,
       "only a classified batch WITH normals gets a glance",
       "%d draws of 3 batches", glcap.n_draws);

    /* ---- the rigged case: springs are GRE1, and they are rigged ------- */
    {
        batch_t *b = add_env_quad(car, ENV_GRE2, toward);
        b->part = 1;
        car->has_rig = 1;
        car->rig.n = 2;
        memset(car->rig.draw[1], 0, sizeof(car->rig.draw[1]));
        car->rig.draw[1][0] = car->rig.draw[1][5] = car->rig.draw[1][10] =
            car->rig.draw[1][15] = 1.f;
        gl_cap_reset();
        envmap_draw(&e, car, ident);
        ck(glcap.n_draws == 2 && mat_max_depth >= 1 && mat_depth == 0
           && !mat_underflow,
           "a rigged env part draws under its own matrix, balanced",
           "%d draws, stack peak %d, left at %d", glcap.n_draws,
           mat_max_depth, mat_depth);
    }

    set_world_state();
    envmap_draw(&e, car, ident);
    /* envmap_draw does not touch the colour array or the depth bias, but it does
       turn blending on, the alpha test off and depth writes off -- and it leaves
       a glColor4f behind, which would tint everything drawn after it. */
    ck_state_restored("envmap_draw");
    ck(cur_color[0] == 1.f && cur_color[3] == 1.f,
       "and puts the vertex colour back to white",
       "colour %.2f,%.2f,%.2f,%.2f", cur_color[0], cur_color[1], cur_color[2],
       cur_color[3]);
}

/* ============================================================== part 9 ====
 *
 * Texture quality (the game's RenderQual / VIDEO_TexQual) and the alpha-test
 * split -- both decided inside scene_load, so both need a real file rather than
 * a hand-built scene_t. The test writes its own minimal VSC6 instead of leaning
 * on the packed 34 MB tracks, so it stays self-contained and still goes through
 * the actual loader.
 */

static void w_u16(FILE *f, unsigned v) { fputc(v & 0xff, f); fputc(v >> 8, f); }
static void w_u32(FILE *f, unsigned v)
{ w_u16(f, v & 0xffff); w_u16(f, v >> 16); }
static void w_name(FILE *f, const char *s)
{ w_u16(f, (unsigned)strlen(s)); fwrite(s, 1, strlen(s), f); }

/* An RGBA texture with a full mip chain. `alpha` is the alpha every texel gets,
   except that when `keyed` is set one texel of level 0 is made transparent. */
static void w_tex_rgba(FILE *f, const char *name, int size, int keyed,
                       int one_level)
{
    int lvl, mips = 0, s;
    for (s = size; s >= 1; s >>= 1)
        mips++;
    /* The lightmap atlases really do ship with a single level (512x512 RGB888,
       no chain), so "skip two levels" has to survive a texture that has none to
       give. Without a fixture like this the clamp in scene_load is unreachable
       and a mutation deleting it survives -- on real data it would upload
       nothing at all for every lit batch's atlas. */
    if (one_level)
        mips = 1;
    w_name(f, name);
    w_u16(f, (unsigned)size);
    w_u16(f, (unsigned)size);
    fputc(1, f);                       /* fmt 1 = RGBA8888 */
    fputc((unsigned)mips, f);
    for (lvl = 0; lvl < mips; lvl++) {
        int lw = size >> lvl, i;
        if (lw < 1) lw = 1;
        for (i = 0; i < lw * lw; i++) {
            fputc(200, f); fputc(150, f); fputc(100, f);
            /* One rejected texel is all it takes to need the test -- and it has
               to be BELOW the 0.5 threshold, not merely non-255. */
            fputc((keyed && lvl == 0 && i == 0) ? 3 : 255, f);
        }
    }
}

/* An RGB565 texture with a known texel whose three fields are all DISTINCT and
   all non-zero: r = 24, g = 32, b = 5.
 *
 * It used to be pure red (0xF800). That is a degenerate fixture: with the low
 * field zero, several wrong formulas produce the right answer by accident, and a
 * mutation shifting the blue field by 5 instead of 11 survived the whole suite.
 * Distinct fields make exactly one arrangement correct. */
#define FIXTURE_565_PACKED   ((24u << 11) | (32u << 5) | 5u)   /* 0xC405 */
#define FIXTURE_565_SWAPPED  (( 5u << 11) | (32u << 5) | 24u)  /* 0x2C18 */
static void w_tex_565(FILE *f, const char *name, int size)
{
    int lvl, mips = 0, s;
    for (s = size; s >= 1; s >>= 1)
        mips++;
    w_name(f, name);
    w_u16(f, (unsigned)size);
    w_u16(f, (unsigned)size);
    fputc(0, f);                       /* fmt 0 = RGB565 */
    fputc((unsigned)mips, f);
    for (lvl = 0; lvl < mips; lvl++) {
        int lw = size >> lvl, i;
        if (lw < 1) lw = 1;
        for (i = 0; i < lw * lw; i++)
            w_u16(f, FIXTURE_565_PACKED);
    }
}

/* A texture DECLARED with no pixels at all: zero mip levels. This is what a name
   that resolves to no file produces, and FORMAT_NOTES records five of them. It
   gets an id from glGenTextures and never a glTexImage2D, so its sampler is
   undefined -- indistinguishable, for drawing purposes, from an out-of-range
   index. */
static void w_tex_empty(FILE *f, const char *name, int size)
{
    w_name(f, name);
    w_u16(f, (unsigned)size);
    w_u16(f, (unsigned)size);
    fputc(1, f);                       /* fmt 1 */
    fputc(0, f);                       /* mips 0 -- no data follows */
}

static void w_batch(FILE *f, unsigned tex, unsigned flags)
{
    int i;
    w_u32(f, tex);
    w_u32(f, flags);
    w_u32(f, 0);                       /* part */
    w_u32(f, 0xFFFFFFFFu);             /* lm_tex: none */
    w_u32(f, 3);                       /* nverts */
    w_u32(f, 3);                       /* nidx */
    for (i = 0; i < 3; i++) {          /* 7 floats per vertex at VSC6 */
        float v[7] = { (float)i, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
        fwrite(v, sizeof(float), 7, f);
    }
    for (i = 0; i < 3; i++)
        w_u16(f, (unsigned)i);
}

static const char *write_fixture_vsc(void)
{
    static const char *path = "/tmp/rccars_vis_test.vsc";
    FILE *f = fopen(path, "wb");
    if (!f)
        return NULL;
    fwrite("VSC6", 1, 4, f);
    w_u32(f, 6);                       /* textures */
    w_u32(f, 7);                       /* batches */
    w_u32(f, 0);                       /* parts */
    w_u32(f, 0);                       /* markers */
    { float r = 0.f; fwrite(&r, sizeof r, 1, f); }
    w_tex_rgba(f, "opaque_a", 64, 0, 0);
    w_tex_rgba(f, "cutout",   64, 1, 0);
    w_tex_rgba(f, "opaque_b", 64, 0, 0);
    w_tex_rgba(f, "flat_lm",  64, 0, 1);   /* one level, like a lightmap atlas */
    w_tex_565 (f, "red565",   64);
    w_tex_empty(f, "no_file", 64);        /* declared, no pixels */
    w_batch(f, 0, 0);
    w_batch(f, 1, 0);
    w_batch(f, 2, 0);
    w_batch(f, 3, 0);
    w_batch(f, 4, 0);
    /* Batch 5 names a texture that does not exist. Every one of the ten real
       tracks has exactly one of these (beach_1: batch 24, tex 0xFFFFFFFF,
       974 tris), and they were invisible only because the global alpha test
       discarded an unbound sampler. See BATCH_NO_TEXTURE. */
    w_batch(f, 0xFFFFFFFFu, 0);
    w_batch(f, 5, 0);                     /* names the pixel-less one */
    fclose(f);
    return path;
}

static void part9_texquality(void)
{
    const char *path = write_fixture_vsc();
    scene_t s;
    int q;

    printf("\n-- part 9: texture quality and the alpha-test split --\n");

    /* A missing fixture is a FAILED check, never a quiet return: rb_test's rig
       checks silently skipped for weeks behind a version test that no longer
       matched, inside output that ended "all checks passed". */
    if (!path) {
        ck(0, "the fixture .vsc could be written", "fopen failed");
        return;
    }

    /* ---- classification ------------------------------------------------- */
    scene_set_tex_quality(0);
    n_uploads = 0;
    if (!scene_load(path, &s)) {
        ck(0, "the fixture .vsc loads", "scene_load failed");
        return;
    }
    ck(1, "the fixture .vsc loads", "%u textures, %u batches",
       s.n_tex, s.n_batches);
    ck(!(s.batches[0].flags & BATCH_ALPHA_KEYED)
       && (s.batches[1].flags & BATCH_ALPHA_KEYED)
       && !(s.batches[2].flags & BATCH_ALPHA_KEYED)
       && !(s.batches[3].flags & BATCH_ALPHA_KEYED)
       && !(s.batches[4].flags & BATCH_ALPHA_KEYED),
       "only the cut-out batch is flagged alpha-keyed",
       "flags %u / %u / %u / %u / %u", s.batches[0].flags, s.batches[1].flags,
       s.batches[2].flags, s.batches[3].flags, s.batches[4].flags);

    /* ---- the dispatch: keyed batches draw with the test, the rest without -- */
    glEnable(GL_ALPHA_TEST);
    gl_cap_reset();
    scene_draw(&s, 0, 0);
    {
        int i, n_off = 0, n_on = 0, wrong = 0;
        for (i = 0; i < glcap.n_draws; i++) {
            /* batch 1 is the cut-out; its three vertices start at x = 0,1,2 like
               every other batch, so identify it by its texture instead. */
            int keyed = (glcap.draws[i].tex == s.batches[1].gl_tex);
            if (glcap.draws[i].alpha_test) n_on++; else n_off++;
            if (keyed != glcap.draws[i].alpha_test) wrong++;
        }
        ck(glcap.n_draws == 5, "the five textured batches draw",
           "%d draws", glcap.n_draws);
        ck(wrong == 0 && n_on == 1 && n_off == 4,
           "the alpha test is on for the cut-out and off for the rest",
           "%d draws with it, %d without, %d mismatched", n_on, n_off, wrong);

        /* ---- THE CHECK THAT WAS MISSING ------------------------------------
         *
         * Confining the alpha test to the batches that need it turned every
         * track's one untextured batch from invisible into 870-4,291 triangles of
         * black, depth-writing geometry -- because their invisibility was an
         * accident of the global test discarding an unbound sampler, not a
         * decision. Three assertions, because each can be satisfied wrongly on
         * its own: that it is recognised, that it is NOT put in the cheap pass,
         * and that it does not draw. */
        {
            int j, drew_untextured = 0;
            for (j = 0; j < glcap.n_draws; j++)
                if (glcap.draws[j].tex == 0)
                    drew_untextured = 1;
            /* BOTH routes to "no texture": an out-of-range index (batch 5) and
               a declared entry that carries no pixels (batch 6). They are
               indistinguishable once drawn, so both must be caught -- checking
               only the index leaves the second one rendering black. */
            ck((s.batches[5].flags & BATCH_NO_TEXTURE) != 0,
               "an out-of-range texture index is recognised",
               "flags %u", s.batches[5].flags);
            ck((s.batches[6].flags & BATCH_NO_TEXTURE) != 0,
               "a declared texture with no pixels is recognised too",
               "flags %u", s.batches[6].flags);
            ck((s.batches[5].flags & BATCH_ALPHA_KEYED) != 0
               && (s.batches[6].flags & BATCH_ALPHA_KEYED) != 0,
               "and both keep the alpha test -- unprovable never means opaque",
               "flags %u and %u", s.batches[5].flags, s.batches[6].flags);
            ck(!drew_untextured,
               "and it is not drawn at all",
               "%d draws for 7 batches, an untextured draw seen = %d",
               glcap.n_draws, drew_untextured);
        }
    }
    ck(st_alpha == 1, "scene_draw leaves the alpha test ENABLED",
       "atest=%d -- main.c sets it at init and assumes it everywhere",
       st_alpha);
    scene_release(&s);

    /* ---- quality: each level halves the resolution GL is handed ---------- */
    for (q = 0; q < SCENE_TEX_QUALITY_LEVELS; q++) {
        int i, top = 0, levels = 0;
        scene_set_tex_quality(q);
        ck(scene_tex_quality() == q, "the quality level is what was set",
           "asked %d, got %d", q, scene_tex_quality());
        n_uploads = 0;
        glcap_mipgen_reset();
        if (!scene_load(path, &s)) {
            ck(0, "the fixture reloads at each quality", "level %d", q);
            return;
        }
        for (i = 0; i < n_uploads; i++)
            if (uploads[i].tex == s.batches[0].gl_tex) {
                if (uploads[i].level == 0)
                    top = uploads[i].w;
                levels++;
            }
        /* Bound to the SIZE, not to the number of levels skipped: 64 >> q is what
           the player is choosing between, and it is the same relationship the
           original's Textures.1/.2/.3 have. */
        ck(top == (64 >> q), "quality level halves the top mip that reaches GL",
           "level %d uploaded %dx%d as GL level 0, wanted %d",
           q, top, top, 64 >> q);
        /* ONE upload, then ONE generation. This used to assert 7-q uploads, back
           when the chain was handed over level by level; vitaGL throws away the
           pixels of every level but 0 and box-downscales the rest itself, and it
           redoes the whole chain on each call, so that was 21 downscales' worth of
           load time to reach the same texture. The pair below is the new contract
           and it is a tighter one -- a second upload at a non-zero level would now
           be a failure rather than the norm. */
        ck(levels == 1, "exactly one level is uploaded, at GL level 0",
           "%d glTexImage2D calls for texture 0 at quality %d", levels, q);
        ck(glcap_mipgen_count(s.batches[0].gl_tex) == 1,
           "and the chain below it is generated, once",
           "%d glGenerateMipmap calls at quality %d",
           glcap_mipgen_count(s.batches[0].gl_tex), q);

        /* EVERY texture, not just the first -- and the classification with it.
         *
         * Checking only texture 0 left a real hole. Skipping the READ of a level
         * rather than only its upload desynchronises the file stream, so every
         * texture AFTER the first gets a garbage header -- while texture 0's own
         * header was parsed before the damage and still looks perfect. A mutation
         * doing exactly that survived the entire suite. */
        {
            unsigned int t;
            int all_top = 1;
            for (t = 0; t < s.n_tex; t++) {
                int seen = -1, chained = (t != 3);
                if (t == 5)      /* declared with no pixels at all */
                    continue;
                for (i = 0; i < n_uploads; i++)
                    if (uploads[i].tex == s.tex_ids[t] && uploads[i].level == 0)
                        seen = uploads[i].w;
                /* Texture 3 has a single level, so it cannot be reduced and must
                   arrive at full size whatever the quality. */
                if (seen != (chained ? (64 >> q) : 64))
                    all_top = 0;
            }
            ck(all_top, "every texture survives the level skip, not just the first",
               "quality %d: all %u textures gave a %d px level 0 (a mismatch here "
               "means the file stream is out of step)", q, s.n_tex, 64 >> q);
        }
        ck(!(s.batches[0].flags & BATCH_ALPHA_KEYED)
           && (s.batches[1].flags & BATCH_ALPHA_KEYED)
           && !(s.batches[2].flags & BATCH_ALPHA_KEYED),
           "and the cut-out is still the batch that is flagged",
           "quality %d: flags %u / %u / %u", q, s.batches[0].flags,
           s.batches[1].flags, s.batches[2].flags);
        ck(!(s.batches[3].flags & BATCH_ALPHA_KEYED),
           "a single-level texture still uploads and is classified",
           "quality %d: flags %u", q, s.batches[3].flags);
        /* ...and is NOT sent for generation. Texture 3 ships one level, so it
           takes GL_LINEAR and no chain; asking vitaGL to generate one anyway
           would allocate a mip pyramid for every lightmap atlas in the game. The
           guard on it is the same `(mips - skip) > 1` the filter choice uses, so
           this check is what stops the two drifting apart. */
        ck(glcap_mipgen_count(s.tex_ids[3]) == 0,
           "a single-level texture is not sent for mip generation",
           "quality %d: %d calls", q, glcap_mipgen_count(s.tex_ids[3]));
        scene_release(&s);
    }

    /* A texture cannot be reduced out of existence: asking to skip more levels
       than it has must leave its smallest, not nothing at all. */
    scene_set_tex_quality(SCENE_TEX_QUALITY_LEVELS + 10);
    ck(scene_tex_quality() == SCENE_TEX_QUALITY_LEVELS - 1,
       "an out-of-range quality is clamped, not obeyed",
       "got %d", scene_tex_quality());
    n_uploads = 0;
    if (scene_load(path, &s)) {
        ck(n_uploads > 0, "and something is still uploaded",
           "%d uploads", n_uploads);

        /* The culling box, built by scene_load and checked HERE rather than in
           part 10 -- part 10 sets its boxes by hand so that a broken builder and
           a broken culler cannot agree with each other.
           Two properties, and the second is the one that matters: the box must
           CONTAIN every vertex (or geometry disappears), and it must contain them
           with the pad to spare (or in-place animation, which is the swell,
           pushes a crest through a plane the box said was clear). */
        {
            unsigned int bi;
            int contains = 1, padded = 1;
            for (bi = 0; bi < s.n_batches; bi++) {
                const batch_t *b = &s.batches[bi];
                unsigned int v;
                if (!b->nverts) continue;
                for (v = 0; v < b->nverts; v++) {
                    const float p[3] = { b->verts[v].x, b->verts[v].y, b->verts[v].z };
                    int k;
                    for (k = 0; k < 3; k++) {
                        if (p[k] < b->bmin[k] || p[k] > b->bmax[k])
                            contains = 0;
                        if (p[k] < b->bmin[k] + SCENE_CULL_PAD - 1e-3f
                            || p[k] > b->bmax[k] - SCENE_CULL_PAD + 1e-3f)
                            padded = 0;
                    }
                }
            }
            ck(contains, "every vertex is inside its batch's culling box",
               "a vertex escaped -- culling would drop geometry that is on screen");
            ck(padded, "and inside it by the full SCENE_CULL_PAD",
               "the %.1f m of slack the animated water needs is not there",
               (double)SCENE_CULL_PAD);
        }
        scene_release(&s);
    }
    scene_set_tex_quality(0);

    /* ---- the RGB565 channel order ---------------------------------------
     *
     * The assets are packed red-high (standard 565) because that is what the
     * hardware reads; Vita3K reads the same GXM format the other way round, so
     * the port carries a switch. Asserted on THE BITS THAT REACH GL, which is all
     * either machine ever sees, and against both literals -- so a mutation that
     * exchanges the wrong fields, or swizzles unconditionally, has nowhere to
     * hide. See scene.h.
     */
    {
        int i, sw;
        /* the fixture's RGBA texel is r=200 g=150 b=100 a=255, which as a
           little-endian word is 0xFF6496C8 */
        const unsigned int RGBA_TEXEL = 0xFF6496C8u;

        for (sw = 0; sw < 2; sw++) {
            unsigned int t565 = 0, trgba = 0;
            scene_set_tex_swap_rb(sw);
            ck(scene_tex_swap_rb() == sw,
               "the channel-order switch is what was set",
               "asked %d, got %d", sw, scene_tex_swap_rb());
            n_uploads = 0;
            if (!scene_load(path, &s)) {
                ck(0, "the fixture reloads for the channel-order check",
                   "swap %d", sw);
                return;
            }
            for (i = 0; i < n_uploads; i++) {
                if (uploads[i].level != 0)
                    continue;
                if (uploads[i].tex == s.batches[4].gl_tex)
                    t565 = uploads[i].texel0;
                if (uploads[i].tex == s.batches[0].gl_tex)
                    trgba = uploads[i].texel0;
            }
            if (!sw)
                ck(t565 == FIXTURE_565_PACKED,
                   "off: the 565 texel reaches GL exactly as packed",
                   "uploaded 0x%04X, packed 0x%04X", t565, FIXTURE_565_PACKED);
            else
                ck(t565 == FIXTURE_565_SWAPPED,
                   "on: red and blue are exchanged, green untouched",
                   "uploaded 0x%04X, wanted 0x%04X", t565, FIXTURE_565_SWAPPED);
            /* An RGBA texture must be untouched either way. It goes to GXM's
               U8U8U8U8_ABGR, which both machines read correctly, so swizzling it
               would break every cut-out in order to fix the terrain. */
            ck(trgba == RGBA_TEXEL,
               "and an RGBA texture is never swizzled",
               "swap %d: uploaded 0x%08X, packed 0x%08X", sw, trgba, RGBA_TEXEL);
            scene_release(&s);
        }
        scene_set_tex_swap_rb(0);
    }
    remove(path);
}

/* ============================================================= part 10 ==== */

/* A perspective projection in GL's column-major order: 90 degrees, square, near
   1, far 100. Built here rather than read back from the recorder on purpose --
   scene_frustum_from_gl's job is to read GL's live stacks, and a stub that
   invented them would be testing the stub. What this leaves uncovered is stated
   below, at the one check that can speak about it. */
static void perspective90(float m[16])
{
    const float zn = 1.f, zf = 100.f;
    int i;
    for (i = 0; i < 16; i++) m[i] = 0.f;
    m[0] = 1.f;                             /* 1/tan(45 deg), aspect 1 */
    m[5] = 1.f;
    m[10] = (zf + zn) / (zn - zf);
    m[11] = -1.f;
    m[14] = 2.f * zf * zn / (zn - zf);
}

/* Put an explicit box on a synthetic batch. The boxes scene_load computes are
   checked separately, against the real fixture file -- mixing the two would let a
   broken box builder and a broken culler agree with each other. */
static void set_box(batch_t *b, float cx, float cy, float cz, float half)
{
    b->bmin[0] = cx - half; b->bmax[0] = cx + half;
    b->bmin[1] = cy - half; b->bmax[1] = cy + half;
    b->bmin[2] = cz - half; b->bmax[2] = cz + half;
}

static void part10_culling(void)
{
    static const char *tex[] = {"t"};
    scene_t *s = make_scene(tex, 1);
    float vp[16];
    scene_stats_t st;
    unsigned int total_tris;

    printf("\n-- part 10: frustum culling --\n");

    perspective90(vp);          /* camera at the origin, looking down -Z */

    /* Four boxes: dead ahead, behind, far off to the right, and just inside the
       right edge. At z = -10 a 90 degree frustum is 10 units of half-width, so
       x = 9 is in and x = 50 is not -- and it is nowhere near the pad. */
    add_grid2(s, 0, 0.f, 0.f, 1.f, 1.f, 1, 0.f);        /* 0: ahead   */
    add_grid2(s, 0, 0.f, 0.f, 1.f, 1.f, 1, 0.f);        /* 1: behind  */
    add_grid2(s, 0, 0.f, 0.f, 1.f, 1.f, 1, 0.f);        /* 2: far right */
    add_grid2(s, 0, 0.f, 0.f, 1.f, 1.f, 1, 0.f);        /* 3: just inside */
    /* Boxes are set AFTER every add_grid2, because add_grid2 reallocs the batch
       array and a pointer taken before the next call is dangling. */
    set_box(&s->batches[0], 0.f, 0.f, -10.f, 1.f);
    set_box(&s->batches[1], 0.f, 0.f, +10.f, 1.f);
    set_box(&s->batches[2], 50.f, 0.f, -10.f, 1.f);
    set_box(&s->batches[3], 9.f, 0.f, -10.f, 1.f);
    total_tris = 4 * 2;

    /* --- off by default -------------------------------------------------- */
    scene_cull_off();
    gl_cap_reset();
    scene_stats_reset();
    scene_draw(s, 0, 0);
    ck(glcap.n_draws == 4, "with no frustum set, everything is drawn",
       "%d draws", glcap.n_draws);

    /* --- and the frustum decides --------------------------------------- */
    scene_set_frustum(vp);
    gl_cap_reset();
    scene_stats_reset();
    scene_draw(s, 0, 0);
    scene_stats_get(&st);
    ck(glcap.n_draws == 2, "only what the frustum contains is submitted",
       "%d draws, wanted the two in front", glcap.n_draws);
    ck(st.batches == 2 && st.batches_culled == 2,
       "and the accounting agrees with the submissions",
       "%u drawn / %u culled", st.batches, st.batches_culled);
    /* Not a restatement of the line above: this is the sum the frame log divides
       by, and a culler that dropped a batch out of BOTH tallies would pass the
       count check and silently under-report the track. */
    ck(st.tris + st.tris_culled == total_tris,
       "every triangle is accounted for on one side or the other",
       "%u + %u, wanted %u", st.tris, st.tris_culled, total_tris);

    /* --- the two exemptions, each for its own reason --------------------- */
    s->batches[2].flags |= BATCH_SKY;
    gl_cap_reset();
    scene_draw(s, 0, 0);
    ck(glcap.n_draws == 3,
       "the sky is never culled -- it rides a matrix the frustum is not from",
       "%d draws", glcap.n_draws);
    s->batches[2].flags &= ~BATCH_SKY;

    s->has_rig = 1;
    s->rig.n = 1;
    gl_cap_reset();
    scene_draw(s, 0, 0);
    ck(glcap.n_draws == 4,
       "and a rigged scene is not culled at all -- its parts move under rig.draw",
       "%d draws", glcap.n_draws);
    s->has_rig = 0;

    /* --- sweeping the edge, because a threshold tested at one value is not
           tested. x from 0 to 20 at z = -10: everything up to the 10-unit
           half-width plus the box's own half-extent must survive, and the far
           side must not. The transition is the only thing asserted, so a culler
           off by a plane sign fails and one off by a rounding does not. */
    {
        int i, last_in = -1, first_out = -1;
        for (i = 0; i <= 40; i++) {
            float x = (float)i * 0.5f;
            set_box(&s->batches[0], x, 0.f, -10.f, 1.f);
            gl_cap_reset();
            scene_draw(s, 0, 0);
            /* batch 0 alone: 3 is also in view, so "drawn" is n_draws > 1 */
            if (glcap.n_draws == 2) { if (first_out < 0) last_in = i; }
            else if (first_out < 0) first_out = i;
        }
        ck(last_in >= 0 && first_out > last_in,
           "the culler has a single crossing as a box slides out sideways",
           "last in at x=%.1f, first out at x=%.1f",
           last_in * 0.5, first_out * 0.5);
        /*
         * And it crosses where the geometry says, derived here rather than
         * copied off a run.
         *
         * NOT at 10. A box-versus-plane test is conservative by construction: it
         * keeps the box while ANY corner is inside, and the corner that lasts
         * longest is the DEEP one. This box spans z = -9 to -11, and a 90 degree
         * frustum's half-width equals |z|, so the far face sits where the frustum
         * is 11 wide; add the box's own 1 of half-extent and the last x that
         * survives is 12. (Being wrong in this direction costs a draw. Being
         * wrong in the other costs a triangle that was on screen, which is why
         * the test is written from the conservative side.)
         */
        {
            float halfw_at_far = 11.f;      /* |z| at the box's far face */
            float want = halfw_at_far + 1.f;    /* + the box's half-extent */
            float got = first_out * 0.5f;
            ck(got > want && got <= want + 0.5f + 1e-3f,
               "and it crosses at the frustum's own edge, from the safe side",
               "first out at x=%.1f, wanted just past %.1f", got, want);
        }
    }
    set_box(&s->batches[0], 0.f, 0.f, -10.f, 1.f);

    /*
     * --- and the NEAR and FAR planes, which nothing above reaches.
     *
     * The box "behind the camera" looks like a depth test and is not one: at
     * z = +10 the side planes reject it first, so deleting near and far entirely
     * left the whole suite green. These two boxes sit on the view axis, where
     * every side plane passes by construction, so only a depth plane can cull
     * them. Near is 1 and far is 100 in perspective90.
     */
    set_box(&s->batches[1], 0.f, 0.f, -200.f, 1.f);     /* past the far plane */
    set_box(&s->batches[2], 0.f, 0.f, -0.2f, 0.1f);     /* inside the near one */
    set_box(&s->batches[3], 0.f, 0.f, -50.f, 1.f);      /* comfortably between */
    gl_cap_reset();
    scene_draw(s, 0, 0);
    ck(glcap.n_draws == 2,
       "on the view axis, only the near and far planes can decide -- and they do",
       "%d draws, wanted the two between near and far", glcap.n_draws);
    set_box(&s->batches[1], 0.f, 0.f, +10.f, 1.f);
    set_box(&s->batches[2], 50.f, 0.f, -10.f, 1.f);
    set_box(&s->batches[3], 9.f, 0.f, -10.f, 1.f);

    /* --- an empty scene is not a crash ----------------------------------- */
    {
        scene_t empty;
        memset(&empty, 0, sizeof(empty));
        gl_cap_reset();
        scene_draw(&empty, 0, 0);
        ck(glcap.n_draws == 0, "an empty scene draws nothing and does not fault",
           "%d draws", glcap.n_draws);
    }

    /* Culling OFF on the way out. It is global state and the next part would
       otherwise inherit a frustum it never asked for. */
    scene_cull_off();
    scene_release(s);
    free(s);
}

/* ============================================================= part 11 ==== */

/*
 * Every component distinct and non-zero, and the indices REVERSED.
 *
 * write_fixture_vsc's batches are three vertices at x = 0,1,2 with every other
 * component zero, which cannot tell a right attribute offset from a wrong one --
 * reading u where lu was meant gives 0.0 either way, and so does reading past
 * the end of the buffer into fresh malloc. Same trap as the pure-red texel in
 * CLAUDE.md. Reversing the indices means the element buffer's CONTENTS have to
 * arrive too, not merely its existence.
 */
static void w_batch_distinct(FILE *f, unsigned tex, unsigned flags,
                             unsigned lm, int seed)
{
    int i;
    w_u32(f, tex);
    w_u32(f, flags);
    w_u32(f, 0);                       /* part */
    w_u32(f, lm);
    w_u32(f, 3);                       /* nverts */
    w_u32(f, 3);                       /* nidx */
    for (i = 0; i < 3; i++) {
        float b = (float)(seed * 100 + i * 10);
        float v[7] = { b + 1.f, b + 2.f, b + 3.f,   /* x  y  z  */
                       b + 4.f, b + 5.f,            /* u  v     */
                       b + 6.f, b + 7.f };          /* lu lv    */
        fwrite(v, sizeof(float), 7, f);
    }
    for (i = 0; i < 3; i++)
        w_u16(f, (unsigned)(2 - i));
}

static const char *write_vbo_fixture(void)
{
    static const char *path = "/tmp/rccars_vis_test_vbo.vsc";
    FILE *f = fopen(path, "wb");
    if (!f)
        return NULL;
    fwrite("VSC6", 1, 4, f);
    w_u32(f, 4);                       /* textures */
    w_u32(f, 4);                       /* batches */
    w_u32(f, 0);                       /* parts */
    w_u32(f, 0);                       /* markers */
    { float r = 0.f; fwrite(&r, sizeof r, 1, f); }
    w_tex_rgba(f, "opaque_a", 64, 0, 0);
    w_tex_rgba(f, "opaque_b", 64, 0, 0);
    w_tex_rgba(f, "opaque_c", 64, 0, 0);
    w_tex_rgba(f, "flat_lm",  64, 0, 1);
    w_batch_distinct(f, 0, 0,            0xFFFFFFFFu, 0);  /* plain */
    w_batch_distinct(f, 1, 0,            3,           1);  /* lightmapped */
    w_batch_distinct(f, 2, BATCH_WATER,  0xFFFFFFFFu, 2);  /* animated */
    w_batch_distinct(f, 0, 0,            0xFFFFFFFFu, 3);  /* plain */
    fclose(f);
    return path;
}

static void part11_vertexbuffers(void)
{
    const char *path = write_vbo_fixture();
    scene_t s;
    int live_before, i, buffered = 0, unbuffered = 0;

    printf("\n-- part 11: static geometry on the GPU --\n");

    if (!path) {
        ck(0, "the vertex-buffer fixture .vsc could be written", "fopen failed");
        return;
    }
    live_before = glcap_buffers_live();
    scene_set_tex_quality(0);
    if (!scene_load(path, &s)) {
        ck(0, "the vertex-buffer fixture loads", "scene_load failed");
        return;
    }

    /* ---- which batches got one ------------------------------------------ */
    for (i = 0; i < (int)s.n_batches; i++)
        if (s.batches[i].gl_vbo && s.batches[i].gl_ibo) buffered++;
        else unbuffered++;
    ck(buffered == 3 && unbuffered == 1,
       "the static batches go on the GPU and the animated one does not",
       "%d buffered, %d on client pointers", buffered, unbuffered);
    /* Named, not counted: it must be the WATER batch that stayed behind. water.c
       rewrites those vertices from `rest` every frame, so a static buffer there
       would freeze the swell at its packed pose -- a still sea, which reads as
       "water is broken" and not as "the buffer is stale". */
    ck(!s.batches[2].gl_vbo && s.batches[0].gl_vbo && s.batches[1].gl_vbo
       && s.batches[3].gl_vbo,
       "and it is the water batch specifically that kept its client pointers",
       "vbos %u %u %u %u", s.batches[0].gl_vbo, s.batches[1].gl_vbo,
       s.batches[2].gl_vbo, s.batches[3].gl_vbo);
    ck(glcap_buf_bound[0] == 0 && glcap_buf_bound[1] == 0,
       "scene_load leaves nothing bound",
       "array %u, element %u", glcap_buf_bound[0], glcap_buf_bound[1]);

    /* ---- the geometry that comes back is the geometry that was packed ----
     *
     * This is the check that the buffer CONTENTS and the attribute offsets are
     * both right, and it reads them back through the same recorder every other
     * geometry check in this file uses. A batch whose vertices never reached its
     * buffer, or an offset naming the wrong component, lands here.
     */
    gl_cap_reset();
    scene_draw(&s, 0, 0);
    ck(glcap.n_draws == 4, "all four batches draw", "%d draws", glcap.n_draws);
    {
        int wrong = 0, d;
        for (d = 0; d < glcap.n_draws && d < 4; d++) {
            const glcap_draw *dr = &glcap.draws[d];
            int k;
            for (k = 0; k < dr->count; k++) {
                /* indices are 2,1,0 -- so submission k is vertex 2-k */
                float b = (float)(d * 100 + (2 - k) * 10);
                const float *p = glcap.pos[dr->first + k];
                const float *t = glcap.uv[dr->first + k];
                if (p[0] != b + 1.f || p[1] != b + 2.f || p[2] != b + 3.f
                    || t[0] != b + 4.f || t[1] != b + 5.f)
                    wrong++;
            }
        }
        ck(wrong == 0,
           "every vertex arrives with the position and UV it was packed with",
           "%d of 12 wrong", wrong);
    }
    /* The reversal is the point: with idx 2,1,0 the FIRST vertex submitted for
       batch 0 is its last one. An element buffer that never got its contents --
       or a draw that ignored it and walked 0,1,2 -- gives 1.0 here. */
    ck(glcap.pos[glcap.draws[0].first][0] == 21.f,
       "and in the order the index buffer asks for, not in array order",
       "first x = %.1f, wanted 21.0 (vertex 2)",
       glcap.pos[glcap.draws[0].first][0]);

    /* ---- the lightmap UV is lu/lv, not u/v ------------------------------
     *
     * Both are two floats in the same vertex 8 bytes apart, so inside a buffer
     * this is purely an offset, and part 4's version of this check compares
     * pointers -- which a buffered batch does not have. Read the value instead:
     * batch 1's last-submitted vertex is its vertex 0, lu = 106.
     */
    ck(cap_lm_uv && ((const float *)cap_lm_uv)[0] == 106.f
                 && ((const float *)cap_lm_uv)[1] == 107.f,
       "a lit batch's second UV set comes from lu/lv inside the buffer",
       "got %.1f, %.1f -- wanted 106.0, 107.0 (lu/lv of batch 1 vertex 0)",
       cap_lm_uv ? ((const float *)cap_lm_uv)[0] : -1.f,
       cap_lm_uv ? ((const float *)cap_lm_uv)[1] : -1.f);

    /* ---- and nothing is left bound -------------------------------------
     *
     * The one way this change can break code it never touched. A bound
     * GL_ARRAY_BUFFER turns every subsequent gl*Pointer argument into an offset,
     * and fx.c, trace.c, ui.c, shadow.c, water.c and envmap.c all still pass real
     * addresses -- which would then be added to the track's buffer base and read
     * as geometry. Asserted directly here, and again below as a consequence.
     */
    ck(glcap_buf_bound[0] == 0 && glcap_buf_bound[1] == 0,
       "scene_draw leaves nothing bound either",
       "array %u, element %u", glcap_buf_bound[0], glcap_buf_bound[1]);

    /* ---- the consequence, through a module that really does draw from a
     *      client pointer ------------------------------------------------
     *
     * Asserting the binding is 0 tests the state; this tests what the state is
     * FOR. fx.c hands GL the address of its own particle buffer and never binds
     * anything, so if scene_draw left a buffer bound the dust would be read from
     * base + that address. The particles are emitted at a car sitting at the
     * origin, so their vertices belong within a few metres of it.
     */
    {
        static const char *dtex[] = {"dust"};
        scene_t *ds = make_scene(dtex, 1);
        col_t plane;
        fx_t fx;
        rb_car c;
        float eye[3] = { 0.f, 1.f, -3.f };
        float right[3] = { 1.f, 0.f, 0.f }, up[3] = { 0.f, 1.f, 0.f };
        int n, far_off = 0;

        make_plane(&plane, 8.f, 8, 0.f);
        fx_init(&fx, ds);
        fake_car(&c, 5.f, 0.f);
        plane_material(&plane, SURF_GRAVEL, SURF_ASPHALT);
        n = emit_once(&fx, &c, &plane, 1.f / 60.f, 30);

        gl_cap_reset();
        scene_draw(&s, 0, 0);         /* buffered scene first ... */
        fx_draw(&fx, eye, right, up); /* ... then a client-pointer module */
        {
            int k;
            for (k = 0; k < glcap.n_verts; k++) {
                const float *p = glcap.pos[k];
                if (!(p[0] > -100.f && p[0] < 1000.f)
                    || !(p[1] > -100.f && p[1] < 1000.f)
                    || !(p[2] > -100.f && p[2] < 1000.f))
                    far_off++;
            }
        }
        ck(n > 0 && far_off == 0,
           "a client-pointer module drawing straight after scene_draw reads its "
           "own array",
           "%d particles, %d vertices out of the world", n, far_off);
        scene_release(ds);
        free(ds);
    }

    /* ---- and they are handed back ---------------------------------------
     *
     * The menu changes track and car all day and each scene is a few MB of
     * vertices. Counted rather than eyeballed, and over two reloads, because a
     * release that frees none looks identical to one that frees all on a single
     * pass. water_init leaked on every reload for exactly this long.
     */
    scene_release(&s);
    ck(glcap_buffers_live() == live_before,
       "scene_release hands every buffer back",
       "%d live, started at %d", glcap_buffers_live(), live_before);
    {
        int j, peak = 0;
        for (j = 0; j < 2; j++) {
            scene_t r;
            if (!scene_load(path, &r)) { ck(0, "reload", "failed"); return; }
            if (glcap_buffers_live() > peak) peak = glcap_buffers_live();
            scene_release(&r);
        }
        ck(glcap_buffers_live() == live_before && peak > live_before,
           "and two more load/release cycles do not grow the count",
           "%d live after, peak %d, started at %d",
           glcap_buffers_live(), peak, live_before);
    }
}

/* ============================================================= part 12 ==== */

/*
 * scene_draw_model -- the drawing half of the knockable props. proptest.c has
 * the physics; this is the part that has a GL to record.
 */
static void part12_propdraw(void)
{
    static const char *tex[] = {"a", "b"};
    scene_t *s = make_scene(tex, 2);
    float m[16];
    int i;

    printf("\n-- part 12: drawing one model out of a shared scene --\n");

    /* Three batches over two models, the way props.vsc is built. */
    add_grid2(s, 0, 0.f, 0.f, 1.f, 1.f, 1, 0.f);
    add_grid2(s, 1, 0.f, 0.f, 1.f, 1.f, 1, 0.f);
    add_grid2(s, 0, 0.f, 0.f, 1.f, 1.f, 1, 0.f);
    s->batches[0].model = 0;
    s->batches[1].model = 1;
    s->batches[2].model = 1;
    s->n_models = 2;

    for (i = 0; i < 16; i++) m[i] = (i % 5) ? 0.f : 1.f;
    m[12] = 7.f; m[13] = 0.f; m[14] = -3.f;

    gl_cap_reset();
    scene_draw_model(s, 1, m);
    ck(glcap.n_draws == 2,
       "only the batches belonging to that model are submitted",
       "%d draws, wanted the 2 of model 1", glcap.n_draws);

    gl_cap_reset();
    scene_draw_model(s, 0, m);
    ck(glcap.n_draws == 1, "and the other model draws only its own",
       "%d draws", glcap.n_draws);

    /* The instance matrix has to reach GL, or every prop draws at the origin --
       which looks like "the props did not load" rather than like a bug. */
    ck(draw_had_matrix[0],
       "the instance matrix is pushed for it",
       "matrix=%d", draw_had_matrix[0]);

    mat_depth = mat_max_depth = mat_underflow = 0;
    for (i = 0; i < 50; i++) {
        gl_cap_reset();
        scene_draw_model(s, i & 1, m);
    }
    ck(mat_depth == 0 && !mat_underflow,
       "and the stack is balanced over 50 instances",
       "depth %d, underflow %d", mat_depth, mat_underflow);
    ck(glcap_buf_bound[0] == 0 && glcap_buf_bound[1] == 0,
       "with no buffer left bound for the client-pointer modules after it",
       "array %u, element %u", glcap_buf_bound[0], glcap_buf_bound[1]);

    /*
     * CULLING MUST NOT APPLY, and this is the check that would have caught it.
     * A prop's batch box is in MODEL space, around the origin; the instance is
     * up to 100 m away. If scene_draw_model consulted the global frustum the
     * props would vanish the moment the track's own pass set one -- i.e. always,
     * in the real app, and never in a test that forgot to set one.
     */
    {
        float f[16];
        for (i = 0; i < 16; i++) f[i] = (i % 5) ? 0.f : 1.f;
        /* a frustum well away from the model-space origin */
        f[12] = -500.f;
        scene_set_frustum(f);
        gl_cap_reset();
        scene_draw_model(s, 1, m);
        ck(glcap.n_draws == 2,
           "a frustum set by the track's pass cannot cull a prop away",
           "%d draws with a frustum the model-space box fails", glcap.n_draws);
        /* and the caller's culling state survives, for the pass after this one */
        ck(scene_cull_is_on(),
           "and the frustum is handed back to whatever draws next",
           "cull_on=%d", scene_cull_is_on());
        scene_cull_off();
    }
    scene_release(s);
    free(s);
}

int main(void)
{
    printf("RC Cars -- visual subsystem harness\n");
    part1_shadow();
    part2_checkpoints();
    part3_water();
    part4_scene();
    part5_antenna();
    part6_fx();
    part7_trace();
    part8_envmap();
    part9_texquality();
    part10_culling();
    part11_vertexbuffers();
    part12_propdraw();
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
