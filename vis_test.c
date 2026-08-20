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
#include "hud_data.h"
#include "antenna.h"
#include "col.h"
#include "vis_data.h"
#include "fx.h"
#include "fx_data.h"      /* fx_surf[] -- the dust rows the engine's class picks */
#include "trace.h"
#include "sun.h"
#include "carani.h"       /* carani_tire_width -- the mark follows the tyre */
#include "rb_data.h"      /* RB_CARS[].tune, the game's own upgrades.ini rows */
#include "rbcar.h"        /* rbcar_init, to bind a real car's rig to its mesh */
#include "envmap.h"
#include "sfx.h"          /* SURF_*, the material classes the grid carries */
#include "ai.h"           /* a REAL opponent, for part 14 */
#include "tracks.h"       /* the ten race starts, for part 2 */
#include "ai_data.h"      /* AI_RACES[].track -- which grid to load */

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
static GLenum st_depth_func = GL_LESS;
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
/* Unit 1's env, and where it takes its alpha from. Separate from unit 0's
   because the two are set to different things in the same pass. */
static GLenum st_env_mode1 = GL_MODULATE, st_a_src1 = GL_TEXTURE;
void glTexEnvi(GLenum t, GLenum p, GLint v)
{
    (void)t;
    if (p == GL_TEXTURE_ENV_MODE) {
        if (cur_unit) st_env_mode1 = (GLenum)v;
        else          st_env_mode  = (GLenum)v;
    }
    if (p == GL_SRC0_ALPHA && cur_unit)
        st_a_src1 = (GLenum)v;
}
void glDepthFunc(GLenum f) { st_depth_func = f; }
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
    d->depth_func = st_depth_func;
    d->unit1_tex = glcap_unit_enabled[1] ? glcap_unit_tex[1] : 0;
    d->unit1_env = st_env_mode1;
    d->unit1_a_src = st_a_src1;
    d->unit1_uv = (glcap_unit_enabled[1] && st_lm_array) ? cap_lm_uv : NULL;
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
/* ai_track.spine for the checks that drive ai_step -- the same adapter main.c
   binds, including the HINT, because the hint is the thing under test. */
static int vt_spine(void *ctx, float x, float y, float z, float hint,
                    float *dist, int *cp)
{
    return cp_spine_dist_near((const checkpoints_t *)ctx, x, y, z, hint,
                              dist, cp);
}

/* ai_track.lap_progress -- the LATCHED measure the placing uses for the player. */
static int vt_progress(void *ctx, float x, float y, float z, float *out)
{
    return cp_lap_progress((const checkpoints_t *)ctx, x, y, z, out);
}

/* How many times THIS drive has crossed the start/finish line. The lap rule
   below needs to tell the opening crossing from the rest, and cp_drive is called
   fresh for each path. */
static int line_crossings;

