/*
 * rb.h -- RC Cars rigid-body vehicle core, transcribed from RCCars.exe.
 *
 * This is the GAME'S OWN model, not an approximation. The original is a
 * textbook Baraff rigid-body simulator (SIGGRAPH "Physically Based Modeling"
 * course notes): a 13-element state vector y = (x, q, P, L) advanced by
 * explicit Euler, with forces accumulated as (point, force) pairs and summed
 * into a force/torque accumulator about the centre of mass.
 *
 * RC Cars extends the state to 32 floats so that the per-wheel suspension
 * travel is saved and restored alongside the body -- the substep search
 * rewinds the whole 32-float vector when a trial step penetrates geometry.
 *
 * Original addresses (RCCars.exe, image base 0x400000):
 *
 *   0x004f5e50  carPhysTick              per-frame entry, <=4 substeps
 *   0x004f5b60  carSubstepCCD            conservative advancement
 *   0x004f0270  carSubstepContact        time-of-impact bisection
 *   0x004f5550  rbOdeStep                thunk
 *   0x004f5590  rbEulerStep              yout[i] = y[i] + dt*ydot[i], i<32
 *   0x004f5400  carStateDeriv            ydot = (v, .5*w(x)q, F, tau)
 *   0x004f5180  carGetState              body -> y[32]
 *   0x004f5290  carSetState              y[32] -> body, then derived quantities
 *   0x004f0910  carAccumForces           builds the (point, force) list
 *   0x00475100  rbSumForcesTorques       F = sum f, tau = sum (p - x) x f
 *   0x004749d0  rbTorqueToCouple         tau -> two opposed forces at +-r
 *   0x00474e00  rbUpdateInvInertiaWorld  Iinv = M' * Ibodyinv * M
 *   0x00474910  rbMat3MulVec3            w = Iinv * L
 *   0x00474ed0  rbPointVelocity          vp = v + w x (p - x)
 *   0x004ed970  carDragForce             -vhat * (Cd*|v|^2 + Cbearing)
 *   0x004f0be0  carSuspTorque            suspension torque, axis-scaled
 *   0x004f1000  carSuspForce             suspension force, normal-projected
 *   0x004f0d70  carSuspBuildForces       per-wheel (point, force) pairs
 *   0x004f0f80  suspSpringDamper         spring/damper with runaway clamp
 *   0x004edac0  carContactSolve          lateral friction: 2x2 simultaneous solve
 *   0x004ee280  carGatherContacts        picks <=2 contacts; recomputes tangents
 *   0x004ee5e0  carDriveForces           gear logic, engine drive at rear contacts
 *   0x004eea50  carEngineAccel           throttle/brake -> acceleration via curves
 *   0x004eeea0  carSurfaceDrag           per-wheel deep-sand / water drag
 *   0x004ed6a0  carContactRecord         builds a contact frame (normal/lat/fwd)
 *   0x004ee180  carTireGrip              upgrade x front/rear coefficient
 *   0x004ee130  carTireDrifting          handbrake + steer beyond angleDriftOn
 *   0x00476ba0  rbAngularAccel           Iinv * (tau - w x L)
 *   0x00476c50  rbPointAccelAlong        d/dt(v_point . dir) * dir
 *   0x00408c20  rbSolve2                 2x2 inverse embedded in a 3x3
 *   0x0040f830  curveEval                piecewise linear, tag 0x311
 *   0x004ef680  carWheelFrame            wheel centre = mount - (0,len,0), world
 *   0x004ef9e0  carGatherCollSpheres     nwheels wheel spheres + 9 body spheres
 *   0x004efe00  carCollide               sphere queries -> per-wheel contacts
 *   0x004fb340  carUpdateSuspension      solves each spring length geometrically
 *   0x004fb9e0  suspRetract              retract until the wheel sphere is clear
 *   0x004fbc50  suspExtend               extend, keep only if it finds ground
 *   0x004fbd70  wheelBuried              is the wheel sphere inside geometry
 *   0x004f3b80  carJump                  the Jump button: hop, or right the car
 *   0x00508600  carResetUpright          level the car in place, clear of geometry
 *
 * Convention notes, both load-bearing:
 *
 *   - Matrices are 4x4 ROW-MAJOR with ROW-VECTOR semantics: a point transforms
 *     as p' = p * M, translation lives in row 3 (+0x30). Consequently the
 *     world inverse inertia is built as M' * Ibodyinv * M, which is the usual
 *     R * Ibodyinv * R' read in the other convention.
 *   - Quaternions are stored (w, x, y, z).
 *   - Gravity is 10.0 m/s^2 (not 9.81), along -Y. Y is up.
 *
 * Precision: intermediates are computed in double and stored to float, which
 * reproduces the original x87 running at control word 0x27F (PC = 53-bit) with
 * float32 state. Build this file with -fno-fast-math -ffp-contract=off; see
 * rccars_re/PHYSICS.md for why.
 */

#ifndef RB_H
#define RB_H

#define RB_STATE_N     32   /* 13 body + 3*6 wheel + 1 aux */
#define RB_MAX_WHEELS   6
#define RB_MAX_FORCES  50   /* the original stack buffers hold 150 floats */

#define RB_GRAVITY   10.0f  /* m/s^2, applied along -Y */

/* The engine's own float32 constants, as they appear in the disassembly. */
#define RB_TWO_PI    6.2831855f
#define RB_RAD2DEG   57.295776f

