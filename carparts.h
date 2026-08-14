/*
 * carparts.h -- show the upgrade parts the player has picked, and the skin.
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
 * THE SKIN is not an upgrade at all -- it is the car's paint, four per car -- but
 * it is the same retexturing operation as the tyres, on the same scene, so it
 * lives here rather than in a file of its own.
 *
 * The engine BUILDS the names instead of tabling them. FUN_0049fc80 validates
 * 0 <= car < 3 and 0 <= skin < 4, sprintf's "car_askin%i%i" and "car_bskin%i%i"
 * from (car+1, skin+1), resolves both through the texture-by-name call at
 * 0x0046e180 -- the one fx.c gets "dust" from -- and returns 0 unless it found
 * what that car needs. FUN_0050bf90, the same function that switches the exhaust
 * groups, hands each result to FUN_005352a0, which walks the model and re-points
 * every texture whose name STARTS WITH the prefix it was given: `car_askin` and
 * `car_bskin`, no digits (the comparison is a strncmp over strlen(prefix), at
 * 0x0053533c). So a repaint is a PREFIX match over the model's own texture refs,
 * which is how Car.sb can ship wearing `car_askin11` and still be repainted by a
 * name no mesh in it has ever referenced.
 *
 * TWO PAGES, AND CAR 1 HAS ONLY ONE. FUN_0049fc80's switch loads both pages for
 * cars 2 and 3 and writes NULL for the second on car 1; the shipped art says the
 * same thing independently, there being no car_bskin1<n> in any of the three
 * texture sets. Neither fact is inferred from the other, and this file needs no
 * car index to know it -- a car has the pages its own meshes reference.
 *
 * WHAT A SKIN COVERS, measured on the packed cars rather than assumed: the
 * painted shell (ENV_BODY), the glass and chrome (ENV_GRE1), the Hummer's
 * lamps/bumpers class (ENV_GRE2), and the ANTENNA -- whose whip is textured off
 * the same atlas on the Buggy and the Hummer and carries no env class at all.
 * 536 / 505 / 792 triangles of 3778 / 3631 / 3890. It is paint and trim; it moves
 * no geometry, which is why nothing outside this file has to know a skin changed.
 *
 * It does NOT cover the exhausts. Those are ENV_UPGRADES and wear their own
 * <prefix>turbo_<n> atlases, so the booster row and this one never fight over a
 * batch -- and the per-car detail atlases (overkill_dviglo, overkill_mashi,
 * mashineriya) are env-mapped too and are not paint either. So "env-mapped"
 * implies nothing about being painted; the implication runs the other way, and
 * only that direction is worth asserting (carparts_test does, using the env
 * field, which pack_vsc.py fills from ENVIR_CAR_BODY NODE names -- a different
 * rule from the texture names this file keys on, hence a real cross-check).
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

/* Skins, and the two atlas pages one is made of. Both are the engine's own
   numbers, not room to grow: FUN_0049fc80 rejects a skin index outside 0..3 and
   there is no third prefix anywhere in the image. */
#define CARPARTS_SKINS 4
#define CARPARTS_SKIN_PAGES 2
/* The painted batches. Six on the Hummer, the most of the three. */
#define CARPARTS_MAX_SKIN_BATCHES 16

/* One atlas page -- `car_askin` or `car_bskin` -- as this car uses it. A car that
   references only the first leaves the second's n_batch at 0, and nothing about
   it is then looked at. */
typedef struct {
    int          batch[CARPARTS_MAX_SKIN_BATCHES];
    int          n_batch;
    GLuint       tex[CARPARTS_SKINS];       /* 0 = not packed for this skin */
    /* "car_askin1" -- the prefix plus this car's own digit, taken from whichever
       skin texture the model ships wearing. The engine derives it from the car
       index; there is no car index here, and the model already says. */
    char         family[24];
} carparts_skin_page;

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

    /* the paint. Page 0 is `car_askin`, page 1 `car_bskin`. */
    carparts_skin_page skin[CARPARTS_SKIN_PAGES];
    /* How many skins this car can actually be given, counted from 1 UPWARD and
       stopping at the first one any painted page is missing. Consecutive rather
       than a count of what is present, so the menu can wrap over 0..n_skin-1 with
       no gaps to map around, and so a half-packed car degrades to the skins it
       really has instead of to a hole. Always at least 1 on a car with paint --
       the model ships wearing skin 1 -- and 0 on a scene with none. */
    int          n_skin;
} carparts_t;

/* Resolve the parts of an already-loaded car scene. Safe on a scene with none;
   everything then reports zero and carparts_apply does nothing. */
void carparts_bind(carparts_t *p, const scene_t *car);

/* Show `booster` (0..3), put level `tires` (0..3) on the wheels and `skin`
   (0..CARPARTS_SKINS-1) on the paint. Levels and skins with no geometry or no
   texture are left as they are rather than blanked. */
void carparts_apply(carparts_t *p, scene_t *car, int tires, int booster,
                    int skin);

#endif
