/*
 * touch.h -- the front touch panel, as SCREEN PIXELS and edges.
 *
 * The Vita's front panel reports in its own units, not in pixels: the panel is
 * 1920 x 1088 of touch resolution over the 960 x 544 the screen draws in, so
 * every report is a factor of two out and the two numbers are NOT the same
 * factor on both axes for every panel revision. sceTouchGetPanelInfo gives the
 * ACTIVE AREA of the panel this machine has, and that -- not a hardcoded 2 --
 * is what the mapping divides by. A port that assumes 1920x1088 works on the
 * machine it was written on and is off by a few pixels near the edges elsewhere,
 * which is exactly the class of bug that only shows up on someone else's Vita.
 *
 * WHAT A UI ACTUALLY NEEDS is not "where is the finger" but the three edges
 * around it -- pressed, just pressed, just released -- and WHERE IT WENT DOWN,
 * because a button is pressed on release inside itself and a drag that starts
 * on a button and ends elsewhere must not fire it. All four are here so that no
 * caller has to keep its own previous frame.
 *
 * HOST-TESTABLE. Nothing in here calls into the SDK when __vita__ is not
 * defined; touch_inject drives it instead, so a harness can press, drag and
 * release without a Vita. That is the same shape menu.h uses for SCE_CTRL_*.
 */
#ifndef TOUCH_H
#define TOUCH_H

typedef struct {
    int   down;         /* a finger is on the panel right now */
    int   pressed;      /* it went down this frame */
    int   released;     /* it came up this frame */
    float x, y;         /* where it is, in screen pixels */
    float down_x, down_y;  /* where it went DOWN, in screen pixels */
    /* The panel's active area, as read from the hardware. 0 until the first
       touch_step, and 0 on the host unless a harness sets it. */
    float panel_w, panel_h;
} touch_state;

/* Reads the panel once and updates the edges. `screen_w`/`screen_h` are what the
   caller is drawing in, which is what x/y come back in. Call once a frame,
   before anything reads the state. */
void touch_step(touch_state *t, int screen_w, int screen_h);

/* Is (x, y) inside the rect, in screen pixels? */
int  touch_in(float x, float y, float rx, float ry, float rw, float rh);

/* A CLICK ON THIS RECT: released inside it, having gone down inside it too.
   That second half is what makes a drag off a button cancel rather than fire,
   which is what every touch UI does and what a user expects when they realise
   mid-press that they meant the row below. */
int  touch_click(const touch_state *t, float rx, float ry, float rw, float rh);

/* THE HOST'S WAY IN. `down` is the panel state for the next touch_step; the
   position is in screen pixels. Ignored on the Vita, where the panel is real. */
void touch_inject(float x, float y, int down);

#endif /* TOUCH_H */
