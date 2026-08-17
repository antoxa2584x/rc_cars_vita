/*
 * countdown.c -- see countdown.h. The artwork, the atlas split, the sizes, the
 * per-message lives and the scale ramp are all the game's own (msg_321_s_f,
 * message slots 5, 6, 7 and 9); only the state machine is the port's, because
 * the original's countdown arrives as one network message per second from a game
 * manager this port does not have.
 */

#include "countdown.h"
#include "ui.h"

#include <string.h>

/* The five cells, verbatim from the slot tables. Order is this file's own,
   ascending through the countdown; `slot` carries the engine's index so the
   numbers can be checked back against 0x56d278 / 0x56d328 without decoding the
   order first. */
const cd_cell CD_CELLS[CD_N_CELLS] = {
    /*  w         h         u0      v0     u1      v1     slot */
    { 0.16000f, 0.213333f, 0.000f, 0.000f, 0.250f, 0.500f, 5 },  /* 3      */
    { 0.16000f, 0.213333f, 0.000f, 0.500f, 0.250f, 1.000f, 6 },  /* 2      */
    { 0.09250f, 0.213333f, 0.250f, 0.500f, 0.395f, 1.000f, 7 },  /* 1      */
    { 0.48000f, 0.213333f, 0.250f, 0.000f, 1.000f, 0.500f, 9 },  /* !!Go!! */
    { 0.38750f, 0.213333f, 0.395f, 0.500f, 1.000f, 1.000f, 8 }   /* FiNiSH */
};

/* The words the font fallback prints, one per cell above. */
static const char *const CD_WORDS[CD_N_CELLS] = { "3", "2", "1", "GO!", "FINISH" };

/* When each message goes up, and how long it lives. Derived from the two
   recovered constants rather than tabled, so the table cannot disagree with
   them. */
#define CD_GO_AT (CD_STEP_TIME * 3.f)
#define CD_END   (CD_GO_AT + CD_GO_TIME)

void countdown_init(countdown_t *c, unsigned int tex)
{
    memset(c, 0, sizeof(*c));
    c->tex = tex;
    c->t = -1.f;
}

void countdown_start(countdown_t *c)
{
    if (!c)
        return;
    /* NOT a memset: the texture binding outlives a restart. It is bound once
       from the shared props scene, which is loaded before the first track and
       never reloaded. Same reasoning as hud_reset. */
    c->t = 0.f;
    c->running = 1;
    c->go = 0;
}

void countdown_stop(countdown_t *c)
{
    if (!c)
        return;
    c->t = -1.f;
    c->running = 0;
    /* No `go` edge. Stopping is not starting the race. */
    c->go = 0;
}

void countdown_step(countdown_t *c, float dt)
{
    float was;

    if (!c)
        return;
    c->go = 0;                          /* written every call: it is an EDGE */
    if (!c->running)
        return;

    was = c->t;
    c->t += dt;

    /* The one call that crosses the line. `was < CD_GO_AT` and not
       `c->t >= CD_GO_AT` alone, or every frame of the GO! banner would be a
       start. A dt of 0 (the menu is open) crosses nothing. */
    if (was < CD_GO_AT && c->t >= CD_GO_AT)
        c->go = 1;

    if (c->t >= CD_END) {
        c->t = -1.f;
        c->running = 0;
    }
}

int countdown_holding(const countdown_t *c)
{
    return c && c->running && c->t >= 0.f && c->t < CD_GO_AT;
}

int countdown_active(const countdown_t *c)
{
    return countdown_cell(c) >= 0;
}

int countdown_cell(const countdown_t *c)
{
    if (!c || !c->running || c->t < 0.f || c->t >= CD_END)
        return -1;
    if (c->t < CD_STEP_TIME)       return CD_CELL_3;
    if (c->t < CD_STEP_TIME * 2.f) return CD_CELL_2;
    if (c->t < CD_GO_AT)           return CD_CELL_1;
    return CD_CELL_GO;
}

