/*
 * player.h -- THE PLAYER PROFILE, in the game's own `.scp` format.
 *
 * The original keeps one file per player in `Players/`, and every fact the
 * front end's player card shows is in it: the name, the portrait, the cash, the
 * scores, the play time, which cars are unlocked and what each is tuned to, and
 * a record book of 22 race slots per track. These notes said for a year that
 * "this port does not read that format"; it does now, and it writes it, so a
 * `.scp` copied off a PC install works here and one written here works there.
 *
 * THE FORMAT IS THE ENGINE'S GENERIC CHUNK STREAM, and it is four lines:
 *
 *     u8  tag          what this record is
 *     u8  type         how to read the payload
 *     u32 next         ABSOLUTE file offset of the record after this one
 *     u8  payload[next - here - 6]
 *
 * -- so a record's length is the difference between two offsets and a container
 * is a record whose payload is more records. The whole file is one record, tag
 * 0x20 type 0x50, whose `next` is the file's own size and whose payload is a
 * float version (1.0) followed by everything else.
 *
 * The types, all of them, read off the engine's writers (`tiohbChWrite*`,
 * FUN_004e7e70) and its reader (FUN_004e7740):
 *
 *     0x00  nothing         a bare marker; the payload is empty
 *     0x02  children        no value of its own
 *     0x14  blob            u32 byte count, then that many bytes
 *     0x24  array           u32 element count, then count * elemsize bytes
 *     0x30  int32
 *     0x32  int32 + children    the value is an INDEX and the children follow
 *     0x40  NUL-terminated string
 *     0x50  float           and, for the root, children after it
 *
 * PROVED BY ROUND TRIP rather than by reading: `player_test` parses the two real
 * saves in `Players/` into the struct below and writes them back BYTE FOR BYTE,
 * all 152,994 of them. Nothing in this header is inferred from a field's value.
 *
 * THE PORTRAIT IS IN THE FILE, all 128 KB of it: chunk 0x22 is the engine's own
 * 160-byte csiHEADER and 0x23 is 128 x 256 BGRA. That header is KEPT VERBATIM
 * when one arrives -- ten of its forty dwords are runtime pointers belonging to
 * whatever process wrote the file, and carrying them through unchanged is the
 * difference between a round trip that is exact and one that is only nearly.
 * A profile this port makes has no such header to keep and writes PL_CSIHDR,
 * whose pointer slots are zero.
 *
 * THE PIXELS ARE MEASURED, not guessed --
 * `Players/Player.scp`'s are `FacesSys/BabyShark.tga`'s with the rows
 * REVERSED (a TGA whose descriptor says bottom-up; the chunk is top-down) and
 * the bytes untouched. So this file writes the same nine portraits back, out of
 * `assets/faces.bin`, and identifies an incoming one by hashing its pixels
 * against them. A `.scp` with no portrait chunk is still valid -- the engine
 * gives it face1.tga -- which is what happens for a face this build cannot
 * supply.
 *
 * THE TWO ORDERINGS ARE NOT THIS PORT'S. The track array is in the ENGINE's
 * order (Surf, Fishers, Fort, AAD, ...), which `hud_data.h`'s MAP_TRACKMAP
 * already carries because the painted maps are numbered in it; pl_track_slot()
 * is that mapping and nothing else converts. The car array is 0..2 and the two
 * orders agree -- championship.ini's Car1/2/3 are Overkill, Buggy, Hummer, which
 * is rb_data.h's order.
 *
 * WHAT IS *NOT* NAMED HERE stays unnamed. Four ints and two floats per track,
 * one float and one int at the top, and four of the eleven words in a race slot
 * have no reading yet -- they are carried through untouched so an exchanged file
 * keeps whatever the original put in them. Guessing at them would be inventing a
 * meaning for a number this port has never seen change; see `docs/traps.md`.
 */
#ifndef PLAYER_H
#define PLAYER_H

/* The engine's own field width -- `tiohbReadString(h, p, 0x10)` -- so 15
   characters and a NUL. A name longer than this is refused rather than
   truncated: a truncated one collides with somebody else's. */
#define PL_NAME      16

/* FUN_004e7c60 refuses the 21st player. Same here, and for the same reason:
   a roster the original cannot open is not an exchange. */
#define PL_MAX       20

#define PL_N_CARS     3
#define PL_N_TRACKS  10

