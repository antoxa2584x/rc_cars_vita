/* results.c -- see results.h for the dialog, the columns and where each number
 * comes from. Every rectangle below is dlgFINISH.ini's, through dlg_data.h. */
#include "results.h"
#include "menu.h"           /* the SCE_CTRL_* bits, host-safe */
#include "touch.h"
#include "sfont.h"
#include "ui.h"
#include "hud_data.h"       /* HUD_REF_W/H */
#include "dlg_data.h"

#include <stdio.h>
#include <string.h>

/* The same two maps mainmenu.c uses, and for the same reason: the dialog is
   authored at 800x600 and the Vita is 960x544. Positions follow the frame,
   sizes that must not distort use the uniform scale. mainmenu.h states the rule
   at length; this file is the other half of the same interface. */
typedef struct { float w, h, sx, sy, us; } rframe;

static rframe rf(int screen_w, int screen_h)
{
    rframe f;
    f.w = (float)screen_w;
    f.h = (float)screen_h;
    f.sx = f.w / HUD_REF_W;
    f.sy = f.h / HUD_REF_H;
    f.us = f.sy;
    return f;
}
static float rx(const rframe *f, float x) { return x * f->sx; }
static float ry(const rframe *f, float y) { return y * f->sy; }

/* THE COLUMN WIDTHS, as dlgFINISH.ini gives them: fractions of the table's own
 * SX, laid end to end from its X0.
 *
 * THERE ARE SEVEN COLUMNS AND SIX HEADINGS. `tableWidth0' is 8% -- 48 px of the
 * 597 -- which is not a column of text at all: `tableItemHeight' is 15% of 315,
 * i.e. 47 px, so column 0 is a SQUARE and it is the portrait's. The six headings
 * then take widths 1..5 and the last one runs to the table's right edge on the
 * 20% the file does not name, which is why the widths sum to 0.80 and not 1.00.
 *
 * The first build read them off by one -- Player got column 0's 48 px and every
 * name ran into the Place column -- and `tableMordaShift' was taken for a shift
 * of the whole table rather than the portrait's own inset in column 0. Both
 * showed up the moment there was a picture. */
static const float RES_COLW[6] = {
    DLG_FINISH_tableWidth0, DLG_FINISH_tableWidth1, DLG_FINISH_tableWidth2,
    DLG_FINISH_tableWidth3, DLG_FINISH_tableWidth4, DLG_FINISH_tableWidth5
};

static const char *const RES_HEAD[6] = {
    "Player", "Place", "Time", "Gap", "Best lap", "Av. speed"
};

/* Heading i's left edge in design pixels, i = 0..5. Column 0 is the portrait's,
   so heading 0 begins one width in. */
static float res_col_x(int i)
{
    float x = DLG_FINISH_tableFinishX0;
    int k;
    for (k = 0; k <= i && k < 6; k++)
        x += DLG_FINISH_tableFinishSX * RES_COLW[k];
    return x;
}

static float res_row_y(int i)
{
    return DLG_FINISH_tableFinishY0
           + DLG_FINISH_tableFinishSY
             * (DLG_FINISH_tableHeaderHeight
                + DLG_FINISH_tableItemHeight * (float)i);
}

static void res_btn_rect(const rframe *f, int btn,
                         float *x, float *y, float *w, float *h)
{
    /* buttonRaceAgain has an X0 and a Y0 of its own and NO SY: the shipped file
       declares `buttonRaceAgainSX 34', which is the height, and never names a
       width. Both buttons are the same size in the original's screenshot --
       150 x 33, which is buttonAbort's -- so that is what both get, and the 34
       is left where it is rather than pressed into service as a width. */
    const float bx = (btn == RES_BTN_QUIT) ? DLG_FINISH_buttonAbortX0
                                           : DLG_FINISH_buttonRaceAgainX0;
    const float by = (btn == RES_BTN_QUIT) ? DLG_FINISH_buttonAbortY0
                                           : DLG_FINISH_buttonRaceAgainY0;
    *x = rx(f, bx);
    *y = ry(f, by);
    *w = rx(f, bx + DLG_FINISH_buttonAbortSX) - *x;
    *h = ry(f, by + DLG_FINISH_buttonAbortSY) - *y;
}

