/*
 * trace.c -- the tyre marks. See trace.h for the functions this came out of and
 * for the collinear-merge mechanism that makes a 32-slot ring hold a long trail.
 */

#include "trace.h"

#include "carani.h"                       /* carani_tire_width */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FALLBACK half-width, for a scene with no rig to measure. FUN_0052f990 builds
 * its two edge vertices as `pos +- param_9 * 0.05 * lat`, and param_9 comes from
 * FUN_0052f310, which is not recovered -- so the width is the port's either way.
 * This is the wheel's own physics radius, which is where it came from before the
 * mesh was measured.
 *
 * It is a fallback and not the rule because the physics radius is not the tyre's
 * WIDTH and the two are not in any fixed proportion. Against the drawn wheels it
 * came out at 92% of the Overkill's tyre, 76% of the Buggy's rear and **107%**
 * of its front -- a mark wider than the tyre that made it -- and 74% of the
 * Hummer's, which is what "the marks are about three quarters of the tyre" was.
 * A single fraction cannot fix three cars that disagree in both directions, so
 * mesh_half_width measures each wheel instead and this is only reached by a car
 * packed without --rig, and by the synthetic fixtures.
 */
#define TRACE_WIDTH_FRAC 0.5f

/* Off the surface along its normal, for the same reason shadow.c lifts its
 * decal: two coplanar surfaces z-fight per pixel. Smaller than SHADOW_LIFT
 * because a mark is a much thinner thing lying under the car -- 8 mm is over the
 * collision grid's own slop and still an eighth of a wheel radius. The engine
 * gets this for free: the contact point it samples is already the 6 mm clear of
 * the surface that suspRetract leaves (the tol = 0.006 at 0x4f5fc9). */
#define TRACE_LIFT 0.008f

/* FUN_0052fd00 skips any quad whose sample is more than this from the camera. */
#define TRACE_DRAW_RANGE 20.0f

/* Longest strip the draw buffer holds: every wheel's whole ring, as quads. */
#define TRACE_MAX_QUADS (RB_MAX_WHEELS * TRACE_RING)

/*
 * Stage 0 for the mark, and back again.
 *
 * The engine's op is D3DTOP_MODULATEALPHA_ADDCOLOR with ARG1 = DIFFUSE and
 * ARG2 = TEXTURE (FUN_0045c980 at 0x0045cda6), which evaluates to
 *
 *     src = diffuse.rgb + diffuse.a * tex
 *         = (1-f) * neutral + f * tex
 *
 * -- a lerp from the texture's neutral level to the texture. GL_COMBINE with
 * GL_INTERPOLATE is the same expression with the arguments named differently:
 * Arg0*Arg2 + Arg1*(1-Arg2), so Arg0 = the texture, Arg1 = the constant, and
 * Arg2 = the vertex colour carrying f.
 *
 * Reading f out of the vertex COLOUR rather than its alpha is deliberate: the
 * two are written the same value, and an operand of GL_SRC_COLOR is the one
 * combiner argument every fixed-function path supports.
 */
static void gl_trace_env(int on)
{
    if (!on) {
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        return;
    }
    {
        /* not const: vitaGL's glTexEnvfv takes a mutable pointer */
        GLfloat neutral[4] = { TRACE_NEUTRAL, TRACE_NEUTRAL,
                               TRACE_NEUTRAL, 1.f };
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_TEXTURE);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB, GL_CONSTANT);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC2_RGB, GL_PRIMARY_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_COLOR);
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, neutral);
    }
}

/* Half the width of the tyre on rig part `part`, off the packed mesh: half the
 * EXTENT of that wheel's vertices along the node's local +Z -- which is the
 * axle, the same axis carani_update rolls and widens the wheel about. Model
 * space, so it is the width the wheel is DRAWN at before the tuning scales it.
 *
 * Half the extent and not the largest offset from the node, because a wheel mesh
 * is not always centred on its own node: the Buggy's front wheel sits 6.7 mm
 * inboard of its, and the largest offset would call that 0.0297 against a tyre
 * that is really 0.0459 wide -- 29% too wide. (The mark is still drawn centred
 * on the contact point, so on that one wheel it is offset from the tread by
 * those 6.7 mm. Centring it on the mesh instead would offset it from the patch
 * the car actually rolls on, which is worse and is also not what the tread
 * leaves.)
 *
 * The widest point of these tyres is the shoulder, at 0.5 to 0.9 of the radius,
 * not the crown -- they are round in section. Taking a crown band instead would
 * make the mark 84% of the tyre on the Overkill and 72% on the Buggy, which is
 * the complaint this replaced, arrived at from the other side. The silhouette is
 * what the eye compares the mark against, so the silhouette is what it uses.
 */