/* 20 indexed race slots per track (chunk 0x71, index 0..19) and two more with
   no index of their own (0x70 before them and 0x79 after). Kept as one array so
   the writer's order is the array's order: [0] is 0x70, [1..20] are the twenty,
   [21] is 0x79. */
#define PL_N_RACE    22
#define PL_RACE_HEAD  0
#define PL_RACE_LIST  1
#define PL_RACE_TAIL 21

/* The portrait. 128 x 256 BGRA, which is what FacesSys ships and what the
   engine's own csiHEADER in every shipped save declares. */
#define PL_FACE_W    128
#define PL_FACE_H    256
#define PL_FACE_BPP    4
#define PL_FACE_BYTES (PL_FACE_W * PL_FACE_H * PL_FACE_BPP)
#define PL_N_FACES     9

/* "No time here." The engine writes 2e6 seconds into an empty race slot
   (0x49f42400) and INT_MAX into the track's own 0x46, and both are what
   FUN_004e8b00 resets a track to. Written out as-is so a fresh profile from
   this port is the fresh profile the original makes. */
#define PL_NO_TIME   2000000.f
#define PL_NO_PLACE  0x7fffffff

/* One race slot, 44 bytes in the engine's own struct and eleven words here.
   `time` is the only one with a settled reading. */
typedef struct {
    float          time;        /* 0x72, seconds; PL_NO_TIME for empty */
    int            a, b, c, d;  /* 0x73, 0x74, 0x75, 0x76 */
    unsigned short e[4];        /* 0x77 -- four u16 */
    unsigned short g[4];        /* 0x78 -- four u16 */
} pl_race;

typedef struct {
    pl_race rec[PL_N_RACE];
    float   f44;                /* 0x44 */
    int     i45;                /* 0x45 -- > 0 on a track that has been raced;
                                   the rank test counts these (FUN_004e85d0) */
    int     i46;                /* 0x46 -- PL_NO_PLACE when untouched */
    float   f47;                /* 0x47 */
} pl_track;

typedef struct {
    int enabled;                /* chunk 0x31 present */
    int up[4];                  /* 0x32, 0x33, 0x34, 0x35 */
} pl_car;

typedef struct {
    char     name[PL_NAME];     /* 0x21 */
    char     nick[PL_NAME];     /* 0x60 -- the same word on every shipped save */
    int      face;              /* 0..PL_N_FACES-1, recovered from the pixels;
                                   -1 for a portrait this build does not know */
    unsigned char fhdr[160];    /* chunk 0x22 as it arrived, or zero */
    int      have_fhdr;
    float    play_time;         /* 0x24, seconds */
    int      cash;              /* 0x25 */
    int      scores;            /* 0x26 */
    float    f27;               /* 0x27 */
    pl_car   car[PL_N_CARS];
    pl_track track[PL_N_TRACKS];        /* ENGINE order -- pl_track_slot() */
    /* The four the engine hands to its race-setup globals the moment a profile
       is selected (FUN_004e8440), in the order it hands them over. The reading
       is the ORDER plus the defaults -- 0, 0, 3, 0 on a fresh profile, and the
       card's "Current car: Road Rage RR" is car 0 -- and it is the one thing in
       this file that is inferred rather than round-tripped. */
    int      sel_track;         /* 0x50, engine track index */
    int      sel_car;           /* 0x51 */
    int      laps;              /* 0x52 */
    int      skill;             /* 0x53 */
    int      i61;               /* 0x61 */
    /* The port's own bookkeeping; not in the file. */
    char     file[64];          /* basename, without the extension */
    int      dirty;
} player_t;

/* THE PORT's track index to the ENGINE's, and back. hud_data.h's MAP_TRACKMAP
   is that permutation already -- it numbers the painted maps -- so this is the
   one place the two orders meet and no second table exists to disagree. */
int pl_track_slot(int port_track);
int pl_track_port(int engine_slot);

/* ------------------------------------------------------------- the roster */

/* Where the .scp files live. `ux0:data/rccars/Players` on the Vita, which is
   the same directory name the PC install uses, so a card and an install can be
   copied between with no renaming. */
const char *player_dir(void);
void        player_set_dir(const char *dir);    /* the harness's own */

/* Read every `*.scp` in the directory, newest state wins, and select the first.
   Returns how many loaded. Safe to call twice; it clears first. */
