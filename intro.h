/*
 * intro.h -- THE LAUNCH SEQUENCE: the three logo/intro movies, then the
 * loading screen, in the order the game boots in.
 *
 * Both halves of this were recovered whole out of the retail exe, and neither
 * had ever appeared in these notes.
 *
 * ---------------------------------------------------------------- THE ORDER
 *
 * `FUN_004a25c0` is the engine's startup state machine -- one switch over 29
 * states, stepped once per frame. Three of them are this file:
 *
 *   state 0x0f   FUN_004a3030(), the intro's own state machine, until it
 *                reports done
 *   state 0x12   advance the interface's load, and at its state 0xe run
 *                FUN_004dd5e0() -- the interface load itself
 *   state 0x13   while the intro's state is past 0xb, DRAW THE LOADING SCREEN
 *
 * so the movies come first, the loading screen covers the interface load, and
 * the main menu comes up after it. Turning the movies off does not remove the
 * loading screen: `AutoRunIntro` 0 sends the intro state machine straight to
 * 0xd, which is still past the 0xb the loading screen is gated on.
 *
 * -------------------------------------------------------------- THE MOVIES
 *
 * `Tracks/Intro.dat` is ONE MPEG-1 program stream, 640x480 at 25 fps, 104.62 s,
 * with MP2 audio -- and `Scripts/intro.ini` cuts it into three parts by time:
 *
 *   part_1   0.00 .. 10.00   the 1C company logo
 *   part_2  10.00 .. 17.00   the Creat Studios logo
 *   part_3  17.20 .. -1      the SmashCars intro film (87.4 s)
 *
 * -1 means "to the end of the file". The bounds are multiplied by 1e7 before
 * use, so they are DirectShow REFERENCE_TIMEs -- which is what the retail
 * player is, and why the Vita cannot simply play the file: sceAvcdec is an
 * H.264 decoder and the OS has no MPEG-1 anything. rccars_re/pack_vid.py
 * transcodes the parts offline; see avc.h.
 *
 * The 0.2 s hole between part 2 and part 3 is the authors' own -- five frames
 * of the movie that the shipped configuration never shows.
 *
 * TWO THINGS ABOUT THE SHIPPED .vid ARE THIS PORT'S CHOICES, NOT THE GAME'S, and
 * both are made in the PACKAGING rather than here, because the runtime's job is
 * to play every part its table names -- which is exactly what the engine does
 * with intro.ini's:
 *
 *   part 1, the 1C logo, is NOT SHIPPED. `pack_vid.py --parts 2,3'. Ten seconds
 *   of a publisher's logo every launch, on a port nobody is selling.
 *
 *   the film's baked-in LETTERBOX is cropped away. `--crop 640:368:0:56': the
 *   source is a 640x368 picture inside a 640x480 frame, 56 rows of STUDIO black
 *   (Y=16, which is why cropdetect's default threshold does not see it) top and
 *   bottom. 640x368 is 1.74:1 against the Vita's 1.765, so it fills the screen
 *   with a 1.5% stretch instead of with two black bands.
 *
 * The container carries the part table it carries, and `introtest` derives which
 * of intro.ini's parts each shipped one IS rather than being told.
 *
 * SKIP IS PER PART, and that is not a simplification. `FUN_004a2f10` takes
 * WM_KEYDOWN, checks the key against 0x1b (ESC) and acts only while the intro
 * state is 2 or 3 -- i.e. while a part is starting or playing. The state
 * machine then goes 3 -> 4 -> 2 and the NEXT part starts. One press skips one
 * logo; three presses skip the sequence.
 *
 * ------------------------------------------------------- THE LOADING SCREEN
 *
 * A table of four rows at 0x56b8a0, stride 0x18: a name, a normalised screen
 * rect, and a texture slot the engine fills on first use. In file order:
 *
 *   Desktop   0.00000, 0.00000 .. 1.00000, 1.00000    drawn OPAQUE
 *   logoRC    0.16000, 0.04000 .. 0.80000, 0.89333    alpha blended
 *   logo1C    0.03125, 0.01167 .. 0.19125, 0.22500    alpha blended
 *   logoCR    0.80875, 0.01167 .. 0.96875, 0.22500    alpha blended
 *
 * The draw loop passes flag 4 for every row after the first, which is the blend
 * bit in `FUN_00471610` -- so the graffiti desktop is the background and the
 * three logos sit over it: RC Cars big in the middle, 1C top left, Creat
 * Studios top right. Under it the engine draws one line of text in the pixel
 * rect {0, h - 90, w, h}.
 *
 * ALL FOUR RECTS ARE SQUARE at the 640x480 the game ran at -- 409.6 px for the
 * badge and 102.4 px for each corner logo, against 512x512 and 128x128 art. So
 * this port places them inside a 4:3 box fitted to the screen HEIGHT and
 * centred, rather than stretching the fractions across 960x544 and making three
 * round logos into three ovals. The desktop still fills the screen, which is
 * what mainmenu.c already does with the same texture.
 */

#ifndef INTRO_H
#define INTRO_H

#include "touch.h"

/* ------------------------------------------------------- the loading screen */

typedef struct {
    unsigned int desktop;       /* `Desktop', the graffiti background */
    unsigned int logo_rc;       /* `logoRC',  the badge  -- 512x512 */
    unsigned int logo_1c;       /* `logo1C',  top left   -- 128x128 */
    unsigned int logo_cr;       /* `logoCR',  top right  -- 128x128 */
} intro_tex;

