/* mainmenu.c -- see mainmenu.h for the art, the layout rule and the input rule. */
#include "mainmenu.h"
#include "menu.h"           /* the SCE_CTRL_* bits, host-safe */
#include "sfont.h"
#include "ui.h"
#include "hud_data.h"       /* HUD_REF_W/H, MAP_TRACK_NAME */
#include "ai_data.h"
#include "rb_data.h"        /* AI_RACES, AI_PLAYERS -- the roster per track */
#include "tracks.h"
#include "dlg_data.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- the layout
 *
 * Every rectangle here is in the 800x600 frame the game's own screenshot is in,
 * and every one of them was MEASURED off that screenshot rather than eyeballed:
 * the red bars by thresholding r-g > 60 and r-b > 60, the green pill by g > 110
 * with r and b under it, the photographs by "not orange" (orange being
 * r-b > 60 with r >= g >= b), and the atlas cells by the alpha channel of the
 * art itself. rccars_re/menushot.c renders this file against the real textures
 * so the numbers can be checked against the picture rather than argued about.
 *
 * 800x600 is also HUD_REF_W/H -- the frame smash20.ini and smash26.ini write
 * their metrics in -- so the font and the layout are in one coordinate system.
 */

/* THE EIGHT RED ROWS. `cy` is the bar's centre, `x0` its left end; every one of
   them ends at the right edge of the screen. The pitch is a flat 45 except
   between Options and Demo play, where it is 65 -- the game's own gap, and the
   reason this is a table and not a start plus a stride. `x0` walks 583, 605,
   618, 630, 636, 638, 634, 598: that is the OVAL, whose right leg bulges out to
   its widest at row six, and it is why there are nine different wedge textures. */
static const float MM_CY[MM_N_ROWS] = {
    144.5f, 189.5f, 234.5f, 279.5f, 324.5f, 369.5f, 434.5f, 479.5f
};
static const float MM_X0[MM_N_ROWS] = {
    583.f, 605.f, 618.f, 630.f, 636.f, 638.f, 634.f, 598.f
};
#define MM_BAR_H     30.f      /* the bar's art is 30 of a 32-row cell; its strict-red core measures 23 */
#define MM_BAR_R    800.f      /* every row ends at the frame's right edge */
#define MM_TEXT_PAD   8.f      /* the label's right margin inside the bar */

/* THE WEDGE under each row. The texture is 256x64 with its plate on rows 12..50,
   so drawing the whole cell into a box `MM_WEDGE_H` tall puts the plate's top
   edge 12/64 of the way down -- which is where the bar sits. Drawn from
   MM_WEDGE_OVER to the left of the bar's own left end so the tip shows. */
#define MM_WEDGE_H   44.f
#define MM_WEDGE_TOP 12.f      /* the plate's first opaque row, of 64 */
/* THE WEDGE STARTS AT THE BAR'S OWN LEFT END and its taper is the texture's,
   not a number here: ButtonPodl_right_1's plate begins 35 px into a 256 px cell,
   _5's 102 and _9's 60, so drawing the whole cell across [bar_left, right edge]
   puts each plate's tip where that row's oval is. Starting it further left
   instead drew a silver slab out past the bar's rounded cap. */
#define MM_WEDGE_OVER 0.f

/* THE HEADER, top left: HeaderSkin's first cell, whose plate is 242 of 256 wide
   and 40 of 64 tall. Drawn off the left edge so its rounded cap is cut the way
   the screenshot's is. */
#define MM_HDR_X   -8.f
#define MM_HDR_Y    28.f
/* THE TITLE, 5 SCREEN pixels lower than the bar's own 18% put it. Asked for in
   screen pixels on a 544-tall panel, so 5 / (544/600) = 5.5 here -- every
   constant in this file is in the 800x600 frame and a raw 5 would move it 4.5
   px on the panel it was judged on. */
#define MM_HDR_TEXT_DY 5.5f
#define MM_HDR_W   262.f
#define MM_HDR_H    66.f

/* THE CAR VIEWPORT'S NUDGE, off dlgRACESUM's own (306, 254): 20 screen pixels
 * left and 30 down, asked for by eye on the 960x544 panel.
 *
 * HERE AND NOT IN dlg_data.h. Those numbers are the exe's own dialog table read
 * straight out of the binary (ui.md), and a hand tweak written into them stops
 * being recoverable data. This is the port's deviation, so it lives at the port's
 * call site.
 *
 * In the 800x600 frame like every other constant in this file, and the two axes
 * do NOT divide by the same thing: mm_box maps a centre through px() and py(),
 * which carry 960/800 across and 544/600 down. A raw -20 and +30 would move the
 * box 24 px left and 27 down on the panel it was judged on -- the same trap
 * MM_HDR_TEXT_DY records. */
#define MM_ANIMCAR_DX (-20.f / (960.f / HUD_REF_W))     /* -16.667 */
#define MM_ANIMCAR_DY ( 30.f / (544.f / HUD_REF_H))     /* +33.088 */

/* THE BADGE, top right. logoRC_Main is 128x128 with its ink at (29,15)..(117,96),
   and the screenshot puts that ink at x 706..795, y 34..115 -- so the whole
   texture goes here and the ink lands where the game puts it. */
#define MM_LOGO_X  677.f
#define MM_LOGO_Y   19.f
#define MM_LOGO_W  128.f
#define MM_LOGO_H  128.f

/* THE PLAYER CARD. The portrait's frame measured x 106..222, y 84..231. */
#define MM_FACE_X  106.f
#define MM_FACE_Y   84.f
#define MM_FACE_W  117.f
#define MM_FACE_H  148.f
#define MM_INFO_X  234.f       /* the text column beside it */
#define MM_INFO_Y   88.f

/* THE CAROUSEL. The centre photo measured x 224..431, y 270..420; the four
   others are 97 x 75 on the same centre line, at x 16, 121, 437 and 542. */
#define MM_BIG_X   224.f
#define MM_BIG_Y   270.f
#define MM_BIG_W   208.f
#define MM_BIG_H   151.f
#define MM_SM_W     97.f
#define MM_SM_H     75.f
static const float MM_SM_X[4] = { 16.f, 121.f, 437.f, 542.f };
#define MM_SM_CY   (MM_BIG_Y + MM_BIG_H * 0.5f)

/* THE TRACK NAME AND ITS FACTS, under the carousel. */
#define MM_NAME_X  232.f
#define MM_NAME_Y  446.f
#define MM_DESC_Y  482.f
#define MM_DESC_LH  20.f

/* THE GREEN RACE BUTTON, bottom left. Button_race's plate is 212 of 256 wide and
   56 of 64 tall; the screenshot's pill runs x 18..178 with its bright core at
   y 543..560, which puts the whole cell here. */
/* 18 SCREEN pixels left of where it was, which on a 960-wide panel is
   18 / (960/800) = 15 in this frame. Same conversion as MM_HDR_TEXT_DY, and the
   other way round because x maps by the width. */
#define MM_RACE_X   (14.f - 15.f)
#define MM_RACE_Y  528.f
#define MM_RACE_W  200.f
#define MM_RACE_H   50.f
#define MM_RACE_ANIM 0.28f     /* seconds per frame of the little car */

/* THE QUIT BUTTON, bottom right: the silver bar out of HeaderSkin's third cell
   with Button_back's orange bar on top of it, which is what the screenshot has. */
#define MM_SIL_X   596.f
#define MM_SIL_Y   566.f
#define MM_SIL_W   210.f
#define MM_SIL_H    42.f
#define MM_QUIT_X  650.f
#define MM_QUIT_Y  576.f
#define MM_QUIT_W  152.f
#define MM_QUIT_H   24.f

/* THE PHOTOGRAPHS AND THE PORTRAIT PAD THEMSELVES INTO A POWER OF TWO, and both
 * carry their OWN silver frame -- so the sub-rect below is the whole framed
 * picture and nothing here draws a border of its own. Measured off the alpha of
 * the shipped art, and the same for all ten shots and all nine faces:
 *
 *   shot_<track>_0   256x256, ink at x 28..227, y 53..202 -- 200 x 150, 4:3
 *   the FacesSys TARGAs 128x256, ink at x  5..122, y  3..152 -- 118 x 150
 *
 * Sampling 0..1 instead drew the padding, which is why the first picture had a
 * photograph floating inside a slab of nothing.
 */
