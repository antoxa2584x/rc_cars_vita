/*
 * mix.c -- sound bank + software mixer. See mix.h.
 *
 * Pure C: no psp2, no vitaGL, no threads. audio.c owns all of that and calls
 * mix_render() from the sceAudioOut thread with the voice lock held.
 */

#include "mix.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ bank */

static int ci_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
}

int mix_init(mix_t *m, const char *bank_path)
{
    unsigned hdr[4], i;
    FILE *f;

    memset(m, 0, sizeof(*m));
    m->master_sfx = 1.f;
    m->master_music = 1.f;
    m->gen_next = 1;

    f = fopen(bank_path, "rb");
    if (!f) return -1;
    if (fread(hdr, 4, 4, f) != 4 || memcmp(hdr, "SBK1", 4) != 0) {
        fclose(f);
        return -2;
    }
    m->rate = hdr[1];
    m->n_ents = hdr[2];
    if (!m->rate || !m->n_ents || m->n_ents > 65536) { fclose(f); return -3; }

    m->ents = (sbk_entry *)calloc(m->n_ents, sizeof(sbk_entry));
    if (!m->ents) { fclose(f); return -4; }

    for (i = 0; i < m->n_ents; i++) {
        unsigned rec[3];
        if (fread(m->ents[i].name, 1, SBK_NAME, f) != SBK_NAME ||
            fread(rec, 4, 3, f) != 3) {
            fclose(f);
            free(m->ents);
            m->ents = NULL;
            return -5;
        }
        m->ents[i].name[SBK_NAME - 1] = 0;
        m->ents[i].off = rec[0];
        m->ents[i].nsamp = rec[1];
        m->ents[i].volume = rec[2];
    }
    fclose(f);

    snprintf(m->path, sizeof(m->path), "%s", bank_path);

    m->acc_cap = 2048;
    m->acc = (float *)malloc(sizeof(float) * 2 * m->acc_cap);
    if (!m->acc) { free(m->ents); m->ents = NULL; return -6; }
    return 0;
}

void mix_free(mix_t *m)
{
    unsigned i;
    if (m->ents) {
        for (i = 0; i < m->n_ents; i++) free(m->ents[i].pcm);
        free(m->ents);
    }
    free(m->acc);
    free(m->music.ring);
    memset(m, 0, sizeof(*m));
}

int mix_find(const mix_t *m, const char *name)
{
    /* pack_snd.py writes the index in lowercase name order, so this could be a
       binary search. It is linear on purpose: 118 entries, and every lookup
       happens at load time, never per frame. sfx.c resolves once and keeps the
       index. */
    unsigned i;
    if (!name) return -1;
    for (i = 0; i < m->n_ents; i++)
        if (ci_cmp(m->ents[i].name, name) == 0) return (int)i;
    return -1;
}

int mix_load(mix_t *m, int snd)
{
    sbk_entry *e;
    FILE *f;
    short *p;

    if (snd < 0 || (unsigned)snd >= m->n_ents) return -1;
    e = &m->ents[snd];
    if (e->pcm) { e->refs++; return 0; }
    if (!e->nsamp) { e->refs++; return 0; }

    p = (short *)malloc((size_t)e->nsamp * 2);
    if (!p) return -1;
    f = fopen(m->path, "rb");
    if (!f) { free(p); return -1; }
    if (fseek(f, (long)e->off, SEEK_SET) != 0 ||
        fread(p, 2, e->nsamp, f) != e->nsamp) {
        fclose(f);
        free(p);
        return -1;
    }
    fclose(f);
    e->pcm = p;
    e->refs++;
    return 0;
}

void mix_unload(mix_t *m, int snd)
{
    sbk_entry *e;
    if (snd < 0 || (unsigned)snd >= m->n_ents) return;
    e = &m->ents[snd];
    if (e->refs > 0) e->refs--;
}

