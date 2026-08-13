/*
 * carparts.h -- show the upgrade parts the player has picked.
 *
 * RC Cars models two of its three upgrades on the car itself, and it does them
 * two different ways:
 *
 *   BOOSTER (the exhaust)   four sibling MESH subtrees, each holding a
 *                           booster_<n> / booster_<n>_body / compressor_<n>
 *                           group. Exactly one is meant to be visible.
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
 * Binding is by DATA, not by a per-car table -- but WHICH datum matters, and
 * this file had the wrong one for the booster. The level came off the texture
 * name's trailing _<n>, and THE TEXTURE SUFFIX IS NOT THE LEVEL:
 *
 *     level    Overkill           Buggy      Hummer
 *       1      overkill_turbo_1   turbo_1    hum_turbo_3
 *       2      overkill_turbo_3   turbo_2    hum_turbo_4
 *       3      overkill_turbo_4   turbo_3    hum_turbo_2
 *       4      overkill_turbo_2   turbo_4    hum_turbo_1
 *
 * Only the Buggy agrees, so picking level 4 on the Overkill drew the level-3
 * exhaust and on the Hummer the level-2 one. Reported as "level 4 exhaust model
 * actually not level 4".
 *
 * The level is the group the mesh lives in, and the engine says so: FUN_0050c163
 * walks the level index over three tables of NODE names at 0x00573914,
 * 0x00573934 and 0x00573924 -- booster_<n>, compressor_<n>, booster_<n>_end --
 * and never looks at a texture. pack_vsc.py now keeps booster_<n> as a PART, so
 * carparts_bind reads the level off the part name. `booster_<n>` rather than the
 * group above it because the group is spelled three different ways across the
 * three cars (UPGRADES<n>, UPGRADE<n>, Upgrades<n>) and booster_<n> is not.
 *
 * fx.c was right all along: it aims the smoke from booster_<n>_end, the third
 * table above, so the exhaust FLAME came out of the correct pipe while the
 * wrong pipe was drawn.
 *
 * The tyre side was and is correct: the family comes from stripping the trailing
 * _<n> off whichever tire texture the artist left assigned (Car1 ships tire3_3,
 * Car2 tire2_3, Car3 tire3_1), and the suffix there really is the level -- the
 * exe's own pointer tables at 0x005738ec and 0x00573900 list tire<f>, tire<f>_1,
 * ... tire<f>_4 in ascending order, and FUN_0050c141 skips the first entry so
 * index 0 is _1. (The STRINGS are stored descending at 0x1739f4 / 0x173a1c;
 * only the pointer table's order counts.)
 */

#ifndef CARPARTS_H
#define CARPARTS_H

#include "scene.h"

#define CARPARTS_LEVELS 4
#define CARPARTS_MAX_WHEEL_BATCHES 12
/* An exhaust is more than one batch: booster_<n> and compressor_<n> are separate
   parts, so a level is at least two, and a group whose meshes do not all share a
   texture would be more. Sized well clear of the 8 the three cars use. */
#define CARPARTS_MAX_BOOST_BATCHES 16

typedef struct {
    /* Every exhaust batch on the car, each tagged with the level it belongs to.
       A flat list rather than one slot per level, because a level is made of
       several batches and which ones is a property of the model. */
    int          boost_batch[CARPARTS_MAX_BOOST_BATCHES];
    unsigned int boost_nidx[CARPARTS_MAX_BOOST_BATCHES];  /* real count, to unhide */
    int          boost_level[CARPARTS_MAX_BOOST_BATCHES]; /* 0..CARPARTS_LEVELS-1 */
    int          n_boost;
    int          n_at_level[CARPARTS_LEVELS];   /* 0 = this car has no such group */
    /* 1 = the levels came from the `booster_<n>` / `compressor_<n>` PART names,
       which is the engine's own key and the only correct one. 0 = this car was
       packed before those were parts, so the levels came off the texture suffix
       -- a different numbering on every car but the Buggy. Repack to fix. */
    int          by_part;

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
