/*
 * water.c -- see water.h for what here is the game's and what is the port's.
 */

#include "water.h"
#include "vis_data.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TWO_PI 6.2831853f
#define DEG (float)(M_PI / 180.0)

/* Water this deep heaves at full amplitude; shallower fades to flat. The
   engine's own equivalent is the magnet/clampY block, which is not
   transcribed. This is the port's stand-in and the one number in this file that
   is not the game's. Chosen as amp + amp2 rounded up: a swell that would
   otherwise reach the seabed is exactly the one that has to be damped. */
#define WATER_DEPTH_FADE 0.5f

/* WSURF_OFFSET is `offset` out of the level's WaterLOD section, raw*0.01 - 0.5,
   which is -0.37 m for beach_1. The CONSTANT is recovered; that it is the
   surface's vertical offset is INFERRED -- the loader stashes it at param_2[0x1d]
   and the code that consumes it is not transcribed. It is applied because the
   authored `sea` tiles sit at y = 0, the waterline that puts on the beach is
   visibly too far up it, and this is the one recovered value left with the right
   sign and magnitude to explain that. If it turns out to mean something else,
   this line is where to look. */

/* WATER_SWELL_AMP lives in water.h -- small on purpose: the reference
   screenshots show an essentially flat ocean whose motion is in the texture and
   the foam, and anything larger reads as a storm next to a 0.42 m car. */


/* ------------------------------------------------------------------ sine LUT
 *
 * Two sines per surface vertex per frame over several thousand vertices is
 * real work on a 444 MHz Cortex-A9, and water has no accuracy requirement
 * whatsoever. 1024 entries is 0.35 degrees of step, which the linear
 * interpolation below smooths out entirely.
 */
#define SIN_BITS 10
#define SIN_N (1 << SIN_BITS)
static float sin_lut[SIN_N + 1];
static int sin_ready;

static void sin_init(void)
{
    int i;
    if (sin_ready)
        return;
    for (i = 0; i <= SIN_N; i++)
        sin_lut[i] = sinf((float)i * (TWO_PI / (float)SIN_N));
    sin_ready = 1;
}

/* phase in turns, not radians */
static float fsin(float turns)
{
    float f = (turns - floorf(turns)) * (float)SIN_N;
    int i = (int)f;
    float frac = f - (float)i;
    return sin_lut[i] + (sin_lut[i + 1] - sin_lut[i]) * frac;
}

static unsigned int rnd(water_t *w)
{
    w->rng ^= w->rng << 13;
    w->rng ^= w->rng >> 17;
    w->rng ^= w->rng << 5;
    return w->rng;
}

/* 0..1 */
static float rnd01(water_t *w) { return (float)(rnd(w) & 0xffffff) / 16777216.f; }

/* ------------------------------------------------------------ the two waves
 *
 * WSURF_* give the two wave trains' DIRECTIONS and RATES. They do not give
 * amplitudes -- FUN_00521540 never reads `amp` at all, and it stores 1/amp2 and
 * 1/length2, which is what you precompute for a divisor rather than for a
 * height. So this returns a normalised -1..+1 signal and the callers scale it.
 *
 * The first version of this file scaled it by `amp` + `amp2` = 0.41 m and
 * displaced the sea surface with it. On a beach whose car is 0.42 m long that is
 * a storm, and `amp` is a key the engine does not even read. The retail game's
 * ocean is visually flat and animated by its texture; see CLAUDE.md.
 *
 * `stretch` divides the whole phase, which scales wavelength and period by the
 * same factor and so leaves the wave SPEED alone. The surface passes 1; the
 * shoreline foam passes COAST_WAVE_STRETCH, because the coast bands are coarser
 * than the recovered wavelength and sampling it per vertex aliases -- see
 * water.h.
 */
