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

/* HOW MANY OPPONENTS ACTUALLY START -- AND IT IS THE DIFFICULTY'S NUMBER, NOT A
 * CONSTANT. This said THREE for a long time, "for a grid of four", on two
 * arguments that both turned out to be wrong:
 *
 *   - "the game itself". The game's own screenshot of dlgCHRACE, the
 *     championship's pre-race table, has SIX rows -- the player and five
 *     opponents, named Doc, BabyShark, Rosy, Da killa and Johny, which is
 *     exactly AI_RACES[beach_1]'s five entries in order, with Doc's engine at
 *     level 1 and BabyShark's tyres at level 1 as ai_data.h has them. Every
 *     cell of that picture is this port's own table.
 *   - "championship.ini has no Place4, so with six cars two places go unpaid".
 *     True, and it is not evidence: two places DO go unpaid, and the engine has
 *     words for them. FUN_004bf150's switch runs 1st..6th over strings
 *     40912..40914 and 40922..40924 -- a table that only makes sense for a
 *     six-car grid.
 *
 * And dlgCHRACE.ini settles it arithmetically: tableSY is 318, tableHeadHeight
 * 10% and tableItemHeight 15%, so the table is 31.8 + 6 x 47.7 = 318.0 exactly.
 * The layout file is sized for six rows and no others.
 *
 * WHAT DECIDES IT is the AI<n>Races mask, which is a DIFFICULTY mask
 * (ailayouts.ini `#define EASY 1 / NORMAL 2 / HARD 4') -- so the field grows
 * with the skill the player picked. On beach_1: three at easy, four at normal,
 * five at hard. That is the rule; this constant was a cap on top of it and is
 * now the array bound, which is what it should always have been.
 * mainmenu_field_size() has computed the same number off the same mask with NO
 * cap since it was written, so the menu has been promising five at hard while
 * this fielded three. */
