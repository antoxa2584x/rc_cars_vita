/*
 * settings.h -- the menu's choices, kept across a launch.
 *
 * Everything the START menu holds that is a PREFERENCE rather than a state:
 * the track and car last driven, the paint of each car, the three upgrade
 * indices, both volumes, texture quality, the 565 byte order and the car's own
 * light. Written to `ux0:data/rccars/settings.txt`, read once at startup, and
 * that is the whole feature -- there is no in-menu Save row, because a setting
 * the player has to remember to store is a setting they will lose.
 *
 * WHY A TEXT FILE and not the struct. `menu_t` is written to by nine files and
 * gains a row every time something new is worth A/B-ing; a raw fwrite of it
 * would be a format that changes silently whenever a field is inserted, and
 * whose old files then load as garbage that has to be *detected* rather than
 * simply ignored. Key/value text has neither problem: a key this build does not
 * know is skipped, a key the file does not carry keeps menu_init's default, and
 * the whole thing can be read -- and fixed -- with a text editor on the card,
 * which matters for `tex_colours`, the one row that can make the game
 * unrecognisable (blue sand, gold sea) before the player can reach the menu.
 *
 * EVERY VALUE IS CLAMPED ON THE WAY IN, and that is not defensive habit: the
 * track index goes into `TRACKS[]` and the car index into `RB_CARS[]`, so a
 * hand-edited `track 99` is an out-of-bounds read at startup, before any menu
 * exists to correct it. A file that says something impossible loses that one
 * line, not the launch.
 *
 * THE WRITE IS ONE FILE, ON ONE EVENT: the frame the menu closes, and again if
 * the Quit row is taken. Not per frame and not per keypress -- see
 * `docs/vita-port.md` on what a memory-card write costs the frame it lands in.
 * Nothing else in the app can change a persisted value, so closing the menu is
 * the only moment where there is anything new to write, and even then the save
 * compares against what is already on the card and does nothing when the player
 * only looked.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include "menu.h"

/* The save is written here and renamed over the real file, so a machine that
   loses power mid-write loses THIS save rather than every setting stored before
   it. In the header only so the harness can clean up after itself. */
#define SETTINGS_TMP_SUFFIX ".new"

/* Bumped only if an existing key changes MEANING -- adding or removing one needs
   no bump, since a missing key is a default and an unknown key is ignored. A
   file from a LATER version than this is left alone entirely: its keys may be
   the same words for different things. */
#define SETTINGS_VERSION 1

/* The persisted subset of menu_t, and nothing else. `row`, `open`, `cue`, the
   req_* requests and `skins` are all state rather than preference -- `skins` in
   particular is a property of the packed car, not a choice, and restoring a
   stale one would offer paint that is not in the scene.
 *
   All int, deliberately: the whole struct is memcmp'd against the last thing
   written to decide whether there is anything to write, and padding between
   fields of mixed size would make that comparison read uninitialised bytes. */
typedef struct {
    int track;
    int car;
    int skin[MENU_N_CARS];
    int tires;
    int reso;
    int boost;
    int vol_sfx;
    int vol_music;
    int tex_quality;
    int tex_swap_rb;
    int car_light;
} settings_t;

/* Read the file and apply it to `m`, which must already have been through
   menu_init: anything the file does not carry keeps the default that put there.
   Returns 1 if a file was read, 0 if there was none (or it was unusable), in
   which case `m` is untouched. Never raises a req_* -- the caller does the first
   load itself, off `m->track` and `m->car`. */
int settings_load(menu_t *m);

/* Write `m`'s preferences out. Returns 1 on success.
 *
   The `_if_changed` form is the one the frame loop calls: it returns 0 without
   touching the card when nothing differs from what was last read or written,
   so opening the menu to look at the map costs no I/O. */
int settings_save(const menu_t *m);
int settings_save_if_changed(const menu_t *m);

/* Where the file is, for the log line that says so. */
const char *settings_path(void);

/* Point the module at another file. For the host harness, which must not write
   into whatever directory it was run from -- and which needs two files in one
   run to check that a save really round-trips. Pass NULL to go back to the
   default. Resets the "already saved" snapshot, since it belongs to a file. */
void settings_set_path(const char *path);

/* The pure halves, exposed for the harness: no file, no clamping inside
   `format`, and `parse` starts from whatever `s` already holds so a partial file
   keeps the caller's defaults. `parse` returns 0 if the text declares a version
   this build will not read. */
void settings_from_menu(const menu_t *m, settings_t *s);
void settings_to_menu(const settings_t *s, menu_t *m);
int settings_parse(const char *text, settings_t *s);
void settings_format(const settings_t *s, char *out, int n);
void settings_clamp(settings_t *s);

#endif
