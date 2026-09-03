/*
 * garage.h -- THE SHOP, and every number in it is shipped.
 *
 * `dlgSETCAR' is the Garage and `dlgSETDETAIL' is the ONE upgrade page its three
 * part buttons open. Both are drawn by mainmenu.c; this file is the RULES behind
 * them -- what a part costs, what it fetches back, which of the three levels a
 * profile may buy next, and what happens to the profile's cash when it does.
 * No GL, no layout, no globals: a `player_t *' in and a reason code out, so the
 * whole economy is testable on the host (`garage_test').
 *
 * WHERE EVERY FIGURE COMES FROM. `Scripts/championship.ini' -- a commented
 * .ini that had never been opened -- carries the entire economy, and it agrees
 * with the game's own screenshots of these four screens to the dollar:
 * DefaultCash 100 is the "$100" the cash line shows, Car1/AccessCash 1000 is
 * the "Sell price: $900" under the spec block, Boost-Car1/Level1 50 is the
 * booster page's "Price: $50" and Resonator-Car1/Level1 65 is the engine
 * page's. `champ_data.h' is that file; `gen_champ_data.py' says how the sell
 * prices come out of the buy prices.
 *
 * THE FOUR STATES OF ONE PART ARE THE ENGINE'S OWN, read off the status text
 * FUN_004d4fc0 writes under the picture, with `up' the level the profile owns
 * (0..3, 0 being Default) and `sel' the level the picker is on (0..2):
 *
 *     up <  sel      "Not available"          (40704), and drawn dim
 *     up == sel      "Price: $n"              (40703) -- the one you may BUY
 *     up == sel + 1  "INSTALLED" and
 *                    "Sell part for $n"       (40705, 40706) -- the one you own
 *     up >  sel + 1  "You have to sell
 *                     previous upgrade first!" (40707)
 *
 * so exactly one level of the three is buyable and exactly one is sellable, and
 * the four strings the game ships for this are what say so. That is where
 * garage_state, garage_can_buy_part and garage_can_sell_part come from; none of
 * it is invented.
 *
 * AND ONE THING HERE IS DELIBERATELY NOT THE RETAIL EXE'S. Its buy handler
 * (FUN_004d45b1 and its two twins) charges the price of the level the PICKER is
 * on and then does `up = up + 1' -- so buying with the picker on level 3 from
 * Default pays 520 and hands over level 1. This file refuses that purchase
 * instead, in the game's own words: 40710 "You already have this upgrade" when
 * the picker is below what you own and 40711 "You shoud buy previous upgrade"
 * when it is above -- and 40711 is a string the retail exe ships and never uses,
 * which is the shape of a guard that was written and then lost. Taking a
 * player's money for a part they do not get is not behaviour worth reproducing;
 * `docs/known-issues.md' records the deviation.
 *
 * SELLING A CAR IS GUARDED, and the guard is the engine's (FUN_004d5fb0): it is
 * refused when it would leave the profile with no car AND with less money than
 * the CHEAPEST car costs -- the sold one included. 40806, "This operation is
 * impossible. You'll not be able to buy any car.", is that check's own words.
 *
 * WHAT LIVES IN THE PROFILE. `pl_car.up[0..3]' per car: booster, engine, tyres
 * and the PAINT. That ordering is measured, not assumed -- the engine's three
 * buy handlers write profile+car*20+0x12c, +0x130 and +0x134 respectively
 * (formats.md's 0x3032, 0x3033, 0x3034) and its Next skin button (FUN_004d62a0)
 * does `up[3] = (up[3] + 1) % nskins' and saves. So a profile carried to a PC
 * install opens there with the same parts fitted and the same paint on.
 */
#ifndef GARAGE_H
#define GARAGE_H

#include "player.h"
#include "champ_data.h"

/* The three part kinds, in pl_car.up[0..2]'s order -- which is also the order
   the Garage's own three buttons are in. */
enum {
    GAR_BOOSTER = 0,
    GAR_ENGINE,                 /* championship.ini calls it the resonator */
    GAR_TIRES,
    GAR_N_KINDS
};

/* pl_car.up[3]. Not a part; the car's paint. */
#define GAR_SKIN 3

/* The three PURCHASABLE levels. A profile's own level runs 0..GAR_N_LEVELS,
   0 being Default, and the picker walks 0..GAR_N_LEVELS-1. */
#define GAR_N_LEVELS CHAMP_N_LEVELS

/* What a request did, or why it did nothing. Every one of these has a string in
   the game's own table; garage_reason returns it. */
typedef enum {
    GAR_OK = 0,
    GAR_NO_PROFILE,     /* nobody to charge -- the port's own, see below */
    GAR_NO_MONEY,       /* 40712 Not enough money */
    GAR_HAVE_IT,        /* 40710 You already have this upgrade */
    GAR_BUY_PREV,       /* 40711 You shoud buy previous upgrade */
    GAR_NO_UPGRADE,     /* 40713 You don't have this upgrade */
    GAR_OWNED,          /* the car is already in the garage */
    GAR_NOT_OWNED,      /* 40713 again -- you do not have this car */
    GAR_STUCK,          /* 40806 you would not be able to buy any car */
    GAR_N_RESULT
} gar_result;

