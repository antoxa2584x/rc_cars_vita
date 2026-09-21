/* opts.c -- see opts.h for what each field drives and where it came from. */
#include "opts.h"

/* THE TEN, in the order the bind list walks them. Deliberately not sorted by
   bit value: this is the order a thumb finds them in -- the four face buttons,
   the two shoulders, then the D-pad. */
const unsigned int OPT_BTN[OPT_N_BTN] = {
    SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, SCE_CTRL_SQUARE, SCE_CTRL_TRIANGLE,
    SCE_CTRL_LTRIGGER, SCE_CTRL_RTRIGGER,
    SCE_CTRL_UP, SCE_CTRL_DOWN, SCE_CTRL_LEFT, SCE_CTRL_RIGHT
};

static const char *const OPT_BTN_NAME[OPT_N_BTN] = {
    "Cross", "Circle", "Square", "Triangle",
    "L", "R", "Up", "Down", "Left", "Right"
};

int opts_btn_ok(unsigned int bits)
{
    int i;
    for (i = 0; i < OPT_N_BTN; i++)
        if (OPT_BTN[i] == bits)
            return 1;
    return 0;
}

const char *opts_btn_name(unsigned int bits)
{
    int i;
    for (i = 0; i < OPT_N_BTN; i++)
        if (OPT_BTN[i] == bits)
            return OPT_BTN_NAME[i];
    return "None";      /* string 19, and what an empty slot shows */
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void opts_default_binds(opts_t *o)
{
    int a, s;
    if (!o) return;
    for (a = 0; a < OPT_N_ACT; a++)
        for (s = 0; s < OPT_N_SLOT; s++)
            o->bind[a][s] = 0;
    /* THE MAP THIS APP SHIPPED WITH, taken straight out of main.c's frame
       loop: R throttle, L brake, the left stick steering, CROSS boost, CIRCLE
       jump. Nothing here moves a control a player already knows.

       AND THE D-PAD MIRRORS THE TWO TRIGGERS in the second slot, because that
       is what makes `Use joystick: No' a complete scheme on its own -- Up,
       Down, Left and Right then drive the car with no stick at all, which is
       the whole point of the row.

       Stop and Reset get NOTHING. The port has no handbrake and no manual
       reset today; a default for either would be this file inventing a
       control rather than exposing one. */
    o->bind[OPT_GEAR][0]    = SCE_CTRL_RTRIGGER;
    o->bind[OPT_GEAR][1]    = SCE_CTRL_UP;
    o->bind[OPT_REVERSE][0] = SCE_CTRL_LTRIGGER;
    o->bind[OPT_REVERSE][1] = SCE_CTRL_DOWN;
    o->bind[OPT_TURN_L][0]  = SCE_CTRL_LEFT;
    o->bind[OPT_TURN_R][0]  = SCE_CTRL_RIGHT;
    o->bind[OPT_BOOST][0]   = SCE_CTRL_CROSS;
    o->bind[OPT_JUMP][0]    = SCE_CTRL_CIRCLE;
}

void opts_init(opts_t *o)
{
    if (!o) return;
    o->use_sound   = 1;
    o->quality     = OPT_Q_HIGH;
    o->bg_sound    = 1;
    o->music_style = OPT_MUSIC_BOTH;
    o->vol_master  = OPT_VOL_STEPS;
    o->stick       = 1;
    o->sens        = OPT_SENS_DEF;
    o->dead        = OPT_DEAD_DEF;
    opts_default_binds(o);
}

int opts_is_custom(const opts_t *o)
{
    opts_t d;
    int a, s;
    if (!o) return 0;
    opts_default_binds(&d);
    for (a = 0; a < OPT_N_ACT; a++)
        for (s = 0; s < OPT_N_SLOT; s++)
            if (o->bind[a][s] != d.bind[a][s])
                return 1;
    return 0;
}

int opts_bind(opts_t *o, int act, int slot, unsigned int bits)
{
    int a, s, changed = 0;

    if (!o || act < 0 || act >= OPT_N_ACT || slot < 0 || slot >= OPT_N_SLOT)
        return 0;
    if (bits && !opts_btn_ok(bits))
        return 0;
    /* THE STEAL, which is what makes a conflict impossible instead of
       reported: the button leaves every other slot it was in first. Including
       the other slot of THIS action -- binding R to Gear's second slot when it
       is already its first leaves one R and not two, so the row cannot end up
       claiming a button twice. */
    if (bits) {
        for (a = 0; a < OPT_N_ACT; a++)
            for (s = 0; s < OPT_N_SLOT; s++)
                if ((a != act || s != slot) && o->bind[a][s] == bits) {
                    o->bind[a][s] = 0;
                    changed = 1;
                }
    }
    if (o->bind[act][slot] != bits) {
        o->bind[act][slot] = bits;
        changed = 1;
    }
    return changed;
}

int opts_held(const opts_t *o, int act, unsigned int buttons)
{
    int s;
    if (!o || act < 0 || act >= OPT_N_ACT)
        return 0;
    for (s = 0; s < OPT_N_SLOT; s++)
        if (o->bind[act][s] && (buttons & o->bind[act][s]))
            return 1;
    return 0;
}

float opts_steer(const opts_t *o, unsigned int buttons, unsigned char raw)
{
    float v = 0.f;

    if (!o)
        return 0.f;
    if (o->stick) {
        const int dz = clampi(o->dead, 0, OPT_DEAD_STEPS) * 8;
        const int d = (int)raw - 128;
        if (d <= -dz || d >= dz) {
            /* THE DEADZONE IS SUBTRACTED, not just tested. main.c's own axis()
               tested it and passed the raw value through, so the stick jumped
               to 0.19 the moment it left the dead band; taking the band out and
               rescaling makes the first millimetre past it worth nothing, which
               is what a deadzone is for. At the default dz of 24 the far end is
               still exactly 1.0, so nothing about full lock moves. */
            const float span = 128.f - (float)dz;
            v = ((d > 0) ? (float)(d - dz) : (float)(d + dz))
                / (span > 1.f ? span : 1.f);
            v *= 0.5f + 0.1f * (float)clampi(o->sens, 0, OPT_SENS_STEPS);
        }
    }
    /* A BOUND BUTTON IS HONOURED EITHER WAY -- see opts.h. Both at once is
       zero, which is what a player holding both ends of a D-pad has asked
       for. */
    if (opts_held(o, OPT_TURN_L, buttons)) v -= 1.f;
    if (opts_held(o, OPT_TURN_R, buttons)) v += 1.f;
    if (v < -1.f) v = -1.f;
    if (v >  1.f) v =  1.f;
    return v;
}

float opts_sfx_gain(const opts_t *o, int sfx_notches)
{
    if (!o || !o->use_sound)
        return 0.f;
    return (float)clampi(sfx_notches, 0, OPT_VOL_STEPS)
           * (float)clampi(o->vol_master, 0, OPT_VOL_STEPS)
           / (float)(OPT_VOL_STEPS * OPT_VOL_STEPS);
}

float opts_music_gain(const opts_t *o, int music_notches)
{
    if (!o || !o->use_sound || !o->bg_sound)
        return 0.f;
    return (float)clampi(music_notches, 0, OPT_VOL_STEPS)
           * (float)clampi(o->vol_master, 0, OPT_VOL_STEPS)
           / (float)(OPT_VOL_STEPS * OPT_VOL_STEPS);
}

int opts_voices(const opts_t *o)
{
    /* 8 / 16 / 24 of mix.c's MIX_VOICES. The top one is the whole pool, so
       High is the mixer exactly as it has always been and only the two below
       it cost anything. */
    switch (o ? o->quality : OPT_Q_HIGH) {
    case OPT_Q_LOW:    return 8;
    case OPT_Q_MEDIUM: return 16;
    default:           return 24;
    }
}

int opts_music_group(const opts_t *o, int in_menu)
{
    if (!o || !o->use_sound || !o->bg_sound)
        return -1;
    /* ROCK IS CYCLE 0 AND TECHNO CYCLE 1 -- the opposite of what this file
       first assumed, and the reason is in opts.h: the pairing is not in the
       data and was settled by somebody playing both banks. */
    if (o->music_style == OPT_MUSIC_ROCK)
        return OPT_GROUP_MENU;
    if (o->music_style == OPT_MUSIC_TECHNO)
        return OPT_GROUP_RACE;
    return in_menu ? OPT_GROUP_MENU : OPT_GROUP_RACE;
}

void opts_clamp(opts_t *o)
{
    int a, s;
    if (!o) return;
    o->use_sound   = o->use_sound ? 1 : 0;
    o->bg_sound    = o->bg_sound ? 1 : 0;
    o->stick       = o->stick ? 1 : 0;
    o->quality     = clampi(o->quality, 0, OPT_N_QUALITY - 1);
    o->music_style = clampi(o->music_style, 0, OPT_N_MUSIC - 1);
    o->vol_master  = clampi(o->vol_master, 0, OPT_VOL_STEPS);
    o->sens        = clampi(o->sens, 0, OPT_SENS_STEPS);
    o->dead        = clampi(o->dead, 0, OPT_DEAD_STEPS);
    /* A BIT THAT IS NOT ONE OF THE TEN IS DROPPED, and a DUPLICATE is dropped
       too -- a hand-edited file that binds Cross to two actions would otherwise
       walk straight past opts_bind's steal and put the page in the one state it
       is built to be unable to reach. Later slots lose, which is the same
       last-writer-loses rule opts_bind applies. */
    for (a = 0; a < OPT_N_ACT; a++)
        for (s = 0; s < OPT_N_SLOT; s++) {
            unsigned int b = o->bind[a][s];
            int a2, s2;
            if (b && !opts_btn_ok(b)) { o->bind[a][s] = 0; continue; }
            if (!b) continue;
            for (a2 = 0; a2 <= a; a2++)
                for (s2 = 0; s2 < OPT_N_SLOT; s2++) {
                    if (a2 == a && s2 >= s) break;
                    if (o->bind[a2][s2] == b) { o->bind[a][s] = 0; a2 = a + 1; break; }
                }
        }
}
