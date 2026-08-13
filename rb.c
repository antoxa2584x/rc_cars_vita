/*
 * rb.c -- RC Cars rigid-body vehicle core, transcribed from RCCars.exe.
 *
 * Each function carries the address it was transcribed from. Operation order
 * inside expressions follows the original x87 code, because the port rule is
 * "double intermediates, float state, preserve operation order" -- that is
 * bit-identical to x87 at PC=53 storing to float32. Where the original
 * evaluates terms in an unusual order (rb_mat4_mul does b[1]-term first), the
 * order is kept deliberately. Do not tidy it.
 *
 * Build with -fno-fast-math -ffp-contract=off.
 */

#include "rb.h"
#include <math.h>
#include <string.h>

#define EPS 1e-06f

/* ------------------------------------------------------------------------- */
/* quaternion / matrix primitives                                            */
/* ------------------------------------------------------------------------- */

/* 0x004079f0 -- Hamilton product, (w, x, y, z). */
void rb_quat_mul(const float a[4], const float b[4], float out[4])
{
    double w, x, y, z;

    w = (double)a[0] * b[0]
        - ((double)a[1] * b[1] + (double)a[2] * b[2] + (double)a[3] * b[3]);

    /* cross(a.xyz, b.xyz), then the two scalar-times-vector terms, in the
       original's order: cross first, scalars folded in afterwards. */
    x = (double)b[3] * a[2] - (double)a[3] * b[2];
    y = (double)a[3] * b[1] - (double)b[3] * a[1];
    z = (double)a[1] * b[2] - (double)a[2] * b[1];

    out[0] = (float)w;
    out[1] = (float)((double)b[0] * a[1] + (double)a[0] * b[1] + x);
    out[2] = (float)((double)b[0] * a[2] + (double)a[0] * b[2] + y);
    out[3] = (float)((double)b[0] * a[3] + (double)a[0] * b[3] + z);
}

/* 0x00407990 */
void rb_quat_scale(float q[4], float s)
{
    q[0] = (float)((double)s * q[0]);
    q[1] = (float)((double)s * q[1]);
    q[2] = (float)((double)s * q[2]);
    q[3] = (float)((double)s * q[3]);
}

/* 0x004078d0 -- note the two-step norm: |xyz| first, then hypot with w. That
   is what the original does and it does not round the same as a single
   four-term sqrt, so keep it. */
void rb_quat_normalize(float q[4])
{
    float n = (float)sqrt((double)q[1] * q[1] + (double)q[2] * q[2]
                          + (double)q[3] * q[3]);
    n = (float)sqrt((double)q[0] * q[0] + (double)n * n);
    if (n >= EPS)
        rb_quat_scale(q, (float)(1.0 / n));
}

/* 0x00407d40 -- quaternion to 4x4 row-major, row-vector convention.
   Uses s = 2/|q|^2 so an unnormalised quaternion still gives a rotation. */
void rb_quat_to_matrix(const float q[4], float m[16])
{
    double w = q[0], x = q[1], y = q[2], z = q[3];
    double n = w * w + x * x + y * y + z * z;
    double s, ty, tz, xy2, yy2;

    if (n < 1e-06) {
        memset(m, 0, 16 * sizeof(float));
        m[0] = m[5] = m[10] = m[15] = 1.0f;
        return;
    }

    s = 2.0 / n;

    m[3] = 0.0f; m[7] = 0.0f; m[11] = 0.0f; m[15] = 1.0f;
    m[12] = 0.0f; m[13] = 0.0f; m[14] = 0.0f;

    ty  = s * y;          /* fVar7 */
    tz  = s * z;          /* fVar6 */
    xy2 = s * x * w;      /* fVar5 */
    yy2 = s * x * x;      /* fVar8 */

    m[0]  = (float)(1.0 - (tz * z + ty * y));
    m[1]  = (float)(ty * x + tz * w);
    m[2]  = (float)(tz * x - ty * w);
    m[4]  = (float)(ty * x - tz * w);
    m[5]  = (float)(1.0 - (tz * z + yy2));
    m[6]  = (float)(tz * y + xy2);
    m[8]  = (float)(tz * x + ty * w);
    m[9]  = (float)(tz * y - xy2);
    m[10] = (float)(1.0 - (ty * y + yy2));
}

/* 0x0040c7f0 -- out = a * b, 4x4 row-major with an affine 4th row.
   The term order (b row 1, then row 2, then row 0) is the original's. */
void rb_mat4_mul(const float a[16], const float b[16], float out[16])
{
    int i, j;

    for (i = 0; i < 3; i++) {
        for (j = 0; j < 4; j++) {
            out[i * 4 + j] = (float)((double)b[4 + j] * a[i * 4 + 1]
                                     + (double)b[8 + j] * a[i * 4 + 2]
                                     + (double)b[j]     * a[i * 4 + 0]);
        }
    }
    for (j = 0; j < 4; j++) {
        out[12 + j] = (float)((double)b[8 + j]  * a[14]
                              + (double)b[4 + j]  * a[13]
                              + (double)b[j]      * a[12]
                              + (double)b[12 + j] * a[15]);
    }
}

