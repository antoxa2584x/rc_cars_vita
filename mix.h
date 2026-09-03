/*
 * mix.h -- the sound bank and the software mixer.
 *
 * Deliberately free of psp2 and vitaGL, so audio_test.c can compile it on the
 * host and read back what it produces. Everything that decides how loud a sound
 * is, where it sits in the stereo field, how a loop wraps and which voice gets
 * stolen lives here; audio.c is only the sceAudioOut thread and the MP3
 * decoder that feed it.
 *
 * THE BANK
 * --------
 * assets/sound.sbk, built by rccars_re/pack_snd.py from the game's own
 * Sound/ wavs and Sound/snd.dat:
 *
 *     magic "SBK1", u32 rate, u32 count, u32 reserved
 *     count x { char name[32]; u32 offset; u32 samples; u32 volume; }
 *     mono s16 PCM, entries concatenated in name order
 *
 * `volume` is the engine's own per-sound level out of snd.dat (85..100 across
 * the retail set) and is applied on top of whatever gain the caller asks for --
 * that is the game's mix, not ours.
 *
 * The index is loaded at init and the PCM is NOT. 118 sounds come to 32.8 MB
 * and the working set is a fraction of it: one car's motor group, one track's
 * ambient bed, the common cues. mix_load()/mix_unload() page entries in and out
 * by refcount and sfx.c drives that at track and car load.
 *
 * RATES
 * -----
 * The bank is 22050 Hz mono (pack_snd.py's default) and the mixer runs at
 * MIX_RATE 44100 stereo, because the music is 44.1k stereo and halving it to
 * match the SFX would be audible on the one thing here that is actually music.
 * SFX therefore play at a source step of 0.5, which costs nothing -- the
 * interpolator has to run anyway for pitch.
 */

#ifndef MIX_H
#define MIX_H

#define MIX_RATE      44100     /* mixer + sceAudioOut output rate */
#define MIX_VOICES    24        /* simultaneous sfx voices */
#define SBK_NAME      32        /* must match pack_snd.py's SBK_NAME */

/* A voice handle. The generation counter is what makes a stale handle safe:
   sfx.c holds handles across frames for its looping voices, and a voice that
   finished or was stolen must not be reachable through the old handle -- that
   is how a one-shot ends up having its gain driven by an engine loop. */
typedef struct { int slot; unsigned gen; } mix_voice;

#define MIX_NOVOICE ((mix_voice){ -1, 0 })

typedef struct {
    char name[SBK_NAME];
    unsigned int off;           /* byte offset of the PCM in the bank file */
    unsigned int nsamp;         /* mono s16 sample count */
    unsigned int volume;        /* snd.dat volume, 0..100 */
    short *pcm;                 /* resident copy, NULL until mix_load() */
    int refs;
} sbk_entry;

typedef struct {
    int active;
    unsigned gen;
    int snd;                    /* bank index */
    const short *pcm;
    unsigned nsamp;

    double pos;                 /* fractional read cursor, in source samples */
    float step;                 /* source samples per output frame */
    float pitch;

    int loop;
    int prio;                   /* higher survives voice stealing */

    /* Gain is ramped across a block rather than stepped at its edge. A stepped
       gain clicks, and the engine loops change gain every single frame. */
    float gl, gr;               /* current, per-sample after the ramp */
    float tl, tr;               /* target for the end of the next block */

    int positional;
    float x, y, z;
    float rmin, rmax;           /* the .sb's 0x50E0 / 0x50E1 */
    float gain;                 /* caller gain, before distance and snd.dat */
} mix_v;

typedef struct {
    /* the music ring, written by audio.c's decoder thread, drained here */
    short *ring;                /* interleaved stereo s16 */
    unsigned cap;               /* frames */
    volatile unsigned wr, rd;   /* frame counters, free-running */
    float gain;
    int   playing;
} mix_music;

typedef struct {
    sbk_entry *ents;
    unsigned n_ents;
    unsigned rate;              /* bank rate, e.g. 22050 */
    char path[256];

    mix_v v[MIX_VOICES];
    unsigned gen_next;

    float master_sfx, master_music;

    /* listener: position and the app's render yaw, in degrees. See cam.c and
       "The renderer's yaw convention is MIRRORED" in CLAUDE.md -- forward is
       (sin yaw, 0, cos yaw) in the rb path, and the right vector follows. */
    float lx, ly, lz, lyaw;

    mix_music music;

    /* mix_render() accumulates here before clamping to s16. Sized for the
       largest grain audio.c asks for. */
    float *acc;
    unsigned acc_cap;
} mix_t;

/* --- lifecycle --------------------------------------------------------- */

/* Reads the bank INDEX only. 0 on success. */
int  mix_init(mix_t *m, const char *bank_path);
void mix_free(mix_t *m);

/* --- the bank ---------------------------------------------------------- */

/* Bank index by name, or -1. Case-insensitive, because snd.dat's names and the
   on-disk filenames disagree on case for 15 of the 117 sounds. */
int  mix_find(const mix_t *m, const char *name);

/* Page an entry's PCM in / out. Refcounted; loading twice costs one read. */
int  mix_load(mix_t *m, int snd);
void mix_unload(mix_t *m, int snd);
/* Drop every resident sound with no references. Called at track/car change. */
void mix_trim(mix_t *m);
/* Bytes of PCM currently resident -- what BUILD.md's memory budget cares about. */
unsigned mix_resident(const mix_t *m);

/* --- voices ------------------------------------------------------------ */

mix_voice mix_play(mix_t *m, int snd, float gain, float pitch, int loop, int prio);
/* rmin: full volume inside. rmax: silent outside. rmax <= 0 => non-positional. */
mix_voice mix_play_3d(mix_t *m, int snd, float x, float y, float z,
                      float rmin, float rmax, float gain, float pitch,
                      int loop, int prio);
void mix_set(mix_t *m, mix_voice h, float gain, float pitch);
void mix_set_pos(mix_t *m, mix_voice h, float x, float y, float z);
void mix_stop(mix_t *m, mix_voice h);
void mix_stop_all(mix_t *m);
int  mix_alive(const mix_t *m, mix_voice h);

void mix_listener(mix_t *m, float x, float y, float z, float yaw_deg);
void mix_master(mix_t *m, float sfx, float music);

/* --- music ------------------------------------------------------------- */

int  mix_music_init(mix_t *m, unsigned frames);
void mix_music_gain(mix_t *m, float g);
/* Frames the decoder may write, and the pointer to write them at. audio.c's
   decoder thread is the only writer; mix_render is the only reader. */
unsigned mix_music_space(const mix_t *m);
void mix_music_write(mix_t *m, const short *stereo, unsigned frames);
void mix_music_reset(mix_t *m);
/* Frames mix_render has actually drained out of the ring since the last reset --
   the AUDIO CLOCK. Only advances on frames that were really mixed, so it stops
   with an underrun rather than running ahead of what was heard. audio.h's
   audio_music_frames() is the interface the game should use. */
unsigned mix_music_played(const mix_t *m);

/* --- rendering --------------------------------------------------------- */

/* Mix `frames` stereo frames into `out`. Called from the audio thread. */
void mix_render(mix_t *m, short *out, unsigned frames);

/* The distance+pan law, exposed so audio_test.c can assert the curve itself
   rather than infer it from rendered samples. Writes left/right gain. */
void mix_pan(const mix_t *m, const mix_v *v, float *l, float *r);

#endif /* MIX_H */
