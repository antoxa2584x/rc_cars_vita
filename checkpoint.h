/*
 * checkpoint.h -- the track's checkpoints and the arrow that marks the next one.
 *
 * A checkpoint in RC Cars is NOT a gate. FUN_004e9560 loads the folder
 * "Checkpoints" and reads cp_1, cp_2, ... ; each cp_N owns a chain of refining
 * points cp_N_1, cp_N_2, ... and the loader stitches
 *
 *     cp_N -> cp_N_1 -> ... -> cp_N_M -> cp_(N+1)
 *
 * into one closed polyline, accumulating a running length as it goes (the
 * DISTTOCP / DISTALL the network code prints). So the checkpoints are the
 * track's spine: waypoints, with intermediate points to bend the line between
 * them. beach_1 has four of them and 15 refining points.
 *
 * EVERY checkpoint is marked. FUN_0052b170 walks the registered list and tints
 * each one, and FUN_0052b1d0 hands back a flat alpha 50 for any checkpoint that
 * is not the current one; only the current one gets the breathing 50..220.
 * FUN_0052abc0 then draws the billboard for the current one over the top.
 *
 *     centre = mean of that checkpoint object's vertices -- which for a gate
 *              box centred on the marker is the marker's own position, and the
 *              marker is what the port has
 *     up     = world up, right = perpendicular to (centre - camera)
 *     quad   = centre + (0, Shift, 0) +/- Size on both axes
 *     texture= cp_ar_3_fN when the index is 0, cp_ar_2_fN otherwise --
 *              FUN_0052a9b0 calls them the "custom" and "common" arrows, and
 *              index 0 is the start/finish, so: red over the finish line, green
 *              over the rest. N = time/BlinkDelta % 3.
 *     alpha  = blink * k, where k ramps 0 -> 1 between MinDist and MaxDist and
 *              STAYS 1 beyond -- the marker is invisible up close and fully on
 *              far away, and is never culled for distance
 *
 * Every constant comes from anim_cp via vis_data.h.
 *
 * Two things are NOT transcribed:
 *
 *   - the progression rule. The game tracks laps, wrong way and the rest
 *     through its race module; here it is arc length along the spine above,
 *     which is the same quantity the original measures progress with. Marked
 *     where it happens.
 *   - FUN_0052b170, which tints the checkpoint GATE OBJECTS by the same
 *     distance fade before the arrow is drawn. The port has no gate objects --
 *     the tracks carry the checkpoints as markers only -- so there is nothing
 *     to tint.
 *
 * WHEN A CHECKPOINT IS PASSED, and why this file got it wrong for a long time.
 * `next` used to advance the moment the car's nearest SPINE SAMPLE changed
 * owner, i.e. at the midpoint between checkpoint N's last refining point and
 * checkpoint N+1 -- a place with no relation to either of them. On the fixture
 * that is 5 m early; on a real track it is wherever the artist happened to stop
 * refining, which is why the cue "triggers somewhere in different places". The
 * arrow flipped to the checkpoint AFTER the one the player was still driving at.
 *
 * It advances on a CROSSING now: the spine gives every checkpoint an arc-length
 * station (cum[k][0]), the car's own progress along the spine is projected onto
 * the nearest spine SEGMENT so it is continuous rather than snapped to a sample,
 * and `passed` is raised on the step where that progress sweeps past the station
 * of the checkpoint being headed for. So the event happens AT the checkpoint,
 * to within one frame of travel.
 *
 * Deliberately NOT a radius around the marker, which is the obvious rule and is
 * the one this file's own comment below warns about: the waypoints sit 30 to
 * 90 m apart and a car taking a wide line never comes inside any sensible
 * radius of one, so the arrow would stick on a checkpoint already behind the
 * player. An arc-length station has no width to miss.
 */

#ifndef CHECKPOINT_H
#define CHECKPOINT_H

#include "col.h"
#include "scene.h"

/* The pulse half-period, from FUN_0052b1d0: the phase runs 0 -> 0.4 and the
   alpha 50 -> 250 with it. */
#define CP_PULSE_TIME 0.4f

/* Flat alpha for a checkpoint that is NOT the one being headed for.
   FUN_0052b1d0 opens with `cmp` on its two index arguments and `mov $0x32,%al`
   when they differ -- 0x32 is 50 -- and FUN_0052b170 applies it across the whole
   registered list. Every checkpoint is marked; only the current one breathes. */
#define CP_ALPHA_OTHER 140.f

