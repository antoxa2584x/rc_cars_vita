/*
 * audio.c -- sceAudioOut output thread, MP3 streaming, playlist. See audio.h.
 */

#include "audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "third_party/minimp3.h"

#ifdef __vita__
#include <psp2/audioout.h>
#include <psp2/kernel/cpu.h>          /* SCE_KERNEL_CPU_MASK_USER_* */
#include <psp2/kernel/threadmgr.h>
#endif

#define GRAIN        1024       /* frames per sceAudioOut block: 23 ms at 44.1k */
#define MUSIC_RING   32768      /* frames: 0.74 s of slack over a memory-card stall */
#define IOBUF        16384      /* MP3 read-ahead */

#define MAX_TRACKS   64
#define NAME_MAX_    64

typedef struct {
    char name[NAME_MAX_];
    int group;
} track_t;

static struct {
    int ok;
    mix_t mix;

    char music_dir[192];
    track_t tracks[MAX_TRACKS];
    int n_tracks;

    /* playlist cursor. `group` is -1 when the music is stopped. */
    volatile int group;
    volatile int cursor;        /* index into tracks[] of what is playing */
    volatile int want_next;     /* set by audio_music_next / group change */

    /* THE ONE-SHOT FILE -- the launch movie's soundtrack. `seq` is bumped by
       the game thread and copied by the decoder thread once it has opened the
       file, which is the whole handshake: the path is written before the bump
       and never touched after it, so the decoder always reads a settled one. */
    volatile int oneshot;
    volatile unsigned oneshot_seq, oneshot_ack;
    char oneshot_path[256];

    /* decoder state, owned by the decoder thread */
    FILE *mf;
    mp3dec_t mp3;
    unsigned char io[IOBUF];
    int io_len, io_pos;
    int mp3_rate, mp3_ch;

    short out[GRAIN * 2];

    volatile int running;

#ifdef __vita__
    int port;
    SceUID th_out, th_dec, mutex;
#endif
} A;

/* ------------------------------------------------------------------ lock */

void audio_lock(void)
{
#ifdef __vita__
    if (A.ok) sceKernelLockMutex(A.mutex, 1, NULL);
#endif
}

void audio_unlock(void)
{
#ifdef __vita__
    if (A.ok) sceKernelUnlockMutex(A.mutex, 1);
#endif
}

mix_t *audio_mix(void) { return &A.mix; }
int audio_ok(void) { return A.ok; }

void audio_master(float sfx, float music)
{
    if (!A.ok) return;
    audio_lock();
    mix_master(&A.mix, sfx, music);
    audio_unlock();
}

/* -------------------------------------------------------------- playlist */

static void read_playlist(const char *music_dir)
{
    char path[256];
    char line[256];
    FILE *f;

    snprintf(path, sizeof(path), "%s/music.idx", music_dir);
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f) && A.n_tracks < MAX_TRACKS) {
        char *tab;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        if (tab - line >= NAME_MAX_) continue;      /* absurd name: skip it */
        memcpy(A.tracks[A.n_tracks].name, line, (size_t)(tab - line));
        A.tracks[A.n_tracks].name[tab - line] = 0;
        A.tracks[A.n_tracks].group = atoi(tab + 1);
        A.n_tracks++;
    }
    fclose(f);
}

int audio_music_count(int group)
{
    int i, n = 0;
    for (i = 0; i < A.n_tracks; i++)
        if (A.tracks[i].group == group) n++;
    return n;
}

const char *audio_music_title(void)
{
    if (!A.ok || A.group < 0 || A.cursor < 0 || A.cursor >= A.n_tracks) return "";
    return A.tracks[A.cursor].name;
}

/* First track of `group` at or after `from`, wrapping. -1 if the group is
   empty -- which is why a missing music.idx costs you music and not a hang. */
static int next_in_group(int group, int from)
{
    int i;
    if (A.n_tracks <= 0) return -1;
    for (i = 0; i < A.n_tracks; i++) {
        int k = ((from + i) % A.n_tracks + A.n_tracks) % A.n_tracks;
        if (A.tracks[k].group == group) return k;
    }
    return -1;
}

