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
 *   HeaderSkin             three bars in one 256x256, and the header is TWO of
 *                          them: the plain silver bar (rows 135..184) with the
 *                          red plate on top of it. The red comes twice -- rows
 *                          12..51 with a red triangle, 76..115 with a WHITE one
 *                          -- and the real menu shows the white
 *   ButtonsTextures        six 256x32 bars: red / red-focused at v 0 and 32,
 *                          orange / orange-focused at 96 and 128, and
 *                          grey / grey-round at 192 and 224. The DISABLED look
 *                          is the artists' own, which is why five dead buttons
 *                          can be drawn honestly instead of hidden
 *   ButtonPodl_right_1..9  the silver wedge under each row, ONE PER ROW, each
 *                          slanted to where the oval crosses that row. Nine
 *                          textures for nine rows is the layout stated in the
 *                          art. Drawn at the cell's OWN size: the plate is 39
 *                          rows against the bar's 31 and stands proud of it
 *   RadioButtonsTextures   three 256x32 cells -- red with a dark dot, red with
 *                          a white one, grey -- which is what the quick-race
 *                          page's three sibling tabs are drawn from
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
 * race, Multiplayer and Demo play have no subsystem behind them in this port.
 * They are on screen because the layout is the game's and because the art has a
 * disabled state; they are not focusable and they are not clickable. See
 * known-issues.md.
 *
 * THE QUICK-RACE PAGE'S FOURTH TAB IS LIVE NOW: it opens dlgSETCAR, the Garage,
 * and dlgSETDETAIL behind its three part buttons -- the shop, on the game's own
 * economy out of championship.ini. See MM_PAGE_IS_CAR below and garage.h.
 */
#ifndef MAINMENU_H
#define MAINMENU_H

#include "touch.h"
#include "player.h"     /* the roster the Select player page is a view of */
#include "garage.h"     /* GAR_N_KINDS / GAR_N_LEVELS -- the upgrade art's
                           dimensions, and the shop dlgSETCAR is a view of */

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
    MM_PAGE_QUICK,      /* dlgRACESUM -- Race summary */
    MM_PAGE_MAPINFO,    /* dlgMAPINFO -- Map and info */
    MM_PAGE_STATS,      /* dlgSTAT    -- Track stats  */
    /* THE AWARD BOOK, and it is the one page in this front end with NO DIALOG
     * BEHIND IT: the game has no achievements, so the exe carries no table to
     * read and there is no screenshot to measure (awards.h). It is a fourth
     * sibling view -- the same frame, the same navigation column, the same
     * green Race button -- and it borrows dlgSTAT's own rectangles, because it
     * is the same shape of thing on the same screen: a table with a scroll bar,
     * a heading over it and one picker under it. Where dlgSTAT gives the table
     * only 273 px because a photograph and a three-line blurb sit above it,
     * this page has neither and takes the height back. */
    MM_PAGE_AWARDS,     /* the port's own -- see awards.h */
    MM_PAGE_PLAYERS,    /* dlgPLRSCOMM -- Select player */
    MM_PAGE_GARAGE,     /* dlgSETCAR   -- the Garage */
    MM_PAGE_DETAIL,     /* dlgSETDETAIL -- one of its three upgrade pages */
    MM_PAGE_MULTI,      /* dlgMULTIPLAYER -- Create game / Join game */
    MM_PAGE_LOBBY,      /* dlgWAITPLAYERS_* -- one of its four views */
    /* THE CHAMPIONSHIP, and BOTH ITS SCREENS ARE SHIPPED. `Settings/' carries
     * dlgCHAMP.ini and dlgCHRACE.ini like every other dialog, so neither of
     * these was measured off anything -- gen_dlg_data.py emits both. See
     * champ.h for the rules behind them and ui.md for the two pages.
     *
     *   dlgCHAMP   the ten-track LADDER: a table of Track / Scores req. /
     *              Cost / Place / Prize, the chosen track's name under it and
     *              three lines saying what it takes to enter and what it pays
     *   dlgCHRACE  the page between the ladder and the flag, and the one that
     *              TAKES THE ENTRY FEE: what you are to pay, what the prize is,
     *              what your cash will be afterwards, and the car you are
     *              taking with its three parts. Race and Back, on the two
     *              button rectangles that dialog ships of its own
     */
    MM_PAGE_CHAMP,
    MM_PAGE_CHRACE,
    MM_N_PAGES
};

/* Whether `page' is one of the championship's two. */
#define MM_PAGE_IS_CHAMP(p) ((p) == MM_PAGE_CHAMP || (p) == MM_PAGE_CHRACE)

