/*
 * menu.h -- the START menu: track, car, tuning parts, texture quality.
 *
 * The menu owns only its own state and drawing. Anything with a cost -- loading
 * a track, swapping the car model, respawning -- is raised as a request that
 * main.c services and clears, so scene ownership stays in one place.
 *
 * Tuning parts map straight onto the two upgrade indices the transcribed model
 * actually reads: rb_car.tire_upgrade feeds carTireGrip (0x004ee180) and
 * rb_car.reso_upgrade feeds carEngineAccel's speed and acceleration scales. Both
 * are 0..3 and both were already in rb_car with nothing driving them. The
 * multiplier each level applies is shown next to it rather than given an
 * invented marketing name -- the numbers are the game's, the names would not be.
 */

#ifndef MENU_H
#define MENU_H

/* The state machine is pure logic and worth testing on the host, where a wrong
   wrap or a request that never clears is a two-second check rather than a
   redeploy (rccars_re/menu_test.c). All it needs from the SDK is the button
   bits, so on anything that is not the Vita they are supplied here. */
#ifdef __vita__
#include <psp2/ctrl.h>
#else
#define SCE_CTRL_START    0x00000008u
#define SCE_CTRL_UP       0x00000010u
#define SCE_CTRL_RIGHT    0x00000020u
#define SCE_CTRL_DOWN     0x00000040u
#define SCE_CTRL_LEFT     0x00000080u
#define SCE_CTRL_CIRCLE   0x00002000u
#define SCE_CTRL_CROSS    0x00004000u
#define SCE_CTRL_SQUARE   0x00008000u
#endif

/* The rows, in display order. Named rather than counted so a test can aim at
   one without walking the list -- adding the Booster row silently broke a test
   that pressed DOWN a fixed number of times. Rows below MENU_FIRST_ACTION are
   pickers driven by left/right; the rest fire on CROSS. */
enum {
    MENU_TRACK = 0,
    MENU_CAR,
    MENU_SKIN,
    MENU_TIRES,
    MENU_RESO,
    MENU_BOOST,
    MENU_VOL_SFX,
    MENU_VOL_MUSIC,
    MENU_TEXQUAL,
    MENU_TEXORDER,
    MENU_CARLIGHT,
    MENU_INTRO,
    MENU_RESTART,
    MENU_RESUME,
    MENU_QUIT,
    MENU_ROWS
};
#define MENU_FIRST_ACTION MENU_RESTART

/* What the last input did, for the caller to turn into one of the game's own
   interface sounds. menu.c stays free of the audio layer -- the same reason
   track loads are raised as requests -- and menu_test.c can assert the cue
   without linking a mixer. Cleared by whoever reads it. */
enum {
    MENU_CUE_NONE = 0,
    MENU_CUE_MOVE,      /* changed row */
    MENU_CUE_CHANGE,    /* changed a value */
    MENU_CUE_SELECT,    /* fired an action, or opened */
    MENU_CUE_CLOSE
};

/* Volume rows are integer notches so the state stays exactly comparable across
   a save/restore; sfx_volumes takes the 0..1 form. */
#define MENU_VOL_STEPS 10

/* MUST MATCH scene.h's SCENE_TEX_QUALITY_LEVELS -- mirrored rather than included
   because menu.c is deliberately free of the GL layer, so menu_test can build it
   without vitaGL or the testgl stubs. main.c includes both headers and carries a
   compile-time check that the two agree. */
#define MENU_TEXQUAL_LEVELS 3

/* Overkill, Buggy, Hummer. The engine's own bound, not a guess: FUN_0049fc80
   rejects a car index outside 0..2 and rb_data.h's RB_CARS has three rows. Named
   because the skin selection is one slot PER car and an array wants a size. */
#define MENU_N_CARS 3

/* MUST MATCH carparts.h's CARPARTS_SKINS -- mirrored for the same reason
   MENU_TEXQUAL_LEVELS is: carparts.h includes scene.h and so drags in the GL
   layer, and menu.c is kept clear of it so menu_test can build without vitaGL.
   main.c includes both and carries the compile-time check that they agree. */