#define MM_SHOT_U0 (27.f / 256.f)
#define MM_SHOT_V0 (52.f / 256.f)
#define MM_SHOT_U1 (229.f / 256.f)
#define MM_SHOT_V1 (204.f / 256.f)
#define MM_FACE_U0 (4.f / 128.f)
#define MM_FACE_V0 (2.f / 256.f)
#define MM_FACE_U1 (124.f / 128.f)
#define MM_FACE_V1 (154.f / 256.f)

/* THE TEXT SIZES, MEASURED OFF THE REAL MENU rather than picked to fit.
 *
 * The engine draws its own font at very nearly the pixel size smash20.ini and
 * smash26.ini name -- i.e. scale 1.0 in this 800x600 frame. Read off the
 * screenshot by taking the columns that carry a TEXT STROKE (3 or more white
 * rows, but not the full band, which is the bar's own highlight running the
 * width of it) and dividing by what sf_w returns at scale 1:
 *
 *   "Welcome, Player"               119 px against 117.3   -> 1.01
 *   "Start the race along the sea"  221 px against 198.9   -> 1.11
 *   and every line's INK measures 19 to 25 px, which is a 28 px cell at ~1.0
 *
 * The first build guessed 0.76 and 0.95 and came out 30% and 14% small -- which
 * is most of why the port's menu read as muddy beside the original: a glyph
 * drawn under its own art size is resampled down and the outline the artists
 * baked into it closes up.
 */
#define MM_TS_LABEL   1.02f    /* the nine buttons */
#define MM_TS_HEAD    1.10f    /* "Main menu" */
#define MM_TS_WELCOME 1.02f    /* the card's heading */
#define MM_TS_INFO    0.98f    /* the card's lines */
#define MM_TS_TRACK   1.15f    /* the track's name */
#define MM_TS_RACE    1.15f    /* the green button */

/* ButtonsTextures' six 256x32 cells, as V ranges over the 256 px atlas. */
#define MM_V_RED     (0.f / 256.f)
#define MM_V_RED_F   (32.f / 256.f)
#define MM_V_ORANGE  (96.f / 256.f)
#define MM_V_ORANGE_F (128.f / 256.f)
#define MM_V_GREY    (192.f / 256.f)
#define MM_V_CELL    (32.f / 256.f)

/* THE FRAME IS 768 x 768 AND IT DOES NOT STRETCH TO THE WINDOW.
 *
 * The four Podl tiles reassemble into one 768x768 picture (mainmenu.h), and the
 * obvious thing to do with it is to stretch it over the screen. The game does
 * not: FITTED against the 800x600 screenshot over every background pixel no
 * widget covers, sweeping width 744..808 and height 560..816, the best fit is
 * 772 x 768 -- i.e. the tiles at their own pixel size, anchored top left, with
 * the right 32 columns left as bare Desktop and the bottom 168 rows simply off
 * the screen. Stretching to 800x600 instead put the oval's right leg 37 px too
 * far right, which is the width of a button's rounded cap, and the first picture
 * had the silver line running THROUGH the buttons.
 *
 * So it is 768 DESIGN pixels here, and the port's own px/py carry it from there
 * -- which reproduces the original exactly at 800x600 and stretches it with
 * everything else at 960x544.
 */
#define MM_FRAME 768.f
#define MM_TILE  512.f

/* ---------------------------------------------------------------- the frame
 *
 * `px`/`py` stretch with the art; `us` is the uniform scale for anything that
 * must not be distorted. See mainmenu.h -- the split between the two IS the
 * resizing rule.
 */
typedef struct { float w, h, sx, sy, us; } mmframe;

static mmframe mm_frame(int screen_w, int screen_h)
{
    mmframe f;
    f.w = (float)screen_w;
    f.h = (float)screen_h;
    f.sx = f.w / HUD_REF_W;
    f.sy = f.h / HUD_REF_H;
    f.us = f.sy;
    return f;
}
static float px(const mmframe *f, float x) { return x * f->sx; }
static float py(const mmframe *f, float y) { return y * f->sy; }

/* A box whose POSITION follows the art and whose SIZE stays square. The centre
   is mapped, then the box is grown about it at the uniform scale -- so a
   photograph lands where the artists put it without being stretched into a
   different photograph. */
static void mm_box(const mmframe *f, float x, float y, float w, float h,
                   float *ox, float *oy, float *ow, float *oh)
{
    const float cx = px(f, x + w * 0.5f), cy = py(f, y + h * 0.5f);
    *ow = w * f->us;
    *oh = h * f->us;
    *ox = cx - *ow * 0.5f;
    *oy = cy - *oh * 0.5f;
}

/* A GROUP: several boxes that belong together, laid out about ONE mapped anchor
 * with UNIFORM offsets between them.
 *
 * mm_box on its own maps every box's centre through px() and then sizes it with
 * us, which is right for one box and wrong for a row of them: on a 960x544 panel
 * the centres spread by 1.2 while the boxes shrink by 0.907, so the carousel's
 * 8 px gaps opened to 37 and the five photographs came apart. The card did the
 * same between the portrait and the text beside it.
 *
 * `ax` is the group's own anchor in design space -- for the carousel, the centre
 * of the row, which is also the centre of the big photograph. Everything else in
 * the group is placed at its design offset from that, times us. The group as a
 * whole still follows the art; only its insides stop stretching.
 */
static float gx(const mmframe *f, float ax, float x)
{
    return px(f, ax) + (x - ax) * f->us;
}

static void mm_gbox(const mmframe *f, float ax, float x, float y,
                    float w, float h,
                    float *ox, float *oy, float *ow, float *oh)
{
    *ox = gx(f, ax, x);
    *oy = py(f, y + h * 0.5f) - h * f->us * 0.5f;
    *ow = w * f->us;
    *oh = h * f->us;
}

/* The carousel's anchor: the middle of the row, which the row is symmetric
   about -- 16 to 639 in design space, and the big photograph is 224 to 432. */
#define MM_GROUP_X ((16.f + 639.f) * 0.5f)

/* ------------------------------------------------- the quick-race page
 *
 * ON dlgRACESUM.ini's OWN RECTANGLES, through dlg_data.h. `Settings/` ships the
 * layout of all 35 dialogs and this page was built before that was found -- it
 * had the four enums as bars in the main menu's right-hand column, which is not
 * where the game puts any of them. What the game puts there is the page's own
 * NAVIGATION: Race summary (this page), Map and info, Track stats and Garage,
 * three of which are dialogs the exe carries and this port does not build.
 *
 * AN ENUM IS FOUR THINGS, and `SE' is the one that is not obvious. The .ini
 * gives X0/Y0/SX/SY and an `SE' percentage:
 *
 *   X0 ............ SE ...... SX
 *   |  label        | [<] value [>]
 *
 * SE splits the widget: the label runs from X0, the BACK arrow's left edge sits
 * at X0 + SX*SE, and the FORWARD arrow's right edge at X0 + SX, with the value
 * centred between them. enumNLaps and enumSkill split at 44%; enumTrack and
 * enumCar have SE 0, so they are all value -- the photograph and the car sit
 * between their two arrows and there is no label at all. Read off the game's own
 * screenshot: its laps arrows land at 313 and 565, which is 115 + 450*0.44 and
 * 115 + 450 exactly.
 */

/* The arrows are drawn larger than the row they belong to -- 34 design px
   against a 25 px rect, measured off the screenshot, and centred on it. */
/* enumarrows is 64x64 = four 32x32 cells: silver, red across the top, grey
   below. A forward arrow is the same cell with its U reversed. */
#define MM_AR_HALF 0.5f

#define MM_Q_ARROW 34.f

/* The small marker beside a labelled row, which the game draws on the two rows
   that have labels and on neither of the two that do not. */
#define MM_Q_BULLET 14.f
#define MM_Q_BULLET_GAP 20.f

/* The page's own right-hand column sits in the main menu's first four measured
   row positions, so its bars and wedges land on the oval with nothing new to
   get wrong -- the one thing here that is the port's and not the .ini's. */
static const char *const MM_QB_NAME[MM_QB_N] = {
    "Race summary", "Map and info", "Track stats", "Garage"
};

