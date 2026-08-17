/*
 * ai.h -- the AI opponents.
 *
 * An RC Cars opponent is mostly a REPLAY. Each one replays a profile that was
 * recorded on this same physics (CarProfiles/<level>/<car><n>.dat, decoded by
 * rccars_re/prof_dump.py and packed by pack_ai.py), and the only thing the game
 * decides for them at runtime is how fast to walk along it. That is the model this
 * file transcribes, and all of it is recovered:
 *
 *   FUN_00503880   the per-frame step. Poses the car from the profile, asks for
 *                  the rubber-band coefficient, and commands
 *                      target = moveTowards(speed_now,
 *                                           speed_recorded * coeff, 5.0, dt)
 *   FUN_00503440   the cursor advance. Given that target speed it walks
 *                  `target * dt` metres along the recorded POLYLINE and turns
 *                  that back into a position between two samples. Returns 1 at
 *                  the end of the path, on which FUN_00503880 sends the cursor
 *                  back to the profile's own CYCLE START -- not to sample 0, so
 *                  the run-up to the grid is never replayed.
 *   FUN_00502ea0   the pose. Lerps the whole 32-float state between the two
 *                  samples the cursor straddles, slerps the quaternion over the
 *                  top, and hands it to carSetState.
 *   FUN_004fd4c0   the coefficient: FuncWaitAccel<slot>(lead) times the
 *                  difficulty coefficient times the track's own
 *                  CoeffCommonOpponents, the last two clamped to 0.5..2.0.
 *   FUN_004fd5e0   `lead` -- the opponent's distance along the track's spine
 *                  minus the player's, less AI_FRONT_SHIFTS[gap] when the
 *                  PLAYER is two or more checkpoints ahead.
 *   FUN_00471a70   the evaluator. Its ramp is fed 1.0, 1.0 and 10.0 by
 *                  FUN_004fd6b0 and therefore returns exactly 1.0, so what
 *                  survives is the spline multiply and nothing else.
 *
 * So an opponent is exact where it matters -- the line, the cornering, the
 * suspension, the jumps are all a human's (or the game's own AI's) lap, played
 * back -- and rubber-banded only in the ONE dimension the original rubber-bands.
 *
 * ------------------------------------------------------------------------
 *
 * THE ORIGINAL HAS A SECOND MODE AND THIS PORT DOES NOT TRANSCRIBE IT.
 *
 * WHICH OF THE TWO THE ORIGINAL RUNS WHEN IS NOT SETTLED. FUN_00503880, the replay
 * above, runs from the per-frame car loop whenever the current profile slot
 * (phys+0x6dc) has samples, and for a type-1 car it advances that replay at the
 * rubber-banded speed -- so it is certainly an AI path. The controller below runs
 * from FUN_004f6b20 gated on phys+0x4398, and nothing here has found what writes
 * that flag or whether both run together. AIEmu's 25 m DistCam makes a near/far
 * split the obvious guess and a guess is all it is; do not read the paragraph after
 * next as more than that.
 *
 * FUN_004fe1f0 -> FUN_004fdb50 -> FUN_004fddd0 is a real steering controller:
 * it finds the nearest sample to where the car actually is, takes a point 2.7 m
 * further along the path, and drives a fully simulated rigid body at it --
 * target speed from the distance to that point (4 to 10 m/s) scaled by how
 * sharply it has to turn, throttle proportional over the top half of that,
 * steer as the signed angle to the point mapped onto +-35 degrees and rate
 * limited to 90 deg/s, full lock for the first second, and reverse-with-5-degrees
 * when it reports itself stuck. All of that is recovered and none of it is here.
 *
 * The reason is cost, and it is the same reason the original has `AIEmu`:
 * a simulated opponent is another rb_car_tick. The port's own hardware
 * measurements put one car's sim at 3-19 ms of Vita time per 1/60 tick after the
 * collision broad phase, and five of those does not fit in a frame at any frame
 * rate. AI_DIST_CAM exists in the original's data for what looks like exactly this
 * reason. The port's answer, for now, is to replay all of it -- which is the path
 * FUN_00503880 is, and which for a real lap driven by a real driver is visually the
 * same car.
 *
 * What that used to cost was interaction: an opponent on rails could shove the
 * player and could not be shoved back, so a car that got in its way was driven
 * over by something infinitely massive. It is BUMPABLE now -- see "the bump
 * offset" below -- which is not the steering controller and does not pretend to
 * be: the recorded line is still the line, and a hit displaces the car FROM it
 * and is pulled back to it. That keeps the cost the replay exists for and buys
 * the one behaviour the rails could not express.
 *
 * ------------------------------------------------------------------------
 *
 * An AI car IS an rb_car. Nothing ticks its physics -- ai_step writes the whole
 * state with rb_car_set_state -- but carrying a real rb_car means carani,
 * shadow.c, rbcar_matrix and rb_wheel_spin_update all work on it unchanged,
 * with no second pose type to keep in step.
 */