void mix_trim(mix_t *m)
{
    unsigned i, j;
    for (i = 0; i < m->n_ents; i++) {
        if (m->ents[i].refs > 0 || !m->ents[i].pcm) continue;
        /* A voice still reading this PCM would be left dangling. Silence any
           such voice before the free -- this is the one place the bank and the
           voice array can disagree, and it is reached from the game thread
           while the audio thread is blocked on the lock. */
        for (j = 0; j < MIX_VOICES; j++)
            if (m->v[j].active && m->v[j].snd == (int)i) {
                m->v[j].active = 0;
                m->v[j].pcm = NULL;
            }
        free(m->ents[i].pcm);
        m->ents[i].pcm = NULL;
    }
}

unsigned mix_resident(const mix_t *m)
{
    unsigned i, n = 0;
    for (i = 0; i < m->n_ents; i++)
        if (m->ents[i].pcm) n += m->ents[i].nsamp * 2;
    return n;
}

/* ---------------------------------------------------------------- voices */

/* Distance and pan.
 *
 * The engine's own curve is NOT recovered. What IS recovered is the pair of
 * radii each MOD_SNDCHANNEL carries -- 0x50E0 and 0x50E1 in the .sb, 0.0 and
 * 3.0 on country_4's transformators -- and their meaning is unambiguous from
 * the data: rmax 0 on the per-track `ground_noise_snd` bed, which is plainly
 * global, and 3.0 on a point source in a 1:10 scale world.
 *
 * So: full gain inside rmin, silent at and beyond rmax, and an inverse rolloff
 * between the two, normalised to hit exactly 0 at rmax. Inverse rather than
 * linear because linear-with-distance sounds wrong -- it stays too loud for too
 * long and then drops off a cliff. This is a port choice and is marked as one.
 *
 * Pan attenuates the FAR ear and leaves the near one alone, with a floor so a
 * source directly to one side is not silent on the other. Forward is
 * (sin yaw, 0, cos yaw) and right is (cos yaw, 0, -sin yaw): see "The
 * renderer's yaw convention is MIRRORED" in CLAUDE.md, and note that
 * mix_listener() is fed rbcar/cam's render yaw, not vehicle_t's.
 *
 * A constant-power law was tried first and is wrong for this mixer. Its centre
 * value is cos(pi/4) = 0.707, so a source dead ahead at rmin came out 3 dB down
 * on the same source played non-positionally -- the distance term said "full
 * gain" and the pan quietly took a third of it away. Normalising the centre to
 * 1 instead makes a hard-panned source LOUDER than a centred one, which is what
 * constant power means and is a poor trade with 24 voices summing into a fixed
 * s16 rail. Attenuating only the far ear keeps every voice's peak bounded by
 * its own gain, which is the property the headroom depends on.
 */
#define MIX_PAN_FLOOR 0.30f     /* port choice: how hard a full-side pan is */

void mix_pan(const mix_t *m, const mix_v *v, float *l, float *r)
{
    float g = v->gain;
    float dx, dz, d, s, c, right, pl, pr, a;

    if (!v->positional || v->rmax <= 0.f) {
        *l = *r = g;
        return;
    }

    dx = v->x - m->lx;
    dz = v->z - m->lz;
    {
        float dy = v->y - m->ly;
        d = sqrtf(dx * dx + dy * dy + dz * dz);
    }

    if (d >= v->rmax) { *l = *r = 0.f; return; }
    if (d > v->rmin) {
        float rmin = v->rmin > 0.f ? v->rmin : 0.01f;
        /* inverse rolloff, then subtract the value it would have at rmax so the
           curve actually reaches zero instead of stopping at a step */
        float inv = rmin / d;
        float end = rmin / v->rmax;
        g *= (inv - end) / (1.f - end);
        if (g < 0.f) g = 0.f;
    }

    /* direction in listener space */
    s = sinf(m->lyaw * (float)(M_PI / 180.0));
    c = cosf(m->lyaw * (float)(M_PI / 180.0));
    d = sqrtf(dx * dx + dz * dz);
    if (d < 1e-4f) { *l = *r = g; return; }
    right = (dx * c - dz * s) / d;          /* right = (cos yaw, 0, -sin yaw) */

    /* Attenuate only the far ear: centre leaves both at 1, a hard pan takes the
       far one down to the floor. Never boosts, so a voice's peak stays bounded
       by its own gain. */
    a = 1.f - MIX_PAN_FLOOR;
    pl = (right > 0.f) ? (1.f - a * right) : 1.f;
    pr = (right < 0.f) ? (1.f + a * right) : 1.f;

    *l = g * pl;
    *r = g * pr;
}