/* 0x00474910 -- 3x3 block of a row-stride-4 matrix times a vec3. */
void rb_mat3_mul_vec3(const float m[16], const float v[3], float o[3])
{
    int i, k;
    for (i = 0; i < 3; i++) {
        double acc = 0.0;
        for (k = 0; k < 3; k++)
            acc = (double)m[i * 4 + k] * v[k] + acc;
        o[i] = (float)acc;
    }
}

/* ------------------------------------------------------------------------- */
/* rigid body                                                                */
/* ------------------------------------------------------------------------- */

/* 0x00474e00 -- Iinv_world = M' * Ibodyinv * M, where M is the row-vector
   body-to-world rotation. (Same thing as R * Ibodyinv * R' in the
   column-vector convention.) */
void rb_update_inv_inertia_world(rb_body *b)
{
    float m[16], mt[16], tmp[16];
    int i, j;

    rb_quat_to_matrix(b->q, m);
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            mt[i * 4 + j] = m[j * 4 + i];

    rb_mat4_mul(mt, b->ibody_inv, tmp);
    rb_mat4_mul(tmp, m, b->iinv);
}

/* 0x00474ed0 -- velocity of a world-space point on the body:
   vp = v + w x (p - x). */
void rb_point_velocity(const rb_body *b, const float p[3], float o[3])
{
    double rx = (double)p[0] - b->x[0];
    double ry = (double)p[1] - b->x[1];
    double rz = (double)p[2] - b->x[2];

    o[0] = (float)((double)b->v[0] + (rz * b->w[1] - ry * b->w[2]));
    o[1] = (float)((double)b->v[1] + (rx * b->w[2] - rz * b->w[0]));
    o[2] = (float)((double)b->v[2] + (ry * b->w[0] - rx * b->w[1]));
}

/* 0x00475100 -- reset the accumulators, then F = sum f and
   tau = sum (p - x) x f. Note this OVERWRITES rather than adds: the original
   uses the accumulators as scratch for the suspension sub-queries and only the
   final call's result is the one that reaches the derivative. */
void rb_sum_forces_torques(rb_body *b, const float pts[][3], int n,
                           const float f[][3])
{
    int i;

    b->force[0] = 0.0f; b->force[1] = 0.0f; b->force[2] = 0.0f;
    for (i = 0; i < n; i++) {
        b->force[0] = (float)((double)f[i][0] + b->force[0]);
        b->force[1] = (float)((double)f[i][1] + b->force[1]);
        b->force[2] = (float)((double)f[i][2] + b->force[2]);
    }

    b->torque[0] = 0.0f; b->torque[1] = 0.0f; b->torque[2] = 0.0f;
    for (i = 0; i < n; i++) {
        double rx = (double)pts[i][0] - b->x[0];
        double ry = (double)pts[i][1] - b->x[1];
        double rz = (double)pts[i][2] - b->x[2];

        b->torque[0] = (float)((ry * f[i][2] - rz * f[i][1]) + b->torque[0]);
        b->torque[1] = (float)((rz * f[i][0] - rx * f[i][2]) + b->torque[1]);
        b->torque[2] = (float)((rx * f[i][1] - ry * f[i][0]) + b->torque[2]);
    }
}

/* 0x004749d0 -- express a pure torque as two opposed forces at +-r from the
   centre of mass, r being a unit vector perpendicular to tau. Picking the
   reference axis by the 45..135 degree test avoids the degenerate case where
   tau is parallel to the axis. */
void rb_torque_to_couple(const rb_body *b, const float tau[3],
                         float f[2][3], float p[2][3])
{
    static const float AXIS_X[3] = { 1.0f, 0.0f, 0.0f };
    static const float AXIS_Y[3] = { 0.0f, 1.0f, 0.0f };
    const float *axis;
    float t[3], r[3], cr[3];
    double mag, len, dot, ang, s;
    int i;

    t[0] = tau[0]; t[1] = tau[1]; t[2] = tau[2];
    mag = sqrt((double)t[0] * t[0] + (double)t[1] * t[1] + (double)t[2] * t[2]);

    if (mag < EPS) {
        for (i = 0; i < 3; i++) {
            f[0][i] = 0.0f; f[1][i] = 0.0f;
            p[0][i] = b->x[i]; p[1][i] = b->x[i];
        }
        return;
    }

    if (fabs(mag - 1.0) >= EPS) {
        double inv = 1.0 / mag;
        t[0] = (float)(t[0] * inv);
        t[1] = (float)(t[1] * inv);
        t[2] = (float)(t[2] * inv);
    }

    /* angle(X, that) in degrees */
    dot = (double)AXIS_X[0] * t[0] + (double)AXIS_X[1] * t[1]
          + (double)AXIS_X[2] * t[2];
    if (dot > 1.0)  dot = 1.0;
    if (dot < -1.0) dot = -1.0;
    ang = acos(dot) * 57.295776;
    axis = (ang > 45.0 && ang < 135.0) ? AXIS_X : AXIS_Y;

    /* r = axis - that * dot(axis, that), i.e. Gram-Schmidt, then normalise */
    dot = (double)axis[0] * t[0] + (double)axis[1] * t[1] + (double)axis[2] * t[2];
    r[0] = (float)(axis[0] - t[0] * dot);
    r[1] = (float)(axis[1] - t[1] * dot);
    r[2] = (float)(axis[2] - t[2] * dot);

    len = sqrt((double)r[0] * r[0] + (double)r[1] * r[1] + (double)r[2] * r[2]);
    if (len >= EPS && fabs(len - 1.0) >= EPS) {
        double inv = 1.0 / len;
        r[0] = (float)(r[0] * inv);
        r[1] = (float)(r[1] * inv);
        r[2] = (float)(r[2] * inv);
    }

    for (i = 0; i < 3; i++) {
        p[0][i] = (float)((double)r[i] + b->x[i]);
        p[1][i] = (float)((double)b->x[i] - r[i]);
    }

    /* cross(that, r) -- note the operand order; it makes the resulting couple
       reproduce +tau, not -tau. */
    cr[0] = (float)((double)r[2] * t[1] - (double)r[1] * t[2]);
    cr[1] = (float)((double)r[0] * t[2] - (double)r[2] * t[0]);
    cr[2] = (float)((double)r[1] * t[0] - (double)r[0] * t[1]);

    /* |tau| is recomputed from the ORIGINAL tau here, not the normalised copy */
    s = sqrt((double)tau[0] * tau[0] + (double)tau[1] * tau[1]
             + (double)tau[2] * tau[2]) * 0.5;
    for (i = 0; i < 3; i++) {
        f[0][i] = (float)(s * cr[i]);
        f[1][i] = (float)(-s * cr[i]);
    }
}

