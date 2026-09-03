/* garage.c -- see garage.h for where every figure and every rule comes from. */
#include "garage.h"
#include "str_data.h"

#include <stdio.h>

/* str_data.h emits three tables per kind; index them the way the profile does.
   A table of pointers rather than a switch, so a fourth kind cannot be added to
   the enum without this line failing to compile. */
static const char *const *const GAR_NAME[GAR_N_KINDS] = {
    &STR_PART_BOOSTER_NAME[0][0], &STR_PART_ENGINE_NAME[0][0],
    &STR_PART_TIRES_NAME[0][0]
};
static const char *const *const GAR_INFO[GAR_N_KINDS] = {
    &STR_PART_BOOSTER_INFO[0][0], &STR_PART_ENGINE_INFO[0][0],
    &STR_PART_TIRES_INFO[0][0]
};

/* The engine's own texture prefixes, in the same order. */
static const char *const GAR_TEX[GAR_N_KINDS] = {
    "upgr_boost", "upgr_reson", "upgr_tires"
};

/* 40700..40702, the heading dlgSETDETAIL's staticUpgradeName carries. */
static const char *const GAR_KIND[GAR_N_KINDS] = {
    STR_UI_BOOSTER, STR_UI_ENGINE, STR_UI_TIRES
};

/* Two copies of one count is how a table falls out of step with the enum it is
   indexed by; make the compiler hold them together instead. */
typedef char gar_levels_agree[(GAR_N_LEVELS == STR_N_PART_LEVELS) ? 1 : -1];
typedef char gar_cars_agree[(PL_N_CARS == CHAMP_N_CARS) ? 1 : -1];
typedef char gar_str_cars_agree[(PL_N_CARS == STR_N_CARS) ? 1 : -1];

static int ok_kind(int kind) { return kind >= 0 && kind < GAR_N_KINDS; }
static int ok_car(int car)   { return car >= 0 && car < PL_N_CARS; }
static int ok_lvl(int lv)    { return lv >= 1 && lv <= GAR_N_LEVELS; }
static int ok_sel(int sel)   { return sel >= 0 && sel < GAR_N_LEVELS; }

/* ------------------------------------------------------------ what it costs */

int garage_part_price(int kind, int car, int level)
{
    if (!ok_kind(kind) || !ok_car(car) || !ok_lvl(level))
        return 0;
    return CHAMP_PART_PRICE[kind][car][level - 1];
}

int garage_part_sell(int kind, int car, int level)
{
    if (!ok_kind(kind) || !ok_car(car) || !ok_lvl(level))
        return 0;
    return CHAMP_PART_SELL[kind][car][level - 1];
}

int garage_car_price(int car)
{
    return ok_car(car) ? CHAMP_CAR_PRICE[car] : 0;
}

int garage_car_sell(const player_t *p, int car)
{
    int total, kind;

    if (!ok_car(car))
        return 0;
    total = CHAMP_CAR_SELL[car];
    if (!p)
        return total;
    /* EVERY LEVEL FITTED, not just the top one -- FUN_004d63f0 walks
       `for (l = 0; l < up[kind]; l++) total += sell(kind, car, l + 1)', so a car
       upgraded to level 3 fetches back the sell price of all three. Which is
       right: all three were paid for on the way up. */
    for (kind = 0; kind < GAR_N_KINDS; kind++) {
        const int up = garage_level(p, kind, car);
        int l;
        for (l = 1; l <= up; l++)
            total += garage_part_sell(kind, car, l);
    }
    return total;
}

/* ------------------------------------------------------------ what it holds */

int garage_owns_car(const player_t *p, int car)
{
    return (p && ok_car(car)) ? (p->car[car].enabled != 0) : 0;
}

int garage_n_cars(const player_t *p)
{
    int i, n = 0;
    for (i = 0; i < PL_N_CARS; i++)
        if (garage_owns_car(p, i))
            n++;
    return n;
}

int garage_level(const player_t *p, int kind, int car)
{
    int up;
    if (!p || !ok_kind(kind) || !ok_car(car))
        return 0;
    up = p->car[car].up[kind];
    /* CLAMPED ON THE WAY OUT. A `.scp' written by a later build, or by a cheat,
       can carry anything here, and this number indexes both a price table and
       str_data.h. settings.c's rule, and for the same reason. */
    if (up < 0) up = 0;
    if (up > GAR_N_LEVELS) up = GAR_N_LEVELS;
    return up;
}