static void voice_targets(mix_t *m, mix_v *v)
{
    float l, r, vol;
    mix_pan(m, v, &l, &r);
    /* the engine's own per-sound level from snd.dat, then the master */
    vol = (v->snd >= 0 && (unsigned)v->snd < m->n_ents)
              ? m->ents[v->snd].volume * 0.01f : 1.f;
    v->tl = l * vol * m->master_sfx;
    v->tr = r * vol * m->master_sfx;
}

static int pick_slot(mix_t *m, int prio)
{
    int i, worst = -1;
    int worst_prio = prio;
    double worst_pos = -1.0;

    for (i = 0; i < MIX_VOICES; i++)
        if (!m->v[i].active) return i;

    /* All busy. Steal the lowest-priority voice, and among equals the one
       furthest through its sample -- a one-shot about to end is the cheapest
       thing to lose. A looping voice never "ends", so its progress is measured
       modulo its length and it competes on priority alone. */
    for (i = 0; i < MIX_VOICES; i++) {
        mix_v *v = &m->v[i];
        double prog;
        if (v->prio > worst_prio) continue;
        prog = v->loop ? 0.0 : (v->nsamp ? v->pos / (double)v->nsamp : 1.0);
        if (v->prio < worst_prio || prog > worst_pos) {
            worst_prio = v->prio;
            worst_pos = prog;
            worst = i;
        }
    }
    return worst;
}

static mix_voice start(mix_t *m, int snd, float gain, float pitch,
                       int loop, int prio)
{
    mix_voice h = MIX_NOVOICE;
    sbk_entry *e;
    mix_v *v;
    int i;

    if (snd < 0 || (unsigned)snd >= m->n_ents) return h;
    e = &m->ents[snd];
    if (!e->pcm || !e->nsamp) return h;         /* not paged in -- silent, not a crash */

    i = pick_slot(m, prio);
    if (i < 0) return h;

    v = &m->v[i];
    memset(v, 0, sizeof(*v));
    v->active = 1;
    v->gen = m->gen_next++;
    if (!m->gen_next) m->gen_next = 1;
    v->snd = snd;
    v->pcm = e->pcm;
    v->nsamp = e->nsamp;
    v->pos = 0.0;
    v->pitch = pitch > 0.f ? pitch : 1.f;
    v->step = v->pitch * (float)m->rate / (float)MIX_RATE;
    v->loop = loop;
    v->prio = prio;
    v->gain = gain;

    h.slot = i;
    h.gen = v->gen;
    return h;
}

mix_voice mix_play(mix_t *m, int snd, float gain, float pitch, int loop, int prio)
{
    mix_voice h = start(m, snd, gain, pitch, loop, prio);
    if (h.slot >= 0) {
        mix_v *v = &m->v[h.slot];
        v->positional = 0;
        voice_targets(m, v);
        /* A one-shot must not fade IN from zero -- that is an audible attack on
           every impact. Start at the target and ramp only on later changes. */
        v->gl = v->tl;
        v->gr = v->tr;
    }
    return h;
}

mix_voice mix_play_3d(mix_t *m, int snd, float x, float y, float z,
                      float rmin, float rmax, float gain, float pitch,
                      int loop, int prio)
{
    mix_voice h = start(m, snd, gain, pitch, loop, prio);
    if (h.slot >= 0) {
        mix_v *v = &m->v[h.slot];
        v->positional = 1;
        v->x = x; v->y = y; v->z = z;
        v->rmin = rmin; v->rmax = rmax;
        voice_targets(m, v);
        v->gl = v->tl;
        v->gr = v->tr;
    }
    return h;
}

