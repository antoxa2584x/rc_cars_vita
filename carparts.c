/*
 * carparts.c -- see carparts.h.
 */

#include "carparts.h"

#include <stdio.h>
#include <string.h>

/* "booster_3" -> 3, "compressor_1" -> 1, else 0. THE PART name, which is the
   level. Both prefixes, because an exhaust is both: the engine walks its level
   index over a table of each (0x00573914 and 0x00573934).

   Requiring the digit to be the LAST character is what rejects "booster_3_end"
   (the pipe tip fx.c aims the smoke from, a third table at 0x00573924) and
   "booster_12_body" (a cap mesh, numbered in its own sequence). */
static int booster_part_level(const char *name)
{
    size_t n = strlen(name), pre = 0;

    if (strncmp(name, "booster_", 8) == 0)
        pre = 8;
    else if (strncmp(name, "compressor_", 11) == 0)
        pre = 11;
    else
        return 0;

    if (n != pre + 1)                 /* prefix + exactly one digit */
        return 0;
    if (name[pre] < '1' || name[pre] > '0' + CARPARTS_LEVELS)
        return 0;
    return name[pre] - '0';
}

/* "overkill_turbo_3" -> 3, "turbo_1" -> 1, "hum_turbo_4" -> 4, else 0.
   Requires the name to actually contain "turbo", so a stray tire3_3 or
   car_askin11 can never be mistaken for an exhaust.

   THE SUFFIX IS NOT THE LEVEL -- see the table in pack_vsc.py's CAR_PARTS_EXTRA
   comment; it agrees with the level only on the Buggy. This is kept solely to
   RECOGNISE an exhaust batch in a car packed before `booster_<n>` became a
   part, where there is nothing better to go on. Ordering such a car by suffix
   is what the port used to do everywhere, and it is wrong on two cars out of
   three; the fallback is a way to keep an old asset drawing an exhaust at all,
   not a way to get the right one. */
static int turbo_tex_suffix(const char *name)
{
    size_t n = strlen(name);
    if (!strstr(name, "turbo"))
        return 0;
    if (n < 2 || name[n - 2] != '_')
        return 0;
    if (name[n - 1] < '1' || name[n - 1] > '0' + CARPARTS_LEVELS)
        return 0;
    return name[n - 1] - '0';
}

/* "tire3_3" -> "tire3". Returns 0 if this is not a tire texture. */
static int tire_family(const char *name, char *out, size_t out_n)
{
    size_t n = strlen(name);
    if (strncmp(name, "tire", 4) != 0)
        return 0;
    if (n >= 2 && name[n - 2] == '_' && name[n - 1] >= '0' && name[n - 1] <= '9')
        n -= 2;
    if (n == 0 || n >= out_n)
        return 0;
    memcpy(out, name, n);
    out[n] = 0;
    return 1;
}