#ifndef AI_H
#define AI_H

#include "rb.h"
#include "ai_data.h"

/* ------------------------------------------------------------------- the file
 *
 * assets/<track>.aip, written by rccars_re/pack_ai.py -- see that file for the
 * layout and for what every check on it measured. Two things about it matter
 * here:
 *
 *   - positions are already in the PORT's body frame. The recording's centre of
 *     mass is the engine's, which is not where gen_rb_data.py parks the port's,
 *     so pack_ai.py lifts every sample along its own body Y by a measured
 *     per-car constant. Nothing at runtime has to know.
 *   - the block is loaded whole and pointed into. `n * 36` bytes is 300 KB to
 *     1.2 MB for a track's five opponents, which is under 4% of the .vsc beside
 *     it, so there is no streaming and no decimation: the polyline the AI walks
 *     is the recording's own, to the metre it was driven.
 */

/* HOW MANY OPPONENTS ACTUALLY START.
 *
 * The layout carries five per track (AI_MAX_OPPONENTS, out of ai_data.h) and the
 * retail game fields THREE, for a grid of four. Two things agree on that: the
 * game itself, and championship.ini, which pays `Place1`, `Place2` and `Place3`
 * per track and has no `Place4` -- with four cars the podium is the whole field
 * bar last, and with six it would leave two places unpaid.
 *
 * The five entries are still all READ, so the roster, the upgrade levels and the
 * spline slots are the layout's own; this is the number that start. Raising it to
 * AI_MAX_OPPONENTS is a one-line change and everything downstream already sizes
 * to five. */
#define AI_MAX_FIELD 3

#define AI_SAMPLE_BYTES 36
#define AI_RECORD_BYTES 100

/* Quantisation, matching pack_ai.py. Each was sized off the measured range over
   all 151 readable profiles; see the packer. */
#define AI_Q_SCALE     32767.0f    /* quaternion */
#define AI_SUSP_FULL   0.3f        /* u8 full scale, metres */
#define AI_STEER_SCALE 100.0f      /* i16 counts per degree */
/* The speed field's scale. It is a SPEED in metres per second and not a
 * momentum, and that is measured rather than assumed -- see ai.h's note under
 * ai_sample and the derivation in pack_ai.py. */
#define AI_VEL_SCALE   2000.0f     /* i16 counts per metre/second */
#define AI_DT_SCALE    10000.0f    /* u16 counts per second */

typedef struct {
    float          p[3];     /* centre of mass, port body frame */
    short          q[4];     /* orientation (w,x,y,z) * AI_Q_SCALE */
    /* THE CAR'S VELOCITY, in metres per second, times AI_VEL_SCALE.
     *
     * PHYSICS.md calls state[7..9] the linear MOMENTUM P, and FUN_004fd740
     * multiplies it by the body's 1/mass to get a velocity -- so this field was
     * read as a momentum and divided by the port's own mass of 2.0 kg, which ran
     * every opponent at exactly half speed.
     *
     * The recording settles it. Integrating |field| dt over a whole profile gives
     * the polyline's own length to within 0.04% on all three cars (459.9 against
     * 459.7 m, 466.5 against 466.1, 471.9 against 471.8), so the implied mass is
     * 1.000 and the field IS the speed. Read as a momentum over a 2 kg body it
     * would also have to be wrong on its face: it peaks at 9.3 to 10.5, which
     * halves to 4.6-5.2 m/s, BELOW the 7.5 m/s flat top speed of a car that
     * averages 6.7 m/s over the lap.
     *
     * That leaves a question this file does not answer and must not: whether the
     * engine's body mass is 1.0 where rb_data.h invented 2.0. See CLAUDE.md. */
    short          mom[3];
    unsigned char  susp[6];  /* suspension length * 255 / AI_SUSP_FULL */
    short          steer;    /* degrees * AI_STEER_SCALE */
    unsigned short dt;       /* seconds * AI_DT_SCALE since the previous sample */
} ai_sample;

