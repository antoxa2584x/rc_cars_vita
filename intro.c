/*
 * intro.c -- see intro.h.
 *
 * The state machine below is FUN_004a3030's, state for state, with two
 * differences and no others:
 *
 *   the engine's states 6..0xc are seven frames spent letting the DirectShow
 *   video window go away before the loading screen appears. There is no such
 *   window here, so STOP goes straight to DONE.
 *
 *   the engine seeks its player to a REFERENCE_TIME. The .vid carries the part
 *   table in FRAMES (pack_vid.py did that conversion once, offline, against the
 *   real frame rate), so a part start here is an index into the frame table and
 *   an IDR is guaranteed to be sitting on it.
 */

#include "intro.h"

#include "avc.h"
#include "audio.h"
#include "menu.h"           /* the SCE_CTRL_ bits, on both targets */
#include "mix.h"            /* MIX_RATE */
#include "rlog.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The frame index is read straight off disk and used in place, so the file's
   little-endian words have to be the host's. Every target this port has is
   little-endian; the check is here so that stops being an assumption. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#error "intro.c reads the .vid frame index in place -- this build needs a swap"
#endif

/* -------------------------------------------------------- the loading screen */

/* The recovered table at 0x56b8a0: name, then the normalised rect. The order is
   the engine's own, and it is also the DRAW order -- Desktop first and opaque,
   the three logos over it. */
static const struct { float x0, y0, x1, y1; } LS_RECT[INTRO_N_ELEMENTS] = {
    { 0.00000f, 0.00000f, 1.00000f, 1.00000f },      /* Desktop */
    { 0.16000f, 0.04000f, 0.80000f, 0.89333f },      /* logoRC  */
    { 0.03125f, 0.01167f, 0.19125f, 0.22500f },      /* logo1C  */
    { 0.80875f, 0.01167f, 0.96875f, 0.22500f },      /* logoCR  */
};

/* The band the engine draws its one line of text in: {0, h - 90, w, h} at the
   480 it ran at. Scaled with the screen rather than left at 90 px, because it
   is a fraction of the picture and not a font size. */
#define LS_TEXT_BAND 90.0f
#define LS_REF_H     480.0f

/* The authored aspect. Every rect above is square at 640x480, so the logos are
   placed in a 4:3 box fitted to the screen height -- see intro.h. */
#define LS_ASPECT (4.0f / 3.0f)

void intro_element_rect(int i, int screen_w, int screen_h,
                        float *x, float *y, float *w, float *h)
{
    float bx, by, bw, bh;

    if (i < 0 || i >= INTRO_N_ELEMENTS) {
        *x = *y = *w = *h = 0.f;
        return;
    }
    if (i == 0) {
        /* The desktop covers the SCREEN. It is a tiling graffiti field with no
           feature that a stretch distorts, and mainmenu.c already draws the
           same texture over the same whole window. */
        bx = 0.f; by = 0.f;
        bw = (float)screen_w; bh = (float)screen_h;
    } else {
        bh = (float)screen_h;
        bw = bh * LS_ASPECT;
        if (bw > (float)screen_w) { bw = (float)screen_w; bh = bw / LS_ASPECT; }
        bx = ((float)screen_w - bw) * 0.5f;
        by = ((float)screen_h - bh) * 0.5f;
    }
    *x = bx + LS_RECT[i].x0 * bw;
    *y = by + LS_RECT[i].y0 * bh;
    *w = (LS_RECT[i].x1 - LS_RECT[i].x0) * bw;
    *h = (LS_RECT[i].y1 - LS_RECT[i].y0) * bh;
}