/* What the status line under the part picture says -- see the header comment;
   these four are the engine's own four branches, in its own order. */
typedef enum {
    GAR_ST_NOT_AVAIL = 0,
    GAR_ST_PRICE,
    GAR_ST_INSTALLED,
    GAR_ST_SELL_PREV
} gar_state;

/* ------------------------------------------------------------ what it costs */

/* `level' is 1..GAR_N_LEVELS. 0 (Default) costs and fetches nothing. */
int  garage_part_price(int kind, int car, int level);
int  garage_part_sell(int kind, int car, int level);

int  garage_car_price(int car);

/* WHAT SELLING THE CAR ACTUALLY FETCHES: its own sell price plus the sell price
   of every level fitted to it, all three kinds. FUN_004d63f0, which is what the
   Garage's own "Sell price:" line shows -- so a fully upgraded car is worth
   more than a bare one, which is why this takes the profile. */
int  garage_car_sell(const player_t *p, int car);

/* ------------------------------------------------------------- what it holds */

int  garage_owns_car(const player_t *p, int car);
int  garage_n_cars(const player_t *p);         /* how many are in the garage */

/* The level fitted, 0..GAR_N_LEVELS. 0 with no profile. */
int  garage_level(const player_t *p, int kind, int car);

/* The picker's own state at `sel' (0..GAR_N_LEVELS-1). */
gar_state garage_state(const player_t *p, int kind, int car, int sel);

/* The paint, pl_car.up[3], and where the Garage's Next skin button puts it.
   `nskins' is what the LOADED car has -- carparts_t::n_skin, which menu.c
   already carries -- because how many skins exist is a property of the packed
   scene and neither this file nor the profile can see one. */
int  garage_skin(const player_t *p, int car);
void garage_set_skin(player_t *p, int car, int skin);
int  garage_next_skin(int skin, int nskins);

/* --------------------------------------------------------------- what it says */

/* The part's own name and its two-line effect block, out of str_data.h's
   twenty-seven. Level 0 is "Default" (41800) for both. */
const char *garage_part_name(int kind, int car, int level);
const char *garage_part_info(int kind, int car, int level);

/* The heading the upgrade page carries: Booster / Engine / Tires, 40700..40702. */
const char *garage_kind_name(int kind);

/* THE PART'S PICTURE, `upgr_boost<level>_<car+1>' and its two siblings -- and
   THE LEVEL IS THE FIRST INDEX, which is easy to get backwards and was: the
   engine builds the name with sprintf("upgr_boost%i_%i", level, car + 1)
   (FUN_0049fdc0), and matching the game's own screenshot of the booster page
   against all nine `upgr_boost*' agrees -- the three that share its SECOND
   index look alike (same car, three exhausts) and the ones that share the first
   do not (three different bodies). Writes into `out' and returns it. */
const char *garage_part_tex(int kind, int car, int level, char *out, int n);

/* A money figure as the game writes one: `$100'. The original renders it
   through a locale-aware currency formatter (FUN_004a98f0, keyed on a
   `Currency' setting) and pastes the result into its own "%s" forms; this port
   writes the one form the shipped English build shows. */
void garage_cash(char *out, int n, int cash);

/* The game's own wording for a reason code, or "" for GAR_OK. */
const char *garage_reason(gar_result r);

/* ------------------------------------------------------------- what it does */

/* Every one of these leaves the profile untouched unless it returns GAR_OK, and
   marks it dirty when it does -- the write itself is main.c's, one file on one
   event, like every other save in this app. `sel' is the PICKER's level,
   0..GAR_N_LEVELS-1.
 *
   With no profile at all -- which is only the harness and the frames before the
   first one is created -- every one of them returns GAR_NO_PROFILE and changes
   nothing. The pages still draw: they show the car, its spec and the price
   list, and the four buttons deny. */
gar_result garage_can_buy_part(const player_t *p, int kind, int car, int sel);
gar_result garage_buy_part(player_t *p, int kind, int car, int sel);
gar_result garage_can_sell_part(const player_t *p, int kind, int car, int sel);
gar_result garage_sell_part(player_t *p, int kind, int car, int sel);

gar_result garage_can_buy_car(const player_t *p, int car);
gar_result garage_buy_car(player_t *p, int car);
gar_result garage_can_sell_car(const player_t *p, int car);
gar_result garage_sell_car(player_t *p, int car);

/* The next car in the garage after `from', wrapping, or -1 when the profile has
   none -- which is what selling out from under the current selection needs. */
int  garage_next_owned_car(const player_t *p, int from);

#endif /* GARAGE_H */
