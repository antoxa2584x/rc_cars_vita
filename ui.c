/*
 * ui.c -- see ui.h.
 *
 * Everything here is one quad list. Text is one draw call per string, panels are
 * one per rectangle; a menu frame is a few dozen calls, which is nothing next to
 * the 77-batch track behind it.
 */

#include "ui.h"
#include "font.h"

#include <vitaGL.h>
#include <string.h>

typedef struct { float x, y, u, v, r, g, b, a; } uivtx;

#define MAX_VTX 4096
static uivtx  g_v[MAX_VTX];
static GLuint g_font_tex;
static int    g_saved_depth, g_saved_alpha, g_saved_cull, g_saved_blend;

void ui_init(void)
{
    /* The atlas is one coverage byte per texel. Expand to RGBA with white RGB so
       the per-vertex colour tints it through the default GL_MODULATE env --
       vitaGL's GL_ALPHA path is not worth relying on when RGBA is already the
       format the scene loader uses. */
    static unsigned char rgba[FONT_CW * FONT_COUNT * FONT_CH * 4];
    int i, n = FONT_CW * FONT_COUNT * FONT_CH;

    for (i = 0; i < n; i++) {
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = FONT_PIX[i];
    }

    glGenTextures(1, &g_font_tex);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, FONT_CW * FONT_COUNT, FONT_CH, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void ui_begin(int screen_w, int screen_h)
{
    /* Ortho with the origin top-left, built by hand rather than through glOrtho
       so there is no dependence on which of the GL/GLES spellings this vitaGL
       happens to provide. x' = 2x/w - 1, y' = 1 - 2y/h. */
    float m[16];

    g_saved_depth = glIsEnabled(GL_DEPTH_TEST);
    g_saved_alpha = glIsEnabled(GL_ALPHA_TEST);
    g_saved_cull  = glIsEnabled(GL_CULL_FACE);
    g_saved_blend = glIsEnabled(GL_BLEND);

    memset(m, 0, sizeof(m));
    m[0]  =  2.0f / (float)screen_w;
    m[5]  = -2.0f / (float)screen_h;
    m[10] = -1.0f;
    m[12] = -1.0f;
    m[13] =  1.0f;
    m[15] =  1.0f;

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadMatrixf(m);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_ALPHA_TEST);       /* the scene's 0.5 cutout would eat the text */
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnableClientState(GL_COLOR_ARRAY);
}

void ui_end(void)
{
    glDisableClientState(GL_COLOR_ARRAY);

    if (!g_saved_blend) glDisable(GL_BLEND);
    if (g_saved_cull)   glEnable(GL_CULL_FACE);
    if (g_saved_alpha)  glEnable(GL_ALPHA_TEST);
    if (g_saved_depth)  glEnable(GL_DEPTH_TEST);

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

static void flush(int nv, GLuint tex)
{
    if (nv <= 0)
        return;
    glBindTexture(GL_TEXTURE_2D, tex);
    glVertexPointer(2, GL_FLOAT, sizeof(uivtx), &g_v[0].x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(uivtx), &g_v[0].u);
    glColorPointer(4, GL_FLOAT, sizeof(uivtx), &g_v[0].r);
    glDrawArrays(GL_TRIANGLES, 0, nv);
}

static int quad(int n, float x0, float y0, float x1, float y1,
                float u0, float v0, float u1, float v1,
                float r, float g, float b, float a)
{
    static const int IX[6] = { 0, 1, 2, 0, 2, 3 };
    float px[4], py[4], pu[4], pv[4];
    int k;

    if (n + 6 > MAX_VTX)
        return n;

    px[0] = x0; py[0] = y0; pu[0] = u0; pv[0] = v0;
    px[1] = x1; py[1] = y0; pu[1] = u1; pv[1] = v0;
    px[2] = x1; py[2] = y1; pu[2] = u1; pv[2] = v1;
    px[3] = x0; py[3] = y1; pu[3] = u0; pv[3] = v1;

    for (k = 0; k < 6; k++) {
        uivtx *v = &g_v[n + k];
        int i = IX[k];
        v->x = px[i]; v->y = py[i];
        v->u = pu[i]; v->v = pv[i];
        v->r = r; v->g = g; v->b = b; v->a = a;
    }
    return n + 6;
}

void ui_rect(float x, float y, float w, float h,
             float r, float g, float b, float a)
{
    int n;
    glDisable(GL_TEXTURE_2D);
    n = quad(0, x, y, x + w, y + h, 0.0f, 0.0f, 0.0f, 1.0f, r, g, b, a);
    flush(n, 0);
    glEnable(GL_TEXTURE_2D);
}

void ui_image(float x, float y, float w, float h, unsigned int tex,
              float u0, float v0, float u1, float v1,
              float r, float g, float b, float a)
{
    int n;
    if (!tex)
        return;
    n = quad(0, x, y, x + w, y + h, u0, v0, u1, v1, r, g, b, a);
    flush(n, (GLuint)tex);
}

float ui_text_w(float scale, const char *s)
{
    return (float)strlen(s) * (float)FONT_CW * scale;
}

float ui_text_h(float scale)
{
    return (float)FONT_CH * scale;
}

void ui_text(float x, float y, float scale,
             float r, float g, float b, float a, const char *s)
{
    const float cw = (float)FONT_CW * scale;
    const float chh = (float)FONT_CH * scale;
    const float step = 1.0f / (float)FONT_COUNT;
    int n = 0;

    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c >= FONT_FIRST && c <= FONT_LAST && c != ' ') {
            int i = c - FONT_FIRST;
            /* Half a texel in from each edge: the atlas is a single row of
               cells and GL_LINEAR would otherwise bleed the neighbouring
               glyph in along the seam. */
            float half = 0.5f / (float)(FONT_CW * FONT_COUNT);
            n = quad(n, x, y, x + cw, y + chh,
                     (float)i * step + half, 0.0f,
                     (float)(i + 1) * step - half, 1.0f,
                     r, g, b, a);
        }
        x += cw;
    }
    flush(n, g_font_tex);
}
