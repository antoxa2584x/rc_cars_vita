/*
 * player.c -- see player.h.
 */

#include "player.h"
#include "champ_data.h"   /* DefaultCash and IsEnabled, which is what a
                                fresh profile is built out of */
#include "rlog.h"
#include "hud_data.h"
#include "str_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#define PL_DIR   "ux0:data/rccars/Players"
#define PL_ROOT  "ux0:data/rccars"
#define PL_FACES "app0:assets/faces.bin"
#else
#include <dirent.h>
#define PL_DIR   "rccars_players"
#define PL_ROOT  "rccars_players"
#define PL_FACES "assets/faces.bin"
#endif

/* The nine, in this port's own order. `pack_faces.py` writes assets/faces.bin
   off exactly this list and stores each name beside its pixels, so a file built
   from a different list is DETECTED rather than silently mis-indexed. */
const char *const PL_FACE_NAME[PL_N_FACES] = {
    "Face1", "Face2", "BabyShark", "Daisy", "Dakilla",
    "Doc", "Johny", "MCJocker", "MaxXMad"
};

static const char *dir_path   = PL_DIR;
static const char *faces_path = PL_FACES;

static player_t roster[PL_MAX];
static int      n_roster;
static int      cur = -1;

/* WHO THE PLAYER RACED AS LAST TIME, and it is a FILE because nothing else in
 * the roster remembers. `player_scan' walks the directory and took the first
 * `.scp' it found, so a launch always came up as whoever the card happened to
 * list first -- pick a profile, quit, come back and you are somebody else, with
 * their cash and their championship.
 *
 * WHY NOT settings.txt. That file is "the persisted subset of menu_t, and
 * nothing else" (settings.h), all int so the struct can be memcmp'd against the
 * last thing written; the current profile is neither in `menu_t` nor an int.
 * The roster is this file's business, so the marker lives beside it -- one line
 * naming the profile's own file base, in the directory the profiles are in. The
 * scan already ignores everything that is not a `.scp', and the game's own
 * Players/ carries a `descript.txt' next to them, so a stray text file there is
 * in keeping and a Players/ copied to a PC install is unharmed.
 *
 * WRITTEN ON THE SAME EVENT EVERY OTHER SAVE IN THIS APP IS. Selecting a row
 * only marks it: `player_save_cur_if_dirty' is called from eight places that
 * already know when it is safe to touch the card, and walking a roster of
 * twenty with a write per keypress is exactly the memory-card habit audio.md
 * forbids. */
#define PL_LAST_FILE "last.txt"
static char sel_file[PL_NAME + 8];   /* the base name last written, or "" */
static int  sel_dirty;

/* Every assignment to `cur' goes through this, so the marker cannot fall out of
   step with the selection -- create and remove move it too. */
static void set_cur(int i)
{
    const char *f;
    cur = i;
    f = (i >= 0 && i < n_roster) ? roster[i].file : "";
    if (strncmp(sel_file, f, sizeof sel_file - 1) != 0) {
        snprintf(sel_file, sizeof sel_file, "%s", f);
        sel_dirty = 1;
    }
}

/* One FNV-1a per shipped portrait, so an incoming one can be named without the
   1.1 MB of pixels being resident. Filled by player_faces_init; `n_faces` is 0
   until then and every match then answers -1, which writes no portrait chunk. */
static unsigned int face_hash[PL_N_FACES];
static int          n_faces;

/* ------------------------------------------------------------- the orders */

int pl_track_slot(int port_track)
{
    if (port_track < 0 || port_track >= PL_N_TRACKS)
        return 0;
    return MAP_TRACKMAP[port_track] - 1;
}

int pl_track_port(int engine_slot)
{
    int i;
    for (i = 0; i < PL_N_TRACKS; i++)
        if (MAP_TRACKMAP[i] - 1 == engine_slot)
            return i;
    return 0;
}

/* ---------------------------------------------------------- little things */

static int ci_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb)
            return 0;
    }
    return *a == *b;
}

