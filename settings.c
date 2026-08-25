/*
 * settings.c -- see settings.h.
 */

#include "settings.h"
#include "rlog.h"
#include "tracks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#define SETTINGS_DIR  "ux0:data/rccars"
#define SETTINGS_FILE "ux0:data/rccars/settings.txt"
#else
#define SETTINGS_FILE "rccars_settings.txt"
#endif

/* SETTINGS_TMP_SUFFIX is in the header. The window where neither file exists is
   one rename; the window a plain remove-then-write leaves open is the whole
   write, and rlog.c's note explains why the remove cannot be skipped. */

/* Room for the file with its comments, several times over. The formatter is
   bounded by construction -- one line per key, MENU_N_CARS numbers on the
   longest of them -- so this is not a limit anything can grow into by accident,
   and settings_format truncates rather than overruns if it ever did. */
#define SETTINGS_TEXT_MAX 1024

static const char *file_path = SETTINGS_FILE;

/* What is on the card, as far as this module knows: filled by a successful load
   and by every successful save, and compared against by save_if_changed. `have`
   is 0 before either, so the first save always writes -- a card with no file on
   it must not be mistaken for a card that already agrees. */
static settings_t saved;
static int have_saved;

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void settings_set_path(const char *path)
{
    file_path = path ? path : SETTINGS_FILE;
    have_saved = 0;
}

const char *settings_path(void)
{
    return file_path;
}

void settings_from_menu(const menu_t *m, settings_t *s)
{
    int i;

    memset(s, 0, sizeof(*s));
    s->track = m->track;
    s->car = m->car;
    for (i = 0; i < MENU_N_CARS; i++)
        s->skin[i] = m->skin[i];
    s->tires = m->tires;
    s->reso = m->reso;
    s->boost = m->boost;
    s->vol_sfx = m->vol_sfx;
    s->vol_music = m->vol_music;
    s->tex_quality = m->tex_quality;
    s->tex_swap_rb = m->tex_swap_rb;
    s->car_light = m->car_light;
}

void settings_to_menu(const settings_t *s, menu_t *m)
{
    int i;

    m->track = s->track;
    m->car = s->car;
    for (i = 0; i < MENU_N_CARS; i++)
        m->skin[i] = s->skin[i];
    m->tires = s->tires;
    m->reso = s->reso;
    m->boost = s->boost;
    m->vol_sfx = s->vol_sfx;
    m->vol_music = s->vol_music;
    m->tex_quality = s->tex_quality;
    m->tex_swap_rb = s->tex_swap_rb;
    m->car_light = s->car_light;
    /* DELIBERATELY NOT TOUCHED: req_track, req_car, req_reload, open, row, cue
       and skins. The caller does the first load itself off m->track and m->car,
       and raising a request here would load the track twice; `skins` is counted
       out of the packed car by load_car, which also clamps skin[] into it. */
}

void settings_clamp(settings_t *s)
{
    int i;

    s->track = clampi(s->track, 0, N_TRACKS - 1);
    s->car = clampi(s->car, 0, MENU_N_CARS - 1);
    /* Against MENU_SKINS, the most any car CAN have. What the loaded one really
       has is a property of its .vsc, so load_car clamps again -- this only keeps
       the index inside the array carparts indexes with it. */
    for (i = 0; i < MENU_N_CARS; i++)
        s->skin[i] = clampi(s->skin[i], 0, MENU_SKINS - 1);
    s->tires = clampi(s->tires, 0, 3);
    s->reso = clampi(s->reso, 0, 3);
    s->boost = clampi(s->boost, 0, 3);
    s->vol_sfx = clampi(s->vol_sfx, 0, MENU_VOL_STEPS);
    s->vol_music = clampi(s->vol_music, 0, MENU_VOL_STEPS);
    s->tex_quality = clampi(s->tex_quality, 0, MENU_TEXQUAL_LEVELS - 1);
    s->tex_swap_rb = !!s->tex_swap_rb;
    s->car_light = !!s->car_light;
}

/*
 * One `key value...` line. Returns the rest of the line past the key, or NULL if
 * this is not that key. The space is required: `car` must not match `car_light`,
 * which is exactly the bug a bare strncmp would ship.
 */
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

/* strtol over as many integers as the line holds, up to `n`. Returns how many it
   read, so a `skin` line with two numbers on it leaves the third car's paint at
   the default instead of zeroing it. */
