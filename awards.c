/*
 * awards.c -- see awards.h. Nothing in this file is the original's.
 */

#include "awards.h"
#include "sfont.h"
#include "ui.h"
#include "rlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/sysmodule.h>
#include <psp2/notificationutil.h>
#define AWARDS_DIR  "ux0:data/rccars"
#define AWARDS_FILE "ux0:data/rccars/awards.txt"
#else
#define AWARDS_FILE "rccars_awards.txt"
#endif

/* ------------------------------------------------------------------ the table
 *
 * The keys are the words in awards.txt and are fixed forever; the names and the
 * lines are the port's own prose. Every `what' is written as an INSTRUCTION,
 * because a line that describes a state ("fifty props knocked") does not tell a
 * player what to go and do.
 */
static const aw_def TAB[AW_N] = {
    { "finish",     "Flag Down",            "Finish a race.",                            AW_K_COUNT, 1 },
    { "win",        "First Blood",          "Win a race.",                               AW_K_COUNT, 1 },
    { "win5",       "Regular",              "Win five races.",                           AW_K_COUNT, 5 },
    { "tracks",     "Grand Tour",           "Finish a race on every track.",             AW_K_BITS, 10 },
    { "wintracks",  "Clean Sweep",          "Win on every track.",                       AW_K_BITS, 10 },
    { "wincars",    "Three Keys",           "Win with all three cars.",                  AW_K_BITS,  3 },
    { "sevenlaps",  "The Long Way",         "Finish a seven-lap race.",                  AW_K_COUNT, 1 },
    { "nodeaths",   "Not a Scratch",        "Win without being put back once.",          AW_K_COUNT, 1 },
    { "homealone",  "Home Alone",           "Win while the whole field is still out.",   AW_K_COUNT, 1 },
    { "netwin",     "Wi-Fi Warrior",        "Win a race against another machine.",       AW_K_COUNT, 1 },

    { "pegged",     "Pegged",               "Hold the speedo at the top of its dial.",   AW_K_COUNT, 1 },
    { "air",        "Air Time",             "Two seconds off the ground.",               AW_K_COUNT, 1 },
    { "tankdry",    "Tank Empty",           "Run the boost meter dry.",                  AW_K_COUNT, 1 },
    { "marathon",   "Marathon",             "Drive 42 km.",                              AW_K_COUNT, AW_MARATHON_M },
    { "drowned",    "Man Overboard",        "Find out how deep the sea is.",             AW_K_COUNT, 1 },
    { "wrongway",   "Scenic Route",         "Be told you are going the wrong way five times.", AW_K_COUNT, 5 },

    { "props50",    "Bull in a China Shop", "Knock over fifty things.",                  AW_K_COUNT, 50 },
    { "props500",   "Demolition Man",       "Knock over five hundred.",                  AW_K_COUNT, 500 },
    { "greathit",   "GREAT !HIT!",          "Knock something over hard enough.",         AW_K_COUNT, 1 },
    { "runover",    "Pedestrian Crossing",  "Run over ten of the locals.",               AW_K_COUNT, 10 },
    { "shotat",     "Wanted",               "Make a guard open fire on you.",            AW_K_COUNT, 1 },
    { "thrown",     "Manhandled",           "Get picked up and thrown.",                 AW_K_COUNT, 1 },

    { "upgrade",    "New Wheels",           "Fit an upgrade in the Garage.",             AW_K_COUNT, 1 },
    { "paints",     "Paint Shop",           "Try all four paints on a car.",             AW_K_BITS,  4 },
    { "hour",       "One More Race",        "Spend an hour racing.",                     AW_K_COUNT, AW_HOUR_S },
};

const aw_def *award_def(int id)
{
    return (id >= 0 && id < AW_N) ? &TAB[id] : NULL;
}

/* ------------------------------------------------------------------ the books */

typedef struct {
    char         name[PL_NAME];
    int          prog[AW_N];
    unsigned int got;
} aw_book;

static aw_book books[PL_MAX];
static int     n_books;
static int     live = -1;           /* index into books, or -1 */
static int     dirty;
static const char *file_path = AWARDS_FILE;