/* Each enum's rectangle and its SE split, straight out of dlgRACESUM.ini. */
static const float MM_Q_RECT[MM_Q_N_ROWS][5] = {
    { DLG_RACESUM_enumTrackX0, DLG_RACESUM_enumTrackY0,
      DLG_RACESUM_enumTrackSX, DLG_RACESUM_enumTrackSY,
      DLG_RACESUM_enumTrackSE },
    { DLG_RACESUM_enumNLapsX0, DLG_RACESUM_enumNLapsY0,
      DLG_RACESUM_enumNLapsSX, DLG_RACESUM_enumNLapsSY,
      DLG_RACESUM_enumNLapsSE },
    { DLG_RACESUM_enumSkillX0, DLG_RACESUM_enumSkillY0,
      DLG_RACESUM_enumSkillSX, DLG_RACESUM_enumSkillSY,
      DLG_RACESUM_enumSkillSE },
    { DLG_RACESUM_enumCarX0, DLG_RACESUM_enumCarY0,
      DLG_RACESUM_enumCarSX, DLG_RACESUM_enumCarSY,
      DLG_RACESUM_enumCarSE },
};

static const char *const MM_Q_LABEL[MM_Q_N_ROWS] = {
    /* enumTrack and enumCar have no label of their own -- the photograph and
       the car ARE the value, and the word `Car' on screen is staticCarName,
       which is a control in its own right. */
    "", "Number of laps", "Opponents skill", ""
};

const int MM_LAPS[MM_N_LAPS] = { 3, 5, 7 };

static const char *const MM_SKILL[MM_N_SKILL] = {
    "Easy", "Normal", "Hard", "Ultra"
};

const char *mainmenu_skill_name(int skill)
{
    if (skill < 0 || skill >= MM_N_SKILL) return "?";
    return MM_SKILL[skill];
}

int mainmenu_field_size(int track, int skill)
{
    const ai_race *r;
    int mask, i, n = 0;
    if (track < 0 || track >= AI_N_RACES)
        return 0;
    r = &AI_RACES[track];
    /* ai.c's own rule: 1 << difficulty, with ultra riding on hard because
       ailayouts.ini declares only three bits. */
    mask = 1 << (skill < 0 ? 0 : skill);
    if (mask > AI_RACE_HARD)
        mask = AI_RACE_HARD;
    for (i = 0; i < r->n; i++)
        if (r->op[i].races & mask)
            n++;
    return n;
}

/* ------------------------------------------------------------------- rows */

static const char *const MM_NAME[MM_N_ROWS] = {
    "Championship", "Quick race", "Ghost race", "Multiplayer",
    "Change player", "Options", "Demo play", "Credits"
};

int mainmenu_row_live(int row)
{
    return row == MM_QUICK_RACE || row == MM_OPTIONS || row == MM_CREDITS;
}

const char *mainmenu_row_name(int row)
{
    if (row == MM_FOCUS_RACE) return "Race";
    if (row == MM_FOCUS_QUIT) return "Quit";
    if (row < 0 || row >= MM_N_ROWS) return "?";
    return MM_NAME[row];
}

/* The bar's rect on screen. The wedge under it is taller and starts further
   left; the HIT BOX is the bar, because that is what the player is aiming at. */
static void mm_bar_rect(const mmframe *f, int row,
                        float *x, float *y, float *w, float *h)
{
    *x = px(f, MM_X0[row]);
    *y = py(f, MM_CY[row] - MM_BAR_H * 0.5f);
    *w = px(f, MM_BAR_R) - *x;
    *h = py(f, MM_CY[row] + MM_BAR_H * 0.5f) - *y;
}

static void mm_race_rect(const mmframe *f, float *x, float *y,
                         float *w, float *h)
{
    *x = px(f, MM_RACE_X);
    *y = py(f, MM_RACE_Y);
    *w = px(f, MM_RACE_X + MM_RACE_W) - *x;
    *h = py(f, MM_RACE_Y + MM_RACE_H) - *y;
}

static void mm_quit_rect(const mmframe *f, float *x, float *y,
                         float *w, float *h)
{
    *x = px(f, MM_QUIT_X);
    *y = py(f, MM_QUIT_Y);
    *w = px(f, MM_QUIT_X + MM_QUIT_W) - *x;
    *h = py(f, MM_QUIT_Y + MM_QUIT_H) - *y;
}

/* The carousel shows five tracks centred on the selection: two behind, the
   selection, two ahead, wrapping. `slot` is 0..4 left to right. */
static int mm_slot_track(const mainmenu_t *m, int slot)
{
    int t = m->track + (slot - 2);
    while (t < 0) t += N_TRACKS;
    while (t >= N_TRACKS) t -= N_TRACKS;
    return t;
}

static void mm_slot_rect(const mmframe *f, int slot,
                         float *x, float *y, float *w, float *h)
{
    if (slot == 2) {
        mm_gbox(f, MM_GROUP_X, MM_BIG_X, MM_BIG_Y, MM_BIG_W, MM_BIG_H,
                x, y, w, h);
    } else {
        const int i = slot < 2 ? slot : slot - 1;
        mm_gbox(f, MM_GROUP_X, MM_SM_X[i], MM_SM_CY - MM_SM_H * 0.5f,
                MM_SM_W, MM_SM_H, x, y, w, h);
    }
}

int mainmenu_row_at(const mainmenu_t *m, int screen_w, int screen_h,
                    float x, float y)
{
    const mmframe f = mm_frame(screen_w, screen_h);
    float bx, by, bw, bh;
    int i;

    (void)m;
    for (i = 0; i < MM_N_ROWS; i++) {
        mm_bar_rect(&f, i, &bx, &by, &bw, &bh);
        if (touch_in(x, y, bx, by, bw, bh))
            return i;
    }
    mm_race_rect(&f, &bx, &by, &bw, &bh);
    if (touch_in(x, y, bx, by, bw, bh))
        return MM_FOCUS_RACE;
    mm_quit_rect(&f, &bx, &by, &bw, &bh);
    if (touch_in(x, y, bx, by, bw, bh))
        return MM_FOCUS_QUIT;
    return -1;
}

/* An enum's two arrows on screen. `back' is the one at the SE split, `fwd' the
   one at the far end; both are MM_Q_ARROW square and centred on the row. */
static void mm_q_arrows(const mmframe *f, int row,
                        float *bx, float *fx, float *ay, float *sz)
{
    const float *R = MM_Q_RECT[row];
    *sz = MM_Q_ARROW * f->us;
    *ay = py(f, R[1] + R[3] * 0.5f) - *sz * 0.5f;
    *bx = px(f, R[0] + R[2] * R[4]);
    *fx = px(f, R[0] + R[2]) - *sz;
}

int mainmenu_q_row_at(const mainmenu_t *m, int screen_w, int screen_h,
                      float x, float y, int *left)
{
    const mmframe f = mm_frame(screen_w, screen_h);
    float bx, by, bw, bh;
    int i;

    (void)m;
    if (left) *left = 0;
    for (i = 0; i < MM_Q_N_ROWS; i++) {
        float ax, fx, ay, sz;
        mm_q_arrows(&f, i, &ax, &fx, &ay, &sz);
        /* THE ARROWS ARE THE BUTTONS, and each is grown by half its own size so
           a thumb has something to hit -- 34 design px is 31 on a 544-tall
           panel, which is under the 9 mm a fingertip covers. The two boxes
           cannot meet: the narrowest enum is enumCar's 304 px between them. */
        if (touch_in(x, y, ax - sz * 0.25f, ay - sz * 0.25f,
                     sz * 1.5f, sz * 1.5f)) {
            if (left) *left = 1;
            return i;
        }
        if (touch_in(x, y, fx - sz * 0.25f, ay - sz * 0.25f,
                     sz * 1.5f, sz * 1.5f))
            return i;
    }
    mm_race_rect(&f, &bx, &by, &bw, &bh);
    if (touch_in(x, y, bx, by, bw, bh))
        return MM_Q_RACE;
    mm_quit_rect(&f, &bx, &by, &bw, &bh);
    if (touch_in(x, y, bx, by, bw, bh))
        return MM_Q_BACK;
    return -1;
}