static float surf_signal(float x, float z, float t, float stretch)
{
    static int ready;
    static float d1x, d1z, k1, d2x, d2z, k2;
    float is = (stretch > 1e-4f) ? 1.f / stretch : 1.f;
    float h;

    if (!ready) {
        float a1 = WSURF_ANGLE_DEG * DEG, a2 = WSURF_ANGLE2_DEG * DEG;
        float l1 = WSURF_SPEED * WSURF_PERIOD;
        d1x = sinf(a1); d1z = cosf(a1);
        d2x = sinf(a2); d2z = cosf(a2);
        k1 = (l1 > 1e-4f) ? 1.f / l1 : 0.f;
        k2 = (WSURF_LENGTH2 > 1e-4f) ? 1.f / WSURF_LENGTH2 : 0.f;
        ready = 1;
    }
    h = 0.5f * fsin(((x * d1x + z * d1z) * k1 - t / WSURF_PERIOD) * is);
    h += 0.5f * fsin(((x * d2x + z * d2z) * k2 - t / WSURF_PERIOD2) * is);
    return h;
}

/*
 * The height the SHORELINE FOAM keys on. This one is pinned down: WaterLOD_Coast
 * ships HeightOn and HeightOff, the surface heights at which the foam is fully
 * on and fully off, so a signal spanning exactly that range is the one the two
 * constants were written for.
 */
static float shore_height(float x, float z, float t)
{
    const float mid = 0.5f * (COAST_HEIGHT_ON + COAST_HEIGHT_OFF);
    const float half = 0.5f * (COAST_HEIGHT_ON - COAST_HEIGHT_OFF);
    return mid + half * surf_signal(x, z, t, COAST_WAVE_STRETCH);
}

float water_height(const water_t *w, float x, float z)
{
    return WATER_SWELL_AMP * surf_signal(x, z, w->t, 1.f);
}

/* ------------------------------------------------------------------- setup */

/* How deep the water is under a surface vertex, or a large number where there
   is no ground below it at all (open ocean, or off the collision grid). */
static float depth_at(const col_t *col, const vtx_t *v)
{
    float gy, nx, ny, nz;
    if (!col || !col_ground_at(col, v->x, v->z, v->y, &gy, &nx, &ny, &nz))
        return 1e9f;
    return v->y - gy;
}

static void build_damping(water_t *w, const col_t *col)
{
    scene_t *s = w->scene;
    unsigned int i, j;

    for (i = 0; i < s->n_batches; i++) {
        batch_t *b = &s->batches[i];
        if (!(b->flags & BATCH_WATER))
            continue;
        w->damp[i] = malloc(b->nverts * sizeof(float));
        if (!w->damp[i])
            continue;
        for (j = 0; j < b->nverts; j++) {
            float d = depth_at(col, &b->verts[j]) / WATER_DEPTH_FADE;
            if (d < 0.f) d = 0.f;
            if (d > 1.f) d = 1.f;
            /* smoothstep, so the seam has no visible crease */
            w->damp[i][j] = d * d * (3.f - 2.f * d);
        }
    }
}

/*
 * water_init is reached again on every track change AND on every texture-quality
 * change (which reloads both scenes). It memsets over three pointer arrays and
 * everything they hold, so without this every reload leaked the damping and the
 * per-vertex colour buffers -- a slow bite out of the same newlib heap vitaGL's
 * RAM pool draws from.
 *
 * It is the CALLER's job, not water_init's, because water_init has always
 * accepted an uninitialised struct and vis_test still hands it a stack local;
 * freeing from in there would free garbage on the first call.
 */
void water_free(water_t *w)
{
    unsigned int i;

    /* n_alloc, not scene->n_batches: by the time a reload gets here the caller
       has usually already released the scene these arrays were sized against,
       and reading the new batch count would walk off the end of the old ones. */
    for (i = 0; i < w->n_alloc; i++) {
        if (w->damp)       free(w->damp[i]);
        if (w->coast_rgba) free(w->coast_rgba[i]);
        if (w->surf_rgba)  free(w->surf_rgba[i]);
    }
    free(w->damp);
    free(w->coast_rgba);
    free(w->surf_rgba);
    memset(w, 0, sizeof(*w));
}

