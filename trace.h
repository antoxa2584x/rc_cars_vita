/*
 * trace.h -- the tyre marks the car leaves on the surface.
 *
 * The game's own subsystem, gated by RCCars.crs's VIDEO_WheelTrace (which ships
 * at 1) and built out of:
 *
 *   FUN_0052f050   create: six ring buffers, one per wheel, 0x20 slots for
 *                  wheels 0 and 1 and 0x30 for the rest, halved for a car that
 *                  is not a player's; loads t_halfdry_tire2_1..4
 *   FUN_005300e0   config: car_trace -- timeLife, maxLen, scaleCoeff, maxHeight
 *   FUN_0052f700   one sample: decide whether it extends the strip, rewrites
 *                  the head, or starts a new strip
 *   FUN_0052fb60   the test that decides which of those it is
 *   FUN_0052f990   write a sample into a slot: two edge vertices, and a U that
 *                  accumulates as scaleCoeff * distance along the trail
 *   FUN_0052fd00   draw: walk the ring, join consecutive samples into quads,
 *                  and skip anything more than 20 m from the camera
 *   FUN_0052ff20   emit one quad, biased 2e-05 further than the last strip
 *
 * The one mechanism worth understanding before touching this: a ring of 32
 * samples at 60 Hz is half a second of driving, but a mark lasts timeLife = 60
 * seconds. It fits because a sample that is COLLINEAR with the last two --
 * within maxHeight of the line through them -- OVERWRITES the head instead of
 * taking a new slot. Driving straight therefore costs almost no slots and the
 * trail stretches; only turning spends them. That is FUN_0052fb60 returning 0,
 * and it is why maxHeight matters (see gen_fx_data.maxheight).
 *
 * HOW A MARK IS BLENDED, which the port had wrong from the start and which is
 * fully recovered -- it is not an alpha-blended decal, and the texture is not a
 * picture of a mark.
 *
 * FUN_0052ff20 hands the quad a material whose flags word is 0x80000021
 * (written at 0x0053001a), and two functions decode that byte:
 *
 *   FUN_0045c3c0  bit 0x20 -> blend mode 5 -> FUN_0045c6e0's table entry
 *                 0x0045c911: SetRenderState(SRCBLEND, D3DBLEND_DESTCOLOR),
 *                 (DESTBLEND, D3DBLEND_SRCCOLOR).  So the framebuffer gets
 *                 dst' = src*dst + dst*src = 2 * src * dst -- a MODULATE-2x,
 *                 not an alpha blend.  Nothing in this path reads src alpha.
 *   FUN_0045c980  the same bit (0x0045cda6) -> stage 0 COLOROP = 0x12 =
 *                 D3DTOP_MODULATEALPHA_ADDCOLOR, COLORARG1 = DIFFUSE,
 *                 COLORARG2 = TEXTURE.  So src = diffuse.rgb + diffuse.a * tex.
 *
 * and FUN_0052fd00 builds that diffuse colour out of one number:
 *
 *   f = strength, held for the first three quarters of the mark's life and
 *       ramped to 0 over the last quarter (0x0052fd9f-0x0052fe08)
 *   diffuse.rgb = (1 - f) * 128     (0x00554390, 0x00554550)
 *   diffuse.a   = f * 255           (0x005544b0)
 *
 * Put together, dst' = dst * (1 - f + 2*f*tex): the mark LERPS the ground from
 * untouched toward 2*tex*dst.  Which is why t_halfdry_tire2_<n> is a 64 x 256
 * image whose flat border is exactly 128 and whose tread runs 96..140 -- it is a
 * SIGNED detail map centred on neutral, darkening the sand where the tread bit
 * and lightening it where the tyre packed it down.  Draw it as an opaque or
 * alpha-blended sprite, as this port did, and you get a solid grey stripe.
 *
 * The port reproduces it exactly with glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR)
 * -- the same 2*src*dst -- and a GL_COMBINE / GL_INTERPOLATE stage, which is the
 * one texture env that can express "lerp between a constant and the texture".
 *
 * The port's own, marked at the point of use: the mark's half-width (the
 * caller's argument to FUN_0052f990 comes from FUN_0052f310, which is not
 * recovered) and the lift off the surface.
 */

#ifndef TRACE_H
#define TRACE_H

#include "fx_data.h"
#include "rb.h"
#include "scene.h"

/* TRACE_RING_REAR, the larger of the two. Rounded up so one array serves both. */
#define TRACE_RING 48

/* The blend constants, all literals in .text rather than config keys, so they
   live here and not in the generated fx_data.h. Addresses are the float they
   were read from. */

/* The neutral level of the mark texture: FUN_0052fd00 scales its grey by 128
   (0x00554550) into a 0..255 diffuse colour, and the 2x in the blend makes
   2 * 128/255 = 1.004 the value that leaves the ground alone. It is also, to the
   bit, the flat border t_halfdry_tire2_<n> is authored with. */