void carparts_bind(carparts_t *p, const scene_t *car)
{
    unsigned int i;
    int k;

    memset(p, 0, sizeof(*p));

    if (!car || !car->n_batches || !car->tex_names)
        return;

    /* THE LEVEL IS THE PART, NOT THE TEXTURE. `booster_<n>` is the node the
       engine's own upgrade loop looks up by name -- FUN_0050c163 walks the level
       index over the table at 0x00573914 -- and n IS the level. The texture
       suffix is an unrelated numbering that agrees with it only on the Buggy;
       the table is in pack_vsc.py's CAR_PARTS_EXTRA comment.

       Decided ONCE, over the whole car, before anything is keyed. Deciding it
       per batch inside the loop would make the answer depend on which exhaust
       came first in the file: the batches before the first part-named one would
       already have been filed under a suffix meaning a different level. */
    p->by_part = 0;
    if (car->has_rig) {
        for (i = 0; i < car->n_batches; i++) {
            const batch_t *b = &car->batches[i];
            if (b->part < (unsigned)car->rig.n
                && booster_part_level(car->rig.part[b->part].name)) {
                p->by_part = 1;
                break;
            }
        }
    }

    for (i = 0; i < car->n_batches; i++) {
        const batch_t *b = &car->batches[i];
        const char *tn;
        int lvl;

        if (b->tex >= car->n_tex)
            continue;
        tn = car->tex_names[b->tex];

        /* A car packed before `booster_<n>` became a part has every exhaust on
           __root__, and then the suffix is all there is. That fallback is wrong
           on two cars out of three -- it is here to keep an old asset drawing an
           exhaust at all, not to get the right one. */
        if (p->by_part) {
            lvl = (b->part < (unsigned)car->rig.n)
                  ? booster_part_level(car->rig.part[b->part].name) : 0;
        } else {
            lvl = turbo_tex_suffix(tn);
        }
        if (lvl) {
            if (p->n_boost < CARPARTS_MAX_BOOST_BATCHES) {
                p->boost_batch[p->n_boost] = (int)i;
                p->boost_nidx[p->n_boost] = b->nidx;
                p->boost_level[p->n_boost] = lvl - 1;
                p->n_boost++;
                p->n_at_level[lvl - 1]++;
            }
            continue;
        }

        if (!p->family[0] && tire_family(tn, p->family, sizeof(p->family))) {
            /* first tire texture seen names the family for the whole car */
        }
        if (p->family[0] && strncmp(tn, p->family, strlen(p->family)) == 0
            && p->n_wheel < CARPARTS_MAX_WHEEL_BATCHES) {
            p->wheel_batch[p->n_wheel++] = (int)i;
        }
    }

    /* One texture per tyre level, if it was packed. Missing levels stay 0 and
       carparts_apply then leaves the wheels on whatever is baked in, so this
       works with a car packed before the extra tyre textures were added. */
    if (p->family[0]) {
        for (k = 0; k < CARPARTS_LEVELS; k++) {
            char nm[32];
            snprintf(nm, sizeof(nm), "%s_%d", p->family, k + 1);
            p->tire_tex[k] = scene_tex(car, nm);
            if (p->tire_tex[k])
                p->n_tire++;
        }
    }
}

void carparts_apply(carparts_t *p, scene_t *car, int tires, int booster)
{
    int k, i;

    if (!car || !car->n_batches)
        return;

    if (booster < 0) booster = 0;
    if (booster >= CARPARTS_LEVELS) booster = CARPARTS_LEVELS - 1;
    if (tires < 0) tires = 0;
    if (tires >= CARPARTS_LEVELS) tires = CARPARTS_LEVELS - 1;

    /* Exactly one exhaust. Hiding is a zero index count, so scene_draw's
       glDrawElements draws nothing and no other file has to know.

       The exhausts are also the only geometry on the car that is genuinely
       half-transparent, so they are marked BATCH_TRANSLUCENT here and main.c
       draws them blended after the rest of the body. Marking every level, not
       just the visible one, keeps the flag a property of the batch. */
    for (k = 0; k < p->n_boost; k++) {
        int b = p->boost_batch[k];
        if (b < 0 || (unsigned)b >= car->n_batches)
            continue;
        car->batches[b].nidx = (p->boost_level[k] == booster)
                               ? p->boost_nidx[k] : 0;
        car->batches[b].flags |= BATCH_TRANSLUCENT;
    }

    /* If this car has no group for the chosen level, fall back to the highest
       one it does have rather than showing no exhaust at all. Every batch of
       that level, not just the first -- an exhaust is a booster and a
       compressor, and showing half of one looks like a broken model. */
    if (p->n_boost && !p->n_at_level[booster]) {
        int use = -1;
        for (k = CARPARTS_LEVELS - 1; k >= 0; k--) {
            if (p->n_at_level[k]) { use = k; break; }
        }
        for (k = 0; use >= 0 && k < p->n_boost; k++) {
            int b = p->boost_batch[k];
            if (p->boost_level[k] == use && b >= 0
                && (unsigned)b < car->n_batches)
                car->batches[b].nidx = p->boost_nidx[k];
        }
    }

    if (p->tire_tex[tires]) {
        for (i = 0; i < p->n_wheel; i++) {
            int b = p->wheel_batch[i];
            if ((unsigned)b < car->n_batches)
                car->batches[b].gl_tex = p->tire_tex[tires];
        }
    }
}