static float mesh_half_width(const scene_t *s, int part)
{
    const carani_t *r = &s->rig;
    const float *rest;
    float ax[3], org[3], n, lo = 1e30f, hi = -1e30f;
    unsigned int b;
    int k;

    if (part < 0 || part >= r->n)
        return 0.f;
    /* It really has to be a wheel. An UNBOUND rig.wheel[] is all zeroes, which
       points at part 0 -- __root__, the whole car body -- and measuring that
       would hand the mark a half-width of the entire model. */
    if (strncmp(r->part[part].name, "WHEEL_", 6) != 0)
        return 0.f;
    rest = r->part[part].rest;
    ax[0] = rest[8]; ax[1] = rest[9]; ax[2] = rest[10];
    n = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    if (n < 1e-6f)
        return 0.f;
    for (k = 0; k < 3; k++)
        ax[k] /= n;
    org[0] = rest[12]; org[1] = rest[13]; org[2] = rest[14];

    for (b = 0; b < s->n_batches; b++) {
        const batch_t *bt = &s->batches[b];
        unsigned int v;
        if ((int)bt->part != part || !bt->verts)
            continue;
        for (v = 0; v < bt->nverts; v++) {
            float d = (bt->verts[v].x - org[0]) * ax[0]
                    + (bt->verts[v].y - org[1]) * ax[1]
                    + (bt->verts[v].z - org[2]) * ax[2];
            if (d < lo) lo = d;
            if (d > hi) hi = d;
        }
    }
    return hi > lo ? (hi - lo) * 0.5f : 0.f;
}

void trace_init(trace_t *tr, const scene_t *src)
{
    int i;

    memset(tr, 0, sizeof(*tr));
    for (i = 0; i < TRACE_TEX_N; i++) {
        char name[32];
        /* the game's own sprintf, FUN_0054603b(buf, "t_halfdry_tire2_%d") */
        snprintf(name, sizeof(name), TRACE_TEX_FMT, i + 1);
        tr->tex[i] = src ? scene_tex(src, name) : 0;
        if (tr->tex[i])
            tr->n_tex = i + 1;
    }
    for (i = 0; i < RB_MAX_WHEELS; i++) {
        tr->w[i].head = -1;
        /* FUN_0052f050: 0x20 for wheels 0 and 1, 0x30 for the rest. */
        tr->w[i].cap = (i < 2) ? TRACE_RING_FRONT : TRACE_RING_REAR;
        if (tr->w[i].cap > TRACE_RING)
            tr->w[i].cap = TRACE_RING;
    }
    tr->enabled = (tr->n_tex > 0);
    trace_fit_tyres(tr, src);
}

void trace_fit_tyres(trace_t *tr, const scene_t *src)
{
    int i;

    for (i = 0; i < RB_MAX_WHEELS; i++)
        tr->half_w[i] = (src && src->has_rig)
                        ? mesh_half_width(src, src->rig.wheel[i]) : 0.f;
}

void trace_clear(trace_t *tr)
{
    int i, k;
    for (i = 0; i < RB_MAX_WHEELS; i++) {
        for (k = 0; k < TRACE_RING; k++)
            tr->w[i].pt[k].used = 0;
        tr->w[i].head = -1;
        tr->w[i].n = 0;
        tr->w[i].was_down = 0;
    }
    tr->n_quads = 0;
}

/* ------------------------------------------------------------- the ring */

static int ring_prev(const trace_ring *r, int i)
{
    int p = i - 1;
    if (p < 0)
        p = r->cap - 1;
    return r->pt[p].used ? p : -1;
}

/*
 * FUN_0052fb60, verbatim. `prev`, `head` and `now` are three successive contact
 * points; the return codes are the engine's and trace.h lists them.
 *
 * Note which vectors it compares: d1 is head-prev, the segment already stored,
 * and d2 is now-prev, from the older sample to the new one. So "collinear" means
 * the new point lies on the line the stored segment already runs along, which is
 * what makes overwriting the head safe.
 */