/* Longest substep the integrator may take, and the iteration budget that goes
 * with it.
 *
 * This is a DIVERGENCE from the original, which caps substeps only by
 * conservative advancement (sphere travel) and allows four per frame. That is not
 * enough here: with the car barely moving, sphere travel never triggers a
 * subdivision, so a 60 Hz frame becomes a single 16.7 ms explicit Euler step --
 * and the roll mode is unstable at that rate. Measured on a perfect plane, a 2
 * degree slope diverges to 31 degrees of tilt error and 12 rad/s at 1/60, and
 * settles to a bounded ~6 degree wobble at 1/240 and finer. On exactly flat
 * ground there is no roll excitation, which is why that case always looked fine.
 *
 * 1/240 with a budget of 8 covers a 60 Hz frame in four substeps and a 30 Hz
 * frame in eight. */
#define RB_MAX_SUBSTEP   (1.0f / 240.0f)
#define RB_MAX_SUBSTEPS  8

/* Rigid body. Field comments give the offset inside the original body struct,
   which begins at car->phys + 0x57f4. */
typedef struct {
    float mass;             /* +0x00 */
    float inv_mass;         /* +0x04 */
    float ibody_inv[16];    /* +0x50  inverse body-space inertia, 4x4 */
    float x[3];             /* +0x90  position of the centre of mass */
    float q[4];             /* +0x9c  orientation (w, x, y, z) */
    float iinv[16];         /* +0xac  inverse world inertia, 4x4 */
    float v[3];             /* +0xec  linear velocity  (derived: P * 1/m) */
    float w[3];             /* +0xf8  angular velocity (derived: Iinv * L) */
    float P[3];             /* +0x104 linear momentum   -- state */
    float L[3];             /* +0x110 angular momentum  -- state */
    float force[3];         /* +0x11c accumulated force */
    float torque[3];        /* +0x128 accumulated torque */
} rb_body;

/* One suspension corner. Offsets are inside the original wheel record, which
   begins at car->phys + 0x5934 and is 0x68 bytes long. Three fields are ODE
   state; the rest is filled once at setup by rb_car_setup_springs.

   NOTE the sense of `len`: it is the CURRENT SPRING LENGTH, not compression.
   The spring pushes when len is below len_free and does nothing at or above it.
   Getting this backwards makes the car fall through the world. */
typedef struct {
    float radius;           /* +0x00  wheel radius (CdtRadWheel) */
    float mount[3];         /* +0x14  mount point, body space */
    float k_pos;            /* +0x20  stiffness -- DERIVED, see setup below */
    float k_speed;          /* +0x24  damping coefficient (kSpeed) */
    float len;              /* +0x28  current length     -- state y[13+i] */
    float len_free;         /* +0x2c  free length ("length") */
    float len_min;          /* +0x30  lenMin, absolute */
    float len_max;          /* +0x34  lenMax, absolute */
    float sag;              /* +0x38  kPos: static sag in metres */
    float dlen;             /* +0x3c  length delta        -- state y[19+i] */
    float len_extra;        /* +0x40  extra length        -- state y[26+i] */
    float drive[3];         /* +0x54  last drive force applied (telemetry) */
    /* Rolling state, purely visual: carWheelSpinUpdate (0x004ef280) integrates
       it and carAniProc1 (0x00504820) turns `spin` into the wheel node's rotation.
       Nothing in the dynamics reads any of these -- they are NOT in the 32-float
       ODE state, which is why a wheel keeps its spin across a substep rewind. */
    float spin;             /* +0x04  rolling angle, radians, wrapped to +-2pi */
    float spin_w;           /* +0x08  rolling rate, rad/s, damped toward target */
    float spin_target;      /* +0x0c  rate implied by the contact-patch velocity */
    float spin_extra;       /* +0x10  wheelspin/lock-up on top; see the tuning */
} rb_wheel;

/* Result of the host's collision query for one wheel. The original's collision
   layer produces a 0x98-byte record per wheel and the physics reads only these
   fields out of it; the surface classification and water probe are engine calls
   (0x00534fc0, 0x00531b10) that belong to the host, not to the dynamics. */
typedef struct {
    int   active;           /* +0x4c  nonzero: this wheel is touching ground */
    float point[3];         /* +0x58  contact point, world */
    float normal[3];        /* +0x70  contact normal, world */
    int   surface;          /* 0x00534fc0 classification; 3 == deep sand */
    /* 0x00531b10 found water AND this wheel's sphere reaches it. The gate is
       rb_collide's, not the probe's: rb_world.water answers per column, so on a
       pier deck over the sea it says yes three metres up. Everything that reads
       this -- the surface loop, the dust, the tyre marks -- means "wet". */
    int   in_water;
    float water_gap;        /* height of the wheel centre above the surface */
} rb_wheel_contact;

/* A contact frame, built by carContactRecord (0x004ed6a0). The original packs
   this into 0x84 bytes; only these fields drive the dynamics. */
typedef struct {
    int   wheel;            /* +0x00  which corner this came from */
    float point[3];         /* +0x0c  contact point, world */
    float r[3];             /* +0x18  point - centre of mass */
    float normal[3];        /* +0x24  contact normal */
    float lat[3];           /* +0x30  friction direction (lateral, or slide) */
    float fwd[3];           /* +0x3c  rolling direction, on the contact plane */
} rb_contact;

