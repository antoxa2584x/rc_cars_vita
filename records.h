/*
 * records.h -- THE RECORD BOOK behind `Track stats', kept across a launch.
 *
 * `dlgSTAT' is a table of Player / <the chosen stat> / Car, one row per racer,
 * with `n/a' where that racer has no time on this track -- and a `Sort results
 * by' enum whose four values (`Best lap', `3 laps', `5 laps', `7 laps') pick
 * BOTH the column shown and the key it is sorted on. Those four strings are the
 * game's own, out of the string table at 40607..40610.
 *
 * WHOSE ROWS THEY ARE IS THE ONE PLACE THIS PORT DEVIATES, and it is the same
 * deviation the main menu's player card already makes. The original's rows are
 * the PLAYER PROFILES -- the `Players/' .scp files, six of them in a stock
 * install, each carrying that player's record on each track -- and nothing in
 * this port reads that format or has a second player to put in it. So the rows
 * here are the
 * racers of the last races actually driven on that track: the player, and the
 * opponents the field fielded, each with the best lap they turned and the car
 * they drove. Every number is measured, none is invented, and a track never
 * raced shows the empty table the original shows on a fresh install.
 *
 * WHERE THE NUMBERS COME FROM: main.c already computes all of them at the flag,
 * for `dlgFINISH' -- `race_ui.best_lap' for the player and `lap_best[k]' for
 * each opponent, watched off its own lap counter. records_note() is handed one
 * finished row at a time and merges it in. Nothing here reaches into the physics,
 * the AI or the checkpoint layer.
 *
 * A ROW IS PER RACER, NOT PER RACE, and the merge keeps the best of each stat
 * separately: a 3-lap race improves `best_lap' and `total[0]' and leaves the
 * other two alone, which is what a record book is. The car stored is the car
 * that set the CURRENT best lap, because that is the one the row's third column
 * is describing.
 *
 * The file is `ux0:data/rccars/records.txt', in settings.c's own shape --
 * key/value text, versioned, clamped on the way in AND out, written through a
 * temporary and renamed. See settings.h for why all of that, once; the two
 * files answer to the same rules and this header does not repeat them.
 */
#ifndef RECORDS_H
#define RECORDS_H

/* Six racers is the most a race here can have (the player and five opponents,
   AI_MAX_OPPONENTS), and the seven shipped drivers plus the player is the most
   distinct names one track can accumulate over many races. Rows past this are
   dropped rather than evicted -- the alternative is a record book that forgets
   a time because somebody new raced, and this bound cannot be reached by the
   shipped roster. */
#define REC_MAX_ROWS 8
#define REC_NAME     24

/* The lap counts the quick-race page offers, in MM_LAPS' own order. */
#define REC_N_LAPS   3
extern const int REC_LAPS[REC_N_LAPS];

/* `Sort results by', in the enum's own order -- 40607..40610. */
enum {
    REC_STAT_BEST_LAP = 0,
    REC_STAT_3_LAPS,
    REC_STAT_5_LAPS,
    REC_STAT_7_LAPS,
    REC_N_STAT
};

typedef struct {
    char  name[REC_NAME];
    int   car;                  /* 0..2 into RB_CARS, -1 if not known */
    float best_lap;             /* seconds; <= 0 means no record */
    float total[REC_N_LAPS];    /* best race time over 3 / 5 / 7 laps */
} rec_row;

typedef struct {
    int     n;
    rec_row row[REC_MAX_ROWS];
} rec_track;

/* Every stat of one row, by REC_STAT_*. <= 0 means `n/a'. */
float records_value(const rec_row *r, int stat);

/* Clear the book. Called before the first load; also what the harness uses to
   start from nothing. */
void records_reset(void);

/* One track's rows as stored, in the order they were first seen. NULL never;
   `n` is 0 for a track nobody has raced. */
const rec_track *records_track(int track);

/* The rows of `track' SORTED by `stat', best first, with the rows that have no
   time for that stat last and in their stored order. Fills `out' with at most
   `max` pointers into the book and returns how many. The pointers stay valid
   until the next records_note call. */
int records_sorted(int track, int stat, const rec_row *out[], int max);

/* Merge one racer's result. `best_lap` <= 0 and `total` <= 0 are "did not set
   one" and leave the stored value alone; `laps` is the race's lap count and is
   matched against REC_LAPS, so a race over some other number of laps still
   contributes its BEST LAP and simply has no total to file. */
void records_note(int track, const char *name, int car,
                  float best_lap, int laps, float total);

/* Whether anything has changed since the last load or save. */
int records_dirty(void);

int  records_load(void);
int  records_save(void);
int  records_save_if_changed(void);

const char *records_path(void);
void records_set_path(const char *path);

/* The pure halves, for the harness: no file and no clamping inside `format'.
   `parse' starts from whatever the book already holds, and returns 0 if the
   text declares a version this build will not read. */
int  records_parse(const char *text);
void records_format(char *out, int n);

/* Bumped only if an existing key changes MEANING. */
#define RECORDS_VERSION 1

/* Room for the file with its comments: ten tracks of REC_MAX_ROWS rows, one
   line each, plus a header. The formatter truncates rather than overruns. */
#define RECORDS_TEXT_MAX 6144

#endif /* RECORDS_H */
