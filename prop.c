/*
 * prop.c -- the 129 knockable props. See prop.h for where every number comes
 * from and for the four things here that are the port's rather than the game's.
 */

#include "prop.h"

#include "rlog.h"

#include <math.h>
#include <string.h>

/* --------------------------------------------------------------- helpers -- */

static void v3_set(float o[3], float a, float b, float c)
{ o[0] = a; o[1] = b; o[2] = c; }

static void v3_mad(float o[3], const float a[3], float s)
{ o[0] += a[0] * s; o[1] += a[1] * s; o[2] += a[2] * s; }

static float v3_dot(const float a[3], const float b[3])
{ return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void v3_cross(const float a[3], const float b[3], float o[3])
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

static float v3_len(const float a[3]) { return sqrtf(v3_dot(a, a)); }

/* The proxy sphere that reaches furthest from the origin -- what the inertia
   and the activity range are sized from. */
static float model_radius(const prop_model_t *pm)
{
    float best = 0.05f;
    int i;
    for (i = 0; i < pm->n_spheres; i++) {
        float d = sqrtf(pm->sphere[i][0] * pm->sphere[i][0]
                        + pm->sphere[i][1] * pm->sphere[i][1]
                        + pm->sphere[i][2] * pm->sphere[i][2])
                  + pm->sphere[i][3];
        if (d > best) best = d;
    }
    return best;
}

void prop_matrix(const prop_t *p, float out[16])
{
    const prop_model_t *pm = &PROP_MODELS[p->model];
    float up[3] = { 0.f, pm->com_y, 0.f }, wup[3];

    rb_quat_to_matrix(p->q, out);
    /* The state is on the centre of mass; the MESH is drawn about the model
       origin, which sits com_y BELOW it along the body's own up axis. Rotate the
       offset before subtracting or a tipped-over bottle draws out of its own
       collision spheres. */
    rb_mat3_mul_vec3(out, up, wup);
    out[12] = p->pos[0] - wup[0];
    out[13] = p->pos[1] - wup[1];
    out[14] = p->pos[2] - wup[2];
    out[15] = 1.f;
}

/* World position of proxy sphere `i`, and its radius. */
static float sphere_world(const prop_t *p, const prop_model_t *pm, int i,
                          const float m[16], float out[3])
{
    float local[3], rot[3];
    local[0] = pm->sphere[i][0];
    local[1] = pm->sphere[i][1] - pm->com_y;   /* relative to the COM */
    local[2] = pm->sphere[i][2];
    rb_mat3_mul_vec3(m, local, rot);
    out[0] = p->pos[0] + rot[0];
    out[1] = p->pos[1] + rot[1];
    out[2] = p->pos[2] + rot[2];
    return pm->sphere[i][3];
}

/*
 * One impulse at a world point. THE PORT'S, but it is the same expression rb.c
 * applies at a car contact (FUN_004756c0): linear straight in, angular as
 * r x j about the centre of mass.
 */
static void apply_impulse(prop_t *p, float inv_mass, float inv_inertia,
                          const float j[3], const float at[3])
{
    float r[3], t[3];
    v3_mad(p->v, j, inv_mass);
    r[0] = at[0] - p->pos[0];
    r[1] = at[1] - p->pos[1];
    r[2] = at[2] - p->pos[2];
    v3_cross(r, j, t);
    v3_mad(p->w, t, inv_inertia);
}


/*
 * The effective mass at a contact: 1/m + n . ((Iinv (r x n)) x r).
 *
 * For the isotropic tensor prop.h settles on, that whole second term reduces
 * exactly to inv_i * |r x n|^2 -- and the |r x n| is the point. The first
 * version of this used inv_i * r * r, which is only right when r is
 * PERPENDICULAR to the normal. A ball resting on the ground is the opposite
 * case: the contact is directly below the centre, r is PARALLEL to n, and the
 * angular term must vanish. It did not, so the denominator was 3.5x too large,
 * every bounce came out mushy, and the effect was worst on the objects with the
 * biggest radius -- which is why a 0.7-restitution ball bounced LESS than a
 * 0.3 can and the whole thing read backwards.
 */
static float contact_denom(float inv_m, float inv_i, const float r[3],
                           const float n[3])
{
    float rn[3];
    v3_cross(r, n, rn);
    return inv_m + inv_i * v3_dot(rn, rn);
}

/* ------------------------------------------------------------------ init -- */

void prop_init(props_t *pr, const scene_t *scene, const col_t *col, int track)
{
    int i, unresolved = 0;
    const prop_track_t *pt;

    memset(pr, 0, sizeof(*pr));
    pr->scene = scene;
    pr->col = col;
    pr->enabled = 1;
    pr->reset_track = track;

    for (i = 0; i < PROP_N_MODELS; i++)
        pr->bind[i] = scene ? scene_model_index(scene, PROP_MODELS[i].name) : -1;

    if (track < 0 || track >= (int)(sizeof(PROP_TRACKS) / sizeof(PROP_TRACKS[0])))
        return;
    pt = &PROP_TRACKS[track];

    for (i = 0; i < pt->n && pr->n < PROP_MAX_INSTANCES; i++) {
        const prop_place_t *pl = &pt->p[i];
        prop_t *p;
        if (pl->model < 0 || pl->model >= PROP_N_MODELS)
            continue;
        p = &pr->p[pr->n++];
        memset(p, 0, sizeof(*p));
        p->model = pl->model;
        /* The placement names the model ORIGIN; the state is on the COM. */
        p->pos[0] = pl->pos[0];
        p->pos[1] = pl->pos[1] + PROP_MODELS[pl->model].com_y;
        p->pos[2] = pl->pos[2];
        {
            float h = pl->yaw * 0.5f * 3.14159265358979f / 180.f;
            p->q[0] = cosf(h); p->q[1] = 0.f; p->q[2] = sinf(h); p->q[3] = 0.f;
        }
        prop_matrix(p, p->m);
    }

    /*
     * SETTLE THEM. The authored Y is a pivot that floats: measured over all ten
     * tracks, every one of the 129 placements sits ABOVE the terrain under it,
     * median 0.47 m, worst 1.64 m. That is not per-prop authoring noise, it is
     * the same convention the checkpoint markers follow (CP_GROUND in
     * checkpoint.h -- those float 0.18-0.49 m and are grounded at load too).
     *
     * Without this they hang in mid-air for the whole race, because prop_init
     * leaves a prop ASLEEP and a sleeping prop is only woken by the car. That is
     * exactly what "hitable objects float in air" was.
     *
     * Dropping them with the real solver rather than snapping the origin to the
     * ground height is what puts a bottle on its base and a cone on its rim: the
     * proxy is a sphere STACK, so where a prop comes to rest depends on which of
     * its spheres touches first and on the slope under it. Bounded, and they
     * fall asleep on their own well before the cap.
     */
    if (pr->col) {
        int step;
        for (i = 0; i < pr->n; i++)
            pr->p[i].awake = 1;
        for (step = 0; step < PROP_SETTLE_STEPS; step++)
            prop_step(pr, NULL, PROP_SETTLE_DT);
        for (i = 0; i < pr->n; i++) {
            pr->p[i].awake = 0;      /* asleep until the car finds them */
            v3_set(pr->p[i].v, 0.f, 0.f, 0.f);
            v3_set(pr->p[i].w, 0.f, 0.f, 0.f);
        }
    }

    if (scene) {
        for (i = 0; i < PROP_N_MODELS; i++)
            if (pr->bind[i] < 0) unresolved++;
    }
    rlog("[rccars] props: %d placed, %d/%d models bound%s\n", pr->n,
         PROP_N_MODELS - unresolved, PROP_N_MODELS,
         scene ? "" : " (no props.vsc -- physics only)");
    if (unresolved)
        for (i = 0; i < PROP_N_MODELS; i++)
            if (pr->bind[i] < 0)
                rlog("[rccars]   prop model '%s' is in prop_data.h and not in "
                     "props.vsc -- it will not draw\n", PROP_MODELS[i].name);
}

void prop_reset(props_t *pr)
{
    /* Cheapest correct reset: re-place from the table. Everything else about a
       props_t is derived. */
    const scene_t *s = pr->scene;
    const col_t *c = pr->col;
    int bind[PROP_N_MODELS];
    int track = pr->reset_track;
    memcpy(bind, pr->bind, sizeof(bind));
    prop_init(pr, s, c, track);
    memcpy(pr->bind, bind, sizeof(bind));
}

/* ------------------------------------------------------------------ step -- */

/* Ground contact for one proxy sphere. Returns 1 if it touched. */
static int ground_contact(props_t *pr, prop_t *p, const prop_model_t *pm,
                          const float c[3], float r, float inv_m, float inv_i)
{
    float gy, n[3] = { 0.f, 1.f, 0.f };
    float pen, at[3], rel[3], rv[3], vn, jn, t[3], tl, jt, imp[3];

    if (!pr->col)
        return 0;
    if (!col_ground_at(pr->col, c[0], c[2], c[1] + r, &gy, &n[0], &n[1], &n[2]))
        return 0;
    pen = (gy + r) - c[1];
    if (pen <= 0.f)
        return 0;

    /* Positional correction along the surface normal, applied to the body. Only
       the deepest sphere's worth per step in effect, because each one runs in
       turn and the next sees the moved position. */
    v3_mad(p->pos, n, pen);

    at[0] = c[0]; at[1] = c[1] - r; at[2] = c[2];
    rel[0] = at[0] - p->pos[0];
    rel[1] = at[1] - p->pos[1];
    rel[2] = at[2] - p->pos[2];
    v3_cross(p->w, rel, rv);
    rv[0] += p->v[0]; rv[1] += p->v[1]; rv[2] += p->v[2];

    vn = v3_dot(rv, n);
    if (vn > 0.f)
        return 1;                     /* already separating */

    /* coeffHookNorm is the restitution -- 0.7 on the balls, 0.3 on everything
       else, which is exactly the difference between a beach ball and a can. */
    jn = -(1.f + pm->restitution) * vn / contact_denom(inv_m, inv_i, rel, n);
    imp[0] = n[0] * jn; imp[1] = n[1] * jn; imp[2] = n[2] * jn;
    apply_impulse(p, inv_m, inv_i, imp, at);

    /* Tangential: coeffFrict is 1.0 on all thirteen, so this is a full Coulomb
       cone against the normal impulse just applied. */
    v3_cross(p->w, rel, rv);
    rv[0] += p->v[0]; rv[1] += p->v[1]; rv[2] += p->v[2];
    vn = v3_dot(rv, n);
    t[0] = rv[0] - n[0] * vn;
    t[1] = rv[1] - n[1] * vn;
    t[2] = rv[2] - n[2] * vn;
    tl = v3_len(t);
    if (tl > 1e-5f) {
        t[0] /= tl; t[1] /= tl; t[2] /= tl;
        jt = -tl / contact_denom(inv_m, inv_i, rel, t);
        if (jt < -pm->friction * jn)
            jt = -pm->friction * jn;
        imp[0] = t[0] * jt; imp[1] = t[1] * jt; imp[2] = t[2] * jt;
        apply_impulse(p, inv_m, inv_i, imp, at);
    }
    return 1;
}

/* The car, one-way -- see the note at the top of prop.h.
 *
 * `closing` takes the HARDEST closing speed of this call, along the contact
 * normal: it is what the sound is scaled by, and the loudest of several
 * simultaneous sphere contacts is the one that describes the knock. */
static int car_contact(prop_t *p, const prop_model_t *pm, const rb_car *car,
                       const float (*cs)[4], int ncs, const float m[16],
                       float inv_m, float inv_i, float *closing)
{
    int i, k, hit = 0;
    for (i = 0; i < pm->n_spheres; i++) {
        float pc[3], pr_ = sphere_world(p, pm, i, m, pc);
        for (k = 0; k < ncs; k++) {
            float d[3], dl, n[3], rel[3], rv[3], cv[3], cr[3], vn, jn, imp[3];
            d[0] = pc[0] - cs[k][0];
            d[1] = pc[1] - cs[k][1];
            d[2] = pc[2] - cs[k][2];
            dl = v3_len(d);
            if (dl >= pr_ + cs[k][3] || dl < 1e-6f)
                continue;
            n[0] = d[0] / dl; n[1] = d[1] / dl; n[2] = d[2] / dl;

            /* Velocity of the CAR at the contact, so a spinning car flicks a can
               the way a moving one shoves it. */
            cr[0] = cs[k][0] - car->body.x[0];
            cr[1] = cs[k][1] - car->body.x[1];
            cr[2] = cs[k][2] - car->body.x[2];
            v3_cross(car->body.w, cr, cv);
            cv[0] += car->body.v[0];
            cv[1] += car->body.v[1];
            cv[2] += car->body.v[2];

            rel[0] = pc[0] - p->pos[0];
            rel[1] = pc[1] - p->pos[1];
            rel[2] = pc[2] - p->pos[2];
            v3_cross(p->w, rel, rv);
            rv[0] += p->v[0] - cv[0];
            rv[1] += p->v[1] - cv[1];
            rv[2] += p->v[2] - cv[2];

            vn = v3_dot(rv, n);
            if (vn > 0.f)
                continue;

            /* Push it out of the car first -- without this a can wedged under a
               wheel is re-hit every substep and buzzes. */
            {
                float pen = (pr_ + cs[k][3]) - dl;
                v3_mad(p->pos, n, pen);
            }
            /* Two-body reduced mass, applied to the prop only -- see
               PROP_CAR_MASS in prop.h for why the infinite-mass form is wrong
               here even though the reaction is deliberately dropped. */
            jn = -(1.f + pm->restitution) * vn
                 * (PROP_CAR_MASS / (pm->mass + PROP_CAR_MASS))
                 / contact_denom(inv_m, inv_i, rel, n);
            imp[0] = n[0] * jn; imp[1] = n[1] * jn; imp[2] = n[2] * jn;
            apply_impulse(p, inv_m, inv_i, imp, pc);
            hit = 1;
            /* vn is negative when closing, and this runs BEFORE the impulse has
               changed anything the caller can see. */
            if (closing && -vn > *closing)
                *closing = -vn;
        }
    }
    return hit;
}

void prop_step(props_t *pr, const rb_car *car, float dt)
{
    float cs[RB_MAX_WHEELS + 16][4];
    int ncs = 0, i;
    const float *eye = car ? car->body.x : NULL;

    if (!pr->enabled || dt <= 0.f)
        return;
    if (car)
        ncs = rb_gather_spheres(car, cs);

    for (i = 0; i < pr->n; i++) {
        prop_t *p = &pr->p[i];
        const prop_model_t *pm = &PROP_MODELS[p->model];
        float rad = model_radius(pm);
        float inv_m = pm->mass > 0.f ? 1.f / pm->mass : 0.f;
        float inv_i = 1.f / (PROP_INERTIA_K * pm->mass * rad * rad);
        float qd[4], wq[4];
        float closing = 0.f;
        int touched = 0, j;

        p->touched = 0;
        p->hit = 0;
        if (eye) {
            float dx = p->pos[0] - eye[0], dz = p->pos[2] - eye[2];
            if (dx * dx + dz * dz > PROP_ACTIVE_RANGE * PROP_ACTIVE_RANGE) {
                /* Out of range is not-touching, so the edge has to be cleared
                   here too or driving away and back re-arms nothing. */
                p->was_touched = 0;
                continue;
            }
        }

        /* A sleeping prop still has to answer the car: the whole point is that
           it wakes when hit. Everything else is skipped. */
        if (!p->awake) {
            if (car && car_contact(p, pm, car, cs, ncs, p->m, inv_m, inv_i,
                                   &closing)) {
                p->awake = 1;
                p->rest_t = 0.f;
                p->touched = 1;
            } else {
                p->was_touched = 0;
                continue;
            }
        }

        v3_mad(p->v, (const float[3]){ 0.f, -PROP_GRAVITY, 0.f }, dt);
        if (pm->wind) {
            /* fWindBlow, and the tumbleweed is the only object that carries it.
               The vector is the port's -- see prop.h. */
            v3_mad(p->v, (const float[3]){ PROP_WIND_X, 0.f, PROP_WIND_Z }, dt);
        }
        v3_mad(p->pos, p->v, dt);

        /* q += dt/2 * (0, w) (x) q, then renormalise -- rb.c's own integrator. */
        wq[0] = 0.f; wq[1] = p->w[0]; wq[2] = p->w[1]; wq[3] = p->w[2];
        rb_quat_mul(wq, p->q, qd);
        for (j = 0; j < 4; j++)
            p->q[j] += 0.5f * dt * qd[j];
        rb_quat_normalize(p->q);
        prop_matrix(p, p->m);

        for (j = 0; j < pm->n_spheres; j++) {
            float c[3], r = sphere_world(p, pm, j, p->m, c);
            if (ground_contact(pr, p, pm, c, r, inv_m, inv_i))
                touched = 1;
        }
        if (car && car_contact(p, pm, car, cs, ncs, p->m, inv_m, inv_i,
                               &closing)) {
            p->touched = 1;
            p->rest_t = 0.f;
        }
        /* Rolling resistance, contact only -- see PROP_ROLL_DAMP in prop.h.
           Coulomb friction has no slip to act on once a sphere is rolling, so
           this is what stops a can accelerating down a beach forever. */
        if (touched) {
            float k = 1.f - PROP_ROLL_DAMP * dt;
            if (k < 0.f) k = 0.f;
            for (j = 0; j < 3; j++) { p->v[j] *= k; p->w[j] *= k; }
        }
        prop_matrix(p, p->m);

        /* Sleep. THE PORT'S -- without it a can on a slope creeps forever, the
           same failure the car's own rest clamp exists to stop. Contact is
           required, so a prop in mid-air can never fall asleep. */
        if (touched && v3_len(p->v) < PROP_SLEEP_V && v3_len(p->w) < PROP_SLEEP_W) {
            p->rest_t += dt;
            if (p->rest_t > PROP_SLEEP_T) {
                p->awake = 0;
                v3_set(p->v, 0.f, 0.f, 0.f);
                v3_set(p->w, 0.f, 0.f, 0.f);
            }
        } else {
            p->rest_t = 0.f;
        }

        /* The edge, last, once this step's contact state is settled. See prop.h:
           `touched` stays high while the car leans on the object, so the SOUND
           has to come off the transition, not off the state. */
        if (p->touched && !p->was_touched) {
            p->hit = 1;
            p->hit_speed = closing;
        }
        p->was_touched = p->touched;
    }
}

/* ------------------------------------------------------------------ draw -- */

/* See prop.h. */
void prop_dump(const props_t *pr, const float eye[3], float radius)
{
    int i, shown = 0;
    float zero[3] = { 0.f, 0.f, 0.f };
    if (!pr) return;
    if (!eye) eye = zero;
    rlog("[rccars] props: %d placed, %d drawn last frame, scene %s\n",
         pr->n, pr->n_drawn, pr->scene ? "loaded" : "MISSING");
    for (i = 0; i < pr->n; i++) {
        const prop_t *p = &pr->p[i];
        float dx = p->pos[0] - eye[0], dy = p->pos[1] - eye[1];
        float dz = p->pos[2] - eye[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > radius) continue;
        shown++;
        rlog("[rccars]   %-10s at (%.1f %.1f %.1f) %.1f m %s mesh=%d\n",
             PROP_MODELS[p->model].name, p->pos[0], p->pos[1], p->pos[2], d,
             p->awake ? "awake" : "asleep",
             (p->model >= 0 && p->model < PROP_N_MODELS)
                 ? pr->bind[p->model] : -1);
    }
    if (!shown)
        rlog("[rccars]   (none within %.0f m)\n", radius);
}

void prop_draw(props_t *pr, const float eye[3])
{
    int i;
    pr->n_drawn = 0;
    if (!pr->enabled || !pr->scene)
        return;
    for (i = 0; i < pr->n; i++) {
        const prop_t *p = &pr->p[i];
        int m = pr->bind[p->model];
        float dx, dz;
        if (m < 0)
            continue;
        dx = p->pos[0] - eye[0];
        dz = p->pos[2] - eye[2];
        if (dx * dx + dz * dz > PROP_ACTIVE_RANGE * PROP_ACTIVE_RANGE)
            continue;
        scene_draw_model(pr->scene, (unsigned int)m, p->m);
        pr->n_drawn++;
    }
}
