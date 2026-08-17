/* sun.c -- the sun disc and its lens flare. Spec, provenance and the three
   divergences are all in sun.h; this file only says WHERE in the arithmetic
   each recovered number lands. */

#include "sun.h"
#include "vis_data.h"
#include <math.h>
#include <string.h>

/* FUN_00478f60's own order, and the reason lens_flare_3 is here twice: the
   engine stores it at both +0x21c (ghost 2) and +0x224 (ghost 4). The fifth is
   the `shine`, which sits on the sun rather than on the ghost line. */
static const char *const SUN_FLARE_TEX[5] = {
    "lens_flare_1", "lens_flare_3", "lens_flare_2", "lens_flare_3", "shine"
};

static const float SUN_FLARE_DIST[4] = {
    SUN_DIST1, SUN_DIST2, SUN_DIST3, SUN_DIST4
};
static const float SUN_FLARE_SIZE[5] = {
    SUN_FLARE_SIZE1, SUN_FLARE_SIZE2, SUN_FLARE_SIZE3,
    SUN_FLARE_SIZE4, SUN_FLARE_SIZE5
};
static const unsigned char SUN_FLARE_RGB[5][3] = {
    { SUN_FLARE_RGB1 }, { SUN_FLARE_RGB2 }, { SUN_FLARE_RGB3 },
    { SUN_FLARE_RGB4 }, { SUN_FLARE_RGB5 }
};

void sun_init(sun_t *s, const scene_t *src)
{
    unsigned int i;

    memset(s, 0, sizeof(*s));
    if (!src)
        return;

    for (i = 0; i < src->n_markers; i++) {
        if (strcmp(src->markers[i].name, SUN_MARKER_NAME))
            continue;
        s->pos[0] = src->markers[i].x;
        s->pos[1] = src->markers[i].y;
        s->pos[2] = src->markers[i].z;
        s->enabled = 1;
        break;
    }
    /* No marker, no sun -- which is urban_1 and urban_2, and is what
       FUN_00478fd0 does with a failed modFindName. */
    if (!s->enabled)
        return;

    s->tex_disc = scene_tex(src, "sun_disc");
    for (i = 0; i < 5; i++)
        s->tex_flare[i] = scene_tex(src, SUN_FLARE_TEX[i]);

    /* A track packed without --markers has the marker but not the textures. The
       disc is the one that matters; without it there is nothing to draw. */
    if (!s->tex_disc)
        s->enabled = 0;

    /* Start blocked-and-off rather than on: the first cast has not happened, and
       fading in from nothing is what the state machine does from state 0. */
    s->state = SUN_OFF;
    s->ray_timer = 0.0f;        /* so the first step casts immediately */
}

void sun_step(sun_t *s, const col_t *col, const float eye[3], float dt)
{
    if (!s->enabled) {
        s->state = SUN_OFF;
        s->alpha = 0.0f;
        return;
    }

    /* The cast, throttled. FUN_00479250 counts +0x184 down by the frame time and
       reloads it with TimeShootRay when it goes negative, so the line of sight
       is re-tested five times in three seconds and held in between. That is a
       cost decision in the original and it is one here too: col_segment over a
       200 m ray is the most expensive query this file makes. */
    s->ray_timer -= dt;
    if (s->ray_timer < 0.0f) {
        s->ray_timer = SUN_RAY_PERIOD;
        /* No world: the sun is in view. A harness with a stub world, and the
           menu before a track is loaded, both land here. */
        s->clear = col ? !col_segment(col, eye, s->pos) : 1;
        s->n_casts++;
    }

    /* The four-state machine, transition for transition. The mirrored timer on a
       reversal (`FadeTime - timer`, LAB_004792fa) is what makes a flare caught
       half way in fade out from where it got to. */
    if (s->clear) {
        switch (s->state) {
        case SUN_OFF:
            s->state = SUN_FADE_IN;
            s->timer = SUN_FADE_TIME;
            break;
        case SUN_FADE_IN:
            s->timer -= dt;
            if (s->timer < 0.0f)
                s->state = SUN_ON;
            break;
        case SUN_FADE_OUT:
            s->state = SUN_FADE_IN;
            s->timer = SUN_FADE_TIME - s->timer;
            break;
        default:
            break;
        }
    } else {
        switch (s->state) {
        case SUN_FADE_IN:
            s->state = SUN_FADE_OUT;
            s->timer = SUN_FADE_TIME - s->timer;
            break;
        case SUN_FADE_OUT:
            s->timer -= dt;
            if (s->timer < 0.0f)
                s->state = SUN_OFF;
            break;
        case SUN_ON:
            s->state = SUN_FADE_OUT;
            s->timer = SUN_FADE_TIME;
            break;
        default:
            break;
        }
    }

    /* THE PORT'S: linear in the timer. See sun.h -- the state machine and both
       timers are recovered, the mapping to an alpha byte is not. Linear is the
       only shape under which the mirrored reversal above is continuous, which is
       the property the original's own transition was written for. */
    switch (s->state) {
    case SUN_ON:       s->alpha = 1.0f; break;
    case SUN_FADE_IN:  s->alpha = 1.0f - s->timer / SUN_FADE_TIME; break;
    case SUN_FADE_OUT: s->alpha = s->timer / SUN_FADE_TIME; break;
    default:           s->alpha = 0.0f; break;
    }
    if (s->alpha < 0.0f) s->alpha = 0.0f;
    if (s->alpha > 1.0f) s->alpha = 1.0f;
}

