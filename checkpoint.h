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
 * Then it advanced on an arc-length CROSSING: the spine gives every checkpoint a
 * station (cum[k][0]), the car's progress along the spine was projected onto the
 * nearest spine segment, and `passed` was raised where that progress swept past
 * the station being headed for. That is a sound rule on a spine that follows the
 * road. THIS SPINE DOES NOT.
 *
 * THE REFINING POINTS ARE NOT ON THE ROAD, and this is the measurement that
 * settles it -- taken against the shipped .aip recordings, which are real driven
 * laps and the only path data the project has that is not the spine itself:
 *
 *   the cp_N CHECKPOINTS are on the racing line and in road order. Over the ten
 *   tracks and three opponents each -- 150 (checkpoint, lap) pairs -- every
 *   recorded lap passes within 1.58 m of every cp_N marker, worst case, and it
 *   visits them cp_1, cp_2, ... cp_n, ascending, on all ten.
 *
 *   the cp_N_M REFINING POINTS are up to 51 m off it, and out of order. On
 *   country_1, cp_1_1..cp_1_5 sit 33-51 m from the nearest point of the lap, out
 *   in a corner of the map the road never enters, and the closest-approach sample
 *   indices of the 39 spine points run 3206, 238, 243, 247, 245, 237, 225, 536,
 *   1719, 1678, 539, ... -- 14 of them going backwards along the lap. All ten
 *   tracks are like this: worst off-line 43 to 63 m, 3 to 14 points out of order,
 *   and the stitched spine comes out 1.6x to 2.1x the length of the lap it is
 *   supposed to measure (827-955 m of spine against 404-555 m of road).
 *
 * The stitching itself is right -- FUN_004e9560 is disassembled and it builds
 * exactly this chain, cp_N -> cp_N_1 -> ... -> cp_N_M -> cp_(N+1) closed back to
 * cp_1, one 0x44-byte segment record each with its own length at +0x30 (0x4e9980)
 * -- so the ENGINE's polyline wanders too. It is not a racing line and was never
 * one; what the engine's own race module makes of it is not recovered. Using it
 * as a progress measure was the port's invention, and the invention is what was
 * wrong. Replaying all 30 shipped recordings through the arc-length rule misses
 * 174 of the 300 crossings, fires 9 spurious ones and gets 54 out of order.
 *
 * SO THE TRIGGER IS A RADIUS AROUND THE MARKER AFTER ALL -- ordered, and fired at
 * the CLOSEST APPROACH rather than on entry. See CP_TRIGGER_RAD for the two-sided
 * bound the radius is chosen inside. The file used to argue against exactly this
 * ("a car taking a wide line never comes inside any sensible radius"); the 1.58 m
 * measurement is what retires the argument, and the ORDER is what makes it safe
 * where a bare radius is not -- country_2's road comes back within 1.17 m of cp_6
 * at a different point in the lap, and `next` is why that does not fire it twice.
 *
 * The arc-length spine is still built and still exported: cp_spine_dist is the
 * AI's rubber-band input and cp_dist_to_next feeds the vis telemetry line, and neither is touched
 * here. What it no longer does is decide when a checkpoint is passed.
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

/* How close to a checkpoint marker counts as reaching it, metres, in XZ.
 *
 * Bounded on BOTH sides by measurement against the shipped .aip recordings, and
 * the two bounds are nearly an order of magnitude apart, which is what makes the
 * number safe rather than tuned:
 *
 *   ABOVE 1.58 m, the worst distance at which any of the 30 recorded laps passes
 *   any of its checkpoints. A radius under that misses real passes.
 *   BELOW 5.45 m, half the closest two checkpoint markers on any track
 *   (country_1's cp_3 and cp_4, 10.91 m apart). Staying under half keeps two
 *   checkpoints' zones from ever overlapping, so the car cannot be inside two at
 *   once and the order can never be ambiguous.
 *
 * 5 m sits 3.2x above the first and just inside the second. The recordings are
 * racing lines and a player wanders more than one, which is the argument for the
 * top of the band rather than the middle.
 *
 * NOT recovered. The engine has a race module that owns this and the port does
 * not have it; see the header comment for what the engine's own checkpoint data
 * turned out to be and why none of it answers this. */
#define CP_TRIGGER_RAD 5.f

/* How far past its closest approach the car has to get before the pass fires,
   metres. The event is the MINIMUM of the distance, so something has to say the
   minimum is behind us; this is that something.
 *
 * It exists to keep a STATIONARY car from firing. Five of the ten grids sit
 * inside cp_1's radius already (beach_1 2.34 m, beach_2 4.41, beach_3 3.28,
 * country_2 3.68, country_3 2.74), the car is held there for the whole 3-second
 * countdown, and firing on ENTRY would sound the cue before GO on all five. A
 * car that never moves never gets 5 cm past anything.
 *
 * 5 cm is under half a frame of travel at this car's 7.5 m/s top speed (12.5 cm
 * at 60 Hz) so it costs nothing at speed, and it is orders of magnitude above the
 * sub-millimetre jitter a sleeping body has. */
