/*
 * prop.h -- the knockable props: cans, bottles, traffic cones, buckets,
 * kettles, balloons, shell casings, balls and one tumbleweed.
 *
 * 129 of them across the ten tracks, and none of them existed in this port until
 * now: every track places them with MOD_INSTANCE nodes pointing at a SEPARATE
 * database (RCCarsDB/stone.sb), and pack_vsc.py flattens only the track's own
 * .sb, so the instances resolved to nothing and the geometry never entered the
 * .vsc. See "The dynamic layer" in CLAUDE.md.
 *
 * WHERE THE NUMBERS COME FROM. Almost all of them are the game's, and
 * prop_data.h says which is which:
 *
 *   mass, coeffHookNorm, coeffFrict, shiftCenterMassOY, fWindBlow
 *          stone.sb carries these as READABLE TEXT in a MOD_SCRIPT per model.
 *          No disassembly was needed. The values are sane real-world ones
 *          because these are real-world objects that a 1:10 scale RC car drives
 *          into: a whiskey bottle is 0.4 kg and 0.35 m tall, a traffic cone
 *          1.0 kg, a balloon 0.15 kg.
 *
 *   the collision proxy
 *          a sphere set per geometry type, read back from the type's own loader
 *          in RCCars.exe (0x4fa2e0-0x4fb2f0). Types 1 (sphere) and 3 (conus)
 *          have no loader at all, so those five models' proxies are FITTED to
 *          the shipped mesh -- see fit_proxy in gen_prop_data.py.
 *
 *   the placements
 *          each track's own Stones folder, position and heading.
 *
 * WHAT IS THE PORT'S, marked at the point of use below: the inertia tensor, the
 * contact solve, the sleep rule and the wind vector. The engine's own prop
 * integrator has not been located, so this is a small rigid body written to the
 * same conventions as rb.c rather than a transcription. It is bounded work --
 * a can that rolls plausibly is the whole requirement -- and nothing here can
 * move the car, deliberately:
 *
 * PROPS DO NOT PUSH BACK ON THE CAR. The car's handling is the transcribed
 * model and only that (CLAUDE.md, "Known issues"), and a reaction force would
 * perturb a system that has been checked against the disassembly line by line.
 * The heaviest prop is a 1.0 kg traffic cone against a 2 kg car whose top speed
 * is 7.5 m/s, so the honest error is small and it is in the direction of leaving
 * the recovered physics alone. One-way, and said so here rather than discovered
 * later.
 */

#ifndef PROP_H
#define PROP_H

#include "col.h"
#include "prop_data.h"
#include "rb.h"
#include "scene.h"

/* One placed object. `pos` is the CENTRE OF MASS, which is the model origin
   lifted by shiftCenterMassOY -- so the draw matrix has to put it back down
   again (see prop_matrix). Keeping the state on the COM is what makes the
   angular impulses correct without a separate offset everywhere. */
typedef struct {
    int   model;            /* index into PROP_MODELS */
    float pos[3];           /* centre of mass, world */
    float q[4];             /* orientation, (w, x, y, z), row-vector like rb.c */
    float v[3], w[3];       /* linear and angular velocity */
    float m[16];            /* body-to-world for drawing, rebuilt each step */
    float rest_t;           /* how long it has been slow enough to sleep */
    int   awake;
    int   touched;          /* the car is in contact this step -- telemetry */

    /* THE CAR JUST HIT IT. `hit` is an EDGE -- one step, on the transition from
       not-touching to touching -- and `hit_speed` is the closing speed along the
       contact normal that raised it, in m/s.

       The edge is the whole point. `touched` stays high for as long as the car
       leans on the object, which for a can wedged under a wheel is every frame
       for as long as the player holds it there; playing a sound off that is a
       machine gun. main.c reads `hit` and hands the model's own sound (see
       prop_data.h, out of stone.sb) to sfx_prop_hit. */
    int   hit;
    float hit_speed;
    int   was_touched;      /* previous step, for the edge */
} prop_t;

#define PROP_MAX_INSTANCES 32       /* the busiest track places 17 */

typedef struct {
    prop_t p[PROP_MAX_INSTANCES];
    int    n;
    const scene_t *scene;           /* props.vsc, or NULL to draw nothing */
    const col_t   *col;
    /* PROP_MODELS index -> the model index inside props.vsc, or -1. Resolved by
       NAME at init, never by position: prop_data.h and props.vsc are generated
       by two different scripts from the same file, and if one ever changes how
       it walks it, index N would silently pair a bottle's mesh with a kettle's
       mass and nothing would fail. */
    int    bind[PROP_N_MODELS];
    int    enabled;
    int    n_drawn;                 /* telemetry: instances drawn last frame */
    int    reset_track;             /* which PROP_TRACKS row prop_reset re-places from */
} props_t;

