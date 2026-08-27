/*
 * sfx.c -- the game's sounds, driven by the transcribed physics. See sfx.h.
 *
 * Pure C apart from audio.h's lock, so audio_test.c compiles it on the host.
 */

#include "sfx.h"
#include "audio.h"
#include "tracks.h"
/* For PROP_MODELS[].sound and its two radii. A pure header with no code behind
   it, so audio_test.c still compiles this file without linking prop.c. */
#include "prop_data.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- the sound names, by the engine's own snd.dat spelling ---------------- */

/* One motor family per car. chn.dat declares three motor channels per car
   (car_motor, car_motor_2, car_motor_ws for the Overkill; the _b_ and _h_
   families for the Buggy and Hummer) and the wav set has exactly the three
   groups below to fill them. */
static const char *const MOTOR_PREFIX[3] = { "motor_", "motor_b_", "motor_h_" };

/* Suffixes, indexed by the layer's slot. */
#define M_IDLE     0
#define M_ENGINE   1
#define M_ACCEL    2
#define M_DECEL    3
#define M_REVERSE  4
#define M_WS       5
#define M_OFF      6
#define M_COUNT    7
static const char *const MOTOR_SUFFIX[M_COUNT] = {
    "idle", "engine", "accel1", "decel1", "reverse", "engine_ws", "off"
};

/* Surface loops and impacts. Index by SURF_*.
 *
 * car_hit_ has no wetsand or grass member in the shipped set, so those fall
 * back to sand -- the soft-ground impact, which is what they are. */
static const char *const SURF_LOOP[SURF_COUNT] = {
    "car_surf_asphalt", "car_surf_sand", "car_surf_wetsand", "car_surf_gravel",
    "car_surf_stone", "car_surf_grass", "car_surf_metal", "car_surf_wood",
    "car_surf_water"
};
static const char *const SURF_HIT[SURF_COUNT] = {
    "car_hit_asphalt", "car_hit_sand", "car_hit_sand", "car_hit_gravel",
    "car_hit_stone", "car_hit_sand", "car_hit_metal", "car_hit_wood",
    "car_hit_water"
};
static const char *const SURF_LABEL[SURF_COUNT] = {
    "asphalt", "sand", "wetsand", "gravel", "stone", "grass", "metal",
    "wood", "water"
};

/* The four resident character voices, IN SFX_VOICE_* ORDER and named by the
   models themselves (each model's own MOD_SNDCHANNEL -- see sfx.h). sfx_init
   loads them through this and sfx_char_wav resolves a model's declared wav
   against it, so the enum, the loader and the lookup cannot drift apart. */
static const char *const VOICE_WAV[SFX_VOICE_COUNT] = {
    "dog_attack",           /* Dog                     */
    "seagull_vzliot",       /* Seagull                 */
    "man_voice",            /* Man, RepairMan, Guard   */
    "woman_voice"           /* Woman                   */
};

static const char *const UI_SND[5] = {
    "arrowfocus", "arrowpress", "buttonpress", "buttonfocus", "msgboxerror"
};

/* ---- tuning of the port's own model -------------------------------------- */

/* All of this is the PORT's, not the engine's -- the crossfade law is not
   recovered (see sfx.h). What is fixed by the data is the three-layer shape and
   which sample belongs in which layer. */
#define ENG_PITCH_LO    0.80f   /* engine loop pitch at rest */
#define ENG_PITCH_HI    1.70f   /* ... and at the car's own top speed */
#define IDLE_PITCH_LO   0.95f
#define IDLE_PITCH_HI   1.10f
#define ENG_GAIN        0.85f
#define IDLE_GAIN       0.70f
#define TRANS_GAIN      0.55f
#define WS_GAIN         0.70f
#define SURF_GAIN       0.90f
#define AMBIENT_GAIN    0.55f

/* Above this fraction of top speed the surface loop is at full level. The car
   is 1:10 scale and tops out around 7.5 m/s, so this is deliberately low. */
#define SURF_FULL_SPEED 0.35f

/* Wheelspin: rb_wheel.spin_extra is the slip the tyre model carries on top of
   the rolling rate. Normalised by this to a 0..1 blend. */
#define WS_FULL_SLIP    25.0f   /* rad/s of extra spin for a full wheelspin layer */

/* THE MOTOR IS A LATCH, and unlike everything above it this part IS the
 * engine's. `0x0050acf0` keeps a per-car "the motor is sounding at all" flag at
 * `phys+0xe7c4`, and `0x0050a2a0` -- the car_motor callback -- returns without
 * touching a voice while it is clear:
 *
 *     prev == 0 && now == 0   ->  return, nothing plays
 *     prev != 0 && now == 0   ->  play the stop transition, then nothing
 *
 * The flag is raised the moment the motor state is neither 0 (stationary, no
 * input) nor 4 (coasting), and cleared again after the state has been 0 for a
 * continuous 4 seconds (`ds:0x5543f0`); the threshold under which a car counts
 * as stationary is 1 m/s (`ds:0x554390`), and the two inputs the state machine
 * reads are `phys+0x576c` and `phys+0x5774` bit 0 -- accelerate and brake,
 * which are `rb_input.accel` and `.brake` here.
 *
 * `0x00509ff0` zeroes the whole 0x25-dword block whenever the car is not
 * audible, so a car that has just been created starts with the flag CLEAR. That
 * is why there is no engine noise over the 3-2-1: the car is on the grid, the
 * controls are locked out, the state never leaves 0, and the motor has not
 * started. It starts on GO, with the first throttle.
 *
 * `motor_off.wav` (1.66 s) is the stop transition, and there is deliberately no
 * `motor_on` to match -- the engine simply begins. It has shipped in every bank
 * this port has ever built and nothing has ever played it. */