/* The toast queue and what is on screen. */
static int   queue[AW_N];
static int   n_queue;
static int   showing = -1;
static float show_t;                /* seconds left of AW_TOAST_LIFE */
static float gap_t;                 /* enforced quiet between two toasts */

/* The three things award_frame keeps between calls: the peg's hold clock, the
   metre remainder the integer tally would otherwise throw away, and the
   previous value of the two FLAGS whose EDGE is the event. */
static float peg_t;
static float metre_frac;
static int   was_wrong, was_great, was_dry;

static int have_bit(const aw_book *b, int id)
{
    return (b->got & (1u << id)) != 0;
}

static int reached(const aw_book *b, int id)
{
    const aw_def *d = &TAB[id];
    if (d->kind == AW_K_BITS) {
        unsigned int v = (unsigned int)b->prog[id];
        int n = 0;
        while (v) { n += (int)(v & 1u); v >>= 1; }
        return n >= d->goal;
    }
    return b->prog[id] >= d->goal;
}

/* ------------------------------------------------------------ the notification
 *
 * THE MACHINE'S OWN NOTIFICATION LIST, which is the one system service a
 * self-signed app can reach with something to say (awards.h says why the trophy
 * service is not it). `sceNotificationUtilSendNotification' takes UTF-16 in a
 * buffer the module reads 0x410 bytes of -- the header says so and the size is
 * not negotiable, so the buffer here is that size whatever the text is.
 *
 * BEST EFFORT, DELIBERATELY. It needs SCE_SYSMODULE_NOTIFICATION_UTIL loaded,
 * and neither the load nor the send is something this app can insist on: the
 * return of both is logged and nothing else in this file depends on either. The
 * player's acknowledgement is the toast, which is drawn by this port and cannot
 * fail. On a machine (or an emulator) where the module does not come up, the
 * only difference is that the notification drawer stays empty.
 */
#ifdef __vita__
static int notify_ready;

static void notify_init(void)
{
    int r;
    if (notify_ready)
        return;
    r = sceSysmoduleLoadModule(SCE_SYSMODULE_NOTIFICATION_UTIL);
    notify_ready = (r >= 0) ? 1 : -1;
    rlog("[rccars] awards: notification module %s (0x%08x)\n",
         r >= 0 ? "loaded" : "unavailable", (unsigned)r);
}

static void notify(const char *name)
{
    /* 0x410 bytes, zeroed: the module reads the whole buffer and the text is a
       NUL-terminated UTF-16 string at the front of it. */
    static SceWChar16 buf[0x410 / sizeof(SceWChar16)];
    const char *p;
    unsigned int i = 0;
    int r;

    notify_init();
    if (notify_ready != 1)
        return;
    memset(buf, 0, sizeof buf);
    /* ASCII only, which is all any string in this file is -- the names are
       written here and nothing localises them. */
    for (p = "Award: "; *p && i < 0x3e; p++)
        buf[i++] = (SceWChar16)(unsigned char)*p;
    for (p = name; *p && i < 0x3e; p++)
        buf[i++] = (SceWChar16)(unsigned char)*p;
    buf[i] = 0;
    r = sceNotificationUtilSendNotification(buf);
    if (r < 0)
        rlog("[rccars] awards: notification refused (0x%08x)\n", (unsigned)r);
}
#else
static void notify(const char *name) { (void)name; }
#endif

/* --------------------------------------------------------------- the unlock */

static void unlock(int id)
{
    aw_book *b = &books[live];

    b->got |= 1u << id;
    dirty = 1;
    if (n_queue < AW_N)
        queue[n_queue++] = id;
    rlog("[rccars] AWARD -- %s (%s), %d of %d\n",
         TAB[id].name, TAB[id].key, award_n_have(), AW_N);
    notify(TAB[id].name);
}

/* The one place progress is written, whatever shape it has. Returns 1 if this
   call is what earned the award, which is what the log line above wants. */
static int bump(int id, int delta)
{
    aw_book *b;

    if (live < 0 || id < 0 || id >= AW_N || delta <= 0)
        return 0;
    b = &books[live];
    if (have_bit(b, id))
        return 0;                   /* held: stop counting, it is done */
    b->prog[id] += delta;
    if (b->prog[id] > TAB[id].goal)
        b->prog[id] = TAB[id].goal;
    dirty = 1;
    if (reached(b, id)) {
        unlock(id);
        return 1;
    }
    return 0;
}