void audio_music_group(int group)
{
    if (!A.ok) return;
    /* A one-shot is not a group, so `already playing that group' must not
       swallow the request that ends it -- the intro's last part is still on the
       decoder when main.c asks for the menu music. */
    if (group == A.group && !A.oneshot) return;
    A.oneshot = 0;
    A.group = group;
    A.cursor = (group < 0) ? -1 : next_in_group(group, 0);
    A.want_next = 1;
}

void audio_music_next(void)
{
    if (!A.ok || A.group < 0) return;
    A.oneshot = 0;
    A.cursor = next_in_group(A.group, A.cursor + 1);
    A.want_next = 1;
}

int audio_music_file(const char *path)
{
    FILE *probe;

    if (!A.ok || !path || !*path) return 0;
    /* OPENED HERE, AND CLOSED AGAIN, only to answer whether it exists. The
       decoder thread does the real open, so without this a missing file would
       report success -- and a caller pacing itself off audio_music_frames()
       would then wait forever for a clock that never starts. */
    probe = fopen(path, "rb");
    if (!probe) return 0;
    fclose(probe);

    A.group = -1;                       /* the playlist is not what is playing */
    A.want_next = 0;                    /* and the reset below is ours, not its */
    snprintf(A.oneshot_path, sizeof A.oneshot_path, "%s", path);
    /* Zeroed HERE rather than on the decoder thread, so audio_music_frames()
       reads 0 for this stream from the caller's next line -- see audio.h. The
       lock is what mix_render is called under. */
    audio_lock();
    mix_music_reset(&A.mix);
    audio_unlock();
    A.oneshot = 1;
    A.oneshot_seq++;
    return 1;
}

void audio_music_stop(void)
{
    if (!A.ok) return;
    A.oneshot = 0;
    A.group = -1;
    A.want_next = 1;
}

unsigned int audio_music_frames(void)
{
    if (!A.ok) return 0u;
    return mix_music_played(&A.mix);
}

/* -------------------------------------------------------------- decoding */

static void dec_close(void)
{
    if (A.mf) { fclose(A.mf); A.mf = NULL; }
    A.io_len = A.io_pos = 0;
}

static int dec_open_path(const char *path)
{
    dec_close();
    A.mf = fopen(path, "rb");
    if (!A.mf) return -1;
    mp3dec_init(&A.mp3);
    A.mp3_rate = 0;
    A.mp3_ch = 0;
    return 0;
}

static int dec_open(int idx)
{
    char path[320];
    if (idx < 0 || idx >= A.n_tracks) { dec_close(); return -1; }
    snprintf(path, sizeof(path), "%s/%s", A.music_dir, A.tracks[idx].name);
    return dec_open_path(path);
}

/* Decode one MP3 frame into the ring. -> frames written, 0 at end of file.
 *
 * minimp3 wants a window of bytes and tells us how many it consumed, so the
 * buffer is compacted and refilled each call rather than read frame by frame:
 * MP3 frames are not a fixed size and the ID3 tag at the head of these files is
 * skipped by the same mechanism (mp3dec_decode_frame returns 0 samples and a
 * non-zero consumed count over a tag, which is exactly what we want).
 */
