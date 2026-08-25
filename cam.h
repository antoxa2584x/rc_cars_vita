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
 *   0x0049d740  easeCurve          the exponent on that pull-back
 *   0x00501180  camPost            THE SECOND HALF, registered as the camera's
 *                                  per-frame hook by 0x00500760: the obstacle
 *                                  lift, the ground clamp, and the VisTurn aim
 *   0x00500e60  camObstacleLift    orbit the eye up over whatever is in the way
 *   0x00500d60  camSightBlocked    the car -> eye segment against the world
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
 * A FOURTH follower runs on the aim, and it is the engine's default descriptor
 * DAT_00564810 = {0, 0, 2e6, 2e6, 2e6, 2e6, 0} -- rest 0 at an infinite rate, so
 * it snaps. The camera always points at the car; camPost then tilts that aim UP
 * by VisTurn, which is what puts the car low in the frame.
 *
 * NOT a divergence, though these notes said it was: camFollow works in the car's
 * CURRENT reference frame, exactly as this file does. It expresses the eye in
 * that frame, runs the three followers on the three components, and rebuilds the
 * eye as origin + RotY(trail) * (0, height, -dist). The previous reference frame
 * it keeps at cam+0x2b is written and never read back inside camFollow.
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
    float yaw;           /* degrees, the RENDERER's view yaw: the direction the
                            camera faces is (-sin, 0, -cos) -- NOT (+sin, ...),
                            which this line used to say and which cam.c's
                            yaw_forward comment exists because of. Read that one:
                            two mirrored-arrow bugs came off the +sin form. */
    float dist;          /* current horizontal distance to the car */
    float height;        /* current height above the car */
    cam_follow f_dist, f_height, f_yaw;
    float boost_speed;   /* phys+0x6e0: speed-dependent pull-back */
    float boost_input;   /* phys+0x6e4: throttle-dependent pull-back */
    /* camPost's two ramps, both on the phys block in the original. */
    float cdt_angle;     /* phys+0x6ec: degrees the eye is currently lifted */
    float cdt_dir_add;   /* phys+0x6f0: the extra aim tilt that lift carries */
    float pitch;         /* the finished aim, degrees, positive = looking DOWN */
    int   valid;
} cam_t;

void cam_init(cam_t *cam, const rb_car *c);

/* One frame. `steer` is no longer read -- VisTurn turned out to have nothing to
   do with steering, see cam.c -- and stays only so main.c and the harnesses do
   not have to change. The car carries the rb_world the obstacle lift and the
   ground clamp query; both are skipped if it has none. */
void cam_update(cam_t *cam, const rb_car *c, float steer, float dt);

/* The finished view pitch, degrees, positive = looking down. NOT
   atan2(height, dist): camPost rotates the aim UP by VisTurn (plus whatever the
   obstacle lift adds), so the car sits BELOW the centre of the frame. */
float cam_pitch_deg(const cam_t *cam);

#endif