static int read_ints(const char *p, int *out, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p)
            break;
        out[i] = (int)v;
        p = end;
    }
    return i;
}

static int read_int(const char *p, int *out)
{
    return read_ints(p, out, 1) == 1;
}

int settings_parse(const char *text, settings_t *s)
{
    const char *p = text;
    char line[160];

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const char *v;
        size_t i;

        if (len >= sizeof(line))
            len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = 0;
        p = nl ? nl + 1 : p + strlen(p);

        /* A comment runs to the end of the line, so a value can be annotated in
           place -- which is how the written file documents its own ranges. With
           every value an integer, strtol would stop at the `#` anyway and no
           input can currently tell the difference (settings_test says so out
           loud); this is what makes it true by construction, and what the first
           non-integer key would need. `\r` for the same reason, so a file edited
           on Windows and copied to the card reads the same. */
        for (i = 0; i < len; i++)
            if (line[i] == '#' || line[i] == '\r') {
                line[i] = 0;
                break;
            }
        {
            char *q = line;
            while (*q == ' ' || *q == '\t')
                q++;
            if (!*q)
                continue;

            /* A version from the future is not read at all: the keys below may
               be the same words for different things, and a plausible-looking
               wrong value is worse than a default. */
            if ((v = match(q, "version")) != NULL) {
                int ver = 0;
                if (read_int(v, &ver) && ver > SETTINGS_VERSION)
                    return 0;
                continue;
            }
            /* THE KEY IS THE FIELD NAME, in every case, so there is no mapping
               table to get out of step with the struct. */
            if ((v = match(q, "track")) != NULL)       { read_int(v, &s->track); continue; }
            if ((v = match(q, "car_light")) != NULL)   { read_int(v, &s->car_light); continue; }
            if ((v = match(q, "car")) != NULL)         { read_int(v, &s->car); continue; }
            if ((v = match(q, "skin")) != NULL)        { read_ints(v, s->skin, MENU_N_CARS); continue; }
            if ((v = match(q, "tires")) != NULL)       { read_int(v, &s->tires); continue; }
            if ((v = match(q, "reso")) != NULL)        { read_int(v, &s->reso); continue; }
            if ((v = match(q, "boost")) != NULL)       { read_int(v, &s->boost); continue; }
            if ((v = match(q, "vol_sfx")) != NULL)     { read_int(v, &s->vol_sfx); continue; }
            if ((v = match(q, "vol_music")) != NULL)   { read_int(v, &s->vol_music); continue; }
            if ((v = match(q, "tex_quality")) != NULL) { read_int(v, &s->tex_quality); continue; }
            if ((v = match(q, "tex_swap_rb")) != NULL) { read_int(v, &s->tex_swap_rb); continue; }
            /* Anything else is a key from another version of this file, or a
               typo. Skipped in silence -- it costs that line and nothing else. */
        }
    }
    return 1;
}

void settings_format(const settings_t *s, char *out, int n)
{
    int k = 0;
    int i;

    /* Every snprintf goes through this, so one truncation stops the rest rather
       than each call writing at a negative offset. */
#define P(...) do { \
        if (k >= 0 && k < n) { \
            int r = snprintf(out + k, (size_t)(n - k), __VA_ARGS__); \
            k = (r < 0 || r >= n - k) ? n : k + r; \
        } \
    } while (0)

    P("# RC Cars, PS Vita port -- the START menu's choices.\n");
    P("# Written when the menu closes. `key value`, one per line; `#` starts a\n");
    P("# comment. A missing key keeps the built-in default, an unknown one is\n");
    P("# ignored, and a value out of range is clamped rather than obeyed.\n");
    P("version %d\n", SETTINGS_VERSION);
    P("track %d           # 0..%d, the order the menu lists them in\n",
      s->track, N_TRACKS - 1);
    P("car %d             # 0 Overkill, 1 Buggy, 2 Hummer\n", s->car);
    P("skin");
    for (i = 0; i < MENU_N_CARS; i++)
        P(" %d", s->skin[i]);
    P("        # one per car, in that order; 0..%d\n", MENU_SKINS - 1);
    P("tires %d           # 0..3, grip\n", s->tires);
    P("reso %d            # 0..3, top speed and acceleration\n", s->reso);
    P("boost %d           # 0..3, exhaust and boost meter\n", s->boost);
    P("vol_sfx %d        # 0..%d\n", s->vol_sfx, MENU_VOL_STEPS);
    P("vol_music %d       # 0..%d\n", s->vol_music, MENU_VOL_STEPS);
    P("tex_quality %d     # 0 high, 1 medium, 2 low\n", s->tex_quality);
    P("tex_swap_rb %d     # 0 real hardware, 1 Vita3K\n", s->tex_swap_rb);
    P("car_light %d       # 1 sun + shade on the car, 0 flat\n", s->car_light);
