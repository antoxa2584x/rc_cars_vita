/*
 * champ.h -- THE CHAMPIONSHIP, and every rule in it is recovered.
 *
 * `dlgCHAMP' is the ten-track ladder and `dlgCHRACE' is the page that takes the
 * entry fee before the flag. Both are drawn by mainmenu.c; this file is the
 * RULES behind them -- which tracks are open, what a start costs, what a finish
 * pays, and what a race writes back into the profile. No GL, no layout, no
 * globals: a `player_t *' in and a reason code out, the way garage.h is, so the
 * whole progression is testable on the host (`champ_test').
 *
 * THE MODEL, and it was sitting in two places nobody had put together
 * ------------------------------------------------------------------
 * `Scripts/championship.ini' has TEN TRACK SECTIONS these notes had listed for
 * years as "cash and placings" and `gen_champ_data.py' deliberately refused to
 * emit, on the grounds that no championship was built:
 *
 *     IsOpened     open from the first race -- the first four
 *     AccessCash   what it takes to unlock the other six
 *     RaceTariff   the entry fee
 *     Place1..3    the prize for the podium; there is no Place4
 *
 * and the exe reads all five through one getter, FUN_004a8f80(track, &access,
 * &tariff, place[3]), which builds the section name with sprintf("Track%i")
 * one-based.
 *
 * `AccessCash' ON A TRACK IS NOT CASH. It is the one thing here that a name
 * would get wrong and two independent things settle:
 *
 *   * FUN_004e84a0, the "is this track open" test, ends in
 *     `cmp [edi+0x120], esi / setge' -- +0x120 is the profile's SCORES word
 *     (FUN_004e8320 initialises it to 0 and puts DefaultCash in +0x11c), and
 *     +0x11c is what the shop spends and what the entry fee is taken from;
 *   * the game's own column heading for it is string 40901, "Scores req.".
 *
 * So SCORES ARE A CAREER TOTAL and CASH IS A BALANCE. FUN_004c56c0 pays every
 * prize into BOTH, and only the cash half is ever spent -- by the Garage and by
 * the entry fee. A player who buys a car has less money and exactly as much
 * ladder unlocked as before, which is the whole reason the engine keeps two
 * numbers where one would nearly do.
 *
 * WHAT A TRACK IS WORTH, then, is the ladder in CHAMP_TRACK_SCORES: 0, 15, 65,
 * 250, 500, 1000, 1500, 2100, 3000, 5000 -- and ONLY SURF IS OPEN TO A NEW
 * PROFILE. `IsOpened' does not apply here: FUN_004e84a0 skips it whenever
 * `_DAT_01499dd0 & 0x100' is set, and entering the championship is what sets
 * that bit (0x4be26f, `or ch,0x1', two instructions before the game-mode
 * setter is handed 5). It is the QUICK RACE's rule -- four tracks free to pick
 * -- and the championship's is the scores column alone. The game's own
 * screenshot of dlgCHAMP is the check: a fresh profile has Surf reading `n/a'
 * and Fishers, Fort and AAD base reading `---', though all four ship
 * IsOpened 1.
 *
 * AND THE LAST TRACK HAS A SECOND LOCK. FUN_004e8580 walks the other nine and
 * refuses War path unless EVERY one of them has a best place of 0, i.e. has
 * been WON. That is the only track with a condition of its own, and 40935 --
 * "Win on all tracks to gain access" -- is the line the engine puts up for it.
 *
 * WHAT A FINISH PAYS
 * ------------------
 * FUN_004c56c0 is the whole end-of-race write, and the money half of it is
 * three lines:
 *
 *     if (mode == 5 && 0 <= place && place < 3) {
 *         int won = Place[place] + bonus;
 *         cash += won;  scores += won;
 *     }
 *     track.best_place = min(track.best_place, place);
 *     track.races++;
 *
 * -- `mode == 5' being the championship (FUN_004a4c90(5) is what the Race
 * button sets, and FUN_004e03b0 then refuses to start it on anything but FIVE
 * LAPS). The races counter and the track's own clocks are written for a quick
 * race too; the money and the best place are not.
 *
 * THE BONUS is the finish screen's own two lines, and it is capped:
 *
 *     bonus = min(5 * gap_seconds + 5 * hits, Place3)
 *
 * with `gap_seconds' the time between the player and THE CAR BEHIND -- rounded
 * to nearest, since FUN_004c47e0 adds 0.5f before the truncating __ftol -- and
 * `hits' the car's own hit counter, phys+0x5748. The two are labelled by the
 * game: 41316 "Gap bonus (%i sec):" and 41314 "Hit bonus (%i hits):", under
 * 41318 "Max bonus allowed: %s", which is Place3. A player who is LAST has no
 * car behind and gets no bonus at all -- FUN_004c47e0 puts up 41320, "No prize
 * money" -- which for a grid of four means places 0..2 always have one.
 *
 * THE HIT COUNTER IS THE ONE THING HERE THIS PORT SUPPLIES ITSELF, and it is
 * supplied from the right event rather than invented: FUN_004f5e50 increments
 * phys+0x5748 in the same branch that raises the two `!HIT!' gates (phys+0x56e8
 * and +0x56ec, hud.h), so one increment is one !HIT! banner. main.c counts the
 * port's own !HIT! events, which run off a prop-hit edge above sfx.c's speed
 * floor. The TRIGGER is therefore the port's, exactly as the banner's already
 * was; the arithmetic on top of it is the engine's.
 *
 * WHAT LIVES IN THE PROFILE, and none of it is new: `pl_track.i45' is the
 * number of races run on that track (+0x530, which the rank test already
 * counts) and `pl_track.i46' is the BEST PLACE, 0-based, PL_NO_PLACE until the
 * track has been placed on (+0x534). player.h has carried both as unnamed ints
 * with the readings "> 0 means raced" and "INT_MAX until touched" for as long
 * as it has existed; FUN_004c56c0 is what names them. So a championship run
 * here is a championship run on a PC install, and the ladder travels with the
 * .scp like everything else.
 *
 * FIVE LAPS, NOT THE PICKER'S. FUN_004e03b0 verifies the lap count against the
 * mode and refuses mode 5 with anything but 5 ("GM_RACE_TYPE_CHAMP NLaps
 * error"), so the championship does not read enumNLaps at all.
 */