/* One camera-facing quad, in world space. */
static void quad_world(vtx_t q[4], const float c[3],
                       const float right[3], const float up[3], float half)
{
    int k;
    for (k = 0; k < 3; k++) {
        float r = right[k] * half, u = up[k] * half;
        ((float *)&q[0].x)[k] = c[k] - r - u;
        ((float *)&q[1].x)[k] = c[k] + r - u;
        ((float *)&q[2].x)[k] = c[k] + r + u;
        ((float *)&q[3].x)[k] = c[k] - r + u;
    }
    q[0].u = 0.f; q[0].v = 1.f;
    q[1].u = 1.f; q[1].v = 1.f;
    q[2].u = 1.f; q[2].v = 0.f;
    q[3].u = 0.f; q[3].v = 0.f;
}

static void draw_quad(const vtx_t q[4], GLuint tex)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glVertexPointer(3, GL_FLOAT, sizeof(vtx_t), &q[0].x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(vtx_t), &q[0].u);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

void sun_draw(const sun_t *s, const float eye[3],
              const float right[3], const float up[3],
              float fovy_deg, float aspect, float focal)
{
    float d[3], dist, tanh_;
    vtx_t q[4];
    int i;

    if (!s->enabled)
        return;

    d[0] = s->pos[0] - eye[0];
    d[1] = s->pos[1] - eye[1];
    d[2] = s->pos[2] - eye[2];
    dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dist < 1e-3f)
        return;

    /* ---- the disc -----------------------------------------------------
     *
     * FUN_00479070 builds a screen-space quad whose half-width is
     * `cam+0x220 * SizeSun * 0.5` -- a fraction of the viewport -- and writes
     * the sun's own projected depth into all four vertices so the depth buffer
     * occludes it.
     *
     * THE PORT'S: a world billboard subtending the same angle instead, so the
     * occlusion comes from the pipeline rather than from a depth float. At
     * distance `dist` a screen half-fraction `f` of the half-width subtends a
     * world half-extent of `f * dist * tan(fovy/2) * aspect`. Exact under a
     * perspective projection, and it means the disc grows and shrinks with the
     * camera exactly as the original's does.
     *
     * Alpha-blended, because sun_disc's RGB stays bright and its ALPHA is the
     * ramp -- flags 0xa0000003, mode 2. See sun.h.
     */
    tanh_ = tanf(fovy_deg * 0.5f * 3.14159265f / 180.0f);
    quad_world(q, s->pos, right, up, SUN_SIZE * dist * tanh_ * aspect);

    glDisable(GL_ALPHA_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glColor4f(SUN_COLOR_R / 255.f, SUN_COLOR_G / 255.f, SUN_COLOR_B / 255.f,
              1.f);
    draw_quad(q, s->tex_disc);

    /* ---- the ghosts ---------------------------------------------------
     *
     * State 0 draws nothing at all: FUN_00479250's `case 0: return` sits before
     * every projection, so a sun behind a building has no flare rather than a
     * faint one.
     */
    if (s->state == SUN_OFF || s->alpha <= 0.0f) {
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glEnable(GL_CULL_FACE);
        glEnable(GL_ALPHA_TEST);
        return;
    }

    {
        /* FUN_00479250's own construction, and it needs no screen-space pass to
         * reproduce: the ghosts are placed at EYE-SPACE points and handed to the
         * ordinary projection.
         *
         *     centre = (0,0,1) * (focal + CamOffset)     0x0055e9b0 is (0,0,1)
         *     dir    = normalise(sun_eye - centre)
         *     ghost  = centre + dir * Dist[i]
         *
         * so putting each ghost back into WORLD space and drawing it as an
         * ordinary camera-facing billboard gives exactly the same screen
         * position that FUN_00406f20 + FUN_00479b20 would. `focal` is the one
         * scale that is not recovered -- it is the original's cam+0x158 -- and
         * the caller anchors it; everything else here is the game's.
         */
        float fwd[3], ex, ey, ez, zc, dir[3], dlen, tanf2;
        int k;

        /* right x up is +forward for this basis (measured, not assumed: cam.c
           builds right and up from the same yaw/pitch fx_draw uses). */
        fwd[0] = right[1] * up[2] - right[2] * up[1];
        fwd[1] = right[2] * up[0] - right[0] * up[2];
        fwd[2] = right[0] * up[1] - right[1] * up[0];
        ex = d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
        ey = d[0] * up[0]    + d[1] * up[1]    + d[2] * up[2];
        ez = d[0] * fwd[0]   + d[1] * fwd[1]   + d[2] * fwd[2];
        if (ez < 0.0f) {                /* the basis is left-handed here */
            ez = -ez;
            fwd[0] = -fwd[0]; fwd[1] = -fwd[1]; fwd[2] = -fwd[2];
        }
        /* FUN_00479250's own guard: cam+0x15c <= sun_z, i.e. in front. */
        if (ez <= 1e-3f) {
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            glEnable(GL_CULL_FACE);
            glEnable(GL_ALPHA_TEST);
            return;
        }

        zc = focal + SUN_CAM_OFFSET;
        if (zc < 1e-3f)
            zc = 1e-3f;

        /* the unit direction from the view centre to the sun, in eye space */
        dir[0] = ex; dir[1] = ey; dir[2] = ez - zc;
        dlen = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (dlen < 1e-6f) {
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            glEnable(GL_CULL_FACE);
            glEnable(GL_ALPHA_TEST);
            return;
        }
        dir[0] /= dlen; dir[1] /= dlen; dir[2] /= dlen;

        /* Additive, and off the depth buffer -- flags 0xa0200005, mode 3 =
           SRCCOLOR / ONE, so `dst' = src*src + dst`. No depth test because the
           original draws these at the near plane (FUN_004799a0 writes
           cam+0x15c + 1e-06 into every vertex): a lens artefact is in the lens,
           not in the world. */
        glDisable(GL_DEPTH_TEST);
        glBlendFunc(GL_SRC_COLOR, GL_ONE);
        tanf2 = tanh_;

        for (i = 0; i < 5; i++) {
            float ep[3], wp[3], half;
            if (!s->tex_flare[i])
                continue;
            if (i < 4) {
                ep[0] = dir[0] * SUN_FLARE_DIST[i];
                ep[1] = dir[1] * SUN_FLARE_DIST[i];
                ep[2] = zc + dir[2] * SUN_FLARE_DIST[i];
            } else {
                /* the shine sits ON the sun -- FUN_00479250 copies +0x14..+0x1c
                   into the fifth slot rather than walking the Dist table */
                ep[0] = ex; ep[1] = ey; ep[2] = ez;
            }
            if (ep[2] <= 1e-3f)
                continue;
            for (k = 0; k < 3; k++)
                wp[k] = eye[k] + right[k] * ep[0] + up[k] * ep[1]
                        + fwd[k] * ep[2];
            /* `Size` is a screen fraction in the original (times cam+0x220);
               the same conversion the disc uses turns it into a world extent at
               this sprite's own depth, so the sprite subtends the authored
               angle whatever the resolution. */
            half = SUN_FLARE_SIZE[i] * ep[2] * tanf2 * aspect;
            quad_world(q, wp, right, up, half);
            /* THE FADE SCALES THE COLOUR, NOT THE ALPHA, and that is forced.
               FUN_004797c0 packs the fade byte into the D3DCOLOR's alpha
               (CONCAT31/21/11 of fade,R,G,B), but mode 3 is SRCCOLOR / ONE and
               the source alpha takes no part in that blend -- a vertex alpha
               here would fade nothing. Under an additive blend the only thing
               that can dim a sprite is its colour. */
            glColor4f(SUN_FLARE_RGB[i][0] / 255.f * s->alpha,
                      SUN_FLARE_RGB[i][1] / 255.f * s->alpha,
                      SUN_FLARE_RGB[i][2] / 255.f * s->alpha,
                      1.f);
            draw_quad(q, s->tex_flare[i]);
        }
        glEnable(GL_DEPTH_TEST);
    }

    glColor4f(1.f, 1.f, 1.f, 1.f);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glEnable(GL_ALPHA_TEST);
}