static int cp_drive(checkpoints_t *c, const float path[][3], int np, int laps,
                    float step, float *worst, int *order, int *lap_bad)
{
    int fires = 0, seg, lap;

    *worst = -1.f;
    *order = 1;
    *lap_bad = 0;
    line_crossings = 0;

    cp_step(c, path[0][0], path[0][1], path[0][2], 0.016f);
    {
        int want = c->next;
        /* laps + a RUN-OUT. The pass fires just after the closest approach, so a
           path that ENDS on a marker never finishes passing it -- and the closed
           path ends on cp_0, which is the crossing that counts the lap. A real car
           drives on; the fixture has to as well or the last lap is one crossing
           short. Two metres of the first leg is enough and is nowhere near the next
           checkpoint. */
        for (lap = 0; lap <= laps; lap++)
        for (seg = 0; seg + 1 < np; seg++) {
            if (lap == laps && seg > 0)
                break;                       /* the run-out is the first leg only */
            float ax = path[seg][0], az = path[seg][2];
            float bx = path[seg + 1][0], bz = path[seg + 1][2];
            float len = sqrtf((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
            int n = (int)(len / step) + 1, i;
            if (lap == laps) {
                n = (int)(2.f / step) + 1;   /* 2 m of run-out, not the whole leg */
                if (len > 1e-6f) { bx = ax + (bx - ax) * (2.f / len);
                                   bz = az + (bz - az) * (2.f / len); }
            }
            for (i = 1; i <= n; i++) {
                float u = (float)i / (float)n;
                float x = ax + (bx - ax) * u, z = az + (bz - az) * u;
                int lap_was = c->lap;
                cp_step(c, x, 0.f, z, 0.016f);
                /* THE LAP HAS TO TICK OVER ON THE START LINE, nowhere else, AND
                   NOT ON THE OPENING CROSSING.
                   Counting it one station early passes any check on the TOTAL --
                   there is still exactly one per lap -- and it is a real error,
                   because the opponents' lead is spine_len*lap + distance along
                   the spine, so a lap counted at the last checkpoint puts a whole
                   lap of it into the stretch before the line. A mutant that did
                   exactly that survived everything here until this line.
                 *
                   And the OPENING crossing counts none: the grid is short of the
                   line on all ten tracks, so every race crosses it in its first
                   second, and counting that put the player a phantom 500 m ahead
                   of a field with no laps -- which read on screen as `Lap 2'
                   before the first corner and as first place for the whole race.
                   See checkpoints_t.started. */
                if (c->passed == 0)
                    line_crossings++;
                {
                    const int want_tick = (c->passed == 0 && line_crossings > 1);
                    if ((c->lap != lap_was) != want_tick)
                        *lap_bad = 1;
                }
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

            /* On the grid -- and path[0] IS the start/finish marker -- nothing
               has been passed, so the arrow is on CHECKPOINT 0 and stays there.
               That identity (next == last + 1, last == -1) is what replaced
               projecting the grid onto the spine, which pointed five of the ten
               real tracks at the wrong checkpoint. */
            cp_step(&c, path[0][0], path[0][1], path[0][2], 0.016f);
            ck(c.next == 0 && c.passed < 0,
               "a fresh cursor heads for the START LINE and passes nothing",
               "next = %d, passed = %d", c.next, c.passed);

            fires = cp_drive(&c, path, np, 2, STEP, &worst, &order_ok, &lap_bad);
            /* L*n + 1, and the +1 is the point. The path starts ON the start line
               with a fresh cursor, so the FIRST thing the car does is cross it --
               which is what a race start is, and what five of the ten real tracks
               never did before this. Then n-1 more per lap and the line again at
               the end of each. */
            ck(fires == 2 * c.n + 1,
               "one crossing per checkpoint per lap, plus the start line at t=0",
               "%d crossings over 2 laps of %d checkpoints (want %d)",
               fires, c.n, 2 * c.n + 1);
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
               "lap = %d after 2 laps from the line -- the OPENING crossing "
               "counts none, so 2 and not 3", c.lap);
            /* AND A RESTART RE-ARMS IT. cp_restart clears `started` along with
               the lap, so the next race's own opening crossing counts none
               either -- otherwise the first race is right and every one after it
               reads `Lap 2' from the grid, which is the same bug with a lifetime
               instead of a line of code. Driven, not asserted on the field. */
            {
                float w2; int o2, lb2, f2;
                cp_restart(&c, path[0][0], path[0][1], path[0][2]);
                ck(c.lap == 0, "a restart puts the lap back to 0", "lap %d", c.lap);
                f2 = cp_drive(&c, path, np, 2, STEP, &w2, &o2, &lb2);
                ck(f2 == 2 * c.n + 1 && !lb2,
                   "and the SECOND race crosses the line the same number of times",
                   "%d crossings, lap_bad %d", f2, lb2);
                ck(c.lap == 2,
                   "and counts the same two laps -- the restart re-armed the "
                   "opening crossing", "lap = %d", c.lap);
            }
        }

        /* A WIDE LINE, 3 m off. This used to be here as the case a radius rule
           could not do, and the radius rule is what ships now -- so it stays as
           the case that says the radius is wide enough for a line that is not the
           racing line. 3 m is inside CP_TRIGGER_RAD's 5 m by design; the measured
           worst real pass is 1.58 m. Same lap, driven 3 m OUTSIDE every leg -- outside
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
            int hit[CP_MAX];
            memset(hit, 0, sizeof(hit));
            cp_restart(&c, path[0][0], path[0][1], path[0][2] - 3.f);
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
                    if (c.passed >= 0) { hit[c.passed] = 1; fires++; }
                }
            }
            for (i3 = 0; i3 < c.n; i3++) if (!hit[i3]) fires = -1;
            ck(fires > 0, "a wide line still passes every checkpoint",
               "%d crossings, 3 m off the line, and %s", fires,
               fires > 0 ? "every checkpoint among them" : "one was MISSED");
        }

        /* A TELEPORT is not a lap. Respawning across the far side of the spine
           sweeps past every station in between, and cp_resync is what stops that
           being a burst of cues. */
        {
            int lap0 = c.lap;
            /* Aim the arrow somewhere it does not belong FIRST, so this tests
               the re-aim and not the value it happened to hold -- which is how a
               mutant that dropped the re-aim entirely once survived it. */
            c.next = (c.last + 2) % c.n;
            cp_resync(&c, c.cp[2].p[0][0], c.cp[2].p[0][1], c.cp[2].p[0][2]);
            cp_step(&c, c.cp[0].p[0][0], 0.f, c.cp[0].p[0][2], 0.016f);
            ck(c.passed < 0 && c.lap == lap0,
               "a teleport back to the start line passes nothing",
               "passed = %d, lap %d -> %d", c.passed, lap0, c.lap);
            /* The arrow comes off `last`, not off the car's position: whatever
               `next` was forced to, a resync puts it back on (last + 1). Forcing
               it to 2 above is what makes this a test of the re-aim rather than of
               the value it happened to hold. */
            ck(c.next == (c.last + 1) % c.n,
               "and re-aims the arrow at the one after the last it really passed",
               "next = %d, last = %d (aimed elsewhere before the teleport)",
               c.next, c.last);
        }

        /* And DRIVING BACKWARDS does not hand a checkpoint back. The radius rule
           has no notion of direction -- it fires on the closest approach to
           whatever `next` is -- so what has to hold is the weaker and more useful
           statement: reversing back over a checkpoint already BEHIND the car fires
           nothing, because that checkpoint is not what the cursor is looking at.
           Drive forward past cp_1 first so there is something behind to reverse
           over, then reverse over it. */
        {
            int lap0, i4, refired = 0, last0;
            cp_restart(&c, c.cp[0].p[0][0], c.cp[0].p[0][1], c.cp[0].p[0][2]);
            /* Out to 30 m: CLEAR of cp_2 at x = 20 by 10 m, i.e. twice
               CP_TRIGGER_RAD, so its pass has definitely completed before the
               reverse starts. Stopping ON it instead would have the reverse
               complete the pass rather than hand one back, which is a different
               statement. */
            for (i4 = 0; i4 <= 60; i4++)
                cp_step(&c, (float)i4 * 0.5f, 0.f, 0.f, 0.016f);
            lap0 = c.lap; last0 = c.last;
            for (i4 = 60; i4 >= 0; i4--)          /* and back over it */
                if (cp_step(&c, (float)i4 * 0.5f, 0.f, 0.f, 0.016f), c.passed >= 0)
                    refired = 1;
            ck(!refired && c.lap == lap0 && c.last == last0,
               "reversing back over a passed checkpoint hands nothing back",
               "lap %d, last %d (was %d, %d)", c.lap, c.last, lap0, last0);
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

    /* BOTH arc measures interpolate along a leg. Measured against the fixture's
       own geometry: the first leg runs 10 m along +X from the origin, so 4 m
       along it is 4 m of arc.
     *
       This check used to assert the OPPOSITE of the second line -- that
       cp_spine_dist SNAPPED to the nearest sample, returning 0 here -- and said
       so as though it were intended ("the AI's measure, left alone"). It was the
       old implementation written down as a requirement: checkpoint.h has always
       described that function as the nearest point on the nearest SEGMENT, and
       with the spine's samples 23 to 40 m apart on the real tracks the snapping
       made its answer jump between them. Same shape as the lap count blessed at
       "1 at the start + 2". */
    {
        float s4 = -1.f, s_seg = -1.f;
        cp_progress(&c, 4.f, 0.f, &s4);
        cp_spine_dist(&c, 4.f, 0.f, 0.f, &s_seg, NULL);
        ck(near(s4, 4.f, 1e-3f), "cp_progress interpolates along a leg",
           "%.3f m of arc, 4 m along a 10 m leg", s4);
        ck(near(s_seg, 4.f, 1e-3f),
           "and so does cp_spine_dist -- the nearest point on the nearest "
           "SEGMENT, which is what its header always said",
           "%.3f m", s_seg);
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
        int cp_ground_n = 0, cp_ground_roof = 0, cp_ground_under = 0;
        int cp_ground_hit = 0, cp_respawn_n = 0, cp_respawn_bad = 0;
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

            /* THE MARKER'S GROUND IS THE FLOOR, NOT THE ROOF OVER IT.
             *
             * cp_t.ground is where BOTH the respawn point and the animated
             * marker go, and col_ground_at returns the HIGHEST surface under its
             * ceiling -- so on a track whose racing line runs through a tunnel
             * the ceiling decides which one it finds. At the 5 m cp_init shipped
             * with, NINE of the fifty checkpoints found a roof: a car sent back
             * to a tunnel checkpoint arrived on top of the tunnel, with its
             * marker up there and nowhere near the graffiti on the road.
             *
             * Both directions are held, because one alone passes on a broken
             * probe: the ground may not be ABOVE the marker by more than half a
             * metre (measured, the floor runs from 0.49 m below it to 0.24 m
             * above), and at least nine checkpoints across the ten must still
             * have something well OVER the ground that was chosen -- which is
             * what says the tunnels are real and this is under them rather than
             * that the probe found nothing at all. */
            {
                int k2;
                for (k2 = 0; k2 < rc.n; k2++) {
                    const float *mk = rc.cp[k2].p[0];
                    float over, nx2, ny2, nz2;
                    cp_ground_n++;
                    if (rc.cp[k2].ground > mk[1] + 0.5f)
                        cp_ground_roof++;
                    /* AND THE PROBE ACTUALLY HIT SOMETHING. cp_init falls back to
                       the marker's own y when nothing is under the ceiling, which
                       is safe but is NOT the ground -- and a ceiling too tight to
                       clear the floor (which runs up to 0.24 m ABOVE the marker)
                       loses it silently, passing the roof test because the
                       marker's own y is obviously not a roof. */
                    if (rc.cp[k2].ground != mk[1])
                        cp_ground_hit++;
                    /* AND IT REALLY IS UNDER SOMETHING on the tunnel ones:
                       anything more than 1.5 m over the ground that was chosen is
                       a roof or a deck. Without this, a probe that found nothing
                       at all would pass the test above. */
                    if (col_ground_at(&tc, mk[0], mk[2],
                                      rc.cp[k2].ground + 8.f,
                                      &over, &nx2, &ny2, &nz2)
                        && over > rc.cp[k2].ground + 1.5f)
                        cp_ground_under++;
                }
                /* AND THE TWO PROBES AGREE, which is the path the reported bug
                 * actually took and which nothing else here covers: cp_init
                 * probes once for cp_t.ground, then main.c's place_car probes
                 * AGAIN from that height with its own ceiling of ref_y + 1 m.
                 * Two ceilings, two call sites, and no harness compiles main.c --
                 * so the composition is replicated here. If they disagree the car
                 * lands somewhere the marker is not, which under a tunnel means
                 * on the roof. */
                for (k2 = 0; k2 < rc.n; k2++) {
                    float pos[3], yaw2, gy2, nx2, ny2, nz2;
                    checkpoints_t rr = rc;
                    rr.last = k2;
                    if (!cp_respawn_pose(&rr, pos, &yaw2))
                        continue;
                    cp_respawn_n++;
                    gy2 = pos[1];
                    /* place_car's own probe, verbatim: ceiling at ref_y + 1. */
                    if (!col_ground_at(&tc, pos[0], pos[2], pos[1] + 1.0f,
                                       &gy2, &nx2, &ny2, &nz2))
                        gy2 = pos[1];
                    if (fabsf(gy2 - rc.cp[k2].ground) > 0.25f)
                        cp_respawn_bad++;
                }
            }

            np = cp_flatten(&rc, path);
            /* 0.125 m: one 1/60 frame at this car's 7.5 m/s top speed, so the
               error bound below is bounded by a real frame and not by a step
               chosen to make it pass. */
            fires = cp_drive(&rc, path, np, 2, 0.125f, &worst, &order_ok,
                             &lap_bad);
            /* 2n + 1 crossings over two laps -- the +1 is the OPENING crossing of
               the line, which every race makes in its first second -- and 2 laps
               COMPLETED, because that opening one completes none. See
               checkpoints_t.started. */
            if (fires != 2 * rc.n + 1 || rc.lap != 2) bad_fires++;
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
        ck(cp_ground_n >= 45,
           "every track's checkpoints report a ground height",
           "%d checkpoints over %d tracks", cp_ground_n, loaded);
        ck(cp_ground_roof == 0,
           "and NONE of them sits on the roof over its own marker -- the "
           "respawn and the animated marker both go where this says",
           "%d of %d above the marker by over 0.5 m",
           cp_ground_roof, cp_ground_n);
        ck(cp_ground_hit >= 48,
           "and the probe really found terrain under all but a couple -- a "
           "ceiling too tight to clear the floor falls back to the marker's own "
           "float height and looks fine",
           "%d of %d hit", cp_ground_hit, cp_ground_n);
        ck(cp_respawn_n >= 45 && cp_respawn_bad == 0,
           "and place_car's SECOND probe lands where cp_init's first one said -- "
           "two ceilings at two call sites, and no harness compiles main.c",
           "%d of %d respawn points disagree by over 0.25 m",
           cp_respawn_bad, cp_respawn_n);
        ck(cp_ground_under >= 9,
           "while at least nine really are UNDER something -- so the tunnels "
           "are real and the probe is finding the floor, not nothing",
           "%d with a surface 1.5 m or more over the chosen ground",
           cp_ground_under);
        ck(loaded == 10 && bad_fires == 0,
           "every real spine passes each checkpoint once a lap, and counts the lap",
           "%d of %d tracks disagree", bad_fires, loaded);
        ck(bad_order == 0, "in the spine's own order on every one of them",
           "%d tracks out of order", bad_order);
        ck(bad_lap == 0, "and the lap only ever ticks over on the start line",
           "%d tracks tick it elsewhere", bad_lap);
        /* Bounded STRUCTURALLY, by the rule's own radius: a pass can only fire
           from inside CP_TRIGGER_RAD, so no cue can ever land further out than
           that however the spine wanders. The old arc rule had no such bound and
           fired 7.3 to 75.6 m early. Note these paths are the SPINE, which is not
           the road (see checkpoint.h) -- the honest measurement of this number is
           over the recorded laps below, where it is 4.24 m worst. */
        ck(loaded == 10 && worst_all >= 0.f
           && worst_all <= CP_TRIGGER_RAD + CP_PASS_EPS,
           "and no crossing on any real track fires from outside its own radius",
           "worst %.3f m over ten tracks, radius %.1f m (the OLD rule: 7.3 to "
           "75.6 m early)", worst_all, CP_TRIGGER_RAD);

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

    /* --- REAL DRIVEN LAPS, from the real grids ---------------------------
     *
     * Everything above drives the SPINE, and the spine is not the road -- that is
     * the whole finding behind the current rule (checkpoint.h). So the rule is
     * also held to the only path data in the project that IS the road: the ten
     * shipped .aip recordings, three recorded laps each, 404 to 555 m at 5 to
     * 25 cm per sample.
     *
     * Two things are asserted, and they are the two that were reported broken:
     *
     *   from the PLAYER's own grid, held there for the three-second countdown and
     *   then driven, the FIRST checkpoint to fire is the start/finish -- on all
     *   ten. Under the old rule it was checkpoint 2 on country_1, country_3,
     *   country_4 and urban_1 and checkpoint 5 on urban_2, and the start/finish
     *   never fired at all on the first lap of any of the five.
     *
     *   nothing fires while the car is HELD on the grid. Five of the ten grids sit
     *   inside cp_1's radius already, so firing on entry would sound the cue
     *   before GO on all five -- which is what CP_PASS_EPS is for.
     *
     * Then every checkpoint, once per lap, in order, over all 30 recordings.
     */
    {
        static const char *const TRK2[10] = {
            "beach_1", "beach_2", "beach_3", "beach_4", "country_1",
            "country_2", "country_3", "country_4", "urban_1", "urban_2"
        };
        int t, loaded = 0, first_wrong = 0, held_fired = 0;
        int miss = 0, ooo = 0, runs = 0;
        float worst = -1.f;

        for (t = 0; t < 10; t++) {
            char pp[64];
            scene_t ts;
            col_t tc;
            checkpoints_t base;
            ai_t ai;
            int i;

            snprintf(pp, sizeof(pp), "assets/%s.vsc", TRK2[t]);
            if (!scene_load(pp, &ts))
                continue;
            snprintf(pp, sizeof(pp), "assets/%s.col", TRK2[t]);
            memset(&tc, 0, sizeof(tc));
            col_load(pp, &tc);
            cp_init(&base, &ts, &tc);
            base.enabled = (base.n > 0);
            memset(&ai, 0, sizeof(ai));
            if (!ai_init(&ai, t, "assets", NULL, 1, 0) || base.n <= 0) {
                free(tc.tris); free(tc.start); free(tc.idx);
                scene_release(&ts);
                continue;
            }
            loaded++;

            for (i = 0; i < ai.n; i++) {
                const ai_car *a = &ai.car[i];
                checkpoints_t rc = base;
                int j, lap, seen[CP_MAX], want, first = -1;
                memset(seen, 0, sizeof(seen));
                /* the PLAYER's grid, not the opponent's slot: that is where the
                   arrow has to be right. */
                cp_restart(&rc, TRACKS[t].x, TRACKS[t].y, TRACKS[t].z);
                for (j = 0; j < 180; j++) {          /* 3 s of countdown, held */
                    cp_step(&rc, TRACKS[t].x, TRACKS[t].y, TRACKS[t].z, 1.f / 60.f);
                    if (rc.passed >= 0) held_fired++;
                }
                want = rc.next;
                for (lap = 0; lap < 2; lap++)
                    for (j = lap ? a->cycle_start : 0; j < a->n; j++) {
                        cp_step(&rc, a->s[j].p[0], a->s[j].p[1], a->s[j].p[2],
                                1.f / 60.f);
                        if (rc.passed < 0)
                            continue;
                        if (first < 0) first = rc.passed;
                        if (rc.passed != want) ooo++;
                        want = (rc.passed + 1) % rc.n;
                        seen[rc.passed]++;
                        {
                            const float *m = rc.cp[rc.passed].p[0];
                            float dx = m[0] - a->s[j].p[0];
                            float dz = m[2] - a->s[j].p[2];
                            float d = sqrtf(dx * dx + dz * dz);
                            if (d > worst) worst = d;
                        }
                    }
                if (first != 0) first_wrong++;
                /* Two laps of the recording plus the crossing that starts it: the
                   line three times, everything else twice. */
                for (j = 0; j < rc.n; j++)
                    if (seen[j] < (j == 0 ? 3 : 2)) miss++;
                runs++;
            }
            ai_free(&ai);
            free(tc.tris); free(tc.start); free(tc.idx);
            scene_release(&ts);
        }

        ck(loaded == 10 && runs == 30,
           "all ten tracks' recorded laps load (three opponents each)",
           "%d tracks, %d recordings", loaded, runs);
        ck(runs == 30 && first_wrong == 0,
           "the START/FINISH is the first checkpoint to fire, on every track",
           "%d of %d recordings started on the wrong one", first_wrong, runs);
        ck(held_fired == 0,
           "and nothing fires while the car is held on the grid for the countdown",
           "%d cues during 3 s x %d grids", held_fired, runs);
        ck(runs == 30 && miss == 0,
           "every checkpoint fires on every recorded lap, none missed",
           "%d missing over %d recordings", miss, runs);
        ck(ooo == 0, "and always in cp_1, cp_2, ... cp_n order",
           "%d out of order", ooo);
        ck(worst >= 0.f && worst <= CP_TRIGGER_RAD + CP_PASS_EPS,
           "each fires within the trigger radius of its own marker",
           "worst %.2f m of %.1f m (the OLD rule missed 174 of these 300 "
           "crossings outright)", worst, CP_TRIGGER_RAD);
    }
}

/* ============================================================== part 3 ==== */

static void part3_water(void)
{
    static const char *tex[] = {"water_wave", "water_wave_alpha"};
    scene_t *s = make_scene(tex, 2);
    water_t w;
    batch_t *sea, *coast, *stream, *pool;
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
    /* A pool: country_1's are 3-11 m across and sit 0-23 cm above the pit
       floor, so a small flat patch is the right stand-in. */
    add_grid2(s, BATCH_POOL, 10.f, -14.f, 18.f, -6.f, 4, -0.1f);
    sea = &s->batches[0];
    coast = &s->batches[1];
    stream = &s->batches[2];
    pool = &s->batches[3];
    /* distinct textures so the recorder can tell the four apart: they all
       submit per-vertex colour or their own alpha, and a check that cannot
       separate them ends up asserting the loosest bound on all of them */
    sea->gl_tex = s->tex_ids[0];
    coast->gl_tex = s->tex_ids[1];
    stream->gl_tex = next_tex_id++;
    pool->gl_tex = next_tex_id++;
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

    water_init(&w, s, &bed, 0);   /* beach_1's water surface section */
    ck(w.n_spawn == 1, "one spawner per water_wave_N marker", "%d", w.n_spawn);
    ck(sea->rest != NULL, "the surface keeps a rest copy", "%p",
       (void *)sea->rest);

    /* --- shallow-water damping ------------------------------------------ */
    {
        float near_shore = -1.f, far_shore = -1.f, quarter = -1.f;
        for (i = 0; i < sea->nverts; i++) {
            if (near(sea->verts[i].x, 0.f, 0.6f)) {
                if (near(sea->verts[i].z, 0.f, 0.6f)) near_shore = w.damp[0][i];
                if (near(sea->verts[i].z, 10.f, 0.6f)) quarter = w.damp[0][i];
                if (near(sea->verts[i].z, 40.f, 0.6f)) far_shore = w.damp[0][i];
            }
        }
        /* The ramp is LINEAR over cfg->magnet_radius -- FUN_0051c000 divides by
           magnetRadius and clamps, with no shaping -- and magnetRadius converts
           as raw*0.05, so the shipped value is 2.55 m and not the 0.5 m the port
           used to guess. The fixture's shelf only reaches 2.5 m under the far
           edge of the sea, which is beach_1's own 2.2 m rounded up, so the far
           row does NOT saturate: it comes out at depth/magnet_radius. Predict
           that from the fixture's geometry rather than asserting 1.0, which is
           what the old bound did and which a 2.55 m radius fails. */
        float deep = 40.f / 16.f;               /* the shelf is y = -z/16 */
        float want = deep / w.cfg->magnet_radius;
        if (want > 1.f) want = 1.f;
        ck(near(near_shore, 0.f, 0.05f),
           "damping is 0 where the seabed reaches the surface",
           "%.3f at the waterline", near_shore);
        /* Tight, because the point of it is that the far row does NOT reach
           1.0: a ramp over the old 0.5 m saturates here and 0.02 of slack was
           enough to let that through. */
        ck(near(far_shore, want, 0.005f),
           "and rises with depth over magnetRadius, without saturating",
           "%.3f at %.2f m of depth, expected %.3f over a %.2f m ramp",
           far_shore, deep, want, w.cfg->magnet_radius);
        /* And it is LINEAR. A quarter of the way up the ramp is where the
           shapes separate -- linear gives 0.25 where a smoothstep gives 0.16 --
           and at the halfway point the two agree exactly, which is why one
           sample cannot tell them apart. */
        {
            float shallow_d = 10.f / 16.f;
            float want_q = shallow_d / w.cfg->magnet_radius;
            float smooth = want_q * want_q * (3.f - 2.f * want_q);
            ck(near(quarter, want_q, 0.01f) && fabsf(quarter - smooth) > 0.05f,
               "and the ramp is LINEAR, not shaped",
               "%.3f at %.2f m: linear says %.3f, a smoothstep would say %.3f",
               quarter, shallow_d, want_q, smooth);
        }
    }

    /* --- the surface moves, and moves less near the shore ---------------- */
    {
        /* PEAK TO PEAK over time, not distance from rest. The surface carries a
           constant vertical offset (cfg->offset), and measuring against rest
           reports that offset as if it were wave height -- which it did, and
           read 0.39 m for a 0.02 m swell. Amplitude is immune to any static
           shift; "distance from rest" is not. */
        static float lo[8192], hi[8192];
        float amp_near = 0.f, amp_far = 0.f;
        const wsurf_t *wc = w.cfg;
        /* Peak to peak, undamped, straight out of the track's config: the amp
           term spans -0.5..+1.0 of `amp` and the amp2 term spans +-amp2. */
        float pp_cfg = 1.5f * wc->amp + 2.f * wc->amp2;
        float lam1 = 6.2831853f / wc->period;
        float damp_far = (40.f / 16.f) / wc->magnet_radius;
        unsigned int nv = sea->nverts < 8192 ? sea->nverts : 8192;
        if (damp_far > 1.f) damp_far = 1.f;
        for (i = 0; i < nv; i++) { lo[i] = 1e30f; hi[i] = -1e30f; }
        /* Twelve seconds. Both trains are slow -- 2*pi/(speed*period) is 5.1 s
           and 2*pi/period2 is 3.5 s on beach_1 -- and the old two-second window
           was sized for a 0.6 s ripple that only existed because `period` was
           being read as a time. A window shorter than a period reports a
           fraction of the swell and calls it the swell. */
        for (int step = 0; step < 12 * 60; step++) {
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
        /* Against the TRACK'S OWN CONFIG, not against a constant in water.c.
           The bound the old build carried was `> 0.4 * WATER_SWELL_AMP`, and
           WATER_SWELL_AMP was the very number the displacement was multiplied
           by -- it passed at 0.02 m and it would have passed at 0.002 m. The
           config is data the port does not get to choose. */
        ck(amp_far > 0.35f * pp_cfg * damp_far,
           "the open sea moves, and by the height its own config asks for",
           "peak to peak %.3f m over 12 s against the config's %.3f m "
           "(damped to %.3f by a %.2f m shelf)",
           2.f * amp_far, pp_cfg, pp_cfg * damp_far, 40.f / 16.f);
        ck(2.f * amp_far <= pp_cfg * damp_far + 1e-3f,
           "and never by more than it",
           "%.3f m <= %.3f m", 2.f * amp_far, pp_cfg * damp_far);
        ck(amp_near < amp_far * 0.25f, "the shoreline barely moves",
           "%.4f m against %.3f m", amp_near, amp_far);
        /* STEEPNESS, which is the bound that actually holds this together.
         *
         * The check that used to sit here was "no taller than a tenth of the
         * car's length", written when 0.41 m of swell had just been rejected as
         * a storm. It was the wrong quantity: 0.41 m is very close to what the
         * engine really does on this track, and what made it look like a storm
         * was the WAVELENGTH -- `period` read as a time gave 1.24 m, so the sea
         * was 0.41 m tall over 1.24 m, a 60% slope. Water cannot hold more than
         * about 14%; past that a wave breaks. A height bound alone passes a
         * flat-calm sea and fails a real one. A steepness bound fails exactly
         * the units bug and nothing else -- all five tracks with a sea land
         * between 2% and 9%.
         *
         * Read off the config, so it also fails if gen_vis_data.py's conversion
         * is what regresses; the amplitude checks above are what tie water.c to
         * the same numbers. */
        /* --- and the WAVELENGTHS water.c actually evaluates ---------------
         *
         * The steepness bound below reads the config, so it guards
         * gen_vis_data.py's conversion; this one guards water.c, by counting
         * how many times the surface turns over along a line. Sample
         * water_height through the radial train's centre in the directional
         * train's own direction, so both phases advance at their full rate --
         * one turn per half wavelength each.
         *
         * At the engine's 10.5 m and 10.1 m that is about 39 turns over 100 m.
         * Reading `period` as a time puts the wavelengths at 1.24 m and 1.61 m
         * and the same line turns over some 300 times: an ocean of ripples, and
         * the thing that made 0.41 m of swell look like a storm. Sampled at
         * 0.1 m, which resolves even the wrong answer -- a check that cannot
         * see the failure it is looking for reports the failure as a pass. */
        {
            const float L = 100.f, ds = 0.1f;
            float lam2 = 6.2831853f * wc->length2;
            /* Two trains this close in wavelength BEAT rather than add turns:
               the weaker one (0.75*amp = 0.12 m against amp2's 0.25 m) mostly
               fails to make extrema of its own, so the count lands on the
               dominant train's 2L/lambda and not on the sum. Bound both ends
               from the config -- the sum is the ceiling, the longer train alone
               is the floor. */
            float lo = 0.7f * 2.f * L / (lam1 > lam2 ? lam1 : lam2);
            float hi = 1.5f * 2.f * L * (1.f / lam1 + 1.f / lam2);
            float expect = 2.f * L * (1.f / lam1 + 1.f / lam2);
            int turns = 0, k;
            float prev_d = 0.f;
            for (k = 0; k < (int)(L / ds); k++) {
                float s0 = (float)k * ds, s1 = s0 + ds;
                float h0 = water_height(&w, wc->pos_x + w.d1x * s0,
                                            wc->pos_z + w.d1z * s0);
                float h1 = water_height(&w, wc->pos_x + w.d1x * s1,
                                            wc->pos_z + w.d1z * s1);
                float d = h1 - h0;
                if (k && ((d > 0.f) != (prev_d > 0.f))) turns++;
                prev_d = d;
            }
            ck((float)turns > lo && (float)turns < hi,
               "the surface turns over as often as its own wavelengths say",
               "%d turns over %.0f m, want %.0f..%.0f for %.1f m and %.1f m "
               "(the two trains together would be %.0f)",
               turns, L, lo, hi, lam1, lam2, expect);
        }
        /* --- WHERE the surface sits, which is the other half of the bug -----
         *
         * The swell rides on rest + offset + (1-damp)*magnetOffset, and that
         * base is what pack_col.py bakes into the .col water grid, so the
         * waterline the car feels is the one it can see. Subtract the swell
         * back off a drawn vertex and the remainder has to BE that base.
         *
         * This is the check that was missing. `offset` is per track, the port
         * compiled beach_1's -0.37 m into all ten, and the grid carried none of
         * it at all -- which put 234 m2 of beach_1 and 459 m2 of beach_4 under
         * water the drawn sea was nowhere near. Nothing in this part could see
         * that: every other bound here measures the surface's MOTION, and a
         * static shift is invisible to all of them. */
        {
            float worst_base = 0.f;
            unsigned int j;
            gl_cap_reset();
            water_draw(&w, eye);
            for (j = 0; j < sea->nverts; j++) {
                float k = w.damp[0][j];
                float base = sea->verts[j].y
                           - k * water_height(&w, sea->rest[j].x, sea->rest[j].z);
                float want = sea->rest[j].y + wc->offset
                           + (1.f - k) * wc->magnet_offset;
                if (fabsf(base - want) > worst_base) worst_base = fabsf(base - want);
            }
            ck(worst_base < 1e-4f,
               "and it rides on rest + the TRACK'S OWN vertical offset, which "
               "is what the .col grid is packed with",
               "off by at most %.5f m over %u vertices (offset %+.3f m, "
               "magnet %+.3f m)", worst_base, sea->nverts, wc->offset,
               wc->magnet_offset);
        }
        ck(pp_cfg / lam1 < 1.f / 7.f,
           "and the swell is not steeper than water can stand",
           "%.3f m over a %.1f m wavelength = %.1f%%, breaking is about 14%%",
           pp_cfg, lam1, 100.f * pp_cfg / lam1);
    }

    /* --- the surface's UVs ORBIT, and never leave the rest values behind --
     *
     * FUN_005240c0 advances a per-vertex phase and puts the UV on a circle of
     * radius texRad about its rest value. It does not scroll along a line: the
     * port did, on texScaleX/Z, whose conversion is raw*0.1 rather than the
     * raw*0.01 that was used and which is the world-to-UV rate for the grid the
     * engine tessellates for itself -- the port's tiles carry authored UVs.
     *
     * So the invariant is the RADIUS, which a linear scroll cannot hold: check
     * that every vertex stays on its own circle, that the circle is inside the
     * texRad range, and that it comes back round. */
    {
        const wsurf_t *wc = w.cfg;
        float worst = 0.f, biggest = 0.f;
        int outside = 0, moved = 0;
        float first[3];
        unsigned int j;
        for (int step = 0; step < 3; step++) {
            water_step(&w, 1.5f);
            gl_cap_reset();
            water_draw(&w, eye);
            for (j = 0; j < sea->nverts; j++) {
                float du = sea->verts[j].u - sea->rest[j].u;
                float dv = sea->verts[j].v - sea->rest[j].v;
                float r = sqrtf(du * du + dv * dv);
                if (step == 0 && j < 3) first[j] = r;
                if (j < 3 && fabsf(r - first[j]) > worst) worst = fabsf(r - first[j]);
                if (r > biggest) biggest = r;
                if (r > wc->tex_rad_max + 1e-4f) outside++;
                if (r > 1e-6f) moved++;
            }
        }
        ck(moved > 0 && outside == 0,
           "every surface UV stays on a circle inside texRadMin..texRadMax",
           "largest radius %.4f UV against texRadMax %.4f, %d outside",
           biggest, wc->tex_rad_max, outside);
        /* A constant radius over 4.5 s is also what says the UV is rebuilt from
           rest every frame rather than accumulated -- an accumulating one drifts
           off its circle. That is the property the old linear check tested. */
        ck(worst < 1e-4f,
           "and the radius holds -- an orbit rebuilt from rest, not a scroll",
           "radius drifted by %.6f UV over 4.5 s", worst);
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
           long enough the moment it became a swell. It is fixed on purpose: a
           window scaled by whatever sets the foam's rate would follow that rate
           anywhere and quietly give up the other half of the check, which is
           that the foam has not been slowed until it stops moving at all. The
           foam's own period is 2*pi/period2 -- 3.5 s on beach_1. */
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
     * The foam alpha is shore_height sampled once per band vertex, and
     * shore_height is FUN_0051c690's radial train: 2*pi*length2 is 10.1 m of
     * wavelength on a band whose triangles are 1.8 m, about six samples a
     * wavelength. It used to be 1.24 m and 1.61 m -- below Nyquist, three full
     * cycles a second -- because `period` was read as a time, and the band
     * strobed. That is what "the foam flickers a lot" was, along with the
     * z-fighting above, and the port's answer was a stretch factor of 12 that
     * this check was written to keep honest. The factor is gone; the check is
     * not, because it is what catches the units bug coming back.
     *
     * Both bounds are written against things no constant in water.c can move:
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
            /* What one triangle edge of band SHOULD cross.
             *
             * shore_height is one sine of the radial train, so between two
             * vertices whose phases differ by dphi the alpha step is
             * (range/2) * 2*sin(dphi/2)*cos(...), and averaged over the phase
             * that is (range/2) * 2*sin(dphi/2) * 2/pi. dphi comes from the
             * band's own geometry -- the difference in distance to the wave's
             * centre, over length2 -- so this is the fixture and the config
             * against the rendered band, with nothing from water.c in it.
             *
             * A bound of "less than a sixth of the range" used to sit here. It
             * was calibrated against a stretch factor of 12 that no longer
             * exists, and the honest figure at the engine's own 10.1 m
             * wavelength is a third: 74 of 219. That is a gradient along the
             * shore, not a strobe -- and the way to say so is to check it
             * against the resolved sine's own answer, because an ALIASED band
             * has a different one. Neighbouring vertices on unrelated phases
             * average (range/2)*4/pi, which is 139 here, and no value of dphi
             * produces that unless the wave is under-sampled. */
            float dphi = 0.f;
            int np = 0;
            for (i = 0; i + 1 < row; i++) {
                float ax = coast->rest[i].x - w.cfg->pos_x;
                float az = coast->rest[i].z - w.cfg->pos_z;
                float bx = coast->rest[i + 1].x - w.cfg->pos_x;
                float bz = coast->rest[i + 1].z - w.cfg->pos_z;
                dphi += fabsf(sqrtf(bx * bx + bz * bz) - sqrtf(ax * ax + az * az))
                      / w.cfg->length2;
                np++;
            }
            dphi /= (float)np;
            {
                float want = range * sinf(0.5f * dphi) * (2.f / 3.14159265f);
                float alias = range * (2.f / 3.14159265f);
                ck(nb > 0.f && fabsf(nb - want) < 0.15f * want,
                   "and one band edge crosses exactly what a RESOLVED sine "
                   "crosses there",
                   "%.0f of %.0f, expected %.0f at %.0f deg of phase per edge "
                   "(an aliased band would average %.0f)",
                   nb, range, want, dphi * 57.2958f, alias);
                /* And the reason it is resolved: the band's triangles are well
                   inside Nyquist for the engine's wavelength. At the 1.61 m the
                   port used to read out of length2 they were not -- one edge was
                   longer than the whole wave. Config against fixture. */
                ck(6.2831853f * w.cfg->length2 > 4.f * (40.f / 22.f),
                   "because the wave is four band edges long or more",
                   "%.1f m of wavelength over a %.2f m edge",
                   6.2831853f * w.cfg->length2, 40.f / 22.f);
            }
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
        ck(shallow >= 0 && near((float)shallow / 255.f, w.cfg->alpha_min, 0.02f),
           "transparent at the waterline, so the sand reads through",
           "alpha %d against alphaMin %.0f", shallow, w.cfg->alpha_min * 255.f);
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

    /* --- and so is a POOL, at its OWN alpha, and it does not flow ---------
     *
     * This is the reported bug in three parts. country_1's seven ponds and
     * beach_4's two were not classified at all, so they drew as opaque
     * `sea`-textured plates; and the family's one invented alpha would have
     * given them the WATERFALL's value even once they were.
     *
     * The three assertions are the three fields of the engine's own table entry
     * that differ from the stream's -- see the 0x575710 block in vis_data.h --
     * and the alpha one is deliberately a comparison BETWEEN the two kinds
     * rather than against either constant, which is a check a regression cannot
     * move along with the code it guards.
     */
    {
        int dp = -1, ds = -1;
        float u_rest, u_later;
        gl_cap_reset();
        water_draw(&w, eye);
        for (int d = 0; d < glcap.n_draws; d++) {
            if (glcap.draws[d].mode != GL_TRIANGLES)
                continue;
            if (glcap.draws[d].tex == pool->gl_tex)   dp = d;
            if (glcap.draws[d].tex == stream->gl_tex) ds = d;
        }
        ck(dp >= 0, "a pool draws", "draw %d", dp);
        ck(dp >= 0 && glcap.draws[dp].blend
           && glcap.draws[dp].color[3] < 0.95f,
           "and is blended and see-through -- an opaque one is a teal plate",
           "blend=%d alpha=%.2f", dp >= 0 ? glcap.draws[dp].blend : -1,
           dp >= 0 ? glcap.draws[dp].color[3] : -1.f);
        ck(dp >= 0 && ds >= 0
           && glcap.draws[dp].color[3] < glcap.draws[ds].color[3],
           "at its OWN alpha, thinner than the stream's, not one shared guess",
           "pool %.3f against stream %.3f (110/255 and 120/255)",
           dp >= 0 ? glcap.draws[dp].color[3] : -1.f,
           ds >= 0 ? glcap.draws[ds].color[3] : -1.f);
        /* +0x20 is clear for `pool`: it shimmers in place. The stream on the
           same frames has to be moving, or this passes on a dead animator. */
        u_rest = pool->verts[0].v;
        {
            float s_rest = stream->verts[0].v;
            for (int step = 0; step < 60; step++)
                water_step(&w, 1.f / 60.f);
            water_draw(&w, eye);
            u_later = pool->verts[0].v;
            ck(u_later == u_rest && stream->verts[0].v != s_rest,
               "and it does not scroll, while the stream on the same frames does",
               "pool %.4f -> %.4f, stream moved %.4f",
               u_rest, u_later, stream->verts[0].v - s_rest);
        }
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

    /* --- and none of it is beach_1's -------------------------------------
     *
     * Everything above runs on track 0, so everything above would also pass on
     * a build that had beach_1's numbers compiled in for all ten tracks -- which
     * is the build this replaced. Re-init on another track and check the two
     * things that differ most: the surface's vertical offset (-0.37 m against
     * -0.02 m) and its height. Both come off WSURF[track] and nothing else. */
    {
        int t;
        for (t = 1; t < WSURF_N_TRACKS; t++) {
            const wsurf_t *c = &WSURF[t];
            float worst = 0.f;
            unsigned int j;
            if (c->offset == WSURF[0].offset && c->amp == WSURF[0].amp)
                continue;                      /* nothing to tell apart */
            water_free(&w);
            water_init(&w, s, &bed, t);
            water_step(&w, 0.37f);
            gl_cap_reset();
            water_draw(&w, eye);
            for (j = 0; j < sea->nverts; j++) {
                float k = w.damp[0][j];
                float base = sea->verts[j].y
                           - k * water_height(&w, sea->rest[j].x, sea->rest[j].z);
                float want = sea->rest[j].y + c->offset
                           + (1.f - k) * c->magnet_offset;
                if (fabsf(base - want) > worst) worst = fabsf(base - want);
            }
            /* By NAME, not by pointer: WSURF is a `static const` in a header,
               so water.c's copy and this file's copy are different objects with
               the same contents. Comparing the addresses compares translation
               units, which is not the question. */
            ck(!strcmp(w.cfg->track, c->track) && worst < 1e-4f,
               "the surface reads its OWN track's row, not beach_1's",
               "%s (%s): offset %+.3f m against beach_1's %+.3f, worst %.5f m",
               c->track, c->section, c->offset, WSURF[0].offset, worst);
        }
        /* and the table is indexed the way tracks.h is -- the two lists are
           generated apart and load_track hands this one tracks.h's number */
        {
            int bad = 0;
            for (t = 0; t < WSURF_N_TRACKS && t < N_TRACKS; t++)
                if (strcmp(WSURF[t].track, TRACKS[t].base))
                    bad++;
            ck(WSURF_N_TRACKS == N_TRACKS && !bad,
               "and WSURF[] is in tracks.h's own order",
               "%d rows, %d disagree", WSURF_N_TRACKS, bad);
        }
        water_free(&w);
    }

    /* --- the packed grid and the drawn surface, over the REAL ten ---------
     *
     * The reported bug: "collision should be at the same place as the water,
     * not on the sand". water.c draws every sea vertex at rest + offset;
     * pack_col.py used to bake rest and nothing else, so rb_world.water put the
     * waterline 0.37 m above where it was drawn and the drag and the water sound
     * fired across a strip of dry beach -- 234 m2 of beach_1, 459 m2 of beach_4.
     *
     * Nothing above can see it: the fixture's col_t carries no water layer at
     * all, and every other bound in this part measures the surface's MOTION.
     * This is the two shipped files against each other.
     *
     * The MEDIAN of (grid height - authored height) over the sea's own vertices,
     * because the grid answers per CELL, sampled at the cell's centre, and takes
     * the highest surface covering it -- so an individual vertex can disagree
     * where a tile is not flat or where a coast decal wins its cell. Half the
     * sea's vertices cannot. */
    {
        static float d[65536];
        int t;
        for (t = 0; t < N_TRACKS; t++) {
            char path[64];
            scene_t ts;
            col_t tc;
            const wsurf_t *c = &WSURF[t];
            unsigned int bi, j;
            int n = 0, k;
            float med;

            snprintf(path, sizeof(path), "assets/%s.vsc", TRACKS[t].base);
            if (!scene_load(path, &ts))
                continue;
            snprintf(path, sizeof(path), "assets/%s.col", TRACKS[t].base);
            memset(&tc, 0, sizeof(tc));
            if (!col_load(path, &tc)) { scene_release(&ts); continue; }

            for (bi = 0; bi < ts.n_batches && n < (int)(sizeof d / sizeof *d); bi++) {
                batch_t *b = &ts.batches[bi];
                if (!(b->flags & BATCH_WATER))
                    continue;
                for (j = 0; j < b->nverts && n < (int)(sizeof d / sizeof *d); j++) {
                    float wy;
                    if (!col_water_at(&tc, b->verts[j].x, b->verts[j].z, &wy))
                        continue;
                    d[n++] = wy - b->verts[j].y;
                }
            }
            if (n) {
                for (k = 1; k < n; k++) {       /* insertion sort, n is small */
                    float v = d[k];
                    int q = k - 1;
                    while (q >= 0 && d[q] > v) { d[q + 1] = d[q]; q--; }
                    d[q + 1] = v;
                }
                med = d[n / 2];
                ck(fabsf(med - c->offset) < 0.02f,
                   "the .col water grid stands where the sea is DRAWN",
                   "%s: grid is %+.3f m off the authored surface, and it is "
                   "drawn %+.3f m off it (%d vertices)",
                   TRACKS[t].base, med, c->offset, n);
            }
            col_free(&tc);
            scene_release(&ts);
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
    memset(fx->em.carry_dust, 0, sizeof(fx->em.carry_dust));
    fx->em.carry_gas = 0.f;
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
        fx_set_pipe(&fx.em, pipe);
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
        fx_set_pipe(&fx.em, pipe);
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
                if (fx_pipe_from_rig(&fx.em, &cs.rig, 0, rbcar_com_oy(ci))) {
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
                            float dot = (d[0]*fx.em.pipe_dir[0] + d[1]*fx.em.pipe_dir[1]
                                         + d[2]*fx.em.pipe_dir[2]) / n;
                            if (dot > 1.f) dot = 1.f;
                            if (dot < -1.f) dot = -1.f;
                            ang = acosf(dot) * 57.29578f;
                            back = acosf(-fx.em.pipe_dir[2] > 1.f ? 1.f
                                         : -fx.em.pipe_dir[2]) * 57.29578f;
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

    /* --- and the REAL shared scene carries the HIT banner -----------------
     *
     * `msg_hits` is the game's own on-screen message texture (RCCars.exe loads
     * six of them by name at 0x004af195; this is the fourth) and hud.c binds it
     * by name out of props.vsc. It rides in THIS file rather than in the ten
     * tracks because props.vsc is the app's one load-once scene.
     *
     * This is the link in that chain nothing else can see: ui_test covers hud.c
     * against a stand-in texture id and has no assets, and the packer has no
     * checker for a VSC8 file. So the question here is only "is it really in
     * there, at the size the message slot expects" -- and a missing one is a
     * FAILED check, not a skip: hud.c would silently fall back to the font. */
    {
        scene_t ps;
        if (!scene_load("assets/props.vsc", &ps)) {
            ck(0, "the shared props scene loads (run from rccars_vita/)",
               "assets/props.vsc");
        } else {
            GLuint t = scene_tex(&ps, "msg_hits");
            int lvl0 = -1, k;
            ck(t != 0,
               "props.vsc carries msg_hits -- the banner hud.c binds by name",
               "id %u, %u textures in the scene", (unsigned)t, ps.n_tex);
            for (k = 0; k < n_uploads; k++)
                if (uploads[k].tex == t && uploads[k].level == 0)
                    lvl0 = k;
            /* 256x256 is what the .csi says and what the UV rects at 0x56d328
               split down the middle -- two 256x128 messages, one per half. A
               non-square atlas would make the recovered 2:1 cell aspect a lie. */
            ck(lvl0 >= 0 && uploads[lvl0].w == 256 && uploads[lvl0].h == 256,
               "and it is the 256x256 atlas the two message slots split in half",
               "%dx%d", lvl0 >= 0 ? uploads[lvl0].w : -1,
               lvl0 >= 0 ? uploads[lvl0].h : -1);
            /* With a real ALPHA channel: it is authored ARGB8888, and drawn over
               the world it has to be a cut-out banner rather than a grey box. */
            ck(lvl0 >= 0 && uploads[lvl0].type == GL_UNSIGNED_BYTE,
               "uploaded through the RGBA path, so its alpha survives",
               "type 0x%x", lvl0 >= 0 ? uploads[lvl0].type : 0);

            /* AND THE START LIGHT, which rides in the same file for the same
             * reasons. msg_321_s_f is the FIFTH of the six message textures, an
             * atlas of five messages -- 3, 2, 1, GO! and FINISH -- and
             * countdown.c binds it by name.
             *
             * 512x256 is the load-bearing number here, not just a sanity check:
             * the five recovered UV rects are fractions, so what turns them into
             * cells is the atlas being twice as wide as it is tall. It is what
             * makes slot 5's 0.25 x 0.5 rect a 128 x 128 SQUARE, which is in turn
             * what makes the recovered size pair come out at one uniform 0.8
             * texels to the pixel across all five. Packed at 256x256 and the "3"
             * would be drawn at half its own aspect.
             *
             * A missing one is a FAILED check and not a skip, for the same reason
             * as above: countdown.c would silently fall back to the font, and the
             * start light would be the word "3" in Consolas. */
            {
                GLuint c = scene_tex(&ps, "msg_321_s_f");
                int cl0 = -1;
                ck(c != 0,
                   "props.vsc carries msg_321_s_f -- the 3-2-1-GO countdown",
                   "id %u, %u textures in the scene", (unsigned)c, ps.n_tex);
                ck(c != 0 && c != t,
                   "and it is a DIFFERENT texture from msg_hits",
                   "msg_321_s_f %u, msg_hits %u", (unsigned)c, (unsigned)t);
                for (k = 0; k < n_uploads; k++)
                    if (uploads[k].tex == c && uploads[k].level == 0)
                        cl0 = k;
                ck(cl0 >= 0 && uploads[cl0].w == 512 && uploads[cl0].h == 256,
                   "the 512x256 atlas the five message slots tile",
                   "%dx%d", cl0 >= 0 ? uploads[cl0].w : -1,
                   cl0 >= 0 ? uploads[cl0].h : -1);
                ck(cl0 >= 0 && uploads[cl0].type == GL_UNSIGNED_BYTE,
                   "uploaded through the RGBA path, so its alpha survives too",
                   "type 0x%x", cl0 >= 0 ? uploads[cl0].type : 0);
            }

            /* AND THE WHOLE IN-RACE HUD, which rides in the same file for the
             * same reasons: race_ui.c binds all of it by name out of props.vsc.
             * Same rule about a skip -- every one of these has a font.h fallback,
             * so a packer that quietly dropped them would look like a working
             * build with a plainer HUD and nobody would report it.
             *
             * THE TWO FONT ATLASES ARE THE POINT OF THIS BLOCK. Smash20.csi and
             * Smash26.csi are NOT in RCCars.pack -- they are in
             * Language/English/, which is why this port spent years recording
             * that the game shipped no glyph atlas -- so they only arrive if
             * pack_props.py was given --texroot2. Nothing else in the suite can
             * see that flag, and losing it degrades silently to Consolas. */
            {
                static const char *const HUDTEX[] = {
                    "map_arrow", "map_cp", "place1", "place2", "place3",
                    "place4", "place5", "place6", "lap",
                    "cockpit_sp1", "cockpit_sp2"
                };
                int i, missing = 0, first_missing = -1;
                for (i = 0; i < (int)(sizeof HUDTEX / sizeof HUDTEX[0]); i++)
                    if (!scene_tex(&ps, HUDTEX[i])) {
                        missing++;
                        if (first_missing < 0) first_missing = i;
                    }
                ck(missing == 0,
                   "props.vsc carries the HUD's eleven textures race_ui.c binds",
                   "%d missing, first %s", missing,
                   first_missing >= 0 ? HUDTEX[first_missing] : "-");
                /* The two marker sheets are 64x32 -- TWO 32x32 cells side by
                   side, which is what race_ui.c's `cell` argument splits and
                   what makes "the checkpoint being headed for" expressible. A
                   square one would make that split a lie. */
                {
                    GLuint m = scene_tex(&ps, "map_arrow");
                    int ml = -1;
                    for (k = 0; k < n_uploads; k++)
                        if (uploads[k].tex == m && uploads[k].level == 0)
                            ml = k;
                    ck(ml >= 0 && uploads[ml].w == 2 * uploads[ml].h,
                       "and map_arrow is twice as wide as tall -- its two cells",
                       "%dx%d", ml >= 0 ? uploads[ml].w : -1,
                       ml >= 0 ? uploads[ml].h : -1);
                }
                /* THE ENGINE'S OWN FONT, both sizes, at the size hud_data.h's
                   10 x 9 grid arithmetic assumes. */
                {
                    GLuint fb = scene_tex(&ps, "Smash26");
                    GLuint fs = scene_tex(&ps, "Smash20");
                    int bl = -1, sl = -1;
                    ck(fb != 0 && fs != 0,
                       "and BOTH Smash atlases -- pack_props.py had --texroot2",
                       "Smash26 %u, Smash20 %u", (unsigned)fb, (unsigned)fs);
                    ck(fb != fs, "and they are two different textures",
                       "%u vs %u", (unsigned)fb, (unsigned)fs);
                    for (k = 0; k < n_uploads; k++) {
                        if (uploads[k].tex == fb && uploads[k].level == 0) bl = k;
                        if (uploads[k].tex == fs && uploads[k].level == 0) sl = k;
                    }
                    ck(bl >= 0 && uploads[bl].w == SF_ATLAS
                       && uploads[bl].h == SF_ATLAS,
                       "Smash26 is the square atlas the 10 x 9 grid divides",
                       "%dx%d, SF_ATLAS %d", bl >= 0 ? uploads[bl].w : -1,
                       bl >= 0 ? uploads[bl].h : -1, SF_ATLAS);
                    ck(sl >= 0 && uploads[sl].w == SF_ATLAS
                       && uploads[sl].h == SF_ATLAS,
                       "and so is Smash20",
                       "%dx%d", sl >= 0 ? uploads[sl].w : -1,
                       sl >= 0 ? uploads[sl].h : -1);
                    ck(bl >= 0 && uploads[bl].type == GL_UNSIGNED_BYTE,
                       "through the RGBA path -- a font IS its alpha",
                       "type 0x%x", bl >= 0 ? uploads[bl].type : 0);
                }
            }
            scene_release(&ps);
        }
    }

    /* --- AND EVERY TRACK CARRIES ITS OWN MAP ---------------------------------
     *
     * The one link between the packer and hud_data.h that neither side can see.
     * The engine does NOT number its levels the way this port does -- its order
     * is Scripts/championship.ini's TrackN order, so beach_2's map is
     * `trackmap_6` and beach_4's is `trackmap_10` -- and packing by position in
     * the track list gave EIGHT of the ten another track's painting, with
     * MAP_CALIB's matching wrong transform on top of it. That was reported as
     * "minimap on the second map is not for the second map", and it was also the
     * whole of the 3-to-19 m "residual" this file's notes used to record as
     * unexplained: with the right pairing every track's checkpoints land within
     * 0.7 m of its own painted ribbon.
     *
     * So: for each of the ten, the track's own .vsc must carry exactly the map
     * MAP_TRACKMAP names for it. A missing one is a FAILED check -- race_ui.c
     * would silently draw no minimap. */
    {
        int t, bad = 0, missing = 0;
        for (t = 0; t < MAP_N_TRACKS; t++) {
            char pth[160], nm[32];
            scene_t ts;
            memset(&ts, 0, sizeof ts);
            snprintf(pth, sizeof pth, "assets/%s.vsc", AI_RACES[t].track);
            if (!scene_load(pth, &ts)) { missing++; continue; }
            snprintf(nm, sizeof nm, "trackmap_%d", MAP_TRACKMAP[t]);
            if (!scene_tex(&ts, nm))
                bad++;
            else {
                /* and NOT any other track's, which is what the bug looked like:
                   a scene carrying two maps would pass the check above. */
                int k2;
                for (k2 = 1; k2 <= MAP_N_TRACKS; k2++) {
                    char other[32];
                    if (k2 == MAP_TRACKMAP[t]) continue;
                    snprintf(other, sizeof other, "trackmap_%d", k2);
                    if (scene_tex(&ts, other)) { bad++; break; }
                }
            }
            scene_release(&ts);
        }
        ck(missing == 0, "all ten packed tracks load for the map check",
           "%d missing", missing);
        ck(bad == 0,
           "and each carries EXACTLY the trackmap MAP_TRACKMAP names for it -- "
           "the engine's level number, not this port's",
           "%d wrong", bad);
    }
    {
        {
        }
    }
}

/* ------------------------------------------------------------------ part 13
 * The sun and its lens flare (sun.c).
 *
 * Bound to the three things the subsystem IS, none of which a capture of the
 * vertices alone can see: the two BLEND MODES (the disc alpha-blends and the
 * flare adds -- the art settles which, see sun.h), the LINE OF SIGHT (state 0
 * returns before drawing anything, so a sun behind a building has no flare
 * rather than a faint one), and the GHOST CHAIN STRADDLING the view centre,
 * which is what the loader's two negated Dists are for.
 */
static int g_sun_block;                  /* what the stub world answers */

static int sun_seg_stub(void *u, const float a[3], const float b[3])
{
    (void)u; (void)a; (void)b;
    return g_sun_block;
}

static void part13_sun(void)
{
    static const char *tex[] = { "sun_disc", "lens_flare_1", "lens_flare_2",
                                 "lens_flare_3", "shine" };
    scene_t *s = make_scene(tex, 5);
    scene_t *bare = make_scene(tex, 5);
    sun_t sun;
    float eye[3] = { 0.f, 0.f, 0.f };
    float right[3] = { 1.f, 0.f, 0.f }, up[3] = { 0.f, 1.f, 0.f };
    int i, n_add, n_alpha;

    (void)sun_seg_stub;
    printf("\n-- part 13: the sun and its lens flare --\n");

    /* A track with a SUN_AF marker, and one without -- the second is urban_1 and
       urban_2, whose .sb genuinely has no such node. */
    add_marker(s, "cp_1", 1.f, 0.f, 1.f, 0.f);
    add_marker(s, SUN_MARKER_NAME, 40.f, 30.f, -100.f, 0.f);
    add_marker(bare, "cp_1", 1.f, 0.f, 1.f, 0.f);

    sun_init(&sun, bare);
    ck(!sun.enabled, "a track with no SUN_AF marker has no sun",
       "enabled=%d", sun.enabled);
    gl_cap_reset();
    sun_draw(&sun, eye, right, up, 65.f, 960.f / 544.f, 1.f);
    ck(glcap.n_draws == 0, "and draws nothing at all",
       "%d draws", glcap.n_draws);

    sun_init(&sun, s);
    ck(sun.enabled, "a track with one has a sun", "enabled=%d", sun.enabled);
    ck(sun.pos[1] == 30.f && sun.pos[2] == -100.f,
       "at the marker's own world position, not a direction",
       "(%.1f %.1f %.1f)", sun.pos[0], sun.pos[1], sun.pos[2]);
    ck(sun.state == SUN_OFF,
       "and starts with the flare off -- no cast has happened yet",
       "state=%d", sun.state);

    /* ---- the line of sight ---- */
    g_sun_block = 0;
    sun_step(&sun, NULL, eye, 1.f / 60.f);
    ck(sun.n_casts == 1, "the first step casts immediately",
       "%d casts", sun.n_casts);
    ck(sun.state == SUN_FADE_IN, "a clear sun starts the flare fading IN",
       "state=%d", sun.state);

    /* Full over FadeTime, bound to the recovered constant from both sides
       rather than to a frame count. */
    for (i = 0; i < 200 && sun.state != SUN_ON; i++)
        sun_step(&sun, NULL, eye, 1.f / 60.f);
    {
        float t = (float)i / 60.f;
        ck(t > SUN_FADE_TIME * 0.6f && t < SUN_FADE_TIME * 1.5f,
           "and reaches full in about FadeTime",
           "%.3f s against FadeTime %.3f", t, SUN_FADE_TIME);
    }
    ck(sun.alpha == 1.f, "at full strength", "alpha=%.3f", sun.alpha);

    /* THROTTLED, and that is a cost decision worth holding to: at 60 Hz over a
       second it fires about 1/SUN_RAY_PERIOD times, not 60. */
    sun.n_casts = 0;
    for (i = 0; i < 60; i++)
        sun_step(&sun, NULL, eye, 1.f / 60.f);
    ck(sun.n_casts >= 1 && sun.n_casts <= 4,
       "the line-of-sight cast is throttled, not one per frame",
       "%d casts in a second at period %.2f s", sun.n_casts, SUN_RAY_PERIOD);

    /* ---- the two blend modes ---- */
    gl_cap_reset();
    sun_draw(&sun, eye, right, up, 65.f, 960.f / 544.f, 1.f);
    n_alpha = n_add = 0;
    for (i = 0; i < glcap.n_draws; i++) {
        if (glcap.draws[i].blend_src == GL_SRC_ALPHA
            && glcap.draws[i].blend_dst == GL_ONE_MINUS_SRC_ALPHA)
            n_alpha++;
        if (glcap.draws[i].blend_src == GL_SRC_COLOR
            && glcap.draws[i].blend_dst == GL_ONE)
            n_add++;
    }
    ck(glcap.n_draws == 6, "the disc and the five sprites are all submitted",
       "%d draws", glcap.n_draws);
    ck(n_alpha == 1,
       "the disc ALPHA-blends: flags 0xa0000003 mode 2, and sun_disc keeps its "
       "RGB while its alpha ramps",
       "%d of %d", n_alpha, glcap.n_draws);
    ck(n_add == 5,
       "the five flare sprites ADD: flags 0xa0200005 mode 3 = SRCCOLOR/ONE, and "
       "their alpha is 255 everywhere while the RGB ramps to black",
       "%d of %d", n_add, glcap.n_draws);
    ck(!glcap.draws[0].depth_mask,
       "nothing in the pass writes depth", "disc depth_mask=%d",
       glcap.draws[0].depth_mask);

    /* ---- the chain STRADDLES the view centre ----
     *
     * Dist1 and Dist2 are negated by the loader (0x00479c3a, 0x00479c5d), so two
     * of the four ghosts land on the FAR side of the view centre from the sun.
     * That is the whole reason the negation is there, and nothing else in this
     * file would notice if it were dropped: the chain would still be a chain,
     * just all on one side. The sun is off to the +X side here, so a ghost's
     * signed offset from the screen centre has to change sign across the four.
     */
    {
        int pos_side = 0, neg_side = 0;
        /* Draws 1..4 are the four ghosts, in Dist order. Each is a world-space
           billboard, so the quantity to look at is its LATERAL offset from the
           view axis -- (centre - eye) . right -- and the sun here is off to the
           +right side. Two ghosts must come out on each side of the axis. */
        for (i = 1; i <= 4 && i < glcap.n_draws; i++) {
            int v = glcap.draws[i].first;
            float c0[3], lat;
            int k;
            for (k = 0; k < 3; k++)
                c0[k] = 0.5f * (glcap.pos[v][k] + glcap.pos[v + 2][k]) - eye[k];
            lat = c0[0] * right[0] + c0[1] * right[1] + c0[2] * right[2];
            if (lat > 0.f) pos_side++;
            else if (lat < 0.f) neg_side++;
        }
        ck(pos_side == 2 && neg_side == 2,
           "two ghosts fall either side of the view axis -- the negated "
           "Dist1/Dist2",
           "%d toward the sun, %d away", pos_side, neg_side);
    }

    /* ---- BLOCKED: the flare goes entirely, the disc stays ----
     *
     * This is the check the whole subsystem exists for. `col_segment` reporting a
     * hit has to take the flare all the way to state 0 and stop it being drawn --
     * a flare that merely dimmed would still be a bright wash over a building.
     */
    /* A NULL world answers "clear" by design, so the block is applied to the
       CAST RESULT and the next cast is held off -- what is under test here is the
       state machine and the draw gate, and col_segment has its own coverage in
       colprof and wetcheck. */
    g_sun_block = 1;
    sun.clear = 0;
    sun.ray_timer = 1e9f;
    for (i = 0; i < 400 && sun.state != SUN_OFF; i++)
        sun_step(&sun, NULL, eye, 1.f / 60.f);
    ck(sun.state == SUN_OFF, "a blocked sun drives the flare all the way to OFF",
       "state=%d after %.3f s", sun.state, (float)i / 60.f);
    {
        float t = (float)i / 60.f;
        ck(t > SUN_FADE_TIME * 0.6f && t < SUN_FADE_TIME * 1.5f,
           "and takes about FadeTime to get there, not a frame",
           "%.3f s against FadeTime %.3f", t, SUN_FADE_TIME);
    }
    ck(sun.alpha == 0.f, "with nothing left of the fade", "alpha=%.3f",
       sun.alpha);
    gl_cap_reset();
    sun_draw(&sun, eye, right, up, 65.f, 960.f / 544.f, 1.f);
    ck(glcap.n_draws == 1,
       "with the flare off ONLY the disc is drawn -- state 0 returns before the "
       "ghosts", "%d draws", glcap.n_draws);
    ck(glcap.draws[0].blend_src == GL_SRC_ALPHA,
       "and it is still the alpha-blended one", "src=%#x",
       glcap.draws[0].blend_src);

    /* The disc grows with distance so that it subtends a FIXED screen angle --
       the port's substitute for a screen-space quad, and wrong in either
       direction if the derivation slips. Twice as far must be twice as wide. */
    {
        float w1, w2;
        gl_cap_reset();
        sun_draw(&sun, eye, right, up, 65.f, 960.f / 544.f, 1.f);
        w1 = glcap.pos[glcap.draws[0].first + 1][0]
             - glcap.pos[glcap.draws[0].first][0];
        sun.pos[0] *= 2.f; sun.pos[1] *= 2.f; sun.pos[2] *= 2.f;
        gl_cap_reset();
        sun_draw(&sun, eye, right, up, 65.f, 960.f / 544.f, 1.f);
        w2 = glcap.pos[glcap.draws[0].first + 1][0]
             - glcap.pos[glcap.draws[0].first][0];
        ck(w1 > 1e-4f && w2 > 1.9f * w1 && w2 < 2.1f * w1,
           "the disc subtends a fixed ANGLE: twice as far, twice as wide",
           "%.4f m then %.4f m", w1, w2);
    }

    free(s->tex_ids); free(s->tex_names); free(s->markers); free(s);
    free(bare->tex_ids); free(bare->tex_names); free(bare->markers); free(bare);
}

/* Empty the shared pool without touching any emitter's carry. */
static void clear_pool(fx_t *fx)
{
    int i;
    for (i = 0; i < FX_MAX_PARTICLES; i++)
        fx->p[i].used = 0;
    fx->n_live = 0;
}

/* ============================================================= part 14 ==== */
/*
 * THE OPPONENTS' OWN DUST AND SMOKE.
 *
 * Reported as AI cars having neither, plus a shell that does not read as
 * plastic. Three separate gaps, and none of them was in the AI model or in the
 * assets -- car<n>.vsc is ONE file that the player and every opponent load, so
 * the meshes, the env classes, the packed normals and the `dust` sprite were all
 * there the whole time. What was missing was three calls and two fields.
 *
 * This part owns the two that can be read back without a renderer:
 *
 *   - fx.c's SHARED POOL. One pool, N emitters, which is what makes a field of
 *     four cars affordable at all. The thing that can go wrong is the ageing:
 *     fx_step is fx_emit + fx_age, so a caller that runs the whole of it per car
 *     ages every particle once per car.
 *   - ai.c's CONTACT POINTS and THROTTLE, the two fields fx reads off a car and
 *     the replay does not record. Both are exercised against a REAL recorded lap
 *     on a REAL grid, because a hand-built ai_car would be asserting that
 *     ai_fake_contacts agrees with a copy of ai_fake_contacts.
 *
 * The glance is NOT here: envmap_draw is already covered by part 8, it is
 * stateless with respect to which car it is handed, and what was wrong was a
 * missing call in main.c -- which nothing compiles. That is said at the call
 * site rather than pretended about here.
 */
static void part14_aifx(void)
{
    static const char *tex[] = {"dust"};
    scene_t *s = make_scene(tex, 1);
    static const float eye[3] = {0.f, 0.f, 0.f};
    col_t plane;
    fx_t fx;
    fx_emitter emA, emB;
    rb_car c;
    int i, n1, n2, nA;

    printf("\npart 14: the opponents' dust, smoke and contact points\n");

    make_plane(&plane, 8.f, 8, 0.f);
    plane_material(&plane, SURF_SAND, SURF_SAND);
    plane_eng_surface(&plane, 1);          /* engine class 1 = sand, dust rises */
    fx_init(&fx, s);
    clear_pool(&fx);

    /* ---- the shared pool ages exactly once, however many cars emit ---- */
    fake_car(&c, 6.f, 0.f);
    c.in.accel = 1;
    fx_emitter_init(&emA);
    fx_emitter_init(&emB);

    /* One emitter, one frame: every particle is exactly one dt old. */
    clear_pool(&fx);
    fx_emit(&fx, &emA, &c, &plane, eye, 1.f / 60.f);
    fx_age(&fx, 1.f / 60.f);
    {
        float amax = 0.f;
        int live = 0;
        for (i = 0; i < FX_MAX_PARTICLES; i++)
            if (fx.p[i].used) { live++; if (fx.p[i].age > amax) amax = fx.p[i].age; }
        n1 = live;
        ck(live > 0 && fabsf(amax - 1.f / 60.f) < 1e-6f,
           "one emitter, one fx_age: a particle is one dt old",
           "%d live, oldest %.5f s against dt %.5f", live, amax, 1.f / 60.f);
    }

    /* THREE emitters and still ONE fx_age -- the field's shape. The oldest
       particle must STILL be one dt old. Run the whole fx_step per car instead
       and this reads 3 dt, which at a wetsand life of 0.05 s is most of a
       particle's life burnt before it has moved. */
    clear_pool(&fx);
    fx_emitter_init(&emA);
    fx_emitter_init(&emB);
    {
        fx_emitter emC;
        float amax = 0.f;
        int live = 0;
        fx_emitter_init(&emC);
        fx_emit(&fx, &emA, &c, &plane, eye, 1.f / 60.f);
        fx_emit(&fx, &emB, &c, &plane, eye, 1.f / 60.f);
        fx_emit(&fx, &emC, &c, &plane, eye, 1.f / 60.f);
        fx_age(&fx, 1.f / 60.f);
        for (i = 0; i < FX_MAX_PARTICLES; i++)
            if (fx.p[i].used) { live++; if (fx.p[i].age > amax) amax = fx.p[i].age; }
        n2 = live;
        ck(fabsf(amax - 1.f / 60.f) < 1e-6f,
           "three emitters, one fx_age: still one dt old",
           "oldest %.5f s, %d live", amax, live);
        /* And they really are three cars' worth, not one car counted thrice:
           the pool is shared but the carries are not. Bounded as a RATIO so it
           cannot be satisfied by moving an emission rate. */
        ck(live >= 3 * n1 - 3 && live <= 3 * n1 + 3,
           "and three emitters spawn three emitters' worth",
           "%d against 3 x %d", live, n1);
    }

    /* ---- and the carries are PER CAR ------------------------------------
     *
     * A shared pool is fine; a shared CARRY is not, and the difference is
     * invisible in a total. FUN_00530b70 keeps the fractional part of rate*dt
     * per system object, so two cars emitting at different rates each keep their
     * own remainder. Point them both at one carry and the totals still come out
     * right -- which is why the three-emitter check above cannot see it -- while
     * the SPLIT collapses: at 0.9 and 0.1 particles per frame the second call
     * finds the first call's 0.9 already banked, takes the whole particle every
     * frame, and the car that was raising nine tenths of the dust raises none.
     *
     * Attributed by POSITION, because a particle does not record which emitter
     * made it and the two cars are 20 m apart. The eye sits midway so both are
     * inside the 12 m emit radius and neither is culled -- the thing under test
     * is the carry, not the cull.
     */
    {
        rb_car cA, cB;
        fx_emitter eA, eB;
        col_t wide;
        float mid[3];
        float rA, rB;
        int nearA = 0, nearB = 0, f;

        /* ITS OWN GRID, wide enough to hold both cars. The shared 8 m plane
           above does not reach the second car, and a car standing off the grid
           gets surface class 0 -- whose IntScale is 0, so it raises nothing and
           this check fails for a reason that has nothing to do with carries.
           It did exactly that first time round. */
        make_plane(&wide, 40.f, 8, 0.f);
        plane_material(&wide, SURF_SAND, SURF_SAND);
        plane_eng_surface(&wide, 1);

        /* BOTH RATES BELOW ONE PARTICLE PER FRAME, which is the regime the
           carry exists for -- fx.c's own note says dropping it "loses every rate
           below 1/dt". Measured on this curve, 2.0 m/s asks for 0.105 particles
           a frame and 4.0 m/s for 0.652. Pick a car above 1/frame and the whole
           particles arrive on their own, the remainder barely matters, and a
           shared carry survives the check -- which is what the first version of
           this did at 2.5 and 8.0 m/s. */
        fake_car(&cA, 2.0f, 0.f);
        fake_car(&cB, 4.0f, 0.f);
        cA.in.accel = cB.in.accel = 1;
        /* Move B 20 m along +X, body and contacts together. */
        cB.m[12] += 20.f;
        for (i = 0; i < cB.nwheels; i++)
            cB.hit[i].point[0] += 20.f;
        mid[0] = 10.f; mid[1] = 0.f; mid[2] = 0.f;

        rA = fx_dust_rate(&fx, &cA, 2, 1, 2.0f * 3.6f);
        rB = fx_dust_rate(&fx, &cB, 2, 1, 4.0f * 3.6f);

        fx_emitter_init(&eA);
        fx_emitter_init(&eB);
        clear_pool(&fx);
        for (f = 0; f < 120; f++) {
            fx_emit(&fx, &eA, &cA, &wide, mid, 1.f / 60.f);
            fx_emit(&fx, &eB, &cB, &wide, mid, 1.f / 60.f);
            fx_age(&fx, 1.f / 60.f);
            for (i = 0; i < FX_MAX_PARTICLES; i++) {
                if (!fx.p[i].used || fx.p[i].sys != FX_SYS_DUST)
                    continue;
                if (fx.p[i].age >= 1.5f / 60.f)
                    continue;                    /* newly born only */
                if (fx.p[i].x < 10.f) nearA++; else nearB++;
            }
        }
        /* Both must be raising dust, and in the ratio their own rates ask for.
           Bounded as a RATIO against the rates, so it cannot be met by moving an
           emission rate -- and both ends matter: a shared carry starves one car
           and feeds the other, in whichever order the caller happens to run. */
        ck(nearA > 0 && nearB > 0,
           "two cars at different rates BOTH raise dust",
           "%d and %d puffs at %.1f and %.1f /s", nearA, nearB, rA, rB);
        ck(nearA > 0 && nearB > 0 && rA > 0.f && rB > 0.f
           && fabsf((float)nearB / (float)nearA - rB / rA) < 0.25f * (rB / rA),
           "and in the ratio their own emission rates ask for",
           "%.2f measured against %.2f expected",
           nearA ? (float)nearB / (float)nearA : 0.f, rB / rA);
    }

    /* ---- one car's backfire must not darken another car's smoke ---- */
    clear_pool(&fx);
    fx_emitter_init(&emA);
    fx_emitter_init(&emB);
    {
        float lumA = 0.f, lumB = 0.f;
        int na = 0, nb = 0;
        /* A backfires: the rising edge of Jump above 10 km/h. */
        c.in.jump = 1;
        emA.prev_jump = 0;
        fx_emit(&fx, &emA, &c, &plane, eye, 1.f / 60.f);
        for (i = 0; i < FX_MAX_PARTICLES; i++)
            if (fx.p[i].used && fx.p[i].sys == FX_SYS_GAS) {
                lumA += fx.p[i].r; na++;
            }
        clear_pool(&fx);
        /* B does not: same car state, but B has not seen the edge, so its own
           explode_t is still zero. */
        c.in.jump = 0;
        fx_emit(&fx, &emB, &c, &plane, eye, 1.f / 60.f);
        for (i = 0; i < FX_MAX_PARTICLES; i++)
            if (fx.p[i].used && fx.p[i].sys == FX_SYS_GAS) {
                lumB += fx.p[i].b; nb++;
            }
        if (na) lumA /= na;
        if (nb) lumB /= nb;
        ck(na > 0 && nb > 0 && lumA < lumB - 1.f,
           "a backfire darkens ITS OWN smoke and no one else's",
           "backfiring %.0f/255, neighbour %.0f/255", lumA, lumB);
        c.in.jump = 0;
    }

    /* ---- and now a REAL opponent on a REAL grid ---- */
    {
        ai_t ai;
        ai_track tr;
        col_t rc;
        char cp[128];
        int have;

        memset(&ai, 0, sizeof(ai));
        memset(&tr, 0, sizeof(tr));
        memset(&rc, 0, sizeof(rc));
        snprintf(cp, sizeof(cp), "assets/%s.col", AI_RACES[0].track);
        have = col_load(cp, &rc);
        if (!have || ai_init(&ai, 0, "assets", col_rb_world(&rc), 1, 0) < 1) {
            ck(0, "beach_1 grid and roster loaded", "col=%d", have);
        } else {
            ai_car *a = &ai.car[0];
            float far_[3] = { 1e6f, 0.f, 1e6f };
            int t, worst_far = 0;
            float maxd = 0.f, span_x = 0.f, span_z = 0.f;
            int origin_hits = 0, ever_wet = 0;
            int dry_wet = 0;
            int gnd_n = 0, gnd_bad = 0;
            double gnd_sum = 0.0;
            int throttle_on = 0, throttle_off = 0, ticks = 0;
            int dust = 0, gas = 0, dust_far = 0, dust_born = 0;
            double dv_on = 0.0, dv_off = 0.0;
            float prev_speed = -1.f;

            fx_emitter_init(&emA);
            clear_pool(&fx);

            for (t = 0; t < 60 * 30; t++) {
                float lo[3], hi[3];
                int w;
                ai_step(&ai, &tr, far_[0], far_[1], far_[2], 0, 1.f / 60.f);
                ticks++;
                /* Accumulate the speed CHANGE on throttle-on ticks against
                   throttle-off ticks. This is the semantic the bit is supposed
                   to carry, and unlike a count it cannot be satisfied by a
                   constant: a hardcoded 1 leaves dv_off at zero, a hardcoded 0
                   leaves dv_on at zero, and either fails the sign test below. */
                if (prev_speed >= 0.f) {
                    double dv = (double)a->speed - prev_speed;
                    if (a->rb.in.accel) dv_on += dv; else dv_off += dv;
                }
                prev_speed = a->speed;
                if (a->rb.in.accel) throttle_on++; else throttle_off++;

                lo[0] = lo[2] = 1e30f; hi[0] = hi[2] = -1e30f;
                for (w = 0; w < a->rb.nwheels; w++) {
                    const float *p = a->rb.hit[w].point;
                    float dx = p[0] - a->rb.body.x[0];
                    float dy = p[1] - a->rb.body.x[1];
                    float dz = p[2] - a->rb.body.x[2];
                    float d = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (d > maxd) maxd = d;
                    if (p[0] == 0.f && p[1] == 0.f && p[2] == 0.f)
                        origin_hits++;
                    if (a->rb.hit[w].in_water) ever_wet++;
                    /* THE PATCH IS AT GROUND LEVEL, checked against the grid
                       itself rather than against the arithmetic that produced
                       it. col_ground_at is an independent oracle: it is the
                       downward query, it knows nothing about wheels, and it says
                       where the floor under this point is. A patch taken at the
                       WHEEL CENTRE instead of one radius below it sits 72 mm up
                       -- a whole wheel radius -- and only this can see that; the
                       distance and span bounds above are metres wide and let it
                       through. Loaded wheels only: a hanging wheel is not on the
                       ground and has no business matching it. */
                    if (a->rb.hit[w].active) {
                        float gy, gnx, gny, gnz;
                        if (col_ground_at(&rc, p[0], p[2], p[1] + 0.30f,
                                          &gy, &gnx, &gny, &gnz)) {
                            double d = fabs((double)p[1] - gy);
                            gnd_sum += d;
                            gnd_n++;
                            if (d > 0.05) gnd_bad++;
                        }
                    }
                    if (p[0] < lo[0]) lo[0] = p[0];
                    if (p[0] > hi[0]) hi[0] = p[0];
                    if (p[2] < lo[2]) lo[2] = p[2];
                    if (p[2] > hi[2]) hi[2] = p[2];
                }
                if (hi[0] - lo[0] > span_x) span_x = hi[0] - lo[0];
                if (hi[2] - lo[2] > span_z) span_z = hi[2] - lo[2];

                /* Emit from where the opponent IS, so the 12 m cull cannot be
                   what decides this. */
                fx_emit(&fx, &emA, &a->rb, &rc, a->rb.body.x, 1.f / 60.f);
                fx_age(&fx, 1.f / 60.f);

                /* SAMPLED EVERY TICK, in particle-ticks, and not once at the
                   end. The exhaust's recovered EG_LIFE is 0.06 s -- 3.6 frames
                   -- so at any single instant the pool holds a handful of smoke
                   or none at all, and reading it once turns "does this car make
                   smoke" into a coin flip. It duly came up tails. */
                for (i = 0; i < FX_MAX_PARTICLES; i++) {
                    if (!fx.p[i].used)
                        continue;
                    if (fx.p[i].sys == FX_SYS_DUST) {
                        /* WHERE it was RAISED, not merely that it exists. With
                           hit[].point left at the memset's zero the dust spawns
                           at the world ORIGIN and beach_1's racing line is tens
                           of metres from it -- so this is the check that fails
                           on the bug, rather than a floor any nonzero count
                           meets.
                
                           MEASURED AT BIRTH, which is why the age test is here.
                           A dust particle lives up to 1.25 s and this car covers
                           about 8 m in that time, so an older puff is legitimately
                           metres behind and a distance bound over the whole pool
                           measures the CAR'S MOTION instead of the spawn point.
                           After fx_age a particle born this tick is exactly one
                           dt old; half a dt of slack keeps that a comparison and
                           not a float equality. 1 m bounds it to the car and its
                           own first frame of travel. */
                        if (fx.p[i].age < 1.5f / 60.f) {
                            float dx = fx.p[i].x - a->rb.body.x[0];
                            float dz = fx.p[i].z - a->rb.body.x[2];
                            if (dx * dx + dz * dz > 1.f)
                                dust_far++;
                            dust_born++;
                        }
                        dust++;
                    } else {
                        gas++;
                    }
                }
            }
            (void)worst_far;

            /* THE BUG, stated as what it did: hit[].point was left at the
               memset's zero, so every opponent raised its dust at the world
               origin -- hundreds of metres from the car, on beach_1's own
               coordinates. Bound to the CAR's size, not to anything ai.c
               computes. */
            ck(origin_hits == 0,
               "an opponent's contact points are not at the world origin",
               "%d of %d samples at (0,0,0)", origin_hits, ticks * a->rb.nwheels);
            ck(maxd > 0.02f && maxd < 0.5f,
               "they sit on the car, within half a car length of its centre",
               "worst %.3f m over %d ticks", maxd, ticks);
            /* And they are a RECTANGLE the size of this car's own track and
               wheelbase -- numbers from the mesh (rb_data.h), not from ai.c.
               The Overkill is 0.282 m across the wheels and 0.298 m along. */
            ck(span_x > 0.10f && span_x < 0.60f && span_z > 0.10f && span_z < 0.60f,
               "and they span the car's own track and wheelbase",
               "%.3f x %.3f m", span_x, span_z);

            /* The throttle. Both states must occur -- all-on means the
               derivation is a constant, all-off means the exhaust is still
               dead -- AND the off ticks must be the ones where the car is
               slowing down. The second clause is the real one: it is what says
               the bit MEANS something, and it is what caught AI_THROTTLE_COAST
               being set three times larger than the acceleration limit, at
               which the bit was on for 1796 of 1800 ticks and this file was
               happily reporting "neither always on nor always off". */
            ck(throttle_on > 0 && throttle_off > 0,
               "the derived throttle is neither always on nor always off",
               "%d on / %d off of %d ticks", throttle_on, throttle_off, ticks);
            ck(throttle_off > ticks / 100 && throttle_off < ticks / 2,
               "and it is off for a corner's worth of the lap, not a rounding",
               "%.1f%% of %d ticks", 100.0 * throttle_off / ticks, ticks);
            ck(dv_off < 0.0 && dv_on > 0.0,
               "throttle OFF is where the car slows and ON where it gains",
               "off %+.2f m/s total, on %+.2f", dv_off, dv_on);

            /* What the whole thing is FOR. */
            /* THE MEAN, not every sample: the outliers are kerbs, jumps and
               the moments col_ground_at's downward ray picks a different face
               from the one the wheel is on. What the mean measures is the known
               BODY-FRAME RESIDUAL -- pack_ai.py lifts every recorded sample by a
               per-car constant because the engine's wheel mounts are not the
               port's, and CLAUDE.md records that residual as 27.8 mm on the
               Overkill. This reads 23.8 mm, which is that number and not a
               coincidence.
               50 mm is the bound because it sits between that and the failure it
               has to catch: taking the patch at the WHEEL CENTRE instead of one
               radius below moves it 71.8 mm (CdtRadWheel), and the same mean
               then reads 81.8 mm with 6007 of 6858 samples beyond 50 mm rather
               than 534. The bound has better than 2x clearance on one side and
               1.6x on the other. */
            ck(gnd_n > 0 && gnd_sum / gnd_n < 0.05,
               "a loaded wheel's patch is ON the grid's own surface",
               "mean %.1f mm off over %d samples (%d beyond 50 mm)",
               gnd_n ? 1000.0 * gnd_sum / gnd_n : 0.0, gnd_n, gnd_bad);

            ck(dust > 0, "a real opponent on a real lap raises DUST",
               "%d dust particle-ticks over %d ticks", dust, ticks);
            ck(dust_born > 0 && dust_far == 0,
               "and it raises it AT THE CAR, not at the world origin",
               "%d of %d newly-born further than 1 m away", dust_far, dust_born);
            ck(gas > 0, "and its exhaust makes SMOKE",
               "%d smoke particle-ticks over %d ticks", gas, ticks);

            /* THE WATER GATE. fx skips a wet wheel -- FUN_00531b10 raises spray
               there, not dust -- so an opponent fording beach_1's river must
               report wet wheels or that branch is dead code on this path. Bound
               at BOTH ends, and the second end is the one that means something:
               CLAUDE.md records that six of the ten tracks have water and that
               country_2, country_4, urban_1 and urban_2 have none. That is a
               fact about the SHIPPED GRIDS, established long before this code,
               so a dry track reporting a wet wheel is this code being wrong and
               not the check being circular. */
            /* BOTH ENDS, and the upper one is the point. The flag alone is
               satisfied by a gate pointing the wrong way -- CLAUDE.md records
               exactly that for col.c, where a flipped gap sign survived a suite
               that only asserted in_water. Measured here: the gate as written
               reports 3.8% of beach_1's wheel-samples wet, and the same gate
               with its comparison reversed reports 27.2%, because it then means
               "there is water somewhere in this column and the wheel is well
               clear of it" -- which is true of every pier and every bank. A
               recorded racing line crosses water at a ford, not for a quarter of
               the lap. 15% sits an order of magnitude clear of one and well
               clear of the other. */
            ck(ever_wet > 0
               && ever_wet < ticks * a->rb.nwheels * 15 / 100,
               "an opponent fording beach_1 reports WET wheels, and only there",
               "%d of %d wheel-samples (%.1f%%)", ever_wet,
               ticks * a->rb.nwheels,
               100.0 * ever_wet / (ticks * a->rb.nwheels));
            {
                ai_t dry;
                col_t dc;
                char dp[128];
                int dt2, dn = 0;
                memset(&dry, 0, sizeof(dry));
                memset(&dc, 0, sizeof(dc));
                /* urban_2, which CLAUDE.md lists among the four tracks with no
                   water at all. AI_RACES is indexed in the port's track order,
                   not the championship's, so urban_2 is 9 -- index 3 is
                   beach_4, which has water and duly reported some. */
                snprintf(dp, sizeof(dp), "assets/%s.col", AI_RACES[9].track);
                if (col_load(dp, &dc)
                    && ai_init(&dry, 9, "assets", col_rb_world(&dc), 1, 0) >= 1) {
                    for (dt2 = 0; dt2 < 60 * 20; dt2++) {
                        int k, w;
                        ai_step(&dry, &tr, far_[0], far_[1], far_[2], 0,
                                1.f / 60.f);
                        for (k = 0; k < dry.n; k++)
                            for (w = 0; w < dry.car[k].rb.nwheels; w++) {
                                dn++;
                                if (dry.car[k].rb.hit[w].in_water)
                                    dry_wet++;
                            }
                    }
                    ck(dn > 0 && dry_wet == 0,
                       "and NO wheel is wet on a track with no water in it",
                       "%s: %d of %d wheel-samples", AI_RACES[9].track,
                       dry_wet, dn);
                    ai_free(&dry);
                }
                col_free(&dc);
            }

            /* --- THE PROGRESS QUERY IS MONOTONIC ALONG A REAL DRIVEN LAP ------
             *
             * The placing and the rubber band both compare
             * `spine_len * lap + cp_spine_dist(x, z)`, which is the engine's own
             * quantity (FUN_004ea7b0 -> FUN_004eb630). So a place that flickers
             * is a flickering INPUT, and the input was the projection: the
             * UNWINDOWED query searches the whole spine for the nearest point and
             * flips between arc positions hundreds of metres apart wherever a
             * track passes near itself.
             *
             * Measured before the fix, over every sample of all 30 shipped
             * recordings: 3 to 16 backward jumps per lap on every one of the ten
             * tracks, the worst 425.8 m of a 460 m lap -- which dwarfs any real
             * gap, so the player's place snapped to first or last and back for as
             * long as the flip lasted. It is the reported bug.
             *
             * This drives REAL laps rather than a synthetic path, because the
             * whole failure is about where a real track doubles back on itself.
             * The seam wrap is not a jump and is subtracted out. */
            {
                int t2, tot_back = 0, tot_big = 0, laps = 0;
                float worst_back = 0.f;
                for (t2 = 0; t2 < 10; t2++) {
                    char pth[160];
                    scene_t s2;
                    col_t c2;
                    checkpoints_t k2;
                    ai_t a2;
                    int q;

                    snprintf(pth, sizeof pth, "assets/%s.vsc", AI_RACES[t2].track);
                    memset(&s2, 0, sizeof s2);
                    if (!scene_load(pth, &s2))
                        continue;
                    snprintf(pth, sizeof pth, "assets/%s.col", AI_RACES[t2].track);
                    memset(&c2, 0, sizeof c2);
                    col_load(pth, &c2);
                    cp_init(&k2, &s2, &c2);
                    memset(&a2, 0, sizeof a2);
                    if (ai_init(&a2, t2, "assets", col_rb_world(&c2), 1, 0)) {
                        for (q = 0; q < a2.n; q++) {
                            const ai_car *ac = &a2.car[q];
                            float prev = -1e9f, hint = -1.f;
                            int si;
                            laps++;
                            for (si = 0; si < ac->n; si++) {
                                float dd, d2 = 0.f;
                                if (!cp_spine_dist_near(&k2, ac->s[si].p[0],
                                                        ac->s[si].p[1],
                                                        ac->s[si].p[2], hint,
                                                        &d2, NULL))
                                    continue;
                                hint = d2;
                                if (prev > -1e8f) {
                                    dd = d2 - prev;
                                    if (dd < -k2.spine_len * 0.5f)
                                        dd += k2.spine_len;   /* the seam */
                                    if (dd < 0.f) {
                                        tot_back++;
                                        if (-dd > worst_back) worst_back = -dd;
                                        if (-dd > 2.f) tot_big++;
                                    }
                                }
                                prev = d2;
                            }
                        }
                        ai_free(&a2);
                    }
                    scene_release(&s2);
                    col_free(&c2);
                }
                ck(laps == 30,
                   "all 30 shipped recordings drive the progress query",
                   "%d of 30", laps);
                /* A KNOWN-DEFECT CHECK, and deliberately the wrong way round:
                   the projection DOES jump, that is why neither side of the
                   placing uses it, and if it ever stops jumping this line is the
                   one that says to go and reconsider that decision. The
                   checkpoint polyline is not the road (traps.md) and its refining
                   points run up to 63 m off it, so where a track passes near
                   itself the nearest segment is genuinely ambiguous. */
                ck(tot_big > 0 && worst_back > 100.f,
                   "the raw projection still jumps -- which is WHY the placing "
                   "does not use it; if this goes green, revisit ai.c",
                   "%d backward steps, worst %.1f m, %d over 2 m",
                   tot_back, worst_back, tot_big);
            }

            /* AND THE SAME THING THROUGH ai_step, which is what the game runs.
             *
             * The check above calls cp_spine_dist_near with a hint it keeps
             * itself, so it holds checkpoint.c and says nothing about ai.c's
             * bookkeeping -- and two mutants proved it: dropping either
             * `ai->player_at = pdist` or `a->spine_at = adist` leaves the hint at
             * -1 for ever, every query goes back to searching the whole spine, and
             * the flicker returns with every check above still green.
             *
             * So this drives a real recorded lap through ai_step and watches
             * ai_car.spine_dist, which is the CUMULATIVE quantity the placing and
             * the rubber band actually compare. Cumulative, so it may not go
             * backwards at all -- not even across the seam. */
            {
                scene_t s3;
                col_t c3;
                checkpoints_t k3;
                ai_t a3;
                ai_track t3;
                int back3 = 0, steps = 0, q, player_back = -1;
                float worst3 = 0.f;
                float prev3[AI_MAX_OPPONENTS];

                memset(&s3, 0, sizeof s3);
                memset(&c3, 0, sizeof c3);
                memset(&a3, 0, sizeof a3);
                if (scene_load("assets/country_1.vsc", &s3)) {
                    col_load("assets/country_1.col", &c3);
                    cp_init(&k3, &s3, &c3);
                    /* country_1: 13 to 16 backward jumps a lap before the fix, the
                       worst of the ten. */
                    if (ai_init(&a3, 4, "assets", col_rb_world(&c3), 1, 0)) {
                        t3.ctx = &k3;
                        t3.spine = vt_spine;
                        t3.spine_len = k3.spine_len;
                        for (q = 0; q < AI_MAX_OPPONENTS; q++)
                            prev3[q] = -1e9f;
                        /* THE PLAYER DRIVES TOO, along the first recording -- it
                           has its own hint (ai_t.player_at) and its own line in
                           ai_step, and parking it on the grid left that half
                           untested: a mutant that dropped `ai->player_at = pdist`
                           survived everything. Watched separately below. */
                        float pprev = -1e9f;
                        int pback = 0;
                        const ai_car *pc = &a3.car[0];
                        for (steps = 0; steps < 6000; steps++) {
                            const int si = steps % (pc->n > 0 ? pc->n : 1);
                            ai_step(&a3, &t3, pc->s[si].p[0], pc->s[si].p[1],
                                    pc->s[si].p[2], 0, 1.f / 60.f);
                            for (q = 0; q < a3.n; q++) {
                                float d3 = a3.car[q].spine_dist;
                                if (prev3[q] > -1e8f && d3 < prev3[q]) {
                                    back3++;
                                    if (prev3[q] - d3 > worst3)
                                        worst3 = prev3[q] - d3;
                                }
                                prev3[q] = d3;
                            }
                            /* THE RESULT, not the hint. Watching ai_t.player_at
                               looks right and is worthless: a mutant that never
                               writes it leaves it at -1 for ever, so it never
                               moves and never goes backwards. player_dist is what
                               the placing compares, and with player_lap 0 it IS
                               the within-lap distance -- so it wraps at the seam
                               once a lap, which is not a jump. */
                            if (pprev > -1e8f) {
                                float dd = a3.player_dist - pprev;
                                if (dd < -k3.spine_len * 0.5f)
                                    dd += k3.spine_len;
                                if (dd < -0.01f) {
                                    pback++;
                                    if (-dd > worst3) worst3 = -dd;
                                }
                            }
                            pprev = a3.player_dist;
                        }
                        player_back = pback;
                        ai_free(&a3);
                    }
                    scene_release(&s3);
                    col_free(&c3);
                }
                ck(steps == 6000, "ai_step drives 100 s of a real recorded lap",
                   "%d steps", steps);
                ck(back3 == 0,
                   "and the OPPONENTS' cumulative progress never goes backwards -- "
                   "ai_car.spine_at's bookkeeping, not checkpoint.c's",
                   "%d backward steps, worst %.1f m", back3, worst3);
                ck(player_back == 0,
                   "and neither does the PLAYER's -- ai_t.player_at, the other "
                   "half, which a parked player never exercised",
                   "%d backward steps", player_back);
            }

            /* --- THE PLACING'S TWO INPUTS, on every real track -----------------
             *
             * MONOTONIC *AND* MOVING. The second half is the one that matters and
             * it caught three wrong fixes in a row: a progress measure that
             * FREEZES is perfectly monotonic and completely useless, and every
             * "0 backward steps" result was vacuous until forward travel was
             * measured beside it. See traps.md.
             *
             * The two sides are measured differently on purpose, because
             * projecting a car onto the checkpoint polyline is not sound on these
             * tracks -- it jumps more than 10 m between consecutive samples 4 to
             * 16 times a lap on every one of the ten:
             *   - an OPPONENT's progress is its own recorded path length walked,
             *     which is exact and monotonic by construction
             *   - the PLAYER's is anchored on the LATCHED checkpoint index, which
             *     cannot flicker
             * The player is driven along the first opponent's own recording, so
             * both cover the same ground. */
            {
                int t2, oback = 0, pback = 0, thin = 0, done = 0;
                float pworst = 0.f;
                for (t2 = 0; t2 < 10; t2++) {
                    char pth[160];
                    scene_t s4; col_t c4; checkpoints_t k4; ai_t a4; ai_track t4;
                    int i2, q;
                    float oprev[AI_MAX_OPPONENTS], pprev = -1e9f;
                    float ofwd = 0.f, pfwd = 0.f;
                    const ai_car *pc;

                    snprintf(pth, sizeof pth, "assets/%s.vsc", AI_RACES[t2].track);
                    memset(&s4, 0, sizeof s4);
                    if (!scene_load(pth, &s4)) continue;
                    snprintf(pth, sizeof pth, "assets/%s.col", AI_RACES[t2].track);
                    memset(&c4, 0, sizeof c4);
                    col_load(pth, &c4);
                    cp_init(&k4, &s4, &c4);
                    /* cp_init only enables the progression when the arrow
                       TEXTURES loaded, and this recorder has none. The real app
                       has them; without this cp_step does nothing and the player
                       looks frozen for a reason that is the fixture's. */
                    k4.enabled = 1;
                    memset(&a4, 0, sizeof a4);
                    if (ai_init(&a4, t2, "assets", col_rb_world(&c4), 1, 0)) {
                        t4.ctx = &k4; t4.spine = vt_spine;
                        t4.lap_progress = vt_progress;
                        t4.spine_len = k4.spine_len;
                        for (q = 0; q < AI_MAX_OPPONENTS; q++) oprev[q] = -1e9f;
                        pc = &a4.car[0];
                        /* 12000 ticks -- 200 s. The slowest track's player-side
                           measure needs that to clear one full lap, and "one full
                           lap" is the bound worth holding. */
                        for (i2 = 0; i2 < 12000; i2++) {
                            const int si = pc->n > 0 ? i2 % pc->n : 0;
                            cp_step(&k4, pc->s[si].p[0], pc->s[si].p[1],
                                    pc->s[si].p[2], 1.f / 60.f);
                            ai_step(&a4, &t4, pc->s[si].p[0], pc->s[si].p[1],
                                    pc->s[si].p[2], k4.lap, 1.f / 60.f);
                            for (q = 0; q < a4.n; q++) {
                                if (oprev[q] > -1e8f) {
                                    float d = a4.car[q].spine_dist - oprev[q];
                                    if (d > 0.f) { if (!q) ofwd += d; }
                                    else if (d < -0.5f) oback++;
                                }
                                oprev[q] = a4.car[q].spine_dist;
                            }
                            if (pprev > -1e8f) {
                                float d = a4.player_dist - pprev;
                                if (d > 0.f) pfwd += d;
                                else if (d < -0.5f) {
                                    pback++;
                                    if (-d > pworst) pworst = -d;
                                }
                            }
                            pprev = a4.player_dist;
                        }
                        /* MOVING: both sides have to cover real ground. A frozen
                           measure passes every other check here. */
                        if (ofwd < k4.spine_len || pfwd < k4.spine_len)
                            thin++;
                        done++;
                        ai_free(&a4);
                    }
                    scene_release(&s4);
                    col_free(&c4);
                }
                ck(done == 10, "all ten tracks drive the placing's inputs",
                   "%d of 10", done);
                ck(thin == 0,
                   "and BOTH sides cover at least a full lap -- a progress "
                   "measure that freezes is monotonic and useless",
                   "%d tracks under one lap", thin);
                ck(oback == 0,
                   "an opponent's progress never goes backwards -- it is its own "
                   "recorded path length walked, exact by construction",
                   "%d backward steps", oback);
                /* The player's is BOUNDED, not zero: it is anchored on the
                   latched checkpoint index and steps at a crossing, because the
                   fraction between two checkpoints is a straight-line distance
                   against an arc span. 25 m against the 426 m the projection used
                   to jump. known-issues.md. */
                ck(pworst < 25.f,
                   "and the player's steps back by less than 25 m -- down from "
                   "the projection's 426",
                   "%d steps, worst %.1f m", pback, pworst);
            }

            /* --- THE PLACING IS THE POSITION ON THE SPINE, not the clock ------
             *
             * ai_player_place ranks by CUMULATIVE spine distance --
             * `spine_len * lap + distance into the lap` -- for the player and for
             * every opponent, which is the original's own quantity
             * (FUN_004eb630). It reads correct and is trivially satisfied by any
             * mutant while the INPUT is wrong, which is what shipped: cp_step
             * counted the opening crossing of the start/finish line as a lap, so
             * the player carried a phantom 450 to 550 m and came first from the
             * grid to the flag on every track. It read as a placing that ignored
             * where the cars were.
             *
             * So this drives the ranking directly, over a field of three at known
             * distances, and then over the phantom itself. */
            {
                ai_t p;
                memset(&p, 0, sizeof p);
                p.n = 3;
                p.car[0].spine_dist = 300.f;
                p.car[1].spine_dist = 200.f;
                p.car[2].spine_dist = 100.f;

                p.player_dist = 400.f;
                ck(ai_player_place(&p) == 1, "ahead of the field is first", "");
                p.player_dist = 250.f;
                ck(ai_player_place(&p) == 2,
                   "between the first and the second is second", "");
                p.player_dist = 150.f;
                ck(ai_player_place(&p) == 3, "and so on down", "");
                p.player_dist = 50.f;
                ck(ai_player_place(&p) == 4, "last of four is fourth", "");
                /* A TIE is not a promotion: the comparison is strict, so a car
                   level with the player does not count as ahead of it. */
                p.player_dist = 200.f;
                ck(ai_player_place(&p) == 2,
                   "and level with one of them beats it -- the compare is strict",
                   "");
                /* THE PHANTOM LAP, which is the bug this exists for. One spurious
                   lap on the player is one spine length of lead, and it takes a
                   car that is LAST to first. Written as the real expression --
                   `spine_len * lap + into the lap` -- rather than as a number, so
                   it says what went wrong. */
                {
                    const float spine_len = 500.f;
                    const float into_lap = 50.f;      /* dead last */
                    p.player_dist = 0.f * spine_len + into_lap;
                    ck(ai_player_place(&p) == 4,
                       "a player 50 m into lap 0 is LAST of four", "");
                    p.player_dist = 1.f * spine_len + into_lap;
                    ck(ai_player_place(&p) == 1,
                       "and one phantom lap makes that same car FIRST -- which is "
                       "why cp_step must not count the opening crossing", "");
                }
            }

            col_free(&rc);
            ai_free(&ai);
        }
    }

}

/* ============================================================= part 15 ==== */

/*
 * The transition bands: two base textures, a mask, and the lightmap over both.
 *
 * Written as a REAL VSC9 file rather than a hand-built scene_t, because half of
 * what can go wrong here is in the reader: the blend's UV array sits between the
 * vertices and the indices and only on a blend batch, exactly like the env-map
 * normals, so a reader that takes it unconditionally -- or skips it -- shifts
 * every batch after the first band. The fixture therefore has an ORDINARY batch
 * after the blend ones, and its vertices are checked.
 *
 * Every float is distinct so a wrong attribute offset cannot pass: reading u
 * where u2 was meant has to come back as a different number, which is the same
 * trap w_batch_distinct exists for.
 */
static void w_batch_blend(FILE *f, unsigned tex, unsigned flags, unsigned lm,
                          unsigned tex2, unsigned mask, int seed)
{
    int i;
    w_u32(f, tex);
    w_u32(f, flags);
    w_u32(f, 0);                       /* part */
    w_u32(f, lm);
    w_u32(f, 0);                       /* env: VSC7 */
    w_u32(f, 0);                       /* model: VSC8 */
    w_u32(f, tex2);                    /* VSC9 */
    w_u32(f, mask);
    w_u32(f, 3);                       /* nverts */
    w_u32(f, 3);                       /* nidx */
    for (i = 0; i < 3; i++) {
        float b = (float)(seed * 1000 + i * 20);
        float v[7] = { b + 1.f, b + 2.f, b + 3.f,
                       b + 4.f, b + 5.f,
                       b + 6.f, b + 7.f };
        fwrite(v, sizeof(float), 7, f);
    }
    if (flags & BATCH_BLEND)
        for (i = 0; i < 3; i++) {
            float b = (float)(seed * 1000 + i * 20);
            float bl[4] = { b + 8.f, b + 9.f, b + 10.f, b + 11.f };
            fwrite(bl, sizeof(float), 4, f);
        }
    for (i = 0; i < 3; i++)
        w_u16(f, (unsigned)(2 - i));
}

static const char *write_blend_fixture(void)
{
    static const char *path = "/tmp/rccars_vis_test_blend.vsc";
    FILE *f = fopen(path, "wb");
    if (!f)
        return NULL;
    fwrite("VSC9", 1, 4, f);
    w_u32(f, 4);                       /* textures */
    w_u32(f, 4);                       /* batches */
    w_u32(f, 0);                       /* parts */
    w_u32(f, 0);                       /* markers */
    { float r = 0.f; fwrite(&r, sizeof r, 1, f); }
    w_u32(f, 0);                       /* models: VSC8 */
    w_tex_rgba(f, "base_a", 64, 0, 0);
    w_tex_rgba(f, "base_b", 64, 0, 0);
    w_tex_rgba(f, "mask",   64, 0, 0);
    w_tex_rgba(f, "flat_lm", 64, 0, 1);
    /* 0: the lit band -- the case every track has */
    w_batch_blend(f, 0, BATCH_BLEND, 3, 1, 2, 1);
    /* 1: a band with no lightmap: pass 3 must skip it rather than multiply by
          an unbound sampler */
    w_batch_blend(f, 0, BATCH_BLEND, 0xFFFFFFFFu, 1, 2, 2);
    /* 2: a band whose mask does not resolve -- the flag has to come OFF, or the
          band blends against an undefined texture */
    w_batch_blend(f, 0, BATCH_BLEND, 3, 1, 99, 3);
    /* 3: an ordinary batch, LAST, so a mis-sized blend UV array shows up as
          garbage geometry here rather than as nothing at all */
    w_batch_blend(f, 0, 0, 3, 0xFFFFFFFFu, 0xFFFFFFFFu, 4);
    fclose(f);
    return path;
}

static void part15_blend(void)
{
    const char *path = write_blend_fixture();
    scene_t s;
    int i;

    printf("\n-- part 15: the surface transition bands --\n");
    if (!path) {
        ck(0, "the blend fixture .vsc could be written", "fopen failed");
        return;
    }
    scene_set_tex_quality(0);
    if (!scene_load(path, &s)) {
        ck(0, "the blend fixture loads", "scene_load failed");
        return;
    }

    /* ---- what the reader made of it ------------------------------------- */
    ck(s.n_batches == 4, "four batches", "%u", s.n_batches);
    ck((s.batches[0].flags & BATCH_BLEND) && s.batches[0].gl_tex2
       && s.batches[0].gl_mask && s.batches[0].buv,
       "a band keeps its second texture, its mask and its extra UVs",
       "flags %#x tex2 %u mask %u buv %p", s.batches[0].flags,
       s.batches[0].gl_tex2, s.batches[0].gl_mask, (void *)s.batches[0].buv);
    ck(!(s.batches[2].flags & BATCH_BLEND) && !s.batches[2].buv,
       "a band whose mask does not resolve loses the flag and draws flat",
       "flags %#x buv %p", s.batches[2].flags, (void *)s.batches[2].buv);
    /* The stream stayed in step: the last batch's own numbers, read back. */
    ck(s.batches[3].verts[2].x == 4041.f && s.batches[3].verts[2].lv == 4047.f,
       "and the ordinary batch AFTER three bands still holds its own vertices",
       "x %.1f lv %.1f", s.batches[3].verts[2].x, s.batches[3].verts[2].lv);
    ck(s.batches[0].buv[1].u2 == 1028.f && s.batches[0].buv[1].mv == 1031.f,
       "the blend UVs are the ones packed, in the right components",
       "u2 %.1f mv %.1f", s.batches[0].buv[1].u2, s.batches[0].buv[1].mv);

    /* ---- the solid pass must not draw them ------------------------------ *
     * main.c filters the world pass on this same flag. If it did not, every band
     * would get an opaque copy of its first texture underneath, which is both a
     * wasted pass and the hard edge the blend exists to remove. */
    gl_cap_reset();
    scene_draw(&s, BATCH_BLEND, 0);
    ck(glcap.n_draws == 2,
       "the world pass draws the flat batches only -- 2 of 4",
       "%d draws", glcap.n_draws);

    /* ---- the three passes ----------------------------------------------- */
    gl_cap_reset();
    scene_draw_blend(&s);
    ck(glcap.n_draws == 5,
       "two bands draw twice each, and only the lit one takes the lightmap pass",
       "%d draws", glcap.n_draws);
    if (glcap.n_draws == 5) {
        glcap_draw *p1 = &glcap.draws[0], *p2 = &glcap.draws[2],
                   *p3 = &glcap.draws[4];
        /* pass 1: the SECOND base texture, opaque, writing depth */
        ck(p1->tex == s.batches[0].gl_tex2 && !p1->blend && p1->depth_mask
           && p1->depth_func == GL_LESS && !p1->alpha_test,
           "pass 1 lays the second texture down opaque, depth writes on",
           "tex %u (want %u) blend %d mask %d func %#x alpha %d",
           p1->tex, s.batches[0].gl_tex2, p1->blend, p1->depth_mask,
           p1->depth_func, p1->alpha_test);
        /* w_batch_blend reverses its indices, so the first vertex SUBMITTED is
           vertex 2, whose base is 1000 + 2*20 = 1040. u2/v2 are +8/+9 of that. */
        ck(glcap.uv[p1->first][0] == 1048.f && glcap.uv[p1->first][1] == 1049.f,
           "and samples it through the SECOND UV set",
           "u %.1f v %.1f", glcap.uv[p1->first][0], glcap.uv[p1->first][1]);
        /* pass 2: the first base texture at alpha = mask */
        ck(p2->tex == s.batches[0].gl_tex && p2->blend
           && p2->blend_src == GL_SRC_ALPHA
           && p2->blend_dst == GL_ONE_MINUS_SRC_ALPHA
           && !p2->depth_mask && p2->depth_func == GL_LEQUAL,
           "pass 2 blends the first texture over it, depth writes off, LEQUAL",
           "tex %u blend %d %#x/%#x mask %d func %#x", p2->tex, p2->blend,
           p2->blend_src, p2->blend_dst, p2->depth_mask, p2->depth_func);
        ck(p2->unit1_tex == s.batches[0].gl_mask
           && p2->unit1_env == GL_COMBINE && p2->unit1_a_src == GL_TEXTURE,
           "with the mask on unit 1, contributing ALPHA and nothing else",
           "unit1 tex %u (want %u) env %#x alpha src %#x", p2->unit1_tex,
           s.batches[0].gl_mask, p2->unit1_env, p2->unit1_a_src);
        /* By VALUE, not by address: this batch is buffer-backed, so what the
           recorder resolved is a pointer into the stub's copy of the buffer.
           buv[0].mu is 1010 and u2 is 1008, so an offset naming the wrong pair
           cannot pass. */
        ck(p2->unit1_uv && ((const float *)p2->unit1_uv)[0] == 1010.f,
           "and unit 1 reading the MASK UVs, not the second texture's",
           "first component %.1f",
           p2->unit1_uv ? ((const float *)p2->unit1_uv)[0] : -1.f);
        ck(glcap.uv[p2->first][0] == 1044.f && glcap.uv[p2->first][1] == 1045.f,
           "unit 0 on the base UVs",
           "u %.1f v %.1f", glcap.uv[p2->first][0], glcap.uv[p2->first][1]);
        /* pass 3: the lightmap, multiplied into the pair */
        ck(p3->tex == s.batches[0].gl_lm && p3->blend
           && p3->blend_src == GL_ZERO && p3->blend_dst == GL_SRC_COLOR
           && !p3->depth_mask,
           "pass 3 multiplies the lightmap in -- ZERO/SRC_COLOR",
           "tex %u (want %u) %#x/%#x mask %d", p3->tex, s.batches[0].gl_lm,
           p3->blend_src, p3->blend_dst, p3->depth_mask);
        ck(glcap.uv[p3->first][0] == 1046.f && glcap.uv[p3->first][1] == 1047.f,
           "through the LIGHTMAP UVs, on unit 0",
           "u %.1f v %.1f", glcap.uv[p3->first][0], glcap.uv[p3->first][1]);
        ck(p3->unit1_tex == 0,
           "and unit 1 out of the way for it", "unit1 %u", p3->unit1_tex);
    }

    /* ---- and the state it hands back ------------------------------------ *
     * All of it is state the rest of the frame assumes: main.c keeps the alpha
     * test enabled, everything else draws with GL_LESS and depth writes on, and
     * a live GL_ARRAY_BUFFER turns every other module's client pointers into
     * offsets (see SCENE VERTEX BUFFERS in scene.h). */
    ck(!glcap_unit_enabled[1], "unit 1 is left disabled",
       "enabled %d", glcap_unit_enabled[1]);
    ck(st_depth_func == GL_LESS && st_depth_mask && st_alpha,
       "the depth func, depth writes and the alpha test all go back",
       "func %#x mask %d alpha %d", st_depth_func, st_depth_mask, st_alpha);
    ck(glcap_buf_bound[0] == 0 && glcap_buf_bound[1] == 0,
       "and nothing is left bound",
       "array %u element %u", glcap_buf_bound[0], glcap_buf_bound[1]);

    /* ---- and the REAL packed track ------------------------------------- *
     * A hand-written fixture is a check on the reader against itself: I wrote
     * both ends of it. What it cannot see is the PACKER and the loader
     * disagreeing about the header -- pack_vsc.py omitted VSC8's model count
     * from a VSC9 file and every field after it was read one texture-name-length
     * out, which this fixture passed happily. So load what the build actually
     * ships. */
    {
        scene_t rt;
        if (scene_load("assets/beach_1.vsc", &rt)) {
            unsigned int nb = 0, ok_in = 0, lit = 0;
            for (i = 0; i < (int)rt.n_batches; i++) {
                batch_t *b = &rt.batches[i];
                if (!(b->flags & BATCH_BLEND))
                    continue;
                nb++;
                if (b->gl_tex && b->gl_tex2 && b->gl_mask && b->buv
                    && b->gl_tex2 != b->gl_tex)
                    ok_in++;
                if (b->gl_lm)
                    lit++;
            }
            ck(nb == 13 && ok_in == nb && lit == nb,
               "the shipped beach_1 carries its 13 bands, all four inputs bound",
               "%u bands, %u complete, %u lit", nb, ok_in, lit);
            {
                unsigned int tris = 0;
                for (i = 0; i < (int)rt.n_batches; i++)
                    if (rt.batches[i].flags & BATCH_BLEND)
                        tris += rt.batches[i].nidx / 3;
                ck(tris == 766,
                   "and the 766 transition faces the scene authors",
                   "%u tris", tris);
            }
            gl_cap_reset();
            scene_cull_off();
            scene_draw_blend(&rt);
            ck(glcap.n_draws == 39,
               "which draw as 13 x 3 passes", "%d draws", glcap.n_draws);
            scene_release(&rt);
        } else {
            printf("  note: assets/beach_1.vsc not present -- the shipped-track "
                   "half of this part did not run\n");
        }
    }

    /* A scene with no band draws nothing at all and touches no state. */
    for (i = 0; i < (int)s.n_batches; i++)
        s.batches[i].flags &= ~BATCH_BLEND;
    gl_cap_reset();
    scene_draw_blend(&s);
    ck(glcap.n_draws == 0, "a scene with no band costs one loop and no draw",
       "%d draws", glcap.n_draws);

    scene_release(&s);
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
    part13_sun();
    part14_aifx();
    part15_blend();
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