/* ------------------------------------------------------------ the track spine
 *
 * The rubber band is driven by the difference between the opponent's progress
 * and the player's, and the original measures both on the same thing:
 * FUN_004ea120 returns a car's checkpoint index and its distance along the
 * track's closed spine. The port's spine lives in checkpoint.c, which pulls in
 * scene.h and therefore GL, so it arrives here behind a callback -- the same
 * shape rb_world uses, and for the same reason: ai.c stays host-testable and a
 * harness can supply a straight line.
 *
 * `dist` must be metres and must INCREASE along the direction of travel WITHIN a
 * lap; ai_step lifts it by each car's lap count to rebuild the cumulative
 * quantity the original compares. `spine_len` is what it lifts by, so it has to
 * be the same closed length the query wraps at.
 */
typedef struct {
    void *ctx;
    /* -> 1 on success. `dist` metres along the spine, `cp` the checkpoint that
       owns the nearest spine point. Either output may be NULL. */
    int (*spine)(void *ctx, float x, float y, float z, float *dist, int *cp);
    float spine_len;         /* the closed spine's total length, metres */
} ai_track;

/* ----------------------------------------------------------------- one opponent
 */
typedef struct {
    /* -- from the file */
    char  name[16];          /* the driver, for the HUD */
    char  path[32];          /* the profile, for the log */
    int   car;               /* 0 Overkill, 1 Buggy, 2 Hummer */
    int   ref;               /* 1..AI_N_PLAYERS */
    int   races;             /* AI_RACE_* mask */
    int   boost, reson, tires;
    int   n;                 /* samples */
    int   cycle_start;       /* where the lap loop rejoins */
    float path_len;          /* the polyline's length, metres */
    float duration;          /* the recording's own time span, seconds */
    float body_dy;           /* the lift pack_ai.py already applied */
    const ai_sample *s;

    /* -- the cursor.
     *
     * FUN_00503440 carries (index, time) and every use of `time` is of the form
     * (t - tA) / (tB - tA), so this carries (index, u) instead and the
     * arithmetic comes out identical -- the derivation is written out at
     * ai_advance. `cursor` is the sample the car is BEHIND, so the segment is
     * s[cursor-1] -> s[cursor] and `cursor` is >= 1 once running. */
    int   cursor;
    float u;                 /* 0..1 through that segment */

    /* -- what the last step decided, all readable for the HUD and the log */
    float speed;             /* the commanded speed, m/s */
    float speed_rec;         /* the recording's own speed at the cursor, m/s */
    float coeff;             /* the rubber-band coefficient actually applied */
    float lead;              /* metres ahead of the player on the spine */
    int   slot;              /* 0..4, which FuncWaitAccel curve */
    int   lap;
    float dist;              /* metres walked along the polyline, all laps */
    float spine_dist;        /* the unwrapped spine distance, for placings */
    int   cp;                /* the checkpoint it is heading for */
    int   airborne;          /* no wheel loaded, from the recorded suspension */

    /* -- the pose. See the header note: an AI car is an rb_car whose state is
       written rather than integrated. */
    rb_car rb;

    /* -- the recorded pose, BEFORE the bump offset below is composed onto it.
     *
     * ai_pose writes this and then ai_bump_apply rebuilds rb.body from
     * `rec + off`, so applying the offset is IDEMPOTENT: the contact solve can
     * push a car and re-pose it several times inside one tick without the
     * pushes compounding. It is also what the finite-difference velocity is
     * taken on -- the speed the rubber band chases is the speed the RECORDING
     * is being played at, not the speed a shove happens to be adding. */
    float rec_x[3];
    float rec_q[4];

    /* -- THE BUMP OFFSET. See "the bump offset" below for the model. All in
     * world space; `off_yaw` is radians about world up, applied about the car's
     * own centre of mass. Zero on an untouched car, and exactly zero, which is
     * what makes an untouched opponent bit-identical to one packed before this
     * existed. */
    float off[3];
    float offv[3];
    float off_yaw, off_yawv;
    /* The terrain follow, which is NOT part of the offset and is not sprung: the
       ground under the displaced car minus the ground under the recorded one.
       Written by ai_bump_apply, read by nothing else, and here rather than local
       so the log and a harness can see a car climbing rather than clipping.
       `off_gnd_at` is the horizontal offset it was last probed at -- see
       AI_BUMP_PROBE_STEP. */
    float off_gnd;
    float off_gnd_at[2];
    /* Per-car, derived at load from the car's own data -- see ai_bump_derive.
     * Kept on the car rather than recomputed because the proxy reach costs a
     * sphere gather. */
    float bump_limit;        /* metres the offset may reach */
    float bump_yaw_limit;    /* radians it may turn */
    float bump_accel;        /* the return's acceleration budget, m/s^2 */
    float bump_w;            /* the return spring's natural frequency, rad/s */
    float bump;              /* |off| right now, for the log */
} ai_car;

