/*
 * char.c -- the tracks' people, animals and road cars. See char.h.
 *
 * Three things happen here per frame, and only the first is expensive:
 *
 *   chr_pose      the animation. A clip is a per-part TRS -- its own 0x5410
 *                 rest pose with every channel that has keys overriding one
 *                 component -- composed down the tree into world[], then
 *                 world * rest_inv into draw[]. That last product is what a
 *                 BIND-POSE vertex goes through, which is the same trick
 *                 carani.c uses for the car's rig and the reason the packed
 *                 vertices can stay in model space.
 *   skin          for the four models that need it, v' = sum w_j draw[j] v.
 *                 On the CPU, into a malloc'd scratch buffer -- see char.h on
 *                 why static will not do.
 *   the machines  a few floats each.
 *
 * Only instances within CHR_DRAW_DIST are posed at all, which is what keeps
 * this off the frame budget: a Guard is 76 parts and 1,064 vertices, and a
 * track can place thirteen characters.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "char.h"
#include "rb.h"
#include "rlog.h"
#include "scene.h"

#define DEG (3.14159265358979f / 180.0f)

/* ------------------------------------------------------------------ maths --- */

static void m_ident(float *m)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* r = a * b, both column-major, both point-on-the-right. */
static void m_mul(float *r, const float *a, const float *b)
{
    int i, j, k;
    float t[16];
    for (j = 0; j < 4; j++)
        for (i = 0; i < 4; i++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++)
                s += a[k * 4 + i] * b[j * 4 + k];
            t[j * 4 + i] = s;
        }
    memcpy(r, t, sizeof t);
}