int trace_break_test(const float prev[3], const float head[3],
                     const float now[3])
{
    float d1[3], d2[3], l1, l2, dot, perp[3], pl;
    int k;

    for (k = 0; k < 3; k++) {
        d1[k] = head[k] - prev[k];
        d2[k] = now[k] - prev[k];
    }
    l1 = d1[0] * d1[0] + d1[1] * d1[1] + d1[2] * d1[2];
    if (l1 < 1e-06f)
        return 0;
    l2 = d2[0] * d2[0] + d2[1] * d2[1] + d2[2] * d2[2];
    if (fabsf(l1 - l2) < 0.01f || l2 < 1e-06f)
        return 5;
    if (TRACE_MAX_LEN * TRACE_MAX_LEN < l2)
        return 4;
    dot = d2[0] * d1[0] + d2[1] * d1[1] + d2[2] * d1[2];
    if (dot < l1)
        return 3;
    dot /= l2;
    for (k = 0; k < 3; k++)
        perp[k] = d1[k] - dot * d2[k];
    pl = perp[0] * perp[0] + perp[1] * perp[1] + perp[2] * perp[2];
    if (TRACE_MAX_HEIGHT * TRACE_MAX_HEIGHT < pl)
        return 1;
    return 0;
}

/* FUN_0052f990: fill one slot. `link` is the sample this one continues from, or
   NULL for the first of a strip -- the running length accumulates through it. */
static void write_pt(trace_pt *p, const trace_pt *link, const float pos[3],
                     const float nrm[3], const float lat[3],
                     float half_w, float strength, int tex, unsigned int strip)
{
    int k;

    memset(p, 0, sizeof(*p));
    p->used = 1;
    p->life = TRACE_LIFE;
    p->strength = strength;
    p->tex = tex;
    p->half_w = half_w;
    p->strip = strip;
    for (k = 0; k < 3; k++) {
        p->pos[k] = pos[k];
        p->nrm[k] = nrm[k];
        p->lat[k] = lat[k];
    }
    if (link) {
        float dx = pos[0] - link->pos[0];
        float dy = pos[1] - link->pos[1];
        float dz = pos[2] - link->pos[2];
        p->len = sqrtf(dx * dx + dy * dy + dz * dz) + link->len;
    }
    p->u = TRACE_SCALE_COEFF * p->len;
    /* The engine wraps at 1000 rather than at 1: the U is a repeat count, and
       keeping it in a small range is what stops a long trail losing precision.
       0x0052fa8b. */
    if (p->u > 1000.0f)
        p->u -= 1000.0f;
}

/*
 * FUN_0052f700: one sample for one wheel.
 *
 * The order of the four outcomes is the original's, and it matters:
 *   - an empty ring just appends;
 *   - `force_new` (the wheel was off the ground) always starts a strip;
 *   - a texture change breaks the strip;
 *   - otherwise, if the mark has not flipped over (dot(lat, prev.lat) >= 0) and
 *     the new point is collinear, REWRITE the head; if the test says "no
 *     movement" do nothing at all; anything else appends.
 */
static void ring_add(trace_ring *r, const float pos[3], const float nrm[3],
                     const float lat[3], float half_w, float strength, int tex,
                     int force_new)
{
    int prev;
    trace_pt *head;

    if (r->head < 0) {
        r->head = 0;
        r->strip++;
        write_pt(&r->pt[0], NULL, pos, nrm, lat, half_w, strength, tex,
                 r->strip);
        r->n = 1;
        return;
    }
    head = &r->pt[r->head];

    if (!force_new && head->tex == tex) {
        prev = ring_prev(r, r->head);
        if (prev >= 0 && r->pt[prev].strip == head->strip) {
            const trace_pt *pp = &r->pt[prev];
            float d = lat[0] * pp->lat[0] + lat[1] * pp->lat[1]
                      + lat[2] * pp->lat[2];
            if (d >= 0.0f) {
                int code = trace_break_test(pp->pos, head->pos, pos);
                if (code == 5)
                    return;                 /* the car has not moved */
                if (code == 0) {
                    write_pt(head, pp, pos, nrm, lat, half_w, strength, tex,
                             head->strip);
                    return;
                }
            }
        }
        /* Not mergeable, but the strip carries on: append into the same strip. */
        r->head = (r->head + 1) % r->cap;
        write_pt(&r->pt[r->head], head, pos, nrm, lat, half_w, strength, tex,
                 head->strip);
        if (r->n < r->cap)
            r->n++;
        return;
    }

    r->head = (r->head + 1) % r->cap;
    r->strip++;
    write_pt(&r->pt[r->head], NULL, pos, nrm, lat, half_w, strength, tex,
             r->strip);
    if (r->n < r->cap)
        r->n++;
}