/* Gravity, matching rb.c's -- the game's 10.0 m/s^2, not 9.81. */
#define PROP_GRAVITY 10.0f

/*
 * THE PORT'S, all four:
 *
 * PROP_INERTIA_K   a solid sphere is 2/5 m r^2. The props are cans, bottles and
 *                  cones, so this is an approximation -- but an isotropic tensor
 *                  needs no rotation into world space each step, and for objects
 *                  this small the visible difference is how fast a can spins,
 *                  not whether the result looks right.
 * PROP_SLEEP_*     nothing recovered says when a prop stops. Without a sleep
 *                  rule a can on a slope creeps forever, which is the same
 *                  failure the car's own rest clamp exists to stop.
 * PROP_WIND        fWindBlow is a flag in stone.sb and the tumbleweed is the
 *                  only object carrying it; the wind VECTOR is not in any file
 *                  found so far. A gentle steady breeze is the smallest thing
 *                  that makes the flag mean something.
 * PROP_CAR_MASS    the car does not receive a reaction (see the header note),
 *                  but the impulse it DELIVERS still uses a two-body reduced
 *                  mass M/(m+M). Treating the car as infinitely heavy instead
 *                  makes the delivered speed (1+e)*v_closing for every prop --
 *                  the mass cancels exactly -- so a 1.0 kg traffic cone and a
 *                  0.15 kg balloon fly off at the SAME speed, which looks wrong
 *                  and is wrong. 2.0 kg is the car's own mass from rb_data.h.
 * PROP_ROLL_DAMP   rolling resistance, applied only while a prop is in contact.
 *                  Coulomb friction cannot stop a rolling sphere -- there is no
 *                  slip at the contact for it to act on -- so without this a can
 *                  on a beach slope accelerates forever and leaves the map.
 *                  Stone.ini declares `Friction` and `CoeffHook` and would have
 *                  been the place to get this from, but neither string is ever
 *                  pushed in the exe (CoeffHook is not even in the image), so
 *                  the retail game does not read them. Same class of dead key as
 *                  SpeedAngMaxREL.
 */
#define PROP_INERTIA_K     0.4f
#define PROP_SLEEP_V       0.12f    /* m/s */
#define PROP_SLEEP_W       0.6f     /* rad/s */
#define PROP_SLEEP_T       0.5f     /* seconds below both before it sleeps */
#define PROP_WIND_X        0.35f    /* m/s^2, only on models with fWindBlow */
#define PROP_WIND_Z        0.15f
#define PROP_CAR_MASS      2.0f
#define PROP_ROLL_DAMP     2.5f     /* 1/s, while touching the ground */

/* Beyond this the instance is neither stepped nor drawn. The chase camera sits
   0.79 m behind the car and the props are 0.1-0.5 m objects, so anything this
   far away is a handful of pixels; stepping 129 rigid bodies a frame when at
   most a few are near the car is the part worth not doing. */
#define PROP_ACTIVE_RANGE  60.0f

/* How long prop_init drops the props for before parking them. The authored
   placements float ~0.5 m above the terrain (see prop_init), so they have to be
   let fall once at load or they hang there for the whole race. 2 s at 120 Hz is
   ample -- a 1.6 m fall takes 0.57 s at this gravity -- and they sleep on their
   own long before the cap. */
#define PROP_SETTLE_STEPS  240
#define PROP_SETTLE_DT     (1.f / 120.f)

/*
 * Place one track's props at their authored positions.
 *
 * `props_scene` is props.vsc (VSC8, all 13 models in one file) and may be NULL,
 * in which case nothing draws but the physics still runs -- which is what the
 * host harness uses. `track` indexes PROP_TRACKS.
 */
void prop_init(props_t *pr, const scene_t *props_scene, const col_t *col,
               int track);

/* Back to the authored placement, asleep. Called on respawn and on a restart. */
void prop_reset(props_t *pr);

/* One step. `car` may be NULL, and then nothing pushes the props but gravity. */
void prop_step(props_t *pr, const rb_car *car, float dt);

/* Draw every awake-or-not instance within PROP_ACTIVE_RANGE of `eye`. */
void prop_draw(props_t *pr, const float eye[3]);

/* The body-to-world matrix for one instance, row-vector, ready for
   glMultMatrixf -- the model origin, not the centre of mass. */
void prop_matrix(const prop_t *p, float out[16]);

#endif /* PROP_H */