int mainmenu_slot_at(const mainmenu_t *m, int screen_w, int screen_h,
                     float x, float y)
{
    const mmframe f = mm_frame(screen_w, screen_h);
    int slot;
    (void)m;
    for (slot = 0; slot < 5; slot++) {
        float bx, by, bw, bh;
        mm_slot_rect(&f, slot, &bx, &by, &bw, &bh);
        if (touch_in(x, y, bx, by, bw, bh))
            return slot;
    }
    return -1;
}

static int mm_focus_live(int focus)
{
    if (focus == MM_FOCUS_RACE || focus == MM_FOCUS_QUIT)
        return 1;
    return mainmenu_row_live(focus);
}

/* Next live stop in the ring, `dir` +1 down / -1 up. The dead rows are skipped
   rather than landed on and refused: a focus that stops somewhere it cannot act
   reads as the pad being broken. */
static int mm_next_focus(int focus, int dir)
{
    int i;
    for (i = 0; i < MM_N_FOCUS; i++) {
        focus += dir;
        while (focus < 0) focus += MM_N_FOCUS;
        while (focus >= MM_N_FOCUS) focus -= MM_N_FOCUS;
        if (mm_focus_live(focus))
            return focus;
    }
    return focus;
}

void mainmenu_set_car_draw(mainmenu_t *m, mm_car_draw fn, void *ctx)
{
    if (!m)
        return;
    m->car_draw = fn;
    m->car_ctx = ctx;
}

void mainmenu_init(mainmenu_t *m, const mainmenu_tex *tex)
{
    if (!m)
        return;
    memset(m, 0, sizeof(*m));
    if (tex)
        m->tex = *tex;
    m->focus = MM_QUICK_RACE;      /* the one live mode */
    m->qfocus = MM_Q_TRACK;
    m->laps = MM_LAPS_DEF;
    m->skill = 1;                  /* normal, which is what ai_init was passed */
    m->armed = -1;
}

/* ------------------------------------------------------------------ a step */

static void mm_fire(mainmenu_t *m, int focus)
{
    switch (focus) {
    case MM_QUICK_RACE:
        /* THE SETUP SCREEN, not the race. dlgRACESUM is what this button opens
           in the original; the green Race button below is what starts one, on
           this page and on that one. */
        m->page = MM_PAGE_QUICK;
        m->cue = MM_CUE_PRESS;
        break;
    case MM_FOCUS_RACE:
        m->action = MM_ACT_RACE;
        m->cue = MM_CUE_PRESS;
        break;
    case MM_OPTIONS:
        m->action = MM_ACT_OPTIONS;
        m->cue = MM_CUE_PRESS;
        break;
    case MM_CREDITS:
        m->credits = !m->credits;
        m->cue = MM_CUE_PRESS;
        break;
    case MM_FOCUS_QUIT:
        m->action = MM_ACT_QUIT;
        m->cue = MM_CUE_PRESS;
        break;
    default:
        m->cue = MM_CUE_DENY;
        break;
    }
}

static void mm_move_track(mainmenu_t *m, int dir)
{
    m->track += dir;
    while (m->track < 0) m->track += N_TRACKS;
    while (m->track >= N_TRACKS) m->track -= N_TRACKS;
    m->cue = MM_CUE_ARROW;
}

/* One step of an enum: `d` is +1 or -1, wrapping. Every row wraps, including
   Laps -- one press past nine is one, which is what every picker in menu.c does
   except the two volumes, and for the same reason: there is no "off" here. */
static void mm_q_move(mainmenu_t *m, int row, int d)
{
    switch (row) {
    case MM_Q_TRACK:
        m->track += d;
        while (m->track < 0) m->track += N_TRACKS;
        while (m->track >= N_TRACKS) m->track -= N_TRACKS;
        break;
    case MM_Q_LAPS: {
        int k, at = 0;
        for (k = 0; k < MM_N_LAPS; k++)
            if (MM_LAPS[k] == m->laps) { at = k; break; }
        m->laps = MM_LAPS[(at + d + MM_N_LAPS) % MM_N_LAPS];
        break;
    }
    case MM_Q_SKILL:
        m->skill = (m->skill + d + MM_N_SKILL) % MM_N_SKILL;
        break;
    case MM_Q_CAR:
        m->car = (m->car + d + MM_N_CARS) % MM_N_CARS;
        break;
    default:
        return;
    }
    m->cue = MM_CUE_ARROW;
}

static void mm_step_quick(mainmenu_t *m, unsigned int down,
                          const touch_state *tp, int screen_w, int screen_h)
{
    if (down & SCE_CTRL_DOWN) {
        m->qfocus = (m->qfocus + 1) % MM_Q_N_FOCUS;
        m->cue = MM_CUE_FOCUS;
    }
    if (down & SCE_CTRL_UP) {
        m->qfocus = (m->qfocus + MM_Q_N_FOCUS - 1) % MM_Q_N_FOCUS;
        m->cue = MM_CUE_FOCUS;
    }
    if (down & SCE_CTRL_LEFT)  mm_q_move(m, m->qfocus, -1);
    if (down & SCE_CTRL_RIGHT) mm_q_move(m, m->qfocus, +1);
    if (down & (SCE_CTRL_CROSS | SCE_CTRL_START)) {
        if (m->qfocus == MM_Q_RACE) {
            m->action = MM_ACT_RACE;
            m->cue = MM_CUE_PRESS;
        } else if (m->qfocus == MM_Q_BACK) {
            m->page = MM_PAGE_MAIN;
            m->cue = MM_CUE_PRESS;
        } else {
            mm_q_move(m, m->qfocus, +1);   /* CROSS on a picker steps it */
        }
    }
    /* CIRCLE is back, everywhere. */
    if (down & SCE_CTRL_CIRCLE) {
        m->page = MM_PAGE_MAIN;
        m->cue = MM_CUE_PRESS;
    }

    if (!tp)
        return;
    if (tp->pressed) {
        int left;
        m->armed = mainmenu_q_row_at(m, screen_w, screen_h,
                                     tp->x, tp->y, &left);
        if (m->armed >= 0 && m->qfocus != m->armed) {
            m->qfocus = m->armed;
            m->cue = MM_CUE_FOCUS;
        }
    }
    if (tp->released) {
        int left;
        const int row = mainmenu_q_row_at(m, screen_w, screen_h,
                                          tp->x, tp->y, &left);
        if (row >= 0 && row == m->armed) {
            if (row == MM_Q_RACE) {
                m->action = MM_ACT_RACE;
                m->cue = MM_CUE_PRESS;
            } else if (row == MM_Q_BACK) {
                m->page = MM_PAGE_MAIN;
                m->cue = MM_CUE_PRESS;
            } else {
                /* THE BAR'S OWN TRIANGLE GOES BACK, the rest goes on -- so one
                   row walks both ways under a thumb and neither direction needs
                   a second control. */
                mm_q_move(m, row, left ? -1 : +1);
            }
        }
        m->armed = -1;
    }
}