/* ------------------------------------------------------------------------- */
/* state marshalling                                                         */
/* ------------------------------------------------------------------------- */

/* 0x004f5180 */
void rb_car_get_state(const rb_car *c, float y[RB_STATE_N])
{
    int i;

    memcpy(&y[0],  c->body.x, 3 * sizeof(float));
    memcpy(&y[3],  c->body.q, 4 * sizeof(float));
    memcpy(&y[7],  c->body.P, 3 * sizeof(float));
    memcpy(&y[10], c->body.L, 3 * sizeof(float));

    for (i = 0; i < RB_MAX_WHEELS; i++) {
        y[13 + i] = c->wheel[i].len;
        y[19 + i] = c->wheel[i].dlen;
        y[26 + i] = c->wheel[i].len_extra;
    }
    y[25] = c->steer;
}

/* 0x004f5290 -- scatter, then rebuild everything derived: normalise q,
   v = P/m, Iinv from the new orientation, w = Iinv * L. */
void rb_car_set_state(rb_car *c, const float y[RB_STATE_N])
{
    rb_body *b = &c->body;
    int i;

    memcpy(b->x, &y[0], 3 * sizeof(float));
    memcpy(b->q, &y[3], 4 * sizeof(float));
    rb_quat_normalize(b->q);

    memcpy(b->P, &y[7],  3 * sizeof(float));
    memcpy(b->L, &y[10], 3 * sizeof(float));

    for (i = 0; i < RB_MAX_WHEELS; i++) {
        c->wheel[i].len       = y[13 + i];
        c->wheel[i].dlen      = y[19 + i];
        c->wheel[i].len_extra = y[26 + i];
    }
    c->steer = y[25];

    b->v[0] = (float)((double)b->inv_mass * b->P[0]);
    b->v[1] = (float)((double)b->inv_mass * b->P[1]);
    b->v[2] = (float)((double)b->inv_mass * b->P[2]);

    rb_update_inv_inertia_world(b);
    rb_mat3_mul_vec3(b->iinv, b->L, b->w);

    rb_car_update_matrix(c);
}

void rb_car_update_matrix(rb_car *c)
{
    rb_quat_to_matrix(c->body.q, c->m);
    c->m[12] = c->body.x[0];
    c->m[13] = c->body.x[1];
    c->m[14] = c->body.x[2];
    c->m[15] = 1.0f;
}

/* 0x004f5400 -- ydot. Indices 13..31 stay zero: the wheel state is carried
   through the step unchanged and updated by the contact code, not integrated.
   That is exactly why they are in the state vector -- so the substep search
   can rewind them. */
void rb_car_state_deriv(rb_car *c, float dt, float ydot[RB_STATE_N])
{
    float wq[4], qd[4];
    int i;

    for (i = 0; i < RB_STATE_N; i++)
        ydot[i] = 0.0f;

    rb_car_accum_forces(c, dt);

    /* dx/dt = v */
    ydot[0] = c->body.v[0];
    ydot[1] = c->body.v[1];
    ydot[2] = c->body.v[2];

    /* dq/dt = 0.5 * (0, w) (x) q */
    wq[0] = 0.0f;
    wq[1] = c->body.w[0];
    wq[2] = c->body.w[1];
    wq[3] = c->body.w[2];
    rb_quat_mul(wq, c->body.q, qd);
    rb_quat_scale(qd, 0.5f);
    ydot[3] = qd[0];
    ydot[4] = qd[1];
    ydot[5] = qd[2];
    ydot[6] = qd[3];

    /* dP/dt = F, dL/dt = tau */
    ydot[7]  = c->body.force[0];
    ydot[8]  = c->body.force[1];
    ydot[9]  = c->body.force[2];
    ydot[10] = c->body.torque[0];
    ydot[11] = c->body.torque[1];
    ydot[12] = c->body.torque[2];
}

/* 0x004f5590 -- explicit Euler over the whole 32-float state. That is the
   entire integrator; there is no RK stage anywhere. */