gar_state garage_state(const player_t *p, int kind, int car, int sel)
{
    const int up = garage_level(p, kind, car);
    if (!ok_sel(sel))
        return GAR_ST_NOT_AVAIL;
    if (up < sel)      return GAR_ST_NOT_AVAIL;
    if (up == sel)     return GAR_ST_PRICE;
    if (up == sel + 1) return GAR_ST_INSTALLED;
    return GAR_ST_SELL_PREV;
}

int garage_skin(const player_t *p, int car)
{
    int s;
    if (!p || !ok_car(car))
        return 0;
    s = p->car[car].up[GAR_SKIN];
    return s > 0 ? s : 0;
}

void garage_set_skin(player_t *p, int car, int skin)
{
    if (!p || !ok_car(car) || skin < 0)
        return;
    if (p->car[car].up[GAR_SKIN] != skin)
        p->dirty = 1;
    p->car[car].up[GAR_SKIN] = skin;
}

int garage_next_skin(int skin, int nskins)
{
    /* The engine's own line, modulo and all: `up[3] = (up[3] + 1) % nskins'
       (FUN_004d62a0). `nskins' below 1 would divide by zero there; it comes
       from the packed car and is 1 until one is loaded. */
    if (nskins < 1)
        return 0;
    if (skin < 0)
        skin = 0;
    return (skin + 1) % nskins;
}

/* ------------------------------------------------------------- what it says */

const char *garage_part_name(int kind, int car, int level)
{
    if (!ok_kind(kind) || !ok_car(car) || !ok_lvl(level))
        return STR_UI_PART_DEFAULT;
    return GAR_NAME[kind][car * GAR_N_LEVELS + (level - 1)];
}

const char *garage_part_info(int kind, int car, int level)
{
    if (!ok_kind(kind) || !ok_car(car) || !ok_lvl(level))
        return STR_UI_PART_DEFAULT;
    return GAR_INFO[kind][car * GAR_N_LEVELS + (level - 1)];
}

const char *garage_kind_name(int kind)
{
    return ok_kind(kind) ? GAR_KIND[kind] : "";
}

const char *garage_part_tex(int kind, int car, int level, char *out, int n)
{
    if (!out || n <= 0)
        return "";
    out[0] = 0;
    if (!ok_kind(kind) || !ok_car(car) || !ok_lvl(level))
        return out;
    /* LEVEL FIRST, CAR SECOND -- see garage.h. */
    snprintf(out, (size_t)n, "%s%d_%d", GAR_TEX[kind], level, car + 1);
    return out;
}

void garage_cash(char *out, int n, int cash)
{
    if (!out || n <= 0)
        return;
    snprintf(out, (size_t)n, "$%d", cash);
}

const char *garage_reason(gar_result r)
{
    switch (r) {
    case GAR_OK:         return "";
    /* The port's own, and the only one here that is: the original always has a
       profile by the time it can reach this screen and says nothing about not
       having one. It reuses the engine's word for the same shape of refusal. */
    case GAR_NO_PROFILE: return STR_UI_NOT_AVAILABLE;
    case GAR_NO_MONEY:   return STR_UI_NO_MONEY;
    case GAR_HAVE_IT:    return STR_UI_HAVE_UPGRADE;
    case GAR_BUY_PREV:   return STR_UI_BUY_PREV;
    case GAR_NO_UPGRADE: return STR_UI_NO_UPGRADE;
    case GAR_OWNED:      return STR_UI_HAVE_UPGRADE;
    case GAR_NOT_OWNED:  return STR_UI_NO_UPGRADE;
    case GAR_STUCK:      return STR_UI_CANT_SELL_CAR;
    default:             return "";
    }
}

/* ------------------------------------------------------------- what it does */

gar_result garage_can_buy_part(const player_t *p, int kind, int car, int sel)
{
    int up;

    if (!p)
        return GAR_NO_PROFILE;
    if (!ok_kind(kind) || !ok_car(car) || !ok_sel(sel))
        return GAR_NOT_OWNED;
    if (!garage_owns_car(p, car))
        return GAR_NOT_OWNED;
    up = garage_level(p, kind, car);
    /* EXACTLY ONE OF THE THREE IS BUYABLE, and it is the one the engine's own
       status text prints a price on: up == sel. Above it you already have the
       part, below it you have skipped one -- and skipping is what the retail
       exe charges for and does not deliver; see garage.h. */
    if (sel < up)
        return GAR_HAVE_IT;
    if (sel > up)
        return GAR_BUY_PREV;
    if (p->cash < garage_part_price(kind, car, sel + 1))
        return GAR_NO_MONEY;
    return GAR_OK;
}