#define ENG_ON_SPEED    1.0f    /* ds:0x554390 -- below this it counts as stopped */
#define ENG_OFF_TIME    4.0f    /* ds:0x5543f0 -- of that before the motor cuts */

/* A landing is only a landing if it lands hard enough to hear. */
#define LAND_MIN_SPEED  0.8f    /* m/s of downward velocity */
#define LAND_FULL_SPEED 5.0f

/* Knocking a prop over. WHICH sound plays and over what radii is the game's --
 * every one of the thirteen models names its own wav in stone.sb, see
 * prop_data.h. How LOUD it is is the port's: nothing recovered says, and a knock
 * that does not scale with the knock sounds like a trigger rather than an impact.
 *
 * The car tops out at 7.5 m/s, so a 4 m/s closing speed is already a hard shunt;
 * the floor exists because the quietest audible nudge should still be audible.
 * PROP_HIT_COOL is what stops a can trapped under a wheel from re-triggering as
 * the contact chatters -- prop.c's edge covers a held contact, this covers one
 * that keeps breaking and remaking. */
#define PROP_MIN_SPEED   0.35f  /* m/s of closing speed below which nothing plays */
#define PROP_FULL_SPEED  4.0f
#define PROP_GAIN_FLOOR  0.35f
#define PROP_HIT_COOL    0.08f  /* per prop, seconds */
/* The car's own "I hit an object" cue, car_cdt_obj, on the engine's own
   car_CDT&Boost channel. One per event however many props were struck, so it
   gets its own cooldown rather than the per-prop one. */
#define PROP_CDT_COOL    0.20f

/* Priorities: a loop must never be stolen by a one-shot. */
#define PRIO_ONESHOT    0
#define PRIO_SURFACE    2
#define PRIO_ENGINE     4
#define PRIO_AMBIENT    1

/* ---- state --------------------------------------------------------------- */

typedef struct {
    int snd;                    /* bank index, -1 if absent */
    mix_voice v;
} loop_t;

static struct {
    int ok;
    int car;                    /* -1 = none loaded */
    int track;
    const col_t *col;
    int paused;
    int no_race;      /* the front end is up -- see sfx_race_active */

    int motor[M_COUNT];         /* bank indices for the current car */
    loop_t l_main;              /* car_motor    : idle <-> engine */
    loop_t l_idle;
    loop_t l_trans;             /* car_motor_2  : accel / decel / reverse */
    loop_t l_ws;                /* car_motor_ws : the wheelspin layer */
    loop_t l_surf;              /* car_Surfaces */
    loop_t l_amb;               /* ground_noise_snd */

    int trans_slot;             /* which of M_ACCEL/M_DECEL/M_REVERSE is loaded */
    int surf_mat;               /* material the surface loop is playing */

    int surf_snd[SURF_COUNT];
    int hit_snd[SURF_COUNT];
    int ui_snd[5];
    int snd_landing, snd_splash, snd_cp, snd_wrong, snd_prestart, snd_reset;
    int snd_beside;
    int snd_start;              /* cp_start -- the four countdown beeps */
    int snd_brake, snd_cdt_obj;
    /* The opponents: one positional loop each off the single motorAI wav that
       ships. See sfx.h. */
    int snd_ai_motor;
    int snd_cdt_car;
    int snd_bullet, snd_cdt_bullet;
    float bullet_cool;
    float car_cool;
    loop_t l_ai[SFX_AI_MAX];
    float ai_pos[SFX_AI_MAX][3];
    /* One bank index per PROP_MODELS entry. Several share a wav, so several
       entries share an index -- mix_load refcounts, so that is free. */
    int prop_snd[PROP_N_MODELS];
    int voice_snd[SFX_VOICE_COUNT];
    float voice_cool[SFX_VOICE_COUNT];
    float prop_cool[PROP_N_MODELS];
    float cdt_cool;

    /* The motor latch, phys+0xe7c4 / +0xe7c8 / +0xe7cc. See ENG_OFF_TIME. */
    int   eng_on;
    int   eng_was_on;
    float eng_off_t;

    /* edge detection */
    int   was_airborne;
    int   was_in_water;
    float prev_vy;
    float brake_cool;
    float hit_cool;

    float vol_sfx, vol_music;
    int   cur_surface;
} S;

/* ---- helpers ------------------------------------------------------------- */

/*
 * PAGE A SOUND IN. CALL THIS WITH THE AUDIO LOCK *NOT* HELD.
 *
 * mix_load opens the bank and reads the samples off the memory card. That is
 * milliseconds on Vita3K's host filesystem and it is a very long time on a real
 * Vita -- a car's motor family is the biggest thing in the bank. All three of the
 * callers below used to do it inside audio_lock(), which blocks the output thread
 * out of mix_render for the whole read, so sceAudioOutOutput is never called, the
 * hardware buffer drains, and THE SOUND STOPS. Every car change, every track
 * change and the whole of sfx_init. On the emulator it is inaudible; on hardware
 * it is the dropout.
 *
 * Doing it unlocked is safe because the audio thread never touches the bank:
 * mix_render reads m->v[] and m->music only, and a voice carries its own `pcm`
 * pointer taken when it started. mix_load/mix_unload/mix_trim are all game-thread
 * calls. Only mix_trim, which frees PCM and has to silence any voice still on it,
 * needs the lock -- and it is cheap.
 */
