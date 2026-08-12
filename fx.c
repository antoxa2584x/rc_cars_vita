/*
 * fx.c -- the wheel dust and the exhaust smoke, transcribed. See fx.h for the
 * map of which function each part came out of, and fx_data.h for the constants.
 *
 * Three things in here are the port's rather than the game's, and each says so
 * where it is defined: FX_SPRITE_UNIT, the billboard construction, and the
 * direction of the DynamicScale ramp (which is argued from the code, not
 * guessed -- see fx_scale).
 */

#include "fx.h"
#include "fx_data.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ScaleX/ScaleY are multipliers on the base sprite size FUN_00477940 sets, and
 * that function is not transcribed -- so the base is the port's one free
 * parameter here. 0.01 m per unit is the engine's own habit everywhere else a
 * size comes out of a slider (ShadowSize raw*0.01, ShadowShift raw*0.01-0.5,
 * every anim_cp key raw*0.01), and it puts the numbers where they belong on a
 * 1:10 car: a dust puff is born 1.5 cm across and grows to 49 cm behind a rear
 * wheel over its second of life, 37 cm behind a front one.
 */
#define FX_SPRITE_UNIT 0.01f

/* FUN_0052e320 walks the cameras and gives up if none is within 12 m of the
   wheel. One camera here. */
#define FX_EMIT_RANGE 12.0f

/* FUN_0052e320's gate on the curve input, in km/h. Also FUN_005303c0's. */
#define FX_MIN_KMH 0.25f

/* FUN_0052e320: rate = curve(speed) * mult * 0.66667. The two thirds is a
   literal at 0x0052e77c. */
#define FX_RATE_SCALE 0.66667f

/* ------------------------------------------------------------------ helpers */

/* FUN_005480ee is the engine's 15-bit rand; the callers turn it into 0..1 with
   *3.051851e-05 (1/32767) and into -1..1 with (x + x - 1). Reproduced with a
   xorshift so the host test is deterministic and does not depend on libc. */
static float rnd01(fx_t *fx)
{
    fx->seed ^= fx->seed << 13;
    fx->seed ^= fx->seed >> 17;
    fx->seed ^= fx->seed << 5;
    return (float)(fx->seed & 0x7FFF) * 3.051851e-05f;
}

static float rnd_sym(fx_t *fx)          /* -1..1, as (x + x - 1) */
{
    float u = rnd01(fx);
    return u + u - 1.0f;
}

/* FUN_0040c4c0(50.0): true half the time. */
static int rnd_half(fx_t *fx) { return rnd01(fx) * 100.0f < 50.0f; }

static float curve_at(const rb_curve_pt *pt, int n, float x)
{
    rb_curve cv;
    cv.pt = pt;
    cv.n = n;
    return rb_curve_eval(&cv, x);
}

/* FUN_005019a0: the wheel's own rate against the rate the ground implies,
   normalised by 20 rad/s and clamped to 1. The three fields it reads sit at
   phys+0x593c/0x5940/0x5944, and rb_wheel begins at phys+0x5934, so they are
   spin_w, spin_target and spin_extra.

   `rate_out` is the function's third argument: spin_extra + spin_w, the wheel's
   TOTAL angular rate. Both callers use it as well as the return value -- the
   normalised slip decides whether the wheel counts as spinning, the raw rate is
   what the intensity curve is then read at. */
static float wheel_slip(const rb_car *c, int w, float *rate_out)
{
    float rate = c->wheel[w].spin_w + c->wheel[w].spin_extra;
    float a = fabsf(rate - c->wheel[w].spin_target);

    if (rate_out)
        *rate_out = rate;
    if (a > 20.0f)
        return 1.0f;
    return a * 0.05f;
}

/* FUN_0050b6a0: |v| * 3.6. Every intensity curve here is read in km/h, which is
   also why the exhaust's 5 and 10 thresholds are reachable on a car whose top
   speed is 27. */
static float speed_kmh(const rb_car *c)
{
    return sqrtf(c->body.v[0] * c->body.v[0] + c->body.v[1] * c->body.v[1]
                 + c->body.v[2] * c->body.v[2]) * 3.6f;
}