#define AI_MAX_FIELD AI_MAX_OPPONENTS

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
       owns the nearest spine point. Either output may be NULL.
     *
     * `hint` is where this car was found LAST time, in the same metres, or
     * negative for "nowhere yet". It is not an optimisation: the unhinted query
     * searches the whole spine and flips between arc positions hundreds of metres
     * apart wherever a track passes near itself -- 3 to 16 times a lap on every
     * shipped recording -- which put the same jump into every lead and every
     * placing. See checkpoint.h at cp_spine_dist_near. */
    int (*spine)(void *ctx, float x, float y, float z, float hint,
                 float *dist, int *cp);
    /* THE PLAYER'S PROGRESS ROUND THE LAP, metres, 0 at the start/finish line.
     *
     * A SEPARATE QUERY from `spine` and the reason is the whole placing bug:
     * `spine` projects a position onto the checkpoint polyline, which is not the
     * road and is genuinely ambiguous where a track passes near itself -- it
     * jumps more than 10 m between consecutive samples 4 to 16 times a lap on
     * every one of the ten. Anchored on the LATCHED checkpoint index instead
     * (cp_lap_progress), it cannot flicker. */
    int (*lap_progress)(void *ctx, float x, float y, float z, float *out);
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
    /* THE PAINT, and the recordings do not carry one. A `.aip' profile names
       three upgrade levels and no skin, so a recorded opponent stays 0 -- which
       is the skin its model ships wearing, so nothing changes for the shipped
       field. A REMOTE player's comes off the wire (`ai_remote_look'), because
       the other machine's driver picked it in the garage. */
    int   skin;
    int   n;                 /* samples */
    int   cycle_start;       /* where the lap loop rejoins */
    float path_len;          /* the polyline's length, metres */

    /* THE LOOP IS THE LAP, AND THE POLYLINE IS NOT.
     *
     * A recording is a LEAD-IN followed by a LOOP: the car starts on its grid
     * slot, drives `lead_in` metres to the point the replay rejoins at, and from
     * there round `lap_len` metres of track back to that same point -- which is
     * why ai_advance sends the cursor back to `cycle_start` and not to 0.
     * Measured on all 30 shipped profiles: the loop closes to within 0.08 to
     * 0.22 m, and `path_len` from the file matches the polyline's own length to
     * within 0.3 m, so `path_len` is `lead_in + lap_len` and NOT one lap.
     *
     * That distinction is a bug's worth: scaling progress by `path_len` treats
     * the lead-in as part of every lap, so every lap after the first came out
     * short by it -- 7.7 to 16.9 m of a 430 to 455 m lap, 1.8% to 3.8%,
     * compounding for as long as the race runs. `lap_len` is what a lap is. */
    float lead_in;
    float lap_len;
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
    /* WITHIN the lap, and the hint the next query gets. Negative until the first
       successful query, so a fresh or reset car searches the whole spine once. */
    float spine_at;
    /* THE SPINE LAP COUNTER IS GONE. It counted `spine_at` wrapping so that
     * `spine_dist` could be built as `arc + lap * spine_len`, and nothing reads
     * it any more: an opponent's progress is `dist / lap_len`, which crosses the
     * line without needing to be told. The player still counts seams
     * (ai_t.player_lap_seam) because its own measure IS within-lap. */
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
    float bump_reach;        /* the proxy's own reach, metres -- the car's size */
    float bump_ref;          /* the spring's reference displacement (bump_w) */
    float bump_limit;        /* metres the offset may reach */
    float bump_up;           /* metres it may be LIFTED -- one car height, off
                                its own proxy. See ai_bump_clamp. */
    float bump_yaw_limit;    /* radians it may turn */
    float bump_accel;        /* the return's acceleration budget, m/s^2 */
    float bump_w;            /* the return spring's natural frequency, rad/s */
    float bump;              /* |off| right now, for the log */
    /* HOW MANY TIMES THIS CAR HAS BEEN PUT BACK ON ITS LINE because a shove left
       it somewhere the player would have died -- see ai.h, "dying". For the log
       and for the harness; nothing in the model reads it. */
    unsigned int respawns;
    /* How long the car has been under the ground without a break, seconds. The
       burial test is the one death that has to PERSIST -- see ai_bump_death. */
    float buried_for;
    /* THE WALL STOP. How far the offset may go along its own current direction
     * before the displaced car would be inside the level, and the horizontal
     * offset that answer was measured at. A negative wall means "not measured"
     * -- see ai_bump_wall. Kept on the car because the query behind it is a
     * world segment test and is amortised the same way the ground probe is. */
    float bump_wall;
    float bump_wall_at[2];

    /* -- THE STEERING DECISION. See "the steering decision" below.
     *
     * `steer_want` is metres ACROSS the recorded line, positive to the left, and
     * it is the offset the car has decided it wants rather than the one a shove
     * gave it -- so it is the spring's target instead of zero. `steer_cmd` is the
     * recovered controller's own steer angle, in degrees, which becomes the
     * target for the heading deviation so the car POINTS where it is going.
     * `steer_side` and `steer_hold` are the hysteresis: a car that has committed
     * to a side finishes the pass on that side.
     *
     * All zero on a car with nothing in its way, and exactly zero, which is what
     * keeps an unobstructed opponent bit-identical to one from before this
     * existed -- the same property the bump offset has. */
    float steer_want;
    /* The unit LEFT axis of the recorded frame the decision was taken in, so
       ai_step can turn the lateral scalar back into a world vector without
       walking the polyline a second time and without a second copy of the frame
       construction to disagree with this one. */
    float steer_left[3];
    float steer_cmd;
    int   steer_side;        /* -1 right, 0 undecided, +1 left */
    float steer_hold;        /* seconds left of the commitment */

    /* -- A REMOTE PLAYER. Set by ai_remote_init and read by nothing in this
     * file's own model: a remote slot is POSED from the network and is never
     * stepped, never rubber-banded, never bumped and never respawned. The flag
     * is here rather than in a parallel array in net.c so that `draw_ai',
     * `ai_matrix', the shadow and the car light all keep working with no idea
     * that a car is somebody else's -- which is the whole reason a remote car
     * is an ai_car in the first place. See net.h. */
    int   remote;
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
    /* The player's own within-lap distance, kept for the same reason
       ai_car.spine_at is. Negative until the first query. */
    float  player_at;
    /* and the PROJECTION's answer, kept only so the hint plumbing has somewhere
       to live and the log can see both. Not what the placing uses. */
    float  player_at_proj;
    /* and its seam-counted lap. The player's own measure is WITHIN a lap
       (cp_lap_progress), so something has to carry the laps; an opponent's is
       cumulative already and needs no counterpart, which is why ai_car has none
       any more. */
    int    player_lap_seam;
    /* THE PLAYER'S OWN PROXY REACH, metres, cached by ai_collide_player because
     * it gathers the player's spheres anyway and ai_step is handed a position
     * and not a car. The steering decision needs it to know how much room a
     * pass wants; it is 0 until the first contact call, and the decision falls
     * back to the opponent's own reach, which is the same number to within the
     * difference between two cars. */
    float  player_reach;
    /* HOLD THE STEERING DECISION OFF. Zero -- decide -- on any ai_t a caller
     * memsets, so the app never touches it and gets the decision.
     *
     * It exists for the fixtures that measure the REACTION: aitest part 8 drives
     * an opponent into a parked player to check the contact solve's two-moving-
     * bodies relative velocity and its pair denominator, and an opponent that
     * politely goes round is an opponent that never arrives. The decision has
     * its own fixtures; these have to keep measuring the thing they were written
     * against. It is also what lets the ten-track survey separate a car that
     * moved because it was HIT from one that moved because it CHOSE to. */
    int    steer_off;
    /* THE PLAYER'S VELOCITY, finite-differenced here because ai_step is handed a
     * position and the steering decision needs to know whether a gap is closing.
     * `player_prev` is last tick's position and is invalid until `player_seen`. */
    float  player_prev[3];
    float  player_v[3];
    int    player_seen;
} ai_t;

