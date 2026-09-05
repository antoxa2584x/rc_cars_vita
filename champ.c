/* champ.c -- see champ.h for where every rule and every figure comes from. */
#include "champ.h"
#include "str_data.h"

#include <stdio.h>

/* Two copies of one count is how a table falls out of step with the enum it is
   indexed by; make the compiler hold them together instead, the way garage.c
   does. CHAMP_N_TRACKS is championship.ini's ten and PL_N_TRACKS is the
   profile's ten, and pl_track_slot() maps one onto the other. */
typedef char ch_tracks_agree[(CHAMP_N_TRACKS == PL_N_TRACKS) ? 1 : -1];
typedef char ch_places_agree[(CHAMP_N_PLACES == 3) ? 1 : -1];

static int ok_track(int t) { return t >= 0 && t < PL_N_TRACKS; }

/* THE ONE CONVERSION. Everything public here takes a PORT track index; the
   tables in champ_data.h are in championship.ini's own (engine) order. */
static int slot(int track) { return pl_track_slot(track); }

/* ------------------------------------------------------------- the ladder */

int champ_scores_req(int track)
{
    return ok_track(track) ? CHAMP_TRACK_SCORES[slot(track)] : 0;
}

int champ_fee(int track)
{
    return ok_track(track) ? CHAMP_TRACK_FEE[slot(track)] : 0;
}

int champ_prize(int track, int place)
{
    if (!ok_track(track) || place < 0 || place >= CHAMP_N_PLACES)
        return 0;
    return CHAMP_TRACK_PRIZE[slot(track)][place];
}

int champ_open_from_start(int track)
{
    return ok_track(track) ? CHAMP_TRACK_OPEN[slot(track)] : 0;
}

int champ_best_place(const player_t *p, int track)
{
    int best;
    if (!p || !ok_track(track))
        return -1;
    best = p->track[slot(track)].i46;
    /* PL_NO_PLACE is the engine's "never placed here". Anything outside the six
       the engine has a word for is treated the same way: a hand-edited .scp
       must not index off the end of champ_place_name's table. */
    if (best < 0 || best >= CHAMP_N_PLACINGS)
        return -1;
    return best;
}

int champ_races(const player_t *p, int track)
{
    int n;
    if (!p || !ok_track(track))
        return 0;
    n = p->track[slot(track)].i45;
    return n > 0 ? n : 0;
}

int champ_won_all_others(const player_t *p, int track)
{
    int i;
    /* FUN_004e8580 starts at 1 and only ever clears it, so a NULL profile --
       which the engine cannot have here and this port can, before the first one
       is created -- answers the way an unwalked loop does. */
    if (!p)
        return 1;
    for (i = 0; i < PL_N_TRACKS; i++) {
        if (i == track)
            continue;
        /* WON means best place 0, which is the engine's own `*piVar2 != 0'
           against a field that is PL_NO_PLACE until it is placed on. */
        if (champ_best_place(p, i) != 0)
            return 0;
    }
    return 1;
}