int results_btn_at(const results_t *r, int screen_w, int screen_h,
                   float x, float y)
{
    const rframe f = rf(screen_w, screen_h);
    int i;
    (void)r;
    for (i = 0; i < RES_N_BTN; i++) {
        float bx, by, bw, bh;
        res_btn_rect(&f, i, &bx, &by, &bw, &bh);
        if (touch_in(x, y, bx, by, bw, bh))
            return i;
    }
    return -1;
}

void results_init(results_t *r, const results_tex *tex)
{
    if (!r)
        return;
    memset(r, 0, sizeof(*r));
    if (tex)
        r->tex = *tex;
    r->armed = -1;
    /* RACE AGAIN, not Quit. The player who just finished one is far more likely
       to want another, and the destructive-looking button is never the one a
       thumb lands on by reflex. */
    r->focus = RES_BTN_AGAIN;
}

void results_finish(results_t *r)
{
    int i, j;
    if (!r || r->n <= 0)
        return;
    /* Insertion sort: finishers first by time, then the rest by how far behind
       they were. Six rows at most, once a race. */
    for (i = 1; i < r->n; i++) {
        results_row v = r->row[i];
        for (j = i - 1; j >= 0; j--) {
            const results_row *a = &r->row[j];
            int worse;
            if (a->finished != v.finished)
                worse = !a->finished;         /* a finisher always beats one that did not */
            else if (v.finished)
                worse = a->time > v.time;
            else
                worse = a->behind_m > v.behind_m;
            if (!worse)
                break;
            r->row[j + 1] = r->row[j];
        }
        r->row[j + 1] = v;
    }
    for (i = 0; i < r->n; i++) {
        r->row[i].place = i + 1;
        /* The gap is against the WINNER, which after the sort is row 0 -- and
           only where both crossed the line. A racer still out there has its
           distance instead, which results.h says is the honest answer. */
        r->row[i].gap = (r->row[i].finished && r->row[0].finished)
                        ? r->row[i].time - r->row[0].time : 0.f;
    }
}

void results_step(results_t *r, unsigned int buttons, const void *tp_,
                  int screen_w, int screen_h, float dt)
{
    const touch_state *tp = (const touch_state *)tp_;
    static unsigned int prev;
    unsigned int down;

    if (!r)
        return;
    r->action = RES_ACT_NONE;
    r->cue = 0;
    r->t += dt;

    down = buttons & ~prev;
    prev = buttons;

    if (down & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_UP | SCE_CTRL_DOWN)) {
        r->focus = (r->focus + 1) % RES_N_BTN;
        r->cue = 1;
    }
    if (down & (SCE_CTRL_CROSS | SCE_CTRL_START)) {
        r->action = (r->focus == RES_BTN_QUIT) ? RES_ACT_QUIT : RES_ACT_AGAIN;
        r->cue = 2;
    }
    if (down & SCE_CTRL_CIRCLE) {
        r->action = RES_ACT_QUIT;
        r->cue = 2;
    }

    if (!tp)
        return;
    if (tp->pressed) {
        r->armed = results_btn_at(r, screen_w, screen_h, tp->x, tp->y);
        if (r->armed >= 0 && r->focus != r->armed) {
            r->focus = r->armed;
            r->cue = 1;
        }
    }
    if (tp->released) {
        const int b = results_btn_at(r, screen_w, screen_h, tp->x, tp->y);
        if (b >= 0 && b == r->armed) {
            r->action = (b == RES_BTN_QUIT) ? RES_ACT_QUIT : RES_ACT_AGAIN;
            r->cue = 2;
        }
        r->armed = -1;
    }
}

/* --------------------------------------------------------------------- draw */

/* `m:ss.hh', which is race_ui.c's own format and the exe's at 0x56b388. */
static void res_time(char *out, int n, float t)
{
    int cs, m, sec;
    if (!(t > 0.f)) { snprintf(out, n, "---"); return; }
    cs = (int)(t * 100.f + 0.5f);
    m = cs / 6000; cs -= m * 6000;
    sec = cs / 100; cs -= sec * 100;
    snprintf(out, n, "%d:%02d.%02d", m, sec, cs);
}