#define CP_PASS_EPS 0.05f

/* A RADIUS CAN BE MISSED, AND THE CURSOR DOES NOT STEP OVER IT. Stated here
 * because it is a decision with a cost either way, and both costs are measured.
 *
 * The drivable ground at a checkpoint is far wider than the racing line through
 * it. Measured off each track's own collision grid, perpendicular to the recorded
 * lap's travel direction and stopping at the first hole, 1.5 m step or wall, the
 * half-width runs from 0.50 m (beach_3 cp_3, a bridge) to the 20 m probe cap
 * (beach_2 cp_3, open sand); 27 of the 50 checkpoints have ground wider than
 * CP_TRIGGER_RAD on at least one side. So a player who takes a beach wide can
 * miss one, and with a strictly ordered cursor the arrow then keeps pointing back
 * at it until they go and get it.
 *
 * A "step over one if you reach the next" escape hatch was built and REMOVED,
 * because on the shipped data it costs more than it buys: country_2's road passes
 * within 2.18-2.89 m of cp_3 early in the lap -- about 130 m before the real pass
 * -- so the hatch stepped over cp_2 on every single one of that track's recorded
 * laps, silently, cue and all. Distance alone cannot separate that fly-by from a
 * pass (the worst real pass is 1.58 m and the fly-by minimum is 2.18 m, a band too
 * narrow to sit a threshold in), and country_2's road also comes back within
 * 1.17 m of cp_6 elsewhere in the same lap.
 *
 * Strict order is therefore what ships: 0 missed and 0 out of order on all 30
 * recorded laps, against 6 silent skips with the hatch in. A stalled arrow points
 * at the thing to go back for, which is what an arrow is for, and the port does not
 * yet enforce a lap limit for it to spoil (see known-issues.md).
 */

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

    /* THE APPROACH TO `next`: whether the car is inside its trigger radius, and
       the closest it has come while inside. The pass fires when the distance
       climbs CP_PASS_EPS back off that minimum, or when the car leaves the radius
       having been in it -- so the cue lands at the checkpoint, not at the edge of
       a circle around it. Cleared on every fire, by cp_init and by cp_resync: a
       car that has been PUT somewhere is not mid-approach to anything. */
    int   in_zone;
    float zone_min;

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
     * `next` IS now (last + 1) % n, always, with -1 meaning "nothing crossed yet,
     * so head for checkpoint 0". That identity is the whole state machine and it
     * is why neither cp_restart nor cp_resync projects a position any more.
     *
     * It used to be derived instead from where the car IS -- the first arc-length
     * station ahead of it -- and at the grid that answer depended on which side of
     * cp_0's seam the start marker happened to fall on. Measured over the ten
     * shipped tracks, FIVE fell on the wrong side: country_1 came out pointing at
     * cp_2 with cp_1 six metres in front of the car, country_3 and urban_1 at cp_2
     * with cp_1 three and six metres ahead, country_4 at cp_2, and urban_2 at cp_5
     * with the whole first half of the lap skipped -- because urban_2's spine
     * passes 8 m overhead of the grid and the projection is in XZ. On all five the
     * START/FINISH LINE never fired on the first lap, which is what got reported.
     *
     * The identity makes it exact instead: at the grid nothing has been crossed,
     * so the car is heading for checkpoint 0, on every track, and cp_1 is the
     * nearest checkpoint to the race-start marker on all ten (2.3 to 20.5 m, and
     * 1.4x to 49x clear of the next nearest checkpoint) -- i.e. the race really
     * does start at the start/finish line, which is the thing the old rule could
     * not see.
     *
     * cp_resync deliberately does NOT clear `last`: a teleport does not un-pass a
     * checkpoint, which is the same reason cp_resync keeps the lap. cp_restart
     * clears both. */
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
   step the car finishes passing the checkpoint it was headed for -- the step its
   distance to that marker starts climbing again, having been inside
   CP_TRIGGER_RAD. Call once per frame, with that frame's dt. */
void cp_step(checkpoints_t *c, float x, float y, float z, float dt);

/* Re-sync without raising anything: the car has been PUT somewhere rather than
   driven there. Aims the arrow at (last + 1), drops any approach in progress, and
   KEEPS the lap count and `last` -- main.c respawns for drowning and for falling
   out of the world as well as for a restart, and neither of those un-drives a lap
   or un-passes a checkpoint.

   The position arguments are no longer read; they are kept so the call reads at
   the site as what it is and so a future rule that needs them does not have to
   change every caller. Every caller puts the car either on the grid or on the
   marker of `last` (cp_respawn_pose), and (last + 1) is the exact answer for
   both -- which is why guessing it from a projection could stop. */
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