#ifndef CHAMP_H
#define CHAMP_H

#include "player.h"
#include "champ_data.h"

/* The championship's own lap count. FUN_004e03b0 case 5. */
#define CHAMP_LAPS 5

/* How many placings the engine has a word for -- FUN_004bf150's switch runs
   1st..6th. Only the first three are paid. */
#define CHAMP_N_PLACINGS 6

/* Both halves of the bonus are worth this many points per unit, and the whole
   thing is capped at the track's Place3. FUN_004c47e0:
   `lea ebx,[esi+esi*4]' and `iVar5 = ebx + edi * 5'. */
#define CHAMP_BONUS_PER_SEC 5
#define CHAMP_BONUS_PER_HIT 5

/* Why a track will not start. Every one of these is the engine's own refusal
   with the engine's own string; champ_reason returns it. */
typedef enum {
    CH_OK = 0,
    CH_NO_PROFILE,      /* nobody to charge -- the port's own, as garage.h's is */
    CH_LOCKED_SCORES,   /* 40928 -- not enough scores for this track */
    CH_LOCKED_WIN,      /* 40929 -- War path, and nine tracks not yet won */
    CH_NO_MONEY,        /* 40919 -- the entry fee is more than the cash */
    CH_NO_CAR,          /* 40713 -- no car in the garage to race (FUN_004e03b0) */
    CH_N_RESULT
} ch_result;

/* ------------------------------------------------------------- the ladder */

/* Everything below takes a PORT track index (TRACKS[] order) and converts with
   player.h's pl_track_slot(). champ.c is the only file that indexes
   champ_data.h's engine-ordered tables, so there is no second mapping to
   disagree with the first. */

int champ_scores_req(int track);        /* Track<n>/AccessCash */
int champ_fee(int track);               /* Track<n>/RaceTariff */
int champ_prize(int track, int place);  /* place 0..2; 0 for the rest */
/* Track<n>/IsOpened -- what the file says, which is the QUICK RACE's rule.
   Nothing on the ladder consults it; see the header comment. */
int champ_open_from_start(int track);

/* Whether `p' may race `track', and why not. With no profile: CH_NO_PROFILE,
   which the page draws as a locked row rather than crashing. */