/* Two PORT ADJUSTMENTS to the marker's placement, both driven by how it looked
   on screen rather than by anything recovered. Stated here so they are easy to
   find and easy to argue with.
 *
 * CP_SIZE_SCALE: FUN_0052abc0 builds the quad as centre +/- Size on both axes,
 * so Size 1.46 is a HALF-extent and the sprite is 2.92 m tall. That is seven
 * car lengths on a 0.42 m car and it towered. Halving it gives a 1.46 m marker.
 * The original's anchor is the mean of the checkpoint GATE OBJECT's vertices --
 * an object these tracks do not carry, since they store checkpoints as markers
 * only -- so the reference this was tuned against does not exist here.
 *
 * CP_GROUND: anchor the quad's BOTTOM on the terrain under the marker instead
 * of centring it at marker + Shift. The markers themselves float 0.18 to 0.49 m
 * above the ground, and Shift then lifted the whole sprite again.
 *
 * The recovered constants are untouched in vis_data.h; this is where they get
 * bent. */
#define CP_SIZE_SCALE 0.5f
#define CP_GROUND 1

/* The most spine metres one cp_step call may plausibly cover. Past that it is a
   teleport -- a respawn, a drowning, a jump-reset -- or the projection swapping
   legs where the track runs back alongside itself; neither is a crossing, so the
   cursor RESYNCS instead of firing. Generous by a factor of five against
   driving (main.c clamps a frame at 0.1 s and the momentum clamp caps the car
   at 20 m/s, so 2 m is the worst real step) and still far short of the 30 to
   90 m between two checkpoints on these tracks, so a real crossing can never be
   mistaken for a jump. */
#define CP_MAX_STEP 10.f

#define CP_MAX 8
#define CP_MAX_POINTS 33          /* cp_N plus up to 32 edges, per the loader */

typedef struct {
    /* p[0] is cp_N itself -- the checkpoint, and where the arrow goes. p[1..]
       are its cp_N_M refining points, which bend the spine on the way to the
       next checkpoint and are only used for progression and distance. */
    float p[CP_MAX_POINTS][3];
    int n;
    float ground;                 /* terrain height under p[0], for the marker */
} cp_t;

typedef struct {
    cp_t cp[CP_MAX];
    int n;
    /* Cumulative arc length along the stitched CLOSED spine at each point, and
     * its total. Built once by cp_init in the loader's own order --
     * cp_0, cp_0_1..., cp_1, cp_1_1..., wrapping back to cp_0 -- which is the
     * polyline FUN_004e9560 accumulates its running length over.
     *
     * This exists for the AI. The rubber band compares an opponent's progress
     * with the player's, and the original measures both as distance along this
     * spine (FUN_004ea120 -> FUN_004eb630 = spine_len * (lap - 1) + this). */
    float cum[CP_MAX][CP_MAX_POINTS];
    float spine_len;
    int next;                     /* the checkpoint being headed for */
    int lap;

    /* Where the car is along the spine, in metres from cp_0, and whether that
       is a usable previous value yet. Continuous -- projected onto the nearest
       spine SEGMENT -- because a crossing test needs sub-sample resolution.
       `have_s` is cleared by cp_init and by cp_resync so the first step after a
       load or a teleport syncs instead of firing. */
    float s;
    int have_s;

    /* THE CAR JUST PASSED A CHECKPOINT: its index, or -1 on every other step.
       An EDGE, the way prop_t.hit is one, and for the same reason -- the host
       hangs one cue on it and must not get a second one per frame for as long as
       the car is near. cp_step writes it on every call, so a host that reads it
       after cp_step sees exactly the crossings that happened. */
    int passed;

    /* THE LAST CHECKPOINT ACTUALLY CROSSED, or -1 if none has been since the
     * race started. `passed` LATCHED, in other words -- and it is a separate
     * field rather than something the host derives from `next`, because the two
     * are not the same question and the difference bites exactly where it
     * matters.
     *
     * `next` is "the first station ahead of where the car IS", which cp_resync
     * recomputes from a position. At the grid that answer depends on which side
     * of cp_0's seam the start marker happens to fall -- the seam is at arc
     * length 0 == spine_len -- so `next - 1` is checkpoint 0 on one side and the
     * LAST checkpoint on the other, half a lap away. A car that has driven
     * nothing must not be sent half a lap backwards on its first drowning, so
     * the respawn point is the crossing the car really made, latched here, and
     * -1 says "none yet, use the grid".
     *
     * cp_resync deliberately does NOT clear it: a teleport does not un-pass a
     * checkpoint, which is the same reason cp_resync keeps the lap. cp_restart
     * clears both.
     *
     * `next` is never `last` after a respawn ONTO a station, and that falls out of
     * cp_progress's arithmetic rather than being enforced -- see the comment on
     * cp_ahead, which also records the guard that used to be there and why it came
     * out. Measured 0 of 50 on the shipped tracks and asserted in vis_test. */
    int last;

    float t;
    GLuint tex_common[3];         /* cp_ar_2_f1..3, green, ordinary checkpoints */
    GLuint tex_custom[3];         /* cp_ar_3_f1..3, red, the start/finish line */
    int enabled;
} checkpoints_t;