#define MENU_SKINS 4

typedef struct {
    int open;
    int row;

    /* current selections */
    int track;
    int car;            /* 0 Overkill, 1 Buggy, 2 Hummer */
    /* The paint, PER CAR -- one slot each, not one selection shared. The four
       skins of the Overkill are four different textures from the four of the
       Buggy (car_askin1<n> against car_askin2<n>), so "skin 3" means a different
       colour on each car and remembering it per car is the only reading of the
       picker that survives switching. Indexed by `car`; 0..MENU_N_CARS-1.
       Purely visual, and live -- carparts_apply re-points a texture. */
    int skin[MENU_N_CARS];
    /* How many skins the CURRENTLY LOADED car actually has, 1..MENU_SKINS.
       The menu cannot know this -- it is a property of the packed scene, and a car
       packed without the three extra skin atlases has exactly one -- so main.c
       writes it after every load_car from carparts_t::n_skin. 1 until then, which
       pins the row rather than letting it offer paint that is not there. */
    int skins;
    int tires;          /* 0..3 -- grip, and the wheel texture */
    int reso;           /* 0..3 -- top speed and acceleration. No visual: the
                           resonator is the one upgrade of the three with no
                           mesh or texture family in Car.sb */
    int boost;          /* 0..3 -- the exhaust the car wears (UPGRADES1..4), and
                           the size and refill rate of the boost meter.
                           UPGRADES.ini's [BOOSTERS] table is engine[7..10] and
                           its one consumer is FUN_004f3800; see rb_boost_update.
                           Live, like the other two: the model reads the index
                           every frame, no reload. */
    /* 0 = full, 1 = half, 2 = quarter resolution -- the game's own RenderQual /
       VIDEO_TexQual, implemented as mip levels skipped at upload. The three
       levels land on exactly the sizes of the original's Textures.1/.2/.3.
       See scene.h. Changing it needs the scene reloaded, hence req_reload. */
    int tex_quality;

    /* 0 = standard 565 (red high) -- correct on real hardware. 1 = swap red
       and blue, which is what Vita3K needs because it reads GXM's U5U6U5_RGB
       the other way round. See scene.h; needs a reload, like tex_quality. */
    int tex_swap_rb;

    /* The car's own directional light and the lightmap that dims it under cover
       -- carlight.h. 1 by default, because it is what the original does; the row
       exists because it is the one thing here that changes how the CAR looks
       rather than the world, and A/B against the flat old appearance is the only
       way to judge it. No reload: it is GL state and a per-frame step. */
    int car_light;
    /* THE LAUNCH MOVIES, Config.gm's own `AutoRunIntro'. 1 in retail and 1 here.
       The port used to spell it only by DELETING assets/intro.vid, which a
       player on a console cannot do -- so 104 seconds of logos had no switch
       inside the game (docs/known-issues.md). Read once, at boot, before the
       intro loop runs; changing it takes effect on the next launch, which is
       what the row says. */
    int intro;

    int vol_sfx;        /* 0..MENU_VOL_STEPS */
    int vol_music;

    int cue;            /* MENU_CUE_*, set by menu_input, cleared by the caller */

    /* requests for main.c: -1 / 0 means nothing pending */
    int req_track;      /* load this track */
    int req_car;        /* load this car model */
    int req_restart;    /* respawn at the race start */
    /* Re-upload the textures at the newly chosen quality. Both the track and the
       car have to be reloaded: the mip levels are picked in scene_load, so
       nothing already on the GPU changes by itself. */
    int req_reload;
    int req_quit;
} menu_t;

void menu_init(menu_t *m, int track, int car);

/* Edge-triggered: pass this frame's buttons and the previous frame's. */
void menu_input(menu_t *m, unsigned int buttons, unsigned int prev);

void menu_draw(const menu_t *m, int screen_w, int screen_h);

#endif