/* THE SELECT PLAYER PAGE, which is `dlgPLRSCOMM' and is the screen the game
 * comes up on when there is nobody to race as. It is a fourth view on the same
 * frame -- the same graffiti ground, the same oval, the same eight measured row
 * positions, the same green Race button -- and it is NOT one of the quick-race
 * siblings: it has its own header ("Select player"), its own right-hand column
 * and its own focus ring, so MM_PAGE_IS_QUICK stays false for it.
 *
 * WHAT IS ON IT, and every rectangle is measured off the game's own 800x600
 * screenshot of this page except the four the game ships in
 * `Settings/dlgPLRSCOMM.ini`, which are used as authored:
 *
 *   shotFace (99, 79, 130x157)  the chosen profile's portrait, with a red
 *                               arrow either side of it that walks the NINE
 *                               shipped portraits and saves the choice
 *   staticName (240, 70)        the name, with a rule under it
 *   staticInfo (240, 112)       Rank / Current car / Play time
 *   staticCash (240, 186)       Scores and Cash
 *   the LIST                    Player / Score / Cash / Time, one row per
 *                               profile, seven visible, with the engine's own
 *                               scroll bar beside it
 *   Create player, Remove player      rows 0 and 1 of the eight
 *   Sort by <score|cash|time|name>    row 6; the four values are the string
 *                                     table's 10025..10028
 *   Face                              row 7 -- NOT BUILT. In the original it
 *                                     opens a chooser over the user's own
 *                                     `Faces/' TARGAs, and this port ships no
 *                                     such directory; the arrows beside the
 *                                     portrait are the face control that is
 *                                     built. Drawn in the artists' own grey
 *   Continue                          the Quit slot, bottom right
 *
 * The list is a VIEW: `psort` orders it and `pview` is that order, so sorting
 * moves rows on screen without moving anything in the roster.
 */
enum {
    MM_P_LIST = 0,      /* the table; UP/DOWN walk its rows */
    MM_P_FACE_L,        /* the portrait's two arrows */
    MM_P_FACE_R,
    MM_P_CREATE,
    MM_P_REMOVE,
    MM_P_SORT,
    MM_P_FACE,          /* the dead one */
    MM_P_RACE,
    MM_P_CONTINUE,
    MM_P_N_FOCUS
};

/* `Sort by ...', in the string table's own order: 10025, 10026, 10027, 10028. */
enum {
    MM_SORT_SCORE = 0,
    MM_SORT_CASH,
    MM_SORT_TIME,
    MM_SORT_NAME,
    MM_N_SORT
};

/* THE THREE MODALS, which are the three the game's own screenshots show. Each
 * one owns every input while it is up, exactly as the credits panel does.
 *
 *   MM_MODAL_CREATE  "Player name" and a field, with Ok and Cancel
 *   MM_MODAL_REMOVE  "Do you want to remove current player?", Yes and No
 *   MM_MODAL_SAY     one line and one Ok -- "Can't remove last player",
 *                    "Player already exists", and the two the file layer can
 *                    raise. All four are the game's own strings
 *
 * THE NAME IS TYPED ON THE MACHINE'S OWN KEYBOARD. `ime.h` opens
 * `sceImeDialog` -- the system's on-screen keyboard, in the player's own
 * language and with their own enter button -- and while it is up the page is in
 * MM_MODAL_IME and does nothing but poll it. The compositor draws the keyboard,
 * so there is nothing here to lay out.
 *
 * THE 10 x 4 GRID IS STILL HERE and is not dead code: it is what comes up when
 * `ime_open` fails, and it is the path `mainmenu_test` walks, because the host
 * has no `psp2/` headers and `ime_available()` answers 0 there. One failed
 * `sceImeDialogInit` is the whole distance between the two.
 */
enum {
    MM_MODAL_NONE = 0,
    MM_MODAL_CREATE,    /* the on-screen grid -- the fallback */
    MM_MODAL_IME,       /* the machine's own keyboard is up */
    MM_MODAL_REMOVE,
    MM_MODAL_SAY,
    /* THE GARAGE'S OWN QUESTION, and it is the same shape as MM_MODAL_REMOVE:
       one line (or two) and Yes/No. It exists because the engine asks before it
       takes money -- "Do you really want to buy car", "Do you want to sell car
       and upgrades?" and "Cash remainder $n", all three its own words -- and
       spending a profile's cash is not undoable. `gask' says what Yes commits. */
    MM_MODAL_ASK,
    /* THE SERVER LIST. `Join game' starts a browse and puts this up: the games
       `net.c' has heard announce, one per row, with Cancel. The original has a
       whole dialog for it (`dlgNETJOIN_SERVINFO', whose own table this port
       does not draw); a modal over the page it was pressed on is the port's,
       and it is what makes the page work without a fifth screen. */
    MM_MODAL_SERVERS,
    MM_N_MODAL
};

#define MM_KB_COLS 10
#define MM_KB_ROWS  4
#define MM_KB_KEYS (MM_KB_COLS * MM_KB_ROWS)
/* The two buttons under the grid are focus stops MM_KB_KEYS and +1. */
#define MM_MODAL_OK     (MM_KB_KEYS)
#define MM_MODAL_CANCEL (MM_KB_KEYS + 1)

/* THE THREE SIBLING PAGES ARE ONE SCREEN with three views, which is what the
 * radio buttons down the right say. They share the frame, the header ("Quick
 * race" on all three), the green Race button, the Main menu button, and the
 * TRACK: the picker on Race summary, the picker on Map and info and the name on
 * Track stats are all `m->track`, so walking the carousel on one page and
 * switching to another shows the same track.
 *
 * dlgMAPINFO adds `shotList` and `enumShot' -- FIVE screenshots per track, which
 * the pack has always carried as shot_<track>_0..4 and this port loaded only the
 * first of. dlgSTAT has no picture control of its own beyond `shotTrack', and on
 * the game's own screenshot of it that picture is the SAME screenshot the
 * shotList on Map and info is standing on. So `m->shot` is shared too; that is
 * an inference off two screenshots agreeing rather than something read out of
 * the exe, and it is the only reading under which the two pictures match. */