/* The engine's surface id for a wheel, via the port's own texture-keyed grid.
   col_material_at returns a SURF_* class (pack_col.py); FX_SURF_MAP takes it to
   the id FUN_0052ee10 switches on. */
static int surf_id(const col_t *col, const float p[3])
{
    static const int map[] = FX_SURF_MAP;
    int m;

    if (!col)
        return 0;
    m = col_material_at(col, p[0], p[1], p[2]);
    if (m < 0 || m >= (int)(sizeof(map) / sizeof(map[0])))
        return 0;
    return map[m];
}

/* --------------------------------------------------------------- the pool */

static fx_particle *fx_alloc(fx_t *fx)
{
    int i;
    for (i = 0; i < FX_MAX_PARTICLES; i++)
        if (!fx->p[i].used)
            return &fx->p[i];
    return NULL;           /* 2048 is the engine's own cap; drop past it */
}

void fx_init(fx_t *fx, const scene_t *src)
{
    memset(fx, 0, sizeof(*fx));
    fx->seed = 0x1234567u;
    fx->tex = src ? scene_tex(src, FX_DUST_TEX) : 0;
    fx->enabled = (fx->tex != 0);
    fx->gas_alpha = 255.0f;
}

void fx_set_pipe(fx_t *fx, const float p[3])
{
    fx->pipe[0] = p[0];
    fx->pipe[1] = p[1];
    fx->pipe[2] = p[2];
    fx->have_pipe = 1;
}

