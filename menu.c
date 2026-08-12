/*
 * menu.c -- see menu.h.
 */

#include "menu.h"
#include "ui.h"
#include "tracks.h"
#include "rb_data.h"
#include "rbcar.h"

#include <stdio.h>
#include <string.h>


static const char *const ROW_LABEL[MENU_ROWS] = {
    "Track", "Car", "Tires", "Resonator", "Booster",
    "Sound volume", "Music volume", "Texture quality", "Texture colours",
    "Restart at race start", "Resume", "Quit"
};

static int wrap(int v, int n)
{
    while (v < 0)  v += n;
    while (v >= n) v -= n;
    return v;
}

void menu_init(menu_t *m, int track, int car)
{
    memset(m, 0, sizeof(*m));
    m->track = wrap(track, N_TRACKS);
    m->car = wrap(car, 3);
    m->req_track = -1;
    m->req_car = -1;
    /* Sound full, music a little under it: the soundtrack is a constant bed and
       the engine is the thing the player is steering by. */
    m->vol_sfx = MENU_VOL_STEPS;
    m->vol_music = 7;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void adjust(menu_t *m, int d)
{
    switch (m->row) {
    case MENU_TRACK:
        m->track = wrap(m->track + d, N_TRACKS);
        m->req_track = m->track;
        break;
    case MENU_CAR:
        m->car = wrap(m->car + d, 3);
        m->req_car = m->car;
        break;
    case MENU_TIRES: m->tires  = wrap(m->tires + d, 4); break;
    case MENU_RESO:  m->reso   = wrap(m->reso + d, 4);  break;
    case MENU_BOOST: m->boost  = wrap(m->boost + d, 4); break;
    /* Volumes CLAMP where every other picker wraps. Wrapping a volume means one
       press past full is silence, which is never what the player meant. */
    case MENU_VOL_SFX:
        m->vol_sfx = clampi(m->vol_sfx + d, 0, MENU_VOL_STEPS);
        break;
    case MENU_VOL_MUSIC:
        m->vol_music = clampi(m->vol_music + d, 0, MENU_VOL_STEPS);
        break;
    /* Quality CLAMPS rather than wraps, for the same reason the volumes do: one
       press past sharpest should not be blurriest, and each step here costs a
       reload of the track and the car. */
    case MENU_TEXQUAL: {
        int q = clampi(m->tex_quality + d, 0, MENU_TEXQUAL_LEVELS - 1);
        if (q != m->tex_quality) {
            m->tex_quality = q;
            m->req_reload = 1;
        }
        break;
    }
    /* Two values, so either direction toggles. Costs a reload, so it only
       raises one when the value really moved. */
    case MENU_TEXORDER:
        m->tex_swap_rb = !m->tex_swap_rb;
        m->req_reload = 1;
        break;
    default: break;
    }
    m->cue = MENU_CUE_CHANGE;
}

void menu_input(menu_t *m, unsigned int buttons, unsigned int prev)
{
    unsigned int hit = buttons & ~prev;

    if (hit & SCE_CTRL_START) {
        /* START is both the way in and the way out. */
        m->open = !m->open;
        if (m->open)
            m->row = 0;
        m->cue = m->open ? MENU_CUE_SELECT : MENU_CUE_CLOSE;
        return;
    }
    if (!m->open)
        return;

    if (hit & SCE_CTRL_CIRCLE) { m->open = 0; m->cue = MENU_CUE_CLOSE; return; }
    if (hit & SCE_CTRL_UP)   { m->row = wrap(m->row - 1, MENU_ROWS); m->cue = MENU_CUE_MOVE; }
    if (hit & SCE_CTRL_DOWN) { m->row = wrap(m->row + 1, MENU_ROWS); m->cue = MENU_CUE_MOVE; }
    if (hit & SCE_CTRL_LEFT)   adjust(m, -1);
    if (hit & SCE_CTRL_RIGHT)  adjust(m, +1);

    if (hit & SCE_CTRL_CROSS) {
        switch (m->row) {
        case MENU_RESTART: m->req_restart = 1; m->open = 0; m->cue = MENU_CUE_SELECT; break;
        case MENU_RESUME:  m->open = 0; m->cue = MENU_CUE_CLOSE; break;
        case MENU_QUIT:    m->req_quit = 1; m->cue = MENU_CUE_SELECT; break;
        default:          adjust(m, +1); break;   /* CROSS cycles a picker */
        }
    }
}

/* The value column for each row. */
static void row_value(const menu_t *m, int row, char *out, int n)
{
    const rb_car_data *d = &RB_CARS[wrap(m->car, 3)];

    switch (row) {
    case MENU_TRACK:
        snprintf(out, n, "< %s >  (%d/%d)",
                 TRACKS[m->track].name, m->track + 1, N_TRACKS);
        break;
    case MENU_CAR:
        snprintf(out, n, "< %s >", d->name);
        break;
    case MENU_TIRES:
        /* carTireGrip multiplies the axle coefficient by this. */
        snprintf(out, n, "< %d >  grip x%.2f",
                 m->tires + 1, (double)d->tune.tire_upgrade[m->tires]);
        break;
    case MENU_RESO:
        /* carEngineAccel scales top speed and acceleration by these. */
        snprintf(out, n, "< %d >  speed x%.2f  accel x%.2f",
                 m->reso + 1,
                 (double)d->tune.resonator_speed[m->reso],
                 (double)d->tune.resonator_accel[m->reso]);
        break;
    case MENU_BOOST:
        /* The exhaust the car wears. Level names the UPGRADES<n> subtree, and
           the multiplier is UPGRADES.ini's [BOOSTERS] row for this car -- shown
           so the number is visible even though nothing reads it yet. */
        snprintf(out, n, "< %d >  exhaust %d  (boost x%.2f, not wired)",
                 m->boost + 1, m->boost + 1,
                 (double)RB_BOOSTER_UPGRADE[wrap(m->car, 3)][m->boost]);
        break;
    case MENU_VOL_SFX:
    case MENU_VOL_MUSIC: {
        /* A bar, because a volume is a quantity and "7" is not. */
        int v = (row == MENU_VOL_SFX) ? m->vol_sfx : m->vol_music;
        char bar[MENU_VOL_STEPS + 1];
        int k;
        for (k = 0; k < MENU_VOL_STEPS; k++)
            bar[k] = (k < v) ? '#' : '.';
        bar[MENU_VOL_STEPS] = 0;
        snprintf(out, n, "< %s >  %d%%", bar, v * 100 / MENU_VOL_STEPS);
        break;
    }
    case MENU_TEXQUAL: {
        /* Named by what it does to the image, with the size the biggest textures
           end up at. Those sizes are the original's own three sets -- 512 is
           Textures.1, 256 is Textures.2, 128 is Textures.3 -- reached by skipping
           mip levels rather than by shipping three copies. See scene.h. */
        static const char *const NAME[MENU_TEXQUAL_LEVELS] = {
            "High", "Medium", "Low"
        };
        int q = clampi(m->tex_quality, 0, MENU_TEXQUAL_LEVELS - 1);
        snprintf(out, n, "< %s >  up to %d px%s", NAME[q], 512 >> q,
                 q ? "  (reloads)" : "");
        break;
    }
    case MENU_TEXORDER:
        /* Says which machine each setting is for, because the symptom is
           unmistakable and the cause is not: the wrong one here gives blue sand
           and a gold sea. Real hardware wants the standard order. */
        snprintf(out, n, "< %s >",
                 m->tex_swap_rb ? "R/B swapped (for Vita3K)"
                                : "standard (for real hardware)");
        break;
    default:
        out[0] = 0;
        break;
    }
}

void menu_draw(const menu_t *m, int screen_w, int screen_h)
{
    const float S = 1.0f;                 /* font scale */
    const float lh = ui_text_h(S) + 8.0f; /* line height */
    const float pad = 22.0f;
    const float pw = 620.0f;
    const float ph = pad * 2.0f + lh * (MENU_ROWS + 3.0f);
    const float px = ((float)screen_w - pw) * 0.5f;
    const float py = ((float)screen_h - ph) * 0.5f;
    const float vx = px + pad + 250.0f;   /* value column */
    float y;
    int i;
    char buf[128];

    ui_begin(screen_w, screen_h);

    /* dim the world, then the panel over it */
    ui_rect(0.0f, 0.0f, (float)screen_w, (float)screen_h, 0.0f, 0.0f, 0.0f, 0.55f);
    ui_rect(px, py, pw, ph, 0.06f, 0.08f, 0.11f, 0.94f);
    ui_rect(px, py, pw, 3.0f, 0.95f, 0.62f, 0.15f, 1.0f);

    y = py + pad;
    ui_text(px + pad, y, S, 0.95f, 0.62f, 0.15f, 1.0f, "RC CARS");
    ui_text(px + pw - pad - ui_text_w(S, "PAUSED"), y, S,
            0.55f, 0.58f, 0.62f, 1.0f, "PAUSED");
    y += lh * 1.6f;

    for (i = 0; i < MENU_ROWS; i++) {
        int sel = (i == m->row);
        if (sel)
            ui_rect(px + pad * 0.5f, y - 4.0f, pw - pad, lh,
                    0.95f, 0.62f, 0.15f, 0.22f);

        ui_text(px + pad, y, S,
                sel ? 1.0f : 0.72f, sel ? 0.85f : 0.75f, sel ? 0.55f : 0.78f, 1.0f,
                ROW_LABEL[i]);

        if (i < MENU_FIRST_ACTION) {
            row_value(m, i, buf, sizeof(buf));
            ui_text(vx, y, S,
                    sel ? 1.0f : 0.78f, sel ? 1.0f : 0.80f, sel ? 1.0f : 0.84f, 1.0f,
                    buf);
        }
        y += lh;
        if (i == MENU_FIRST_ACTION - 1)
            y += lh * 0.4f;               /* breathe before the actions */
    }

    y = py + ph - pad - ui_text_h(S);
    ui_text(px + pad, y, S, 0.45f, 0.48f, 0.52f, 1.0f,
            "D-pad move/change   X select   O or START close");

    ui_end();
}