#define MM_N_SHOTS 5

/* THE PAGE'S OWN RIGHT-HAND COLUMN, which is NOT the main menu's eight: the
   quick-race screen navigates to its own siblings. ALL FOUR ARE BUILT NOW --
   dlgRACESUM, dlgMAPINFO, dlgSTAT and dlgSETCAR, the Garage.

   The first three are RADIO buttons out of RadioButtonsTextures and the page
   you are on is the one with the WHITE dot, which is what the game's own shots
   of Map and info and of Track stats both have. The fourth is an ARROW off
   ButtonsTextures, because it is the one of the four that goes to a screen of
   its own rather than to a view of this race -- which is what the game's own
   shot of the quick-race page has, and it is now an arrow that leads somewhere
   instead of a grey bar that denies. */
enum {
    MM_QB_SUMMARY = 0,
    MM_QB_MAPINFO,
    MM_QB_STATS,
    MM_QB_AWARDS,
    MM_QB_GARAGE,
    MM_QB_N
};

/* Which page each bar opens; -1 for the one that is not built. */
extern const int MM_QB_PAGE[MM_QB_N];

/* Whether `page' is one of the three sibling views. */
#define MM_PAGE_IS_QUICK(p) ((p) >= MM_PAGE_QUICK && (p) <= MM_PAGE_AWARDS)

/* THE GARAGE AND ITS UPGRADE PAGE -- dlgSETCAR and dlgSETDETAIL, two more views
 * on the same frame, reached from the quick-race page's fourth bar. They are
 * one screen in the same sense the three siblings are: the same header
 * ("Garage" on both), the same four radio bars down the right, the same green
 * Race button, the same CAR -- and dlgCARSCOMM's furniture, which is where the
 * car viewport, the cash line, the "Car" heading and the spec block come from
 * on BOTH of them. See garage.h for the shop's own rules and dlg_data.h for
 * every rectangle.
 *
 * THE FOUR BARS ARE RADIO BUTTONS, and the game's own screenshots say so: each
 * of the four carries a DOT out of RadioButtonsTextures, the one you are on has
 * the WHITE one, and it stands out on the focus curve's settled 0.361 -- 26 px
 * on both shots, measured off the left cap of the bar against its siblings'
 * (557 against 583 on the Garage, 579 against 605 on the booster page). */
#define MM_PAGE_IS_CAR(p) ((p) == MM_PAGE_GARAGE || (p) == MM_PAGE_DETAIL)

/* The four bars: the Garage itself, then one per part kind. `MM_GB_PART + k'
   opens dlgSETDETAIL on GAR_BOOSTER + k. */
enum {
    MM_GB_SELECT = 0,       /* Select car -- dlgSETCAR */
    MM_GB_PART,             /* Upgrade booster */
    MM_GB_PART_ENGINE,      /* Upgrade engine */
    MM_GB_PART_TIRES,       /* Upgrade tires */
    MM_GB_N
};

/* THE FOCUS SPACE THE TWO PAGES SHARE, in the order the pad walks it: the
   page's own enum on the left, then the four bars, then the page's own two or
   three buttons down the right, then Race, then Back.
 *
   MM_G_BUY and MM_G_SELL are one pair of stops with two meanings, because they
   are one pair of BARS -- rows 6 and 7 on both pages: Buy car / Sell car on the
   Garage and Upgrade / Downgrade on the part page. MM_G_SKIN is the Garage's
   alone (row 4, `Next skin'); the part page has no stop there and the ring
   skips it, the same way the main menu's ring skips its five dead rows. */
enum {
    MM_G_ENUM = 0,      /* enumCar on the Garage, enumUpgrades on the part page */
    MM_G_SKIN,          /* row 4 -- Garage only */
    MM_G_BUY,           /* row 6 -- Buy car / Upgrade */
    MM_G_SELL,          /* row 7 -- Sell car / Downgrade */
    MM_G_RACE,
    MM_G_BACK,
    MM_G_TAB,           /* MM_G_TAB + MM_GB_* -- the four radio bars */
    MM_G_N_FOCUS = MM_G_TAB + MM_GB_N
};

/* THE FOCUS SPACE THE THREE SIBLING PAGES SHARE. 0..3 are the page's OWN enums
 * in its dialog's order -- four on Race summary, two on Map and info, one on
 * Track stats -- and everything from MM_Q_RACE up is on all three. A page that
 * has no enum at an index simply has no focus stop there, and the ring skips it
 * the same way it skips the dead rows on the main menu.
 *
 * Race summary's four keep the names they had, because they are what
 * mainmenu_test.c and menushot's script letters address. */
enum {
    MM_Q_TRACK = 0,
    MM_Q_LAPS,
    MM_Q_SKILL,
    MM_Q_CAR,
    MM_Q_N_ROWS
};