/* Seconds this message has been up, and how long it lives -- the two inputs the
   recovered ramp takes. */
static void cd_phase(const countdown_t *c, float *elapsed, float *life)
{
    int cell = countdown_cell(c);
    if (cell == CD_CELL_GO) {
        *elapsed = c->t - CD_GO_AT;
        *life = CD_GO_TIME;
    } else {
        /* CD_CELL_3/2/1 are 0/1/2 in that order, so the cell index IS the step
           number and its message went up at index * CD_STEP_TIME. Written this
           way rather than as a table so there is nothing to keep in step; the
           ordering is asserted in ui_test. */
        *elapsed = c->t - (float)cell * CD_STEP_TIME;
        *life = CD_STEP_TIME;
    }
}

float countdown_scale(const countdown_t *c)
{
    float elapsed, life, d;

    if (countdown_cell(c) < 0)
        return 0.f;
    cd_phase(c, &elapsed, &life);

    /* 0x4b028d: d = min(life, 0.8); k = elapsed >= d ? 1.0 : elapsed / d.
       Growing, not shrinking, and it holds at the top rather than coming back
       down -- there is no fade in this layer at all. */
    d = life < CD_GROW_TIME ? life : CD_GROW_TIME;
    if (!(d > 0.f))
        return 1.f;
    if (elapsed >= d)
        return 1.f;
    if (elapsed <= 0.f)
        return 0.f;
    return elapsed / d;
}

void countdown_draw(const countdown_t *c, int screen_w, int screen_h)
{
    const float sw = (float)screen_w, sh = (float)screen_h;
    const cd_cell *cell;
    float k, w, hh, cx, cy;
    int idx = countdown_cell(c);

    if (idx < 0)
        return;
    k = countdown_scale(c);
    /* FUN_004b12b0 returns 0 below 1e-6 and the drawer then skips the quad
       entirely, so the first frame of a message really does draw nothing. */
    if (k < CD_MIN_SCALE)
        return;

    cell = &CD_CELLS[idx];

    /* HEIGHT is the recovered fraction of the screen; WIDTH comes from the
       recovered pair and the 4:3 frame they were authored against. See
       CD_REF_ASPECT. The scale is about the rect's own centre, which is why this
       is expressed as a centre and a size. */
    hh = cell->h * sh * k;
    w  = hh * (cell->w / cell->h) * CD_REF_ASPECT;

    cx = sw * 0.5f;                      /* x0 = (1 - w)/2 */
    {
        const float base = (CD_BAND == 0) ? 1.0f
                         : (CD_BAND == 1) ? 0.5f : 1.5f;
        cy = sh * base * 0.5f;           /* y0 = (base - h)/2, yoff all zero */
    }

    ui_begin(screen_w, screen_h);

    if (c->tex) {
        /* The game's own cell, white so the ARGB8888 art arrives unmodulated --
           and at a flat alpha of 1, because this layer never fades. */
        ui_image(cx - w * 0.5f, cy - hh * 0.5f, w, hh, c->tex,
                 cell->u0, cell->v0, cell->u1, cell->v1,
                 1.f, 1.f, 1.f, 1.f);
    } else {
        /* msg_321_s_f was not packed. The compiled-in font instead, so the start
           light degrades rather than disappearing -- font.h is in the binary and
           cannot fail to load. The same scale ramp, so the fallback still grows
           and a mutant in it still shows. */
        const char *word = CD_WORDS[idx];
        const float scale = CD_TEXT_SCALE * k;
        const float x = cx - ui_text_w(scale, word) * 0.5f;
        const float y = cy - ui_text_h(scale) * 0.5f;
        /* A shadow, offset with the size -- the tracks are sand, asphalt and pale
           stone, and bright text alone on any of them is a smear. */
        const float shd = ui_text_h(scale) * 0.06f;
        ui_text(x + shd, y + shd, scale, 0.f, 0.f, 0.f, 0.7f, word);
        ui_text(x, y, scale, 1.f, 0.25f, 0.20f, 1.f, word);
    }

    ui_end();
}