void rb_euler_step(rb_car *c, const float y[RB_STATE_N], float dt,
                   float yout[RB_STATE_N])
{
    float ydot[RB_STATE_N];
    int i;

    rb_car_state_deriv(c, dt, ydot);

    for (i = 0; i < RB_STATE_N; i++)
        yout[i] = (float)((double)dt * ydot[i] + y[i]);
}

/* ------------------------------------------------------------------------- */
/* forces                                                                    */
/* ------------------------------------------------------------------------- */

/* 0x004ed970 -- F = -vhat * (Cair * |v|^2 + Cbearings).
   Quadratic air drag plus a constant rolling/bearing term. */
void rb_car_drag_force(const rb_car *c, float out[3])
{
    const rb_body *b = &c->body;
    float d[3];
    double speed, mag;

    out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f;

    d[0] = b->v[0]; d[1] = b->v[1]; d[2] = b->v[2];
    speed = sqrt((double)d[0] * d[0] + (double)d[1] * d[1] + (double)d[2] * d[2]);
    if (speed <= 0.0001)
        return;

    if (speed >= EPS && fabs(speed - 1.0) >= EPS) {
        double inv = 1.0 / speed;
        d[0] = (float)(inv * d[0]);
        d[1] = (float)(d[1] * inv);
        d[2] = (float)(d[2] * inv);
    }

    mag = -(speed * c->tune.coeff_air_resistance * speed
            + c->tune.coeff_friction_bearings);

    out[0] = (float)(mag * d[0]);
    out[1] = (float)(d[1] * mag);
    out[2] = (float)(d[2] * mag);
}

/* 0x004f0f80 -- spring/damper for one corner.
 *
 *   damp   = k_speed * dlen / dt
 *   spring = -((len_extra + len - len_free) * k_pos)
 *   result = spring - damp, with |damp| clamped to |spring| * 2e6
 *
 * `len` is the current spring length, so `len_free - len` is the compression
 * and the spring force is positive (pushing the body up) while compressed.
 * Compressing further makes dlen negative, so `- damp` ADDS force: the damper
 * resists the compression, as it should.
 *
 * The clamp is the original's runaway guard: at small dt the dlen/dt term can
 * blow up, so it is capped relative to the spring term rather than absolutely.
 */
float rb_susp_spring_damper(const rb_wheel *wh, float dt)
{
    double damp, spring, lim;

    if (dt <= 0.001f)
        damp = 0.0;
    else
        damp = ((double)wh->k_speed * wh->dlen) / dt;

    spring = -(((double)wh->len_extra + wh->len - wh->len_free) * wh->k_pos);
    lim = fabs(spring) * 2e+06;

    if (fabs(damp) < lim)
        return (float)(spring - damp);
    if (damp > 0.0)
        return (float)(spring - lim);
    return (float)(lim + spring);
}

/* 0x004f0d70 -- one (point, force) pair per loaded corner. The force acts
   along the car's local Y (up) axis, which is row 1 of the body matrix.
   With project_normal set, the force is then projected onto the contact
   normal: f = n * dot(n, f). */
void rb_car_susp_build(rb_car *c, float dt, int *n, float pts[][3],
                       float f[][3], int project_normal)
{
    const float *up = &c->m[4];   /* 0x40cbd0: matrix row 1 */
    int i, k;

    if (!c->susp_enabled)
        return;

    for (i = 0; i < c->nwheels && *n < RB_MAX_FORCES; i++) {
        rb_wheel *wh = &c->wheel[i];
        double mag;

        /* the corner must be in contact ... */
        if (!c->hit[i].active)
            continue;
        /* ... and loaded, i.e. the spring is shorter than its free length */
        if ((double)wh->len_extra + wh->len > wh->len_free)
            continue;

        mag = rb_susp_spring_damper(wh, dt);

        /* mount point to world: p = mount * M + translation */
        for (k = 0; k < 3; k++) {
            pts[*n][k] = (float)((double)c->m[0 + k] * wh->mount[0]
                                 + (double)c->m[4 + k] * wh->mount[1]
                                 + (double)c->m[8 + k] * wh->mount[2]
                                 + c->m[12 + k]);
        }

        f[*n][0] = (float)((double)up[0] * mag);
        f[*n][1] = (float)((double)up[1] * mag);
        f[*n][2] = (float)((double)up[2] * mag);

        if (project_normal) {
            const float *nrm = c->hit[i].normal;
            double d = (double)nrm[0] * f[*n][0] + (double)nrm[1] * f[*n][1]
                       + (double)nrm[2] * f[*n][2];
            f[*n][0] = (float)(d * nrm[0]);
            f[*n][1] = (float)(d * nrm[1]);
            f[*n][2] = (float)(d * nrm[2]);
        }

        (*n)++;
    }
}

/* 0x004f0be0 -- suspension torque, decomposed onto the car's own X and Z axes
 * and scaled by coeffMomentOX / coeffMomentOZ. The Y (yaw) component is
 * DISCARDED -- suspension cannot yaw the car.
 *
 * This is the cross-check that pinned those two constants: the code multiplies
 * exactly the X-axis component by springs[0] and the Z-axis component by
 * springs[1], and those two globals are where the loaders write coeffMomentOX
 * and coeffMomentOZ.
 */