/* Read the cp_* markers out of the scene and pick up the arrow textures. `col`
   may be NULL, in which case the marker's own y is used instead of the ground
   under it. */
void cp_init(checkpoints_t *c, const scene_t *scene, const col_t *col);

/* Advance the progression from the car's position, and raise `passed` on the
   step the car crosses the checkpoint it was headed for. Call once per frame,
   with that frame's dt. */
void cp_step(checkpoints_t *c, float x, float y, float z, float dt);

/* Re-sync the cursor to (x, y, z) without raising anything: the car has been
   put somewhere rather than driven there. Points the arrow at the first
   checkpoint ahead of the new position. KEEPS the lap count and `last` --
   main.c respawns for drowning and for falling out of the world as well as for
   a restart, and neither of those un-drives a lap or un-passes a checkpoint. */
void cp_resync(checkpoints_t *c, float x, float y, float z);

/* THE RACE IS STARTING OVER. cp_resync, plus the two things a resync must not
   touch: the lap goes back to 0, and `last` back to -1, so the next
   death sends the car to the grid rather than to a checkpoint from the previous
   run. This is the call for a restart, a track change or a car change; the
   drowning path takes cp_resync. */
void cp_restart(checkpoints_t *c, float x, float y, float z);

/* WHERE TO PUT A DEAD CAR BACK: the last checkpoint it actually crossed, aimed
 * along the spine at whatever comes next.
 *
 * -> 0 and touches nothing when no checkpoint has been crossed yet (`last` < 0),
 * when there is no spine, or when the spine is too degenerate to give a
 * direction -- in every one of which the caller should use the race start, which
 * is where the car would have been anyway.
 *
 * `pos` is the checkpoint marker's own x/z with cp_t.ground for y, so the caller
 * still owns the ground probe and its ceiling (the marker floats 0.18 to 0.49 m
 * over the terrain, and on beach_2 there is an overpass 7 m above the start).
 * `yaw_deg` is in the convention rbcar_init takes: local +Z on
 * (sin yaw, 0, cos yaw). */
int cp_respawn_pose(const checkpoints_t *c, float pos[3], float *yaw_deg);

/* Metres along the closed spine at (x, z), continuous: the nearest point on the
   nearest spine SEGMENT rather than the nearest sample. -> 0 with `out_s`
   untouched when there is no spine.

   This is the progress measure the crossing test runs on. It is deliberately NOT
   what cp_spine_dist answers with -- see there. */
int cp_progress(const checkpoints_t *c, float x, float z, float *out_s);

/* Draw the arrows. `eye` is the camera; the quad turns to face it. Call last,
   after everything else, and it leaves GL state as it found it. */
void cp_draw(checkpoints_t *c, const float eye[3]);

/* Metres along the spine from the car to the next checkpoint's centre. */
float cp_dist_to_next(const checkpoints_t *c, float x, float y, float z);

/* Progress along the closed spine at (x, y, z), WITHIN a lap: `dist` metres from
 * the start line to the nearest spine point, `cp` the checkpoint that owns it.
 * Either output may be NULL. -> 0 when there is no spine loaded, leaving both
 * untouched.
 *
 * SNAPPED TO A SAMPLE, unlike cp_progress, and left that way on purpose: this is
 * the function ai.h's `ai_track.spine` binds to, so it is the input to the
 * opponents' rubber band and to every number aitest measures. Making it
 * continuous would be an improvement -- the samples are 15 to 40 m apart on a
 * band that clamps at +/-45 m -- and it would move the AI, so it wants its own
 * pass with aitest as the evidence rather than riding along with a fix to the
 * checkpoint cue. */
int cp_spine_dist(const checkpoints_t *c, float x, float y, float z,
                  float *dist, int *cp);

#endif