static mix_v *resolve(mix_t *m, mix_voice h)
{
    mix_v *v;
    if (h.slot < 0 || h.slot >= MIX_VOICES) return NULL;
    v = &m->v[h.slot];
    if (!v->active || v->gen != h.gen) return NULL;
    return v;
}

int mix_alive(const mix_t *m, mix_voice h)
{
    const mix_v *v;
    if (h.slot < 0 || h.slot >= MIX_VOICES) return 0;
    v = &m->v[h.slot];
    return v->active && v->gen == h.gen;
}

void mix_set(mix_t *m, mix_voice h, float gain, float pitch)
{
    mix_v *v = resolve(m, h);
    if (!v) return;
    v->gain = gain;
    if (pitch > 0.f) {
        v->pitch = pitch;
        v->step = pitch * (float)m->rate / (float)MIX_RATE;
    }
    voice_targets(m, v);
}

void mix_set_pos(mix_t *m, mix_voice h, float x, float y, float z)
{
    mix_v *v = resolve(m, h);
    if (!v) return;
    v->x = x; v->y = y; v->z = z;
    v->positional = 1;
    voice_targets(m, v);
}

void mix_stop(mix_t *m, mix_voice h)
{
    mix_v *v = resolve(m, h);
    if (!v) return;
    v->active = 0;
    v->pcm = NULL;
}

void mix_stop_all(mix_t *m)
{
    int i;
    for (i = 0; i < MIX_VOICES; i++) {
        m->v[i].active = 0;
        m->v[i].pcm = NULL;
    }
}

void mix_listener(mix_t *m, float x, float y, float z, float yaw_deg)
{
    int i;
    m->lx = x; m->ly = y; m->lz = z; m->lyaw = yaw_deg;
    for (i = 0; i < MIX_VOICES; i++)
        if (m->v[i].active && m->v[i].positional)
            voice_targets(m, &m->v[i]);
}

void mix_master(mix_t *m, float sfx, float music)
{
    int i;
    if (sfx >= 0.f) m->master_sfx = sfx;
    if (music >= 0.f) m->master_music = music;
    for (i = 0; i < MIX_VOICES; i++)
        if (m->v[i].active) voice_targets(m, &m->v[i]);
}

/* ----------------------------------------------------------------- music */

int mix_music_init(mix_t *m, unsigned frames)
{
    free(m->music.ring);
    m->music.ring = (short *)calloc((size_t)frames * 2, sizeof(short));
    if (!m->music.ring) { m->music.cap = 0; return -1; }
    m->music.cap = frames;
    m->music.wr = m->music.rd = 0;
    m->music.gain = 1.f;
    m->music.playing = 1;
    return 0;
}

void mix_music_gain(mix_t *m, float g) { m->music.gain = g; }

unsigned mix_music_space(const mix_t *m)
{
    unsigned rd;
    if (!m->music.cap) return 0;
    /* acquire on the consumer's counter, so space seen here never overstates
       what the mixer has actually finished reading */
    rd = __atomic_load_n(&m->music.rd, __ATOMIC_ACQUIRE);
    return m->music.cap - 1 - (m->music.wr - rd);
}

/*
 * The ring is single-producer (audio.c's decoder thread) / single-consumer (its
 * output thread) and is deliberately NOT under the voice lock -- putting it there
 * would make a 10 ms decoder tick contend with the mixer's 23 ms deadline for no
 * reason. What that costs is a barrier: on ARM the samples and the counter can
 * become visible to the other core in either order, so a consumer can see `wr`
 * advance over frames it then reads as whatever was in the ring before. Publish
 * the counter with a release, take it with an acquire, and the pairing holds.
 */