/* A bitmask award: set one bit. `bit' out of range is dropped rather than
   wrapped -- a track index this build does not have is a caller bug and
   masking it would credit the wrong track. */
static void set_bit(int id, int bit)
{
    aw_book *b;
    int mask;

    if (live < 0 || id < 0 || id >= AW_N || bit < 0 || bit >= 31)
        return;
    b = &books[live];
    if (have_bit(b, id))
        return;
    mask = 1 << bit;
    if (b->prog[id] & mask)
        return;
    b->prog[id] |= mask;
    dirty = 1;
    if (reached(b, id))
        unlock(id);
}

/* A tally that is a HIGH-WATER MARK rather than a sum -- the profile's own play
   time, which arrives as an absolute figure every time it is polled. */
static void at_least(int id, int value)
{
    aw_book *b;

    if (live < 0 || id < 0 || id >= AW_N)
        return;
    b = &books[live];
    if (have_bit(b, id) || value <= b->prog[id])
        return;
    b->prog[id] = value > TAB[id].goal ? TAB[id].goal : value;
    dirty = 1;
    if (reached(b, id))
        unlock(id);
}

/* -------------------------------------------------------------- the selection */

static int find_book(const char *name)
{
    int i;
    for (i = 0; i < n_books; i++) {
        const char *a = books[i].name, *bb = name;
        while (*a && *bb) {
            int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
            int cb = (*bb >= 'A' && *bb <= 'Z') ? *bb + 32 : *bb;
            if (ca != cb) break;
            a++; bb++;
        }
        if (!*a && !*bb)
            return i;
    }
    if (n_books >= PL_MAX)
        return -1;
    memset(&books[n_books], 0, sizeof books[0]);
    snprintf(books[n_books].name, sizeof books[0].name, "%s", name);
    return n_books++;
}

void award_select(const char *player)
{
    if (!player || !*player) {
        live = -1;
        return;
    }
    live = find_book(player);
    /* The per-race and per-frame carry-overs belong to whoever was driving, not
       to whoever is driving now. */
    peg_t = metre_frac = 0.f;
    was_wrong = was_great = was_dry = 0;
    if (live >= 0)
        rlog("[rccars] awards: `%s' -- %d of %d\n",
             books[live].name, award_n_have(), AW_N);
}

const char *award_player(void)
{
    return live >= 0 ? books[live].name : "";
}

int award_have(int id)
{
    if (live < 0 || id < 0 || id >= AW_N)
        return 0;
    return have_bit(&books[live], id);
}

int award_progress(int id)
{
    if (live < 0 || id < 0 || id >= AW_N)
        return 0;
    return books[live].prog[id];
}

int award_n_have(void)
{
    int i, n = 0;
    if (live < 0)
        return 0;
    for (i = 0; i < AW_N; i++)
        if (have_bit(&books[live], i))
            n++;
    return n;
}

float award_frac(int id)
{
    const aw_def *d = award_def(id);
    float f;

    if (!d || live < 0)
        return 0.f;
    if (award_have(id))
        return 1.f;
    if (d->kind == AW_K_BITS) {
        unsigned int v = (unsigned int)books[live].prog[id];
        int n = 0;
        while (v) { n += (int)(v & 1u); v >>= 1; }
        f = d->goal > 0 ? (float)n / (float)d->goal : 0.f;
    } else {
        f = d->goal > 0 ? (float)books[live].prog[id] / (float)d->goal : 0.f;
    }
    return f < 0.f ? 0.f : (f > 1.f ? 1.f : f);
}

/* ---------------------------------------------------------------- the events */

