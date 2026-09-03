/*
 * records.c -- see records.h.
 */

#include "records.h"
#include "rlog.h"
#include "tracks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#define RECORDS_DIR  "ux0:data/rccars"
#define RECORDS_FILE "ux0:data/rccars/records.txt"
#else
#define RECORDS_FILE "rccars_records.txt"
#endif

/* The same suffix-and-rename settings.c uses, and for the same reason. */
#define RECORDS_TMP_SUFFIX ".new"

const int REC_LAPS[REC_N_LAPS] = { 3, 5, 7 };

static rec_track book[N_TRACKS];
static const char *file_path = RECORDS_FILE;
static int dirty;

/* ------------------------------------------------------------------ query */

float records_value(const rec_row *r, int stat)
{
    if (!r)
        return 0.f;
    if (stat == REC_STAT_BEST_LAP)
        return r->best_lap;
    if (stat >= REC_STAT_3_LAPS && stat < REC_STAT_3_LAPS + REC_N_LAPS)
        return r->total[stat - REC_STAT_3_LAPS];
    return 0.f;
}

void records_reset(void)
{
    memset(book, 0, sizeof book);
    dirty = 0;
}

const rec_track *records_track(int track)
{
    static const rec_track empty;
    if (track < 0 || track >= N_TRACKS)
        return &empty;
    return &book[track];
}

int records_sorted(int track, int stat, const rec_row *out[], int max)
{
    const rec_track *t = records_track(track);
    int n = 0, i, j;

    if (!out || max <= 0)
        return 0;
    /* INSERTION SORT, and it is not laziness: eight rows at most, and it is
       STABLE, which is what keeps the rows with no time for this stat in the
       order they were first seen instead of shuffling every time the enum
       moves. A row with no time sorts after every row that has one. */
    for (i = 0; i < t->n && n < max; i++) {
        const float v = records_value(&t->row[i], stat);
        for (j = n; j > 0; j--) {
            const float u = records_value(out[j - 1], stat);
            const int u_has = u > 0.f, v_has = v > 0.f;
            if (u_has && (!v_has || u <= v))
                break;
            if (!u_has && !v_has)
                break;
            out[j] = out[j - 1];
        }
        out[j] = &t->row[i];
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ merge */

static rec_row *find_row(int track, const char *name)
{
    rec_track *t = &book[track];
    int i;

    for (i = 0; i < t->n; i++)
        if (strncmp(t->row[i].name, name, REC_NAME - 1) == 0)
            return &t->row[i];
    if (t->n >= REC_MAX_ROWS)
        return NULL;
    {
        rec_row *r = &t->row[t->n++];
        memset(r, 0, sizeof *r);
        snprintf(r->name, sizeof r->name, "%s", name);
        r->car = -1;
        return r;
    }
}

void records_note(int track, const char *name, int car,
                  float best_lap, int laps, float total)
{
    rec_row *r;
    int k;

    if (track < 0 || track >= N_TRACKS || !name || !*name)
        return;
    r = find_row(track, name);
    if (!r)
        return;

    if (best_lap > 0.f && (r->best_lap <= 0.f || best_lap < r->best_lap)) {
        r->best_lap = best_lap;
        /* THE CAR THE COLUMN DESCRIBES is the one that set the standing best
           lap, not the one driven most recently -- the row reads
           "<name> <time> <car>" and the three have to be one result. */
        if (car >= 0)
            r->car = car;
        dirty = 1;
    } else if (r->car < 0 && car >= 0) {
        r->car = car;
        dirty = 1;
    }

    if (total > 0.f) {
        for (k = 0; k < REC_N_LAPS; k++) {
            if (REC_LAPS[k] != laps)
                continue;
            if (r->total[k] <= 0.f || total < r->total[k]) {
                r->total[k] = total;
                dirty = 1;
            }
            break;
        }
        /* A race over some other lap count files no total, deliberately: a
           5-lap time in the "3 laps" column would be a wrong record rather
           than a missing one. The best lap it turned is still kept above. */
    }
}

int records_dirty(void)
{
    return dirty;
}

/* ------------------------------------------------------------------- text */

static void clamp_book(void)
{
    int t, i, k;

    for (t = 0; t < N_TRACKS; t++) {
        rec_track *b = &book[t];
        if (b->n < 0) b->n = 0;
        if (b->n > REC_MAX_ROWS) b->n = REC_MAX_ROWS;
        for (i = 0; i < b->n; i++) {
            rec_row *r = &b->row[i];
            r->name[REC_NAME - 1] = 0;
            if (r->car < -1 || r->car > 2)
                r->car = -1;
            /* A negative or absurd time is a hand-edited file or a torn one.
               An hour is longer than any lap this game can produce and shorter
               than anything a float mis-read lands on. */
            if (!(r->best_lap > 0.f) || r->best_lap > 3600.f)
                r->best_lap = 0.f;
            for (k = 0; k < REC_N_LAPS; k++)
                if (!(r->total[k] > 0.f) || r->total[k] > 3600.f)
                    r->total[k] = 0.f;
        }
    }
}

/* settings.c's own `match`, kept separate rather than shared: the two files are
   independent formats and a helper reached across would make a change to one
   able to break the other. */
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

/*
 * One `row <track> <car> <best> <t3> <t5> <t7> <name...>` line. The NAME IS
 * LAST and runs to the end of the line, because a driver's name can hold a
 * space and a quoted field would be a second escaping rule to get wrong.
 */
static void parse_row(const char *p)
{
    int track = -1, car = -1, k;
    float best = 0.f, tot[REC_N_LAPS];
    rec_row *r;
    char *end;
    const char *name;

    track = (int)strtol(p, &end, 10);
    if (end == p) return;
    p = end;
    car = (int)strtol(p, &end, 10);
    if (end == p) return;
    p = end;
    best = strtof(p, &end);
    if (end == p) return;
    p = end;
    for (k = 0; k < REC_N_LAPS; k++) {
        tot[k] = strtof(p, &end);
        if (end == p) return;
        p = end;
    }
    while (*p == ' ' || *p == '\t')
        p++;
    name = p;
    if (!*name)
        return;
    if (track < 0 || track >= N_TRACKS)
        return;

    r = find_row(track, name);
    if (!r)
        return;
    r->car = car;
    r->best_lap = best;
    for (k = 0; k < REC_N_LAPS; k++)
        r->total[k] = tot[k];
}

int records_parse(const char *text)
{
    const char *p = text;
    char line[192];

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

        /* `#` starts a comment and `\r` ends the line, as in settings.txt. A
           NAME CANNOT CONTAIN A `#` because of this, which is a real limit and
           is why the writer does not put a trailing comment on a row line. */
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
            if (ver > RECORDS_VERSION)
                return 0;
            continue;
        }
        if ((v = match(q, "row")) != NULL) {
            parse_row(v);
            continue;
        }
        /* Anything else is a key from another version, or a typo. */
    }
    return 1;
}