static int find_load(const char *name)
{
    mix_t *m = audio_mix();
    int i = mix_find(m, name);
    if (i >= 0 && mix_load(m, i) != 0) return -1;
    return i;
}

static void loop_stop(loop_t *l)
{
    if (l->v.slot >= 0) mix_stop(audio_mix(), l->v);
    l->v = MIX_NOVOICE;
}

/* Keep a looping voice alive at the given gain/pitch, starting it if needed.
   gain <= 0 stops it, so a layer fades out and then simply is not a voice. */
static void loop_drive(loop_t *l, float gain, float pitch, int prio)
{
    mix_t *m = audio_mix();
    if (l->snd < 0) return;
    if (gain <= 0.001f) {
        loop_stop(l);
        return;
    }
    if (!mix_alive(m, l->v))
        l->v = mix_play(m, l->snd, gain, pitch, 1, prio);
    else
        mix_set(m, l->v, gain, pitch);
}

static void one_shot(int snd, float gain, float pitch)
{
    if (snd < 0 || gain <= 0.001f) return;
    mix_play(audio_mix(), snd, gain, pitch, 0, PRIO_ONESHOT);
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- lifecycle ----------------------------------------------------------- */

int sfx_init(void)
{
    int i;
    memset(&S, 0, sizeof(S));
    S.car = S.track = -1;
    S.trans_slot = -1;
    S.surf_mat = -1;
    S.vol_sfx = 1.f;
    S.vol_music = 0.7f;
    S.l_main.v = S.l_idle.v = S.l_trans.v = S.l_ws.v = MIX_NOVOICE;
    S.l_surf.v = S.l_amb.v = MIX_NOVOICE;
    S.l_main.snd = S.l_idle.snd = S.l_trans.snd = S.l_ws.snd = -1;
    S.l_surf.snd = S.l_amb.snd = -1;
    for (i = 0; i < M_COUNT; i++) S.motor[i] = -1;

    if (!audio_ok()) return -1;

    /* The common set: paged in once and kept for the whole session. It is small
       -- the big entries are the motor families and the ambient beds, and those
       are per-car and per-track. UNLOCKED: see find_load. */
    for (i = 0; i < SURF_COUNT; i++) {
        S.surf_snd[i] = find_load(SURF_LOOP[i]);
        S.hit_snd[i] = find_load(SURF_HIT[i]);
    }
    for (i = 0; i < 5; i++) S.ui_snd[i] = find_load(UI_SND[i]);
    S.snd_landing  = find_load("car_landing");
    S.snd_splash   = find_load("water_splash");
    S.snd_cp       = find_load("cp");
    S.snd_wrong    = find_load("cp_wrongway");
    /* `cp_beside\' -- shipped, named by snd.dat, packed by pack_snd.py and until
       now never loaded, because nothing knew what raised it. The engine fires it
       once per checkpoint when the car has been within 4 m of the marker it is
       heading for and has got back outside 5 m (FUN_004eb550, message-system id
       0x25e). See dirarrow.h. */
    S.snd_beside   = find_load("cp_beside");
    S.snd_prestart = find_load("prestart");
    /* THE WHOLE 3-2-1-GO IN ONE FILE. cp_start.wav is 4.44 s holding four beeps
       whose onsets are at 0.0005, 1.0005, 2.0005 and 3.0005 s -- 1.0000 s apart
       to the sample, which is exactly the one second per digit the exe's four
       poster calls ask for. So countdown.c plays this once at t = 0 and the beeps
       land on 3, 2, 1 and GO by themselves. See countdown.h. */
    S.snd_start    = find_load("cp_start");
    S.snd_reset    = find_load("cp_reset");
    S.snd_brake    = find_load("car_break");
    S.snd_cdt_obj  = find_load("car_cdt_obj");
    /* The opponents' engine. Loaded once at init rather than with the car,
       because it does not depend on which car the PLAYER drives -- and it is one
       sample against the motor family's three. */
    S.snd_ai_motor = find_load("motorAI_accel1");
    S.snd_cdt_car  = find_load("car_cdt_car");
    /* THE GUARDS SHOOT, AND BOTH SOUNDS SHIP. `snd.dat` names `bullet` and
       `car_cdt_bullet` and both wavs are in Sound/; `chn.dat` declares a
       `bullet` channel and a `burst` one beside `radar`. This project's notes
       said the guard was a man who stands and turns, and the whole burst -- the
       clips, the constants, the particle systems and these two cues -- was in
       the shipped data. See char.c's step_guard. */
    S.snd_bullet     = find_load("bullet");
    S.snd_cdt_bullet = find_load("car_cdt_bullet");
    for (i = 0; i < SFX_AI_MAX; i++) {
        S.l_ai[i].snd = S.snd_ai_motor;
        S.l_ai[i].v = MIX_NOVOICE;
    }

    /* The prop sounds belong in the common set rather than being paged per
       track. There are only seven distinct wavs behind the thirteen models
       (PROP_SOUNDS), they are among the smallest things in the bank, and every
       track places props -- so a per-track load would re-read almost the same
       set on every track change for no saving. Resolved per MODEL so the lookup
       at the hit is an array index and not a string search. */
    for (i = 0; i < PROP_N_MODELS; i++)
        S.prop_snd[i] = find_load(PROP_MODELS[i].sound);
    /* The characters' four voices, resident like the props' thirteen: they are
       one-shots of a second or two and the working set is already the car's
       motor family. Named by the models themselves -- see sfx.h. */
    for (i = 0; i < SFX_VOICE_COUNT; i++)
        S.voice_snd[i] = find_load(VOICE_WAV[i]);

    audio_lock();
    mix_master(audio_mix(), S.vol_sfx, S.vol_music);
    audio_unlock();

    S.ok = 1;
    return 0;
}

void sfx_shutdown(void)
{
    if (!S.ok) return;
    audio_lock();
    mix_stop_all(audio_mix());
    audio_unlock();
    S.ok = 0;
}

void sfx_set_car(int car_index)
{
    mix_t *m;
    int i;
    char name[64];

    if (!S.ok || car_index == S.car) return;
    if (car_index < 0 || car_index > 2) return;

    m = audio_mix();

    /* Silence the layers first, under the lock, so nothing is reading the old
       family while it is being dropped. */
    audio_lock();
    loop_stop(&S.l_main);
    loop_stop(&S.l_idle);
    loop_stop(&S.l_trans);
    loop_stop(&S.l_ws);
    S.l_main.snd = S.l_idle.snd = S.l_ws.snd = S.l_trans.snd = -1;
    S.trans_slot = -1;
    /* A different car is a different motor: the latch goes with the family. */
    S.eng_on = 0;
    S.eng_was_on = 0;
    S.eng_off_t = 0.f;
    audio_unlock();

    /* Refcounts and the memory-card read, both UNLOCKED -- this is the several
       megabytes that used to hold the output thread off. Unload before load so
       an entry shared with the new family keeps its pcm rather than being freed
       and read back. */
    for (i = 0; i < M_COUNT; i++)
        if (S.motor[i] >= 0) mix_unload(m, S.motor[i]);
    for (i = 0; i < M_COUNT; i++) {
        snprintf(name, sizeof(name), "%s%s", MOTOR_PREFIX[car_index],
                 MOTOR_SUFFIX[i]);
        S.motor[i] = find_load(name);
    }

    /* Publish, and free what the unload above dropped to zero. mix_trim frees
       PCM and has to be able to silence a voice still pointing at it, so it is
       the one part that belongs inside the lock. */
    audio_lock();
    S.l_main.snd = S.motor[M_ENGINE];
    S.l_idle.snd = S.motor[M_IDLE];
    S.l_ws.snd = S.motor[M_WS];
    mix_trim(m);
    audio_unlock();

    S.car = car_index;
}

void sfx_set_track(int track_index, const col_t *col)
{
    mix_t *m;
    int old, snd;

    if (!S.ok) return;
    S.col = col;
    if (track_index == S.track) return;
    if (track_index < 0 || track_index >= N_TRACKS) return;

    m = audio_mix();

    audio_lock();
    loop_stop(&S.l_amb);
    old = S.l_amb.snd;
    S.l_amb.snd = -1;
    audio_unlock();

    /* Unlocked, for the same reason sfx_set_car's family load is -- an ambient
       bed is a long wav and this runs on every track change. */
    if (old >= 0) mix_unload(m, old);
    snd = TRACKS[track_index].ambient[0]
              ? find_load(TRACKS[track_index].ambient) : -1;

    audio_lock();
    S.l_amb.snd = snd;
    mix_trim(m);
    audio_unlock();

    S.track = track_index;
    /* The surface loop is per-material and the material set is per-track, so
       force it to re-pick on the next update rather than carrying the old one
       across a track change. */
    S.surf_mat = -1;
}

/* Every loop this file owns, stopped. Shared by sfx_pause and sfx_race_active,
   because "nothing about the world is sounding" is one operation however it was
   asked for. */
static void sfx_stop_loops(void)
{
    int i;
    audio_lock();
    loop_stop(&S.l_main);
    loop_stop(&S.l_idle);
    loop_stop(&S.l_trans);
    loop_stop(&S.l_ws);
    loop_stop(&S.l_surf);
    loop_stop(&S.l_amb);
    /* The opponents go quiet too. sfx_ai_motor also declines to start a voice
       while stopped, so nothing restarts them until the world runs again. */
    for (i = 0; i < SFX_AI_MAX; i++)
        loop_stop(&S.l_ai[i]);
    audio_unlock();
}

void sfx_pause(int paused)
{
    if (!S.ok || paused == S.paused) return;
    S.paused = paused;
    /* One-shots are deliberately left to ring out, which is what sfx_pause has
       always done -- a cue that was already sounding when the menu opened is
       part of what just happened. sfx_race_active is the one that refuses them,
       because under the front end there is no "just happened". */
    if (paused)
        sfx_stop_loops();
}

/* SEE sfx.h. The front end's own switch, and the reason it is HERE and not three
   `if' statements in main.c is that it kept being one `if' short. */
void sfx_race_active(int active)
{
    if (!S.ok || S.no_race == !active) return;
    S.no_race = !active;
    if (S.no_race)
        sfx_stop_loops();
}

/* The car has just been put on the grid. 0x00509ff0 zeroes the whole sound
   block whenever a car is not audible, so a car that has just been created
   starts with the motor flag clear -- this is that, at the one place the port
   has a race start. It clears the edge tracker too: a grid reset is not an
   engine being switched off, so it must not play the stop transition. */
void sfx_engine_off(void)
{
    S.eng_on = 0;
    S.eng_was_on = 0;
    S.eng_off_t = 0.f;
    if (!S.ok) return;
    audio_lock();
    loop_stop(&S.l_main);
    loop_stop(&S.l_idle);
    loop_stop(&S.l_trans);
    loop_stop(&S.l_ws);
    S.trans_slot = -1;
    audio_unlock();
}

/* ---- the per-frame model ------------------------------------------------- */

/* The car's top speed in m/s, from its own tuning: speedBaseMax is km/h (see
   PHYSICS.md -- it is divided by 3.6 before comparison) and the resonator
   upgrade scales it, which is why the engine note rises with the upgrade. */
static float top_speed(const rb_car *c)
{
    int r = c->reso_upgrade;
    float s = c->tune.speed_base_max;
    if (r >= 0 && r < 4) s *= c->tune.resonator_speed[r];
    s /= 3.6f;
    return s > 0.5f ? s : 0.5f;
}

static void set_trans(int slot)
{
    mix_t *m = audio_mix();
    if (slot == S.trans_slot) return;
    loop_stop(&S.l_trans);
    S.trans_slot = slot;
    S.l_trans.snd = (slot >= 0) ? S.motor[slot] : -1;
    (void)m;
}

void sfx_update(const rb_car *c, const float eye[3], float eye_yaw_deg, float dt)
{
    mix_t *m;
    float v[3], speed, fwd_speed, ratio, load, pitch;
    float g_idle, g_eng, g_ws, slip;
    int i, contacts, airborne, in_water, mat;

    if (!S.ok || !c) return;
    m = audio_mix();

    audio_lock();
    mix_listener(m, eye[0], eye[1], eye[2], eye_yaw_deg);
    audio_unlock();

    if (S.paused || S.no_race) return;

    /* --- read the car ---------------------------------------------------- */
    v[0] = c->body.v[0]; v[1] = c->body.v[1]; v[2] = c->body.v[2];
    speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    /* forward is the body's local +Z: row 2 of the row-vector matrix. See
       "The two models have OPPOSITE forward conventions" in CLAUDE.md. */
    fwd_speed = v[0] * c->m[8] + v[1] * c->m[9] + v[2] * c->m[10];
    ratio = clampf(speed / top_speed(c), 0.f, 1.4f);

    contacts = 0;
    in_water = 0;
    slip = 0.f;
    for (i = 0; i < c->nwheels; i++) {
        if (c->hit[i].active) contacts++;
        if (c->hit[i].in_water) in_water = 1;
        if (fabsf(c->wheel[i].spin_extra) > slip)
            slip = fabsf(c->wheel[i].spin_extra);
    }
    airborne = (contacts == 0);

    load = clampf(c->in.throttle, 0.f, 1.f);

    /* material under the car; water wins over whatever is beneath it */
    mat = SURF_ASPHALT;
    if (S.col) mat = col_material_at(S.col, c->body.x[0], c->body.x[1], c->body.x[2]);
    if (mat < 0 || mat >= SURF_COUNT) mat = SURF_ASPHALT;
    if (in_water) mat = SURF_WATER;
    S.cur_surface = mat;

    /* --- the motor latch, before any of the three layers ------------------
     *
     * This is the engine's own, not a port model: see ENG_OFF_TIME. The motor
     * state is 0 while the car is under ENG_ON_SPEED with neither accelerate nor
     * brake held; the flag rises the instant it leaves that, and falls again
     * after ENG_OFF_TIME continuous seconds back in it. Coasting fast with
     * nothing pressed is the state machine's 4, which neither raises nor lowers
     * it, so the timer is simply held.
     *
     * The countdown needs no special case at all and does not get one. The car
     * is on the grid, main.c has zeroed the controls and is spending no ticks,
     * so speed is 0 and both inputs are clear -- state 0 from a flag that
     * respawn() cleared. The engine starts on GO with the first throttle. */
    if (c->in.accel || c->in.brake) {
        S.eng_off_t = 0.f;
        S.eng_on = 1;
    } else if (speed < ENG_ON_SPEED) {
        S.eng_off_t += dt;
        if (S.eng_off_t >= ENG_OFF_TIME)
            S.eng_on = 0;
    } else {
        S.eng_off_t = 0.f;
    }

    audio_lock();

    /* The stop transition, on the falling edge and nowhere else. 0x0050a2a0
       plays it exactly here, off the 0xe7c8 / 0xe7c4 pair this mirrors. */
    if (S.eng_was_on && !S.eng_on)
        one_shot(S.motor[M_OFF], ENG_GAIN, 1.f);
    S.eng_was_on = S.eng_on;

    /* Not "gain 0" -- no voice at all, which is what the callback's early return
       means. The surface loop, the ambient bed and the one-shots below are NOT
       part of the motor and keep running either way: a car rolling with the
       engine off still makes tyre noise. */
    if (!S.eng_on) {
        loop_stop(&S.l_main);
        loop_stop(&S.l_idle);
        loop_stop(&S.l_trans);
        loop_stop(&S.l_ws);
        S.trans_slot = -1;
    } else {
        /* --- layer 1: the sustained engine, idle crossfaded into running ------ */
        /* A car at rest with no throttle is pure idle; either revs or road speed
           brings the running loop up. They overlap deliberately -- crossfading to
           silence in the middle leaves an audible hole at walking pace. */
        g_eng = clampf(load > ratio ? load : ratio, 0.f, 1.f);
        g_idle = 1.f - clampf(g_eng * 1.3f, 0.f, 1.f);
        pitch = ENG_PITCH_LO + (ENG_PITCH_HI - ENG_PITCH_LO) * clampf(ratio, 0.f, 1.f);
        /* Off the ground the wheels are free, so the note runs up with throttle
           rather than with road speed. This is why car_flying exists in the set. */
        if (airborne) pitch += 0.25f * load;

        loop_drive(&S.l_main, ENG_GAIN * g_eng, pitch, PRIO_ENGINE);
        loop_drive(&S.l_idle, IDLE_GAIN * g_idle,
                   IDLE_PITCH_LO + (IDLE_PITCH_HI - IDLE_PITCH_LO) * load,
                   PRIO_ENGINE);

        /* --- layer 2: the transient -- accel, decel or reverse ---------------- */
        if (c->gear < 0 && fwd_speed < -0.2f) {
            set_trans(M_REVERSE);
            loop_drive(&S.l_trans, TRANS_GAIN * clampf(-fwd_speed / 2.f, 0.f, 1.f),
                       0.9f + 0.4f * ratio, PRIO_ENGINE);
        } else if (load > 0.05f) {
            set_trans(M_ACCEL);
            /* loudest while actually pulling: full throttle well below top speed */
            loop_drive(&S.l_trans, TRANS_GAIN * load * (1.f - 0.6f * ratio),
                       0.85f + 0.55f * ratio, PRIO_ENGINE);
        } else if (ratio > 0.08f) {
            set_trans(M_DECEL);
            loop_drive(&S.l_trans, TRANS_GAIN * clampf(ratio, 0.f, 1.f) * 0.8f,
                       0.85f + 0.5f * ratio, PRIO_ENGINE);
        } else {
            set_trans(-1);
            loop_stop(&S.l_trans);
        }

        /* --- layer 3: wheelspin ---------------------------------------------- */
        g_ws = clampf(slip / WS_FULL_SLIP, 0.f, 1.f);
        if (airborne) g_ws = 0.f;               /* nothing to spin against */
        loop_drive(&S.l_ws, WS_GAIN * g_ws, 0.9f + 0.5f * ratio, PRIO_ENGINE);
    }

    /* --- the surface loop ------------------------------------------------- */
    if (airborne) {
        loop_stop(&S.l_surf);
        S.surf_mat = -1;
    } else {
        float g = clampf(ratio / SURF_FULL_SPEED, 0.f, 1.f);
        g *= (float)contacts / (float)(c->nwheels ? c->nwheels : 1);
        if (mat != S.surf_mat) {
            loop_stop(&S.l_surf);
            S.surf_mat = mat;
            S.l_surf.snd = S.surf_snd[mat];
        }
        loop_drive(&S.l_surf, SURF_GAIN * g, 0.9f + 0.3f * ratio, PRIO_SURFACE);
    }

    /* --- the track's ambient bed ------------------------------------------ */
    loop_drive(&S.l_amb, AMBIENT_GAIN, 1.f, PRIO_AMBIENT);

    /* --- one-shots inferred from state ------------------------------------ */
    if (S.hit_cool > 0.f) S.hit_cool -= dt;
    if (S.brake_cool > 0.f) S.brake_cool -= dt;
    if (S.cdt_cool > 0.f) S.cdt_cool -= dt;
    if (S.car_cool > 0.f) S.car_cool -= dt;
    if (S.bullet_cool > 0.f) S.bullet_cool -= dt;
    for (i = 0; i < PROP_N_MODELS; i++)
        if (S.prop_cool[i] > 0.f) S.prop_cool[i] -= dt;
    for (i = 0; i < SFX_VOICE_COUNT; i++)
        if (S.voice_cool[i] > 0.f) S.voice_cool[i] -= dt;

    /* landing: contact regained, scaled by how hard it came down */
    if (S.was_airborne && !airborne && S.hit_cool <= 0.f) {
        float impact = -S.prev_vy;
        if (impact > LAND_MIN_SPEED) {
            float g = clampf((impact - LAND_MIN_SPEED) /
                             (LAND_FULL_SPEED - LAND_MIN_SPEED), 0.f, 1.f);
            one_shot(S.hit_snd[mat], 0.5f + 0.5f * g, 0.95f + 0.1f * g);
            one_shot(S.snd_landing, 0.4f + 0.6f * g, 1.f);
            S.hit_cool = 0.15f;
        }
    }

    /* entering water */
    if (in_water && !S.was_in_water)
        one_shot(S.snd_splash, clampf(0.3f + speed * 0.15f, 0.f, 1.f), 1.f);

    /* braking hard while rolling */
    if (c->in.brake && speed > 1.0f && S.brake_cool <= 0.f) {
        one_shot(S.snd_brake, 0.6f, 1.f);
        S.brake_cool = 1.2f;
    }

    audio_unlock();

    S.was_airborne = airborne;
    S.was_in_water = in_water;
    S.prev_vy = v[1];
}

/* ---- explicit one-shots -------------------------------------------------- */

void sfx_ui(sfx_ui_t which)
{
    if (!S.ok || (int)which < 0 || (int)which >= 5) return;
    audio_lock();
    one_shot(S.ui_snd[which], 0.9f, 1.f);
    audio_unlock();
}

/* THE SIX THAT BELONG TO A RACE -- the checkpoint, the wrong way, the beside
   cue, the pre-start, the 3-2-1 and the spawn. Every one of them is dropped
   while there is no race, which is what keeps them off the main menu; sfx_ui is
   deliberately NOT one of these, because the menu is what needs it. */
#define SFX_CUE(fn, field, gain)          \
    void fn(void) {                       \
        if (!S.ok || S.no_race) return;   \
        audio_lock();                     \
        one_shot(S.field, gain, 1.f);     \
        audio_unlock();                   \
    }

SFX_CUE(sfx_checkpoint, snd_cp, 0.9f)
SFX_CUE(sfx_wrongway, snd_wrong, 0.9f)
SFX_CUE(sfx_cp_beside, snd_beside, 0.8f)
SFX_CUE(sfx_prestart, snd_prestart, 1.0f)
SFX_CUE(sfx_countdown, snd_start, 1.0f)
SFX_CUE(sfx_respawn, snd_reset, 0.9f)

void sfx_prop_hit(int model, const float pos[3], float speed)
{
    const prop_model_t *pm;
    float g, pitch;

    if (!S.ok || S.paused || S.no_race || !pos)
        return;
    if (model < 0 || model >= PROP_N_MODELS)
        return;
    if (speed < PROP_MIN_SPEED)
        return;                     /* a nudge too gentle to be a knock */
    if (S.prop_cool[model] > 0.f)
        return;                     /* a contact chattering, not a new hit */

    pm = &PROP_MODELS[model];
    g = clampf((speed - PROP_MIN_SPEED) / (PROP_FULL_SPEED - PROP_MIN_SPEED),
               0.f, 1.f);
    g = PROP_GAIN_FLOOR + (1.f - PROP_GAIN_FLOOR) * g;
    /* A harder knock rings higher. The port's, and small enough to read as the
       same object rather than a different one. */
    pitch = 0.94f + 0.12f * g;
    S.prop_cool[model] = PROP_HIT_COOL;

    audio_lock();
    if (S.prop_snd[model] >= 0)
        mix_play_3d(audio_mix(), S.prop_snd[model], pos[0], pos[1], pos[2],
                    pm->snd_rmin, pm->snd_rmax, g, pitch, 0, PRIO_ONESHOT);
    /* And the car's own knock. Not positional -- it is the player's own car, and
       the engine puts it on car_CDT&Boost with the rest of the car's cues. */
    if (S.cdt_cool <= 0.f) {
        one_shot(S.snd_cdt_obj, 0.55f + 0.45f * g, 1.f);
        S.cdt_cool = PROP_CDT_COOL;
    }
    audio_unlock();
}

/* The radii the models themselves declare -- see sfx.h. Kept here rather than
   in char_data.h because they are an AUDIO property and this is the audio
   layer, the same split col_material_at keeps from the surface classifier. */
static const struct { float rmin, rmax; } VOICE_R[SFX_VOICE_COUNT] = {
    { 15.0f, 5.0f },        /* Dog       */
    { 50.0f, 5.0f },        /* Seagull   */
    { 20.0f, 0.0f },        /* Man       */
    { 20.0f, 0.0f }         /* Woman     */
};

#define VOICE_COOL 0.35f    /* THE PORT'S: a dog re-triggering its own bark */

void sfx_char_voice(sfx_voice_t which, const float pos[3], float gain)
{
    if (!S.ok || S.paused || S.no_race || !pos)
        return;
    if ((int)which < 0 || (int)which >= SFX_VOICE_COUNT)
        return;
    if (S.voice_snd[which] < 0 || S.voice_cool[which] > 0.f)
        return;
    if (gain < 0.f) gain = 0.f;
    if (gain > 1.f) gain = 1.f;
    S.voice_cool[which] = VOICE_COOL;
    audio_lock();
    mix_play_3d(audio_mix(), S.voice_snd[which], pos[0], pos[1], pos[2],
                VOICE_R[which].rmin, VOICE_R[which].rmax, gain, 1.f, 0,
                PRIO_ONESHOT);
    audio_unlock();
}

/*
 * The same one-shot, ASKED FOR BY THE WAV NAME THE MODEL ITSELF DECLARES.
 *
 * The four resident voices are named by the models (see sfx.h), so a caller that
 * has read a model's own MOD_SNDCHANNEL can hand the name straight over and never
 * decide anything: a model with no channel hands over NULL and a model naming a
 * wav the bank has no voice for is silent too, which is the data's answer in both
 * cases. This exists because the caller's own answer -- "everything that is not a
 * Woman is a Man" -- gave a squashed CRAB a man's voice, and the Crab has no
 * channel at all.
 */
void sfx_char_wav(const char *wav, const float pos[3], float gain)
{
    int i;
    if (!wav) return;
    for (i = 0; i < SFX_VOICE_COUNT; i++)
        if (VOICE_WAV[i] && !strcmp(VOICE_WAV[i], wav)) {
            sfx_char_voice((sfx_voice_t)i, pos, gain);
            return;
        }
}

void sfx_ai_motor(int slot, const float pos[3], float speed_ratio, int active)
{
    loop_t *l;
    float pitch, gain;
    mix_t *m;

    if (!S.ok || slot < 0 || slot >= SFX_AI_MAX)
        return;
    l = &S.l_ai[slot];
    if (l->snd < 0)
        return;

    audio_lock();
    if (!active || S.paused || S.no_race) {
        loop_stop(l);
        audio_unlock();
        return;
    }
    S.ai_pos[slot][0] = pos[0];
    S.ai_pos[slot][1] = pos[1];
    S.ai_pos[slot][2] = pos[2];
    speed_ratio = clampf(speed_ratio, 0.f, 1.4f);
    pitch = SFX_AI_PITCH_LO
          + (SFX_AI_PITCH_HI - SFX_AI_PITCH_LO) * clampf(speed_ratio, 0.f, 1.f);
    gain = SFX_AI_GAIN * S.vol_sfx;
    m = audio_mix();
    if (!mix_alive(m, l->v))
        l->v = mix_play_3d(m, l->snd, pos[0], pos[1], pos[2],
                           SFX_AI_RMIN, SFX_AI_RMAX, gain, pitch, 1,
                           PRIO_ENGINE);
    else {
        mix_set_pos(m, l->v, pos[0], pos[1], pos[2]);
        mix_set(m, l->v, gain, pitch);
    }
    audio_unlock();
}

void sfx_car_hit(float speed)
{
    float g;

    if (!S.ok || S.snd_cdt_car < 0 || S.paused || S.no_race)
        return;
    if (speed < PROP_MIN_SPEED)
        return;                     /* a nudge, not a hit */
    if (S.car_cool > 0.f)
        return;
    g = clampf((speed - PROP_MIN_SPEED) / (PROP_FULL_SPEED - PROP_MIN_SPEED),
               0.f, 1.f);
    g = PROP_GAIN_FLOOR + (1.f - PROP_GAIN_FLOOR) * g;
    S.car_cool = PROP_CDT_COOL;
    audio_lock();
    one_shot(S.snd_cdt_car, g, 0.96f + 0.08f * g);
    audio_unlock();
}

/*
 * A ROUND FROM A GUARD'S BURST, and the hit it lands.
 *
 * `bullet` is positional at the muzzle; `car_cdt_bullet` is not, because it is
 * the player's own car being hit and the engine keeps every car_cdt_* cue on
 * car_CDT&Boost with the rest of the car's own -- the same split sfx_prop_hit
 * already makes between the prop's knock and the car's.
 *
 * THE RADII ARE THE PORT'S. The burst manager's own MOD_SNDCHANNEL is inside
 * the 'BRMN' object the engine spawns by 4CC, which is not in any of the three
 * character databases pack_chars.py reads, and chn.dat carries a name and a
 * volume and no routing at all. So these are the props' declared 30/8 knock
 * pair -- the nearest thing in the shipped data to a sharp report -- and they
 * are marked rather than presented as recovered.
 */
#define BULLET_RMIN 30.0f
#define BULLET_RMAX 8.0f
#define BULLET_COOL 0.05f       /* under one TimeShoot, so all six are heard */

void sfx_bullet(const float pos[3])
{
    if (!S.ok || S.paused || S.no_race || !pos || S.snd_bullet < 0)
        return;
    if (S.bullet_cool > 0.f)
        return;
    S.bullet_cool = BULLET_COOL;
    audio_lock();
    mix_play_3d(audio_mix(), S.snd_bullet, pos[0], pos[1], pos[2],
                BULLET_RMIN, BULLET_RMAX, 1.f, 1.f, 0, PRIO_ONESHOT);
    audio_unlock();
}

void sfx_bullet_hit(void)
{
    if (!S.ok || S.paused || S.no_race || S.snd_cdt_bullet < 0)
        return;
    audio_lock();
    one_shot(S.snd_cdt_bullet, 1.f, 1.f);
    audio_unlock();
}

void sfx_ai_silence(void)
{
    int i;
    if (!S.ok)
        return;
    audio_lock();
    for (i = 0; i < SFX_AI_MAX; i++)
        loop_stop(&S.l_ai[i]);
    audio_unlock();
}

void sfx_volumes(float sfx, float music)
{
    S.vol_sfx = clampf(sfx, 0.f, 1.f);
    S.vol_music = clampf(music, 0.f, 1.f);
    if (!S.ok) return;
    audio_lock();
    mix_master(audio_mix(), S.vol_sfx, S.vol_music);
    audio_unlock();
}

float sfx_vol_sfx(void) { return S.vol_sfx; }
float sfx_vol_music(void) { return S.vol_music; }

const char *sfx_surface_name(int mat)
{
    if (mat < 0 || mat >= SURF_COUNT) return "?";
    return SURF_LABEL[mat];
}

int sfx_current_surface(void) { return S.cur_surface; }

unsigned sfx_resident_bytes(void)
{
    unsigned n;
    if (!S.ok) return 0;
    audio_lock();
    n = mix_resident(audio_mix());
    audio_unlock();
    return n;
}
