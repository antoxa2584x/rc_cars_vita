/*
 * Vehicle physics for the RC Cars Vita port.
 *
 * The CONSTANTS AND CURVES here are the game's own, recovered from
 * Settings and Splines data and converted to physical units with the
 * scale constants taken out of the original loaders (see rccars_re/PHYSICS.md).
 *
 * The MODEL here is not. What follows is a conventional bicycle model with load
 * transfer and a spring/damper suspension, driven by the real numbers. Expect it
 * to behave in the right ballpark, not to match the original lap for lap.
 *
 * The game's actual model HAS since been recovered -- explicit Euler over a
 * 32-float Baraff rigid-body state -- and is transcribed in rb.c / rb.h. It is
 * not wired up here yet because its tire, engine and contact forces
 * (carContactSolve, 0x004edac0) are still being transcribed; rb.c currently
 * simulates body + gravity + drag + suspension only. When that lands, this file
 * goes away.
 *
 * NOTE: FUN_005074D0, named here previously as the integrator, is the car's
 * VISUAL update. See rccars_re/PHYSICS.md.
 */

#ifndef PHYSICS_H
#define PHYSICS_H

/* Ground query supplied by the host: returns 1 and fills *y (and the surface
   normal) if there is ground under (x,z) at or below ceil_y. */
typedef int (*ground_fn)(float x, float z, float ceil_y,
                         float *y, float *nx, float *ny, float *nz);

typedef struct {
    /* pose */
    float x, y, z;
    float yaw;               /* degrees; forward = (sin yaw, 0, -cos yaw) */
    float pitch, roll;       /* degrees, from suspension compression */

    /* motion */
    float vlong;             /* along heading, m/s */
    float vlat;              /* sideways, m/s */
    float yaw_rate;          /* deg/s */
    float vy;                /* vertical, m/s */

    /* suspension travel per corner: FL, FR, RL, RR */
    float susp[4];

    int car;                 /* 0..2 -> Overkill, Buggy, Hummer */
    int on_ground;
    int drifting;
    float boost;             /* 0..1 ramp */
} vehicle_t;

typedef struct {
    float throttle;          /* 0..1 */
    float brake;             /* 0..1 */
    float steer;             /* -1..1 */
    int boost;
} vehicle_input_t;

void vehicle_init(vehicle_t *v, int car, float x, float y, float z, float yaw);
void vehicle_step(vehicle_t *v, const vehicle_input_t *in, float dt, ground_fn ground);
const char *vehicle_car_name(int car);
float vehicle_speed(const vehicle_t *v);

#endif