void records_format(char *out, int n)
{
    int k = 0, t, i, j;

#define P(...) do { \
        if (k >= 0 && k < n) { \
            int r = snprintf(out + k, (size_t)(n - k), __VA_ARGS__); \
            k = (r < 0 || r >= n - k) ? n : k + r; \
        } \
    } while (0)

    P("# RC Cars, PS Vita port -- the track record book behind `Track stats'.\n");
    P("# Written when a race finishes. One line per racer per track:\n");
    P("#   row <track> <car> <best lap> <3 laps> <5 laps> <7 laps> <name>\n");
    P("# Times are seconds; 0 means no record. The name runs to the end of the\n");
    P("# line, so it may hold spaces but not a `#'. Delete a line to forget it.\n");
    P("version %d\n", RECORDS_VERSION);
    for (t = 0; t < N_TRACKS; t++) {
        if (book[t].n <= 0)
            continue;
        P("# %s\n", TRACKS[t].base);
        for (i = 0; i < book[t].n; i++) {
            /* NOT `r': the P macro declares one of its own. */
            const rec_row *w = &book[t].row[i];
            P("row %d %d %.3f", t, w->car, (double)w->best_lap);
            for (j = 0; j < REC_N_LAPS; j++)
                P(" %.3f", (double)w->total[j]);
            P(" %s\n", w->name);
        }
    }
#undef P

    if (n > 0)
        out[n - 1] = 0;
}

/* --------------------------------------------------------------------- io */

const char *records_path(void)
{
    return file_path;
}

void records_set_path(const char *path)
{
    file_path = path ? path : RECORDS_FILE;
}

int records_load(void)
{
    char text[RECORDS_TEXT_MAX];
    FILE *f;
    size_t got;
    int t, rows = 0;

    records_reset();
    f = fopen(file_path, "rb");
    if (!f) {
        rlog("[rccars] records: no %s -- the stats page starts empty\n",
             file_path);
        return 0;
    }
    got = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[got] = 0;

    if (!records_parse(text)) {
        rlog("[rccars] records: %s is from a newer version -- ignored\n",
             file_path);
        records_reset();
        return 0;
    }
    clamp_book();
    dirty = 0;                  /* this IS what is on the card */
    for (t = 0; t < N_TRACKS; t++)
        rows += book[t].n;
    rlog("[rccars] records: %s -- %d row(s)\n", file_path, rows);
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

int records_save(void)
{
    char text[RECORDS_TEXT_MAX];
    char tmp[160];
    size_t len;
    int ok;

    clamp_book();
    records_format(text, sizeof text);
    len = strlen(text);

#ifdef __vita__
    sceIoMkdir(RECORDS_DIR, 0777);
#endif

    if ((int)strlen(file_path) + (int)sizeof(RECORDS_TMP_SUFFIX)
        <= (int)sizeof(tmp)) {
        snprintf(tmp, sizeof tmp, "%s%s", file_path, RECORDS_TMP_SUFFIX);
        ok = write_file(tmp, text, len);
        if (ok) {
            remove(file_path);
#ifdef __vita__
            ok = sceIoRename(tmp, file_path) >= 0;
#else
            ok = rename(tmp, file_path) == 0;
#endif
            if (!ok) {
                rlog("[rccars] records: rename failed -- writing %s directly\n",
                     file_path);
                remove(tmp);
                ok = write_file(file_path, text, len);
            }
        }
    } else {
        ok = write_file(file_path, text, len);
    }

    if (!ok) {
        rlog("[rccars] records: CANNOT WRITE %s\n", file_path);
        return 0;
    }
    dirty = 0;
    rlog("[rccars] records: saved to %s (%d bytes)\n", file_path, (int)len);
    return 1;
}

int records_save_if_changed(void)
{
    if (!dirty)
        return 0;
    return records_save();
}
