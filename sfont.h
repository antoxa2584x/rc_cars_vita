/*
 * sfont.h -- THE ENGINE'S OWN FONT, drawn through ui.c.
 *
 * `Language/English/Smash20.csi` and `Smash26.csi`: 90 glyphs each on a 10 x 9
 * atlas, the character order in a matching 91-byte `.dat`, the metrics in
 * `Settings/smash20.ini` / `smash26.ini`. gen_hud_data.py bakes all of it into
 * hud_data.h -- the atlas layout, the per-glyph INK bounds and the two .ini
 * blocks -- and this is the drawer over that table.
 *
 * IT LIVED IN race_ui.c AND NOW IT DOES NOT, because the main menu wants the
 * same letters and a second copy of a font renderer is a second set of metrics
 * to get out of step. race_ui.c is unchanged in what it draws; it includes this
 * instead of defining it. font.h's baked Consolas stays where it is: the START
 * menu must come up whether or not any asset loaded, and this needs an atlas.
 *
 * `scale` is a multiplier on letSizeX/letSizeY, so 1.0 draws the atlas at the
 * pixel size its own .ini names -- pixels of the 800x600 frame those .ini files
 * are written in, which is what HUD_REF_W/H are.
 */
#ifndef SFONT_H
#define SFONT_H

#include "hud_data.h"

typedef struct {
    unsigned int tex;
    const float (*ink)[2];
    float size_x, size_y, space, space_len;
} sfont;

/* The two the game ships. `tex` is the atlas handle the caller resolved --
   0 for "not loaded", which makes every draw below a no-op so the caller can
   fall back to ui_text. */
sfont sf_big(unsigned int tex);
sfont sf_small(unsigned int tex);

/* Width of `s` in pixels at `scale`, and the line height. */
float sf_w(const sfont *f, float scale, const char *s);
float sf_h(const sfont *f, float scale);

/* One string, its TOP-LEFT at (x, y). */
void  sf_text(const sfont *f, float x, float y, float scale,
              float r, float g, float b, float a, const char *s);

/* The same, with a dark copy under it -- see the note in sfont.c. */
void  sf_text_shadowed(const sfont *f, float x, float y, float scale,
                       float r, float g, float b, float a, const char *s);

#endif /* SFONT_H */
