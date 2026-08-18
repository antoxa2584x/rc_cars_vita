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
    int snd_start;              /* cp_start -- the four countdown beeps */
    int snd_brake, snd_cdt_obj;
    /* The opponents: one positional loop each off the single motorAI wav that
       ships. See sfx.h. */
    int snd_ai_motor;
    int snd_cdt_car;
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
    S.voice_snd[SFX_VOICE_DOG]     = find_load("dog_attack");
    S.voice_snd[SFX_VOICE_SEAGULL] = find_load("seagull_vzliot");
    S.voice_snd[SFX_VOICE_MAN]     = find_load("man_voice");
    S.voice_snd[SFX_VOICE_WOMAN]   = find_load("woman_voice");

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

void sfx_pause(int paused)
{
    if (!S.ok || paused == S.paused) return;
    S.paused = paused;
    if (paused) {
        audio_lock();
        loop_stop(&S.l_main);
        loop_stop(&S.l_idle);
        loop_stop(&S.l_trans);
        loop_stop(&S.l_ws);
        loop_stop(&S.l_surf);
        loop_stop(&S.l_amb);
        /* The opponents go quiet too. sfx_ai_motor also declines to start a
           voice while paused, so nothing restarts them until the world runs
           again -- one-shots are deliberately left to ring out, which is what
           sfx_pause has always done. */
        {
            int i;
            for (i = 0; i < SFX_AI_MAX; i++)
                loop_stop(&S.l_ai[i]);
        }
        audio_unlock();
    }
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

    if (S.paused) return;

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

    audio_lock();

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

#define SFX_CUE(fn, field, gain)          \
    void fn(void) {                       \
        if (!S.ok) return;                \
        audio_lock();                     \
        one_shot(S.field, gain, 1.f);     \
        audio_unlock();                   \
    }

SFX_CUE(sfx_checkpoint, snd_cp, 0.9f)
SFX_CUE(sfx_wrongway, snd_wrong, 0.9f)
SFX_CUE(sfx_prestart, snd_prestart, 1.0f)
SFX_CUE(sfx_countdown, snd_start, 1.0f)
SFX_CUE(sfx_respawn, snd_reset, 0.9f)

void sfx_prop_hit(int model, const float pos[3], float speed)
{
    const prop_model_t *pm;
    float g, pitch;

    if (!S.ok || S.paused || !pos)
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
    if (!S.ok || S.paused || !pos)
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
    if (!active || S.paused) {
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

    if (!S.ok || S.snd_cdt_car < 0 || S.paused)
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