/* One of the three body collision spheres, from the cdt params. */
typedef struct {
    float offset[3];        /* CdtDeltaX/Y/Z(n), body space */
    float radius;           /* CdtRadUp(n) */
    float central_z;        /* CdtDeltaZ_CENTRAL(n); added only for the Buggy */
} rb_body_sphere;

/* One world-collision result. The original copies 15 floats of surface data
   into the wheel record; the port keeps only what the dynamics reads. */
typedef struct {
    float point[3];         /* the point on the surface */
    int   surface;          /* class; 3 == deep sand (0x00534fc0) */
    /* The surface's own OUTWARD normal at `point` -- which side of it is open
       space. All-zero means the host cannot say, and the two places that need it
       (rb_coll_list, rb_body_depenetrate) then fall back to world up, which is
       what they always did. col_sphere fills it from the triangle's winding.

       Without it a contact cannot tell "under a ceiling" from "sunk into the
       ground": both put the sphere centre BELOW the contact point, which is the
       test those two used to make. See the long note in rb_coll_list. */
    float normal[3];
} rb_world_hit;

/* Everything the physics needs from the level. In the original these are calls
   into the engine's collision module (0x00454970 sphere, 0x004557e0 segment,
   0x00531b10 water probe); here they are the host's, which is the natural seam
   for the Vita port's own .col grid. */
typedef struct {
    /* Sphere against world. Return nonzero if touching, and fill up to
       max_hits results. Mirrors 0x00454970 with query type 4. */
    int (*sphere)(void *ctx, const float centre[3], float radius,
                  rb_world_hit *hits, int max_hits, int *n_hits);
    /* Segment against world. Nonzero if it crosses geometry. 0x004557e0. */
    int (*segment)(void *ctx, const float a[3], const float b[3]);
    /* Water under a wheel: nonzero if present, `gap` = height of the wheel
       centre above the surface. 0x00531b10.
     *
     * `centre` is that wheel's sphere centre, world space, and is the port's
     * one addition to the original's argument list. The engine does not need it:
     * its WaterLOD module fills a per-wheel record (phys+0x5d40) as it animates
     * the surface, and 0x00531b10 only reads the record back. The port has no
     * such per-wheel bookkeeping -- the host answers the query on the spot from
     * the track's water-height grid -- so it has to be told WHERE. */
    int (*water)(void *ctx, int wheel, const float centre[3], float *gap);
    /* OPTIONAL, and only for spawn placement: highest surface at (x,z) NOT ABOVE
       `ceil_y`, with its normal. Not part of the transcribed model -- the
       original places cars from the track's start markers. Without it rbcar_init
       cannot rest a car on a slope, and starting a car part-buried on a slope is
       what makes it tumble.
     *
     * `ceil_y` is load-bearing and was added after this probed the whole column.
     * A race start under an overpass then resolved to the overpass: on beach_2
     * the ground is at y = 3.19 and the unbounded probe returned 10.08, so the
     * car spawned seven metres up on a roof, rolled off its edge and fell out of
     * the world. The caller already knows which surface it means -- it passes the
     * height it placed the car at -- so the probe must not be free to pick
     * another one. */
    int (*ground)(void *ctx, float x, float z, float ceil_y,
                  float *y, float n[3]);
    void *ctx;
} rb_world;

/* A piecewise-linear curve. The original stores these as spline objects with
   type tag 0x311 and a precomputed slope per segment (0x0040f830 evaluates
   them); outside the range it clamps to the endpoint value. */
typedef struct { float in, out; } rb_curve_pt;
typedef struct { const rb_curve_pt *pt; int n; } rb_curve;

/* Per-car tuning, read from the game's own .crs files under Settings. These are the
   converted physical values -- see rccars_re/PHYSICS.md and physics_data.h.
   The original reaches them through per-car global arrays indexed by
   car->phys + 0x54, via the getters at 0x004f73d0..0x004f74a0. */
