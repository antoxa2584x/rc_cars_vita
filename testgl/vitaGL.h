/*
 * testgl/vitaGL.h -- a recording stand-in for vitaGL, so the port's rendering
 * modules can be compiled and RUN on the host.
 *
 * The point is not to draw anything. It is that shadow.c, water.c and
 * checkpoint.c are mostly geometry, and geometry is exactly the kind of code
 * this port keeps getting wrong in ways that only show up on a screen (see
 * CLAUDE.md: the renderer never being switched over, the model's half turn,
 * VisTurn's sign, the mirrored yaw convention). Every one of those would have
 * been caught by putting a point through the real transform and looking at the
 * number.
 *
 * So: the entry points are real functions that record what was submitted, and
 * vis_test.c reads the recording back. gl_cap_reset() clears it.
 *
 * Only the calls those three modules and scene.c actually make are here.
 */

#ifndef TEST_VITAGL_H
#define TEST_VITAGL_H

#include <stddef.h>

typedef unsigned int GLuint;
typedef int GLint;
typedef unsigned int GLenum;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef void GLvoid;

#define GL_FALSE 0
#define GL_TRUE 1

#define GL_TEXTURE_2D            0x0DE1
#define GL_RGB                   0x1907
#define GL_RGBA                  0x1908
#define GL_UNSIGNED_BYTE         0x1401
#define GL_UNSIGNED_SHORT        0x1403
#define GL_FLOAT                 0x1406
#define GL_UNSIGNED_SHORT_5_6_5  0x8363
#define GL_TEXTURE_MIN_FILTER    0x2801
#define GL_TEXTURE_MAG_FILTER    0x2800
#define GL_TEXTURE_WRAP_S        0x2802
#define GL_TEXTURE_WRAP_T        0x2803
#define GL_LINEAR                0x2601
#define GL_LINEAR_MIPMAP_LINEAR  0x2703
#define GL_REPEAT                0x2901
#define GL_CLAMP_TO_EDGE         0x812F
#define GL_TRIANGLES             0x0004
#define GL_TRIANGLE_FAN          0x0006
#define GL_BLEND                 0x0BE2
#define GL_ALPHA_TEST            0x0BC0
#define GL_CULL_FACE             0x0B44
#define GL_DEPTH_TEST            0x0B71
/* GL_ONE and GL_ZERO: sun.c's flare pass is SRCCOLOR/ONE, which is the
   original's blend mode 3 (0x0045c81e sets DESTBLEND <- 2 = D3DBLEND_ONE). */
#define GL_ZERO                  0
#define GL_ONE                   1
#define GL_SRC_COLOR             0x0300
#define GL_SRC_ALPHA             0x0302
#define GL_ONE_MINUS_SRC_ALPHA   0x0303
#define GL_DST_COLOR             0x0306
#define GL_POLYGON_OFFSET_FILL   0x8037
#define GL_COLOR_ARRAY           0x8076
#define GL_VERTEX_ARRAY          0x8074
#define GL_TEXTURE_COORD_ARRAY   0x8078

/* ------------------------------------------------------------- recording */

#define GLCAP_MAX_VERTS 200000
#define GLCAP_MAX_DRAWS 256

typedef struct {
    GLenum mode;
    int first, count;        /* into the captured vertex arrays */
    GLuint tex;
    float color[4];
    int depth_mask, blend, alpha_test, cull;
    /* the depth bias in force for this draw: 0 when GL_POLYGON_OFFSET_FILL is
       off, whatever glPolygonOffset last set when it is on. Recorded because
       the foam band is a decal the level art laid EXACTLY on the terrain, and a
       decal drawn with no bias z-fights -- which no capture of the vertices
       alone can see. */
    float pol_factor, pol_units;
    /* The blend factors and the texture env mode, for the same reason and after
       the same bug: the tyre marks came out as an opaque grey stripe for months
       because they were alpha-blended instead of modulating the ground, and
       glBlendFunc was a NO-OP STUB, so nothing in the vertices, the state flags
       or the depth bias could see it. A recorder that discards its arguments
       cannot fail -- this file has now learned that three times (glTexImage2D,
       glMultMatrixf, and this). */
    GLenum blend_src, blend_dst;
    GLenum env_mode;
} glcap_draw;

typedef struct {
    float pos[GLCAP_MAX_VERTS][3];
    float uv[GLCAP_MAX_VERTS][2];
    unsigned char rgba[GLCAP_MAX_VERTS][4];
    int has_color[GLCAP_MAX_VERTS];
    int n_verts;
    glcap_draw draws[GLCAP_MAX_DRAWS];
    int n_draws;
    int overflow;
} glcap_t;

extern glcap_t glcap;

void gl_cap_reset(void);

/* the calls the port makes */
void glGenTextures(GLsizei n, GLuint *out);
void glBindTexture(GLenum t, GLuint id);
void glTexImage2D(GLenum t, GLint lvl, GLint ifmt, GLsizei w, GLsizei h,
                  GLint border, GLenum fmt, GLenum type, const void *px);
void glTexParameteri(GLenum t, GLenum p, GLint v);
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *p);
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *p);
void glColorPointer(GLint size, GLenum type, GLsizei stride, const void *p);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx);
void glEnable(GLenum c);
void glDisable(GLenum c);
void glEnableClientState(GLenum c);
void glDisableClientState(GLenum c);
void glBlendFunc(GLenum a, GLenum b);
void glPolygonOffset(GLfloat factor, GLfloat units);
void glDepthMask(GLboolean b);
void glColor4f(float r, float g, float b, float a);
void glPushMatrix(void);
void glPopMatrix(void);
void glMultMatrixf(const float *m);