typedef struct {
    ai_car car[AI_MAX_OPPONENTS];
    int    n;
    void  *blob;             /* the whole .aip, freed by ai_free */
    int    track;            /* index into AI_RACES / TRACKS */
    int    difficulty;       /* 0 easy .. 3 ultra */
    int    championship;     /* selects AI_FWA_A over AI_FWA, and the
                                difficulty coefficient -- FUN_004f11b0 gates on
                                the mode being 1 */
    float  coeff_static;     /* difficulty * CoeffCommonOpponents, clamped */
    /* the player's last known CUMULATIVE progress, so ai_player_place can rank
       without asking twice */
    float  player_dist;
    int    player_cp;
} ai_t;

/* The rate the commanded speed chases its target, m/s^2. FUN_00503880's third
   argument to moveTowards, an immediate 5.0. */
#define AI_ACCEL_LIMIT 5.0f

/* FUN_00503440 floors the commanded speed here (0x50347e) -- a stopped opponent
   would otherwise never leave the sample it is on, and a lap would never end. */
#define AI_SPEED_FLOOR 0.1f

/* FUN_00502ea0 refuses to interpolate across a segment implying more than
 * `maxImpLinear * 1.5` of HORIZONTAL speed and takes the earlier sample whole.
 * maxImpLinear is 20.0 m/s (MAX_IMPULSE, loaded at 0x004f7793), so this is
 * 30 m/s. It never fires on the shipped profiles -- the fastest segment in any
 * of them is 0.811 m over 0.122 s, 6.6 m/s -- which is the point: it is a guard
 * against a discontinuity in the recording, not part of the motion. */
#define AI_TELEPORT_SPEED (20.0f * 1.5f)

/* A wheel whose recorded suspension has extended to within this of its own FREE
 * length is hanging, not loaded. Used only to tell rb_wheel_spin_update whether
 * the car is on the ground -- the recorded state carries no contact flags. THE
 * PORT'S, and see ai_fake_contacts for why it is len_free and NOT len_max. */
#define AI_DROOP_TOL 0.002f

/* ---------------------------------------------------------------------- API */

/* Load assets/<base>.aip and build one rb_car per opponent that races at this
 * difficulty (the AI<n>Races mask). `track` indexes AI_RACES and TRACKS alike.
 * `w` is stored on each body for a future physics path and is not otherwise
 * used -- ai_reset places every car from its own profile, not from a probe.
 *
 * -> the number of opponents loaded, or 0. Safe to call on a live ai_t; it frees
 * first. A missing or short .aip is not an error, it is a race with no
 * opponents, and it says so in the log. */
int  ai_init(ai_t *ai, int track, const char *asset_dir, const rb_world *w,
             int difficulty, int championship);

void ai_free(ai_t *ai);

/* Put every opponent back on the start of its own path. Called by ai_init and
 * again on a restart. */