typedef struct {
    /* springs, cfgSpringParams -> 0x014c4c58 + i*0x38 */
    float coeff_moment_ox;          /* springs[0] */
    float coeff_moment_oz;          /* springs[1] */
    /* springs[11] lenAxe. NOT the mesh half-track (0.089 against 0.141 on the
       Overkill) -- the visual axle angle divides the mean wheel drop by it, and
       nothing in the dynamics reads it. carAniProc1 0x00504820. */
    float len_axe;
    /* springs[12] angleProcUpMax and springs[13] angleProcDownMax, degrees.
       Also purely visual: they are the two ends of the clamp carAniProc2
       (0x00505780) puts on the Buggy's wishbone angle, swapped for the
       right-hand pair. Both convert 1..100 -> 0..90 through the remap at
       0x004f9010 / 0x0040ba70, and both are 0 on the Overkill and the Hummer,
       whose procs never read them -- springs2.ini is the only one of the three
       that ships a non-zero default. carani.c. */
    float angle_proc_up, angle_proc_down;
    /* resistance, cfgResistParams -> 0x014c4b90 + i*0x08 */
    float coeff_friction_bearings;  /* resist[0] */
    float coeff_air_resistance;     /* resist[1] */
    /* tires, cfgTireParams -> 0x014c4ae8 + i*0x34 */
    float coeff_front_tires;        /* tire[0] */
    float coeff_rear_tires;         /* tire[1] */
    float angle_drift_on;           /* tire[3]  degrees of steer to start a drift */
    float speed_drift_on;           /* tire[4] */
    float coeff_drift_rear;         /* tire[5]  rear grip multiplier while drifting */
    float coeff_deep_sand;          /* tire[6] */
    float coeff_water;              /* tire[7] */
    float tire_upgrade[4];          /* tire[8..11]  UPGRADES.ini [TIRES] */
    /* tire[12] SpeedAngMaxREL -- extra wheel spin (rad/s) under throttle, or
       lock-up under brakes, blended out between 0 and 4 m/s. physLoadTires reads
       it with the FLOAT getter (0x00404950), and no shipped tires<n>.crs carries
       the key, so the getter leaves the global at its BSS zero and the effect is
       off in the retail game. Kept because editing the .crs turns it on. */
    float speed_ang_max_rel;        /* tire[12] */
    /* engine, cfgEngineParams -> 0x014c46d0 + i*0x4c */
    float speed_base_max;           /* engine[2]  km/h -- see PHYSICS.md */
    float speed_boost_max;          /* engine[3]  km/h */
    float boost_ratio;              /* engine[4] = speed_boost_max/speed_base_max */
    float resonator_speed[4];       /* engine[11..14] RESONATORS <car>Upgrades */
    float resonator_accel[4];       /* engine[15..18] RESONATORS <car>Upgrades_SPL_OY */
    rb_curve accel;                 /* engine[6]  <CAR>.bspl */
    rb_curve restrict_;             /* engine[5]  <CAR>_RESTRICT.bspl */
    /* collision proxy, cfgCdtParams -> 0x014c4d00 + i*0xb4 */
    float cdt_rad_wheel;            /* cdt[1]  CdtRadWheel */
    float cdt_rad_back;             /* cdt[3]  CdtRadBackWheels * CdtRadWheel */
    float cdt_front_x;              /* cdt[15] front-wheel X offset */
    float cdt_side_x;               /* cdt[43] shiftRoofBaseLefter */
    rb_body_sphere body_sphere[3];  /* cdt[11..13] radii, cdt[18..26] offsets */
} rb_tuning;

/* Driver inputs. The original keeps these as bits and floats inside phys;
   offsets are noted so the mapping stays checkable. */
typedef struct {
    int   accel;            /* phys+0x576c bit0 */
    float throttle;         /* phys+0x5770  0..1 */
    int   brake;            /* phys+0x5774 bit0 -- also the drift trigger */
    float brake_amount;     /* phys+0x5778  0..1 */
    int   blocked;          /* phys+0x5784 bit0: drive inhibited by the game */
    int   boost;            /* phys+0x573c */
    /* phys+0x577c bit0 -- the Jump action. The original's input layer numbers
       its eight player actions 1 Boost, 2 Left, 3 Right, 4 Forw, 5 Back,
       6 JUMP, 7 Brake, 8 Reset (the binding table at 0x004e4188 onward), and
       FUN_005029c0 decodes control bit 0x04 into this field. Read by
       rb_car_jump and by the rest clamp's wake test. */
    int   jump;
} rb_input;

typedef struct rb_car rb_car;

struct rb_car {
    rb_body   body;
    rb_wheel  wheel[RB_MAX_WHEELS];
    rb_wheel_contact hit[RB_MAX_WHEELS];  /* filled by the host each substep */
    int       nwheels;      /* phys + 0x5928 */
    int       susp_enabled; /* phys + 0x48 */
    int       max_contacts; /* cap on simultaneous friction contacts */
    float     m[16];        /* car + 0xf8: body-to-world, row-vector 4x4 */
    float     steer;        /* phys + 0x5c6c, degrees, carried as state y[25] */
    int       gear;         /* phys + 0x57f0: 0 forward, -1 reverse */
    int       drive_blocked;/* phys + 0x57ec */
    int       tire_upgrade; /* phys + 0xe45c + i*0xb0, 0..3 */
    int       reso_upgrade; /* phys + 0xe454 + i*0xb0, 0..3 */
    int       car_index;    /* phys + 0x54: 0 Overkill, 1 Buggy, 2 Hummer */
    int       embedded;     /* phys + 0x0c: a wheel is buried in geometry */
    float     susp_ramp;    /* phys + 0x08: extension-rate ramp, 0..0.5 */
    const rb_world *world;  /* host collision, see above */
    int       frozen;       /* phys + 0xa4: held on the line -- no drag, no
                               contacts, and horizontal force / yaw torque are
                               zeroed after summing, so it can only settle */
    int       rest_damp;    /* phys + 0x04 == 0 enables the rest damper */
    /* The rest clamp's four timers (0x004f59a0). See rb_car_rest_update. */
    float     rest_slow_t;  /* phys + 0x5c74: time below 0.3611 m/s */
    float     rest_spin_t;  /* phys + 0x5c78: time below 1.0 rad/s */
    float     no_contact_t; /* phys + 0x5c7c: time since a contact was found */
    float     rest_ground_t;/* phys + 0x5c80: time continuously in contact */
    int       asleep;       /* telemetry: the rest clamp fired this frame */
    /* The Jump action's two pieces of state (0x004f3b80). `jump_t` is the
       cooldown, advanced every frame and zeroed by a successful hop;
       `jump_latch` is the button edge detector, so holding Jump hops once. */
    float     jump_t;       /* phys + 0x6d4 */
    int       jump_latch;   /* phys + 0x123a0 */
    rb_input  in;
    rb_tuning tune;
};