static unsigned int rd32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static float rdf(const unsigned char *p)
{
    unsigned int u = rd32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static void wr32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void wrf(unsigned char *p, float f)
{
    unsigned int u;
    memcpy(&u, &f, 4);
    wr32(p, u);
}

/* --------------------------------------------------------------- the read */

/* The record at `off`. Returns 0 when it does not fit or its `next` does not
   move forward -- a malformed file stops the walk rather than looping. */
static int chunk_at(const unsigned char *b, int len, int off,
                    int *tag, int *type, int *next)
{
    unsigned int nx;
    if (off < 0 || off + 6 > len)
        return 0;
    nx = rd32(b + off + 2);
    if (nx <= (unsigned int)(off + 5) || nx > (unsigned int)len)
        return 0;
    *tag = b[off];
    *type = b[off + 1];
    *next = (int)nx;
    return 1;
}

static void read_str(const unsigned char *b, int off, int end,
                     char *out, int n)
{
    int i = 0;
    while (off + i < end && i < n - 1 && b[off + i])
        out[i] = (char)b[off + i], i++;
    out[i] = 0;
}

static void read_race(const unsigned char *b, int len, int off, int end,
                      pl_race *r)
{
    int tag, type, next, k;
    while (off < end && chunk_at(b, len, off, &tag, &type, &next)) {
        const unsigned char *p = b + off + 6;
        const int plen = next - off - 6;
        switch (tag) {
        case 0x72: if (plen >= 4) r->time = rdf(p); break;
        case 0x73: if (plen >= 4) r->a = (int)rd32(p); break;
        case 0x74: if (plen >= 4) r->b = (int)rd32(p); break;
        case 0x75: if (plen >= 4) r->c = (int)rd32(p); break;
        case 0x76: if (plen >= 4) r->d = (int)rd32(p); break;
        case 0x77:
            /* u32 count then count u16. The engine refuses a count that is not
               4 and skips the chunk; so does this. */
            if (plen == 12 && rd32(p) == 4)
                for (k = 0; k < 4; k++)
                    r->e[k] = (unsigned short)(p[4 + k * 2]
                                               | (p[5 + k * 2] << 8));
            break;
        case 0x78:
            if (plen == 12 && rd32(p) == 4)
                for (k = 0; k < 4; k++)
                    r->g[k] = (unsigned short)(p[4 + k * 2]
                                               | (p[5 + k * 2] << 8));
            break;
        default:
            break;
        }
        off = next;
    }
}

static void read_car(const unsigned char *b, int len, int off, int end,
                     pl_car *c)
{
    int tag, type, next;
    while (off < end && chunk_at(b, len, off, &tag, &type, &next)) {
        const unsigned char *p = b + off + 6;
        const int plen = next - off - 6;
        if (tag == 0x31)
            c->enabled = 1;
        else if (tag >= 0x32 && tag <= 0x35 && plen >= 4)
            c->up[tag - 0x32] = (int)rd32(p);
        off = next;
    }
}

static void read_track(const unsigned char *b, int len, int off, int end,
                       pl_track *t)
{
    int tag, type, next;
    while (off < end && chunk_at(b, len, off, &tag, &type, &next)) {
        const unsigned char *p = b + off + 6;
        const int plen = next - off - 6;
        switch (tag) {
        case 0x70: read_race(b, len, off + 6, next, &t->rec[PL_RACE_HEAD]);
                   break;
        case 0x79: read_race(b, len, off + 6, next, &t->rec[PL_RACE_TAIL]);
                   break;
        case 0x71: {
            const int i = plen >= 4 ? (int)rd32(p) : -1;
            if (i >= 0 && i < PL_N_RACE - 2)
                read_race(b, len, off + 10, next, &t->rec[PL_RACE_LIST + i]);
            break;
        }
        case 0x44: if (plen >= 4) t->f44 = rdf(p); break;
        case 0x45: if (plen >= 4) t->i45 = (int)rd32(p); break;
        case 0x46: if (plen >= 4) t->i46 = (int)rd32(p); break;
        case 0x47: if (plen >= 4) t->f47 = rdf(p); break;
        default: break;
        }
        off = next;
    }
}

int player_parse(const unsigned char *buf, int len, player_t *p)
{
    int tag, type, next, off, end;

    if (!buf || !p || len < 10)
        return 0;
    if (!chunk_at(buf, len, 0, &tag, &type, &next))
        return 0;
    if (tag != 0x20 || type != 0x50)
        return 0;

    /* Everything a chunk does not carry keeps the blank profile's value, which
       is what FUN_004e7740 does: it resets each car and each track before
       reading into it. */
    player_init_new(p, "", -1);
    p->name[0] = 0;
    p->nick[0] = 0;

    end = next;
    off = 10;                   /* past the root's own float version */
    while (off < end && chunk_at(buf, len, off, &tag, &type, &next)) {
        const unsigned char *q = buf + off + 6;
        const int plen = next - off - 6;
        switch (tag) {
        case 0x21: read_str(buf, off + 6, next, p->name, PL_NAME); break;
        case 0x60: read_str(buf, off + 6, next, p->nick, PL_NAME); break;
        case 0x22:
            /* Kept as it came. See player.h -- ten of these forty dwords are
               the writing process's own pointers and this port has no business
               inventing values for them. */
            if (plen == 4 + 160 && rd32(q) == 160) {
                memcpy(p->fhdr, q + 4, 160);
                p->have_fhdr = 1;
            }
            break;
        case 0x23:
            /* The portrait. Named by its pixels; a face this build has no copy
               of stays -1 and is simply not written back. */
            if (plen >= 4 + PL_FACE_BYTES && rd32(q) == PL_FACE_BYTES)
                p->face = player_face_match(q + 4);
            break;
        case 0x24: if (plen >= 4) p->play_time = rdf(q); break;
        case 0x25: if (plen >= 4) p->cash = (int)rd32(q); break;
        case 0x26: if (plen >= 4) p->scores = (int)rd32(q); break;
        case 0x27: if (plen >= 4) p->f27 = rdf(q); break;
        case 0x50: if (plen >= 4) p->sel_track = (int)rd32(q); break;
        case 0x51: if (plen >= 4) p->sel_car = (int)rd32(q); break;
        case 0x52: if (plen >= 4) p->laps = (int)rd32(q); break;
        case 0x53: if (plen >= 4) p->skill = (int)rd32(q); break;
        case 0x61: if (plen >= 4) p->i61 = (int)rd32(q); break;
        case 0x30: {
            const int i = plen >= 4 ? (int)rd32(q) : -1;
            if (i >= 0 && i < PL_N_CARS) {
                memset(&p->car[i], 0, sizeof p->car[i]);
                read_car(buf, len, off + 10, next, &p->car[i]);
            }
            break;
        }
        case 0x40: {
            const int i = plen >= 4 ? (int)rd32(q) : -1;
            if (i >= 0 && i < PL_N_TRACKS)
                read_track(buf, len, off + 10, next, &p->track[i]);
            break;
        }
        default:
            break;      /* the engine warns and seeks past; so do we, quietly */
        }
        off = next;
    }
    return 1;
}

/* -------------------------------------------------------------- the write */

/* The writer is a cursor over `buf` with a stack of open records, which is what
   tiohbWriteChunk / tiohbUpdateChunk are: the header goes down with a hole where
   `next` belongs and the hole is filled with the cursor when the record closes.
   `over` latches an overrun so one test at the end covers every put. */
typedef struct { unsigned char *b; int n, max, over; } plw;

static void w_raw(plw *w, const void *p, int n)
{
    if (w->n + n > w->max) { w->over = 1; w->n += n; return; }
    memcpy(w->b + w->n, p, (size_t)n);
    w->n += n;
}

static void w_u32(plw *w, unsigned int v)
{
    unsigned char t[4];
    wr32(t, v);
    w_raw(w, t, 4);
}

static void w_f32(plw *w, float f)
{
    unsigned char t[4];
    wrf(t, f);
    w_raw(w, t, 4);
}

/* Opens a record and returns where its `next` hole is. `tag` and `type` go down
   as two bytes in that order, which is the u16 the engine writes. */
static int w_open(plw *w, int tag, int type)
{
    unsigned char h[2];
    int hole;
    h[0] = (unsigned char)tag;
    h[1] = (unsigned char)type;
    w_raw(w, h, 2);
    hole = w->n;
    w_u32(w, 0);
    return hole;
}

static void w_close(plw *w, int hole)
{
    if (!w->over && hole + 4 <= w->max)
        wr32(w->b + hole, (unsigned int)w->n);
}

static void w_dw(plw *w, int tag, int v)
{
    const int h = w_open(w, tag, 0x30);
    w_u32(w, (unsigned int)v);
    w_close(w, h);
}

static void w_fl(plw *w, int tag, float v)
{
    const int h = w_open(w, tag, 0x50);
    w_f32(w, v);
    w_close(w, h);
}

static void w_str(plw *w, int tag, const char *s)
{
    const int h = w_open(w, tag, 0x40);
    const int n = (int)strlen(s);
    w_raw(w, s, n);
    w_raw(w, "", 1);
    w_close(w, h);
}

static void w_blob(plw *w, int tag, const unsigned char *p, int n)
{
    const int h = w_open(w, tag, 0x14);
    w_u32(w, (unsigned int)n);
    w_raw(w, p, n);
    w_close(w, h);
}

static void w_race(plw *w, int tag, int type, const pl_race *r, int index)
{
    const int h = w_open(w, tag, type);
    int k, a;
    if (index >= 0)
        w_u32(w, (unsigned int)index);
    w_fl(w, 0x72, r->time);
    w_dw(w, 0x73, r->a);
    w_dw(w, 0x74, r->b);
    w_dw(w, 0x75, r->c);
    w_dw(w, 0x76, r->d);
    /* The two arrays. Type 0x24 writes an ELEMENT count, not a byte count --
       four u16 declared as 4, which is what the engine's reader checks. */
    for (a = 0; a < 2; a++) {
        const unsigned short *v = a ? r->g : r->e;
        const int t = a ? 0x78 : 0x77;
        const int hh = w_open(w, t, 0x24);
        w_u32(w, 4);
        for (k = 0; k < 4; k++) {
            unsigned char b2[2];
            b2[0] = (unsigned char)(v[k] & 0xff);
            b2[1] = (unsigned char)(v[k] >> 8);
            w_raw(w, b2, 2);
        }
        w_close(w, hh);
    }
    w_close(w, h);
}

/* THE 160-BYTE csiHEADER, as every shipped save carries it, with the ten
 * dwords that are RUNTIME POINTERS zeroed.
 *
 * They are pointers and not data, which is measurable rather than assumed: the
 * three saves in the PC install carry three different sets of them, all of them
 * addresses in whatever process wrote the file, and all three load. csiCreateImage
 * (0x4414a0) copies the 160 bytes into a fresh csiPICTURE and then allocates the
 * pixel planes itself at +0xa8, past the copy -- so nothing it does reads one.
 *
 * The rest is the format, and it is the format FacesSys ships: 128 x 256, four
 * bytes a pixel, one plane, ARGB with alpha at bits 24..31, red 16..23, green
 * 8..15 and blue 0..7.
 */
static const unsigned int PL_CSIHDR[40] = {
    0x4353494du, 0x00000000u, 0x000000a0u, 0x00000000u,
    0x00000080u, 0x00000100u, 0x00000004u, 0x00000001u,
    0x00000004u, 0x42475241u, 0x00000000u, 0x17101f18u,
    0x07000f08u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00410001u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00020000u,
    0x00000000u, 0x00000000u, 0x00800000u, 0x08200100u,
    0x00000002u, 0x00000020u, 0x08080808u, 0x00000000u
};

int player_format(const player_t *p, const unsigned char *face_pixels,
                  unsigned char *buf, int max)
{
    plw w;
    int root, i, k;

    if (!p || !buf || max < 16)
        return 0;
    w.b = buf; w.n = 0; w.max = max; w.over = 0;

    root = w_open(&w, 0x20, 0x50);
    w_f32(&w, 1.f);                     /* the version every shipped save has */
    w_str(&w, 0x21, p->name);
    if (face_pixels) {
        unsigned char hdr[160];
        if (p->have_fhdr) {
            memcpy(hdr, p->fhdr, 160);
        } else {
            for (i = 0; i < 40; i++)
                wr32(hdr + i * 4, PL_CSIHDR[i]);
        }
        w_blob(&w, 0x22, hdr, 160);
        w_blob(&w, 0x23, face_pixels, PL_FACE_BYTES);
    }
    w_fl(&w, 0x24, p->play_time);
    w_dw(&w, 0x25, p->cash);
    w_dw(&w, 0x26, p->scores);
    w_fl(&w, 0x27, p->f27);
    for (i = 0; i < PL_N_CARS; i++) {
        const int h = w_open(&w, 0x30, 0x32);
        w_u32(&w, (unsigned int)i);
        /* THE MARKER IS THE FLAG. A car that is not unlocked writes no children
           at all -- not a zero -- which is what the shipped saves have and what
           FUN_004e8700 reads: it clears the entry and only chunk 0x31 sets it. */
        if (p->car[i].enabled) {
            const int hh = w_open(&w, 0x31, 0x00);
            w_close(&w, hh);
            for (k = 0; k < 4; k++)
                w_dw(&w, 0x32 + k, p->car[i].up[k]);
        }
        w_close(&w, h);
    }
    for (i = 0; i < PL_N_TRACKS; i++) {
        const pl_track *t = &p->track[i];
        const int h = w_open(&w, 0x40, 0x32);
        w_u32(&w, (unsigned int)i);
        w_race(&w, 0x70, 0x02, &t->rec[PL_RACE_HEAD], -1);
        for (k = 0; k < PL_N_RACE - 2; k++)
            w_race(&w, 0x71, 0x32, &t->rec[PL_RACE_LIST + k], k);
        w_race(&w, 0x79, 0x02, &t->rec[PL_RACE_TAIL], -1);
        w_fl(&w, 0x44, t->f44);
        w_dw(&w, 0x45, t->i45);
        w_dw(&w, 0x46, t->i46);
        w_fl(&w, 0x47, t->f47);
        w_close(&w, h);
    }
    w_dw(&w, 0x50, p->sel_track);
    w_dw(&w, 0x51, p->sel_car);
    w_dw(&w, 0x52, p->laps);
    w_dw(&w, 0x53, p->skill);
    w_str(&w, 0x60, p->nick);
    w_dw(&w, 0x61, p->i61);
    w_close(&w, root);
    return w.over ? 0 : w.n;
}

int player_format_size(const player_t *p, int with_face)
{
    /* Every record is a fixed size but the two strings, so this counts rather
       than guesses -- and it is what the caller's buffer is sized off. */
    const int race = 6 + 10 + 10 + 10 + 10 + 10 + 18 + 18;   /* 92, no index */
    const int racei = race + 4;
    int n = 6 + 4;                                  /* root header + version */
    n += 6 + (int)strlen(p->name) + 1;
    if (with_face)
        n += 6 + 4 + 160 + 6 + 4 + PL_FACE_BYTES;
    n += 10 * 4;                                    /* 0x24..0x27 */
    {
        int i;
        for (i = 0; i < PL_N_CARS; i++) {
            n += 6 + 4;
            if (p->car[i].enabled)
                n += 6 + 10 * 4;
        }
    }
    n += PL_N_TRACKS * (6 + 4 + race * 2 + racei * (PL_N_RACE - 2) + 10 * 4);
    n += 10 * 4;                                    /* 0x50..0x53 */
    n += 6 + (int)strlen(p->nick) + 1;
    n += 10;                                        /* 0x61 */
    return n;
}

/* -------------------------------------------------------- a blank profile */

void player_init_new(player_t *p, const char *name, int face)
{
    int i, k;

    memset(p, 0, sizeof(*p));
    if (name) {
        strncpy(p->name, name, PL_NAME - 1);
        strncpy(p->nick, name, PL_NAME - 1);
    }
    p->face = face;
    /* championship.ini's Common/DefaultCash, and which cars IsEnabled says a
       fresh profile already owns -- the two things FUN_004e8320 reads out of
       that file when it builds one. Out of champ_data.h now rather than typed
       here: the Garage reads the same file for what a car costs, and two copies
       of one number is how the shop and the profile disagree. */
    p->cash = CHAMP_DEFAULT_CASH;
    p->scores = 0;
    for (i = 0; i < PL_N_CARS && i < CHAMP_N_CARS; i++)
        p->car[i].enabled = CHAMP_CAR_OWNED[i];
    p->laps = 3;                /* the only non-zero of the four; FUN_004e7c60 */
    for (i = 0; i < PL_N_TRACKS; i++) {
        pl_track *t = &p->track[i];
        for (k = 0; k < PL_N_RACE; k++)
            t->rec[k].time = PL_NO_TIME;
        t->i46 = PL_NO_PLACE;
    }
}

/* ---------------------------------------------------------- the portraits */

void player_faces_set_path(const char *path)
{
    faces_path = path ? path : PL_FACES;
    n_faces = 0;
}

static unsigned int fnv(const unsigned char *p, int n)
{
    unsigned int h = 2166136261u;
    int i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* assets/faces.bin: 'FACE', version, count, w, h, bpp, then count 16-byte names
   and then count images. `pack_faces.py` writes it; this refuses anything whose
   geometry is not the format's own. */
#define PL_FACES_HDR   24
#define PL_FACES_MAGIC 0x45434146u      /* 'FACE' little-endian */

static int faces_open(FILE **f, int *count)
{
    unsigned char h[PL_FACES_HDR];
    FILE *fp = fopen(faces_path, "rb");
    if (!fp)
        return 0;
    if (fread(h, 1, PL_FACES_HDR, fp) != PL_FACES_HDR
        || rd32(h) != PL_FACES_MAGIC
        || rd32(h + 12) != PL_FACE_W || rd32(h + 16) != PL_FACE_H
        || rd32(h + 20) != PL_FACE_BPP) {
        fclose(fp);
        return 0;
    }
    *count = (int)rd32(h + 8);
    if (*count > PL_N_FACES)
        *count = PL_N_FACES;
    *f = fp;
    return 1;
}

int player_faces_init(void)
{
    unsigned char *pix;
    FILE *f = NULL;
    int count = 0, i;

    n_faces = 0;
    if (!faces_open(&f, &count)) {
        rlog("[rccars] player: no %s -- profiles will carry no portrait "
             "(the original then shows face1.tga)\n", faces_path);
        return 0;
    }
    pix = (unsigned char *)malloc(PL_FACE_BYTES);
    if (!pix) {
        fclose(f);
        return 0;
    }
    fseek(f, PL_FACES_HDR + count * 16, SEEK_SET);
    for (i = 0; i < count; i++) {
        if (fread(pix, 1, PL_FACE_BYTES, f) != PL_FACE_BYTES)
            break;
        face_hash[i] = fnv(pix, PL_FACE_BYTES);
    }
    n_faces = i;
    free(pix);
    fclose(f);
    rlog("[rccars] player: %s -- %d portraits\n", faces_path, n_faces);
    return n_faces;
}

int player_face_pixels(int face, unsigned char *out)
{
    FILE *f = NULL;
    int count = 0, ok;

    if (!out || face < 0 || face >= PL_N_FACES)
        return 0;
    if (!faces_open(&f, &count))
        return 0;
    if (face >= count) {
        fclose(f);
        return 0;
    }
    fseek(f, PL_FACES_HDR + count * 16 + face * PL_FACE_BYTES, SEEK_SET);
    ok = fread(out, 1, PL_FACE_BYTES, f) == PL_FACE_BYTES;
    fclose(f);
    return ok;
}

int player_face_match(const unsigned char *pix)
{
    const unsigned int h = fnv(pix, PL_FACE_BYTES);
    int i;
    for (i = 0; i < n_faces; i++)
        if (face_hash[i] == h)
            return i;
    return -1;
}

/* ----------------------------------------------------------- what it says */

const char *player_rank(const player_t *p)
{
    int played = 0, i;

    if (!p)
        return STR_RANK[1];
    for (i = 0; i < PL_N_TRACKS; i++)
        if (p->track[i].i45 > 0)
            played++;
    /* FUN_004e85d0, rung for rung. The bottom one needs all three of its
       conditions, which is why a fresh profile -- 0 scores but 100 cash -- comes
       out one above it. */
    if (p->scores < 50 && p->cash < 50 && played > 1) return STR_RANK[0];
    if (p->scores < 100)   return STR_RANK[1];
    if (p->scores < 250)   return STR_RANK[2];
    if (p->scores < 500)   return STR_RANK[3];
    if (p->scores < 1000)  return STR_RANK[4];
    if (p->scores < 2000)  return STR_RANK[5];
    if (p->scores < 4000)  return STR_RANK[6];
    if (p->scores < 7000)  return STR_RANK[7];
    return STR_RANK[p->scores > 9999 ? 9 : 8];
}

void player_time_str(float seconds, char *out, int n)
{
    long s = (long)(seconds < 0.f ? 0.f : seconds);
    snprintf(out, (size_t)n, "%ld:%02ld:%02ld",
             s / 3600, (s / 60) % 60, s % 60);
}

/* ------------------------------------------------------------- the roster */

const char *player_dir(void) { return dir_path; }

void player_set_dir(const char *dir)
{
    dir_path = dir ? dir : PL_DIR;
    n_roster = 0;
    cur = -1;
    sel_file[0] = 0;
    sel_dirty = 0;
}

int player_count(void) { return n_roster; }

player_t *player_at(int i)
{
    return (i >= 0 && i < n_roster) ? &roster[i] : NULL;
}

player_t *player_cur(void) { return player_at(cur); }
int       player_cur_index(void) { return cur; }

void player_select(int i)
{
    if (i >= 0 && i < n_roster)
        set_cur(i);
}

int player_index_of(const char *name)
{
    int i;
    if (!name || !*name)
        return -1;
    for (i = 0; i < n_roster; i++)
        if (ci_eq(roster[i].name, name))
            return i;
    return -1;
}

static void join(char *out, int n, const char *a, const char *b)
{
    snprintf(out, (size_t)n, "%s/%s", a, b);
}

/* THE FILENAME IS THE NAME, sanitised: anything that is not a letter, a digit
   or a space becomes '_'. The engine does the same thing in FUN_004e7c60 (it
   masks to 7 bits and substitutes for every non-alphanumeric), and the two need
   not agree character for character because the DISPLAY name is chunk 0x21 and
   not the filename -- what has to hold is that a name maps to one file and that
   the file is one the original's FindFirstFile("*.scp") picks up. */
static void name_to_file(const char *name, char *out, int n)
{
    int i = 0;
    for (; name[i] && i < n - 1; i++) {
        const unsigned char c = (unsigned char)name[i];
        out[i] = (char)((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
                        || (c >= 'a' && c <= 'z') || c == ' ' ? c : '_');
    }
    out[i] = 0;
}

static int load_one(const char *path, const char *base, player_t *p)
{
    unsigned char *buf;
    long len;
    FILE *f = fopen(path, "rb");
    int ok;

    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 10 || len > 4 * 1024 * 1024) {
        fclose(f);
        return 0;
    }
    buf = (unsigned char *)malloc((size_t)len);
    if (!buf) {
        fclose(f);
        return 0;
    }
    ok = fread(buf, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    if (ok)
        ok = player_parse(buf, (int)len, p);
    free(buf);
    if (!ok)
        return 0;
    snprintf(p->file, sizeof p->file, "%s", base);
    /* A profile whose 0x21 chunk is missing takes its name from the filename,
       which is what FUN_004e7740 does before it reads the chunk. */
    if (!p->name[0])
        snprintf(p->name, sizeof p->name, "%s", base);
    if (!p->nick[0]) {
        memcpy(p->nick, p->name, sizeof p->nick);
        p->nick[PL_NAME - 1] = 0;
    }
    p->dirty = 0;
    return 1;
}

/* Defined with the other save, below; the scan is what reads it. */
static int  load_selection(void);

static int has_scp_ext(const char *n)
{
    const int l = (int)strlen(n);
    return l > 4 && ci_eq(n + l - 4, ".scp");
}

int player_scan(void)
{
    char path[320], base[64];
    int i;

    n_roster = 0;
    cur = -1;

#ifdef __vita__
    {
        SceUID d = sceIoDopen(dir_path);
        SceIoDirent e;
        if (d < 0) {
            rlog("[rccars] player: no %s -- no profiles yet\n", dir_path);
            return 0;
        }
        memset(&e, 0, sizeof e);
        while (n_roster < PL_MAX && sceIoDread(d, &e) > 0) {
            if (!has_scp_ext(e.d_name))
                continue;
            snprintf(base, sizeof base, "%s", e.d_name);
            base[strlen(base) - 4] = 0;
            join(path, sizeof path, dir_path, e.d_name);
            if (load_one(path, base, &roster[n_roster]))
                n_roster++;
            memset(&e, 0, sizeof e);
        }
        sceIoDclose(d);
    }
#else
    {
        DIR *d = opendir(dir_path);
        struct dirent *e;
        if (!d) {
            rlog("[rccars] player: no %s -- no profiles yet\n", dir_path);
            return 0;
        }
        while (n_roster < PL_MAX && (e = readdir(d)) != NULL) {
            if (!has_scp_ext(e->d_name))
                continue;
            snprintf(base, sizeof base, "%s", e->d_name);
            base[strlen(base) - 4] = 0;
            join(path, sizeof path, dir_path, e->d_name);
            if (load_one(path, base, &roster[n_roster]))
                n_roster++;
        }
        closedir(d);
    }
#endif

    /* THE PROFILE THE LAST LAUNCH ENDED ON, and the first row only when there
       is no marker or it names somebody who is gone. */
    if (n_roster > 0) {
        const int last = load_selection();
        cur = last >= 0 ? last : 0;
        if (last < 0)
            set_cur(cur);       /* nothing on the card yet: write one */
    }
    for (i = 0; i < n_roster; i++)
        rlog("[rccars] player: %s -- %s, %d scores, $%d, face %d\n",
             roster[i].file, roster[i].name, roster[i].scores,
             roster[i].cash, roster[i].face);
    return n_roster;
}

int player_save(int i)
{
    player_t *p = player_at(i);
    unsigned char *buf, *face = NULL;
    char path[320], tmp[336];
    int n, ok, want;
    FILE *f;

    if (!p)
        return 0;

#ifdef __vita__
    sceIoMkdir(PL_ROOT, 0777);
    sceIoMkdir(dir_path, 0777);
#endif

    if (p->face >= 0) {
        face = (unsigned char *)malloc(PL_FACE_BYTES);
        if (face && !player_face_pixels(p->face, face)) {
            free(face);
            face = NULL;
        }
    }
    want = player_format_size(p, face != NULL);
    buf = (unsigned char *)malloc((size_t)want);
    if (!buf) {
        free(face);
        return 0;
    }
    n = player_format(p, face, buf, want);
    free(face);
    if (n <= 0) {
        free(buf);
        rlog("[rccars] player: %s would not serialise\n", p->name);
        return 0;
    }

    join(path, sizeof path, dir_path, p->file);
    snprintf(tmp, sizeof tmp, "%s.scp.new", path);
    strncat(path, ".scp", sizeof path - strlen(path) - 1);

    /* settings.c's rule: through a temporary and renamed over, so a card that
       loses power mid-write loses this save and not the profile. */
    remove(tmp);
    f = fopen(tmp, "wb");
    ok = f != NULL;
    if (ok)
        ok = fwrite(buf, 1, (size_t)n, f) == (size_t)n;
    if (f)
        ok = (fclose(f) == 0) && ok;
    free(buf);
    if (!ok) {
        remove(tmp);
        rlog("[rccars] player: could not write %s\n", tmp);
        return 0;
    }
#ifdef __vita__
    remove(path);                  /* sceIoRename will not replace */
    ok = sceIoRename(tmp, path) >= 0;
#else
    ok = rename(tmp, path) == 0;
#endif
    if (!ok) {
        rlog("[rccars] player: rename failed -- %s left in place\n", tmp);
        return 0;
    }
    p->dirty = 0;
    rlog("[rccars] player: wrote %s (%d bytes%s)\n", path, n,
         p->face >= 0 ? ", portrait" : "");
    return 1;
}

/* The marker, if the selection has moved since it was last written. */
static void save_selection(void)
{
    char path[320];
    FILE *f;

    if (!sel_dirty)
        return;
    sel_dirty = 0;              /* one attempt: a card that will not take it
                                   must not be asked again every save */
    join(path, sizeof path, dir_path, PL_LAST_FILE);
    f = fopen(path, "wb");
    if (!f) {
        rlog("[rccars] player: could not write %s\n", path);
        return;
    }
    fprintf(f, "%s\n", sel_file);
    fclose(f);
}

/* Which profile the last launch ended on, or -1. */
static int load_selection(void)
{
    char path[320], line[PL_NAME + 8];
    FILE *f;
    int i, n;

    sel_file[0] = 0;
    sel_dirty = 0;
    join(path, sizeof path, dir_path, PL_LAST_FILE);
    f = fopen(path, "rb");
    if (!f)
        return -1;
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    n = (int)strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'
                     || line[n - 1] == ' '))
        line[--n] = 0;
    if (n == 0)
        return -1;
    for (i = 0; i < n_roster; i++)
        if (ci_eq(roster[i].file, line)) {
            /* remember what is already on the card, so a launch that changes
               nothing writes nothing */
            snprintf(sel_file, sizeof sel_file, "%s", roster[i].file);
            return i;
        }
    rlog("[rccars] player: %s names `%s', which is not in the roster\n",
         PL_LAST_FILE, line);
    return -1;
}

int player_save_cur_if_dirty(void)
{
    player_t *p = player_cur();
    /* THE MARKER GOES FIRST AND UNCONDITIONALLY. Choosing a different profile
       does not make that profile dirty -- nothing about it changed -- so a save
       gated on `p->dirty' would never write the one thing that did. */
    save_selection();
    if (!p || !p->dirty)
        return 0;
    return player_save(cur);
}

void player_add_play_time(float dt)
{
    player_t *p = player_cur();
    if (!p || dt <= 0.f)
        return;
    p->play_time += dt;
    p->dirty = 1;
}

int player_create(const char *name, int face)
{
    player_t *p;
    int len;

    if (!name)
        return -1;
    len = (int)strlen(name);
    if (len < 1 || len > PL_NAME - 1)
        return -1;
    if (player_index_of(name) >= 0)
        return -2;
    if (n_roster >= PL_MAX)
        return -3;

    p = &roster[n_roster];
    player_init_new(p, name, face);
    name_to_file(name, p->file, sizeof p->file);
    n_roster++;
    if (!player_save(n_roster - 1)) {
        n_roster--;             /* not kept: a profile with no file is a lie */
        return -4;
    }
    set_cur(n_roster - 1);
    return 0;
}

int player_remove(int i)
{
    char path[320];
    int k;

    if (i < 0 || i >= n_roster)
        return -1;
    /* FUN_004e8250's own refusal, and the game's own dialog for it. */
    if (n_roster <= 1)
        return -2;

    join(path, sizeof path, dir_path, roster[i].file);
    strncat(path, ".scp", sizeof path - strlen(path) - 1);
    if (remove(path) != 0) {
        rlog("[rccars] player: could not delete %s\n", path);
        return -3;
    }
    for (k = i; k < n_roster - 1; k++)
        roster[k] = roster[k + 1];
    n_roster--;
    /* The selection follows the list rather than the index: removing the row
       above the current one must not move which profile is current. */
    if (cur > i)
        set_cur(cur - 1);
    else if (cur == i)
        set_cur(i < n_roster ? i : n_roster - 1);
    else
        set_cur(cur);       /* the row moved under it; re-read its file */
    return 0;
}