int         player_scan(void);

int         player_count(void);
player_t   *player_at(int i);
int         player_index_of(const char *name);  /* -1 if none; case-insensitive,
                                                   which is what the engine's
                                                   own __strcmpi lookup does */

/* THE CURRENT PROFILE, and NULL when the roster is empty -- which is the state
   the front end opens the Select player page in. */
player_t   *player_cur(void);
int         player_cur_index(void);
void        player_select(int i);

/* Make one, save it, and select it. Returns
     0  made it
    -1  the name is empty or longer than PL_NAME-1
    -2  a player of that name already exists
    -3  the roster is full (PL_MAX)
    -4  the file could not be written -- the profile is NOT kept
   `face` is 0..PL_N_FACES-1. Everything else is the engine's own new-profile
   state: DefaultCash 100 (championship.ini), Car1 enabled and the other two
   not, three laps, every track empty. */
int         player_create(const char *name, int face);

/* Delete `i`, its file included. Returns
     0  gone
    -1  no such row
    -2  it is the last one -- "Can't remove last player", which is the engine's
        own refusal (FUN_004e8250) and the dialog in the game's own screenshot
    -3  the file would not delete
   Removing the CURRENT profile selects its neighbour first, which is what the
   game's "Do you want to remove current player?" implies and what the engine's
   own remove refuses to do on its behalf. */
int         player_remove(int i);

/* Write one out. The write goes to `<name>.scp.new` and is renamed over the
   real file, so a machine that loses power mid-write loses THIS save rather
   than the profile -- settings.c's rule, and for the same reason. */
int         player_save(int i);
int         player_save_cur_if_dirty(void);

/* Seconds of wall clock to add to the current profile's play time. Marks it
   dirty; the write itself is one file on one event, like every other save in
   this app. */
void        player_add_play_time(float dt);

/* ------------------------------------------------------------ what it says */

/* The rank the card shows, out of the string table's 40550..40559 through the
   thresholds in FUN_004e85d0: < 50 scores AND < 50 cash AND more than one track
   raced is the bottom rung, then 100 / 250 / 500 / 1000 / 2000 / 4000 / 7000,
   and 10000 is the top. A fresh profile has 0 scores and 100 cash, so it comes
   out "Casual" -- which is what the game's own screenshot of this page says. */
const char *player_rank(const player_t *p);

/* "h:mm:ss", the card's own format. `out` is at least 16 bytes. */
void        player_time_str(float seconds, char *out, int n);

/* ---------------------------------------------------------- the portraits */

/* The nine, in the order this port numbers them; the same list `pack_faces.py`
   writes into assets/faces.bin and the same names menu.vsc carries. */
extern const char *const PL_FACE_NAME[PL_N_FACES];

/* Read assets/faces.bin's index once, so an incoming portrait can be named.
   Costs one pass over the file and keeps nine hashes. Returns how many faces
   the file declared; 0 (no file) is not fatal -- a save then simply carries no
   portrait and the engine gives it face1.tga. */
int         player_faces_init(void);
void        player_faces_set_path(const char *path);

/* --------------------------------------------------------- the pure halves */

/* Parse a whole file image. Returns 1 on success. `p` is zeroed first and then
   filled; an unknown tag is skipped the way the engine skips it, so a file
   written by a later build still loads what this one understands. */
int         player_parse(const unsigned char *buf, int len, player_t *p);

/* Serialise `p` into `buf`, at most `max` bytes, and return how many were
   written -- 0 if it would not fit. `face_pixels` is PL_FACE_BYTES of BGRA or
   NULL for "write no portrait chunk". */
int         player_format(const player_t *p, const unsigned char *face_pixels,
                          unsigned char *buf, int max);

/* How big player_format's output will be, portrait included. */
int         player_format_size(const player_t *p, int with_face);

/* A blank profile, exactly as FUN_004e7c60 and FUN_004e8320 build one. */
void        player_init_new(player_t *p, const char *name, int face);

/* The nine faces' pixels, for the writer and for the harness. Returns 1 and
   fills `out` (PL_FACE_BYTES) when the asset has that face, 0 otherwise. */
int         player_face_pixels(int face, unsigned char *out);

/* Which of the nine `pix` is, or -1. */
int         player_face_match(const unsigned char *pix);

#endif /* PLAYER_H */