/* ------------------------------------------------------------------ step */

void trace_step(trace_t *tr, const rb_car *c, const col_t *col, float dt)
{
    int w, k;

    if (!tr->enabled)
        return;

    for (w = 0; w < c->nwheels && w < RB_MAX_WHEELS; w++) {
        const rb_wheel_contact *h = &c->hit[w];
        trace_ring *r = &tr->w[w];
        float lat[3], len, half_w, strength;
        int tex, force;

        /* Age this wheel's ring whatever happens to it this frame. */
        for (k = 0; k < TRACE_RING; k++) {
            trace_pt *p = &r->pt[k];
            if (!p->used)
                continue;
            p->life -= dt;
            if (p->life <= 0.0f) {
                p->used = 0;
                if (r->n > 0)
                    r->n--;
                if (k == r->head)
                    r->head = -1;
            }
        }

        if (!h->active || h->in_water) {
            r->was_down = 0;
            continue;
        }

        /* Across the mark: the wheel's axle direction, projected onto the
           contact plane. The body's +X is the axle line (see CLAUDE.md, "Car
           axes"), and taking the component along the normal out of it keeps the
           quad flat on the surface even on a slope. */
        lat[0] = c->m[0];
        lat[1] = c->m[1];
        lat[2] = c->m[2];
        len = lat[0] * h->normal[0] + lat[1] * h->normal[1]
              + lat[2] * h->normal[2];
        for (k = 0; k < 3; k++)
            lat[k] -= len * h->normal[k];
        len = sqrtf(lat[0] * lat[0] + lat[1] * lat[1] + lat[2] * lat[2]);
        if (len < 1e-4f)
            continue;                       /* body rolled onto its side */
        for (k = 0; k < 3; k++)
            lat[k] /= len;

        /* As wide as the tyre above it, and as wide as the TUNING made that
           tyre -- carani_tire_width is the one place a level becomes a width,
           so the two cannot drift apart. */
        half_w = (tr->half_w[w] > 0.f ? tr->half_w[w]
                                      : c->wheel[w].radius * TRACE_WIDTH_FRAC)
                 * carani_tire_width(c);
        /* What this surface takes. FUN_0052f310 calls the classifier at
           0x0052f4f8 and indexes the jump table at 0x0052f6dc with the class it
           returns; classes 0, 4, 6 and 8 -- default, grass, metal and stone --
           jump past the whole mark and lay nothing. See TRACE_STRENGTH_TABLE.
           Falling through here rather than laying a zero-strength mark is what
           the original does, and it also means the next sample on ground that
           DOES mark starts a fresh strip rather than joining across the gap. */
        {
            static const float st[TRACE_SURF_CLASSES] = TRACE_STRENGTH_TABLE;
            int cls = col ? col_surface_at(col, h->point[0], h->point[1],
                                           h->point[2]) : 0;
            if (!col || !col->eng_surf)
                strength = TRACE_STRENGTH;      /* no data: as before */
            else if (cls > 0 && cls < TRACE_SURF_CLASSES)
                strength = st[cls];
            else
                strength = 0.f;
            if (strength <= 0.f)
                continue;
        }
        /* FUN_0052f310 at 0x0052f537 halves the strength for wheels 0 and 1 --
           the front pair -- so the steered wheels leave a fainter mark than the
           driven ones. */
        if (w < 2)
            strength *= TRACE_FRONT_STRENGTH;
        /* Which of the four marks. The engine loads all four and the choice is
           in FUN_0052f310, which is not recovered; the port keys it on the
           tyre upgrade, which is the one thing about a wheel that changes what
           its tread looks like -- and it is also what selects the tyre TEXTURE
           (carparts.c). Levels above the packed set fall back to the first. */
        tex = c->tire_upgrade;
        if (tex < 0 || tex >= tr->n_tex)
            tex = 0;

        /* A wheel that was off the ground cannot continue the strip it left --
           this is FUN_0052f700's param_8, and it is the only thing besides a
           texture change that breaks a strip. Nothing else will: an overrun of
           maxLen appends into the SAME strip in the original (FUN_0052f900 with
           param_8 = 1 clears the old head's +0x44, which is what permits the
           join), so a landing that is not flagged here is drawn as one quad
           reaching all the way back to the take-off. */
        force = (r->head < 0) || !r->pt[r->head].used || !r->was_down;
        ring_add(r, h->point, h->normal, lat, half_w, strength, tex, force);
        r->was_down = 1;
    }
}