ch_result champ_track_open(const player_t *p, int track)
{
    if (!ok_track(track))
        return CH_LOCKED_SCORES;
    if (!p)
        return CH_NO_PROFILE;
    /* `IsOpened' IS NOT THE CHAMPIONSHIP'S RULE, and this port read it as one
       until the game's own screenshot of dlgCHAMP said otherwise: a fresh
       profile there has Surf open and Fishers, Fort and AAD base showing `---',
       though all four carry IsOpened 1.
     *
       FUN_004e84a0 explains it. The IsOpened branch is guarded by
       `if ((_DAT_01499dd0 & 0x100) == 0)', and entering the championship is
       what SETS that bit -- 0x4be26f is `or ch,0x1' two instructions before
       `push 5 / call FUN_004a4c90', the game-mode setter. Leaving it clears the
       bit again (0x4be5af and 0x4c1567, both `and' against 0xfffffeff).
     *
       So the two modes read the file differently, which is the whole point of
       there being two keys: `IsOpened' is the QUICK RACE's rule -- pick any of
       the four the game ships open -- and the CHAMPIONSHIP ignores it and gates
       every one of the ten on scores alone. Surf's AccessCash is 0, so Surf is
       the only rung a new profile can enter, which is what the picture shows.
       champ_open_from_start() is kept because it is what the file says and the
       quick-race page is entitled to it; nothing on this ladder consults it. */
    /* THE SECOND LOCK IS THE LAST RUNG'S ALONE. FUN_004e84a0 calls the win test
       under `if (track == 9)' and ands its answer into the scores test's, so on
       the other nine it is the constant 1 the function initialised it to -- and
       applying it to all ten locks Mines behind a championship the player has
       to have already finished. Which one to NAME in a refusal is
       FUN_004bea70's choice: 40929 when the win test is what failed and 40928
       otherwise. */
    if (pl_track_slot(track) == PL_N_TRACKS - 1
        && !champ_won_all_others(p, track))
        return CH_LOCKED_WIN;
    if (p->scores < champ_scores_req(track))
        return CH_LOCKED_SCORES;
    return CH_OK;
}

int champ_complete(const player_t *p)
{
    int i;
    if (!p)
        return 0;
    for (i = 0; i < PL_N_TRACKS; i++)
        if (champ_best_place(p, i) != 0)
            return 0;
    return 1;
}

int champ_n_open(const player_t *p)
{
    int i, n = 0;
    for (i = 0; i < PL_N_TRACKS; i++)
        if (champ_track_open(p, i) == CH_OK)
            n++;
    return n;
}

/* ------------------------------------------------------------ what it pays */

int champ_bonus(int track, float gap, int hits)
{
    int secs, cap, bonus;

    if (!ok_track(track))
        return 0;
    /* ROUNDED TO NEAREST, because FUN_004c47e0 adds 0.5f before __ftol, which
       chops. A gap this port cannot measure -- nobody behind -- arrives as 0
       or negative and counts nothing, which is also what the engine does with
       it: no car behind means no prize at all (41320). */
    secs = (gap > 0.f) ? (int)(gap + 0.5f) : 0;
    if (hits < 0)
        hits = 0;
    bonus = CHAMP_BONUS_PER_SEC * secs + CHAMP_BONUS_PER_HIT * hits;
    /* "Max bonus allowed", 41318, and it is Place3 -- the SMALLEST of the three
       prizes, so on Surf the bonus can never be worth more than 15. */
    cap = champ_prize(track, CHAMP_N_PLACES - 1);
    return bonus > cap ? cap : bonus;
}

int champ_award(int track, int place, float gap, int hits)
{
    if (place < 0 || place >= CHAMP_N_PLACES)
        return 0;               /* off the podium -- "No prize money" */
    return champ_prize(track, place) + champ_bonus(track, gap, hits);
}

/* ------------------------------------------------------------ what it does */

ch_result champ_pay_fee(player_t *p, int track)
{
    int fee;

    if (!p)
        return CH_NO_PROFILE;
    if (!ok_track(track))
        return CH_NO_MONEY;
    fee = champ_fee(track);
    /* THE GUARD IS ONE SCREEN EARLIER in the original -- FUN_004bea70 refuses
       the Race button with 40919 and FUN_004c0300 then subtracts without
       checking. Both are here, because a page that can be reached two ways is
       a page whose second way nobody tested. */
    if (p->cash < fee)
        return CH_NO_MONEY;
    if (fee != 0) {
        p->cash -= fee;
        p->dirty = 1;
    }
    return CH_OK;
}

void champ_note_race(player_t *p, int track)
{
    if (!p || !ok_track(track))
        return;
    p->track[slot(track)].i45++;
    p->dirty = 1;
}