void rb_car_susp_torque(rb_car *c, float dt, float out[3])
{
    float pts[RB_MAX_FORCES][3], f[RB_MAX_FORCES][3];
    const float *ax = &c->m[0];   /* row 0: X axis */
    const float *az = &c->m[8];   /* row 2: Z axis */
    double cx, cz;
    int n = 0;

    out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f;

    rb_car_susp_build(c, dt, &n, pts, f, 0);
    rb_sum_forces_torques(&c->body, (const float (*)[3])pts, n,
                          (const float (*)[3])f);

    out[0] = c->body.torque[0];
    out[1] = c->body.torque[1];
    out[2] = c->body.torque[2];

    cx = ((double)ax[0] * out[0] + (double)ax[1] * out[1] + (double)ax[2] * out[2])
         * c->tune.coeff_moment_ox;
    cz = ((double)az[0] * out[0] + (double)az[1] * out[1] + (double)az[2] * out[2])
         * c->tune.coeff_moment_oz;

    out[0] = (float)(az[0] * cz + cx * ax[0]);
    out[1] = (float)(az[1] * cz + ax[1] * cx);
    out[2] = (float)(ax[2] * cx + az[2] * cz);
}

/* 0x004f1000 -- suspension force, taken from the normal-projected pass. */
void rb_car_susp_force(rb_car *c, float dt, float out[3])
{
    float pts[RB_MAX_FORCES][3], f[RB_MAX_FORCES][3];
    int n = 0;

    out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f;

    rb_car_susp_build(c, dt, &n, pts, f, 1);
    rb_sum_forces_torques(&c->body, (const float (*)[3])pts, n,
                          (const float (*)[3])f);

    out[0] = c->body.force[0];
    out[1] = c->body.force[1];
    out[2] = c->body.force[2];
}

/* 0x004f0910 -- build the whole (point, force) list for this instant and
 * reduce it to force + torque.
 *
 * Order matters only for float rounding, but it is the original's order:
 *   1. gravity, at the centre of mass
 *   2. drag, at the centre of mass
 *   3. suspension force (normal-projected), at the centre of mass
 *   4. suspension torque, as a force couple at +-r
 *   5. contact + tire + engine forces  (carContactSolve, not yet transcribed)
 */
/* 0x0049d7e0 -- step `cur` toward `target` by rate*dt, snapping when close. */
float rb_move_towards(float cur, float target, float rate, float dt)
{
    double step = (double)rate * dt;
    if (fabs((double)cur - target) < step)
        return target;
    if (cur < target)
        return (float)(step + cur);
    return (float)((double)cur - step);
}

/* 0x004f1930 -- see the header. k_pos is chosen so the car settles at `sag`. */
void rb_car_setup_springs(rb_car *c)
{
    int i;
    for (i = 0; i < c->nwheels; i++) {
        double d = (double)c->nwheels * c->wheel[i].sag;
        c->wheel[i].k_pos = (fabs(d) < EPS)
            ? 0.0f
            : (float)(((double)c->body.mass * 10.0) / d);
    }
}