#define TRACE_NEUTRAL (128.0f / 255.0f)

/* The mark holds full strength until this much of its life is left, then ramps
   to nothing: FUN_0052fd00 compares the remaining life against timeLife * 0.25
   (0x00554460) and scales by the ratio below it. */
#define TRACE_FADE_FRAC 0.25f

/* FUN_0052f310 at 0x0052f537: wheels 0 and 1 -- the front pair -- lay their
   mark at half strength (0x00554384). */
#define TRACE_FRONT_STRENGTH 0.5f

/* FUN_0052f310's jump table at 0x0052f6dc keys the strength on the surface id
   that FUN_00534fc0 returns: 1 -> 0.7, 2 -> 1.0, 3 -> 0.3, 5 and 7 -> 0.6, and
   4 and 6 leave NO mark at all. That classifier is not recovered (it is the same
   one CLAUDE.md records as missing for the surface sounds), and the port's own
   material classes are a different set, so mapping one onto the other would be a
   guess. Every surface gets the table's strongest value until it is recovered --
   which is what the port already drew, so nothing regresses. */
#define TRACE_STRENGTH 1.0f

typedef struct {
    float pos[3];           /* record +0x08, the contact point */
    float nrm[3];           /* record +0x14, the surface normal */
    float lat[3];           /* record +0x20, across the mark */
    float len;              /* record +0x38, distance along the trail */
    /* record +0x54/0xa4, scaleCoeff * len, wrapped. Drawn as the V coordinate,
       because the mark texture is 64 x 256 with its tread down the long axis --
       see the note in trace_draw. */
    float u;
    float half_w;           /* half the mark's width, in metres */
    float life;             /* record +0x00, seconds remaining */
    /* record +0x04, FUN_0052f990's param_5: how hard this sample marks the
       ground, 0..1. The fade multiplies it; see TRACE_STRENGTH. */
    float strength;
    int tex;                /* record +0x34, which t_halfdry_tire2_<n> */
    /* The engine keeps two flags per record (+0x44 "nothing follows me yet" and
       +0x48 "the strip breaks after me") and FUN_0052fd00 joins a pair only when
       both are clear. A strip id is the same statement in one field, and it is
       far easier to assert: two samples are joined exactly when their ids match. */
    unsigned int strip;
    int used;
} trace_pt;

typedef struct {
    trace_pt pt[TRACE_RING];
    int head;               /* newest sample, -1 when empty */
    int n;
    int cap;                /* 0x20 for wheels 0 and 1, 0x30 for the rest */
    unsigned int strip;     /* the id new samples are given */
    /* Was this wheel on the ground last frame? FUN_0052f700's `param_8` is the
       caller's "start a new strip whatever else you decide", and a wheel that has
       been in the air is what it is for: nothing else in the chain breaks a
       strip. Without it a car that jumps 9 m lands still attached to the mark it
       left before take-off, and the draw stretches one quad across the gap. */
    int was_down;
} trace_ring;

typedef struct {
    trace_ring w[RB_MAX_WHEELS];
    GLuint tex[TRACE_TEX_N];
    /* Half the width of the tyre that makes this wheel's mark, measured off the
       packed mesh at trace_init. 0 when the scene carries no rig to measure, and
       then the mark falls back to TRACE_WIDTH_FRAC of the physics radius. */
    float half_w[RB_MAX_WHEELS];
    int n_tex;
    int enabled;
    int n_quads;            /* drawn last frame, for telemetry */
} trace_t;

/* `src` is the scene the t_halfdry_tire2_* textures were packed into. */
void trace_init(trace_t *tr, const scene_t *src);

/* Measure each wheel's tyre off the packed mesh, so the mark is as wide as the
 * tyre that makes it. trace_init calls this itself, but it is separate because
 * the rb-wheel-index -> mesh-node mapping is carani_bind's and main.c binds the
 * rig in respawn(), AFTER load_car has built the trace -- so respawn calls it
 * again once the mapping exists. Safe in either order and safe to repeat: an
 * unbound rig leaves every width at 0 and the mark falls back to the radius. */
void trace_fit_tyres(trace_t *tr, const scene_t *src);

/* Sample every wheel in contact and age everything. Call once per frame. */
void trace_step(trace_t *tr, const rb_car *c, float dt);

/* Every live strip, as quads. `eye` drives FUN_0052fd00's 20 m cull. */
void trace_draw(trace_t *tr, const float eye[3]);

/* Throw away every mark -- on a respawn, or a track change. */
void trace_clear(trace_t *tr);

/* FUN_0052fb60. Exposed because it is the whole of the ring's behaviour and the
   test needs to drive it directly. Returns the engine's own codes:
     0  collinear enough to merge into the head
     1  off the line by more than maxHeight
     3  behind the previous sample
     4  further than maxLen
     5  no movement worth recording  */
int trace_break_test(const float prev[3], const float head[3],
                     const float now[3]);

#endif