int fx_pipe_from_rig(fx_t *fx, const carani_t *rig, int booster)
{
    char want[32];
    int i;

    if (!rig || rig->n <= 0)
        return 0;
    if (booster < 0 || booster > 3)
        booster = 0;
    snprintf(want, sizeof(want), "booster_%d_end", booster + 1);
    for (i = 0; i < rig->n; i++) {
        if (strcmp(rig->part[i].name, want))
            continue;
        {
            /* MODEL space is not BODY space, and this is the same trap that
               floated every car off the ground: the rigid body's origin is the
               centre of mass, which gen_rb_data.py parks on the wheel-centre
               plane, while the model's origin is wherever the artist left it.
               main.c reconciles them with glTranslatef(0, -wheel_plane_y, 0), so
               the same shift belongs here -- 57 mm on the Overkill, which is most
               of the height of the pipe above the road. */
            float p[3];
            p[0] = rig->part[i].rest[12];
            p[1] = rig->part[i].rest[13] - carani_wheel_plane_y(rig);
            p[2] = rig->part[i].rest[14];
            fx_set_pipe(fx, p);
        }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------- dust */

/*
 * FUN_0052e320 message 2, minus the parts that belong to the host: the camera
 * walk (one camera here) and the water branch (rb_wheel_contact.in_water).
 *
 * The two gates that shape what this looks like on screen:
 *
 *   - the surface table. surf_default's DustX_IntScale is 0, and FUN_0052e320
 *     returns as soon as it reads a scale below 1e-06 -- so asphalt, grass,
 *     metal and wood raise nothing at all. Only sand, wet sand, dune sand and
 *     the stone road do.
 *   - front or rear. On every surface EXCEPT dune sand (id 3) the block at
 *     0x0052e650 returns unless the wheel is at the back while driving forward,
 *     or at the front while reversing. Reversing then scales the rate by 0.5 at
 *     the front and 0.25 at the back.
 */
float fx_dust_rate(const fx_t *fx, const rb_car *c, int wheel, int surface,
                   float kmh)
{
    const fx_surf_t *t;
    float mult = 1.0f, in = kmh;
    float rate, slip;
    int reversing;

    (void)fx;
    if (surface < 0 || surface >= FX_SURF_N)
        surface = 0;
    t = &fx_surf[surface];
    if (t->int_scale < 1e-06f)
        return 0.0f;

    /* Forward or backward, from the body's +Z against its velocity. Row 2 of
       the row-vector matrix is the body's forward (mat4GetRow2 at 0x0052e6d1). */
    reversing = (c->m[8] * c->body.v[0] + c->m[9] * c->body.v[1]
                 + c->m[10] * c->body.v[2]) < 0.0f;

    slip = wheel_slip(c, wheel, &rate);
    if ((c->in.accel || c->in.brake) && slip > DUSTX_SPIN_THRESH) {
        /* Spinning. Two things change: the curve is read at the wheel's own
           angular RATE rather than the road speed -- which saturates the curve,
           because v/r is around 107 rad/s at top speed on a 70 mm wheel -- and
           the rate is scaled by WheelSpinScale. A wheel turning backwards
           counts as reversing whatever the body is doing. */
        in = fabsf(rate);
        mult = DUSTX_SPIN_SCALE;
        reversing = (rate < 0.0f);
    }

    if (surface != 3) {
        if (!reversing) {
            if (wheel < 2)
                return 0.0f;
        } else if (wheel >= 2) {
            return 0.0f;
        }
    }
    if (reversing)
        mult *= (wheel < 2) ? 0.5f : 0.25f;

    if (fabsf(in) <= FX_MIN_KMH)
        return 0.0f;
    return curve_at(fx_dust_int, FX_DUST_INT_N, fabsf(in))
           * mult * FX_RATE_SCALE * t->int_scale;
}

/* FUN_0052e810 message 5, for one dust particle. */
static void spawn_dust(fx_t *fx, const float pos[3], const float vel[3],
                       int wheel, int surface)
{
    const fx_surf_t *t = &fx_surf[surface];
    fx_particle *p = fx_alloc(fx);
    float speed, k;

    if (!p)
        return;
    memset(p, 0, sizeof(*p));
    p->used = 1;
    p->life = rnd_sym(fx) * t->life_disp + t->life;
    if (p->life < 1e-06f)
        p->life = 1e-06f;                   /* 0x358637bd, the no-table case */
    p->angle = rnd01(fx) * 180.0f;
    p->spin = rnd_half(fx) ? -DUSTX_TWIST : DUSTX_TWIST;
    p->x = pos[0]; p->y = pos[1]; p->z = pos[2];

    /* dust_speed is a PERCENT of the wheel's velocity (the *0.01 at
       0x0052e8ee), and below 0.25 the particle simply starts at rest. */
    speed = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
    if (speed >= 0.25f) {
        k = curve_at(fx_dust_speed, FX_DUST_SPEED_N, speed) * 0.01f;
        p->vx = vel[0] * k; p->vy = vel[1] * k; p->vz = vel[2] * k;
    }

    p->sx = DUSTX_SCALE_X + rnd_sym(fx) * DUSTX_SCALE_X_DISP;
    p->sy = DUSTX_SCALE_Y + rnd_sym(fx) * DUSTX_SCALE_Y_DISP;
    p->sx *= FX_SPRITE_UNIT;
    p->sy *= FX_SPRITE_UNIT;
    /* FUN_0052e180 picks the ramp on the WHEEL INDEX -- < 2 is the front pair. */
    p->grow = (wheel < 2) ? DUSTX_DYN_SCALE_FORW : DUSTX_DYN_SCALE;
    p->r = (unsigned char)t->r;
    p->g = (unsigned char)t->g;
    p->b = (unsigned char)t->b;
    p->a = (unsigned char)t->a;
}

/* --------------------------------------------------------------- exhaust */

/*
 * FUN_005303c0 message 2. The three phys flags it reads are already named in
 * rb.h: +0x576c is the throttle bit (in.accel), +0x5774 the brake (in.brake),
 * +0x573c the boost, and +0x577c the Jump action.
 *
 * The backfire is the part worth keeping, and its trigger is the RISING EDGE of
 * Jump above 10 km/h -- the emitter remembers last frame's bit at +0x28 and only
 * fires when it was clear and is now set. For the next ExplodeTime seconds the
 * curve is read at a flat 50 km/h, the rate doubles, particles live three times
 * as long, and the smoke darkens to ExplodeColor*255 (41 of 255, nearly black)
 * before ramping back to white at 256 per second.
 */
static float gas_rate(fx_t *fx, const rb_car *c, float kmh,
                      int *spinning, int *loaded, float dt)
{
    float in = kmh, v;

    *spinning = 0;
    *loaded = 0;
    if (fx->explode_t <= 0.0f) {
        /* The rear pair, wheels 2 and 3 -- the wheels FUN_005303c0 asks about,
           and the driven ones (carDriveForces puts the engine force at the two
           rear contacts). The louder of the two wins. */
        float r2 = 0.0f, r3 = 0.0f, rate;
        float slip2 = wheel_slip(c, 2, &r2);
        float slip3 = wheel_slip(c, 3, &r3);
        float slip = slip2 >= slip3 ? slip2 : slip3;
        rate = slip2 >= slip3 ? r2 : r3;

        if (!fx->prev_jump && c->in.jump && kmh > 10.0f) {
            fx->explode_t = EG_EXPLODE_TIME;
        } else if ((c->in.accel || c->in.brake) && slip > EG_SPIN_THRESH) {
            in = fabsf(rate) + fabsf(rate);
            *spinning = 1;
        } else if (c->in.boost && kmh > 5.0f) {
            *loaded = 1;
        }
        fx->prev_jump = c->in.jump ? 1 : 0;
    } else {
        in = 50.0f;
        fx->explode_t -= dt;
    }

    v = curve_at(fx_eg_int, FX_EG_INT_N, in);
    if (fx->explode_t > 0.0f)
        return v + v;
    if (*spinning)
        return v;
    if (in <= FX_MIN_KMH)
        return 0.0f;
    if (*loaded)
        return v * 1.5f;
    if (c->in.accel)
        return v * 0.75f;
    return 0.0f;
}

/* FUN_005303c0's tail: white at 255, darkening to ExplodeColor*255 while the
   backfire holds. Ramps are 256/s up and 512/s down. */
static void gas_colour(fx_t *fx, float dt)
{
    if (fx->explode_t <= 0.0f) {
        if (fx->gas_alpha < 255.0f) {
            fx->gas_alpha += dt * 256.0f;
            if (fx->gas_alpha > 255.0f)
                fx->gas_alpha = 255.0f;
        }
    } else {
        float floor_ = EG_EXPLODE_COLOR * 255.0f;
        if (fx->gas_alpha > floor_) {
            fx->gas_alpha -= dt * 512.0f;
            if (fx->gas_alpha < floor_)
                fx->gas_alpha = floor_;
        }
    }
}

/* FUN_005306d0 message 5. */
static void spawn_gas(fx_t *fx, const float pos[3], const float dir[3],
                      const float carv[3], int spinning, int loaded)
{
    fx_particle *p = fx_alloc(fx);
    unsigned char lum;

    if (!p)
        return;
    memset(p, 0, sizeof(*p));
    p->used = 1;
    p->life = rnd_sym(fx) * EG_LIFE_DISP + EG_LIFE;
    if (fx->explode_t > 0.0f)
        p->life *= 3.0f;
    else if (spinning)
        p->life *= 2.5f;
    else if (loaded)
        p->life *= 1.5f;
    if (p->life < 1e-06f)
        p->life = 1e-06f;

    p->angle = rnd01(fx) * 180.0f;
    p->spin = rnd_half(fx) ? -EG_TWIST : EG_TWIST;
    p->x = pos[0]; p->y = pos[1]; p->z = pos[2];
    /* Speed along the pipe, plus the car's own velocity so the puff is left
       behind rather than dragged along. */
    p->vx = EG_SPEED * dir[0] + carv[0];
    p->vy = EG_SPEED * dir[1] + carv[1];
    p->vz = EG_SPEED * dir[2] + carv[2];

    p->sx = (EG_SCALE_X + rnd_sym(fx) * EG_SCALE_X_DISP) * FX_SPRITE_UNIT;
    p->sy = (EG_SCALE_Y + rnd_sym(fx) * EG_SCALE_Y_DISP) * FX_SPRITE_UNIT;
    p->grow = EG_DYN_SCALE;
    lum = (unsigned char)fx->gas_alpha;
    p->r = p->g = p->b = lum;
    p->a = 255;
}

/* ------------------------------------------------------------------ step */

/* FUN_00530b70: n = rate*dt + carry, spawn n, keep the fraction. Dropping the
   carry loses every rate below 1/dt, which at 60 Hz is everything under 60
   particles a second -- i.e. most of the dust. */
static int emit_count(float rate, float dt, float *carry)
{
    float f;
    int n;

    if (rate < 1e-06f)
        return 0;
    f = rate * dt + *carry;
    n = (int)f;
    *carry = f - (float)n;
    return n;
}

/* FUN_0052e960 / FUN_00530800, which are the same function twice: decay the
 * sprite roll and the velocity by pow(SpeedAtt, dt), advance the position, then
 * add the system gravity.
 *
 * pow, not a linear damp: FUN_005478f0 is MSVC's _CIpow (fyl2x, and the 0x27F
 * control-word check this port already documents). And only vx and vz are
 * decayed -- the stores at 0x0052ea1c and 0x0052ea22 are +0x28 and +0x30, and
 * +0x2c is skipped. That is the original; a vertical component only ever
 * changes through gravity.
 */
static void move(fx_particle *p, float grav_y, float att, float dt)
{
    float k = powf(att, dt);
    float sign = (p->spin < 0.0f) ? -1.0f : 1.0f;

    p->angle = fabsf(p->angle * k) * sign;
    p->x += dt * p->vx;
    p->y += dt * p->vy;
    p->z += dt * p->vz;
    p->vx *= k;
    p->vz *= k;
    p->vy += dt * grav_y;
}

void fx_step(fx_t *fx, const rb_car *c, const col_t *col,
             const float eye[3], float dt)
{
    int i, w, n;

    if (!fx->enabled || dt <= 0.0f)
        return;

    /* ---- dust, one emitter per wheel ---- */
    for (w = 0; w < c->nwheels && w < RB_MAX_WHEELS; w++) {
        const rb_wheel_contact *h = &c->hit[w];
        float pos[3], vel[3], rate, dx, dy, dz;
        int sid;

        if (!h->active)
            continue;                       /* FUN_00501820 failed: no contact */
        if (h->in_water)
            continue;                       /* FUN_00531b10: spray, not dust */
        dx = h->point[0] - eye[0];
        dy = h->point[1] - eye[1];
        dz = h->point[2] - eye[2];
        if (dx * dx + dy * dy + dz * dz >= FX_EMIT_RANGE * FX_EMIT_RANGE)
            continue;

        sid = surf_id(col, h->point);
        rate = fx_dust_rate(fx, c, w, sid, speed_kmh(c));
        n = emit_count(rate, dt, &fx->carry_dust[w]);
        if (!n)
            continue;

        pos[0] = h->point[0];
        pos[1] = h->point[1];
        pos[2] = h->point[2];
        /* ShiftX: the front pair emit 6% of the way toward each other, so the
           two plumes are not exactly at the contact patches (0x0052e6a0). */
        if (w < 2) {
            const rb_wheel_contact *o = &c->hit[w ^ 1];
            if (o->active) {
                pos[0] += (o->point[0] - h->point[0]) * DUSTX_SHIFT_X;
                pos[1] += (o->point[1] - h->point[1]) * DUSTX_SHIFT_X;
                pos[2] += (o->point[2] - h->point[2]) * DUSTX_SHIFT_X;
            }
        }
        {
            float rate_w = 0.0f;
            float slip = wheel_slip(c, w, &rate_w);
            if ((c->in.accel || c->in.brake) && slip > DUSTX_SPIN_THRESH) {
                /* Spinning: the dust is thrown along the body's forward at
                   -WheelSpinSpeed times the wheel's own rate, so a wheel
                   driving forward throws it backward. It is NOT carried with
                   the car -- the emitter overwrites its velocity. */
                float k = -DUSTX_SPIN_SPEED * rate_w;
                vel[0] = c->m[8] * k;
                vel[1] = c->m[9] * k;
                vel[2] = c->m[10] * k;
            } else {
                vel[0] = c->body.v[0];
                vel[1] = c->body.v[1];
                vel[2] = c->body.v[2];
            }
        }
        while (n-- > 0)
            spawn_dust(fx, pos, vel, w, sid);
    }

    /* ---- exhaust, one emitter at the pipe ---- */
    {
        int spinning, loaded;
        float rate = gas_rate(fx, c, speed_kmh(c), &spinning, &loaded, dt);
        float pos[3], dir[3];

        gas_colour(fx, dt);
        /* FUN_0050bcc0 transforms a body-space point and vector by the car
           matrix. The point is the fitted booster's own `_end` node; the
           direction is the body's -Z, straight out the back. */
        pos[0] = fx->pipe[0] * c->m[0] + fx->pipe[1] * c->m[4]
                 + fx->pipe[2] * c->m[8] + c->m[12];
        pos[1] = fx->pipe[0] * c->m[1] + fx->pipe[1] * c->m[5]
                 + fx->pipe[2] * c->m[9] + c->m[13];
        pos[2] = fx->pipe[0] * c->m[2] + fx->pipe[1] * c->m[6]
                 + fx->pipe[2] * c->m[10] + c->m[14];
        dir[0] = -c->m[8];
        dir[1] = -c->m[9];
        dir[2] = -c->m[10];
        n = emit_count(rate, dt, &fx->carry_gas);
        while (n-- > 0)
            spawn_gas(fx, pos, dir, c->body.v, spinning, loaded);
    }

    /* ---- move and age everything ---- */
    fx->n_live = 0;
    for (i = 0; i < FX_MAX_PARTICLES; i++) {
        fx_particle *p = &fx->p[i];
        if (!p->used)
            continue;
        p->age += dt;
        if (p->age >= p->life) {
            p->used = 0;
            continue;
        }
        /* One call for both systems, which is only correct because the two
           configs agree on both numbers: car_dustx and exhausted_gas both ship
           GravityY 0 (so this term is currently nothing at all) and SpeedAtt 10,
           i.e. 0.1. They are separate sliders and a modified settings file could
           part them, at which point a particle needs to remember which system
           emitted it. */
        move(p, DUSTX_GRAVITY_Y, DUSTX_SPEED_ATT, dt);
        fx->n_live++;
    }
}

/* ------------------------------------------------------------------ draw */

/*
 * FUN_00530330 is the shorter of the two scale callbacks and the one that
 * settles which way the ramp runs:
 *
 *     k = (DynamicScale - 1) * (p[8] - p[0xc]) + 1
 *
 * with no clamp on the interpolant at all, while FUN_0052e180 wraps the same
 * expression in `if (t < 0) k = 1; else if (t <= 1) ...`. An unclamped affine
 * blend between 1 and DynamicScale is only meaningful if t is a 0..1 fraction,
 * and the guard in the other version is there for the ends of that range. So
 * t is the age fraction, k runs 1 -> DynamicScale, and the particle GROWS.
 */
static float fx_scale(const fx_particle *p)
{
    float t = p->life > 1e-09f ? p->age / p->life : 1.0f;
    if (t < 0.0f)
        return 1.0f;
    if (t > 1.0f)
        t = 1.0f;
    return (p->grow - 1.0f) * t + 1.0f;
}

void fx_draw(const fx_t *fx, const float eye[3],
             const float right[3], const float up[3])
{
    /* Two triangles per particle, positions and UVs interleaved the same way
       scene.c does it so one vertex pointer setup covers both.

       ON THE HEAP RATHER THAN IN BSS, and that is a correctness requirement, not
       a preference. vitaGL maps the newlib heap for GPU access at init
       (mem_utils.c:535) and nothing else -- the app's own image is not mapped.
       The library is now built with SAFER_DRAW_SPEEDHACK, which stops copying a
       draw's vertices into a mapped temp buffer and hands GXM the client pointer
       directly once a draw exceeds 32 KB. A full emitter here is
       2048 * 6 * 28 = 344 KB, so this buffer does cross that line, and out of
       BSS the GPU would be handed an address it cannot see. Same reasoning in
       trace.c and envmap.c; see SCENE VERTEX BUFFERS in scene.h for how the two
       halves of this change fit together. */
    static vtx_t *v;
    static unsigned char *col;
    int i, nv = 0;

    if (!fx->enabled)
        return;
    if (!v) {
        v = malloc(sizeof(*v) * FX_MAX_PARTICLES * 6);
        col = malloc((size_t)FX_MAX_PARTICLES * 6 * 4);
    }
    if (!v || !col)
        return;

    for (i = 0; i < FX_MAX_PARTICLES; i++) {
        const fx_particle *p = &fx->p[i];
        float s, cs, sn, a, ex, ey, ez, hx[3], hy[3], k;
        int j;
        static const float qu[4] = { 0.f, 1.f, 1.f, 0.f };
        static const float qv[4] = { 0.f, 0.f, 1.f, 1.f };
        static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
        float px[4], py[4], pz[4];

        if (!p->used)
            continue;
        /* ZIgnoreRad: FUN_0052e270 flags a particle within 2 m of the camera so
           it is not drawn. Without it the dust behind the car fills the screen
           the moment the chase camera closes in -- and this camera sits 0.79 m
           behind the car, well inside the radius the original was tuned for. */
        ex = p->x - eye[0];
        ey = p->y - eye[1];
        ez = p->z - eye[2];
        if (ex * ex + ey * ey + ez * ez < DUSTX_ZIGNORE_RAD * DUSTX_ZIGNORE_RAD)
            continue;
        if (nv + 6 > FX_MAX_PARTICLES * 6)
            break;

        k = fx_scale(p);
        s = p->angle * (float)(M_PI / 180.0);
        cs = cosf(s);
        sn = sinf(s);
        /* Camera-facing quad, rolled by the sprite angle. The engine hands the
           sprite to its own particle renderer, which is not transcribed, so the
           construction here is the port's: the view basis, which is what makes
           a billboard face the camera at all. */
        for (j = 0; j < 3; j++) {
            hx[j] = (right[j] * cs + up[j] * sn) * p->sx * k;
            hy[j] = (up[j] * cs - right[j] * sn) * p->sy * k;
        }
        px[0] = p->x - hx[0] - hy[0]; py[0] = p->y - hx[1] - hy[1]; pz[0] = p->z - hx[2] - hy[2];
        px[1] = p->x + hx[0] - hy[0]; py[1] = p->y + hx[1] - hy[1]; pz[1] = p->z + hx[2] - hy[2];
        px[2] = p->x + hx[0] + hy[0]; py[2] = p->y + hx[1] + hy[1]; pz[2] = p->z + hx[2] + hy[2];
        px[3] = p->x - hx[0] + hy[0]; py[3] = p->y - hx[1] + hy[1]; pz[3] = p->z - hx[2] + hy[2];

        /* Fade out over the life. The engine's own alpha ramp for these two
           systems lives in the particle renderer's per-frame alpha spline,
           which is not transcribed; a linear fade is the port's, and without
           one every puff vanishes at full opacity. */
        a = (float)p->a * (1.0f - (p->life > 1e-09f ? p->age / p->life : 1.0f));

        for (j = 0; j < 6; j++) {
            int q = tri[j];
            vtx_t *o = &v[nv];
            unsigned char *c4 = &col[nv * 4];
            o->x = px[q]; o->y = py[q]; o->z = pz[q];
            o->u = qu[q]; o->v = qv[q];
            o->lu = o->lv = 0.f;
            c4[0] = p->r; c4[1] = p->g; c4[2] = p->b;
            c4[3] = (unsigned char)(a < 0.f ? 0.f : (a > 255.f ? 255.f : a));
            nv++;
        }
    }
    if (!nv)
        return;

    glBindTexture(GL_TEXTURE_2D, fx->tex);
    /* The world's cut-out test at 0.5 would discard the whole plume. */
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnableClientState(GL_COLOR_ARRAY);

    glVertexPointer(3, GL_FLOAT, sizeof(vtx_t), &v[0].x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(vtx_t), &v[0].u);
    glColorPointer(4, GL_UNSIGNED_BYTE, 4, col);
    glDrawArrays(GL_TRIANGLES, 0, nv);

    glDisableClientState(GL_COLOR_ARRAY);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);
}
