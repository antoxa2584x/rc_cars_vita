/*
 * mainmenu.h -- THE GAME'S OWN MAIN MENU, on the game's own art, with touch.
 *
 * What the app used to have was a START menu over a frozen world (menu.c) and no
 * front end at all: it booted straight into a race. This is the screen the game
 * boots into -- the graffiti desktop, the silver oval, the nine buttons down the
 * right, the player card, the track carousel and the green Race button.
 *
 * EVERY PIXEL OF IT IS SHIPPED ART, and this file draws nothing it invented:
 *
 *   Desktop                the tiled graffiti ground
 *   Podl_LeftTop/..Bottom  ONE 768x768 picture cut into four power-of-two tiles
 *                          (512x512, 256x512, 512x256, 256x256). Reassembled, it
 *                          is the silver oval and the orange field inside it --
 *                          and it is drawn at its OWN 768 design pixels, not
 *                          stretched to the window; see MM_FRAME for the fit that
 *                          settled that
 *   HeaderSkin             three bars in one 256x256: the red header with its
 *                          triangle at y 0..64, the focused copy at 64..128, and
 *                          the plain silver bar at 128..192
 *   ButtonsTextures        six 256x32 bars: red / red-focused at v 0 and 32,
 *                          orange / orange-focused at 96 and 128, and
 *                          grey / grey-round at 192 and 224. The DISABLED look
 *                          is the artists' own, which is why five dead buttons
 *                          can be drawn honestly instead of hidden
 *   ButtonPodl_right_1..9  the silver wedge under each row, ONE PER ROW, each
 *                          slanted to where the oval crosses that row. Nine
 *                          textures for nine rows is the layout stated in the art
 *   Button_race            five 256x64 states: 0 disabled, 1-2 the normal pair,
 *                          3-4 the focused pair -- the little car's wheels turn,
 *                          so the pairs are animation frames and not two moods
 *   Button_back            three 256x32 states, orange / focused / disabled
 *   logoRC_Main            the badge, top right
 *   shot_<track>_0         one photograph per track, for the carousel
 *
 * Interface.sb is the manifest that names all of it, in folders -- "Dialog
 * Textures", "Shot textures", "Track preview", "Control sounds" -- and
 * build.sh's `menu` stage packs menu.vsc off exactly that list.
 *
 * THE LAYOUT IS MEASURED, not guessed. Every constant in mainmenu.c is a
 * rectangle read off the game's own 800x600 screenshot by thresholding for the
 * red bars, the green pill and the non-orange photographs; the file says which
 * measurement each came from. 800x600 is also the frame the engine's own font
 * metrics are written in (HUD_REF_W/H), so the two agree by construction.
 *
 * AND IT RE-FLOWS, because the Vita is 960x544 and the art is 4:3. Two maps, and
 * the split between them is the whole of the resizing rule:
 *
 *   POSITION stretches with the art -- x * w/800, y * h/600. The oval, the
 *   wedges and the bars are one stretched picture, so anything laid out this way
 *   stays glued to the oval at any aspect ratio. Letterboxing would have been
 *   the other answer and costs 24% of a 960x544 screen to black bars.
 *
 *   SIZE of anything that must not be distorted -- the photographs, the badge,
 *   the portrait, every glyph -- uses the UNIFORM scale h/600 about that mapped
 *   position. A track photo stretched 1.32:1 is a wrong photo; a bar stretched
 *   1.32:1 is the bar the artists drew.
 *
 *   AND A GROUP MOVES TOGETHER. Those two rules applied box by box pull a ROW of
 *   boxes apart -- the carousel's 8 px gaps opened to 37 on a 960x544 panel,
 *   because the centres stretched by 1.2 while the boxes shrank by 0.907. The
 *   card and the carousel are one group about one mapped anchor, with uniform
 *   offsets inside it, so the block keeps the shape the artists gave it. See
 *   gx() in mainmenu.c.
 *
 * INPUT IS BOTH, and neither is a translation of the other. Touch is direct: the
 * rects below are hit boxes, a press arms and a release inside the same rect
 * fires (touch.h). The pad walks a focus ring that skips the disabled rows, so a
 * player who never touches the screen never lands on a button that does nothing.
 * The focus follows the finger, so the two never disagree about what is lit.
 *
 * WHAT IS NOT BUILT IS DRAWN GREY AND DOES NOT RESPOND. Championship, Ghost
 * race, Multiplayer, Change player and Demo play have no subsystem behind them
 * in this port. They are on screen because the layout is the game's and because
 * the art has a disabled state; they are not focusable and they are not
 * clickable. See known-issues.md.
 */
#ifndef MAINMENU_H
#define MAINMENU_H

#include "touch.h"

/* The nine rows down the right, in the game's own order. The first eight are the
   red bars; QUIT is the orange one in the bottom-right corner and is a row only
   in the sense that it is the ninth thing the focus ring visits. */
enum {
    MM_CHAMPIONSHIP = 0,
    MM_QUICK_RACE,
    MM_GHOST,
    MM_MULTIPLAYER,
    MM_CHANGE_PLAYER,
    MM_OPTIONS,
    MM_DEMO,
    MM_CREDITS,
    MM_N_ROWS
};