ch_result champ_track_open(const player_t *p, int track);

/* The last track's own second lock: 1 when every OTHER track has been won.
   FUN_004e8580. Answers 1 with no profile, which is what the engine's
   `iVar3 = 1' default does before it walks anything. */
int champ_won_all_others(const player_t *p, int track);

/* What the profile has done on `track'. `champ_best_place' is 0-based and
   returns -1 for a track never placed on; `champ_races' is how many races have
   been run on it, quick races included, because that is what the engine counts
   there. */
int champ_best_place(const player_t *p, int track);
int champ_races(const player_t *p, int track);

/* THE WHOLE LADDER IS DONE: every track won. The engine sets a global for this
   the moment a first place lands on the last track (FUN_004c56c0's
   `DAT_014a3a88 = 1'); this is the same fact read off the profile instead, so
   it survives a relaunch. */
int champ_complete(const player_t *p);

/* How many of the ten are open to `p' right now -- for the page's heading and
   for the harness. */
int champ_n_open(const player_t *p);

/* ------------------------------------------------------------ what it pays */

/* min(5*gap + 5*hits, Place3). `gap' is seconds to the car BEHIND and is
   rounded to nearest the way FUN_004c47e0 rounds it; a negative or absent gap
   (nobody behind) counts 0. */
int champ_bonus(int track, float gap, int hits);

/* The prize the finish screen shows and champ_finish pays: the placing's own
   Place<n> plus the bonus, or 0 outside the podium. */
int champ_award(int track, int place, float gap, int hits);

/* ------------------------------------------------------------ what it does */

/* THE ENTRY FEE, taken when dlgCHRACE is confirmed -- FUN_004c0300 on control
   0xce4, which does `cash -= tariff' with the guard one screen earlier.
   Returns CH_NO_MONEY and takes nothing when the profile cannot pay. Marks the
   profile dirty; the write is main.c's, one file on one event. */
ch_result champ_pay_fee(player_t *p, int track);

/* THE END OF A CHAMPIONSHIP RACE. `place' is 0-based, `gap' the seconds to the
   car behind, `hits' the !HIT! count for the race. Pays the prize into cash AND
   scores, lowers the track's best place if this beat it, and returns what was
   paid. Does nothing and returns 0 with no profile.
 *
   THE RACE COUNTER IS NOT HERE. FUN_004c56c0 bumps `track.races' for every
   race whatever the mode, so main.c calls champ_note_race for a quick race too
   and this function does not double it. */
int champ_finish(player_t *p, int track, int place, float gap, int hits);

/* One race run on `track', whatever the mode -- the counter the rank ladder
   reads. Call once per finish, before or after champ_finish; they touch
   different fields. */
void champ_note_race(player_t *p, int track);

/* NEW CHAMPIONSHIP -- 10053, the row the game's own front end has and the
   question it asks first (40927, "Warning! Do you really want to erase data
   and start new championship?"). Clears every track's best place and race
   count and puts the profile back on DefaultCash with no scores.
 *
   WHAT IT DOES NOT CLEAR is the record book in the twenty-two race slots and
   the two clocks beside them -- FUN_004e8b00 resets those only when its second
   argument is set, and the championship reset does not set it. A player's best
   lap on Surf is theirs whatever they do to the ladder.
 *
   THE GARAGE IS NOT CLEARED EITHER: the cars a profile owns and the parts
   fitted to them stay. That is this port's reading of a reset that the engine
   only ever reaches through a dialog whose handler is not recovered, and it is
   the reading that cannot destroy something a player paid for. See
   docs/known-issues.md. */
void champ_new(player_t *p);

/* ------------------------------------------------------------ what it says */

/* "1st".."6th" for 0..5, out of 40912..40914 and 40922..40924 -- the engine's
   own FUN_004bf150 split. "" for anything else. */
const char *champ_place_name(int place);

/* The game's own wording for a reason code, or "" for CH_OK. */
const char *champ_reason(ch_result r);

/* The status line dlgCHAMP puts under the ladder for `track' -- 40932 "Not
   started", 40933 "Previous race result %s", 40934 or 40935 when it is locked.
   Writes into `out' and returns it; FUN_004bed50's own four branches. */
const char *champ_status(const player_t *p, int track, char *out, int n);

#endif /* CHAMP_H */