/* ---------------------------------------------------- A REMOTE PLAYER'S CAR
 *
 * MULTIPLAYER PUTS THE OTHER HUMANS IN THESE SLOTS, and it costs one flag and
 * two functions because a recorded opponent and a remote player are the same
 * thing seen from here: a car whose pose arrives from outside and is written
 * into a real `rb_car' with `rb_car_set_state'. `net.h' says why the wire
 * format IS `ai_sample'.
 *
 * A NETWORK RACE FIELDS NO RECORDINGS. `ai_init' is not called at all; the app
 * calls `ai_remote_init' with as many slots as there are other peers, so the
 * recorded-lap machinery -- the rubber band, the spine, the bump offset, the
 * steering decision -- is not merely skipped but absent, and there is no way
 * for a replayed car and a remote one to end up in the same array.
 */

/* Build `n' remote slots on `ai', each on the car model `car[i]' gives, parked
 * at the origin until the first packet. `w' is the world the bodies collide
 * against, as ai_init takes it. Clears everything else on the ai_t, so the two
 * initialisers cannot both have run. -> the number of slots built. */
int  ai_remote_init(ai_t *ai, int track, const rb_world *w,
                    const unsigned char *car, int n);

/* Pose slot `i' from `s'. The state goes in exactly as a recorded sample does
 * -- so the springs sit where the sender's did and the rig animates -- and the
 * linear velocity is taken from the sample rather than finite-differenced,
 * because the sender measured it and two consecutive packets are a send
 * interval apart. THE WHEELS ARE NOT TURNED HERE: their rolling angle is not in
 * the state and not on the wire. See `ai_remote_spin', which is what turns
 * them, and call it. */
void ai_remote_pose(ai_t *ai, int i, const ai_sample *s);

/* THE AUTHORED GRID, without loading a single recording.
 *
 * Every `.aip' record holds a sample block, and a recording BEGINS on its car's
 * grid slot -- that is what the lead-in is (see `cycle_start' above). So the
 * shipped starting grid for a track is the first sample of each of its profiles,
 * and the port does not have to invent one. A network race has no recordings and
 * still wants the grid, which is the whole reason this is separate from
 * `ai_init': it reads the header, walks the record array and reads TWELVE BYTES
 * per profile, keeping no blob and building no ai_t.
 *
 * Fills `out[i][0..2]` with slot i's position in world metres, in FILE order --
 * the same order `ai_init' walks, so slot i here is the opponent that would
 * have been opponent i in a single race. The PLAYER's own start is not in here;
 * it is that level's own `Players/Player' instance and lives in tracks.h.
 * Positions only: every car on a start line points the same way, and the yaw is
 * the track's.
 *
 * -> the number of slots filled, 0 if the file is missing or malformed. */
int  ai_grid(int track, const char *asset_dir, float out[][3], int max);

/* PARK slot `i' on the grid, until its first packet arrives.
 *
 * `ai_remote_init' leaves every remote body at the ORIGIN, which is not a
 * neutral place to leave a solid car: the player's own start is 2.9 m from the
 * world origin on urban_2, so a peer whose first state packet had not yet
 * arrived was a car sitting INSIDE the player's during the countdown. And with
 * no packet ever -- a peer that drops on the line -- it stayed there.
 *
 * `yaw' is the rig convention, the one rbcar_init takes. No-op on a slot that is
 * not remote. */
void ai_remote_park(ai_t *ai, int i, float x, float y, float z, float yaw);

/* ADVANCE EVERY REMOTE SLOT'S VISUAL RIG BY `dt', which today is its WHEELS.
 *
 * A REMOTE CAR'S WHEELS DO NOT COME OFF THE WIRE, and this is the call that was
 * missing. `rb_wheel.spin' -- the rolling angle carAniProc1 puts on the wheel
 * node -- is NOT one of the 32 floats (rb.h says so where it is declared: "purely
 * visual ... NOT in the ODE state"), so it is not in `ai_sample' either and
 * `ai_remote_pose' cannot write it. It is INTEGRATED, once a frame, by
 * `rb_wheel_spin_update'. A recorded opponent reaches that through `ai_step';
 * the player reaches it through `rbcar_step'; a remote player reached it through
 * NOTHING, because a network race does not call `ai_step' at all (main.c gates
 * it on `!net_race' and `ai_step' skips remote slots besides). So every remote
 * car in every multiplayer race slid on four frozen wheels -- on BOTH machines,
 * because the omission is in the receiver and both ends are receivers.
 *
 * It needs no more than the pose does: `ai_fake_contacts' reads the suspension
 * that arrived to decide which wheels are loaded -- the same signal, and the
 * same AI_DROOP_TOL, a replayed opponent uses -- and the patch velocity comes
 * from the body velocity `ai_remote_pose' took off the wire. Nothing is guessed
 * and nothing is simulated: the wheel turns at the speed the sender said its
 * car was doing.
 *
 * CALL IT AFTER `ai_remote_pose', once per FRAME and on the frame's own dt --
 * not on the tick loop's. The pose it works from is the interpolated one, which
 * is a per-frame quantity, and the rate this feeds (`rb_move_towards' at 200
 * rad/s^2) is a chase toward a target rather than an integration of a force, so
 * it converges to the same place whatever the step. */