static int dec_frame(void)
{
    mp3dec_frame_info_t info;
    short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int n;

    if (!A.mf) return 0;

    if (A.io_len - A.io_pos < 4096) {
        int keep = A.io_len - A.io_pos;
        size_t got;
        if (keep > 0) memmove(A.io, A.io + A.io_pos, (size_t)keep);
        A.io_pos = 0;
        A.io_len = keep;
        got = fread(A.io + A.io_len, 1, (size_t)(IOBUF - A.io_len), A.mf);
        A.io_len += (int)got;
    }
    if (A.io_len - A.io_pos <= 0) return 0;

    n = mp3dec_decode_frame(&A.mp3, A.io + A.io_pos, A.io_len - A.io_pos,
                            pcm, &info);
    if (info.frame_bytes <= 0) return 0;
    A.io_pos += info.frame_bytes;
    if (n <= 0) return -1;                  /* skipped a tag or a bad frame */

    A.mp3_rate = info.hz;
    A.mp3_ch = info.channels;

    /* These are all 44.1 kHz stereo, which is the mixer's own rate, so the
       common path is a straight copy. Mono and any other rate are handled
       rather than assumed away -- a nearest-neighbour step is fine for a
       fallback that the shipped soundtrack never takes. */
    if (info.channels == 2 && info.hz == MIX_RATE) {
        mix_music_write(&A.mix, pcm, (unsigned)n);
        return n;
    } else {
        static short tmp[MINIMP3_MAX_SAMPLES_PER_FRAME * 4];
        int outn = (int)((long long)n * MIX_RATE / (info.hz ? info.hz : MIX_RATE));
        int i;
        if (outn > (int)(sizeof(tmp) / (2 * sizeof(short))))
            outn = (int)(sizeof(tmp) / (2 * sizeof(short)));
        for (i = 0; i < outn; i++) {
            int j = (int)((long long)i * n / (outn ? outn : 1));
            short l, r;
            if (j >= n) j = n - 1;
            if (info.channels == 1) { l = r = pcm[j]; }
            else { l = pcm[j * 2]; r = pcm[j * 2 + 1]; }
            tmp[i * 2 + 0] = l;
            tmp[i * 2 + 1] = r;
        }
        mix_music_write(&A.mix, tmp, (unsigned)outn);
        return outn;
    }
}

/* One decoder step: honours a pending track change, tops the ring up, and
   advances the playlist at end of file. Shared by the Vita decoder thread and
   the host's audio_pump_host(). */
static void dec_step(void)
{
    int guard;

    /* THE ONE-SHOT FIRST, because it is the thing that is playing when it is
       set and the playlist below must not also get a turn. Its ring reset was
       done by audio_music_file on the game thread; all that is owed here is the
       open, once per request. */
    if (A.oneshot) {
        unsigned seq = A.oneshot_seq;
        if (A.oneshot_ack != seq) {
            A.oneshot_ack = seq;
            if (dec_open_path(A.oneshot_path) != 0) {
                A.oneshot = 0;
                return;
            }
        }
        if (!A.mf) return;
        for (guard = 0; guard < 64; guard++) {
            int n = (int)mix_music_space(&A.mix);
            if (n < 1152) return;
            /* End of file STOPS, and does not advance anything: a launch movie
               that looped its own logo music would be the playlist bug this
               entry point exists to avoid. */
            if (dec_frame() == 0) {
                dec_close();
                return;
            }
        }
        return;
    }

    if (A.want_next) {
        A.want_next = 0;
        audio_lock();
        mix_music_reset(&A.mix);
        audio_unlock();
        dec_close();
        if (A.group >= 0) dec_open(A.cursor);
    }
    if (A.group < 0 || !A.mf) return;

    /* Bounded so a stream of tag/garbage frames cannot spin here forever.
       mix_music_space and mix_music_write need no lock -- the ring is SPSC and
       this thread is its only producer (see mix.c). Taking the lock here meant
       up to 64 acquisitions per 10 ms tick, every one of them a chance to sit
       in front of the output thread's 23 ms deadline. */
    for (guard = 0; guard < 64; guard++) {
        int n = (int)mix_music_space(&A.mix);
        if (n < 1152) break;

        n = dec_frame();
        if (n == 0) {                       /* end of file -- next track */
            int nx = next_in_group(A.group, A.cursor + 1);
            dec_close();
            if (nx < 0) { A.group = -1; return; }
            A.cursor = nx;
            if (dec_open(nx) != 0) { A.group = -1; return; }
        }
    }
}

void audio_pump_host(void)
{
#ifndef __vita__
    if (A.ok) dec_step();
#endif
}

/* --------------------------------------------------------------- threads */

#ifdef __vita__

static int out_thread(SceSize argc, void *argv)
{
    (void)argc; (void)argv;
    while (A.running) {
        audio_lock();
        mix_render(&A.mix, A.out, GRAIN);
        audio_unlock();
        sceAudioOutOutput(A.port, A.out);
    }
    return 0;
}

static int dec_thread(SceSize argc, void *argv)
{
    (void)argc; (void)argv;
    while (A.running) {
        dec_step();
        sceKernelDelayThread(10000);        /* 10 ms; the ring holds 740 */
    }
    dec_close();
    return 0;
}

#endif

/* ------------------------------------------------------------- lifecycle */

