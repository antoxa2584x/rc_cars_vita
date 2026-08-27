/*
 * results.h -- THE FINISH SCREEN, on the engine's own `dlgFINISH'.
 *
 * The exe carries the dialog as a table of
 * { u32 id; u32 type; char *name; u32 0 } at 0x56e9a8:
 *
 *   0x170c &MBB  buttonAbort        the orange one -- Quit
 *   0x170d &MBB  none               the red one -- Race again
 *   0x170e &TBL  tableFinish        the results grid
 *   0x170f &TBL  tableBonus         \
 *   0x1711 &STT  staticBonusGained   |  the championship's prize money,
 *   0x1712 &STT  staticBonusGainedCash |  which a quick race has none of
 *   0x1713 &STT  staticBonusGained   |
 *   0x1714 &STT  staticBonusCash    /
 *
 * AND ITS LAYOUT IS SHIPPED. `Settings/dlgFINISH.ini` gives every rectangle in
 * the 800x600 frame the whole interface is authored in, and the table's six
 * column widths as percentages -- so nothing here is measured off a picture and
 * nothing is invented. gen_dlg_data.py bakes it into dlg_data.h. See ui.md.
 *
 * WHAT GOES IN THE TABLE. Six columns, which the original's own screenshot reads
 * as Player / Place / Time / Gap / Best lap / Av. speed, with the portrait in a
 * seventh column ahead of them (`tableMordaShift' -- morda is the artists' word
 * for the face). Every number is one this port already has or can keep cheaply:
 *
 *   Place      the placing, off the same progress the HUD's badge uses
 *   Time       the race clock at the moment that racer crossed for the last
 *              time. A racer who had NOT finished when the player did shows
 *              `---': the race ends at the player's flag, which is what this
 *              port does, and inventing a time for a car still driving would be
 *              inventing a result
 *   Gap        behind the winner. Seconds for a finisher; for a racer still out
 *              there, the DISTANCE it was behind, in metres, which is the true
 *              answer to the same question and says plainly that it did not
 *              finish
 *   Best lap   the quickest lap that racer turned, watched off its own lap
 *              counter -- the player's is race_ui's, which already tracks it
 *   Av. speed  the road actually driven over the time it took, km/h
 *
 * The caller fills a `results_state' and this draws it; nothing in here reaches
 * into the physics, the AI or the checkpoint layer, which is the rule race_ui.h
 * and dirarrow.h are written to.
 */
#ifndef RESULTS_H
#define RESULTS_H

#define RES_MAX_ROWS 6
#define RES_NAME 24

/* The two buttons, in the dialog's own order. */
enum {
    RES_BTN_QUIT = 0,        /* buttonAbort */
    RES_BTN_AGAIN,           /* the one the table calls `none' */
    RES_N_BTN
};

typedef enum {
    RES_ACT_NONE = 0,
    RES_ACT_QUIT,            /* back to the main menu */
    RES_ACT_AGAIN            /* restart this race */
} res_action;

typedef struct {
    unsigned int back;       /* Button_back    -- the orange Quit */
    unsigned int buttons;    /* ButtonsTextures -- the red Race again */
    unsigned int panel;      /* messagebox_empty, the table's own plate */
    unsigned int face[RES_MAX_ROWS];  /* one portrait per row, 0 for none */
    unsigned int font_big;   /* Smash26 */
    unsigned int font_small; /* Smash20 */
} results_tex;

typedef struct {
    char  name[RES_NAME];
    int   place;             /* 1-based */
    int   finished;          /* crossed the last line before the race ended */
    float time;              /* seconds; only meaningful when `finished' */
    float gap;               /* seconds behind the winner, when both finished */
    float behind_m;          /* metres behind the winner, when it did not */
    float best_lap;          /* seconds, 0 for none turned */
    float av_speed;          /* km/h over the road it drove */
    int   is_player;
} results_row;

typedef struct {
    results_tex tex;
    results_row row[RES_MAX_ROWS];
    int   n;
    int   focus;             /* RES_BTN_* */
    int   armed;             /* the button a touch went down on, or -1 */
    float t;                 /* seconds since it came up, for the grow-in */
    res_action action;       /* cleared every step */
    int   cue;               /* 1 focus moved, 2 a button fired */
} results_t;

void results_init(results_t *r, const results_tex *tex);

/* Sorts the rows into place order and stamps the places and the gaps. Called
   once, when the race ends. */
void results_finish(results_t *r);

/* One frame. `buttons` is SceCtrlData.buttons; `tp` may be NULL. */
void results_step(results_t *r, unsigned int buttons, const void *tp,
                  int screen_w, int screen_h, float dt);

/* Between ui_begin/ui_end, over the frozen world. */
void results_draw(const results_t *r, int screen_w, int screen_h);

/* Which button is under (x, y), or -1. Exposed for the harness. */
int  results_btn_at(const results_t *r, int screen_w, int screen_h,
                    float x, float y);

#endif /* RESULTS_H */