void ai_remote_spin(ai_t *ai, float dt);

/* WHAT SLOT `i' LOOKS LIKE: the three upgrade levels in garage.h's order
 * (booster, engine, tyres) and the paint. The MODEL is fixed at
 * ai_remote_init -- it decides which scene is loaded -- but the exhaust, the
 * tyres and the skin are retexturings of that scene, so they can be handed over
 * afterwards, and they have to be: the roster row they come from is the peer's,
 * not this machine's. No-op on a slot that is not remote. */
void ai_remote_look(ai_t *ai, int i, const unsigned char up[3], int skin);

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

/* How far BELOW the rubber-banded command an opponent's actual speed may sit and
 * still count as throttle-down. THE PORT'S, and it is a NOISE FLOOR, not a
 * judgement about driving: in the steady state rb_move_towards returns the
 * command exactly and the car converges onto it, so a strict `command >= speed`
 * flickers on float noise and a cruising opponent's exhaust strobes.
 *
 * MEASURED rather than picked, because the first value here was picked and was
 * wrong by two orders of magnitude. Over 80 s of every opponent on beach_1,
 * beach_2 and beach_3 -- 14,400 car-ticks each -- the distribution of
 * (command - speed) where it is negative has a hole in it:
 *
 *     under 1 mm/s   4008 / 4299 / 4061 ticks     <- cruise, the noise floor
 *     1 to 5 mm/s      16 /  104 /    6
 *     5 to 10 mm/s     20 /   83 /    7
 *     10 to 20 mm/s    28 /  109 /   13
 *     ...to a worst of -1.20 / -1.18 / -0.45 m/s  <- real braking
 *
 * so the cruise samples all sit under 1 mm/s and everything real is above about
 * 5. 0.01 m/s is an order of magnitude clear of the noise on one side and an
 * order clear of one tick of the 5 m/s^2 acceleration limit (83 mm/s) on the
 * other, and it leaves the throttle off for 2-3% of a lap -- the corners.
 *
 * THE FIRST VALUE WAS 0.25 m/s, "well inside one tick of the acceleration
 * limit", which is exactly backwards: 0.25 is three times that limit, so it
 * swallowed every deceleration the car can physically express and the bit was on
 * for 1796 of 1800 ticks. A derived signal that is 99.8% constant is not a
 * derived signal, and the check that caught it is the one in vis_test part 14
 * that ties throttle-off to the speed actually FALLING. See ai_throttle. */
#define AI_THROTTLE_COAST 0.01f

/* ---------------------------------------------------------------------- API */

/* Load assets/<base>.aip and build one rb_car per opponent that races at this
 * difficulty (the AI<n>Races mask). `track` indexes AI_RACES and TRACKS alike.
 * `w` is stored on each body for a future physics path and is not otherwise
 * used -- ai_reset places every car from its own profile, not from a probe.
 *
 * -> the number of opponents loaded, or 0. Safe to call on a live ai_t; it frees
 * first. A missing or short .aip is not an error, it is a race with no
 * opponents, and it says so in the log. */
/* THE SKILL ROW'S ONE REAL EFFECT, and it is the engine's own rule enabled in a
 * mode the original leaves it off in.
 *
 * Every opponent carries AI<n>Races, a mask of the race types it appears in
 * (ai_data.h), and the roster loop only consults it in a CHAMPIONSHIP: outside
 * one the original starts the whole field whatever the difficulty is, which is
 * why a single race has always had five. The masks are graded and shipped --
 * beach_1's five read 4, 7, 6, 7, 7, so EASY fields three, NORMAL four and HARD
 * five -- and nothing outside the championship ever reads them.
 *
 * The quick-race dialog the exe carries (dlgRACESUM, ui.md) has an `enumSkill'
 * on it, so the row is the game's; what the row DOES here is the port's, and
 * this is it: turn the engine's own mask on for a single race. The alternative
 * was a row that changes nothing, which is the thing the five grey buttons on
 * the main menu already say honestly.
 *
 * SET BEFORE ai_init, which is when the roster is built -- the same shape
 * scene_set_tex_quality has, and for the same reason. Default 0, so every
 * harness and every existing caller keeps the field it measured. */
void ai_set_skill_field(int on);

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

