/*
 * msg.c -- see msg.h. The eleven-slot table, the three arbitration rules, the
 * hold-over, the three bands and the grow-in are all the engine's; the two lives
 * for PAUSE and BEST LAP, msg_hold, and the font fallbacks are the port's.
 */

#include "msg.h"
#include "ui.h"

#include <string.h>

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int slot_ok(int slot)
{
    return slot >= 0 && slot < MSG_N_SLOTS;
}

/* ------------------------------------------------------------------ the state */

void msg_init(msg_t *m, const unsigned int *tex)
{
    int i;
    if (!m)
        return;
    memset(m, 0, sizeof(*m));
    if (tex)
        for (i = 0; i < MSG_N_TEX; i++)
            m->tex[i] = tex[i];
    m->slot = -1;
    m->cue = -1;
    m->anchor = 0;
}

void msg_reset(msg_t *m)
{
    if (!m)
        return;
    /* The engine's own `post slot -1', which the poster treats as "clear" -- and
       NOT a memset: the six bindings come out of the load-once props scene and
       outlive every restart. */
    m->slot = -1;
    m->life = 0.f;
    m->total = 0.f;
    m->age = 0.f;
    m->hold = 0.f;
    m->animate = 0;
    m->cue = -1;
}

int msg_post(msg_t *m, int slot, float life, float hold, int animate)
{
    if (!m || !slot_ok(slot))
        return 0;

    /* RULE 1: a post of the slot already showing is DROPPED. This is the whole
       of the pulse: a caller that posts every frame while some condition holds
       gets one flash per life-plus-hold, and needs no state of its own. */
    if (m->slot == slot)
        return 0;

    /* RULE 2: dropped when what is showing outranks it. STRICTLY higher, so
       equal priorities replace -- which is most pairs, seven of the eleven slots
       being priority 0. */
    if (m->slot >= 0 && MSG_SLOT[m->slot].prio > MSG_SLOT[slot].prio)
        return 0;

    m->slot = slot;
    m->life = life;
    m->total = life;
    m->age = 0.f;
    m->hold = hold;
    m->animate = animate;
    m->cue = slot;
    return 1;
}

int msg_hold(msg_t *m, int slot, float life)
{
    if (!m || m->slot != slot || !slot_ok(slot))
        return 0;
    /* The LIFE only. Not `total', which would restart the swell every frame, and
       not `age', which is what makes the swell progress at all -- refreshing the
       life alone is exactly why `age' has to be its own field rather than
       `total - life'. See msg.h. */
    if (life > m->life)
        m->life = life;
    return 1;
}

void msg_step(msg_t *m, float dt)
{
    if (!m || m->slot < 0)
        return;
    /* 0x4af28e, and the hold-over is why this is not `life <= 0': the slot stays
       SELECTED -- and so keeps defending itself against equal-priority posts --
       for `hold' seconds after it stops being drawn. */
    m->life -= dt;
    m->age += dt;
    if (m->life < -m->hold) {
        m->slot = -1;
        m->cue = -1;
    }
}

int msg_slot(const msg_t *m)
{
    return m ? m->slot : -1;
}

int msg_visible(const msg_t *m, int slot)
{
    return m && m->slot == slot && m->life > 0.f;
}

int msg_contender(const msg_t *m)
{
    if (!m || m->slot < 0 || m->life <= 0.f)
        return -1;
    return m->slot;
}

int msg_cue(msg_t *m)
{
    int c;
    if (!m)
        return -1;
    c = m->cue;
    m->cue = -1;
    return c;
}

int msg_arbitrate(const int *slots, int n)
{
    int i, best = -1;
    if (!slots)
        return -1;
    for (i = 0; i < n; i++) {
        const int s = slots[i];
        if (!slot_ok(s))
            continue;
        /* STRICTLY greater, so the FIRST of a tie holds the screen -- which is
           rule 2 ("equal replaces") seen from the caller's side: whichever of two
           equal contenders posted first is the one already showing. */
        if (best < 0 || MSG_SLOT[s].prio > MSG_SLOT[best].prio)
            best = s;
    }
    return best;
}

/* -------------------------------------------------------------- the geometry */