void award_race(const aw_race *r)
{
    if (live < 0 || !r)
        return;

    bump(AW_FIRST_RACE, 1);
    if (r->track >= 0)
        set_bit(AW_ALL_TRACKS, r->track);
    if (r->laps >= 7)
        bump(AW_SEVEN_LAPS, 1);

    if (r->place != 1)
        return;
    bump(AW_FIRST_WIN, 1);
    bump(AW_FIVE_WINS, 1);
    if (r->track >= 0)
        set_bit(AW_WIN_TRACKS, r->track);
    if (r->car >= 0)
        set_bit(AW_WIN_CARS, r->car);
    if (r->deaths == 0)
        bump(AW_NO_DEATHS, 1);
    /* NOBODY ELSE HOME, and it needs a field to be worth anything: on an empty
       grid the player is the only finisher by definition. */
    if (r->n_racers > 1 && r->n_finished <= 1)
        bump(AW_HOME_ALONE, 1);
    if (r->net)
        bump(AW_NET_WIN, 1);
}

void award_frame(float dt, float speed, float speed_max, float air,
                 int boost_dry, int wrong_way, int great_hit)
{
    if (live < 0 || dt <= 0.f)
        return;

    /* THE NEEDLE AT THE TOP OF ITS OWN DIAL, held. `speed_max' is the car's
       boost top speed through whatever resonator is fitted, which is what the
       gauge is scaled to -- so this is "peg your own car" and not a km/h
       figure that only the fast one can reach. */
    if (speed_max > 0.1f && speed >= speed_max * AW_PEG_FRAC) {
        peg_t += dt;
        if (peg_t >= AW_PEG_HOLD)
            bump(AW_PEGGED, 1);
    } else {
        peg_t = 0.f;
    }

    if (air >= AW_AIR_TIME)
        bump(AW_AIR, 1);

    /* THE METRES, which are a float and the tally is an int -- so the remainder
       is carried rather than dropped. Without this, 42 km at 60 fps rounds away
       to nothing: a frame at 30 km/h is 0.14 m. */
    metre_frac += speed * dt;
    if (metre_frac >= 1.f) {
        int whole = (int)metre_frac;
        metre_frac -= (float)whole;
        bump(AW_MARATHON, whole);
    }

    /* The three edges. The caller hands over flags; the previous value lives
       here, so no call site has to keep one. */
    if (boost_dry && !was_dry)
        bump(AW_TANK_DRY, 1);
    was_dry = boost_dry;
    if (wrong_way && !was_wrong)
        bump(AW_WRONG_WAY, 1);
    was_wrong = wrong_way;
    if (great_hit && !was_great)
        bump(AW_GREAT_HIT, 1);
    was_great = great_hit;
}

void award_prop(void)     { bump(AW_PROPS_50, 1); bump(AW_PROPS_500, 1); }
void award_run_over(void) { bump(AW_RUN_OVER, 1); }
void award_shot_at(void)  { bump(AW_SHOT_AT, 1); }
void award_thrown(void)   { bump(AW_THROWN, 1); }
void award_drowned(void)  { bump(AW_DROWNED, 1); }

void award_shop(int best_part_level, int skin, float play_time)
{
    if (live < 0)
        return;
    if (best_part_level >= 1)
        bump(AW_UPGRADE, 1);
    if (skin >= 0 && skin < 4)
        set_bit(AW_PAINTS, skin);
    if (play_time > 0.f)
        at_least(AW_HOUR, (int)play_time);
}

/* ----------------------------------------------------------------- the toast */

void award_step(float dt)
{
    if (dt < 0.f)
        dt = 0.f;
    if (showing >= 0) {
        show_t -= dt;
        if (show_t <= 0.f) {
            showing = -1;
            gap_t = AW_TOAST_GAP;
        }
        return;
    }
    /* The quiet between two toasts, and the FALL-THROUGH matters: consuming
       the gap and starting the next one are the same call, or a caller stepping
       exactly AW_TOAST_GAP would see the screen stay empty for another frame. */
    if (gap_t > 0.f) {
        gap_t -= dt;
        if (gap_t > 0.f)
            return;
        gap_t = 0.f;
    }
    if (n_queue > 0) {
        int i;
        showing = queue[0];
        for (i = 1; i < n_queue; i++)
            queue[i - 1] = queue[i];
        n_queue--;
        show_t = AW_TOAST_LIFE;
    }
}

int award_showing(void) { return showing; }
int award_pending(void) { return n_queue; }

