/*
 * race_ui.c -- see race_ui.h. The minimap, the place badge, the two clocks and
 * the two gauges, on the game's own artwork and at the game's own layout.
 */

#include "race_ui.h"
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define DEG2RAD 0.017453292519943295f
#define RUI_PI  3.14159265358979f

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ------------------------------------------------- the 800x600 -> screen map
 *
 * Every number in hud_data.h is a pixel of the 800x600 frame the HUD was
 * authored at. The Vita's panel is 960x544 and 16:9, so the mapping has to
 * choose, and it chooses the same way hud.h does for the message layer: scale
 * UNIFORMLY, off the HEIGHT, so nothing 4:3 is stretched 4/3 wider -- and then
 * anchor each group to the EDGE it was authored against, so a badge in the
 * corner stays in the corner instead of drifting toward the middle.
 *
 * Three anchors, and every element in the file names the one it uses:
 *   L  the place badge and the boost dial      (from the left edge)
 *   R  the lap word, the lap number, the map, the speed dial   (from the right)
 *   C  the two clocks                          (about the centre)
 */
typedef struct { float s, w, h; } frame;

static frame fr_of(int screen_w, int screen_h)
{
    frame f;
    f.w = (float)screen_w;
    f.h = (float)screen_h;
    f.s = f.h / HUD_REF_H;
    return f;
}

static float axL(const frame *f, float x) { return x * f->s; }
static float axR(const frame *f, float x) { return f->w - (HUD_REF_W - x) * f->s; }
static float axC(const frame *f, float x)
{
    return f->w * 0.5f + (x - HUD_REF_W * 0.5f) * f->s;
}
static float ay(const frame *f, float y) { return y * f->s; }

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
typedef struct {
    unsigned int tex;
    const float (*ink)[2];
    float size_x, size_y, space, space_len;
} sfont;

static sfont sf_big(const race_ui_t *r)
{
    sfont f;
    f.tex = r->tex.font_big;
    f.ink = SF_INK_BIG;
    f.size_x = SF_BIG_SIZE_X;
    f.size_y = SF_BIG_SIZE_Y;
    f.space = SF_BIG_SPACE;
    f.space_len = SF_BIG_SPACE_LEN;
    return f;
}