void ai_reset(ai_t *ai);

/* One 1/60 tick, for every opponent.
 *
 * `px/py/pz` is the player's centre of mass and `player_lap` its lap count,
 * which ai.c cannot know -- checkpoint.c owns it, and the original reads the
 * equivalent out of each car's own race record rather than off its position.
 * Both are needed because the distance the rubber band compares is CUMULATIVE:
 * FUN_004eb630 returns `spine_len * (lap - 1) + distance into the lap`, so a car
 * a lap up reads hundreds of metres ahead instead of wrapping to nothing.
 *
 * `tr` may be NULL, in which case the lead is zero and the coefficient reduces
 * to the static difficulty product -- which is exactly what the original does
 * before the first checkpoint query succeeds (FUN_004fd5e0 returns 0 and
 * FUN_004fd4c0 returns the product alone). */
void ai_step(ai_t *ai, const ai_track *tr, float px, float py, float pz,
             int player_lap, float dt);

/* Ready for glMultMatrixf, like rbcar_matrix. */
const float *ai_matrix(const ai_t *ai, int i);

/* 1 if opponent `i` is closer than `d` metres to (x,y,z). For the draw-distance
   and voice-budget decisions main.c has to make. */
int   ai_within(const ai_t *ai, int i, float x, float y, float z, float d);

/* Race placing 1..n+1 for the player, by cumulative spine distance. Needs
   ai_step to have run at least once with a non-NULL `tr`. */
int   ai_player_place(const ai_t *ai);

/* ------------------------------------------------------------- the bump offset
 *
 * THE OPPONENTS ARE SOLID AND THEY ARE BUMPABLE. Running into one shoves the
 * player the way running into anything else does -- AND shoves the opponent,
 * which then steers itself back onto its recorded line.
 *
 * The offset is the whole idea. An opponent's position is
 *
 *     pose = the recording, interpolated at the cursor   (rec_x, rec_q)
 *          + a displacement the recording knows nothing about   (off, off_yaw)
 *
 * and the second term is the only thing a collision may touch. So the replay is
 * untouched -- the line, the cornering, the suspension, the jumps and every
 * measurement in aitest are the recording's own -- while the car can be pushed
 * off that line, and is pulled back onto it by a spring. Nothing here simulates
 * the opponent; being knocked aside and recovering is exactly the interaction a
 * replay cannot express, and it is the whole of what this adds.
 *
 * ONE-WAY IS WHAT THIS REPLACES, and it was a bug as well as a limitation. With
 * the opponent infinitely massive the player took the entire impulse AND the
 * entire positional push, so a player wedged under a bigger car (a Buggy under
 * the Hummer, as reported) was pushed out of one sphere pair per tick while the
 * opponent's next pose put it straight back in -- a car that could not get out
 * from under one that could not get out of the way. Splitting both halves by
 * mass means the pair SEPARATES: each body carries away its share and the
 * opponent's share persists, because the offset is state.
 *
 * The LAW is the engine's own, not an invention. rb_coll_resolve (0x004f0750) is
 * up to ten Gauss-Seidel passes in which every contact whose relative normal
 * velocity is at or below 0.02 m/s gets enough impulse to reach 0.05 m/s of
 * separation, through rb_impulse_denom (0x004754a0) and rb_apply_impulse
 * (0x004756c0). All of that is reused. Four things are extended, and every one
 * is marked THE PORT'S at the point of use:
 *
 *   - the relative velocity is between two MOVING bodies. rb_coll_resolve's own
 *     cannot be: it solves a body against a static world.
 *   - the denominator is the pair's, k = k_player + k_opponent, so the impulse
 *     delivers its dv across both. An opponent contributes the yaw-only form
 *     (ai_denom), which is exactly the response ai_take_impulse then applies --
 *     the two must agree or the solve over- or under-corrects.
 *   - ROLL AND PITCH ARE DISCARDED from an opponent's angular response. The
 *     recorded orientation carries the car's attitude on its suspension, and a
 *     bump has no way to give that back; yaw is the axis a shove actually shows
 *     on, and it is the one a car recovers from by steering.
 *   - a positional push, because there is no carSubstepContact bisection here to
 *     stop the two proxies overlapping in the first place. The deepest pair PER
 *     OPPONENT, split by mass -- see ai.c for why it is not the global deepest
 *     and not every pair.
 *
 * Vertically two separate things happen, and keeping them apart is what makes
 * the rest simple. The offset itself is an ordinary three-axis spring, so a car
 * hit hard enough IS lifted and the spring puts it back -- bounded up by the same
 * limit as sideways and down by a centimetre, because the ground is right there
 * and nothing on this path models it holding the car up. On top of that, and
 * outside the spring, sits a TERRAIN FOLLOW: a car shoved sideways up a slope has
 * to climb it, or it is buried on the high side and hanging on the low one.
 *
 * Refusing the vertical outright was tried first and is worse than it sounds. It
 * looks conservative -- an opponent cannot be launched -- and what it actually
 * does is make the one case this whole change exists for unresolvable: a player
 * wedged UNDER a car can only be freed by lifting the car, and with that
 * forbidden the pair grinds. Measured, the player ended up 12 cm below the
 * opponent with their centres 4 cm apart, which is the reported bug with the
 * roles swapped.
 *
 * Call ai_collide_player once per 1/60 tick, right after ai_step, so the contact
 * sees the pose the step just wrote. -> the closing speed of the hardest contact
 * in m/s, or 0 for no contact, which is what main.c raises `car_cdt_car` off.
 * OPPONENT AGAINST OPPONENT is solved inside ai_step, by the same routine and
 * for the same reason -- with both of them bumpable, driving through each other
 * is the one remaining way for the field to look fake. */