gar_result garage_buy_part(player_t *p, int kind, int car, int sel)
{
    const gar_result r = garage_can_buy_part(p, kind, car, sel);
    if (r != GAR_OK)
        return r;
    p->cash -= garage_part_price(kind, car, sel + 1);
    p->car[car].up[kind] = sel + 1;
    p->dirty = 1;
    return GAR_OK;
}

gar_result garage_can_sell_part(const player_t *p, int kind, int car, int sel)
{
    int up;

    if (!p)
        return GAR_NO_PROFILE;
    if (!ok_kind(kind) || !ok_car(car) || !ok_sel(sel))
        return GAR_NOT_OWNED;
    if (!garage_owns_car(p, car))
        return GAR_NOT_OWNED;
    up = garage_level(p, kind, car);
    /* And exactly one is sellable: the one the status text marks INSTALLED. */
    if (up != sel + 1)
        return GAR_NO_UPGRADE;
    return GAR_OK;
}

gar_result garage_sell_part(player_t *p, int kind, int car, int sel)
{
    const gar_result r = garage_can_sell_part(p, kind, car, sel);
    if (r != GAR_OK)
        return r;
    p->cash += garage_part_sell(kind, car, sel + 1);
    p->car[car].up[kind] = sel;
    p->dirty = 1;
    return GAR_OK;
}

gar_result garage_can_buy_car(const player_t *p, int car)
{
    if (!p)
        return GAR_NO_PROFILE;
    if (!ok_car(car))
        return GAR_NOT_OWNED;
    if (garage_owns_car(p, car))
        return GAR_OWNED;
    if (p->cash < garage_car_price(car))
        return GAR_NO_MONEY;
    return GAR_OK;
}

gar_result garage_buy_car(player_t *p, int car)
{
    const gar_result r = garage_can_buy_car(p, car);
    int k;
    if (r != GAR_OK)
        return r;
    p->cash -= garage_car_price(car);
    p->car[car].enabled = 1;
    /* A CAR ARRIVES BARE. Nothing in the profile says otherwise -- a fresh one
       has 0/0/0/0 on the Overkill it comes with -- and the Garage's own
       "Current upgrades" block reads Default/Default/Default on it. The paint
       goes to 0 with them: skin 3 of a car you have not bought is not paint you
       kept, it is a number left over from the last time you owned one. */
    for (k = 0; k < 4; k++)
        p->car[car].up[k] = 0;
    p->dirty = 1;
    return GAR_OK;
}

gar_result garage_can_sell_car(const player_t *p, int car)
{
    int i, cheapest, after;

    if (!p)
        return GAR_NO_PROFILE;
    if (!ok_car(car))
        return GAR_NOT_OWNED;
    if (!garage_owns_car(p, car))
        return GAR_NOT_OWNED;
    for (i = 0; i < PL_N_CARS; i++)
        if (i != car && garage_owns_car(p, i))
            return GAR_OK;          /* another car in the garage -- fine */
    /* THE LAST CAR: allowed only if the money it fetches can buy the CHEAPEST
       car, the sold one included. FUN_004d5fb0 starts its running minimum at
       this car's own price and takes the min with the other two, then compares
       against cash + everything the sale fetches. */
    cheapest = garage_car_price(car);
    for (i = 0; i < PL_N_CARS; i++)
        if (garage_car_price(i) < cheapest)
            cheapest = garage_car_price(i);
    after = p->cash + garage_car_sell(p, car);
    return cheapest > after ? GAR_STUCK : GAR_OK;
}

gar_result garage_sell_car(player_t *p, int car)
{
    const gar_result r = garage_can_sell_car(p, car);
    int k;
    if (r != GAR_OK)
        return r;
    p->cash += garage_car_sell(p, car);
    p->car[car].enabled = 0;
    for (k = 0; k < 4; k++)
        p->car[car].up[k] = 0;
    p->dirty = 1;
    return GAR_OK;
}

int garage_next_owned_car(const player_t *p, int from)
{
    int i;
    if (!p)
        return -1;
    if (!ok_car(from))
        from = 0;
    for (i = 1; i <= PL_N_CARS; i++) {
        const int c = (from + i) % PL_N_CARS;
        if (garage_owns_car(p, c))
            return c;
    }
    return -1;
}