static sfont sf_small(const race_ui_t *r)
{
    sfont f;
    f.tex = r->tex.font_small;
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

static float sf_w(const sfont *f, float scale, const char *s)
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

static float sf_h(const sfont *f, float scale)
{
    return f->size_y * scale;
}

/* One string, top-left at (x, y). Nothing is drawn when the atlas is missing --
   the caller falls back to ui_text. */
static void sf_text(const sfont *f, float x, float y, float scale,
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
static void sf_text_shadowed(const sfont *f, float x, float y, float scale,
                             float r, float g, float b, float a, const char *s)
{
    const float d = sf_h(f, scale) * 0.07f;
    sf_text(f, x + d, y + d, scale, 0.f, 0.f, 0.f, a * 0.65f, s);
    sf_text(f, x, y, scale, r, g, b, a, s);
}

/* ----------------------------------------------------------------- the clocks */

/* `%i:%02i.%02i`, which is the format string at 0x56b388 -- minutes, seconds,
   hundredths. The exe's own is `%i` on the minutes and the artwork in
   opt_cock_all reads `01:16.92`, so the port pads to two: a clock whose width
   changes on the way past a minute is not a clock. */
static void fmt_time(char *out, int n, float t)
{
    int cs, m, s;
    if (t < 0.f)
        t = 0.f;
    cs = (int)(t * 100.f + 0.5f);
    m = cs / 6000;
    s = (cs / 100) % 60;
    cs %= 100;
    if (m > 99)
        m = 99;
    snprintf(out, n, "%02i:%02i.%02i", m, s, cs);
}

/* ------------------------------------------------------------------- the API */

void race_ui_init(race_ui_t *r, const race_ui_tex *tex)
{
    memset(r, 0, sizeof(*r));
    if (tex)
        r->tex = *tex;
    r->track = -1;
    r->lap_hold = -1.f;
    r->units = RUI_UNITS_DEFAULT;
}

void race_ui_units(race_ui_t *r, rui_units u)
{
    if (r)
        r->units = (u == RUI_IMPERIAL) ? RUI_IMPERIAL : RUI_METRIC;
}

/* The affine through the three (x, z) -> (u, v) pairs. Cramer over a 3x3,
   which is what FUN_004b8500 does with the same six numbers -- and the same
   degeneracy guard, because three coincident markers give no transform. */
static int solve_map(const map_calib *c, float m[6])
{
    const float x0 = c->world[0][0], z0 = c->world[0][1];
    const float x1 = c->world[1][0], z1 = c->world[1][1];
    const float x2 = c->world[2][0], z2 = c->world[2][1];
    const float det = (x1 - x0) * (z2 - z0) - (x2 - x0) * (z1 - z0);
    int k;

    if (fabsf(det) < 1e-6f)
        return 0;
    for (k = 0; k < 2; k++) {
        const float q0 = c->art[0][k], q1 = c->art[1][k], q2 = c->art[2][k];
        const float a = ((q1 - q0) * (z2 - z0) - (q2 - q0) * (z1 - z0)) / det;
        const float b = ((x1 - x0) * (q2 - q0) - (x2 - x0) * (q1 - q0)) / det;
        m[k * 3 + 0] = a;
        m[k * 3 + 1] = b;
        m[k * 3 + 2] = q0 - a * x0 - b * z0;
    }
    return 1;
}

void race_ui_set_track(race_ui_t *r, int track, unsigned int map_tex)
{
    if (!r)
        return;
    r->track = track;
    r->tex.map = map_tex;
    r->have_map = 0;
    if (track < 0 || track >= MAP_N_TRACKS || !map_tex)
        return;
    r->have_map = solve_map(&MAP_CALIB[track], r->m);
}

void race_ui_start(race_ui_t *r)
{
    if (!r)
        return;
    r->t_race = 0.f;
    r->t_lap = 0.f;
    r->lap_hold = -1.f;
    r->hold_left = 0.f;
    r->blink_t = 0.f;
    r->running = 1;
}

void race_ui_lap(race_ui_t *r)
{
    if (!r || !r->running)
        return;
    r->lap_hold = r->t_lap;
    r->hold_left = HUD_LAP_BLINK;
    r->blink_t = 0.f;
    r->t_lap = 0.f;
}

void race_ui_stop(race_ui_t *r)
{
    if (r)
        r->running = 0;
}

void race_ui_step(race_ui_t *r, float dt)
{
    if (!r)
        return;
    /* The HOLD runs whether or not the race does, so a lap crossed on the last
       frame of a race still gets its 4.2 s on screen. The clocks do not. */
    if (r->hold_left > 0.f) {
        r->hold_left -= dt;
        r->blink_t += dt;
        if (r->hold_left <= 0.f) {
            r->hold_left = 0.f;
            r->lap_hold = -1.f;
            r->blink_t = 0.f;
        }
    }
    if (!r->running)
        return;
    r->t_race += dt;
    r->t_lap += dt;
}

int race_ui_map_heading(const race_ui_t *r, float yaw, float *out)
{
    /* The car's forward in world XZ -- the rig's convention, local +Z on
       (sin yaw, 0, cos yaw) -- through the transform's linear part. The
       translation is deliberately absent: this is a DIRECTION. */
    const float fx = sinf(yaw), fz = cosf(yaw);
    float du, dv;

    if (!r || !r->have_map)
        return 0;
    du = r->m[0] * fx + r->m[1] * fz;
    dv = r->m[3] * fx + r->m[4] * fz;
    if (!(du * du + dv * dv > 1e-20f))
        return 0;
    /* Screen: u right, v DOWN, so `up` is (0, -1) and an angle measured from up
       clockwise is atan2(du, -dv). map_arrow's own triangle points up -- measured,
       not assumed: its ink is 4 px wide at the top row and 22 at the bottom. */
    if (out)
        *out = atan2f(du, -dv);
    return 1;
}

int race_ui_map_uv(const race_ui_t *r, float x, float z, float *u, float *v)
{
    if (!r || !r->have_map)
        return 0;
    if (u)
        *u = r->m[0] * x + r->m[1] * z + r->m[2];
    if (v)
        *v = r->m[3] * x + r->m[4] * z + r->m[5];
    return 1;
}

float race_ui_lap_time(const race_ui_t *r, int *held)
{
    if (!r) {
        if (held)
            *held = 0;
        return 0.f;
    }
    if (r->hold_left > 0.f && r->lap_hold >= 0.f) {
        if (held)
            *held = 1;
        return r->lap_hold;
    }
    if (held)
        *held = 0;
    return r->t_lap;
}

/* --------------------------------------------------------------- the minimap */

/* One row of the outline table, centred about the panel's middle and scaled to
   its side -- which is exactly (u * 167 - 83.5) * 1/512 rescaled, i.e. what
   0x4b79ea computes, with the 1/512 and the later 512 cancelled out. */
static void map_pt(float px, float py, float side, int i,
                   float *ox, float *oy)
{
    *ox = px + MAP_OUTLINE[i][0] * side;
    *oy = py + MAP_OUTLINE[i][1] * side;
}

/* THE WINDOW OF THE ART THE PANEL SHOWS: a square of side 1/RUI_MAP_ZOOM in the
 * texture's own 0..1, centred on the player and CLAMPED to the art.
 *
 * Clamped and not wrapped, so a car near the edge of the painting stops the
 * scroll rather than tearing the map -- which is why the arrow is not always at
 * the panel's centre and why map_win_uv exists rather than every caller assuming
 * the centre. At zoom 1 the window is the whole art and every expression below
 * collapses to the identity, which is what keeps the original's own view one
 * constant away.
 *
 * Writes the window's origin and its side. */
typedef struct { float u0, v0, side; } map_win;

static map_win map_window(const race_ui_t *r, const race_ui_state *s)
{
    map_win w;
    float cu = 0.5f, cv = 0.5f;

    w.side = 1.f / (RUI_MAP_ZOOM > 1.f ? RUI_MAP_ZOOM : 1.f);
    /* Centred on the CAR. Off the art -- which happens: the transform puts the
       car outside 0..1 on the edges of some tracks -- the clamp below still
       leaves a sane window rather than a blank one. */
    race_ui_map_uv(r, s->car_x, s->car_z, &cu, &cv);
    w.u0 = clampf(cu - w.side * 0.5f, 0.f, 1.f - w.side);
    w.v0 = clampf(cv - w.side * 0.5f, 0.f, 1.f - w.side);
    return w;
}

/* A point in the art -> the same point in the window's own 0..1, i.e. where it
   falls across the panel. Outside 0..1 means outside the panel. */
static void map_win_uv(const map_win *w, float u, float v,
                       float *ou, float *ov)
{
    *ou = (u - w->u0) / w->side;
    *ov = (v - w->v0) / w->side;
}

/* The panel: three quads over the eight outline points, rows (0,1)-(2,3),
   (2,3)-(4,5) and (4,5)-(6,7), which is the strip 0x4b79ea draws. `grow`
   inflates the shape about its own centre for the dark plate underneath.
   `tex` 0 draws the plate untextured. */
static void map_panel(float px, float py, float side,
                      float grow, unsigned int tex, const map_win *win,
                      float cr, float cg, float cb, float ca)
{
    const float cx = px + side * 0.5f, cy = py + side * 0.5f;
    int row;

    for (row = 0; row < 3; row++) {
        const int i0 = row * 2, i1 = row * 2 + 1;
        const int i2 = row * 2 + 2, i3 = row * 2 + 3;
        float x[4], y[4], u[4], v[4];
        float ax, ayy;

        map_pt(px, py, side, i0, &ax, &ayy);
        x[0] = cx + (ax - cx) * grow; y[0] = cy + (ayy - cy) * grow;
        u[0] = win->u0 + MAP_OUTLINE[i0][0] * win->side;
        v[0] = win->v0 + MAP_OUTLINE[i0][1] * win->side;
        map_pt(px, py, side, i1, &ax, &ayy);
        x[1] = cx + (ax - cx) * grow; y[1] = cy + (ayy - cy) * grow;
        u[1] = win->u0 + MAP_OUTLINE[i1][0] * win->side;
        v[1] = win->v0 + MAP_OUTLINE[i1][1] * win->side;
        map_pt(px, py, side, i3, &ax, &ayy);
        x[2] = cx + (ax - cx) * grow; y[2] = cy + (ayy - cy) * grow;
        u[2] = win->u0 + MAP_OUTLINE[i3][0] * win->side;
        v[2] = win->v0 + MAP_OUTLINE[i3][1] * win->side;
        map_pt(px, py, side, i2, &ax, &ayy);
        x[3] = cx + (ax - cx) * grow; y[3] = cy + (ayy - cy) * grow;
        u[3] = win->u0 + MAP_OUTLINE[i2][0] * win->side;
        v[3] = win->v0 + MAP_OUTLINE[i2][1] * win->side;

        ui_image_quad(x, y, u, v, tex, cr, cg, cb, ca);
    }
}

/* A marker on the panel, at the world point (wx, wz). `cell` picks which half of
   the 64x32 art (0 or 1); `ang` rotates it, which only map_arrow uses. -> 0 when
   the point falls outside the art, so a car off the edge of a hand-painted map
   does not draw a marker on the frame. */
static int map_mark(const race_ui_t *r, const map_win *win,
                    float px, float py, float side,
                    float wx, float wz, unsigned int tex, int cell, float ang,
                    float cr, float cg, float cb, float ca)
{
    float u, v, mk;

    if (!tex || !race_ui_map_uv(r, wx, wz, &u, &v))
        return 0;
    /* Culled against the WINDOW, not against the art: at any zoom past 1 most of
       the painting is off the panel and a marker there would be drawn on the
       frame. The whole-art test is the zoom-1 case of this one. */
    map_win_uv(win, u, v, &u, &v);
    if (u < 0.f || u > 1.f || v < 0.f || v > 1.f)
        return 0;
    mk = side * RUI_MARK_FRAC;
    ui_image_rot(px + u * side, py + v * side, mk, mk, ang, tex,
                 cell ? 0.5f : 0.f, 0.f, cell ? 1.f : 0.5f, 1.f,
                 cr, cg, cb, ca);
    return 1;
}

static void draw_map(const race_ui_t *r, const race_ui_state *s, const frame *f)
{
    const float side = MAP_SIDE * f->s;
    const float px = axR(f, MAP_POS_X), py = ay(f, MAP_POS_Y);
    map_win win;
    int i;

    if (!r->have_map)
        return;

    win = map_window(r, s);

    /* The plate, then the map inset in it. The plate is untextured, so the window
       it is handed is only there to keep one signature. */
    map_panel(px, py, side, RUI_PLATE_GROW, 0, &win,
              0.f, 0.f, 0.f, RUI_PLATE_ALPHA);
    map_panel(px, py, side, 1.f, r->tex.map, &win, 1.f, 1.f, 1.f, 1.f);

    /* The checkpoints under the cars, and the one being headed for in the other
       cell -- which is what map_cp's two cells are for. */
    for (i = 0; i < s->n_cp && i < RUI_MAX_CP; i++)
        map_mark(r, &win, px, py, side, s->cp_x[i], s->cp_z[i], r->tex.cp,
                 i == s->cp_next ? 0 : 1, 0.f, 1.f, 1.f, 1.f, 1.f);

    /* The opponents in map_arrow's second cell, TURNED the same way the player is
       -- the recorded pose carries their heading and the two cells are the same
       triangle in two colours, so an unturned one is the only arrow on the panel
       that does not say which way its car is going. */
    for (i = 0; i < s->n_others && i < RUI_MAX_OTHERS; i++) {
        float oang = 0.f;
        race_ui_map_heading(r, s->other_yaw[i], &oang);
        map_mark(r, &win, px, py, side, s->other_x[i], s->other_z[i],
                 r->tex.arrow, 1, oang, 1.f, 1.f, 1.f, 1.f);
    }

    /* The player last, so nothing draws over it, and TURNED THROUGH THE MAP'S OWN
     * TRANSFORM -- see race_ui_map_heading. This used to be `pi - yaw`, derived
     * on paper from "+z runs down the art", and every one of the ten paintings is
     * rotated. */
    {
        float ang = 0.f;
        race_ui_map_heading(r, s->car_yaw, &ang);
        map_mark(r, &win, px, py, side, s->car_x, s->car_z, r->tex.arrow, 0,
                 ang, 1.f, 1.f, 1.f, 1.f);
    }
}

/* ---------------------------------------------------------------- the gauges */

/* One dial. `frac` is 0..1 of the sweep; `mirror` flips the art and the sweep,
   which is what makes the right-hand dial the left one's reflection -- exactly
   as opt_cock_all shows them. */
static void draw_dial(const race_ui_t *r, const frame *f, int mirror,
                      float frac, const char *value, const char *label)
{
    /* The quad's side follows from the ring's measured screen radius and the
       art's own outer radius, so the two cannot drift apart. */
    const float side = (HUD_DIAL_R_PX / HUD_DIAL_R1) * f->s;
    const float rpx = HUD_DIAL_R_PX * f->s;
    const float cx = mirror ? axR(f, HUD_REF_W - HUD_DIAL_CX)
                            : axL(f, HUD_DIAL_CX);
    const float cy = ay(f, HUD_DIAL_CY);
    /* The ring is not quite centred in its texture; the quad has to be placed so
       that the RING lands on (cx, cy), not so that the texture does. */
    const float qx = cx - (mirror ? (1.f - HUD_DIAL_CU) : HUD_DIAL_CU) * side;
    const float qy = cy - HUD_DIAL_CV * side;
    const float r0 = HUD_DIAL_R0 * side, r1 = HUD_DIAL_R1 * side;
    const float a0 = HUD_IND_ANG_MIN * DEG2RAD;
    const float a1 = HUD_IND_ANG_MAX * DEG2RAD;
    const float sw = mirror ? -1.f : 1.f;
    sfont fs = sf_small(r);

    frac = clampf(frac, 0.f, 1.f);

    /* The dial itself: disc, silver rim and the gold sweep track, whole. */
    if (r->tex.dial)
        ui_image(qx, qy, side, side, r->tex.dial,
                 mirror ? 1.f : 0.f, 0.f, mirror ? 0.f : 1.f, 1.f,
                 1.f, 1.f, 1.f, 1.f);

    /* The value, as a sector of cockpit_sp2 revealed from the sweep's start.
     *
     * indicatorShiftX/Y (22, 0) is DELIBERATELY NOT APPLIED. It is a real key and
     * the split-screen file's own pair for it is (172, 210) / (172, 230), which
     * reads as a POSITION rather than an offset -- and the two rings were
     * measured concentric to 0.006 of their texture's side, so striking the arc
     * 22 px off the dial would pull it off the gold track it is meant to fill.
     * hud_data.h emits it; what it is measured from has not been recovered, so it
     * is left unread rather than guessed at. See known-issues.md.
     *
     * `tex_mirror` on the right-hand dial: the screen sweep runs the other way
     * and the SAMPLE does not, because the red arc is not symmetric about the
     * ring and reflecting the sample point would read grey. */
    if (r->tex.dial_arc && frac > 0.001f)
        ui_arc(cx, cy, r0, r1,
               sw * a0, sw * (a0 + (a1 - a0) * frac), RUI_ARC_SEGS,
               r->tex.dial_arc, HUD_DIAL_CU, HUD_DIAL_CV,
               HUD_DIAL_R0, HUD_DIAL_R1, mirror,
               1.f, 1.f, 1.f, 1.f);

    /* The reading and its label, inside the disc: the number above the middle,
       a rule on it, the label under. opt_cock_all's own arrangement. */
    if (fs.tex) {
        const float vs = RUI_DIAL_VAL_SCALE * f->s;
        const float ls = RUI_DIAL_LBL_SCALE * f->s;
        const float vw = sf_w(&fs, vs, value), vh = sf_h(&fs, vs);
        const float lw = sf_w(&fs, ls, label);
        /* The whole group -- number, rule, label -- drops together, so the rule
           stays between the two. */
        const float ty = cy + RUI_DIAL_TEXT_DY * f->s;
        sf_text_shadowed(&fs, cx - vw * 0.5f, ty - vh, vs,
                         1.f, 1.f, 1.f, 1.f, value);
        ui_rect(cx - rpx * 0.42f, ty + vh * 0.06f, rpx * 0.84f,
                f->s > 1.f ? f->s : 1.f, 1.f, 1.f, 1.f, 0.75f);
        sf_text_shadowed(&fs, cx - lw * 0.5f, ty + vh * 0.20f, ls,
                         0.85f, 0.85f, 0.90f, 1.f, label);
    }
}

/* -------------------------------------------------------------------- the draw */

void race_ui_draw(const race_ui_t *r, const race_ui_state *s,
                  int screen_w, int screen_h)
{
    frame f = fr_of(screen_w, screen_h);
    char buf[32], buf2[48];
    int held = 0;
    sfont big, small_;

    if (!r || !s)
        return;
    big = sf_big(r);
    small_ = sf_small(r);

    ui_begin(screen_w, screen_h);

    draw_map(r, s, &f);

    /* THE PLACE, in place1..place6. The badge is a 128x128 quad drawn 1:1 at
       cockpit.ini's own corner, whose Y is NEGATIVE -- and right: the art
       carries 34 px of transparent margin above its word, so the ink lands on
       screen with the quad's top edge above it. */
    {
        const int p = s->place;
        if (p >= 1 && p <= 6 && r->tex.place[p - 1]) {
            ui_image(axL(&f, HUD_PLACE_X), ay(&f, HUD_PLACE_Y),
                     HUD_BADGE_SIDE * f.s, HUD_BADGE_SIDE * f.s,
                     r->tex.place[p - 1], 0.f, 0.f, 1.f, 1.f,
                     1.f, 1.f, 1.f, 1.f);
        } else if (p >= 1) {
            /* No badge art: the font, at the badge's own centre. Degrades
               rather than disappearing, like hud.c's word fallback. */
            static const char *const ORD[6] = { "1st", "2nd", "3rd",
                                                "4th", "5th", "6th" };
            const char *w = (p <= 6) ? ORD[p - 1] : "---";
            const float sc = 1.4f * f.s;
            const float bx = axL(&f, HUD_PLACE_X + HUD_BADGE_SIDE * 0.5f);
            const float by = ay(&f, HUD_PLACE_Y + HUD_BADGE_SIDE * 0.5f);
            if (big.tex)
                sf_text_shadowed(&big, bx - sf_w(&big, sc, w) * 0.5f,
                                 by - sf_h(&big, sc) * 0.5f, sc,
                                 1.f, 0.25f, 0.20f, 1.f, w);
            else
                ui_text(bx - ui_text_w(2.f * f.s, w) * 0.5f,
                        by - ui_text_h(2.f * f.s) * 0.5f, 2.f * f.s,
                        1.f, 0.25f, 0.20f, 1.f, w);
        }
    }

    /* THE LAP: the word as art, the count as text, at their own two corners. */
    if (r->tex.lap)
        ui_image(axR(&f, HUD_LAP_X), ay(&f, HUD_LAP_Y),
                 HUD_BADGE_SIDE * f.s, HUD_BADGE_SIDE * f.s,
                 r->tex.lap, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f);

    /* `%i/%i` is the exe's own format for it, at 0x56d698. */
    if (s->n_laps > 0)
        snprintf(buf, sizeof buf, "%i/%i", s->lap < 1 ? 1 : s->lap, s->n_laps);
    else
        snprintf(buf, sizeof buf, "%i", s->lap < 1 ? 1 : s->lap);
    if (big.tex)
        sf_text_shadowed(&big, axR(&f, HUD_LAPNUM_X + RUI_LAPNUM_DX),
                         ay(&f, HUD_LAPNUM_Y + RUI_LAPNUM_DY),
                         RUI_LAPNUM_SCALE * f.s, 1.f, 1.f, 1.f, 1.f, buf);
    else
        ui_text(axR(&f, HUD_LAPNUM_X + RUI_LAPNUM_DX),
                ay(&f, HUD_LAPNUM_Y + RUI_LAPNUM_DY),
                RUI_LAPNUM_SCALE * 1.5f * f.s, 1.f, 1.f, 1.f, 1.f, buf);

    /* THE TWO CLOCKS, centred across the screen at their own two Y. */
    fmt_time(buf, sizeof buf, r->t_race);
    if (big.tex)
        sf_text_shadowed(&big, axC(&f, HUD_REF_W * 0.5f)
                         - sf_w(&big, f.s, buf) * 0.5f,
                         ay(&f, HUD_TIME_TOTAL_Y), f.s,
                         1.f, 1.f, 1.f, 1.f, buf);
    else
        ui_text(axC(&f, HUD_REF_W * 0.5f)
                - ui_text_w(1.8f * f.s, buf) * 0.5f,
                ay(&f, HUD_TIME_TOTAL_Y), 1.8f * f.s,
                1.f, 1.f, 1.f, 1.f, buf);

    fmt_time(buf, sizeof buf, race_ui_lap_time(r, &held));
    snprintf(buf2, sizeof buf2, "lap %s", buf);
    {
        /* A held lap time blinks. `held` is only ever true for
           timeLapBlinkInSec (4.2 s) after a crossing. */
        float a = 1.f;
        if (held) {
            const float ph = fmodf(r->blink_t, RUI_BLINK_PERIOD * 2.f);
            a = (ph < RUI_BLINK_PERIOD) ? 1.f : 0.25f;
        }
        if (small_.tex)
            sf_text_shadowed(&small_, axC(&f, HUD_REF_W * 0.5f)
                             - sf_w(&small_, f.s, buf2) * 0.5f,
                             ay(&f, HUD_TIME_LAP_Y), f.s,
                             0.85f, 0.85f, 0.90f, a, buf2);
        else
            ui_text(axC(&f, HUD_REF_W * 0.5f)
                    - ui_text_w(f.s, buf2) * 0.5f,
                    ay(&f, HUD_TIME_LAP_Y), f.s,
                    0.85f, 0.85f, 0.90f, a, buf2);
    }

    /* THE TWO DIALS. Boost bottom left, speed bottom right, with the labels
       opt_cock_all itself carries. mph, not km/h, for the same reason: that is
       what the shipped screenshot reads. */
    {
        const float bmax = s->boost_max > 0.f ? s->boost_max : 1.f;
        const float smax = s->speed_max > 0.f ? s->speed_max : 1.f;
        snprintf(buf, sizeof buf, "%i", (int)(s->boost + 0.5f));
        draw_dial(r, &f, 0, s->boost / bmax, buf, "Boost");
        /* METRIC by default -- see rui_units. The engine's own tuning is in km/h
           (`speedBaseMax`) and the physics in m/s, so km/h is the one conversion
           that is already the game's; `opt_cock_all`'s `mph' is kept behind
           race_ui_units rather than deleted, because it IS what the original
           reads. */
        {
            const int imp = (r->units == RUI_IMPERIAL);
            const float k = imp ? RUI_MPH_PER_MS : RUI_KMH_PER_MS;
            snprintf(buf, sizeof buf, "%i", (int)(s->speed * k + 0.5f));
            draw_dial(r, &f, 1, s->speed / smax, buf, imp ? "mph" : "km/h");
        }
    }

    ui_end();
}