int champ_finish(player_t *p, int track, int place, float gap, int hits)
{
    int won, best;

    if (!p || !ok_track(track))
        return 0;
    won = champ_award(track, place, gap, hits);
    if (won != 0) {
        /* INTO BOTH, and this is the whole reason the ladder works: cash is a
           balance the Garage and the entry fee draw down, scores are the career
           total nothing ever spends. FUN_004c56c0's two adjacent adds. */
        p->cash += won;
        p->scores += won;
    }
    /* THE BEST PLACE, and `min' means a worse finish never overwrites a better
       one -- the engine's `if (best < place) place = best'. A track never
       placed on holds PL_NO_PLACE, which every real placing is below. */
    best = p->track[slot(track)].i46;
    if (place >= 0 && place < best)
        p->track[slot(track)].i46 = place;
    p->dirty = 1;
    return won;
}

void champ_new(player_t *p)
{
    int i;

    if (!p)
        return;
    for (i = 0; i < PL_N_TRACKS; i++) {
        /* FUN_004e8b00 with its second argument clear: the best place goes back
           to PL_NO_PLACE and the two counters to zero, and the twenty-two race
           slots -- the record book -- are left exactly as they are. */
        p->track[i].i45 = 0;
        p->track[i].i46 = PL_NO_PLACE;
        p->track[i].f44 = 0.f;
    }
    p->cash = CHAMP_DEFAULT_CASH;
    p->scores = 0;
    p->dirty = 1;
}

/* ------------------------------------------------------------ what it says */

const char *champ_place_name(int place)
{
    /* 40912..40914 and then 40922..40924 -- FUN_004bf150's switch, which is
       exactly this table and exactly this split. */
    static const char *const NAME[CHAMP_N_PLACINGS] = {
        STR_UI_PLACE_1, STR_UI_PLACE_2, STR_UI_PLACE_3,
        STR_UI_PLACE_4, STR_UI_PLACE_5, STR_UI_PLACE_6
    };
    if (place < 0 || place >= CHAMP_N_PLACINGS)
        return "";
    return NAME[place];
}

const char *champ_reason(ch_result r)
{
    switch (r) {
    case CH_OK:            return "";
    /* The port's own, for the same reason garage.c's is: the original always
       has a profile by the time it can reach this screen. */
    case CH_NO_PROFILE:    return STR_UI_NOT_AVAILABLE;
    case CH_LOCKED_SCORES: return STR_UI_CH_LOCKED_SCORES;
    case CH_LOCKED_WIN:    return STR_UI_CH_LOCKED_WIN;
    case CH_NO_MONEY:      return STR_UI_NO_MONEY;
    case CH_NO_CAR:        return STR_UI_NO_UPGRADE;
    default:               return "";
    }
}

const char *champ_status(const player_t *p, int track, char *out, int n)
{
    ch_result r;
    int best;

    if (!out || n <= 0)
        return "";
    out[0] = 0;
    /* FUN_004bed50's four branches, in its own order: the two locked lines
       first, then "Not started" for an open track nobody has raced, then the
       previous result. */
    r = champ_track_open(p, track);
    /* With no profile there is nothing to report and no race to run; the page
       is unreachable in that state (the front end opens on Select player when
       the roster is empty) and an empty line is the honest answer rather than a
       lock the player cannot lift. */
    if (r == CH_NO_PROFILE)
        return out;
    if (r == CH_LOCKED_WIN) {
        snprintf(out, (size_t)n, "%s", STR_UI_CH_WIN_ALL);
        return out;
    }
    if (r != CH_OK) {
        snprintf(out, (size_t)n, "%s", STR_UI_CH_NO_SCORES);
        return out;
    }
    best = champ_best_place(p, track);
    /* THE ENGINE ASKS FOR BOTH: `races != 0' AND a best place in 0..5. A track
       raced but never placed inside the six shows "Not started", which is the
       same line an unraced one shows -- its own test, not this port's. */
    if (champ_races(p, track) == 0 || best < 0)
        snprintf(out, (size_t)n, "%s", STR_UI_CH_NOT_STARTED);
    else
        snprintf(out, (size_t)n, STR_UI_CH_PREV_RESULT, champ_place_name(best));
    return out;
}
