/*
 * sfx.h -- the game's sounds, driven by the transcribed physics.
 *
 * This is the layer main.c talks to. It owns the working set (which sounds are
 * paged into the bank for the current car and track), the looping voices, and
 * the one-shot triggers. mix.c does the mixing; audio.c owns the threads.
 *
 * WHAT IS RECOVERED AND WHAT IS NOT
 * ---------------------------------
 * Recovered, and used as data:
 *   - the sound list and each sound's volume       Sound/snd.dat
 *   - the channel list, which is the structure     Sound/chn.dat
 *   - the per-track ambient bed and its sound      the .sb MOD_SNDCHANNEL nodes
 *   - the music playlist and its two groups        Autoexec.gm
 *
 * NOT recovered, and therefore a port model marked as one at each site:
 *   - the engine's crossfade law between idle / running / accel / decel. What
 *     the data fixes is the SHAPE: chn.dat declares exactly three motor
 *     channels per car (car_motor, car_motor_2, car_motor_ws) and the wav set
 *     has exactly three families to match (the sustained loops, the transients,
 *     and the _ws wheelspin layer), so the three-layer structure below is the
 *     game's. The gains and pitches inside it are ours.
 *   - the surface classifier (0x00534fc0). pack_col.py classifies by texture
 *     name instead; see there for the measured coverage.
 *   - the placement of the water and object ambiences. The .sb gives radii
 *     (0x50E0/0x50E1, e.g. 3.0 m on country_4's transformers) but the parent is
 *     a MOD_VOLUME whose extent is not in that subtree, so those point sources
 *     are NOT played. The global per-track bed, which is, is.
 */

#ifndef SFX_H
#define SFX_H

#include "rb.h"
#include "col.h"

/* Surface materials. MUST match pack_col.py's SURF_NAMES, index for index. */
#define SURF_ASPHALT 0
#define SURF_SAND    1
#define SURF_WETSAND 2
#define SURF_GRAVEL  3
#define SURF_STONE   4
#define SURF_GRASS   5
#define SURF_METAL   6
#define SURF_WOOD    7
#define SURF_WATER   8
#define SURF_COUNT   9

/* UI cues, by the name the engine's own snd.dat gives them. */
typedef enum {
    SFX_UI_FOCUS,       /* arrowfocus  -- moving between menu rows */
    SFX_UI_PRESS,       /* arrowpress  -- changing a value */
    SFX_UI_ENTER,       /* buttonpress -- opening / confirming */
    SFX_UI_BACK,        /* buttonfocus -- closing */
    SFX_UI_ERROR        /* msgboxerror */
} sfx_ui_t;

int  sfx_init(void);            /* after audio_init(). 0 on success. */
void sfx_shutdown(void);

/* Page in the working set for a car / track and start their loops. Safe to call
   repeatedly; changing either releases the previous set. The col_t must outlive
   the track selection -- sfx_update samples it for the surface material. */
void sfx_set_car(int car_index);
void sfx_set_track(int track_index, const col_t *col);

/* Per frame, with the car and the listener. `dt` in seconds. Handles the engine
   layers, the surface loop, landings, splashes and the ambient bed. */
void sfx_update(const rb_car *c, const float eye[3], float eye_yaw_deg, float dt);

/* Everything stops and the ambient bed goes quiet; used while the menu is up.
   The music keeps playing -- that is the menu's business, not this one's. */
void sfx_pause(int paused);

/* One-shots the game raises rather than sfx_update inferring them. */
void sfx_ui(sfx_ui_t which);
void sfx_checkpoint(void);      /* cp */
void sfx_wrongway(void);        /* cp_wrongway */
void sfx_prestart(void);        /* prestart */
/* cp_start -- the WHOLE 3-2-1-GO, four beeps 1.0000 s apart in one 4.44 s wav.
   Raise it ONCE, at the top of the countdown, and the beeps land on 3, 2, 1 and
   GO by themselves. countdown.h has the measurement and why it agrees with the
   exe's own message timing to the sample. */
void sfx_countdown(void);       /* cp_start */
void sfx_respawn(void);         /* cp_reset */

/* The car knocked a prop over. `model` indexes PROP_MODELS, `pos` is where the
   object is, `speed` the closing speed in m/s. Raise it on prop_t.hit -- which is
   an EDGE, see prop.h -- and it plays two things: the object's OWN sound,
   positionally, over the two radii its MOD_SNDCHANNEL in stone.sb declares, and
   the car's own car_cdt_obj cue. Both are rate-limited in here, so calling this
   once per prop struck in a frame is correct and does not stack up.

   Which sound and which radii are the game's data. The gain curve is the port's
   -- PROP_MIN_SPEED and friends in sfx.c. */
void sfx_prop_hit(int model, const float pos[3], float speed);