void intro_load_screen(const intro_tex *t, int screen_w, int screen_h,
                       const char *caption, float progress)
{
    unsigned int tex[INTRO_N_ELEMENTS];
    float band, y;
    int i;

    tex[0] = t ? t->desktop : 0u;
    tex[1] = t ? t->logo_rc : 0u;
    tex[2] = t ? t->logo_1c : 0u;
    tex[3] = t ? t->logo_cr : 0u;

    ui_begin(screen_w, screen_h);
    /* THE GROUND FIRST, in the desktop's own colour, and not conditional on the
       texture: this is a whole screen rather than an overlay, and the caller's
       clear is the sky blue the world is drawn against. A missing Desktop then
       reads as the same page in flat paint instead of as a hole -- the rule
       hud.c and mainmenu.c follow for every binding they take. */
    ui_rect(0.f, 0.f, (float)screen_w, (float)screen_h,
            0.93f, 0.65f, 0.09f, 1.f);
    for (i = 0; i < INTRO_N_ELEMENTS; i++) {
        float x, yy, w, h;
        if (!tex[i])
            continue;
        intro_element_rect(i, screen_w, screen_h, &x, &yy, &w, &h);
        ui_image(x, yy, w, h, tex[i], 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f);
    }

    band = LS_TEXT_BAND * (float)screen_h / LS_REF_H;
    y = (float)screen_h - band;
    if (caption && *caption) {
        const float sc = 2.0f;
        float tw = ui_text_w(sc, caption);
        ui_text(((float)screen_w - tw) * 0.5f, y + band * 0.20f, sc,
                1.f, 1.f, 1.f, 1.f, caption);
    }
    if (progress >= 0.f) {
        /* One bar, in the same band, at the desktop's own width. The engine
           draws no bar -- it has one line of text and nothing else down here --
           but the loads this screen covers are 30 to 40 MB apiece and a screen
           that never changes is indistinguishable from one that has hung. */
        const float bw = (float)screen_w * 0.5f;
        const float bx = ((float)screen_w - bw) * 0.5f;
        const float by = y + band * 0.70f;
        const float bh = band * 0.12f;
        float p = progress > 1.f ? 1.f : progress;
        ui_rect(bx - 1.f, by - 1.f, bw + 2.f, bh + 2.f, 0.f, 0.f, 0.f, 0.5f);
        ui_rect(bx, by, bw * p, bh, 1.f, 1.f, 1.f, 0.9f);
    }
    ui_end();
}

/* ----------------------------------------------------------- the launch movie */

#define VID_MAGIC   0x31444956u     /* "VID1", little-endian */
#define VID_HDR     48u
#define VID_IDR     0x80000000u

/* The read-ahead over the access units. The AUs are laid down in frame order and
   a part only ever plays forwards, so this is a sliding window and not a cache:
   one read every ~35 frames instead of a seek and a read every frame. Sized
   generously against the largest AU rather than tuned -- it is freed the moment
   the intro ends. */
#define WIN_BYTES   (512u * 1024u)

/* How many access units one tick may feed. A tick that has fallen behind
   catches up gradually rather than decoding a second of video inside one frame
   and falling further behind for it. */
#define FEED_PER_TICK 4

/* Behind by more than this and the catch-up above will never close the gap, so
   the picture jumps to the last IDR at or before where it should be. pack_vid.py
   puts one every 50 frames, so the jump lands within two seconds. */
#define RESYNC_FRAMES 25

static unsigned int rd32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

int intro_parse_header(intro_t *in, const unsigned char *buf, unsigned int len)
{
    unsigned int i;

    if (len < VID_HDR || rd32(buf) != VID_MAGIC)
        return 0;
    if (rd32(buf + 4) != 1u)
        return 0;
    in->coded_w  = rd32(buf + 8);
    in->coded_h  = rd32(buf + 12);
    in->fps_n    = rd32(buf + 16);
    in->fps_d    = rd32(buf + 20);
    in->n_frames = rd32(buf + 24);
    in->n_parts  = rd32(buf + 28);
    in->max_au   = rd32(buf + 32);
    in->data_off = rd32(buf + 36);

    if (!in->coded_w || !in->coded_h || !in->fps_n || !in->fps_d)
        return 0;
    if (!in->n_frames || !in->n_parts || !in->max_au)
        return 0;
    if (in->n_parts > INTRO_MAX_PARTS)
        return 0;
    if (in->data_off != VID_HDR + 8u * in->n_parts + 8u * in->n_frames)
        return 0;
    if (len < VID_HDR + 8u * in->n_parts)
        return 0;

    for (i = 0; i < in->n_parts; i++) {
        in->part[i].first = rd32(buf + VID_HDR + 8u * i);
        in->part[i].count = rd32(buf + VID_HDR + 8u * i + 4u);
        /* A part that runs off the end of the frame table would read the file's
           own index out of bounds, which is a black screen at best. */
        if (!in->part[i].count
            || in->part[i].first >= in->n_frames
            || in->part[i].first + in->part[i].count > in->n_frames)
            return 0;
    }
    return 1;
}

