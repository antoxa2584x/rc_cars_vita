/*
 * carparts.h -- show the upgrade parts the player has picked.
 *
 * RC Cars models two of its three upgrades on the car itself, and it does them
 * two different ways:
 *
 *   BOOSTER (the exhaust)   four sibling MESH subtrees, UPGRADES1..UPGRADES4,
 *                           each holding a booster_<n> / booster_<n>_body /
 *                           compressor_<n> group with its own <prefix>turbo_<n>
 *                           texture. Exactly one is meant to be visible.
 *   TIRES                   one wheel mesh, retextured. The exe carries the full
 *                           set as two string tables (0x1739f4, 0x173a1c):
 *                           tire2, tire2_1..4 and tire3, tire3_1..4.
 *
 * The RESONATOR has no visual -- it is the only one of the three with no mesh or
 * texture family behind it in Car.sb.
 *
 * WHY THIS EXISTS AT ALL: pack_vsc.py flattens the whole car subtree, so all
 * four UPGRADES groups end up in the scene and every one of them draws. Before
 * this, each car was rendering its four exhausts stacked on top of each other --
 * 662 overlapping triangles on the Overkill (batches 19-22 of car1.vsc).
 *
 * Nothing here needs a change to scene.c. A batch is hidden by zeroing its index
 * count, which makes scene_draw's glDrawElements a no-op, and the real count is
 * kept so it can be shown again.
 *
 * Binding is by DATA, not by a per-car table: the booster level comes from the
 * texture name's trailing _<n> (which covers overkill_turbo_<n>, turbo_<n> and
 * hum_turbo_<n> alike), and the tyre family comes from stripping the trailing
 * _<n> off whichever tire texture the artist happened to leave assigned (Car1
 * ships tire3_3, Car2 tire2_3, Car3 tire3_1).
 */

#ifndef CARPARTS_H
#define CARPARTS_H

#include "scene.h"

#define CARPARTS_LEVELS 4
#define CARPARTS_MAX_WHEEL_BATCHES 12

typedef struct {
    /* the exhaust group for each level, -1 when this car has no such group */
    int          boost_batch[CARPARTS_LEVELS];
    unsigned int boost_nidx[CARPARTS_LEVELS];   /* real count, for un-hiding */
    int          n_boost;

    /* the wheel batches, and the texture to give them at each level */
    int          wheel_batch[CARPARTS_MAX_WHEEL_BATCHES];
    int          n_wheel;
    GLuint       tire_tex[CARPARTS_LEVELS];     /* 0 = not packed for this level */
    int          n_tire;

    char         family[24];                    /* "tire2" / "tire3", for logs */
} carparts_t;

/* Resolve the parts of an already-loaded car scene. Safe on a scene with none;
   everything then reports zero and carparts_apply does nothing. */
void carparts_bind(carparts_t *p, const scene_t *car);

/* Show `booster` (0..3) and put level `tires` (0..3) on the wheels. Levels with
   no geometry or no texture are left as they are rather than blanked. */
void carparts_apply(carparts_t *p, scene_t *car, int tires, int booster);

#endif