#undef P

    if (n > 0)
        out[n - 1] = 0;
}

int settings_load(menu_t *m)
{
    char text[SETTINGS_TEXT_MAX];
    settings_t s;
    FILE *f;
    size_t got;

    f = fopen(file_path, "rb");
    if (!f) {
        rlog("[rccars] settings: no %s -- using defaults\n", file_path);
        return 0;
    }
    got = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[got] = 0;

    /* Start from what menu_init left, so a file missing a key -- an older one,
       or a hand-written one with two lines in it -- keeps that default rather
       than a zero. */
    settings_from_menu(m, &s);
    if (!settings_parse(text, &s)) {
        rlog("[rccars] settings: %s is from a newer version -- ignored\n",
             file_path);
        return 0;
    }
    settings_clamp(&s);
    settings_to_menu(&s, m);

    /* This IS what is on the card now, so a menu closed without a change writes
       nothing. */
    saved = s;
    have_saved = 1;

    rlog("[rccars] settings: %s -- track %d car %d skins %d/%d/%d "
         "tires %d reso %d boost %d vol %d/%d texq %d swap_rb %d light %d\n",
         file_path, s.track, s.car, s.skin[0], s.skin[1], s.skin[2],
         s.tires, s.reso, s.boost, s.vol_sfx, s.vol_music,
         s.tex_quality, s.tex_swap_rb, s.car_light);
    return 1;
}

static int write_file(const char *path, const char *text, size_t len)
{
    FILE *f;
    size_t put;

    /* remove() first for the reason rlog_init gives: "w" does not reliably
       truncate here, and a settings file that is half this save and half a
       longer earlier one parses as neither. */
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

int settings_save(const menu_t *m)
{
    char text[SETTINGS_TEXT_MAX];
    char tmp[160];
    settings_t s;
    size_t len;
    int ok;

    settings_from_menu(m, &s);
    /* Clamped on the way OUT as well as in. Nothing in the menu can leave a row
       out of range, but this is the file every later launch trusts, and the
       cheapest place to be sure of it is where it is written. */
    settings_clamp(&s);
    settings_format(&s, text, sizeof(text));
    len = strlen(text);

#ifdef __vita__
    /* ux0:data exists on every machine, the app's own subdirectory may not, and
       mkdir over an existing one is an error worth ignoring. rlog_init has
       normally done this already; a save must not depend on that. */
    sceIoMkdir(SETTINGS_DIR, 0777);
#endif

    if ((int)strlen(file_path) + (int)sizeof(SETTINGS_TMP_SUFFIX)
        <= (int)sizeof(tmp)) {
        snprintf(tmp, sizeof(tmp), "%s%s", file_path, SETTINGS_TMP_SUFFIX);
        ok = write_file(tmp, text, len);
        if (ok) {
            /* The destination has to go first: sceIoRename fails outright when
               it exists, where POSIX rename would replace it. */
            remove(file_path);
#ifdef __vita__
            ok = sceIoRename(tmp, file_path) >= 0;
#else
            ok = rename(tmp, file_path) == 0;
#endif
            if (!ok) {
                /* Rather than lose the save to a card that will not rename,
                   write the real file directly -- the risk that buys is a torn
                   file on a power cut during one 300-byte write. */
                rlog("[rccars] settings: rename failed -- writing %s directly\n",
                     file_path);
                remove(tmp);
                ok = write_file(file_path, text, len);
            }
        }
    } else {
        ok = write_file(file_path, text, len);
    }

    if (!ok) {
        rlog("[rccars] settings: CANNOT WRITE %s\n", file_path);
        return 0;
    }
    saved = s;
    have_saved = 1;
    rlog("[rccars] settings: saved to %s (%d bytes)\n", file_path, (int)len);
    return 1;
}

int settings_save_if_changed(const menu_t *m)
{
    settings_t s;

    settings_from_menu(m, &s);
    settings_clamp(&s);
    if (have_saved && memcmp(&s, &saved, sizeof(s)) == 0)
        return 0;
    return settings_save(m);
}