unsigned int intro_frame_at(const intro_t *in, double t)
{
    double f;
    if (t <= 0.0 || !in->fps_d)
        return 0u;
    f = t * (double)in->fps_n / (double)in->fps_d;
    if (f < 0.0)
        return 0u;
    if (f > (double)in->n_frames)
        return in->n_frames;
    return (unsigned int)f;
}

static double part_seconds(const intro_t *in, int p)
{
    if (p < 0 || p >= (int)in->n_parts || !in->fps_n)
        return 0.0;
    return (double)in->part[p].count * (double)in->fps_d / (double)in->fps_n;
}

/* -> the access unit for absolute frame `f`, or NULL. Refills the window when
   the unit is not wholly inside it. */
static const unsigned char *au_at(intro_t *in, unsigned int f,
                                  unsigned int *size, int *is_idr)
{
    unsigned int off, sz;
    FILE *fp = (FILE *)in->fp;

    if (!fp || !in->ftab || f >= in->n_frames)
        return NULL;
    off = in->ftab[f * 2u];
    sz  = in->ftab[f * 2u + 1u];
    if (is_idr) *is_idr = (sz & VID_IDR) != 0u;
    sz &= ~VID_IDR;
    if (!sz || sz > in->win_cap)
        return NULL;
    *size = sz;

    if (!(off >= in->win_off && off + sz <= in->win_off + in->win_len)) {
        size_t got;
        if (fseek(fp, (long)(in->data_off + off), SEEK_SET) != 0)
            return NULL;
        got = fread(in->win, 1, (size_t)in->win_cap, fp);
        in->win_off = off;
        in->win_len = (unsigned int)got;
        if (got < (size_t)sz)
            return NULL;
    }
    return in->win + (off - in->win_off);
}

/* Feed one frame of the current part. -> 1 fed, 0 nothing left, <0 on error. */
static int feed_one(intro_t *in)
{
    const unsigned char *au;
    unsigned int size = 0, abs_f;
    int idr = 0;

    if (in->fed >= in->part[in->cur].count)
        return 0;
    abs_f = in->part[in->cur].first + in->fed;
    au = au_at(in, abs_f, &size, &idr);
    if (!au)
        return -1;
    in->fed++;
    if (!in->decoder)
        return 1;                   /* no decoder: the clock still runs */
    if (avc_decode(au, (int)size) < 0)
        return -1;
    return 1;
}

static void audio_stop_part(intro_t *in)
{
    if (in->have_audio) {
        audio_music_stop();
        in->have_audio = 0;
    }
}