void mix_music_write(mix_t *m, const short *stereo, unsigned frames)
{
    unsigned i, wr;
    if (!m->music.cap) return;
    wr = m->music.wr;
    for (i = 0; i < frames; i++) {
        unsigned s = (wr + i) % m->music.cap;
        m->music.ring[s * 2 + 0] = stereo[i * 2 + 0];
        m->music.ring[s * 2 + 1] = stereo[i * 2 + 1];
    }
    __atomic_store_n(&m->music.wr, wr + frames, __ATOMIC_RELEASE);
}

unsigned mix_music_played(const mix_t *m)
{
    if (!m->music.cap) return 0;
    /* acquire, pairing with mix_render's release store */
    return __atomic_load_n(&m->music.rd, __ATOMIC_ACQUIRE);
}

void mix_music_reset(mix_t *m)
{
    m->music.wr = m->music.rd = 0;
    if (m->music.ring && m->music.cap)
        memset(m->music.ring, 0, (size_t)m->music.cap * 2 * sizeof(short));
}

/* --------------------------------------------------------------- render */

static short clamp16(float x)
{
    if (x > 32767.f) return 32767;
    if (x < -32768.f) return -32768;
    return (short)(x < 0.f ? x - 0.5f : x + 0.5f);
}

void mix_render(mix_t *m, short *out, unsigned frames)
{
    unsigned i, n;
    int k;
    float *acc = m->acc;

    if (frames > m->acc_cap) frames = m->acc_cap;
    memset(acc, 0, sizeof(float) * 2 * frames);

    for (k = 0; k < MIX_VOICES; k++) {
        mix_v *v = &m->v[k];
        float gl, gr, dl, dr;
        if (!v->active || !v->pcm || !v->nsamp) continue;

        /* Ramp the gain across the block instead of stepping it at the edge.
           sfx.c rewrites the engine loop's gain every frame; stepped, that is a
           buzz at the frame rate rather than a smooth swell. */
        gl = v->gl;
        gr = v->gr;
        dl = (v->tl - gl) / (float)frames;
        dr = (v->tr - gr) / (float)frames;

        for (i = 0; i < frames; i++) {
            double p = v->pos;
            unsigned j;
            float f, s0, s1, s;

            if (p >= (double)v->nsamp) {
                if (!v->loop) { v->active = 0; v->pcm = NULL; break; }
                p = fmod(p, (double)v->nsamp);
                v->pos = p;
            }
            j = (unsigned)p;
            f = (float)(p - (double)j);
            s0 = v->pcm[j];
            /* At the very end of a looping sample the partner sample is the
               first one, not a repeat of the last: interpolating against the
               last sample flattens the join and ticks once per loop. */
            s1 = (j + 1 < v->nsamp) ? v->pcm[j + 1]
                                    : (v->loop ? v->pcm[0] : s0);
            s = s0 + (s1 - s0) * f;

            acc[i * 2 + 0] += s * gl;
            acc[i * 2 + 1] += s * gr;

            gl += dl;
            gr += dr;
            v->pos += v->step;
        }
        v->gl = v->tl;
        v->gr = v->tr;
    }

    /* music */
    if (m->music.cap && m->music.playing) {
        float g = m->music.gain * m->master_music;
        /* acquire, pairing with mix_music_write's release */
        unsigned wr = __atomic_load_n(&m->music.wr, __ATOMIC_ACQUIRE);
        unsigned rd = m->music.rd;
        unsigned avail = wr - rd;
        n = frames < avail ? frames : avail;
        for (i = 0; i < n; i++) {
            unsigned s = (rd + i) % m->music.cap;
            acc[i * 2 + 0] += m->music.ring[s * 2 + 0] * g;
            acc[i * 2 + 1] += m->music.ring[s * 2 + 1] * g;
        }
        __atomic_store_n(&m->music.rd, rd + n, __ATOMIC_RELEASE);
        /* Underrun leaves silence in the tail rather than repeating the last
           block; a repeat is a much louder artefact than a gap. */
    }

    for (i = 0; i < frames * 2; i++) out[i] = clamp16(acc[i]);
}