/* --- vector / quaternion / matrix primitives ------------------------------ */

void  rb_quat_mul(const float a[4], const float b[4], float out[4]);   /* 0x4079f0 */
void  rb_quat_scale(float q[4], float s);                              /* 0x407990 */
void  rb_quat_normalize(float q[4]);                                   /* 0x4078d0 */
void  rb_quat_to_matrix(const float q[4], float m[16]);                /* 0x407d40 */
void  rb_mat4_mul(const float a[16], const float b[16], float out[16]);/* 0x40c7f0 */
void  rb_mat3_mul_vec3(const float m[16], const float v[3], float o[3]);/* 0x474910 */

/* --- rigid body ---------------------------------------------------------- */

void  rb_update_inv_inertia_world(rb_body *b);                         /* 0x474e00 */
void  rb_point_velocity(const rb_body *b, const float p[3], float o[3]);/* 0x474ed0 */
void  rb_sum_forces_torques(rb_body *b, const float pts[][3], int n,
                            const float f[][3]);                       /* 0x475100 */
void  rb_torque_to_couple(const rb_body *b, const float tau[3],
                          float f[2][3], float p[2][3]);               /* 0x4749d0 */

/* --- state marshalling --------------------------------------------------- */

void  rb_car_get_state(const rb_car *c, float y[RB_STATE_N]);          /* 0x4f5180 */
void  rb_car_set_state(rb_car *c, const float y[RB_STATE_N]);          /* 0x4f5290 */
void  rb_car_state_deriv(rb_car *c, float dt, float ydot[RB_STATE_N]); /* 0x4f5400 */
void  rb_euler_step(rb_car *c, const float y[RB_STATE_N], float dt,
                    float yout[RB_STATE_N]);                           /* 0x4f5590 */

/* --- forces -------------------------------------------------------------- */

void  rb_car_drag_force(const rb_car *c, float out[3]);                /* 0x4ed970 */
float rb_susp_spring_damper(const rb_wheel *wh, float dt);             /* 0x4f0f80 */
void  rb_car_susp_build(rb_car *c, float dt, int *n, float pts[][3],
                        float f[][3], int project_normal);             /* 0x4f0d70 */
void  rb_car_susp_torque(rb_car *c, float dt, float out[3]);           /* 0x4f0be0 */
void  rb_car_susp_force(rb_car *c, float dt, float out[3]);            /* 0x4f1000 */
void  rb_car_accum_forces(rb_car *c, float dt);                        /* 0x4f0910 */
float rb_move_towards(float cur, float target, float rate, float dt);  /* 0x49d7e0 */

/* 0x004f1930 -- derive each corner's runtime stiffness from the config.
 *
 *   k_pos = (mass * 10) / (nwheels * sag)
 *
 * so that the static equilibrium compression is exactly `sag`. That is what
 * the config's `kPos` key actually means: the suspension sag in metres, NOT a
 * spring rate. Call after setting mass, nwheels and each corner's sag. */
void  rb_car_setup_springs(rb_car *c);

/* --- tires, engine, contacts (contact.c) --------------------------------- */

float rb_curve_eval(const rb_curve *cv, float x);                      /* 0x40f830 */
float rb_tire_grip(const rb_car *c, int rear);                         /* 0x4ee180 */
int   rb_tire_drifting(const rb_car *c);                               /* 0x4ee130 */
float rb_engine_accel(rb_car *c, float *restrict_out);                 /* 0x4eea50 */

void  rb_angular_accel(rb_body *b, const float pts[][3], int n,
                       const float f[][3], float out[3]);              /* 0x476ba0 */
void  rb_point_accel_along(rb_body *b, const float p[3], const float dir[3],
                           const float pts[][3], int n, const float f[][3],
                           float out[3]);                              /* 0x476c50 */
int   rb_solve2(const float A[2][2], const float b[2], float f[2]);    /* 0x408c20 */

void  rb_contact_record(const rb_car *c, int wheel, float steer_deg,
                        rb_contact *out);                              /* 0x4ed6a0 */
void  rb_surface_drag(rb_car *c, int *n, float pts[][3], float f[][3]);/* 0x4eeea0 */
int   rb_drive_forces(rb_car *c, float dt, int *n, float pts[][3],
                      float f[][3]);                                   /* 0x4ee5e0 */
int   rb_gather_contacts(rb_car *c, float dt, rb_contact *rec, int *nrec,
                         int *n, float pts[][3], float f[][3]);        /* 0x4ee280 */
int   rb_contact_solve(rb_car *c, float dt, int *n, float pts[][3],
                       float f[][3]);                                  /* 0x4edac0 */

/* --- collision (collide.c) ----------------------------------------------- */

#define RB_MAX_SPHERES  (RB_MAX_WHEELS + 9)   /* wheels + 3 centre + 3 left + 3 right */

/* 0x004ef680 -- the wheel's world frame. `centre` is the wheel centre, which is
 * the mount point dropped along the body Y axis by the current spring length.
 * `roll` is the rolling direction (local +Z steered), `side` the axle
 * direction. Any out-parameter may be NULL. */
void rb_wheel_frame(const rb_car *c, int i, int use_extra, float centre[3],
                    float *radius, float roll[3], float side[3]);