/* HOW MANY OF THE TWENTY-FIVE AWARDS THE PAGE SHOWS AT ONCE. In the header
   because the list's bottom stop is AW_N - MM_AW_ROWS and the harness walks the
   scroller to it -- a copy of this number over there would be a check against
   itself. The rest of that table's geometry is mainmenu.c's own. */
#define MM_AW_ROWS 8

/* Map and info's two, Track stats' one and the award page's one, at the same
   indices. */
#define MM_Q_MI_TRACK  0        /* shotTrackEnum */
#define MM_Q_MI_SHOT   1        /* enumShot */
#define MM_Q_ST_TYPE   0        /* enumStatType, `Sort results by' */

#define MM_Q_RACE (MM_Q_N_ROWS)         /* 4 */
#define MM_Q_BACK (MM_Q_N_ROWS + 1)     /* 5 */
/* The four navigation bars, MM_QB_* added to this. */
#define MM_Q_NAV  (MM_Q_N_ROWS + 2)     /* 6..9 */
#define MM_Q_N_FOCUS (MM_Q_NAV + MM_QB_N)

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

/* ================================================== MULTIPLAYER, two screens
 *
 * `dlgMULTIPLAYER' is the front page -- the player card, the track carousel and
 * two buttons -- and `dlgWAITPLAYERS_*' is the lobby, which is one screen with
 * four views the way the quick-race page is one screen with three. `net.h' is
 * the transport under both; nothing here opens a socket.
 *
 * TWO PLAYERS IS NOT BUILT, and that is asked for rather than missing. The
 * game's own front page has a third button, `Two players' (10130), which is
 * split-screen on one machine -- and this port is on a 960x544 handheld with one
 * pad. It is not drawn at all rather than drawn grey: a disabled button says
 * "not yet", and this one is "not on this machine". The layout keeps its own
 * row EMPTY where the original puts it, which is why the two live buttons stay
 * on rows 0 and 1 and nothing shifts up.
 *
 * AND CHAT AND OPTIONS ARE NOT BUILT EITHER, for the same kind of reason: the
 * lobby's six tabs are Race summary, Map and info, Car setup, Chat, Car
 * restrictions and Options, and typing to each other on a machine whose only
 * keyboard is a modal system dialog is not a feature, while `dlgWAITPLAYERS_
 * CHAT's other half -- a nickname and a chat colour -- is a profile field and a
 * chat colour. The four that remain are drawn on rows 0..3 rather than in the
 * original's 0, 1, 2 and 4: the port's own choice, so the column reads as a
 * designed four the way the quick-race page's does instead of a six with two
 * holes punched in it.
 *
 * THE MESSAGE PANEL STAYS. It is the `chat' control, and what the game's own
 * lobby screenshot has in it is not chat -- it is ">Map is changed to: Fishers",
 * which is string 20214. The engine writes seventeen such lines (20200..20216)
 * and they are the only way a player learns that somebody joined, that the host
 * moved the track, or that somebody is ready. So the panel is drawn and fed by
 * `net.c's log, and the `editChat' input line under it is not.
 */

/* The front page's own stops. Create game and Join game are the game's own rows
   0 and 1; the third row it puts `Two players' on is left empty. */
enum {
    MM_M_CREATE = 0,
    MM_M_JOIN,
    MM_M_RACE,
    MM_M_BACK,
    MM_M_N_FOCUS
};

/* THE LOBBY'S FOUR VIEWS, in the order its own tabs are in. */
enum {
    MM_L_RACESUM = 0,   /* dlgWAITPLAYERS_RACESUM  -- the map, the laps, the table */
    MM_L_MAPINFO,       /* dlgMAPINFO again, which is the multiplayer map page too:
                           the exe's own dlgMAPINFO control table carries `chat'
                           and `editChat', so that dialog IS this tab */
    MM_L_CARSETUP,      /* dlgWAITPLAYERS_CARSETUP -- your own car and its parts */
    MM_L_CARRESTR,      /* dlgWAITPLAYERS_CARRESTR -- what the host allows */
    MM_L_N_VIEW
};

/* The lobby's focus space, shared by the four views the way MM_G_* is shared by
   the Garage's two. 0..3 are the view's OWN controls in its dialog's order -- two
   on Race summary, two on Map and info, four on Car setup, three on Car
   restrictions -- and everything from MM_LB_ROW up is on all four. */
enum {
    MM_LB_C0 = 0,
    MM_LB_C1,
    MM_LB_C2,
    MM_LB_C3,
    MM_LB_ROW,          /* row 7: Kick player off / Next skin, per view */
    MM_LB_RACE,         /* Ready on a client, Race on the host */
    MM_LB_BACK,         /* Disconnect */
    MM_LB_TAB,          /* MM_LB_TAB + MM_L_* -- the four radio bars */
    MM_LB_N_FOCUS = MM_LB_TAB + MM_L_N_VIEW
};

/* How many of the four `C' stops each view has. */
extern const int MM_L_NCTRL[MM_L_N_VIEW];

