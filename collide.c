/*
 * collide.c -- RC Cars collision proxy and suspension solve, transcribed from
 * RCCars.exe. Third and last piece, after rb.c (rigid body + integrator) and
 * contact.c (tires, engine, friction).
 *
 * The thing worth understanding here is that RC Cars does NOT integrate its
 * suspension. Each wheel is a sphere on a prismatic joint along the body Y
 * axis, and the spring length is solved GEOMETRICALLY every substep: extend the
 * wheel until its sphere touches ground, or retract it until it is clear. The
 * spring/damper in rb.c then turns that length into a force.
 *
 * That is why `len`, `dlen` and `len_extra` sit in the 32-float ODE state with a
 * zero derivative. They are not integrated; they are a collision result that the
 * substep search needs to be able to roll back.
 *
 * The world queries are host callbacks (rb_world). In the original they are
 * calls into the engine's collision module -- 0x00454970 for spheres,
 * 0x004557e0 for segments, 0x00531b10 for the water probe. Keeping them as
 * callbacks is the natural seam: the Vita port has its own .col grid.
 *
 * Build with -fno-fast-math -ffp-contract=off.
 */

#include "rb.h"
#include <math.h>
#include <string.h>
#ifdef RB_DEBUG_SUSP
#include <stdio.h>
int rb_dbg_susp = 0;
const char *rb_dbg_path[RB_MAX_WHEELS];
#define DBG(i,s) do { if (rb_dbg_susp) rb_dbg_path[i] = (s); } while (0)
#else
#define DBG(i,s) do { } while (0)
#endif

#define EPS       1e-06f
#define SLOW_MPS  1.388889f    /* 5 km/h -- the original's "barely moving" test */

/* ------------------------------------------------------------------------- */
/* the wheel's world frame                                                   */
/* ------------------------------------------------------------------------- */

/* Transform a body-space point by the car matrix (row-vector convention). */
static void to_world(const rb_car *c, const float local[3], float out[3])
{
    int k;
    for (k = 0; k < 3; k++) {
        out[k] = (float)((double)c->m[0 + k] * local[0]
                         + (double)c->m[4 + k] * local[1]
                         + (double)c->m[8 + k] * local[2]
                         + c->m[12 + k]);
    }
}

/* Rotate a body-space direction into world space (no translation). */
static void dir_to_world(const rb_car *c, const float local[3], float out[3])
{
    int k;
    for (k = 0; k < 3; k++) {
        out[k] = (float)((double)c->m[0 + k] * local[0]
                         + (double)c->m[4 + k] * local[1]
                         + (double)c->m[8 + k] * local[2]);
    }
}

/* local +Z rotated about local Y by `deg`, then taken to world. */
static void steered_dir(const rb_car *c, float deg, float out[3])
{
    float local[3];
    double s = sin((double)deg * 0.017453292);
    double k = cos((double)deg * 0.017453292);
    local[0] = (float)s;
    local[1] = 0.0f;
    local[2] = (float)k;
    dir_to_world(c, local, out);
}

/* 0x004ef680 */
void rb_wheel_frame(const rb_car *c, int i, int use_extra, float centre[3],
                    float *radius, float roll[3], float side[3])
{
    const rb_wheel *wh = &c->wheel[i];
    float local[3];
    float steer;

    local[0] = wh->mount[0];
    local[1] = (float)((double)wh->mount[1] - wh->len);
    local[2] = wh->mount[2];
    if (use_extra)
        local[1] = (float)((double)local[1] - wh->len_extra);

    /* the front pair is pushed out by a per-car track offset */
    if (i == 0)      local[0] = (float)((double)local[0] + c->tune.cdt_front_x);
    else if (i == 1) local[0] = (float)((double)local[0] - c->tune.cdt_front_x);

    if (centre)
        to_world(c, local, centre);
    if (radius)
        *radius = wh->radius;

    /* only the front pair steers -- except the Hummer, whose middle axle
       steers a third as much */
    steer = 0.0f;
    if (i == 0 || i == 1)
        steer = c->steer;
    if (c->car_index == 2 && (i == 4 || i == 5))
        steer = (float)((double)c->steer * 0.33333334);

    if (roll)
        steered_dir(c, steer, roll);
    if (side) {
        float s = (i == 0 || i == 2) ? (float)((double)steer + 90.0)
                                     : (float)((double)steer - 90.0);
        steered_dir(c, s, side);
    }
}

/* ------------------------------------------------------------------------- */
/* rolling the wheels for the renderer                                       */
/* ------------------------------------------------------------------------- */

/* 0x004ef280 -- carWheelSpinUpdate. Nothing here feeds the dynamics; it exists
 * so carAniProc1 (0x00504820) has an angle to put on each wheel node.
 *
 * The rate a wheel "should" be turning at is the velocity of its CONTACT PATCH
 * -- the wheel centre dropped one radius along the body up axis, not the centre
 * itself -- resolved along the wheel's own rolling direction, over the radius.
 * The stored rate chases that, and the angle integrates the rate.
 *
 * Two deliberate divergences from the original, both noted where they happen:
 * the airborne-wheel velocity, and re-using this frame's contacts instead of
 * re-querying the world.
 */