/* ------------------------------------------------------------------ draw */

void trace_draw(trace_t *tr, const float eye[3])
{
    /* Heap, not BSS -- a full set of marks is TRACE_MAX_QUADS * 6 * 28 = 48 KB,
       past the 32 KB at which vitaGL's SAFER_DRAW_SPEEDHACK stops copying and
       hands GXM this pointer, and only the newlib heap is mapped for the GPU.
       The reasoning in full is on the same buffer in fx.c. */
    static vtx_t *v;
    static unsigned char *col;
    int w, i, k, nv, tex;
    float bias;

    tr->n_quads = 0;
    if (!tr->enabled)
        return;
    if (!v) {
        v = malloc(sizeof(*v) * TRACE_MAX_QUADS * 6);
        col = malloc((size_t)TRACE_MAX_QUADS * 6 * 4);
    }
    if (!v || !col)
        return;

    /* One draw per texture, because a batch is one bound texture and the marks
       can carry more than one. Usually only one pass has anything in it. */
    for (tex = 0; tex < tr->n_tex; tex++) {
        nv = 0;
        /* FUN_0052f250 zeroes the bias every frame and FUN_0052ff20 steps it by
           2e-05 per strip, so overlapping marks are separated by a hair instead
           of z-fighting with each other. It is NOT what lifts them off the
           ground -- that is TRACE_LIFT. */
        bias = 0.0f;
        for (w = 0; w < RB_MAX_WHEELS; w++) {
            trace_ring *r = &tr->w[w];
            for (i = 0; i < r->cap; i++) {
                int j = (r->head - i + r->cap * 2) % r->cap;
                int p2 = (j - 1 + r->cap) % r->cap;
                const trace_pt *b = &r->pt[j];
                const trace_pt *a = &r->pt[p2];
                float ax[3], bx[3], ay[3], by[3], f, dx, dy, dz;

                if (r->head < 0)
                    break;
                if (!b->used || !a->used || a == b)
                    continue;
                if (a->strip != b->strip)    /* the strip broke here */
                    continue;
                if (b->tex != tex)
                    continue;
                dx = b->pos[0] - eye[0];
                dy = b->pos[1] - eye[1];
                dz = b->pos[2] - eye[2];
                if (dx * dx + dy * dy + dz * dz
                    >= TRACE_DRAW_RANGE * TRACE_DRAW_RANGE)
                    continue;
                if (nv + 6 > TRACE_MAX_QUADS * 6)
                    break;

                bias += TRACE_DEPTH_STEP;
                for (k = 0; k < 3; k++) {
                    float la = a->nrm[k] * TRACE_LIFT;
                    float lb = b->nrm[k] * TRACE_LIFT;
                    ax[k] = a->pos[k] - a->lat[k] * a->half_w + la;
                    ay[k] = a->pos[k] + a->lat[k] * a->half_w + la;
                    bx[k] = b->pos[k] - b->lat[k] * b->half_w + lb;
                    by[k] = b->pos[k] + b->lat[k] * b->half_w + lb;
                }
                ax[1] += bias; ay[1] += bias; bx[1] += bias; by[1] += bias;

                /* FUN_0052fd00, 0x0052fd9f-0x0052fe08: the mark's strength is
                   held flat until only timeLife/4 is left, then ramped linearly
                   to nothing. Not a fade over the whole life -- a 60 s mark sits
                   at full strength for 45 s and then goes in 15.

                   The engine packs this one number into the vertex colour twice
                   over, as (1-f)*128 in RGB and f*255 in alpha, because its
                   stage op is `diffuse.rgb + diffuse.a * texture`. The port
                   feeds f itself and lets GL_INTERPOLATE do the same lerp, so
                   both channels carry f. See the blend note in trace.h. */
                f = b->life / (TRACE_LIFE * TRACE_FADE_FRAC);
                if (f > 1.f) f = 1.f;
                if (f < 0.f) f = 0.f;
                f *= b->strength;

                /*
                 * The repeating coordinate is V and the 0..1 one is U, not the
                 * other way round.
                 *
                 * FUN_0052f990 writes a 0.0/1.0 pair into one component of its
                 * two edge vertices and scaleCoeff*length into the other, and
                 * which is U and which is V does not survive the decompilation.
                 * The TEXTURE settles it: t_halfdry_tire2_<n> is 64 x 256 with
                 * the tread running down the LONG axis, so the mark's width has
                 * to be the 64-wide axis (U) and the direction of travel the
                 * 256-tall one (V). Getting it backwards draws the tread rotated
                 * a quarter turn, across the mark instead of along it.
                 *
                 * It also confirms scaleCoeff independently: one repeat per
                 * 1/3.28 = 305 mm of travel, against the 4:1 texture wanting
                 * 4 x 72 mm = 288 mm for a mark as wide as the tyre. Two
                 * recovered numbers and a texture aspect agreeing to 6%.
                 */
                {
                    const float *q[6] = { ax, ay, by, ax, by, bx };
                    const float qv[6] = { a->u, a->u, b->u, a->u, b->u, b->u };
                    const float qu[6] = { 0.f, 1.f, 1.f, 0.f, 1.f, 0.f };
                    for (k = 0; k < 6; k++) {
                        vtx_t *o = &v[nv];
                        unsigned char *c4 = &col[nv * 4];
                        o->x = q[k][0]; o->y = q[k][1]; o->z = q[k][2];
                        o->u = qu[k]; o->v = qv[k];
                        o->lu = o->lv = 0.f;
                        c4[0] = c4[1] = c4[2] = (unsigned char)(f * 255.f);
                        c4[3] = (unsigned char)(f * 255.f);
                        nv++;
                    }
                }
                tr->n_quads++;
            }
        }
        if (!nv)
            continue;

        glBindTexture(GL_TEXTURE_2D, tr->tex[tex]);
        glDisable(GL_ALPHA_TEST);
        glEnable(GL_BLEND);
        /* The engine's blend mode 5, verbatim: SRCBLEND = D3DBLEND_DESTCOLOR
           and DESTBLEND = D3DBLEND_SRCCOLOR at 0x0045c911, i.e. 2 * src * dst.
           A mark is a modulation of the ground, not a sprite laid over it. */
        glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);
        /* And stage 0's D3DTOP_MODULATEALPHA_ADDCOLOR (0x0045cda6), which is
           `lerp(neutral, texture, f)` once the diffuse colour is unpacked. GL
           has exactly one texture env that says that. Without it the mark fades
           to BLACK instead of to nothing, because plain GL_MODULATE has no term
           the constant can live in. */
        gl_trace_env(1);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        /* Both halves, as water.c does for its coplanar decals: the lift is a
           fixed distance and stops separating anything far out, the offset
           scales with the depth slope but depends on what GXM makes of its
           units. Neither alone is trustworthy here. */
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.f, -2.f);
        glEnableClientState(GL_COLOR_ARRAY);

        glVertexPointer(3, GL_FLOAT, sizeof(vtx_t), &v[0].x);
        glTexCoordPointer(2, GL_FLOAT, sizeof(vtx_t), &v[0].u);
        glColorPointer(4, GL_UNSIGNED_BYTE, 4, col);
        glDrawArrays(GL_TRIANGLES, 0, nv);

        glDisableClientState(GL_COLOR_ARRAY);
        glPolygonOffset(0.f, 0.f);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glEnable(GL_CULL_FACE);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glEnable(GL_ALPHA_TEST);
        /* Put the env mode back. Everything else in the app draws through
           GL_MODULATE, and a combiner left set makes the next batch sample a
           constant. Same class of bug as leaving GL_ALPHA_TEST off, which the
           shadow already caught once. */
        gl_trace_env(0);
    }
}