/*
 * A CHARACTER'S OWN VOICE, and every one of these is named by the model itself.
 *
 * Each MOD_MODEL in AIChars.sb and people.sb carries a MOD_SNDCHANNEL naming a
 * wav and its two radii -- the same structure gen_prop_data.py reads a prop's
 * knock out of, and the same one gen_tracks.py reads a track's ambient bed out
 * of. Four wavs cover the six models that have a channel:
 *
 *     Dog        dog_attack        rmin 15.0  rmax 5.0
 *     Seagull    seagull_vzliot    rmin 50.0  rmax 5.0   (vzlyot = takeoff)
 *     Man        man_voice         rmin 20.0  rmax 0.0
 *     Woman      woman_voice       rmin 20.0  rmax 0.0
 *     RepairMan  man_voice
 *     Guard      man_voice
 *
 * The Crab and the Spider have no channel and make no noise, which is the
 * data's answer rather than an omission.
 *
 * rmin > rmax on all of them, exactly as the props' 30/8 does, and it is passed
 * through as authored for the same reason: under mix_pan's law the pair means
 * full volume out to rmax and silence past it, which is a well-defined answer.
 * WHAT RAISES EACH ONE IS THE PORT'S -- char.c calls this on the edge of a Dog
 * starting an attack, a Seagull taking off, and a character being run over.
 */
typedef enum {
    SFX_VOICE_DOG,
    SFX_VOICE_SEAGULL,
    SFX_VOICE_MAN,
    SFX_VOICE_WOMAN,
    SFX_VOICE_COUNT
} sfx_voice_t;

void sfx_char_voice(sfx_voice_t which, const float pos[3], float gain);

/* The same one-shot, asked for by the WAV NAME a model declares in its own
   MOD_SNDCHANNEL (char.c's chr_model_wav). NULL, or a name no resident voice
   carries, is silent -- which is what the Crab and the Spider are. Use this
   wherever the model is known: it takes the decision out of the caller, and the
   caller's own version of it gave a squashed crab a man's voice. */
void sfx_char_wav(const char *wav, const float pos[3], float gain);

/* ------------------------------------------------------------- the opponents
 *
 * One positional engine loop per AI car, because that is all the shipped data
 * can support: `motorAI_accel1` is the ONLY motorAI wav in the pack. snd.dat
 * names `motorAI_decel3` too and there is no file for it -- one of the eleven
 * entries the engine's own findsoundpath() fails on -- so where the player gets
 * three layers (car_motor, car_motor_2, car_motor_ws) an opponent gets one,
 * pitched by how fast it is going.
 *
 * `slot` is 0..SFX_AI_MAX-1 and must stay the same for the same opponent from
 * frame to frame; it is the voice's identity. `speed_ratio` is the car's speed
 * over its own top speed. `active` 0 stops the voice, which is how an opponent
 * outside the audible radius costs nothing.
 *
 * The RADII are the port's and are anchored to the prop channels rather than
 * invented: stone.sb gives every knockable object rmin 30 / rmax 8, i.e. full
 * volume to 8 m and silence past it, and a car is a much bigger noise than a
 * falling can. SFX_AI_RMAX is that scaled to the length of a straight these
 * tracks have. Nothing recovered says how loud an opponent is -- the engine
 * attaches carAI_* to its own channels and the channel volumes in chn.dat carry
 * no radii. */
#define SFX_AI_MAX 5

/* Radii, gain and the pitch band, all THE PORT'S. The pitch range is the same
 * 0.80..1.70 the player's own sustained layer runs over, so an opponent
 * alongside reads as the same kind of engine. SFX_AI_RMAX is out here because
 * the CALLER needs it: mix_pan already attenuates to silence past it, so the
 * only reason to stop a distant voice is the voice budget, and only the caller
 * knows where the listener is. */
#define SFX_AI_RMIN  6.0f
#define SFX_AI_RMAX  40.0f
#define SFX_AI_GAIN  0.55f
#define SFX_AI_PITCH_LO 0.80f
#define SFX_AI_PITCH_HI 1.70f

void sfx_ai_motor(int slot, const float pos[3], float speed_ratio, int active);

/* Stop every opponent voice -- on a track change, a restart, or a pause. */
void sfx_ai_silence(void);

/* Car against car: `car_cdt_car`, which `snd.dat` names and which had nothing to
 * raise it until the opponents became solid. Not positional -- it is the
 * player's own car being hit, and the engine puts the car's collision cues on
 * its own car_CDT&Boost channel with the rest of them, which is what
 * sfx_prop_hit already does for `car_cdt_obj`. `speed` is the closing speed in
 * m/s; anything under PROP_MIN_SPEED is a nudge and is ignored, and it
 * rate-limits itself the same way the prop cue does. */
void sfx_car_hit(float speed);

/* Master levels, 0..1. Persisted by the menu. */
void sfx_volumes(float sfx, float music);
float sfx_vol_sfx(void);
float sfx_vol_music(void);

/* Telemetry for main.c's once-a-second log line. */
const char *sfx_surface_name(int mat);
int   sfx_current_surface(void);
unsigned sfx_resident_bytes(void);

#endif /* SFX_H */