/* 0x004ef280 -- roll the wheels for the renderer. Once per FRAME, not per
 * substep: the original calls it from the car's frame update (0x004f6ea0), after
 * the physics has run. Each wheel's target rate is the contact-patch velocity
 * along its rolling direction over its radius; the rate chases that at 200 rad/s²
 * while any wheel is on the ground and 5 rad/s² when the car is airborne, so a
 * jump keeps its wheel speed. Purely visual -- see rb_wheel's spin fields. */
void rb_wheel_spin_update(rb_car *c, float dt);

/* 0x004ef9e0 -- the collision proxy: one sphere per wheel, then three body
 * spheres on the centre line and the same three offset left and right.
 * Writes (x, y, z, radius) quads; returns the count.
 *
 * The three body slots are the chassis station (recovered, from the cdt keys)
 * and two ROOF stations -- which is what the engine's own names for the absent
 * slots say they are, shiftRoofBaseLefter. No shipped .crs carries the keys for
 * slots 1 and 2, so gen_rb_data.py fits them to ENVIR_CAR_BODY's top face
 * instead; without them a car on its roof has no proxy under the roof and the
 * shell sinks into the terrain. See gen_rb_data.py and rccars_re/flipped.c. */
int  rb_gather_spheres(const rb_car *c, float out[][4]);

/* 0x004efe00 -- run the sphere queries and turn the results into per-wheel
 * contacts. `mode`: 0 wheels only, 1 body only, 2 everything. Returns nonzero if
 * anything was hit. Pass NULL for hit_out to use it purely as a penetration test
 * (0x004efdc0 / 0x004f01f0).
 *
 * `tol` is added to every radius and MUST be RB_CONTACT_TOL for the per-wheel
 * gather: suspRetract leaves the wheel clear by radius*0.01, so a zero-tolerance
 * query reports no contact at all. `opaque` matches the original's unused
 * body-pointer argument. */
/* Time-of-impact bisection passes on the BODY spheres -- the port's stand-in for
   carSubstepContact (0x004f0270), which the original reaches through the shell
   query this port does not have.
 *
 * The failure it exists for was photographed on hardware: a car driven at
 * beach_2's 0.23 m gas-station kerb ends up INSIDE it, resting, reporting
 * `contacts=1111 emb=0 spd=0` -- perfectly happy. Probing that pose, only 2 of
 * the 13 collision spheres overlapped anything, both WHEELS, both against the
 * kerb's TOP face. Not one body sphere reported contact, because col_sphere is a
 * closest-point query and a sphere buried deeper inside a solid than its own
 * radius is near nothing at all. So `emb` stays 0, rb_body_depenetrate finds
 * nothing to push, and the car occupies the concrete indefinitely.
 *
 * Depenetration cannot fix that -- it needs the overlap it cannot see. The cure
 * has to stop the car ENTERING, which is what the original's bisection does: it
 * walks the substep back to the moment of touching so the body never ends a step
 * inside geometry.
 *
 * Gating on a SEGMENT CROSSING rather than on penetration is the whole trick, and
 * the distinction matters -- rb_car_tick's own note records that gating the
 * advance on a penetration test makes a landing car crawl, because the springs
 * are never allowed to compress. A resting or settling body sphere does not sweep
 * THROUGH a face; only one entering solid matter does.
 *
 * WHEELS ARE DELIBERATELY EXEMPT. carUpdateSuspension resolves a touching wheel
 * geometrically by retracting the strut, and that overlap is the recovered
 * behaviour, not a fault. Gating on it would refuse every landing.
 *
 * 6 passes bisect a substep to 1/64 of itself; at RB_MAX_SUBSTEP that is 65 us,
 * and a body sphere moving at the 20 m/s momentum clamp covers 1.3 mm in it --
 * comfortably inside RB_CONTACT_TOL, so the contact solve still sees the touch. */
#define RB_TOI_PASSES 6

#define RB_CONTACT_TOL   0.006f   /* carPhysTick's tolerance, call site 0x4f5fc9 */

/* Slack allowed before a solid overlap counts as penetration, applied as a
 * negative tolerance. This stands in for the original's SHELL query
 * (carCheckContact passes mode8 = 1, setting an inner radius of 0.2r and flags
 * 0x404, interpreted inside the engine's untranscribed collision module).
 * Without it the substep search stalls: carUpdateSuspension parks each wheel
 * only radius*0.01 clear, so a resting car sits at effectively zero clearance
 * and a strict solid test flips on and off under sub-millimetre motion.
 * 1 mm is ~1.4% of a wheel radius, the same scale the retract works at. */
#define RB_PENETRATION_SLACK  0.001f
int  rb_collide(rb_car *c, float opaque, float tol, int mode, int limit,
                rb_wheel_contact *hit_out);

/* --- the collision-contact solve (0x004f0560 / 0x004f0750) ---------------- */

/* One collision contact, as carCollide (0x004efe00) writes it into its 0x98-byte
 * record. The impulse solve reads exactly three fields out of it:
 *   +0x58 point, +0x70 normal, +0x94 nonzero for a WHEEL sphere.
 * The normal points from the surface point back toward the sphere centre, so it
 * is the separating direction and an approaching body has a NEGATIVE relative
 * normal velocity. */
typedef struct {
    float point[3];
    float normal[3];
    int   is_wheel;
} rb_coll_contact;

/* FUN_004f6730 caps its own copy of this list at 0x14. */
#define RB_MAX_COLL_CONTACTS  20

