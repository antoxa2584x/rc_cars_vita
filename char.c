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
    free(c->skin);
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
        glGenTextures((GLsizei)c->n_tex, c->tex);
        for (i = 0; i < c->n_tex; i++)
            scene_read_texture(f, c->tex[i], NULL, 0, NULL, NULL);
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

static const char *const CLIP_IDLE[] = { "stand", "Stand", "flight_idle",
                                         "hide", NULL };
static const char *const CLIP_WALK[] = { "walk", "Walk", "WALK", "run", NULL };
static const char *const CLIP_RUN[] = { "run", "runQuick", "Walk", "WALK",
                                        NULL };
static const char *const CLIP_ACT[] = { "attack", "shoot", "HIT", "hide",
                                        "Brosok", NULL };
static const char *const CLIP_HURT[] = { "Hurt", "Pendal", "HIT", NULL };

static void set_clip(chr_inst_t *in, const chr_model_t *m, int clip, float rate)
{
    if (clip < 0 || (unsigned)clip >= m->n_clips)
        return;
    if (in->clip != clip) {
        in->clip = clip;
        in->clip_t = 0.0f;
    }
    in->clip_rate = rate;
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

    /* One scratch big enough for the largest skinned batch of any model here.
       Malloc'd -- see char.h. */
    for (i = 0; i < c->n_models; i++)
        if (c->model[i].max_verts > c->skin_cap)
            c->skin_cap = c->model[i].max_verts;
    if (c->skin_cap)
        c->skin = malloc((size_t)c->skin_cap * 5 * sizeof(float));

    char_reset(c);
    rlog("[rccars] %s: %u characters, %u models, %u paths, %u textures\n",
         path, c->n_inst, c->n_models, c->n_paths, c->n_tex);
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
        in->clip = -1;
        in->clip_rate = 1.0f;
        in->clip_t = 0.0f;
        set_clip(in, m, clip_any(m, CLIP_IDLE), 1.0f);
        if (in->clip < 0 && m->n_clips)
            set_clip(in, m, 0, 1.0f);

        if (in->path >= 0) {
            const chr_path_t *pa = &c->path[in->path];
            in->path_t = pl->phase * pa->duration;
            in->cursor = 0;
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
 * THE SEAGULL. SpeedRun 0.21 m/s on the ground, SpeedFlight 0.25 m/s in the
 * air, FlightHeight 1.5 m, VertAngleMax 45 degrees, TimeIdle 0.22 s,
 * TimeTurn 2.57 s. Its brief is an inhabitVolume it pecks around in, a
 * visibilityVolume that startles it, and a flightDir marker it leaves along.
 *
 * The machine is the port's; the five clips it picks between are the model's
 * own (flight_idle, look_around, walk, start_flight, flight).
 */
static void step_seagull(chr_t *c, chr_inst_t *in, const chr_model_t *m,
                         const float car[3], float dt)
{
    const chr_place_t *pl = in->place;
    int startled = (pl->have & CHR_VOL_SIGHT)
                   && chr_in_volume(&pl->sight, car[0], car[1], car[2]);

    switch (in->state) {
    case CHR_ST_FLY: {
        float want = (pl->have & CHR_MARK_A)
                         ? face(in->x, in->z, pl->mark_a.x, pl->mark_a.z)
                         : in->yaw;
        float climb;
        turn_to(in, want, 45.0f, dt);
        /* Rise to FlightHeight above the ground under it, no steeper than
           VertAngleMax. */
        climb = CHR_SEAGULL_SPEEDFLIGHT * tanf(CHR_SEAGULL_VERTANGLEMAX * DEG);
        {
            float g = ground(c, in->x, in->z, in->y, in->y + 50.0f);
            float want_y = g + CHR_SEAGULL_FLIGHTHEIGHT;
            float dy = want_y - in->y;
            float step = climb * dt;
            if (dy > step) dy = step;
            if (dy < -step) dy = -step;
            in->y += dy;
        }
        in->x += sinf(in->yaw * DEG) * CHR_SEAGULL_SPEEDFLIGHT * dt;
        in->z += cosf(in->yaw * DEG) * CHR_SEAGULL_SPEEDFLIGHT * dt;
        in->speed = CHR_SEAGULL_SPEEDFLIGHT;
        set_clip(in, m, chr_clip_index(m, "flight"), 1.0f);
        in->timer -= dt;
        /* Once it is out of sight and the car has moved on, it is back where
           it started -- a bird that flies off for good leaves the track
           emptier every lap. */
        if (in->timer <= 0.0f && !startled) {
            in->x = pl->x;
            in->z = pl->z;
            drop(c, in);
            in->state = CHR_ST_IDLE;
            in->timer = CHR_SEAGULL_TIMEIDLE;
        }
        break;
    }
    case CHR_ST_ACT:                        /* start_flight, then away */
        in->timer -= dt;
        in->speed = 0.0f;
        if (in->timer <= 0.0f) {
            in->state = CHR_ST_FLY;
            in->timer = CHR_SEAGULL_TIMETURN * 4.0f;
            in->event = CHR_EV_TAKEOFF;
        }
        break;
    case CHR_ST_GO:
        if (startled) {
            in->state = CHR_ST_ACT;
            in->timer = CHR_SEAGULL_TIMETURN;
            set_clip(in, m, chr_clip_index(m, "start_flight"), 1.0f);
            break;
        }
        if (walk_to(c, in, CHR_SEAGULL_SPEEDRUN, 0.15f, 90.0f, dt)) {
            in->state = CHR_ST_IDLE;
            in->timer = CHR_SEAGULL_TIMEIDLE;
        }
        set_clip(in, m, clip_any(m, CLIP_WALK),
                 stride_rate(m, clip_any(m, CLIP_WALK), in->speed));
        break;
    default:
        in->speed = 0.0f;
        if (startled) {
            in->state = CHR_ST_ACT;
            in->timer = CHR_SEAGULL_TIMETURN;
            set_clip(in, m, chr_clip_index(m, "start_flight"), 1.0f);
            break;
        }
        set_clip(in, m, clip_any(m, CLIP_IDLE), 1.0f);
        in->timer -= dt;
        if (in->timer <= 0.0f) {
            /* Somewhere else in the volume it was given. Deterministic -- it
               walks the four corners in turn -- because nothing here has a
               random number generator and a bird that picks the same four
               spots is indistinguishable from one that does not. */
            if (pl->have & CHR_VOL_HOME) {
                in->leg = (in->leg + 1) & 3;
                in->tx = (pl->home.qx[in->leg] + pl->x) * 0.5f;
                in->tz = (pl->home.qz[in->leg] + pl->z) * 0.5f;
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
 * THE GUARD. TimeFollow 1.0 s to come round onto the car, TimeAttack 1.5 s an
 * attack lasts, TimeFollowEx 4.02 s it keeps watching after losing sight, and
 * ExVolumeDelta 3.0 m the watched volume grows by once it has engaged. Its
 * brief is a visibilityVolume and a guardVolume, and it has no speed constant
 * of any kind -- so it stands its post and turns.
 */
static void step_guard(chr_t *c, chr_inst_t *in, const chr_model_t *m,
                       const float car[3], float dt)
{
    const chr_place_t *pl = in->place;
    int inside = 0;

    (void)c;
    if (pl->have & CHR_VOL_SIGHT)
        inside = chr_in_volume(&pl->sight, car[0], car[1], car[2]);
    if (!inside && (pl->have & CHR_VOL_HOME))
        inside = chr_in_volume(&pl->home, car[0], car[1], car[2]);
    /* ExVolumeDelta: once engaged the watched area is bigger, so a car circling
       the edge is not picked up and dropped every second. Applied as a plain
       radius about the guard, which is the only thing a delta can mean without
       the volume-growing code. */
    if (!inside && in->timer > 0.0f) {
        float d2 = dist2_xz(in->x, in->z, car[0], car[2]);
        float r = CHR_GUARD_EXVOLUMEDELTA;
        inside = d2 <= r * r;
    }

    in->speed = 0.0f;
    if (inside) {
        in->timer = CHR_GUARD_TIMEFOLLOWEX;
        turn_to(in, face(in->x, in->z, car[0], car[2]),
                180.0f / CHR_GUARD_TIMEFOLLOW, dt);
        in->seen = 1;
    } else {
        if (in->timer > 0.0f) {
            in->timer -= dt;
            turn_to(in, face(in->x, in->z, car[0], car[2]),
                    180.0f / CHR_GUARD_TIMEFOLLOW, dt);
        } else {
            turn_to(in, wrap180(pl->yaw), 60.0f, dt);
        }
        in->seen = 0;
    }

    /* An engaged guard cycles its shoot clip; otherwise it stands. */
    if (in->seen) {
        int sh = chr_clip_index(m, "shoot");
        set_clip(in, m, sh >= 0 ? sh : clip_any(m, CLIP_IDLE), 1.0f);
    } else {
        set_clip(in, m, clip_any(m, CLIP_IDLE), 1.0f);
    }
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

    (void)car_fwd;
    if (!c || !c->n_inst || dt <= 0.0f)
        return;
    if (!car) car = zero;
    c->n_stepped = 0;

    for (i = 0; i < c->n_inst; i++) {
        chr_inst_t *in = &c->inst[i];
        const chr_model_t *m = &c->model[in->model];
        float dx, dy, dz;

        dx = in->x - car[0];
        dy = in->y - car[1];
        dz = in->z - car[2];
        in->dist2 = dx * dx + dy * dy + dz * dz;
        in->event = CHR_EV_NONE;
        in->solid_hit = 0;
        if (in->dist2 > CHR_STEP_DIST * CHR_STEP_DIST)
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
        } else {
            switch (in->place->kind) {
            case CHR_KIND_DOG:     step_dog(c, in, m, car, dt); break;
            case CHR_KIND_SEAGULL: step_seagull(c, in, m, car, dt); break;
            case CHR_KIND_BURROW:  step_burrow(c, in, m, car, dt); break;
            case CHR_KIND_GUARD:   step_guard(c, in, m, car, dt); break;
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
            if (d > 1e-4f)
                while (in->clip_t >= d)
                    in->clip_t -= d;
            else
                in->clip_t = 0.0f;
        }
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
    case CHR_PX_COLUMN:
        /* Stacked along WORLD +Y from the instance position, which is what the
           providers do -- the up vector at 0x55e9a0 is (0, 1, 0) and neither the
           step nor the radius is turned by the instance's own yaw or scaled by
           its scale. The Crab's -0.18 and the Spider's CdtShiftY -0.24 are the
           same field: a shift on the first sphere. */
        for (k = 0; k < px->n && k < CHR_PROXY_MAX; k++) {
            out[n][0] = in->x;
            out[n][1] = in->y + px->shift_y + px->step_y * (float)k;
            out[n][2] = in->z;
            out[n][3] = px->radius;
            n++;
        }
        break;

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
                float *out = c->skin;
                if (!out || bt->nverts > c->skin_cap)
                    continue;
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
