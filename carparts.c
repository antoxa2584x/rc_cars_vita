/*
 * carparts.c -- see carparts.h.
 */

#include "carparts.h"

#include <stdio.h>
#include <string.h>

/* "overkill_turbo_3" -> 3, "turbo_1" -> 1, "hum_turbo_4" -> 4, else 0.
   Requires the name to actually contain "turbo", so a stray tire3_3 or
   car_askin11 can never be mistaken for an exhaust level. */
static int turbo_level(const char *name)
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
    for (k = 0; k < CARPARTS_LEVELS; k++)
        p->boost_batch[k] = -1;

    if (!car || !car->n_batches || !car->tex_names)
        return;

    for (i = 0; i < car->n_batches; i++) {
        const batch_t *b = &car->batches[i];
        const char *tn;
        int lvl;

        if (b->tex >= car->n_tex)
            continue;
        tn = car->tex_names[b->tex];

        lvl = turbo_level(tn);
        if (lvl) {
            p->boost_batch[lvl - 1] = (int)i;
            p->boost_nidx[lvl - 1] = b->nidx;
            p->n_boost++;
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
    for (k = 0; k < CARPARTS_LEVELS; k++) {
        int b = p->boost_batch[k];
        if (b < 0 || (unsigned)b >= car->n_batches)
            continue;
        car->batches[b].nidx = (k == booster) ? p->boost_nidx[k] : 0;
        car->batches[b].flags |= BATCH_TRANSLUCENT;
    }

    /* If this car has no group for the chosen level, fall back to the highest
       one it does have rather than showing no exhaust at all. */
    if (p->n_boost && p->boost_batch[booster] < 0) {
        for (k = CARPARTS_LEVELS - 1; k >= 0; k--) {
            int b = p->boost_batch[k];
            if (b >= 0 && (unsigned)b < car->n_batches) {
                car->batches[b].nidx = p->boost_nidx[k];
                break;
            }
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