static void m_point(const float *m, const float *p, float *o)
{
    float x = p[0], y = p[1], z = p[2];
    o[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
    o[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
    o[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
}

/*
 * local(v) = t + R*S*v with R = Rz*Ry*Rx -- sb2obj.compose's rule, which is
 * read off the engine's own builder (FUN_004089b0, each rotate called with mode
 * 1 so it pre-multiplies). Getting this order wrong is invisible on a node with
 * only one non-zero angle and wrong on every joint that has two, which is most
 * of a skeleton.
 */
static void m_trs(float *m, const float *t, const float *s, const float *rdeg)
{
    float cx = cosf(rdeg[0] * DEG), sx = sinf(rdeg[0] * DEG);
    float cy = cosf(rdeg[1] * DEG), sy = sinf(rdeg[1] * DEG);
    float cz = cosf(rdeg[2] * DEG), sz = sinf(rdeg[2] * DEG);
    /* R = Rz * Ry * Rx, as a 3x3 in row-index/column-index terms */
    float r00 = cz * cy;
    float r01 = cz * sy * sx - sz * cx;
    float r02 = cz * sy * cx + sz * sx;
    float r10 = sz * cy;
    float r11 = sz * sy * sx + cz * cx;
    float r12 = sz * sy * cx - cz * sx;
    float r20 = -sy;
    float r21 = cy * sx;
    float r22 = cy * cx;
    m[0] = r00 * s[0];  m[1] = r10 * s[0];  m[2] = r20 * s[0];  m[3] = 0.0f;
    m[4] = r01 * s[1];  m[5] = r11 * s[1];  m[6] = r21 * s[1];  m[7] = 0.0f;
    m[8] = r02 * s[2];  m[9] = r12 * s[2];  m[10] = r22 * s[2]; m[11] = 0.0f;
    m[12] = t[0];       m[13] = t[1];       m[14] = t[2];       m[15] = 1.0f;
}

static float wrap180(float a)
{
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* --------------------------------------------------------------- sampling --- */

float chr_sample(const chr_chan_t *ch, float t)
{
    unsigned int lo, hi;
    float t0, v0, t1, v1, u;

    if (!ch || !ch->n)
        return 0.0f;
    if (ch->n == 1 || t <= ch->k[0])
        return ch->k[1];
    if (t >= ch->k[(ch->n - 1) * 2])
        return ch->k[(ch->n - 1) * 2 + 1];
    lo = 0;
    hi = ch->n - 1;
    while (hi - lo > 1) {
        unsigned int mid = (lo + hi) / 2;
        if (ch->k[mid * 2] <= t) lo = mid; else hi = mid;
    }
    t0 = ch->k[lo * 2];      v0 = ch->k[lo * 2 + 1];
    t1 = ch->k[hi * 2];      v1 = ch->k[hi * 2 + 1];
    if (t1 - t0 < 1e-9f)
        return v1;
    u = (t - t0) / (t1 - t0);
    return v0 + (v1 - v0) * u;
}

/* ------------------------------------------------------------------ names --- */

int chr_model_index(const chr_t *c, const char *name)
{
    unsigned int i;
    if (!c || !name) return -1;
    for (i = 0; i < c->n_models; i++)
        if (!strcmp(c->model[i].name, name))
            return (int)i;
    return -1;
}

int chr_part_index(const chr_model_t *m, const char *name)
{
    unsigned int i;
    if (!m || !name) return -1;
    for (i = 0; i < m->n_parts; i++)
        if (!strcmp(m->part[i].name, name))
            return (int)i;
    return -1;
}

int chr_clip_index(const chr_model_t *m, const char *name)
{
    unsigned int i;
    if (!m || !name) return -1;
    for (i = 0; i < m->n_clips; i++)
        if (!strcmp(m->clip[i].name, name))
            return (int)i;
    return -1;
}

/*
 * The wav ONE INSTANCE'S MODEL names in its own MOD_SNDCHANNEL, or NULL for a
 * model that has none -- the Crab, the Spider, the Vulture and the four road
 * cars, seven of the thirteen. char_data.h's CHR_SND is the table and it is read
 * out of the models themselves.
 *
 * This is here rather than in main.c because WHICH SOUND A MODEL OWNS is data;
 * what that sound is played through is the caller's, which is the same split
 * menu.c keeps from sfx.c. A cue from a model with no channel is silent, and it
 * was raising a man's voice for a squashed crab.
 */
const char *chr_model_wav(const chr_t *c, unsigned int idx)
{
    unsigned int j;
    const char *model;
    if (!c || idx >= c->n_inst) return NULL;
    model = c->inst[idx].place->model;
    for (j = 0; j < (unsigned)CHR_N_SND; j++)
        if (!strcmp(CHR_SND[j].model, model))
            return CHR_SND[j].wav;
    return NULL;
}

/* The first of several names the model has. The people call their walk "Walk"
   and the Dog calls its "run"; asking for a list rather than a name keeps the
   per-kind code from carrying a per-model table. */
static int clip_any(const chr_model_t *m, const char *const *names)
{
    int i;
    for (i = 0; names[i]; i++) {
        int k = chr_clip_index(m, names[i]);
        if (k >= 0) return k;
    }
    return -1;
}

/* --------------------------------------------------------------- volumes --- */

/*
 * A MOD_VOLUME is a y range and four (x, z) corners in traversal order. Tested
 * as a general convex quad -- the sign of the cross product of each edge with
 * the point has to agree all the way round -- rather than as the parallelogram
 * every shipped one happens to be. Both windings are accepted, because nothing
 * in the file fixes one.
 */
int chr_in_volume(const chr_volume_t *v, float x, float y, float z)
{
    int i, pos = 0, neg = 0;
    if (!v) return 0;
    if (v->yhi <= v->ylo) return 0;             /* absent */
    if (y < v->ylo || y > v->yhi) return 0;
    for (i = 0; i < 4; i++) {
        int j = (i + 1) & 3;
        float ex = v->qx[j] - v->qx[i];
        float ez = v->qz[j] - v->qz[i];
        float px = x - v->qx[i];
        float pz = z - v->qz[i];
        float cr = ex * pz - ez * px;
        if (cr > 0.0f) pos = 1;
        if (cr < 0.0f) neg = 1;
    }
    return !(pos && neg);
}

/* The same quad ignoring height. The Dog's attackVolume is authored with a y
   range that its own resetPlace does not always sit inside, and a dog that
   stops chasing because it ran down a step is not what the volume is for. */
static int in_quad(const chr_volume_t *v, float x, float z)
{
    chr_volume_t t;
    if (!v || v->yhi <= v->ylo) return 0;
    t = *v;
    t.ylo = -1e9f;
    t.yhi = 1e9f;
    return chr_in_volume(&t, x, 0.0f, z);
}

/* ------------------------------------------------------------------- load --- */

static int rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }

static void rname(FILE *f, char *out, size_t cap)
{
    unsigned short n = 0;
    char buf[256];
    rd(f, &n, 2);
    if (n > 255) n = 255;
    rd(f, buf, n);
    buf[n] = 0;
    if (cap) {
        size_t k = strlen(buf);
        if (k > cap - 1) k = cap - 1;
        memcpy(out, buf, k);
        out[k] = 0;
    }
}

static void free_model(chr_model_t *m)
{
    unsigned int i, j;
    for (i = 0; i < m->n_batches; i++) {
        free(m->batch[i].verts);
        free(m->batch[i].idx);
        free(m->batch[i].inf_n);
        free(m->batch[i].inf_part);
        free(m->batch[i].inf_w);
    }
    for (i = 0; i < m->n_clips; i++) {
        for (j = 0; j < m->clip[i].n_chan; j++)
            free(m->clip[i].chan[j].k);
        free(m->clip[i].chan);
        free(m->clip[i].pose);
        free(m->clip[i].pose_part);
    }
    free(m->batch);
    free(m->clip);
    free(m->part);
    free(m->var);
    memset(m, 0, sizeof(*m));
}

void char_free(chr_t *c)
{
    unsigned int i;
    if (!c) return;
    for (i = 0; i < c->n_models; i++)
        free_model(&c->model[i]);
    for (i = 0; i < c->n_paths; i++) {
        free(c->path[i].t);
        free(c->path[i].p);
        free(c->path[i].f);
        free(c->path[i].u);
        free(c->path[i].s);
    }
    if (c->tex && c->n_tex)
        glDeleteTextures((GLsizei)c->n_tex, c->tex);
    free(c->tex);
    free(c->model);
    free(c->path);
    {
        unsigned int r;
        for (r = 0; r < CHR_SKIN_RINGS; r++)
            free(c->skin[r]);
    }
    free(c->tex_name);
    free(c->tex_px);
    memset(c, 0, sizeof(*c));
}

static int load_file(chr_t *c, const char *path)
{
    FILE *f = fopen(path, "rb");
    char magic[4];
    unsigned int i, j, k;

    if (!f) return 0;
    if (!rd(f, magic, 4) || memcmp(magic, "CHR2", 4)) {
        rlog("[rccars] %s: bad magic\n", path);
        fclose(f);
        return 0;
    }
    rd(f, &c->n_tex, 4);
    rd(f, &c->n_models, 4);
    rd(f, &c->n_paths, 4);

    c->tex = c->n_tex ? calloc(c->n_tex, sizeof(GLuint)) : NULL;
    if (c->n_tex) {
        c->tex_name = calloc(c->n_tex, sizeof(*c->tex_name));
        c->tex_px = calloc(c->n_tex, 1);
        glGenTextures((GLsizei)c->n_tex, c->tex);
        for (i = 0; i < c->n_tex; i++) {
            int got = 0;
            scene_read_texture(f, c->tex[i], c->tex_name ? c->tex_name[i] : NULL,
                               c->tex_name ? CHR_NAME : 0, NULL, &got);
            if (c->tex_px) c->tex_px[i] = (unsigned char)got;
            /* scene.c reports the same thing for a track and then refuses to draw
               the batch; here it is the one fact that separates a model drawing
               WHITE (nothing bound) from one drawing black (bound, no image). */
            if (!got)
                rlog("[rccars] %s: texture %u '%s' has no image\n", path, i,
                     c->tex_name ? c->tex_name[i] : "?");
        }
    }

    c->model = c->n_models ? calloc(c->n_models, sizeof(chr_model_t)) : NULL;
    for (i = 0; i < c->n_models; i++) {
        chr_model_t *m = &c->model[i];
        rname(f, m->name, CHR_NAME);
        rd(f, &m->n_parts, 4);
        rd(f, &m->n_batches, 4);
        rd(f, &m->n_clips, 4);
        rd(f, &m->n_slot, 4);
        rd(f, &m->n_var, 4);
        rd(f, m->bmin, 12);
        rd(f, m->bmax, 12);
        rd(f, &m->yaw_off, 4);
        {
            float dx = m->bmax[0] - m->bmin[0];
            float dy = m->bmax[1] - m->bmin[1];
            float dz = m->bmax[2] - m->bmin[2];
            float far0 = 0.0f, t;
            int a;
            for (a = 0; a < 3; a++) {
                t = fabsf(m->bmin[a]);
                if (t > far0) far0 = t;
                t = fabsf(m->bmax[a]);
                if (t > far0) far0 = t;
            }
            (void)dx; (void)dy; (void)dz;
            m->radius = far0;
        }
        m->part = calloc(m->n_parts ? m->n_parts : 1, sizeof(chr_part_t));
        for (j = 0; j < m->n_parts; j++) {
            rname(f, m->part[j].name, CHR_NAME);
            rd(f, &m->part[j].parent, 4);
            rd(f, m->part[j].trs, 36);
            rd(f, m->part[j].bind, 36);
            rd(f, m->part[j].rest_inv, 64);
        }
        if (m->n_var && m->n_slot) {
            m->var = calloc(m->n_var * m->n_slot, sizeof(unsigned int));
            rd(f, m->var, m->n_var * m->n_slot * 4);
        }
        m->batch = calloc(m->n_batches ? m->n_batches : 1, sizeof(chr_batch_t));
        for (j = 0; j < m->n_batches; j++) {
            chr_batch_t *b = &m->batch[j];
            rd(f, &b->slot, 4);
            rd(f, &b->part, 4);
            rd(f, &b->nverts, 4);
            rd(f, &b->nidx, 4);
            rd(f, &b->skinned, 4);
            b->verts = malloc((size_t)b->nverts * 5 * sizeof(float));
            rd(f, b->verts, (size_t)b->nverts * 5 * sizeof(float));
            if (b->skinned) {
                b->inf_n = calloc(b->nverts ? b->nverts : 1, 1);
                b->inf_part = calloc((size_t)(b->nverts ? b->nverts : 1)
                                     * CHR_MAX_INF, sizeof(unsigned short));
                b->inf_w = calloc((size_t)(b->nverts ? b->nverts : 1)
                                  * CHR_MAX_INF, sizeof(float));
                for (k = 0; k < b->nverts; k++) {
                    unsigned char n = 0, q;
                    float tot = 0.0f;
                    rd(f, &n, 1);
                    for (q = 0; q < n; q++) {
                        unsigned short bp = 0;
                        float w = 0.0f;
                        rd(f, &bp, 2);
                        rd(f, &w, 4);
                        /* Past CHR_MAX_INF the influence is READ and dropped;
                           the file has to stay in step whatever this build's
                           cap is, and the weights were normalised at pack time
                           so dropping the tail leaves a small error rather than
                           a collapsed vertex. */
                        if (q < CHR_MAX_INF) {
                            b->inf_part[k * CHR_MAX_INF + q] = bp;
                            b->inf_w[k * CHR_MAX_INF + q] = w;
                            b->inf_n[k] = (unsigned char)(q + 1);
                            tot += w;
                        }
                    }
                    /* Renormalise what survived the cap. Without this a vertex
                       whose tail was dropped is pulled toward the origin by
                       exactly the weight that went missing. */
                    if (tot > 1e-6f && fabsf(tot - 1.0f) > 1e-6f)
                        for (q = 0; q < b->inf_n[k]; q++)
                            b->inf_w[k * CHR_MAX_INF + q] /= tot;
                }
                if (b->nverts > m->max_verts)
                    m->max_verts = b->nverts;
                /* Every skinned vertex this model submits in one frame -- the
                   skin ring is sized on the sum, not the largest, because each
                   batch needs its OWN slice. See CHR_SKIN_RINGS. */
                m->skin_verts += b->nverts;
            }
            b->idx = malloc((size_t)b->nidx * sizeof(unsigned short));
            rd(f, b->idx, (size_t)b->nidx * sizeof(unsigned short));
        }
        m->clip = calloc(m->n_clips ? m->n_clips : 1, sizeof(chr_clip_t));
        for (j = 0; j < m->n_clips; j++) {
            chr_clip_t *cl = &m->clip[j];
            rname(f, cl->name, CHR_NAME);
            rd(f, &cl->duration, 4);
            rd(f, &cl->frames, 4);
            rd(f, &cl->n_pose, 4);
            if (cl->n_pose) {
                cl->pose_part = calloc(cl->n_pose, sizeof(unsigned short));
                cl->pose = calloc(cl->n_pose * 9, sizeof(float));
                for (k = 0; k < cl->n_pose; k++) {
                    rd(f, &cl->pose_part[k], 2);
                    rd(f, &cl->pose[k * 9], 36);
                }
            }
            rd(f, &cl->n_chan, 4);
            if (cl->n_chan) {
                cl->chan = calloc(cl->n_chan, sizeof(chr_chan_t));
                for (k = 0; k < cl->n_chan; k++) {
                    chr_chan_t *ch = &cl->chan[k];
                    rd(f, &ch->part, 2);
                    rd(f, &ch->comp, 2);
                    rd(f, &ch->n, 4);
                    ch->k = malloc((size_t)ch->n * 2 * sizeof(float));
                    rd(f, ch->k, (size_t)ch->n * 2 * sizeof(float));
                }
            }
        }
        /* The model's collision proxy, by name, once. A model the table does
           not carry is not solid rather than solid by accident. */
        for (j = 0; j < (unsigned)CHR_N_PROXY; j++)
            if (!strcmp(CHR_PROXY[j].model, m->name)) {
                m->proxy = &CHR_PROXY[j];
                break;
            }
        if (m->n_parts > CHR_MAX_PARTS)
            rlog("[rccars] %s: %s has %u parts, CHR_MAX_PARTS is %d -- the tail "
                 "will not animate\n", path, m->name, m->n_parts,
                 CHR_MAX_PARTS);
    }

    c->path = c->n_paths ? calloc(c->n_paths, sizeof(chr_path_t)) : NULL;
    for (i = 0; i < c->n_paths; i++) {
        chr_path_t *pa = &c->path[i];
        rname(f, pa->name, CHR_NAME);
        rd(f, &pa->kind, 4);
        rd(f, &pa->n, 4);
        rd(f, &pa->duration, 4);
        pa->t = calloc(pa->n ? pa->n : 1, sizeof(float));
        pa->p = calloc((size_t)(pa->n ? pa->n : 1) * 3, sizeof(float));
        pa->f = calloc((size_t)(pa->n ? pa->n : 1) * 3, sizeof(float));
        pa->u = calloc((size_t)(pa->n ? pa->n : 1) * 3, sizeof(float));
        pa->s = calloc(pa->n ? pa->n : 1, sizeof(float));
        for (j = 0; j < pa->n; j++) {
            float r[11];
            rd(f, r, sizeof r);
            pa->t[j] = r[0];
            memcpy(&pa->p[j * 3], r + 1, 12);
            memcpy(&pa->f[j * 3], r + 4, 12);
            memcpy(&pa->u[j * 3], r + 7, 12);
            pa->s[j] = r[10];
        }
        pa->kind = pa->kind ? 1u : 0u;
    }
    fclose(f);
    return 1;
}

/* ------------------------------------------------------------- placement --- */

static float ground(const chr_t *c, float x, float z, float fallback, float ceil)
{
    float y = fallback, nx, ny, nz;
    if (c->col && col_ground_at(c->col, x, z, ceil, &y, &nx, &ny, &nz))
        return y;
    return fallback;
}

/*
 * The authored Y is a pivot that floats -- the same convention the props and
 * the checkpoint markers follow, and prop_init has to drop those too. A
 * character is put on the surface under its own (x, z), with a ceiling a metre
 * above the pivot so a Guard authored under a gantry is not lifted onto its
 * roof (which is exactly what beach_2's overpass did to the car's spawn).
 */
/*
 * THE FIRST TIME ALONG A RECORDING AT WHICH THE CHARACTER IS OUT OF THE SEA.
 *
 * beach_1's two people begin their recorded walks UNDER THE WATER. Measured
 * against the track's own water grid: the Man is at y -1.65 against a surface at
 * -0.37 for his first ~30 s of 469, and the Woman at -1.38 for her first ~60 s of
 * 201 -- 1,713 of her 3,603 samples are below the surface, and the visible mesh
 * within four metres of her there is the water plane at 0.00 and a sea bed at
 * -2.24, so she is not standing on anything. Both come ashore and walk the beach
 * for the rest of the recording.
 *
 * The sea is drawn after the characters and blends by depth (beach_1's ramp is
 * alpha 0.10 to 1.00 over magnetRadius), so a submerged person is at best a smear
 * and at a race start there is nothing there at all -- reported as "on the first
 * map two npc people are missing", and they were, for the first minute, which is
 * the minute anyone looks.
 *
 * THE PORT'S, and it moves the PHASE and not the recording: `phase` is already
 * this file's own field (see char_data.h), the samples are untouched, and a path
 * with no water under it -- every road car's -- is unaffected. What it picks is
 * the first sample the recording itself puts at or above the water, which is the
 * character walking rather than the character parked.
 */
static float first_dry_t(const chr_t *c, const chr_path_t *pa, float from)
{
    unsigned int j;
    if (!c->col || !pa->n)
        return from;
    for (j = 0; j < pa->n; j++) {
        float x = pa->p[j * 3], y = pa->p[j * 3 + 1], z = pa->p[j * 3 + 2];
        float w = 0.0f, g = 0.0f, nx, ny, nz;
        if (pa->t[j] < from)
            continue;
        /* Out of the sea AND standing on the track: the two halves of "in play".
           Without the second, beach_1's Woman starts a metre above her own sea
           bed, dry but hovering, because the recording leaves the water before it
           reaches the sand. */
        if (col_water_at(c->col, x, z, &w) && y < w)
            continue;
        if (!col_ground_at(c->col, x, z, y + 0.5f, &g, &nx, &ny, &nz))
            continue;
        if (y - g > 0.5f || y - g < -0.5f)
            continue;
        return pa->t[j];
    }
    return from;                    /* never in play: leave the phase alone */
}

/* Two probes -- see CHR_DROP_CEIL in char.h for why one is not enough. */
static void drop(const chr_t *c, chr_inst_t *in)
{
    float y, nx, ny, nz;
    if (!c->col)
        return;
    if (col_ground_at(c->col, in->x, in->z, in->y + CHR_DROP_CEIL, &y,
                      &nx, &ny, &nz)) {
        in->y = y;
        return;
    }
    if (col_ground_at(c->col, in->x, in->z, in->y + CHR_DROP_LIFT, &y,
                      &nx, &ny, &nz))
        in->y = y;
}

/*
 * WHICH CLIP MEANS WHAT, and the ENGINE'S OWN TABLES SAY -- see char_data.h's
 * "THE REACTION" for the three of them and the flag decoders that index them.
 * The lists here are ordered so the first name that a model carries is the one
 * the engine would have picked for that state, which matters most for the
 * Seagull: it has FIVE clips and two of them are airborne.
 *
 *   `look_around` is the Seagull's GROUND idle (one-shot, chaining to `walk`),
 *   not `flight_idle`, which is what it holds in the AIR. Reading it the other
 *   way is what stood a gull on the sand with its wings out for as long as this
 *   file has existed, and it is why "the seagull has no walk animation" -- the
 *   walk was there and the bird was hardly ever in it.
 */
static const char *const CLIP_IDLE[] = { "stand", "Stand", "look_around",
                                         "hide", NULL };
static const char *const CLIP_WALK[] = { "walk", "Walk", "WALK", "run", NULL };
static const char *const CLIP_RUN[] = { "run", "runQuick", "Walk", "WALK",
                                        NULL };
static const char *const CLIP_ACT[] = { "attack", "HIT", "hide", NULL };
static const char *const CLIP_HURT[] = { "Hurt", "HIT", NULL };
/* The two reactions, and the Guard's own names for them. It has no throw. */
static const char *const CLIP_KICK[] = { "Pendal", "kick", NULL };
static const char *const CLIP_THROW[] = { "Brosok", NULL };
static const char *const CLIP_SHOOT[] = { "shoot", NULL };

static void set_clip(chr_inst_t *in, const chr_model_t *m, int clip, float rate)
{
    if (clip < 0 || (unsigned)clip >= m->n_clips)
        return;
    if (in->clip != clip) {
        in->clip = clip;
        in->clip_t = 0.0f;
    }
    in->clip_rate = rate;
    in->once = 0;
}

/*
 * A ONE-SHOT CLIP. The engine's animation tables mark Brosok, Pendal, Hurt and
 * the guard's shoot as play-through and chain each back to Stand when it ends
 * (0x50f6c0 sets the flag and the successor for states 2, 3 and 4; 0x51ad50 for
 * the guard's 1); everything else loops. So the cursor has to STOP at the
 * duration rather than wrap, because the whole reaction is timed off it -- the
 * sole connects 0.65 s in and the car leaves the hand at 2.0 s, and a wrapped
 * cursor would fire both again every cycle.
 *
 * RATE 1.0 AND NOTHING ELSE. Every animation in the engine is advanced by the
 * constant at 0x554390, which is 1.0 -- 0x50f7d0 and 0x513820 both load it and
 * both compute an index they then throw away. That is what makes the recovered
 * cursors readable as seconds against the clip durations out of the .chr
 * (Pendal 3.000, Brosok 4.700, the guard's kick 3.000 and shoot 1.500).
 */
static void set_clip_once(chr_inst_t *in, const chr_model_t *m, int clip)
{
    if (clip < 0 || (unsigned)clip >= m->n_clips)
        return;
    if (in->clip != clip) {
        in->clip = clip;
        in->clip_t = 0.0f;
    }
    in->clip_rate = 1.0f;
    in->once = 1;
}

int chr_clip_done(const chr_t *c, unsigned int inst)
{
    const chr_inst_t *in;
    const chr_model_t *m;
    if (!c || inst >= c->n_inst)
        return 0;
    in = &c->inst[inst];
    m = &c->model[in->model];
    if (!in->once || in->clip < 0 || (unsigned)in->clip >= m->n_clips)
        return 0;
    return in->clip_t >= m->clip[in->clip].duration;
}

/*
 * A ROLL OF THE DICE, AND THE ENGINE HAS ONE WHERE THIS FILE DID NOT.
 *
 * 0x510f40 asks rand()/32768 whether to throw the car or kick it and 0x52de30
 * asks it three times for the burst's sweep axis, so two of the recovered
 * decisions are genuinely random and there was nothing here to be random with --
 * every other machine in this file is deterministic on purpose. This is a
 * per-instance 32-bit LCG (Numerical Recipes' constants), advanced only where
 * the engine advances its own: same sequence for the same sequence of
 * decisions, which is what a fixture needs to be able to drive both branches.
 */
static float rnd01(chr_inst_t *in)
{
    in->rnd = in->rnd * 1664525u + 1013904223u;
    return (float)((in->rnd >> 16) & 0x7fff) * (1.0f / 32768.0f);
}

/*
 * A clip's own forward speed, used to keep the feet from skating.
 *
 * The animations carry NO ROOT MOTION -- the Dog's run translates its CENTRE
 * node by 0.000 m over the cycle and bobs it 53 mm vertically -- so the ground
 * speed cannot be read out of them. What can be read is the stride RATE, and
 * the anchor for how far one stride covers is the model's own length: an animal
 * at a run covers about its own body length per cycle. So the clip is played at
 * `speed / (length / duration)`, which ties the two together whatever the speed
 * constant turns out to mean, and a character that is standing still plays at
 * 1.0 rather than stopping dead.
 *
 * THE PORT'S, and it is the honest form of a choice that has to be made: the
 * Dog's only speed-shaped constant is `AniSpeed` (4.0), whose NAME says
 * animation rate and whose VALUE reads as metres per second for a running dog.
 * Taking it as the ground speed and deriving the rate from it, or taking it as
 * the rate and deriving the speed, land within a factor of 1.5 of each other.
 */
static float stride_rate(const chr_model_t *m, int clip, float speed)
{
    float len, cycle;
    if (clip < 0 || (unsigned)clip >= m->n_clips)
        return 1.0f;
    cycle = m->clip[clip].duration;
    if (cycle < 1e-3f)
        return 1.0f;
    len = m->bmax[2] - m->bmin[2];
    if (m->bmax[0] - m->bmin[0] > len)
        len = m->bmax[0] - m->bmin[0];
    if (len < 0.05f)
        len = 0.05f;
    if (speed < 1e-3f)
        return 1.0f;
    return clampf(speed / (len / cycle), 0.25f, 4.0f);
}

/* char_reset places a path replayer by calling this with dt = 0 -- see there. */
static void advance_path(chr_t *c, chr_inst_t *in, const chr_model_t *m,
                         float dt);

int char_init(chr_t *c, const char *path, const char *track, const col_t *col)
{
    unsigned int t, i;
    const chr_track_t *ct = NULL;

    memset(c, 0, sizeof(*c));
    c->col = col;
    c->track = -1;
    for (t = 0; t < CHR_N_TRACKS; t++)
        if (!strcmp(CHR_TRACKS[t].track, track)) {
            ct = &CHR_TRACKS[t];
            c->track = (int)t;
            break;
        }
    if (!ct || !ct->n)
        return 0;
    if (!load_file(c, path)) {
        rlog("[rccars] %s: no characters loaded\n", path);
        return 0;
    }

    for (i = 0; i < ct->n && c->n_inst < CHR_MAX_INST; i++) {
        const chr_place_t *pl = &ct->place[i];
        chr_inst_t *in = &c->inst[c->n_inst];
        int mi = chr_model_index(c, pl->model);
        if (mi < 0) {
            rlog("[rccars] %s: no model %s for %s\n", path, pl->model,
                 pl->name);
            continue;
        }
        memset(in, 0, sizeof(*in));
        in->place = pl;
        in->model = mi;
        in->path = -1;
        if (pl->path) {
            unsigned int k;
            for (k = 0; k < c->n_paths; k++)
                if (!strcmp(c->path[k].name, pl->path)) {
                    in->path = (int)k;
                    break;
                }
            if (in->path < 0)
                rlog("[rccars] %s: %s wants path %s and it is not packed\n",
                     path, pl->name, pl->path);
        }
        /* clother / body, clamped to what this model actually packs -- a
           variant the artist named and the .csi tree does not carry falls back
           to set 0 rather than sampling an unbound texture. */
        in->variant = pl->variant;
        if ((unsigned)in->variant >= c->model[mi].n_var)
            in->variant = 0;
        c->n_inst++;
    }

    /*
     * The skin ring: CHR_SKIN_RINGS arenas, each big enough for every skinned
     * vertex every PLACED instance could submit in one frame. Not the largest
     * batch -- the sum -- because a batch handed to GXM as a client pointer is
     * read at flush and must still be its own by then. See char.h.
     *
     * The worst case is small: beach_2's eleven come to 11,485 verts (230 KB an
     * arena, 690 KB for the ring) and beach_3's thirteen to 13,366. Malloc'd,
     * because vitaGL maps only the heap for the GPU.
     */
    for (i = 0; i < c->n_inst; i++)
        c->skin_cap += c->model[c->inst[i].model].skin_verts;
    if (c->skin_cap) {
        unsigned int r;
        for (r = 0; r < CHR_SKIN_RINGS; r++) {
            c->skin[r] = malloc((size_t)c->skin_cap * 5 * sizeof(float));
            if (!c->skin[r]) {
                rlog("[rccars] %s: no room for skinning arena %u/%u (%u KB) -- "
                     "the skinned models will not draw\n", path, r + 1,
                     (unsigned)CHR_SKIN_RINGS,
                     (unsigned)(c->skin_cap * 5 * sizeof(float) / 1024));
                break;
            }
        }
    }

    char_reset(c);
    rlog("[rccars] %s: %u characters, %u models, %u paths, %u textures, "
         "skin ring %ux%u KB\n",
         path, c->n_inst, c->n_models, c->n_paths, c->n_tex,
         (unsigned)CHR_SKIN_RINGS,
         (unsigned)(c->skin_cap * 5 * sizeof(float) / 1024));
    return 1;
}

void char_reset(chr_t *c)
{
    unsigned int i;
    for (i = 0; i < c->n_inst; i++) {
        chr_inst_t *in = &c->inst[i];
        const chr_place_t *pl = in->place;
        const chr_model_t *m = &c->model[in->model];

        in->x = pl->x;
        in->y = pl->y;
        in->z = pl->z;
        in->yaw = wrap180(pl->yaw);
        in->pitch = in->roll = 0.0f;
        in->scale = pl->scale > 1e-4f ? pl->scale : 1.0f;
        if (pl->kind == CHR_KIND_PATH && !strcmp(pl->model, "Vulture")) {
            /* Both scales apply -- see char.h. */
            in->scale *= CHR_VULTURE_SCALE;
        }
        in->state = CHR_ST_IDLE;
        in->timer = 0.0f;
        in->speed = 0.0f;
        in->leg = 0;
        in->hit_cool = 0.0f;
        in->event = CHR_EV_NONE;
        /*
         * AND THE REACTION, WHICH MUST NOT SURVIVE A RESTART. `carry` is the
         * dangerous one: char_carrier is what tells main.c to put the car in
         * this instance's hand instead of simulating it, so a race restarted
         * while someone was mid-throw would hand the new car straight back to
         * him. `rnd` is seeded from the instance's index rather than left at
         * zero so two Guards on one track do not spray identically -- and
         * seeded rather than randomised, so a restart is reproducible.
         */
        in->react = CHR_RX_NONE;
        in->react_t = 0.0f;
        in->react_hit = 0;
        in->once = 0;
        in->carry = 0;
        in->carry_q[0] = in->carry_q[1] = in->carry_q[2] = in->carry_q[3] = 0.0f;
        in->shots = (int)CHR_BURST_NSHOOTS;      /* no burst in the air */
        in->shot_t = 0.0f;
        in->sweep[0] = in->sweep[1] = in->sweep[2] = 0.0f;
        in->w_dwell = 0.0f;
        in->w_count = 0;
        in->w_have = 0;
        in->w_seen = 0;
        in->imp_kind = CHR_IMP_NONE;
        in->rnd = 0x9e3779b9u + i * 2654435761u;
        in->clip = -1;
        in->clip_rate = 1.0f;
        in->clip_t = 0.0f;
        set_clip(in, m, clip_any(m, CLIP_IDLE), 1.0f);
        if (in->clip < 0 && m->n_clips)
            set_clip(in, m, 0, 1.0f);

        if (in->path >= 0) {
            const chr_path_t *pa = &c->path[in->path];
            in->path_t = pl->phase * pa->duration;
            /* ...and forward to where the recording has it out of the sea --
               see first_dry_t. Nothing moves for a path with no water under it. */
            in->path_t = first_dry_t(c, pa, in->path_t);
            in->cursor = 0;
            /*
             * AND PLACED ON ITS PATH, not on the authored pivot.
             *
             * A replayer's own `place` is a DUMMY. All six of beach_2's road
             * cars, all six of beach_3's and urban_1's Hammer carry the same
             * (-19.58, 8.07, 43.15) -- one triple in three different levels,
             * with no surface at all under it on two of them and 0.65 m of air
             * under it on the third -- and beach_1's Man and Woman share one
             * whose nearest sample of the Woman's OWN recorded walk is 89.6 m
             * away. Left there until something stepped them, beach_2's six
             * vehicles sat interpenetrating in the air 100 m off the racing
             * line, which is the reported "the npc cars are white boxes floating
             * in the air", and beach_1's two people stood where nobody drives,
             * which is the reported "two people missing". `chrfloat` prints
             * every pivot, its ground and its distance to its own path.
             *
             * dt = 0 advances nothing: advance_path interpolates the sample pair
             * `path_t` already names and writes the position, the heading and the
             * clip. So an instance is on its path from the first frame, before
             * anything has stepped it.
             */
            advance_path(c, in, m, 0.0f);
        } else if (pl->kind != CHR_KIND_PATH) {
            drop(c, in);
        }
    }
}

/* --------------------------------------------------------------- the pose --- */

static void pose_from(chr_t *c, unsigned int idx, int use_bind);

void chr_pose(chr_t *c, unsigned int idx)
{
    pose_from(c, idx, 0);
}

/*
 * The same composition run off each part's BIND transform instead of its saved
 * one, with no clip. Since rest_inv is the inverse of the composed bind chain,
 * this must give the identity on every part -- it is the only statement that
 * ties the two halves of a packed part record together, and the reason `bind`
 * is in the file at all. chr_pose's own no-clip result is NOT the identity and
 * must not be asserted to be: a harness that did is what let the wrong bind
 * pose ship.
 */
void chr_pose_bind(chr_t *c, unsigned int idx)
{
    pose_from(c, idx, 1);
}

static void pose_from(chr_t *c, unsigned int idx, int use_bind)
{
    chr_inst_t *in;
    const chr_model_t *m;
    const chr_clip_t *cl;
    unsigned int i;
    float local[16];

    if (idx >= c->n_inst) return;
    in = &c->inst[idx];
    m = &c->model[in->model];
    cl = (!use_bind && in->clip >= 0 && (unsigned)in->clip < m->n_clips)
             ? &m->clip[in->clip] : NULL;

    for (i = 0; i < m->n_parts && i < CHR_MAX_PARTS; i++) {
        const chr_part_t *p = &m->part[i];
        const float *base = use_bind ? p->bind : p->trs;
        float t[3], s[3], r[3];
        unsigned int j;

        /*
         * A part's local transform in a clip is the clip's OWN rest pose --
         * 0x5410, in T,R,S order, which is NOT the T,S,R that the node's own
         * 0x540B uses -- with every channel that has keys replacing one
         * component. A part the clip says nothing about keeps its node pose.
         */
        t[0] = base[0]; t[1] = base[1]; t[2] = base[2];
        s[0] = base[3]; s[1] = base[4]; s[2] = base[5];
        r[0] = base[6]; r[1] = base[7]; r[2] = base[8];
        if (cl) {
            for (j = 0; j < cl->n_pose; j++)
                if (cl->pose_part[j] == i) {
                    const float *q = &cl->pose[j * 9];
                    t[0] = q[0]; t[1] = q[1]; t[2] = q[2];
                    r[0] = q[3]; r[1] = q[4]; r[2] = q[5];
                    s[0] = q[6]; s[1] = q[7]; s[2] = q[8];
                    break;
                }
            for (j = 0; j < cl->n_chan; j++) {
                const chr_chan_t *ch = &cl->chan[j];
                float v;
                if (ch->part != i) continue;
                v = chr_sample(ch, in->clip_t);
                if (ch->comp < 3)      t[ch->comp] = v;
                else if (ch->comp < 6) r[ch->comp - 3] = v;
                else                   s[ch->comp - 6] = v;
            }
        }
        m_trs(local, t, s, r);
        if (p->parent >= 0 && p->parent < (int)i)
            m_mul(c->world[i], c->world[p->parent], local);
        else
            memcpy(c->world[i], local, sizeof local);
        m_mul(c->draw[i], c->world[i], p->rest_inv);
    }
    for (; i < CHR_MAX_PARTS; i++) {
        m_ident(c->world[i]);
        m_ident(c->draw[i]);
    }
}

/* ------------------------------------------------------------- behaviours --- */

static float dist2_xz(float ax, float az, float bx, float bz)
{
    float dx = ax - bx, dz = az - bz;
    return dx * dx + dz * dz;
}

/* Turn `yaw` toward `want` at `rate` degrees a second. */
static void turn_to(chr_inst_t *in, float want, float rate, float dt)
{
    float d = wrap180(want - in->yaw);
    float step = rate * dt;
    if (d > step) d = step;
    if (d < -step) d = -step;
    in->yaw = wrap180(in->yaw + d);
}

/* The heading that points at (tx, tz). The rigs face their own +Z, which is
   the same convention rbcar_init uses for the car and gen_tracks.py for the
   race start -- so the angle is atan2(dx, dz), not atan2(dz, dx). */
static float face(float x, float z, float tx, float tz)
{
    return atan2f(tx - x, tz - z) / DEG;
}

/* Walk toward (tx, tz) at `speed`; returns 1 on arrival within `reach`. */
static int walk_to(chr_t *c, chr_inst_t *in, float speed, float reach,
                   float turn_rate, float dt)
{
    float want = face(in->x, in->z, in->tx, in->tz);
    float d2 = dist2_xz(in->x, in->z, in->tx, in->tz);
    float step;
    turn_to(in, want, turn_rate, dt);
    step = speed * dt;
    if (d2 > 1e-8f) {
        float d = sqrtf(d2);
        if (step > d) step = d;
        in->x += sinf(in->yaw * DEG) * step;
        in->z += cosf(in->yaw * DEG) * step;
    }
    drop(c, in);
    in->speed = speed;
    return d2 <= reach * reach;
}

/*
 * WHERE A NAMED JOINT IS IN THE WORLD, posed. Same three lines the Dog's BONES
 * proxy uses and in the same order char_draw builds on the matrix stack: the
 * model-space joint out of chr_pose, times the instance's scale, its yaw (plus
 * the model's own yaw_off) and its position. Returns 0 if the model has no such
 * node -- which for LHANDeff cannot happen on a person, because 0x50f460 FAILS
 * THE WHOLE INSTANCE when either hand effector is missing.
 */
static int bone_world(chr_t *c, unsigned int idx, const char *name, float o[3])
{
    chr_inst_t *in;
    const chr_model_t *m;
    int b;
    float cy, sy, lx, ly, lz;

    if (!c || idx >= c->n_inst)
        return 0;
    in = &c->inst[idx];
    m = &c->model[in->model];
    b = chr_part_index(m, name);
    if (b < 0 || b >= CHR_MAX_PARTS)
        return 0;
    chr_pose(c, idx);
    cy = cosf((in->yaw + m->yaw_off) * DEG);
    sy = sinf((in->yaw + m->yaw_off) * DEG);
    lx = c->world[b][12] * in->scale;
    ly = c->world[b][13] * in->scale;
    lz = c->world[b][14] * in->scale;
    o[0] = in->x + cy * lx + sy * lz;
    o[1] = in->y + ly;
    o[2] = in->z - sy * lx + cy * lz;
    return 1;
}

/*
 * CAN IT LIFT THE CAR OVER ITS HEAD? 0x510ea0, and it is a real question about
 * the world rather than a coin toss: the engine puts a sphere of radius 1.5 at
 * CHR_REACT_LIFT_UP (2.2 m) above the person and asks the world whether
 * anything is in it. Under a pier deck or a bridge there is, and the person
 * kicks instead of throwing.
 *
 * The engine's query is worldSphereQuery with kind 4 -- a sphere -- and the
 * port's equivalent is col_sphere against the same shipped grid, so this is the
 * one clause here that changes with the collision data rather than with a
 * constant. The radius is the engine's own 0x3fc00000, in char.h as
 * CHR_LIFT_RADIUS -- an immediate at the site rather than a .data read, which is
 * why read_react cannot reach it.
 */
static int can_lift(const chr_t *c, const chr_inst_t *in)
{
    float p[3];
    rb_world_hit h[1];
    int nh = 0;
    if (!c->col)
        return 1;                   /* no grid loaded: do not veto the throw */
    p[0] = in->x;
    p[1] = in->y + CHR_REACT_LIFT_UP;
    p[2] = in->z;
    /* One hit is all the question needs, but it has to be ONE and not NONE:
       col_sphere's max_hits is the size of the narrow phase's output, and at 0
       it returns "clear" without looking, which read as "there is always
       headroom" and never vetoed a throw.  Passing NULL for n_hits on top of
       that stored through it and took the process down (psp2core of
       2026-08-25, char_step -> col_sphere+0xa). */
    return !col_sphere(c->col, p, CHR_LIFT_RADIUS, h, 1, &nh);
}

/*
 * THE DECISION -- 0x510f40, and it is the whole of "the NPCs react to cars".
 *
 * Two ways in, and the person has to be roughly level with the car for either:
 *
 *   - the car is INSIDE CHR_REACT_NEAR (0.8 m). Then it always reacts, whichever
 *     way it happens to be facing. A guard kicks. A person rolls: over
 *     CHR_REACT_THROW_P (0.2) it kicks, and under it tries to pick the car up
 *     and throw it -- falling back to the kick when there is no headroom;
 *   - or the car is coming AT it: within CHR_REACT_CONE (25 degrees) of the
 *     person's own forward, inside CHR_REACT_FAR (4 m), doing more than
 *     CHR_REACT_CAR_SPEED (4.1667 m/s, which is exactly 15 km/h). That one is
 *     always the kick, and it is not open to a guard.
 *
 * -> CHR_RX_NONE, CHR_RX_KICK or CHR_RX_GRAB.
 */
static int react_decide(const chr_t *c, chr_inst_t *in, const float car[3],
                        float car_speed, int is_guard)
{
    float d2, d;

    if (fabsf(car[1] - in->y) > CHR_REACT_DY)
        return CHR_RX_NONE;
    d2 = dist2_xz(in->x, in->z, car[0], car[2]);
    d = sqrtf(d2);
    if (d < CHR_REACT_NEAR) {
        if (is_guard)
            return CHR_RX_KICK;
        if (rnd01(in) > CHR_REACT_THROW_P)
            return CHR_RX_KICK;
        return can_lift(c, in) ? CHR_RX_GRAB : CHR_RX_KICK;
    }
    if (is_guard)
        return CHR_RX_NONE;
    if (d < CHR_REACT_FAR && car_speed > CHR_REACT_CAR_SPEED) {
        float want = face(in->x, in->z, car[0], car[2]);
        if (fabsf(wrap180(want - in->yaw)) < CHR_REACT_CONE)
            return CHR_RX_KICK;
    }
    return CHR_RX_NONE;
}

/*
 * AND WHAT IT DOES ABOUT IT -- 0x511470 (the kick), 0x5111f0 (the grab) and
 * 0x510b90 (the carry and the release), which share one shape:
 *
 *   turn onto the car at CHR_REACT_TURN_RATE while the clip runs;
 *   past CHR_REACT_WINDUP, if the car has got further than the reaction's own
 *   reach (1.1 m for the kick, 1.4 m for the grab), give up;
 *   otherwise, at the clip cursor the engine names, LAND it.
 *
 * The wind-up runs in step with the cursor because the clip is raised on the
 * same frame the decision fires, which is why the engine can afford to keep both
 * and test them separately.
 *
 * -> nonzero while it is busy, so nothing else in the machine runs.
 */
static int step_react(chr_t *c, unsigned int idx, const float car[3],
                      const float car_fwd[3], float car_speed, float dt)
{
    chr_inst_t *in = &c->inst[idx];
    const chr_model_t *m = &c->model[in->model];
    int is_guard = (in->place->kind == CHR_KIND_GUARD);
    float d, dur;
    int clip;

    if (in->react == CHR_RX_NONE)
        return 0;

    /* The carry is the one state that does not care where the car is: it IS
       where the car is. Everything else tracks it. */
    if (in->react != CHR_RX_CARRY)
        turn_to(in, face(in->x, in->z, car[0], car[2]), CHR_REACT_TURN_RATE, dt);
    in->speed = 0.0f;
    in->react_t += dt;
    d = sqrtf(dist2_xz(in->x, in->z, car[0], car[2]));
    clip = in->clip;
    dur = (clip >= 0 && (unsigned)clip < m->n_clips) ? m->clip[clip].duration
                                                     : 0.0f;

    switch (in->react) {
    case CHR_RX_KICK:
        if (in->react_t >= CHR_REACT_WINDUP && !in->react_hit
            && d > CHR_REACT_KICK_LOST) {
            in->react = CHR_RX_NONE;                /* it got away */
            in->react_t = 0.0f;
            break;
        }
        if (in->react_t >= CHR_REACT_WINDUP && !in->react_hit
            && in->clip_t > CHR_REACT_KICK_T0 && in->clip_t < dur) {
            /* THE SOLE CONNECTS. Away from the person, horizontally, plus
               straight up -- 0x4f3470 normalises (car - person) in XZ only, so
               the vertical part is entirely CHR_REACT_KICK_UP. */
            float dx = car[0] - in->x, dz = car[2] - in->z;
            float l = sqrtf(dx * dx + dz * dz);
            if (l > 1e-6f) { dx /= l; dz /= l; }
            else { dx = sinf(in->yaw * DEG); dz = cosf(in->yaw * DEG); }
            in->imp_kind = CHR_IMP_KICK;
            in->imp_point[0] = car[0];
            in->imp_point[1] = car[1];
            in->imp_point[2] = car[2];
            in->imp_dv[0] = dx * CHR_REACT_KICK_AWAY;
            in->imp_dv[1] = CHR_REACT_KICK_UP;
            in->imp_dv[2] = dz * CHR_REACT_KICK_AWAY;
            in->react_hit = 1;
        }
        if (chr_clip_done(c, idx)) {
            in->react = CHR_RX_NONE;
            in->react_t = 0.0f;
        }
        break;

    case CHR_RX_GRAB:
        if (in->react_t >= CHR_REACT_WINDUP && d > CHR_REACT_GRAB_LOST) {
            in->react = CHR_RX_NONE;
            in->react_t = 0.0f;
            break;
        }
        if (in->react_t >= CHR_REACT_WINDUP
            && in->clip_t > CHR_REACT_GRAB_T0 && in->clip_t < dur) {
            /* IT LEAVES THE GROUND. The engine records the car's attitude in
               the hand's own frame here (0x5111f0's two three-by-three loops)
               so it keeps that attitude for the whole lift; char_carry_car
               fills carry_q on the first frame it runs, because the car's
               orientation is the caller's to read, not this file's. */
            in->carry = 1;
            in->carry_q[0] = 0.0f;      /* "not taken yet" -- see the carry */
            in->react = CHR_RX_CARRY;
        }
        if (chr_clip_done(c, idx)) {
            in->react = CHR_RX_NONE;
            in->react_t = 0.0f;
        }
        break;

    case CHR_RX_CARRY:
        if (in->clip_t > CHR_REACT_LET_GO_T) {
            /* AND IT LETS GO: along the person's own forward, and up. Faster
               forward than up, which is the other way round from the kick --
               a kick pops the car and a throw hurls it, and at 7.0 m/s it
               leaves faster than the car's own 6.91 top speed. */
            in->imp_kind = CHR_IMP_THROW;
            in->imp_point[0] = car[0];
            in->imp_point[1] = car[1];
            in->imp_point[2] = car[2];
            in->imp_dv[0] = sinf(in->yaw * DEG) * CHR_REACT_THROW_FWD;
            in->imp_dv[1] = CHR_REACT_THROW_UP;
            in->imp_dv[2] = cosf(in->yaw * DEG) * CHR_REACT_THROW_FWD;
            in->carry = 0;
            in->react = CHR_RX_NONE;
            in->react_t = 0.0f;
        }
        break;

    default:
        break;
    }

    /* Whatever it is doing, it is doing it in its own clip: the state above only
       ever ends by that clip finishing or by the car leaving. */
    if (in->react == CHR_RX_KICK)
        set_clip_once(in, m, clip_any(m, CLIP_KICK));
    else if (in->react == CHR_RX_GRAB || in->react == CHR_RX_CARRY)
        set_clip_once(in, m, clip_any(m, CLIP_THROW));
    (void)is_guard;
    return in->react != CHR_RX_NONE;
}

/* Begin a reaction. Separate from step_react so the two callers -- a person and
   a guard -- start it identically, and so the fixture has one place to look. */
static void react_enter(chr_inst_t *in, int what)
{
    in->react = what;
    in->react_t = 0.0f;
    in->react_hit = 0;
}

/*
 * OPEN FIRE -- 0x51aa81 spawns a 'BRMN' at the guard's own position plus
 * CHR_GUARD_MUZZLE, and 0x52de30 gives it its sweep: one random unit vector,
 * scaled by Radius, whose sign the six rounds walk across.
 */
static void burst_start(chr_inst_t *in)
{
    float x = rnd01(in) - 0.5f, y = rnd01(in) - 0.5f, z = rnd01(in) - 0.5f;
    float l = sqrtf(x * x + y * y + z * z);
    if (l < 1e-6f) { x = 1.0f; y = 0.0f; z = 0.0f; l = 1.0f; }
    in->sweep[0] = x / l * CHR_BURST_RADIUS;
    in->sweep[1] = y / l * CHR_BURST_RADIUS;
    in->sweep[2] = z / l * CHR_BURST_RADIUS;
    in->shots = 0;
    in->shot_t = 0.0f;
}

/*
 * AND ONE ROUND A STEP WHILE IT HAS ANY LEFT -- 0x52da00.
 *
 * Six rounds CHR_BURST_TIMESHOOT apart, each aimed at where the car is GOING:
 * its own position plus its heading times its speed IN KM/H times
 * CHR_BULLET_LEAD x Offset -- the one place in this port where a recovered
 * constant wants km/h -- plus a sweep that walks from +Radius to -Radius across
 * the burst. So the burst crosses the car rather than tracking it, and a car
 * that keeps moving is mostly missed.
 *
 * The FIRST round leaves at once: the engine seeds the last-fired index at -1
 * (0x52de30) so index 0 is already a change. At most one a step, which is what
 * the engine does too -- it fires when the index changes, not per unit of time.
 */
static void burst_step(chr_inst_t *in, const float car[3],
                       const float car_fwd[3], float car_speed, float dt)
{
    int k;
    if (in->shots >= (int)CHR_BURST_NSHOOTS)
        return;
    k = (int)(in->shot_t / CHR_BURST_TIMESHOOT);
    if (k >= in->shots) {
        float f = (float)in->shots / CHR_BURST_NSHOOTS;
        float lead = car_speed * 3.6f * CHR_BURST_OFFSET * CHR_BULLET_LEAD;
        float w = 1.0f - 2.0f * f;
        in->imp_kind = CHR_IMP_SHOT;
        in->imp_point[0] = in->x;
        in->imp_point[1] = in->y + CHR_GUARD_MUZZLE;
        in->imp_point[2] = in->z;
        in->imp_aim[0] = car[0] + car_fwd[0] * lead + in->sweep[0] * w;
        in->imp_aim[1] = car[1] + car_fwd[1] * lead + in->sweep[1] * w;
        in->imp_aim[2] = car[2] + car_fwd[2] * lead + in->sweep[2] * w;
        in->shots++;
    }
    in->shot_t += dt;
}

/*
 * THE ATTENTION MACHINE -- 0x5357e0, shared by the Guard, the Dog, the Btr's
 * tower and the Seagull, and its constants are the arguments at each creation
 * site (char_data.h's CHR_WATCH_*).
 *
 * Two volumes: nothing happens at all unless a car is in the ACQUIRE one, and
 * the target has to stay in the HOLD one. Then a dwell clock -- `first` before
 * the first attack and `again` between later ones -- and after `count` attacks
 * it drops the target and looks again.
 *
 *   0  nothing is in the acquire volume
 *   1  it had a target and lost it, or it has none to take
 *   2  tracking: turn onto the car and wait the dwell out
 *   3  ATTACK, this step
 *
 * ONE CAR. The engine walks every '$CAR' and takes the nearest with the last
 * one it attacked counted at DOUBLE distance, so it works round a field; the
 * port's opponents are recorded replays with no rigid body for an impulse to act
 * on, so this watches the player and the tie-break has nothing to break. Marked
 * because it is a reduction of the original rather than the original.
 */
static int watch_step(chr_inst_t *in, const chr_watch_t *w, const float car[3],
                      float dt)
{
    const chr_place_t *pl = in->place;
    int acquire = !(pl->have & CHR_VOL_SIGHT)
                  || chr_in_volume(&pl->sight, car[0], car[1], car[2]);
    int hold = !(pl->have & CHR_VOL_HOME)
               || chr_in_volume(&pl->home, car[0], car[1], car[2]);
    float thr;

    if (!acquire) {
        in->w_have = 0;
        return 0;
    }
    if (!in->w_have) {
        if (!hold)
            return 1;
        in->w_have = 1;
        in->w_dwell = 0.0f;
        in->w_count = 0;
        return 2;
    }
    if (!hold) {
        in->w_have = 0;
        return 1;
    }
    in->w_dwell += dt;
    thr = in->w_count == 0 ? w->first : w->again;
    if (in->w_dwell < thr)
        return 2;
    in->w_dwell = 0.0f;
    if (++in->w_count > w->count)
        in->w_have = 0;
    return 3;
}

/*
 * THE DOG. char_data.h gives it a vision cone (ConeAngle 45 degrees), two
 * radii (RadNear 0.35 m all round, RadFar_Far 3.0 m inside the cone), a reach
 * (RadReach 0.30 m), an attack that lasts TimeAttack 1.5 s and AniSpeed 4.0.
 * Its own subtree gives it an attackVolume it stays inside and, on six of the
 * nine, a resetPlace to go home to.
 *
 * The engine's own use of those is not recovered; this machine is the port's.
 * What is not invented is any threshold in it: every number below is a
 * recovered constant or a point out of the instance's brief.
 */
static void step_dog(chr_t *c, chr_inst_t *in, const chr_model_t *m,
                     const float car[3], float dt)
{
    const chr_place_t *pl = in->place;
    float d2 = dist2_xz(in->x, in->z, car[0], car[2]);
    float far2 = CHR_DOG_RADFAR_FAR * CHR_DOG_RADFAR_FAR;
    float near2 = CHR_DOG_RADNEAR * CHR_DOG_RADNEAR;
    float home_x = pl->x, home_z = pl->z;

    if (pl->have & CHR_MARK_A) {
        home_x = pl->mark_a.x;
        home_z = pl->mark_a.z;
    }

    in->seen = 0;
    if (d2 <= near2) {
        in->seen = 1;                       /* close enough not to need eyes */
    } else if (d2 <= far2) {
        float want = face(in->x, in->z, car[0], car[2]);
        if (fabsf(wrap180(want - in->yaw)) <= CHR_DOG_CONEANGLE)
            in->seen = 1;
    }
    /* And it will not leave the volume it was given, if it was given one. */
    if (in->seen && (pl->have & CHR_VOL_ATTACK)
        && !in_quad(&pl->attack, car[0], car[2]))
        in->seen = 0;

    switch (in->state) {
    case CHR_ST_ACT:
        in->timer -= dt;
        in->speed = 0.0f;
        if (in->timer <= 0.0f)
            in->state = in->seen ? CHR_ST_GO : CHR_ST_RETURN;
        break;
    case CHR_ST_GO:
        in->tx = car[0];
        in->tz = car[2];
        if (walk_to(c, in, CHR_DOG_ANISPEED, CHR_DOG_RADREACH, 240.0f, dt)) {
            in->state = CHR_ST_ACT;
            in->timer = CHR_DOG_TIMEATTACK;
            in->event = CHR_EV_BARK;
            set_clip(in, m, clip_any(m, CLIP_ACT), 1.0f);
        } else if (!in->seen) {
            in->state = CHR_ST_RETURN;
        }
        break;
    case CHR_ST_RETURN:
        in->tx = home_x;
        in->tz = home_z;
        if (in->seen)
            in->state = CHR_ST_GO;
        else if (walk_to(c, in, CHR_DOG_ANISPEED * 0.5f, CHR_DOG_RADREACH,
                         180.0f, dt))
            in->state = CHR_ST_IDLE;
        break;
    default:
        in->speed = 0.0f;
        if (in->seen)
            in->state = CHR_ST_GO;
        break;
    }

    if (in->state == CHR_ST_GO || in->state == CHR_ST_RETURN)
        set_clip(in, m, clip_any(m, CLIP_RUN),
                 stride_rate(m, clip_any(m, CLIP_RUN), in->speed));
    else if (in->state == CHR_ST_IDLE)
        set_clip(in, m, clip_any(m, CLIP_IDLE), 1.0f);
}

/*
 * THE SEAGULL -- 0x512b30, RECOVERED, and it is a four-state machine whose every
 * clip choice this file used to get wrong.
 *
 * The state lives in one field and IS the animation flag word: 0x512760 hands it
 * straight to 0x5135d0, which decodes it through 0x5137c0 against the clip table
 * at 0x573e84, [start_flight, flight_idle, look_around, walk, flight]:
 *
 *   1  LOOK AROUND -- stand still for TimeTurn (2.57 s), playing `look_around`
 *   2  WALK        -- to a point in the inhabitVolume at SpeedRun (0.21 m/s),
 *                     playing `walk`, until within CHR_GULL_ARRIVE (0.25 m)
 *   16 STARTLED    -- `start_flight` for TimeIdle (0.22 s)
 *   4  FLY         -- to the flightDir marker at SpeedFlight (0.25 m/s), playing
 *                     `flight`, climbing at VertAngleMax (45 deg) scaled down
 *                     linearly to level as it reaches FlightHeight (1.5 m)
 *
 * THREE THINGS THIS FILE HAD WRONG, and together they are the whole of "the
 * seagull has no walk animation and its flight is too slow":
 *
 *   - `flight_idle` was the ground idle. It is an AIRBORNE clip; the ground idle
 *     is `look_around`, and a gull standing with its wings spread reads as a
 *     bird with no walk cycle at all;
 *   - TimeIdle and TimeTurn were SWAPPED. TimeTurn is how long it looks around
 *     (2.57 s) and TimeIdle is the startle before it goes up (0.22 s), so the
 *     bird used to idle for a fifth of a second and then spend two and a half
 *     seconds crouched in `start_flight` -- eleven times too long, and that is
 *     the slow takeoff;
 *   - every clip ran at stride_rate. The engine advances EVERY animation at the
 *     constant 1.0 (0x513820 loads 0x554390, as 0x50f7d0 does for the people),
 *     so a 0.7 s walk cycle is a 0.7 s walk cycle. stride_rate exists for the
 *     Dog, whose ground speed has to come from somewhere; the gull's is a
 *     recovered constant and its clips are authored to match.
 *
 * What startles it is not the visibilityVolume: that is only the gate that lets
 * the machine run at all (0x513040). The trigger is 0x513100 -- a 'PEOP', a
 * 'DOG$' or a '$CAR' within CHR_GULL_STARTLE (4.0 m), in XZ. Only the car is
 * tested here, because that is the only one of the three this signature is
 * handed; the people and the dogs on the same beach are in c->inst and cost a
 * loop, so they are tested too.
 */
static int gull_startled(const chr_t *c, const chr_inst_t *self,
                         const float car[3])
{
    unsigned int i;
    float r2 = CHR_GULL_STARTLE * CHR_GULL_STARTLE;

    if (dist2_xz(self->x, self->z, car[0], car[2]) < r2)
        return 1;
    for (i = 0; i < c->n_inst; i++) {
        const chr_inst_t *o = &c->inst[i];
        if (o == self)
            continue;
        /* 'PEOP' and 'DOG$' -- the two other kinds 0x513100 looks for. */
        if (o->place->kind != CHR_KIND_WALKER && o->place->kind != CHR_KIND_DOG)
            continue;
        if (dist2_xz(self->x, self->z, o->x, o->z) < r2)
            return 1;
    }
    return 0;
}

static void step_seagull(chr_t *c, chr_inst_t *in, const chr_model_t *m,
                         const float car[3], float dt)
{
    const chr_place_t *pl = in->place;
    /* THE GATE, 0x513040: a car inside the visibilityVolume, or the gull does
       nothing at all. An instance with no volume authored is always awake. */
    int awake = !(pl->have & CHR_VOL_SIGHT)
                || chr_in_volume(&pl->sight, car[0], car[1], car[2]);

    if (!awake) {
        in->speed = 0.0f;
        set_clip(in, m, clip_any(m, CLIP_IDLE), 1.0f);
        return;
    }

    /* The startle test runs in the two GROUND states only, which is what
       0x512b30 does: the check sits under `if (state == 1 || state == 2)`. */
    if ((in->state == CHR_ST_IDLE || in->state == CHR_ST_GO)
        && gull_startled(c, in, car)) {
        in->state = CHR_ST_ACT;
        in->timer = CHR_SEAGULL_TIMEIDLE;
        set_clip(in, m, chr_clip_index(m, "start_flight"), 1.0f);
    }

    switch (in->state) {
    case CHR_ST_ACT:                        /* startled: start_flight */
        in->speed = 0.0f;
        in->timer -= dt;
        if (in->timer <= 0.0f) {
            in->state = CHR_ST_FLY;
            in->event = CHR_EV_TAKEOFF;
            /* Where it goes: its own flightDir marker. Nothing recovered brings
               it back -- 0x512b30 has no state past 4 -- but a bird that leaves
               for good empties the track a lap at a time, so the port turns it
               round once it is out of the volume that woke it. Marked; the
               duration is the walk timer's, not an invented one. */
            in->timer = CHR_SEAGULL_TIMETURN * 4.0f;
        }
        break;

    case CHR_ST_FLY: {
        float want = (pl->have & CHR_MARK_A)
                         ? face(in->x, in->z, pl->mark_a.x, pl->mark_a.z)
                         : in->yaw;
        float g = ground(c, in->x, in->z, in->y, in->y + 50.0f);
        float h = in->y - g;
        float pitch, climb, dy, step;
        /* VertAngleMax at ground level, falling linearly to level flight at
           FlightHeight and staying level above it -- 0x512eee..0x512f4d. */
        if (h < 0.0f)
            pitch = CHR_SEAGULL_VERTANGLEMAX;
        else if (h <= CHR_SEAGULL_FLIGHTHEIGHT)
            pitch = CHR_SEAGULL_VERTANGLEMAX
                    * (1.0f - h / CHR_SEAGULL_FLIGHTHEIGHT);
        else
            pitch = 0.0f;
        turn_to(in, want, 45.0f, dt);
        climb = CHR_SEAGULL_SPEEDFLIGHT * tanf(pitch * DEG);
        step = CHR_SEAGULL_SPEEDFLIGHT * dt;
        dy = climb * dt;
        in->y += dy;
        in->x += sinf(in->yaw * DEG) * step;
        in->z += cosf(in->yaw * DEG) * step;
        in->speed = CHR_SEAGULL_SPEEDFLIGHT;
        in->pitch = pitch;
        set_clip(in, m, chr_clip_index(m, "flight"), 1.0f);
        in->timer -= dt;
        if (in->timer <= 0.0f && !gull_startled(c, in, car)) {
            in->x = pl->x;
            in->z = pl->z;
            in->pitch = 0.0f;
            drop(c, in);
            in->state = CHR_ST_IDLE;
            in->timer = CHR_SEAGULL_TIMETURN;
        }
        break;
    }

    case CHR_ST_GO:                         /* walking to its wander target */
        if (walk_to(c, in, CHR_SEAGULL_SPEEDRUN, CHR_GULL_ARRIVE, 90.0f, dt)) {
            in->state = CHR_ST_IDLE;
            in->timer = CHR_SEAGULL_TIMETURN;
        }
        set_clip(in, m, clip_any(m, CLIP_WALK), 1.0f);
        break;

    default:                                /* look_around, for TimeTurn */
        in->speed = 0.0f;
        set_clip(in, m, clip_any(m, CLIP_IDLE), 1.0f);
        in->timer -= dt;
        if (in->timer <= 0.0f) {
            /*
             * A point in the inhabitVolume, AWAY FROM THE CAR -- 0x513200 takes
             * the volume's centre and its two edge vectors, picks the quadrant
             * whose cross products with (car - centre) are both negative, and
             * lands somewhere random in it. So a disturbed gull walks off rather
             * than wandering into the road.
             *
             * This file used to walk the four corners in turn and say it did so
             * because nothing here had a random number generator. There is one
             * now (rnd01), and the engine's own rule is cheaper than the
             * substitute was.
             */
            if (pl->have & CHR_VOL_HOME) {
                float cx = 0.0f, cz = 0.0f;
                float ex, ez, fx, fz, u, w;
                int k;
                for (k = 0; k < 4; k++) {
                    cx += pl->home.qx[k];
                    cz += pl->home.qz[k];
                }
                cx *= 0.25f; cz *= 0.25f;
                /* The two half-edges out of the centre, as 0x513200 builds them
                   from corners 0..3 in traversal order. */
                ex = (pl->home.qx[1] - pl->home.qx[0]) * 0.5f;
                ez = (pl->home.qz[1] - pl->home.qz[0]) * 0.5f;
                fx = (pl->home.qx[3] - pl->home.qx[0]) * 0.5f;
                fz = (pl->home.qz[3] - pl->home.qz[0]) * 0.5f;
                u = (fz * (car[0] - cx) - fx * (car[2] - cz)) < 0.0f ? 1.0f
                                                                    : -1.0f;
                w = (ez * (car[0] - cx) - ex * (car[2] - cz)) < 0.0f ? 1.0f
                                                                    : -1.0f;
                u *= rnd01(in);
                w *= rnd01(in);
                in->tx = cx + u * fx + w * ex;
                in->tz = cz + u * fz + w * ez;
            } else {
                in->tx = pl->x;
                in->tz = pl->z;
            }
            in->state = CHR_ST_GO;
        }
        break;
    }
}

/*
 * THE CRAB AND THE SPIDER. Both are given two markers -- seahole1/landhole1 and
 * hole1/hole2 -- and a visibilityVolume, and nothing else. So: sit at a hole,
 * and when the car comes into the volume, run to the other one.
 *
 * Neither has a speed constant of its own (the Spider's four keys are a
 * collision radius, a y shift and two floor heights), so the speed comes from
 * the same stride anchor the clip rate does: one body length per animation
 * cycle, times the instance's own scale. Measured on the shipped models that
 * is 0.50 m/s for the Crab at its authored scale 2.0 and 0.34 m/s for the
 * Spider at 1.7.
 */
static void step_burrow(chr_t *c, chr_inst_t *in, const chr_model_t *m,
                        const float car[3], float dt)
{
    const chr_place_t *pl = in->place;
    int seen = (pl->have & CHR_VOL_SIGHT)
               && chr_in_volume(&pl->sight, car[0], car[1], car[2]);
    int run = clip_any(m, CLIP_RUN);
    float len = m->bmax[2] - m->bmin[2];
    float cycle = (run >= 0 && (unsigned)run < m->n_clips)
                      ? m->clip[run].duration : 0.5f;
    float speed;

    if (len < 0.02f) len = 0.02f;
    if (cycle < 1e-3f) cycle = 0.5f;
    speed = (len / cycle) * in->scale;

    switch (in->state) {
    case CHR_ST_GO:
        if (walk_to(c, in, speed, 0.05f, 360.0f, dt)) {
            in->state = CHR_ST_IDLE;
            in->timer = 0.5f;
        }
        set_clip(in, m, run, stride_rate(m, run, speed));
        break;
    default:
        in->speed = 0.0f;
        set_clip(in, m, clip_any(m, CLIP_IDLE), 1.0f);
        if (in->timer > 0.0f) {
            in->timer -= dt;
            break;
        }
        if (seen && (pl->have & CHR_MARK_A) && (pl->have & CHR_MARK_B)) {
            in->leg = !in->leg;
            in->tx = in->leg ? pl->mark_b.x : pl->mark_a.x;
            in->tz = in->leg ? pl->mark_b.z : pl->mark_a.z;
            in->state = CHR_ST_GO;
        }
        break;
    }
}

/*
 * THE GUARD -- 0x51a950, and it SHOOTS. All of it was in the shipped data while
 * these notes recorded the guard as a man who stands and turns:
 *
 *   people.sb gives him a `shoot` clip (1.5 s) and a `kick` (3.0 s) and the
 *   exe's clip table at 0x574bec puts them in that order; Settings ships
 *   burst_mng.ini + .crs, bullet_dust.crs, bullet_smoke.crs, bullet_explode.crs
 *   and a `bullet_footprint`; and 0x51aa81 pushes the 4CC 'BRMN' -- the burst
 *   manager -- spawning one at the guard's own position plus CHR_GUARD_MUZZLE.
 *
 * What drives it is the shared attention machine, NOT guard.ini: all four of
 * that file's keys are loaded (0x51aaa0, TimeFollow / TimeAttack / TimeFollowEx /
 * ExVolumeDelta) and NOTHING IN THE IMAGE READS ANY OF THEM. Searched by
 * scanning .text for every dword inside their own range, which finds the dog's
 * two neighbours' readers either side of them and none of theirs. They are dead
 * keys, like `CollisionLen` and `SpeedAngMaxREL`; the numbers the guard actually
 * runs on are the watcher's, pushed at 0x51a670 -- 0.15 s to the first attack,
 * 1.25 s between later ones, 2 attacks before it looks for another car.
 *
 * The close-range kick is tried EVERY step, ahead of the dwell: a car inside
 * CHR_REACT_NEAR gets booted whatever the watcher is counting.
 */
/* Back to `stand`, but never over the top of a one-shot that is still running:
   the shoot clip is 1.5 s against a 0.5 s burst, so the guard is still lowering
   its rifle for a second after the last round. */
static void guard_idle(chr_t *c, unsigned int idx)
{
    chr_inst_t *in = &c->inst[idx];
    const chr_model_t *m = &c->model[in->model];
    if (in->once && !chr_clip_done(c, idx))
        return;
    set_clip(in, m, clip_any(m, CLIP_IDLE), 1.0f);
}

static void step_guard(chr_t *c, unsigned int idx, const float car[3],
                       const float car_fwd[3], float car_speed, float dt)
{
    static const chr_watch_t W = CHR_WATCH_GUARD;
    chr_inst_t *in = &c->inst[idx];
    const chr_model_t *m = &c->model[in->model];
    const chr_place_t *pl = in->place;
    int v = watch_step(in, &W, car, dt);

    in->w_seen = v;
    in->speed = 0.0f;
    in->seen = (v >= 2);

    /* A burst already in the air keeps firing whatever the guard does next: it
       is a separate object with its own clock. */
    burst_step(in, car, car_fwd, car_speed, dt);

    if (v < 2) {
        /* Back to the post it was authored facing. Nothing recovered sets this
           rate -- the engine leaves an idle guard's yaw alone entirely -- so it
           is the port's, and slow enough to read as standing down. */
        turn_to(in, wrap180(pl->yaw), 60.0f, dt);
        guard_idle(c, idx);
        return;
    }

    {
        int r = react_decide(c, in, car, car_speed, 1);
        if (r != CHR_RX_NONE) {
            react_enter(in, r);
            return;
        }
    }
    turn_to(in, face(in->x, in->z, car[0], car[2]), CHR_REACT_TURN_RATE, dt);
    if (v == 3) {
        burst_start(in);
        set_clip_once(in, m, clip_any(m, CLIP_SHOOT));
        in->event = CHR_EV_SHOOT;
        return;
    }
    guard_idle(c, idx);
}

/*
 * THE WALKERS and the PATH FOLLOWERS: replay, at the recorded timestamps.
 *
 * A person's .dat is time, position, a unit heading whose y is always exactly
 * 0, and a state column that is NOT decoded -- see pack_chars.read_person_path.
 * The clip comes from the recorded SPEED instead, which is a property of the
 * path rather than of the recorder, so it cannot go stale if the state column
 * is ever read.
 *
 * A .gpf is the same shape with an UP vector on the nine-float form (the
 * vultures), which is where a banking bird's roll comes from. The road cars'
 * eight-float form has no up, so they take world up.
 */
static void advance_path(chr_t *c, chr_inst_t *in, const chr_model_t *m,
                         float dt)
{
    const chr_path_t *pa;
    unsigned int i, n;
    float u, t;
    float p0[3], p1[3], f0[3], f1[3], u0[3], u1[3];
    float speed;

    if (in->path < 0 || (unsigned)in->path >= c->n_paths)
        return;
    pa = &c->path[in->path];
    if (pa->n < 2)
        return;

    in->path_t += dt;
    if (pa->duration > 1e-3f)
        while (in->path_t >= pa->duration)
            in->path_t -= pa->duration;
    t = in->path_t;

    /* Walk the cursor forward -- a replay only ever moves one way, and a
       binary search per frame over 30,000 samples is the thing this avoids. */
    n = pa->n;
    if (in->cursor >= n - 1 || pa->t[in->cursor] > t)
        in->cursor = 0;
    for (i = in->cursor; i + 1 < n && pa->t[i + 1] < t; i++)
        ;
    in->cursor = i;
    {
        float ta = pa->t[i], tb = pa->t[i + 1];
        u = (tb - ta > 1e-6f) ? (t - ta) / (tb - ta) : 0.0f;
        u = clampf(u, 0.0f, 1.0f);
    }
    memcpy(p0, &pa->p[i * 3], 12);
    memcpy(p1, &pa->p[(i + 1) * 3], 12);
    memcpy(f0, &pa->f[i * 3], 12);
    memcpy(f1, &pa->f[(i + 1) * 3], 12);
    memcpy(u0, &pa->u[i * 3], 12);
    memcpy(u1, &pa->u[(i + 1) * 3], 12);

    in->x = p0[0] + (p1[0] - p0[0]) * u;
    in->y = p0[1] + (p1[1] - p0[1]) * u;
    in->z = p0[2] + (p1[2] - p0[2]) * u;
    {
        float fx = f0[0] + (f1[0] - f0[0]) * u;
        float fy = f0[1] + (f1[1] - f0[1]) * u;
        float fz = f0[2] + (f1[2] - f0[2]) * u;
        float l = sqrtf(fx * fx + fy * fy + fz * fz);
        if (l > 1e-6f) {
            fx /= l; fy /= l; fz /= l;
            in->yaw = atan2f(fx, fz) / DEG;
            in->pitch = asinf(clampf(fy, -1.0f, 1.0f)) / DEG;
        }
        {
            /*
             * ROLL, off the recorded up vector -- how far it leans out of the
             * vertical plane that contains the heading. The right axis is
             * fwd x world_up normalised, and the roll is the angle of `up`
             * about the heading measured from that plane.
             *
             * The road cars' eight-float record carries no up and pack_chars.py
             * fills it with world up, which lands here as exactly zero roll --
             * so nothing has to know which record shape it came from.
             */
            float ux = u0[0] + (u1[0] - u0[0]) * u;
            float uy = u0[1] + (u1[1] - u0[1]) * u;
            float uz = u0[2] + (u1[2] - u0[2]) * u;
            float rx = fz, rz = -fx;            /* fwd x (0,1,0), unnormalised */
            float rl = sqrtf(rx * rx + rz * rz);
            if (rl > 1e-6f) {
                rx /= rl; rz /= rl;
                in->roll = atan2f(-(ux * rx + uz * rz), uy) / DEG;
            } else {
                in->roll = 0.0f;
            }
        }
    }
    {
        float ta = pa->t[i], tb = pa->t[i + 1];
        float dx = p1[0] - p0[0], dy = p1[1] - p0[1], dz = p1[2] - p0[2];
        float dtl = tb - ta;
        speed = (dtl > 1e-6f) ? sqrtf(dx * dx + dy * dy + dz * dz) / dtl : 0.0f;
    }
    in->speed = speed;

    if (m->n_clips) {
        int walk = clip_any(m, CLIP_WALK);
        int idle = clip_any(m, CLIP_IDLE);
        int want = (speed > 0.05f) ? walk : idle;
        if (want < 0) want = walk >= 0 ? walk : idle;
        set_clip(in, m, want, (want == walk) ? stride_rate(m, walk, speed)
                                             : 1.0f);
    }
}

/* ------------------------------------------------------------------- step --- */

void char_step(chr_t *c, float dt, const float car[3], const float car_fwd[3],
               float car_speed)
{
    unsigned int i;
    float zero[3] = { 0.0f, 0.0f, 0.0f };

    if (!c || !c->n_inst || dt <= 0.0f)
        return;
    if (!car) car = zero;
    if (!car_fwd) car_fwd = zero;
    c->n_stepped = 0;

    for (i = 0; i < c->n_inst; i++) {
        chr_inst_t *in = &c->inst[i];
        const chr_model_t *m = &c->model[in->model];
        float dx, dy, dz;
        /*
         * A REPLAYER IS NEVER CULLED. CHR_STEP_DIST bounds the invented state
         * machines and the recovered reactions -- the Dog's cone, the Seagull's
         * four states, the Crab's shuttle, the Guard's attention machine and the
         * kick -- because those are what a character does ABOUT the
         * car and they cost a volume test and a walk each. A recorded path is
         * neither: it is where that character IS at time t, it costs a cursor
         * step and a lerp, and the engine's own layer is stepped once per frame
         * over everything (0x4fc685 with dt; the 10 m range at 0x53343b bounds
         * the COLLISION sweep, not the step).
         *
         * Culled, a replayer's clock stopped the moment the car left, so it
         * froze wherever it happened to be -- for beach_1's Man and Woman, one
         * frame after the start, on the first sample of a 469 s and a 201 s
         * walk, for the whole race. Nothing else in this loop runs out of range:
         * being run over needs the car within CHR_HIT_RADIUS and the reactions
         * need a state that only the machines above can enter.
         */
        int replay = (in->place->kind == CHR_KIND_PATH
                      || in->place->kind == CHR_KIND_WALKER
                      /* and a reaction in progress, for the same reason a
                         replayer is exempt: it holds a clock, and culling it
                         would freeze a person mid-swing with the car in the
                         air. It cannot happen from CHR_STEP_DIST alone -- the
                         reaction only starts within 4 m -- but a thrown car
                         travels, and the carry moves the car with the hand. */
                      || in->react != CHR_RX_NONE);

        dx = in->x - car[0];
        dy = in->y - car[1];
        dz = in->z - car[2];
        in->dist2 = dx * dx + dy * dy + dz * dz;
        in->event = CHR_EV_NONE;
        in->imp_kind = CHR_IMP_NONE;
        in->solid_hit = 0;
        if (!replay && in->dist2 > CHR_STEP_DIST * CHR_STEP_DIST)
            continue;
        c->n_stepped++;

        if (in->hit_cool > 0.0f)
            in->hit_cool -= dt;

        /*
         * BEING RUN OVER. Not a collision -- nothing moves and the car does not
         * feel it -- but a character that a car drives straight through with no
         * reaction at all is worse than one that has none of these clips. The
         * radius and the speed floor are the port's; the floor is deliberately
         * the same 0.35 m/s sfx.c calls a knock.
         */
        if (in->state != CHR_ST_HURT && in->hit_cool <= 0.0f
            && car_speed >= CHR_HIT_MIN_SPEED
            && in->place->kind != CHR_KIND_PATH) {
            float d2 = dist2_xz(in->x, in->z, car[0], car[2]);
            float r = CHR_HIT_RADIUS * in->scale;
            if (d2 <= r * r && fabsf(in->y - car[1]) < 2.0f) {
                int hurt = clip_any(m, CLIP_HURT);
                if (hurt >= 0) {
                    in->state = CHR_ST_HURT;
                    in->timer = m->clip[hurt].duration;
                    in->event = CHR_EV_HURT;
                    set_clip(in, m, hurt, 1.0f);
                }
            }
        }

        if (in->state == CHR_ST_HURT) {
            in->timer -= dt;
            in->speed = 0.0f;
            if (in->timer <= 0.0f) {
                in->state = CHR_ST_IDLE;
                in->timer = 0.0f;
                /* The cooldown starts when the reaction ENDS, not when it
                   begins. Started at the beginning it is spent before the clip
                   is over -- the Hurt clip runs 4.5 s against a 3 s cooldown --
                   and a car parked on top sets the whole thing off again on the
                   very next frame. */
                in->hit_cool = CHR_HIT_COOL;
                set_clip(in, m, clip_any(m, CLIP_IDLE), 1.0f);
            }
        } else if (step_react(c, i, car, car_fwd, car_speed, dt)) {
            /*
             * IT IS KICKING, THROWING OR SHOOTING, and nothing else runs while
             * it is -- which is also what stops a WALKER's recorded path: the
             * engine advances that replay only while the animation state is
             * Stand or Walk (0x510422 tests exactly those two before calling
             * 0x511b50), so a person who plants a boot in the car stops walking
             * for the length of the clip and picks the recording up where it
             * left off. Both halves of that fall out of this branch.
             */
        } else if (in->place->kind == CHR_KIND_WALKER && in->once
                   && !chr_clip_done(c, i)) {
            /* A one-shot clip still playing out: the swing has landed and the
               leg is coming down. The decision is not re-taken until it ends,
               which is what keeps a person from re-grabbing a car it has just
               thrown while the Brosok clip still has 2.7 s to run.
               THE GUARD IS NOT HELD LIKE THIS: its shoot clip is one-shot too,
               and the engine spawns the burst as a SEPARATE object -- the guard
               goes on tracking and its attention machine goes on counting while
               the rounds leave, so it has to keep being stepped. */
        } else {
            /*
             * A FINISHED ONE-SHOT CHAINS BACK TO STAND, and it has to happen
             * HERE -- before the decision -- rather than as a tidy-up after it.
             *
             * The engine's animation table gives Brosok, Pendal and Hurt a
             * successor (0x50f6c0) and 0x50f570's tail switches to it when the
             * clip ends; and 0x50f700 only restarts a clip when the STATE
             * changes. So without the chain a second kick is not a change --
             * the state is already Pendal -- its cursor stays parked at the
             * clip's end, and the connect test (`cursor > 0.65 AND < duration`)
             * can never be true again. The reaction fired exactly once per
             * instance per race and then ping-ponged in and out of the state
             * every frame, which is what this looked like from the harness: one
             * good kick, and then nothing while a car sat on the man's feet.
             */
            if (in->once && chr_clip_done(c, i))
                set_clip(in, m, clip_any(m, CLIP_IDLE), 1.0f);
            /* THE TWO KINDS THE ENGINE'S REACTION REACHES: 'PEOP' (the Man, the
               Woman and the RepairMan) and 'GUAM' (the Guard). 0x511690 is the
               people's and 0x51a950 the guard's, and both go through 0x510f40. */
            if (in->place->kind == CHR_KIND_WALKER) {
                int r = react_decide(c, in, car, car_speed, 0);
                if (r != CHR_RX_NONE) {
                    react_enter(in, r);
                    in->event = CHR_EV_SWING;
                }
            }
            if (in->react != CHR_RX_NONE)
                ;                       /* it starts on the next step */
            else switch (in->place->kind) {
            case CHR_KIND_DOG:     step_dog(c, in, m, car, dt); break;
            case CHR_KIND_SEAGULL: step_seagull(c, in, m, car, dt); break;
            case CHR_KIND_BURROW:  step_burrow(c, in, m, car, dt); break;
            case CHR_KIND_GUARD:   step_guard(c, i, car, car_fwd, car_speed,
                                                dt); break;
            case CHR_KIND_WALKER:
            case CHR_KIND_PATH:    advance_path(c, in, m, dt); break;
            default: break;
            }
        }

        /* The clip clock. A clip with no duration (a model with no sequences at
           all -- the Vulture and the road cars) stays at 0 and poses its bind
           pose, which is exactly what those models want. */
        if (in->clip >= 0 && (unsigned)in->clip < m->n_clips) {
            float d = m->clip[in->clip].duration;
            in->clip_t += dt * in->clip_rate;
            if (d <= 1e-4f)
                in->clip_t = 0.0f;
            else if (in->once) {
                /* A ONE-SHOT STOPS at its own end rather than wrapping -- the
                   engine's animation table marks Brosok, Pendal, Hurt and the
                   guard's shoot that way and chains each back to Stand when it
                   gets there. The whole reaction is timed off this cursor, so a
                   wrap would fire the swing again every cycle. */
                if (in->clip_t > d)
                    in->clip_t = d;
            } else {
                while (in->clip_t >= d)
                    in->clip_t -= d;
            }
        }
    }
}

/* See char.h. Deliberately one line per instance plus one per model, so a whole
   track fits in a screenful of log and a screenshot can be matched to it. */
void char_dump(const chr_t *c, const float eye[3])
{
    unsigned int i, k;
    float zero[3] = { 0.0f, 0.0f, 0.0f };
    if (!c) return;
    if (!eye) eye = zero;
    rlog("[rccars] chars: %u placed, %u models, %u textures\n",
         c->n_inst, c->n_models, c->n_tex);
    for (i = 0; i < c->n_inst; i++) {
        const chr_inst_t *in = &c->inst[i];
        const chr_model_t *m = &c->model[in->model];
        float dx = in->x - eye[0], dy = in->y - eye[1], dz = in->z - eye[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        char slots[160];
        int n = 0;
        slots[0] = 0;
        for (k = 0; k < m->n_slot && n < (int)sizeof slots - 24; k++) {
            unsigned int ti = m->var
                ? m->var[(unsigned)in->variant * m->n_slot + k] : 0xFFFFFFFFu;
            GLuint id = (ti < c->n_tex) ? c->tex[ti] : 0;
            const char *nm = (ti < c->n_tex && c->tex_name) ? c->tex_name[ti]
                                                            : "-";
            n += snprintf(slots + n, sizeof slots - (size_t)n, " %s#%u%s", nm,
                          (unsigned)id,
                          !id ? "!" : ((c->tex_px && ti < c->n_tex
                                       && !c->tex_px[ti]) ? "?" : ""));
        }
        /* `rx` is the reaction and `w` the attention machine's verdict, because
           "why is that guard not shooting" and "why did he only kick once" are
           both questions a screenshot cannot answer and both are one field. */
        rlog("[rccars]   %-20s %-10s var%d at (%.1f %.1f %.1f) %.1f m %s "
             "batches=%u clip=%s%s rx=%d%s w=%d%s\n",
             in->place->name, m->name, in->variant, in->x, in->y, in->z, d,
             in->drawn ? "DRAWN" : "culled", m->n_batches,
             (in->clip >= 0 && (unsigned)in->clip < m->n_clips)
                 ? m->clip[in->clip].name : "-", slots,
             in->react, in->carry ? "+CARRY" : "", in->w_seen,
             in->shots < (int)CHR_BURST_NSHOOTS ? "+FIRING" : "");
    }
}

int char_pos(const chr_t *c, unsigned int i, float out[3])
{
    if (!c || i >= c->n_inst || !out) return 0;
    out[0] = c->inst[i].x;
    out[1] = c->inst[i].y;
    out[2] = c->inst[i].z;
    return 1;
}

/* ----------------------------------------------------------------- solid --- */

/*
 * THE CHARACTERS ARE SOLID, AND THE ENGINE SAYS WHICH ONES AND WITH WHAT.
 *
 * See char_data.h's CHR_PROXY for the two registries this is transcribed from --
 * a sphere-set provider per object 4CC and a resolver per PAIR of them, both
 * filled at 0x534d00. What matters here:
 *
 *   - the car collides with PEOP, GUAM, CRAB, SPDR, RDCR and DOG$, and the
 *     Seagull and the Vulture are in neither registry because they fly;
 *   - a provider writes (x, y, z, radius) quads, the same shape
 *     rb_gather_spheres writes for the car;
 *   - every resolver ends at rb_coll_resolve (0x4f0750) with the CAR as the only
 *     body. So the car is stopped and pushed out and the character does not
 *     move: it is the static side, exactly as the world is. That is also why
 *     this does not touch the transcribed handling model -- it is the same
 *     routine, the same law and the same one-body form the world contact uses;
 *   - the layer is stepped once per FRAME with dt (0x4fc685) over the objects
 *     within CHR_SOLID_RANGE (10 m) of the car, so there is no
 *     carSubstepContact bisection behind it and NO DEPENETRATION IN IT. The
 *     solve drives the closing speed to 0.05 m/s of separation every tick and
 *     the pair comes apart over a few of them. That is the original.
 *
 * Being run over (char_step) is a separate, older thing and stays: it is a
 * proximity test that raises the model's own Hurt clip and its voice. This makes
 * the car feel the person; that makes the person react.
 */

/* One overlapping sphere pair, in rb_coll_contact's own convention: the normal
   points out of the CHARACTER and toward the car, so an approaching car has a
   negative relative normal velocity. */
typedef struct {
    float point[3];
    float normal[3];
    float depth;
} chr_touch;

int char_proxy(chr_t *c, unsigned int idx, float out[][4])
{
    const chr_inst_t *in;
    const chr_model_t *m;
    const chr_proxy_t *px;
    unsigned int k;
    int n = 0;

    if (!c || idx >= c->n_inst || !out)
        return 0;
    in = &c->inst[idx];
    m = &c->model[in->model];
    px = m->proxy;
    if (!px || !px->n)
        return 0;

    switch (px->kind) {
    case CHR_PX_COLUMN: {
        /* Stacked along WORLD +Y from the instance position, which is what the
           providers do -- the up vector at 0x55e9a0 is (0, 1, 0) and neither the
           step nor the radius is turned by the instance's own yaw or scaled by
           its scale. The Crab's -0.18 and the Spider's CdtShiftY -0.24 are the
           same field: a shift on the first sphere. */
        /*
         * AND THE DOWNWARD SHIFT IS CLAMPED. THE PORT'S, and the one number here
         * that is not the engine's.
         *
         * The recovered shifts bury both burrowers. A character is dropped onto
         * the surface, so the Crab's r 0.2 sphere at pivot - 0.18 (0x554b14)
         * spans -0.380 .. +0.020 m about the sand and the Spider's r 0.23 at
         * CdtShiftY -0.24, off a pivot spider.ini puts HeightFloor 0.010 above
         * the floor, tops out at 0.000 -- flush with it, to the millimetre. A
         * settled car's own thirteen spheres span -0.002 .. 0.252 m above the
         * ground, so nothing the car has can reach either one except by pressing
         * a tyre into the terrain: `chrfloat` drives a real car straight at both
         * of beach_1's Crabs and records 0 contacts, against 148 for a Woman.
         * That is the reported "the crab has no collision", and the animal has a
         * 3.3 s HIT clip and a place in the engine's own $CAR/CRAB pair for
         * something.
         *
         * What the retail resolvers do with these is not fully recovered -- the
         * $CAR/CRAB resolver at 0x534670 builds ONE contact out of two POSITIONS
         * (0x534be0: normal = a - b normalised, point = their midpoint) rather
         * than out of the two sphere sets this does -- so the shift may well have
         * been tuned against a coarser test. What is not in question is that a
         * sphere entirely under the surface cannot be touched by a car on top of
         * it, so the centre is clamped to the pivot plane: the PEOP and GUAM
         * convention, a first sphere centred at the feet, with the recovered
         * radius and count untouched. Nothing else in the table is affected --
         * every other shift is 0.
         */
        float shift = px->shift_y < 0.0f ? 0.0f : px->shift_y;
        for (k = 0; k < px->n && k < CHR_PROXY_MAX; k++) {
            out[n][0] = in->x;
            out[n][1] = in->y + shift + px->step_y * (float)k;
            out[n][2] = in->z;
            out[n][3] = px->radius;
            n++;
        }
        break;
    }

    case CHR_PX_BONES: {
        /* The Dog's: five spheres at the POSED world positions of LHAND1,
           RHAND1, LFOOT2, RFOOT2 and HEAD -- the four paws and the head, cached
           by name at 0x518cb5..0x518d9d. So a dog mid-stride has its proxy where
           its legs are, which a column could not do.

           chr_pose leaves c->world in MODEL space, so each joint still has to go
           through the instance's own placement: scale, then the yaw the draw
           uses (in->yaw + the model's own yaw_off), then the position. Same
           transform char_draw builds on the matrix stack, in the same order. */
        float cy = cosf((in->yaw + m->yaw_off) * DEG);
        float sy = sinf((in->yaw + m->yaw_off) * DEG);
        chr_pose(c, idx);
        for (k = 0; k < px->n && k < CHR_PROXY_BONES && k < CHR_PROXY_MAX; k++) {
            int b = chr_part_index(m, px->bone[k]);
            float lx, ly, lz;
            if (b < 0 || b >= CHR_MAX_PARTS)
                continue;
            lx = c->world[b][12] * in->scale;
            ly = c->world[b][13] * in->scale;
            lz = c->world[b][14] * in->scale;
            /* glRotatef(a, 0,1,0) on a column vector: x' = c*x + s*z,
               z' = -s*x + c*z. */
            out[n][0] = in->x + cy * lx + sy * lz;
            out[n][1] = in->y + ly;
            out[n][2] = in->z - sy * lx + cy * lz;
            out[n][3] = px->radius;
            n++;
        }
        break;
    }

    case CHR_PX_HULL: {
        /*
         * THE PORT'S, and the one proxy here that is not the engine's numbers.
         *
         * RDCR's provider (0x516480) reads up to eight local offsets and radii
         * out of a per-instance table (state+0x264 stride 0xc, state+0x2c4
         * stride 4) and turns them by the object's matrix. WHERE THAT TABLE IS
         * FILLED FROM IS NOT RECOVERED: it is written through base-plus-index
         * arithmetic, and no road-car node name, .ini key or .sb chunk carries
         * it. So the shape is the engine's -- spheres along the vehicle, turned
         * with it -- and the numbers come from the only measurable thing to hand,
         * the model's own posed bounding box out of the .chr:
         *
         *   n spheres spread along the model's LONG horizontal axis, each of
         *   radius half the SHORT one, centred at the box's own mid-height.
         *
         * A Truck is 4.49 x 1.44 x 2.02 m, so that is three spheres of r 1.01
         * at 1.24 m spacing -- a capsule down the truck. It is deliberately the
         * inscribed radius rather than the circumscribed one: a proxy wider than
         * the vehicle stops the car short of something it can see it is not
         * touching, which reads as a bug in a way a slightly thin one does not.
         */
        float cy = cosf((in->yaw + m->yaw_off) * DEG);
        float sy = sinf((in->yaw + m->yaw_off) * DEG);
        float ex = (m->bmax[0] - m->bmin[0]) * 0.5f * in->scale;
        float ez = (m->bmax[2] - m->bmin[2]) * 0.5f * in->scale;
        float mx = (m->bmax[0] + m->bmin[0]) * 0.5f * in->scale;
        float my = (m->bmax[1] + m->bmin[1]) * 0.5f * in->scale;
        float mz = (m->bmax[2] + m->bmin[2]) * 0.5f * in->scale;
        int along_x = ex >= ez;
        float half = along_x ? ex : ez;              /* the long axis */
        float r = along_x ? ez : ex;                 /* the short one */
        unsigned int nn = px->n < 1u ? 1u : px->n;
        if (r < 0.05f) r = 0.05f;
        for (k = 0; k < nn && k < CHR_PROXY_MAX; k++) {
            /* Spread the CENTRES over the part of the axis a sphere of r can
               reach, so the capsule ends flush with the box rather than past it. */
            float span = half - r;
            float u = (nn == 1u) ? 0.0f
                    : (-span + 2.0f * span * (float)k / (float)(nn - 1));
            float lx = mx + (along_x ? u : 0.0f);
            float lz = mz + (along_x ? 0.0f : u);
            if (span < 0.0f) { lx = mx; lz = mz; }
            out[n][0] = in->x + cy * lx + sy * lz;
            out[n][1] = in->y + my;
            out[n][2] = in->z - sy * lx + cy * lz;
            out[n][3] = r;
            n++;
        }
        break;
    }

    default:
        break;
    }
    return n;
}

/* The overlapping pairs between the car's proxy and one character's. Same test
   ai.c's ai_touch_list runs for two cars, and the same convention. */
static int solid_touch(const float cs[][4], int nc,
                       const float hs[][4], int nh,
                       chr_touch *t, int max)
{
    int p, o, nt = 0;

    for (p = 0; p < nc && nt < max; p++) {
        for (o = 0; o < nh && nt < max; o++) {
            double ex = (double)cs[p][0] - hs[o][0];
            double ey = (double)cs[p][1] - hs[o][1];
            double ez = (double)cs[p][2] - hs[o][2];
            double d2 = ex * ex + ey * ey + ez * ez;
            double sum = (double)cs[p][3] + hs[o][3];
            double len, inv;

            if (d2 >= sum * sum || d2 < 1e-12)
                continue;
            len = sqrt(d2);
            inv = 1.0 / len;
            t[nt].normal[0] = (float)(ex * inv);
            t[nt].normal[1] = (float)(ey * inv);
            t[nt].normal[2] = (float)(ez * inv);
            /* The point on the CHARACTER's surface, which is where the car's
               lever arm is measured from. */
            t[nt].point[0] = (float)(hs[o][0] + t[nt].normal[0] * hs[o][3]);
            t[nt].point[1] = (float)(hs[o][1] + t[nt].normal[1] * hs[o][3]);
            t[nt].point[2] = (float)(hs[o][2] + t[nt].normal[2] * hs[o][3]);
            t[nt].depth = (float)(sum - len);
            nt++;
        }
    }
    return nt;
}

int char_car_solid(chr_t *c, rb_car *car, float *impact)
{
    float cs[RB_MAX_SPHERES][4];
    rb_coll_contact rec[RB_MAX_COLL_CONTACTS];
    unsigned int i;
    int nc, hit = 0, nrec = 0;
    float worst = 0.0f;

    if (impact) *impact = 0.0f;
    if (!c || !car || !c->n_inst)
        return 0;

    nc = rb_gather_spheres(car, cs);
    if (nc <= 0)
        return 0;

    for (i = 0; i < c->n_inst; i++) {
        chr_inst_t *in = &c->inst[i];
        float hs[CHR_PROXY_MAX][4];
        chr_touch t[CHR_PROXY_MAX * 4];
        int nh, nt, k;
        float dx, dy, dz;

        if (!c->model[in->model].proxy || !c->model[in->model].proxy->n)
            continue;
        dx = in->x - car->body.x[0];
        dy = in->y - car->body.x[1];
        dz = in->z - car->body.x[2];
        if (dx * dx + dy * dy + dz * dz
            > CHR_SOLID_RANGE * CHR_SOLID_RANGE)
            continue;

        nh = char_proxy(c, i, hs);
        if (nh <= 0)
            continue;
        nt = solid_touch(cs, nc, hs, nh, t,
                         (int)(sizeof t / sizeof t[0]));
        if (nt <= 0)
            continue;
        hit++;
        in->solid_hit = 1;
        for (k = 0; k < nt && nrec < RB_MAX_COLL_CONTACTS; k++) {
            float pv[3], closing;
            rb_point_velocity(&car->body, t[k].point, pv);
            /* The normal separates, so a closing pair has a negative projection
               and `closing` comes out positive. */
            closing = -(pv[0] * t[k].normal[0] + pv[1] * t[k].normal[1]
                        + pv[2] * t[k].normal[2]);
            if (closing > worst)
                worst = closing;
            memcpy(rec[nrec].point, t[k].point, sizeof rec[nrec].point);
            memcpy(rec[nrec].normal, t[k].normal, sizeof rec[nrec].normal);
            rec[nrec].is_wheel = 0;
            nrec++;
        }
    }

    /* One call over the whole list. rb_coll_resolve IS a Gauss-Seidel sweep over
       a contact list -- ten passes, every contact closing faster than -0.02 m/s
       driven to 0.05 m/s of separation -- so handing it every pair is using it
       as written. The retail resolvers pass the single pair their own overlap
       test returns, which is the same routine with a shorter list. */
    if (nrec > 0)
        rb_coll_resolve(car, nrec, rec);
    if (impact) *impact = worst;
    return hit;
}

/* ------------------------------------------------------------------- draw --- */

static GLuint slot_tex(const chr_t *c, const chr_model_t *m, int variant,
                       unsigned int slot)
{
    unsigned int ti;
    if (!m->var || slot >= m->n_slot)
        return 0;
    if ((unsigned)variant >= m->n_var)
        variant = 0;
    ti = m->var[(unsigned)variant * m->n_slot + slot];
    if (ti >= c->n_tex)
        return 0;
    return c->tex[ti];
}

void char_draw(chr_t *c, const float eye[3])
{
    unsigned int i, b, v;
    float zero[3] = { 0.0f, 0.0f, 0.0f };

    if (!c || !c->n_inst)
        return;
    if (!eye) eye = zero;
    c->n_drawn = 0;
    /* Next arena, empty. One frame per display buffer, which is what says the
       GPU has finished with the one three frames back -- see CHR_SKIN_RINGS. */
    c->skin_ring = (c->skin_ring + 1u) % (unsigned)CHR_SKIN_RINGS;
    c->skin_used = 0;
    for (i = 0; i < c->n_inst; i++) {
        chr_inst_t *in = &c->inst[i];
        float dx = in->x - eye[0], dy = in->y - eye[1], dz = in->z - eye[2];
        in->eye2 = dx * dx + dy * dy + dz * dz;
        in->drawn = in->eye2 <= CHR_DRAW_DIST * CHR_DRAW_DIST;
    }

    /*
     * THE VERTEX AND TEXCOORD ARRAYS ARE APP-WIDE STATE AND MUST NOT BE TOUCHED.
     *
     * main.c enables both ONCE before the frame loop (main.c:877) and every
     * other module here relies on that: scene.c, water.c, trace.c, fx.c and
     * ui.c only ever toggle GL_COLOR_ARRAY, and scene.c's lm_bind only touches
     * unit 1's. This function used to enable them for itself and DISABLE them
     * on the way out, which left them off for everything drawn after it --
     * the water, the dust, the tyre marks, the sun, the checkpoint arrows and
     * the whole of ui.c, so the world and the menu both stopped submitting
     * geometry while the characters carried on drawing.
     *
     * The alpha test IS ours to set, and it is set the way scene_draw_model
     * sets it: off for opaque geometry, back on for whatever draws next, with
     * glAlphaFunc left alone because main.c owns the threshold.
     */
    glDisable(GL_ALPHA_TEST);

    for (i = 0; i < c->n_inst; i++) {
        chr_inst_t *in = &c->inst[i];
        const chr_model_t *m;
        if (!in->drawn)
            continue;
        /* The isolation ladder -- see CHR_HIDE_* in char.h. */
        if (c->hide == CHR_HIDE_ALL)
            continue;
        if (c->hide == CHR_HIDE_PATH
            && in->place->kind == CHR_KIND_PATH)
            continue;
        m = &c->model[in->model];
        chr_pose(c, i);
        c->n_drawn++;

        glPushMatrix();
        glTranslatef(in->x, in->y, in->z);
        glRotatef(in->yaw + m->yaw_off, 0.0f, 1.0f, 0.0f);
        if (in->place->kind == CHR_KIND_PATH) {
            glRotatef(in->pitch, 1.0f, 0.0f, 0.0f);
            glRotatef(in->roll, 0.0f, 0.0f, 1.0f);
        }
        if (in->scale != 1.0f)
            glScalef(in->scale, in->scale, in->scale);
        if (in->place->kind == CHR_KIND_PATH
            && !strcmp(m->name, "Vulture"))
            glTranslatef(0.0f, CHR_VULTURE_SHIFTY, 0.0f);

        for (b = 0; b < m->n_batches; b++) {
            const chr_batch_t *bt = &m->batch[b];
            GLuint tex = slot_tex(c, m, in->variant, bt->slot);
            if (!bt->nidx)
                continue;
            glBindTexture(GL_TEXTURE_2D, tex);
            if (bt->skinned) {
                /*
                 * ITS OWN SLICE OF THIS FRAME'S ARENA. Not a shared scratch:
                 * past 32 KB of vertex data vitaGL hands GXM this very pointer
                 * and reads it at flush, so two draws sharing it means the
                 * second fill is what the first one draws. See CHR_SKIN_RINGS
                 * in char.h for the measurement and for who was over the line.
                 */
                float *base = c->skin[c->skin_ring];
                float *out;
                if (!base) continue;
                if (c->skin_used + bt->nverts > c->skin_cap) {
                    if (!c->skin_full) {
                        c->skin_full = 1;
                        rlog("[rccars] char: skinning arena full at %u/%u verts"
                             " -- a batch of %u is not drawn\n", c->skin_used,
                             c->skin_cap, bt->nverts);
                    }
                    continue;
                }
                out = base + (size_t)c->skin_used * 5;
                c->skin_used += bt->nverts;
                for (v = 0; v < bt->nverts; v++) {
                    const float *src = &bt->verts[v * 5];
                    float acc[3] = { 0.0f, 0.0f, 0.0f };
                    unsigned int q, n = bt->inf_n[v];
                    for (q = 0; q < n; q++) {
                        unsigned int bp = bt->inf_part[v * CHR_MAX_INF + q];
                        float w = bt->inf_w[v * CHR_MAX_INF + q];
                        float t[3];
                        if (bp >= CHR_MAX_PARTS) continue;
                        m_point(c->draw[bp], src, t);
                        acc[0] += t[0] * w;
                        acc[1] += t[1] * w;
                        acc[2] += t[2] * w;
                    }
                    if (!n) {
                        acc[0] = src[0]; acc[1] = src[1]; acc[2] = src[2];
                    }
                    out[v * 5 + 0] = acc[0];
                    out[v * 5 + 1] = acc[1];
                    out[v * 5 + 2] = acc[2];
                    out[v * 5 + 3] = src[3];
                    out[v * 5 + 4] = src[4];
                }
                glVertexPointer(3, GL_FLOAT, 5 * sizeof(float), out);
                glTexCoordPointer(2, GL_FLOAT, 5 * sizeof(float), out + 3);
                glDrawElements(GL_TRIANGLES, (GLsizei)bt->nidx,
                               GL_UNSIGNED_SHORT, bt->idx);
            } else {
                glPushMatrix();
                if (bt->part < CHR_MAX_PARTS)
                    glMultMatrixf(c->draw[bt->part]);
                glVertexPointer(3, GL_FLOAT, 5 * sizeof(float), bt->verts);
                glTexCoordPointer(2, GL_FLOAT, 5 * sizeof(float),
                                  bt->verts + 3);
                glDrawElements(GL_TRIANGLES, (GLsizei)bt->nidx,
                               GL_UNSIGNED_SHORT, bt->idx);
                glPopMatrix();
            }
        }
        glPopMatrix();
    }

    glEnable(GL_ALPHA_TEST);
}

/* --------------------------------------------------- the reaction's forces --- */

/*
 * SEGMENT AGAINST ONE SPHERE. The engine's bullet is a ray into
 * worldRayQuery plus an OBB test on the car built out of its own half-extents
 * (0x508290 assembles the box from phys+0x5708..0x571c and 0x508e70 intersects
 * it). The port tests the car's OWN COLLISION PROXY instead -- rb_gather_spheres,
 * the same set every wheel and body contact already uses -- because that set is
 * shipped, fitted to the drawn mesh, and the thing a bullet ought to agree with.
 * Marked as a substitution of shape, not of numbers.
 *
 * -> the nearest t in [0, 1] at which the segment enters the sphere, or -1.
 */
static float seg_sphere(const float a[3], const float b[3], const float s[4])
{
    float d[3], m[3], A, B, C, disc, t;
    d[0] = b[0] - a[0]; d[1] = b[1] - a[1]; d[2] = b[2] - a[2];
    m[0] = a[0] - s[0]; m[1] = a[1] - s[1]; m[2] = a[2] - s[2];
    A = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (A < 1e-12f)
        return -1.0f;
    B = 2.0f * (m[0] * d[0] + m[1] * d[1] + m[2] * d[2]);
    C = m[0] * m[0] + m[1] * m[1] + m[2] * m[2] - s[3] * s[3];
    disc = B * B - 4.0f * A * C;
    if (disc < 0.0f)
        return -1.0f;
    disc = sqrtf(disc);
    t = (-B - disc) / (2.0f * A);
    if (t < 0.0f)
        t = (-B + disc) / (2.0f * A);
    if (t < 0.0f || t > 1.0f)
        return -1.0f;
    return t;
}

int char_car_react(chr_t *c, rb_car *car)
{
    unsigned int i;
    int n = 0;

    if (!c || !car || !c->n_inst)
        return 0;

    for (i = 0; i < c->n_inst; i++) {
        chr_inst_t *in = &c->inst[i];
        float j[3];

        if (in->imp_kind == CHR_IMP_NONE)
            continue;

        if (in->imp_kind == CHR_IMP_SHOT) {
            /*
             * ONE ROUND. It has to reach the car before it reaches the world,
             * or the guard is shooting through a wall: col_segment answers the
             * second question and the proxy the first, and the nearer wins.
             *
             * The engine's ray runs CHR_BULLET_REACH times the distance to the
             * aim point or CHR_BULLET_MIN, whichever is longer -- so it carries
             * PAST the aim, which is what lets a burst aimed ahead of a car
             * still hit a car that has not got there.
             */
            float from[3], to[3], dir[3], len, reach, best = 2.0f;
            float cs[RB_MAX_SPHERES][4];
            int nc, k, kbest = -1;

            memcpy(from, in->imp_point, sizeof from);
            dir[0] = in->imp_aim[0] - from[0];
            dir[1] = in->imp_aim[1] - from[1];
            dir[2] = in->imp_aim[2] - from[2];
            len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            if (len < 1e-4f)
                continue;
            reach = len * CHR_BULLET_REACH;
            if (reach < CHR_BULLET_MIN)
                reach = CHR_BULLET_MIN;
            to[0] = from[0] + dir[0] / len * reach;
            to[1] = from[1] + dir[1] / len * reach;
            to[2] = from[2] + dir[2] / len * reach;

            /*
             * AND IT IS TESTED AGAINST THE PROXY'S ENCLOSING SPHERE, not
             * against the thirteen spheres themselves.
             *
             * THE PORT'S, and it is a substitution the numbers force. The
             * engine builds an oriented BOX out of the car's own half-extents
             * (0x508290 assembles it from phys+0x5708..0x5710 and 0x508e70
             * intersects the ray with it) and the port does not carry those
             * three floats at all -- nothing in rb_data.h needs them, because
             * every other consumer of the car's shape uses the sphere set.
             *
             * But a ray CANNOT use that set: rb_gather_spheres is four wheels
             * of r 0.072 and nine body spheres of r 0.051 spread over a 0.42 m
             * car, so it is mostly holes. A segment dropped straight down
             * through the car's own centre of mass passes between all thirteen
             * -- measured, and it is what a first version of this did, reporting
             * a guard who could not hit a stationary car from directly above it.
             * The enclosing sphere of the same set is derived from the same
             * shipped data, is about 0.21 m on all three cars, and is the right
             * order for a 0.42 m body; it is generous at the corners, which for
             * a burst that sprays over four metres is not the term that decides
             * anything.
             */
            nc = rb_gather_spheres(car, cs);
            if (nc > 0) {
                float hull[4];
                hull[0] = car->body.x[0];
                hull[1] = car->body.x[1];
                hull[2] = car->body.x[2];
                hull[3] = 0.0f;
                for (k = 0; k < nc; k++) {
                    float dx = cs[k][0] - hull[0];
                    float dy = cs[k][1] - hull[1];
                    float dz = cs[k][2] - hull[2];
                    float r = sqrtf(dx * dx + dy * dy + dz * dz) + cs[k][3];
                    if (r > hull[3]) hull[3] = r;
                }
                best = seg_sphere(from, to, hull);
                kbest = best >= 0.0f ? 0 : -1;
            }
            if (kbest < 0)
                continue;
            {
                /* Stop the segment at the car and ask whether the world was in
                   the way of THAT much of it. */
                float hit[3];
                hit[0] = from[0] + (to[0] - from[0]) * best;
                hit[1] = from[1] + (to[1] - from[1]) * best;
                hit[2] = from[2] + (to[2] - from[2]) * best;
                if (c->col && col_segment(c->col, from, hit))
                    continue;
                /* IT LANDS: away from where it hit, and up. Same routine, same
                   law, different two numbers -- 0x508560 pushes 6.5 and 3.25
                   where the kick pushes 4.0 and 7.0. */
                {
                    float dx = car->body.x[0] - hit[0];
                    float dz = car->body.x[2] - hit[2];
                    float l = sqrtf(dx * dx + dz * dz);
                    if (l > 1e-6f) { dx /= l; dz /= l; }
                    else { dx = 0.0f; dz = 0.0f; }
                    in->imp_dv[0] = dx * CHR_BULLET_AWAY;
                    in->imp_dv[1] = CHR_BULLET_UP;
                    in->imp_dv[2] = dz * CHR_BULLET_AWAY;
                    memcpy(in->imp_point, hit, sizeof in->imp_point);
                }
            }
        }

        if (in->imp_kind == CHR_IMP_THROW) {
            /*
             * THE RELEASE SETS the momentum rather than adding to it, and zeroes
             * the angular part -- 0x4f3690, which is why a thrown car leaves
             * without the spin the lift gave it.
             */
            car->body.P[0] = in->imp_dv[0] * car->body.mass;
            car->body.P[1] = in->imp_dv[1] * car->body.mass;
            car->body.P[2] = in->imp_dv[2] * car->body.mass;
            car->body.L[0] = car->body.L[1] = car->body.L[2] = 0.0f;
            rb_update_inv_inertia_world(&car->body);
            car->body.v[0] = in->imp_dv[0];
            car->body.v[1] = in->imp_dv[1];
            car->body.v[2] = in->imp_dv[2];
            car->body.w[0] = car->body.w[1] = car->body.w[2] = 0.0f;
            n++;
            continue;
        }

        /* And a kick or a round is an IMPULSE at the car's own origin: 0x4f3470
           multiplies the velocity change by the mass and hands it to 0x4756c0,
           which is rb_apply_impulse. */
        j[0] = in->imp_dv[0] * car->body.mass;
        j[1] = in->imp_dv[1] * car->body.mass;
        j[2] = in->imp_dv[2] * car->body.mass;
        rb_apply_impulse(car, in->imp_point, j);
        n++;
    }
    return n;
}

int char_carrier(const chr_t *c)
{
    unsigned int i;
    if (!c)
        return -1;
    for (i = 0; i < c->n_inst; i++)
        if (c->inst[i].carry)
            return (int)i;
    return -1;
}

/* The 3x3 of a matrix, columns normalised, as a quaternion (w, x, y, z). A
   joint matrix can carry a scale -- the people's ROOT is authored at 0.04 -- so
   the axes have to be normalised before the trace form means anything. */
static void mat_to_quat(const float m[16], float q[4])
{
    float a[9];
    float tr, s;
    int k;
    for (k = 0; k < 3; k++) {
        float l = sqrtf(m[k * 4 + 0] * m[k * 4 + 0] + m[k * 4 + 1] * m[k * 4 + 1]
                        + m[k * 4 + 2] * m[k * 4 + 2]);
        if (l < 1e-8f) l = 1.0f;
        a[k * 3 + 0] = m[k * 4 + 0] / l;
        a[k * 3 + 1] = m[k * 4 + 1] / l;
        a[k * 3 + 2] = m[k * 4 + 2] / l;
    }
    tr = a[0] + a[4] + a[8];
    if (tr > 0.0f) {
        s = sqrtf(tr + 1.0f) * 2.0f;
        q[0] = 0.25f * s;
        q[1] = (a[5] - a[7]) / s;
        q[2] = (a[6] - a[2]) / s;
        q[3] = (a[1] - a[3]) / s;
    } else if (a[0] > a[4] && a[0] > a[8]) {
        s = sqrtf(1.0f + a[0] - a[4] - a[8]) * 2.0f;
        q[0] = (a[5] - a[7]) / s;
        q[1] = 0.25f * s;
        q[2] = (a[3] + a[1]) / s;
        q[3] = (a[6] + a[2]) / s;
    } else if (a[4] > a[8]) {
        s = sqrtf(1.0f + a[4] - a[0] - a[8]) * 2.0f;
        q[0] = (a[6] - a[2]) / s;
        q[1] = (a[3] + a[1]) / s;
        q[2] = 0.25f * s;
        q[3] = (a[7] + a[5]) / s;
    } else {
        s = sqrtf(1.0f + a[8] - a[0] - a[4]) * 2.0f;
        q[0] = (a[1] - a[3]) / s;
        q[1] = (a[6] + a[2]) / s;
        q[2] = (a[7] + a[5]) / s;
        q[3] = 0.25f * s;
    }
    rb_quat_normalize(q);
}

void char_carry_car(chr_t *c, rb_car *car)
{
    int idx = char_carrier(c);
    chr_inst_t *in;
    const chr_model_t *m;
    int b;
    float hand[3], hq[4], mm[16], cy, sy, ry[16];

    if (!c || !car || idx < 0)
        return;
    in = &c->inst[idx];
    m = &c->model[in->model];

    /*
     * IT RIDES THE LEFT HAND. 0x50f460 looks up LHANDeff and RHANDeff by name on
     * every person and FAILS THE WHOLE INSTANCE if either is missing, and the
     * carry (0x510b90) reads the left one: the car's world transform is that
     * node's own, times the attitude recorded at the grab.
     */
    b = chr_part_index(m, "LHANDeff");
    if (b < 0 || b >= CHR_MAX_PARTS || !bone_world(c, (unsigned)idx, "LHANDeff",
                                                   hand))
        return;

    /* The joint's rotation in the world: the instance's yaw about the model's,
       which is the same composition char_draw pushes on the matrix stack. */
    cy = cosf((in->yaw + m->yaw_off) * DEG);
    sy = sinf((in->yaw + m->yaw_off) * DEG);
    memset(ry, 0, sizeof ry);
    ry[0] = cy;  ry[2] = -sy;
    ry[5] = 1.0f;
    ry[8] = sy;  ry[10] = cy;
    ry[15] = 1.0f;
    rb_mat4_mul(c->world[b], ry, mm);
    mat_to_quat(mm, hq);

    if (in->carry_q[0] == 0.0f && in->carry_q[1] == 0.0f
        && in->carry_q[2] == 0.0f && in->carry_q[3] == 0.0f) {
        /* First frame of the lift: record the car's attitude in the hand's own
           frame, q_rel = conj(q_hand) * q_car, so it keeps it for the whole
           lift the way the engine's two stored rows do. */
        float ci[4];
        ci[0] = hq[0]; ci[1] = -hq[1]; ci[2] = -hq[2]; ci[3] = -hq[3];
        rb_quat_mul(ci, car->body.q, in->carry_q);
        rb_quat_normalize(in->carry_q);
    }

    rb_quat_mul(hq, in->carry_q, car->body.q);
    rb_quat_normalize(car->body.q);
    car->body.x[0] = hand[0];
    car->body.x[1] = hand[1];
    car->body.x[2] = hand[2];
    /*
     * Nothing integrates while it is up there -- 0x4f3640 sets the engine's own
     * two held flags and 0x4f3700 writes the pose straight in.
     *
     * AND THE REST CLAMP CANNOT FIRE UNDERNEATH IT, which is worth writing down
     * because it is a coincidence of two recovered numbers rather than a
     * guarantee: rb_car_at_rest wants rest_slow_t past 2.0 s AND rest_ground_t
     * past 1.0 s of continuous contact, and a carry is exactly
     * CHR_REACT_LET_GO_T - CHR_REACT_GRAB_T0 = 1.4 s long with the car a metre
     * in the air the whole time. So the slow timer never reaches its threshold
     * and the ground timer is held at zero. If either constant ever moves, a
     * released car could come out of the hand already clamped to a dead stop.
     */
    car->body.P[0] = car->body.P[1] = car->body.P[2] = 0.0f;
    car->body.L[0] = car->body.L[1] = car->body.L[2] = 0.0f;
    car->body.v[0] = car->body.v[1] = car->body.v[2] = 0.0f;
    car->body.w[0] = car->body.w[1] = car->body.w[2] = 0.0f;
    car->body.force[0] = car->body.force[1] = car->body.force[2] = 0.0f;
    car->body.torque[0] = car->body.torque[1] = car->body.torque[2] = 0.0f;
    rb_update_inv_inertia_world(&car->body);
}