static void res_text(const sfont *f, float x, float y, float sc,
                     float a, const char *s)
{
    if (f->tex)
        sf_text_shadowed(f, x, y, sc, 1.f, 1.f, 1.f, a, s);
    else
        ui_text(x, y, sc, 1.f, 1.f, 1.f, a, s);
}

static float res_text_w(const sfont *f, float sc, const char *s)
{
    return f->tex ? sf_w(f, sc, s) : ui_text_w(sc, s);
}

void results_draw(const results_t *r, int screen_w, int screen_h)
{
    const rframe f = rf(screen_w, screen_h);
    const sfont big = sf_big(r->tex.font_big);
    const sfont small_ = sf_small(r->tex.font_small);
    const float sc = f.us * 0.82f;
    const float lh = (small_.tex ? sf_h(&small_, sc) : ui_text_h(sc));
    /* The grow-in: the panel comes up over half a second, which is the same
       shape msg.c's banners use. Nothing about it is recovered, and it is here
       so the screen does not appear between two frames on top of a car that is
       still braking. */
    const float grow = r->t < 0.5f ? r->t / 0.5f : 1.f;
    const float a = grow;
    int i, k;
    char buf[64];

    if (!r)
        return;

    /* The world goes dark behind it, which is what the original's screenshot
       shows -- the race is still there, it is just not the subject any more. */
    ui_rect(0.f, 0.f, f.w, f.h, 0.f, 0.f, 0.f, 0.55f * a);

    /* rectFrame: the dialog's own area. THE ORIGINAL DRAWS NO PLATE OVER IT --
     * its own screenshot shows the race straight through, dimmed, with the
     * table's two rules the only furniture and the cockpit's rounded outline
     * behind (that outline is the HUD's, not this dialog's, which is why
     * rectFrame is 570 tall and the outline is not).
     *
     * So: a band just dark enough under the table to read white text on any
     * track, grown about its own centre so the screen opens rather than
     * appearing between two frames on a car that is still braking. */
    {
        const float x = rx(&f, DLG_FINISH_rectFrameX0);
        const float w = rx(&f, DLG_FINISH_rectFrameX0 + DLG_FINISH_rectFrameSX) - x;
        const float y = ry(&f, DLG_FINISH_tableFinishY0);
        const float hh = ry(&f, DLG_FINISH_tableFinishY0
                                + DLG_FINISH_tableFinishSY) - y;
        const float cy = y + hh * 0.5f;
        const float gh = hh * (0.25f + 0.75f * grow);
        ui_rect(x, cy - gh * 0.5f, w, gh, 0.f, 0.f, 0.f, 0.45f * a);
    }
    if (grow < 1.f)
        return;                    /* the contents arrive with the plate open */

    /* --- the header row -------------------------------------------------- */
    for (i = 0; i < 6; i++)
        res_text(&small_, rx(&f, res_col_x(i)),
                 ry(&f, DLG_FINISH_tableFinishY0), sc, a, RES_HEAD[i]);
    /* the rule under it, at the header's own height */
    ui_rect(rx(&f, DLG_FINISH_tableFinishX0),
            ry(&f, DLG_FINISH_tableFinishY0
                   + DLG_FINISH_tableFinishSY * DLG_FINISH_tableHeaderHeight)
              - f.us * 2.f,
            rx(&f, DLG_FINISH_tableFinishX0 + DLG_FINISH_tableFinishSX)
              - rx(&f, DLG_FINISH_tableFinishX0),
            f.us * 1.5f, 1.f, 1.f, 1.f, 0.55f * a);

    /* and the one under the last row, at the table's own bottom edge */
    ui_rect(rx(&f, DLG_FINISH_tableFinishX0),
            ry(&f, DLG_FINISH_tableFinishY0 + DLG_FINISH_tableFinishSY),
            rx(&f, DLG_FINISH_tableFinishX0 + DLG_FINISH_tableFinishSX)
              - rx(&f, DLG_FINISH_tableFinishX0),
            f.us * 1.5f, 1.f, 1.f, 1.f, 0.55f * a);

    /* --- one row per racer ----------------------------------------------- */
    for (k = 0; k < r->n && k < RES_MAX_ROWS; k++) {
        const results_row *w = &r->row[k];
        const float y = ry(&f, res_row_y(k));
        const float rowh = ry(&f, DLG_FINISH_tableFinishSY
                                  * DLG_FINISH_tableItemHeight);
        const float ty = y + (rowh - lh) * 0.5f;
        /* THE PLAYER'S ROW IS BRIGHTER, which is the only thing in this table
           that is not a number: four rows of white on black and the player has
           to count to find themselves. */
        const float ra = a * (w->is_player ? 1.f : 0.78f);

        /* THE PORTRAIT FILLS COLUMN 0 and stops where the Player column starts.
           Column 0 is 8% of 597 = 47.8 px and a row is 15% of 315 = 47.25, so
           the artists cut it square on purpose and the picture wants no margin
           of this file's invention. `tableMordaShift' (9%) is not used: what it
           shifts is not recoverable from the .ini alone, and guessing it put the
           faces under the names. */
        if (r->tex.face[k]) {
            const float cw = rx(&f, DLG_FINISH_tableFinishX0
                                    + DLG_FINISH_tableFinishSX * RES_COLW[0])
                             - rx(&f, DLG_FINISH_tableFinishX0);
            const float pw = (cw < rowh ? cw : rowh);
            ui_image(rx(&f, DLG_FINISH_tableFinishX0), y + (rowh - pw) * 0.5f,
                     pw, pw, r->tex.face[k],
                     4.f / 128.f, 2.f / 256.f, 124.f / 128.f, 154.f / 256.f,
                     1.f, 1.f, 1.f, ra);
        }

        res_text(&small_, rx(&f, res_col_x(0)), ty, sc, ra, w->name);

        snprintf(buf, sizeof buf, "%d", w->place);
        res_text(&small_, rx(&f, res_col_x(1)), ty, sc, ra, buf);

        res_time(buf, sizeof buf, w->finished ? w->time : 0.f);
        res_text(&small_, rx(&f, res_col_x(2)), ty, sc, ra, buf);

        if (k == 0)
            snprintf(buf, sizeof buf, "---");
        else if (w->finished) {
            char t2[32];
            res_time(t2, sizeof t2, w->gap);
            snprintf(buf, sizeof buf, "+%s", t2);
        } else
            snprintf(buf, sizeof buf, "+%.0f m", w->behind_m);
        res_text(&small_, rx(&f, res_col_x(3)), ty, sc, ra, buf);

        res_time(buf, sizeof buf, w->best_lap);
        res_text(&small_, rx(&f, res_col_x(4)), ty, sc, ra, buf);

        snprintf(buf, sizeof buf, "%.2f km/h", w->av_speed);
        res_text(&small_, rx(&f, res_col_x(5)), ty, sc, ra, buf);
    }

    /* --- buttonAbort and the one the table calls `none' ------------------- */
    for (i = 0; i < RES_N_BTN; i++) {
        float bx, by, bw, bh, v;
        const int lit = (r->focus == i);
        res_btn_rect(&f, i, &bx, &by, &bw, &bh);
        if (i == RES_BTN_QUIT) {
            /* Button_back: orange, focused, disabled at 32 rows each of 128 */
            if (r->tex.back)
                ui_image(bx, by, bw, bh, r->tex.back,
                         0.f, lit ? 32.f / 128.f : 0.f,
                         1.f, lit ? 64.f / 128.f : 32.f / 128.f,
                         1.f, 1.f, 1.f, a);
            else
                ui_rect(bx, by, bw, bh, 0.90f, 0.55f, 0.04f, a);
        } else {
            v = lit ? 32.f / 256.f : 0.f;    /* ButtonsTextures' red pair */
            if (r->tex.buttons)
                ui_image(bx, by, bw, bh, r->tex.buttons,
                         0.f, v, 1.f, v + 32.f / 256.f, 1.f, 1.f, 1.f, a);
            else
                ui_rect(bx, by, bw, bh, 0.72f, 0.09f, 0.11f, a);
        }
        {
            const char *s = (i == RES_BTN_QUIT) ? "Quit" : "Race again";
            const float ls = f.us * 1.02f;
            const float tw = res_text_w(&big, ls, s);
            res_text(&big, bx + (bw - tw) * 0.5f,
                     by + (bh - (big.tex ? sf_h(&big, ls) : ui_text_h(ls)))
                          * 0.5f, ls, a, s);
        }
    }
}
