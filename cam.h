/*
 * cam.h -- RC Cars' follow camera, transcribed from RCCars.exe.
 *
 * The chain in the original:
 *
 *   0x005007a0  camUpdate          per frame: build the reference frame, fill the
 *                                  four follower descriptors, update the camera
 *   0x00500840  camRefFrame        the car's position and basis, with a special
 *                                  case when the car is pointing near-vertically
 *   0x00500a80  camSetupTargets    copies the loaded defaults, then applies the
 *                                  speed and throttle pull-back
 *   0x00471c00  camSetFollowers    installs the four descriptors on the camera
 *   0x00471ca0  camFollow          the actual placement
 *   0x004722c0  camFollowStep      one follower: deadzone, slack window, rate and
 *                                  acceleration limit, eased pull-back to rest
 *   0x004f9da0  physLoadCamera     Settings/Camera.crs -> the defaults
 *
 * Three quantities are tracked independently, each by its own follower:
 * horizontal distance, height, and yaw. That is what gives the camera its
 * character -- the yaw one has a +-20 degree slack window, so the view swings
 * behind the car rather than being welded to it.
 *
 * The recovered numbers (Settings/Camera.crs, in rb_data.h) put the camera
 * 0.79 m behind and 0.36 m above the car. They are small because these are 1:10
 * scale models: a 0.5 m car doing 27 km/h on 0.072 m wheels.
 *
 * DIVERGENCE, deliberate: the original expresses the car in the camera's PREVIOUS
 * frame and rebuilds from there, an incremental formulation whose remaining half
 * (the matrix assembly in camFollow) is not transcribed. This works in the car's
 * frame instead, which is equivalent while the camera trails the car -- the case
 * that matters -- and is far easier to read. The follower shape and every
 * constant are the originals.
 */

#ifndef CAM_H
#define CAM_H

#include "rb.h"

/* One follower. Field order matches the original's 7-float descriptor. */
typedef struct {
    float rest;       /* [0] value it is pulled back to */
    float dead;       /* [1] deadzone: inside this, no offset at all */
    float slack_lo;   /* [2] how far below rest it may stray */
    float slack_hi;   /* [3] how far above rest it may stray */
    float accel;      /* [4] pull-back acceleration */
    float max_speed;  /* [5] rate limit */
    float ease;       /* [6] pull-back easing exponent, 0 = linear */
} cam_follow;

typedef struct {
    float pos[3];        /* eye, world space */
    float yaw;           /* degrees, app convention: forward = (sin, 0, -cos) */
    float dist;          /* current horizontal distance to the car */
    float height;        /* current height above the car */
    cam_follow f_dist, f_height, f_yaw;
    float boost_speed;   /* phys+0x6e0: speed-dependent pull-back */
    float boost_input;   /* phys+0x6e4: throttle-dependent pull-back */
    int   valid;
} cam_t;

void cam_init(cam_t *cam, const rb_car *c);

/* One frame. `steer` in -1..1 drives the look-into-turn term (VisTurn). */
void cam_update(cam_t *cam, const rb_car *c, float steer, float dt);

/* View angles that put the car in the middle of the frame. */
float cam_pitch_deg(const cam_t *cam);

#endif