/* WHERE THE TOAST SITS, in the 800x600 frame every other HUD number is written
 * in (hud_data.h), and it is the one band of that frame nothing else is in:
 *
 *   the place badge   (25, -20) 128 square      -> down to y 108
 *   the two clocks    top centre
 *   the lap word      x 613 and right
 *   the map panel     (620, 79) 167 square
 *   the boost dial    centre (87, 507) r 92     -> from y 415
 *   the speed dial    its mirror, bottom right
 *   the arrow         a 0.17 square at (0.5, 0.87) of the screen
 *
 * So (25, 150) 300 x 56, anchored LEFT off a uniform scale, which is
 * race_ui.c's own mapping and its `L' anchor. It clears the badge above it by
 * 42 px and the dial below it by 209.
 */
#define AW_T_X    25.f
#define AW_T_Y   150.f
#define AW_T_W   300.f
#define AW_T_H    58.f

void award_draw(const award_tex *t, int screen_w, int screen_h)
{
    const aw_def *d = award_def(showing);
    float s, x, y, w, h, a;
    sfont big, small_;

    if (!d || !t || screen_w <= 0 || screen_h <= 0)
        return;

    s = (float)screen_h / HUD_REF_H;
    x = AW_T_X * s;
    y = AW_T_Y * s;
    w = AW_T_W * s;
    h = AW_T_H * s;

    /* Fade in and out over the same slice of the life at each end -- the
       message layer scales instead (msg.h) and this is not one of its slots, so
       it takes the port's own hud.c behaviour: a fade. */
    a = 1.f;
    if (show_t > AW_TOAST_LIFE - AW_TOAST_FADE)
        a = (AW_TOAST_LIFE - show_t) / AW_TOAST_FADE;
    else if (show_t < AW_TOAST_FADE)
        a = show_t / AW_TOAST_FADE;
    if (a < 0.f) a = 0.f;
    if (a > 1.f) a = 1.f;

    big = sf_big(t->font_big);
    small_ = sf_small(t->font_small);

    ui_begin(screen_w, screen_h);
    /* THE PLATE: a dark panel and a gold rule along its top edge, which is the
       front end's own division of a heading from what is under it (mm_rule_at)
       and the only thing here that is borrowed from anywhere. See award_tex on
       why the shipped panel art is not used. */
    ui_rect(x, y, w, h, 0.04f, 0.04f, 0.05f, 0.80f * a);
    ui_rect(x, y, w, 2.f * s, 1.f, 0.82f, 0.20f, 0.90f * a);

    /* THE WORD `AWARD' in the small font and the award's NAME in the big one,
       and the name is SHRUNK TO FIT rather than clipped: `Bull in a China Shop'
       is 20 characters and the plate is 300 wide, so one of the two has to give
       and it is not the plate -- a toast that changes width every time reads as
       a different element. */
    {
        const float pad = 10.f * s;
        const float hs = 0.55f * s;
        const char *hdr = "AWARD";
        float ns = 0.92f * s, nw;

        /* The two lines do not overlap and that is arithmetic, not taste: the
           font's cell is SF_SMALL_SIZE_Y (28) design px tall times the scale,
           so 0.55 is 15 px from y + 5 and 0.92 is 26 px from y + 24, inside a
           plate of 58. */
        if (small_.tex)
            sf_text_shadowed(&small_, x + pad, y + 5.f * s, hs,
                             1.f, 0.82f, 0.20f, a, hdr);
        else
            ui_text(x + pad, y + 5.f * s, hs, 1.f, 0.82f, 0.20f, a, hdr);

        nw = big.tex ? sf_w(&big, ns, d->name) : ui_text_w(ns, d->name);
        if (nw > w - pad * 2.f && nw > 0.f)
            ns *= (w - pad * 2.f) / nw;
        if (big.tex)
            sf_text_shadowed(&big, x + pad, y + 24.f * s, ns,
                             1.f, 1.f, 1.f, a, d->name);
        else
            ui_text(x + pad, y + 24.f * s, ns, 1.f, 1.f, 1.f, a, d->name);
    }
    ui_end();
}

/* ------------------------------------------------------------------- the text */

/* settings.c's and records.c's `match', kept separate for the reason records.c
   states: three independent formats, and a helper reached across would make a
   change to one able to break the other two. */