/* WHERE EACH CHECKPOINT REALLY FALLS ROUND A LAP, as a fraction in [0, 1), fitted
 * off the loaded recordings -- which are the only description of the road this
 * port has. `mk` is the checkpoints' own markers in order, `n_mk` how many, and
 * `frac` takes n_mk fractions with frac[0] == 0 exactly (they are re-based on
 * checkpoint 0, the start/finish line). `lap_len_out` takes the mean road length
 * of one lap, in metres, which is what a caller needs to put a distance measured
 * on the ground into the same scale.
 *
 * WHY IT EXISTS. The placing compares the player against the opponents, and the
 * two were not in the same metres: an opponent's progress is uniform in road
 * distance, while the player's was laid out on the spine's cum[] arc stations --
 * and those are not where the checkpoints are. Measured against the recordings,
 * cum[] is 39.0 m out on average and 142 m at worst (country_4's cp_3), which is
 * a bigger error than the whole of the interpolation fix that preceded this.
 * `k/n` is no better at 40.1 m; there is no shortcut and the road has to be
 * asked.
 *
 * THE FIT IS ON THE LOOP, not the whole polyline -- `s[cycle_start .. n-1]` is
 * exactly one lap (ai_car.lap_len) -- and it is AVERAGED over every opponent
 * whose fit validates, because they are three separate drives of the same road.
 *
 * IT VALIDATES, and -> 0 leaving `frac` alone if it cannot: every marker's
 * closest approach must be within AI_CP_FIT_NEAR of the recorded line and the
 * re-based fractions must come out strictly increasing. On the ten shipped tracks
 * every one of the 30 recordings passes every one of its checkpoints within
 * 1.3 m, so the guard is not there for them -- it is there so that a track packed
 * without a usable recording falls back to cum[] rather than to nonsense. */
int   ai_cp_fractions(const ai_t *ai, const float (*mk)[3], int n_mk,
                      float *frac, float *lap_len_out);

/* checkpoint.h's CP_MAX. ai.c must NOT include checkpoint.h -- that file pulls in
   scene.h and therefore GL, which is the whole reason the spine arrives here
   behind a callback (see ai_track) -- so this is the one place the two have to
   agree, and main.c, which includes both, asserts that they do. */
#define AI_MAX_CP 8

/* How near a marker the recorded line has to pass before the fit above will
   believe it went through that checkpoint. The worst of the 300 real pairs is
   1.3 m and the widest checkpoint's drivable ground is 20 m, so 10 m is clear of
   every real pass and well inside a miss. */
#define AI_CP_FIT_NEAR 10.0f

/* --------------------------------------------------------------- THE ROAD
 *
 * ONE OPPONENT'S RECORDED LOOP, HANDED OVER AS THE ROAD. The fit above already
 * proves a recording passes every marker; this returns the polyline it proved it
 * on, so that something other than the checkpoint spine can be asked where a car
 * is round the lap.
 *
 * WHY THIS EXISTS. The player's progress was measured by an ODOMETER -- road
 * metres driven since the last checkpoint, against that stretch's fitted length
 * -- and an odometer can only grow. A wide line, a correction, a slide, a spin
 * or a shove all add to it and none of them is progress, so the player's own
 * measure ran AHEAD of where the player was, always in the player's favour,
 * while every opponent's stayed exact (its recording's own arc length). Measured
 * on the ten shipped tracks with the player driving car 0's recording under a
 * lateral wobble that lengthens the path 20%: the SAME CAR read +13.3 m further
 * on the player's ruler than on the opponents', worst +78 m -- enough to put the
 * player ahead of a car it was behind whenever that car was near, which is
 * exactly what "the place says 1st while two cars are in front" is.
 *
 * A PROJECTION ONTO THE ROAD CANNOT INFLATE: a wide line is perpendicular to it
 * and contributes nothing. The spine cannot serve -- it is 1.4 to 2.1x longer
 * than the lap it describes and wanders up to 84 m off it (checkpoint.h) -- but
 * a recorded lap IS the road, by construction, and this game ships thirty of
 * them. Its samples are 0.06 to 0.25 m apart, which is what makes a WINDOWED
 * projection possible at all: the same window on the spine, whose samples are 23
 * to 40 m apart, saw one segment and froze (ui.md records that attempt).
 *
 * `pt` is XZ only, for the same reason every other query on this road is
 * (checkpoint.c: a car under a deck is not at the checkpoint above it). `cum` is
 * arc from the loop's first sample; the loop CLOSES from the last sample back to
 * the first, which is 0.3 m or less on all thirty. `at[k]` is the arc at which
 * the loop passes checkpoint k -- the same closest approach ai_cp_fractions
 * averages, kept unaveraged here because the window has to be laid out on THIS
 * car's line and not on the mean of three. */
typedef struct {
    float (*pt)[2];         /* the loop's samples, XZ */
    float  *cum;            /* arc from pt[0], metres */
    int     n;
    float   len;            /* the loop closed -- one lap of road */
    float   at[AI_MAX_CP];
    int     n_cp;
    int     from;           /* which opponent's recording it is */
} ai_line;

/* -> 1 and fills `L` off the FIRST recording whose fit validates, which is the
   same test ai_cp_fractions applies. 0 leaves the caller with no road, which is
   what a track with no usable recording gets; the odometer is still there. */