/* 0x004efe00, with the contact list kept rather than folded into the per-wheel
 * records. Same gather, same normals, same `tol` and `mode` as rb_collide;
 * returns the number written. */
int  rb_coll_list(rb_car *c, float tol, int mode, rb_coll_contact *out, int max);

/* 0x004754a0 -- the denominator of a contact impulse:
 *     k = 1/m + n . ((Iinv (r x n)) x r),      r = point - centre of mass
 * so an impulse of (dv / k) * n changes the relative normal velocity at `point`
 * by exactly dv. This is the same quantity the tire friction matrix is built
 * from, which is why a wrong centre-of-mass height starves both. */
double rb_impulse_denom(rb_car *c, const float point[3], const float n[3]);

/* 0x004756c0 -- apply an impulse at a world point: P += j, L += r x j, then
 * rederive v and w (the original reaches the rederive through 0x004757a0 with
 * dt = 0). Changing L is the whole point: this is what lets the contact solve
 * arrest a car's ROTATION, which a momentum projection cannot do. */
void rb_apply_impulse(rb_car *c, const float point[3], const float j[3]);

/* 0x004f0560 -- tangential friction for the body shell. Takes the FIRST contact
 * that is not a wheel and whose contact-point velocity has a tangential
 * component, and applies an impulse of -min(|v_t|, dt*10) along it. One contact
 * only, then it returns -- that is the original, not a simplification. This is
 * what stops a car lying on its roof from sliding forever. */
void rb_coll_friction(rb_car *c, int n, const rb_coll_contact *rec, float dt);

/* 0x004f0750 (+ 0x004f0840) -- the normal contact solve: up to ten Gauss-Seidel
 * passes over the list, each giving every contact whose relative normal velocity
 * is at or below 0.02 m/s enough impulse to bring it to 0.05 m/s of separation.
 * Velocity level only -- it does not depenetrate, which the original does not
 * need because carSubstepContact bisects the step back to the touching moment
 * first. Returns nonzero if it converged, 0 if it ran out of passes. */
int  rb_coll_resolve(rb_car *c, int n, const rb_coll_contact *rec);

/* --- the rest clamp (0x004f59a0 / 0x004f6610 / 0x004f5980) ---------------- */

/* MAX_IMPULSE from the engine config: `maxImpLinear` 20.0 m/s and `maxImpAng`
 * 1.0 kg.m^2/s, loaded at 0x004f777d..0x004f77a4 and defaulted to those values in
 * the image. No shipped settings file overrides them. */
#define RB_MAX_IMP_LINEAR  20.0f
#define RB_MAX_IMP_ANG     1.0f

/* 0x004f5770 -- clamp |P| to maxImpLinear * mass and |L| to maxImpAng, at the top
 * of every substep. It does NOT rederive v and w, so they stay one substep stale
 * when it fires; that is the original and it is harmless because the clamp only
 * bites on a fall past 20 m/s or a tumble past |L| = 1. */
void rb_clamp_momentum(rb_car *c);

/* 0x004f59a0 -- the four rest timers, once per tick.
 *
 * Any control input resets them all and parks the no-contact timer at 1999872.0
 * (0x49f42400), which is how "being driven" is expressed. Otherwise: time below
 * 0.3611111 m/s (measured on the LARGER of |v| and |P|, which for a 2 kg car is
 * always |P|), time below 1.0 rad/s, time since the last contact, and time
 * continuously in contact. */
void rb_car_rest_update(rb_car *c, float dt);

/* 0x004f5980 -- contacts were found: reset the no-contact timer. */
void rb_car_rest_touch(rb_car *c);

/* 0x004f6610 -- THE REST CLAMP, and it is not a damper: if the car has been
 * slower than 0.3611 m/s for over 2 s, slower than 1.0 rad/s for over 2 s, and in
 * contact for over 1 s, it ZEROES P, L, v and w and returns nonzero -- and
 * carPhysTick then skips the entire physics step for that frame, doing nothing but
 * a fat (tol 0.03) penetration test to refresh the contact timer.
 *
 * So the engine puts a settled car to SLEEP outright. Nothing in this port did,
 * which is why a parked car could sit on a 1 degree slope trading spring lag for
 * body tilt forever: the limit cycle is real, and the original simply stops
 * integrating before it can develop. It wakes on any input, or when the car loses
 * contact for more than 0.1 s.
 *
 * This is also what makes rb_car.frozen work on the start line. */
int  rb_car_at_rest(rb_car *c);

/* 0x004fbe60 -- len_extra[i] = radius[i] * 0.02, every substep.
 *
 * It really is that: a constant 2% of the wheel radius (1.4 mm on the Overkill),
 * re-asserted rather than integrated. It sits in the 32-float ODE state with a
 * zero derivative so the substep search can roll it back, and it offsets the
 * radius*0.01 clearance suspRetract deliberately leaves, so the spring force
 * sees the compression the geometry actually has. Nothing in this port wrote it
 * before, which left every spring reading 1.4 mm slack. */
void rb_susp_len_extra(rb_car *c);