int audio_init(const char *bank_path, const char *music_dir)
{
    memset(&A, 0, sizeof(A));
    A.group = -1;
    A.cursor = -1;

    if (mix_init(&A.mix, bank_path) != 0) return -1;
    if (mix_music_init(&A.mix, MUSIC_RING) != 0) { mix_free(&A.mix); return -2; }

    snprintf(A.music_dir, sizeof(A.music_dir), "%s", music_dir);
    read_playlist(music_dir);

#ifdef __vita__
    A.port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN,
                                 MIX_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (A.port < 0) { mix_free(&A.mix); return -3; }
    sceAudioOutSetVolume(A.port, SCE_AUDIO_VOLUME_FLAG_L_CH |
                                 SCE_AUDIO_VOLUME_FLAG_R_CH,
                         (int[]){ SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB });

    A.mutex = sceKernelCreateMutex("rccars_audio", 0, 0, NULL);
    if (A.mutex < 0) { sceAudioOutReleasePort(A.port); mix_free(&A.mix); return -4; }

    A.ok = 1;
    A.running = 1;

    /* The output thread has to beat the 23 ms grain; the decoder can be lazy
       because the ring is 740 ms deep. Hence the priority split.
     *
     * AND THEY GO ON THEIR OWN CORES. The affinity argument used to be 0, which is
     * SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT -- inherit the creator's -- so
     * both of these ran on core 0 alongside the render loop and the physics, and
     * the machine showed one core busy and two idle. A Vita gives an app three
     * (0, 1, 2; the fourth is the OS's).
     *
     * The mixer is the natural thing to move: it is pure C over its own bank and
     * ring buffers (mix.c touches no GL and no game state), it has a hard deadline
     * the render loop must not be able to push it past, and the MP3 decode is a
     * few hundred KB/s of work that has no business competing with the frame. */
    /* AND THE OUTPUT THREAD RUNS ABOVE DEFAULT. 0x10000100 is
     * SCE_KERNEL_DEFAULT_PRIORITY_USER, i.e. exactly the main thread's, and lower
     * numbers are HIGHER priority here. At parity the mixer only keeps its 23 ms
     * deadline while its core stays free -- which stops being true the moment the
     * loader, GXM's own worker or the kernel's I/O threads want core 1. It cannot
     * starve anything by being raised: it spends its life blocked, either in
     * sceAudioOutOutput or on the mutex. The decoder stays BELOW default; it is
     * working 740 ms ahead and has nothing to be urgent about. */
    A.th_out = sceKernelCreateThread("rccars_snd", out_thread, 0x10000100 - 32,
                                     0x10000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    A.th_dec = sceKernelCreateThread("rccars_mus", dec_thread, 0x10000100 + 16,
                                     0x10000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (A.th_out < 0 || A.th_dec < 0) {
        A.ok = 0;
        A.running = 0;
        sceKernelDeleteMutex(A.mutex);
        sceAudioOutReleasePort(A.port);
        mix_free(&A.mix);
        return -5;
    }
    sceKernelStartThread(A.th_out, 0, NULL);
    sceKernelStartThread(A.th_dec, 0, NULL);
#else
    A.ok = 1;
    A.running = 1;
#endif
    return 0;
}

void audio_shutdown(void)
{
    if (!A.ok) return;
#ifdef __vita__
    /* The output thread is blocked inside sceAudioOutOutput and clearing
       A.running does not wake it -- it only sees the flag after the block
       returns. The comment here used to say "releasing the port wakes it" and
       then waited BEFORE releasing anything, so shutdown hung on the wait until
       the block happened to come back on its own. Push one more (silent) buffer
       through by letting the port drain, with a bounded wait either way: a
       shutdown path must not be able to hang the exit. */
    sceKernelWaitThreadEnd(A.th_out, NULL, (SceUInt[]){ 500000 });   /* 0.5 s */
    sceKernelWaitThreadEnd(A.th_dec, NULL, (SceUInt[]){ 500000 });
    sceAudioOutReleasePort(A.port);
    sceKernelDeleteThread(A.th_out);
    sceKernelDeleteThread(A.th_dec);
    sceKernelDeleteMutex(A.mutex);
#endif
    dec_close();
    A.ok = 0;
    mix_free(&A.mix);
}