int  ai_fit_line(const ai_t *ai, const float (*mk)[3], int n_mk, ai_line *L);
void ai_line_free(ai_line *L);

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

/* THE SPRING'S REFERENCE DISPLACEMENT, in multiples of the car's own collision
 * proxy reach (the furthest any of its 13 or 15 spheres gets from the centre of
 * mass, plus that sphere's radius -- measured at load, so a Hummer gets a bigger
 * number than a Buggy because it IS bigger). Two reaches is the distance at
 * which the car that hit it is completely clear of it. 0.63 m on the Overkill.
 *
 * THIS USED TO BE THE DISPLACEMENT LIMIT AS WELL, and that was the bug behind
 * "player can bump cars a bit, but cant bump it out of track... they seem too
 * heavy or just screwed to their way". One number was doing two jobs:
 *
 *   - it set the spring, w = sqrt(accel / ref), which is right. It is a
 *     RECOVERY scale: how far a car has to be off its line before the return
 *     wants the whole of its grip. That is a property of the car's size.
 *   - and it capped how far a shove could ever carry the car, which is not the
 *     same quantity at all -- and at 0.63 m it saturated at ordinary racing
 *     speed. Measured on a lone beach_1 opponent hit sideways: 1 m/s moved it
 *     0.099 m, 2 m/s 0.275, and 4, 6, 8 and 12 m/s ALL moved it 0.630 -- the
 *     clamp, to the millimetre, with the yaw pinned at exactly 30 degrees from
 *     1 m/s upward. Hitting harder did nothing, which is exactly what a car
 *     bolted to its line feels like.
 *
 * So this one keeps the spring, and bump_limit below is anchored on its own
 * quantity. The split leaves small knocks bit-for-bit as they were. */
#define AI_BUMP_REF_REACH 2.0f

/* HOW FAR a bump may carry an opponent, and it is now the car's own GRIP that
 * says so rather than its size.
 *
 * A car knocked sideways at v slides until its tyres stop it, and the
 * deceleration they can manage is already in this model: bump_accel, the
 * recovered coeff_rear_tires * g that caps the return. So the distance is
 * v^2 / (2 * accel), and the furthest a shove can ever legitimately carry a car
 * is that distance from the fastest hit the game can deliver -- which is the
 * car's own recovered top speed, `speed_boost_max` (35 km/h = 9.72 m/s, engine
 * word 3). 6.75 m on the Overkill.
 *
 * IT IS A SAFETY BOUND, NOT THE THING THAT STOPS THE CAR. What stops it in
 * ordinary play is the acceleration cap in the relax -- displacement comes out
 * proportional to v^2 across the whole useful range, 0.11 m for a 1 m/s tap and
 * 4.6 m for an 8 m/s ram -- and, before either, the WALL: a shove now stops at
 * geometry (ai_bump_wall). Without that stop this budget would be unusable, and
 * that is measured rather than assumed: sampling every opponent's own lap on all
 * ten tracks, a straight sideways displacement crosses level geometry 1.3% of
 * the time at the old 0.63 m and 64% of the time at 5 m.
 *
 * There is no constant here: the whole of it is the car's own data, and
 * ai_bump_derive is where the two numbers meet. */

/* THE WALL STOP -- what a shove actually runs out of, and the reason the budget
 * above can be metres instead of centimetres.
 *
 * Nothing else in the bump model looks sideways at the level. The offset is a
 * displacement from a recorded pose and the only world query under it is a
 * GROUND probe, so a car shoved far enough went through whatever was beside the
 * track. That did not matter while the budget was 0.63 m (1.3% of directions
 * blocked); it decides the feature at 5 m (64%).
 *
 * The test is the world's own segment query -- rb_world.segment, the engine's
 * 0x004557e0 -- from the recorded position to the displaced one, run at
 * AI_WALL_CLEAR above the ground AT EACH END so that a slope the terrain follow
 * would climb does not read as a wall. Blocked, the reach is bisected
 * AI_WALL_BISECT times and the offset is held at the last clear point, with the
 * outward velocity killed exactly as the budget does it.
 *
 * The clearance is the car's own half height (extent[1]), because that is what
 * decides whether the BODY passes: a lip under it is something the car rolls
 * over and the ground follow already handles, and anything taller across the
 * path is a wall. Four bisections put the stop within 1/16 of the reach, which
 * on a metre of shove is 6 cm -- under the proxy radius, so a car cannot end up
 * visibly inside anything. */
#define AI_WALL_CLEAR_EXTENT 1.0f
#define AI_WALL_BISECT 4
/* The wall answer is reused until the car has moved this far, the same
   amortisation and the same reason as AI_BUMP_PROBE_STEP -- the contact solve
   re-poses a car after every one of its eight depenetration passes, and a
   segment query per pass per touching pair is not affordable. */
#define AI_WALL_STEP 0.02f

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