/* PARTLY TRANSCRIBED -- the POSITIONAL half of the body-sphere contact solve.
 *
 * The velocity half is transcribed now (rb_coll_friction + rb_coll_resolve). What
 * is still the port's own is depenetration: the original never needs it, because
 * carSubstepContact bisects the substep back to the moment of touching so the
 * body never ends a step inside geometry, and that bisection depends on the shell
 * query (carCheckContact's mode8 = 1, inner radius 0.2r, flags 0x404) which lives
 * inside the engine's untranscribed collision module.
 *
 * So this pushes the SINGLE deepest overlapping body sphere out along its contact
 * normal and stops. One sphere, not all of them: lifting the body once per
 * penetrating sphere accumulates, and with six spheres on the ground a rolled car
 * climbed out of the world instead of resting on it.
 *
 * Returns nonzero if anything moved. */
int  rb_body_depenetrate(rb_car *c);

/* 0x004f6fd8 (inside FUN_004f6ea0, the per-frame car loop) -- advance the
 * suspension extension-rate ramp at phys+0x08 by one FRAME's dt. It is a plain
 * time accumulator: carUpdateSuspension resets it whenever it retracts a wheel at
 * low speed, and the extension rate ramps 1x -> 10x over the half second after
 * that. Call once per frame, before the substep loop. */
void rb_susp_ramp_advance(rb_car *c, float dt);

/* 0x004fbd70 / 0x004fb9e0 / 0x004fbc50 */
int  rb_wheel_buried(const rb_car *c, int i);
int  rb_susp_retract(rb_car *c, int i, float max_step);
void rb_susp_extend(rb_car *c, int i, float rate);

/* 0x004fb340 -- solve every spring length against the world and set `dlen`.
 * Call once per substep BEFORE rb_car_tick: the lengths are ODE state with a
 * zero derivative precisely because this function, not the integrator, sets
 * them. Returns nonzero if a wheel could not be resolved ("stuck"). */
int  rb_car_update_suspension(rb_car *c, float dt, int slow_mode,
                              int *embedded_out);

/* 0x004f0270 -- corrective substep: bisect toward the time of first contact.
 * NOT on rb_car_tick's path; see its comment in rb.c. It is the pass for BODY
 * collisions, which this port's world does not report yet. Exposed so it does
 * not rot silently. */
float rb_substep_contact(rb_car *c, float dt);

/* --- the Jump action (0x004f3b80) and its reset (0x00508600) -------------- */

/* Jump is one of the original's eight player actions, and it does two different
 * things depending on which way up the car is. carJump reads the car's own up
 * axis (matrix row 1) against world up:
 *
 *   within RB_JUMP_FLIP_DEG    a HOP: an impulse of RB_JUMP_SPEED * mass along
 *                              the car's up axis, and yaw momentum x 1.5
 *   beyond it                  a RESET: rb_car_reset_upright, which levels the
 *                              car where it stands
 *
 * Either way it first has to be TOUCHING something -- the hop tests the wheel
 * spheres (rb_collide mode 0), the reset tests the body spheres (mode 1), both at
 * RB_CONTACT_TOL. You cannot hop in mid-air, and you cannot right a car that is
 * still tumbling through the air.
 */
#define RB_JUMP_COOLDOWN     0.5f    /* 0x554384: seconds between hops */
#define RB_JUMP_FLIP_DEG    80.0f    /* 0x554958: past this, Jump resets */
#define RB_JUMP_SPEED        3.0f    /* 0x5543f4: m/s along the body up axis */
#define RB_JUMP_SPEED_BUGGY  3.3000002f  /* 0x5549ac: car index 1 hops higher */
#define RB_JUMP_SPIN_MULT    1.5f    /* 0x554824: L.y multiplier on a hop */
#define RB_JUMP_LIMIT         0x32   /* the contact-list cap at the call site */

/* What rb_car_jump did this frame. */
#define RB_JUMP_NONE   0
#define RB_JUMP_HOP    1
#define RB_JUMP_RESET  2

/* 0x004f3b80 -- once per FRAME, not per substep, and BEFORE rb_car_tick: the
 * original calls it from FUN_004f6b20, which the per-frame car loop
 * (FUN_004f6ea0) runs ahead of the physics tick at 0x004f72f0. Advances the
 * cooldown by `dt` and acts on the rising edge of c->in.jump. */
int  rb_car_jump(rb_car *c, float dt);

/* 0x00508600 -- put the car back on its wheels where it stands.
 *
 * Zeroes v, w, P, L and both accumulators; levels the body about world up while
 * keeping its heading (the flattened, renormalised local +Z); then pushes the
 * body origin out of geometry with a RB_RESET_CLEAR_RADIUS sphere, at most
 * RB_RESET_CLEAR_PASSES times.
 *
 * The original prefers the direction of the track spline the car is nearest
 * (FUN_004873c0 / 0x00406660) and falls back to the car's own forward; the port
 * has no spline bound to the car, so it always takes the fallback. Same code
 * path in the original, just always the second branch. */
#define RB_RESET_CLEAR_RADIUS  0.5f  /* the query's sphere radius, 0x3f000000 */
#define RB_RESET_CLEAR_PASSES  10
void rb_car_reset_upright(rb_car *c);

/* --- driver -------------------------------------------------------------- */

/* One full frame. Advances by at most `dt`, subdividing into at most 4
   substeps, and returns the time actually consumed. Mirrors carPhysTick
   (0x4f5e50) minus the collision-geometry queries. */
float rb_car_tick(rb_car *c, float dt);

/* Rebuild c->m from the body pose. carSetState does this via the engine's
   scene-node update; here it is explicit. */
void  rb_car_update_matrix(rb_car *c);

#endif /* RB_H */
