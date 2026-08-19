/*
 * race_ui.h -- the in-race HUD: the minimap, the place, the two clocks and the
 * two gauges. All FOUR on the game's own artwork, and every position, angle and
 * transform in it out of the shipped data.
 *
 * WHAT WAS IN THE DATA, and it was the whole feature
 * -------------------------------------------------
 * This started as "invent a HUD", and there was nothing to invent. Listing what
 * ships -- the standing rule, again -- turned up the art, the layout, the
 * transform and the FONT:
 *
 *   Textures.1/trackmap_1..10   one 512x512 painted map per track
 *   Textures.1/map_arrow        64x32, TWO 32x32 cells: the player's marker and
 *                               an opponent's
 *   Textures.1/map_cp           64x32, likewise: the checkpoint being headed for
 *                               and the rest
 *   Textures.1/place1..place6   128x128 each, `1st' .. `6th' as word art
 *   Textures.1/lap              128x128, the word `Lap'
 *   Textures.1/cockpit_sp1      the dial: disc, silver rim, gold sweep track
 *   Textures.1/cockpit_sp2      the same ring in two tones, red over the sweep
 *   Settings/map.ini            the minimap's screen corner
 *   Settings/cockpit.ini        the badges, the lap number, the sweep's angles
 *   Settings/cockpit_time.ini   the two clocks and the lap-time blink
 *   Language/English/Smash20    the engine's font, 20 px, atlas + character order
 *   Language/English/Smash26    the same at 26 px
 *   RCCars.exe 0x56c378/0x56c468  the per-track world -> map transform
 *   RCCars.exe 0x56db70         the map panel's own outline
 *
 * and one asset settles the layout on its own: **`Textures.1/opt_cock_all.csi`
 * is a screenshot of the finished HUD**, the cockpit option's preview thumbnail
 * -- `3rd' top left, two clocks top centre, `Lap 1/6' top right, `100 Boost'
 * bottom left, `29 mph' bottom right -- and `opt_cock_m_1.csi' is the same shot
 * showing the minimap in place with the green arrow on it. Between the .ini
 * files and those two thumbnails there was no layout left to guess at.
 *
 * All of it is generated into hud_data.h by rccars_re/gen_hud_data.py, which
 * cites the address or the key behind every number and CHECKS the ones that can
 * be checked -- see its docstring.
 *
 * AND ONE THING IS KNOWN WRONG: the minimap's arrow lands 3 to 19 m off the
 * painted road on eight of the ten tracks. The arithmetic here IS the engine's,
 * read out of FUN_004b8500 + 0x410430 instruction by instruction, and two tracks
 * come out sub-metre -- so the reading of the tables is right and the residual is
 * something else. rccars_re/mapcheck.py prints the per-track number; the leads are
 * the map module's two untranscribed marker draws (0x4b7fc8, 0x4b845f) and its
 * vertex builder (0x4b87c0). See known-issues.md before believing the arrow.
 *
 * THE FONT, and a documented negative that had rotted
 * --------------------------------------------------
 * `ui.h` said, for years, that "the game ships no reusable glyph
 * atlas -- Faces/ and FacesSys/ are avatar portraits and Interface.sb has only
 * dialog skins". The claim was made over `Textures.1/2/3` and `Interface.sb`,
 * and both of the game's fonts are in `Language/English/`: `Smash20.csi` and
 * `Smash26.csi`, 90 glyphs each on a 10 x 9 grid, with the character order in a
 * matching 91-byte `.dat` and the metrics in `Settings/smash20.ini` /
 * `smash26.ini`. `font.h`'s baked Consolas stays where it is -- the menu must
 * not depend on an asset load -- but the numbers on the HUD are the game's own
 * letters. See traps.md, "A DOCUMENTED NEGATIVE IS STILL A CLAIM".
 *
 * WHAT IS THE PORT'S
 * ------------------
 *   - the race CLOCK. The original's arrives from its game manager, the same one
 *     that sends the countdown; there is no such thing here, so this file keeps
 *     it, off the same dt everything else banks
 *   - how the dial is COMPOSITED: cockpit_sp1 whole as the track, a sector of
 *     cockpit_sp2 revealed up to the value. The two textures and the sweep are
 *     the game's; that this is how they go together is read off the art (see
 *     hud_data.h's dial block) and not off any code
 *   - which quantity each dial shows -- boost left, speed right -- which is what
 *     opt_cock_all's own labels say, so it is barely the port's
 *   - the marker size on the minimap, and the 800x600 -> screen mapping
 *
 * WHY IT IS ITS OWN FILE, and it is the same reason as hud.c: nothing compiles
 * main.c. This links against ui.c alone -- no scene.h, no rb.h, no ai.h, no GL
 * headers -- so every texture and every number arrives as an argument and
 * ui_test's recorder can read back what really went on screen.
 */

#ifndef RACE_UI_H
#define RACE_UI_H

#include "hud_data.h"

/* Enough for every shipped track: AI_MAX_OPPONENTS is 5 and CP_MAX is 8. Kept
   local rather than included, so this file stays free of ai.h and checkpoint.h
   the way hud.c is free of prop.h. */
#define RUI_MAX_OTHERS 8
#define RUI_MAX_CP     8

/* ------------------------------------------------------------ the port's own */

/* THE MINIMAP MARKERS' SIZE, as a fraction of the panel's side. The engine
   carries no key for it; measured off opt_cock_m_1, where the green arrow is
   13-15 px inside a 167 px panel. map_arrow's cells are square, so one number
   does both axes. */