void mainmenu_step(mainmenu_t *m, unsigned int buttons, const touch_state *tp,
                   int screen_w, int screen_h, float dt)
{
    unsigned int down;

    if (!m)
        return;
    m->action = MM_ACT_NONE;
    m->cue = MM_CUE_NONE;
    m->t += dt;

    down = buttons & ~m->prev_buttons;   /* edges only */
    m->prev_buttons = buttons;

    /* THE CREDITS PANEL EATS EVERYTHING. Any button and any touch closes it,
       which is what a panel with nothing to choose in it should do. */
    if (m->credits) {
        if (down || (tp && tp->released)) {
            m->credits = 0;
            m->cue = MM_CUE_PRESS;
        }
        return;
    }

    if (m->page == MM_PAGE_QUICK) {
        mm_step_quick(m, down, tp, screen_w, screen_h);
        return;
    }

    if (down & SCE_CTRL_DOWN) {
        m->focus = mm_next_focus(m->focus, +1);
        m->cue = MM_CUE_FOCUS;
    }
    if (down & SCE_CTRL_UP) {
        m->focus = mm_next_focus(m->focus, -1);
        m->cue = MM_CUE_FOCUS;
    }
    if (down & SCE_CTRL_LEFT)
        mm_move_track(m, -1);
    if (down & SCE_CTRL_RIGHT)
        mm_move_track(m, +1);
    if (down & (SCE_CTRL_CROSS | SCE_CTRL_START))
        mm_fire(m, m->focus);

    if (!tp)
        return;

    /* TOUCH. The press ARMS a row and the release fires it, and only if both
       landed on the same one -- see touch.h. The focus follows the finger on the
       way down so that the pad and the panel never light different buttons. */
    if (tp->pressed) {
        int slot;
        m->armed = mainmenu_row_at(m, screen_w, screen_h, tp->x, tp->y);
        if (m->armed >= 0 && mm_focus_live(m->armed)) {
            if (m->focus != m->armed)
                m->cue = MM_CUE_FOCUS;
            m->focus = m->armed;
        }
        /* A PHOTOGRAPH IS A BUTTON TOO, and the centre one is not: tapping the
           selection again does nothing, tapping a neighbour walks the carousel
           that far. Two taps to reach the far slots is the same number of
           presses the pad takes, which is what makes them agree. */
        slot = mainmenu_slot_at(m, screen_w, screen_h, tp->x, tp->y);
        if (slot >= 0 && slot != 2) {
            m->track = mm_slot_track(m, slot);
            m->cue = MM_CUE_ARROW;
        }
    }
    if (tp->released) {
        const int row = mainmenu_row_at(m, screen_w, screen_h, tp->x, tp->y);
        if (row >= 0 && row == m->armed)
            mm_fire(m, row);
        m->armed = -1;
    }
}

/* -------------------------------------------------------------------- draw */

/* The frame: the graffiti ground over everything, then the four tiles of the one
   768x768 picture at their own size, anchored top left. See MM_FRAME. */
static void mm_draw_frame(const mainmenu_t *m, const mmframe *f)
{
    const float x0 = 0.f, y0 = 0.f;
    const float xc = px(f, MM_TILE), yc = py(f, MM_TILE);
    const float x1 = px(f, MM_FRAME), y1 = py(f, MM_FRAME);

    /* The ground covers the WHOLE screen, because the frame does not: at
       800x600 there are 32 columns of it beyond the frame's right edge, and on
       a 16:9 panel there are more. */
    if (m->tex.desktop)
        ui_image(0.f, 0.f, f->w, f->h, m->tex.desktop,
                 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f);
    else
        ui_rect(0.f, 0.f, f->w, f->h, 0.86f, 0.60f, 0.06f, 1.f);

    if (m->tex.podl_lt)
        ui_image(x0, y0, xc - x0, yc - y0, m->tex.podl_lt,
                 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f);
    if (m->tex.podl_rt)
        ui_image(xc, y0, x1 - xc, yc - y0, m->tex.podl_rt,
                 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f);
    if (m->tex.podl_lb)
        ui_image(x0, yc, xc - x0, y1 - yc, m->tex.podl_lb,
                 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f);
    if (m->tex.podl_rb)
        ui_image(xc, yc, x1 - xc, y1 - yc, m->tex.podl_rb,
                 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f);
}

/* A label right-aligned inside a bar, in the engine's own letters, falling back
   to the compiled-in Consolas when the atlas did not load. */
static void mm_label(const mainmenu_t *m, const mmframe *f,
                     float bx, float by, float bw, float bh,
                     const char *s, float r, float g, float b)
{
    const sfont sf = sf_small(m->tex.font_small);
    const float pad = MM_TEXT_PAD * f->us;

    if (sf.tex) {
        const float sc = f->us * MM_TS_LABEL;
        const float w = sf_w(&sf, sc, s), h = sf_h(&sf, sc);
        /* PLAIN, not sf_text_shadowed. The HUD's second copy is there because
           it sits on sand and asphalt; these letters sit on a red or a grey
           bar and the atlas already carries its own dark outline, so a black
           copy offset under them only smears the edge the artists drew. */
        sf_text(&sf, bx + bw - pad - w, by + (bh - h) * 0.5f, sc,
                r, g, b, 1.f, s);
    } else {
        const float sc = f->us * MM_TS_LABEL;
        const float w = ui_text_w(sc, s), h = ui_text_h(sc);
        ui_text(bx + bw - pad - w, by + (bh - h) * 0.5f, sc, r, g, b, 1.f, s);
    }
}

static void mm_draw_rows(const mainmenu_t *m, const mmframe *f)
{
    int i;
    for (i = 0; i < MM_N_ROWS; i++) {
        float bx, by, bw, bh, v;
        const int live = mainmenu_row_live(i);
        const int lit = live && m->focus == i;

        mm_bar_rect(f, i, &bx, &by, &bw, &bh);

        /* The wedge first: it is behind the bar and taller, and its plate's top
           row (12 of 64) is what the bar's top edge sits on. */
        if (m->tex.wedge[i]) {
            const float wh = py(f, MM_WEDGE_H);
            const float wx = bx - px(f, MM_WEDGE_OVER);
            ui_image(wx, by - wh * (MM_WEDGE_TOP / 64.f),
                     px(f, MM_BAR_R) - wx, wh, m->tex.wedge[i],
                     0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f);
        }

        v = !live ? MM_V_GREY : (lit ? MM_V_RED_F : MM_V_RED);
        if (m->tex.buttons)
            ui_image(bx, by, bw, bh, m->tex.buttons,
                     0.f, v, 1.f, v + MM_V_CELL, 1.f, 1.f, 1.f, 1.f);
        else
            ui_rect(bx, by, bw, bh,
                    live ? 0.72f : 0.45f, live ? 0.09f : 0.45f,
                    live ? 0.11f : 0.47f, 1.f);

        /* THE DEAD ROWS ARE DIMMED IN THE TEXT TOO. The grey bar alone still
           reads as a button on a bright orange field; grey letters on it say
           "nothing here" at a glance, which is the whole point of drawing them
           rather than hiding them. */
        /* THE DEAD ROWS ARE DIMMED, but only a little: at 0.62 the grey letters
           and the grey bar ran together and the row could not be read at all.
           0.82 is still visibly duller than a live row and is still a label. */
        mm_label(m, f, bx, by, bw, bh, MM_NAME[i],
                 live ? 1.f : 0.82f, live ? 1.f : 0.82f, live ? 1.f : 0.84f);
    }
}

static void mm_draw_race(const mainmenu_t *m, const mmframe *f)
{
    float x, y, w, h;
    int cell;

    mm_race_rect(f, &x, &y, &w, &h);
    /* 0 disabled, 1-2 the normal pair, 3-4 the focused pair -- the pairs are
       frames of the little car's wheels, not two moods. */
    cell = ((m->page == MM_PAGE_QUICK) ? (m->qfocus == MM_Q_RACE)
                                       : (m->focus == MM_FOCUS_RACE))
           ? 3 : 1;
    if ((int)(m->t / MM_RACE_ANIM) & 1)
        cell += 1;
    if (m->tex.race)
        ui_image(x, y, w, h, m->tex.race,
                 0.f, cell * (64.f / 512.f), 1.f, (cell + 1) * (64.f / 512.f),
                 1.f, 1.f, 1.f, 1.f);
    else
        ui_rect(x, y, w, h, 0.05f, 0.55f, 0.08f, 1.f);
    {
        /* The word sits left of the car icon, which is the art's own right end. */
        const sfont sf = sf_big(m->tex.font_big);
        const float sc = f->us * MM_TS_RACE;
        const float tx = x + w * 0.30f;
        if (sf.tex)
            sf_text(&sf, tx, y + (h - sf_h(&sf, sc)) * 0.5f, sc,
                    1.f, 1.f, 1.f, 1.f, "Race");
        else
            ui_text(tx, y + (h - ui_text_h(sc)) * 0.5f, sc,
                    1.f, 1.f, 1.f, 1.f, "Race");
    }
}