/* Draw the loading screen. `caption` is the line in the engine's own bottom
 * band, or NULL for none; `progress` in [0,1] draws a bar under it, and a
 * negative value leaves it out -- a load with no measure should not draw a bar
 * that stands still.
 *
 * Brackets its own ui_begin/ui_end and paints its own ground, so it covers
 * whatever was on the screen: it is a whole page, not an overlay. It does NOT
 * clear and does NOT swap -- the caller owns the frame, because on the port the
 * loads it covers are blocking calls between two swaps rather than a state
 * machine stepped once a frame. */
void intro_load_screen(const intro_tex *t, int screen_w, int screen_h,
                       const char *caption, float progress);

/* ----------------------------------------------------------- the launch movie */

#define INTRO_MAX_PARTS 4       /* FUN_004a3030 clamps introNum to 4 */

/* What a step reports. */
#define INTRO_PLAYING 0
#define INTRO_DONE    1

typedef struct {
    unsigned int first, count;  /* frame range, out of the .vid's part table */
} intro_part;

typedef struct {
    int state;                  /* INTRO_ST_*, below -- the engine's numbering */
    int open;

    /* the container */
    void *fp;                   /* FILE * */
    unsigned int coded_w, coded_h;
    unsigned int fps_n, fps_d;
    unsigned int n_frames, n_parts, max_au;
    unsigned int data_off;
    intro_part part[INTRO_MAX_PARTS];
    unsigned int *ftab;         /* 2 words per frame: offset, size | IDR bit */

    /* the read-ahead window over the access units */
    unsigned char *win;
    unsigned int win_cap, win_off, win_len;

    /* where we are */
    int cur;                    /* which part */
    unsigned int fed;           /* frames of THIS part fed to the decoder */
    double clock;               /* seconds into this part */
    int have_audio;             /* the part's own mp3 opened */
    unsigned int audio_base;    /* the mixer's frame counter when it started */
    unsigned int audio_last;    /* and what it read last tick -- the watchdog */
    float audio_stall;          /* seconds the audio clock has not moved */
    float wall;                 /* seconds of real dt this part has taken */
    int decoder;                /* avc_open succeeded */
    int skipped;                /* parts ended by a button rather than by time */

    char dir[192];              /* where intro_N.mp3 live */
} intro_t;

/* The engine's own state numbers, so the transcription can be read against
   FUN_004a3030. 6..0xc are the seven frames it spends letting the video window
   go away before the loading screen appears; this port has no such window and
   goes straight from STOP to DONE. */
#define INTRO_ST_INIT  1
#define INTRO_ST_START 2
#define INTRO_ST_PLAY  3
#define INTRO_ST_NEXT  4
#define INTRO_ST_STOP  5
#define INTRO_ST_DONE  13

/* Open `vid_path` (assets/intro.vid) and bring the decoder up. `audio_dir` is
 * where intro_1.mp3 ... intro_N.mp3 are, or NULL for a silent intro.
 *
 * Returns 1 if there is something to play. Returns 0 -- having left `in` in the
 * DONE state, so a caller can step it regardless -- if the file is absent or
 * unusable, which is how this port spells `AutoRunIntro` 0: delete the asset
 * and the launch goes straight to the loading screen. */
int intro_open(intro_t *in, const char *vid_path, const char *audio_dir);

void intro_close(intro_t *in);

/* One frame of the sequence. Feeds the decoder up to the audio clock, honours
 * a skip, and advances the part. -> INTRO_PLAYING or INTRO_DONE.
 *
 * `buttons`/`prev` are the pad's edges, the way menu_input takes them (menu.h
 * supplies the SCE_CTRL_ bits on the host); CROSS, CIRCLE or a touch ends the
 * part that is playing, which is what ESC does in the retail player. START ends
 * the WHOLE sequence, which it does not -- see intro.c.
 *
 * `dt` is only the fallback clock: with audio up, the audio is the clock. */
int intro_step(intro_t *in, unsigned int buttons, unsigned int prev,
               const touch_state *tc, float dt);

/* Draw the current picture, FILLING the screen, over black. Draws nothing but
 * the black where there is no decoder. See intro.c on why it fills rather than
 * letterboxes, and on what that costs. */
void intro_draw(const intro_t *in, int screen_w, int screen_h);

/* Which part is up (1-based) and how many there are -- for the log line. */
int intro_part_no(const intro_t *in);
int intro_n_parts(const intro_t *in);
int intro_skipped(const intro_t *in);

/* ------------------------------------------------------------- for the harness */

/* Parse a .vid header out of `buf` into `in` without opening any file. Returns
 * 1 on a header this build will play, 0 otherwise. Exposed because the part
 * table and the frame index are the half of this file that can be silently
 * wrong, and a bad one is a black screen rather than a crash. */
int intro_parse_header(intro_t *in, const unsigned char *buf, unsigned int len);

/* The frame the part clock is asking for, from seconds. Pure, and the one piece
   of arithmetic that decides whether the picture keeps up with the sound. */
unsigned int intro_frame_at(const intro_t *in, double t);

/* Where the loading screen puts one of its four elements, in pixels, given the
   screen. `i` is 0..3 in the engine's own table order. Pure, so the harness can
   hold the layout against the recovered rects. */
void intro_element_rect(int i, int screen_w, int screen_h,
                        float *x, float *y, float *w, float *h);
#define INTRO_N_ELEMENTS 4

#endif /* INTRO_H */