#define RUI_MARK_FRAC 0.084f

/* The panel's dark plate, drawn as the outline shape one marker's worth larger
   than the map itself so the map reads as inset in a frame. opt_cock_m_1 shows a
   silver rim around a dark plate around the map; the rim's own texture has not
   been identified, so this is the plate alone, and it is the outline's shape
   rather than a rectangle -- the geometry is the game's even where the art is
   not. */
#define RUI_PLATE_GROW 1.055f
#define RUI_PLATE_ALPHA 0.55f

/* The number and the label inside a dial, as multiples of the small font's
   letSizeY. opt_cock_all's `100' is about 30 px against a 65 px disc radius and
   its `Boost' about 12; the small font's cell is 28. */
#define RUI_DIAL_VAL_SCALE 1.05f
#define RUI_DIAL_LBL_SCALE 0.45f

/* How many quads the value sector is cut into. 161 degrees over 48 is 3.4
   degrees a quad, a 5 px chord at the dial's 92 px radius. */
#define RUI_ARC_SEGS 48

/* The lap-time line blinks while it is holding a FINISHED lap's time -- that is
   what timeLapBlinkInSec (4.2 s) names. This is the blink's own period; the
   engine carries no key for it, and half a second is the rate a value that has
   stopped changing has to flash at to read as held rather than as broken. */
#define RUI_BLINK_PERIOD 0.5f

/* ------------------------------------------------------------------ the state */

/* Every texture the HUD draws, bound once by the caller. A 0 in any of these
   draws that element's font fallback, or skips it where there is nothing to fall
   back to -- the same rule hud.c and countdown.c follow, so a props.vsc packed
   without the extra art degrades rather than crashing. */
typedef struct {
    unsigned int map;        /* trackmap_<track+1>, out of the TRACK scene */
    unsigned int arrow;      /* map_arrow */
    unsigned int cp;         /* map_cp */
    unsigned int place[6];   /* place1 .. place6 */
    unsigned int lap;        /* lap */
    unsigned int dial;       /* cockpit_sp1 */
    unsigned int dial_arc;   /* cockpit_sp2 */
    unsigned int font_big;   /* Smash26 */
    unsigned int font_small; /* Smash20 */
} race_ui_tex;

/* What to show this frame. The caller fills it from the physics, the checkpoint
   layer and the AI; nothing in here is a pointer into any of them. */
typedef struct {
    float car_x, car_z;           /* world metres */
    float car_yaw;                /* radians; 0 faces +Z, the rig's convention */

    int   n_others;               /* opponents, world metres */
    float other_x[RUI_MAX_OTHERS];
    float other_z[RUI_MAX_OTHERS];

    int   n_cp;                   /* checkpoint markers, world metres */
    float cp_x[RUI_MAX_CP];
    float cp_z[RUI_MAX_CP];
    int   cp_next;                /* which one is being headed for, or -1 */

    int   place, n_racers;        /* 1-based */
    int   lap, n_laps;            /* 1-based; n_laps <= 0 hides the `/n' */

    float speed, speed_max;       /* m/s */
    float boost, boost_max;       /* meter units */
} race_ui_state;

typedef struct {
    race_ui_tex tex;

    /* The per-track world(x, z) -> art(u, v) affine, solved once out of
       MAP_CALIB: u = m[0] x + m[1] z + m[2], v = m[3] x + m[4] z + m[5]. */
    float m[6];
    int   have_map;
    int   track;

    /* The clocks. `lap_hold` is the finished lap's time being blinked, negative
       when the live lap time has the line. */
    float t_race;
    float t_lap;
    float lap_hold;
    float hold_left;
    float blink_t;
    int   running;
} race_ui_t;

/* ------------------------------------------------------------------- the API */

void race_ui_init(race_ui_t *r, const race_ui_tex *tex);

/* Solve the map transform for `track` (0..MAP_N_TRACKS-1) and bind its map
   texture. A track outside that range, or a 0 texture, leaves the minimap out
   and everything else on. */
void race_ui_set_track(race_ui_t *r, int track, unsigned int map_tex);

/* The race is starting: both clocks to zero, the blink cleared. Not a memset --
   the textures and the map transform outlive it, the same way hud_reset keeps
   its binding. */
void race_ui_start(race_ui_t *r);

/* A lap line was crossed. The lap clock's reading is held and blinked for
   HUD_LAP_BLINK seconds and the clock restarts. */
void race_ui_lap(race_ui_t *r);

/* The race is over: the clocks stop where they are. */
void race_ui_stop(race_ui_t *r);

/* Both clocks and the blink, off the frame's own dt -- so a dt of 0 while the
   menu is up stops the race clock along with everything else, which is what a
   paused race means. */
void race_ui_step(race_ui_t *r, float dt);

/* WORLD -> MINIMAP, in the art's own 0..1. -> 0 and touches nothing when there
   is no transform for this track. Public because it is the one piece of real
   arithmetic in this file and ui_test measures it directly. */
int  race_ui_map_uv(const race_ui_t *r, float x, float z, float *u, float *v);

/* The lap-time line's reading, and whether it is a held one. */
float race_ui_lap_time(const race_ui_t *r, int *held);

void race_ui_draw(const race_ui_t *r, const race_ui_state *s,
                  int screen_w, int screen_h);

#endif /* RACE_UI_H */