/* ================================================ THE CHAMPIONSHIP's two pages
 *
 * dlgCHAMP's own focus ring. The LADDER is one stop that UP and DOWN walk, the
 * way the Select player page's list is -- ten rows in a table is not ten stops
 * in a ring. The rest are the buttons the original's page has (FUN_004bea70's
 * switch: 0x76d Race, 0x773 New championship, 0x774 the car setup) drawn in
 * this port's own right-hand column, plus the Main menu button every page has.
 */
/* dlgCHAMP's RIGHT-HAND COLUMN, which is the game's own six bars on the frame's
   own eight row positions -- read straight off its screenshot, whose bar
   centres land on MM_CY[0..4] and MM_CY[6] to the pixel. The first three are
   RADIO cells (this page and the two sibling views), the Garage is an ARROW
   because it goes to a screen of its own, and the last two are the ORANGE
   Button_back plate rather than the red one, which is what the picture has. */
enum {
    MM_CB_CHAMP = 0,    /* row 0 -- the page you are on */
    MM_CB_MAPINFO,      /* row 1 -- dlgMAPINFO, on the ladder's own track */
    MM_CB_STATS,        /* row 2 -- dlgSTAT */
    MM_CB_GARAGE,       /* row 3 */
    MM_CB_NEW,          /* row 4 -- New championship, 10053 */
    MM_CB_TRAINING,     /* row 6 -- 10054. NOT BUILT; drawn in the grey */
    MM_CB_N
};

/* Which of the frame's eight rows each bar sits on. Row 5 and row 7 are empty
   on this page, exactly as they are on the game's own. */
extern const int MM_CB_ROW[MM_CB_N];

/* dlgCHAMP's own focus ring. The LADDER is one stop that UP and DOWN walk, the
   way the Select player page's list is -- ten rows in a table is not ten stops
   in a ring. */
enum {
    MM_C_LIST = 0,      /* the ten-track table; UP/DOWN walk it */
    MM_C_NAV,           /* MM_C_NAV + MM_CB_* -- the six bars */
    MM_C_RACE = MM_C_NAV + MM_CB_N,   /* the green one: on to dlgCHRACE */
    MM_C_BACK,          /* the corner, and on this page it says `Main menu' */
    MM_C_N_FOCUS
};

/* dlgCHRACE's, and this dialog ships its OWN two buttons -- buttonRace at
   (257, 530) and buttonBack at (414, 530), both 134x36 -- so unlike every other
   page in this front end they are not the frame's, and neither is the GREEN
   plate: the game's own shot of this page has a RED `Race' and an ORANGE
   `Back', side by side and centred, which is what those two rectangles are.
 *
   AND THE PAGE IS AN OVERLAY. That screenshot shows the ladder still there
   underneath, dimmed -- dlgCHRACE is a panel over dlgCHAMP and not a fifth
   view of the frame, which is why its own rectFrame is 782x573 and why it
   carries buttons at all. */
enum {
    MM_R_RACE = 0,
    MM_R_BACK,
    MM_R_N_FOCUS
};

/* What mainmenu_step decided this frame. The caller acts and the menu forgets:
   the field is cleared at the top of every step, like menu.c's `cue`. */