int intro_open(intro_t *in, const char *vid_path, const char *audio_dir)
{
    unsigned char hdr[VID_HDR + 8u * INTRO_MAX_PARTS];
    FILE *fp;
    size_t want;

    memset(in, 0, sizeof *in);
    in->state = INTRO_ST_DONE;
    in->cur = -1;

    if (!vid_path)
        return 0;
    fp = fopen(vid_path, "rb");
    if (!fp) {
        rlog("[rccars] intro: no %s -- straight to the loading screen\n",
             vid_path);
        return 0;
    }
    if (fread(hdr, 1, sizeof hdr, fp) < VID_HDR
        || !intro_parse_header(in, hdr, (unsigned int)sizeof hdr)) {
        rlog("[rccars] intro: %s is not a .vid this build reads\n", vid_path);
        fclose(fp);
        memset(in, 0, sizeof *in);
        in->state = INTRO_ST_DONE;
        return 0;
    }

    /* The frame index, whole: 8 bytes a frame is 21 KB over the shipped movie,
       and holding it means a part start costs one seek rather than a walk. */
    want = (size_t)in->n_frames * 8u;
    in->ftab = (unsigned int *)malloc(want);
    in->win_cap = WIN_BYTES > in->max_au ? WIN_BYTES : in->max_au + 4096u;
    in->win = (unsigned char *)malloc(in->win_cap);
    if (!in->ftab || !in->win) {
        rlog("[rccars] intro: no room for a %u frame index\n", in->n_frames);
        free(in->ftab); free(in->win);
        fclose(fp);
        memset(in, 0, sizeof *in);
        in->state = INTRO_ST_DONE;
        return 0;
    }
    if (fseek(fp, (long)(VID_HDR + 8u * in->n_parts), SEEK_SET) != 0
        || fread(in->ftab, 1, want, fp) != want) {
        rlog("[rccars] intro: %s is short of its own frame index\n", vid_path);
        free(in->ftab); free(in->win);
        fclose(fp);
        memset(in, 0, sizeof *in);
        in->state = INTRO_ST_DONE;
        return 0;
    }
    in->win_off = 0;
    in->win_len = 0;

    in->fp = fp;
    in->open = 1;
    if (audio_dir)
        snprintf(in->dir, sizeof in->dir, "%s", audio_dir);

    in->decoder = (avc_open((int)in->coded_w, (int)in->coded_h,
                            (int)in->max_au) == 0);
    if (!in->decoder)
        rlog("[rccars] intro: no decoder -- the parts will run silent-black\n");

    in->cur = 0;
    in->state = INTRO_ST_INIT;
    rlog("[rccars] intro: %s  %ux%u  %u/%u fps  %u frames  %u parts  "
         "max AU %u B  decoder %s\n",
         vid_path, in->coded_w, in->coded_h, in->fps_n, in->fps_d,
         in->n_frames, in->n_parts, in->max_au, in->decoder ? "up" : "OFF");
    return 1;
}

void intro_close(intro_t *in)
{
    if (!in)
        return;
    audio_stop_part(in);
    if (in->decoder)
        avc_close();
    if (in->fp)
        fclose((FILE *)in->fp);
    free(in->ftab);
    free(in->win);
    memset(in, 0, sizeof *in);
    in->state = INTRO_ST_DONE;
    in->cur = -1;
}

