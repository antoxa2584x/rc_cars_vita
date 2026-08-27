/* sfont.c -- see sfont.h. Lifted verbatim out of race_ui.c, which had it as a
 * file-static and is now one of two callers. */
#include "sfont.h"
#include "ui.h"

/* ------------------------------------------------------- the engine's own font
 *
 * A glyph is one cell of a 10 x 9 atlas, and the quad is the cell's INK rather
 * than the whole cell: the art is set with a wide, ragged margin either side
 * (0.08 to 0.43 of a cell on the left alone), so a monospaced advance spreads
 * `1/6` and a clock out into nonsense. SF_INK_* carries the measured bounds and
 * the advance is the ink plus letSpace.
 *
 * `scale` is a multiplier on letSizeX/letSizeY, so 1.0 draws the atlas at the
 * pixel size its own .ini names.
 */
sfont sf_big(unsigned int tex)
{
    sfont f;
    f.tex = tex;
    f.ink = SF_INK_BIG;
    f.size_x = SF_BIG_SIZE_X;
    f.size_y = SF_BIG_SIZE_Y;
    f.space = SF_BIG_SPACE;
    f.space_len = SF_BIG_SPACE_LEN;
    return f;
}

sfont sf_small(unsigned int tex)
{
    sfont f;
    f.tex = tex;
    f.ink = SF_INK_SMALL;
    f.size_x = SF_SMALL_SIZE_X;
    f.size_y = SF_SMALL_SIZE_Y;
    f.space = SF_SMALL_SPACE;
    f.space_len = SF_SMALL_SPACE_LEN;
    return f;
}

static int sf_glyph(int c)
{
    return SF_INDEX[(unsigned char)c];
}

float sf_w(const sfont *f, float scale, const char *s)
{
    float w = 0.f;
    for (; *s; s++) {
        int g = sf_glyph(*s);
        if (g < 0)
            w += f->space_len * scale;
        else
            w += (f->ink[g][1] - f->ink[g][0]) * f->size_x * scale
                 + f->space * scale;
    }
    return w;
}

float sf_h(const sfont *f, float scale)
{
    return f->size_y * scale;
}

/* One string, top-left at (x, y). Nothing is drawn when the atlas is missing --
   the caller falls back to ui_text. */
void sf_text(const sfont *f, float x, float y, float scale,
                    float r, float g, float b, float a, const char *s)
{
    /* Half a texel in on V. The nine rows are adjacent in the atlas, so
       GL_LINEAR would otherwise bleed the row above and below into a glyph's
       top and bottom. U needs no such inset: the ink bounds are already inside
       the cell on both sides for all 90 glyphs. */
    const float vh = 0.5f / (float)SF_ATLAS;
    const float hh = sf_h(f, scale);

    if (!f->tex)
        return;
    for (; *s; s++) {
        int gi = sf_glyph(*s);
        if (gi < 0) {
            x += f->space_len * scale;
            continue;
        }
        {
            const int col = gi % SF_COLS, row = gi / SF_COLS;
            const float l = f->ink[gi][0], rr = f->ink[gi][1];
            const float w = (rr - l) * f->size_x * scale;
            ui_image(x, y, w, hh, f->tex,
                     ((float)col + l) / (float)SF_COLS,
                     (float)row / (float)SF_ROWS + vh,
                     ((float)col + rr) / (float)SF_COLS,
                     (float)(row + 1) / (float)SF_ROWS - vh,
                     r, g, b, a);
            x += w + f->space * scale;
        }
    }
}

/* The same string twice: a dark copy offset by a fraction of its own height,
   then the bright one. The tracks are sand, asphalt and pale stone, and the
   game's own font is light grey -- unshadowed it disappears on half of them.
   The offset scales with the text so it does not become a smear at one size and
   invisible at another. */
void sf_text_shadowed(const sfont *f, float x, float y, float scale,
                             float r, float g, float b, float a, const char *s)
{
    const float d = sf_h(f, scale) * 0.07f;
    sf_text(f, x + d, y + d, scale, 0.f, 0.f, 0.f, a * 0.65f, s);
    sf_text(f, x, y, scale, r, g, b, a, s);
}

