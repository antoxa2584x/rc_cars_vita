/*
 * envmap.h -- the car body's plastic glance.
 *
 * This is the game's own environment map, and it is not an overlay: the engine
 * flags geometry the car model already has.
 *
 *   physCreateSubsystem (0x004fcce0) looks the body up by name --
 *   "ENVIR_CAR_BODY_body", falling back to "ENVIR_CAR_BODY" -- and writes
 *   `0xffffff | EnvMapBody << 24` into its colour: white, at that alpha.
 *
 *   FUN_00507130 does the same lookup for the three upgrade instances, then
 *   hands carVisualHelperB all four CarEnvMap alphas at once -- EnvMapBody,
 *   EnvMapGRE1, EnvMapGRE2, EnvMapUpgrades -- and carVisualHelperA applies them
 *   across the instance. carVisualWheels (0x00507ba0) does the wheels, gated on
 *   Video/VIDEO_EnvMap.
 *
 * So the classes are node-name classes, and the names are all over Car.sb:
 * GLASSES_GRE1, FARI_GRE2 (headlights), BAMP_*_GRE2 (bumpers), Spring<n>_1_GRE1
 * (the chrome springs). pack_vsc.py --envmap resolves them at pack time into a
 * per-batch class and packs that batch's vertex normals; see ENV_* in scene.h.
 *
 * Two things are the port's, both because the engine's own answer is a runtime
 * texture and a D3D8 texture stage rather than data:
 *
 *   - the SOURCE. Car.sb declares a texture called S_SKY and it is the only
 *     declaration in the whole database with no source path, so the engine fills
 *     it in at runtime. The port binds the track's own sky texture, which is
 *     what S_SKY names.
 *   - the UVs. D3D8 generates them from the camera-space normal
 *     (D3DTSS_TCI_CAMERASPACENORMAL); vitaGL has no texgen, so envmap_draw
 *     computes the same thing on the CPU -- u = 0.5 + nx/2, v = 0.5 - ny/2 off
 *     the view-space normal. A few hundred vertices per car.
 *
 * The alphas themselves are the game's (fx_data.h, from FUN_004f9aa0): body
 * 112/255, GRE1 255/255, GRE2 137/255, the bolt-on exhaust 156/255.
 */

#ifndef ENVMAP_H
#define ENVMAP_H

#include "scene.h"

typedef struct {
    GLuint tex;             /* the track's sky -- the engine's S_SKY slot */
    int enabled;
    int n_batches;          /* drawn last frame, for telemetry */
    int n_tris;
} envmap_t;

/* `track` supplies the sky. Call again when the track changes. */
void envmap_init(envmap_t *e, const scene_t *track);

/* Draw the glance over a car already drawn. `n3` is the 3x3 that takes a
   MODEL-space normal into VIEW space, row-vector like everything else here: the
   caller has it, because it is the same product it built the modelview from.
   The modelview must still be the car's when this is called. */
void envmap_draw(envmap_t *e, const scene_t *car, const float n3[9]);

/* The alpha a class is drawn at, 0..1. Split out so the test can check the four
   against the recovered bytes without a GL context. */
float envmap_alpha(unsigned int env_class);

#endif