int intro_step(intro_t *in, unsigned int buttons, unsigned int prev,
               const touch_state *tc, float dt)
{
    const unsigned int SKIP_BITS = SCE_CTRL_CROSS | SCE_CTRL_CIRCLE;
    unsigned int edge = buttons & ~prev;
    int skip, skip_all;

    if (!in || in->state == INTRO_ST_DONE)
        return INTRO_DONE;

    /* ESC, in the engine, and only while a part is starting or playing --
       FUN_004a2f10 checks the state as well as the key. START is this port's
       own addition: the retail sequence is skipped with three taps of one key
       and three taps of START is not obviously the same gesture on a pad. */
    skip_all = (edge & SCE_CTRL_START) != 0u;
    skip = skip_all || (edge & SKIP_BITS) != 0u
           || (tc && tc->pressed);

    switch (in->state) {
    case INTRO_ST_INIT:
        /* The engine's 0 -> 1: one frame doing nothing, which is where it read
           AutoRunIntro. Here the asset's presence is that switch and it has
           already been answered, so this state only exists to keep the numbers
           readable against FUN_004a3030. */
        in->state = INTRO_ST_START;
        return INTRO_PLAYING;

    case INTRO_ST_START:
        if (in->cur < 0 || in->cur >= (int)in->n_parts) {
            in->state = INTRO_ST_STOP;
            return INTRO_PLAYING;
        }
        in->fed = 0;
        in->clock = 0.0;
        in->win_len = 0;            /* the window is behind us now */
        audio_stop_part(in);
        if (in->dir[0]) {
            char p[256];
            snprintf(p, sizeof p, "%s/intro_%d.mp3", in->dir, in->cur + 1);
            in->have_audio = audio_music_file(p);
        }
        /* The clock is read as a DIFFERENCE from here, so it does not matter
           that the mixer's counter has been running since audio_init. */
        in->audio_base = audio_music_frames();
        in->audio_last = in->audio_base;
        in->audio_stall = 0.f;
        in->wall = 0.f;
        in->state = INTRO_ST_PLAY;
        rlog("[rccars] intro: part %d/%u  frames %u..%u  %.2f s  audio %s\n",
             in->cur + 1, in->n_parts, in->part[in->cur].first,
             in->part[in->cur].first + in->part[in->cur].count - 1u,
             part_seconds(in, in->cur), in->have_audio ? "on" : "off");
        return INTRO_PLAYING;

    case INTRO_ST_PLAY: {
        unsigned int want;
        int n;

        if (skip) {
            /* NAMED IN THE LOG, because "the intro skipped itself" is otherwise
               indistinguishable from "the intro failed" -- and on an emulator
               the panel and the pad both report things a Vita would not. */
            rlog("[rccars] intro: part %d skipped at %.2f s by %s%s%s%s\n",
                 in->cur + 1, in->clock,
                 (edge & SCE_CTRL_START) ? "START " : "",
                 (edge & SCE_CTRL_CROSS) ? "CROSS " : "",
                 (edge & SCE_CTRL_CIRCLE) ? "CIRCLE " : "",
                 (tc && tc->pressed) ? "touch" : "");
            in->skipped++;
            in->state = skip_all ? INTRO_ST_STOP : INTRO_ST_NEXT;
            if (skip_all)
                in->cur = (int)in->n_parts;
            return INTRO_PLAYING;
        }

        /* THE AUDIO IS THE CLOCK where there is audio, because it is the one
           clock the player can hear: a frame late is invisible and a frame of
           music missing is not. audio_music_frames counts what the mixer has
           actually rendered, so an underrun stalls the picture with the sound
           rather than running past it. dt is the fallback. */
        in->wall += dt;
        if (in->have_audio && audio_ok()) {
            unsigned int now = audio_music_frames();
            in->clock = (double)(now - in->audio_base) / (double)MIX_RATE;
            /* AND A WATCHDOG OVER IT. A clock that has stopped is indis-
               tinguishable from one that is early, and this one gates the only
               way out of the boot: if the mixer stops feeding -- a card stall
               that outlasts the ring, a decode that gave up on a truncated mp3
               -- the launch would sit on one frame of a logo forever. Two
               seconds of that and the part finishes on `dt' instead. */
            if (now != in->audio_last) {
                in->audio_last = now;
                in->audio_stall = 0.f;
            } else if ((in->audio_stall += dt) > 2.f) {
                rlog("[rccars] intro: part %d's audio clock stalled at "
                     "%.2f s -- running on dt\n", in->cur + 1, in->clock);
                in->have_audio = 0;
            }
        } else {
            in->clock += (double)dt;
        }

        want = intro_frame_at(in, in->clock);
        if (want > in->part[in->cur].count)
            want = in->part[in->cur].count;

        if (in->decoder && in->fed + RESYNC_FRAMES < want) {
            /* Too far behind for FEED_PER_TICK to close: jump to the last IDR
               at or before `want` rather than decode a second of video in one
               tick. Nothing is dropped that could have been shown. */
            unsigned int k = want ? want - 1u : 0u;
            while (k > 0u
                   && !(in->ftab[(in->part[in->cur].first + k) * 2u + 1u]
                        & VID_IDR))
                k--;
            rlog("[rccars] intro: %u frames behind -- resync to %u\n",
                 want - in->fed, k);
            in->fed = k;
        }

        for (n = 0; n < FEED_PER_TICK && in->fed < want; n++) {
            int r = feed_one(in);
            if (r < 0) {
                rlog("[rccars] intro: part %d failed at frame %u\n",
                     in->cur + 1, in->fed);
                in->state = INTRO_ST_NEXT;
                return INTRO_PLAYING;
            }
            if (r == 0)
                break;
        }

        /* The part ends when its frames are spent AND its time is up: with the
           audio as the clock the frames run out first (there is a whole part of
           audio behind the last picture) and cutting on the picture would clip
           the sound. */
        if (in->fed >= in->part[in->cur].count
            && in->clock >= part_seconds(in, in->cur))
            in->state = INTRO_ST_NEXT;
        return INTRO_PLAYING;
    }

    case INTRO_ST_NEXT:
        /* WHAT THE PART ACTUALLY COST, in both clocks. They should agree: the
           audio clock is what the picture is paced by and `wall' is real time,
           so a part whose sound ran at 1.4x -- which is what an emulator with an
           unthrottled audio port would do -- shows up here and nowhere else. */
        if (in->cur >= 0 && in->cur < (int)in->n_parts)
            rlog("[rccars] intro: part %d ended -- %.2f s of clock over "
                 "%.2f s of real time, %u/%u frames\n", in->cur + 1,
                 in->clock, in->wall, in->fed, in->part[in->cur].count);
        /* FUN_004a3030 case 4, arithmetic and all: cur++, and the next state is
           2 while there are parts left and 5 when there are not. */
        in->cur++;
        in->state = (in->cur < (int)in->n_parts) ? INTRO_ST_START
                                                 : INTRO_ST_STOP;
        return INTRO_PLAYING;

    case INTRO_ST_STOP:
        /* The engine's case 5 is the player's teardown, then seven idle frames.
           Here the decoder and the file go now, so the 6 MB of physically
           contiguous memory is back before main() loads its first 36 MB scene. */
        audio_stop_part(in);
        if (in->decoder) {
            avc_close();
            in->decoder = 0;
        }
        if (in->fp) { fclose((FILE *)in->fp); in->fp = NULL; }
        free(in->ftab); in->ftab = NULL;
        free(in->win);  in->win = NULL;
        in->open = 0;
        in->state = INTRO_ST_DONE;
        rlog("[rccars] intro: done, %d part%s skipped\n",
             in->skipped, in->skipped == 1 ? "" : "s");
        return INTRO_DONE;

    default:
        in->state = INTRO_ST_DONE;
        return INTRO_DONE;
    }
}