/* THE SCREENS. The front end is two pages on one frame: the main menu, and the
 * QUICK RACE setup the exe carries as `dlgRACESUM'. That dialog is recovered in
 * full -- the exe lays every dialog out as a table of
 * { u32 id; u32 type; char *name; u32 0 } terminated by a zero record, and
 * RACESUM's runs 0x960..0x96a:
 *
 *   0x960 &SHS  shotTrack          the track's photograph
 *   0x961 &ENM  enumTrack
 *   0x962 &STT  staticTrackName
 *   0x963 &STT  staticTrackInfo
 *   0x964 &ENM  enumNLaps          <- the lap limit, which this port had no
 *                                     source for and recorded as a gap
 *   0x965 &ENM  enumSkill
 *   0x966 &APV  animCar            a live car viewport -- not built, see
 *                                  known-issues.md
 *   0x967 &ENM  enumCar
 *   0x968 &STT  staticCarName
 *   0x969 &STT  staticCarInfo
 *   0x96a &STT  staticClosedInfo   the locked-track line; championship only
 *
 * (0x96b/0x96c, enumGhostType and staticGhostTypeExplain, belong to
 * dlgRACESUM_GHOST, which shares the table.)
 *
 * So the CONTENTS of this screen are the game's, control for control. Its
 * PLACEMENT is the port's -- the exe's control records carry no rectangle and no
 * screenshot of it was available -- and it is laid out in the main menu's own
 * language so the two read as one interface: the same frame, the same bars in
 * the same eight measured row positions, the same Race button. */
enum {
    MM_PAGE_MAIN = 0,
    MM_PAGE_QUICK,
    MM_N_PAGES
};

/* dlgRACESUM's four enums, in its own order. */
enum {
    MM_Q_TRACK = 0,
    MM_Q_LAPS,
    MM_Q_SKILL,
    MM_Q_CAR,
    MM_Q_N_ROWS
};
#define MM_Q_RACE (MM_Q_N_ROWS)
#define MM_Q_BACK (MM_Q_N_ROWS + 1)
#define MM_Q_N_FOCUS (MM_Q_N_ROWS + 2)

/* THE PAGE'S OWN RIGHT-HAND COLUMN, which is NOT the main menu's eight: the
   quick-race screen navigates to its own siblings. `Race summary' is this page;
   the other three are dialogs the exe carries and this port does not build --
   dlgMAPINFO, dlgSTAT and dlgSETCAR, the Garage. Drawn in the artists' own grey,
   like the five on the main menu. */
enum {
    MM_QB_SUMMARY = 0,
    MM_QB_MAPINFO,
    MM_QB_STATS,
    MM_QB_GARAGE,
    MM_QB_N
};

/* enumNLaps' values: THREE, FIVE OR SEVEN, which is what the original offers.
   championship.ini carries cash and placings and no lap count -- that is why the
   HUD used to show a bare `3' -- so the list is not derivable from the data; it
   is what the game's own quick-race screen shows. Odd numbers only, and 3 is the
   default the HUD's own placeholder already assumed. */
#define MM_N_LAPS 3
extern const int MM_LAPS[MM_N_LAPS];
#define MM_LAPS_DEF 3

/* enumSkill. Four, because AI_DIFFICULTY is a four-row table and ai.h documents
   0..3 as easy..ultra; ailayouts.ini declares only three race-type bits, so
   ultra rides with hard. */
#define MM_N_SKILL 4

/* enumCar: rb_data.h's three. */
#define MM_N_CARS 3

/* The focus ring visits the rows above plus these two. */
#define MM_FOCUS_RACE (MM_N_ROWS)
#define MM_FOCUS_QUIT (MM_N_ROWS + 1)
#define MM_N_FOCUS    (MM_N_ROWS + 2)

/* What mainmenu_step decided this frame. The caller acts and the menu forgets:
   the field is cleared at the top of every step, like menu.c's `cue`. */
typedef enum {
    MM_ACT_NONE = 0,
    MM_ACT_RACE,        /* start a race on `track` */
    MM_ACT_OPTIONS,     /* open the existing settings menu */
    MM_ACT_QUIT
} mm_action;

/* The sounds the menu asks for, by what the input DID rather than by which
   sample -- the same split menu.c uses, so main.c owns the mapping onto the
   game's own interface bank. Interface.sb's "Control sounds" folder names the
   engine's own six: ButtonFocus, ButtonPress, ArrowFocus, ArrowPress, RaceFocus,
   RacePress. */
typedef enum {
    MM_CUE_NONE = 0,
    MM_CUE_FOCUS,       /* the focus moved to another button */
    MM_CUE_PRESS,       /* a button fired */
    MM_CUE_ARROW,       /* the carousel moved */
    MM_CUE_DENY         /* a disabled button was touched */
} mm_cue;

/* Every handle the menu draws with. 0 means "not loaded", and every draw checks:
   a menu.vsc that failed to pack must degrade to the text fallback rather than
   drawing nothing at all, which is the same rule race_ui.c and hud.c follow. */