void rb_car_accum_forces(rb_car *c, float dt)
{
    float pts[RB_MAX_FORCES][3], f[RB_MAX_FORCES][3];
    float tau[3], fsusp[3], drag[3];
    float cf[2][3], cp[2][3];
    int n = 0;
    int i;
    int resting = 0;

    rb_car_susp_torque(c, dt, tau);
    rb_car_susp_force(c, dt, fsusp);

    /* 1. gravity: mass * -10 along +Y, i.e. straight down */
    {
        double g = (double)c->body.mass * -RB_GRAVITY;
        memcpy(pts[n], c->body.x, 3 * sizeof(float));
        f[n][0] = 0.0f;
        f[n][1] = (float)(1.0 * g);
        f[n][2] = 0.0f;
        n++;
    }

    /* 2. drag -- skipped while the car is held */
    if (!c->frozen) {
        rb_car_drag_force(c, drag);
        memcpy(pts[n], c->body.x, 3 * sizeof(float));
        memcpy(f[n], drag, 3 * sizeof(float));
        n++;
    }

    /* 3. suspension force */
    memcpy(pts[n], c->body.x, 3 * sizeof(float));
    memcpy(f[n], fsusp, 3 * sizeof(float));
    n++;

    /* 4. suspension torque as a couple */
    rb_torque_to_couple(&c->body, tau, cf, cp);
    for (i = 0; i < 2; i++) {
        memcpy(pts[n], cp[i], 3 * sizeof(float));
        memcpy(f[n], cf[i], 3 * sizeof(float));
        n++;
    }

    /* 5. drive, environment drag and the lateral friction solve */
    if (!c->frozen && rb_contact_solve(c, dt, &n, pts, f)
        && fabsf(c->steer) < 0.2f)
        resting = 1;

    rb_sum_forces_torques(&c->body, (const float (*)[3])pts, n,
                          (const float (*)[3])f);

    /* Held on the line: keep only vertical force and roll/pitch torque, so the
       car settles onto its springs without moving or yawing. */
    if (c->frozen) {
        c->body.force[0] = 0.0f;
        c->body.force[2] = 0.0f;
        c->body.torque[1] = 0.0f;
    }

    /* Rest damper: add exactly the torque that cancels the remaining angular
       momentum. The original's rate is 2e6, so for any real dt the move-towards
       reaches zero in one step and this is simply torque -= L. It is what stops
       a parked car from creeping and jittering. */
    if (resting && c->rest_damp) {
        static const float ZERO[3] = { 0.0f, 0.0f, 0.0f };
        for (i = 0; i < 3; i++) {
            float t = rb_move_towards(c->body.L[i], ZERO[i], 2e+06f, dt);
            c->body.torque[i] = (float)(((double)t - c->body.L[i])
                                        + c->body.torque[i]);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* driver                                                                    */
/* ------------------------------------------------------------------------- */

/* 0x004f0270 -- one substep, bisecting toward the time of impact.
 *
 * Trial time is t_lo*a + (1-a)*t_hi with a = 0.8 on the first probe and after a
 * penetration, 0.5 once a clean step has been found, at most 5 probes. Returns
 * the time consumed.
 *
 * RESTRUCTURED, deliberately, and the reason matters. The original distinguishes
 * two query shapes: carCheckPenetration is a SOLID sphere test, while
 * carCheckContact passes mode8 = 1, which sets a second radius of 0.2r and flags
 * 0x404 -- a shell test, "near the surface but not deep inside". Those flags are
 * interpreted inside the engine's collision module, which is not transcribed, so
 * this port has only a solid test plus a tolerance.
 *
 * That difference bites: carUpdateSuspension deliberately parks each wheel about
 * radius*0.01 clear, so a resting car sits at effectively zero clearance and a
 * strict solid test flips on and off under sub-millimetre motion. Bisecting on
 * that stalls a car that is merely sitting on the ground.
 *
 * So instead of bisecting unconditionally, this tries the whole step first and
 * only bisects if that step ends up penetrating by more than the slack. Both
 * properties the original has are preserved: a grounded car advances at full
 * rate, and an approaching car is stopped at first contact.
 */
float rb_substep_contact(rb_car *c, float dt)
{
    float y0[RB_STATE_N], y1[RB_STATE_N];
    float a = 0.8f, t_lo = 0.0f, t_hi = dt, trial;
    int i;

    rb_car_get_state(c, y0);

    /* optimistic: take the whole step and keep it if nothing went inside */
    rb_euler_step(c, y0, dt, y1);
    rb_car_set_state(c, y1);
    if (!rb_collide(c, 0.0f, -RB_PENETRATION_SLACK, 2, -1, 0))
        return dt;

    for (i = 0; i < 5; i++) {
        trial = (float)((double)t_lo * a + (1.0 - a) * t_hi);

        rb_car_set_state(c, y0);
        rb_euler_step(c, y0, trial, y1);
        rb_car_set_state(c, y1);

        if (rb_collide(c, 0.0f, -RB_PENETRATION_SLACK, 2, -1, 0)) {
            a = 0.8f;                     /* penetrating: shrink */
            t_hi = trial;
            continue;
        }

        /* The accepted time advances on every non-penetrating probe, before the
           contact test -- verified at 0x4f0376, where the store is
           unconditional. Ghidra places it inside the branch. */
        t_lo = trial;
        if (rb_collide(c, 0.0f, RB_CONTACT_TOL, 2, -1, 0))
            return trial;                 /* just touching: stop here */
        a = 0.5f;
    }

    if (t_lo <= 0.0f) {
        /* Already inside geometry and no probe got out. Take the step anyway:
           freezing would be worse, and the suspension solve gets another go
           next frame. */
        rb_car_set_state(c, y0);
        rb_euler_step(c, y0, dt, y1);
        rb_car_set_state(c, y1);
        return dt;
    }

    rb_car_set_state(c, y0);
    rb_euler_step(c, y0, t_lo, y1);
    rb_car_set_state(c, y1);
    return t_lo;
}

/* 0x004f5b60 -- conservative advancement. Cap the step so that no collision
 * sphere travels more than 0.9 of its own radius, which is what guarantees the
 * contact search cannot step clean over thin geometry. Point velocities come
 * from rbPointVelocity, i.e. v + w x r, so a spinning car is limited by its
 * fastest sphere, not by the centre of mass.
 */
static float rb_ccd_limit(rb_car *c, float dt)
{
    float spheres[RB_MAX_SPHERES][4];
    int n, i;
    double limit = dt;

    if (!c->world || !c->world->sphere)
        return dt;

    n = rb_gather_spheres(c, spheres);
    for (i = 0; i < n; i++) {
        float vp[3];
        double sp, safe;

        if (spheres[i][3] <= 0.0f)
            continue;
        rb_point_velocity(&c->body, spheres[i], vp);
        sp = sqrt((double)vp[0] * vp[0] + (double)vp[1] * vp[1]
                  + (double)vp[2] * vp[2]);
        if (sp <= 0.001)
            continue;
        safe = ((double)spheres[i][3] * 0.9) / sp;
        if (safe < limit)
            limit = safe;
    }
    return (float)limit;
}

/* Did any BODY sphere sweep THROUGH a face between two poses?
 *
 * The port's stand-in for carSubstepContact's shell query -- see RB_TOI_PASSES in
 * rb.h for why this is a crossing test and not a penetration test, and why the
 * wheels are exempt. `pre` must have come from rb_gather_spheres at the earlier
 * pose; the car is currently AT the later one.
 */
static int rb_body_swept_through(rb_car *c, const float pre[][4], int npre)
{
    float post[RB_MAX_SPHERES][4];
    int n, i;

    if (!c->world || !c->world->segment)
        return 0;
    n = rb_gather_spheres(c, post);
    if (n > npre)
        n = npre;
    for (i = c->nwheels; i < n; i++) {
        float dx, dy, dz;
        if (pre[i][3] <= 0.0f)
            continue;
        dx = post[i][0] - pre[i][0];
        dy = post[i][1] - pre[i][1];
        dz = post[i][2] - pre[i][2];
        /* Below a tenth of a millimetre there is nothing to cross, and asking
           anyway is 9 segment queries a substep for no answer. */
        if (dx * dx + dy * dy + dz * dz < 1e-8f)
            continue;
        if (c->world->segment(c->world->ctx, pre[i], post[i]))
            return 1;
    }
    return 0;
}

/* 0x004f5e50 -- one frame.
 *
 * At most four substeps, in carPhysTick's own order (its call sequence at
 * 0x4f5fxx is carSubstepCCD, carUpdateSuspension mode 0, carSubstepContact,
 * carUpdateSuspension mode 1):
 *
 *   1. cap the step by conservative advancement -- no collision sphere may
 *      travel more than 0.9 of its own radius. THIS is what makes tunnelling
 *      impossible, and it is the whole of carSubstepCCD's contribution. The cap
 *      is then tightened to RB_MAX_SUBSTEP for integrator stability.
 *   2. solve the spring lengths against the world, and refresh the contacts
 *   3. advance by the capped step
 *   4. solve the spring lengths again, in slow mode
 *
 * Note what does NOT gate the advance: penetration. The primary step is limited
 * only by the sphere-travel cap, and any resulting overlap is absorbed by the
 * geometric suspension solve, which retracts the spring so the wheel stays at
 * the surface while the body keeps moving. Gating the advance on a solid
 * penetration test instead makes a landing car crawl at a few percent of real
 * time, because the springs are never given the chance to compress.
 *
 * What stops the car when the suspension runs out of travel is the `stuck`
 * return from carUpdateSuspension. Once a strut bottoms out at lenMin, retract
 * can no longer resolve the overlap, the solve reports failure, and the substep
 * is REFUSED -- the body does not advance. carPhysTick branches on exactly this
 * (its `iVar8` at 0x4f5f..). Without it a hard landing walks the car straight
 * through the floor, because a bottomed strut has no travel left to absorb it
 * and the model has no normal impulse of its own.
 *
 * rb_substep_contact is therefore not on this path. It is the corrective pass
 * for BODY collisions -- walls and scenery, which this port's world does not
 * report yet. See its comment.
 *
 * With no world attached this degenerates to a single full Euler step.
 *
 * The substep budget is RB_MAX_SUBSTEPS (8), not the original's 4, because the
 * substeps are now also bounded in time rather than only by sphere travel.
 */
float rb_car_tick(rb_car *c, float dt)
{
    float y0[RB_STATE_N], y1[RB_STATE_N];
    float pre[RB_MAX_SPHERES][4];
    float remaining = dt;
    int iter = 0, stuck, event, toi, npre = 0;

    if (!c->world || !c->world->sphere) {
        rb_car_get_state(c, y0);
        rb_euler_step(c, y0, dt, y1);
        rb_car_set_state(c, y1);
        return dt;
    }

    /* carPhysTick opens with the rest timers and then the rest clamp, and when
       the clamp fires it does NOTHING else for the whole frame -- no forces, no
       integration, just a fat penetration test to keep the contact timer alive.
       That is the engine putting a settled car to sleep; see rb_car_at_rest. */
    rb_car_rest_update(c, dt);
    if (rb_car_at_rest(c)) {
        c->asleep = 1;
        /* DIVERGENCE, and the same one rb_body_depenetrate exists for at all.
           The original does nothing here because it CANNOT arrive here inside
           geometry: carSubstepContact bisects every substep back to the moment
           of touching, so a body never ends a step overlapping and a car that
           falls asleep is by construction resting on the surface. This port has
           no bisection, so a car CAN settle while a body sphere is buried -- and
           once it does, the branch above used to run no forces, no integration
           and no depenetration ever again. It stayed buried permanently, which
           is the one state the sleep clamp must not be able to make stable.

           Position only, and it does not wake the car: pushing a settled car out
           of the ground is not motion it should be charged for. A car resting
           properly gets nothing -- rb_body_depenetrate returns 0 unless a body
           sphere is solidly overlapping. */
        rb_body_depenetrate(c);
        if (rb_collide(c, 0.0f, 0.03f, 2, -1, 0))
            rb_car_rest_touch(c);
        rb_car_update_matrix(c);
        return 0.0f;
    }
    c->asleep = 0;

    while (remaining > 0.0001f && iter < RB_MAX_SUBSTEPS) {
        float step;

        /* 0x004f5770, first thing in carPhysTick's loop body. */
        rb_clamp_momentum(c);

        step = rb_ccd_limit(c, remaining);

        /* Hold the substep at or below RB_MAX_SUBSTEP -- see the note in rb.h.
           Conservative advancement alone does not subdivide a slow-moving car,
           and one 60 Hz Euler step is unstable in roll. */
        if (step > RB_MAX_SUBSTEP)
            step = RB_MAX_SUBSTEP;
        if (step > remaining)
            step = remaining;

        if (step <= 0.0f)
            break;

        /* carGatherWheelContacts, and 0x004f5980 on its result. These are the
           contacts the derivative below will use, so they are gathered BEFORE
           the advance, exactly as at 0x4f5fc9. */
        if (rb_collide(c, 0.0f, RB_CONTACT_TOL, 0, -1, c->hit))
            rb_car_rest_touch(c);

        /* THE ADVANCE COMES FIRST. This is carSubstepCCD's position in
         * carPhysTick's loop, and the ordering is load-bearing for the DAMPER,
         * not just for tidiness.
         *
         * rb_susp_spring_damper's damping term is k_speed * dlen / dt, and `dlen`
         * is whatever the last suspension pass moved the length by. Solve the
         * suspension before the step, as this port used to, and the pose has not
         * changed since the previous substep's second pass already solved it -- so
         * `dlen` is not the body's motion at all, it is just the extension rate
         * limit, and a trace shows it pinned at 10*dt*radius = 2.992 mm frame
         * after frame. The damper then carries no information about how fast the
         * body is actually moving, which is how a suspension with a paper damping
         * ratio of 0.46 sustained a limit cycle at its own 1.46 Hz.
         *
         * Stepping first means the derivative uses the `dlen` from the PREVIOUS
         * substep's slow pass, which ran immediately after that substep's
         * advance and therefore does measure the geometric change the advance
         * caused. That is the original's signal.
         */
        rb_car_get_state(c, y0);
        npre = rb_gather_spheres(c, pre);
        rb_euler_step(c, y0, step, y1);
        rb_car_set_state(c, y1);

        /* TIME OF IMPACT. If that advance swept a body sphere through a face,
           walk it back to the largest step that did not -- carSubstepContact's
           job, and the only thing that stops the car ENDING a substep inside
           solid geometry. See RB_TOI_PASSES. */
        toi = 0;
        if (rb_body_swept_through(c, pre, npre)) {
            float lo = 0.0f, hi = step;
            int k;
            for (k = 0; k < RB_TOI_PASSES; k++) {
                float mid = 0.5f * (lo + hi);
                rb_euler_step(c, y0, mid, y1);
                rb_car_set_state(c, y1);
                if (rb_body_swept_through(c, pre, npre))
                    hi = mid;
                else
                    lo = mid;
            }
            /* Commit the last known-good pose. `step` itself is NOT reduced: the
               substep's worth of time has passed either way, and shortening it
               would leave `remaining` unspent and let the loop retry the same
               blocked advance until its budget ran out -- a stall rather than a
               wall. The body simply does not get there. */
            rb_euler_step(c, y0, lo, y1);
            rb_car_set_state(c, y1);
            toi = 1;
        }

        /* --- the contact solve (carPhysTick, 0x4f5f9c..0x4f6120) -------------
         *
         * The original gates this on a SOLID penetration test, in exactly this
         * order: the BODY spheres first, and only if they are clear does it run
         * the suspension and then ask whether a bottomed-out strut has left a
         * WHEEL overlapping. Either answer is a contact event, and an event runs
         * carContactFriction and then the impulse solve.
         *
         * The gate is what keeps all of this off a normally driving car: the
         * suspension holds the wheels clear, nothing penetrates solidly, and the
         * solve never runs. Ungated it would force every resting wheel to
         * 0.05 m/s of separation and launch the car off the ground.
         *
         * Structural divergence: the original reaches the solve through
         * carSubstepContact, which bisects the substep back to the moment of
         * touching so the body never ENDS a step inside geometry. That bisection
         * rests on the shell query this port does not have, so depenetration is
         * rb_body_depenetrate's job instead -- see its note in rb.h.
         */
        event = rb_collide(c, 0.0f, -RB_PENETRATION_SLACK, 1, -1, 0);
        /* A bisected substep IS a contact event even though it ends CLEAR of the
           surface -- that is the point of it. Without this the inward velocity is
           never absorbed: the car stops where it should, then presses into the
           face again every substep forever. rb_coll_list's RB_CONTACT_TOL window
           is what finds the touch the bisection stopped just short of, and
           rb_body_depenetrate is a no-op here because nothing is overlapping. */
        if (toi)
            event = 1;
        stuck = 0;
        if (!event) {
            stuck = rb_car_update_suspension(c, step, 0, 0);
            if (stuck && rb_collide(c, 0.0f, -RB_PENETRATION_SLACK, 0, -1, 0))
                event = 1;
        }

        if (event) {
            rb_coll_contact rec[RB_MAX_COLL_CONTACTS];
            int nrec;

            rb_body_depenetrate(c);
            nrec = rb_coll_list(c, RB_CONTACT_TOL, 2, rec,
                                RB_MAX_COLL_CONTACTS);
            if (nrec > 0) {
                rb_coll_friction(c, nrec, rec, step);
                rb_coll_resolve(c, nrec, rec);
            }
        }

        /* 0x004fbe60, the last call in carPhysTick's loop body. */
        rb_susp_len_extra(c);

        remaining = (float)((double)remaining - step);
        iter++;
    }
    return (float)((double)dt - remaining);
}