void intro_draw(const intro_t *in, int screen_w, int screen_h)
{
    unsigned int tex = avc_tex();
    float w, h, x, y;

    ui_begin(screen_w, screen_h);
    /* Black under it, for the frames before the first picture. Drawn rather than
       cleared so this file needs no GL of its own -- the caller owns the frame,
       and its clear colour is the world's sky. */
    ui_rect(0.f, 0.f, (float)screen_w, (float)screen_h, 0.f, 0.f, 0.f, 1.f);
    if (in && tex && in->coded_h) {
        /* FULL SCREEN. The movie is 4:3 and the Vita's screen is 960x544, so
           the two cannot both be satisfied and this is a choice rather than a
           derivation: the picture is STRETCHED to fill, which is 20% wide and
           loses nothing, instead of letterboxed (black bars down both sides,
           which is what it did first and what was asked to go) or scaled to fill
           and cropped (no distortion, but 24% of the height gone -- acceptable
           for two centred logos and not for 87 seconds of film that uses the
           whole frame).
         *
           The original filled a 4:3 monitor, so filling the screen is the more
           faithful of the two things that cannot both be true here. */
        x = 0.f;
        y = 0.f;
        w = (float)screen_w;
        h = (float)screen_h;
        ui_image(x, y, w, h, tex, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f);
    }
    ui_end();
}

int intro_part_no(const intro_t *in)
{
    if (!in || in->cur < 0 || in->cur >= (int)in->n_parts)
        return 0;
    return in->cur + 1;
}

int intro_n_parts(const intro_t *in) { return in ? (int)in->n_parts : 0; }
int intro_skipped(const intro_t *in) { return in ? in->skipped : 0; }