void water_init(water_t *w, scene_t *scene, const col_t *col)
{
    unsigned int i;

    /* A PLAIN memset, not water_free: this has always accepted an uninitialised
       struct (vis_test hands it a stack local) and it must keep doing so. The
       caller owns the free -- see water_free's comment for why that is not an
       oversight. */
    memset(w, 0, sizeof(*w));
    w->scene = scene;
    w->n_alloc = scene->n_batches;
    w->rng = 0x1234567u;
    sin_init();

    w->damp = calloc(scene->n_batches, sizeof(float *));
    w->coast_rgba = calloc(scene->n_batches, sizeof(unsigned char *));
    w->surf_rgba = calloc(scene->n_batches, sizeof(unsigned char *));

    for (i = 0; i < scene->n_batches; i++) {
        batch_t *b = &scene->batches[i];
        if (!(b->flags & BATCH_ANY_WATER))
            continue;
        scene_keep_rest(b);
        if (b->flags & BATCH_COAST)
            w->coast_rgba[i] = malloc((size_t)b->nverts * 4);
        if (b->flags & BATCH_WATER)
            w->surf_rgba[i] = malloc((size_t)b->nverts * 4);
    }
    build_damping(w, col);

    /* The surface's own alpha, from the depth measure build_damping just made:
       alphaMin in the shallows so the wet sand reads through, alphaMax out at
       sea. alphaPow shapes the ramp. These are all recovered (FUN_00521540) and
       went unused in the first build, which is why the sea met the sand at a
       hard edge and read as a step. */
    for (i = 0; i < scene->n_batches; i++) {
        batch_t *b = &scene->batches[i];
        unsigned int j;
        if (!(b->flags & BATCH_WATER) || !w->surf_rgba[i])
            continue;
        for (j = 0; j < b->nverts; j++) {
            /* Its OWN depth ramp, not the swell damping's. Sharing the latter
               put every vertex past 0.5 m of depth at full opacity, which is
               most of the sea. */
            float d = depth_at(col, &b->verts[j]) / WATER_ALPHA_DEPTH;
            float k, av;
            unsigned char *p;
            if (d < 0.f) d = 0.f;
            if (d > 1.f) d = 1.f;
            k = powf(d, WSURF_ALPHA_POW);
            av = WSURF_ALPHA_MIN + (WSURF_ALPHA_MAX - WSURF_ALPHA_MIN) * k;
            p = &w->surf_rgba[i][j * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = (unsigned char)(av * 255.f + 0.5f);
        }
    }

    w->wave_tex = scene_tex(scene, "water_wave");

    /* One spawner per water_wave_N marker, each with the two independent
       timers FUN_00525700 keeps: a long one and a short one. */
    for (i = 0; i < scene->n_markers && w->n_spawn < WATER_MAX_SPAWN; i++) {
        marker_t *m = &scene->markers[i];
        wave_spawn_t *sp;
        if (strncmp(m->name, "water_wave_", 11))
            continue;
        sp = &w->spawn[w->n_spawn++];
        sp->x = m->x;
        sp->y = m->y;
        sp->z = m->z;
        sp->dx = sinf(m->yaw * DEG);
        sp->dz = cosf(m->yaw * DEG);
        /* stagger the first firing so all five markers do not break together */
        sp->t_long = WAVE_TIME_LONG * rnd01(w);
        sp->t_short = WAVE_TIME_SHORT * rnd01(w);
    }
}

/* --------------------------------------------------------------- the waves */

static void wave_spawn(water_t *w, const wave_spawn_t *sp, int is_long)
{
    int i;
    float a, c, s;

    for (i = 0; i < WATER_MAX_WAVES; i++)
        if (!w->waves[i].active)
            break;
    if (i == WATER_MAX_WAVES)
        return;

    /* WaterLOD_Wave: Angle is the spread around the marker's facing, Len the
       spread along the crest. */
    a = (rnd01(w) * 2.f - 1.f) * WAVE_SPAWN_ANGLE * DEG;
    c = cosf(a);
    s = sinf(a);
    w->waves[i].dx = sp->dx * c + sp->dz * s;
    w->waves[i].dz = -sp->dx * s + sp->dz * c;
    /* offset along the crest, which is the travel direction turned 90 degrees */
    a = (rnd01(w) * 2.f - 1.f) * WAVE_SPAWN_LEN;
    w->waves[i].x = sp->x + w->waves[i].dz * a;
    w->waves[i].y = sp->y;
    w->waves[i].z = sp->z - w->waves[i].dx * a;
    /* FUN_00529c60: a long wave takes TimeLifeLong flat, a short one takes
       TimeLifeShort with +/- TimeLifeShortDisp of spread. */
    w->waves[i].life = is_long
        ? WAVE_LIFE_LONG
        : WAVE_LIFE_SHORT + (rnd01(w) * 2.f - 1.f) * WAVE_LIFE_SHORT_DISP;
    w->waves[i].age = 0.f;
    w->waves[i].u0 = rnd01(w);
    w->waves[i].active = 1;
}

/* The crest's height envelope over its life: up over IncTime, flat, down over
   what is left after DecTime. FUN_0052a030 reads it out of the record at +0x48
   and multiplies it by Height. */
static float wave_env(const wave_t *v)
{
    float a = (v->life > 1e-4f) ? v->age / v->life : 1.f;
    if (a < WAVE_INC_TIME)
        return (WAVE_INC_TIME > 1e-4f) ? a / WAVE_INC_TIME : 1.f;
    if (a > WAVE_DEC_TIME) {
        float d = 1.f - WAVE_DEC_TIME;
        return (d > 1e-4f) ? (1.f - a) / d : 0.f;
    }
    return 1.f;
}

void water_step(water_t *w, float dt)
{
    int i;

    w->t += dt;
    w->n_live = 0;

    for (i = 0; i < w->n_spawn; i++) {
        wave_spawn_t *sp = &w->spawn[i];
        sp->t_long -= dt;
        if (sp->t_long < 0.f) {
            wave_spawn(w, sp, 1);
            sp->t_long = WAVE_TIME_LONG;
        }
        sp->t_short -= dt;
        if (sp->t_short < 0.f) {
            wave_spawn(w, sp, 0);
            sp->t_short = WAVE_TIME_SHORT;
        }
    }

    for (i = 0; i < WATER_MAX_WAVES; i++) {
        wave_t *v = &w->waves[i];
        if (!v->active)
            continue;
        v->age += dt;
        if (v->age >= v->life) {
            v->active = 0;
            continue;
        }
        v->x += v->dx * WAVE_SPEED * dt;
        v->z += v->dz * WAVE_SPEED * dt;
        w->n_live++;
    }
}

/*
 * One wave sprite. FUN_0052a030's geometry, in its own terms:
 *
 *   axis    the crest direction; the quad runs +/- Len along it
 *   up      (camera - position), with its component along the crest removed,
 *           normalised -- so the quad is a billboard hinged on its crest
 *   bottom  position +/- Len*axis            + DHeight*up
 *   top     bottom + Height*envelope*up
 *
 * u sweeps one full turn of the texture, offset by the record's scroll value.
 */
static void wave_draw(water_t *w, const wave_t *v, const float eye[3])
{
    vtx_t q[4];
    float ax = v->dz, az = -v->dx;          /* the crest, across the travel */
    float ex = eye[0] - v->x, ey = eye[1] - v->y, ez = eye[2] - v->z;
    float d = ex * ax + ez * az;
    float ux, uy, uz, len;
    float env = wave_env(v);
    float h = WAVE_HEIGHT * env;
    float u0 = v->u0 + w->t * WAVE_ANIM_SPEED;

    ux = ex - ax * d;
    uy = ey;
    uz = ez - az * d;
    len = sqrtf(ux * ux + uy * uy + uz * uz);
    if (len < 1e-4f)
        return;
    ux /= len; uy /= len; uz /= len;

    q[0].x = v->x - ax * WAVE_LEN + ux * WAVE_DHEIGHT;
    q[0].y = v->y + uy * WAVE_DHEIGHT;
    q[0].z = v->z - az * WAVE_LEN + uz * WAVE_DHEIGHT;
    q[1].x = v->x + ax * WAVE_LEN + ux * WAVE_DHEIGHT;
    q[1].y = q[0].y;
    q[1].z = v->z + az * WAVE_LEN + uz * WAVE_DHEIGHT;
    q[2].x = q[1].x + ux * h; q[2].y = q[1].y + uy * h; q[2].z = q[1].z + uz * h;
    q[3].x = q[0].x + ux * h; q[3].y = q[0].y + uy * h; q[3].z = q[0].z + uz * h;

    q[0].u = u0;        q[0].v = 1.f;
    q[1].u = u0 + 1.f;  q[1].v = 1.f;
    q[2].u = u0 + 1.f;  q[2].v = 0.f;
    q[3].u = u0;        q[3].v = 0.f;

    glColor4f(1.f, 1.f, 1.f, env);
    glVertexPointer(3, GL_FLOAT, sizeof(vtx_t), &q[0].x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(vtx_t), &q[0].u);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

/* ------------------------------------------------------------------- draw  */

static void animate_surface(water_t *w, unsigned int bi)
{
    batch_t *b = &w->scene->batches[bi];
    const float *damp = w->damp[bi];
    float su = w->t * WSURF_TEX_SPEED_MIN, sv = w->t * WSURF_TEX_SPEED_MAX;
    unsigned int j;

    if (!b->rest)
        return;
    for (j = 0; j < b->nverts; j++) {
        const vtx_t *r = &b->rest[j];
        float k = damp ? damp[j] : 1.f;
        b->verts[j].y = r->y + WSURF_OFFSET
                      + WATER_SWELL_AMP * surf_signal(r->x, r->z, w->t, 1.f) * k;
        /* The engine scrolls the surface texture rather than the vertices; the
           tiles' own UVs stay put and texScaleX/Z set the world-to-UV rate. */
        b->verts[j].u = r->u + su * WSURF_TEX_SCALE_X;
        b->verts[j].v = r->v + sv * WSURF_TEX_SCALE_Z;
    }
}

/* The coast band, the stream and the waterfall: scroll the UVs, and hold the
   band WATER_DECAL_LIFT off its rest height. These three are the surfaces the
   art laid on solid geometry -- see water.h for the measurement -- so without
   the lift they z-fight the ground they sit on. Written from rest every frame
   rather than once at load, so it cannot drift and so a test can see it. */
static void animate_scroll(water_t *w, unsigned int bi, float du, float dv)
{
    batch_t *b = &w->scene->batches[bi];
    unsigned int j;

    if (!b->rest)
        return;
    for (j = 0; j < b->nverts; j++) {
        b->verts[j].y = b->rest[j].y + WATER_DECAL_LIFT;
        b->verts[j].u = b->rest[j].u + du;
        b->verts[j].v = b->rest[j].v + dv;
    }
}

static void animate_coast(water_t *w, unsigned int bi)
{
    batch_t *b = &w->scene->batches[bi];
    unsigned char *rgba = w->coast_rgba[bi];
    float span = COAST_HEIGHT_ON - COAST_HEIGHT_OFF;
    unsigned int j;

    animate_scroll(w, bi, 0.f, w->t * COAST_SCROLL_VEL);
    if (!rgba)
        return;
    for (j = 0; j < b->nverts; j++) {
        /* WaterLOD_Coast's two heights, used as what they say they are: the
           surface height at which the foam is fully on, and the one at which
           it is fully off. */
        float h = shore_height(b->rest[j].x, b->rest[j].z, w->t);
        float k = (span > 1e-4f) ? (h - COAST_HEIGHT_OFF) / span : 1.f;
        float a;
        if (k < 0.f) k = 0.f;
        if (k > 1.f) k = 1.f;
        a = COAST_ALPHA_MIN + (COAST_ALPHA_MAX - COAST_ALPHA_MIN) * k;
        rgba[j * 4] = rgba[j * 4 + 1] = rgba[j * 4 + 2] = 255;
        rgba[j * 4 + 3] = (unsigned char)(a * 255.f);
    }
}

static void draw_batch(const batch_t *b)
{
    glBindTexture(GL_TEXTURE_2D, b->gl_tex);
    glVertexPointer(3, GL_FLOAT, sizeof(vtx_t), &b->verts[0].x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(vtx_t), &b->verts[0].u);
    glDrawElements(GL_TRIANGLES, b->nidx, GL_UNSIGNED_SHORT, b->idx);
}

void water_draw(water_t *w, const float eye[3])
{
    scene_t *s = w->scene;
    unsigned int i;
    int j;

    /* --- the sea surface: BLENDED, with the depth-driven alpha above ------
       Depth writes stay ON: it is a single layer with no self-overlap, and the
       foam and the wave sprites have to depth-test against it. */
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnableClientState(GL_COLOR_ARRAY);
    for (i = 0; i < s->n_batches; i++) {
        batch_t *b = &s->batches[i];
        if (!(b->flags & BATCH_WATER) || !w->surf_rgba[i])
            continue;
        animate_surface(w, i);
        glColorPointer(4, GL_UNSIGNED_BYTE, 0, w->surf_rgba[i]);
        draw_batch(b);
    }
    glDisableClientState(GL_COLOR_ARRAY);
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);

    /* --- the stream and the waterfall, also blended ----------------------
       From here down every surface is a band the art laid ON solid geometry, so
       everything is drawn biased toward the camera. WATER_DECAL_LIFT does the
       same job in world space and the two are deliberately both on: the lift is
       a fixed distance and stops being enough far out, the bias scales with the
       depth slope but depends on what the driver makes of GXM's bias units. */
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(WATER_DECAL_OFFSET_FACTOR, WATER_DECAL_OFFSET_UNITS);

    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.f, 1.f, 1.f, WATER_STREAM_ALPHA);
    for (i = 0; i < s->n_batches; i++) {
        batch_t *b = &s->batches[i];
        if (b->flags & (BATCH_STREAM | BATCH_FALL)) {
            animate_scroll(w, i, 0.f, w->t * STREAM_SCROLL_VEL);
            draw_batch(b);
        }
    }
    glColor4f(1.f, 1.f, 1.f, 1.f);
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);

    /* --- blended: the foam band ------------------------------------------ */
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    glEnableClientState(GL_COLOR_ARRAY);
    for (i = 0; i < s->n_batches; i++) {
        batch_t *b = &s->batches[i];
        if (!(b->flags & BATCH_COAST) || !w->coast_rgba[i])
            continue;
        animate_coast(w, i);
        glColorPointer(4, GL_UNSIGNED_BYTE, 0, w->coast_rgba[i]);
        draw_batch(b);
    }
    glDisableClientState(GL_COLOR_ARRAY);

    /* --- blended: the breaking waves ------------------------------------- */
    if (w->wave_tex) {
        glBindTexture(GL_TEXTURE_2D, w->wave_tex);
        glDisable(GL_CULL_FACE);
        for (j = 0; j < WATER_MAX_WAVES; j++)
            if (w->waves[j].active)
                wave_draw(w, &w->waves[j], eye);
        glEnable(GL_CULL_FACE);
        glColor4f(1.f, 1.f, 1.f, 1.f);
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(0.f, 0.f);
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);
}