static const char *match(const char *line, const char *key)
{
    size_t n = strlen(key);

    if (strncmp(line, key, n) != 0)
        return NULL;
    if (line[n] != ' ' && line[n] != '\t')
        return NULL;
    line += n;
    while (*line == ' ' || *line == '\t')
        line++;
    return line;
}

static int key_id(const char *key, int len)
{
    int i;
    for (i = 0; i < AW_N; i++)
        if ((int)strlen(TAB[i].key) == len && !strncmp(TAB[i].key, key, (size_t)len))
            return i;
    return -1;
}

void award_reset(void)
{
    memset(books, 0, sizeof books);
    n_books = 0;
    live = -1;
    dirty = 0;
    n_queue = 0;
    showing = -1;
    show_t = gap_t = 0.f;
    peg_t = metre_frac = 0.f;
    was_wrong = was_great = was_dry = 0;
}

int award_dirty(void) { return dirty; }

/*
 * `book <name...>` opens one and every `aw <key> <progress> <got>` after it
 * belongs to it. The NAME IS LAST on its line and runs to the end of it, which
 * is records.c's rule and its reason: a profile name can hold a space and a
 * quoted field is a second escaping rule to get wrong.
 */
int award_parse(const char *text)
{
    const char *p = text;
    char line[192];
    int at = -1;

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const char *v;
        size_t i;
        char *q;

        if (len >= sizeof line)
            len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        p = nl ? nl + 1 : p + strlen(p);

        for (i = 0; i < len; i++)
            if (line[i] == '#' || line[i] == '\r') {
                line[i] = 0;
                break;
            }
        q = line;
        while (*q == ' ' || *q == '\t')
            q++;
        if (!*q)
            continue;
        if ((v = match(q, "version")) != NULL) {
            long ver = strtol(v, NULL, 10);
            if (ver > AWARDS_VERSION)
                return 0;
            continue;
        }
        if ((v = match(q, "book")) != NULL) {
            at = *v ? find_book(v) : -1;
            continue;
        }
        if ((v = match(q, "aw")) != NULL) {
            const char *k = v;
            int klen = 0, id, prog, got;
            char *end;
            while (k[klen] && k[klen] != ' ' && k[klen] != '\t')
                klen++;
            id = key_id(k, klen);
            v = k + klen;
            prog = (int)strtol(v, &end, 0);
            if (end == v)
                continue;
            v = end;
            got = (int)strtol(v, &end, 10);
            if (end == v)
                got = 0;
            /* An UNKNOWN KEY is an award from another build and is skipped, not
               refused -- settings.h's rule. So is a row before any `book'. */
            if (id < 0 || at < 0)
                continue;
            books[at].prog[id] = prog;
            if (got)
                books[at].got |= 1u << id;
            continue;
        }
        /* Anything else is a key from another version, or a typo. */
    }
    return 1;
}

/* Every progress value is clamped into its own award's goal and every `got' bit
   is checked against it -- a hand-edited file is the reason this exists, and a
   `marathon 999999999' that overflowed a later sum is the reason it clamps
   rather than merely rejects. A GOT BIT IS KEPT even where the progress does
   not support it: an award once earned is earned, and a build that raised a
   threshold must not take it away. */
static void clamp_books(void)
{
    int i, k;

    if (n_books < 0) n_books = 0;
    if (n_books > PL_MAX) n_books = PL_MAX;
    for (i = 0; i < n_books; i++) {
        books[i].name[PL_NAME - 1] = 0;
        books[i].got &= (AW_N >= 32) ? 0xffffffffu : ((1u << AW_N) - 1u);
        for (k = 0; k < AW_N; k++) {
            int *v = &books[i].prog[k];
            if (*v < 0)
                *v = 0;
            if (TAB[k].kind == AW_K_COUNT && *v > TAB[k].goal)
                *v = TAB[k].goal;
            if (TAB[k].kind == AW_K_BITS)
                *v &= 0x7fffffff;
            /* And the other direction: a row whose progress is at the goal but
               whose `got' is 0 -- which is what a file written by a build with
               a LOWER threshold looks like -- is held as earned, silently. The
               toast is for the frame it happens in, and this did not happen in
               a frame. */
            if (reached(&books[i], k))
                books[i].got |= 1u << k;
        }
    }
}