static void mm_draw_quit(const mainmenu_t *m, const mmframe *f)
{
    float x, y, w, h;
    const int lit = (m->page == MM_PAGE_QUICK)
                    ? (m->qfocus == MM_Q_BACK)
                    : (m->focus == MM_FOCUS_QUIT);

    /* HeaderSkin's third cell is the plain silver bar the orange one sits on. */
    if (m->tex.header)
        ui_image(px(f, MM_SIL_X), py(f, MM_SIL_Y),
                 px(f, MM_SIL_X + MM_SIL_W) - px(f, MM_SIL_X),
                 py(f, MM_SIL_Y + MM_SIL_H) - py(f, MM_SIL_Y),
                 m->tex.header, 0.f, 128.f / 256.f, 1.f, 192.f / 256.f,
                 1.f, 1.f, 1.f, 1.f);

    mm_quit_rect(f, &x, &y, &w, &h);
    if (m->tex.back)
        ui_image(x, y, w, h, m->tex.back,
                 0.f, lit ? 32.f / 128.f : 0.f,
                 1.f, lit ? 64.f / 128.f : 32.f / 128.f, 1.f, 1.f, 1.f, 1.f);
    else
        ui_rect(x, y, w, h, 0.90f, 0.55f, 0.04f, 1.f);
    mm_label(m, f, x, y, w, h,
             m->page == MM_PAGE_QUICK ? "Main menu" : "Quit",
             1.f, 1.f, 1.f);
}

static void mm_draw_header(const mainmenu_t *m, const mmframe *f)
{
    const float x = px(f, MM_HDR_X), y = py(f, MM_HDR_Y);
    const float w = px(f, MM_HDR_X + MM_HDR_W) - x;
    const float h = py(f, MM_HDR_Y + MM_HDR_H) - y;

    if (m->tex.header)
        ui_image(x, y, w, h, m->tex.header, 0.f, 0.f, 1.f, 64.f / 256.f,
                 1.f, 1.f, 1.f, 1.f);
    {
        const sfont sf = sf_big(m->tex.font_big);
        const float sc = f->us * MM_TS_HEAD;
        const float tx = x + w * 0.24f;
        const char *title = (m->page == MM_PAGE_QUICK) ? "Quick race"
                                                       : "Main menu";
        const float ty = y + h * 0.18f + py(f, MM_HDR_TEXT_DY);
        if (sf.tex)
            sf_text(&sf, tx, ty, sc, 1.f, 1.f, 1.f, 1.f, title);
        else
            ui_text(tx, ty, sc, 1.f, 1.f, 1.f, 1.f, title);
    }
    if (m->tex.logo) {
        float lx, ly, lw, lh;
        mm_box(f, MM_LOGO_X, MM_LOGO_Y, MM_LOGO_W, MM_LOGO_H,
               &lx, &ly, &lw, &lh);
        ui_image(lx, ly, lw, lh, m->tex.logo, 0.f, 0.f, 1.f, 1.f,
                 1.f, 1.f, 1.f, 1.f);
    }
}

/* A thin rule under a heading, which is what the screenshot puts there. */
static void mm_rule_at(const mmframe *f, float x, float y, float w)
{
    ui_rect(x, y, w, f->us * 1.5f, 1.f, 1.f, 1.f, 0.55f);
}

static void mm_draw_card(const mainmenu_t *m, const mmframe *f)
{
    const sfont big = sf_big(m->tex.font_big);
    const sfont small_ = sf_small(m->tex.font_small);
    float x, y, w, h;
    float tx, ty;
    char line[96];

    /* ON THE SAME ANCHOR as the carousel, so the whole left block is one group:
       the card's text column and the track's name under the photographs line
       up in the original, and they only keep lining up if the portrait, the
       row and the caption all move together. */
    mm_gbox(f, MM_GROUP_X, MM_FACE_X, MM_FACE_Y, MM_FACE_W, MM_FACE_H,
            &x, &y, &w, &h);
    /* THE COLUMN HANGS OFF THE PORTRAIT rather than being mapped on its own, so
       the gap between the two stays the gap the artists left. See gx(). */
    tx = x + w + (MM_INFO_X - (MM_FACE_X + MM_FACE_W)) * f->us;
    /* The portrait brings its own silver frame -- see MM_FACE_U0. A missing one
       leaves a plate where the layout says the card is, so the row of text
       beside it still reads as a card. */
    if (m->tex.face)
        ui_image(x, y, w, h, m->tex.face,
                 MM_FACE_U0, MM_FACE_V0, MM_FACE_U1, MM_FACE_V1,
                 1.f, 1.f, 1.f, 1.f);
    else
        ui_rect(x, y, w, h, 0.20f, 0.22f, 0.28f, 1.f);

    ty = MM_INFO_Y;
    if (big.tex) {
        const float sc = f->us * MM_TS_WELCOME;
        sf_text_shadowed(&big, tx, py(f, ty), sc,
                         1.f, 1.f, 1.f, 1.f, "Welcome, Player");
    } else {
        ui_text(tx, py(f, ty), f->us,
                1.f, 1.f, 1.f, 1.f, "Welcome, Player");
    }
    mm_rule_at(f, tx, py(f, ty + 26.f), 290.f * f->us);
    ty += 34.f;

    /* THE THREE FACTS THE PORT ACTUALLY KNOWS. The original's card reads Rank,
       Current car, Play time, Scores and Cash, and every one of those is a
       PROFILE field -- the Players/ .scp files, which this port does not read and Change
       player does not open. What is here instead is true: the car the settings
       file is on, the track the carousel is on, and the field it fields. */
    {
        const int car = TRACKS[m->track].car;
        static const char *const CARNAME[3] = { "Overkill", "Buggy", "Hummer" };
        snprintf(line, sizeof line, "Car: %s",
                 CARNAME[car >= 0 && car < 3 ? car : 0]);
        if (small_.tex)
            sf_text_shadowed(&small_, tx, py(f, ty),
                             f->us * MM_TS_INFO, 1.f, 1.f, 1.f, 1.f, line);
        else
            ui_text(tx, py(f, ty), f->us * 0.9f,
                    1.f, 1.f, 1.f, 1.f, line);
        ty += 22.f;
        snprintf(line, sizeof line, "Track: %s", MAP_TRACK_NAME[m->track]);
        if (small_.tex)
            sf_text_shadowed(&small_, tx, py(f, ty),
                             f->us * MM_TS_INFO, 1.f, 1.f, 1.f, 1.f, line);
        else
            ui_text(tx, py(f, ty), f->us * 0.9f,
                    1.f, 1.f, 1.f, 1.f, line);
        ty += 22.f;
        snprintf(line, sizeof line, "Field: %d opponents",
                 AI_RACES[m->track].n);
        if (small_.tex)
            sf_text_shadowed(&small_, tx, py(f, ty),
                             f->us * MM_TS_INFO, 1.f, 1.f, 1.f, 1.f, line);
        else
            ui_text(tx, py(f, ty), f->us * 0.9f,
                    1.f, 1.f, 1.f, 1.f, line);
    }
}

