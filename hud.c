/*
 * hud.c -- see hud.h. The artwork, its size and its atlas split are the game's
 * own (msg_hits, message slots 3 and 4); the trigger, the timing and the choice
 * between the two halves are the port's, because the retail exe never posts
 * either slot.
 */

#include "hud.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void hud_init(hud_t *h, unsigned int tex)
{
    memset(h, 0, sizeof(*h));
    h->tex = tex;
}

void hud_reset(hud_t *h)
{
    if (!h)
        return;
    /* NOT a memset: the texture binding outlives a respawn. It is bound once
       from the shared props scene, which is loaded before the first track and
       never reloaded. */
    h->t = 0.f;
    h->count = 0;
    h->speed = 0.f;
}

void hud_hit(hud_t *h, float speed)
{
    if (!h)
        return;
    /* A brush too gentle to make a noise makes no banner either -- see
       HUD_HIT_MIN_SPEED. */
    if (!(speed >= HUD_HIT_MIN_SPEED))
        return;

    /* A pop already up is EXTENDED and counted up rather than replaced. Driving
       through a cluster of cans is one event to the player, and three overlapping
       banners fading at three different rates is not what that looks like. The
       hardest knock in the group is the one that decides the tier. */
    if (h->t > 0.f) {
        h->count++;
        if (speed > h->speed)
            h->speed = speed;
    } else {
        h->count = 1;
        h->speed = speed;
    }
    h->t = HUD_HIT_TIME;
}

void hud_step(hud_t *h, float dt)
{
    if (!h || h->t <= 0.f)
        return;
    h->t -= dt;
    if (h->t <= 0.f) {
        h->t = 0.f;
        h->count = 0;
        h->speed = 0.f;
    }
}

int hud_active(const hud_t *h)
{
    return h && h->t > 0.f && h->count > 0;
}

int hud_is_great(const hud_t *h)
{
    if (!hud_active(h))
        return 0;
    return h->speed >= HUD_GREAT_SPEED || h->count >= HUD_GREAT_COUNT;
}

void hud_draw(const hud_t *h, int screen_w, int screen_h)
{
    float age, k, alpha, cx, cy, w, hh;
    const float sw = (float)screen_w, sh = (float)screen_h;

    if (!hud_active(h))
        return;

    age = HUD_HIT_TIME - h->t;

    /* The overshoot, about the banner's own centre -- FUN_004b12b0's idea, with
       the port's curve. Linear: an ease here would be another invented curve for
       something that is on screen for a tenth of a second. */
    k = 1.f;
    if (age < HUD_HIT_POP && HUD_HIT_POP > 0.f)
        k += (HUD_HIT_OVERSHOOT - 1.f) * (1.f - age / HUD_HIT_POP);

    /* Full alpha until HUD_HIT_HOLD of life is left, then out. The original
       never fades a message -- its colour argument is a flat 0xffffffff -- but it
       also has a message queue to retire one, and this does not. */
    alpha = (h->t >= HUD_HIT_HOLD) ? 1.f : (h->t / HUD_HIT_HOLD);
    alpha = clampf(alpha, 0.f, 1.f);

    /* The recovered geometry. HEIGHT is the recovered fraction of the screen;
       WIDTH comes from the recovered pair and the 4:3 frame they were authored
       against, which is what keeps the art at its own aspect on a 16:9 panel.
       See HUD_MSG_REF_ASPECT. */
    hh = HUD_MSG_H * sh * k;
    w  = hh * (HUD_MSG_W / HUD_MSG_H) * HUD_MSG_REF_ASPECT;

    /* x0 = (1 - w)/2: FUN_004b11e0 centres every message across the screen. */
    cx = sw * 0.5f;
    /* y0 = (base - h)/2, base 1.0 / 0.5 / 1.5 for the drawer's three vertical
       bands, with its own yoff table zero. Expressed as the band's centre so the
       overshoot scales about it. */
    {
        const float base = (HUD_MSG_ANCHOR == 0) ? 1.0f
                         : (HUD_MSG_ANCHOR == 1) ? 0.5f : 1.5f;
        cy = sh * base * 0.5f;
    }

    ui_begin(screen_w, screen_h);

    if (h->tex) {
        /* The game's own banner: the top half of msg_hits for an ordinary hit,
           the bottom half for a great one. White, so the ARGB8888 art arrives
           unmodulated; only the fade touches alpha. */
        const float v0 = hud_is_great(h) ? HUD_MSG_V_SPLIT : 0.f;
        const float v1 = hud_is_great(h) ? 1.f : HUD_MSG_V_SPLIT;
        ui_image(cx - w * 0.5f, cy - hh * 0.5f, w, hh, h->tex,
                 0.f, v0, 1.f, v1, 1.f, 1.f, 1.f, alpha);
    } else {
        /* msg_hits was not packed. The compiled-in font instead, so the feedback
           degrades rather than disappearing -- font.h is in the binary and cannot
           fail to load. Same tier split, in words. */
        const char *word = hud_is_great(h) ? "GREAT HIT!" : "HIT!";
        const float scale = HUD_HIT_SCALE * k;
        const float x = cx - ui_text_w(scale, word) * 0.5f;
        const float y = cy - ui_text_h(scale) * 0.5f;
        /* A shadow, offset with the size. The tracks are sand, asphalt and pale
           stone; bright text alone on any of them is a smear. */
        const float shd = ui_text_h(scale) * 0.06f;
        ui_text(x + shd, y + shd, scale, 0.f, 0.f, 0.f, alpha * 0.7f, word);
        ui_text(x, y, scale, 1.f, 0.85f, 0.25f, alpha, word);
    }

    ui_end();
}
