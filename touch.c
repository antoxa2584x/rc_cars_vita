/* touch.c -- see touch.h. */
#include "touch.h"

#ifdef __vita__
#include <psp2/touch.h>
#endif

/* Injected state, for the host. Two globals rather than a parameter because
   touch_step's signature has to be the same on both sides. */
static float g_inj_x, g_inj_y;
static int   g_inj_down;

void touch_inject(float x, float y, int down)
{
    g_inj_x = x;
    g_inj_y = y;
    g_inj_down = down;
}

void touch_step(touch_state *t, int screen_w, int screen_h)
{
    int down = 0;
    float px = t->x, py = t->y;

    if (!t)
        return;

#ifdef __vita__
    {
        static int started;
        static float pw, ph;
        SceTouchData d;

        /* ONCE. sceTouchSetSamplingState is what turns the panel on; without it
           sceTouchPeek returns zero reports forever and the menu is simply
           dead to touch with nothing in the log to say why. */
        if (!started) {
            SceTouchPanelInfo info;
            sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
                                     SCE_TOUCH_SAMPLING_STATE_START);
            /* THE PANEL'S OWN ACTIVE AREA, not a hardcoded 1920x1088 -- see
               touch.h. A panel that will not answer keeps 0, and the mapping
               below falls back to the documented nominal size rather than
               dividing by nothing. */
            pw = ph = 0.f;
            if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &info) >= 0) {
                pw = (float)(info.maxAaX - info.minAaX);
                ph = (float)(info.maxAaY - info.minAaY);
            }
            if (!(pw > 1.f)) pw = 1920.f;
            if (!(ph > 1.f)) ph = 1088.f;
            started = 1;
        }
        t->panel_w = pw;
        t->panel_h = ph;

        if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &d, 1) >= 0 && d.reportNum > 0) {
            /* THE FIRST REPORT ONLY. The panel tracks up to six fingers; a menu
               wants one, and taking the first keeps a stray palm on the far side
               of the panel from dragging the selection around. */
            down = 1;
            px = (float)d.report[0].x / pw * (float)screen_w;
            py = (float)d.report[0].y / ph * (float)screen_h;
        }
    }
#else
    if (t->panel_w <= 0.f) { t->panel_w = 1920.f; t->panel_h = 1088.f; }
    down = g_inj_down;
    if (down) { px = g_inj_x; py = g_inj_y; }
    (void)screen_w; (void)screen_h;
#endif

    t->pressed  = down && !t->down;
    t->released = !down && t->down;
    /* THE POSITION IS HELD THROUGH THE RELEASE. The panel reports nothing at
       all on the frame a finger leaves, so reading x/y then would give wherever
       the finger was two frames ago -- or, on the first release of a session,
       the origin, which is inside whatever is drawn in the top-left corner.
       Keeping the last DOWN position is what makes touch_click land where the
       finger actually lifted. */
    if (down) {
        t->x = px;
        t->y = py;
    }
    if (t->pressed) {
        t->down_x = t->x;
        t->down_y = t->y;
    }
    t->down = down;
}

int touch_in(float x, float y, float rx, float ry, float rw, float rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

int touch_click(const touch_state *t, float rx, float ry, float rw, float rh)
{
    if (!t || !t->released)
        return 0;
    return touch_in(t->x, t->y, rx, ry, rw, rh)
        && touch_in(t->down_x, t->down_y, rx, ry, rw, rh);
}