float ai_collide_player(ai_t *ai, rb_car *player, float dt);

/* Shove opponent `i` with impulse `j` (kg m/s) at world point `point`, through
 * the same path a contact takes. Exposed so a harness can inject a known
 * impulse and measure the recovery against it, rather than having to arrange a
 * collision and then argue about what the collision delivered. */
void ai_bump_impulse(ai_t *ai, int i, const float point[3], const float j[3]);

/* Broad phase for the above: an opponent further than this from the player
 * cannot be touching it. The proxy reaches about 0.30 m from a centre of mass
 * (body tops 0.251/0.202/0.248 m, tyres at |x| 0.21), so two cars need 0.6 m and
 * this is 2.5x clear of that. */
#define AI_COLLIDE_RANGE 1.5f

/* rb_coll_resolve's own two numbers (0x004f0750): solve any contact closing
 * faster than -0.02 m/s, and give it 0.05 m/s of separation. */
#define AI_CONTACT_VREL  0.02f
#define AI_CONTACT_SEP   0.05f
#define AI_CONTACT_PASSES 10

/* How many times the positional half re-measures and pushes again. A car proxy
 * is 13 spheres and they WEDGE -- clearing the deepest pair moves the car into a
 * different one -- so one pass per tick is not depenetration, it is one step
 * toward it. Each pass re-gathers both proxies, so it always works on the
 * overlap that is really there and cannot over-correct.
 *
 * Measured over the SECOND AFTER FIRST CONTACT of aitest part 8's two rams,
 * worst overlap left standing:
 *
 *              3 m run-up      12 m run-up
 *   1 pass       0.081 m         0.080 m
 *   2            0.047           0.083
 *   4            0.016           0.047
 *   8            0.011           0.022
 *
 * The window matters as much as the number. Over the whole 12 s run -- which is
 * ten seconds of one car bulldozing another that has reached its offset limit --
 * the same sweep is not monotone in either column and lands anywhere between 2
 * and 8 cm, because a sustained shove between two 13-sphere proxies is chaotic.
 * Choosing a pass count against THAT is fitting to noise; this is the arrival,
 * which is the thing the passes actually govern. Eight, and the cost is a sphere
 * gather and 169 distance tests per pass per touching pair -- nothing beside the
 * ~92 world queries a tick the car itself issues. */
#define AI_DEPEN_PASSES 8

/* ---------------------------------------------------- the bump's own numbers
 *
 * THE PORT'S, every one -- the original has no such mechanism to transcribe.
 * What they are anchored to is the car's own recovered data, so that none of
 * them is a number somebody liked the look of, and ai_bump_derive builds all
 * four per car out of these.
 */

