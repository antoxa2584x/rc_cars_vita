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
#include "str_data.h"       /* the game's own authored text, decoded */
#include "records.h"
#include "awards.h"        /* the record book behind Track stats */
#include "player.h"         /* the roster dlgPLRSCOMM is a view of */
#include "garage.h"
#include "champ.h"         /* the shop behind dlgSETCAR/dlgSETDETAIL */
#include "net.h"            /* the transport behind dlgMULTIPLAYER */
#include "ime.h"            /* the machine's own keyboard, or a stub */

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
    583.f, 605.f, 618.f, 630.f, 636.f, 638.f, 634.f, 628.f
};
/* CREDITS' 628 IS A CORRECTION, and the shape of the oval is what says so. The
   other seven walk 583, 605, 618, 630, 636, 638, 634 -- a bulge that opens and
   closes by a dozen pixels at a time -- and the eighth was measured at 598, a
   36 px step no leg of that curve makes. It was measured off a shot with
   CREDITS FOCUSED: a focused row slides its cap out to the left (MM_SLIDE), and
   the number that came back was the slid position, not the row's. On a shot
   with the focus elsewhere every one of the five visible rows puts its cap
   exactly MM_WEDGE_TIP left of its own wedge's tip, and Credits' lands at 628. */
#define MM_BAR_H     31.f      /* the cell is 32 rows and 31 of them are opaque */
#define MM_BAR_R    800.f      /* every row ends at the frame's right edge */
#define MM_TEXT_PAD   8.f      /* the label's right margin inside the bar */

/* THE WEDGE under each row -- ButtonPodl_right_1..9, AT THE ART'S OWN SIZE.
 *
 * The cell is 256x64 with the silver plate on rows 12..50 -- 39 rows -- and a
 * slanted tip at a different column in each of the nine, which is what glues a
 * row to the oval. Two numbers place it, and both are measured off the game's
 * own 800x600 screens (five rows of the main menu, four of the quick-race page,
 * and every one of the nine agrees to the pixel):
 *
 *   the plate's TOP EDGE lands 3 px above the bar's, so it stands proud of it
 *   top and bottom -- the silver tray the bars sit on;
 *   the plate's TIP lands 7 px right of the bar's own left cap.
 *
 * THE FIRST BUILD DREW IT INVISIBLE. It squeezed the whole 64-row cell into a
 * 44 px box, which makes the 39-row plate 27 px -- SHORTER than the 31 px bar --
 * and put all nine of them entirely behind the bars they belong under. The
 * screen had bars floating on bare orange where the original has a tray.
 *
 * MM_WEDGE_TIP_U is each cell's own tip column, read off the alpha at plate row
 * 13; anchoring on it is what makes one placement rule serve nine textures.
 */
#define MM_WEDGE_CELL 64.f     /* the cell's height, drawn 1:1 in the design frame */
#define MM_WEDGE_TOP  12.f     /* the plate's first opaque row, of 64 */
#define MM_WEDGE_RISE  3.f     /* how far the plate stands above the bar's top */
/* HOW FAR THE TIP STANDS RIGHT OF THE BAR'S CAP -- and the 7 measured here was
   FOUR PX WIDE. Rendered at 800x600, which is the frame every constant on this
   screen was measured in, and held against the retail shot of the same page at
   the same design coordinates: on Track stats (X0 618, unfocused in both) the
   two BARS agree to a pixel -- 617 against 618 -- and the two TRAYS do not,
   624 against 627. So the tray, and only the tray, sat right of the game's, and
   it is one nudge for all nine. Anything larger walks the tips out from under
   their own bars: at 25 the plate stands clear of the cap on every row. */
#define MM_WEDGE_TIP  (7.f - 4.f)
static const float MM_WEDGE_TIP_U[9] = {
    36.f, 61.f, 80.f, 94.f, 103.f, 107.f, 105.f, 99.f, 76.f
};

/* THE HEADER, top left: TWO of HeaderSkin's three cells, one on the other.
 *
 * The port drew the red plate alone and the original does not: under it is the
 * PLAIN SILVER BAR -- the same third cell the Quit button already sits on --
 * and it is 12 texels wider and 10 taller, so it shows as a rim along the top
 * and a thick rounded cap on the right. Without it the header ended in a bare
 * red cap and read as the wrong shape.
 *
 * AND IT IS THE SECOND RED CELL, not the first. HeaderSkin carries the header
 * twice, v 12..51 with a RED triangle and v 76..115 with a WHITE one, and every
 * shot of the real menu has the white one.
 *
 * AT THE ART'S OWN PIXEL SIZE, which the first build stretched: measured off
 * the 800x600 screen, the atlas's left edge sits at x -10 (so the rounded left
 * cap is cut the way the original's is), the silver's 50 rows run y 23..73 and
 * the red's 40 run y 28..68. That puts the red plate's right cap at 231 and the
 * silver's at 243, which is where the screenshot has them, 11 px apart. */
#define MM_HDR_X   (-10.f)
#define MM_HDR_W    256.f      /* 1:1 -- the cell is not stretched to a box */
#define MM_HDR_SIL_Y  23.f
#define MM_HDR_SIL_H  50.f
#define MM_HDR_RED_Y  28.f
#define MM_HDR_RED_H  40.f
/* THE TITLE IS RIGHT-ALIGNED AGAINST THE TRIANGLE, which is baked into the cell
   at texel 206; the original leaves 9 px of red between the last letter and it.
   The first build left-aligned it at 24% of a stretched box, which on a 960
   wide panel opened a hand's width of empty bar between the word and the ▼. */
#define MM_HDR_TEXT_R (206.f - 9.f)
/* and 2 screen pixels above the plate's own centre, which is where the ink is. */
#define MM_HDR_TEXT_DY (-2.f)

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

/* `messagebox' -- AND ITS RIGHT HALF IS THE DIALOG BUTTON THIS PORT KEPT TRYING
 * TO BUILD OUT OF SOMETHING ELSE.
 *
 * 256x128. The left half is a rounded frame like messagebox_empty's; the right
 * half is FOUR 128x32 cells, each a SHORT PILL with BOTH caps rounded and a dot
 * inside the left one -- red with a dark dot, red with a white one, orange with
 * a dark one, orange with a white one. That is a Yes/No pair and its focus, in
 * one texture, and it is the only art in the interface with a right cap on it.
 *
 * WHY THIS TOOK THREE TRIES. Every other plate in the front end is one of
 * ButtonsTextures' 256x32 cells, which run FLAT to column 255 because every row
 * of the menu proper runs off the right edge of the screen. Stretched into a
 * 133 px dialog button they squash; three-sliced they keep their caps but there
 * is no right cap to slice, and mirroring the left one brings the DOT with it.
 * The answer was never a slicing rule -- it is that the dialog has its own art
 * and this port had never opened `messagebox'. Held against the game's own
 * `erase data' shot, the cell fits the Yes plate's every pixel: the cap ramps,
 * the highlight and the dot all land, at 128 texels drawn over 137 px.
 *
 * `messagebox_empty' has been in these notes since the map panel was drawn, and
 * `messagebox' -- one word shorter, in the same directory, in Interface.sb's own
 * manifest -- had never been looked at. traps.md.
 */
#define MM_MB_U0     (128.f / 256.f)   /* the right half: the four pills */
#define MM_MB_CELL   (32.f / 128.f)
#define MM_MB_RED    (0.f / 128.f)     /* dark dot -- unfocused */
#define MM_MB_RED_F  (32.f / 128.f)    /* white dot -- focused */
#define MM_MB_ORA    (64.f / 128.f)
#define MM_MB_ORA_F  (96.f / 128.f)

/* RadioButtonsTextures' three 256x32 cells, over a 256x128 atlas: red with a
   dark DOT, red with a white one, and grey. THE QUICK-RACE PAGE'S FIRST THREE
   ROWS ARE RADIO BUTTONS -- they pick which of the four sibling dialogs is up,
   and the game's own shot of that page has a dot on Race summary, Map and info
   and Track stats and an ARROW on Garage, which is the only one of the four
   that goes somewhere else. The port drew all four off ButtonsTextures and had
   four arrows; this texture is in the pack for exactly this. */
#define MM_V_RAD      (0.f / 128.f)
#define MM_V_RAD_ON   (32.f / 128.f)
#define MM_V_RAD_OFF  (64.f / 128.f)
#define MM_V_RAD_CELL (32.f / 128.f)

/* --------------------------------------------- the engine's own button curves
 *
 * Splines/ButtonFocSpline.spl, ButtonUnFocSpline.spl and ButtonEnterSpline.spl,
 * SHIPPED and read off rather than invented. Each file is a count and then that
 * many chains of four `<t> <value>' control points -- a cubic Bezier in both
 * axes, t running 0..1 across the whole chain -- so a value is found by solving
 * the t axis for the parameter and reading the other axis off it.
 *
 * WHAT THE VALUE MEANS is not in the file, and the screen says it. On the game's
 * own quick-race shot the three unfocused rows each put their left cap exactly
 * MM_WEDGE_TIP left of their wedge's tip; the FOCUSED row's cap stands 23 px
 * further out than that. ButtonFocSpline settles at 0.361, and 23 / 0.361 = 64
 * -- the button cell's own height, and the unit the rest of the curve is in.
 *
 * So a focused button SLIDES OUT to the left by MM_SLIDE * spline(t), through an
 * overshoot of 50 px (0.789) on the way. ButtonUnFocSpline runs the other way
 * and dips to -0.754, the bar pulling IN past its rest before it stops, and
 * ButtonEnterSpline is the PRESS: it starts where a focused button stands and
 * recoils through the same dip. The wedge under a row does not move with any of
 * it -- on the original's screen the focused bar runs out over a wedge that
 * stayed where it was, which is how the 23 px was measurable in the first place.
 */
#define MM_SLIDE      64.f     /* the cell height, which is the curves' unit */
/* WHERE ButtonFocSpline SETTLES, i.e. how far a focused -- or, on the pages
   whose bars are radio buttons, a SELECTED -- row stands out: 0.361 * 64 is
   23 px. Measured on the quick-race page's own bars at 25 and 28 and on the
   Garage's at 26; the spline is what says 0.361. */
#define MM_SETTLED    0.361f
#define MM_ANIM_FOC   0.30f    /* seconds a focus or unfocus takes */
#define MM_ANIM_PRESS 0.26f    /* seconds a press takes */

static const float MM_SPL_FOC[16][2] = {
    { 0.000f,  0.000f }, { 0.141f,  0.000f }, { 0.286f, -0.009f }, { 0.348f, 0.143f },
    { 0.348f,  0.143f }, { 0.410f,  0.295f }, { 0.447f,  0.789f }, { 0.520f, 0.728f },
    { 0.520f,  0.728f }, { 0.592f,  0.666f }, { 0.608f,  0.377f }, { 0.670f, 0.366f },
    { 0.670f,  0.366f }, { 0.733f,  0.355f }, { 0.945f,  0.364f }, { 1.000f, 0.361f }
};
static const float MM_SPL_UNFOC[12][2] = {
    { 0.000f,  0.361f }, { 0.159f,  0.395f }, { 0.252f, -0.754f }, { 0.402f, -0.699f },
    { 0.402f, -0.699f }, { 0.552f, -0.644f }, { 0.677f, -0.007f }, { 0.779f,  0.000f },
    { 0.779f,  0.000f }, { 0.881f,  0.006f }, { 0.934f, -0.003f }, { 1.000f, -0.001f }
};
static const float MM_SPL_ENTER[12][2] = {
    { 0.000f,  0.355f }, { 0.059f,  0.388f }, { 0.252f, -0.754f }, { 0.402f, -0.699f },
    { 0.402f, -0.699f }, { 0.552f, -0.644f }, { 0.677f, -0.007f }, { 0.779f,  0.000f },
    { 0.779f,  0.000f }, { 0.881f,  0.006f }, { 0.934f, -0.003f }, { 1.000f, -0.001f }
};

static float mm_bez(float a, float b, float c, float d, float u)
{
    const float v = 1.f - u;
    return v * v * v * a + 3.f * v * v * u * b + 3.f * v * u * u * c
         + u * u * u * d;
}

/* One of the three curves at time `t'. The t axis is monotone inside a segment,
   so the parameter is bisected out of it rather than assumed to equal t -- the
   control points are NOT evenly spaced in t and reading the value off u = t
   flattens the overshoot the artists drew. */
static float mm_spline(const float (*p)[2], int nseg, float t)
{
    int s, i;
    float lo = 0.f, hi = 1.f, u;

    if (!(t > 0.f))  return p[0][1];
    if (t >= 1.f)    return p[nseg * 4 - 1][1];
    for (s = 0; s < nseg - 1; s++)
        if (t <= p[s * 4 + 3][0])
            break;
    p += s * 4;
    for (i = 0; i < 20; i++) {
        u = (lo + hi) * 0.5f;
        if (mm_bez(p[0][0], p[1][0], p[2][0], p[3][0], u) < t)
            lo = u;
        else
            hi = u;
    }
    return mm_bez(p[0][1], p[1][1], p[2][1], p[3][1], (lo + hi) * 0.5f);
}

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

/* ONE BUTTON PLATE, THREE-SLICED ACROSS -- and every plate in a DIALOG needs it.
 *
 * ButtonsTextures' and RadioButtonsTextures' cells are 256 x 32 and every one of
 * them is a PILL: a round cap with a dot (or a triangle) inside it, a straight
 * middle, and a round cap at the other end. Stretched whole into a 133 px dialog
 * button the caps go elliptical and the dot goes with them, which is what
 * "the buttons are squashed" was.
 *
 * The original does not stretch them. Measured on its own `erase data' dialog,
 * whose plate is 133 x 32 design px: the white dot lands 13 x 13 on screen
 * against the atlas's own 11 x 12, and the cap's arc has the same radius in the
 * picture as in the texture. So the CAPS ARE DRAWN AT THE ART'S OWN SIZE and
 * only the 192 texels between them are stretched -- the cell is 32 rows tall, so
 * a 32-texel cap drawn at the plate's height is square.
 *
 * AND THE CELL HAS ONLY ONE CAP. The pill runs FLAT to texel column 255 -- read
 * off the alpha, every one of the last ten columns is 30 rows tall -- because
 * every bar in the front end proper runs off the right edge of the screen and
 * never shows a right end. A dialog button does, and the game's own shot has it
 * rounded, so THE RIGHT CAP IS THE LEFT ONE WITH U MIRRORED: the same trick
 * mm_frame9 plays with messagebox_empty, which is a left-hand C for the same
 * reason. Slicing the last 32 texels instead gives a straight cut, which is what
 * "the buttons are cut off on the right" was.
 *
 * BUT NOT THE WHOLE 32, because the MARKER lives inside the left cap and a plain
 * mirror brings it along -- a dot at both ends, which is not what the game's shot
 * has. The two spans do not overlap and the alpha says where they part: the arc
 * reaches its full 30 rows at column 11 in every one of the nine cells, and the
 * marker's leftmost pixel is at 15. So the mirror takes FOURTEEN texels -- the
 * whole arc, none of the dot -- and it measures right: on the original the Yes
 * plate's right arc is 13 design px wide.
 *
 * A plate too narrow for its two caps falls back to the whole cell rather than
 * drawing them over each other. */
#define MM_CAP_TX 32.f                    /* the left cap, and the cell's height */
#define MM_CAP_RX 14.f                    /* the mirror: arc, no marker */
#define MM_CAP_U  (MM_CAP_TX / 256.f)
#define MM_CAP_RU (MM_CAP_RX / 256.f)
static void mm_bar3(float x, float y, float w, float h, unsigned int tex,
                    float v0, float v1)
{
    const float cap = h;                          /* 32 texels, square */
    const float rcap = h * (MM_CAP_RX / MM_CAP_TX);
    if (w < cap + rcap) {
        ui_image(x, y, w, h, tex, 0.f, v0, 1.f, v1, 1.f, 1.f, 1.f, 1.f);
        return;
    }
    ui_image(x, y, cap, h, tex, 0.f, v0, MM_CAP_U, v1, 1.f, 1.f, 1.f, 1.f);
    ui_image(x + cap, y, w - cap - rcap, h, tex,
             MM_CAP_U, v0, 1.f, v1, 1.f, 1.f, 1.f, 1.f);
    /* u runs BACKWARDS -- the left cap's arc, mirrored */
    ui_image(x + w - rcap, y, rcap, h, tex,
             MM_CAP_RU, v0, 0.f, v1, 1.f, 1.f, 1.f, 1.f);
}

/* ONE DIALOG BUTTON off `messagebox', whole -- no slicing, because this cell is
   the right shape already. `orange' picks the pair's right-hand plate and `lit'
   its white dot. Answers 0 when the texture is not there (a menu.vsc packed
   before it was named), so the caller can fall back to the row bars. */
static int mm_msgbtn(const mainmenu_t *m, float x, float y, float w, float h,
                     int orange, int lit)
{
    float v;
    if (!m->tex.msgbox)
        return 0;
    v = orange ? (lit ? MM_MB_ORA_F : MM_MB_ORA)
               : (lit ? MM_MB_RED_F : MM_MB_RED);
    ui_image(x, y, w, h, m->tex.msgbox, MM_MB_U0, v, 1.f, v + MM_MB_CELL,
             1.f, 1.f, 1.f, 1.f);
    return 1;
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

/* THE PITCH OF A MULTI-LINE BLURB, and it is NOT the control's height divided
   by its lines. dlgMAPINFO's staticTrackInfo is 156 tall over five lines (31.2)
   and dlgSTAT's is 73 over three (24.3), and on the game's own screenshots BOTH
   set their lines 25 design pixels apart -- so the pitch is the font's, not the
   box's, and the box is simply generous on one page. Measured off the five lines
   of Surf's description and the three of its short one. */
#define MM_LINE_H 25.f

/* The page's own right-hand column sits in the main menu's first four measured
   row positions, so its bars and wedges land on the oval with nothing new to
   get wrong -- the one thing here that is the port's and not the .ini's. The
   four words are the string table's own, 10015..10017 and 10052. */
/* AND THE FIFTH BAR IS THE PORT'S OWN WORD. The other four are the string
   table's -- 10015..10017 and 10052 -- and there is no shipped string for a
   thing the game does not have, so `Awards' is written here rather than in
   str_data.h, which is generated out of english.tbl and holds the original's
   text and nothing else. See awards.h. */
#define MM_UI_AWARDS "Awards"

static const char *const MM_QB_NAME[MM_QB_N] = {
    STR_UI_RACE_SUMMARY, STR_UI_MAP_AND_INFO, STR_UI_TRACK_STATS,
    MM_UI_AWARDS, STR_UI_GARAGE
};

const int MM_QB_PAGE[MM_QB_N] = {
    MM_PAGE_QUICK, MM_PAGE_MAPINFO, MM_PAGE_STATS,
    MM_PAGE_AWARDS,             /* the port's own page -- awards.h */
    MM_PAGE_GARAGE              /* dlgSETCAR -- and it is built now */
};

/* AN ENUM ROW, and the same four numbers describe one on any of the three
   pages: the .ini rectangle, its SE split and the label to its left (empty for
   the three whose VALUE is a picture and which therefore have no label). */
typedef struct {
    float x0, y0, sx, sy, se;
    const char *label;
} mm_enum;

/* The pages' enums, each straight out of its own dlg*.ini. Indexed by
   MM_PAGE_*, so MM_PAGE_MAIN's empty row is deliberate -- the main menu is not
   one of these and has no enums. */
static const mm_enum MM_Q_ENUM[MM_N_PAGES][MM_Q_N_ROWS] = {
    /* MM_PAGE_MAIN -- none */
    { { 0.f, 0.f, 0.f, 0.f, 0.f, "" } },
    /* MM_PAGE_QUICK -- dlgRACESUM's four */
    { { DLG_RACESUM_enumTrackX0, DLG_RACESUM_enumTrackY0,
        DLG_RACESUM_enumTrackSX, DLG_RACESUM_enumTrackSY,
        DLG_RACESUM_enumTrackSE, "" },
      { DLG_RACESUM_enumNLapsX0, DLG_RACESUM_enumNLapsY0,
        DLG_RACESUM_enumNLapsSX, DLG_RACESUM_enumNLapsSY,
        DLG_RACESUM_enumNLapsSE, STR_UI_N_LAPS },
      { DLG_RACESUM_enumSkillX0, DLG_RACESUM_enumSkillY0,
        DLG_RACESUM_enumSkillSX, DLG_RACESUM_enumSkillSY,
        DLG_RACESUM_enumSkillSE, STR_UI_SKILL },
      { DLG_RACESUM_enumCarX0, DLG_RACESUM_enumCarY0,
        DLG_RACESUM_enumCarSX, DLG_RACESUM_enumCarSY,
        DLG_RACESUM_enumCarSE, "" } },
    /* MM_PAGE_MAPINFO -- the track picker over the description, and the
       screenshot picker under the strip. Both are all value, no label: the
       track's NAME sits between the first pair and the word `Screenshots' is
       staticShortListText, a control of its own to the left. */
    { { DLG_MAPINFO_shotTrackEnumX0, DLG_MAPINFO_shotTrackEnumY0,
        DLG_MAPINFO_shotTrackEnumSX, DLG_MAPINFO_shotTrackEnumSY, 0.f, "" },
      { DLG_MAPINFO_enumShotX0, DLG_MAPINFO_enumShotY0,
        DLG_MAPINFO_enumShotSX, DLG_MAPINFO_enumShotSY, 0.f, "" } },
    /* MM_PAGE_STATS -- `Sort results by', whose label is staticShowResult and
       therefore also not the enum's own. */
    { { DLG_STAT_enumStatTypeX0, DLG_STAT_enumStatTypeY0,
        DLG_STAT_enumStatTypeSX, DLG_STAT_enumStatTypeSY, 0.f, "" } },
    /* MM_PAGE_AWARDS -- the list's own SCROLLER, in dlgSTAT's enum rectangle
       (mainmenu.h says why this page borrows that dialog's boxes). Its value is
       which rows are on screen, so it is the one control on the page and the
       one thing a touch-only player needs: the arrows either side of it are hit
       boxes already, through mainmenu_q_at's enum pass. */
    { { DLG_STAT_enumStatTypeX0, DLG_STAT_enumStatTypeY0,
        DLG_STAT_enumStatTypeSX, DLG_STAT_enumStatTypeSY, 0.f, "" } },
};
static const int MM_Q_NENUM[MM_N_PAGES] = { 0, 4, 2, 1, 1 };

/* ------------------------------------------------- dlgMAPINFO's shot list
 *
 * FIVE SCREENSHOTS PER TRACK, and the pack has carried them since the beginning
 * as shot_<track>_0..4 -- this port loaded the first of the five and these notes
 * called the other four absent. `shotList' is the SELECTED one, at its own
 * (349, 387) 199x152; the rest are shrunk into the band the four Bound keys
 * describe, y 402..478, and step outward from the selected one with `Space'
 * between.
 *
 * A THUMBNAIL'S WIDTH IS NOT IN THE FILE and it is not a free choice: the band
 * is 76 tall and the shot's own framed ink is 202 x 152, so a thumbnail that is
 * not a squashed photograph is 76 * 202/152 = 101.0 wide. That drops the three
 * to the left of the selection at 243, 137 and 31 -- and 31 is inside
 * BoundL = 25 by six pixels, with the fourth slot outside it, which is exactly
 * where the game's own screenshot puts them (measured 243, 136 and one running
 * off the oval). The width falls out of the aspect and the aspect is the art's.
 */
#define MM_SHOTLIST_H  (DLG_MAPINFO_shotListBoundDn - DLG_MAPINFO_shotListBoundUp)
#define MM_SHOTLIST_W  (MM_SHOTLIST_H * ((MM_SHOT_U1 - MM_SHOT_U0) \
                                         / (MM_SHOT_V1 - MM_SHOT_V0)))

/* -------------------------------------------------- dlgSTAT's results table
 *
 * dlgSTAT.ini gives the table's rectangle and NOTHING ELSE -- no `tableWidth%i',
 * no header or item height, which dlgFINISH.ini does ship. So these five are
 * measured off the game's own screenshot of the page, the same method the main
 * menu's rows came from and the same one this file's header warns about:
 *
 *   the header's rule lands at y 237 and the four row rules at 265, 292, 319
 *   and 347 -- a pitch of 27.3, which is 0.10 of the table's 273, and the same
 *   0.10 for the header (dlgFINISH's own two are 0.10 and 0.15, so the shape of
 *   the answer is the engine's);
 *   the Player column's text starts at x 114, the middle column CENTRES on
 *   312.5 and the Car column on 447.5 -- 0.060, 0.517 and 0.828 of the table's
 *   434 from its own left edge;
 *   the rule under each player name runs 114..252, so it stops 0.378 across.
 *
 * WHAT IS NOT MODELLED: the table's closing rule and the scrollbar's foot both
 * land at 478 on the original and at Y0 + SY = 483 here. Five pixels, and it is
 * not on the row grid either way, so there is no rule to recover from it. */
#define MM_ST_HEAD_H   0.10f    /* of tableStatSY */
#define MM_ST_ITEM_H   0.10f
#define MM_ST_COL0     0.060f   /* of tableStatSX, the name's left edge */
#define MM_ST_COL1     0.517f   /* the stat column's CENTRE */
#define MM_ST_COL2     0.828f   /* the car column's centre */
#define MM_ST_RULE0    0.378f   /* where the per-name rule stops */
#define MM_ST_MARK     7.f      /* the row marker, design px right of X0 */

/* THE AWARD PAGE'S TABLE. dlgSTAT's own left edge and width -- 88 and 434 --
   with the HEIGHT taken back: that dialog gives its table 273 px starting at
   210 because a photograph and a three-line blurb sit above it, and this page
   has neither. So the box runs from under the heading (130) to just above the
   scroller's own row (480), which is 350, and eight rows of 43.75 px is a row
   deep enough for the award's name AND the line saying how it is earned.
   Nothing here is measured off anything -- see mainmenu.h on this page. */
#define MM_AW_Y0     130.f
#define MM_AW_SY     350.f
/* MM_AW_ROWS is in mainmenu.h: the harness walks the scroller to its stop and
   the stop is AW_N - MM_AW_ROWS, so a second copy of the row count there would
   be a check against itself. */
#define MM_AW_NAME_TS 0.78f     /* the name */
#define MM_AW_WHAT_TS 0.60f     /* the line under it */
#define MM_AW_STATE_TS 0.62f    /* the tally on the right */
#define MM_AW_STATE_W 92.f      /* how much of the row the tally may take */

/* THE SCROLLBAR, `scrollbar.csi': 256x64, eight 32-wide cells with 26 px of ink
 * in each -- the TOP cap with an up-arrow in grey, red and hollow, the BOTTOM
 * cap with a down-arrow in the same three, the red thumb, and the plain trough.
 * Measured on the original at x 524..549 and y 237..478, which is the table's
 * own right edge plus 2, the width of the art's ink, and the rows area.
 *
 * IT CANNOT MOVE IN THIS PORT and it is drawn anyway. REC_MAX_ROWS is 8 and
 * nine rows fit, so the thumb is always full height -- but the bar is what the
 * original draws in that same situation (its own screenshot has four rows and a
 * bar), and the row cap is a decision that could change. */
#define MM_SB_CELL_W   (32.f / 256.f)
#define MM_SB_INK_W    26.f
#define MM_SB_CAP_H    64.f
#define MM_SB_GAP      2.f      /* between the table's right edge and the bar */
#define MM_SB_THUMB_H  50.f

/* The bar as a control -- defined with the pages that draw it, declared here
   because two of the three step functions come first in the file. */
static int mm_sb_drive(const mmframe *f, const touch_state *tp,
                       float x0, float y0, float y1,
                       int *first, int rows, int shown, int *drag);

/* ------------------------------------------------- dlgMAPINFO's map panel
 *
 * THE MAP IS DRAWN TWICE, and the shipped art is what says so. `trackmap_<n>'
 * paints the ROUTE -- the ribbon, its arrows and its checkpoint discs -- at
 * alpha 255 and paints everything else below it: the terrain and the water at
 * 129..254, and a flat halo over the rest of the 512 at exactly 128. Held
 * against the game's own screenshot of this page, pixel for pixel:
 *
 *   outside the panel the halo is NOT there -- the desktop reads (237,165,23),
 *   its own orange, where the texture would have put (128,82,41);
 *   outside the panel the ROUTE is there, at the texture's own colour;
 *   inside the panel the terrain IS there, blended over that same desktop --
 *   at one sample the texture's (72,56,79) at alpha 202 over the desktop gives
 *   (107,79,67) and the screenshot has (108,83,61).
 *
 * So: the terrain is clipped to the panel and the route is not. One pass with
 * the whole texture cannot do both. The route pass keys on 254/255, which is
 * the artists' own division of the alpha channel and not a threshold picked by
 * eye -- see ui_alpha_test.
 *
 * THE PANEL'S RECTANGLE IS MEASURED and dlgMAPINFO.ini does not carry it: the
 * silver frame's four edges land at x 70..325 and y 100..352 on the 800x600
 * original, found by thresholding for low-saturation bright pixels the way the
 * main menu's own rows were. Its frame is `messagebox_empty', a 128x128 rounded
 * rectangle with a silver rim and nothing inside it -- the one piece of the
 * interface manifest that is a window rather than a bar.
 *
 * AND WHAT THE ORIGINAL PUTS INSIDE THAT PANEL IS NOT THIS TEXTURE. Held against
 * the screenshot at the panel's own edge, where `trackmap_1' has only its flat
 * halo -- (25, 4, 57) at alpha 128 -- the original reads (31, 71, 95), a
 * saturated blue no blend of that halo over anything on this page produces. The
 * control is called shotTrackVIEW, and dlgRACESUM's `animCar' next door is a
 * live 3D viewport, so the likeliest reading is that this one is too: the level
 * rendered from above into the panel, with the painted map over it. This port
 * does not do that -- it would mean loading a 15 MB scene to change the
 * carousel -- so the panel holds the painting alone, over the dark plate
 * race_ui.c already puts under the same map in the HUD. The terrain and the
 * route are the game's; the water is dimmer toward the panel's edges than the
 * original's. known-issues.md.
 */
#define MM_MAP_X   70.f
#define MM_MAP_Y  100.f
#define MM_MAP_W  255.f
#define MM_MAP_H  252.f
#define MM_MAP_KEY (254.f / 255.f)   /* the route, and only the route */
/* race_ui.h's RUI_PLATE_ALPHA, not included from here: mainmenu.c owns no HUD.
   Without it the painting's own soft edge lets the graffiti desktop through and
   the panel reads as a hole rather than as a window. */
#define MM_MAP_PLATE 0.55f

/* messagebox_empty IS HALF A FRAME. The 128x128 cell carries the rounded rim
 * down its LEFT side -- both left corners and the top and bottom rails running
 * off the right edge -- and nothing else; the interior and the whole right end
 * are transparent. So it is meant to be MIRRORED, and stretching the cell over
 * a 255 px panel instead multiplies its 27-texel corner radius by two, which is
 * three times the ~20 px radius the original's panel actually has.
 *
 * Drawn as a nine-slice with the corners at the ART'S OWN size and only the
 * rails stretched: four corners out of the left half (U mirrored for the right
 * pair, V for the bottom pair), four rails out of a straight slice of each. */
#define MM_MBE_CORNER 32.f      /* design px, and 32 of the cell's 128 texels */
#define MM_MBE_CU     (32.f / 128.f)
#define MM_MBE_RAIL   (40.f / 128.f)   /* a straight slice, past the corner */

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
    /* CHANGE PLAYER IS LIVE NOW. It was one of the five drawn in the artists'
       own grey because "this port does not read that format"; it does, so the
       row opens the game's own Select player page (dlgPLRSCOMM). */
    /* AND MULTIPLAYER IS LIVE NOW: it opens dlgMULTIPLAYER, which is Create
       game and Join game over `net.c'. */
    /* AND SO IS CHAMPIONSHIP, which was the last of the four that needed a
       whole subsystem rather than a wire-up: championship.ini's ten track
       sections are the progression and champ.c is the rules over them. Ghost
       race and Demo play are the two that are left in the artists' grey. */
    return row == MM_CHAMPIONSHIP || row == MM_QUICK_RACE
        || row == MM_CHANGE_PLAYER
        || row == MM_MULTIPLAYER || row == MM_OPTIONS || row == MM_CREDITS;
}

const char *mainmenu_row_name(int row)
{
    if (row == MM_FOCUS_RACE) return "Race";
    if (row == MM_FOCUS_QUIT) return "Quit";
    if (row < 0 || row >= MM_N_ROWS) return "?";
    return MM_NAME[row];
}

/* The bar's rect on screen AT REST. The wedge under it is taller and starts
   further left; the HIT BOX is this rect, not the animated one, because a row
   that is mid-slide is still the same button and a hit box that breathes is a
   hit box that misses. */
static void mm_bar_rect(const mmframe *f, int row,
                        float *x, float *y, float *w, float *h)
{
    *x = px(f, MM_X0[row]);
    *y = py(f, MM_CY[row] - MM_BAR_H * 0.5f);
    *w = px(f, MM_BAR_R) - *x;
    *h = py(f, MM_CY[row] + MM_BAR_H * 0.5f) - *y;
}

float mainmenu_row_slide(const mainmenu_t *m, int row)
{
    if (!m || row < 0)
        return 0.f;
    if (row == m->press_row && m->press_t >= 0.f)
        return MM_SLIDE * mm_spline(MM_SPL_ENTER, 3,
                                    m->press_t / MM_ANIM_PRESS);
    if (row == m->focus)
        return MM_SLIDE * mm_spline(MM_SPL_FOC, 4, m->anim_t / MM_ANIM_FOC);
    if (row == m->anim_prev)
        return MM_SLIDE * mm_spline(MM_SPL_UNFOC, 3, m->anim_t / MM_ANIM_FOC);
    return 0.f;
}

/* The bar as DRAWN: the rest rect with its left cap pulled out by the slide.
   Only the cap moves -- the right end is the screen edge on every row, so the
   label, which is right-aligned, does not walk about while a row animates. */
static void mm_bar_draw_rect(const mainmenu_t *m, const mmframe *f, int row,
                             float slide,
                             float *x, float *y, float *w, float *h)
{
    (void)m;
    mm_bar_rect(f, row, x, y, w, h);
    *x -= px(f, slide);
    *w += px(f, slide);
}

/* THE WEDGE UNDER ONE ROW, anchored on its own tip. It does not take the slide:
   see MM_SPL_*. The right edge is carried to MM_BAR_R where the cell would stop
   short of it -- a tip column of 99 leaves the plate 8 px shy of the frame's
   edge, and those 8 px are the only ones the bar does not cover. */
static void mm_draw_wedge(const mainmenu_t *m, const mmframe *f, int row)
{
    const float top = MM_CY[row] - MM_BAR_H * 0.5f
                      - MM_WEDGE_RISE - MM_WEDGE_TOP;
    float x0, x1;

    if (row < 0 || row >= 9 || !m->tex.wedge[row])
        return;
    x0 = MM_X0[row] + MM_WEDGE_TIP - MM_WEDGE_TIP_U[row];
    x1 = x0 + 256.f;
    if (x1 < MM_BAR_R)
        x1 = MM_BAR_R;
    ui_image(px(f, x0), py(f, top),
             px(f, x1) - px(f, x0),
             py(f, top + MM_WEDGE_CELL) - py(f, top),
             m->tex.wedge[row], 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f);
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
    /* AND NO QUIT BOX. The button is not drawn on this page (mm_draw_quit), and
       a hit box under nothing is how a touch UI grows a corner that does
       something invisible -- so the corner answers -1 and the touch falls
       through to the desktop. */
    return -1;
}

/* An enum's two arrows on screen. `back' is the one at the SE split, `fwd' the
   one at the far end; both are MM_Q_ARROW square and centred on the row. */
static void mm_q_arrows(const mmframe *f, int page, int row,
                        float *bx, float *fx, float *ay, float *sz)
{
    const mm_enum *e = &MM_Q_ENUM[page][row];
    *sz = MM_Q_ARROW * f->us;
    *ay = py(f, e->y0 + e->sy * 0.5f) - *sz * 0.5f;
    *bx = px(f, e->x0 + e->sx * e->se);
    *fx = px(f, e->x0 + e->sx) - *sz;
}

/* THE FOCUS RING OF ONE SIBLING PAGE, in the order the pad walks it: the page's
   own enums down the left, then the four navigation bars down the right, then
   the green Race button, then Main menu. The Garage bar is in the ring's TABLE
   and is skipped by mainmenu_q_stop's caller the way the main menu skips its
   five dead rows -- a focus that lands somewhere it cannot act reads as the pad
   being broken. */
static int mm_q_ring(int page, int *out)
{
    int n = 0, i;

    if (page < 0 || page >= MM_N_PAGES)
        return 0;
    for (i = 0; i < MM_Q_NENUM[page]; i++)
        out[n++] = i;
    for (i = 0; i < MM_QB_N; i++)
        if (MM_QB_PAGE[i] >= 0)
            out[n++] = MM_Q_NAV + i;
    out[n++] = MM_Q_RACE;
    out[n++] = MM_Q_BACK;
    return n;
}

int mainmenu_q_nfocus(int page)
{
    int ring[MM_Q_N_FOCUS];
    return mm_q_ring(page, ring);
}

int mainmenu_q_stop(int page, int i)
{
    int ring[MM_Q_N_FOCUS];
    const int n = mm_q_ring(page, ring);
    if (n <= 0)
        return -1;
    while (i < 0) i += n;
    return ring[i % n];
}

/* Where `focus' sits in the ring, or 0 if it is not on this page at all --
   which is what a page switch leaves behind, and starting the new page at its
   first stop is the right answer to it. */
static int mm_q_index(int page, int focus)
{
    int ring[MM_Q_N_FOCUS];
    const int n = mm_q_ring(page, ring);
    int i;
    for (i = 0; i < n; i++)
        if (ring[i] == focus)
            return i;
    return 0;
}

/* ONE SLOT'S BOX in the shot list, in design pixels, and WHICH SHOT IS IN IT.
 *
 * THE LIST WRAPS. `slot' is an offset from the selection, -3..+1, and the shot
 * it shows is (selected + slot) MODULO five -- the same ring the main menu's
 * carousel walks. The game's own screenshot of this page is what says so: its
 * big slot holds shot_beach1_0, the default, and to its LEFT stand 2, 3 and 4
 * in that order with 1 to its right. Read as absolute indices instead, the
 * strip came out rotated by two and the big picture was the wrong photograph.
 *
 * The selected slot is the big rectangle; the rest step outward from it and are
 * dropped when they fall outside the list's own bounds, which is what leaves
 * three thumbnails to the left and one to the right. */
static int mm_shot_slot(const mainmenu_t *m, int slot, int *shot,
                        float *x, float *y, float *w, float *h)
{
    const float pitch = MM_SHOTLIST_W + DLG_MAPINFO_shotListSpace;
    const int d = slot;

    if (shot)
        *shot = ((m->shot + slot) % MM_N_SHOTS + MM_N_SHOTS) % MM_N_SHOTS;
    if (slot <= -MM_N_SHOTS || slot >= MM_N_SHOTS)
        return 0;
    if (d == 0) {
        *x = DLG_MAPINFO_shotListX0;
        *y = DLG_MAPINFO_shotListY0;
        *w = DLG_MAPINFO_shotListSX;
        *h = DLG_MAPINFO_shotListSY;
        return 1;
    }
    *y = DLG_MAPINFO_shotListBoundUp;
    *w = MM_SHOTLIST_W;
    *h = MM_SHOTLIST_H;
    if (d < 0)
        *x = DLG_MAPINFO_shotListX0 - DLG_MAPINFO_shotListSpace
             + (float)d * pitch + (pitch - MM_SHOTLIST_W);
    else
        *x = DLG_MAPINFO_shotListX0 + DLG_MAPINFO_shotListSX
             + DLG_MAPINFO_shotListSpace + (float)(d - 1) * pitch;
    return *x >= DLG_MAPINFO_shotListBoundL
        && *x + *w <= DLG_MAPINFO_shotListBoundR;
}

int mainmenu_shot_at(const mainmenu_t *m, int screen_w, int screen_h,
                     float x, float y)
{
    const mmframe f = mm_frame(screen_w, screen_h);
    int slot;

    if (!m || m->page != MM_PAGE_MAPINFO)
        return -1;
    for (slot = -(MM_N_SHOTS - 1); slot < MM_N_SHOTS; slot++) {
        float dx, dy, dw, dh, bx, by, bw, bh;
        int sh;
        if (!mm_shot_slot(m, slot, &sh, &dx, &dy, &dw, &dh))
            continue;
        mm_box(&f, dx, dy, dw, dh, &bx, &by, &bw, &bh);
        if (touch_in(x, y, bx, by, bw, bh))
            return sh;
    }
    return -1;
}

int mainmenu_q_row_at(const mainmenu_t *m, int screen_w, int screen_h,
                      float x, float y, int *left)
{
    const mmframe f = mm_frame(screen_w, screen_h);
    const int page = (m && MM_PAGE_IS_QUICK(m->page)) ? m->page : MM_PAGE_QUICK;
    float bx, by, bw, bh;
    int i;

    if (left) *left = 0;
    for (i = 0; i < MM_Q_NENUM[page]; i++) {
        float ax, fx, ay, sz;
        mm_q_arrows(&f, page, i, &ax, &fx, &ay, &sz);
        /* THE ARROWS ARE THE BUTTONS, and each is grown by half its own size so
           a thumb has something to hit -- 34 design px is 31 on a 544-tall
           panel, which is under the 9 mm a fingertip covers. The two boxes
           cannot meet: the narrowest enum is dlgSTAT's enumStatType, 166 px
           wide, which still leaves 98 between its two arrows. */
        if (touch_in(x, y, ax - sz * 0.25f, ay - sz * 0.25f,
                     sz * 1.5f, sz * 1.5f)) {
            if (left) *left = 1;
            return i;
        }
        if (touch_in(x, y, fx - sz * 0.25f, ay - sz * 0.25f,
                     sz * 1.5f, sz * 1.5f))
            return i;
    }
    /* THE FOUR NAVIGATION BARS, on the main menu's own first four row rects --
       the hit box is the bar AT REST, not the slid one, for the reason
       mm_bar_rect gives. A DEAD bar still answers, so a touch on the Garage can
       deny rather than do nothing. */
    for (i = 0; i < MM_QB_N; i++) {
        mm_bar_rect(&f, i, &bx, &by, &bw, &bh);
        if (touch_in(x, y, bx, by, bw, bh))
            return MM_Q_NAV + i;
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

/* THE MAIN MENU HAS NO QUIT BUTTON. `MM_FOCUS_QUIT' is still the ring's last
   index and the bottom-right button is still drawn on every OTHER page -- where
   it says Main menu, Back, Continue or Disconnect -- but on the front page
   itself there is nothing to draw and nothing to visit: a Vita app is left with
   the PS button, and every write this one does is on an event rather than at
   exit. So the stop is dead here, and it is skipped rather than landed on and
   refused, which is the rule the five unbuilt rows already follow. See ui.md. */
static int mm_focus_live(int focus)
{
    if (focus == MM_FOCUS_RACE)
        return 1;
    if (focus == MM_FOCUS_QUIT)
        return 0;
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
    m->shot = 0;                   /* shot_<track>_0, the carousel's own */
    m->stat = REC_STAT_BEST_LAP;
    m->laps = MM_LAPS_DEF;
    m->skill = 1;                  /* normal, which is what ai_init was passed */
    m->armed = -1;
    /* the focus starts SETTLED, not sliding: the menu opens with Quick race
       already out, the way the game's own screen is, rather than animating a
       button in on the first frame the player sees. */
    m->anim_t = MM_ANIM_FOC;
    m->anim_prev = -1;
    m->press_t = -1.f;
    m->press_row = -1;
    m->pfocus = MM_P_LIST;
    m->parmed = -1;
    m->marmed = -1;
    m->psort = MM_SORT_SCORE;
    /* The Garage's own state. `gskins' is 1 rather than 0 because it is a COUNT
       and every wrap divides by it -- see garage_next_skin; the caller writes
       the loaded car's real one after every load_car. */
    m->gfocus = MM_G_TAB + MM_GB_SELECT;
    m->gkind = GAR_BOOSTER;
    m->gsel = 0;
    m->garmed = -1;
    m->gskins = 1;
    mainmenu_players_sync(m);
}

/* The focus moves, and both rows start animating: the one arriving on the focus
   curve, the one leaving on the unfocus curve. One clock drives the pair. */
static void mm_set_focus(mainmenu_t *m, int row)
{
    if (row == m->focus)
        return;
    m->anim_prev = m->focus;
    m->focus = row;
    m->anim_t = 0.f;
}

/* ------------------------------------------------------------------ a step */

static void mm_fire(mainmenu_t *m, int focus)
{
    /* THE PRESS ANIMATES whatever it lands on, live or not -- a denied button
       that does not move under the thumb reads as a dropped input rather than
       as a refusal. */
    m->press_row = focus;
    m->press_t = 0.f;
    switch (focus) {
    case MM_QUICK_RACE:
        /* THE SETUP SCREEN, not the race. dlgRACESUM is what this button opens
           in the original; the green Race button below is what starts one, on
           this page and on that one. */
        m->qfrom = MM_PAGE_QUICK;
        m->page = MM_PAGE_QUICK;
        m->cue = MM_CUE_PRESS;
        break;
    case MM_FOCUS_RACE:
        m->action = MM_ACT_RACE;
        m->cue = MM_CUE_PRESS;
        break;
    case MM_CHAMPIONSHIP:
        /* dlgCHAMP, the ladder -- and like Quick race and Multiplayer the
           button opens the SETUP and not a race. Nothing has been charged. */
        mainmenu_open_champ(m);
        m->cue = MM_CUE_PRESS;
        break;
    case MM_CHANGE_PLAYER:
        mainmenu_open_players(m, player_count() == 0);
        m->cue = MM_CUE_PRESS;
        break;
    case MM_MULTIPLAYER:
        /* dlgMULTIPLAYER. Like Quick race, the button opens the SETUP and not a
           race; nothing has touched the socket yet. */
        mainmenu_open_multi(m);
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
    /* NO `case MM_FOCUS_QUIT' -- the front page has no Quit button, so the ring
       never lands there and a touch in that corner answers -1. `MM_ACT_QUIT'
       itself is kept: it is the app's one ORDERLY shutdown (the profile and the
       award book written, the network told) and nothing raises it today, which
       is a fact worth stating rather than a leftover. Give any row this line and
       it works. */
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
   except the two volumes, and for the same reason: there is no "off" here.
   `row' is an index into the CURRENT page's enums, so the same 0 is the track
   picker on two pages and the sort key on the third. */
static void mm_q_move(mainmenu_t *m, int row, int d)
{
    if (row < 0 || row >= MM_Q_NENUM[m->page])
        return;
    if (m->page == MM_PAGE_MAPINFO) {
        if (row == MM_Q_MI_TRACK) {
            m->track += d;
            while (m->track < 0) m->track += N_TRACKS;
            while (m->track >= N_TRACKS) m->track -= N_TRACKS;
        } else {
            m->shot = (m->shot + d + MM_N_SHOTS) % MM_N_SHOTS;
        }
        m->cue = MM_CUE_ARROW;
        return;
    }
    if (m->page == MM_PAGE_STATS) {
        m->stat = (m->stat + d + REC_N_STAT) % REC_N_STAT;
        m->cue = MM_CUE_ARROW;
        return;
    }
    if (m->page == MM_PAGE_AWARDS) {
        /* ONE ROW AT A TIME AND IT DOES NOT WRAP, which is the difference
           between this picker and every other one on these pages: those pick a
           VALUE out of a ring and this one moves a VIEW over a list, where
           running off the end and reappearing at the top is not a step, it is a
           jump. The last full page is the bottom stop. */
        const int last = AW_N - MM_AW_ROWS;
        m->aw_top += d;
        if (m->aw_top > last) m->aw_top = last;
        if (m->aw_top < 0) m->aw_top = 0;
        m->cue = MM_CUE_ARROW;
        return;
    }
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

/* Open one of the four sibling views. THE FOCUS STAYS ON THE BAR that opened
   it, rather than jumping to the new page's first control: the player is walking
   the tabs, and a focus that leaves the column after every press makes the
   second tab two presses away. It is also what the game's own screenshots of
   these two pages show -- every enum arrow on both of them is at rest.
 *
   THE GARAGE IS THE EXCEPTION, because it is not a view of this race: it is a
   screen of its own with its own header, its own four bars and its own focus
   ring, so it is opened rather than switched to -- which is exactly why the
   game's own shot of this page draws an ARROW on that bar and a dot on the
   other three. */
/* THE CHAMPIONSHIP'S three, used from the quick-race page's own fire handler
   because dlgMAPINFO and dlgSTAT are on BOTH navigation columns. Defined with
   the rest of that page, far below. */
static void mg_say(mainmenu_t *m, const char *line);
static void mm_c_open_race(mainmenu_t *m);
static ch_result mm_c_can_race(const mainmenu_t *m, const player_t *p);

static void mm_q_nav(mainmenu_t *m, int bar)
{
    if (bar < 0 || bar >= MM_QB_N)
        return;
    if (MM_QB_PAGE[bar] == MM_PAGE_GARAGE) {
        mainmenu_open_garage(m, -1);
        m->cue = MM_CUE_PRESS;
        return;
    }
    /* THE FIRST BAR IS THE WAY HOME, and home is not always the quick-race
       summary: dlgMAPINFO and dlgSTAT are on the CHAMPIONSHIP's navigation
       column too (the game's own shot of dlgCHAMP has both), so the bar that
       says "Race summary" there says "Championship" and returns to the ladder.
       `qfrom' is which. */
    if (bar == MM_QB_SUMMARY && m->qfrom == MM_PAGE_CHAMP) {
        m->csel = m->track;
        mainmenu_open_champ(m);
        m->cfocus = MM_C_NAV + MM_CB_CHAMP;
        m->cue = MM_CUE_PRESS;
        return;
    }
    m->page = MM_QB_PAGE[bar];
    m->qfocus = MM_Q_NAV + bar;
    m->cue = MM_CUE_PRESS;
}

/* What the focused stop does when CROSS lands on it. */
static void mm_q_fire(mainmenu_t *m, int focus)
{
    if (focus == MM_Q_RACE) {
        /* THE GREEN BUTTON STARTS WHAT THIS PAGE IS A VIEW OF. Reached from the
           championship's own column it is a view of a ROUND, so it goes to the
           fee panel and not into a quick race -- otherwise the same button on
           the same screen would start two different things depending on a
           breadcrumb the player cannot see. */
        if (m->qfrom == MM_PAGE_CHAMP) {
            const player_t *p = player_cur();
            ch_result r;
            m->csel = m->track;
            r = mm_c_can_race(m, p);
            if (r != CH_OK) {
                mg_say(m, r == CH_NO_CAR ? garage_reason(GAR_NOT_OWNED)
                                         : champ_reason(r));
                m->cue = MM_CUE_DENY;
                return;
            }
            mm_c_open_race(m);
            return;
        }
        m->action = MM_ACT_RACE;
        m->cue = MM_CUE_PRESS;
    } else if (focus == MM_Q_BACK) {
        m->page = MM_PAGE_MAIN;
        m->qfrom = MM_PAGE_QUICK;
        m->cue = MM_CUE_PRESS;
    } else if (focus >= MM_Q_NAV) {
        mm_q_nav(m, focus - MM_Q_NAV);
    } else {
        mm_q_move(m, focus, +1);   /* CROSS on a picker steps it */
    }
}

static void mm_step_quick(mainmenu_t *m, unsigned int down,
                          const touch_state *tp, int screen_w, int screen_h)
{
    const int page = m->page;
    const int n = mainmenu_q_nfocus(page);

    if (n <= 0)
        return;
    if (down & SCE_CTRL_DOWN) {
        m->qfocus = mainmenu_q_stop(page, mm_q_index(page, m->qfocus) + 1);
        m->cue = MM_CUE_FOCUS;
    }
    if (down & SCE_CTRL_UP) {
        m->qfocus = mainmenu_q_stop(page, mm_q_index(page, m->qfocus) - 1);
        m->cue = MM_CUE_FOCUS;
    }
    if (down & SCE_CTRL_LEFT)  mm_q_move(m, m->qfocus, -1);
    if (down & SCE_CTRL_RIGHT) mm_q_move(m, m->qfocus, +1);
    if (down & (SCE_CTRL_CROSS | SCE_CTRL_START))
        mm_q_fire(m, m->qfocus);
    /* CIRCLE is back, everywhere. From a sibling view it goes to Race summary
       first and only then to the main menu, because that is the screen the
       player opened it from -- one CIRCLE per step in, the way every other back
       button in this app behaves. */
    if (down & SCE_CTRL_CIRCLE) {
        if (page != MM_PAGE_QUICK)
            mm_q_nav(m, MM_QB_SUMMARY);
        else
            m->page = MM_PAGE_MAIN;
        m->cue = MM_CUE_PRESS;
    }

    if (!tp)
        return;
    if (m->page == MM_PAGE_AWARDS) {
        const mmframe f = mm_frame(screen_w, screen_h);
        const int was = m->aw_top;
        if (mm_sb_drive(&f, tp,
                        DLG_STAT_tableStatX0 + DLG_STAT_tableStatSX + MM_SB_GAP,
                        MM_AW_Y0, MM_AW_Y0 + MM_AW_SY,
                        &m->aw_top, AW_N, MM_AW_ROWS, &m->sb_drag)) {
            if (m->aw_top != was)
                m->cue = MM_CUE_FOCUS;
            m->armed = -1;
            return;
        }
    } else {
        m->sb_drag = 0;
    }
    if (tp->pressed) {
        int left;
        m->armed = mainmenu_q_row_at(m, screen_w, screen_h,
                                     tp->x, tp->y, &left);
        if (m->armed >= 0 && m->qfocus != m->armed) {
            m->qfocus = m->armed;
            m->cue = MM_CUE_FOCUS;
        }
        /* A SCREENSHOT IS A BUTTON TOO, and the selected one is not -- the same
           rule the main menu's carousel follows, and the same reason: tapping
           the picture you are looking at should do nothing. */
        {
            const int sh = mainmenu_shot_at(m, screen_w, screen_h, tp->x, tp->y);
            if (sh >= 0 && sh != m->shot) {
                m->shot = sh;
                m->cue = MM_CUE_ARROW;
            }
        }
    }
    if (tp->released) {
        int left;
        const int row = mainmenu_q_row_at(m, screen_w, screen_h,
                                          tp->x, tp->y, &left);
        if (row >= 0 && row == m->armed) {
            if (row < MM_Q_RACE) {
                /* THE BAR'S OWN TRIANGLE GOES BACK, the rest goes on -- so one
                   row walks both ways under a thumb and neither direction needs
                   a second control. */
                mm_q_move(m, row, left ? -1 : +1);
            } else {
                mm_q_fire(m, row);
            }
        }
        m->armed = -1;
    }
}

/* dlgPLRSCOMM's own step, defined with the rest of that page below -- it needs
   the layout helpers the drawing half is written against. So are the Garage's
   own step and the modal machinery both pages share. */
static void mm_step_players(mainmenu_t *m, unsigned int down,
                            const touch_state *tp, int screen_w, int screen_h);
static void mm_step_garage(mainmenu_t *m, unsigned int down,
                           const touch_state *tp, int screen_w, int screen_h);
static void mm_step_multi(mainmenu_t *m, unsigned int down,
                          const touch_state *tp, int screen_w, int screen_h);
static void mm_step_lobby(mainmenu_t *m, unsigned int down,
                          const touch_state *tp, int screen_w, int screen_h);
static void mm_step_champ(mainmenu_t *m, unsigned int down,
                          const touch_state *tp, int screen_w, int screen_h);
static void mm_step_chrace(mainmenu_t *m, unsigned int down,
                           const touch_state *tp, int screen_w, int screen_h);
static void mm_draw_champ(const mainmenu_t *m, const mmframe *f);
static void mm_draw_chrace(const mainmenu_t *m, const mmframe *f);
/* The server list's own modal, defined with the roster page's panel because it
   stands on that panel's rectangle. */
static void mm_s_step(mainmenu_t *m, unsigned int down, const touch_state *tp,
                      int screen_w, int screen_h);
static void mm_s_draw(const mainmenu_t *m, const mmframe *f);
static void mm_step_modal(mainmenu_t *m, unsigned int down,
                          const touch_state *tp, int screen_w, int screen_h);

void mainmenu_step(mainmenu_t *m, unsigned int buttons, const touch_state *tp,
                   int screen_w, int screen_h, float dt)
{
    unsigned int down;

    if (!m)
        return;
    m->action = MM_ACT_NONE;
    m->cue = MM_CUE_NONE;
    m->t += dt;

    /* THE ANIMATION CLOCKS. A press runs to its end and then HANDS BACK to the
       focus curve -- Enter finishes at 0 and a focused button rests at 0.361,
       so replaying Foc from zero is what carries it back out. That chain is the
       engine's own: three splines, and Enter is the one with no resting value
       of its own. */
    m->anim_t += dt;
    if (m->press_t >= 0.f) {
        m->press_t += dt;
        if (m->press_t > MM_ANIM_PRESS) {
            m->press_t = -1.f;
            m->press_row = -1;
            m->anim_t = 0.f;
            m->anim_prev = -1;
        }
    }

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

    /* A MODAL OWNS EVERY INPUT WHILE IT IS UP, on whichever page raised it --
       the roster's three and the Garage's question are one machine now, so the
       gate is here rather than inside one page's step. */
    if (m->modal) {
        /* THE SERVER LIST IS ITS OWN MODAL: a list and a Cancel, not a
           question and not a keyboard, so it does not go through the roster's
           Ok/Cancel machine. It still OWNS the frame, which is the only thing
           every modal here has in common. */
        if (m->modal == MM_MODAL_SERVERS)
            mm_s_step(m, down, tp, screen_w, screen_h);
        else
            mm_step_modal(m, down, tp, screen_w, screen_h);
        /* AND A JOIN THAT LANDED closes it, which the page below has to see --
           so the multiplayer page's own step still runs under it. */
        if (m->page == MM_PAGE_MULTI && net_mode_now() == NET_JOINED) {
            m->modal = MM_MODAL_NONE;
            mainmenu_open_lobby(m, MM_L_RACESUM);
        }
        return;
    }
    if (m->page == MM_PAGE_PLAYERS) {
        mm_step_players(m, down, tp, screen_w, screen_h);
        return;
    }
    if (MM_PAGE_IS_QUICK(m->page)) {
        mm_step_quick(m, down, tp, screen_w, screen_h);
        return;
    }
    if (MM_PAGE_IS_CAR(m->page)) {
        mm_step_garage(m, down, tp, screen_w, screen_h);
        return;
    }
    if (m->page == MM_PAGE_MULTI) {
        mm_step_multi(m, down, tp, screen_w, screen_h);
        return;
    }
    if (m->page == MM_PAGE_LOBBY) {
        mm_step_lobby(m, down, tp, screen_w, screen_h);
        return;
    }
    if (m->page == MM_PAGE_CHAMP) {
        mm_step_champ(m, down, tp, screen_w, screen_h);
        return;
    }
    if (m->page == MM_PAGE_CHRACE) {
        mm_step_chrace(m, down, tp, screen_w, screen_h);
        return;
    }

    if (down & SCE_CTRL_DOWN) {
        mm_set_focus(m, mm_next_focus(m->focus, +1));
        m->cue = MM_CUE_FOCUS;
    }
    if (down & SCE_CTRL_UP) {
        mm_set_focus(m, mm_next_focus(m->focus, -1));
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
            mm_set_focus(m, m->armed);
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

        /* The wedge first: it is behind the bar, taller, and it stays put
           while the bar slides out over it. */
        mm_draw_wedge(m, f, i);
        mm_bar_draw_rect(m, f, i, mainmenu_row_slide(m, i),
                         &bx, &by, &bw, &bh);

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
    /* CELL 0 IS THE DISABLED PLATE, and the multiplayer front page is the one
       screen that needs it: there is no game to race in until Create or Join
       has made one, and the game's own screenshot of that page draws the button
       grey. */
    if ((m->page == MM_PAGE_MULTI && !mainmenu_m_live(m, MM_M_RACE))
        || (MM_PAGE_IS_CHAMP(m->page) && !mainmenu_c_live(m, MM_C_RACE))
        || (MM_PAGE_IS_QUICK(m->page) && m->qfrom == MM_PAGE_CHAMP
            && !mainmenu_c_live(m, MM_C_RACE))) {
        const sfont sf = sf_big(m->tex.font_big);
        const float sc = f->us * MM_TS_RACE;
        const float tx = x + w * 0.30f;
        if (m->tex.race)
            ui_image(x, y, w, h, m->tex.race, 0.f, 0.f, 1.f, 64.f / 512.f,
                     1.f, 1.f, 1.f, 1.f);
        else
            ui_rect(x, y, w, h, 0.28f, 0.30f, 0.28f, 1.f);
        /* AND IT KEEPS ITS WORD, dimmed to the 0.82 the main menu's dead rows
           use -- the game's own screenshot of this page has `Race' on the grey
           plate, and a button with no label reads as a button that failed to
           draw. */
        if (sf.tex)
            sf_text(&sf, tx, y + (h - sf_h(&sf, sc)) * 0.5f, sc,
                    0.82f, 0.82f, 0.84f, 1.f, STR_UI_RACE);
        else
            ui_text(tx, y + (h - ui_text_h(sc)) * 0.5f, sc,
                    0.82f, 0.82f, 0.84f, 1.f, STR_UI_RACE);
        return;
    }
    cell = (MM_PAGE_IS_CHAMP(m->page) ? (m->cfocus == MM_C_RACE)
            : (m->page == MM_PAGE_PLAYERS ? (m->pfocus == MM_P_RACE)
            : (MM_PAGE_IS_QUICK(m->page) ? (m->qfocus == MM_Q_RACE)
            : (MM_PAGE_IS_CAR(m->page) ? (m->gfocus == MM_G_RACE)
            : (m->page == MM_PAGE_MULTI ? (m->mfocus_multi == MM_M_RACE)
            : (m->page == MM_PAGE_LOBBY ? (m->lfocus == MM_LB_RACE)
                                        : (m->focus == MM_FOCUS_RACE)))))))
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
                    1.f, 1.f, 1.f, 1.f, STR_UI_RACE);
        else
            ui_text(tx, y + (h - ui_text_h(sc)) * 0.5f, sc,
                    1.f, 1.f, 1.f, 1.f, STR_UI_RACE);
    }
}

static void mm_draw_quit(const mainmenu_t *m, const mmframe *f)
{
    float x, y, w, h;
    /* NOT ON THE FRONT PAGE, tray and all: with no Quit there is no button to
       sit on the silver bar, and a bare tray in the corner reads as a button
       that failed to draw. Every other page keeps both -- that is where the
       word is Main menu, Back, Continue or Disconnect. */
    if (m->page == MM_PAGE_MAIN)
        return;
    const int lit = MM_PAGE_IS_CHAMP(m->page)
                    ? (m->cfocus == MM_C_BACK)
                    : (m->page == MM_PAGE_PLAYERS
                    ? (m->pfocus == MM_P_CONTINUE)
                    : (MM_PAGE_IS_QUICK(m->page)
                       ? (m->qfocus == MM_Q_BACK)
                    : (MM_PAGE_IS_CAR(m->page)
                       ? (m->gfocus == MM_G_BACK)
                    : (m->page == MM_PAGE_MULTI
                       ? (m->mfocus_multi == MM_M_BACK)
                    : (m->page == MM_PAGE_LOBBY
                       ? (m->lfocus == MM_LB_BACK)
                       : (m->focus == MM_FOCUS_QUIT))))));

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
    /* THE BOTTOM-RIGHT BUTTON SAYS WHERE IT GOES, and on the car pages that is
       BACK -- 10003, and the word on the game's own Garage screenshot. */
    mm_label(m, f, x, y, w, h,
             m->page == MM_PAGE_PLAYERS ? STR_UI_CONTINUE
             : (MM_PAGE_IS_CHAMP(m->page) ? STR_UI_MAIN_MENU
             : (MM_PAGE_IS_QUICK(m->page) ? STR_UI_MAIN_MENU
             : (MM_PAGE_IS_CAR(m->page) ? STR_UI_BACK
             : (m->page == MM_PAGE_MULTI ? STR_UI_MAIN_MENU
             : (m->page == MM_PAGE_LOBBY ? STR_UI_DISCONNECT
                                         : STR_UI_QUIT))))),
             1.f, 1.f, 1.f);
}

static void mm_draw_header(const mainmenu_t *m, const mmframe *f)
{
    const float x = px(f, MM_HDR_X);
    const float w = px(f, MM_HDR_X + MM_HDR_W) - x;
    const float ry = py(f, MM_HDR_RED_Y);
    const float rh = py(f, MM_HDR_RED_Y + MM_HDR_RED_H) - ry;

    if (m->tex.header) {
        /* the plain silver bar (cell 3) under the red plate (cell 2, the one
           with the WHITE triangle) -- see MM_HDR_X. */
        const float sy = py(f, MM_HDR_SIL_Y);
        ui_image(x, sy, w, py(f, MM_HDR_SIL_Y + MM_HDR_SIL_H) - sy,
                 m->tex.header, 0.f, 135.f / 256.f, 1.f, 185.f / 256.f,
                 1.f, 1.f, 1.f, 1.f);
        ui_image(x, ry, w, rh,
                 m->tex.header, 0.f, 76.f / 256.f, 1.f, 116.f / 256.f,
                 1.f, 1.f, 1.f, 1.f);
    }
    {
        const sfont sf = sf_big(m->tex.font_big);
        const float sc = f->us * MM_TS_HEAD;
        /* "Garage" on BOTH car pages, which is what all four of the game's own
           shots of them have -- the part page does not retitle itself. */
        const char *title = MM_PAGE_IS_CHAMP(m->page)
                            || (MM_PAGE_IS_QUICK(m->page)
                                && m->qfrom == MM_PAGE_CHAMP)
                          ? STR_UI_CHAMPIONSHIP
                          : (m->page == MM_PAGE_PLAYERS ? STR_UI_SELECT_PLAYER
                          : (m->page == MM_PAGE_AWARDS ? MM_UI_AWARDS
                          : (MM_PAGE_IS_QUICK(m->page) ? STR_UI_QUICK_RACE
                          : (MM_PAGE_IS_CAR(m->page) ? STR_UI_GARAGE
                          : (m->page == MM_PAGE_MULTI ? STR_UI_MULTIPLAYER
                          : (m->page == MM_PAGE_LOBBY ? STR_UI_WAIT_PLAYERS
                                                      : STR_UI_MAIN_MENU))))));
        /* right-aligned against the triangle the cell already carries */
        const float tr = px(f, MM_HDR_X + MM_HDR_TEXT_R);
        const float tw = sf.tex ? sf_w(&sf, sc, title) : ui_text_w(sc, title);
        const float th = sf.tex ? sf_h(&sf, sc) : ui_text_h(sc);
        const float tx = tr - tw;
        const float ty = ry + (rh - th) * 0.5f + f->us * MM_HDR_TEXT_DY;
        if (sf.tex)
            sf_text(&sf, tx, ty, sc, 1.f, 1.f, 1.f, 1.f, title);
        else
            ui_text(tx, ty, sc, 1.f, 1.f, 1.f, 1.f, title);
    }
    /* THE BADGE IS THE MAIN MENU'S ALONE. The game's own screenshots of Map and
       info and of Track stats both have bare oval where logoRC_Main would be,
       and the port drew it on every page. */
    if (m->tex.logo && m->page == MM_PAGE_MAIN) {
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
    const player_t *p = player_cur();
    float x, y, w, h;
    float tx, ty;
    char line[96], t[24];
    int face;

    /* ON THE SAME ANCHOR as the carousel, so the whole left block is one group:
       the card's text column and the track's name under the photographs line
       up in the original, and they only keep lining up if the portrait, the
       row and the caption all move together. */
    mm_gbox(f, MM_GROUP_X, MM_FACE_X, MM_FACE_Y, MM_FACE_W, MM_FACE_H,
            &x, &y, &w, &h);
    /* THE COLUMN HANGS OFF THE PORTRAIT rather than being mapped on its own, so
       the gap between the two stays the gap the artists left. See gx(). */
    tx = x + w + (MM_INFO_X - (MM_FACE_X + MM_FACE_W)) * f->us;
    /* THE PORTRAIT IS THE PROFILE'S, out of the nine, and it brings its own
       silver frame -- see MM_FACE_U0. A missing one leaves a plate where the
       layout says the card is, so the row of text beside it still reads as a
       card. */
    face = p && p->face >= 0 && p->face < PL_N_FACES ? p->face : 0;
    if (m->tex.face[face])
        ui_image(x, y, w, h, m->tex.face[face],
                 MM_FACE_U0, MM_FACE_V0, MM_FACE_U1, MM_FACE_V1,
                 1.f, 1.f, 1.f, 1.f);
    else
        ui_rect(x, y, w, h, 0.20f, 0.22f, 0.28f, 1.f);

    ty = MM_INFO_Y;
    snprintf(line, sizeof line, "Welcome, %s",
             p ? p->name : STR_UI_DEFAULT_NAME);
    if (big.tex) {
        const float sc = f->us * MM_TS_WELCOME;
        sf_text_shadowed(&big, tx, py(f, ty), sc, 1.f, 1.f, 1.f, 1.f, line);
    } else {
        ui_text(tx, py(f, ty), f->us, 1.f, 1.f, 1.f, 1.f, line);
    }
    mm_rule_at(f, tx, py(f, ty + 26.f), 290.f * f->us);
    ty += 34.f;

    /* THE FIVE FACTS THE ORIGINAL'S CARD READS -- Rank, Current car, Play time,
     * Scores and Cash -- and every one of them is a PROFILE field. This file
     * used to say that the `Players/' .scp files were a format the port did not
     * read; player.c reads and writes them, so the card is the game's own again.
     *
     * With no profile at all -- a first launch, before Create player -- what is
     * here instead is what was here before: the car, the track and the size of
     * the field, out of tracks.h and ai_data.h. Those are true without a
     * profile; Rank and Cash are not. */
    {
        int i, n = 0;
        const char *l[5];
        char buf[5][96];
        if (p) {
            snprintf(buf[0], sizeof buf[0], "%s: %s", STR_UI_RANK,
                     player_rank(p));
            snprintf(buf[1], sizeof buf[1], "%s: %s", STR_UI_CURRENT_CAR,
                     STR_CAR_NAME[p->sel_car >= 0 && p->sel_car < STR_N_CARS
                                  ? p->sel_car : 0]);
            player_time_str(p->play_time, t, sizeof t);
            snprintf(buf[2], sizeof buf[2], "%s: %s", STR_UI_PLAY_TIME, t);
            snprintf(buf[3], sizeof buf[3], "%s: %d    %s: $%d",
                     STR_UI_SCORES, p->scores, STR_UI_CASH, p->cash);
            n = 4;
        } else {
            const int car = TRACKS[m->track].car;
            snprintf(buf[0], sizeof buf[0], "Car: %s",
                     STR_CAR_NAME[car >= 0 && car < 3 ? car : 0]);
            snprintf(buf[1], sizeof buf[1], "Track: %s",
                     STR_TRACK_NAME[m->track]);
            snprintf(buf[2], sizeof buf[2], "Field: %d opponents",
                     AI_RACES[m->track].n);
            n = 3;
        }
        for (i = 0; i < n; i++)
            l[i] = buf[i];
        for (i = 0; i < n; i++) {
            if (small_.tex)
                sf_text_shadowed(&small_, tx, py(f, ty),
                                 f->us * MM_TS_INFO, 1.f, 1.f, 1.f, 1.f, l[i]);
            else
                ui_text(tx, py(f, ty), f->us * 0.9f,
                        1.f, 1.f, 1.f, 1.f, l[i]);
            ty += 22.f;
        }
    }
}

/* THE CAROUSEL, and `with_caption' 0 leaves the track's name and its blurb to
   the caller. dlgMULTIPLAYER draws the same five photographs but puts that text
   at ITS own two rectangles -- staticInfoHeader (233, 417) and staticInfo
   (234, 449) -- which are not the main menu's MM_NAME_Y and MM_DESC_Y. Drawn
   both ways it came out twice, overlapping, which is what the picture showed. */
static void mm_draw_carousel_at(const mainmenu_t *m, const mmframe *f,
                                int with_caption)
{
    const sfont big = sf_big(m->tex.font_big);
    const sfont small_ = sf_small(m->tex.font_small);
    int slot;
    char line[96];
    float ty, nx;

    for (slot = 0; slot < 5; slot++) {
        const int t = mm_slot_track(m, slot);
        float x, y, w, h;
        mm_slot_rect(f, slot, &x, &y, &w, &h);
        /* The photograph brings its own rounded silver frame -- the measured box
           IS the framed picture, so nothing is drawn behind it. */
        if (m->tex.shot[t][0])
            ui_image(x, y, w, h, m->tex.shot[t][0],
                     MM_SHOT_U0, MM_SHOT_V0, MM_SHOT_U1, MM_SHOT_V1,
                     1.f, 1.f, 1.f, 1.f);
        else
            ui_rect(x, y, w, h, 0.25f, 0.28f, 0.34f, 1.f);
    }

    /* THE NAME AND ITS LINES sit under the big photograph and move with it --
       gx() again, so the block does not drift off the picture it describes. */
    nx = gx(f, MM_GROUP_X, MM_NAME_X);
    if (!with_caption)
        return;
    if (big.tex)
        sf_text_shadowed(&big, nx, py(f, MM_NAME_Y),
                         f->us * MM_TS_TRACK, 1.f, 1.f, 1.f, 1.f,
                         STR_TRACK_NAME[m->track]);
    else
        ui_text(nx, py(f, MM_NAME_Y), f->us * MM_TS_TRACK,
                1.f, 1.f, 1.f, 1.f, STR_TRACK_NAME[m->track]);
    mm_rule_at(f, nx, py(f, MM_NAME_Y + 28.f), 180.f * f->us);

    /* THREE LINES OF THE AUTHORS' OWN PROSE, 40040 + t -- and the line this
       file's header quotes as measured off the original's screen,
       "Start the race along the sea", is the first of Surf's three. These notes
       recorded this text as lost behind an unread string table and put the
       track's AI roster here instead; str_data.h carries it now. */
    ty = MM_DESC_Y;
    {
        const char *info = STR_TRACK_MENU[m->track];
        while (*info) {
            const char *nl = strchr(info, '\n');
            size_t len = nl ? (size_t)(nl - info) : strlen(info);
            if (len >= sizeof line) len = sizeof line - 1;
            memcpy(line, info, len);
            line[len] = 0;
            if (small_.tex)
                sf_text_shadowed(&small_, nx, py(f, ty),
                                 f->us * MM_TS_INFO, 1.f, 1.f, 1.f, 1.f, line);
            else
                ui_text(nx, py(f, ty), f->us * 0.9f, 1.f, 1.f, 1.f, 1.f, line);
            ty += MM_DESC_LH;
            if (!nl) break;
            info = nl + 1;
        }
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

/* The four values of `Sort results by', 40607..40610. */
static const char *const MM_ST_NAME[REC_N_STAT] = {
    STR_UI_BEST_LAP, STR_UI_3_LAPS, STR_UI_5_LAPS, STR_UI_7_LAPS
};

/* AND THE TWO LAP LISTS HAVE TO AGREE. MM_LAPS is the picker's and REC_LAPS is
   the record book's, and the stats page indexes one with the other. Two copies
   of one list is how a 5-lap time ends up in the "3 laps" column. */
typedef char mm_laps_agree[(MM_N_LAPS == REC_N_LAPS) ? 1 : -1];

/* THE AWARD LIST'S SCROLL POSITION, clamped. Its own function because three
   places need the same answer -- the picker, the enum's value and the draw --
   and a list whose scroller and whose rows disagree about where it starts shows
   the wrong rows under the right label. */
static int mm_aw_top(const mainmenu_t *m)
{
    const int last = AW_N - MM_AW_ROWS;
    int t = m ? m->aw_top : 0;
    if (t > last) t = last;
    if (t < 0) t = 0;
    return t;
}

/* The value a row shows BETWEEN ITS TWO ARROWS, or "" for the three enums whose
   value is a picture -- the track photograph and the car viewport on Race
   summary, the screenshot strip on Map and info. Every one is shipped data or a
   count of it. */
static void mm_q_value(const mainmenu_t *m, int row, char *out, int n)
{
    out[0] = 0;
    if (m->page == MM_PAGE_MAPINFO) {
        if (row == MM_Q_MI_TRACK)
            snprintf(out, n, "%s", STR_TRACK_NAME[m->track]);
        return;
    }
    if (m->page == MM_PAGE_STATS) {
        const int k = (m->stat < 0 || m->stat >= REC_N_STAT) ? 0 : m->stat;
        snprintf(out, n, "%s", MM_ST_NAME[k]);
        return;
    }
    if (m->page == MM_PAGE_AWARDS) {
        /* WHICH ROWS ARE ON SCREEN, which is a scroller's own value -- and it
           is also the only place the page says how long the list is. */
        const int top = mm_aw_top(m);
        snprintf(out, n, "%d-%d of %d", top + 1, top + MM_AW_ROWS, AW_N);
        return;
    }
    switch (row) {
    case MM_Q_LAPS:
        snprintf(out, n, "%d", m->laps);
        break;
    case MM_Q_SKILL:
        snprintf(out, n, "%s", mainmenu_skill_name(m->skill));
        break;
    default:
        break;
    }
}

/* A time as the game writes one: `1:00.62'. Hundredths, because that is the
   precision the original's own table shows and the precision race_ui keeps.
   A time of zero or less is `n/a', which is the string table's own word for a
   racer with no record on this track. */
static void mm_time(char *out, int n, float t)
{
    int cs;
    if (!(t > 0.f)) {
        snprintf(out, n, "%s", STR_UI_NA);
        return;
    }
    cs = (int)(t * 100.f + 0.5f);
    snprintf(out, n, "%d:%02d.%02d", cs / 6000, (cs / 100) % 60, cs % 100);
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

/* A blurb, one line per '\n', at the pitch MM_LINE_H records. The game's own
   descriptions are three or five lines and arrive from str_data.h with their
   newlines intact, so nothing here has to wrap. */
static void mm_q_lines(const mainmenu_t *m, const mmframe *f, int big_font,
                       float x, float y, float ts, const char *s)
{
    char line[128];
    while (s && *s) {
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s) : strlen(s);
        if (len >= sizeof line)
            len = sizeof line - 1;
        memcpy(line, s, len);
        line[len] = 0;
        mm_q_text(m, f, big_font, x, y, ts, 0, 1.f, line);
        y += MM_LINE_H;
        if (!nl)
            break;
        s = nl + 1;
    }
}

/* One screenshot of the current track, at a box already in design pixels. The
   photograph brings its own rounded silver frame -- MM_SHOT_U0 -- so nothing is
   drawn behind it. */
static void mm_q_shot(const mainmenu_t *m, const mmframe *f, int idx,
                      float dx, float dy, float dw, float dh)
{
    float x, y, w, h;
    const unsigned int t = (idx >= 0 && idx < MM_N_SHOTS)
                           ? m->tex.shot[m->track][idx] : 0;
    mm_box(f, dx, dy, dw, dh, &x, &y, &w, &h);
    if (t)
        ui_image(x, y, w, h, t, MM_SHOT_U0, MM_SHOT_V0, MM_SHOT_U1, MM_SHOT_V1,
                 1.f, 1.f, 1.f, 1.f);
    else
        ui_rect(x, y, w, h, 0.25f, 0.28f, 0.34f, 1.f);
}

/* ------------------------------------------------ the three pages' furniture
 *
 * THE NAVIGATION COLUMN, shared by all three. The first three bars are RADIO
 * buttons out of RadioButtonsTextures -- red with a dark dot, red with a WHITE
 * one, grey -- and the page you are on is the white one. Only the Garage is an
 * arrow, off ButtonsTextures' grey cell, because it is the one of the four that
 * is not built.
 *
 * THE ROW YOU ARE ON STANDS OUT, at the focus curve's own settled 0.361, and
 * that is measured: on the game's shot of Map and info that bar's cap sits 25 px
 * left of where its siblings' do, and on the shot of Track stats it is 28. The
 * FOCUSED row slides too, on the same curve -- the art has no focused radio
 * cell, so the slide is the only signal the artists left for it, and a row that
 * is both takes the larger of the two.
 */
static void mm_draw_qnav(const mainmenu_t *m, const mmframe *f)
{
    int i;

    for (i = 0; i < MM_QB_N; i++) {
        float bx, by, bw, bh, v, slide;
        const int live = MM_QB_PAGE[i] >= 0;
        const int here = MM_QB_PAGE[i] == m->page;
        const int lit = live && m->qfocus == MM_Q_NAV + i;

        slide = here ? MM_SLIDE * MM_SETTLED : 0.f;
        if (lit && MM_SLIDE * MM_SETTLED > slide)
            slide = MM_SLIDE * MM_SETTLED;
        mm_draw_wedge(m, f, i);
        mm_bar_draw_rect(m, f, i, slide, &bx, &by, &bw, &bh);

        /* THE FIRST THREE ARE RADIO CELLS and the GARAGE IS AN ARROW, which is
           what the game's own shot of this page has: a dot on the three views of
           this race and a triangle on the one bar that goes to a screen of its
           own. It used to be the grey cell, because the Garage was not built. */
        if (live && i != MM_QB_GARAGE && m->tex.radio) {
            v = here ? MM_V_RAD_ON : MM_V_RAD;
            ui_image(bx, by, bw, bh, m->tex.radio,
                     0.f, v, 1.f, v + MM_V_RAD_CELL, 1.f, 1.f, 1.f, 1.f);
        } else if (m->tex.buttons) {
            v = live ? (here ? MM_V_RED_F : MM_V_RED) : MM_V_GREY;
            ui_image(bx, by, bw, bh, m->tex.buttons,
                     0.f, v, 1.f, v + MM_V_CELL, 1.f, 1.f, 1.f, 1.f);
        } else {
            ui_rect(bx, by, bw, bh, live ? 0.72f : 0.45f,
                    live ? 0.09f : 0.45f, live ? 0.11f : 0.47f, 1.f);
        }
        mm_label(m, f, bx, by, bw, bh,
                 (i == MM_QB_SUMMARY && m->qfrom == MM_PAGE_CHAMP)
                 ? STR_UI_CHAMPIONSHIP : MM_QB_NAME[i],
                 live ? 1.f : 0.82f, live ? 1.f : 0.82f, live ? 1.f : 0.84f);
    }
}

/* THE PAGE'S ENUMS: label, marker, back arrow, value, forward arrow. One loop
   for all three pages, because an enum is the same four numbers everywhere --
   see mm_enum. */
static void mm_draw_qenums(const mainmenu_t *m, const mmframe *f)
{
    char line[96];
    int i;

    for (i = 0; i < MM_Q_NENUM[m->page]; i++) {
        const mm_enum *e = &MM_Q_ENUM[m->page][i];
        const int lit = (m->qfocus == i);
        float ax, fx, ay, sz;
        mm_q_arrows(f, m->page, i, &ax, &fx, &ay, &sz);

        if (e->label[0]) {
            /* the small marker the game draws beside a LABELLED row, and on
               none of the rows that have no label */
            mm_arrow(m, px(f, e->x0 - MM_Q_BULLET_GAP),
                     py(f, e->y0 + e->sy * 0.5f) - MM_Q_BULLET * f->us * 0.5f,
                     MM_Q_BULLET * f->us, lit ? 1 : 0, 0);
            mm_q_text(m, f, 0, e->x0, e->y0, MM_TS_LABEL, 0, 1.f, e->label);
            mm_rule_at(f, px(f, e->x0), py(f, e->y0 + 24.f),
                       (e->sx * e->se - 30.f) * f->us);
        }
        mm_arrow(m, ax, ay, sz, lit ? 1 : 0, 0);
        mm_arrow(m, fx, ay, sz, lit ? 1 : 0, 1);

        /* the value, centred between the two arrows */
        mm_q_value(m, i, line, sizeof line);
        if (line[0]) {
            const sfont sf = sf_small(m->tex.font_small);
            const float sc = f->us * MM_TS_LABEL;
            const float w = sf.tex ? sf_w(&sf, sc, line) : ui_text_w(sc, line);
            const float cx = (ax + sz + fx) * 0.5f;
            const float ty = py(f, e->y0 + e->sy * 0.5f)
                             - (sf.tex ? sf_h(&sf, sc) : ui_text_h(sc)) * 0.5f;
            if (sf.tex)
                sf_text_shadowed(&sf, cx - w * 0.5f, ty, sc,
                                 1.f, 1.f, 1.f, 1.f, line);
            else
                ui_text(cx - w * 0.5f, ty, sc, 1.f, 1.f, 1.f, 1.f, line);
        }
    }
}

static void mm_draw_quick(const mainmenu_t *m, const mmframe *f)
{
    const int c = (m->car < 0 || m->car >= MM_N_CARS) ? 0 : m->car;
    const rb_car_data *cd = &RB_CARS[c];
    char line[96];

    /* --- shotTrack, between enumTrack's two arrows ----------------------- */
    mm_q_shot(m, f, 0, DLG_RACESUM_shotTrackX0, DLG_RACESUM_shotTrackY0,
              DLG_RACESUM_shotTrackSX, DLG_RACESUM_shotTrackSY);

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

    /* --- staticTrackName and staticTrackInfo -----------------------------
     *
     * THE DESCRIPTION IS THE GAME'S OWN, at last. These notes recorded it as
     * lost behind an unread string table and this page showed the track's AI
     * roster instead; str_data.h now carries all ten, three lines each, and the
     * roster line moved under them where it is an addition rather than a
     * substitute. */
    mm_q_text(m, f, 1, DLG_RACESUM_staticTrackNameX0,
              DLG_RACESUM_staticTrackNameY0, MM_TS_TRACK, 0, 1.f,
              STR_TRACK_NAME[m->track]);
    mm_rule_at(f, px(f, DLG_RACESUM_staticTrackNameX0),
               py(f, DLG_RACESUM_staticTrackNameY0 + 28.f),
               DLG_RACESUM_staticTrackNameSX * f->us);
    {
        const ai_race *r = &AI_RACES[m->track];
        mm_q_lines(m, f, 0, DLG_RACESUM_staticTrackInfoX0,
                   DLG_RACESUM_staticTrackInfoY0, MM_TS_INFO,
                   STR_TRACK_MENU[m->track]);
        snprintf(line, sizeof line, "%d opponents, rubber band %d%%",
                 mainmenu_field_size(m->track, m->skill),
                 (int)(r->coeff_common * 100.f + 0.5f));
        mm_q_text(m, f, 0, DLG_RACESUM_staticTrackInfoX0,
                  DLG_RACESUM_staticTrackInfoY0 + 3.f * MM_LINE_H,
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
        /* THE CAR'S OWN NAME AND ITS OWN SPEC BLOCK, both the game's --
           "Road Rage RR" and the three lines under it were the example this
           file used to give of what the unread string table was costing. The
           three-line form is the one that fits staticCarInfo's 110 px. */
        mm_q_text(m, f, 1, rx_, ty, MM_TS_WELCOME, 1, 1.f, STR_CAR_NAME[c]);
        ty += 26.f;
        {
            const char *info = STR_CAR_INFO_SHORT[c];
            while (*info) {
                const char *nl = strchr(info, '\n');
                size_t len = nl ? (size_t)(nl - info) : strlen(info);
                if (len >= sizeof line) len = sizeof line - 1;
                memcpy(line, info, len);
                line[len] = 0;
                mm_q_text(m, f, 0, rx_, ty, MM_TS_INFO, 1, 1.f, line);
                ty += MM_LINE_H;
                if (!nl) break;
                info = nl + 1;
            }
        }
    }

}


/* ------------------------------------------------------- dlgMAPINFO
 *
 * THE MAP IS NOT DRAWN, IT IS SHIPPED. `trackmap_<n>' is a 512x512 painting of
 * the track from above with the route, its direction arrows and its checkpoint
 * discs already on it -- the same texture race_ui.c puts the minimap marker over
 * during a race, and the reason the port has ten of them already. dlgMAPINFO
 * draws it AT ITS OWN PIXEL SIZE with its origin at (-44, -13): shotTrackView's
 * (56, 87) is a signed slider carrying a +100 bias (gen_dlg_data.py), and the
 * negative origin is what lets the painting hang off the top and left of the
 * frame the way it does on the game's own screen. Verified by matching the four
 * checkpoint discs painted into trackmap_1 against the same four on that
 * screenshot: scale 1.000, offset (-43.5, -14.5).
 *
 * The engine numbers its maps in championship.ini's order and this port does
 * not, which is what MAP_TRACKMAP is for -- main.c asks for `trackmap_<n>' by
 * the engine's number and hands the handle back in the port's own track order.
 */
/* THE NINE SLICES' OWN UVs, in one place, because there are two drawers over
 * this texture and they used to carry a copy each.
 *
 * `messagebox_empty` is 128x128 and is a LEFT-HAND C: a top rail at texel rows
 * 4..7, a left rail at columns 4..10 with both of its arcs, and a BOTTOM RAIL
 * AT ROWS 120..127. Read off the alpha, not assumed. So the right-hand side is
 * the left one with U mirrored -- and the bottom is NOT the top mirrored, it is
 * the art's own.
 *
 * THAT V FLIP WAS THE BUG. Both drawers sampled the bottom slice as v 1 -> 1-cu,
 * which turns a 32-texel band upside down and puts its 8 rows of ink at the TOP
 * of the destination -- 24 design pixels above the frame's own bottom edge. On
 * the Select player page's create dialog that put the rail under the Ok and
 * Cancel buttons, which drew over it, and the panel came up with no bottom at
 * all; on dlgMAPINFO's window it was a rim sitting 24 px inside its own frame,
 * which reads as a frame that is merely small. One rule, one copy, and both
 * call sites fixed by fixing it here -- traps.md, "grep for the rule".
 *
 * `part`: 0..3 the corners (bit 0 right, bit 1 bottom), 4 top rail, 5 bottom
 * rail, 6 left rail, 7 right rail.
 */
static void mm_frame9_uv(int part, float *u0, float *v0, float *u1, float *v1)
{
    const float cu = MM_MBE_CU, r0 = MM_MBE_RAIL, r1 = MM_MBE_RAIL + 0.05f;
    int right = 0, bottom = 0;

    if (part < 4) {
        right = part & 1;
        bottom = (part >> 1) & 1;
    } else if (part == 5) {
        bottom = 1;
    } else if (part == 7) {
        right = 1;
    }
    if (part >= 4 && part <= 5) {          /* the horizontal rails */
        *u0 = r0; *u1 = r1;
    } else {                               /* corners and vertical rails */
        *u0 = right ? cu : 0.f;
        *u1 = right ? 0.f : cu;
    }
    if (part >= 6) {                       /* the vertical rails */
        *v0 = r0; *v1 = r1;
    } else {
        *v0 = bottom ? 1.f - cu : 0.f;
        *v1 = bottom ? 1.f : cu;
    }
}

/* One nine-sliced frame: `x, y, w, h` in design pixels, corners at their own
   size. Nothing is drawn in the middle -- this is a rim. */
/* THE NINE-SLICE, IN SCREEN PIXELS -- and `x, y, w, h' are screen pixels, not
 * design ones, which is the whole of a bug that shipped.
 *
 * The first version took a DESIGN rectangle and put each of the eight slices
 * through `mm_box' on its own. mm_box maps a centre through px()/py() and then
 * sizes the box UNIFORMLY -- which is right for one photograph and wrong for a
 * row of boxes that have to touch, and mainmenu.h's own "a GROUP moves
 * together" note says so at length. On a 960x544 panel the pieces come apart:
 * for dlgMAPINFO's own 255 px window the top-left corner ends at x 112.5 and
 * the top rail starts at 145.9, a THIRTY-THREE PIXEL GAP, and the assembled
 * frame reads as two disconnected arcs with a rail floating between them.
 *
 * SO THE CALLER MAPS ONCE and this lays the slices out inside that: the corners
 * at their own uniform size (MM_MBE_CORNER * us, so a rounded corner stays
 * round) and only the rails stretched to fill between them. That is what
 * `mp_frame9' -- the roster page's twin of this -- had been doing correctly all
 * along, which is the second time these two drawers have disagreed about the
 * same nine slices: they already share `mm_frame9_uv' because the first
 * disagreement was over which way up the bottom rail goes (ui.md). There is one
 * of them now.
 */
static void mm_frame9(const mmframe *f, float x, float y, float w, float h,
                      unsigned int tex)
{
    const float c = MM_MBE_CORNER * f->us;
    int i;

    if (!tex || w < c * 2.f || h < c * 2.f)
        return;
    for (i = 0; i < 8; i++) {
        float u0, v0, u1, v1;
        const int right = (i < 4) ? (i & 1) : (i == 7);
        const int bottom = (i < 4) ? ((i >> 1) & 1) : (i == 5);
        const float sx = (i == 4 || i == 5) ? x + c : (right ? x + w - c : x);
        const float sy = (i >= 6) ? y + c : (bottom ? y + h - c : y);
        const float sw = (i == 4 || i == 5) ? w - c * 2.f : c;
        const float sh = (i >= 6) ? h - c * 2.f : c;
        mm_frame9_uv(i, &u0, &v0, &u1, &v1);
        ui_image(sx, sy, sw, sh, tex, u0, v0, u1, v1, 1.f, 1.f, 1.f, 1.f);
    }
}

static void mm_draw_mapinfo(const mainmenu_t *m, const mmframe *f)
{
    int i;

    /* the map first: everything else on this page sits over it */
    if (m->tex.trackmap[m->track]) {
        const unsigned int tm = m->tex.trackmap[m->track];
        const float mx = DLG_MAPINFO_shotTrackViewX0;
        const float my = DLG_MAPINFO_shotTrackViewY0;
        const float ms = DLG_MAPINFO_shotTrackViewSX;
        const float mt = DLG_MAPINFO_shotTrackViewSY;
        float x, y, w, h;

        /* the frame, then the slice of the painting that falls inside it. The
           slice is an axis-aligned sub-rect with its own UVs rather than a
           scissor, which ui.c has no notion of; the frame's rounded corners are
           drawn over it afterwards and cover the square ones. */
        /* ONE rectangle for the plate, the slice of the painting AND the
           frame -- see mm_frame9 on why the frame is handed screen pixels. */
        const float u0 = (MM_MAP_X - mx) / ms;
        const float v0 = (MM_MAP_Y - my) / mt;
        const float u1 = (MM_MAP_X + MM_MAP_W - mx) / ms;
        const float v1 = (MM_MAP_Y + MM_MAP_H - my) / mt;

        mm_box(f, MM_MAP_X, MM_MAP_Y, MM_MAP_W, MM_MAP_H, &x, &y, &w, &h);
        ui_rect(x, y, w, h, 0.f, 0.f, 0.f, MM_MAP_PLATE);
        ui_image(x, y, w, h, tm, u0, v0, u1, v1, 1.f, 1.f, 1.f, 1.f);
        mm_frame9(f, x, y, w, h, m->tex.panel);

        /* AND THE ROUTE, over the whole page -- ON THE PANEL'S OWN MAPPING and
           not on a second mm_box.
         *
           THE TWO PASSES DRAW THE SAME PIXELS INSIDE THE PANEL, so any
           disagreement between them shows as the route drawn TWICE, a few
           pixels apart, all the way round the window. mm_box maps a box's
           CENTRE through px() -- the stretch -- and then sizes it at us: two
           boxes with different centres therefore land at different scales
           relative to each other, and the painting's centre (212, 243) is not
           the panel's (197.5, 226). On a 960x544 panel that is 4 px, which is
           exactly the ghost.
         *
           So the whole painting's rectangle is DERIVED from the panel's: the
           slice u0..u1 must cover x..x+w by construction, and there is one
           mapping on the page instead of two that agree at 800x600 and nowhere
           else. */
        w /= (u1 - u0);
        h /= (v1 - v0);
        x -= u0 * w;
        y -= v0 * h;
        ui_alpha_test(MM_MAP_KEY);
        ui_image(x, y, w, h, tm, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f);
        ui_alpha_test(0.f);
    }

    /* staticTrackInfo -- the five-line description, 40030 + t */
    mm_q_lines(m, f, 0, DLG_MAPINFO_staticTrackInfoX0,
               DLG_MAPINFO_staticTrackInfoY0, MM_TS_INFO,
               STR_TRACK_LONG[m->track]);

    /* shotList: the selected screenshot big, the rest wrapping outward from it
       and dropped where they fall outside the list's own bounds. */
    for (i = -(MM_N_SHOTS - 1); i < MM_N_SHOTS; i++) {
        float dx, dy, dw, dh;
        int sh;
        if (mm_shot_slot(m, i, &sh, &dx, &dy, &dw, &dh))
            mm_q_shot(m, f, sh, dx, dy, dw, dh);
    }

    /* staticShortListText -- the word, and the rule the screen puts under it */
    mm_q_text(m, f, 0, DLG_MAPINFO_staticShortListTextX0,
              DLG_MAPINFO_staticShortListTextY0, MM_TS_LABEL, 0, 1.f,
              STR_UI_SCREENSHOTS);
    mm_rule_at(f, px(f, DLG_MAPINFO_staticShortListTextX0),
               py(f, DLG_MAPINFO_staticShortListTextY0 + 24.f),
               DLG_MAPINFO_staticShortListTextSX * f->us);
}

/* ---------------------------------------------------------- dlgSTAT
 *
 * The table's rows are the record book's -- see records.h for whose they are and
 * why they are not the original's player profiles. Everything else on the page
 * is the game's: the name and the three-line blurb out of the string table, the
 * screenshot the sibling page is standing on, and the four words of
 * `Sort results by'.
 */
static void mm_draw_scrollbar(const mainmenu_t *m, const mmframe *f,
                              float x0, float y0, float y1,
                              int first, int rows, int shown)
{
    const float u = 1.f / 8.f;      /* eight 32-wide cells over 256 */
    float bx, by, bw, bh, cap;
    float tx, ty, tw, th, frac, top;

    if (!m->tex.scrollbar)
        return;
    /* the two caps at the art's own size, and the trough between them taken
       from the top cell's own straight shaft so nothing is invented */
    cap = MM_SB_CAP_H;
    if (cap * 2.f > y1 - y0)
        cap = (y1 - y0) * 0.5f;
    mm_box(f, x0, y0, MM_SB_INK_W, cap, &bx, &by, &bw, &bh);
    ui_image(bx, by, bw, bh, m->tex.scrollbar,
             MM_SB_CELL_W * 0.f + 3.f / 256.f, 0.f,
             MM_SB_CELL_W * 0.f + 29.f / 256.f, cap / MM_SB_CAP_H,
             1.f, 1.f, 1.f, 1.f);
    mm_box(f, x0, y1 - cap, MM_SB_INK_W, cap, &bx, &by, &bw, &bh);
    ui_image(bx, by, bw, bh, m->tex.scrollbar,
             u * 3.f + 3.f / 256.f, 1.f - cap / MM_SB_CAP_H,
             u * 3.f + 29.f / 256.f, 1.f, 1.f, 1.f, 1.f, 1.f);
    if (y1 - y0 > cap * 2.f) {
        mm_box(f, x0, y0 + cap, MM_SB_INK_W, y1 - y0 - cap * 2.f,
               &bx, &by, &bw, &bh);
        /* two rows of the top cell's straight shaft, stretched */
        ui_image(bx, by, bw, bh, m->tex.scrollbar,
                 3.f / 256.f, 60.f / 64.f, 29.f / 256.f, 62.f / 64.f,
                 1.f, 1.f, 1.f, 1.f);
    }
    /* THE THUMB AT THE ART'S OWN SIZE, which is what the original does -- its
       screenshot of this page has four rows in a table that holds nine and a
       50 px pill sitting at the top of the trough, not a pill stretched to fill
       it. `frac' is how far down the list the view is: 0 on a table that fits,
       which is every table dlgSTAT can build (REC_MAX_ROWS is under the rows it
       shows), and a real fraction on the player list, which holds twenty. */
    frac = rows > shown ? (float)first / (float)(rows - shown) : 0.f;
    if (frac < 0.f) frac = 0.f;
    if (frac > 1.f) frac = 1.f;
    th = MM_SB_THUMB_H;
    top = y0 + cap + (y1 - y0 - cap * 2.f - th) * frac;
    mm_box(f, x0, top, MM_SB_INK_W, th, &tx, &ty, &tw, &th);
    ui_image(tx, ty, tw, th, m->tex.scrollbar,
             u * 6.f + 3.f / 256.f, 0.f, u * 6.f + 29.f / 256.f, 1.f,
             1.f, 1.f, 1.f, 1.f);
}

/* ---------------------------------------------- and the bar as a CONTROL
 *
 * It was drawn and nothing read it: three pages put a scroller beside a list
 * that really does scroll -- the ladder (ten rungs, six shown), the award book
 * (twenty-five, eight) and the roster (twenty, seven) -- and a thumb that walks
 * when the cursor walks but cannot be pushed is a control that does not work.
 *
 * WHAT A PRESS MEANS is the caps' own arrows: the top cap is one row back, the
 * bottom cap one row on, and anywhere between the two is the THUMB JUMPING TO
 * THE FINGER and then following it until the finger comes up. Nothing about the
 * behaviour is recovered -- the engine's own rate lives in FUN_004bc180 on
 * control 0x776 and this port has not read it -- so it is the ordinary one, and
 * it is named here rather than left implicit.
 *
 * THE HIT BOX IS WIDER THAN THE INK. The art is 26 design px across, which is
 * 24 screen px on a 960x544 panel and thinner than a thumb; MM_SB_GRAB is the
 * slack either side. It is the port's own and it is the same argument the row
 * hit boxes make -- a control you cannot land on is a control that does not
 * work either.
 */
#define MM_SB_GRAB 10.f

static void mm_sb_box(const mmframe *f, float x0, float y0, float y1,
                      float *x, float *y, float *w, float *h)
{
    mm_box(f, x0 - MM_SB_GRAB, y0, MM_SB_INK_W + MM_SB_GRAB * 2.f, y1 - y0,
           x, y, w, h);
}

/* mm_draw_scrollbar's own cap height for this bar -- one copy, because a hit
   box that disagrees with the picture is worse than no hit box. */
static float mm_sb_cap(float y0, float y1)
{
    float cap = MM_SB_CAP_H;
    if (cap * 2.f > y1 - y0)
        cap = (y1 - y0) * 0.5f;
    return cap;
}

/* 0 the up cap, 1 the down cap, 2 the trough, -1 not on the bar. */
static int mm_sb_at(const mmframe *f, float x0, float y0, float y1,
                    float tx, float ty)
{
    const float cap = mm_sb_cap(y0, y1) * f->us;
    float x, y, w, h;
    mm_sb_box(f, x0, y0, y1, &x, &y, &w, &h);
    if (!touch_in(tx, ty, x, y, w, h))
        return -1;
    if (ty < y + cap)
        return 0;
    if (ty > y + h - cap)
        return 1;
    return 2;
}

/* Where the finger puts the THUMB'S CENTRE, as a first row. */
static int mm_sb_first_at(const mmframe *f, float x0, float y0, float y1,
                          int rows, int shown, float ty)
{
    const float cap = mm_sb_cap(y0, y1);
    const float run = (y1 - y0 - cap * 2.f - MM_SB_THUMB_H) * f->us;
    const float top = py(f, y0 + cap) + MM_SB_THUMB_H * f->us * 0.5f;
    float t;
    int n;
    (void)x0;
    if (rows <= shown || run <= 0.f)
        return 0;
    t = (ty - top) / run;
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;
    n = (int)(t * (float)(rows - shown) + 0.5f);
    if (n < 0) n = 0;
    if (n > rows - shown) n = rows - shown;
    return n;
}

/* ONE FRAME OF THE BAR. `first' is the list's own top row and is written in
   place; `drag' is the caller's one-bit memory of "this touch belongs to the
   trough", which is what makes a drag keep working once the finger has left the
   bar sideways. Answers 1 when the bar took the touch, so the page can stop
   looking for a row under the same finger. */
static int mm_sb_drive(const mmframe *f, const touch_state *tp,
                       float x0, float y0, float y1,
                       int *first, int rows, int shown, int *drag)
{
    const int last = rows > shown ? rows - shown : 0;
    int at;

    if (!tp) {
        *drag = 0;
        return 0;
    }
    if (!tp->down) {
        const int had = *drag;
        *drag = 0;
        return had;
    }
    if (*drag) {
        *first = mm_sb_first_at(f, x0, y0, y1, rows, shown, tp->y);
        return 1;
    }
    if (!tp->pressed)
        return 0;
    at = mm_sb_at(f, x0, y0, y1, tp->x, tp->y);
    if (at < 0)
        return 0;
    if (at == 0)
        *first -= 1;
    else if (at == 1)
        *first += 1;
    else {
        *drag = 1;
        *first = mm_sb_first_at(f, x0, y0, y1, rows, shown, tp->y);
    }
    if (*first > last) *first = last;
    if (*first < 0)    *first = 0;
    return 1;
}

static void mm_draw_stats(const mainmenu_t *m, const mmframe *f)
{
    const float tx0 = DLG_STAT_tableStatX0, ty0 = DLG_STAT_tableStatY0;
    const float tsx = DLG_STAT_tableStatSX, tsy = DLG_STAT_tableStatSY;
    const float head = tsy * MM_ST_HEAD_H, item = tsy * MM_ST_ITEM_H;
    const float col0 = tx0 + tsx * MM_ST_COL0;
    const float col1 = tx0 + tsx * MM_ST_COL1;
    const float col2 = tx0 + tsx * MM_ST_COL2;
    const int shown = (int)((tsy - head) / item);
    const int stat = (m->stat < 0 || m->stat >= REC_N_STAT) ? 0 : m->stat;
    const rec_row *rows[REC_MAX_ROWS];
    char line[96];
    int n, i;

    /* staticTrackName, its rule, and the three-line blurb under it */
    mm_q_text(m, f, 1, DLG_STAT_staticTrackNameX0, DLG_STAT_staticTrackNameY0,
              MM_TS_TRACK, 0, 1.f, STR_TRACK_NAME[m->track]);
    mm_rule_at(f, px(f, DLG_STAT_staticTrackNameX0),
               py(f, DLG_STAT_staticTrackNameY0 + 24.f),
               DLG_STAT_staticTrackNameSX * f->us);
    mm_q_lines(m, f, 0, DLG_STAT_staticTrackInfoX0, DLG_STAT_staticTrackInfoY0,
               MM_TS_INFO, STR_TRACK_SHORT[m->track]);

    /* shotTrack -- the SAME screenshot the shot list is standing on */
    mm_q_shot(m, f, m->shot, DLG_STAT_shotTrackX0, DLG_STAT_shotTrackY0,
              DLG_STAT_shotTrackSX, DLG_STAT_shotTrackSY);

    /* the header: Player / <the chosen stat> / Car, and the rule under it */
    mm_q_text(m, f, 0, col0, ty0 + (head - MM_LINE_H) * 0.5f, MM_TS_LABEL,
              0, 1.f, STR_UI_COL_PLAYER);
    {
        const sfont sf = sf_small(m->tex.font_small);
        const float sc = f->us * MM_TS_LABEL;
        const float hy = py(f, ty0 + (head - MM_LINE_H) * 0.5f);
        const char *h1 = MM_ST_NAME[stat], *h2 = STR_UI_COL_CAR;
        const float w1 = sf.tex ? sf_w(&sf, sc, h1) : ui_text_w(sc, h1);
        const float w2 = sf.tex ? sf_w(&sf, sc, h2) : ui_text_w(sc, h2);
        if (sf.tex) {
            sf_text_shadowed(&sf, px(f, col1) - w1 * 0.5f, hy, sc,
                             1.f, 1.f, 1.f, 1.f, h1);
            sf_text_shadowed(&sf, px(f, col2) - w2 * 0.5f, hy, sc,
                             1.f, 1.f, 1.f, 1.f, h2);
        } else {
            ui_text(px(f, col1) - w1 * 0.5f, hy, sc, 1.f, 1.f, 1.f, 1.f, h1);
            ui_text(px(f, col2) - w2 * 0.5f, hy, sc, 1.f, 1.f, 1.f, 1.f, h2);
        }
    }
    mm_rule_at(f, px(f, col0), py(f, ty0 + head),
               (tx0 + tsx - col0) * f->us);

    /* the rows */
    n = records_sorted(m->track, stat, rows, REC_MAX_ROWS);
    if (n > shown)
        n = shown;
    for (i = 0; i < n; i++) {
        const rec_row *r = rows[i];
        const float ry = ty0 + head + item * (float)i;
        const float txt = ry + (item - MM_LINE_H) * 0.5f;
        const sfont sf = sf_small(m->tex.font_small);
        const float sc = f->us * MM_TS_INFO;
        const int mine = strcmp(r->name, "Player") == 0;

        /* the marker beside each row: the game's own enumarrows cell, RED on
           the row that is the player's and grey on the rest -- which is what
           its shot of this page has beside the profile that is signed in.
           State 0 is the red cell and state 1 the silver one; see mm_arrow. */
        mm_arrow(m, px(f, tx0 + MM_ST_MARK),
                 py(f, ry + item * 0.5f) - MM_Q_BULLET * f->us * 0.5f,
                 MM_Q_BULLET * f->us, mine ? 0 : 2, 0);

        mm_q_text(m, f, 0, col0, txt, MM_TS_INFO, 0, 1.f, r->name);
        mm_rule_at(f, px(f, col0), py(f, ry + item - 3.f),
                   (tx0 + tsx * MM_ST_RULE0 - col0) * f->us);

        mm_time(line, sizeof line, records_value(r, stat));
        {
            const float w = sf.tex ? sf_w(&sf, sc, line) : ui_text_w(sc, line);
            if (sf.tex)
                sf_text_shadowed(&sf, px(f, col1) - w * 0.5f, py(f, txt), sc,
                                 1.f, 1.f, 1.f, 1.f, line);
            else
                ui_text(px(f, col1) - w * 0.5f, py(f, txt), sc,
                        1.f, 1.f, 1.f, 1.f, line);
        }
        snprintf(line, sizeof line, "%s",
                 (r->car >= 0 && r->car < MM_N_CARS) ? STR_CAR_NAME[r->car]
                                                     : STR_UI_NA);
        {
            const float w = sf.tex ? sf_w(&sf, sc, line) : ui_text_w(sc, line);
            if (sf.tex)
                sf_text_shadowed(&sf, px(f, col2) - w * 0.5f, py(f, txt), sc,
                                 1.f, 1.f, 1.f, 1.f, line);
            else
                ui_text(px(f, col2) - w * 0.5f, py(f, txt), sc,
                        1.f, 1.f, 1.f, 1.f, line);
        }
    }
    /* the table's closing rule */
    mm_rule_at(f, px(f, col0), py(f, ty0 + tsy), (tx0 + tsx - col0) * f->us);

    mm_draw_scrollbar(m, f, tx0 + tsx + MM_SB_GAP, ty0 + head, ty0 + tsy,
                      0, records_track(m->track)->n, shown);

    /* staticShowResult -- the label enumStatType does not carry itself */
    mm_q_text(m, f, 0, DLG_STAT_staticShowResultX0,
              DLG_STAT_staticShowResultY0, MM_TS_LABEL, 0, 1.f, STR_UI_SORT_BY);
    mm_rule_at(f, px(f, DLG_STAT_staticShowResultX0),
               py(f, DLG_STAT_staticShowResultY0 + 24.f),
               DLG_STAT_staticShowResultSX * f->us);
}

/* ============================================== THE AWARD PAGE -- no dialog
 *
 * The port's own, on the port's own list (awards.h). Eight rows of the
 * twenty-five, each one a name over the line that says how it is earned, with
 * the marker beside it RED when the award is held and silver when it is not --
 * which is the same cell of the same `enumarrows' atlas dlgSTAT puts beside the
 * signed-in player, used for the same reason: it is the game's own way of
 * saying "this row is yours".
 *
 * A row that is not held shows its TALLY where a held one shows nothing: the
 * numbers are the award's own progress and goal, and for the by-track and
 * by-car awards they are how many of the ten (or three) have been done, which
 * is what a bitmask means. An award with a goal of one has no tally to show --
 * "0/1" tells nobody anything they cannot see from the marker.
 *
 * BOTH STRINGS ARE SHRUNK TO FIT and neither is clipped. The names and the
 * lines are written in awards.c and the column is 434 design px wide, so a
 * table that clipped would silently reward whoever wrote the shortest line --
 * see the toast in awards.c, which does the same for the same reason.
 */
static void mm_draw_awards(const mainmenu_t *m, const mmframe *f)
{
    const float tx0 = DLG_STAT_tableStatX0, ty0 = MM_AW_Y0;
    const float tsx = DLG_STAT_tableStatSX, tsy = MM_AW_SY;
    const float item = tsy / (float)MM_AW_ROWS;
    const float col0 = tx0 + tsx * MM_ST_COL0;
    const int top = mm_aw_top(m);
    const sfont big = sf_big(m->tex.font_big);
    const sfont small_ = sf_small(m->tex.font_small);
    char line[96];
    int i;

    /* WHOSE BOOK THIS IS, where dlgSTAT puts the track's name -- and with its
       rule, which is that control's own bottom edge run its full width. With no
       profile selected there is nobody to have earned anything, and the page
       says so rather than showing an empty book as if it were somebody's. */
    if (*award_player())
        snprintf(line, sizeof line, "%s", award_player());
    else
        snprintf(line, sizeof line, "%s", STR_UI_SELECT_PLAYER);
    mm_q_text(m, f, 1, DLG_STAT_staticTrackNameX0, DLG_STAT_staticTrackNameY0,
              MM_TS_TRACK, 0, 1.f, line);
    mm_rule_at(f, px(f, DLG_STAT_staticTrackNameX0),
               py(f, DLG_STAT_staticTrackNameY0 + 24.f),
               DLG_STAT_staticTrackNameSX * f->us);

    for (i = 0; i < MM_AW_ROWS && top + i < AW_N; i++) {
        const int id = top + i;
        const aw_def *d = award_def(id);
        const int got = award_have(id);
        const float ry = ty0 + item * (float)i;
        const float sc_n = f->us * MM_AW_NAME_TS;
        const float sc_w = f->us * MM_AW_WHAT_TS;
        float avail, wid;

        if (!d)
            continue;

        /* the marker: dlgSTAT's own two cells, red for held and silver for not */
        mm_arrow(m, px(f, tx0 + MM_ST_MARK),
                 py(f, ry + item * 0.5f) - MM_Q_BULLET * f->us * 0.5f,
                 MM_Q_BULLET * f->us, got ? 0 : 2, 0);

        /* THE NAME, and a held award is white while one still to come is the
           0.82 grey the front end's own dead rows use -- so the book reads at a
           glance, which is the whole point of a page like this. */
        avail = (tsx - tsx * MM_ST_COL0 - MM_AW_STATE_W) * f->us;
        {
            float r = got ? 1.f : 0.82f, g = got ? 1.f : 0.82f;
            float b = got ? 1.f : 0.84f;
            const float x = px(f, col0);
            float ns = sc_n;
            wid = big.tex ? sf_w(&big, ns, d->name) : ui_text_w(ns, d->name);
            if (wid > avail && wid > 0.f)
                ns *= avail / wid;
            if (big.tex)
                sf_text_shadowed(&big, x, py(f, ry + 3.f), ns, r, g, b, 1.f,
                                 d->name);
            else
                ui_text(x, py(f, ry + 3.f), ns, r, g, b, 1.f, d->name);

            ns = sc_w;
            wid = small_.tex ? sf_w(&small_, ns, d->what)
                             : ui_text_w(ns, d->what);
            if (wid > avail && wid > 0.f)
                ns *= avail / wid;
            if (small_.tex)
                sf_text_shadowed(&small_, x, py(f, ry + 24.f), ns,
                                 0.72f, 0.74f, 0.76f, 1.f, d->what);
            else
                ui_text(x, py(f, ry + 24.f), ns, 0.72f, 0.74f, 0.76f, 1.f,
                        d->what);
        }

        /* THE TALLY, right-aligned against the table's own right edge. Only on
           an award that is neither held nor a one-shot. */
        if (!got && d->goal > 1) {
            int have = award_progress(id);
            if (d->kind == AW_K_BITS) {
                unsigned int v = (unsigned int)have;
                int c = 0;
                while (v) { c += (int)(v & 1u); v >>= 1; }
                have = c;
            }
            snprintf(line, sizeof line, "%d/%d", have, d->goal);
            mm_q_text(m, f, 0, tx0 + tsx, ry + 8.f, MM_AW_STATE_TS, 1, 0.9f,
                      line);
        }
        /* the per-row rule, the stats table's own, run the table's full width */
        if (i + 1 < MM_AW_ROWS)
            mm_rule_at(f, px(f, col0), py(f, ry + item - 3.f),
                       (tx0 + tsx - col0) * f->us);
    }
    mm_rule_at(f, px(f, col0), py(f, ty0 + tsy), (tx0 + tsx - col0) * f->us);

    mm_draw_scrollbar(m, f, tx0 + tsx + MM_SB_GAP, ty0, ty0 + tsy,
                      top, AW_N, MM_AW_ROWS);

    /* AND THE COUNT, where dlgSTAT puts `Sort results by' -- the label of the
       picker below, in the same place, with the same rule under it. It says how
       many are held; the picker's own value says which rows are on screen. */
    snprintf(line, sizeof line, "%s: %d of %d", MM_UI_AWARDS,
             award_n_have(), AW_N);
    mm_q_text(m, f, 0, DLG_STAT_staticShowResultX0,
              DLG_STAT_staticShowResultY0, MM_TS_LABEL, 0, 1.f, line);
    mm_rule_at(f, px(f, DLG_STAT_staticShowResultX0),
               py(f, DLG_STAT_staticShowResultY0 + 24.f),
               DLG_STAT_staticShowResultSX * f->us);
}

/* ================================ dlgSETCAR / dlgSETDETAIL -- THE GARAGE
 *
 * TWO PAGES, and between them SEVENTEEN CONTROLS, every one of them out of the
 * exe's own dialog tables with its rectangle out of the shipped `.ini' beside
 * it. The tables are at 0x56f890 and 0x56f780 -- `{ u32 id; u32 type;
 * char *name; u32 0 }' records terminated by a zero one, the same shape
 * dlgRACESUM's is:
 *
 *   dlgSETCAR, ids 0x898..0x89d
 *     0x898 &ENM  enumCar             the car, between two arrows
 *     0x899 &STT  staticCarInfoLeft   "Booster: / Engine: / Tires:"
 *     0x89a &STT  staticCarInfoRight  what each one is fitted with
 *     0x89b &STT  staticUpgrades      the "Current upgrades" heading
 *     0x89c &STT  staticStatus        "You can buy this car" / "Not available"
 *     0x89d &STT  staticPrice         "Buy price: $n" / "Sell price: $n"
 *
 *   dlgSETDETAIL, ids 0x9c6..0x9cc
 *     0x9c6 &SHS  shotlistUpgrade     the part's own photograph
 *     0x9c7 &ENM  enumUpgrades        the three levels, the picture between its
 *                                     arrows -- SE 0, like enumTrack
 *     0x9c8 &STT  staticUpgradeName   the heading: Booster / Engine / Tires
 *     0x9c9 &STT  staticUpgradeInfo   the part, its effects and its price
 *     0x9ca &ENM  staticCarEnum       the car again, READ-ONLY -- an &ENM whose
 *                                     name begins `static', and whose arrows are
 *                                     grey on all three of the game's own shots
 *     0x9cb &STT  staticUpgradeStatus INSTALLED / Not available / sell-first
 *     0x9cc &STT  staticUpgradeStatus  (declared twice, one name)
 *
 * AND THE FURNITURE UNDER BOTH IS dlgCARSCOMM's, which is why neither dialog's
 * own file carries a rectangle for any of it: animCarPreview (the car in 3D),
 * staticCashText and staticPlayerCash (the "Your cash / $100" line),
 * staticCarName (the "Car" heading) and staticCarExplain (the seven-line spec
 * block). CARSCOMM is cars-common; both car screens stand on it, which is the
 * reading under which every one of these five lands where the picture has it.
 *
 * WHAT WAS MEASURED HERE, and only these, off the game's own 800x600 shots of
 * the four states:
 *
 *  - A HEADING'S RULE IS ITS BOX'S OWN BOTTOM EDGE, run its full width. The
 *    "Your cash" rule lands at y 100 spanning x 116..302 and staticCashText is
 *    (115, 75) 185 x 25 -- 75 + 25 = 100 and 115 + 185 = 300, to the pixel. The
 *    same holds for "Current upgrades": its rule is at 435 across 114..539
 *    against staticUpgrades' (114, 410) 426 x 25. So mg_rule is not a guess.
 *  - THE BLOCKS ARE SET AT THE SMALL FONT'S OWN LINE PITCH, not at the 25 the
 *    quick-race pages use. The spec block's eight lines land at y 204, 225, 249,
 *    274, 297, 319, 342, 363 -- a flat 22.7 -- and smash20.ini's own letSizeY is
 *    28 with lineSpace -5, i.e. 23. That is the number, and it comes out of
 *    hud_data.h rather than off the picture.
 *  - THE SPEC BLOCK AND THE PART BLOCK ARE RIGHT-ALIGNED, on their own boxes'
 *    right edges: every one of the spec block's eight lines ends at x 542
 *    against staticCarExplain's 339 + 205 = 544, and every one of the part
 *    block's four ends at 303 against staticUpgradeInfo's 105 + 200 = 305.
 *  - THE PRICE IS THE SPEC BLOCK'S EIGHTH LINE. staticPrice's own rectangle is
 *    (342, 336) 200 x 50 -- its right edge, 542, is the spec block's, but its Y
 *    is nowhere near where the line actually lands. On the picture that line
 *    continues the block at its own pitch, so that is where it is drawn and
 *    staticPrice supplies only the right edge it shares. Said out loud because
 *    it is the one place these two pages depart from a shipped rectangle.
 *  - THE FOUR BARS ARE ROWS 0..3 of the main menu's own eight, Next skin is row
 *    4, row 5 is EMPTY on both pages, and rows 6 and 7 carry the money buttons.
 *    Measured off the trays: their tops land at y 131, 176, 221, 266, 326, 421
 *    and 481 -- a flat 45 with the same one gap the main menu has, and every one
 *    4.5 px below where MM_CY puts the main menu's. The bars use MM_CY anyway:
 *    one row table for five pages, and a 4.5 px absolute correction would have
 *    to be re-checked against every one of them. known-issues.md.
 *  - AND THE BAR YOU ARE ON STANDS OUT 26 PX, which is MM_SLIDE at the focus
 *    curve's own settled MM_SETTLED (23) to within the read: Select car's cap is
 *    at 557 against its siblings' 583 on the Garage shot and Upgrade booster's
 *    at 579 against 605 on the booster shot.
 *
 * THE ENUM ARROWS ARE DRAWN AT MM_Q_ARROW, like every other enum in the front
 * end, and that is a deliberate rounding: on these two shots the cell measures
 * 32 design px at the art's own size (the red cell's ink is 25 x 26 of 32 x 32
 * and lands 23 x 25 on screen) where MM_Q_ARROW is 34. One arrow size across
 * five pages is worth more than two pixels. Their PLACEMENT is dlg_data.h's
 * through the same SE rule -- see MM_Q_ENUM -- and dlgSETDETAIL's enumUpgrades
 * confirms it on the picture: its forward arrow's cell right edge measures 581
 * against enumUpgrades' own 310 + 277 = 587. dlgSETCAR's enumCar does NOT: its
 * pair measures 35 px inside its own rectangle at BOTH ends, on the same shot,
 * and no reading of SE produces that. known-issues.md carries it.
 */

/* ButtonsTextures' sixth cell: GREY WITH A DOT, v 224. Its fifth (v 192) is
   grey with a triangle, and the difference matters here -- every bar on these
   two pages carries a dot, so a denying one has to come out of the dotted grey
   or its cap changes shape when it goes dead. */
#define MM_V_GREY_DOT (224.f / 256.f)

/* THE CAR PAGES' LINE PITCH, which is the small font's own and not the 25
   MM_LINE_H carries: SF_SMALL_SIZE_Y + SF_SMALL_LINE is 28 - 5 = 23, and the
   spec block measures 22.7 on the game's own screenshot. */
#define MM_G_LINE_H (SF_SMALL_SIZE_Y + SF_SMALL_LINE)

/* One bar per part kind plus the Garage itself; the compiler holds the two
   counts together rather than a comment doing it. */
typedef char mm_gb_agree[(MM_GB_N == GAR_N_KINDS + 1) ? 1 : -1];
typedef char mm_gcar_agree[(MM_N_CARS == PL_N_CARS) ? 1 : -1];

/* dlgPLRSCOMM's player card, which the multiplayer front page draws as well --
   defined with that page below, because it is written against its layout.
   `scores_only' is dlgMULTIPLAYER's own difference; see that page. */
static void mp_draw_card_at(const mainmenu_t *m, const mmframe *f,
                            int scores_only);

/* A heading's rule: the bottom edge of its own box, its full width. */
static void mg_rule(const mmframe *f, float x0, float y0, float sx, float sy)
{
    mm_rule_at(f, px(f, x0), py(f, y0 + sy), sx * f->us);
}

/* A blurb, one line per '\n', left- or right-aligned, at that pitch. Returns
   the y the line AFTER the last one would be at, so a caller can go on adding
   to the block -- which is what the price line is. */
static float mg_lines(const mainmenu_t *m, const mmframe *f, float x, float y,
                      int right, float alpha, const char *s)
{
    char line[128];
    while (s && *s) {
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s) : strlen(s);
        if (len >= sizeof line)
            len = sizeof line - 1;
        memcpy(line, s, len);
        line[len] = 0;
        mm_q_text(m, f, 0, x, y, MM_TS_INFO, right, alpha, line);
        y += MM_G_LINE_H;
        if (!nl)
            break;
        s = nl + 1;
    }
    return y;
}

/* One texture in a design-space box, sampling the framed ink the track shots
   and the upgrade art BOTH carry -- they have identical bounds, x 28..227 and
   y 53..202 of 256 x 256, so MM_SHOT_U0.. serves both and nothing here draws a
   border of its own. */
static void mg_shot(const mmframe *f, unsigned int tex,
                    float dx, float dy, float dw, float dh)
{
    float x, y, w, h;
    mm_box(f, dx, dy, dw, dh, &x, &y, &w, &h);
    if (tex)
        ui_image(x, y, w, h, tex, MM_SHOT_U0, MM_SHOT_V0, MM_SHOT_U1,
                 MM_SHOT_V1, 1.f, 1.f, 1.f, 1.f);
    else
        ui_rect(x, y, w, h, 0.25f, 0.28f, 0.34f, 1.f);
}

/* The four bars' own words -- 10059 and 10057/10058/10056, the string table's. */
static const char *const MM_GB_NAME[MM_GB_N] = {
    STR_UI_SELECT_CAR, STR_UI_UP_BOOSTER, STR_UI_UP_ENGINE, STR_UI_UP_TIRES
};

/* Which part kind a bar opens its page on; -1 for the Garage, which has none. */
static int mg_tab_kind(int tab)
{
    return (tab > MM_GB_SELECT && tab < MM_GB_N) ? tab - MM_GB_PART : -1;
}

/* Whether the page you are on is this bar's -- which is what puts the WHITE dot
   on it and slides it out. */
static int mg_tab_here(const mainmenu_t *m, int tab)
{
    if (tab == MM_GB_SELECT)
        return m->page == MM_PAGE_GARAGE;
    return m->page == MM_PAGE_DETAIL && m->gkind == mg_tab_kind(tab);
}

/* Which of the eight measured rows a stop's bar is, and whether it is drawn in
   the ORANGE cell rather than the red -- Next skin, Sell car and Upgrade are
   orange on the game's own shots and the four tabs are red. */
static int mg_bar_row(int stop, int *orange)
{
    if (orange) *orange = 0;
    if (stop >= MM_G_TAB && stop < MM_G_TAB + MM_GB_N)
        return stop - MM_G_TAB;             /* rows 0..3 */
    switch (stop) {
    case MM_G_SKIN: if (orange) *orange = 1; return 4;
    /* ROW 5 IS EMPTY on both pages, and that is the game's own gap: the trays on
       its screenshots run 131, 176, 221, 266, 326 and then nothing until 421. */
    case MM_G_BUY:  if (orange) *orange = 1; return 6;
    case MM_G_SELL: if (orange) *orange = 1; return 7;
    default: return -1;
    }
}

/* WHAT THE TWO MONEY BARS SAY, which is the whole difference between the two
   pages' columns: 10013/10060 on the Garage, 10010/10061 on the part page. */
static const char *mg_bar_name(const mainmenu_t *m, int stop)
{
    const int garage = m->page == MM_PAGE_GARAGE;
    switch (stop) {
    case MM_G_SKIN: return STR_UI_NEXT_SKIN;
    case MM_G_BUY:  return garage ? STR_UI_BUY_CAR : STR_UI_UPGRADE;
    case MM_G_SELL: return garage ? STR_UI_SELL_CAR : STR_UI_DOWNGRADE;
    default:
        if (stop >= MM_G_TAB && stop < MM_G_TAB + MM_GB_N)
            return MM_GB_NAME[stop - MM_G_TAB];
        return "";
    }
}

/* ------------------------------------------------------------- the ring */

/* The page's own stops in the order the pad walks them. MM_G_SKIN is on the
   Garage and not on the part page, which is the one difference. */
static int mg_ring(int page, int *out)
{
    int n = 0, i;
    if (!MM_PAGE_IS_CAR(page))
        return 0;
    out[n++] = MM_G_ENUM;
    for (i = 0; i < MM_GB_N; i++)
        out[n++] = MM_G_TAB + i;
    if (page == MM_PAGE_GARAGE)
        out[n++] = MM_G_SKIN;
    out[n++] = MM_G_BUY;
    out[n++] = MM_G_SELL;
    out[n++] = MM_G_RACE;
    out[n++] = MM_G_BACK;
    return n;
}

int mainmenu_g_nfocus(int page)
{
    int ring[MM_G_N_FOCUS];
    return mg_ring(page, ring);
}

int mainmenu_g_stop(int page, int i)
{
    int ring[MM_G_N_FOCUS];
    const int n = mg_ring(page, ring);
    if (n <= 0)
        return -1;
    while (i < 0) i += n;
    return ring[i % n];
}

static int mg_index(int page, int stop)
{
    int ring[MM_G_N_FOCUS];
    const int n = mg_ring(page, ring);
    int i;
    for (i = 0; i < n; i++)
        if (ring[i] == stop)
            return i;
    return 0;
}

/* WHETHER A STOP CAN ACT RIGHT NOW, which on these two pages is a question
   about the PROFILE and not only about the build -- the two money bars answer
   for the car or the part they are pointed at, and the game's own screenshots
   show both of them denying: Buy car grey beside a car already in the garage,
   Downgrade grey beside a part at Default. The focus ring still visits them, so
   a press can say WHY; the main menu's dead rows are skipped instead, because
   there is nothing there to say. */
int mainmenu_g_live(const mainmenu_t *m, int stop)
{
    const player_t *p;
    if (!m || !MM_PAGE_IS_CAR(m->page))
        return 0;
    p = player_cur();
    switch (stop) {
    case MM_G_BUY:
        return m->page == MM_PAGE_GARAGE
               ? garage_can_buy_car(p, m->car) == GAR_OK
               : garage_can_buy_part(p, m->gkind, m->car, m->gsel) == GAR_OK;
    case MM_G_SELL:
        return m->page == MM_PAGE_GARAGE
               ? garage_can_sell_car(p, m->car) == GAR_OK
               : garage_can_sell_part(p, m->gkind, m->car, m->gsel) == GAR_OK;
    /* NEXT SKIN NEEDS PAINT TO OFFER. `gskins' is the loaded car's own count and
       is 1 until one is loaded, so the row denies rather than repainting a car
       with the paint it already has. */
    case MM_G_SKIN:
        return m->gskins > 1 && (!p || garage_owns_car(p, m->car));
    /* RACE NEEDS A CAR YOU OWN. The picker walks all three, because reaching a
       car is how you buy one, so the Garage is the one page in the front end
       where the green button can be pointed at a car that is not yours. */
    case MM_G_RACE:
        return !p || garage_owns_car(p, m->car);
    default:
        return 1;
    }
}

/* ---------------------------------------------------------- the geometry */

/* The page's own enum: enumCar on the Garage, enumUpgrades on the part page --
   and dlgSETDETAIL's staticCarEnum, the read-only one, is NOT a stop. Both come
   straight out of dlg_data.h with SE 0, which is what an enum whose value is a
   picture or a bare name has. */
static void mg_enum_rect(const mainmenu_t *m, float *x0, float *y0,
                         float *sx, float *sy)
{
    if (m->page == MM_PAGE_GARAGE) {
        *x0 = DLG_SETCAR_enumCarX0;  *y0 = DLG_SETCAR_enumCarY0;
        *sx = DLG_SETCAR_enumCarSX;  *sy = DLG_SETCAR_enumCarSY;
    } else {
        *x0 = DLG_SETDETAIL_enumUpgradesX0; *y0 = DLG_SETDETAIL_enumUpgradesY0;
        *sx = DLG_SETDETAIL_enumUpgradesSX; *sy = DLG_SETDETAIL_enumUpgradesSY;
    }
}

static void mg_arrows(const mainmenu_t *m, const mmframe *f,
                      float *bx, float *fx, float *ay, float *sz)
{
    float x0, y0, sx, sy;
    mg_enum_rect(m, &x0, &y0, &sx, &sy);
    *sz = MM_Q_ARROW * f->us;
    *ay = py(f, y0 + sy * 0.5f) - *sz * 0.5f;
    *bx = px(f, x0);                    /* SE is 0 on both */
    *fx = px(f, x0 + sx) - *sz;
}

int mainmenu_g_row_at(const mainmenu_t *m, int screen_w, int screen_h,
                      float x, float y, int *left)
{
    const mmframe f = mm_frame(screen_w, screen_h);
    float bx, by, bw, bh, ax, fx, ay, sz;
    int i;

    if (left) *left = 0;
    if (!m || !MM_PAGE_IS_CAR(m->page))
        return -1;
    mg_arrows(m, &f, &ax, &fx, &ay, &sz);
    /* Grown by half its own size, for the reason mainmenu_q_row_at gives; the
       two boxes cannot meet, the narrower of these two enums being 277 wide. */
    if (touch_in(x, y, ax - sz * 0.25f, ay - sz * 0.25f, sz * 1.5f, sz * 1.5f)) {
        if (left) *left = 1;
        return MM_G_ENUM;
    }
    if (touch_in(x, y, fx - sz * 0.25f, ay - sz * 0.25f, sz * 1.5f, sz * 1.5f))
        return MM_G_ENUM;
    /* The bars, on the main menu's own rects AT REST -- a hit box that breathes
       with the slide is a hit box that misses. A DENYING one still answers, so a
       touch on it says why rather than going quiet. */
    for (i = 0; i < MM_G_N_FOCUS; i++) {
        const int row = mg_bar_row(i, NULL);
        if (row < 0 || (i == MM_G_SKIN && m->page != MM_PAGE_GARAGE))
            continue;
        mm_bar_rect(&f, row, &bx, &by, &bw, &bh);
        if (touch_in(x, y, bx, by, bw, bh))
            return i;
    }
    mm_race_rect(&f, &bx, &by, &bw, &bh);
    if (touch_in(x, y, bx, by, bw, bh))
        return MM_G_RACE;
    mm_quit_rect(&f, &bx, &by, &bw, &bh);
    if (touch_in(x, y, bx, by, bw, bh))
        return MM_G_BACK;
    return -1;
}

/* -------------------------------------------------------------- the step */

void mainmenu_open_garage(mainmenu_t *m, int kind)
{
    if (!m)
        return;
    /* REMEMBER THE WAY IN, but only from a page that is not already the shop --
       stepping between dlgSETCAR and dlgSETDETAIL must not overwrite it. */
    if (!MM_PAGE_IS_CAR(m->page))
        m->gfrom = m->page;
    if (kind >= 0 && kind < GAR_N_KINDS) {
        m->page = MM_PAGE_DETAIL;
        m->gkind = kind;
    } else {
        m->page = MM_PAGE_GARAGE;
    }
    /* THE PICKER OPENS ON THE LEVEL YOU COULD BUY NEXT, which is the one the
       engine's own status text prints a price on -- and at level 3 it opens on
       the top one, the one marked INSTALLED. Opening at 0 every time would put
       "You have to sell previous upgrade first!" under the picture of a car
       whose parts are all fitted. */
    if (m->page == MM_PAGE_DETAIL) {
        const int up = garage_level(player_cur(), m->gkind, m->car);
        m->gsel = up < GAR_N_LEVELS ? up : GAR_N_LEVELS - 1;
    }
    m->gfocus = MM_G_TAB + (m->page == MM_PAGE_GARAGE
                            ? MM_GB_SELECT : MM_GB_PART + m->gkind);
    m->garmed = -1;
}

/* THE SHOP'S FOUR ACTIONS, which is what MM_MODAL_ASK's Yes commits. */
enum { MG_ASK_BUY_CAR = 0, MG_ASK_SELL_CAR, MG_ASK_BUY_PART,
       MG_ASK_SELL_PART,
       /* AND THE LOBBY'S OWN, which is not the Garage's business but
          shares its Yes/No panel: 42930, "Do you really want to
          disconnect?". One modal machine, four questions. */
       MG_ASK_DISCONNECT,
       /* AND THE CHAMPIONSHIP'S, which is the one question in this app that
          destroys progress rather than spending money: 40927, "Warning! Do you
          really want to erase data and start new championship?". */
       MG_ASK_NEW_CHAMP };

/* WHETHER THE PICKER CAN GO THAT WAY, and the two pages answer differently --
 * which is measured off the arrows themselves.
 *
 * THE CAR PICKER WRAPS. On the game's own Garage shot the car is the FIRST of
 * the three and BOTH its arrows are red, so there is no end to walk off.
 *
 * THE LEVEL PICKER CLAMPS. On all three of the game's own part-page shots the
 * picker is on level 1 and its LEFT arrow is grey where its right one is red --
 * the only reading of that is a list with ends, and the ends grey out. Which is
 * also the right shape for it: the three levels are a ladder, not a ring, and
 * the status text under the picture is written per level (`Not available' below
 * what you own, `INSTALLED' on it) rather than per step.
 */
static int mg_can_move(const mainmenu_t *m, int d)
{
    if (m->page == MM_PAGE_GARAGE)
        return 1;
    return d < 0 ? m->gsel > 0 : m->gsel < GAR_N_LEVELS - 1;
}

/* One step of the page's picker. On the Garage it walks the CAR -- all three,
   because reaching one is how you buy it -- and on the part page the three
   levels of the part the page is on. */
static void mg_move(mainmenu_t *m, int d)
{
    if (!mg_can_move(m, d)) {
        m->cue = MM_CUE_DENY;
        return;
    }
    if (m->page == MM_PAGE_GARAGE)
        m->car = (m->car + d + MM_N_CARS) % MM_N_CARS;
    else
        m->gsel += d;
    m->cue = MM_CUE_ARROW;
}

/* The three fitted parts, for 40801's `details' field. Every word is the
   game's -- the part names out of str_data.h and "Default" where nothing is
   fitted; the comma between them is the only thing here the port wrote. */
static const char *mg_parts_line(const mainmenu_t *m, int car)
{
    static char buf[128];
    int k, n = 0;
    buf[0] = 0;
    for (k = 0; k < GAR_N_KINDS; k++) {
        const char *s = garage_part_name(k, car,
                                         garage_level(player_cur(), k, car));
        n += snprintf(buf + n, sizeof buf - (size_t)n, "%s%s",
                      k ? ", " : "", s);
        if (n < 0 || (size_t)n >= sizeof buf)
            break;
    }
    (void)m;
    return buf;
}

static void mg_say(mainmenu_t *m, const char *line)
{
    m->modal = MM_MODAL_SAY;
    m->msay = line;
    m->mfocus = MM_MODAL_OK;
    m->marmed = -1;
}

/* The question, and it opens on NO. Every one of these spends or gives up
   something this screen cannot undo, so the destructive answer is never the one
   a thumb lands on by reflex -- the rule the roster's "Do you want to remove
   current player?" already follows. */
static void mg_ask(mainmenu_t *m, int what, const char *line)
{
    m->modal = MM_MODAL_ASK;
    m->gask = what;
    m->msay = line;
    m->mfocus = MM_MODAL_CANCEL;
    m->marmed = -1;
}

/* `Cash remainder $n' -- 40709, the engine's own question line, with the figure
   the purchase would leave. Both of the part page's buttons ask with it, which
   is what the exe's own two handlers do. */
static void mg_ask_cash(mainmenu_t *m, int what, int cash_after)
{
    char money[24];
    garage_cash(money, sizeof money, cash_after);
    snprintf(m->gline, sizeof m->gline, STR_UI_CASH_LEFT, money);
    mg_ask(m, what, m->gline);
}

static void mg_fire(mainmenu_t *m, int stop)
{
    player_t *p = player_cur();
    const int garage = m->page == MM_PAGE_GARAGE;
    gar_result r;

    if (stop >= MM_G_TAB && stop < MM_G_TAB + MM_GB_N) {
        const int tab = stop - MM_G_TAB;
        if (!mg_tab_here(m, tab))
            mainmenu_open_garage(m, mg_tab_kind(tab));
        m->cue = MM_CUE_PRESS;
        return;
    }
    switch (stop) {
    case MM_G_ENUM:
        mg_move(m, +1);                 /* CROSS on a picker steps it */
        return;
    case MM_G_RACE:
        if (!mainmenu_g_live(m, stop)) {
            /* A car that is not in the garage cannot be raced, and the engine
               has a word for a car you do not have. */
            mg_say(m, garage_reason(GAR_NOT_OWNED));
            m->cue = MM_CUE_DENY;
            return;
        }
        m->action = MM_ACT_RACE;
        m->cue = MM_CUE_PRESS;
        return;
    case MM_G_BACK:
        /* BACK GOES TO THE PAGE YOU CAME IN BY: a part page steps back to the
           Garage and the Garage to the quick-race summary, which is the bar
           that opened it. One press per step in, like every other back button
           in this app. */
        if (m->page == MM_PAGE_DETAIL) {
            mainmenu_open_garage(m, -1);
        } else if (m->gfrom == MM_PAGE_CHAMP) {
            /* THE LADDER OPENED IT, so the ladder is where Back goes -- the
               same one-press-in, one-press-out rule the quick-race page's own
               Garage bar follows, applied to the second page that can open the
               shop. */
            m->page = MM_PAGE_CHAMP;
            m->cfocus = MM_C_NAV + MM_CB_GARAGE;
        } else {
            /* AND IT LEAVES THE FOCUS ON THE BAR THAT OPENED IT, not on Race
               summary's own -- one press in and one press out, landing where
               the thumb already was. mm_q_nav would put it on the summary bar,
               which is right when a tab SWITCHES a view and wrong when a Back
               unwinds a step. */
            m->page = MM_PAGE_QUICK;
            m->qfocus = MM_Q_NAV + MM_QB_GARAGE;
        }
        m->cue = MM_CUE_PRESS;
        return;
    case MM_G_SKIN:
        if (!mainmenu_g_live(m, stop)) {
            m->cue = MM_CUE_DENY;
            return;
        }
        /* THE ENGINE'S OWN LINE, and it SAVES: FUN_004d62a0 does
           `up[3] = (up[3] + 1) % nskins' and writes the profile out. A paint is
           a property of the car and has to survive the launch. */
        garage_set_skin(p, m->car,
                        garage_next_skin(garage_skin(p, m->car), m->gskins));
        m->action = MM_ACT_GARAGE;
        m->cue = MM_CUE_PRESS;
        return;
    case MM_G_BUY:
    case MM_G_SELL:
        break;
    default:
        return;
    }

    /* The four money buttons. Each ASKS in the engine's own words, and the
       refusals are the engine's own words too -- garage.h has the table. `p' is
       never dereferenced below without a guard having passed, and every guard
       answers GAR_NO_PROFILE first when there is none. */
    if (garage && stop == MM_G_BUY) {
        char money[24];
        r = garage_can_buy_car(p, m->car);
        if (r != GAR_OK) { mg_say(m, garage_reason(r)); m->cue = MM_CUE_DENY; return; }
        garage_cash(money, sizeof money, p->cash - garage_car_price(m->car));
        snprintf(m->gline, sizeof m->gline, "%s %s?  %s",
                 STR_UI_BUY_CAR_ASK, STR_CAR_NAME[m->car], money);
        mg_ask(m, MG_ASK_BUY_CAR, m->gline);
    } else if (garage) {
        r = garage_can_sell_car(p, m->car);
        if (r != GAR_OK) { mg_say(m, garage_reason(r)); m->cue = MM_CUE_DENY; return; }
        snprintf(m->gline, sizeof m->gline, STR_UI_SELL_CAR_ASK,
                 STR_CAR_NAME[m->car], mg_parts_line(m, m->car));
        mg_ask(m, MG_ASK_SELL_CAR, m->gline);
    } else if (stop == MM_G_BUY) {
        r = garage_can_buy_part(p, m->gkind, m->car, m->gsel);
        if (r != GAR_OK) { mg_say(m, garage_reason(r)); m->cue = MM_CUE_DENY; return; }
        mg_ask_cash(m, MG_ASK_BUY_PART,
                    p->cash - garage_part_price(m->gkind, m->car, m->gsel + 1));
    } else {
        r = garage_can_sell_part(p, m->gkind, m->car, m->gsel);
        if (r != GAR_OK) { mg_say(m, garage_reason(r)); m->cue = MM_CUE_DENY; return; }
        mg_ask_cash(m, MG_ASK_SELL_PART,
                    p->cash + garage_part_sell(m->gkind, m->car, m->gsel + 1));
    }
    m->cue = MM_CUE_PRESS;
}

/* What Yes does. Every one of these goes back through garage.c, so the guard
   that answered the button is the guard that answers here as well: a modal can
   sit on screen while something else moves, and asking twice costs nothing. */
static void mg_commit(mainmenu_t *m)
{
    player_t *p = player_cur();
    gar_result r = GAR_NO_PROFILE;

    m->modal = MM_MODAL_NONE;
    switch (m->gask) {
    case MG_ASK_BUY_CAR:   r = garage_buy_car(p, m->car); break;
    case MG_ASK_SELL_CAR:
        r = garage_sell_car(p, m->car);
        if (r == GAR_OK) {
            /* THE CAR YOU SOLD CANNOT STAY SELECTED. The picker moves to the
               next one in the garage; there is always one, because selling the
               last is only allowed when the money can buy another and
               garage_can_sell_car is what enforces that. */
            const int nxt = garage_next_owned_car(p, m->car);
            if (nxt >= 0)
                m->car = nxt;
        }
        break;
    case MG_ASK_BUY_PART:
        r = garage_buy_part(p, m->gkind, m->car, m->gsel);
        break;
    case MG_ASK_SELL_PART:
        r = garage_sell_part(p, m->gkind, m->car, m->gsel);
        break;
    case MG_ASK_NEW_CHAMP:
        /* THE LADDER GOES BACK TO THE BOTTOM and the cursor with it -- champ.c
           says exactly what is cleared and what is not (the record book and the
           Garage survive). The profile is written by the caller, on
           MM_ACT_GARAGE, which is already "the shop changed the profile, save
           it"; this is the same event with a different cause. */
        champ_new(p);
        mainmenu_open_champ(m);
        m->action = MM_ACT_GARAGE;
        m->cue = MM_CUE_PRESS;
        return;
    case MG_ASK_DISCONNECT:
        /* THE LOBBY'S OWN. Leaving is not a purchase and has no reason code:
           tell the other end, drop the socket's session and go back to the page
           the game was made on. */
        net_leave();
        mainmenu_open_multi(m);
        m->cue = MM_CUE_PRESS;
        return;
    default:
        break;
    }
    if (r != GAR_OK) {
        mg_say(m, garage_reason(r));
        m->cue = MM_CUE_DENY;
        return;
    }
    m->action = MM_ACT_GARAGE;
    m->cue = MM_CUE_PRESS;
}

static void mm_step_garage(mainmenu_t *m, unsigned int down,
                           const touch_state *tp, int screen_w, int screen_h)
{
    const int page = m->page;
    const int n = mainmenu_g_nfocus(page);

    if (n <= 0)
        return;
    if (down & SCE_CTRL_DOWN) {
        m->gfocus = mainmenu_g_stop(page, mg_index(page, m->gfocus) + 1);
        m->cue = MM_CUE_FOCUS;
    }
    if (down & SCE_CTRL_UP) {
        m->gfocus = mainmenu_g_stop(page, mg_index(page, m->gfocus) - 1);
        m->cue = MM_CUE_FOCUS;
    }
    /* LEFT and RIGHT walk the page's own picker wherever the focus is, which is
       what they do on the main menu's carousel: the picker is the page's
       subject, and reaching it should not cost a trip round the ring. */
    if (down & SCE_CTRL_LEFT)  mg_move(m, -1);
    if (down & SCE_CTRL_RIGHT) mg_move(m, +1);
    if (down & (SCE_CTRL_CROSS | SCE_CTRL_START))
        mg_fire(m, m->gfocus);
    if (down & SCE_CTRL_CIRCLE) {
        mg_fire(m, MM_G_BACK);
        return;
    }

    if (!tp)
        return;
    if (tp->pressed) {
        int left;
        m->garmed = mainmenu_g_row_at(m, screen_w, screen_h,
                                      tp->x, tp->y, &left);
        if (m->garmed >= 0 && m->gfocus != m->garmed) {
            m->gfocus = m->garmed;
            m->cue = MM_CUE_FOCUS;
        }
    }
    if (tp->released) {
        int left;
        const int row = mainmenu_g_row_at(m, screen_w, screen_h,
                                          tp->x, tp->y, &left);
        if (row >= 0 && row == m->garmed) {
            if (row == MM_G_ENUM)
                mg_move(m, left ? -1 : +1);   /* the back arrow goes back */
            else
                mg_fire(m, row);
        }
        m->garmed = -1;
    }
}

/* -------------------------------------------------------------- the draw */

/* The four radio bars and the page's own two or three money bars, on the main
   menu's own eight row rects. */
static void mg_draw_bars(const mainmenu_t *m, const mmframe *f)
{
    int i;

    for (i = 0; i < MM_G_N_FOCUS; i++) {
        float bx, by, bw, bh, slide = 0.f;
        int orange = 0;
        const int row = mg_bar_row(i, &orange);
        const int tab = i >= MM_G_TAB && i < MM_G_TAB + MM_GB_N;
        const int here = tab && mg_tab_here(m, i - MM_G_TAB);
        const int lit = m->gfocus == i;
        const int live = tab ? 1 : mainmenu_g_live(m, i);

        if (row < 0 || (i == MM_G_SKIN && m->page != MM_PAGE_GARAGE))
            continue;
        if (here || lit)
            slide = MM_SLIDE * MM_SETTLED;
        mm_draw_wedge(m, f, row);
        mm_bar_draw_rect(m, f, row, slide, &bx, &by, &bw, &bh);

        /* THE FOUR TABS ARE RADIO CELLS and the money bars are not, and the art
           is what says so: the game's shots put a DOT on every bar on these two
           pages, and ButtonsTextures' orange and grey cells carry one of their
           own (v 96 and v 224) where its red cells carry a triangle. So the tabs
           come out of RadioButtonsTextures and the rest out of the two dotted
           cells that already have it. */
        if (tab && m->tex.radio) {
            const float v = here ? MM_V_RAD_ON : MM_V_RAD;
            ui_image(bx, by, bw, bh, m->tex.radio,
                     0.f, v, 1.f, v + MM_V_RAD_CELL, 1.f, 1.f, 1.f, 1.f);
        } else if (m->tex.buttons) {
            const float v = !live ? MM_V_GREY_DOT
                                  : (orange ? (lit ? MM_V_ORANGE_F : MM_V_ORANGE)
                                            : (lit ? MM_V_RED_F : MM_V_RED));
            ui_image(bx, by, bw, bh, m->tex.buttons,
                     0.f, v, 1.f, v + MM_V_CELL, 1.f, 1.f, 1.f, 1.f);
        } else {
            ui_rect(bx, by, bw, bh, live ? 0.72f : 0.45f,
                    live ? 0.09f : 0.45f, live ? 0.11f : 0.47f, 1.f);
        }
        mm_label(m, f, bx, by, bw, bh, mg_bar_name(m, i),
                 live ? 1.f : 0.82f, live ? 1.f : 0.82f, live ? 1.f : 0.84f);
    }
}

/* dlgCARSCOMM's furniture, which both pages stand on: the car itself, the cash
   line and the "Car" heading. */
static void mg_draw_common(const mainmenu_t *m, const mmframe *f)
{
    const player_t *p = player_cur();
    const int c = (m->car < 0 || m->car >= MM_N_CARS) ? 0 : m->car;
    char money[24];

    /* animCarPreview -- the car in 3D, out of ui.c's ortho and back in, exactly
       as animCar is on the quick-race page. */
    {
        float x, y, w, h;
        mm_box(f, DLG_CARSCOMM_animCarPreviewX0, DLG_CARSCOMM_animCarPreviewY0,
               DLG_CARSCOMM_animCarPreviewSX, DLG_CARSCOMM_animCarPreviewSY,
               &x, &y, &w, &h);
        if (m->car_draw) {
            ui_end();
            m->car_draw(m->car_ctx, x, y, w, h);
            ui_begin((int)f->w, (int)f->h);
        } else {
            const sfont sf = sf_big(m->tex.font_big);
            const float sc = f->us * MM_TS_TRACK;
            const char *s = STR_CAR_NAME[c];
            const float tw = sf.tex ? sf_w(&sf, sc, s) : ui_text_w(sc, s);
            if (sf.tex)
                sf_text_shadowed(&sf, x + w * 0.5f - tw * 0.5f, y + h * 0.55f,
                                 sc, 1.f, 1.f, 1.f, 0.45f, s);
            else
                ui_text(x + w * 0.5f - tw * 0.5f, y + h * 0.55f, sc,
                        1.f, 1.f, 1.f, 0.45f, s);
        }
    }

    /* staticCashText / staticPlayerCash -- "Your cash" over its own rule, with
       the figure to its right. Both left-aligned on their own X0, which is what
       the screenshot has: the word starts at 116 and the figure at 317. */
    mm_q_text(m, f, 0, DLG_CARSCOMM_staticCashTextX0,
              DLG_CARSCOMM_staticCashTextY0, MM_TS_LABEL, 0, 1.f,
              STR_UI_YOUR_CASH);
    mg_rule(f, DLG_CARSCOMM_staticCashTextX0, DLG_CARSCOMM_staticCashTextY0,
            DLG_CARSCOMM_staticCashTextSX, DLG_CARSCOMM_staticCashTextSY);
    garage_cash(money, sizeof money, p ? p->cash : CHAMP_DEFAULT_CASH);
    mm_q_text(m, f, 0, DLG_CARSCOMM_staticPlayerCashX0,
              DLG_CARSCOMM_staticPlayerCashY0, MM_TS_LABEL, 0, 1.f, money);

    /* staticCarName -- the word "Car" and its rule, 41500. */
    mm_q_text(m, f, 0, DLG_CARSCOMM_staticCarNameX0,
              DLG_CARSCOMM_staticCarNameY0, MM_TS_LABEL, 0, 1.f, STR_UI_CAR);
    mg_rule(f, DLG_CARSCOMM_staticCarNameX0, DLG_CARSCOMM_staticCarNameY0,
            DLG_CARSCOMM_staticCarNameSX, DLG_CARSCOMM_staticCarNameSY);
}

/* The Garage's own half: the spec block with its price line, the status line,
   and the "Current upgrades" table. */
static void mg_draw_setcar(const mainmenu_t *m, const mmframe *f)
{
    const player_t *p = player_cur();
    const int c = (m->car < 0 || m->car >= MM_N_CARS) ? 0 : m->car;
    const int owned = garage_owns_car(p, c);
    const float rx = DLG_CARSCOMM_staticCarExplainX0
                     + DLG_CARSCOMM_staticCarExplainSX;
    char money[24], line[96];
    float y;
    int k;

    /* staticCarExplain -- the car's own SEVEN-line spec block, right-aligned,
       and then the price as its eighth line. See the section header on why the
       price line is here rather than at staticPrice's own Y. */
    y = mg_lines(m, f, rx, DLG_CARSCOMM_staticCarExplainY0, 1, 1.f,
                 STR_CAR_INFO[c]);
    garage_cash(money, sizeof money,
                owned ? garage_car_sell(p, c) : garage_car_price(c));
    snprintf(line, sizeof line,
             owned ? STR_UI_SELL_PRICE : STR_UI_BUY_PRICE, money);
    mm_q_text(m, f, 0, DLG_SETCAR_staticPriceX0 + DLG_SETCAR_staticPriceSX,
              y, MM_TS_INFO, 1, 1.f, line);

    /* staticStatus -- only a car you do NOT own has anything to say here, which
       is the engine's own branch: 40807 when the cash covers it and 40808 when
       it does not. */
    if (!owned)
        mg_lines(m, f, DLG_SETCAR_staticStatusX0, DLG_SETCAR_staticStatusY0,
                 0, 1.f,
                 garage_can_buy_car(p, c) == GAR_OK ? STR_UI_CAN_BUY_CAR
                                                    : STR_UI_NOT_AVAILABLE);

    /* staticUpgrades / staticCarInfoLeft / staticCarInfoRight -- the heading
       with its rule, the three labels as the engine's own one string, and the
       three fitted parts beside them. */
    mm_q_text(m, f, 0, DLG_SETCAR_staticUpgradesX0,
              DLG_SETCAR_staticUpgradesY0, MM_TS_LABEL, 0, 1.f,
              STR_UI_CUR_UPGRADES);
    mg_rule(f, DLG_SETCAR_staticUpgradesX0, DLG_SETCAR_staticUpgradesY0,
            DLG_SETCAR_staticUpgradesSX, DLG_SETCAR_staticUpgradesSY);
    mg_lines(m, f, DLG_SETCAR_staticCarInfoLeftX0,
             DLG_SETCAR_staticCarInfoLeftY0, 0, 1.f, STR_UI_UPGRADE_LABELS);
    y = DLG_SETCAR_staticCarInfoRightY0;
    for (k = 0; k < GAR_N_KINDS; k++) {
        mm_q_text(m, f, 0, DLG_SETCAR_staticCarInfoRightX0, y, MM_TS_INFO, 0,
                  1.f, garage_part_name(k, c, garage_level(p, k, c)));
        y += MM_G_LINE_H;
    }
}

/* The part page's own half: the heading, the part's block with its price line,
   the photograph between the picker's arrows, and the status word. */
static void mg_draw_setdetail(const mainmenu_t *m, const mmframe *f)
{
    const player_t *p = player_cur();
    const int c = (m->car < 0 || m->car >= MM_N_CARS) ? 0 : m->car;
    const int kind = (m->gkind < 0 || m->gkind >= GAR_N_KINDS) ? 0 : m->gkind;
    const int sel = (m->gsel < 0 || m->gsel >= GAR_N_LEVELS) ? 0 : m->gsel;
    const gar_state st = garage_state(p, kind, c, sel);
    const float rx = DLG_SETDETAIL_staticUpgradeInfoX0
                     + DLG_SETDETAIL_staticUpgradeInfoSX;
    char money[24], line[96];
    float y;

    /* staticCarExplain again -- the SEVEN-line block with no price line, which
       is what all three of the game's own part-page shots have. */
    mg_lines(m, f, DLG_CARSCOMM_staticCarExplainX0
                   + DLG_CARSCOMM_staticCarExplainSX,
             DLG_CARSCOMM_staticCarExplainY0, 1, 1.f, STR_CAR_INFO[c]);

    /* staticUpgradeName -- Booster / Engine / Tires, with its rule. */
    mm_q_text(m, f, 0, DLG_SETDETAIL_staticUpgradeNameX0,
              DLG_SETDETAIL_staticUpgradeNameY0, MM_TS_LABEL, 0, 1.f,
              garage_kind_name(kind));
    mg_rule(f, DLG_SETDETAIL_staticUpgradeNameX0,
            DLG_SETDETAIL_staticUpgradeNameY0,
            DLG_SETDETAIL_staticUpgradeNameSX,
            DLG_SETDETAIL_staticUpgradeNameSY);

    /* staticUpgradeInfo -- the part's name, its two-line effect block and then
       its price: four right-aligned lines at the small font's own pitch. The
       fourth depends on which of the four states the picker is in -- a price on
       the one you may buy, what it fetches back on the one you own, and nothing
       at all on the two you cannot act on. */
    y = mg_lines(m, f, rx, DLG_SETDETAIL_staticUpgradeInfoY0, 1, 1.f,
                 garage_part_name(kind, c, sel + 1));
    y = mg_lines(m, f, rx, y, 1, 1.f, garage_part_info(kind, c, sel + 1));
    if (st == GAR_ST_PRICE || st == GAR_ST_INSTALLED) {
        garage_cash(money, sizeof money,
                    st == GAR_ST_PRICE
                    ? garage_part_price(kind, c, sel + 1)
                    : garage_part_sell(kind, c, sel + 1));
        snprintf(line, sizeof line, "%s: %s",
                 st == GAR_ST_PRICE ? STR_UI_PRICE : STR_UI_SELL_PART_FOR,
                 money);
        mm_q_text(m, f, 0, rx, y, MM_TS_INFO, 1, 1.f, line);
    }

    /* shotlistUpgrade -- `upgr_<fam><level>_<car+1>', the LEVEL first; see
       garage.h, which says how that came out and how it was checked. */
    mg_shot(f, m->tex.upgr[kind][c][sel],
            DLG_SETDETAIL_shotlistUpgradeX0, DLG_SETDETAIL_shotlistUpgradeY0,
            DLG_SETDETAIL_shotlistUpgradeSX, DLG_SETDETAIL_shotlistUpgradeSY);

    /* staticUpgradeStatus -- the word under the picture. The buyable state says
       nothing here (its price is in the block above), which is what the game's
       own three shots have: all three are at Default with the picker on level 1
       and none of them carries a status word. `Not available' is drawn at the
       engine's own 0.5 -- FUN_004d4fc0 writes 0x3f000000 beside that string and
       beside no other. */
    if (st != GAR_ST_PRICE)
        mg_lines(m, f, DLG_SETDETAIL_staticUpgradeStatusX0,
                 DLG_SETDETAIL_staticUpgradeStatusY0, 0,
                 st == GAR_ST_NOT_AVAIL ? 0.5f : 1.f,
                 st == GAR_ST_NOT_AVAIL ? STR_UI_NOT_AVAILABLE
                 : (st == GAR_ST_INSTALLED ? STR_UI_INSTALLED
                                           : STR_UI_SELL_PREV));
}

/* One enum's value, centred between its two arrows -- the car's name, which is
   what both of these pages put there. */
static void mg_value(const mainmenu_t *m, const mmframe *f, float ax, float sz,
                     float fx, float cy, float alpha, const char *s)
{
    const sfont sf = sf_small(m->tex.font_small);
    const float sc = f->us * MM_TS_LABEL;
    const float w = sf.tex ? sf_w(&sf, sc, s) : ui_text_w(sc, s);
    const float h = sf.tex ? sf_h(&sf, sc) : ui_text_h(sc);
    const float x = (ax + sz + fx) * 0.5f - w * 0.5f;
    if (sf.tex)
        sf_text_shadowed(&sf, x, cy - h * 0.5f, sc, 1.f, 1.f, 1.f, alpha, s);
    else
        ui_text(x, cy - h * 0.5f, sc, 1.f, 1.f, 1.f, alpha, s);
}

/* The page's enum arrows, drawn last so the pair either side of the part
   photograph sits over it -- the order the game's own screens have. */
static void mg_draw_enum(const mainmenu_t *m, const mmframe *f)
{
    const int c = (m->car < 0 || m->car >= MM_N_CARS) ? 0 : m->car;
    float ax, fx, ay, sz, x0, y0, sx, sy;
    const int lit = m->gfocus == MM_G_ENUM;

    mg_arrows(m, f, &ax, &fx, &ay, &sz);
    mg_enum_rect(m, &x0, &y0, &sx, &sy);
    /* AN ARROW THAT CANNOT MOVE IS GREY, which is the artists' own third cell
       and what the game's own part-page shots have on the LEFT one at level 1.
       A focused pair is silver; the disabled cell wins over both. */
    mm_arrow(m, ax, ay, sz, !mg_can_move(m, -1) ? 2 : (lit ? 1 : 0), 0);
    mm_arrow(m, fx, ay, sz, !mg_can_move(m, +1) ? 2 : (lit ? 1 : 0), 1);

    /* THE GARAGE'S ENUM SHOWS THE CAR'S NAME between its arrows; the part
       page's value is the PHOTOGRAPH and carries no text at all, which is what
       enumTrack does with the track's picture on dlgRACESUM. */
    if (m->page == MM_PAGE_GARAGE)
        mg_value(m, f, ax, sz, fx, py(f, y0 + sy * 0.5f), 1.f,
                 STR_CAR_NAME[c]);

    /* AND THE PART PAGE CARRIES A SECOND, DEAD PAIR: staticCarEnum, an &ENM the
       exe's own table names `static', at dlgSETCAR's enumCar rectangle. Its
       arrows are GREY on all three of the game's shots and its value is the
       car's name -- you cannot change car from a part page, and the control says
       so in the artists' own disabled cell. */
    if (m->page == MM_PAGE_DETAIL) {
        const float asz = MM_Q_ARROW * f->us;
        const float cy = py(f, DLG_SETDETAIL_staticCarEnumY0
                               + DLG_SETDETAIL_staticCarEnumSY * 0.5f);
        const float bx = px(f, DLG_SETDETAIL_staticCarEnumX0);
        const float fx2 = px(f, DLG_SETDETAIL_staticCarEnumX0
                                + DLG_SETDETAIL_staticCarEnumSX) - asz;
        mm_arrow(m, bx, cy - asz * 0.5f, asz, 2, 0);
        mm_arrow(m, fx2, cy - asz * 0.5f, asz, 2, 1);
        mg_value(m, f, bx, asz, fx2, cy, 0.82f, STR_CAR_NAME[c]);
    }
}

static void mm_draw_garage(const mainmenu_t *m, const mmframe *f)
{
    mg_draw_bars(m, f);
    mg_draw_common(m, f);
    if (m->page == MM_PAGE_GARAGE)
        mg_draw_setcar(m, f);
    else
        mg_draw_setdetail(m, f);
    mg_draw_enum(m, f);
}

/* ============================== dlgMULTIPLAYER and dlgWAITPLAYERS_* -- NET
 *
 * TWO SCREENS, SIX DIALOGS' WORTH OF CONTROLS, and every rectangle is shipped.
 * `net.h' is the transport; see mainmenu.h for what is deliberately NOT built
 * here (Two players, Chat, Options) and why.
 *
 * THE EXE'S OWN CONTROL TABLES, the same `{ id, type, name, label }' records
 * dlgRACESUM's are -- and the fourth field, which the pages built before this
 * one all had zero in, turns out to be a STRING ID: the control's own label.
 * That is a general fact about the whole table and it is what makes these three
 * dialogs readable without measuring a single word:
 *
 *   dlgMULTIPLAYER @ 0x56f0b8, ids 0xede..0xee1
 *     0xede &SHS  shotListTracks      the track carousel
 *     0xedf &APV  animPrevCar         behind it, and unused -- dlgMAIN has the
 *                                     same pair and the main menu draws no car
 *     0xee0 &STT  staticInfoHeader    the track's NAME, with a rule
 *     0xee1 &STT  staticInfo          its three-line blurb
 *
 *   dlgWAITPLAYERS_RACESUM @ 0x570130, ids 0x1450..0x1455
 *     0x1450 &ENM  enumNLaps   label 42904 `N laps'
 *     0x1451 &ENM  enumTrack   label 42906 `Map'
 *     0x1452 &TBL  table
 *     0x1454 &CHA  chat        the message panel
 *     0x1455 &EDT  editChat    -- not built
 *
 *   dlgWAITPLAYERS_CARSETUP @ 0x56ffc8, ids 0x14b4..0x14ba
 *     0x14b4 &ENM  enumBoost   label 43000 `Booster:'
 *     0x14b5 &ENM  enumReson   label 43001 `Engine:'
 *     0x14b6 &ENM  enumTires   label 43002 `Tires:'
 *     0x14b7 &ENM  enumCar
 *     0x14b8 &CHA  chat
 *     0x14b9 &EDT  editChat    -- not built
 *     0x14ba &BTN  none        label 10012 `Next skin'  <- row 7
 *
 *   dlgWAITPLAYERS_CARRESTR @ 0x56ff08, ids 0x157c..0x1582
 *     0x157c &ENM  enumCar1    label 40100 `Road Rage RR'
 *     0x157d &ENM  enumCar2    label 40104 `Tornado Buggy TB'
 *     0x157e &ENM  enumCar3    label 40108 `Warhammer WH'
 *     0x157f &STT  staticHeader1 label 43211 `Car type'
 *     0x1580 &STT  staticHeader2 label 43212 `Car enabled'
 *     0x1581 &CHA  chat
 *     0x1582 &EDT  editChat    -- not built
 *
 * AND THE PLAYER CARD ON THE FRONT PAGE IS dlgPLRSCOMM's, to the pixel. That is
 * measured, off the game's own screenshot of dlgMULTIPLAYER: the name's ink at
 * y 89 from x 240, its rule at 110 running 240..519, three info lines at 119,
 * 142 and 164 on a flat 23 pitch, the second rule at 184 and `Scores' at 191 --
 * which is MP_NAME_Y, MP_RULE1_Y, MP_RULE_W, MP_INFO_Y, MP_INFO_LH, MP_RULE2_Y
 * and MP_CASH_Y, every one of them, with nothing to change. So `mp_draw_card'
 * draws it, and dlgPLRSCOMM.ini's four rectangles are in `dlg_data.h' now
 * rather than only in that page's measured constants.
 *
 * ONE THING ON IT IS NOT THE ROSTER PAGE'S: this page's `Scores' line is
 * LEFT-aligned at 240 and carries no `Cash' beside it, where the roster page
 * right-aligns the pair on 520. Measured on the same shot -- the line runs
 * 240..306 -- so the card takes a flag rather than being copied.
 *
 * WHAT WAS MEASURED FOR THE REST, and only these:
 *
 *  - THE BOTTOM BLOCK'S RULE IS ITS BOX'S OWN BOTTOM EDGE, run its full width,
 *    which is the same rule the Garage's headings follow: `Surf' ink at 426..442
 *    from x 233 with its rule at 447 spanning 233..406, against
 *    staticInfoHeader's (233, 417) 174x30 -- 417 + 30 and 233 + 174, exactly.
 *    The three blurb lines then sit at 456, 479 and 501, a flat 22.7, which is
 *    the small font's own 23 (SF_SMALL_SIZE_Y + SF_SMALL_LINE) and not the 25
 *    the quick-race pages use.
 *  - THE CAROUSEL IS THE MAIN MENU'S. shotListTracks is (223, 269) 199x152 with
 *    bounds L 12, R 632, band 303..379 and space 3, which places the four
 *    thumbnails at 15, 119, 425 and 529 at 101 px wide -- and the game's own
 *    screenshot of this page has its left pair at exactly 15..114 and 119..219.
 *    So `mm_draw_carousel' draws it, and the 12 px by which the main menu's own
 *    measured right pair (437, 542) differs from the shipped 425 and 529 is a
 *    gap in `known-issues.md' rather than a second carousel.
 *  - THE SIX TABS ARE ROWS 0..5 of the main menu's own eight and the page's own
 *    button is row 7, measured off the trays: 131, 176, 221, 266, 311, 356 and
 *    then 466 on the Race summary shot (Kick player off) and 481 on the Car
 *    setup one (Next skin, orange, so the mask sees less of it). Four tabs are
 *    built, so they take rows 0..3.
 *  - AND THE TABLE'S COLUMNS ARE CENTRED, except the first. Off the header's own
 *    ink -- Player 113..157, Car 241..264, Boost 320..357, Engine 374..415,
 *    Tires 436..468, Status 493..536, with the header's rule at y 196 -- against
 *    tableWidth1..5 of 19, 26, 13, 13, 13 percent of 466 from x 86 and the last
 *    column taking the 16% the file never names. Centred, the last column lands
 *    at 514.75 against a measured 514.5 and the rest within 17 px; `Player' is
 *    LEFT-aligned instead, because the skin swatch is in front of it -- which is
 *    what `koefDrawSkin', `shiftDrawSkinX' and `sizeDrawSkinY' are for, and the
 *    swatch measures x 92..107, i.e. tableX0 + shiftDrawSkinX exactly.
 */

/* THE SKIN SWATCH in the table's first column, and all three of the keys the
 * dialog ships for it are used.
 *
 * `skin_ik_vse' is Interface.sb's own skin-icon sheet: 128x128, a 4 x 4 GRID of
 * 32x32 cells -- twelve paint swatches and a camera icon in the thirteenth. Cell
 * (car, skin) is row `car', column `skin', which the game's own lobby
 * screenshot settles: its one row is the Overkill on skin 0 and the patch on
 * screen reads (77, 80, 153) blue over (251, 70, 28) orange, which is the
 * TOP-LEFT cell -- the blue body with the orange flames.
 *
 * AND IT IS A SQUARE OF `sizeDrawSkinY / koefDrawSkin'. Measured on that shot
 * at x 89..106 and y 203..220, i.e. 18 x 18 centred in its row; the two keys
 * are 9 and 0.50, and 9 / 0.5 is 18. That is a reading rather than a proof --
 * two numbers whose quotient is the answer could be a coincidence -- but it is
 * the only reading under which both keys mean anything, and the square is
 * measured either way. `shiftDrawSkinX' is 6 and tableX0 + 6 is 92 against a
 * measured 89..91. */
#define MM_L_SWATCH (DLG_WAITPLAYERS_RACESUM_sizeDrawSkinY \
                     / DLG_WAITPLAYERS_RACESUM_koefDrawSkin)
#define MM_L_SWATCH_CELL 0.25f      /* 4 x 4 cells of a 128 px sheet */
#define MM_L_TEXT_GAP   5.f

/* Where the first column's TEXT starts: past the swatch. Both the header and
   every name line up on it on the game's own shot, at x 113. */
#define MM_L_NAME_X (DLG_WAITPLAYERS_RACESUM_tableX0 \
                     + DLG_WAITPLAYERS_RACESUM_shiftDrawSkinX \
                     + MM_L_SWATCH + MM_L_TEXT_GAP)

/* A cell's text, centred on `cx' -- which is what the table's columns 1..5 are
   and what makes its last one land on the measured 514.5. */
static void mm_l_text_c(const mainmenu_t *m, const mmframe *f, float cx,
                        float y, float alpha, const char *s)
{
    const sfont sf = sf_small(m->tex.font_small);
    const float sc = f->us * MM_TS_INFO;
    const float w = sf.tex ? sf_w(&sf, sc, s) : ui_text_w(sc, s);
    if (sf.tex)
        sf_text_shadowed(&sf, px(f, cx) - w * 0.5f, py(f, y), sc,
                         1.f, 1.f, 1.f, alpha, s);
    else
        ui_text(px(f, cx) - w * 0.5f, py(f, y), sc, 1.f, 1.f, 1.f, alpha, s);
}

/* The lobby's own line pitch, which is the car pages' -- the small font's. */
#define MM_L_LINE_H (SF_SMALL_SIZE_Y + SF_SMALL_LINE)

const int MM_L_NCTRL[MM_L_N_VIEW] = { 2, 2, 4, 3 };

/* The four tabs' own words: 10015, 10016, 42900, 42902. */
static const char *const MM_L_TAB_NAME[MM_L_N_VIEW] = {
    STR_UI_RACE_SUMMARY, STR_UI_MAP_AND_INFO, STR_UI_CAR_SETUP,
    STR_UI_CAR_RESTR
};

/* dlgWAITPLAYERS_CARRESTR's own five values: 43206 and 43207..43210. Index 0 is
   `Disable' -- the car is not allowed -- and 1..4 name the highest upgrade level
   it may wear. NET_RESTR_* is the same numbering. */
static const char *const MM_L_RESTR_NAME[NET_RESTR_N] = {
    STR_UI_RESTR_DISABLE, STR_UI_PART_DEFAULT,
    STR_UI_LEVEL_1, STR_UI_LEVEL_2, STR_UI_LEVEL_3
};

/* ------------------------------------------------------ the front page */

/* Which of the eight rows a front-page stop is; -1 for the two that are not
   bars. THE ROW `Two players' WOULD BE IS LEFT EMPTY -- see mainmenu.h. */
static int mm_m_bar_row(int stop)
{
    switch (stop) {
    case MM_M_CREATE: return 0;
    case MM_M_JOIN:   return 1;
    default:          return -1;
    }
}

int mainmenu_m_live(const mainmenu_t *m, int stop)
{
    if (!m)
        return 0;
    /* THE GREEN BUTTON IS DEAD ON THIS PAGE and the game's own screenshot of it
       says so: there is no game to race in until Create or Join has made one,
       so Race is grey exactly the way the main menu's five unbuilt rows are. */
    if (stop == MM_M_RACE)
        return 0;
    return 1;
}

int mainmenu_m_row_at(const mainmenu_t *m, int screen_w, int screen_h,
                      float x, float y)
{
    const mmframe f = mm_frame(screen_w, screen_h);
    float bx, by, bw, bh;
    int i;

    if (!m || m->page != MM_PAGE_MULTI)
        return -1;
    for (i = 0; i < MM_M_N_FOCUS; i++) {
        const int row = mm_m_bar_row(i);
        if (row < 0) continue;
        mm_bar_rect(&f, row, &bx, &by, &bw, &bh);
        if (touch_in(x, y, bx, by, bw, bh))
            return i;
    }
    mm_race_rect(&f, &bx, &by, &bw, &bh);
    if (touch_in(x, y, bx, by, bw, bh))
        return MM_M_RACE;
    mm_quit_rect(&f, &bx, &by, &bw, &bh);
    if (touch_in(x, y, bx, by, bw, bh))
        return MM_M_BACK;
    return -1;
}

void mainmenu_open_multi(mainmenu_t *m)
{
    if (!m)
        return;
    m->page = MM_PAGE_MULTI;
    m->mfocus_multi = MM_M_CREATE;
    m->marmed_multi = -1;
}

/* ------------------------------------------------------------ the lobby */

/* One of the lobby's own controls, per view: the rectangle and the SE split
   straight out of its dialog's `.ini', with the label the exe's own record
   names. An empty label is a control whose value is all there is. */
static const mm_enum *mm_l_ctrl(int view, int i)
{
    static const mm_enum V[MM_L_N_VIEW][4] = {
        /* MM_L_RACESUM -- Map over N laps, both labelled, both the HOST's */
        { { DLG_WAITPLAYERS_RACESUM_enumTrackX0,
            DLG_WAITPLAYERS_RACESUM_enumTrackY0,
            DLG_WAITPLAYERS_RACESUM_enumTrackSX,
            DLG_WAITPLAYERS_RACESUM_enumTrackSY,
            DLG_WAITPLAYERS_RACESUM_enumTrackSE, STR_UI_MAP },
          { DLG_WAITPLAYERS_RACESUM_enumNLapsX0,
            DLG_WAITPLAYERS_RACESUM_enumNLapsY0,
            DLG_WAITPLAYERS_RACESUM_enumNLapsSX,
            DLG_WAITPLAYERS_RACESUM_enumNLapsSY,
            DLG_WAITPLAYERS_RACESUM_enumNLapsSE, STR_UI_N_LAPS_SHORT } },
        /* MM_L_MAPINFO -- dlgMAPINFO's own two, which the quick-race page
           already draws. The track picker is the host's; the screenshot picker
           is anybody's. */
        { { DLG_MAPINFO_shotTrackEnumX0, DLG_MAPINFO_shotTrackEnumY0,
            DLG_MAPINFO_shotTrackEnumSX, DLG_MAPINFO_shotTrackEnumSY, 0.f, "" },
          { DLG_MAPINFO_enumShotX0, DLG_MAPINFO_enumShotY0,
            DLG_MAPINFO_enumShotSX, DLG_MAPINFO_enumShotSY, 0.f, "" } },
        /* MM_L_CARSETUP -- your own car and its three parts */
        { { DLG_WAITPLAYERS_CARSETUP_enumCarX0,
            DLG_WAITPLAYERS_CARSETUP_enumCarY0,
            DLG_WAITPLAYERS_CARSETUP_enumCarSX,
            DLG_WAITPLAYERS_CARSETUP_enumCarSY,
            DLG_WAITPLAYERS_CARSETUP_enumCarSE, "" },
          { DLG_WAITPLAYERS_CARSETUP_enumBoostX0,
            DLG_WAITPLAYERS_CARSETUP_enumBoostY0,
            DLG_WAITPLAYERS_CARSETUP_enumBoostSX,
            DLG_WAITPLAYERS_CARSETUP_enumBoostSY,
            DLG_WAITPLAYERS_CARSETUP_enumBoostSE, STR_UI_BOOSTER_LBL },
          { DLG_WAITPLAYERS_CARSETUP_enumResonX0,
            DLG_WAITPLAYERS_CARSETUP_enumResonY0,
            DLG_WAITPLAYERS_CARSETUP_enumResonSX,
            DLG_WAITPLAYERS_CARSETUP_enumResonSY,
            DLG_WAITPLAYERS_CARSETUP_enumResonSE, STR_UI_ENGINE_LBL },
          { DLG_WAITPLAYERS_CARSETUP_enumTiresX0,
            DLG_WAITPLAYERS_CARSETUP_enumTiresY0,
            DLG_WAITPLAYERS_CARSETUP_enumTiresSX,
            DLG_WAITPLAYERS_CARSETUP_enumTiresSY,
            DLG_WAITPLAYERS_CARSETUP_enumTiresSE, STR_UI_TIRES_LBL } },
        /* MM_L_CARRESTR -- the host's three, labelled with the car names the
           exe's own records point at */
        { { DLG_WAITPLAYERS_CARRESTR_enumCar1X0,
            DLG_WAITPLAYERS_CARRESTR_enumCar1Y0,
            DLG_WAITPLAYERS_CARRESTR_enumCar1SX,
            DLG_WAITPLAYERS_CARRESTR_enumCar1SY,
            DLG_WAITPLAYERS_CARRESTR_enumCar1SE, "" },
          { DLG_WAITPLAYERS_CARRESTR_enumCar2X0,
            DLG_WAITPLAYERS_CARRESTR_enumCar2Y0,
            DLG_WAITPLAYERS_CARRESTR_enumCar2SX,
            DLG_WAITPLAYERS_CARRESTR_enumCar2SY,
            DLG_WAITPLAYERS_CARRESTR_enumCar2SE, "" },
          { DLG_WAITPLAYERS_CARRESTR_enumCar3X0,
            DLG_WAITPLAYERS_CARRESTR_enumCar3Y0,
            DLG_WAITPLAYERS_CARRESTR_enumCar3SX,
            DLG_WAITPLAYERS_CARRESTR_enumCar3SY,
            DLG_WAITPLAYERS_CARRESTR_enumCar3SE, "" } }
    };
    if (view < 0 || view >= MM_L_N_VIEW || i < 0 || i >= MM_L_NCTRL[view])
        return 0;
    return &V[view][i];
}

/* Which of the eight rows a lobby stop's bar is, and whether it is orange. */
static int mm_l_bar_row(const mainmenu_t *m, int stop, int *orange)
{
    if (orange) *orange = 0;
    if (stop >= MM_LB_TAB && stop < MM_LB_TAB + MM_L_N_VIEW)
        return stop - MM_LB_TAB;            /* rows 0..3 */
    if (stop == MM_LB_ROW) {
        /* ROW 7, and what it does depends on the view: Kick player off on Race
           summary (grey on the game's own shot of it, because the only row in
           the table was the player's own) and Next skin on Car setup (orange).
           The other two views have no row-7 button at all. */
        if (m->lview == MM_L_RACESUM) return 7;
        if (m->lview == MM_L_CARSETUP) { if (orange) *orange = 1; return 7; }
        return -1;
    }
    return -1;
}

static const char *mm_l_row_name(const mainmenu_t *m)
{
    if (m->lview == MM_L_RACESUM)  return STR_UI_KICK_PLAYER;
    if (m->lview == MM_L_CARSETUP) return STR_UI_NEXT_SKIN;
    return "";
}

static int mm_l_ring(int view, int *out)
{
    int n = 0, i;
    if (view < 0 || view >= MM_L_N_VIEW)
        return 0;
    for (i = 0; i < MM_L_NCTRL[view]; i++)
        out[n++] = MM_LB_C0 + i;
    for (i = 0; i < MM_L_N_VIEW; i++)
        out[n++] = MM_LB_TAB + i;
    if (view == MM_L_RACESUM || view == MM_L_CARSETUP)
        out[n++] = MM_LB_ROW;
    out[n++] = MM_LB_RACE;
    out[n++] = MM_LB_BACK;
    return n;
}

int mainmenu_l_nfocus(int view)
{
    int ring[MM_LB_N_FOCUS];
    return mm_l_ring(view, ring);
}

int mainmenu_l_stop(int view, int i)
{
    int ring[MM_LB_N_FOCUS];
    const int n = mm_l_ring(view, ring);
    if (n <= 0)
        return -1;
    while (i < 0) i += n;
    return ring[i % n];
}

static int mm_l_index(int view, int stop)
{
    int ring[MM_LB_N_FOCUS];
    const int n = mm_l_ring(view, ring);
    int i;
    for (i = 0; i < n; i++)
        if (ring[i] == stop) return i;
    return 0;
}

/* WHETHER A LOBBY STOP CAN ACT, and on this screen that is mostly the question
   "are we the host". The two settings enums and the three restrictions are the
   host's alone -- a client that could change the map would be a client whose
   screen disagrees with the race it is about to load -- and Kick is the host's
   too. Everything about YOUR OWN car is yours either way. */
int mainmenu_l_live(const mainmenu_t *m, int stop)
{
    if (!m || m->page != MM_PAGE_LOBBY)
        return 0;
    switch (stop) {
    case MM_LB_C0:
    case MM_LB_C1:
    case MM_LB_C2:
    case MM_LB_C3: {
        const int i = stop - MM_LB_C0;
        if (i >= MM_L_NCTRL[m->lview])
            return 0;
        if (m->lview == MM_L_RACESUM)  return net_is_host();
        if (m->lview == MM_L_CARRESTR) return net_is_host();
        /* Map and info: the track picker is the host's, the screenshot picker
           is anybody's. Car setup: all four are your own car. */
        if (m->lview == MM_L_MAPINFO)  return i == MM_Q_MI_SHOT || net_is_host();
        return 1;
    }
    case MM_LB_ROW:
        if (m->lview == MM_L_RACESUM)
            return net_is_host() && net_n_peers() > 1;
        if (m->lview == MM_L_CARSETUP)
            return m->gskins > 1;
        return 0;
    /* THE GREEN BUTTON MEANS TWO DIFFERENT THINGS and is live for both: on the
       host it starts the race and waits for everyone to be ready; on a client
       it is the ready flag, which is the only thing a client has to say. */
    case MM_LB_RACE:
        return net_is_host() ? net_can_start() : 1;
    default:
        return 1;
    }
}

/* The table's own geometry, in design pixels. `row' -1 is the header. */
static void mm_l_table(const mmframe *f, int row, float *y, float *h)
{
    const float head = DLG_WAITPLAYERS_RACESUM_tableSY
                       * DLG_WAITPLAYERS_RACESUM_tableHeaderHeight;
    const float item = DLG_WAITPLAYERS_RACESUM_tableSY
                       * DLG_WAITPLAYERS_RACESUM_tableItemHeight;
    (void)f;
    if (row < 0) {
        *y = DLG_WAITPLAYERS_RACESUM_tableY0;
        *h = head;
    } else {
        *y = DLG_WAITPLAYERS_RACESUM_tableY0 + head + item * (float)row;
        *h = item;
    }
}

/* Column `c' (0..5) in design pixels: its left edge and its width. The five
   shipped widths, and the last column takes the remainder the file never
   names -- dlgFINISH's own rule. */
static void mm_l_col(int c, float *x, float *w)
{
    static const float W[6] = {
        DLG_WAITPLAYERS_RACESUM_tableWidth1, DLG_WAITPLAYERS_RACESUM_tableWidth2,
        DLG_WAITPLAYERS_RACESUM_tableWidth3, DLG_WAITPLAYERS_RACESUM_tableWidth4,
        DLG_WAITPLAYERS_RACESUM_tableWidth5, 0.f
    };
    float used = 0.f, i;
    int k;
    for (k = 0; k < 5; k++) used += W[k];
    *x = DLG_WAITPLAYERS_RACESUM_tableX0;
    for (k = 0; k < c && k < 6; k++)
        *x += DLG_WAITPLAYERS_RACESUM_tableSX * W[k];
    i = (c == 5) ? (1.f - used) : W[c];
    *w = DLG_WAITPLAYERS_RACESUM_tableSX * i;
}

static void mm_l_arrows(const mainmenu_t *m, const mmframe *f, int i,
                        float *bx, float *fx, float *ay, float *sz)
{
    const mm_enum *e = mm_l_ctrl(m->lview, i);
    *sz = MM_Q_ARROW * f->us;
    if (!e) { *bx = *fx = *ay = 0.f; return; }
    *ay = py(f, e->y0 + e->sy * 0.5f) - *sz * 0.5f;
    *bx = px(f, e->x0 + e->sx * e->se);
    *fx = px(f, e->x0 + e->sx) - *sz;
}

int mainmenu_l_row_at(const mainmenu_t *m, int screen_w, int screen_h,
                      float x, float y, int *left, int *row)
{
    const mmframe f = mm_frame(screen_w, screen_h);
    float bx, by, bw, bh;
    int i;

    if (left) *left = 0;
    if (row) *row = -1;
    if (!m || m->page != MM_PAGE_LOBBY)
        return -1;
    for (i = 0; i < MM_L_NCTRL[m->lview]; i++) {
        float ax, fx, ay, sz;
        mm_l_arrows(m, &f, i, &ax, &fx, &ay, &sz);
        if (touch_in(x, y, ax - sz * 0.25f, ay - sz * 0.25f,
                     sz * 1.5f, sz * 1.5f)) {
            if (left) *left = 1;
            return MM_LB_C0 + i;
        }
        if (touch_in(x, y, fx - sz * 0.25f, ay - sz * 0.25f,
                     sz * 1.5f, sz * 1.5f))
            return MM_LB_C0 + i;
    }
    for (i = 0; i < MM_LB_N_FOCUS; i++) {
        const int r = mm_l_bar_row(m, i, NULL);
        if (r < 0) continue;
        mm_bar_rect(&f, r, &bx, &by, &bw, &bh);
        if (touch_in(x, y, bx, by, bw, bh))
            return i;
    }
    mm_race_rect(&f, &bx, &by, &bw, &bh);
    if (touch_in(x, y, bx, by, bw, bh))
        return MM_LB_RACE;
    mm_quit_rect(&f, &bx, &by, &bw, &bh);
    if (touch_in(x, y, bx, by, bw, bh))
        return MM_LB_BACK;
    /* AND THE TABLE'S ROWS, which are the Kick button's aim. Only on Race
       summary, and only over a row the roster really has. */
    if (m->lview == MM_L_RACESUM && row) {
        for (i = 0; i < NET_MAX; i++) {
            float ty, th, rx, ry, rw, rh;
            const net_peer *q = net_peer_at(i);
            if (!q || !q->used) continue;
            mm_l_table(&f, i, &ty, &th);
            rx = px(&f, DLG_WAITPLAYERS_RACESUM_tableX0);
            ry = py(&f, ty);
            rw = px(&f, DLG_WAITPLAYERS_RACESUM_tableX0
                        + DLG_WAITPLAYERS_RACESUM_tableSX) - rx;
            rh = py(&f, ty + th) - ry;
            if (touch_in(x, y, rx, ry, rw, rh)) {
                *row = i;
                return -1;
            }
        }
    }
    return -1;
}

void mainmenu_open_lobby(mainmenu_t *m, int view)
{
    if (!m)
        return;
    m->page = MM_PAGE_LOBBY;
    m->lview = (view >= 0 && view < MM_L_N_VIEW) ? view : MM_L_RACESUM;
    m->lfocus = MM_LB_TAB + m->lview;
    m->larmed = -1;
    if (m->lsel < 0 || m->lsel >= NET_MAX)
        m->lsel = 0;
    /* THE LOBBY'S TRACK AND LAPS ARE THE HOST'S, so the page comes up on what
       the network says rather than on what this machine last chose -- otherwise
       a client's first frame shows its own carousel's track under the host's
       name for it. */
    {
        const net_settings *s = net_settings_now();
        if (s->track < N_TRACKS) m->track = s->track;
        if (s->laps > 0) m->laps = s->laps;
    }
}

/* ------------------------------------------------------------- the step */

/* Our own car and parts, as the network wants them. */
static void mm_l_push_me(mainmenu_t *m)
{
    unsigned char up[3];
    const player_t *p = player_cur();
    const int c = (m->car >= 0 && m->car < MM_N_CARS) ? m->car : 0;
    up[0] = (unsigned char)garage_level(p, GAR_BOOSTER, c);
    up[1] = (unsigned char)garage_level(p, GAR_ENGINE, c);
    up[2] = (unsigned char)garage_level(p, GAR_TIRES, c);
    /* CLAMPED BY THE HOST'S RESTRICTIONS. A car the host has disabled cannot be
       raced and a part above the ceiling cannot be worn, so the page shows what
       will actually turn up on the grid rather than what the profile owns. */
    {
        const int mx = net_max_upgrade(c);
        int i;
        if (mx >= 0)
            for (i = 0; i < 3; i++)
                if (up[i] > (unsigned char)mx) up[i] = (unsigned char)mx;
    }
    net_set_me(c, up, garage_skin(p, c));
}

/* One step of a lobby control. */
static void mm_l_move(mainmenu_t *m, int stop, int d)
{
    const int i = stop - MM_LB_C0;
    if (i < 0 || i >= MM_L_NCTRL[m->lview])
        return;
    if (!mainmenu_l_live(m, stop)) {
        m->cue = MM_CUE_DENY;
        return;
    }
    switch (m->lview) {
    case MM_L_RACESUM:
        if (i == 0) {
            m->track = (m->track + d + N_TRACKS) % N_TRACKS;
            net_set_track(m->track);
        } else {
            int k, at = 0;
            for (k = 0; k < MM_N_LAPS; k++)
                if (MM_LAPS[k] == m->laps) { at = k; break; }
            m->laps = MM_LAPS[(at + d + MM_N_LAPS) % MM_N_LAPS];
            net_set_laps(m->laps);
        }
        break;
    case MM_L_MAPINFO:
        if (i == MM_Q_MI_TRACK) {
            m->track = (m->track + d + N_TRACKS) % N_TRACKS;
            net_set_track(m->track);
        } else {
            m->shot = (m->shot + d + MM_N_SHOTS) % MM_N_SHOTS;
        }
        break;
    case MM_L_CARSETUP:
        if (i == 0) {
            /* THE CAR PICKER SKIPS WHAT THE HOST HAS DISABLED. Walking onto a
               car you cannot race and being told so on the Race button is a
               worse screen than a picker that only offers what is allowed. */
            int k = m->car;
            int tries;
            for (tries = 0; tries < MM_N_CARS; tries++) {
                k = (k + d + MM_N_CARS) % MM_N_CARS;
                if (net_car_allowed(k)) break;
            }
            m->car = k;
        } else {
            const int kind = i - 1;         /* 0 booster, 1 engine, 2 tyres */
            player_t *p = player_cur();
            const int mx = net_max_upgrade(m->car);
            int lv = garage_level(p, kind, m->car) + d;
            if (mx < 0) { m->cue = MM_CUE_DENY; return; }
            if (lv < 0) lv = 0;
            if (lv > mx) lv = mx;
            /* THE PARTS ARE THE PROFILE'S, and this page does not BUY them: the
               enum walks what the profile already owns, capped by the host's
               ceiling. A lobby that could fit a level 3 booster for nothing
               would be a shop with no prices. */
            if (p && lv > garage_level(p, kind, m->car))
                lv = garage_level(p, kind, m->car);
            if (p) p->car[m->car].up[kind] = (unsigned char)lv;
        }
        break;
    case MM_L_CARRESTR: {
        const net_settings *s = net_settings_now();
        int v = (int)s->restr[i] + d;
        if (v < 0) v = NET_RESTR_N - 1;
        if (v >= NET_RESTR_N) v = 0;
        net_set_restr(i, v);
        break;
    }
    default:
        return;
    }
    mm_l_push_me(m);
    m->cue = MM_CUE_ARROW;
}

static void mm_l_fire(mainmenu_t *m, int stop)
{
    if (stop >= MM_LB_TAB && stop < MM_LB_TAB + MM_L_N_VIEW) {
        const int v = stop - MM_LB_TAB;
        if (v != m->lview) {
            m->lview = v;
            m->lfocus = stop;
        }
        m->cue = MM_CUE_PRESS;
        return;
    }
    switch (stop) {
    case MM_LB_C0: case MM_LB_C1: case MM_LB_C2: case MM_LB_C3:
        mm_l_move(m, stop, +1);
        return;
    case MM_LB_ROW:
        if (!mainmenu_l_live(m, stop)) { m->cue = MM_CUE_DENY; return; }
        if (m->lview == MM_L_RACESUM) {
            const int r = net_kick(m->lsel);
            if (r == -2)
                mg_say(m, STR_UI_CANT_KICK_SELF);   /* 42907 */
            m->cue = r == 0 ? MM_CUE_PRESS : MM_CUE_DENY;
        } else {
            player_t *p = player_cur();
            garage_set_skin(p, m->car,
                            garage_next_skin(garage_skin(p, m->car),
                                             m->gskins));
            mm_l_push_me(m);
            m->action = MM_ACT_GARAGE;
            m->cue = MM_CUE_PRESS;
        }
        return;
    case MM_LB_RACE:
        if (net_is_host()) {
            if (!net_can_start()) { m->cue = MM_CUE_DENY; return; }
            net_start();
            m->action = MM_ACT_NET_RACE;
        } else {
            /* READY, and it TOGGLES: a player who is not ready any more has to
               be able to say so, and the table's Status column is what shows
               which they are. */
            const net_peer *me = net_peer_at(net_slot());
            net_set_ready(!(me && me->ready));
        }
        m->cue = MM_CUE_PRESS;
        return;
    case MM_LB_BACK:
        /* 42930, "Do you really want to disconnect?" -- leaving a game is not
           something to do by brushing a button, and the engine asks. */
        mg_ask(m, MG_ASK_DISCONNECT, STR_UI_DISCONNECT_ASK);
        m->cue = MM_CUE_PRESS;
        return;
    default:
        return;
    }
}

static void mm_step_lobby(mainmenu_t *m, unsigned int down,
                          const touch_state *tp, int screen_w, int screen_h)
{
    const int view = m->lview;
    const int n = mainmenu_l_nfocus(view);

    /* THE OTHER END CAN END THIS. A host that went away, a kick, or the game
       filling up while we asked -- all of them come back through net_error, and
       the page says so in the engine's own words and goes home. */
    if (net_mode_now() == NET_OFF) {
        const net_err e = net_error();
        mainmenu_open_multi(m);
        if (e == NET_ERR_KICKED)      mg_say(m, STR_UI_KICKED_OFF);
        else if (e == NET_ERR_TIMEOUT) mg_say(m, STR_UI_CONN_LOST);
        net_clear_error();
        return;
    }
    /* THE HOST PRESSED RACE. Everyone else finds out here. */
    if (net_take_start()) {
        const net_settings *s = net_settings_now();
        if (s->track < N_TRACKS) m->track = s->track;
        if (s->laps > 0) m->laps = s->laps;
        m->action = MM_ACT_NET_RACE;
        return;
    }
    /* AND THE HOST'S SETTINGS FOLLOW THE NETWORK on a client, every frame: the
       two enums are read-only there, so what they show has to be what arrived
       rather than what this machine last had. */
    if (!net_is_host()) {
        const net_settings *s = net_settings_now();
        if (s->track < N_TRACKS) m->track = s->track;
        if (s->laps > 0) m->laps = s->laps;
    }

    if (n <= 0)
        return;
    if (down & SCE_CTRL_DOWN) {
        m->lfocus = mainmenu_l_stop(view, mm_l_index(view, m->lfocus) + 1);
        m->cue = MM_CUE_FOCUS;
    }
    if (down & SCE_CTRL_UP) {
        m->lfocus = mainmenu_l_stop(view, mm_l_index(view, m->lfocus) - 1);
        m->cue = MM_CUE_FOCUS;
    }
    if (down & SCE_CTRL_LEFT)  mm_l_move(m, m->lfocus, -1);
    if (down & SCE_CTRL_RIGHT) mm_l_move(m, m->lfocus, +1);
    /* SQUARE walks the table, which is what the Kick button aims with. Its own
       stop would be a sixth thing in a ring the pad already has to walk. */
    if ((down & SCE_CTRL_SQUARE) && m->lview == MM_L_RACESUM) {
        int i;
        for (i = 1; i <= NET_MAX; i++) {
            const int k = (m->lsel + i) % NET_MAX;
            const net_peer *q = net_peer_at(k);
            if (q && q->used) { m->lsel = k; break; }
        }
        m->cue = MM_CUE_FOCUS;
    }
    if (down & (SCE_CTRL_CROSS | SCE_CTRL_START))
        mm_l_fire(m, m->lfocus);
    if (down & SCE_CTRL_CIRCLE) {
        mm_l_fire(m, MM_LB_BACK);
        return;
    }

    if (!tp)
        return;
    if (tp->pressed) {
        int left, row;
        m->larmed = mainmenu_l_row_at(m, screen_w, screen_h, tp->x, tp->y,
                                     &left, &row);
        if (row >= 0 && row != m->lsel) {
            m->lsel = row;
            m->cue = MM_CUE_FOCUS;
        }
        if (m->larmed >= 0 && m->lfocus != m->larmed) {
            m->lfocus = m->larmed;
            m->cue = MM_CUE_FOCUS;
        }
    }
    if (tp->released) {
        int left, row;
        const int at = mainmenu_l_row_at(m, screen_w, screen_h, tp->x, tp->y,
                                        &left, &row);
        if (at >= 0 && at == m->larmed) {
            if (at <= MM_LB_C3)
                mm_l_move(m, at, left ? -1 : +1);
            else
                mm_l_fire(m, at);
        }
        m->larmed = -1;
    }
}

/* --------------------------------------------------- the front page's step */

static void mm_m_fire(mainmenu_t *m, int stop)
{
    const player_t *p = player_cur();
    unsigned char up[3];
    const int c = (m->car >= 0 && m->car < MM_N_CARS) ? m->car : 0;

    up[0] = (unsigned char)garage_level(p, GAR_BOOSTER, c);
    up[1] = (unsigned char)garage_level(p, GAR_ENGINE, c);
    up[2] = (unsigned char)garage_level(p, GAR_TIRES, c);

    switch (stop) {
    case MM_M_CREATE:
        if (!net_host(p ? p->name : STR_UI_DEFAULT_NAME, c, up,
                      garage_skin(p, c), p ? p->face : 0)) {
            mg_say(m, STR_UI_CONNECT_ERR);
            m->cue = MM_CUE_DENY;
            return;
        }
        /* THE HOST'S OWN CAROUSEL TRACK is the game's, which is why the page
           has a carousel at all. */
        net_set_track(m->track);
        net_set_laps(m->laps);
        mainmenu_open_lobby(m, MM_L_RACESUM);
        m->cue = MM_CUE_PRESS;
        return;
    case MM_M_JOIN:
        if (!net_browse()) {
            mg_say(m, STR_UI_CONNECT_ERR);
            m->cue = MM_CUE_DENY;
            return;
        }
        m->modal = MM_MODAL_SERVERS;
        m->srvsel = 0;
        m->marmed = -1;
        m->cue = MM_CUE_PRESS;
        return;
    case MM_M_RACE:
        m->cue = MM_CUE_DENY;               /* no game yet -- see mainmenu_m_live */
        return;
    case MM_M_BACK:
        net_leave();
        m->page = MM_PAGE_MAIN;
        m->focus = MM_MULTIPLAYER;
        m->cue = MM_CUE_PRESS;
        return;
    default:
        return;
    }
}

static void mm_step_multi(mainmenu_t *m, unsigned int down,
                          const touch_state *tp, int screen_w, int screen_h)
{
    /* A JOIN THAT SUCCEEDED lands us in somebody's lobby, from whichever page
       we were looking at. */
    if (net_mode_now() == NET_JOINED) {
        m->modal = MM_MODAL_NONE;
        mainmenu_open_lobby(m, MM_L_RACESUM);
        return;
    }
    if (net_error() != NET_ERR_NONE && m->modal != MM_MODAL_SAY) {
        const net_err e = net_error();
        mg_say(m, e == NET_ERR_FULL ? STR_UI_CONNECT_ERR
               : (e == NET_ERR_KICKED ? STR_UI_KICKED_OFF
                                      : STR_UI_CONNECT_ERR));
        net_clear_error();
    }

    if (down & SCE_CTRL_DOWN) {
        m->mfocus_multi = (m->mfocus_multi + 1) % MM_M_N_FOCUS;
        m->cue = MM_CUE_FOCUS;
    }
    if (down & SCE_CTRL_UP) {
        m->mfocus_multi = (m->mfocus_multi + MM_M_N_FOCUS - 1) % MM_M_N_FOCUS;
        m->cue = MM_CUE_FOCUS;
    }
    /* LEFT and RIGHT walk the carousel, exactly as on the main menu: the track
       is what this page is choosing, and a host takes it into the game. */
    if (down & SCE_CTRL_LEFT)  mm_move_track(m, -1);
    if (down & SCE_CTRL_RIGHT) mm_move_track(m, +1);
    if (down & (SCE_CTRL_CROSS | SCE_CTRL_START))
        mm_m_fire(m, m->mfocus_multi);
    if (down & SCE_CTRL_CIRCLE) {
        mm_m_fire(m, MM_M_BACK);
        return;
    }

    if (!tp)
        return;
    if (tp->pressed) {
        int slot;
        m->marmed_multi = mainmenu_m_row_at(m, screen_w, screen_h,
                                            tp->x, tp->y);
        if (m->marmed_multi >= 0 && m->mfocus_multi != m->marmed_multi) {
            m->mfocus_multi = m->marmed_multi;
            m->cue = MM_CUE_FOCUS;
        }
        slot = mainmenu_slot_at(m, screen_w, screen_h, tp->x, tp->y);
        if (slot >= 0 && slot != 2) {
            m->track = mm_slot_track(m, slot);
            m->cue = MM_CUE_ARROW;
        }
    }
    if (tp->released) {
        const int at = mainmenu_m_row_at(m, screen_w, screen_h, tp->x, tp->y);
        if (at >= 0 && at == m->marmed_multi)
            mm_m_fire(m, at);
        m->marmed_multi = -1;
    }
}

/* -------------------------------------------------------------- the draw */

static void mm_m_draw_bars(const mainmenu_t *m, const mmframe *f)
{
    static const char *const NAME[MM_M_N_FOCUS] = {
        STR_UI_CREATE_GAME, STR_UI_JOIN_GAME, "", ""
    };
    int i;
    for (i = 0; i < MM_M_N_FOCUS; i++) {
        float bx, by, bw, bh;
        const int row = mm_m_bar_row(i);
        const int lit = m->mfocus_multi == i;
        if (row < 0) continue;
        mm_draw_wedge(m, f, row);
        mm_bar_draw_rect(m, f, row, lit ? MM_SLIDE * MM_SETTLED : 0.f,
                         &bx, &by, &bw, &bh);
        if (m->tex.buttons) {
            const float v = lit ? MM_V_RED_F : MM_V_RED;
            ui_image(bx, by, bw, bh, m->tex.buttons,
                     0.f, v, 1.f, v + MM_V_CELL, 1.f, 1.f, 1.f, 1.f);
        } else {
            ui_rect(bx, by, bw, bh, 0.72f, 0.09f, 0.11f, 1.f);
        }
        mm_label(m, f, bx, by, bw, bh, NAME[i], 1.f, 1.f, 1.f);
    }
    /* AND THE ROW `Two players' WOULD BE IS LEFT COMPLETELY EMPTY -- no bar and
       NO TRAY. The tray was drawn there first, on the reasoning that the
       original's page has one; on screen it is a silver sliver with nothing on
       it, which reads as a bar that failed to draw rather than as a gap. The
       main menu's own wedges are only drawn under rows that exist. */
}

static void mm_draw_multi(const mainmenu_t *m, const mmframe *f)
{
    mm_m_draw_bars(m, f);
    /* THE CARD IS dlgPLRSCOMM's, and `scores_only' is the one difference --
       this page's Scores line is left-aligned and has no Cash beside it. */
    mp_draw_card_at(m, f, 1);
    mm_draw_carousel_at(m, f, 0);
    /* staticInfoHeader: the track's name over its own box's bottom edge, and
       then staticInfo's three lines at the small font's own pitch. */
    mm_q_text(m, f, 1, DLG_MULTIPLAYER_staticInfoHeaderX0,
              DLG_MULTIPLAYER_staticInfoHeaderY0, MM_TS_TRACK, 0, 1.f,
              STR_TRACK_NAME[m->track]);
    mg_rule(f, DLG_MULTIPLAYER_staticInfoHeaderX0,
            DLG_MULTIPLAYER_staticInfoHeaderY0,
            DLG_MULTIPLAYER_staticInfoHeaderSX,
            DLG_MULTIPLAYER_staticInfoHeaderSY);
    mg_lines(m, f, DLG_MULTIPLAYER_staticInfoX0, DLG_MULTIPLAYER_staticInfoY0,
             0, 1.f, STR_TRACK_MENU[m->track]);
}

/* THE MESSAGE PANEL -- the `chat' control, carrying net.c's own log. Every line
 * in it is one of the engine's 20200..20216.
 *
 * THE PANEL AND ITS TEXT ARE ONE GROUP, mapped ONCE. The first version put the
 * plate through `mm_box' and then the lines through `mm_q_text', which maps with
 * px()/py() -- two different rules for one box, so on a 960x544 panel the text
 * started above the plate and ran out of the bottom of it. Everything here is
 * placed off the plate's own screen rectangle instead.
 *
 * AND IT DRAWS AS MANY LINES AS FIT AND NO MORE, newest at the bottom. The log
 * holds NET_LOG_LINES (six) because a history is useful in the app log; the
 * panel is 90 design pixels, which is three of the small font's 23 -- and six
 * were drawn into it, so half of them fell outside the frame. The count comes
 * off the panel's own height rather than being a second constant to keep in
 * step with it.
 */
#define MM_L_LOG_PAD 8.f

static void mm_l_draw_log(const mainmenu_t *m, const mmframe *f)
{
    const sfont sf = sf_small(m->tex.font_small);
    const float sc = f->us * MM_TS_INFO;
    const float pad = MM_L_LOG_PAD * f->us;
    const float lh = MM_L_LINE_H * f->us;
    float x, y, w, h;
    int i, fit, first, n = net_n_log();

    mm_box(f, DLG_WAITPLAYERS_RACESUM_chatX0, DLG_WAITPLAYERS_RACESUM_chatY0,
           DLG_WAITPLAYERS_RACESUM_chatSX, DLG_WAITPLAYERS_RACESUM_chatSY,
           &x, &y, &w, &h);
    /* The dark plate and the silver rim the game's own panel has: the rim is
       messagebox_empty nine-sliced, which is what dlgMAPINFO's map window and
       every modal in this file already use. */
    ui_rect(x, y, w, h, 0.10f, 0.07f, 0.05f, 0.55f);
    if (m->tex.panel)
        mm_frame9(f, x, y, w, h, m->tex.panel);

    fit = (int)((h - pad * 2.f) / lh);
    if (fit < 1) fit = 1;
    first = n > fit ? n - fit : 0;
    for (i = first; i < n; i++) {
        const char *line = net_log_at(i);
        const float ty = y + pad + (float)(i - first) * lh;
        if (sf.tex)
            sf_text_shadowed(&sf, x + pad, ty, sc, 1.f, 1.f, 1.f, 1.f, line);
        else
            ui_text(x + pad, ty, sc, 1.f, 1.f, 1.f, 1.f, line);
    }
}

/* The roster table. */
static void mm_l_draw_table(const mainmenu_t *m, const mmframe *f)
{
    static const char *const HEAD[6] = {
        STR_UI_COL_PLAYER, STR_UI_COL_CAR, STR_UI_COL_BOOST,
        STR_UI_COL_ENGINE, STR_UI_COL_TIRES, STR_UI_COL_STATUS
    };
    float ty, th;
    int c, i;

    /* the header, and its rule at the header band's own bottom */
    mm_l_table(f, -1, &ty, &th);
    for (c = 0; c < 6; c++) {
        float cx, cw;
        mm_l_col(c, &cx, &cw);
        if (c == 0)
            mm_q_text(m, f, 0, MM_L_NAME_X, ty + 6.f, MM_TS_INFO, 0, 1.f,
                      HEAD[c]);
        else
            mm_l_text_c(m, f, cx + cw * 0.5f, ty + 6.f, 1.f, HEAD[c]);
    }
    /* the header's rule, from the name column to the table's right edge --
       measured at y 196 running x 114..551 against tableY0 + header = 199 and
       tableX0 + SX = 552 */
    mm_rule_at(f, px(f, MM_L_NAME_X), py(f, ty + th),
               (DLG_WAITPLAYERS_RACESUM_tableX0
                + DLG_WAITPLAYERS_RACESUM_tableSX - MM_L_NAME_X) * f->us);

    for (i = 0; i < NET_MAX; i++) {
        const net_peer *q = net_peer_at(i);
        char line[32];
        float alpha = 1.f;
        if (!q || !q->used)
            continue;
        mm_l_table(f, i, &ty, &th);
        /* THE ROW THE KICK BUTTON IS AIMED AT stands out, which is the only way
           a table with no cursor of its own can say which row a button will
           take. It is also what the game's own shot has: its one row is drawn
           lit, with a rule under the name. */
        if (i == m->lsel && net_is_host()) {
            float rx = px(f, DLG_WAITPLAYERS_RACESUM_tableX0);
            float rw = px(f, DLG_WAITPLAYERS_RACESUM_tableX0
                             + DLG_WAITPLAYERS_RACESUM_tableSX) - rx;
            ui_rect(rx, py(f, ty), rw, py(f, ty + th) - py(f, ty),
                    1.f, 1.f, 1.f, 0.10f);
        }
        if (i != net_slot())
            alpha = 0.88f;
        /* THE PAINT SWATCH: cell (car, skin) of skin_ik_vse's 4 x 4 grid. */
        {
            float sx, sy, sw, sh;
            const float u0 = (float)(q->skin & 3) * MM_L_SWATCH_CELL;
            const float v0 = (float)(q->car % 3) * MM_L_SWATCH_CELL;
            mm_box(f, DLG_WAITPLAYERS_RACESUM_tableX0
                      + DLG_WAITPLAYERS_RACESUM_shiftDrawSkinX,
                   ty + (th - MM_L_SWATCH) * 0.5f,
                   MM_L_SWATCH, MM_L_SWATCH, &sx, &sy, &sw, &sh);
            if (m->tex.skinicons)
                ui_image(sx, sy, sw, sh, m->tex.skinicons,
                         u0, v0, u0 + MM_L_SWATCH_CELL,
                         v0 + MM_L_SWATCH_CELL, 1.f, 1.f, 1.f, 1.f);
            else
                ui_rect(sx, sy, sw, sh, 0.8f, 0.7f, 0.2f, 1.f);
        }
        mm_q_text(m, f, 0, MM_L_NAME_X, ty + 6.f, MM_TS_INFO, 0, alpha,
                  q->name);
        for (c = 1; c < 6; c++) {
            float cx, cw;
            mm_l_col(c, &cx, &cw);
            switch (c) {
            case 1:
                snprintf(line, sizeof line, "%s",
                         STR_CAR_NAME[q->car < STR_N_CARS ? q->car : 0]);
                break;
            case 2: case 3: case 4:
                snprintf(line, sizeof line, "%d", (int)q->up[c - 2]);
                break;
            default:
                snprintf(line, sizeof line, "%s",
                         q->ready ? STR_UI_READY : STR_UI_NOT_READY);
                break;
            }
            mm_l_text_c(m, f, cx + cw * 0.5f, ty + 6.f, alpha, line);
        }
    }
}

static void mm_l_draw_tabs(const mainmenu_t *m, const mmframe *f)
{
    int i;
    for (i = 0; i < MM_LB_N_FOCUS; i++) {
        float bx, by, bw, bh, slide = 0.f;
        int orange = 0;
        const int row = mm_l_bar_row(m, i, &orange);
        const int tab = i >= MM_LB_TAB && i < MM_LB_TAB + MM_L_N_VIEW;
        const int here = tab && (i - MM_LB_TAB) == m->lview;
        const int lit = m->lfocus == i;
        const int live = tab ? 1 : mainmenu_l_live(m, i);

        if (row < 0) continue;
        if (here || lit) slide = MM_SLIDE * MM_SETTLED;
        mm_draw_wedge(m, f, row);
        mm_bar_draw_rect(m, f, row, slide, &bx, &by, &bw, &bh);
        if (tab && m->tex.radio) {
            const float v = here ? MM_V_RAD_ON : MM_V_RAD;
            ui_image(bx, by, bw, bh, m->tex.radio,
                     0.f, v, 1.f, v + MM_V_RAD_CELL, 1.f, 1.f, 1.f, 1.f);
        } else if (m->tex.buttons) {
            const float v = !live ? MM_V_GREY_DOT
                                  : (orange ? (lit ? MM_V_ORANGE_F : MM_V_ORANGE)
                                            : (lit ? MM_V_RED_F : MM_V_RED));
            ui_image(bx, by, bw, bh, m->tex.buttons,
                     0.f, v, 1.f, v + MM_V_CELL, 1.f, 1.f, 1.f, 1.f);
        } else {
            ui_rect(bx, by, bw, bh, live ? 0.72f : 0.45f,
                    live ? 0.09f : 0.45f, live ? 0.11f : 0.47f, 1.f);
        }
        mm_label(m, f, bx, by, bw, bh,
                 tab ? MM_L_TAB_NAME[i - MM_LB_TAB] : mm_l_row_name(m),
                 live ? 1.f : 0.82f, live ? 1.f : 0.82f, live ? 1.f : 0.84f);
    }
}

/* The value a lobby control shows between its arrows. */
static void mm_l_value(const mainmenu_t *m, int i, char *out, int n)
{
    const net_settings *s = net_settings_now();
    const player_t *p = player_cur();
    out[0] = 0;
    switch (m->lview) {
    case MM_L_RACESUM:
        if (i == 0) snprintf(out, n, "%s", STR_TRACK_NAME[m->track]);
        else        snprintf(out, n, "%d", m->laps);
        return;
    case MM_L_MAPINFO:
        if (i == MM_Q_MI_TRACK) snprintf(out, n, "%s", STR_TRACK_NAME[m->track]);
        return;
    case MM_L_CARSETUP:
        if (i == 0)
            snprintf(out, n, "%s", STR_CAR_NAME[m->car]);
        else
            snprintf(out, n, "%s",
                     garage_part_name(i - 1, m->car,
                                      garage_level(p, i - 1, m->car)));
        return;
    case MM_L_CARRESTR:
        if (i >= 0 && i < 3)
            snprintf(out, n, "%s", MM_L_RESTR_NAME[s->restr[i] < NET_RESTR_N
                                                   ? s->restr[i] : 0]);
        return;
    default:
        return;
    }
}

static void mm_l_draw_ctrls(const mainmenu_t *m, const mmframe *f)
{
    char line[64];
    int i;
    for (i = 0; i < MM_L_NCTRL[m->lview]; i++) {
        const mm_enum *e = mm_l_ctrl(m->lview, i);
        const int live = mainmenu_l_live(m, MM_LB_C0 + i);
        const int lit = m->lfocus == MM_LB_C0 + i;
        float ax, fx, ay, sz;
        const char *lbl = e->label;
        if (!e) continue;
        mm_l_arrows(m, f, i, &ax, &fx, &ay, &sz);
        /* THE CAR RESTRICTIONS' LABELS ARE THE CAR NAMES, which the exe's own
           records carry as string ids (40100/40104/40108) rather than as text
           in the dialog. */
        if (m->lview == MM_L_CARRESTR)
            lbl = STR_CAR_NAME[i];
        if (lbl && lbl[0]) {
            mm_arrow(m, px(f, e->x0 - MM_Q_BULLET_GAP),
                     py(f, e->y0 + e->sy * 0.5f) - MM_Q_BULLET * f->us * 0.5f,
                     MM_Q_BULLET * f->us, live ? (lit ? 1 : 0) : 2, 0);
            mm_q_text(m, f, 0, e->x0, e->y0, MM_TS_LABEL, 0,
                      live ? 1.f : 0.82f, lbl);
            mm_rule_at(f, px(f, e->x0), py(f, e->y0 + 24.f),
                       (e->sx * e->se - 30.f) * f->us);
        }
        /* A CONTROL THE HOST OWNS IS GREY ON A CLIENT, in the artists' own
           disabled arrow cell -- which is what the read-only enum on the
           Garage's part page already does. */
        mm_arrow(m, ax, ay, sz, live ? (lit ? 1 : 0) : 2, 0);
        mm_arrow(m, fx, ay, sz, live ? (lit ? 1 : 0) : 2, 1);
        mm_l_value(m, i, line, sizeof line);
        if (line[0])
            mg_value(m, f, ax, sz, fx, py(f, e->y0 + e->sy * 0.5f),
                     live ? 1.f : 0.82f, line);
    }
}

static void mm_draw_lobby(const mainmenu_t *m, const mmframe *f)
{
    mm_l_draw_tabs(m, f);
    switch (m->lview) {
    case MM_L_RACESUM:
        mm_l_draw_table(m, f);
        break;
    case MM_L_MAPINFO:
        /* dlgMAPINFO ITSELF, which the exe says is this tab: its own control
           table carries `chat' and `editChat'. One page, two screens. */
        mm_draw_mapinfo(m, f);
        break;
    case MM_L_CARSETUP:
        /* staticCarName -- dlgCARSCOMM's `Car' heading with its own rule, which
           is on the game's own shot of this page at x 115 even though
           dlgWAITPLAYERS_CARSETUP's `enumCar' has SE 0 and carries no label of
           its own. The furniture is CARSCOMM's on this page too. */
        mm_q_text(m, f, 0, DLG_CARSCOMM_staticCarNameX0,
                  DLG_CARSCOMM_staticCarNameY0, MM_TS_LABEL, 0, 1.f,
                  STR_UI_CAR);
        mg_rule(f, DLG_CARSCOMM_staticCarNameX0,
                DLG_CARSCOMM_staticCarNameY0,
                DLG_CARSCOMM_staticCarNameSX,
                DLG_CARSCOMM_staticCarNameSY);
        /* animCarPreview -- dlgCARSCOMM's, the same viewport the Garage uses. */
        {
            float x, y, w, h;
            mm_box(f, DLG_CARSCOMM_animCarPreviewX0,
                   DLG_CARSCOMM_animCarPreviewY0,
                   DLG_CARSCOMM_animCarPreviewSX,
                   DLG_CARSCOMM_animCarPreviewSY, &x, &y, &w, &h);
            if (m->car_draw) {
                ui_end();
                m->car_draw(m->car_ctx, x, y, w, h);
                ui_begin((int)f->w, (int)f->h);
            }
        }
        break;
    case MM_L_CARRESTR:
        /* staticHeader1 and staticHeader2, each with its own rule. */
        mm_q_text(m, f, 0, DLG_WAITPLAYERS_CARRESTR_staticHeader1X0,
                  DLG_WAITPLAYERS_CARRESTR_staticHeader1Y0, MM_TS_LABEL, 0,
                  1.f, STR_UI_CAR_TYPE);
        mg_rule(f, DLG_WAITPLAYERS_CARRESTR_staticHeader1X0,
                DLG_WAITPLAYERS_CARRESTR_staticHeader1Y0,
                DLG_WAITPLAYERS_CARRESTR_staticHeader1SX,
                DLG_WAITPLAYERS_CARRESTR_staticHeader1SY);
        mm_q_text(m, f, 0, DLG_WAITPLAYERS_CARRESTR_staticHeader2X0,
                  DLG_WAITPLAYERS_CARRESTR_staticHeader2Y0, MM_TS_LABEL, 0,
                  1.f, STR_UI_CAR_ENABLED);
        break;
    default:
        break;
    }
    /* THE MESSAGE PANEL IS ON EVERY VIEW, which is what all four of the game's
       own lobby screenshots have -- and on Map and info the panel is that
       dialog's own `chat' control at the same rectangle. */
    mm_l_draw_log(m, f);
    mm_l_draw_ctrls(m, f);
}

/* ============================================ dlgPLRSCOMM -- Select player
 *
 * See mainmenu.h for what is on this page and where each rectangle came from.
 * The four in `Settings/dlgPLRSCOMM.ini` are used as the artists authored them;
 * everything else is read off the game's own 800x600 screenshot of this screen,
 * the same way the main menu's own constants were:
 *
 *   the portrait's two arrows   x 79..100 and 231..252, y 194..214
 *   the name's ink              y 89..108, with its rule at 110
 *   Rank / Current car / Play time   y 119, 142, 165 -- a flat 23 pitch
 *   the second rule             y 184, and Scores/Cash under it at 191
 *   the table's rules           x 90..478, at y 263 and 509
 *   the column headers' ink     y 239..255
 *   row 0's ink                 y 274, its underline at 292, and the second
 *                               row's underline at 322 -- so the pitch is 30
 *                               and seven rows fit between the two rules
 *   the scroll bar              x 518..541, y 280..508
 *   the four bars               rows 0, 1, 6 and 7 of the eight the main menu
 *                               already measures: Create player's centre lands
 *                               on MM_CY[0] and Remove player's on MM_CY[1],
 *                               and the two orange ones on MM_CY[6] and [7]
 */
#define MP_FACE_X    99.f      /* dlgPLRSCOMM.ini shotFaceX0/Y0/SX/SY */
#define MP_FACE_Y    79.f
#define MP_FACE_W   130.f
#define MP_FACE_H   157.f
#define MP_ARROW_LX  79.f
#define MP_ARROW_RX 231.f
#define MP_ARROW_Y  194.f
#define MP_ARROW_SZ  22.f
#define MP_TEXT_X   240.f      /* staticName/Info/CashX0, all three */
#define MP_NAME_Y    89.f
#define MP_RULE1_Y  110.f
#define MP_INFO_Y   119.f
#define MP_INFO_LH   23.f
#define MP_RULE2_Y  184.f
#define MP_CASH_Y   191.f
#define MP_RULE_W   280.f      /* staticNameSX */

#define MP_LIST_L    90.f
#define MP_LIST_R   478.f
#define MP_HEAD_Y   239.f
#define MP_HEAD_RULE 263.f
#define MP_ROW_Y    274.f
#define MP_ROW_H     30.f
#define MP_ROW_RULE  18.f      /* the underline, below the row's ink top */
#define MP_FOOT_Y   509.f
#define MP_N_VIS      7
#define MP_MARK_X    91.f
#define MP_MARK_SZ   12.f
#define MP_COL_NAME 110.f
#define MP_BAR_X    518.f
#define MP_BAR_Y    280.f
#define MP_BAR_W     24.f
#define MP_BAR_H    228.f

/* Score, Cash and Time are CENTRED on their headings -- measured: the heading
   "Score" runs 251..290 and the value "0" 268..274, both about 270.5, and the
   same holds for the other two. */
static const float MP_COL_CX[3] = { 270.5f, 371.5f, 473.f };

/* Which of the eight bars each of the page's four is. */
#define MP_ROW_CREATE 0
#define MP_ROW_REMOVE 1
#define MP_ROW_SORT   6
#define MP_ROW_FACE   7

/* THE MODALS. The two-button panel is measured off the game's own "Do you want
   to remove current player?" shot -- the frame x 216..584, y 232..365, its Yes
   at x 254..387 and No at 414..546, both y 312..333, so the pair is 133 x 22 on
   centres 80 either side of 400 and the single Ok of "Can't remove last player"
   is one of them on the centre line.

   THE CREATE PANEL IS TALLER BECAUSE IT CARRIES A KEYBOARD, which the original
   does not need. Same width, same centre, and the same button bar 168 below its
   own top. */
#define MP_DLG_X    216.f
#define MP_DLG_W    368.f
#define MP_DLG_Y    232.f
#define MP_DLG_H    133.f
#define MP_BTN_W    137.f
/* THE PAIR'S OWN RECTANGLE, FITTED to the game's `erase data' shot rather than
   read off a threshold. `messagebox' band 1 (red, white dot) was resampled over
   a grid of (width, left edge) and least-squares matched against the Yes plate
   on five of its rows, and band 2 (orange, dark dot) against No: the two land at
   centres 322.4 and 481.4, both 137.5 px wide -- so the pair is 137 on centres
   80 either side of the dialog's own 400, and the cell's 28 opaque rows of 32
   measure 31 on screen, which puts the plate at 36 and its top at 309.
   The 133 x 22 this carried came off the "strong red" extent alone, which
   misses a pill's dark ends at both edges. */
#define MP_BTN_H     36.f
#define MP_BTN_DX    80.f
#define MP_DLG_CX   400.f
#define MP_SAY_BTN_DY 77.f
#define MP_CDLG_Y   196.f
#define MP_CDLG_H   208.f
#define MP_CBTN_DY  168.f
#define MP_KB_X     250.f
#define MP_KB_Y     248.f
#define MP_KEY_W     30.f
#define MP_KEY_H     26.f
#define MP_FIELD_Y  214.f      /* the prompt and the field, one line */
#define MP_FIELD_PAD 24.f      /* inside the panel's own left edge */
#define MP_FIELD_X  140.f      /* where the name starts, panel-relative */

/* The caret. A trailing underscore, because the atlas has one and a blinking
   bar would need a clock this drawer does not take. */
#define STR_UI_CARET "_"

/* The grid, row by row. 0..36 type themselves, 37 is a space, 38 rubs out and
   39 is the case toggle -- and that is the whole layout, so nothing else in
   this file has to know where a letter is. */
static const char MP_KB_CHARS[] = "abcdefghijklmnopqrstuvwxyz0123456789_";
#define MP_KEY_SPACE 37
#define MP_KEY_BACK  38
#define MP_KEY_SHIFT 39

char mainmenu_kb_char(const mainmenu_t *m, int k)
{
    char c;
    if (k < 0 || k >= MM_KB_KEYS)
        return 0;
    if (k == MP_KEY_SPACE)
        return ' ';
    if (k >= MP_KEY_BACK)
        return 0;
    c = MP_KB_CHARS[k];
    if (m && m->mshift && c >= 'a' && c <= 'z')
        c = (char)(c - 32);
    return c;
}

static const char *mp_key_label(const mainmenu_t *m, int k, char *buf)
{
    if (k == MP_KEY_BACK)  return "<-";
    if (k == MP_KEY_SHIFT) return m->mshift ? "AB" : "ab";
    if (k == MP_KEY_SPACE) return "__";
    buf[0] = mainmenu_kb_char(m, k);
    buf[1] = 0;
    return buf;
}

/* --------------------------------------------------------------- the view */

static int mp_name_cmp(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb)
            return ca - cb;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* Best first for the three numeric keys, A to Z for the fourth, and the name is
   the tie-break everywhere -- so the order is total and a redraw never shuffles
   two profiles that compare equal. */
static int mp_cmp(int ia, int ib, int sort)
{
    const player_t *a = player_at(ia), *b = player_at(ib);
    if (!a || !b)
        return 0;
    switch (sort) {
    case MM_SORT_SCORE:
        if (a->scores != b->scores) return b->scores - a->scores;
        break;
    case MM_SORT_CASH:
        if (a->cash != b->cash) return b->cash - a->cash;
        break;
    case MM_SORT_TIME:
        if (a->play_time != b->play_time)
            return a->play_time < b->play_time ? 1 : -1;
        break;
    default:
        break;
    }
    return mp_name_cmp(a->name, b->name);
}

void mainmenu_players_sync(mainmenu_t *m)
{
    const int n = player_count();
    const int cur = player_cur_index();
    int i, j;

    if (!m)
        return;
    m->pnview = n < PL_MAX ? n : PL_MAX;
    for (i = 0; i < m->pnview; i++)
        m->pview[i] = i;
    /* Insertion sort: twenty rows at most, and it is stable, so equal rows keep
       the order the directory handed them. */
    for (i = 1; i < m->pnview; i++) {
        const int v = m->pview[i];
        for (j = i; j > 0 && mp_cmp(m->pview[j - 1], v, m->psort) > 0; j--)
            m->pview[j] = m->pview[j - 1];
        m->pview[j] = v;
    }
    m->psel = 0;
    for (i = 0; i < m->pnview; i++)
        if (m->pview[i] == cur)
            m->psel = i;
    if (m->ptop > m->psel)
        m->ptop = m->psel;
    if (m->psel >= m->ptop + MP_N_VIS)
        m->ptop = m->psel - MP_N_VIS + 1;
    if (m->ptop < 0)
        m->ptop = 0;
}

/* The profile the cursor is on, or NULL when there is none at all. */
static player_t *mp_sel(const mainmenu_t *m)
{
    if (m->psel < 0 || m->psel >= m->pnview)
        return NULL;
    return player_at(m->pview[m->psel]);
}

/* Defined with the rest of the step half below; the page's opener needs it. */
static void mp_open_name(mainmenu_t *m);

void mainmenu_open_players(mainmenu_t *m, int with_create)
{
    if (!m)
        return;
    m->page = MM_PAGE_PLAYERS;
    m->pfocus = MM_P_LIST;
    m->parmed = -1;
    m->marmed = -1;
    mainmenu_players_sync(m);
    if (with_create)
        mp_open_name(m);
    else
        m->modal = MM_MODAL_NONE;
}

int mainmenu_p_live(const mainmenu_t *m, int stop)
{
    switch (stop) {
    case MM_P_LIST:
        return m->pnview > 0;
    case MM_P_FACE_L:
    case MM_P_FACE_R:
        return m->pnview > 0;
    case MM_P_REMOVE:
        /* Focusable with one profile in the roster, because pressing it is how
           the game says "Can't remove last player" -- a button that refuses out
           loud is not the same as a button that is not there. */
        return m->pnview > 0;
    case MM_P_FACE:
        return 0;               /* the chooser over Faces/, not built */
    case MM_P_CREATE:
    case MM_P_SORT:
    case MM_P_CONTINUE:
        return 1;
    case MM_P_RACE:
        return m->pnview > 0;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------- geometry */

static void mp_bar_row(int stop, int *row, int *orange)
{
    switch (stop) {
    case MM_P_CREATE: *row = MP_ROW_CREATE; *orange = 0; return;
    case MM_P_REMOVE: *row = MP_ROW_REMOVE; *orange = 0; return;
    case MM_P_SORT:   *row = MP_ROW_SORT;   *orange = 1; return;
    case MM_P_FACE:   *row = MP_ROW_FACE;   *orange = 1; return;
    default:          *row = -1;            *orange = 0; return;
    }
}

static void mp_arrow_rect(const mmframe *f, int right,
                          float *x, float *y, float *w, float *h)
{
    mm_gbox(f, MP_TEXT_X, right ? MP_ARROW_RX : MP_ARROW_LX, MP_ARROW_Y,
            MP_ARROW_SZ, MP_ARROW_SZ, x, y, w, h);
}

static void mp_list_rect(const mmframe *f, int row,
                         float *x, float *y, float *w, float *h)
{
    const float top = MP_ROW_Y + MP_ROW_H * row - 4.f;
    *x = px(f, MP_LIST_L);
    *y = py(f, top);
    *w = px(f, MP_LIST_R) - *x;
    *h = py(f, top + MP_ROW_H) - *y;
}

static void mp_modal_rect(const mainmenu_t *m, const mmframe *f,
                          float *x, float *y, float *w, float *h)
{
    const int big = m->modal == MM_MODAL_CREATE;
    mm_gbox(f, MP_DLG_CX, MP_DLG_X, big ? MP_CDLG_Y : MP_DLG_Y,
            MP_DLG_W, big ? MP_CDLG_H : MP_DLG_H, x, y, w, h);
}

/* One modal button. `i` is 0 for the left of a pair, 1 for the right; a modal
   with ONE button puts it on the centre line, which is where the game's own
   "Can't remove last player" has it. */
static void mp_btn_rect(const mainmenu_t *m, const mmframe *f, int i, int pair,
                        float *x, float *y, float *w, float *h)
{
    const int big = m->modal == MM_MODAL_CREATE;
    const float cx = pair ? MP_DLG_CX + (i ? MP_BTN_DX : -MP_BTN_DX)
                          : MP_DLG_CX;
    const float ty = (big ? MP_CDLG_Y + MP_CBTN_DY : MP_DLG_Y + MP_SAY_BTN_DY);
    mm_gbox(f, MP_DLG_CX, cx - MP_BTN_W * 0.5f, ty, MP_BTN_W, MP_BTN_H,
            x, y, w, h);
}

static void mp_key_rect(const mmframe *f, int k,
                        float *x, float *y, float *w, float *h)
{
    const int c = k % MM_KB_COLS, r = k / MM_KB_COLS;
    mm_gbox(f, MP_DLG_CX, MP_KB_X + MP_KEY_W * c, MP_KB_Y + MP_KEY_H * r,
            MP_KEY_W - 2.f, MP_KEY_H - 2.f, x, y, w, h);
}

static int mp_in(float px_, float py_, float x, float y, float w, float h)
{
    return px_ >= x && px_ < x + w && py_ >= y && py_ < y + h;
}

int mainmenu_p_stop_at(const mainmenu_t *m, int screen_w, int screen_h,
                       float tx, float ty, int *row)
{
    const mmframe f = mm_frame(screen_w, screen_h);
    float x, y, w, h;
    int i;

    if (row)
        *row = -1;
    if (!m)
        return -1;
    for (i = 0; i < MM_P_N_FOCUS; i++) {
        int bar, orange;
        mp_bar_row(i, &bar, &orange);
        if (bar >= 0) {
            mm_bar_rect(&f, bar, &x, &y, &w, &h);
            if (mp_in(tx, ty, x, y, w, h))
                return i;
        }
    }
    mp_arrow_rect(&f, 0, &x, &y, &w, &h);
    if (mp_in(tx, ty, x, y, w, h)) return MM_P_FACE_L;
    mp_arrow_rect(&f, 1, &x, &y, &w, &h);
    if (mp_in(tx, ty, x, y, w, h)) return MM_P_FACE_R;
    mm_race_rect(&f, &x, &y, &w, &h);
    if (mp_in(tx, ty, x, y, w, h)) return MM_P_RACE;
    mm_quit_rect(&f, &x, &y, &w, &h);
    if (mp_in(tx, ty, x, y, w, h)) return MM_P_CONTINUE;
    for (i = 0; i < MP_N_VIS && m->ptop + i < m->pnview; i++) {
        mp_list_rect(&f, i, &x, &y, &w, &h);
        if (mp_in(tx, ty, x, y, w, h)) {
            if (row)
                *row = m->ptop + i;
            return MM_P_LIST;
        }
    }
    return -1;
}

int mainmenu_modal_at(const mainmenu_t *m, int screen_w, int screen_h,
                      float tx, float ty)
{
    const mmframe f = mm_frame(screen_w, screen_h);
    const int pair = m && m->modal != MM_MODAL_SAY;
    float x, y, w, h;
    int i;

    if (!m || !m->modal || m->modal == MM_MODAL_IME)
        return -1;      /* the system dialog has the panel while it is up */
    if (m->modal == MM_MODAL_CREATE)
        for (i = 0; i < MM_KB_KEYS; i++) {
            mp_key_rect(&f, i, &x, &y, &w, &h);
            if (mp_in(tx, ty, x, y, w, h))
                return i;
        }
    mp_btn_rect(m, &f, 0, pair, &x, &y, &w, &h);
    if (mp_in(tx, ty, x, y, w, h)) return MM_MODAL_OK;
    if (pair) {
        mp_btn_rect(m, &f, 1, pair, &x, &y, &w, &h);
        if (mp_in(tx, ty, x, y, w, h)) return MM_MODAL_CANCEL;
    }
    return -1;
}

/* ---------------------------------------------------------------- a step */

/* THE NAME DIALOG. The machine's own keyboard where there is one, and the grid
   where `ime_open` said no -- which is every host build and any Vita whose
   common dialog would not configure. Both leave `mname` empty and the same
   commit runs off it. */
static void mp_open_name(mainmenu_t *m)
{
    m->mfocus = 0;
    m->mshift = 1;              /* a name starts with a capital */
    m->marmed = -1;
    m->mname[0] = 0;
    m->modal = (ime_available()
                && ime_open(STR_UI_PLAYER_NAME, "", PL_NAME - 1))
               ? MM_MODAL_IME : MM_MODAL_CREATE;
}

static void mp_say(mainmenu_t *m, const char *line)
{
    m->modal = MM_MODAL_SAY;
    m->msay = line;
    m->mfocus = MM_MODAL_OK;
    m->marmed = -1;
}

/* THE PORTRAIT ARROWS walk the nine and mark the profile DIRTY. A face is a
   property of the profile and not of this screen, so the choice has to survive
   the launch -- but the write is on the way OUT of the page and not on the
   press: a `.scp` is 150 KB and a held arrow would put one on the memory card
   per repeat. One file, on one event, which is settings.c's rule. */
static void mp_move_face(mainmenu_t *m, int d)
{
    player_t *p = mp_sel(m);
    if (!p)
        return;
    p->face = ((p->face < 0 ? 0 : p->face) + d + PL_N_FACES) % PL_N_FACES;
    p->dirty = 1;
    m->cue = MM_CUE_ARROW;
}

/* Leaving the page: whatever it changed goes down now. */
static void mp_leave(mainmenu_t *m, int page)
{
    player_save_cur_if_dirty();
    m->page = page;
}

static void mp_set_sel(mainmenu_t *m, int row)
{
    if (row < 0 || row >= m->pnview)
        return;
    m->psel = row;
    if (m->ptop > m->psel)
        m->ptop = m->psel;
    if (m->psel >= m->ptop + MP_N_VIS)
        m->ptop = m->psel - MP_N_VIS + 1;
    /* SELECTING A ROW SELECTS THE PROFILE. There is no separate "use this one"
       press on the game's own page either -- Continue leaves, it does not
       choose -- and the card beside the list is already showing whoever the
       cursor is on. */
    player_select(m->pview[m->psel]);
    m->action = MM_ACT_PLAYER;
}

static void mp_fire(mainmenu_t *m, int stop)
{
    int bar, orange;
    mp_bar_row(stop, &bar, &orange);
    if (bar >= 0) {
        m->press_row = bar;
        m->press_t = 0.f;
    }
    if (!mainmenu_p_live(m, stop)) {
        m->cue = MM_CUE_DENY;
        return;
    }
    switch (stop) {
    case MM_P_CREATE:
        mp_open_name(m);
        m->cue = MM_CUE_PRESS;
        break;
    case MM_P_REMOVE:
        /* The refusal is the game's own, and it is a DIALOG, not a silence. */
        if (player_count() <= 1) {
            mp_say(m, STR_UI_LAST_PLAYER);
            m->cue = MM_CUE_DENY;
        } else {
            m->modal = MM_MODAL_REMOVE;
            m->mfocus = MM_MODAL_CANCEL;   /* No, on a destructive question */
            m->marmed = -1;
            m->cue = MM_CUE_PRESS;
        }
        break;
    case MM_P_SORT:
        m->psort = (m->psort + 1) % MM_N_SORT;
        mainmenu_players_sync(m);
        m->cue = MM_CUE_PRESS;
        break;
    case MM_P_FACE_L: mp_move_face(m, -1); break;
    case MM_P_FACE_R: mp_move_face(m, +1); break;
    case MM_P_LIST:
        m->cue = MM_CUE_PRESS;
        break;
    case MM_P_RACE:
        /* main.c writes the profile at a race start anyway; this is the face,
           which is not part of the setup it stores. */
        player_save_cur_if_dirty();
        m->action = MM_ACT_RACE;
        m->cue = MM_CUE_PRESS;
        break;
    case MM_P_CONTINUE:
        mp_leave(m, MM_PAGE_MAIN);
        m->cue = MM_CUE_PRESS;
        break;
    default:
        m->cue = MM_CUE_DENY;
        break;
    }
}

/* The ring skips the dead stops, the way the main menu's does. */
static int mp_next_focus(const mainmenu_t *m, int from, int d)
{
    int i, k = from;
    for (i = 0; i < MM_P_N_FOCUS; i++) {
        k = (k + d + MM_P_N_FOCUS) % MM_P_N_FOCUS;
        if (mainmenu_p_live(m, k))
            return k;
    }
    return from;
}

/* Returns 1 when the profile was made. A REFUSAL raises the game's own dialog
   (which replaces the modal) and returns 0; an EMPTY name is refused quietly,
   because both dialogs carry a Cancel and pressing Ok on nothing is not one --
   the grid stays open on it and the keyboard has already gone. */
static int mp_commit_create(mainmenu_t *m)
{
    player_t *p = mp_sel(m);
    const int face = p && p->face >= 0 ? p->face : 0;
    int r;

    /* Trim: a name that is all spaces is not a name, and a trailing space in a
       filename is a filename that is hard to type on the other machine. */
    {
        int a = 0, b = (int)strlen(m->mname);
        while (m->mname[a] == ' ') a++;
        while (b > a && m->mname[b - 1] == ' ') b--;
        memmove(m->mname, m->mname + a, (size_t)(b - a));
        m->mname[b - a] = 0;
    }
    if (!m->mname[0]) {
        m->cue = MM_CUE_DENY;
        return 0;
    }
    r = player_create(m->mname, face);
    if (r == -2) { mp_say(m, STR_UI_PLAYER_EXISTS); m->cue = MM_CUE_DENY; return 0; }
    if (r == -4) { mp_say(m, STR_UI_SAVE_FAILED);   m->cue = MM_CUE_DENY; return 0; }
    if (r != 0)  { mp_say(m, STR_UI_CREATE_FAILED); m->cue = MM_CUE_DENY; return 0; }
    m->modal = MM_MODAL_NONE;
    m->pfocus = MM_P_LIST;
    mainmenu_players_sync(m);
    m->action = MM_ACT_PLAYER;
    m->cue = MM_CUE_PRESS;
    return 1;
}

static void mp_commit_remove(mainmenu_t *m)
{
    /* The cursor can only be nowhere if the roster emptied under the dialog,
       which nothing does -- but pview[-1] is the read it would cost. */
    const int r = (m->psel >= 0 && m->psel < m->pnview)
                  ? player_remove(m->pview[m->psel]) : -1;
    m->modal = MM_MODAL_NONE;
    if (r == -2) { mp_say(m, STR_UI_LAST_PLAYER); m->cue = MM_CUE_DENY; return; }
    if (r != 0)  { mp_say(m, STR_UI_SAVE_FAILED); m->cue = MM_CUE_DENY; return; }
    mainmenu_players_sync(m);
    m->pfocus = MM_P_LIST;
    m->action = MM_ACT_PLAYER;
    m->cue = MM_CUE_PRESS;
}

static void mp_key(mainmenu_t *m, int k)
{
    const int n = (int)strlen(m->mname);
    if (k == MP_KEY_SHIFT) {
        m->mshift = !m->mshift;
        m->cue = MM_CUE_PRESS;
        return;
    }
    if (k == MP_KEY_BACK) {
        if (n > 0)
            m->mname[n - 1] = 0;
        m->cue = MM_CUE_PRESS;
        return;
    }
    if (n >= PL_NAME - 1) {
        m->cue = MM_CUE_DENY;   /* the engine's own field is sixteen bytes */
        return;
    }
    m->mname[n] = mainmenu_kb_char(m, k);
    m->mname[n + 1] = 0;
    /* One capital, then lower case -- which is how a name is spelt and saves a
       press per letter. */
    if (m->mshift && n == 0)
        m->mshift = 0;
    m->cue = MM_CUE_PRESS;
}

static void mp_modal_fire(mainmenu_t *m, int stop)
{
    if (stop < 0)
        return;
    if (stop < MM_KB_KEYS) {
        mp_key(m, stop);
        return;
    }
    if (stop == MM_MODAL_CANCEL) {
        m->modal = MM_MODAL_NONE;
        m->cue = MM_CUE_PRESS;
        return;
    }
    switch (m->modal) {
    case MM_MODAL_CREATE: mp_commit_create(m); break;
    case MM_MODAL_REMOVE: mp_commit_remove(m); break;
    case MM_MODAL_ASK:    mg_commit(m); break;
    default:
        m->modal = MM_MODAL_NONE;
        m->cue = MM_CUE_PRESS;
        break;
    }
}

/* THE MODAL OWNS EVERY INPUT while it is up, which is what a modal is. */
static void mm_step_modal(mainmenu_t *m, unsigned int down,
                          const touch_state *tp, int screen_w, int screen_h)
{
    const int pair = m->modal != MM_MODAL_SAY;
    const int keys = m->modal == MM_MODAL_CREATE;

    /* THE MACHINE'S KEYBOARD OWNS THE FRAME while it is up: the compositor has
       the input and the screen, and all this does is ask whether it is done.
       Every pad bit is dropped -- the press that opened the dialog is still
       held on the frame after it, and reading it here would post it twice. */
    if (m->modal == MM_MODAL_IME) {
        char got[PL_NAME];
        const int r = ime_poll(got, (int)sizeof got);
        if (r == 0)
            return;
        m->modal = MM_MODAL_NONE;           /* the keyboard is gone either way */
        if (r > 0) {
            snprintf(m->mname, sizeof m->mname, "%s", got);
            mp_commit_create(m);            /* may raise the refusal dialog */
        }
        return;
    }

    if (down & SCE_CTRL_CIRCLE) {       /* the pad's own "cancel" */
        if (m->modal == MM_MODAL_SAY)
            mp_modal_fire(m, MM_MODAL_OK);
        else
            mp_modal_fire(m, MM_MODAL_CANCEL);
        return;
    }
    if (down & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) {
        const int d = (down & SCE_CTRL_RIGHT) ? +1 : -1;
        if (keys && m->mfocus < MM_KB_KEYS) {
            const int r = m->mfocus / MM_KB_COLS;
            const int c = (m->mfocus % MM_KB_COLS + d + MM_KB_COLS)
                          % MM_KB_COLS;
            m->mfocus = r * MM_KB_COLS + c;
        } else if (pair) {
            m->mfocus = m->mfocus == MM_MODAL_OK ? MM_MODAL_CANCEL
                                                 : MM_MODAL_OK;
        }
        m->cue = MM_CUE_FOCUS;
    }
    if (down & (SCE_CTRL_UP | SCE_CTRL_DOWN)) {
        const int d = (down & SCE_CTRL_DOWN) ? +1 : -1;
        if (keys) {
            /* The grid and the button bar are one column of rows: the row under
               the bottom key row is Ok/Cancel, and it wraps back to the top. */
            const int rows = MM_KB_ROWS + 1;
            int r = m->mfocus < MM_KB_KEYS ? m->mfocus / MM_KB_COLS
                                           : MM_KB_ROWS;
            int c = m->mfocus < MM_KB_KEYS ? m->mfocus % MM_KB_COLS
                                           : (m->mfocus == MM_MODAL_OK ? 2 : 7);
            r = (r + d + rows) % rows;
            m->mfocus = r < MM_KB_ROWS ? r * MM_KB_COLS + c
                                       : (c < MM_KB_COLS / 2 ? MM_MODAL_OK
                                                             : MM_MODAL_CANCEL);
        } else if (pair) {
            m->mfocus = m->mfocus == MM_MODAL_OK ? MM_MODAL_CANCEL
                                                 : MM_MODAL_OK;
        }
        m->cue = MM_CUE_FOCUS;
    }
    /* SQUARE rubs out, which is the one shortcut worth having: the key is nine
       presses away from the far corner of the grid. */
    if (keys && (down & SCE_CTRL_SQUARE))
        mp_key(m, MP_KEY_BACK);
    if (down & (SCE_CTRL_CROSS | SCE_CTRL_START))
        mp_modal_fire(m, m->mfocus);

    if (!tp)
        return;
    if (tp->pressed) {
        m->marmed = mainmenu_modal_at(m, screen_w, screen_h, tp->x, tp->y);
        if (m->marmed >= 0 && m->marmed != m->mfocus) {
            m->mfocus = m->marmed;
            m->cue = MM_CUE_FOCUS;
        }
    }
    if (tp->released) {
        const int at = mainmenu_modal_at(m, screen_w, screen_h, tp->x, tp->y);
        if (at >= 0 && at == m->marmed)
            mp_modal_fire(m, at);
        m->marmed = -1;
    }
}

static void mm_step_players(mainmenu_t *m, unsigned int down,
                            const touch_state *tp, int screen_w, int screen_h)
{
    /* No modal gate here: mainmenu_step takes it for every page. */
    /* THE LIST IS THE ONE STOP THAT EATS UP AND DOWN, because the rows are what
       those keys mean while the cursor is in a table. Everywhere else on the
       page they walk the ring, and LEFT/RIGHT step off the list onto it. */
    if (m->pfocus == MM_P_LIST && m->pnview > 0) {
        if (down & SCE_CTRL_UP) {
            if (m->psel > 0) mp_set_sel(m, m->psel - 1);
            else             m->pfocus = mp_next_focus(m, MM_P_LIST, -1);
            m->cue = MM_CUE_FOCUS;
        }
        if (down & SCE_CTRL_DOWN) {
            if (m->psel < m->pnview - 1) mp_set_sel(m, m->psel + 1);
            else                         m->pfocus = mp_next_focus(m, MM_P_LIST, +1);
            m->cue = MM_CUE_FOCUS;
        }
    } else {
        if (down & SCE_CTRL_UP) {
            m->pfocus = mp_next_focus(m, m->pfocus, -1);
            m->cue = MM_CUE_FOCUS;
        }
        if (down & SCE_CTRL_DOWN) {
            m->pfocus = mp_next_focus(m, m->pfocus, +1);
            m->cue = MM_CUE_FOCUS;
        }
    }
    /* LEFT and RIGHT are the portrait's, wherever the focus is: they are the
       only pair of arrows on the page and they do one thing. */
    if (down & SCE_CTRL_LEFT)  mp_move_face(m, -1);
    if (down & SCE_CTRL_RIGHT) mp_move_face(m, +1);
    if (down & (SCE_CTRL_CROSS | SCE_CTRL_START))
        mp_fire(m, m->pfocus);
    /* CIRCLE leaves, unless there is nobody to leave as: a first launch has to
       make somebody before the front end has a profile to race with. */
    if ((down & SCE_CTRL_CIRCLE) && m->pnview > 0) {
        mp_leave(m, MM_PAGE_MAIN);
        m->cue = MM_CUE_PRESS;
    }

    if (!tp)
        return;
    {
        const mmframe f = mm_frame(screen_w, screen_h);
        const int was = m->ptop;
        if (mm_sb_drive(&f, tp, MP_BAR_X, MP_BAR_Y, MP_BAR_Y + MP_BAR_H,
                        &m->ptop, m->pnview, MP_N_VIS, &m->sb_drag)) {
            if (m->ptop != was)
                m->cue = MM_CUE_FOCUS;
            m->parmed = -1;
            return;
        }
    }
    if (tp->pressed) {
        int row = -1;
        m->parmed = mainmenu_p_stop_at(m, screen_w, screen_h, tp->x, tp->y,
                                       &row);
        if (m->parmed >= 0 && mainmenu_p_live(m, m->parmed)) {
            if (m->pfocus != m->parmed)
                m->cue = MM_CUE_FOCUS;
            m->pfocus = m->parmed;
        }
        if (row >= 0 && row != m->psel) {
            mp_set_sel(m, row);
            m->cue = MM_CUE_FOCUS;
        }
    }
    if (tp->released) {
        int row = -1;
        const int at = mainmenu_p_stop_at(m, screen_w, screen_h, tp->x, tp->y,
                                          &row);
        if (at >= 0 && at == m->parmed)
            mp_fire(m, at);
        m->parmed = -1;
    }
}

/* ---------------------------------------------------------------- the draw */

/* One string in the engine's own small letters, with the compiled-in font as the
   fallback every other drawer here uses. `align` 0 left, 1 centred on x,
   2 right of x. */
/* THE HEIGHT OF THE FONT THAT WILL ACTUALLY BE DRAWN, which is not
   ui_text_h(). That one is the FALLBACK's cell -- FONT_CH, 19 -- and Smash20's
   and Smash26's are 28, so a caller that centres a label with ui_text_h while
   mp_text draws the real font puts it 4 px low in a 29 px plate and hangs it
   over the bottom edge. Three call sites had it wrong and every one of them was
   invisible in the harnesses, which run the fallback path. One copy now. */
static float mp_text_h(const mainmenu_t *m, const mmframe *f, int big, float ts)
{
    const sfont sf = big ? sf_big(m->tex.font_big) : sf_small(m->tex.font_small);
    const float sc = f->us * ts;
    return sf.tex ? sf_h(&sf, sc) : ui_text_h(sc);
}

static void mp_text(const mainmenu_t *m, const mmframe *f, int big,
                    float x, float y, float ts, int align,
                    float r, float g, float b, const char *s)
{
    const sfont sf = big ? sf_big(m->tex.font_big) : sf_small(m->tex.font_small);
    const float sc = f->us * ts;
    const float w = sf.tex ? sf_w(&sf, sc, s) : ui_text_w(sc, s);
    const float tx = align == 1 ? x - w * 0.5f : (align == 2 ? x - w : x);
    if (sf.tex)
        sf_text_shadowed(&sf, tx, y, sc, r, g, b, 1.f, s);
    else
        ui_text(tx, y, sc, r, g, b, 1.f, s);
}

/* The four bars this page puts in the eight-row column. */
static void mp_draw_bars(const mainmenu_t *m, const mmframe *f)
{
    static const int STOP[4] = { MM_P_CREATE, MM_P_REMOVE,
                                 MM_P_SORT, MM_P_FACE };
    static const char *const SORT_NAME[MM_N_SORT] = {
        STR_UI_SORT_BY_SCORE, STR_UI_SORT_BY_CASH,
        STR_UI_SORT_BY_TIME,  STR_UI_SORT_BY_NAME
    };
    int i;

    for (i = 0; i < 4; i++) {
        const int stop = STOP[i];
        const int live = mainmenu_p_live(m, stop);
        const int lit = live && m->pfocus == stop;
        const char *label;
        float bx, by, bw, bh, v, slide;
        int row, orange;

        mp_bar_row(stop, &row, &orange);
        switch (stop) {
        case MM_P_CREATE: label = STR_UI_CREATE_PLAYER; break;
        case MM_P_REMOVE: label = STR_UI_REMOVE_PLAYER; break;
        case MM_P_SORT:   label = SORT_NAME[m->psort];  break;
        default:          label = STR_UI_FACE;          break;
        }
        slide = lit ? MM_SLIDE * MM_SETTLED : 0.f;
        mm_draw_wedge(m, f, row);
        mm_bar_draw_rect(m, f, row, slide, &bx, &by, &bw, &bh);
        v = !live ? MM_V_GREY
                  : (orange ? (lit ? MM_V_ORANGE_F : MM_V_ORANGE)
                            : (lit ? MM_V_RED_F : MM_V_RED));
        if (m->tex.buttons)
            ui_image(bx, by, bw, bh, m->tex.buttons,
                     0.f, v, 1.f, v + MM_V_CELL, 1.f, 1.f, 1.f, 1.f);
        else
            ui_rect(bx, by, bw, bh,
                    live ? (orange ? 0.90f : 0.72f) : 0.45f,
                    live ? (orange ? 0.55f : 0.09f) : 0.45f,
                    live ? (orange ? 0.04f : 0.11f) : 0.47f, 1.f);
        mm_label(m, f, bx, by, bw, bh, label,
                 live ? 1.f : 0.82f, live ? 1.f : 0.82f, live ? 1.f : 0.84f);
    }
}

/* The card: the portrait, its two arrows, and the five facts beside it -- all of
   them profile fields, which is what this port could not draw until it read the
   format. */
/* THE CARD, AND TWO PAGES DRAW IT. `scores_only' is dlgMULTIPLAYER's own
   difference from the roster page's: that page's `Scores' line is LEFT-aligned
   at MP_TEXT_X and has no `Cash' beside it, measured on the game's own shot of
   it at 240..306, where this one right-aligns the pair on 520. Everything above
   the second rule is identical on both, to the pixel. */
static void mp_draw_card_at(const mainmenu_t *m, const mmframe *f,
                            int scores_only)
{
    /* WHOSE CARD IT IS depends on the page. On the roster page it is the row
       the cursor is on -- the point of that screen is to look at a profile
       before selecting it. Everywhere else it is the profile that IS selected;
       `mp_sel' reads `pview', which is the roster page's own order and is empty
       until that page has been opened, so the multiplayer page drew `Create
       player' over a full roster. */
    const player_t *p = (m->page == MM_PAGE_PLAYERS) ? mp_sel(m)
                                                     : player_cur();
    char line[96], t[24];
    float x, y, w, h, ty;
    int face;

    mm_gbox(f, MP_TEXT_X, MP_FACE_X, MP_FACE_Y, MP_FACE_W, MP_FACE_H,
            &x, &y, &w, &h);
    face = p && p->face >= 0 && p->face < PL_N_FACES ? p->face : 0;
    if (m->tex.face[face])
        ui_image(x, y, w, h, m->tex.face[face],
                 MM_FACE_U0, MM_FACE_V0, MM_FACE_U1, MM_FACE_V1,
                 1.f, 1.f, 1.f, 1.f);
    else
        ui_rect(x, y, w, h, 0.20f, 0.22f, 0.28f, 1.f);

    if (m->pnview > 0) {
        mp_arrow_rect(f, 0, &x, &y, &w, &h);
        mm_arrow(m, x, y, w, m->pfocus == MM_P_FACE_L ? 1 : 0, 0);
        mp_arrow_rect(f, 1, &x, &y, &w, &h);
        mm_arrow(m, x, y, w, m->pfocus == MM_P_FACE_R ? 1 : 0, 1);
    }

    if (!p) {
        /* An empty roster still draws the card's frame and says what to do,
           which is the one thing there is to do on this screen. */
        mp_text(m, f, 0, gx(f, MP_TEXT_X, MP_TEXT_X), py(f, MP_NAME_Y),
                MM_TS_INFO, 0, 1.f, 1.f, 1.f, STR_UI_CREATE_PLAYER);
        return;
    }

    mp_text(m, f, 1, gx(f, MP_TEXT_X, MP_TEXT_X), py(f, MP_NAME_Y),
            MM_TS_WELCOME, 0, 1.f, 1.f, 1.f, p->name);
    mm_rule_at(f, gx(f, MP_TEXT_X, MP_TEXT_X), py(f, MP_RULE1_Y),
               MP_RULE_W * f->us);

    ty = MP_INFO_Y;
    snprintf(line, sizeof line, "%s: %s", STR_UI_RANK, player_rank(p));
    mp_text(m, f, 0, gx(f, MP_TEXT_X, MP_TEXT_X), py(f, ty),
            MM_TS_INFO, 0, 1.f, 1.f, 1.f, line);
    ty += MP_INFO_LH;
    snprintf(line, sizeof line, "%s: %s", STR_UI_CURRENT_CAR,
             STR_CAR_NAME[p->sel_car >= 0 && p->sel_car < STR_N_CARS
                          ? p->sel_car : 0]);
    mp_text(m, f, 0, gx(f, MP_TEXT_X, MP_TEXT_X), py(f, ty),
            MM_TS_INFO, 0, 1.f, 1.f, 1.f, line);
    ty += MP_INFO_LH;
    player_time_str(p->play_time, t, sizeof t);
    snprintf(line, sizeof line, "%s: %s", STR_UI_PLAY_TIME, t);
    mp_text(m, f, 0, gx(f, MP_TEXT_X, MP_TEXT_X), py(f, ty),
            MM_TS_INFO, 0, 1.f, 1.f, 1.f, line);

    mm_rule_at(f, gx(f, MP_TEXT_X, MP_TEXT_X), py(f, MP_RULE2_Y),
               MP_RULE_W * f->us);
    /* RIGHT-ALIGNED on the rule's own right end, which is where the game puts
       it: measured, the pair runs x 357..517 under a rule that ends at 519,
       while every line above it starts at 240.
     *
       AND dlgMULTIPLAYER'S IS NEITHER. On the game's own shot of that page the
       line is `Scores: 0' alone, LEFT-aligned at 240 and running to 306, with
       no Cash beside it -- so the same card, one line differently. */
    if (scores_only) {
        snprintf(line, sizeof line, "%s: %d", STR_UI_SCORES, p->scores);
        mp_text(m, f, 0, gx(f, MP_TEXT_X, MP_TEXT_X), py(f, MP_CASH_Y),
                MM_TS_INFO, 0, 1.f, 1.f, 1.f, line);
        return;
    }
    snprintf(line, sizeof line, "%s: %d    %s: $%d",
             STR_UI_SCORES, p->scores, STR_UI_CASH, p->cash);
    mp_text(m, f, 0, gx(f, MP_TEXT_X, MP_TEXT_X + MP_RULE_W), py(f, MP_CASH_Y),
            MM_TS_INFO, 2, 1.f, 1.f, 1.f, line);
}

static void mp_draw_card(const mainmenu_t *m, const mmframe *f)
{
    mp_draw_card_at(m, f, 0);
}

/* The table. Four columns, seven rows, the marker on the row the cursor is on,
   and the engine's own scroll bar beside it when there is more than a page. */
static void mp_draw_list(const mainmenu_t *m, const mmframe *f)
{
    const float l = px(f, MP_LIST_L), r = px(f, MP_LIST_R);
    char line[64], t[24];
    int i;

    mp_text(m, f, 0, l + (MP_COL_NAME - MP_LIST_L) * f->us, py(f, MP_HEAD_Y),
            MM_TS_INFO, 0, 1.f, 1.f, 1.f, STR_UI_COL_PLAYER);
    mp_text(m, f, 0, px(f, MP_COL_CX[0]), py(f, MP_HEAD_Y),
            MM_TS_INFO, 1, 1.f, 1.f, 1.f, STR_UI_COL_SCORE);
    mp_text(m, f, 0, px(f, MP_COL_CX[1]), py(f, MP_HEAD_Y),
            MM_TS_INFO, 1, 1.f, 1.f, 1.f, STR_UI_COL_CASH);
    mp_text(m, f, 0, px(f, MP_COL_CX[2]), py(f, MP_HEAD_Y),
            MM_TS_INFO, 1, 1.f, 1.f, 1.f, STR_UI_COL_TIME);
    mm_rule_at(f, l, py(f, MP_HEAD_RULE), r - l);
    mm_rule_at(f, l, py(f, MP_FOOT_Y), r - l);

    for (i = 0; i < MP_N_VIS && m->ptop + i < m->pnview; i++) {
        const int row = m->ptop + i;
        const player_t *p = player_at(m->pview[row]);
        const float y = py(f, MP_ROW_Y + MP_ROW_H * i);
        const int here = row == m->psel;
        const float g = here ? 1.f : 0.86f;
        if (!p)
            continue;
        if (here) {
            /* the red marker the game puts against the chosen row, and the
               underline under its name */
            float ax, ay, aw, ah;
            mm_gbox(f, MP_LIST_L, MP_MARK_X, MP_ROW_Y + MP_ROW_H * i + 3.f,
                    MP_MARK_SZ, MP_MARK_SZ, &ax, &ay, &aw, &ah);
            /* state 0 is the RED cell -- see mm_arrow. The game's own row
               marker is red and the silver is what a focused arrow goes. */
            mm_arrow(m, ax, ay, aw, 0, 0);
            mm_rule_at(f, l, py(f, MP_ROW_Y + MP_ROW_H * i + MP_ROW_RULE),
                       (MP_RULE_W - 158.f) * f->us);
        }
        mp_text(m, f, 0, l + (MP_COL_NAME - MP_LIST_L) * f->us, y,
                MM_TS_INFO, 0, 1.f, g, g, p->name);
        snprintf(line, sizeof line, "%d", p->scores);
        mp_text(m, f, 0, px(f, MP_COL_CX[0]), y, MM_TS_INFO, 1,
                1.f, g, g, line);
        snprintf(line, sizeof line, "%d", p->cash);
        mp_text(m, f, 0, px(f, MP_COL_CX[1]), y, MM_TS_INFO, 1,
                1.f, g, g, line);
        player_time_str(p->play_time, t, sizeof t);
        mp_text(m, f, 0, px(f, MP_COL_CX[2]), y, MM_TS_INFO, 1,
                1.f, g, g, t);
    }

    /* dlgSTAT's own bar, on the same texture and the same rule: the thumb is
       the visible window's share of the list and it sits where the window is. */
    if (m->pnview > MP_N_VIS)
        mm_draw_scrollbar(m, f, MP_BAR_X, MP_BAR_Y, MP_BAR_Y + MP_BAR_H,
                          m->ptop, m->pnview, MP_N_VIS);
}

/* messagebox_empty's own nine slices, in SCREEN pixels rather than design ones.
 *
 * mm_frame9 does the same job for dlgMAPINFO's window and takes design
 * coordinates, which map x through px() -- the stretch. This page's modal is a
 * GROUP about the screen's centre line and its insides are placed with gx(), so
 * a rim mapped the other way would sit a dozen pixels off its own contents on a
 * 960x544 panel. Same texture, same three constants, same corner size in the
 * uniform scale. */
/* THERE WAS A SECOND NINE-SLICE DRAWER HERE and it was the correct one. The
   two have been merged into mm_frame9 -- see its own note on the layout bug
   that came of having two, which is the second time these two disagreed about
   the same eight slices. */

/* One modal button, with the game's own red-then-orange pair. */
static void mp_draw_btn(const mainmenu_t *m, const mmframe *f, int i, int pair,
                        const char *label, int lit)
{
    float x, y, w, h;
    mp_btn_rect(m, f, i, pair, &x, &y, &w, &h);
    /* THE DIALOG'S OWN ART -- `messagebox's right half, one whole cell per
       plate. The left of a pair is the RED one and the right the ORANGE, and
       the focused one wears the white dot, which is exactly what the game's
       shot of `erase data' has. */
    if (mm_msgbtn(m, x, y, w, h, i != 0, lit)) {
        /* drawn */
    } else if (i == 0 && m->tex.radio) {
        /* NO menu.vsc WITH `messagebox' IN IT: the row bars, three-sliced so
           they at least keep their left cap. RadioButtonsTextures is where a
           red-with-a-dot lives -- ButtonsTextures' red cells carry a TRIANGLE
           and Button_back a curled RETURN ARROW, and this port drew one of
           each. */
        const float v = lit ? MM_V_RAD_ON : MM_V_RAD;
        mm_bar3(x, y, w, h, m->tex.radio, v, v + MM_V_RAD_CELL);
    } else if (i != 0 && m->tex.buttons) {
        const float v = lit ? MM_V_ORANGE_F : MM_V_ORANGE;
        mm_bar3(x, y, w, h, m->tex.buttons, v, v + MM_V_CELL);
    } else {
        ui_rect(x, y, w, h, i ? 0.90f : 0.72f, i ? 0.55f : 0.09f,
                i ? 0.04f : 0.11f, 1.f);
    }
    /* CENTRED ON THE FONT THAT IS ACTUALLY DRAWN. `ui_text_h' is the FALLBACK
       font's cell -- FONT_CH, 19 -- and Smash20's is 28, so measuring with the
       wrong one put the label 4 px low in a 29 px plate and hung it over the
       bottom edge. mm_r_button had this right; this did not, and the harnesses
       could not see it because they run the fallback path. */
    mp_text(m, f, 0, x + w * 0.5f,
            y + (h - mp_text_h(m, f, 0, MM_TS_LABEL)) * 0.5f,
            MM_TS_LABEL, 1, 1.f, 1.f, 1.f, label);
}

static void mp_draw_modal(const mainmenu_t *m, const mmframe *f)
{
    float x, y, w, h;

    /* THE MACHINE'S KEYBOARD DRAWS ITSELF, over everything, and it dims what is
       under it as well -- so this draws nothing at all while it is up rather
       than a second dim and an empty panel behind it. */
    if (m->modal == MM_MODAL_IME)
        return;
    char buf[4];
    int i;

    ui_rect(0.f, 0.f, f->w, f->h, 0.f, 0.f, 0.f, 0.55f);
    mp_modal_rect(m, f, &x, &y, &w, &h);
    /* A GROUND UNDER THE RIM, AND ONLY UNDER THE KEYBOARD. messagebox_empty is
       a RIM -- 32 texels of rounded silver with nothing in the middle -- and on
       the game's own shot of this dialog the page shows STRAIGHT THROUGH it:
       the whole screen behind is already dimmed and there are two buttons and
       one line to read. A plate here is a SQUARE behind a rounded frame, which
       is what it read as -- its corners stand outside the rim's arcs. The CREATE
       dialog is the exception and the reason this was ever drawn: forty keys the
       original has not got, over a table of names, need a ground of their own. */
    if (m->modal == MM_MODAL_CREATE)
        ui_rect(x, y, w, h, 0.08f, 0.07f, 0.06f, 0.45f);
    if (m->tex.panel) {
        mm_frame9(f, x, y, w, h, m->tex.panel);
    } else {
        ui_rect(x, y, w, f->us * 2.f, 0.85f, 0.85f, 0.88f, 0.9f);
        ui_rect(x, y + h - f->us * 2.f, w, f->us * 2.f,
                0.85f, 0.85f, 0.88f, 0.9f);
    }

    if (m->modal == MM_MODAL_CREATE) {
        /* THE PROMPT AND THE FIELD ARE ONE LINE, which is what the game's own
           shot of this dialog has: `Player name:' and the caret to its right,
           over a rule that runs the width of the panel's inside. The field is
           at a FIXED offset rather than after the label's own width, so the
           caret does not walk when the label is drawn in the fallback font. */
        char field[PL_NAME + 2];
        snprintf(field, sizeof field, "%s%s", m->mname, STR_UI_CARET);
        mp_text(m, f, 0, gx(f, MP_DLG_CX, MP_DLG_X + MP_FIELD_PAD),
                py(f, MP_FIELD_Y), MM_TS_INFO, 0,
                1.f, 1.f, 1.f, STR_UI_PLAYER_NAME ":");
        mp_text(m, f, 0, gx(f, MP_DLG_CX, MP_DLG_X + MP_FIELD_X),
                py(f, MP_FIELD_Y), MM_TS_INFO, 0,
                1.f, 1.f, 0.72f, field);
        mm_rule_at(f, gx(f, MP_DLG_CX, MP_DLG_X + MP_FIELD_PAD),
                   py(f, MP_FIELD_Y + 20.f),
                   (MP_DLG_W - MP_FIELD_PAD * 2.f) * f->us);
        for (i = 0; i < MM_KB_KEYS; i++) {
            const int lit = m->mfocus == i;
            mp_key_rect(f, i, &x, &y, &w, &h);
            if (m->tex.buttons) {
                const float v = lit ? MM_V_RED_F : MM_V_GREY;
                ui_image(x, y, w, h, m->tex.buttons, 0.2f, v, 0.8f,
                         v + MM_V_CELL, 1.f, 1.f, 1.f, 1.f);
            } else {
                ui_rect(x, y, w, h, lit ? 0.72f : 0.28f,
                        lit ? 0.09f : 0.28f, lit ? 0.11f : 0.30f, 1.f);
            }
            mp_text(m, f, 0, x + w * 0.5f,
                    y + (h - mp_text_h(m, f, 0, MM_TS_LABEL)) * 0.5f,
                    MM_TS_LABEL, 1, 1.f, 1.f, 1.f,
                    mp_key_label(m, i, buf));
        }
        mp_draw_btn(m, f, 0, 1, STR_UI_OK, m->mfocus == MM_MODAL_OK);
        mp_draw_btn(m, f, 1, 1, STR_UI_CANCEL, m->mfocus == MM_MODAL_CANCEL);
        return;
    }

    /* ONE LINE PER '\n', because the Garage's questions and two of the engine's
       refusals are written with them -- "Do you want to sell car and
       upgrades?\ncar: ..." and "You have to sell\nprevious upgrade first!".
       Centred, at the small font's own pitch, and the block is centred in the
       panel so a one-line dialog sits exactly where the game's own shot of
       "Can't remove last player" has it. */
    {
        const char *say = m->modal == MM_MODAL_REMOVE ? STR_UI_REMOVE_ASK
                                                      : (m->msay ? m->msay : "");
        const char *q = say;
        int nl = 1;
        float ty;
        for (; *q; q++)
            if (*q == '\n') nl++;
        ty = MP_DLG_Y + 40.f - (float)(nl - 1) * MM_G_LINE_H * 0.5f;
        while (*say) {
            const char *br = strchr(say, '\n');
            size_t len = br ? (size_t)(br - say) : strlen(say);
            char one[160];
            if (len >= sizeof one) len = sizeof one - 1;
            memcpy(one, say, len);
            one[len] = 0;
            mp_text(m, f, 0, px(f, MP_DLG_CX), py(f, ty),
                    MM_TS_INFO, 1, 1.f, 1.f, 1.f, one);
            ty += MM_G_LINE_H;
            if (!br) break;
            say = br + 1;
        }
    }
    if (m->modal == MM_MODAL_REMOVE || m->modal == MM_MODAL_ASK) {
        mp_draw_btn(m, f, 0, 1, STR_UI_YES, m->mfocus == MM_MODAL_OK);
        mp_draw_btn(m, f, 1, 1, STR_UI_NO,  m->mfocus == MM_MODAL_CANCEL);
    } else {
        mp_draw_btn(m, f, 0, 0, STR_UI_OK, 1);
    }
}

/* ------------------------------------------------ the server list modal
 *
 * `Join game' starts a browse and puts this up. The original has a whole dialog
 * for it -- `dlgNETJOIN_SERVINFO', whose table is `editServerName',
 * `editNPlayers', `editNMaxPlayers', `editTrack', `editNLaps', `editLag', the
 * three `enumCar' restrictions and `staticRestr' -- and this port draws a
 * modal over the page the button was pressed on instead. That is the port's
 * own: a fifth screen for a list that is usually one row long is a screen to
 * navigate rather than a thing to read. `known-issues.md'.
 *
 * ON THE ROSTER PAGE'S OWN PANEL, which is the create dialog's rectangle -- it
 * is the taller of the two, and eight rows and a Cancel button need the room. */
#define MM_S_ROW_H   22.f
#define MM_S_TOP     (MP_CDLG_Y + 34.f)
#define MM_S_PAD     20.f

static void mm_s_row_rect(const mmframe *f, int i,
                          float *x, float *y, float *w, float *h)
{
    mm_gbox(f, MP_DLG_CX, MP_DLG_X + MM_S_PAD,
            MM_S_TOP + MM_S_ROW_H * (float)i,
            MP_DLG_W - MM_S_PAD * 2.f, MM_S_ROW_H - 2.f, x, y, w, h);
}

/* Which row is under (x, y), or -1. NET_MAX_SERVERS is the Cancel button. */
static int mm_s_at(const mainmenu_t *m, int screen_w, int screen_h,
                   float x, float y)
{
    const mmframe f = mm_frame(screen_w, screen_h);
    float bx, by, bw, bh;
    int i;
    if (!m || m->modal != MM_MODAL_SERVERS)
        return -1;
    for (i = 0; i < net_n_servers(); i++) {
        mm_s_row_rect(&f, i, &bx, &by, &bw, &bh);
        if (mp_in(x, y, bx, by, bw, bh))
            return i;
    }
    mp_btn_rect(m, &f, 0, 0, &bx, &by, &bw, &bh);
    if (mp_in(x, y, bx, by, bw, bh))
        return NET_MAX_SERVERS;
    return -1;
}

/* Join the row the cursor is on, or close. */
static void mm_s_fire(mainmenu_t *m, int at)
{
    const player_t *p = player_cur();
    unsigned char up[3];
    const int c = (m->car >= 0 && m->car < MM_N_CARS) ? m->car : 0;

    if (at == NET_MAX_SERVERS || at < 0) {
        net_leave();
        m->modal = MM_MODAL_NONE;
        m->cue = MM_CUE_PRESS;
        return;
    }
    up[0] = (unsigned char)garage_level(p, GAR_BOOSTER, c);
    up[1] = (unsigned char)garage_level(p, GAR_ENGINE, c);
    up[2] = (unsigned char)garage_level(p, GAR_TIRES, c);
    if (!net_join(at, p ? p->name : STR_UI_DEFAULT_NAME, c, up,
                  garage_skin(p, c), p ? p->face : 0)) {
        m->cue = MM_CUE_DENY;
        return;
    }
    /* THE MODAL STAYS UP over `Connecting...' until the welcome arrives -- which
       is 42451, the engine's own word for the wait, and the step function is
       what closes it when net_mode_now() says NET_JOINED. */
    m->cue = MM_CUE_PRESS;
}

static void mm_s_draw(const mainmenu_t *m, const mmframe *f)
{
    float x, y, w, h;
    int i, n = net_n_servers();

    ui_rect(0.f, 0.f, f->w, f->h, 0.f, 0.f, 0.f, 0.55f);
    mm_gbox(f, MP_DLG_CX, MP_DLG_X, MP_CDLG_Y, MP_DLG_W, MP_CDLG_H,
            &x, &y, &w, &h);
    /* AN OPAQUE PLATE, and this one is not the roster's 0.45. Those dialogs are
       a line of text and two buttons over a page the player already knows; this
       is a LIST to read, and at 0.45 the carousel's photographs showed straight
       through the rows. */
    ui_rect(x, y, w, h, 0.08f, 0.07f, 0.06f, 0.88f);
    if (m->tex.panel)
        mm_frame9(f, x, y, w, h, m->tex.panel);
    /* 42503, `Server' -- the heading the list is of. */
    mp_text(m, f, 0, px(f, MP_DLG_CX), py(f, MP_CDLG_Y + 12.f),
            MM_TS_LABEL, 1, 1.f, 1.f, 1.f, STR_UI_SERVER);
    if (n == 0) {
        /* TWO DIFFERENT STATES AND TWO DIFFERENT WORDS. Both branches of this
           used to be 42451, `Connecting... / Please wait' -- a leftover, and a
           lie while nothing has been asked yet: a browse is LISTENING, and the
           announces come twice a second. 10800 is the game's own `Please wait'
           and is what an empty list says; 42451 is for a join that is actually
           out.
         *
           AND IT IS TWO LINES. 42451 carries its own '\n' and this drew it as
           one, which put a glyph the atlas has no cell for in the middle of the
           sentence. */
        const char *say = net_mode_now() == NET_JOINING ? STR_UI_CONNECTING
                                                        : STR_UI_PLEASE_WAIT;
        float ty = MM_S_TOP + MM_S_ROW_H;
        while (*say) {
            const char *nl = strchr(say, '\n');
            size_t len = nl ? (size_t)(nl - say) : strlen(say);
            char one[64];
            if (len >= sizeof one) len = sizeof one - 1;
            memcpy(one, say, len);
            one[len] = 0;
            mp_text(m, f, 0, px(f, MP_DLG_CX), py(f, ty),
                    MM_TS_INFO, 1, 1.f, 1.f, 0.8f, one);
            ty += MM_L_LINE_H;
            if (!nl) break;
            say = nl + 1;
        }
    }
    for (i = 0; i < n; i++) {
        const net_server *s = net_server_at(i);
        char line[80];
        const int lit = m->srvsel == i;
        if (!s) continue;
        mm_s_row_rect(f, i, &x, &y, &w, &h);
        if (lit)
            ui_rect(x, y, w, h, 1.f, 1.f, 1.f, 0.14f);
        /* The host's name, the track it is on and how full it is -- which is
           dlgNETJOIN_SERVINFO's own six fields boiled down to the three a
           player picking a game needs. */
        snprintf(line, sizeof line, "%s   %s   %d/%d", s->name,
                 STR_TRACK_NAME[s->track < STR_N_TRACKS ? s->track : 0],
                 (int)s->players, (int)s->maxplayers);
        mp_text(m, f, 0, x + 8.f * f->us,
                y + (h - mp_text_h(m, f, 0, MM_TS_INFO)) * 0.5f,
                MM_TS_INFO, 0, 1.f, 1.f, 1.f, line);
    }
    mp_draw_btn(m, f, 0, 0, STR_UI_CANCEL, 1);
}

static void mm_s_step(mainmenu_t *m, unsigned int down, const touch_state *tp,
                      int screen_w, int screen_h)
{
    const int n = net_n_servers();

    if (down & (SCE_CTRL_DOWN | SCE_CTRL_RIGHT)) {
        if (n > 0) m->srvsel = (m->srvsel + 1) % n;
        m->cue = MM_CUE_FOCUS;
    }
    if (down & (SCE_CTRL_UP | SCE_CTRL_LEFT)) {
        if (n > 0) m->srvsel = (m->srvsel + n - 1) % n;
        m->cue = MM_CUE_FOCUS;
    }
    if (down & (SCE_CTRL_CROSS | SCE_CTRL_START)) {
        if (n > 0) mm_s_fire(m, m->srvsel);
        else       m->cue = MM_CUE_DENY;
    }
    if (down & SCE_CTRL_CIRCLE)
        mm_s_fire(m, NET_MAX_SERVERS);
    if (!tp)
        return;
    if (tp->pressed) {
        const int at = mm_s_at(m, screen_w, screen_h, tp->x, tp->y);
        m->marmed = at;
        if (at >= 0 && at < NET_MAX_SERVERS && at != m->srvsel) {
            m->srvsel = at;
            m->cue = MM_CUE_FOCUS;
        }
    }
    if (tp->released) {
        const int at = mm_s_at(m, screen_w, screen_h, tp->x, tp->y);
        if (at >= 0 && at == m->marmed)
            mm_s_fire(m, at);
        m->marmed = -1;
    }
}

/* ========================================== THE CHAMPIONSHIP, and both pages
 *
 * dlgCHAMP is the ladder and dlgCHRACE is the panel that takes the entry fee.
 * BOTH LAYOUTS ARE SHIPPED -- Settings/dlgCHAMP.ini and Settings/dlgCHRACE.ini,
 * through gen_dlg_data.py -- and both screens are now matched against the
 * game's own screenshots of them. champ.h is the rules; this is the two pages
 * over them, and ui.md records what each picture settled.
 *
 * THE LADDER IS IN THE ENGINE'S TRACK ORDER, which is the one thing on this
 * page that is not a rectangle and matters more than one: championship.ini's
 * Track1..10 IS the ladder, cheapest to dearest (Surf, Fishers, Fort, AAD,
 * Mines, Camping, AWACS, Rancho, Silo, War path), and TRACKS[] is not in it.
 * FUN_004bf1f0 walks 0..9 in the engine's numbering; so does this, through
 * pl_track_port(). A ladder shown in the port's own order would put the $5000
 * track fourth and read as a list rather than as a climb.
 */

/* tableMain's own five columns. dlgCHAMP.ini ships FOUR widths -- 20, 20, 17
   and 12 per cent -- for a table the engine writes five columns into, so the
   fifth (Prize) takes the remainder, 31 per cent. That is the file's
   arithmetic and not a choice made here. */
#define MM_CH_COL_N   5
static const float MM_CH_COLW[MM_CH_COL_N] = {
    DLG_CHAMP_tableMainColWidht0, DLG_CHAMP_tableMainColWidht1,
    DLG_CHAMP_tableMainColWidht2, DLG_CHAMP_tableMainColWidht3,
    1.f - (DLG_CHAMP_tableMainColWidht0 + DLG_CHAMP_tableMainColWidht1
           + DLG_CHAMP_tableMainColWidht2 + DLG_CHAMP_tableMainColWidht3)
};

/* The row marker, design px right of tableMainX0 -- dlgSTAT's own MM_ST_MARK,
   because this is the same table furniture on the same screen. */
#define MM_CH_MARK    7.f
/* How far the rule under each track name runs, as a fraction of the name
   column -- the game's screenshot underlines the NAME and not the cell. */
#define MM_CH_NAME_RULE 0.80f

const int MM_CB_ROW[MM_CB_N] = { 0, 1, 2, 3, 4, 6 };

static const char *const MM_CB_NAME[MM_CB_N] = {
    STR_UI_CHAMPIONSHIP, STR_UI_MAP_AND_INFO, STR_UI_TRACK_STATS,
    STR_UI_GARAGE, STR_UI_NEW_CHAMPIONSHIP, STR_UI_TRAINING
};

/* The ladder's row rectangle, in design pixels. Row `i' is the i-th rung
   VISIBLE, so the track it names is pl_track_port(m->ctop + i). */
static void mm_c_row_box(const mmframe *f, int i,
                         float *x, float *y, float *w, float *h)
{
    const float head = DLG_CHAMP_tableMainSY * DLG_CHAMP_tableMainHeadHeight;
    const float item = DLG_CHAMP_tableMainSY * DLG_CHAMP_tableMainItemHeight;
    *x = px(f, DLG_CHAMP_tableMainX0);
    *y = py(f, DLG_CHAMP_tableMainY0 + head + item * (float)i);
    *w = DLG_CHAMP_tableMainSX * f->us;
    *h = item * f->us;
}

/* How many rungs fit under the heading. Ten at 13% of 227 px is 295 px against
   the 190 the table has left, so the table SCROLLS -- which is why dlgCHAMP
   ships an item height at all, and the game's own shot of the page has six
   rows and a scroll bar. */
static int mm_c_shown(void)
{
    const float head = DLG_CHAMP_tableMainSY * DLG_CHAMP_tableMainHeadHeight;
    const float item = DLG_CHAMP_tableMainSY * DLG_CHAMP_tableMainItemHeight;
    int n = (int)((DLG_CHAMP_tableMainSY - head) / item);
    if (n < 1) n = 1;
    if (n > PL_N_TRACKS) n = PL_N_TRACKS;
    return n;
}

/* Keep the cursor inside the window. */
static void mm_c_scroll(mainmenu_t *m)
{
    const int shown = mm_c_shown();
    const int rung = pl_track_slot(m->csel);
    if (m->ctop > rung)                 m->ctop = rung;
    if (m->ctop < rung - (shown - 1))   m->ctop = rung - (shown - 1);
    if (m->ctop > PL_N_TRACKS - shown)  m->ctop = PL_N_TRACKS - shown;
    if (m->ctop < 0)                    m->ctop = 0;
}

void mainmenu_open_champ(mainmenu_t *m)
{
    const player_t *p;
    int i;

    if (!m)
        return;
    p = player_cur();
    m->page = MM_PAGE_CHAMP;
    m->cfocus = MM_C_LIST;
    m->carmed = -1;
    /* THE PROFILE'S OWN TRACK first -- `sel_track' is what the engine hands its
       race-setup globals the moment a profile is selected (player.h), so a
       player comes back to the rung they left. */
    m->csel = p ? pl_track_port(p->sel_track) : 0;
    if (champ_track_open(p, m->csel) != CH_OK) {
        /* ...and the HIGHEST open rung otherwise, walked in the ladder's order,
           because that is the one a player is climbing towards. A fresh profile
           has only Surf open and lands there. */
        m->csel = 0;
        for (i = 0; i < PL_N_TRACKS; i++) {
            const int t = pl_track_port(i);
            if (champ_track_open(p, t) == CH_OK)
                m->csel = t;
        }
    }
    /* THE CAROUSEL AND THE SIBLING VIEWS FOLLOW THE LADDER. dlgMAPINFO and
       dlgSTAT are on this page's own navigation column and they draw
       `m->track', so the two selections are one. */
    m->track = m->csel;
    m->qfrom = MM_PAGE_CHAMP;
    mm_c_scroll(m);
}

int mainmenu_c_live(const mainmenu_t *m, int stop)
{
    const player_t *p = player_cur();

    if (!m)
        return 0;
    /* TRAINING IS NOT BUILT. It is on screen because the game's own page has it
       and the art has a disabled look, which is the same call the main menu's
       Ghost race and Demo play rows get. */
    if (stop == MM_C_NAV + MM_CB_TRAINING)
        return 0;
    if (!p)
        return stop == MM_C_BACK;       /* nothing to race as; only the way out */
    switch (stop) {
    case MM_C_RACE:
        /* THE GREEN BUTTON IS THE LADDER'S OWN GUARD, and it is the engine's
           two: the track has to be open (FUN_004e84a0) and the cash has to
           cover the entry fee (FUN_004bea70's 40919). Both are asked again on
           the press, so this only decides whether the plate is drawn grey. */
        if (champ_track_open(p, m->csel) != CH_OK)
            return 0;
        if (p->cash < champ_fee(m->csel))
            return 0;
        /* AND A CAR TO RACE IT IN. FUN_004e03b0 refuses a race whose car is not
           enabled ("Car is not enabled: %i"). */
        return garage_owns_car(p, m->car);
    default:
        return 1;
    }
}

int mainmenu_c_stop_at(const mainmenu_t *m, int screen_w, int screen_h,
                       float x, float y, int *row)
{
    const mmframe f = mm_frame(screen_w, screen_h);
    float bx, by, bw, bh;
    int i;

    if (row)
        *row = -1;
    if (!m || m->page != MM_PAGE_CHAMP)
        return -1;
    for (i = 0; i < MM_CB_N; i++) {
        mm_bar_rect(&f, MM_CB_ROW[i], &bx, &by, &bw, &bh);
        if (touch_in(x, y, bx, by, bw, bh))
            return MM_C_NAV + i;
    }
    mm_race_rect(&f, &bx, &by, &bw, &bh);
    if (touch_in(x, y, bx, by, bw, bh))
        return MM_C_RACE;
    mm_quit_rect(&f, &bx, &by, &bw, &bh);
    if (touch_in(x, y, bx, by, bw, bh))
        return MM_C_BACK;
    /* THE TABLE IS ONE STOP AND TEN TARGETS: a touch on a rung both focuses the
       list and moves its cursor there, which is one press where the pad needs
       several. */
    for (i = 0; i < mm_c_shown(); i++) {
        const int rung = m->ctop + i;
        if (rung >= PL_N_TRACKS) break;
        mm_c_row_box(&f, i, &bx, &by, &bw, &bh);
        if (touch_in(x, y, bx, by, bw, bh)) {
            if (row) *row = rung;
            return MM_C_LIST;
        }
    }
    return -1;
}

/* Open dlgCHRACE, the fee panel, over the ladder. */
static void mm_c_open_race(mainmenu_t *m)
{
    m->page = MM_PAGE_CHRACE;
    m->rfocus = MM_R_RACE;
    m->rarmed = -1;
    m->cue = MM_CUE_PRESS;
}

/* The three questions the green button asks, in FUN_004bea70's own order:
   the track, then the car, then the money. Returns CH_OK or says why. */
static ch_result mm_c_can_race(const mainmenu_t *m, const player_t *p)
{
    ch_result r = champ_track_open(p, m->csel);
    if (r != CH_OK)
        return r;
    if (!garage_owns_car(p, m->car))
        return CH_NO_CAR;
    if (p->cash < champ_fee(m->csel))
        return CH_NO_MONEY;
    return CH_OK;
}

static void mm_c_fire(mainmenu_t *m, int stop)
{
    const player_t *p = player_cur();
    ch_result r;

    if (stop >= MM_C_NAV && stop < MM_C_NAV + MM_CB_N) {
        m->press_row = MM_CB_ROW[stop - MM_C_NAV];
        m->press_t = 0.f;
    }
    if (!mainmenu_c_live(m, stop)) {
        m->cue = MM_CUE_DENY;
        return;
    }
    switch (stop) {
    case MM_C_LIST:
        /* CROSS on the ladder is the same as pressing Race on the rung the
           cursor is on -- the list has nothing else to do with a press. */
        mm_c_fire(m, MM_C_RACE);
        return;
    case MM_C_RACE:
        r = mm_c_can_race(m, p);
        if (r != CH_OK) {
            mg_say(m, r == CH_NO_CAR ? garage_reason(GAR_NOT_OWNED)
                                     : champ_reason(r));
            m->cue = MM_CUE_DENY;
            return;
        }
        /* ON TO dlgCHRACE, which is where the money actually moves. */
        mm_c_open_race(m);
        return;
    case MM_C_NAV + MM_CB_CHAMP:
        m->cue = MM_CUE_PRESS;          /* already here */
        return;
    case MM_C_NAV + MM_CB_MAPINFO:
    case MM_C_NAV + MM_CB_STATS:
        /* THE TWO SIBLING VIEWS, on the ladder's own track and knowing the way
           home -- `qfrom'. They are the same dlgMAPINFO and dlgSTAT the
           quick-race page navigates to; what changes is the first nav bar, the
           header and what the green button does. */
        m->track = m->csel;
        m->qfrom = MM_PAGE_CHAMP;
        m->page = (stop == MM_C_NAV + MM_CB_MAPINFO) ? MM_PAGE_MAPINFO
                                                     : MM_PAGE_STATS;
        m->qfocus = MM_Q_NAV + ((stop == MM_C_NAV + MM_CB_MAPINFO)
                                ? MM_QB_MAPINFO : MM_QB_STATS);
        m->cue = MM_CUE_PRESS;
        return;
    case MM_C_NAV + MM_CB_GARAGE:
        /* THE SAME GARAGE the quick-race page opens, on the same profile and
           the same prices -- which is the whole point of prize money. Its Back
           button comes here, because that is the page that opened it. */
        mainmenu_open_garage(m, -1);
        m->cue = MM_CUE_PRESS;
        return;
    case MM_C_NAV + MM_CB_NEW:
        /* 40927 first, and the question is the game's own: this erases a
           ladder. */
        mg_ask(m, MG_ASK_NEW_CHAMP, STR_UI_CH_ERASE_ASK);
        m->cue = MM_CUE_PRESS;
        return;
    case MM_C_BACK:
        m->page = MM_PAGE_MAIN;
        m->focus = MM_CHAMPIONSHIP;
        m->qfrom = MM_PAGE_QUICK;
        m->cue = MM_CUE_PRESS;
        return;
    default:
        m->cue = MM_CUE_DENY;
        return;
    }
}

static void mm_c_move(mainmenu_t *m, int d)
{
    int rung = pl_track_slot(m->csel) + d;
    if (rung < 0) rung = PL_N_TRACKS - 1;
    if (rung >= PL_N_TRACKS) rung = 0;
    m->csel = pl_track_port(rung);
    m->track = m->csel;
    mm_c_scroll(m);
    m->cue = MM_CUE_FOCUS;
}

/* The next live stop of the ring, skipping Training the way the main menu's own
   ring skips its two grey rows. */
static int mm_c_next(const mainmenu_t *m, int from, int d)
{
    int i, k = from;
    for (i = 0; i < MM_C_N_FOCUS; i++) {
        k += d;
        if (k < 0) k = MM_C_N_FOCUS - 1;
        if (k >= MM_C_N_FOCUS) k = 0;
        if (mainmenu_c_live(m, k))
            return k;
    }
    return from;
}

static void mm_step_champ(mainmenu_t *m, unsigned int down,
                          const touch_state *tp, int screen_w, int screen_h)
{
    /* UP and DOWN walk the LADDER while the list has the focus and the focus
       ring otherwise, which is what makes ten rungs cost ten presses instead of
       ten plus the ring. LEFT and RIGHT are the ring either way. */
    if (m->cfocus == MM_C_LIST) {
        if (down & SCE_CTRL_DOWN) mm_c_move(m, +1);
        if (down & SCE_CTRL_UP)   mm_c_move(m, -1);
    } else {
        if (down & SCE_CTRL_DOWN) {
            m->cfocus = mm_c_next(m, m->cfocus, +1);
            m->cue = MM_CUE_FOCUS;
        }
        if (down & SCE_CTRL_UP) {
            m->cfocus = mm_c_next(m, m->cfocus, -1);
            m->cue = MM_CUE_FOCUS;
        }
    }
    if (down & SCE_CTRL_RIGHT) {
        m->cfocus = mm_c_next(m, m->cfocus, +1);
        m->cue = MM_CUE_FOCUS;
    }
    if (down & SCE_CTRL_LEFT) {
        m->cfocus = mm_c_next(m, m->cfocus, -1);
        m->cue = MM_CUE_FOCUS;
    }
    if (down & (SCE_CTRL_CROSS | SCE_CTRL_START))
        mm_c_fire(m, m->cfocus);
    if (down & SCE_CTRL_CIRCLE) {
        mm_c_fire(m, MM_C_BACK);
        return;
    }

    if (!tp)
        return;
    /* THE BAR FIRST, and it EATS the touch: it stands beside the table's rows
       and a finger on it must not select the rung it is level with. */
    {
        const mmframe f = mm_frame(screen_w, screen_h);
        const float head = DLG_CHAMP_tableMainSY * DLG_CHAMP_tableMainHeadHeight;
        const int was = m->ctop;
        if (mm_sb_drive(&f, tp,
                        DLG_CHAMP_tableMainX0 + DLG_CHAMP_tableMainSX
                        + MM_SB_GAP,
                        DLG_CHAMP_tableMainY0 + head,
                        DLG_CHAMP_tableMainY0 + DLG_CHAMP_tableMainSY,
                        &m->ctop, PL_N_TRACKS, mm_c_shown(), &m->sb_drag)) {
            if (m->ctop != was)
                m->cue = MM_CUE_FOCUS;
            m->carmed = -1;
            return;
        }
    }
    if (tp->pressed) {
        int rung;
        m->carmed = mainmenu_c_stop_at(m, screen_w, screen_h, tp->x, tp->y,
                                       &rung);
        if (m->carmed >= 0 && m->cfocus != m->carmed
            && mainmenu_c_live(m, m->carmed)) {
            m->cfocus = m->carmed;
            m->cue = MM_CUE_FOCUS;
        }
        if (rung >= 0) {
            const int t = pl_track_port(rung);
            if (t != m->csel) {
                m->csel = t;
                m->track = t;
                m->cue = MM_CUE_FOCUS;
            }
        }
    }
    if (tp->released) {
        int rung;
        const int at = mainmenu_c_stop_at(m, screen_w, screen_h, tp->x, tp->y,
                                          &rung);
        /* A TAP ON A RUNG SELECTS IT AND DOES NOT START IT. The carousel's own
           rule applied to a list whose press is worth money: the green button
           is what commits, and it is one thumb-width away. */
        if (at >= 0 && at == m->carmed && at != MM_C_LIST)
            mm_c_fire(m, at);
        m->carmed = -1;
    }
}

/* ------------------------------------------------------ dlgCHRACE, the fee */

/* THIS WHOLE PANEL IS ONE GROUP, and that is what was wrong with it.
 *
 * Every x on this page mapped through px() -- the stretch -- while every width
 * scaled with us. At 800x600 the two agree and the panel is right; on a 960x544
 * screen rectFrame started at px(10) = 12 and ran 782 * 0.907 = 709 wide, so the
 * dialog sat 113 px left of the screen's centre with its table spread wider than
 * the frame around it. It is the group rule mainmenu.h already states, applied
 * to the one page that never had it: ONE anchor -- rectFrame's own centre, which
 * is 401 and therefore the screen's -- and everything else at its design offset
 * from that, times us. See gx().
 *
 * The anchor is the frame's centre and NOT 400, because 401 is what the file
 * ships and the two buttons are placed symmetrically about it. */
#define MM_R_AX (DLG_CHRACE_rectFrameX0 + DLG_CHRACE_rectFrameSX * 0.5f)
static float rx(const mmframe *f, float x) { return gx(f, MM_R_AX, x); }

/* Its own two buttons, which no other page in this front end has: buttonRace
   and buttonBack, at the rectangles the dialog ships. */
static void mm_r_rect(const mmframe *f, int stop,
                      float *x, float *y, float *w, float *h)
{
    if (stop == MM_R_BACK)
        mm_gbox(f, MM_R_AX, DLG_CHRACE_buttonBackX0, DLG_CHRACE_buttonBackY0,
                DLG_CHRACE_buttonBackSX, DLG_CHRACE_buttonBackSY, x, y, w, h);
    else
        mm_gbox(f, MM_R_AX, DLG_CHRACE_buttonRaceX0, DLG_CHRACE_buttonRaceY0,
                DLG_CHRACE_buttonRaceSX, DLG_CHRACE_buttonRaceSY, x, y, w, h);
}

int mainmenu_r_stop_at(const mainmenu_t *m, int screen_w, int screen_h,
                       float x, float y)
{
    const mmframe f = mm_frame(screen_w, screen_h);
    float bx, by, bw, bh;
    int i;

    if (!m || m->page != MM_PAGE_CHRACE)
        return -1;
    for (i = 0; i < MM_R_N_FOCUS; i++) {
        mm_r_rect(&f, i, &bx, &by, &bw, &bh);
        if (touch_in(x, y, bx, by, bw, bh))
            return i;
    }
    return -1;
}

int mainmenu_r_live(const mainmenu_t *m, int stop)
{
    if (!m)
        return 0;
    if (stop == MM_R_RACE)
        return mainmenu_c_live(m, MM_C_RACE);
    return 1;
}

static void mm_r_fire(mainmenu_t *m, int stop)
{
    player_t *p = player_cur();
    ch_result r;

    if (stop == MM_R_BACK) {
        m->page = MM_PAGE_CHAMP;
        m->cfocus = MM_C_RACE;
        m->cue = MM_CUE_PRESS;
        return;
    }
    /* THE FEE IS TAKEN HERE, on the button the original takes it on
       (FUN_004c0300, control 0xce4) -- and only after all three questions have
       been asked again, because the Garage can have moved the cash since the
       ladder drew this panel. */
    r = mm_c_can_race(m, p);
    if (r == CH_OK)
        r = champ_pay_fee(p, m->csel);
    if (r != CH_OK) {
        mg_say(m, r == CH_NO_CAR ? garage_reason(GAR_NOT_OWNED)
                                 : champ_reason(r));
        m->cue = MM_CUE_DENY;
        return;
    }
    /* THE TRACK THE RACE RUNS ON IS THE LADDER'S, not the carousel's -- so the
       carousel follows, the way it does for every other way into a race in this
       app (main.c, MM_ACT_RACE). */
    m->track = m->csel;
    m->action = MM_ACT_CHAMP_RACE;
    m->cue = MM_CUE_PRESS;
}

static void mm_step_chrace(mainmenu_t *m, unsigned int down,
                           const touch_state *tp, int screen_w, int screen_h)
{
    if (down & (SCE_CTRL_DOWN | SCE_CTRL_UP | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) {
        m->rfocus = m->rfocus == MM_R_RACE ? MM_R_BACK : MM_R_RACE;
        m->cue = MM_CUE_FOCUS;
    }
    if (down & (SCE_CTRL_CROSS | SCE_CTRL_START))
        mm_r_fire(m, m->rfocus);
    if (down & SCE_CTRL_CIRCLE) {
        mm_r_fire(m, MM_R_BACK);
        return;
    }
    if (!tp)
        return;
    if (tp->pressed) {
        m->rarmed = mainmenu_r_stop_at(m, screen_w, screen_h, tp->x, tp->y);
        if (m->rarmed >= 0 && m->rfocus != m->rarmed) {
            m->rfocus = m->rarmed;
            m->cue = MM_CUE_FOCUS;
        }
    }
    if (tp->released) {
        const int at = mainmenu_r_stop_at(m, screen_w, screen_h, tp->x, tp->y);
        if (at >= 0 && at == m->rarmed)
            mm_r_fire(m, at);
        m->rarmed = -1;
    }
}

/* ---------------------------------------------------------------- the draw */

/* One cell of a table, centred in its column. The stats table does this inline
   three times; both championship tables need it five and six times, so it is a
   helper here rather than a fourth copy. `x' and `w' are design pixels. */
/* `ax' is the GROUP this cell belongs to -- see gx(). A page whose controls all
   map through px() passes its own x and gets px(x) back, which is what the
   plain mm_cell below does; dlgCHRACE is one panel and passes the panel's
   centre, so its columns keep their spacing instead of spreading. */
static void mm_cell_at(const mainmenu_t *m, const mmframe *f, float ax,
                       float x, float w, float y, float ts, float alpha,
                       const char *s)
{
    const sfont sf = sf_small(m->tex.font_small);
    const float sc = f->us * ts;
    const float tw = sf.tex ? sf_w(&sf, sc, s) : ui_text_w(sc, s);
    const float tx = gx(f, ax, x) + (w * f->us - tw) * 0.5f;
    if (sf.tex)
        sf_text_shadowed(&sf, tx, py(f, y), sc, 1.f, 1.f, 1.f, alpha, s);
    else
        ui_text(tx, py(f, y), sc, 1.f, 1.f, 1.f, alpha, s);
}

static void mm_cell(const mainmenu_t *m, const mmframe *f, float x, float w,
                    float y, float ts, float alpha, const char *s)
{
    mm_cell_at(m, f, x, x, w, y, ts, alpha, s);
}

/* A line centred on `cx', in design pixels -- what the fee panel's headings and
   its two money lines are, and what the frame's own right-hand column never
   needs. */
static void mm_ctext_at(const mainmenu_t *m, const mmframe *f, int big,
                        float ax, float cx, float y, float ts, const char *s)
{
    const sfont sf = big ? sf_big(m->tex.font_big) : sf_small(m->tex.font_small);
    const float sc = f->us * ts;
    const float tw = sf.tex ? sf_w(&sf, sc, s) : ui_text_w(sc, s);
    const float tx = gx(f, ax, cx) - tw * 0.5f;
    if (sf.tex)
        sf_text_shadowed(&sf, tx, py(f, y), sc, 1.f, 1.f, 1.f, 1.f, s);
    else
        ui_text(tx, py(f, y), sc, 1.f, 1.f, 1.f, 1.f, s);
}


/* The left edge of column `c', in design pixels. */
static float mm_ch_col(int c)
{
    float x = DLG_CHAMP_tableMainX0;
    int i;
    for (i = 0; i < c && i < MM_CH_COL_N; i++)
        x += DLG_CHAMP_tableMainSX * MM_CH_COLW[i];
    return x;
}

/* THE RIGHT-HAND COLUMN, six bars on the frame's own rows -- and three kinds of
   plate, which is what the game's own screenshot of this page has: RADIO cells
   on the page and its two sibling views, the RED cell with an arrow on the
   Garage because it leads to a screen of its own, and the ORANGE Button_back
   plate on New championship and Training. */
static void mm_c_draw_bars(const mainmenu_t *m, const mmframe *f)
{
    int i;
    for (i = 0; i < MM_CB_N; i++) {
        float bx, by, bw, bh, v;
        const int row = MM_CB_ROW[i];
        const int live = mainmenu_c_live(m, MM_C_NAV + i);
        const int here = (i == MM_CB_CHAMP);
        const int lit = live && m->cfocus == MM_C_NAV + i;
        const int orange = (i == MM_CB_NEW || i == MM_CB_TRAINING);
        float slide = here ? MM_SLIDE * MM_SETTLED : 0.f;

        if (lit && MM_SLIDE * MM_SETTLED > slide)
            slide = MM_SLIDE * MM_SETTLED;
        mm_draw_wedge(m, f, row);
        mm_bar_draw_rect(m, f, row, slide, &bx, &by, &bw, &bh);

        if (orange && m->tex.back) {
            /* Button_back: orange, focused, disabled -- 32 rows each of 128. */
            v = live ? (lit ? 32.f / 128.f : 0.f) : 64.f / 128.f;
            ui_image(bx, by, bw, bh, m->tex.back,
                     0.f, v, 1.f, v + 32.f / 128.f, 1.f, 1.f, 1.f, 1.f);
        } else if (!orange && i != MM_CB_GARAGE && m->tex.radio) {
            v = here ? MM_V_RAD_ON : MM_V_RAD;
            ui_image(bx, by, bw, bh, m->tex.radio,
                     0.f, v, 1.f, v + MM_V_RAD_CELL, 1.f, 1.f, 1.f, 1.f);
        } else if (m->tex.buttons) {
            v = live ? (lit ? MM_V_RED_F : MM_V_RED) : MM_V_GREY;
            ui_image(bx, by, bw, bh, m->tex.buttons,
                     0.f, v, 1.f, v + MM_V_CELL, 1.f, 1.f, 1.f, 1.f);
        } else {
            ui_rect(bx, by, bw, bh, live ? 0.72f : 0.45f,
                    live ? 0.09f : 0.45f, live ? 0.11f : 0.47f, 1.f);
        }
        mm_label(m, f, bx, by, bw, bh, MM_CB_NAME[i],
                 live ? 1.f : 0.82f, live ? 1.f : 0.82f, live ? 1.f : 0.84f);
    }
}

static void mm_draw_champ(const mainmenu_t *m, const mmframe *f)
{
    const player_t *p = player_cur();
    const float head = DLG_CHAMP_tableMainSY * DLG_CHAMP_tableMainHeadHeight;
    const float item = DLG_CHAMP_tableMainSY * DLG_CHAMP_tableMainItemHeight;
    static const char *const HEAD[MM_CH_COL_N] = {
        STR_UI_COL_TRACK, STR_UI_COL_SCORES_REQ, STR_UI_COL_COST,
        STR_UI_COL_PLACE, STR_UI_COL_PRIZE
    };
    char line[128], money[24], m2[24], m3[24];
    int i, c;
    const int shown = mm_c_shown();

    mm_c_draw_bars(m, f);
    /* THE PLAYER CARD, and it is dlgPLRSCOMM's own -- the game's screenshot of
       this page has the portrait, the name over its rule, Rank / Current car /
       Play time, and `Scores: n  Cash: $n' under them, which is that card with
       both money lines rather than the multiplayer page's Scores alone. The
       ladder needs it more than any other page does: the scores column it is
       read against is on the card and nowhere else. */
    mp_draw_card_at(m, f, 0);

    /* the heading, and the rule under it */
    for (c = 0; c < MM_CH_COL_N; c++)
        mm_cell(m, f, mm_ch_col(c), DLG_CHAMP_tableMainSX * MM_CH_COLW[c],
                DLG_CHAMP_tableMainY0 + (head - MM_LINE_H) * 0.5f,
                MM_TS_LABEL, 1.f, HEAD[c]);
    mm_rule_at(f, px(f, DLG_CHAMP_tableMainX0),
               py(f, DLG_CHAMP_tableMainY0 + head),
               DLG_CHAMP_tableMainSX * f->us);

    /* THE RUNGS, in the ladder's own order. A locked one is drawn at the same
       0.82 grey the main menu's dead rows use -- shown rather than hidden,
       because what a ladder has to say is how far up it goes. */
    for (i = 0; i < shown; i++) {
        const int rung = m->ctop + i;
        const int t = pl_track_port(rung);
        const int open = champ_track_open(p, t) == CH_OK;
        const float ry = DLG_CHAMP_tableMainY0 + head + item * (float)i;
        const float ty = ry + (item - MM_LINE_H) * 0.5f;
        const float a = open ? 1.f : 0.55f;
        const int best = champ_best_place(p, t);
        const float namew = DLG_CHAMP_tableMainSX * MM_CH_COLW[0];

        if (rung >= PL_N_TRACKS)
            break;
        /* A MARKER ON EVERY ROW, which is what the picture has: the game's own
           enumarrows cell, RED on the rung the cursor is on and the grey one on
           the rest. dlgSTAT's table marks one row; this marks all ten, because
           on this page the marker is a cursor and not a label. */
        mm_arrow(m, px(f, DLG_CHAMP_tableMainX0 + MM_CH_MARK),
                 py(f, ry + item * 0.5f) - MM_Q_BULLET * f->us * 0.5f,
                 MM_Q_BULLET * f->us, (t == m->csel) ? 0 : 2, 0);

        mm_cell(m, f, mm_ch_col(0), namew, ty, MM_TS_INFO, a,
                STR_TRACK_NAME[t]);
        /* and the rule under the NAME, which the screenshot draws per row */
        mm_rule_at(f, px(f, mm_ch_col(0) + namew * (1.f - MM_CH_NAME_RULE) * 0.5f),
                   py(f, ry + item - 4.f), namew * MM_CH_NAME_RULE * f->us);
        /* SCORES REQ., which the retail exe leaves blank. Its own column
           heading is 40901 "Scores req." and FUN_004bf1f0 writes "" into it;
           the number is what the heading promises and what the player needs to
           read the ladder at all, so this port fills it. See ui.md. */
        snprintf(line, sizeof line, "%d", champ_scores_req(t));
        mm_cell(m, f, mm_ch_col(1), DLG_CHAMP_tableMainSX * MM_CH_COLW[1],
                ty, MM_TS_INFO, a, line);
        garage_cash(money, sizeof money, champ_fee(t));
        mm_cell(m, f, mm_ch_col(2), DLG_CHAMP_tableMainSX * MM_CH_COLW[2],
                ty, MM_TS_INFO, a, money);
        /* THE PLACE COLUMN HAS THREE STATES, and they are FUN_004bf1f0's own
           three: `---' (40905) for a rung that is not open, `n/a' (40906) for
           one that is open and has no placing on it, and the placing itself.
           The game's own shot of a fresh profile is exactly this -- Surf `n/a'
           and every rung below it `---'. */
        mm_cell(m, f, mm_ch_col(3), DLG_CHAMP_tableMainSX * MM_CH_COLW[3],
                ty, MM_TS_INFO, a,
                !open ? STR_UI_DASH
                      : (best >= 0 ? champ_place_name(best) : STR_UI_NA));
        /* THE PRIZES, three figures in one cell -- and BARE, with no currency
           mark on them, which is what the game's own screenshot of this table
           has ("50/30/15" against the Cost column's "$0"). FUN_004bf1f0 writes
           this cell as a plain "%i %i %i" while every other money figure on the
           page goes through the currency formatter, and the picture agrees. */
        snprintf(line, sizeof line, "%d/%d/%d", champ_prize(t, 0),
                 champ_prize(t, 1), champ_prize(t, 2));
        mm_cell(m, f, mm_ch_col(4), DLG_CHAMP_tableMainSX * MM_CH_COLW[4],
                ty, MM_TS_INFO, a, line);
    }

    /* THE SCROLL BAR, because six of the ten rungs fit and the file says so:
       dlgCHAMP's own item height leaves room for six, and the engine gives this
       table a scroll rate of its own (FUN_004bc180 on control 0x776). The
       game's own screenshot has one in the same place. */
    mm_draw_scrollbar(m, f, DLG_CHAMP_tableMainX0 + DLG_CHAMP_tableMainSX
                            + MM_SB_GAP,
                      DLG_CHAMP_tableMainY0 + head,
                      DLG_CHAMP_tableMainY0 + DLG_CHAMP_tableMainSY,
                      m->ctop, PL_N_TRACKS, shown);

    /* staticMapName and staticMapInfo -- the chosen rung's name over its own
       rule, then the three lines FUN_004bed50 writes: what it takes to enter
       (40930), what it pays (40931) and where the profile stands on it
       (40932/3/4/5). The game's screenshot RIGHT-ALIGNS all three against the
       table's own right edge, which is what puts them clear of the green Race
       plate this front end keeps in that corner. */
    mm_rule_at(f, px(f, DLG_CHAMP_tableMainX0),
               py(f, DLG_CHAMP_staticMapNameY0 - 6.f),
               DLG_CHAMP_tableMainSX * f->us);
    mm_q_text(m, f, 1, DLG_CHAMP_staticMapNameX0, DLG_CHAMP_staticMapNameY0,
              MM_TS_TRACK, 0, 1.f, STR_TRACK_NAME[m->csel]);
    {
        const float rgt = DLG_CHAMP_tableMainX0 + DLG_CHAMP_tableMainSX;
        float y = DLG_CHAMP_staticMapInfoY0;
        mm_rule_at(f, px(f, DLG_CHAMP_tableMainX0), py(f, y - 6.f),
                   DLG_CHAMP_tableMainSX * f->us);
        garage_cash(money, sizeof money, champ_fee(m->csel));
        snprintf(line, sizeof line, STR_UI_CH_SCORES_FEE,
                 champ_scores_req(m->csel), money);
        mm_q_text(m, f, 0, rgt, y, MM_TS_INFO, 1, 1.f, line);
        y += MM_LINE_H;
        garage_cash(money, sizeof money, champ_prize(m->csel, 0));
        garage_cash(m2, sizeof m2, champ_prize(m->csel, 1));
        garage_cash(m3, sizeof m3, champ_prize(m->csel, 2));
        snprintf(line, sizeof line, STR_UI_CH_PRIZES, money, m2, m3);
        mm_q_text(m, f, 0, rgt, y, MM_TS_INFO, 1, 1.f, line);
        y += MM_LINE_H;
        champ_status(p, m->csel, line, sizeof line);
        mm_q_text(m, f, 0, rgt, y, MM_TS_INFO, 1, 1.f, line);
    }
}

/* dlgCHRACE -- THE GRID, AND IT IS SIX. The game's own screenshot of this page
 * has the player and FIVE opponents, named Doc, BabyShark, Rosy, Da killa and
 * Johny with Doc's engine at level 1 and BabyShark's tyres at level 1, which is
 * AI_RACES[beach_1]'s five entries cell for cell. ai.h carries the correction
 * that came out of it -- the field size is the DIFFICULTY's, three at easy and
 * five at hard, and AI_MAX_FIELD was capping it at three.
 *
 * And the dialog's own table agrees arithmetically: tableSY 318 with a 10%
 * head and 15% items is 31.8 + 6 x 47.7 = 318.0 exactly. It is sized for six
 * rows and no others.
 *
 * The five columns are the dialog's own headings, 41405..41409 -- and note the
 * ORDER, Player / Car / Engine / Booster / Tires, which is NOT pl_car.up[]'s.
 * Every row below fills its `lv[3]' in THAT order, engine first.
 */
static const float MM_R_COLW[5] = {
    DLG_CHRACE_tableColWidht0, DLG_CHRACE_tableColWidht1,
    DLG_CHRACE_tableColWidht2, DLG_CHRACE_tableColWidht3,
    DLG_CHRACE_tableColWidht4
};

/* A car's SHORT code, which is what the game's own table prints: RR, TB, WH --
   the last word of each name in the string table ("Road Rage RR", "Tornado
   Buggy TB", "Warhammer WH"). Taken off the name rather than kept as a second
   table, so a translated build cannot disagree with itself. */
static const char *mm_r_car_code(int car)
{
    const char *s, *sp;
    if (car < 0 || car >= MM_N_CARS)
        return STR_UI_NA;
    s = STR_CAR_NAME[car];
    sp = strrchr(s, ' ');
    return sp ? sp + 1 : s;
}

/* One row of the fee table: a portrait, a name over its rule, the car code and
   the three part levels. `face' is 0 for none. */
static void mm_r_row(const mainmenu_t *m, const mmframe *f, float ry,
                     float item, unsigned int face, const char *name,
                     int car, const int lv[3], int mine)
{
    const float morda = DLG_CHRACE_tableSX * DLG_CHRACE_tableMordaShift;
    const float ty = ry + (item - MM_LINE_H) * 0.5f;
    float x = DLG_CHRACE_tableX0;
    char line[64];
    int c;

    /* the cursor marker, then the portrait in tableMordaShift's own column --
       `morda' is the artists' word for the face, and the key is what says this
       table has a picture column at all */
    mm_arrow(m, rx(f, x), py(f, ry + item * 0.5f) - MM_Q_BULLET * f->us * 0.5f,
             MM_Q_BULLET * f->us, mine ? 0 : 2, 0);
    if (face)
        ui_image(rx(f, x + MM_Q_BULLET + 2.f), py(f, ry + 2.f),
                 (morda - MM_Q_BULLET - 4.f) * f->us, (item - 4.f) * f->us,
                 face, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f);
    x += morda;

    mm_cell_at(m, f, MM_R_AX, x, DLG_CHRACE_tableSX * MM_R_COLW[0], ty,
               MM_TS_INFO, 1.f, name);
    mm_rule_at(f, rx(f, x), py(f, ry + item - 6.f),
               DLG_CHRACE_tableSX * MM_R_COLW[0] * f->us);
    x += DLG_CHRACE_tableSX * MM_R_COLW[0];
    mm_cell_at(m, f, MM_R_AX, x, DLG_CHRACE_tableSX * MM_R_COLW[1], ty,
               MM_TS_INFO, 1.f, mm_r_car_code(car));
    x += DLG_CHRACE_tableSX * MM_R_COLW[1];
    for (c = 0; c < 3; c++) {
        /* 41410 "level %i" and 41411 "none" -- the dialog's own two words for a
           part that is fitted and one that is not. */
        if (lv[c] > 0)
            snprintf(line, sizeof line, STR_UI_CH_LEVEL, lv[c]);
        else
            snprintf(line, sizeof line, "%s", STR_UI_CH_NONE);
        mm_cell_at(m, f, MM_R_AX, x, DLG_CHRACE_tableSX * MM_R_COLW[c + 2], ty,
                   MM_TS_INFO, 1.f, line);
        x += DLG_CHRACE_tableSX * MM_R_COLW[c + 2];
    }
}

/* One of the two centred buttons at the foot of the panel. `cell_v` picks the
   row of its own atlas. */
static void mm_r_button(const mainmenu_t *m, const mmframe *f, int stop,
                        int live, const char *label)
{
    float bx, by, bw, bh;
    const int lit = m->rfocus == stop;
    const float g = live ? 1.f : 0.82f;

    mm_r_rect(f, stop, &bx, &by, &bw, &bh);
    /* THE SAME PAIR EVERY DIALOG IN THIS FRONT END DRAWS -- `messagebox's own
       red-and-orange, whole. See mp_draw_btn: ButtonsTextures' RED cells carry a
       triangle and Button_back a curled return arrow, and this panel wore one of
       each. A dead Race keeps the fallback's dotted grey, which `messagebox' has
       no cell for. */
    if (live && mm_msgbtn(m, bx, by, bw, bh, stop == MM_R_BACK, lit)) {
        /* drawn */
    } else if (stop == MM_R_BACK) {
        if (m->tex.buttons) {
            const float v = lit ? MM_V_ORANGE_F : MM_V_ORANGE;
            mm_bar3(bx, by, bw, bh, m->tex.buttons, v, v + MM_V_CELL);
        } else {
            ui_rect(bx, by, bw, bh, 0.90f, 0.55f, 0.04f, 1.f);
        }
    } else if (live && m->tex.radio) {
        /* RED, not the green Race plate -- the game's own shot of this panel
           has the ordinary red bar here, because this button is one of a pair
           inside a dialog and not the frame's own corner. */
        const float v = lit ? MM_V_RAD_ON : MM_V_RAD;
        mm_bar3(bx, by, bw, bh, m->tex.radio, v, v + MM_V_RAD_CELL);
    } else if (m->tex.buttons) {
        /* dead: ButtonsTextures' DOTTED grey, so the cap does not change shape
           when the rung cannot be entered (see MM_V_GREY_DOT). */
        const float v = live ? MM_V_RED : MM_V_GREY_DOT;
        mm_bar3(bx, by, bw, bh, m->tex.buttons, v, v + MM_V_CELL);
    } else {
        ui_rect(bx, by, bw, bh, live ? 0.72f : 0.45f,
                live ? 0.09f : 0.45f, live ? 0.11f : 0.47f, 1.f);
    }
    /* CENTRED in the plate rather than mm_label's right-aligned, because these
       two are a centred pair and not a row of the right-hand column. */
    {
        const sfont sf = sf_small(m->tex.font_small);
        const float sc = f->us * MM_TS_LABEL;
        const float tw = sf.tex ? sf_w(&sf, sc, label) : ui_text_w(sc, label);
        const float th = sf.tex ? sf_h(&sf, sc) : ui_text_h(sc);
        if (sf.tex)
            sf_text(&sf, bx + (bw - tw) * 0.5f, by + (bh - th) * 0.5f, sc,
                    g, g, g, 1.f, label);
        else
            ui_text(bx + (bw - tw) * 0.5f, by + (bh - th) * 0.5f, sc,
                    g, g, g, 1.f, label);
    }
}

static void mm_draw_chrace(const mainmenu_t *m, const mmframe *f)
{
    const player_t *p = player_cur();
    static const char *const HEAD[5] = {
        STR_UI_COL_PLAYER, STR_UI_CAR, STR_UI_ENGINE, STR_UI_BOOSTER,
        STR_UI_TIRES
    };
    const float head = DLG_CHRACE_tableSY * DLG_CHRACE_tableHeadHeight;
    const float item = DLG_CHRACE_tableSY * DLG_CHRACE_tableItemHeight;
    const float morda = DLG_CHRACE_tableSX * DLG_CHRACE_tableMordaShift;
    const int fee = champ_fee(m->csel);
    const ai_race *r = &AI_RACES[m->csel];
    int mask = 1 << (m->skill < 0 ? 0 : m->skill);
    char line[128], money[24];
    float x, y;
    int c, i, n = 0, lv[3];

    if (mask > AI_RACE_HARD)
        mask = AI_RACE_HARD;

    /* THE PANEL IS AN OVERLAY. The game's own screenshot has the ladder still
       there underneath, dimmed -- so mainmenu_draw draws dlgCHAMP first and
       this dims the lot and stands rectFrame on top of it. */
    ui_rect(0.f, 0.f, f->w, f->h, 0.f, 0.f, 0.f, 0.55f);
    ui_rect(rx(f, DLG_CHRACE_rectFrameX0), py(f, DLG_CHRACE_rectFrameY0),
            DLG_CHRACE_rectFrameSX * f->us, DLG_CHRACE_rectFrameSY * f->us,
            0.f, 0.f, 0.f, 0.72f);

    /* THE TWO HEADINGS, centred over the table: `Track: "Surf"' and the prize.
       The dialog's own staticHeader (66, 73) and staticPrize are the y's; the
       centring is the picture's, and known-issues.md says why those four
       `static*' rectangles cannot be read literally. */
    snprintf(line, sizeof line, "%s: \"%s\"", STR_UI_CH_TRACK,
             STR_TRACK_NAME[m->csel]);
    mm_ctext_at(m, f, 1, MM_R_AX, MM_R_AX,
                DLG_CHRACE_staticHeaderY0 - MM_LINE_H, MM_TS_TRACK, line);
    garage_cash(money, sizeof money, champ_prize(m->csel, 0));
    snprintf(line, sizeof line, "%s: %s", STR_UI_CH_PRIZE, money);
    mm_ctext_at(m, f, 0, MM_R_AX, MM_R_AX,
                DLG_CHRACE_staticHeaderY0 + 4.f, MM_TS_INFO, line);

    /* the table's heading and its rule */
    x = DLG_CHRACE_tableX0 + morda;
    for (c = 0; c < 5; c++) {
        const float w = DLG_CHRACE_tableSX * MM_R_COLW[c];
        mm_cell_at(m, f, MM_R_AX, x, w,
                   DLG_CHRACE_tableY0 + (head - MM_LINE_H) * 0.5f,
                   MM_TS_LABEL, 1.f, HEAD[c]);
        x += w;
    }
    mm_rule_at(f, rx(f, DLG_CHRACE_tableX0), py(f, DLG_CHRACE_tableY0 + head),
               DLG_CHRACE_tableSX * f->us);

    /* THE PLAYER'S ROW FIRST, then the field this track fields at this
       difficulty -- the same mask ai.c loads them with, so the table promises
       exactly the grid that turns up. */
    y = DLG_CHRACE_tableY0 + head;
    lv[0] = garage_level(p, GAR_ENGINE, m->car);
    lv[1] = garage_level(p, GAR_BOOSTER, m->car);
    lv[2] = garage_level(p, GAR_TIRES, m->car);
    mm_r_row(m, f, y, item, p ? m->tex.face[p->face >= 0
                                           && p->face < PL_N_FACES
                                           ? p->face : 0] : 0,
             p ? p->name : STR_UI_DEFAULT_NAME, m->car, lv, 1);
    y += item;
    for (i = 0; i < r->n && n < AI_MAX_OPPONENTS; i++) {
        const ai_opponent *o = &r->op[i];
        const ai_player *ap;
        unsigned int face = 0;
        int j;
        if (!(o->races & mask))
            continue;
        n++;
        if (o->ref < 1 || o->ref > AI_N_PLAYERS)
            continue;
        ap = &AI_PLAYERS[o->ref - 1];
        /* THE DRIVER'S OWN PORTRAIT, matched by the .tga name ailayouts.ini
           gives them against the nine this port ships -- the same lookup the
           finish screen does for its rows. */
        for (j = 0; j < PL_N_FACES; j++) {
            const char *fn = PL_FACE_NAME[j];
            size_t l = strlen(fn);
            if (!strncmp(ap->face, fn, l) && ap->face[l] == '.') {
                face = m->tex.face[j];
                break;
            }
        }
        lv[0] = o->reson;
        lv[1] = o->boost;
        lv[2] = o->tires;
        mm_r_row(m, f, y, item, face, ap->name, o->car, lv, 0);
        y += item;
    }

    /* THE MONEY, centred under the table: what a start costs and what is left.
       41400 "You are to pay" and 41401 "remainder will be", the second of which
       the engine wraps in brackets.
     *
       THE AUTHORED Y'S FALL INSIDE THE TABLE once it holds its six rows --
       staticPayment is 416 and the table runs 109..427 -- which is the same
       "these four rectangles cannot be read literally" this page already
       carries (known-issues.md). So the pair is pushed down to clear the
       table's bottom edge, keeping its OWN 30 px pitch, which is the
       difference between the two authored y's and therefore the file's. */
    {
        const float cx = MM_R_AX;
        const float pitch = DLG_CHRACE_staticRemainderY0
                            - DLG_CHRACE_staticPaymentY0;
        const float bot = DLG_CHRACE_tableY0 + DLG_CHRACE_tableSY + 13.f;
        const float py0 = DLG_CHRACE_staticPaymentY0 > bot
                          ? DLG_CHRACE_staticPaymentY0 : bot;
        /* the table's own closing rule, which its bottom edge is */
        mm_rule_at(f, rx(f, DLG_CHRACE_tableX0),
                   py(f, DLG_CHRACE_tableY0 + DLG_CHRACE_tableSY),
                   DLG_CHRACE_tableSX * f->us);
        garage_cash(money, sizeof money, fee);
        snprintf(line, sizeof line, "%s: %s", STR_UI_CH_PAY, money);
        mm_ctext_at(m, f, 0, MM_R_AX, cx, py0, MM_TS_INFO, line);
        garage_cash(money, sizeof money, (p ? p->cash : 0) - fee);
        snprintf(line, sizeof line, "(%s: %s)", STR_UI_CH_REMAINDER, money);
        mm_ctext_at(m, f, 0, MM_R_AX, cx, py0 + pitch, MM_TS_INFO, line);
    }

    mm_r_button(m, f, MM_R_RACE, mainmenu_r_live(m, MM_R_RACE), STR_UI_RACE);
    mm_r_button(m, f, MM_R_BACK, 1, STR_UI_BACK);
}


static void mm_draw_players(const mainmenu_t *m, const mmframe *f)
{
    mp_draw_bars(m, f);
    mp_draw_card(m, f);
    mp_draw_list(m, f);
}

void mainmenu_draw(const mainmenu_t *m, int screen_w, int screen_h)
{
    const mmframe f = mm_frame(screen_w, screen_h);

    if (!m)
        return;
    mm_draw_frame(m, &f);
    mm_draw_header(m, &f);
    if (MM_PAGE_IS_QUICK(m->page)) {
        /* THE COLUMN FIRST, THEN THE PAGE, THEN THE ENUMS. The bars sit under
           the oval's right leg and nothing on any of the three pages reaches
           them; the arrows go last because on Race summary two of them are
           drawn OVER the car viewport. */
        mm_draw_qnav(m, &f);
        if (m->page == MM_PAGE_MAPINFO)
            mm_draw_mapinfo(m, &f);
        else if (m->page == MM_PAGE_STATS)
            mm_draw_stats(m, &f);
        else if (m->page == MM_PAGE_AWARDS)
            mm_draw_awards(m, &f);
        else
            mm_draw_quick(m, &f);
        mm_draw_qenums(m, &f);
    } else if (m->page == MM_PAGE_PLAYERS) {
        mm_draw_players(m, &f);
    } else if (MM_PAGE_IS_CAR(m->page)) {
        mm_draw_garage(m, &f);
    } else if (m->page == MM_PAGE_MULTI) {
        mm_draw_multi(m, &f);
    } else if (m->page == MM_PAGE_LOBBY) {
        mm_draw_lobby(m, &f);
    } else if (MM_PAGE_IS_CHAMP(m->page)) {
        /* THE LADDER IS UNDER BOTH. dlgCHRACE is a PANEL over dlgCHAMP -- the
           game's own screenshot of it has the ladder still there, dimmed --
           so the ladder draws either way and the panel goes on top. */
        mm_draw_champ(m, &f);
        if (m->page == MM_PAGE_CHRACE)
            mm_draw_chrace(m, &f);
    } else {
        mm_draw_card(m, &f);
        mm_draw_carousel_at(m, &f, 1);
        mm_draw_rows(m, &f);
    }
    /* dlgCHRACE SHIPS ITS OWN Race AND Back, at its own two rectangles, so the
       frame's pair is not drawn over it -- the only page in this front end
       where that is true, and it is true because the file says so. */
    if (m->page != MM_PAGE_CHRACE) {
        mm_draw_race(m, &f);
        mm_draw_quit(m, &f);
    }
    /* THE MODAL IS LAST AND OVER EVERYTHING, which is what makes it one: it
       dims the whole page under it, the way the credits panel does. On whichever
       page raised it -- the roster's three and the Garage's question. */
    if (m->modal == MM_MODAL_SERVERS)
        mm_s_draw(m, &f);
    else if (m->modal)
        mp_draw_modal(m, &f);
    if (m->credits)
        mm_draw_credits(m, &f);
}