void award_format(char *out, int n)
{
    int k = 0, i, j;

#define P(...) do { \
        if (k >= 0 && k < n) { \
            int r = snprintf(out + k, (size_t)(n - k), __VA_ARGS__); \
            k = (r < 0 || r >= n - k) ? n : k + r; \
        } \
    } while (0)

    P("# RC Cars, PS Vita port -- the award book behind `Awards'.\n");
    P("# The awards are this PORT's, not the game's: RCCars.exe has none.\n");
    P("# One `book' per player, then one line per award:\n");
    P("#   aw <key> <progress> <earned>\n");
    P("# `progress' is a tally, or a BITMASK for the by-track and by-car ones.\n");
    P("# Delete a book to forget it; delete a line to forget one award.\n");
    P("version %d\n", AWARDS_VERSION);
    for (i = 0; i < n_books; i++) {
        P("book %s\n", books[i].name);
        for (j = 0; j < AW_N; j++) {
            if (!books[i].prog[j] && !(books[i].got & (1u << j)))
                continue;       /* nothing to say about this one */
            P("aw %-10s %8d %d   # %s\n", TAB[j].key, books[i].prog[j],
              (books[i].got & (1u << j)) ? 1 : 0, TAB[j].name);
        }
    }
#undef P

    if (n > 0)
        out[n - 1] = 0;
}

/* --------------------------------------------------------------------- io */

const char *award_path(void) { return file_path; }

void award_set_path(const char *path)
{
    file_path = path ? path : AWARDS_FILE;
}

int award_load(void)
{
    char text[AWARDS_TEXT_MAX];
    FILE *f;
    size_t got;
    char keep[PL_NAME];

    /* WHOEVER WAS SELECTED STAYS SELECTED across a reload, by NAME -- the
       indices are about to be rebuilt and the app has a profile up. */
    snprintf(keep, sizeof keep, "%s", award_player());
    award_reset();
    f = fopen(file_path, "rb");
    if (!f) {
        rlog("[rccars] awards: no %s -- everybody starts at zero\n", file_path);
        if (*keep) award_select(keep);
        return 0;
    }
    got = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[got] = 0;

    if (!award_parse(text)) {
        rlog("[rccars] awards: %s is from a newer version -- ignored\n",
             file_path);
        award_reset();
        if (*keep) award_select(keep);
        return 0;
    }
    clamp_books();
    dirty = 0;                  /* this IS what is on the card */
    rlog("[rccars] awards: %s -- %d book(s)\n", file_path, n_books);
    if (*keep) award_select(keep);
    return 1;
}

static int write_file(const char *path, const char *text, size_t len)
{
    FILE *f;
    size_t put;

    remove(path);
    f = fopen(path, "wb");
    if (!f)
        return 0;
    put = fwrite(text, 1, len, f);
    if (fclose(f) != 0 || put != len) {
        remove(path);
        return 0;
    }
    return 1;
}

int award_save(void)
{
    char text[AWARDS_TEXT_MAX];
    char tmp[160];
    size_t len;
    int ok;

    clamp_books();
    award_format(text, sizeof text);
    len = strlen(text);

#ifdef __vita__
    sceIoMkdir(AWARDS_DIR, 0777);
#endif

    /* The suffix-and-rename settings.c uses, and for its reason: a machine that
       loses power mid-write loses THIS save rather than every award before it. */
    if ((int)strlen(file_path) + (int)sizeof(AWARDS_TMP_SUFFIX)
        <= (int)sizeof(tmp)) {
        snprintf(tmp, sizeof tmp, "%s%s", file_path, AWARDS_TMP_SUFFIX);
        ok = write_file(tmp, text, len);
        if (ok) {
            remove(file_path);
            if (rename(tmp, file_path) != 0) {
                remove(tmp);
                ok = 0;
            }
        }
    } else {
        ok = write_file(file_path, text, len);
    }

    if (ok)
        dirty = 0;
    else
        rlog("[rccars] awards: could not write %s\n", file_path);
    return ok;
}

int award_save_if_changed(void)
{
    if (!dirty)
        return 0;
    return award_save();
}