/* HOW FAR a bump may carry an opponent, in multiples of its own collision
 * proxy's reach (the furthest any of its 13 or 15 spheres gets from the centre
 * of mass, plus that sphere's radius -- measured at load, so a Hummer gets more
 * room than a Buggy because it IS bigger). Two reaches is the distance at which
 * the car that hit it is completely clear of it, which is as far as a bump has
 * anything to say; past that something is dragging an opponent off the track
 * rather than knocking it aside. 0.55 m on the Overkill. */
#define AI_BUMP_LIMIT_REACH 2.0f

/* HOW HARD it pulls itself back: the return acceleration is capped at the car's
 * own grip times gravity -- coeff_rear_tires * RB_GRAVITY, 7.0 m/s^2 on the
 * Overkill -- because a car recovering its line is doing it through its tyres,
 * and carFrictionSolve clamps against the same product.
 *
 * That fixes the spring too, and it is why there is no frequency constant here.
 * A critically damped return from a displacement d peaks at w^2 * d, so
 * requiring a FULLY displaced car to come back at exactly its grip limit gives
 *
 *     w = sqrt(grip * g / limit)
 *
 * -- 3.6 rad/s on the Overkill, a 0.28 s time constant and about a second to
 * settle, which is what a driver correcting a knock looks like. */

/* HOW FAR OFF its heading a bump may turn it, as a fraction of the car's own
 * steering lock (AngleSteer, 30 degrees on all three). One lock is the angle it
 * could take out with a single input, so it is the natural bound on a yaw the
 * car is going to correct by steering. */
#define AI_BUMP_YAW_LOCKS 1.0f

/* The offset SNAPS to exactly zero below this, so a recovered opponent is
 * bit-identical to one that was never touched -- otherwise the pose carries a
 * micrometre of displacement forever and the ground probes below run for the
 * rest of the race. Half a millimetre and a centimetre a second are both an
 * order of magnitude under anything visible on a 0.42 m car. */
#define AI_BUMP_SNAP    0.0005f
#define AI_BUMP_SNAP_V  0.01f
#define AI_BUMP_SNAP_YAW   0.0005f   /* radians, 0.03 degrees */
#define AI_BUMP_SNAP_YAWV  0.01f

/* The vertical follow. `ceil` is how far above the recorded position the ground
 * probe may look, for the reason rb_world.ground's own note gives -- beach_2's
 * start is under an overpass -- widened from RBCAR_PLACE_CEIL because a bumped
 * car may legitimately be shoved up onto a kerb. The lift is clamped so that a
 * probe that lands on something unexpected cannot teleport a car. */
#define AI_BUMP_CEIL      0.50f
#define AI_BUMP_MAX_LIFT  0.35f

/* How far the car has to have moved sideways before the terrain is probed again.
 *
 * This is a COST bound, and it is not a small one. Composing the offset onto the
 * pose is idempotent by design, so the contact solve re-poses a car after every
 * push -- with AI_DEPEN_PASSES of those plus two poses a tick, an opponent in
 * contact would re-probe the ground about eighteen times a tick, two queries
 * each. The car's whole own physics issues about ninety-two world queries a
 * tick, so that is a 40% sim increase paid exactly when a frame is busiest.
 * Two centimetres of movement is 3.5 mm of height on a 10-degree slope and the
 * depenetration passes move millimetres, so this collapses it to one probe pair
 * per tick and changes nothing visible. */
#define AI_BUMP_PROBE_STEP 0.02f

/* How far DOWN the offset may go. Upward and sideways it is bounded by
   bump_limit, which is ONE budget over all three axes. Not zero, because a
   contact resolved to the last float would otherwise chatter against the clamp;
   not more, because there is ground under the car. */
#define AI_BUMP_MAX_SINK  0.01f

/* The rubber-band coefficient, exposed so a harness can bind to it directly:
 * FuncWaitAccel<slot>(lead) * clamp(difficulty * track, 0.5, 2.0). `gap` is how
 * many checkpoints the PLAYER is ahead by; 2 or more pulls AI_FRONT_SHIFTS in.
 */
float ai_coeff(const ai_t *ai, int slot, float lead, int gap);

#endif