/* Added for ui.c, the app's only HAND-BUILT projection: it loads its own ortho
   matrix rather than going through glFrustum, and saves/restores the enables
   around the menu. ui_test.c puts a point through the matrix ui.c actually
   loaded, which is the same reason this stub exists at all. */
#define GL_MODELVIEW             0x1700
#define GL_PROJECTION            0x1701

void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glLoadMatrixf(const float *m);
void glDeleteTextures(GLsizei n, const GLuint *ids);
GLboolean glIsEnabled(GLenum c);

/* The last matrix loaded, in the order glLoadMatrixf received it -- OpenGL
   column-major column-vector, so element [c*4+r] is column c, row r. */
extern float glcap_matrix[16];
extern int   glcap_matrix_mode;

/* Added for scene.c's frustum culling and its one-call mip generation.
 *
 * glGetFloatv is here so scene_frustum_from_gl COMPILES on the host; it hands
 * back the identity, and no test drives the culler through it. The culler is
 * exercised through scene_set_frustum with a matrix the test builds itself,
 * which is the honest way round -- scene_frustum_from_gl's whole job is to read
 * GL's real stacks, and a recorder that invented them would be testing the
 * recorder. What it does mean is stated where the checks are.
 *
 * glGenerateMipmap IS recorded: it replaced N-1 glTexImage2D calls, so without
 * it "the chain reaches the GPU" has nothing left to assert against. */
#define GL_MODELVIEW_MATRIX      0x0BA6
#define GL_PROJECTION_MATRIX     0x0BA7

void glGetFloatv(GLenum pname, GLfloat *out);
void glGenerateMipmap(GLenum target);

/* How many times glGenerateMipmap was called for a given texture id. */
int glcap_mipgen_count(GLuint tex);
void glcap_mipgen_reset(void);

/* Added for scene.c's lightmap pass, which puts the atlas on texture unit 1 and
   modulates. vis_test records which unit was active so the second UV set can be
   checked as a submission, not as an intention. */
#define GL_TEXTURE0              0x84C0
#define GL_TEXTURE1              0x84C1
#define GL_TEXTURE_ENV           0x2300
#define GL_TEXTURE_ENV_MODE      0x2200
#define GL_TEXTURE_ENV_COLOR     0x2201
#define GL_MODULATE              0x2100

/* The texture combiner, for trace.c: the tyre mark's stage op is the engine's
   D3DTOP_MODULATEALPHA_ADDCOLOR, and GL_INTERPOLATE is the only fixed-function
   env that expresses it. See trace.h. */
#define GL_COMBINE               0x8570
#define GL_COMBINE_RGB           0x8571
#define GL_INTERPOLATE           0x8575
#define GL_CONSTANT              0x8576
#define GL_PRIMARY_COLOR         0x8577
#define GL_TEXTURE               0x1702
#define GL_SRC0_RGB              0x8580
#define GL_SRC1_RGB              0x8581
#define GL_SRC2_RGB              0x8582
#define GL_OPERAND0_RGB          0x8590
#define GL_OPERAND1_RGB          0x8591
#define GL_OPERAND2_RGB          0x8592

void glActiveTexture(GLenum unit);
void glClientActiveTexture(GLenum unit);
void glTexEnvi(GLenum target, GLenum pname, GLint param);
void glTexEnvfv(GLenum target, GLenum pname, GLfloat *param);

/* The GL_TEXTURE_ENV_COLOR last set, so the mark's neutral level is checkable
   as a submission rather than as an intention. */
extern float glcap_env_color[4];

/* per-unit capture: [0] is the base texture, [1] the lightmap */
extern GLuint glcap_unit_tex[2];
extern int    glcap_unit_enabled[2];

/*
 * Added for scene.c's vertex buffers -- see SCENE VERTEX BUFFERS in scene.h.
 *
 * These are NOT no-ops, and that is the point. A batch on the GPU passes
 * gl*Pointer a byte OFFSET where it used to pass an address, so a recorder that
 * threw the buffer away would dereference a small integer and read the bottom of
 * the host's address space. Worse, it would do it quietly: every geometry check
 * in vis_test.c reads its positions and UVs back through this recorder, and they
 * would all be reading rubbish while still comparing it against itself.
 *
 * So the stub keeps the bytes glBufferData was given and resolves base + offset
 * at the pointer calls, the way GL does. That turns the 200-odd geometry checks
 * already here into a test of the buffered path as well: wrong data in a buffer,
 * or a wrong attribute offset, and they go red.
 *
 * (CLAUDE.md lists this as a standing trap -- "every no-op stub is a blind
 * spot". glTexImage2D, glMultMatrixf, glBlendFunc and glTexEnvi each hid a real
 * bug here by discarding their arguments.)
 */
#define GL_ARRAY_BUFFER          0x8892
#define GL_ELEMENT_ARRAY_BUFFER  0x8893
#define GL_STATIC_DRAW           0x88E4

typedef long GLsizeiptr;

void glGenBuffers(GLsizei n, GLuint *out);
void glBindBuffer(GLenum target, GLuint id);
void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
void glDeleteBuffers(GLsizei n, const GLuint *ids);

/* What is bound right now, so a test can assert scene_draw left nothing behind.
   A stale GL_ARRAY_BUFFER turns every other module's client pointers into
   offsets, which is the one way this change can break code it never touched. */
extern GLuint glcap_buf_bound[2];        /* [0] array, [1] element array */

/* How many buffers currently hold data: glGenBuffers up, glDeleteBuffers down.
   A track reload that leaked them shows as a number that only ever grows. */
int glcap_buffers_live(void);

#endif