typedef struct {
    unsigned int desktop;
    unsigned int podl_lt, podl_rt, podl_lb, podl_rb;
    unsigned int header;         /* HeaderSkin */
    unsigned int buttons;        /* ButtonsTextures */
    unsigned int wedge[9];       /* ButtonPodl_right_1..9 */
    unsigned int race;           /* Button_race */
    unsigned int back;           /* Button_back */
    unsigned int logo;           /* logoRC_Main */
    unsigned int shot[10];       /* shot_<track>_0, in TRACKS[] order */
    unsigned int face;           /* the player portrait */
    unsigned int arrows;         /* enumarrows -- 2x2 of 32x32: silver, red,
                                    grey, in that order across then down */
    unsigned int font_big;       /* Smash26 */
    unsigned int font_small;     /* Smash20 */
} mainmenu_tex;

/* THE CAR VIEWPORT's own drawer -- `animCar', which is a 374x304 3D view of the
 * chosen car and the one control on the quick-race page this file cannot draw.
 * It is 3D, and mainmenu.c owns no scene, no camera and no GL beyond ui.c.
 *
 * So the caller supplies it. mainmenu_draw calls this BETWEEN the frame and the
 * widgets, with the rectangle in screen pixels, having left ui.c's ortho pass
 * first -- so the callback gets the GL state it would have outside a menu, and
 * the enum arrows either side of the car are drawn over it afterwards. NULL,
 * which is the default, leaves the space to the caption. */
typedef void (*mm_car_draw)(void *ctx, float x, float y, float w, float h);
typedef struct {
    mainmenu_tex tex;

    int   page;         /* MM_PAGE_* */
    int   qfocus;       /* MM_Q_* on the quick-race page */
    int   laps;         /* MM_LAPS_MIN .. MM_LAPS_MAX */
    int   skill;        /* 0 .. MM_N_SKILL-1 */
    int   car;          /* 0 .. MM_N_CARS-1 -- the caller's, synced both ways */

    int   focus;        /* MM_* row, MM_FOCUS_RACE or MM_FOCUS_QUIT */
    int   track;        /* 0 .. 9, indexes TRACKS[] */
    int   armed;        /* the row a touch went down on, or -1 */
    int   credits;      /* the credits panel is up */
    float t;            /* seconds since init, for the car animation */

    mm_action action;   /* cleared every step */
    mm_cue    cue;      /* cleared every step */

    /* The pad, edge-detected here so the caller hands over raw buttons and the
       menu owns the repeat rule -- exactly what menu.c does. */
    unsigned int prev_buttons;

    /* animCar's drawer -- see mm_car_draw. */
    mm_car_draw car_draw;
    void       *car_ctx;
} mainmenu_t;

void mainmenu_set_car_draw(mainmenu_t *m, mm_car_draw fn, void *ctx);

/* `tex` is copied. Leaves the focus on Quick race, which is the one live mode. */
void mainmenu_init(mainmenu_t *m, const mainmenu_tex *tex);

/* One frame. `buttons` is SceCtrlData.buttons (menu.h supplies the bits on the
   host); `tp` may be NULL where there is no panel. Reads `action` and `cue`
   afterwards. */
void mainmenu_step(mainmenu_t *m, unsigned int buttons, const touch_state *tp,
                   int screen_w, int screen_h, float dt);

/* Draws between ui_begin/ui_end -- the caller brackets it, because main.c draws
   the menu and the settings overlay in one pass. */
void mainmenu_draw(const mainmenu_t *m, int screen_w, int screen_h);

/* THE QUICK-RACE PAGE's row under (x, y), MM_Q_* or -1. `left` comes back 1 when
   the point is in the row's own back-arrow, which is what makes one row walk
   both ways under a thumb. */
int  mainmenu_q_row_at(const mainmenu_t *m, int screen_w, int screen_h,
                       float x, float y, int *left);

/* The four skill names, and the field each one fields on `track` -- counted off
   ai_data.h's own AI<n>Races masks, which is where the difference comes from.
   See ai_set_skill_field. */
const char *mainmenu_skill_name(int skill);
int  mainmenu_field_size(int track, int skill);

/* Which row, if any, is under (x, y) in screen pixels. -1 for none; a DISABLED
   row still answers, so a touch on one can deny rather than do nothing. Exposed
   for the harness. */
int  mainmenu_row_at(const mainmenu_t *m, int screen_w, int screen_h,
                     float x, float y);

/* WHICH CAROUSEL SLOT is under (x, y), 0..4 left to right, or -1. Slot 2 is the
   selection itself and answers like any other -- tapping it is a no-op, which is
   the step function's business and not this one's. Exposed for the harness,
   which otherwise has to rebuild the layout to find a point inside a
   photograph, and a test that rebuilds the layout is testing its own copy. */
int  mainmenu_slot_at(const mainmenu_t *m, int screen_w, int screen_h,
                      float x, float y);

/* Whether a row does anything in this build. The five that do not are drawn from
   the artists' own grey. */
int  mainmenu_row_live(int row);

/* The label the row carries, for the harness and for the log. */
const char *mainmenu_row_name(int row);

#endif /* MAINMENU_H */
