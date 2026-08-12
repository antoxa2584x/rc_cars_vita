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

/* Master levels, 0..1. Persisted by the menu. */
void sfx_volumes(float sfx, float music);
float sfx_vol_sfx(void);
float sfx_vol_music(void);

/* Telemetry for main.c's once-a-second log line. */
const char *sfx_surface_name(int mat);
int   sfx_current_surface(void);
unsigned sfx_resident_bytes(void);

#endif /* SFX_H */