typedef enum {
    MM_ACT_NONE = 0,
    MM_ACT_RACE,        /* start a race on `track` */
    MM_ACT_OPTIONS,     /* open the existing settings menu */
    MM_ACT_PLAYER,      /* the current profile changed -- reload its choices */
    /* THE GARAGE CHANGED THE CAR: a part bought or sold, a car bought or sold,
       or the paint stepped. The caller re-reads the profile's own four numbers
       for the selected car into the live tuning and writes the profile out --
       one action for all six, so a seventh thing the shop can do tomorrow is
       applied by construction rather than by somebody remembering. */
    MM_ACT_GARAGE,
    /* THE NETWORK RACE IS STARTING. The caller loads the host's track, builds
       the field out of the roster instead of out of a recording, and races.
       Distinct from MM_ACT_RACE because the two set up different fields and
       because only one of them can be aborted by the other end going away. */
    MM_ACT_NET_RACE,
    /* THE CHAMPIONSHIP ROUND IS STARTING, on `track', and the caller's part of
       it is what the engine's game mode 5 is: FIVE laps whatever the picker
       says (FUN_004e03b0 refuses anything else) and a finish that is worth
       money. THE ENTRY FEE HAS ALREADY BEEN TAKEN by the time this is raised --
       dlgCHRACE's Race button is what pays it, exactly as FUN_004c0300 does,
       so a caller that ignores this action still leaves the profile paid up.
       Distinct from MM_ACT_RACE because only one of the two pays out. */
    MM_ACT_CHAMP_RACE,
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
    unsigned int radio;          /* RadioButtonsTextures */
    unsigned int wedge[9];       /* ButtonPodl_right_1..9 */
    unsigned int race;           /* Button_race */
    unsigned int back;           /* Button_back */
    unsigned int logo;           /* logoRC_Main */
    unsigned int shot[10][MM_N_SHOTS];  /* shot_<track>_0..4, TRACKS[] order */
    unsigned int trackmap[10];   /* trackmap_<n> -- the painted top-down map */
    unsigned int scrollbar;      /* the stats table's own bar */
    unsigned int panel;          /* messagebox_empty -- the map's silver frame */
    /* `messagebox' -- and its RIGHT HALF is THE DIALOG'S OWN BUTTON, which this
       port spent a long time trying to build out of the row bars. 256x128: the
       left half is another rounded frame, the right half is FOUR 128x32 pills
       with BOTH caps rounded and a dot in the left one -- red/dark, red/white,
       orange/dark, orange/white, which is exactly a Yes/No pair and its focus.
       See mm_msgbtn. */
    unsigned int msgbox;
    unsigned int face[PL_N_FACES];  /* the nine portraits, PL_FACE_NAME order */
    unsigned int arrows;         /* enumarrows -- 2x2 of 32x32: silver, red,
                                    grey, in that order across then down */
    /* THE TWENTY-SEVEN UPGRADE PHOTOGRAPHS, `upgr_boost<L>_<car+1>' and its two
       families -- Interface.sb's own `Upgrades' folder, three kinds by three
       cars by three levels. Indexed [kind][car][level-1]: the LEVEL is the
       texture name's first number and the car its second, which garage.h says
       how it knows. Each is a 256x256 with the same framed ink the track
       photographs have, x 28..227 and y 53..202, so it needs no border drawn
       for it. */
    unsigned int upgr[GAR_N_KINDS][PL_N_CARS][GAR_N_LEVELS];
    /* `skin_ik_vse' -- Interface.sb's own skin-icon sheet, 128x128 as a 4 x 4
       grid of 32x32 cells: twelve paint swatches (three cars by four skins,
       car-major) and a camera icon in the thirteenth. The lobby's roster table
       draws one per row, which is what dlgWAITPLAYERS_RACESUM's three
       `DrawSkin' keys are for. */
    unsigned int skinicons;
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
    int   qfocus;       /* MM_Q_* on any of the three sibling pages */
    int   shot;         /* 0..MM_N_SHOTS-1, shared by shotList and dlgSTAT */
    int   stat;         /* REC_STAT_*, `Sort results by' */
    int   aw_top;       /* the award page's first visible row -- its own picker
                           scrolls the list, and that is the only thing on that
                           page there is to hold */
    int   laps;         /* MM_LAPS_MIN .. MM_LAPS_MAX */
    int   skill;        /* 0 .. MM_N_SKILL-1 */
    int   car;          /* 0 .. MM_N_CARS-1 -- the caller's, synced both ways */

    /* THE SCROLL BAR'S ONE BIT: this touch went down in a trough and the thumb
       is following the finger, so a drag that wanders off the bar sideways --
       which every drag on a 24 px wide control does -- keeps scrolling instead
       of stopping. Cleared the frame the finger comes up. One flag for all
       three pages that have a bar, because only one of them is ever up. */
    int   sb_drag;

    int   focus;        /* MM_* row, MM_FOCUS_RACE or MM_FOCUS_QUIT */
    int   track;        /* 0 .. 9, indexes TRACKS[] */
    int   armed;        /* the row a touch went down on, or -1 */
    int   credits;      /* the credits panel is up */
    float t;            /* seconds since init, for the car animation */

    /* THE BUTTON ANIMATION, on the engine's own three splines -- see MM_SPL_*
       in mainmenu.c. `anim_t' is seconds since the focus last moved, `anim_prev'
       the row it left (so that row can play the unfocus curve while the new one
       plays the focus curve), and `press_*' the row that fired and how far into
       its recoil it is; press_t is negative when nothing is pressed. */
    float anim_t;
    int   anim_prev;
    float press_t;
    int   press_row;

    mm_action action;   /* cleared every step */
    mm_cue    cue;      /* cleared every step */

    /* The pad, edge-detected here so the caller hands over raw buttons and the
       menu owns the repeat rule -- exactly what menu.c does. */
    unsigned int prev_buttons;

    /* animCar's drawer -- see mm_car_draw. */
    mm_car_draw car_draw;
    void       *car_ctx;

    /* ---- the Select player page. `psel` is a row of the VIEW, not of the
       roster: pview[psel] is the profile it names. */
    int   pfocus;               /* MM_P_* */
    int   psel;                 /* 0 .. player_count()-1, in view order */
    int   ptop;                 /* the first visible row */
    int   psort;                /* MM_SORT_* */
    int   pview[PL_MAX];
    int   pnview;
    int   parmed;               /* the stop a touch went down on, or -1 */

    /* ---- the Garage and its upgrade page. `gkind' is which of the three
       parts dlgSETDETAIL is showing (GAR_BOOSTER..GAR_TIRES) and `gsel' which
       of its three levels the picker is on -- a level the profile may not own,
       because the picker is a CATALOGUE: the status text under the picture is
       what says which of the three is buyable and which is fitted. */
    int   gfocus;               /* MM_G_* */
    int   gkind;
    int   gsel;
    int   garmed;
    /* HOW MANY SKINS THE LOADED CAR HAS, which is not a fact this file can
       reach -- it is carparts_t::n_skin, a property of the packed scene, and
       menu.c already carries it for the same reason. The caller writes it; 1
       until a car is loaded, so Next skin can never offer paint that is not
       there. See render-car.md, "The paint: four skins a car". */
    int   gskins;
    /* What MM_MODAL_ASK's Yes commits -- one of the four things the Garage
       spends or fetches money for. Private to mainmenu.c. */
    int   gask;
    /* WHICH PAGE OPENED THE SHOP, so its Back button unwinds one step to the
       page the thumb came in from. Two pages can now: the quick-race summary's
       own Garage bar and the championship ladder's. */
    int   gfrom;

    /* ---- the modal over it */
    int   modal;                /* MM_MODAL_* */
    int   mfocus;               /* a key index, MM_MODAL_OK or _CANCEL */
    int   mshift;               /* the grid is drawing capitals */
    int   marmed;
    const char *msay;           /* MM_MODAL_SAY's one line */
    char  mname[PL_NAME];
    /* THE GARAGE'S QUESTION, composed: the engine's own format string with a
       car's name or a money figure in it. Here rather than on the stack because
       `msay' is a borrowed pointer and the modal outlives the frame that raised
       it. 256 because the longest of them is 40801's two lines with three part
       names in it -- "Do you want to sell car and upgrades? / car: Warhammer
       WH, details: WHB-1A "Devastator", WH-Nitro-B "Road Prince",
       WHT-1A "Meatgrinder"" is 150 characters, and a question that truncates
       reads as a bug rather than as a question. */
    char  gline[256];

    /* ---- multiplayer. `mfocus_srv' is which row of the server modal is lit;
       the rest is the lobby's own cursor. `lsel' is the table row the Kick
       button would take, which is a row of the ROSTER and not of a view. */
    int   mfocus_multi;         /* MM_M_* on the front page */
    int   marmed_multi;
    int   lfocus;               /* MM_LB_* in the lobby */
    int   larmed;
    int   lview;                /* MM_L_* */
    int   lsel;                 /* the highlighted roster row */
    int   srvsel;               /* the server modal's own cursor */

    /* ---- the championship. `csel' is the row of the ladder the cursor is on,
       which is a TRACK index (TRACKS[] order) and not a row of a view: the
       ladder is always all ten in the same order, locked rows included, because
       a ladder you cannot see the top of is not a ladder. `ctop' scrolls it
       when the table is shorter than ten rows. */
    int   cfocus;               /* MM_C_* */
    /* WHICH PAGE THE THREE SIBLING VIEWS WERE ENTERED FROM. dlgMAPINFO and
       dlgSTAT are on the championship's navigation column as well as the
       quick-race page's, so the first nav bar, the header and the green button
       all have to know which way home is. MM_PAGE_QUICK unless the ladder
       opened them. */
    int   qfrom;
    int   csel;                 /* 0..9 -- the chosen track */
    int   ctop;                 /* the first visible row */
    int   carmed;
    int   rfocus;               /* MM_R_* on dlgCHRACE */
    int   rarmed;
} mainmenu_t;