/* NOTHING GOES FASTER THAN A CAR CAN GO, and until now nothing added the two
 * velocities up.
 *
 * An opponent's velocity is its RECORDING's plus its OFFSET's, and only the
 * first of the two was ever bounded. A car held back by a player wedged into it
 * fills its offset along the road at the speed its recording is running away at,
 * about 7 m/s, and metres of budget take about a second to fill; when the pair
 * comes apart the spring returns the lot at the grip limit. Measured on urban_2:
 * 6.56 m of along-track offset coming back at 9.81 m/s while the recording under
 * it was doing 7 -- sixteen metres a second, on a car whose own recovered top
 * speed is 9.72. Reported as "player car stuck in opponent car and oponent car
 * accselerate insanly to its line".
 *
 * So the SUM is capped at `speed_boost_max`, in the relax, on the velocity. What
 * comes out is a car driving back to its line at the fastest a car in this game
 * drives, which is what a real one held up by a shunt does -- about two seconds
 * for the whole budget instead of half of one. See ai_bump_relax. */

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

/* WHEN ONE CAR COUNTS AS BEING ON TOP OF ANOTHER: the cosine of the engine's own
 * 46-degree floor cone, the angle carDriveForce (0x4ee8fc) uses to decide a face
 * is drivable rather than a wall. A contact normal inside it is a ride-over and
 * the separation has a real vertical share; outside it the pair is two cars side
 * by side and is pushed apart ALONG THE GROUND, because the vertical push has no
 * counterpart -- AI_BUMP_MAX_SINK stops the lower car being pushed down and the
 * return spring is two orders of magnitude weaker than the push -- so a
 * sustained graze walks a car into the air. See ai_pair_resolve. */
#define AI_TOP_COS  0.694658f

/* A CEILING ON THE FIELD SOLVE'S SWEEPS, so a bigger field can never turn into
   a bigger frame cost without somebody choosing it. The layout carries five
   opponents, which asks for three sweeps of a ten-pair list; this is that, and
   it is a bound rather than the number itself (ai_collide_field derives the
   number from how many cars are actually racing). */
#define AI_FIELD_SWEEPS_MAX 4

/* ------------------------------------------------------------------ DYING
 *
 * AN OPPONENT DIES THE WAY THE PLAYER DIES, and goes back to the same place the
 * player goes back to.
 *
 * The player has two deaths (main.c): DROWNED -- its centre of mass a whole
 * DROWN_DEPTH under a water surface -- and FELL OUT OF THE WORLD, measured
 * against the ground it was last placed on rather than a constant, because the
 * ten tracks sit at very different heights. Either one puts it back on the last
 * checkpoint it crossed, keeping its lap: a death is not a race restart.
 *
 * An opponent had neither, and the shove that can now carry it metres is exactly
 * what puts it in the sea off the end of beach_1's sand. So it gets the same two
 * tests, with the same two numbers, against the one place it can be put back to:
 * ITS OWN RECORDED LINE. That is the opponent's last checkpoint and more -- the
 * recording is a real lap driven on this physics, so the line is drivable by
 * construction, and the cursor, the lap and the rubber band are all still where
 * the car left them. Putting it back is exactly clearing the offset.
 *
 * ONLY WHEN THE SHOVE DID IT. The test is on the car's actual pose, and an
 * opponent with no offset IS its recording -- so if the recording itself fords a
 * stream, clearing an offset of zero would change nothing and the car would be
 * declared dead every tick for the rest of the race. A car on its line cannot
 * die; there is nowhere better to send it.
 *
 * The numbers are the player's, quoted rather than shared, because ai.c cannot
 * see main.c: DROWN_DEPTH is 0.5 m -- the tallest body box is 0.14 m, so half a
 * metre under is unambiguously submerged, while the deepest water a car can
 * actually ford on any of the ten tracks is the stream at 2 to 13 cm -- and the
 * fall is 30 m, here below the car's own RECORDED height rather than below a
 * spawn height, which is the same idea measured off something that follows the
 * track. */
#define AI_DROWN_DEPTH 0.5f
#define AI_FELL_BELOW  30.0f
/* An offset this small is not a shove that put the car anywhere: below it the
   car is on its line and there is nothing to put it back to. A wheel radius. */
#define AI_DEATH_MIN_OFF 0.072f

/* HOW LONG A CAR HAS TO BE BURIED BEFORE IT COUNTS AS STUCK, in time constants
 * of its own return spring (1 / bump_w, 0.30 s on an Overkill).
 *
 * Drowning and falling are unambiguous the instant they happen, which is why the
 * player dies on the spot for both. Being under the ground is not: a car sliding
 * along a bank clips into it and out again constantly, and killing it for that
 * is the teleport the whole of this work has been removing -- tested on the
 * instant, 130 of 640 hard shoves ended in a respawn, one in five.
 *
 * So the burial test gives the machinery that already exists its full chance
 * first. Three time constants is the settling time of a critically damped
 * return, which is the same number AI_STEER_SETTLE is three of and the same
 * reasoning: after it, the spring has finished and whatever is still true is
 * going to stay true. */