void msg_slot_rect(int slot, int anchor, float k, int screen_w, int screen_h,
                   float *cx, float *cy, float *w, float *h)
{
    const float sw = (float)screen_w, sh = (float)screen_h;
    float base, hh, ww;

    if (!slot_ok(slot))
        return;

    /* THE HEIGHT is the recovered fraction; the WIDTH comes from the recovered
       pair's own ratio and the 4:3 frame it was authored against. On a 4:3 panel
       that is bit-identical to using both fractions; on 960x544 it is the
       difference between the art and the art stretched 4/3 wider. */
    hh = MSG_SLOT[slot].h * sh * k;
    ww = hh * (MSG_SLOT[slot].w / MSG_SLOT[slot].h) * MSG_REF_ASPECT;

    /* FUN_004b11e0: x0 = (1 - w)/2 -- every message is centred across the screen
       -- and y0 = (base - h)/2 + yoff[anchor], with base 1.0, 0.5 or 1.5 for the
       three bands and yoff in the zero-filled part of .data. Expressed as the
       band's centre so the grow-in scales about it, which is what
       FUN_004b12b0 does. */
    base = (anchor == 0) ? 1.0f : (anchor == 1) ? 0.5f : 1.5f;

    if (cx) *cx = sw * 0.5f;
    if (cy) *cy = sh * base * 0.5f;
    if (w)  *w  = ww;
    if (h)  *h  = hh;
}

/* The grow-in: elapsed / max(total, MSG_GROW_FLOOR), clamped, and only when the
   post asked for it. The original never touches a message's alpha -- its colour
   argument is a flat 0xffffffff -- so what animates is the SCALE and nothing
   else. */
static float grow_of(const msg_t *m)
{
    float kk;
    if (m->animate != 1)
        return 1.f;
    kk = (m->total > MSG_GROW_FLOOR) ? m->total : MSG_GROW_FLOOR;
    if (!(kk > 0.f))
        return 1.f;
    return clampf(m->age / kk, 0.f, 1.f);
}

/* The fallback word per slot, for a props.vsc packed without that texture. Only
   the slots this port raises have one; the rest draw nothing rather than a
   made-up word. */
static const char *fallback_word(int slot)
{
    switch (slot) {
    case MSG_PAUSE:     return "PAUSE";
    case MSG_WRONG_WAY: return "WRONG WAY";
    case MSG_BEST_LAP:  return "BEST LAP";
    default:            return 0;
    }
}

void msg_draw(const msg_t *m, int screen_w, int screen_h)
{
    const msg_slot_def *d;
    /* Initialised because msg_slot_rect returns early on a slot it does not
       like, and the range check that makes that impossible here is three lines
       up where the compiler cannot see it. */
    float k, cx = 0.f, cy = 0.f, w = 0.f, h = 0.f;

    if (!m || m->slot < 0 || m->life <= 0.f
        || screen_w <= 0 || screen_h <= 0)
        return;

    d = &MSG_SLOT[m->slot];
    k = grow_of(m);
    msg_slot_rect(m->slot, m->anchor, k, screen_w, screen_h, &cx, &cy, &w, &h);

    ui_begin(screen_w, screen_h);
    if (m->tex[d->tex]) {
        /* The slot's own cell, white -- four of the six textures are atlases and
           the rect is what separates one message from the next in them. */
        ui_image(cx - w * 0.5f, cy - h * 0.5f, w, h, m->tex[d->tex],
                 d->u0, d->v0, d->u1, d->v1, 1.f, 1.f, 1.f, 1.f);
    } else {
        const char *word = fallback_word(m->slot);
        if (word && k > 0.f) {
            const float fs = MSG_FALLBACK_SCALE * k;
            const float x = cx - ui_text_w(fs, word) * 0.5f;
            const float y = cy - ui_text_h(fs) * 0.5f;
            /* A shadow, offset with the size: the tracks are sand, asphalt and
               pale stone, and bright text alone on any of them is a smear. */
            const float shd = ui_text_h(fs) * 0.06f;
            ui_text(x + shd, y + shd, fs, 0.f, 0.f, 0.f, 0.7f, word);
            ui_text(x, y, fs, 1.f, 0.15f, 0.10f, 1.f, word);
        }
    }
    ui_end();
}