void mainmenu_set_car_draw(mainmenu_t *m, mm_car_draw fn, void *ctx);

/* `tex` is copied. Leaves the focus on Quick race, which is the one live mode. */
void mainmenu_init(mainmenu_t *m, const mainmenu_tex *tex);

/* Open the Select player page. `with_create` also raises the name modal, which
   is what an empty roster wants: the first launch has nothing to select and the
   only thing to do on the page is make somebody. */
void mainmenu_open_players(mainmenu_t *m, int with_create);

/* Rebuild the list's view order off `psort` and put the cursor on the current
   profile. Called by the page itself; exposed for the harness and for main.c,
   which has to call it after it scans the roster. */
void mainmenu_players_sync(mainmenu_t *m);

/* The page's focus ring, and whether a stop does anything in this build --
   MM_P_FACE does not, and MM_P_REMOVE does not when there is one profile. */
int  mainmenu_p_live(const mainmenu_t *m, int stop);

/* Which stop is under (x, y), or -1. `row` comes back as the LIST row when the
   point is inside the table, so one call answers both questions the way
   mainmenu_row_at does for the main menu. Exposed for the harness. */
int  mainmenu_p_stop_at(const mainmenu_t *m, int screen_w, int screen_h,
                        float x, float y, int *row);

/* Which modal control is under (x, y): a key index, MM_MODAL_OK, MM_MODAL_CANCEL
   or -1. */
int  mainmenu_modal_at(const mainmenu_t *m, int screen_w, int screen_h,
                       float x, float y);

/* The character key `k` types at the current shift state, or 0 for the three
   that are not characters (space is ' '). Exposed so the harness can spell a
   name without a second copy of the layout. */
char mainmenu_kb_char(const mainmenu_t *m, int k);

/* One frame. `buttons` is SceCtrlData.buttons (menu.h supplies the bits on the
   host); `tp` may be NULL where there is no panel. Reads `action` and `cue`
   afterwards. */
void mainmenu_step(mainmenu_t *m, unsigned int buttons, const touch_state *tp,
                   int screen_w, int screen_h, float dt);

/* Draws between ui_begin/ui_end -- the caller brackets it, because main.c draws
   the menu and the settings overlay in one pass. */
void mainmenu_draw(const mainmenu_t *m, int screen_w, int screen_h);

/* THE CURRENT SIBLING PAGE's row under (x, y): one of the page's own enums,
   MM_Q_RACE, MM_Q_BACK, MM_Q_NAV + MM_QB_*, or -1. `left` comes back 1 when the
   point is in the row's own back-arrow, which is what makes one enum walk both
   ways under a thumb; it is always 0 for a bar. */