void rb_wheel_spin_update(rb_car *c, float dt)
{
    /* Is ANY wheel on the ground? The original calls carGatherWheelContacts here
       and uses its return; the port reads the contacts the substep already
       gathered, which is the same set for the same frame and saves a full
       collision pass. */
    int grounded = 0, i;
    float up[3], speed;

    for (i = 0; i < c->nwheels; i++)
        if (c->hit[i].active) { grounded = 1; break; }

    up[0] = c->m[4]; up[1] = c->m[5]; up[2] = c->m[6];

    speed = (float)sqrt((double)c->body.v[0] * c->body.v[0]
                        + (double)c->body.v[1] * c->body.v[1]
                        + (double)c->body.v[2] * c->body.v[2]);

    for (i = 0; i < c->nwheels; i++) {
        rb_wheel *wh = &c->wheel[i];
        float centre[3], roll[3], radius, vp[3], target, rate, extra, erate;

        /* NOTE use_extra = 0 here: carWheelFrameNoExtra, not the 0x004ef9b0
           thunk the animation proc uses. */
        rb_wheel_frame(c, i, 0, centre, &radius, roll, NULL);
        centre[0] = (float)((double)centre[0] - (double)up[0] * radius);
        centre[1] = (float)((double)centre[1] - (double)up[1] * radius);
        centre[2] = (float)((double)centre[2] - (double)up[2] * radius);

        rb_point_velocity(&c->body, centre, vp);
        if (!c->hit[i].active) {
            /* The original substitutes a global vec3 (DAT_0070c658) that it
               never writes on this path -- it is an engine scratch triple shared
               by a dozen unrelated helpers. Zero is the honest stand-in: with the
               car airborne the rate is 5 rad/s², so the wheel keeps spinning
               through a jump either way. */
            vp[0] = vp[1] = vp[2] = 0.0f;
        }

        target = (float)(((double)roll[0] * vp[0] + (double)roll[1] * vp[1]
                          + (double)roll[2] * vp[2]) / radius);
        wh->spin_target = target;

        rate = grounded ? 200.0f : 5.0f;
        wh->spin_w = c->drive_blocked
                   ? 0.0f
                   : rb_move_towards(wh->spin_w, target, rate, dt);

        /* Wheelspin under power, lock-up under brakes. Off in the retail game --
           see rb_tuning.speed_ang_max_rel. */
        if (c->in.accel && c->in.throttle > 0.3f) {
            extra = c->tune.speed_ang_max_rel;
            erate = 70.0f;
        } else if (c->in.brake && c->in.brake_amount > 0.3f) {
            extra = -c->tune.speed_ang_max_rel;
            erate = 70.0f;
        } else {
            extra = 0.0f;
            erate = 35.0f;
        }
        if (grounded)
            extra *= (speed <= 4.0f) ? (float)(1.0 - (double)speed * 0.25) : 0.0f;

        wh->spin_extra = c->drive_blocked
                       ? 0.0f
                       : rb_move_towards(wh->spin_extra, extra, erate, dt);

        wh->spin = (float)((double)wh->spin
                           + ((double)wh->spin_extra + wh->spin_w) * dt);
        /* wrap into [-2pi, 2pi) -- 0x0040c6d0 with floor, so it works in both
           directions and a reversing wheel does not stick at the low end */
        {
            double range = 2.0 * RB_TWO_PI;
            wh->spin = (float)((double)wh->spin
                               - floor(((double)wh->spin + RB_TWO_PI) / range) * range);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* the collision proxy                                                       */
/* ------------------------------------------------------------------------- */

/* 0x004ef9e0 -- one sphere per wheel, then three body spheres on the centre
 * line and the same three offset to each side. A four-wheel car is 13 spheres;
 * the Hummer is 15.
 *
 * That count was aspirational until the roof stations landed, and the gap was a
 * real bug rather than a detail. Slots 1 and 2 come from .crs keys no shipped
 * car carries, the loop below skips a radius-0 slot, and so a retail car really
 * did gather 7 spheres and 9 -- everything the body had was ONE lateral triplet
 * at ONE Z station, with nothing at the roof or either end. Right way up that is
 * invisible, because the wheels carry the car. On its roof it meant the shell
 * had nothing to rest on and sank into the terrain up to the wheel arches: 97 mm
 * of Overkill, 64 of Buggy, 104 of Hummer, against shells 111-157 mm tall.
 * gen_rb_data.py now fits slots 1 and 2 to ENVIR_CAR_BODY's own roof, so the
 * counts above are what actually gets built. rccars_re/flipped.c is the check.
 */
int rb_gather_spheres(const rb_car *c, float out[][4])
{
    int n = 0, i, k, pass;

    for (i = 0; i < c->nwheels; i++) {
        float p[3], r;
        rb_wheel_frame(c, i, 0, p, &r, 0, 0);
        out[n][0] = p[0]; out[n][1] = p[1]; out[n][2] = p[2];
        out[n][3] = r;
        n++;
    }

    /* pass 0: centre line, pass 1: +side_x, pass 2: -side_x */
    for (pass = 0; pass < 3; pass++) {
        for (k = 0; k < 3; k++) {
            const rb_body_sphere *bs = &c->tune.body_sphere[k];
            float local[3], p[3];

            /* A radius of 0 means this car's .crs carries no keys for the
               sphere. The original would fall back to the loader's clamp
               default; that value is unknown, and a sphere left at the body
               origin would collide with the ground permanently, so skip it. */
            if (bs->radius <= 0.0f)
                continue;

            local[0] = bs->offset[0];
            local[1] = bs->offset[1];
            local[2] = bs->offset[2];

            if (pass == 0) {
                /* the Buggy shifts these spheres along Z */
                if (c->car_index == 1)
                    local[2] = (float)((double)local[2] + bs->central_z);
            } else if (pass == 1) {
                local[0] = (float)((double)local[0] + c->tune.cdt_side_x);
            } else {
                local[0] = (float)((double)local[0] - c->tune.cdt_side_x);
            }

            to_world(c, local, p);
            out[n][0] = p[0]; out[n][1] = p[1]; out[n][2] = p[2];
            out[n][3] = bs->radius;
            n++;
        }
    }
    return n;
}

/* 0x004efe00 -- query every sphere and turn the results into per-wheel
 * contacts.
 *
 * For each hit, the contact normal points from the surface point back toward the
 * sphere centre, and the contact point is placed on the sphere at
 * `centre - normal * radius`. That is what carContactRecord and the suspension
 * force then consume.
 *
 * `tol` is added to every radius, and it is LOAD-BEARING. carPhysTick gathers
 * the wheel contacts with tol = 0.006 (verified at the call site, 0x4f5fc9),
 * because suspRetract deliberately leaves the wheel clear of the surface by
 * radius*0.01. With tol = 0 the gather finds nothing and the car free-falls
 * through its own suspension.
 *
 * `opaque` is the original's second argument: it passes the rigid-body pointer
 * through into the wheel record and the dynamics never reads it. Kept only so
 * the signature matches.
 */
int rb_collide(rb_car *c, float opaque, float tol, int mode, int limit,
               rb_wheel_contact *hit_out)
{
    float spheres[RB_MAX_SPHERES][4];
    rb_world_hit hits[8];
    int nspheres, i, h, nh;
    int found = 0;

    (void)opaque;

    if (!c->world || !c->world->sphere)
        return 0;

    if (limit < 0 || limit > c->nwheels)
        limit = c->nwheels;

    if (hit_out)
        memset(hit_out, 0, RB_MAX_WHEELS * sizeof(rb_wheel_contact));

    nspheres = rb_gather_spheres(c, spheres);

    for (i = 0; i < nspheres; i++) {
        float radius = (float)((double)spheres[i][3] + tol);

        /* mode 0: wheels only; 1: body only; 2: everything */
        if (mode == 0 && i > limit - 1)
            continue;
        if (mode == 1 && i < c->nwheels)
            continue;

        nh = 0;
        if (!c->world->sphere(c->world->ctx, spheres[i], radius,
                              hits, (int)(sizeof(hits) / sizeof(hits[0])), &nh))
            continue;
        found = 1;

        for (h = 0; h < nh; h++) {
            float nrm[3];
            double len;
            int k;

            for (k = 0; k < 3; k++)
                nrm[k] = (float)((double)spheres[i][k] - hits[h].point[k]);
            len = sqrt((double)nrm[0] * nrm[0] + (double)nrm[1] * nrm[1]
                       + (double)nrm[2] * nrm[2]);
            if (len < EPS) {
                nrm[0] = 0.0f; nrm[1] = 1.0f; nrm[2] = 0.0f;
            } else if (fabs(len - 1.0) >= EPS) {
                double inv = 1.0 / len;
                nrm[0] = (float)(inv * nrm[0]);
                nrm[1] = (float)(nrm[1] * inv);
                nrm[2] = (float)(nrm[2] * inv);
            }

            /* only wheel spheres produce per-wheel contacts */
            if (hit_out && i < limit && i < c->nwheels) {
                rb_wheel_contact *w = &hit_out[i];
                if (!w->active) {
                    w->active = 1;
                    for (k = 0; k < 3; k++) {
                        w->normal[k] = nrm[k];
                        w->point[k] = (float)((double)spheres[i][k]
                                              - (double)radius * nrm[k]);
                    }
                    w->surface = hits[h].surface;
                    w->in_water = 0;
                    w->water_gap = 0.0f;
                    if (c->world->water
                        && c->world->water(c->world->ctx, i, spheres[i],
                                           &w->water_gap)) {
                        /* The probe answers "this COLUMN has water", because a
                           per-cell surface height cannot say more. The wheel is
                           only IN it once its sphere reaches the surface -- the
                           same threshold carSurfaceDrag applies for itself at
                           contact.c's `radius <= water_gap`.

                           Without the gate a wooden deck over the sea reads as
                           water. beach_1's pier is 46 collision floors at
                           y 3.4..5.2 standing over cells whose water height is
                           0.00, so all four wheels came back in_water three
                           metres up: the water surface LOOP instead of
                           car_surf_wood, no dust and no tyre marks, for the
                           whole length of the pier. */
                        w->in_water = w->water_gap < c->wheel[i].radius;
                    }
                }
            }
        }
    }
    return found;
}

/* ------------------------------------------------------------------------- */
/* the collision-contact solve                                               */
/* ------------------------------------------------------------------------- */

/* 0x004efe00 with the contact list kept. Identical gather to rb_collide above --
 * the only difference is where the results go. */
int rb_coll_list(rb_car *c, float tol, int mode, rb_coll_contact *out, int max)
{
    float spheres[RB_MAX_SPHERES][4];
    rb_world_hit hits[8];
    int nspheres, i, h, nh, n = 0;

    if (!c->world || !c->world->sphere || max <= 0)
        return 0;

    nspheres = rb_gather_spheres(c, spheres);

    for (i = 0; i < nspheres; i++) {
        float radius = (float)((double)spheres[i][3] + tol);

        /* mode 0: wheels only; 1: body only; 2: everything */
        if (mode == 0 && i >= c->nwheels)
            continue;
        if (mode == 1 && i < c->nwheels)
            continue;
        if (spheres[i][3] <= 0.0f)
            continue;

        nh = 0;
        /* Cleared because the normal below is OPTIONAL: a host that predates it
           -- every stub world in the harnesses -- fills only `point`, and reading
           an uninitialised stack normal would pick a separating direction out of
           whatever was on the stack. All-zero is the documented "cannot say". */
        memset(hits, 0, sizeof(hits));
        if (!c->world->sphere(c->world->ctx, spheres[i], radius, hits,
                              (int)(sizeof(hits) / sizeof(hits[0])), &nh))
            continue;

        for (h = 0; h < nh && n < max; h++) {
            float nrm[3];
            double len;
            int k;

            for (k = 0; k < 3; k++)
                nrm[k] = (float)((double)spheres[i][k] - hits[h].point[k]);
            len = sqrt((double)nrm[0] * nrm[0] + (double)nrm[1] * nrm[1]
                       + (double)nrm[2] * nrm[2]);

            /* DIVERGENCE, and a necessary one. The recovered normal is
             * normalise(centre - surfacePoint), which is the separating direction
             * ONLY while the sphere centre is still outside the surface. Push the
             * centre through and the very same formula points INTO the ground --
             * and the contact solve below, whose whole job is to drive the relative
             * normal velocity to +0.05 m/s ALONG the normal, then becomes a pump
             * that walks the car out of the bottom of the world at a steady
             * 5 cm/s. Measured on the inverted Hummer: it sank from y = +0.04 to
             * -174 m without ever exceeding 0.06 m/s.
             *
             * The original never reaches that state, because carSubstepContact
             * bisects the substep back to the touching moment and a centre is
             * therefore never below the surface when the solve runs. Lacking that
             * bisection (see rb_body_depenetrate), detect the state and push out
             * the front of the face instead.
             *
             * WHICH SIDE the centre is on is the whole question, and "is it below
             * the hit point" is NOT the way to ask it. That test was written for a
             * downward-ray host and this host is not one: col_sphere is a genuine
             * closest-point query and it returns CEILINGS. A body sphere under a
             * low roof sits below its contact point with no penetration at all,
             * the test called that "sunk", and the solve then drove the car up
             * into the roof at +0.05 m/s of "separation" -- reported from the game
             * as flipping and smashing through the upper wall in the tight spots,
             * and reproduced in rccars_re/ceiling.c, where every ceiling contact
             * came back with normal (0, +1, 0) and the car cleared a 0.44 m roof
             * in a single step.
             *
             * The surface's own outward normal answers it properly:
             * dot(centre - point, faceN) > 0 means the centre is in the open
             * space in FRONT of the face, whichever way that face happens to
             * point. Behind it, faceN itself is the way out -- which for ground
             * is up (what this always did, and now correctly along a slope) and
             * for a ceiling is down. A host that supplies no normal keeps the old
             * behaviour exactly. */
            {
                const float *fn = hits[h].normal;
                int have_fn = (fn[0] != 0.0f || fn[1] != 0.0f || fn[2] != 0.0f);
                double side = have_fn
                    ? (double)nrm[0]*fn[0] + (double)nrm[1]*fn[1]
                      + (double)nrm[2]*fn[2]
                    : (double)spheres[i][1] - hits[h].point[1];

                if (len < EPS || side <= 0.0) {
                    if (have_fn) {
                        nrm[0] = fn[0]; nrm[1] = fn[1]; nrm[2] = fn[2];
                    } else {
                        nrm[0] = 0.0f; nrm[1] = 1.0f; nrm[2] = 0.0f;
                    }
                } else if (fabs(len - 1.0) >= EPS) {
                    double inv = 1.0 / len;
                    for (k = 0; k < 3; k++)
                        nrm[k] = (float)(inv * nrm[k]);
                }
            }

            for (k = 0; k < 3; k++) {
                out[n].normal[k] = nrm[k];
                out[n].point[k] = (float)((double)spheres[i][k]
                                          - (double)radius * nrm[k]);
            }
            out[n].is_wheel = (i < c->nwheels);
            n++;
        }
        if (n >= max)
            break;
    }
    return n;
}

/* 0x004754a0 -- k = 1/m + n . ((Iinv (r x n)) x r). See rb.h. */
double rb_impulse_denom(rb_car *c, const float point[3], const float n[3])
{
    double r[3], rxn[3], t[3], cr[3];
    float rxnf[3], tf[3];
    int k;

    for (k = 0; k < 3; k++)
        r[k] = (double)point[k] - c->body.x[k];

    rxn[0] = r[1] * n[2] - r[2] * n[1];
    rxn[1] = r[2] * n[0] - r[0] * n[2];
    rxn[2] = r[0] * n[1] - r[1] * n[0];

    /* The original refreshes Iinv from the quaternion before using it. */
    rb_update_inv_inertia_world(&c->body);
    for (k = 0; k < 3; k++)
        rxnf[k] = (float)rxn[k];
    rb_mat3_mul_vec3(c->body.iinv, rxnf, tf);
    for (k = 0; k < 3; k++)
        t[k] = tf[k];

    cr[0] = t[1] * r[2] - t[2] * r[1];
    cr[1] = t[2] * r[0] - t[0] * r[2];
    cr[2] = t[0] * r[1] - t[1] * r[0];

    return cr[0] * n[0] + cr[1] * n[1] + cr[2] * n[2]
           + (double)c->body.inv_mass;
}

/* 0x004756c0 -- P += j, L += r x j, then rederive v and w. */
void rb_apply_impulse(rb_car *c, const float point[3], const float j[3])
{
    double r[3];
    int k;

    for (k = 0; k < 3; k++)
        r[k] = (double)point[k] - c->body.x[k];

    for (k = 0; k < 3; k++)
        c->body.P[k] = (float)((double)c->body.P[k] + j[k]);

    c->body.L[0] = (float)((double)c->body.L[0] + (r[1]*j[2] - r[2]*j[1]));
    c->body.L[1] = (float)((double)c->body.L[1] + (r[2]*j[0] - r[0]*j[2]));
    c->body.L[2] = (float)((double)c->body.L[2] + (r[0]*j[1] - r[1]*j[0]));

    /* 0x004757a0 with dt = 0: no force integration, just the derived pair. */
    for (k = 0; k < 3; k++)
        c->body.v[k] = (float)((double)c->body.inv_mass * c->body.P[k]);
    rb_update_inv_inertia_world(&c->body);
    rb_mat3_mul_vec3(c->body.iinv, c->body.L, c->body.w);
}

/* 0x004f0840, single-body path: turn a desired change in relative normal
 * velocity into an impulse and apply it. The original substitutes a zero triple
 * when the denominator is degenerate. */
static void coll_apply(rb_car *c, const rb_coll_contact *rec, double dv)
{
    double k = rb_impulse_denom(c, rec->point, rec->normal);
    float j[3];
    double s;
    int i;

    if (fabs(k) < 1e-06) {
        j[0] = j[1] = j[2] = 0.0f;
    } else {
        s = dv / k;
        for (i = 0; i < 3; i++)
            j[i] = (float)(s * rec->normal[i]);
    }
    rb_apply_impulse(c, rec->point, j);
}

/* 0x004f0560 -- see rb.h. The original takes the first non-wheel contact with a
 * tangential component and returns; the loop only skips wheels and contacts that
 * are already stationary in the surface plane. */
void rb_coll_friction(rb_car *c, int n, const rb_coll_contact *rec, float dt)
{
    int i;

    if (n <= 0 || !rec)
        return;

    for (i = 0; i < n; i++) {
        float vp[3], j[3];
        double vn, vt[3], mag, lim, s;
        int k;

        if (rec[i].is_wheel)
            continue;

        rb_point_velocity(&c->body, rec[i].point, vp);
        vn = (double)vp[0] * rec[i].normal[0] + (double)vp[1] * rec[i].normal[1]
             + (double)vp[2] * rec[i].normal[2];
        for (k = 0; k < 3; k++)
            vt[k] = (double)vp[k] - vn * rec[i].normal[k];

        mag = sqrt(vt[0]*vt[0] + vt[1]*vt[1] + vt[2]*vt[2]);
        if (mag < 1e-06)
            continue;

        /* dt*10 -- the original clamps the tangential speed against it directly,
           so the impulse is at most dt*10 N.s, i.e. a 10 N scrape. Units are
           mixed in the original (a speed against an impulse bound) and that is
           reproduced as written. */
        lim = (double)dt * 10.0;
        if (lim < 0.0)
            lim = 0.0;
        s = (mag < lim) ? mag : lim;

        for (k = 0; k < 3; k++)
            j[k] = (float)(-s * (vt[k] / mag));

        rb_apply_impulse(c, rec[i].point, j);
        return;                       /* one contact only -- 0x4f0712 */
    }
}

/* 0x004f0750 -- see rb.h. */
int rb_coll_resolve(rb_car *c, int n, const rb_coll_contact *rec)
{
    int iter = 0, any = 1, i;

    if (n <= 0 || !rec)
        return 1;

    while (any) {
        any = 0;
        for (i = 0; i < n; i++) {
            float vp[3];
            double vrel, dv;

            /* 0x004f0460: relative velocity along the contact normal. */
            rb_point_velocity(&c->body, rec[i].point, vp);
            vrel = (double)vp[0] * rec[i].normal[0]
                   + (double)vp[1] * rec[i].normal[1]
                   + (double)vp[2] * rec[i].normal[2];

            if (vrel > 0.02)
                continue;             /* separating fast enough already */

            any = 1;
            dv = 0.05 - vrel;
            if (dv < 0.0)
                dv = 0.0;
            coll_apply(c, &rec[i], dv);
        }
        iter++;
        if (iter > 9)
            return 0;
    }
    return 1;
}

/* PARTLY TRANSCRIBED -- the positional half only. See rb.h.
 *
 * Only the BODY spheres are considered: the wheels have the suspension, which is
 * the transcribed path and must stay in charge of them.
 */
int rb_body_depenetrate(rb_car *c)
{
    float spheres[RB_MAX_SPHERES][4];
    rb_world_hit hits[8];
    int n, i, h, nh, k;
    double worst = 0.0, worst_radius = 0.0, worst_nrm[3] = { 0.0, 1.0, 0.0 };

    if (!c->world || !c->world->sphere)
        return 0;

    n = rb_gather_spheres(c, spheres);

    for (i = c->nwheels; i < n; i++) {
        float radius = spheres[i][3];

        if (radius <= 0.0f)
            continue;

        nh = 0;
        memset(hits, 0, sizeof(hits));   /* see rb_coll_list */
        if (!c->world->sphere(c->world->ctx, spheres[i], radius, hits,
                              (int)(sizeof(hits) / sizeof(hits[0])), &nh))
            continue;

        for (h = 0; h < nh; h++) {
            double nrm[3], len, depth;

            for (k = 0; k < 3; k++)
                nrm[k] = (double)spheres[i][k] - hits[h].point[k];
            len = sqrt(nrm[0]*nrm[0] + nrm[1]*nrm[1] + nrm[2]*nrm[2]);

            /* Dead centre, or a centre that has passed through the surface: no
               usable direction from the raw vector, and following it in the
               second case drives the car further in. Same test and same reasoning
               as in rb_coll_list -- see the long note there, including why "below
               the hit point" was the wrong question and what a ceiling did to it.
               This site is the more violent of the two: it moves the body
               POSITIONALLY, so a ceiling contact did not pump the car through the
               roof over several frames, it teleported it through in one. */
            {
                const float *fn = hits[h].normal;
                int have_fn = (fn[0] != 0.0f || fn[1] != 0.0f || fn[2] != 0.0f);
                double side = have_fn
                    ? nrm[0]*fn[0] + nrm[1]*fn[1] + nrm[2]*fn[2]
                    : (double)spheres[i][1] - hits[h].point[1];

                if (len < EPS || side <= 0.0) {
                    if (have_fn) {
                        for (k = 0; k < 3; k++) nrm[k] = fn[k];
                        /* `side` is the centre's signed distance in front of the
                           face; it wants to end up a full radius in front. */
                        depth = (double)radius - side;
                    } else {
                        nrm[0] = 0.0; nrm[1] = 1.0; nrm[2] = 0.0;
                        depth = radius + ((double)hits[h].point[1] - spheres[i][1]);
                    }
                } else {
                    for (k = 0; k < 3; k++) nrm[k] /= len;
                    depth = (double)radius - len;
                }
            }
            if (depth <= worst)
                continue;

            worst = depth;
            worst_radius = radius;
            for (k = 0; k < 3; k++)
                worst_nrm[k] = nrm[k];
        }
    }

    if (worst <= 0.0)
        return 0;

    /* The deepest sphere only, and by its own depth plus the radius*0.01 margin
       suspRetract leaves -- a sphere resting at exactly zero clearance makes the
       overlap test flip on and off under sub-millimetre motion. Position only:
       the velocity is the contact solve's job now. */
    worst += worst_radius * 0.01;
    for (k = 0; k < 3; k++)
        c->body.x[k] = (float)((double)c->body.x[k] + worst_nrm[k] * worst);
    rb_car_update_matrix(c);
    return 1;
}

/* 0x004fbe60 -- len_extra[i] = radius[i] * 0.02. See rb.h. */
void rb_susp_len_extra(rb_car *c)
{
    int i;

    for (i = 0; i < c->nwheels; i++)
        c->wheel[i].len_extra = (float)((double)c->wheel[i].radius * 0.02);
}

/* ------------------------------------------------------------------------- */
/* the rest clamp                                                            */
/* ------------------------------------------------------------------------- */

/* Clamp a 3-vector's length to `lim`, the original's way round: normalise first
   (skipping it when the length is already 1), then scale. */
static void clamp_len(float v[3], double lim)
{
    double mag = sqrt((double)v[0]*v[0] + (double)v[1]*v[1] + (double)v[2]*v[2]);
    int k;

    if (mag <= lim)
        return;
    if (mag >= 1e-06 && fabs(mag - 1.0) >= 1e-06) {
        double inv = 1.0 / mag;
        for (k = 0; k < 3; k++)
            v[k] = (float)(inv * v[k]);
    }
    for (k = 0; k < 3; k++)
        v[k] = (float)(lim * v[k]);
}

/* 0x004f5770 -- see rb.h. */
void rb_clamp_momentum(rb_car *c)
{
    clamp_len(c->body.P, (double)RB_MAX_IMP_LINEAR * c->body.mass);
    clamp_len(c->body.L, (double)RB_MAX_IMP_ANG);
}

/* 0x004f59a0 -- see rb.h. */
void rb_car_rest_update(rb_car *c, float dt)
{
    double sv, sp, pick, sw;

    /* The original tests five input bits -- 0x575c, 0x5764, 0x576c (accel),
       0x5774 (brake) and 0x577c. 0x577c is the JUMP action (see rb_car_jump),
       and it matters here for the obvious reason: a car the rest clamp has put
       to sleep is skipping its whole physics step, so unless pressing Jump wakes
       it the hop impulse would be zeroed again before anything integrated it.
       The one bit still unmapped is 0x575c, which cannot move a stationary car
       on its own -- the effect of leaving it out is that a car being steered and
       nothing else is allowed to fall asleep. */
    /* boost_button, not boost: this asks what the driver is holding, and
       in.boost is now the meter's verdict -- an empty meter must not be able to
       let a car the driver is still leaning on fall asleep. */
    if (c->in.accel || c->in.brake || c->in.boost_button || c->in.blocked
        || c->in.jump) {
        c->rest_slow_t   = 0.0f;
        c->rest_spin_t   = 0.0f;
        c->no_contact_t  = 1999872.0f;      /* 0x49f42400 */
        c->rest_ground_t = 0.0f;
    }

    sv = sqrt((double)c->body.v[0]*c->body.v[0]
              + (double)c->body.v[1]*c->body.v[1]
              + (double)c->body.v[2]*c->body.v[2]);
    sp = sqrt((double)c->body.P[0]*c->body.P[0]
              + (double)c->body.P[1]*c->body.P[1]
              + (double)c->body.P[2]*c->body.P[2]);
    pick = (sv <= sp) ? sp : sv;
    if (pick >= 0.3611111)
        c->rest_slow_t = 0.0f;
    else
        c->rest_slow_t = (float)((double)c->rest_slow_t + dt);

    sw = sqrt((double)c->body.w[0]*c->body.w[0]
              + (double)c->body.w[1]*c->body.w[1]
              + (double)c->body.w[2]*c->body.w[2]);
    if (sw >= 1.0)
        c->rest_spin_t = 0.0f;
    else
        c->rest_spin_t = (float)((double)c->rest_spin_t + dt);

    c->no_contact_t = (float)((double)c->no_contact_t + dt);
    if (c->no_contact_t < 0.1f)
        c->rest_ground_t = (float)((double)c->rest_ground_t + dt);
    else
        c->rest_ground_t = 0.0f;
}

/* 0x004f5980 */
void rb_car_rest_touch(rb_car *c)
{
    c->no_contact_t = 0.0f;
}

/* 0x004f6610 -- see rb.h. Zeroing the momenta is a SIDE EFFECT of the test, and
   the caller skips the whole step when it returns nonzero. */
int rb_car_at_rest(rb_car *c)
{
    int k;

    if (!(c->rest_slow_t > 2.0f && c->rest_spin_t > 2.0f
          && c->rest_ground_t > 1.0f))
        return 0;

    for (k = 0; k < 3; k++) {
        c->body.P[k] = 0.0f;
        c->body.v[k] = 0.0f;
        c->body.L[k] = 0.0f;
        c->body.w[k] = 0.0f;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* the suspension solve                                                      */
/* ------------------------------------------------------------------------- */

/* Is this wheel's sphere touching anything, and by how much?
 *
 * ONE query answers both, and it has to: the retract loop below asks "touching?"
 * and then "how deep?" about the same sphere at the same instant, and those used
 * to be two separate wheel_touching/wheel_overlap calls into the world -- the
 * identical query, run twice, up to five passes per wheel per suspension solve,
 * twice per substep. Against beach_1's dense cells that was half of a 110-query
 * tick. Nothing moves between the two, so merging them is exact.
 *
 * `depth_out` may be NULL when only the flag is wanted. Depth is 0 for a wheel
 * resting exactly tangent, which is still touching -- rb_susp_retract depends on
 * that distinction, so the return value and the depth are NOT interchangeable. */
static int wheel_probe(const rb_car *c, int i, double *depth_out)
{
    float p[3], r;
    rb_world_hit hits[8];
    int nh = 0, h, touching;
    double worst = 0.0;

    if (depth_out)
        *depth_out = 0.0;
    if (!c->world || !c->world->sphere)
        return 0;
    rb_wheel_frame(c, i, 0, p, &r, 0, 0);
    touching = c->world->sphere(c->world->ctx, p, r, hits,
                                (int)(sizeof(hits) / sizeof(hits[0])), &nh);
    if (!touching || !depth_out)
        return touching;
    for (h = 0; h < nh; h++) {
        double dx = (double)p[0] - hits[h].point[0];
        double dy = (double)p[1] - hits[h].point[1];
        double dz = (double)p[2] - hits[h].point[2];
        double d = (double)r - sqrt(dx * dx + dy * dy + dz * dz);
        if (d > worst)
            worst = d;
    }
    *depth_out = worst;
    return touching;
}

/* 0x004fbd70 -- is the wheel buried? A ray straight up from the wheel centre by
 * 1.5 radii, then a fat sphere test at the far end.
 *
 * DIVERGENCE, and it is important. Those two probes alone are far too eager: on a
 * slope a wheel resting only a few centimetres low trips the fat sphere, and
 * `embedded` is not per-wheel -- carUpdateSuspension applies it to the WHOLE car,
 * putting every strut on the "push it out" branch instead of resolving contact
 * normally. One slightly sunk wheel then stops the suspension working at all, and
 * the car tumbles. The original's probes go through the engine's collision module
 * with its own semantics, which are not transcribed.
 *
 * So this additionally requires the wheel's own sphere to be deeply overlapped --
 * more than half a radius, i.e. past what suspRetract could resolve in one pass.
 * That keeps the original's intent (the wheel is genuinely inside geometry)
 * without firing for a wheel that is merely riding low.
 */
int rb_wheel_buried(const rb_car *c, int i)
{
    float p[3], r, up[3], far[3];
    rb_world_hit hits[8];
    int nh = 0, k;

    if (!c->world)
        return 0;

    {
        double deep;
        wheel_probe(c, i, &deep);
        if (deep <= (double)c->wheel[i].radius * 0.5)
            return 0;
    }

    rb_wheel_frame(c, i, 0, p, &r, 0, 0);
    up[0] = 0.0f; up[1] = 1.0f; up[2] = 0.0f;
    for (k = 0; k < 3; k++)
        far[k] = (float)((double)up[k] * (r * 1.5) + p[k]);

    if (c->world->segment && c->world->segment(c->world->ctx, p, far))
        return 1;
    if (c->world->sphere
        && c->world->sphere(c->world->ctx, far, (float)((double)r * 1.5),
                            hits, (int)(sizeof(hits) / sizeof(hits[0])), &nh))
        return 1;
    return 0;
}

/* 0x004fb9e0 -- retract the spring until the wheel sphere is clear, by at most
 * `max_step` and in at most 5 passes. Returns nonzero on success; on ANY failure
 * it restores the original length and returns 0, and the caller treats that as
 * "stuck".
 *
 * Failure cases, all of which restore and return 0:
 *   - the wheel has already bottomed out (len below lenMin): the bump stop
 *   - resolving would need more than max_step
 *   - five passes were not enough
 *
 * Note it retracts by the penetration depth PLUS radius*0.01, so a resolved
 * wheel ends up slightly clear of the surface. That is why the per-wheel contact
 * gather needs RB_CONTACT_TOL.
 *
 * The original resolves the depth by intersecting a ray along the body up axis
 * against the contact sphere; here the host's query reports the overlap
 * directly, which is the same quantity by a shorter route.
 */
int rb_susp_retract(rb_car *c, int i, float max_step)
{
    rb_wheel *wh = &c->wheel[i];
    float len0 = wh->len;
    double total = 0.0;
    int pass;

    if ((double)max_step > (double)wh->radius - EPS)
        return 0;

    for (pass = 0; pass < 5; pass++) {
        double depth;

        /* One probe, both answers -- see wheel_probe. This loop is the largest
           single source of world queries in the sim: up to five passes, per
           wheel, twice per substep, four substeps per tick. */
        if (!wheel_probe(c, i, &depth))
            return 1;                     /* clear -- keep the retracted length */
        if ((double)wh->len < (double)wh->len_min + 0.0001)
            break;                        /* bottomed out */

        /* A wheel resting exactly tangent has zero overlap, and it still has to
           be pushed clear -- otherwise the solid penetration test fires forever
           and the substep search can never advance time. That is what the
           radius*0.01 term below is for, so do NOT skip on depth == 0. */
        if (depth < 0.0)
            depth = 0.0;

        total += depth;
        if ((double)max_step < total)
            break;                        /* over budget */

        wh->len = (float)(((double)len0 - total) - (double)wh->radius * 0.01);
        if (wh->len < wh->len_min)
            wh->len = wh->len_min;
    }
    wh->len = len0;
    return 0;
}

/* 0x004fbc50 -- extend by `rate`, clamped to 0.99 radius and to lenMax, and
 * keep the result only if the extended wheel actually finds ground. */
void rb_susp_extend(rb_car *c, int i, float rate)
{
    rb_wheel *wh = &c->wheel[i];
    float len0 = wh->len;
    float step, cap;

    if ((double)wh->radius < rate)
        rate = (float)((double)wh->radius * 0.99);
    cap = (float)((double)wh->radius * 0.99);
    step = (rate < cap) ? rate : cap;

    wh->len = (float)((double)step + wh->len);
    if (wh->len_max < wh->len)
        wh->len = wh->len_max;

    if (rb_susp_retract(c, i, cap)) {
        if (wh->len < len0)
            wh->len = len0;
        return;
    }
    wh->len = len0;
}

/* The `phys + 0x08` extension-rate ramp -- both halves, at last.
 *
 * carUpdateSuspension only ever RESETS this value. The writer is FUN_004f6ea0,
 * the engine's per-frame car loop, at 0x004f6fd8: `phys+0x08 += dt`, alongside
 * half a dozen other frame timers. So it is simply the TIME since the suspension
 * was last retracted at low speed, and the consumer's range check says what that
 * time buys:
 *
 *     if (t >= 0.0 && t <= 0.5)  rate += 2*t*(fast - rate);   else  rate = fast;
 *
 * so 0 is the slow rate, 0.5 is the fast one, and anything OUTSIDE [0, 0.5] is
 * also the fast one. It ramps 1x -> 10x over half a second of not being reset:
 * a parked car extends slowly, a car that has been moving for 0.5 s extends ten
 * times faster. rb_susp_ramp_advance is the writer's half; this is the reset.
 *
 * The history matters, because transcribing the reset WITHOUT the writer is what
 * made a parked car shake, and it is an easy mistake to make again. With no
 * writer the ramp zeroed on the first parked frame and stayed there, so the rate
 * was pinned at 1x forever -- including at speed, where the `speed >= SLOW_MPS`
 * branch is meant to give 10x. A strut extending at radius/240 per substep covers
 * 72 mm/s, while a body pitching at ~1 rad/s moves its wheel mounts at ~145 mm/s
 * over the 0.149 m half-wheelbase, so `len` lagged the true geometry -- and a
 * lagged spring length is a negative-damping term. Measured 5.16 degrees of body
 * tilt peak to peak on a 1 degree slope, forever.
 */
static void rb_susp_ramp_reset(rb_car *c)
{
    c->susp_ramp = 0.0f;
}

/* 0x004f6fd8, inside FUN_004f6ea0 -- the writer. Once per FRAME, not per
 * substep: it sits with the engine's other frame timers (0x56f0..0x5704), before
 * the physics runs. */
void rb_susp_ramp_advance(rb_car *c, float dt)
{
    c->susp_ramp = (float)((double)c->susp_ramp + dt);
}

/* 0x004fb340 -- solve every spring length against the world, then set `dlen`.
 *
 * Per wheel:
 *   touching  -> retract until clear, bounded by 0.95 radius
 *   clear     -> extend toward the free length (or, when barely moving, toward
 *                free - 0.95*sag, which is the parked ride height)
 *
 * The extension rate is `dt * radius`, ten times that once moving, ramped by
 * susp_ramp while it is in 0..0.5. Finally a segment test from the old wheel
 * centre to the new one rejects any move that tunnelled through geometry.
 */
int rb_car_update_suspension(rb_car *c, float dt, int slow_mode,
                             int *embedded_out)
{
    float prev_len[RB_MAX_WHEELS];
    double speed;
    int i, stuck = 0;

    for (i = 0; i < c->nwheels; i++)
        prev_len[i] = c->wheel[i].len;

    speed = sqrt((double)c->body.v[0] * c->body.v[0]
                 + (double)c->body.v[1] * c->body.v[1]
                 + (double)c->body.v[2] * c->body.v[2]);

    /* is any wheel buried? one buried wheel changes the whole pass */
    c->embedded = 0;
    for (i = 0; i < c->nwheels; i++) {
        c->embedded = rb_wheel_buried(c, i);
        if (c->embedded)
            break;
    }
    if (embedded_out)
        *embedded_out = (c->embedded != 0);

    /* DIVERGENCE: the original abandons the whole pass on the first wheel it
     * cannot resolve (`stuck = 1; break;`). Reproducing that leaves every later
     * wheel holding a stale length, and stale lengths mean wrong spring forces
     * on that side -- measured, two of four wheels sat frozen for 150+ frames
     * while the body was thrown from 12 to 176 degrees of tilt and back. The
     * original can afford the early exit because its body-sphere contact solve
     * catches what the struts cannot; this port has no such backstop.
     *
     * So: solve every wheel, and still report `stuck` at the end. The caller uses
     * it the same way.
     */
    for (i = 0; i < c->nwheels; i++) {
        rb_wheel *wh = &c->wheel[i];
        float saved = wh->len;
        float old_centre[3], new_centre[3], radius;
        double min_len, max_step, rate, target;

        rb_wheel_frame(c, i, 0, old_centre, &radius, 0, 0);

        min_len  = (double)wh->len_free - (double)wh->sag * 0.95;
        max_step = (double)radius * 0.95;

        if (wheel_probe(c, i, 0)) {
            if (!c->embedded) {
                if (rb_susp_retract(c, i, (float)max_step)) {
                    DBG(i, "touch:retract-ok");
                    goto verify;
                }
                DBG(i, "touch:RETRACT-FAIL");
            } else {
                double step = max_step;
                double cap = (double)radius * 0.99;
                if (step < 0.0) step = 0.0;
                else if (step > cap) step = cap;
                wh->len = rb_move_towards(wh->len, (float)min_len,
                                          (float)step, dt);
                if (!wheel_probe(c, i, 0)) {
                    if (speed < SLOW_MPS)
                        rb_susp_ramp_reset(c);
                    DBG(i, "embed:pushed-out");
                    goto verify;
                }
                wh->len = saved;
                DBG(i, "embed:STUCK");
            }
            stuck = 1;
            continue;      /* see the note above the loop: do NOT break */
        }

        /* clear: extend back out */
        rate = (double)dt * radius;
        if ((slow_mode == 0 && c->embedded == 0) || speed >= SLOW_MPS) {
            double fast = rate * 10.0;
            double t = c->susp_ramp;
            target = wh->len_free;
            if (t >= 0.0 && t <= 0.5) {
                double d = (fast - rate) * t;
                rate = d + d + rate;
            } else {
                rate = fast;
            }
        } else {
            rb_susp_ramp_reset(c);
            target = min_len;
        }
        if (c->frozen)
            rate = (double)dt * radius * 10.0;

        {
            double cap = (double)radius * 0.99;
            if (rate < 0.0) rate = 0.0;
            else if (rate > cap) rate = cap;
        }

        if ((double)wh->len < target) {
            DBG(i, "clear:extend");
            rb_susp_extend(c, i, (float)rate);
        } else {
            DBG(i, "clear:no-extend");
        }

        if (target < (double)wh->len) {
            float before = wh->len;
            wh->len = rb_move_towards(wh->len, (float)target, (float)rate, 1.0f);
            if (wheel_probe(c, i, 0))
                wh->len = before;
        }

verify:
        /* reject a move that swept the wheel through geometry */
        rb_wheel_frame(c, i, 0, new_centre, 0, 0, 0);
        if (c->world && c->world->segment
            && c->world->segment(c->world->ctx, old_centre, new_centre)) {
            wh->len = saved;
            stuck = 1;
            DBG(i, "verify:SEGMENT-REJECT");
        }
    }

    for (i = 0; i < c->nwheels; i++) {
        c->wheel[i].dlen = stuck
            ? 0.0f
            : (float)((double)c->wheel[i].len - prev_len[i]);
    }
    return stuck;
}

/* ------------------------------------------------------------------------- */
/* the Jump action, and the reset it turns into when the car is upside down   */
/* ------------------------------------------------------------------------- */

/* DAT_0055e9a0, the engine's world-up triple. */
static const float WORLD_UP[3] = { 0.0f, 1.0f, 0.0f };

/* 0x00410130 -- the angle between two vectors in degrees, via 0x00410080's
 * normalised dot product. Two details of the original are kept because
 * rb_car_jump's 80-degree test sits right on top of them: the cosine is CLAMPED
 * to [-1, 1] before the acos (0x554398 / 0x554390), and a degenerate length
 * makes 0x00410080 return 0.0, so the angle comes out 90 degrees rather than
 * NaN. Ninety is on the reset side of the test, which is the safe side: it
 * cannot fire a hop from an unknown attitude. */
static double vec3_angle_deg(const float a[3], const float b[3])
{
    double la = sqrt((double)a[0]*a[0] + (double)a[1]*a[1] + (double)a[2]*a[2]);
    /* the original stores this sqrt to f32 and reloads it before multiplying */
    float  lbf = (float)sqrt((double)b[0]*b[0] + (double)b[1]*b[1]
                             + (double)b[2]*b[2]);
    double den = (double)lbf * la;
    double cs;

    if (fabs(den) < (double)EPS)
        return 0.0 * RB_RAD2DEG;
    cs = ((double)a[2]*b[2] + (double)a[1]*b[1] + (double)a[0]*b[0]) / den;
    if (cs < -1.0)     cs = -1.0;
    else if (cs > 1.0) cs = 1.0;
    return acos(cs) * (double)RB_RAD2DEG;
}

/* 0x00508600 -- see rb.h. */
void rb_car_reset_upright(rb_car *c)
{
    float pos[3], fwd[2];
    double len;
    int pass;

    /* Every momentum and every accumulator, zeroed. The original writes them in
       an odd order -- w then v = w, L then P = L, torque then force = torque --
       which is just the compiler reusing a zero; the result is all six. */
    c->body.v[0] = c->body.v[1] = c->body.v[2] = 0.0f;
    c->body.w[0] = c->body.w[1] = c->body.w[2] = 0.0f;
    c->body.P[0] = c->body.P[1] = c->body.P[2] = 0.0f;
    c->body.L[0] = c->body.L[1] = c->body.L[2] = 0.0f;
    c->body.force[0]  = c->body.force[1]  = c->body.force[2]  = 0.0f;
    c->body.torque[0] = c->body.torque[1] = c->body.torque[2] = 0.0f;

    pos[0] = c->body.x[0];
    pos[1] = c->body.x[1];
    pos[2] = c->body.x[2];

    /* Heading: the car's own forward (matrix row 2), flattened into the ground
     * plane and renormalised, falling back to world +Z (0x0055e9b0) when the car
     * is standing exactly on its nose and the flattened vector vanishes.
     *
     * The original consults the track spline first (FUN_004873c0) and only uses
     * the car's forward when the car has none bound; the port always takes that
     * second branch -- see rb.h.
     *
     * Only x and z are carried, as fwd[0] and fwd[1]. The original zeroes the y
     * component explicitly because it goes on to hand the whole vector to
     * 0x00408460 to build a matrix from; here the orientation comes out of
     * atan2(x, z) below, which is the same flattening expressed once rather than
     * twice. A y term that nothing reads would look like part of the calculation
     * and would not be -- mutating it survived the whole suite, which is how this
     * shape got chosen. */
    fwd[0] = c->m[8];
    fwd[1] = c->m[10];
    len = sqrt((double)fwd[0]*fwd[0] + (double)fwd[1]*fwd[1]);
    if (len < (double)EPS) {
        fwd[0] = 0.0f; fwd[1] = 1.0f;              /* world +Z, DAT_0055e9b0 */
    } else if (fabs(len - 1.0) >= (double)EPS) {
        fwd[0] = (float)(fwd[0] * (1.0 / len));
        fwd[1] = (float)(fwd[1] * (1.0 / len));
    }

    /* Level it. The original builds the whole 4x4 from (position, up = world Y,
     * forward) with 0x00408460 and pushes it into the rigid body; with up pinned
     * to world Y that orientation is a pure rotation about Y, so here it is the
     * yaw quaternion directly. The mapping is the one rbcar_init uses and
     * startchk measures: a node's local +Z lands on (sin yaw, 0, cos yaw), so
     * yaw = atan2(fwd.x, fwd.z). */
    {
        double half = atan2((double)fwd[0], (double)fwd[1]) * 0.5;
        c->body.q[0] = (float)cos(half);
        c->body.q[1] = 0.0f;
        c->body.q[2] = (float)sin(half);
        c->body.q[3] = 0.0f;
    }

    /* Push the body origin clear of geometry: a RB_RESET_CLEAR_RADIUS sphere at
     * the origin, moved out along the direction away from whatever it finds,
     * until nothing is inside it. At most RB_RESET_CLEAR_PASSES passes.
     *
     * That radius is much larger than the car -- half a metre against a 0.42 m
     * Overkill -- so on open ground this does not merely un-bury the car, it
     * lifts it to 0.5 m and lets it fall. That IS the original: carCollide fills
     * the same query struct at 0x004eff17, which places the sphere radius at
     * +0x14, and FUN_00508600 writes 0x3f000000 there. It is why a reset in the
     * real game drops the car in from a little height rather than snapping it to
     * the surface, and the drop is the same order as the hop's own 0.45 m. */
    for (pass = 0; pass < RB_RESET_CLEAR_PASSES; pass++) {
        rb_world_hit hits[8];
        float d[3];
        double dl, push;
        int nh = 0, k;

        if (!c->world || !c->world->sphere)
            break;
        if (!c->world->sphere(c->world->ctx, pos, RB_RESET_CLEAR_RADIUS,
                              hits, (int)(sizeof(hits)/sizeof(hits[0])), &nh))
            break;

        for (k = 0; k < 3; k++)
            d[k] = (float)((double)pos[k] - hits[0].point[k]);
        dl = sqrt((double)d[0]*d[0] + (double)d[1]*d[1] + (double)d[2]*d[2]);
        if (dl >= (double)EPS && fabs(dl - 1.0) >= (double)EPS) {
            double inv = 1.0 / dl;
            d[0] = (float)(d[0] * inv);
            d[1] = (float)(d[1] * inv);
            d[2] = (float)(d[2] * inv);
        }
        push = ((double)RB_RESET_CLEAR_RADIUS - dl) + (double)EPS;
        for (k = 0; k < 3; k++)
            pos[k] = (float)((double)d[k] * push + pos[k]);
    }

    c->body.x[0] = pos[0];
    c->body.x[1] = pos[1];
    c->body.x[2] = pos[2];
    rb_car_update_matrix(c);
    rb_update_inv_inertia_world(&c->body);

    /* The original ends by re-gathering the collision spheres, comparing them
       with the set it saved on entry, and on any movement calling FUN_004f6af0
       -> FUN_00508b50. That is engine bookkeeping for a teleport: snap the
       follow camera (camUpdate with a 2e6 dt), re-baseline the odometer's
       previous-frame position, and poke the AI's path progress. None of it is
       dynamics, and the port has no such registration -- the camera snap is the
       one piece that shows, and main.c does it off rb_car_jump's return value. */
}

/* 0x004f3b80 -- see rb.h. */
int rb_car_jump(rb_car *c, float dt)
{
    float up[3];
    double angle;

    /* The cooldown runs whether or not the button is down, and the whole
       function is gated on it -- so is the release that clears the latch. */
    c->jump_t = (float)((double)c->jump_t + dt);
    if (c->jump_t < RB_JUMP_COOLDOWN)
        return RB_JUMP_NONE;

    /* Edge detect. Holding Jump hops once, not once per frame. */
    if (!c->in.jump)
        c->jump_latch = 0;
    if (c->jump_latch || !c->in.jump)
        return RB_JUMP_NONE;
    c->jump_latch = 1;

    up[0] = c->m[4];
    up[1] = c->m[5];
    up[2] = c->m[6];
    angle = vec3_angle_deg(up, WORLD_UP);

    if (angle > (double)RB_JUMP_FLIP_DEG) {
        /* On its side or its roof. Right it -- but only if it is actually lying
           on something: mode 1 is the BODY spheres, so a car still tumbling
           through the air cannot be reset mid-flight. The latch is cleared
           either way, so a second press retries immediately rather than waiting
           out the cooldown. */
        int touching = rb_collide(c, 0.0f, RB_CONTACT_TOL, 1, RB_JUMP_LIMIT, 0);
        if (touching)
            rb_car_reset_upright(c);
        c->jump_latch = 0;
        return touching ? RB_JUMP_RESET : RB_JUMP_NONE;
    }

    /* Upright: hop, if a WHEEL sphere is touching (mode 0). */
    if (rb_collide(c, 0.0f, RB_CONTACT_TOL, 0, RB_JUMP_LIMIT, 0)) {
        double f = (c->car_index == 1) ? (double)RB_JUMP_SPEED_BUGGY
                                       : (double)RB_JUMP_SPEED;
        f *= (double)c->body.mass;
        c->body.P[0] = (float)((double)up[0] * f + c->body.P[0]);
        c->body.P[1] = (float)((double)up[1] * f + c->body.P[1]);
        c->body.P[2] = (float)((double)up[2] * f + c->body.P[2]);
        /* Yaw momentum only, and multiplied rather than added: whatever spin the
           car already had about its vertical axis comes off the ground amplified.
           This is what lets a hop out of a turn snap the nose round. */
        c->body.L[1] = (float)((double)c->body.L[1] * RB_JUMP_SPIN_MULT);
        c->jump_t = 0.0f;
        return RB_JUMP_HOP;
    }
    return RB_JUMP_NONE;
}