static void mm_draw_carousel(const mainmenu_t *m, const mmframe *f)
{
    const sfont big = sf_big(m->tex.font_big);
    const sfont small_ = sf_small(m->tex.font_small);
    int slot, i;
    char line[96];
    float ty, nx;

    for (slot = 0; slot < 5; slot++) {
        const int t = mm_slot_track(m, slot);
        float x, y, w, h;
        mm_slot_rect(f, slot, &x, &y, &w, &h);
        /* The photograph brings its own rounded silver frame -- the measured box
           IS the framed picture, so nothing is drawn behind it. */
        if (m->tex.shot[t])
            ui_image(x, y, w, h, m->tex.shot[t],
                     MM_SHOT_U0, MM_SHOT_V0, MM_SHOT_U1, MM_SHOT_V1,
                     1.f, 1.f, 1.f, 1.f);
        else
            ui_rect(x, y, w, h, 0.25f, 0.28f, 0.34f, 1.f);
    }

    /* THE NAME AND ITS LINES sit under the big photograph and move with it --
       gx() again, so the block does not drift off the picture it describes. */
    nx = gx(f, MM_GROUP_X, MM_NAME_X);
    if (big.tex)
        sf_text_shadowed(&big, nx, py(f, MM_NAME_Y),
                         f->us * MM_TS_TRACK, 1.f, 1.f, 1.f, 1.f,
                         MAP_TRACK_NAME[m->track]);
    else
        ui_text(nx, py(f, MM_NAME_Y), f->us * MM_TS_TRACK,
                1.f, 1.f, 1.f, 1.f, MAP_TRACK_NAME[m->track]);
    mm_rule_at(f, nx, py(f, MM_NAME_Y + 28.f), 180.f * f->us);

    /* WHAT THE ORIGINAL PUTS HERE IS THREE LINES OF AUTHORED PROSE, and it is
       in Language/Game/english.tbl, whose DSRC chunk is obfuscated behind the
       key in its LSRC -- not read yet (known-issues.md). Rather than invent a
       description, this states the track's own roster, which IS shipped data:
       ailayouts.ini's drivers for this race, through ai_data.h. */
    ty = MM_DESC_Y;
    {
        const ai_race *r = &AI_RACES[m->track];
        int n = 0;
        line[0] = 0;
        for (i = 0; i < r->n && n < 3; i++) {
            /* `ref` is 1-based into ailayouts.ini's AIPlayer<n> list. */
            const int p = r->op[i].ref - 1;
            if (p < 0 || p >= AI_N_PLAYERS)
                continue;
            if (n)
                strncat(line, ", ", sizeof line - strlen(line) - 1);
            strncat(line, AI_PLAYERS[p].name, sizeof line - strlen(line) - 1);
            n++;
        }
        if (small_.tex)
            sf_text_shadowed(&small_, nx, py(f, ty),
                             f->us * MM_TS_INFO, 1.f, 1.f, 1.f, 1.f, line);
        else
            ui_text(nx, py(f, ty), f->us * 0.9f,
                    1.f, 1.f, 1.f, 1.f, line);
        ty += MM_DESC_LH;
        snprintf(line, sizeof line, "Rubber band %d%%",
                 (int)(r->coeff_common * 100.f + 0.5f));
        if (small_.tex)
            sf_text_shadowed(&small_, nx, py(f, ty),
                             f->us * MM_TS_INFO, 1.f, 1.f, 1.f, 1.f, line);
        else
            ui_text(nx, py(f, ty), f->us * 0.9f,
                    1.f, 1.f, 1.f, 1.f, line);
    }
}

static void mm_draw_credits(const mainmenu_t *m, const mmframe *f)
{
    static const char *const LINES[] = {
        "RC Cars", "Creat Studios, 2003",
        "",
        "PS Vita port",
        "Direct3D 8 fixed function onto vitaGL",
        "Physics, AI and audio recovered from",
        "RCCars.exe and CRSGame20.dll",
        "",
        "Touch or press any button",
    };
    const int n = (int)(sizeof LINES / sizeof LINES[0]);
    const sfont sf = sf_small(m->tex.font_small);
    const float sc = f->us * 0.80f;
    const float lh = (sf.tex ? sf_h(&sf, sc) : ui_text_h(sc)) * 1.35f;
    const float bh = lh * (n + 2);
    const float bw = f->w * 0.62f;
    const float bx = (f->w - bw) * 0.5f, by = (f->h - bh) * 0.5f;
    int i;

    ui_rect(0.f, 0.f, f->w, f->h, 0.f, 0.f, 0.f, 0.55f);
    ui_rect(bx, by, bw, bh, 0.08f, 0.07f, 0.06f, 0.92f);
    ui_rect(bx, by, bw, f->us * 2.f, 0.85f, 0.85f, 0.88f, 0.9f);
    ui_rect(bx, by + bh - f->us * 2.f, bw, f->us * 2.f,
            0.85f, 0.85f, 0.88f, 0.9f);
    for (i = 0; i < n; i++) {
        const char *s = LINES[i];
        const float w = sf.tex ? sf_w(&sf, sc, s) : ui_text_w(sc, s);
        const float x = bx + (bw - w) * 0.5f, y = by + lh * (i + 1);
        if (sf.tex)
            sf_text(&sf, x, y, sc, 1.f, 1.f, 1.f, 1.f, s);
        else
            ui_text(x, y, sc, 1.f, 1.f, 1.f, 1.f, s);
    }
}

/* One forward arrow out of enumarrows, at (x, y) sized `sz`. The texture's cells
   all point LEFT, so a forward arrow is the same cell with its U reversed --
   which is also why there is no right-facing art to have got the wrong way
   round. `state` 0 silver, 1 red (focused), 2 grey. */
/* `state`: 0 the resting look, 1 focused, 2 disabled.
 *
 * THE RESTING LOOK IS THE RED CELL, not the silver one. Every arrow on the
 * game's own quick-race screen is red; the silver is what a focused one goes,
 * which reads as lit against it. The first build had them the other way round
 * and the page came up grey. */
static void mm_arrow(const mainmenu_t *m, float x, float y, float sz,
                     int state, int forward)
{
    const float u0 = (state == 1) ? 0.f : MM_AR_HALF;
    const float u1 = u0 + MM_AR_HALF;
    const float v0 = (state == 2) ? MM_AR_HALF : 0.f;
    if (!m->tex.arrows)
        return;
    ui_image(x, y, sz, sz, m->tex.arrows,
             forward ? u1 : u0, v0, forward ? u0 : u1, v0 + MM_AR_HALF,
             1.f, 1.f, 1.f, 1.f);
}

/* The value a row shows. Every one of them is shipped data or a count of it. */
static void mm_q_value(const mainmenu_t *m, int row, char *out, int n)
{
    switch (row) {
    case MM_Q_TRACK:
        snprintf(out, n, "%s", MAP_TRACK_NAME[m->track]);
        break;
    case MM_Q_LAPS:
        snprintf(out, n, "%d", m->laps);
        break;
    case MM_Q_SKILL:
        snprintf(out, n, "%s", mainmenu_skill_name(m->skill));
        break;
    case MM_Q_CAR:
        snprintf(out, n, "%s", RB_CARS[m->car < 0 || m->car >= MM_N_CARS
                                       ? 0 : m->car].name);
        break;
    default:
        out[0] = 0;
        break;
    }
}

/* A line of text, left- or right-aligned in design space. */
static void mm_q_text(const mainmenu_t *m, const mmframe *f, int big_font,
                      float x, float y, float ts, int right, float alpha,
                      const char *s)
{
    const sfont sf = big_font ? sf_big(m->tex.font_big)
                              : sf_small(m->tex.font_small);
    const float sc = f->us * ts;
    const float w = sf.tex ? sf_w(&sf, sc, s) : ui_text_w(sc, s);
    const float px_ = right ? (px(f, x) - w) : px(f, x);
    if (sf.tex)
        sf_text_shadowed(&sf, px_, py(f, y), sc, 1.f, 1.f, 1.f, alpha, s);
    else
        ui_text(px_, py(f, y), sc, 1.f, 1.f, 1.f, alpha, s);
}