#define AI_BURIED_SETTLE 3.0f

/* ------------------------------------------------------ the steering decision
 *
 * WHAT THIS IS FOR. Being knocked aside and springing back is a REACTION; it is
 * not a decision, and until now a shoved opponent had nothing else. It could not
 * see a car stopped in its path, it drove into it, and it was pushed past --
 * `known-issues.md` carried that as "what is missing is not the reaction, it is
 * the decision". This is the decision.
 *
 * WHAT IT IS NOT. It is not FUN_004fddd0 transcribed. That controller drives a
 * FULLY SIMULATED rigid body and the port cannot afford three of those: colprof
 * puts one car's collision queries alone at a median 2 ms of Vita time per 1/60
 * tick and 10% of the grid past 4 ms, against a 16.7 ms frame that also has to
 * render -- re-measured, not quoted. So the port keeps the replay and gives it a
 * decision, and the decision uses THE CONTROLLER'S OWN LAW AND ITS OWN NUMBERS:
 *
 *   the lookahead        FUN_004fda90's 2.7 m further along the path
 *   the signed angle     FUN_00410150, transcribed at ai_signed_angle
 *   the deadband         its +-0.5 degrees
 *   the lock             its +-35 degrees
 *   the rate limit       its 90 deg/s
 *
 * HOW IT ACTS. An opponent's pose is `recording + offset`, and the offset is all
 * this is allowed to touch -- the replay, the cursor, the lap, the rubber band
 * and every measurement in aitest are untouched by construction, which is the
 * same guarantee the bump has and aitest part 9 checks bit for bit.
 *
 *   1. LOOK 2.7 m up the recorded path and ask what is in the corridor between
 *      here and there -- the player and every other opponent, against the pair's
 *      own proxy reaches plus a wheel's width of daylight.
 *   2. If something is, pick the side with the room and set `steer_want`, the
 *      lateral offset that clears it. Committed: `steer_side` and AI_STEER_HOLD
 *      keep the car on that side until the pass is over, because a car that
 *      re-decides every tick weaves.
 *   3. The spring, unchanged in every constant, pulls the offset toward
 *      `steer_want` instead of toward zero. The grip cap is still
 *      `coeff_rear_tires * RB_GRAVITY`, so an opponent moves over no harder than
 *      its own tyres could pull it -- a decision it could actually execute.
 *   4. The steer angle out of the controller's law becomes the target for the
 *      heading deviation, so the car POINTS into the move. Without it a car that
 *      changes line slides sideways like a hovercraft, which is what gives the
 *      whole thing away.
 *
 * `AI_STEER_LOCK` is 35 degrees and `bump_yaw_limit` is one `AngleSteer`, 30 --
 * both apply, both recovered, and they are different quantities: the first is
 * how hard the controller may ask, the second is how far off its recorded
 * heading a car may end up.
 */
#define AI_STEER_LOOKAHEAD  2.7f   /* FUN_004fda90 */
#define AI_STEER_DEADBAND   0.5f   /* degrees, FUN_004fddd0 */
#define AI_STEER_LOCK      35.0f   /* degrees, FUN_004fddd0 */
#define AI_STEER_RATE      90.0f   /* degrees/second, FUN_004fddd0 */

/* How long a committed side is held after the last tick that still saw the
 * obstacle, seconds. THE PORT'S, and it is the offset spring's own time constant
 * (1 / bump_w, 0.28 s on an Overkill) rounded up to the nearest tenth: a car
 * that lets go sooner than the spring can move it has not finished the pass it
 * decided on, and one that holds much longer is driving a line nothing asked
 * for. Below it the car weaves, which is the failure a fixture can see. */
#define AI_STEER_HOLD       0.3f

/* Below this the car is not going anywhere and decides nothing, m/s. THE PORT'S,
 * and it is AI_SPEED_FLOOR -- the speed ai_advance refuses to walk the cursor
 * slower than, so it is already this file's own answer to "stopped". */
#define AI_STEER_MIN_SPEED  AI_SPEED_FLOOR

/* The far end of the horizon, in time constants of the offset spring's own
 * `bump_w`. Three of them is the standard settling time of a critically damped
 * second order system -- 0.92 s on an Overkill -- and it is how long this car
 * takes to complete a lane change it decides on. Derived rather than typed:
 * change the spring and this follows it. */
#define AI_STEER_SETTLE     3.0f

/* The rubber-band coefficient, exposed so a harness can bind to it directly:
 * FuncWaitAccel<slot>(lead) * clamp(difficulty * track, 0.5, 2.0). `gap` is how
 * many checkpoints the PLAYER is ahead by; 2 or more pulls AI_FRONT_SHIFTS in.
 */
float ai_coeff(const ai_t *ai, int slot, float lead, int gap);

#endif