int  mainmenu_q_row_at(const mainmenu_t *m, int screen_w, int screen_h,
                       float x, float y, int *left);

/* WHICH SCREENSHOT of `shotList' is under (x, y), 0..MM_N_SHOTS-1, or -1. Only
   answers on Map and info. The one already selected answers like any other --
   tapping it is a no-op, which is the step function's business, exactly as it
   is for the main menu's carousel. */
int  mainmenu_shot_at(const mainmenu_t *m, int screen_w, int screen_h,
                      float x, float y);

/* How many focus stops the page has, and the `i`th of them. Exposed because the
   ring is per page and a harness that rebuilds it is testing its own copy. */
int  mainmenu_q_nfocus(int page);
int  mainmenu_q_stop(int page, int i);

/* THE GARAGE'S OWN RING, the same two questions -- MM_G_SKIN is not on the part
   page and the ring skips it there. */
int  mainmenu_g_nfocus(int page);
int  mainmenu_g_stop(int page, int i);

/* Whether a stop on the Garage or its part page does anything RIGHT NOW, which
   here depends on the profile's own cash and parts and not only on the build:
   Buy car denies on a car already in the garage, Downgrade on a part at
   Default. The bars are drawn in the artists' grey when this is 0, which is
   what the game's own two screenshots have -- Buy car grey beside a car you
   own, Downgrade grey beside a part at Default. */
int  mainmenu_g_live(const mainmenu_t *m, int stop);

/* The Garage's row under (x, y): one of MM_G_*, or -1. `left' comes back 1 in
   the enum's own back arrow, like mainmenu_q_row_at's. */
int  mainmenu_g_row_at(const mainmenu_t *m, int screen_w, int screen_h,
                       float x, float y, int *left);

/* Open the Garage (or, with `kind' 0..2, one of its three part pages). */
void mainmenu_open_garage(mainmenu_t *m, int kind);

/* -------------------------------------------------------- multiplayer */

/* Open dlgMULTIPLAYER. Does not touch the socket; `Create game' and `Join game'
   do that, so a player who walks in and out of this page costs nothing. */
void mainmenu_open_multi(mainmenu_t *m);

/* Open the lobby on `view' (MM_L_*). Called by the page itself when a host or a
   join succeeds, and by the harness. */
void mainmenu_open_lobby(mainmenu_t *m, int view);

/* The front page's ring, its hit test, and whether a stop can act -- Race is
   dead on this page (there is no game yet) and is drawn in the artists' grey,
   which is what the game's own screenshot of it has. */
int  mainmenu_m_row_at(const mainmenu_t *m, int screen_w, int screen_h,
                       float x, float y);
int  mainmenu_m_live(const mainmenu_t *m, int stop);

/* The lobby's ring, its hit test, and the same question. `left' comes back 1 in
   an enum's own back arrow. `row' comes back as the ROSTER row when the point
   is inside the table, which is what the Kick button aims with. */
int  mainmenu_l_nfocus(int view);
int  mainmenu_l_stop(int view, int i);
int  mainmenu_l_live(const mainmenu_t *m, int stop);
int  mainmenu_l_row_at(const mainmenu_t *m, int screen_w, int screen_h,
                       float x, float y, int *left, int *row);

/* The four skill names, and the field each one fields on `track` -- counted off
   ai_data.h's own AI<n>Races masks, which is where the difference comes from.
   See ai_set_skill_field. */
/* ---------------------------------------------------- the championship's two */

/* Open the ladder on the profile's own current track, or on the first one that
   is open when that one is not. */
void mainmenu_open_champ(mainmenu_t *m);

/* Whether a stop on either championship page does anything right now. The Race
   button on the ladder is dead while the chosen track is locked or the profile
   cannot pay its fee; the whole page is dead with no profile. */
int  mainmenu_c_live(const mainmenu_t *m, int stop);
int  mainmenu_r_live(const mainmenu_t *m, int stop);

/* Which stop of dlgCHAMP is under (x, y), or -1 -- and, when it is the table,
   `row' comes back as the ladder row (0..9) so a touch can both focus the list
   and move its cursor in one press. `row' is -1 otherwise. Exposed for the
   harness, as every other page's hit test is. */
int  mainmenu_c_stop_at(const mainmenu_t *m, int screen_w, int screen_h,
                        float x, float y, int *row);

/* Which of dlgCHRACE's own two buttons is under (x, y), or -1. */
int  mainmenu_r_stop_at(const mainmenu_t *m, int screen_w, int screen_h,
                        float x, float y);

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

/* HOW FAR THAT ROW'S BAR STANDS OUT, in 800x600 design pixels, this frame: the
   focus, unfocus and press curves out of Splines/, evaluated on the menu's own
   clock. 0 is the row at rest. Exposed for the harness, which otherwise has to
   own a second copy of three splines to check the slide against. */
float mainmenu_row_slide(const mainmenu_t *m, int row);

/* The label the row carries, for the harness and for the log. */
const char *mainmenu_row_name(int row);

#endif /* MAINMENU_H */