static void mm_draw_quick(const mainmenu_t *m, const mmframe *f)
{
    const int c = (m->car < 0 || m->car >= MM_N_CARS) ? 0 : m->car;
    const rb_car_data *cd = &RB_CARS[c];
    char line[96];
    int i;

    /* --- the page's own navigation, in the main menu's row positions ----- */
    for (i = 0; i < MM_QB_N; i++) {
        float bx, by, bw, bh, v;
        /* Only the page you are on is live. The other three are dlgMAPINFO,
           dlgSTAT and dlgSETCAR, which this port does not build. */
        const int live = (i == MM_QB_SUMMARY);
        mm_bar_rect(f, i, &bx, &by, &bw, &bh);
        if (m->tex.wedge[i]) {
            const float wh = py(f, MM_WEDGE_H);
            ui_image(bx, by - wh * (MM_WEDGE_TOP / 64.f),
                     px(f, MM_BAR_R) - bx, wh, m->tex.wedge[i],
                     0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f);
        }
        v = live ? MM_V_RED : MM_V_GREY;
        if (m->tex.buttons)
            ui_image(bx, by, bw, bh, m->tex.buttons,
                     0.f, v, 1.f, v + MM_V_CELL, 1.f, 1.f, 1.f, 1.f);
        else
            ui_rect(bx, by, bw, bh, live ? 0.72f : 0.45f,
                    live ? 0.09f : 0.45f, live ? 0.11f : 0.47f, 1.f);
        mm_label(m, f, bx, by, bw, bh, MM_QB_NAME[i],
                 live ? 1.f : 0.82f, live ? 1.f : 0.82f, live ? 1.f : 0.84f);
    }

    /* --- shotTrack, between enumTrack's two arrows ----------------------- */
    {
        float x, y, w, h;
        mm_box(f, DLG_RACESUM_shotTrackX0, DLG_RACESUM_shotTrackY0,
               DLG_RACESUM_shotTrackSX, DLG_RACESUM_shotTrackSY,
               &x, &y, &w, &h);
        if (m->tex.shot[m->track])
            ui_image(x, y, w, h, m->tex.shot[m->track],
                     MM_SHOT_U0, MM_SHOT_V0, MM_SHOT_U1, MM_SHOT_V1,
                     1.f, 1.f, 1.f, 1.f);
        else
            ui_rect(x, y, w, h, 0.25f, 0.28f, 0.34f, 1.f);
    }

    /* --- animCar: the chosen car, turning, in its own 374x304 viewport ---- */
    {
        float x, y, w, h;
        mm_box(f, DLG_RACESUM_animCarX0 + MM_ANIMCAR_DX,
               DLG_RACESUM_animCarY0 + MM_ANIMCAR_DY,
               DLG_RACESUM_animCarSX, DLG_RACESUM_animCarSY, &x, &y, &w, &h);
        if (m->car_draw) {
            /* OUT OF ui.c's ORTHO AND BACK IN. The callback draws in 3D and
               wants the state it would have outside a menu; ui_end puts back
               exactly what ui_begin took, so the pair is the seam. Everything
               after this -- the enum arrows either side of the car, the text --
               is drawn over it, which is the order the game's own screen has. */
            ui_end();
            m->car_draw(m->car_ctx, x, y, w, h);
            ui_begin((int)f->w, (int)f->h);
        } else {
            /* No drawer: the name goes in the middle of the space, dim, so the
               hole says what it is about rather than reading as a bug. */
            const sfont sf = sf_big(m->tex.font_big);
            const float sc = f->us * MM_TS_TRACK;
            const float tw = sf.tex ? sf_w(&sf, sc, cd->name)
                                    : ui_text_w(sc, cd->name);
            const float cx = x + w * 0.5f - tw * 0.5f;
            const float cy = y + h * 0.55f;
            if (sf.tex)
                sf_text_shadowed(&sf, cx, cy, sc, 1.f, 1.f, 1.f, 0.45f,
                                 cd->name);
            else
                ui_text(cx, cy, sc, 1.f, 1.f, 1.f, 0.45f, cd->name);
        }
    }

    /* --- staticTrackName and staticTrackInfo ----------------------------- */
    mm_q_text(m, f, 1, DLG_RACESUM_staticTrackNameX0,
              DLG_RACESUM_staticTrackNameY0, MM_TS_TRACK, 0, 1.f,
              MAP_TRACK_NAME[m->track]);
    mm_rule_at(f, px(f, DLG_RACESUM_staticTrackNameX0),
               py(f, DLG_RACESUM_staticTrackNameY0 + 28.f),
               DLG_RACESUM_staticTrackNameSX * f->us);
    {
        /* THE AUTHORED DESCRIPTION IS IN THE ENCRYPTED STRING TABLE
           (known-issues.md), so what fills staticTrackInfo is the track's own
           roster and the field this skill fields -- shipped data either way. */
        const ai_race *r = &AI_RACES[m->track];
        float ty = DLG_RACESUM_staticTrackInfoY0;
        int n = 0;
        line[0] = 0;
        for (i = 0; i < r->n && n < 3; i++) {
            const int p = r->op[i].ref - 1;
            if (p < 0 || p >= AI_N_PLAYERS)
                continue;
            if (n)
                strncat(line, ", ", sizeof line - strlen(line) - 1);
            strncat(line, AI_PLAYERS[p].name, sizeof line - strlen(line) - 1);
            n++;
        }
        mm_q_text(m, f, 0, DLG_RACESUM_staticTrackInfoX0, ty,
                  MM_TS_INFO, 0, 1.f, line);
        ty += 22.f;
        snprintf(line, sizeof line, "%d opponents, rubber band %d%%",
                 mainmenu_field_size(m->track, m->skill),
                 (int)(r->coeff_common * 100.f + 0.5f));
        mm_q_text(m, f, 0, DLG_RACESUM_staticTrackInfoX0, ty,
                  MM_TS_INFO, 0, 1.f, line);
    }

    /* --- staticCarName and staticCarInfo, right-aligned as the game has --- */
    mm_q_text(m, f, 1, DLG_RACESUM_staticCarNameX0, DLG_RACESUM_staticCarNameY0,
              MM_TS_WELCOME, 0, 1.f, "Car");
    mm_rule_at(f, px(f, DLG_RACESUM_staticCarNameX0),
               py(f, DLG_RACESUM_staticCarNameY0 + 26.f),
               DLG_RACESUM_staticCarNameSX * f->us);
    {
        const float rx_ = DLG_RACESUM_staticCarInfoX0 + DLG_RACESUM_staticCarInfoSX;
        float ty = DLG_RACESUM_staticCarInfoY0;
        mm_q_text(m, f, 1, rx_, ty, MM_TS_WELCOME, 1, 1.f, cd->name);
        ty += 26.f;
        /* rb_data.h's own numbers. The original names the three upgrades here
           ("WHB Devastator" and the rest) and those are in the string table. */
        snprintf(line, sizeof line, "%.0f cm, %.1f kg",
                 cd->extent[2] * 100.f, cd->mass);
        mm_q_text(m, f, 0, rx_, ty, MM_TS_INFO, 1, 1.f, line);
        ty += 22.f;
        snprintf(line, sizeof line, "%.0f deg of steering", cd->steer_max_deg);
        mm_q_text(m, f, 0, rx_, ty, MM_TS_INFO, 1, 1.f, line);
    }

    /* --- the four enums: label, back arrow, value, forward arrow --------- */
    for (i = 0; i < MM_Q_N_ROWS; i++) {
        const float *R = MM_Q_RECT[i];
        const int lit = (m->qfocus == i);
        float ax, fx, ay, sz;
        mm_q_arrows(f, i, &ax, &fx, &ay, &sz);

        if (MM_Q_LABEL[i][0]) {
            /* the small marker the game draws beside a LABELLED row, and on
               neither of the two that have no label */
            mm_arrow(m, px(f, R[0] - MM_Q_BULLET_GAP),
                     py(f, R[1] + R[3] * 0.5f) - MM_Q_BULLET * f->us * 0.5f,
                     MM_Q_BULLET * f->us, lit ? 1 : 0, 0);
            mm_q_text(m, f, 0, R[0], R[1], MM_TS_LABEL, 0, 1.f, MM_Q_LABEL[i]);
            mm_rule_at(f, px(f, R[0]), py(f, R[1] + 24.f),
                       (R[2] * R[4] - 30.f) * f->us);
        }
        mm_arrow(m, ax, ay, sz, lit ? 1 : 0, 0);
        mm_arrow(m, fx, ay, sz, lit ? 1 : 0, 1);

        /* the value, centred between the two arrows */
        if (i == MM_Q_LAPS || i == MM_Q_SKILL) {
            const sfont sf = sf_small(m->tex.font_small);
            const float sc = f->us * MM_TS_LABEL;
            float w;
            mm_q_value(m, i, line, sizeof line);
            w = sf.tex ? sf_w(&sf, sc, line) : ui_text_w(sc, line);
            {
                const float cx = (ax + sz + fx) * 0.5f;
                if (sf.tex)
                    sf_text_shadowed(&sf, cx - w * 0.5f, py(f, R[1]), sc,
                                     1.f, 1.f, 1.f, 1.f, line);
                else
                    ui_text(cx - w * 0.5f, py(f, R[1]), sc,
                            1.f, 1.f, 1.f, 1.f, line);
            }
        }
    }
}

void mainmenu_draw(const mainmenu_t *m, int screen_w, int screen_h)
{
    const mmframe f = mm_frame(screen_w, screen_h);

    if (!m)
        return;
    mm_draw_frame(m, &f);
    mm_draw_header(m, &f);
    if (m->page == MM_PAGE_QUICK) {
        mm_draw_quick(m, &f);
    } else {
        mm_draw_card(m, &f);
        mm_draw_carousel(m, &f);
        mm_draw_rows(m, &f);
    }
    mm_draw_race(m, &f);
    mm_draw_quit(m, &f);
    if (m->credits)
        mm_draw_credits(m, &f);
}
