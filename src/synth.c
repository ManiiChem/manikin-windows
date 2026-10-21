// each sound is a few short notes (segments) mixed together then normalised to its categorys
// loudness. all soft on purpose since you hear him all day, so mostly triangles, sines and
// lowpassed squares near half duty with quick attacks and quiet tails. noise is seeded per
// sound so a recipe always renders the exact same samples

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "synth.h"

#define PI 3.14159265358979f
#define MAX_SEGS 6
#define FADE_IN  0.002f           // secs, every sound ramps in and out so it doesnt click
#define FADE_OUT 0.006f
#define CEILING  0.97f            // peak after limiting (int16 rounding stays under 0.98)

typedef enum { SQUARE, TRIANGLE, SINE, SAW, NOISE, SOFT_NOISE } Wave;

// one note, a + h + r secs long. for the noises f0/f1 is how often a new random value
// gets picked (higher = hissier)
typedef struct {
    float at;                     // secs into the sound it starts (notes can overlap)
    Wave wave;
    float duty;                   // SQUARE only, 0 means 0.5
    float f0, f1;                 // Hz, slides f0 -> f1 (f1 = 0 for no slide)
    float slide;                  // secs the slide takes (0 = the whole note)
    float a, h, r;                // secs of attack, hold at full, release
    float vib, vib_hz;            // vibrato depth in semitones and rate in Hz
    float lp;                     // Hz, one pole lowpass cutoff (0 = off)
    float gain;
} Seg;

typedef struct {
    Seg seg[MAX_SEGS];
} Recipe;

#define C4 261.63f
#define D4 293.66f
#define E4 329.63f
#define G4 392.00f
#define A4 440.00f
#define B4 493.88f
#define C5 523.25f
#define D5 587.33f
#define E5 659.26f
#define G5 783.99f
#define A5 880.00f
#define C6 1046.50f
#define D6 1174.66f
#define E6 1318.51f
#define G6 1567.98f
#define A6 1760.00f
#define C7 2093.00f

// ---- names, categories and max lengths ----

#define UI      SFX_CAT_UI
#define VOICE   SFX_CAT_VOICE
#define BODY    SFX_CAT_BODY
#define AMBIENT SFX_CAT_AMBIENT

static const SfxInfo INFO[SFX_COUNT] = {
    [SFX_BOOT]        = { "boot",        UI,      900 },
    [SFX_QUIT]        = { "quit",        UI,      600 },
    [SFX_BRAIN_DIED]  = { "brain_died",  UI,      900 },
    [SFX_POKE]        = { "poke",        UI,      300 },
    [SFX_POKE_GRUMBLE]= { "poke_grumble",UI,      400 },
    [SFX_PICKUP]      = { "pickup",      UI,      300 },
    [SFX_SHAKE]       = { "shake",       UI,      300 },
    [SFX_DIZZY]       = { "dizzy",       UI,      600 },
    [SFX_LAND]        = { "land",        BODY,    300 },
    [SFX_CHAT_OPEN]   = { "chat_open",   UI,      300 },
    [SFX_CHAT_CLOSE]  = { "chat_close",  UI,      300 },
    [SFX_SUBMIT]      = { "submit",      UI,      300 },
    [SFX_ASK]         = { "ask",         UI,      400 },
    [SFX_YES]         = { "yes",         UI,      300 },
    [SFX_NO]          = { "no",          UI,      300 },
    [SFX_LOCK]        = { "lock",        UI,      300 },
    [SFX_UNLOCK]      = { "unlock",      UI,      900 },
    [SFX_WRONG]       = { "wrong",       UI,      300 },
    [SFX_CANCEL]      = { "cancel",      UI,      300 },
    [SFX_SOUND_ON]    = { "sound_on",    UI,      300 },
    [SFX_BUBBLE]      = { "bubble",      UI,      300 },
    [SFX_PAGE]        = { "page",        UI,      300 },
    [SFX_HISTORY]     = { "history",     UI,      300 },
    [SFX_HEY]         = { "hey",         UI,      300 },
    [SFX_REMIND]      = { "remind",      UI,      900 },
    [SFX_ERROR]       = { "error",       UI,      400 },
    [SFX_THINK]       = { "think",       UI,      300 },
    [SFX_HOP]         = { "hop",         BODY,    300 },
    [SFX_SPARKLE]     = { "sparkle",     BODY,    400 },
    [SFX_LAUGH]       = { "laugh",       BODY,    300 },
    [SFX_HEART]       = { "heart",       BODY,    300 },
    [SFX_STING]       = { "sting",       BODY,    300 },
    [SFX_HUH]         = { "huh",         BODY,    400 },
    [SFX_TING]        = { "ting",        BODY,    400 },
    [SFX_SIGH]        = { "sigh",        BODY,    1200 },
    [SFX_QUIVER]      = { "quiver",      BODY,    400 },
    [SFX_STOMP]       = { "stomp",       BODY,    300 },
    [SFX_HMPH]        = { "hmph",        BODY,    400 },
    [SFX_YAWN]        = { "yawn",        AMBIENT, 1200 },
    [SFX_NOD]         = { "nod",         AMBIENT, 300 },
    [SFX_SETTLE]      = { "settle",      AMBIENT, 900 },
    [SFX_SNORE_POP]   = { "snore_pop",   AMBIENT, 300 },
    [SFX_WAKE_JOLT]   = { "wake_jolt",   AMBIENT, 400 },
    [SFX_WAKE_SOFT]   = { "wake_soft",   AMBIENT, 400 },
    [SFX_STEP]        = { "step",        AMBIENT, 80 },
    [SFX_ZIP_OUT]     = { "zip_out",     UI,      300 },
    [SFX_ZIP_IN]      = { "zip_in",      UI,      300 },
    [SFX_PLUG]        = { "plug",        BODY,    300 },
    [SFX_RING]        = { "ring",        AMBIENT, 400 },
    [SFX_SUCCESS]     = { "success",     UI,      900 },
    [SFX_UNPLUG]      = { "unplug",      UI,      300 },
    [SFX_WHISTLE]     = { "whistle",     AMBIENT, 300 },
    [SFX_GRAB]        = { "grab",        BODY,    300 },
    [SFX_CLACK]       = { "clack",       UI,      120 },
    [SFX_KEY]         = { "key",         BODY,    60 },
    [SFX_HAUL]        = { "haul",        BODY,    600 },
    [SFX_LETGO]       = { "letgo",       UI,      300 },
    [SFX_VICTORY]     = { "victory",     UI,      1200 },
    [SFX_OOPS]        = { "oops",        BODY,    400 },
};

// ---- the recipes, tweak these by ear ----

static const Recipe RECIPES[SFX_COUNT] = {
    // power on, soft sweep under a C major arpeggio that rings out at the top
    [SFX_BOOT] = {{
        { .at = 0.00f, .wave = SINE,     .f0 = 180, .f1 = 520, .a = .02f, .h = .05f, .r = .08f, .gain = .35f },
        { .at = 0.00f, .wave = TRIANGLE, .f0 = C5, .a = .004f, .h = .05f, .r = .10f, .gain = 1 },
        { .at = 0.09f, .wave = TRIANGLE, .f0 = E5, .a = .004f, .h = .05f, .r = .10f, .gain = 1 },
        { .at = 0.18f, .wave = TRIANGLE, .f0 = G5, .a = .004f, .h = .05f, .r = .10f, .gain = 1 },
        { .at = 0.27f, .wave = TRIANGLE, .f0 = C6, .a = .004f, .h = .08f, .r = .25f, .gain = 1 },
        { .at = 0.27f, .wave = SQUARE, .duty = .5f, .f0 = C5, .a = .004f, .h = .06f, .r = .2f, .lp = 1400, .gain = .25f },
    }},
    // boop, a sine dropping a fifth with a lowpassed square under it for body
    [SFX_POKE] = {{
        { .wave = SINE, .f0 = 640, .f1 = 430, .a = .003f, .h = .03f, .r = .06f, .gain = 1 },
        { .wave = SQUARE, .duty = .5f, .f0 = 320, .f1 = 215, .a = .003f, .h = .02f, .r = .05f, .lp = 1200, .gain = .3f },
    }},
    // triangle sliding up an octave and a half (whoop)
    [SFX_PICKUP] = {{
        { .wave = TRIANGLE, .f0 = 300, .f1 = 900, .a = .01f, .h = .08f, .r = .07f, .gain = 1 },
        { .wave = SQUARE, .duty = .25f, .f0 = 300, .f1 = 900, .a = .01f, .h = .07f, .r = .05f, .lp = 2200, .gain = .2f },
    }},
    // thud (falling sine plus a puff of dull noise) then a tiny bounce
    [SFX_LAND] = {{
        { .wave = SINE, .f0 = 170, .f1 = 60, .slide = .08f, .a = .002f, .h = .03f, .r = .09f, .gain = 1 },
        { .wave = SOFT_NOISE, .f0 = 1500, .a = .001f, .h = .01f, .r = .04f, .lp = 700, .gain = .5f },
        { .at = .13f, .wave = SINE, .f0 = 240, .f1 = 160, .a = .003f, .h = .015f, .r = .05f, .gain = .45f },
    }},
    // soft pops, a sine flicking up to open and down to close
    [SFX_CHAT_OPEN] = {{
        { .wave = SINE, .f0 = 420, .f1 = 950, .slide = .05f, .a = .002f, .h = .03f, .r = .06f, .gain = 1 },
        { .wave = TRIANGLE, .f0 = 840, .f1 = 1900, .slide = .05f, .a = .002f, .h = .015f, .r = .04f, .lp = 3000, .gain = .2f },
    }},
    [SFX_CHAT_CLOSE] = {{
        { .wave = SINE, .f0 = 950, .f1 = 420, .slide = .05f, .a = .002f, .h = .03f, .r = .06f, .gain = 1 },
        { .wave = TRIANGLE, .f0 = 1900, .f1 = 840, .slide = .05f, .a = .002f, .h = .015f, .r = .04f, .lp = 3000, .gain = .2f },
    }},
    // swoosh, airy noise getting brighter with a faint rising whistle in it
    [SFX_SUBMIT] = {{
        { .wave = SOFT_NOISE, .f0 = 2500, .f1 = 11000, .a = .07f, .h = .03f, .r = .12f, .lp = 5000, .gain = 1 },
        { .wave = SINE, .f0 = 500, .f1 = 1500, .a = .05f, .h = .05f, .r = .08f, .gain = .3f },
    }},
    // "ding?" E5 then A5, the second one bends up like a question
    [SFX_ASK] = {{
        { .at = 0.00f, .wave = TRIANGLE, .f0 = E5, .a = .003f, .h = .05f, .r = .08f, .gain = 1 },
        { .at = 0.00f, .wave = SINE, .f0 = E5 * 2, .a = .003f, .h = .02f, .r = .06f, .gain = .2f },
        { .at = 0.12f, .wave = TRIANGLE, .f0 = A5, .f1 = A5 * 1.06f, .a = .003f, .h = .06f, .r = .12f, .gain = 1 },
        { .at = 0.12f, .wave = SINE, .f0 = A5 * 2, .f1 = A5 * 2.12f, .a = .003f, .h = .03f, .r = .08f, .gain = .2f },
    }},
    // tiny pop
    [SFX_BUBBLE] = {{
        { .wave = SINE, .f0 = 650, .f1 = 1250, .slide = .025f, .a = .001f, .h = .015f, .r = .035f, .gain = 1 },
    }},
    // "hey!" G5 up to C6, each one scoops up a little
    [SFX_HEY] = {{
        { .at = 0.00f, .wave = SQUARE, .duty = .4f, .f0 = G5 * .97f, .f1 = G5, .slide = .02f, .a = .004f, .h = .04f, .r = .05f, .lp = 2800, .gain = .8f },
        { .at = 0.00f, .wave = TRIANGLE, .f0 = G5 * .97f, .f1 = G5, .slide = .02f, .a = .004f, .h = .04f, .r = .05f, .gain = .6f },
        { .at = 0.10f, .wave = SQUARE, .duty = .4f, .f0 = C6 * .97f, .f1 = C6, .slide = .02f, .a = .004f, .h = .05f, .r = .08f, .lp = 2800, .gain = .8f },
        { .at = 0.10f, .wave = TRIANGLE, .f0 = C6 * .97f, .f1 = C6, .slide = .02f, .a = .004f, .h = .05f, .r = .08f, .gain = .6f },
    }},
    // hwaaah, wobbly glide down with a bit of breath under it
    [SFX_YAWN] = {{
        { .wave = TRIANGLE, .f0 = 520, .f1 = 230, .a = .12f, .h = .35f, .r = .43f, .vib = .5f, .vib_hz = 5, .lp = 1500, .gain = 1 },
        { .wave = SOFT_NOISE, .f0 = 3000, .a = .15f, .h = .3f, .r = .4f, .lp = 1200, .gain = .25f },
    }},
    // soft tick, a dull tap and a tiny bit of noise
    [SFX_STEP] = {{
        { .wave = SINE, .f0 = 200, .f1 = 120, .a = .001f, .h = .008f, .r = .03f, .gain = 1 },
        { .wave = SOFT_NOISE, .f0 = 4000, .a = .001f, .h = .004f, .r = .02f, .lp = 2500, .gain = .5f },
    }},
    // click-chunk, crisp noise click then a low thunk as it seats
    [SFX_PLUG] = {{
        { .at = 0.000f, .wave = NOISE, .f0 = 9000, .a = .0005f, .h = .003f, .r = .01f, .lp = 6000, .gain = .9f },
        { .at = 0.000f, .wave = SINE, .f0 = 2200, .f1 = 1500, .a = .0005f, .h = .003f, .r = .012f, .gain = .3f },
        { .at = 0.035f, .wave = SINE, .f0 = 190, .f1 = 75, .slide = .05f, .a = .002f, .h = .03f, .r = .09f, .gain = 1 },
        { .at = 0.035f, .wave = SQUARE, .duty = .5f, .f0 = 110, .f1 = 60, .slide = .05f, .a = .002f, .h = .02f, .r = .07f, .lp = 600, .gain = .45f },
        { .at = 0.035f, .wave = SOFT_NOISE, .f0 = 2000, .a = .001f, .h = .01f, .r = .03f, .lp = 900, .gain = .35f },
    }},
    // ding-ding, G5 then C6 with a quiet E6 ringing over it
    [SFX_SUCCESS] = {{
        { .at = 0.00f, .wave = TRIANGLE, .f0 = G5, .a = .003f, .h = .05f, .r = .15f, .gain = 1 },
        { .at = 0.00f, .wave = SINE, .f0 = G5 * 2, .a = .003f, .h = .02f, .r = .1f, .gain = .2f },
        { .at = 0.13f, .wave = TRIANGLE, .f0 = C6, .a = .003f, .h = .08f, .r = .28f, .gain = 1 },
        { .at = 0.13f, .wave = SINE, .f0 = E6, .a = .003f, .h = .06f, .r = .25f, .gain = .3f },
        { .at = 0.13f, .wave = SINE, .f0 = C6 * 2, .a = .003f, .h = .02f, .r = .12f, .gain = .15f },
    }},
    // bip-bip up a fourth
    [SFX_SOUND_ON] = {{
        { .at = 0.00f, .wave = TRIANGLE, .f0 = E5, .a = .002f, .h = .03f, .r = .03f, .gain = 1 },
        { .at = 0.06f, .wave = TRIANGLE, .f0 = A5, .a = .002f, .h = .04f, .r = .06f, .gain = 1 },
    }},
    // one whistled note, sine with a slight vibrato and some air
    [SFX_WHISTLE] = {{
        { .wave = SINE, .f0 = G5, .a = .03f, .h = .11f, .r = .1f, .vib = .2f, .vib_hz = 5.5f, .gain = 1 },
        { .wave = SOFT_NOISE, .f0 = 6000, .a = .03f, .h = .08f, .r = .1f, .lp = 3000, .gain = .06f },
    }},

    // -- driving the real pointer --
    // magnetic clunk, a buzzy pull in then the plug slams home with a metal clang
    [SFX_GRAB] = {{
        { .at = 0.000f, .wave = SQUARE, .duty = .5f, .f0 = 90, .f1 = 150, .slide = .035f, .a = .004f, .h = .025f, .r = .012f, .vib = .8f, .vib_hz = 40, .lp = 900, .gain = .45f },
        { .at = 0.035f, .wave = NOISE, .f0 = 9000, .a = .0005f, .h = .003f, .r = .012f, .lp = 6000, .gain = .8f },
        { .at = 0.035f, .wave = SINE, .f0 = 200, .f1 = 70, .slide = .05f, .a = .002f, .h = .035f, .r = .1f, .gain = 1 },
        { .at = 0.035f, .wave = SQUARE, .duty = .5f, .f0 = 110, .f1 = 55, .slide = .05f, .a = .002f, .h = .02f, .r = .07f, .lp = 500, .gain = .4f },
        { .at = 0.038f, .wave = SINE, .f0 = 1250, .a = .001f, .h = .01f, .r = .09f, .gain = .18f },
        { .at = 0.038f, .wave = SINE, .f0 = 1850, .a = .001f, .h = .005f, .r = .06f, .gain = .1f },
    }},
    // chunky mouse click, "cl" (sharp plastic tick over a knock) then "ack" as the button comes back up
    [SFX_CLACK] = {{
        { .at = 0.000f, .wave = NOISE, .f0 = 7000, .a = .0005f, .h = .002f, .r = .008f, .lp = 5000, .gain = 1 },
        { .at = 0.000f, .wave = TRIANGLE, .f0 = 1800, .f1 = 1100, .slide = .006f, .a = .0005f, .h = .003f, .r = .012f, .gain = .5f },
        { .at = 0.006f, .wave = SINE, .f0 = 420, .f1 = 240, .slide = .02f, .a = .001f, .h = .012f, .r = .03f, .gain = .9f },
        { .at = 0.006f, .wave = SQUARE, .duty = .3f, .f0 = 220, .f1 = 160, .a = .001f, .h = .008f, .r = .02f, .lp = 1400, .gain = .35f },
        { .at = 0.032f, .wave = NOISE, .f0 = 6000, .a = .0005f, .h = .002f, .r = .006f, .lp = 4000, .gain = .35f },
        { .at = 0.032f, .wave = TRIANGLE, .f0 = 1500, .f1 = 1200, .a = .0005f, .h = .002f, .r = .008f, .gain = .25f },
    }},
    // one key on his little keyboard, a tiny "tk" short enough to do 15 a second
    [SFX_KEY] = {{
        { .at = 0.000f, .wave = SOFT_NOISE, .f0 = 6000, .a = .0005f, .h = .003f, .r = .01f, .lp = 3500, .gain = .7f },
        { .at = 0.000f, .wave = SINE, .f0 = 1500, .f1 = 1100, .slide = .008f, .a = .0005f, .h = .004f, .r = .012f, .gain = .6f },
        { .at = 0.002f, .wave = TRIANGLE, .f0 = 600, .f1 = 420, .a = .001f, .h = .004f, .r = .012f, .gain = .4f },
    }},
    // nnngh! shaky strained hum creeping up with some breath under it
    [SFX_HAUL] = {{
        { .at = 0.00f, .wave = SQUARE, .duty = .3f, .f0 = 150, .f1 = 185, .a = .05f, .h = .25f, .r = .12f, .vib = .7f, .vib_hz = 13, .lp = 900, .gain = .8f },
        { .at = 0.00f, .wave = TRIANGLE, .f0 = 150, .f1 = 185, .a = .05f, .h = .25f, .r = .12f, .vib = .7f, .vib_hz = 13, .gain = .6f },
        { .at = 0.00f, .wave = SOFT_NOISE, .f0 = 1200, .f1 = 800, .a = .06f, .h = .22f, .r = .12f, .lp = 700, .gain = .3f },
        { .at = 0.10f, .wave = SINE, .f0 = 300, .f1 = 370, .a = .05f, .h = .15f, .r = .1f, .vib = .7f, .vib_hz = 13, .gain = .15f },
    }},
    // pop! the plug comes off like a cork and the cable boings back
    [SFX_LETGO] = {{
        { .at = 0.00f, .wave = SINE, .f0 = 380, .f1 = 1300, .slide = .012f, .a = .0008f, .h = .006f, .r = .02f, .gain = 1 },
        { .at = 0.00f, .wave = NOISE, .f0 = 7000, .a = .0005f, .h = .002f, .r = .01f, .lp = 5000, .gain = .35f },
        { .at = 0.03f, .wave = TRIANGLE, .f0 = 900, .f1 = 600, .slide = .05f, .a = .003f, .h = .02f, .r = .06f, .vib = .4f, .vib_hz = 20, .lp = 3000, .gain = .45f },
    }},
    // victory lap fanfare, ta-ta-ta-TAAA (kept different from BOOT and SUCCESS on purpose)
    [SFX_VICTORY] = {{
        { .at = 0.00f, .wave = SQUARE, .duty = .4f, .f0 = G5, .a = .003f, .h = .035f, .r = .03f, .lp = 3200, .gain = .75f },
        { .at = 0.09f, .wave = SQUARE, .duty = .4f, .f0 = G5, .a = .003f, .h = .035f, .r = .03f, .lp = 3200, .gain = .75f },
        { .at = 0.18f, .wave = SQUARE, .duty = .4f, .f0 = G5, .a = .003f, .h = .035f, .r = .03f, .lp = 3200, .gain = .75f },
        { .at = 0.29f, .wave = SQUARE, .duty = .4f, .f0 = C6, .a = .004f, .h = .18f, .r = .3f, .vib = .25f, .vib_hz = 6, .lp = 3500, .gain = .8f },
        { .at = 0.29f, .wave = TRIANGLE, .f0 = E6, .a = .004f, .h = .15f, .r = .3f, .gain = .5f },
        { .at = 0.29f, .wave = TRIANGLE, .f0 = C5, .a = .004f, .h = .18f, .r = .3f, .gain = .6f },
    }},
    // whoo-ops, slide whistle up then tumbling down with a wobble
    [SFX_OOPS] = {{
        { .at = 0.00f, .wave = TRIANGLE, .f0 = 520, .f1 = 820, .slide = .07f, .a = .004f, .h = .06f, .r = .02f, .lp = 2600, .gain = .9f },
        { .at = 0.00f, .wave = SQUARE, .duty = .25f, .f0 = 520, .f1 = 820, .slide = .07f, .a = .004f, .h = .06f, .r = .02f, .lp = 1600, .gain = .3f },
        { .at = 0.10f, .wave = TRIANGLE, .f0 = 760, .f1 = 330, .slide = .12f, .a = .004f, .h = .1f, .r = .08f, .vib = .5f, .vib_hz = 14, .lp = 2400, .gain = 1 },
        { .at = 0.10f, .wave = SQUARE, .duty = .25f, .f0 = 760, .f1 = 330, .slide = .12f, .a = .004f, .h = .1f, .r = .08f, .vib = .5f, .vib_hz = 14, .lp = 1400, .gain = .3f },
    }},

    // -- system --
    // power down, basically BOOT backwards with C major tumbling down over a sagging sweep
    [SFX_QUIT] = {{
        { .at = 0.00f, .wave = SINE,     .f0 = 520, .f1 = 170, .a = .02f, .h = .1f, .r = .2f, .gain = .3f },
        { .at = 0.00f, .wave = TRIANGLE, .f0 = C6, .a = .004f, .h = .04f, .r = .07f, .gain = 1 },
        { .at = 0.07f, .wave = TRIANGLE, .f0 = G5, .a = .004f, .h = .04f, .r = .07f, .gain = 1 },
        { .at = 0.14f, .wave = TRIANGLE, .f0 = E5, .a = .004f, .h = .04f, .r = .07f, .gain = 1 },
        { .at = 0.21f, .wave = TRIANGLE, .f0 = C5, .f1 = C5 * .97f, .a = .004f, .h = .06f, .r = .2f, .gain = 1 },
        { .at = 0.21f, .wave = SQUARE, .duty = .5f, .f0 = C4, .f1 = C4 * .97f, .a = .004f, .h = .05f, .r = .16f, .lp = 1000, .gain = .2f },
    }},
    // soft power fail, two stuttering blips then a wobbly sag down to nothing
    [SFX_BRAIN_DIED] = {{
        { .at = 0.00f, .wave = SQUARE, .duty = .25f, .f0 = A5, .f1 = A5 * .92f, .a = .002f, .h = .025f, .r = .02f, .lp = 2000, .gain = .5f },
        { .at = 0.07f, .wave = SQUARE, .duty = .25f, .f0 = E5, .f1 = E5 * .92f, .a = .002f, .h = .02f, .r = .02f, .lp = 2000, .gain = .5f },
        { .at = 0.00f, .wave = SOFT_NOISE, .f0 = 3000, .a = .002f, .h = .03f, .r = .06f, .lp = 1500, .gain = .12f },
        { .at = 0.14f, .wave = TRIANGLE, .f0 = E5, .f1 = C4, .a = .005f, .h = .15f, .r = .35f, .vib = .6f, .vib_hz = 9, .lp = 1600, .gain = 1 },
        { .at = 0.14f, .wave = SINE, .f0 = E4, .f1 = 110, .a = .01f, .h = .15f, .r = .35f, .gain = .35f },
    }},

    // -- you and him --
    // hm-PH, short low hum then a lower huff with a puff of air
    [SFX_POKE_GRUMBLE] = {{
        { .at = 0.00f, .wave = SQUARE, .duty = .35f, .f0 = 220, .f1 = 208, .a = .006f, .h = .06f, .r = .04f, .lp = 1100, .gain = .8f },
        { .at = 0.00f, .wave = TRIANGLE, .f0 = 220, .f1 = 208, .a = .006f, .h = .06f, .r = .04f, .gain = .6f },
        { .at = 0.11f, .wave = SQUARE, .duty = .35f, .f0 = 247, .f1 = 175, .slide = .06f, .a = .004f, .h = .04f, .r = .08f, .lp = 1100, .gain = .8f },
        { .at = 0.11f, .wave = TRIANGLE, .f0 = 247, .f1 = 175, .slide = .06f, .a = .004f, .h = .04f, .r = .08f, .gain = .6f },
        { .at = 0.11f, .wave = SOFT_NOISE, .f0 = 2500, .a = .01f, .h = .03f, .r = .08f, .lp = 1400, .gain = .25f },
    }},
    // two tiny ticks with a bit of noise each, like a light rattle
    [SFX_SHAKE] = {{
        { .at = 0.00f, .wave = TRIANGLE, .f0 = 900, .f1 = 700, .a = .001f, .h = .008f, .r = .02f, .lp = 2500, .gain = .6f },
        { .at = 0.00f, .wave = SOFT_NOISE, .f0 = 5000, .a = .001f, .h = .006f, .r = .02f, .lp = 3000, .gain = .5f },
        { .at = 0.03f, .wave = TRIANGLE, .f0 = 1050, .f1 = 820, .a = .001f, .h = .006f, .r = .02f, .lp = 2500, .gain = .45f },
        { .at = 0.03f, .wave = SOFT_NOISE, .f0 = 5000, .a = .001f, .h = .005f, .r = .02f, .lp = 3000, .gain = .4f },
    }},
    // springs up an octave then goes all wobbly (bwoing)
    [SFX_DIZZY] = {{
        { .wave = TRIANGLE, .f0 = 260, .f1 = 520, .slide = .07f, .a = .005f, .h = .15f, .r = .25f, .vib = 1.0f, .vib_hz = 7, .lp = 1800, .gain = 1 },
        { .wave = SQUARE, .duty = .5f, .f0 = 260, .f1 = 520, .slide = .07f, .a = .005f, .h = .12f, .r = .2f, .vib = 1.0f, .vib_hz = 7, .lp = 1200, .gain = .3f },
    }},
    [SFX_YES] = {{
        { .at = 0.00f, .wave = TRIANGLE, .f0 = C5, .a = .002f, .h = .03f, .r = .03f, .gain = 1 },
        { .at = 0.07f, .wave = TRIANGLE, .f0 = G5, .a = .002f, .h = .04f, .r = .08f, .gain = 1 },
        { .at = 0.07f, .wave = SINE, .f0 = G5 * 2, .a = .002f, .h = .02f, .r = .05f, .gain = .2f },
    }},
    // a bit duller than yes
    [SFX_NO] = {{
        { .at = 0.00f, .wave = TRIANGLE, .f0 = E5, .a = .002f, .h = .03f, .r = .03f, .gain = 1 },
        { .at = 0.07f, .wave = TRIANGLE, .f0 = A4, .a = .002f, .h = .04f, .r = .08f, .gain = 1 },
        { .at = 0.07f, .wave = SQUARE, .duty = .5f, .f0 = A4, .a = .002f, .h = .03f, .r = .06f, .lp = 1400, .gain = .3f },
    }},
    // small lock click, a crisp tick then a tiny metal catch
    [SFX_LOCK] = {{
        { .at = 0.000f, .wave = NOISE, .f0 = 8000, .a = .0005f, .h = .002f, .r = .008f, .lp = 5000, .gain = .5f },
        { .at = 0.000f, .wave = SINE, .f0 = 1800, .f1 = 1400, .a = .0005f, .h = .006f, .r = .025f, .gain = .6f },
        { .at = 0.030f, .wave = SINE, .f0 = 900, .f1 = 700, .a = .001f, .h = .015f, .r = .04f, .gain = .8f },
        { .at = 0.030f, .wave = TRIANGLE, .f0 = 1350, .a = .001f, .h = .01f, .r = .03f, .lp = 3000, .gain = .3f },
    }},
    // ka-chink then C6 and E6 chiming out
    [SFX_UNLOCK] = {{
        { .at = 0.00f, .wave = NOISE, .f0 = 8000, .a = .0005f, .h = .002f, .r = .01f, .lp = 5000, .gain = .5f },
        { .at = 0.00f, .wave = SINE, .f0 = 1400, .f1 = 1000, .a = .0005f, .h = .006f, .r = .025f, .gain = .5f },
        { .at = 0.06f, .wave = NOISE, .f0 = 9000, .a = .0005f, .h = .003f, .r = .012f, .lp = 6000, .gain = .5f },
        { .at = 0.06f, .wave = SINE, .f0 = 2400, .f1 = 2000, .a = .0005f, .h = .008f, .r = .03f, .gain = .45f },
        { .at = 0.13f, .wave = TRIANGLE, .f0 = C6, .a = .003f, .h = .05f, .r = .3f, .gain = 1 },
        { .at = 0.20f, .wave = SINE, .f0 = E6, .a = .003f, .h = .05f, .r = .3f, .gain = .6f },
    }},
    // soft low double buzz, the second one a hair lower
    [SFX_WRONG] = {{
        { .at = 0.00f, .wave = SQUARE, .duty = .5f, .f0 = 150, .a = .004f, .h = .05f, .r = .03f, .lp = 700, .gain = 1 },
        { .at = 0.00f, .wave = TRIANGLE, .f0 = 150, .a = .004f, .h = .05f, .r = .03f, .gain = .6f },
        { .at = 0.11f, .wave = SQUARE, .duty = .5f, .f0 = 140, .a = .004f, .h = .05f, .r = .05f, .lp = 700, .gain = 1 },
        { .at = 0.11f, .wave = TRIANGLE, .f0 = 140, .a = .004f, .h = .05f, .r = .05f, .gain = .6f },
    }},
    // never mind, little blip sliding down an octave
    [SFX_CANCEL] = {{
        { .wave = SQUARE, .duty = .25f, .f0 = 800, .f1 = 400, .slide = .05f, .a = .002f, .h = .03f, .r = .05f, .lp = 2000, .gain = .5f },
        { .wave = SINE, .f0 = 800, .f1 = 400, .slide = .05f, .a = .002f, .h = .03f, .r = .05f, .gain = 1 },
    }},

    // -- the bubble --
    // paper flick, quick bright brush of noise with a tiny tick in it
    [SFX_PAGE] = {{
        { .wave = SOFT_NOISE, .f0 = 14000, .f1 = 7000, .a = .008f, .h = .015f, .r = .04f, .lp = 4500, .gain = .7f },
        { .wave = SOFT_NOISE, .f0 = 12000, .f1 = 6000, .a = .006f, .h = .015f, .r = .04f, .lp = 4500, .gain = .7f },
        { .wave = SINE, .f0 = 1200, .f1 = 1600, .a = .001f, .h = .005f, .r = .015f, .gain = .2f },
    }},
    // tiny tick
    [SFX_HISTORY] = {{
        { .wave = SINE, .f0 = 1500, .f1 = 1200, .a = .0008f, .h = .006f, .r = .018f, .gain = 1 },
        { .wave = TRIANGLE, .f0 = 750, .a = .001f, .h = .004f, .r = .012f, .gain = .3f },
    }},
    // little bell struck twice (G5 then C6), sines plus the off key overtone a real bell has
    [SFX_REMIND] = {{
        { .at = 0.00f, .wave = SINE, .f0 = G5, .a = .002f, .h = .03f, .r = .45f, .gain = 1 },
        { .at = 0.00f, .wave = SINE, .f0 = G5 * 2.76f, .a = .001f, .h = .01f, .r = .18f, .gain = .3f },
        { .at = 0.18f, .wave = SINE, .f0 = C6, .a = .002f, .h = .03f, .r = .55f, .gain = 1 },
        { .at = 0.18f, .wave = SINE, .f0 = C6 * 2.76f, .a = .001f, .h = .01f, .r = .2f, .gain = .3f },
        { .at = 0.18f, .wave = SINE, .f0 = C6 * 5.4f, .a = .001f, .h = .005f, .r = .08f, .gain = .1f },
        { .at = 0.18f, .wave = TRIANGLE, .f0 = C5, .a = .004f, .h = .03f, .r = .3f, .gain = .25f },
    }},
    // bwomp, dull square sagging down with a sine an octave under it
    [SFX_ERROR] = {{
        { .wave = SQUARE, .duty = .5f, .f0 = 330, .f1 = 150, .slide = .15f, .a = .005f, .h = .08f, .r = .12f, .lp = 900, .gain = .8f },
        { .wave = TRIANGLE, .f0 = 330, .f1 = 150, .slide = .15f, .a = .005f, .h = .08f, .r = .12f, .gain = .6f },
        { .wave = SINE, .f0 = 165, .f1 = 75, .slide = .15f, .a = .005f, .h = .08f, .r = .12f, .gain = .4f },
    }},
    // curious "hm?", soft low hum then a sine bending up. pure sines since it plays on every reply
    [SFX_THINK] = {{
        { .at = 0.00f, .wave = SINE, .f0 = A4, .f1 = A4 * 1.03f, .a = .015f, .h = .04f, .r = .03f, .gain = .7f },
        { .at = 0.08f, .wave = SINE, .f0 = B4, .f1 = E5, .slide = .06f, .a = .012f, .h = .04f, .r = .06f, .gain = 1 },
    }},

    // -- emotion beats --
    // boing as the hop lands, with a bit of wobble
    [SFX_HOP] = {{
        { .wave = TRIANGLE, .f0 = 180, .f1 = 440, .slide = .05f, .a = .002f, .h = .06f, .r = .08f, .vib = .5f, .vib_hz = 18, .lp = 2000, .gain = 1 },
        { .wave = SINE, .f0 = 180, .f1 = 440, .slide = .05f, .a = .002f, .h = .06f, .r = .08f, .vib = .5f, .vib_hz = 18, .gain = .5f },
    }},
    // glitter, G5 C6 E6 G6 quick in a row with a high sine shimmering on top
    [SFX_SPARKLE] = {{
        { .at = 0.00f, .wave = TRIANGLE, .f0 = G5, .a = .002f, .h = .015f, .r = .06f, .gain = 1 },
        { .at = 0.04f, .wave = TRIANGLE, .f0 = C6, .a = .002f, .h = .015f, .r = .06f, .gain = 1 },
        { .at = 0.08f, .wave = TRIANGLE, .f0 = E6, .a = .002f, .h = .015f, .r = .06f, .gain = 1 },
        { .at = 0.12f, .wave = TRIANGLE, .f0 = G6, .a = .002f, .h = .02f, .r = .12f, .gain = 1 },
        { .at = 0.12f, .wave = SINE, .f0 = G6 * 2, .a = .002f, .h = .02f, .r = .15f, .vib = .15f, .vib_hz = 12, .gain = .25f },
    }},
    // "ha" one short bouncy pulse, dips a little
    [SFX_LAUGH] = {{
        { .wave = SQUARE, .duty = .4f, .f0 = 620, .f1 = 520, .slide = .03f, .a = .002f, .h = .03f, .r = .03f, .lp = 2600, .gain = .8f },
        { .wave = TRIANGLE, .f0 = 620, .f1 = 520, .slide = .03f, .a = .002f, .h = .03f, .r = .03f, .gain = .5f },
    }},
    // soft sine scooping up with a wobble, pwip
    [SFX_HEART] = {{
        { .wave = SINE, .f0 = 600, .f1 = 900, .slide = .04f, .a = .004f, .h = .03f, .r = .06f, .vib = .3f, .vib_hz = 10, .gain = 1 },
        { .wave = SINE, .f0 = 1200, .f1 = 1800, .slide = .04f, .a = .004f, .h = .02f, .r = .04f, .vib = .3f, .vib_hz = 10, .gain = .15f },
    }},
    // "!" quick bright pop up onto B5
    [SFX_STING] = {{
        { .wave = SQUARE, .duty = .3f, .f0 = B4 * 1.4f, .f1 = B4 * 2, .slide = .015f, .a = .001f, .h = .04f, .r = .06f, .lp = 3500, .gain = .7f },
        { .wave = TRIANGLE, .f0 = B4 * 1.4f, .f1 = B4 * 2, .slide = .015f, .a = .001f, .h = .04f, .r = .06f, .gain = .5f },
        { .wave = SINE, .f0 = B4 * 2.8f, .f1 = B4 * 4, .slide = .015f, .a = .001f, .h = .02f, .r = .04f, .gain = .15f },
    }},
    // huh? rising wobbly question
    [SFX_HUH] = {{
        { .wave = TRIANGLE, .f0 = 330, .f1 = 520, .a = .01f, .h = .14f, .r = .1f, .vib = 1.0f, .vib_hz = 12, .lp = 2200, .gain = 1 },
        { .wave = SQUARE, .duty = .5f, .f0 = 330, .f1 = 520, .a = .01f, .h = .14f, .r = .1f, .vib = 1.0f, .vib_hz = 12, .lp = 1400, .gain = .35f },
    }},
    // smug glint, bright little bell on E6 with a glassy overtone
    [SFX_TING] = {{
        { .wave = SINE, .f0 = E6, .a = .001f, .h = .02f, .r = .3f, .gain = 1 },
        { .wave = SINE, .f0 = E6 * 1.5f, .a = .001f, .h = .01f, .r = .15f, .gain = .3f },
        { .wave = TRIANGLE, .f0 = E6, .a = .001f, .h = .01f, .r = .1f, .lp = 4000, .gain = .3f },
        { .wave = SOFT_NOISE, .f0 = 8000, .a = .001f, .h = .01f, .r = .03f, .lp = 7000, .gain = .1f },
    }},
    // soft breath out with a faint falling tone in it
    [SFX_SIGH] = {{
        { .wave = SOFT_NOISE, .f0 = 9000, .f1 = 5000, .a = .12f, .h = .15f, .r = .4f, .lp = 1100, .gain = 1 },
        { .wave = SINE, .f0 = A4, .f1 = 300, .a = .1f, .h = .15f, .r = .35f, .vib = .2f, .vib_hz = 4, .gain = .3f },
    }},
    // nervous tremble, four shaky blips over a quivering hum
    [SFX_QUIVER] = {{
        { .at = 0.000f, .wave = TRIANGLE, .f0 = G4, .f1 = G4 * .95f, .a = .01f, .h = .14f, .r = .08f, .vib = .5f, .vib_hz = 22, .lp = 1800, .gain = .5f },
        { .at = 0.000f, .wave = SINE, .f0 = A4, .a = .004f, .h = .015f, .r = .02f, .gain = 1 },
        { .at = 0.045f, .wave = SINE, .f0 = A4 * .98f, .a = .004f, .h = .015f, .r = .02f, .gain = .9f },
        { .at = 0.090f, .wave = SINE, .f0 = A4, .a = .004f, .h = .015f, .r = .02f, .gain = .8f },
        { .at = 0.135f, .wave = SINE, .f0 = A4 * .97f, .a = .004f, .h = .015f, .r = .03f, .gain = .7f },
    }},
    // falling thump and a dull scuff
    [SFX_STOMP] = {{
        { .wave = SINE, .f0 = 150, .f1 = 60, .slide = .05f, .a = .001f, .h = .02f, .r = .07f, .gain = 1 },
        { .wave = SQUARE, .duty = .5f, .f0 = 90, .f1 = 50, .slide = .05f, .a = .001f, .h = .015f, .r = .05f, .lp = 400, .gain = .3f },
        { .wave = SOFT_NOISE, .f0 = 1200, .a = .001f, .h = .008f, .r = .03f, .lp = 500, .gain = .5f },
    }},
    // short nasal hum then a huff of air (hmf)
    [SFX_HMPH] = {{
        { .at = 0.00f, .wave = SQUARE, .duty = .35f, .f0 = 196, .f1 = 175, .a = .01f, .h = .06f, .r = .05f, .lp = 900, .gain = .9f },
        { .at = 0.00f, .wave = TRIANGLE, .f0 = 196, .f1 = 175, .a = .01f, .h = .06f, .r = .05f, .gain = .5f },
        { .at = 0.07f, .wave = SOFT_NOISE, .f0 = 3000, .f1 = 1500, .a = .005f, .h = .04f, .r = .1f, .lp = 2000, .gain = .5f },
    }},

    // -- sleep --
    // tiny tick as his head snaps up
    [SFX_NOD] = {{
        { .wave = SINE, .f0 = 700, .f1 = 1000, .slide = .015f, .a = .001f, .h = .008f, .r = .02f, .gain = 1 },
        { .wave = SOFT_NOISE, .f0 = 4000, .a = .001f, .h = .003f, .r = .01f, .lp = 3000, .gain = .3f },
    }},
    // sleepy "mm" settling down with a little breath
    [SFX_SETTLE] = {{
        { .wave = SINE, .f0 = E4, .f1 = C4, .a = .08f, .h = .2f, .r = .35f, .vib = .15f, .vib_hz = 4, .gain = 1 },
        { .wave = TRIANGLE, .f0 = E4, .f1 = C4, .a = .08f, .h = .2f, .r = .3f, .lp = 900, .gain = .4f },
        { .wave = SOFT_NOISE, .f0 = 3000, .a = .1f, .h = .15f, .r = .3f, .lp = 1200, .gain = .1f },
    }},
    // snot bubble pop, tiny high blip
    [SFX_SNORE_POP] = {{
        { .wave = SINE, .f0 = 900, .f1 = 2000, .slide = .012f, .a = .0008f, .h = .006f, .r = .02f, .gain = 1 },
        { .wave = SOFT_NOISE, .f0 = 6000, .a = .0005f, .h = .002f, .r = .008f, .lp = 4000, .gain = .2f },
    }},
    // startled "!", quick jump up
    [SFX_WAKE_JOLT] = {{
        { .wave = SQUARE, .duty = .35f, .f0 = 400, .f1 = 900, .slide = .03f, .a = .001f, .h = .05f, .r = .07f, .lp = 2500, .gain = .7f },
        { .wave = TRIANGLE, .f0 = 400, .f1 = 900, .slide = .03f, .a = .001f, .h = .05f, .r = .07f, .gain = .5f },
        { .wave = SINE, .f0 = 800, .f1 = 1800, .slide = .03f, .a = .001f, .h = .02f, .r = .04f, .gain = .15f },
    }},
    // drowsy "mm?" going up at the end
    [SFX_WAKE_SOFT] = {{
        { .wave = SINE, .f0 = D4, .f1 = G4, .a = .04f, .h = .1f, .r = .1f, .vib = .2f, .vib_hz = 5, .gain = 1 },
        { .wave = TRIANGLE, .f0 = D4, .f1 = G4, .a = .04f, .h = .1f, .r = .1f, .lp = 1000, .gain = .3f },
    }},

    // -- pointing --
    // quick rising zip as the cable stretches out
    [SFX_ZIP_OUT] = {{
        { .wave = TRIANGLE, .f0 = 400, .f1 = 1600, .a = .003f, .h = .06f, .r = .03f, .gain = .6f },
        { .wave = SQUARE, .duty = .25f, .f0 = 400, .f1 = 1600, .a = .003f, .h = .06f, .r = .03f, .lp = 3000, .gain = .4f },
        { .wave = SOFT_NOISE, .f0 = 3000, .f1 = 9000, .a = .003f, .h = .05f, .r = .03f, .lp = 5000, .gain = .3f },
    }},
    // falling zwip as it snaps back
    [SFX_ZIP_IN] = {{
        { .wave = TRIANGLE, .f0 = 1500, .f1 = 450, .a = .002f, .h = .04f, .r = .03f, .gain = .6f },
        { .wave = SQUARE, .duty = .25f, .f0 = 1500, .f1 = 450, .a = .002f, .h = .04f, .r = .03f, .lp = 3000, .gain = .4f },
        { .wave = SOFT_NOISE, .f0 = 9000, .f1 = 3000, .a = .002f, .h = .035f, .r = .03f, .lp = 5000, .gain = .3f },
    }},
    // shimmer, E6 G6 C7 twinkling over the PLUG thunk (its ambient so it sits quieter under it)
    [SFX_RING] = {{
        { .at = 0.00f, .wave = SINE, .f0 = E6, .a = .004f, .h = .03f, .r = .2f, .vib = .15f, .vib_hz = 8, .gain = 1 },
        { .at = 0.03f, .wave = SINE, .f0 = G6, .a = .004f, .h = .03f, .r = .2f, .vib = .15f, .vib_hz = 8, .gain = .8f },
        { .at = 0.06f, .wave = SINE, .f0 = C7, .a = .004f, .h = .03f, .r = .22f, .vib = .15f, .vib_hz = 8, .gain = .6f },
        { .at = 0.06f, .wave = SOFT_NOISE, .f0 = 9000, .a = .01f, .h = .03f, .r = .1f, .lp = 8000, .gain = .05f },
    }},
    // pok as the plug lets go then a little E5 -> C5 going down
    [SFX_UNPLUG] = {{
        { .at = 0.00f, .wave = SINE, .f0 = 900, .f1 = 500, .slide = .015f, .a = .0008f, .h = .01f, .r = .03f, .gain = 1 },
        { .at = 0.00f, .wave = SOFT_NOISE, .f0 = 5000, .a = .0005f, .h = .003f, .r = .01f, .lp = 3500, .gain = .3f },
        { .at = 0.05f, .wave = TRIANGLE, .f0 = E5, .f1 = C5, .slide = .05f, .a = .003f, .h = .04f, .r = .08f, .gain = .7f },
        { .at = 0.05f, .wave = SINE, .f0 = E5 * 2, .f1 = C5 * 2, .slide = .05f, .a = .003f, .h = .02f, .r = .05f, .gain = .12f },
    }},
};

// one syllable of babble per emotion at its base pitch, about 50 ms (love a bit longer)
// so it still sounds like a blip pitched anywhere from 0.7x to 1.6x
static const Recipe VOICES[EMOTION_COUNT] = {
    // neutral friendly square that settles down a bit like speech does
    [EMOTION_NONE] = {{
        { .wave = SQUARE, .duty = .5f, .f0 = G4, .f1 = G4 * .94f, .a = .003f, .h = .02f, .r = .03f, .lp = 2600, .gain = .8f },
        { .wave = TRIANGLE, .f0 = G4, .f1 = G4 * .94f, .a = .003f, .h = .02f, .r = .03f, .gain = .5f },
    }},
    // brighter, +3 semitones with a little upturn
    [EMOTION_HAPPY] = {{
        { .wave = SQUARE, .duty = .4f, .f0 = 466.16f, .f1 = 466.16f * 1.03f, .a = .003f, .h = .02f, .r = .03f, .lp = 3200, .gain = .8f },
        { .wave = TRIANGLE, .f0 = 466.16f, .f1 = 466.16f * 1.03f, .a = .003f, .h = .02f, .r = .03f, .gain = .5f },
    }},
    // higher and shorter
    [EMOTION_EXCITED] = {{
        { .wave = SQUARE, .duty = .35f, .f0 = C5, .f1 = C5 * 1.05f, .a = .002f, .h = .015f, .r = .022f, .lp = 3500, .gain = .8f },
        { .wave = TRIANGLE, .f0 = C5, .f1 = C5 * 1.05f, .a = .002f, .h = .015f, .r = .022f, .gain = .5f },
    }},
    // staccato, quick hop down cut short
    [EMOTION_LAUGHING] = {{
        { .wave = SQUARE, .duty = .4f, .f0 = A4 * 1.2f, .f1 = A4, .slide = .012f, .a = .002f, .h = .012f, .r = .02f, .lp = 3000, .gain = .8f },
        { .wave = TRIANGLE, .f0 = A4 * 1.2f, .f1 = A4, .slide = .012f, .a = .002f, .h = .012f, .r = .02f, .gain = .5f },
    }},
    // cooing "mwoo", a round sine scooping up 3 semitones into the note with a soft attack
    // and warm vibrato, plus a bit of triangle under it for the "m"
    [EMOTION_LOVE] = {{
        { .wave = SINE, .f0 = C5 * .84f, .f1 = C5, .slide = .028f, .a = .014f, .h = .03f, .r = .038f, .vib = .45f, .vib_hz = 11, .gain = 1 },
        { .wave = SINE, .f0 = C5 * 2 * .84f, .f1 = C5 * 2, .slide = .028f, .a = .016f, .h = .02f, .r = .03f, .vib = .45f, .vib_hz = 11, .gain = .14f },
        { .wave = TRIANGLE, .f0 = C5 * .84f, .f1 = C5, .slide = .028f, .a = .01f, .h = .02f, .r = .03f, .vib = .45f, .vib_hz = 11, .lp = 1300, .gain = .3f },
    }},
    // bright with a pop at the start
    [EMOTION_SURPRISED] = {{
        { .wave = SQUARE, .duty = .3f, .f0 = B4 * 1.5f, .f1 = B4, .slide = .008f, .a = .001f, .h = .02f, .r = .03f, .lp = 4000, .gain = .8f },
        { .wave = TRIANGLE, .f0 = B4 * 1.5f, .f1 = B4, .slide = .008f, .a = .001f, .h = .02f, .r = .03f, .gain = .5f },
    }},
    // wobble
    [EMOTION_CONFUSED] = {{
        { .wave = TRIANGLE, .f0 = G4, .a = .003f, .h = .025f, .r = .03f, .vib = 1.2f, .vib_hz = 20, .gain = 1 },
        { .wave = SQUARE, .duty = .5f, .f0 = G4, .a = .003f, .h = .025f, .r = .03f, .vib = 1.2f, .vib_hz = 20, .lp = 1800, .gain = .5f },
    }},
    // sly sounding saw in the mid range, slides up a hair
    [EMOTION_SMUG] = {{
        { .wave = SAW, .f0 = 330, .f1 = 345, .a = .004f, .h = .022f, .r = .03f, .lp = 1800, .gain = .8f },
        { .wave = TRIANGLE, .f0 = 330, .f1 = 345, .a = .004f, .h = .022f, .r = .03f, .gain = .4f },
    }},
    // low soft triangle, sagging
    [EMOTION_SAD] = {{
        { .wave = TRIANGLE, .f0 = 262, .f1 = 262 * .92f, .a = .006f, .h = .025f, .r = .035f, .lp = 1800, .gain = 1 },
    }},
    // low soft triangle with a nervous tremble
    [EMOTION_WORRIED] = {{
        { .wave = TRIANGLE, .f0 = 294, .f1 = 294 * .96f, .a = .005f, .h = .025f, .r = .03f, .vib = .3f, .vib_hz = 24, .lp = 2000, .gain = 1 },
    }},
    // gruff, low and narrow duty, darkened
    [EMOTION_ANNOYED] = {{
        { .wave = SQUARE, .duty = .15f, .f0 = A4 / 2, .f1 = A4 / 2 * .95f, .a = .003f, .h = .025f, .r = .03f, .lp = 1500, .gain = 1 },
        { .wave = TRIANGLE, .f0 = A4 / 2, .f1 = A4 / 2 * .95f, .a = .003f, .h = .025f, .r = .03f, .gain = .4f },
    }},
};

// each categorys loudness in RMS dBFS, ambient sits well under the rest
static const float TARGET_DB[SFX_CAT_COUNT] = {
    [SFX_CAT_UI] = -18, [SFX_CAT_VOICE] = -20, [SFX_CAT_BODY] = -19, [SFX_CAT_AMBIENT] = -24,
};

static const char *const EMOTION_NAMES[EMOTION_COUNT] = {
    "none", "happy", "excited", "laughing", "love", "surprised",
    "confused", "smug", "sad", "worried", "annoyed",
};

// ---- the renderer ----

static float seg_len(const Seg *s) { return s->a + s->h + s->r; }

static bool has_recipe(const Recipe *rc) { return seg_len(&rc->seg[0]) > 0; }

// xorshift32 for the noise, seeded per sound so a render comes out the same every time
static float noise_next(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return (float)(*s >> 8) / 8388608.0f - 1.0f;      // [-1, 1)
}

// PolyBLEP, rounds off the square and saw jumps so they dont alias into fizz
static float blep(float t, float dt)
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0;
}

static float envelope(const Seg *s, float t)
{
    if (t < s->a) return t / s->a;
    t -= s->a;
    if (t < s->h) return 1;
    t -= s->h;
    float u = 1.0f - t / s->r;
    return u > 0 ? u * u : 0;                      // squared so it falls quick into a quiet tail
}

static void render_seg(const Seg *s, uint32_t seed, float *out, int frames)
{
    int start = (int)(s->at * SYNTH_RATE);
    int n = (int)(seg_len(s) * SYNTH_RATE);
    float duty = s->duty > 0 ? s->duty : 0.5f;
    float f1 = s->f1 > 0 ? s->f1 : s->f0;
    float slide = s->slide > 0 ? s->slide : seg_len(s);
    float lp_k = s->lp > 0 ? 1.0f - expf(-2.0f * PI * s->lp / SYNTH_RATE) : 1.0f;
    float phase = 0, lp = 0;
    uint32_t rng = seed ? seed : 1;
    float held = noise_next(&rng), next = noise_next(&rng);

    for (int i = 0; i < n && start + i < frames; i++) {
        float t = (float)i / SYNTH_RATE;
        float k = t < slide ? t / slide : 1.0f;
        float f = s->f0 * powf(f1 / s->f0, k);
        if (s->vib > 0) f *= powf(2.0f, s->vib * sinf(2.0f * PI * s->vib_hz * t) / 12.0f);
        float dt = f / SYNTH_RATE;
        if (dt > 0.5f) dt = 0.5f;

        float v;
        switch (s->wave) {
        case SQUARE:
            v = phase < duty ? 1.0f : -1.0f;
            v += blep(phase, dt);
            v -= blep(fmodf(phase + 1.0f - duty, 1.0f), dt);
            v -= 2.0f * duty - 1.0f;                  // narrow pulses carry DC, take it out
            break;
        case TRIANGLE: v = 4.0f * fabsf(phase - 0.5f) - 1.0f; break;
        case SINE:     v = sinf(2.0f * PI * phase); break;
        case SAW:      v = 2.0f * phase - 1.0f - blep(phase, dt); break;
        case NOISE:    v = held; break;
        default:       v = held + (next - held) * phase; break;    // SOFT_NOISE glides between picks
        }
        phase += dt;
        if (phase >= 1.0f) {
            phase -= 1.0f;
            held = next;
            next = noise_next(&rng);
        }
        lp += (v - lp) * lp_k;
        out[start + i] += lp * envelope(s, t) * s->gain;
    }
}

static float rms(const float *x, int n)
{
    double sum = 0;
    for (int i = 0; i < n; i++) sum += (double)x[i] * x[i];
    return n > 0 ? (float)sqrt(sum / n) : 0;
}

// mixes the notes, fades the ends, normalises to the categorys loudness and limits the peak
static int16_t *render(const Recipe *rc, SfxCategory cat, uint32_t seed, int *frames)
{
    *frames = 0;
    if (!has_recipe(rc)) return NULL;
    float len = 0;
    for (int i = 0; i < MAX_SEGS; i++) {
        float end = rc->seg[i].at + seg_len(&rc->seg[i]);
        if (end > len) len = end;
    }
    int n = (int)(len * SYNTH_RATE) + 1;
    float *mix = calloc((size_t)n, sizeof *mix);
    int16_t *pcm = malloc((size_t)n * sizeof *pcm);
    if (!mix || !pcm) { free(mix); free(pcm); return NULL; }

    for (int i = 0; i < MAX_SEGS; i++)
        if (seg_len(&rc->seg[i]) > 0) render_seg(&rc->seg[i], seed * 2654435761u + (uint32_t)i * 40503u, mix, n);

    int fin = (int)(FADE_IN * SYNTH_RATE), fout = (int)(FADE_OUT * SYNTH_RATE);
    for (int i = 0; i < fin && i < n; i++) mix[i] *= (float)i / fin;
    for (int i = 0; i < fout && i < n; i++) mix[n - 1 - i] *= (float)i / fout;

    // loudness first then the peak, it only ever gets scaled down (never clipped) so it stays soft
    float level = rms(mix, n), peak = 0;
    float gain = level > 0 ? powf(10.0f, TARGET_DB[cat] / 20.0f) / level : 0;
    for (int i = 0; i < n; i++) {
        mix[i] *= gain;
        if (fabsf(mix[i]) > peak) peak = fabsf(mix[i]);
    }
    float limit = peak > CEILING ? CEILING / peak : 1.0f;
    for (int i = 0; i < n; i++) pcm[i] = (int16_t)lrintf(mix[i] * limit * 32767.0f);

    free(mix);
    *frames = n;
    return pcm;
}

// ---- the public side ----

const SfxInfo *synth_info(SfxId id)
{
    return &INFO[id];
}

float synth_target_db(SfxCategory cat)
{
    return TARGET_DB[cat];
}

float synth_rms_db(const int16_t *samples, int frames)
{
    double sum = 0;
    for (int i = 0; i < frames; i++) {
        double v = samples[i] / 32767.0;
        sum += v * v;
    }
    return sum > 0 ? (float)(10.0 * log10(sum / frames)) : -200.0f;
}

const char *synth_emotion_name(Emotion emotion)
{
    return EMOTION_NAMES[emotion];
}

int16_t *synth_render(SfxId id, int *frames)
{
    return render(&RECIPES[id], INFO[id].category, 0x5EED0000u + (uint32_t)id, frames);
}

int16_t *synth_render_voice(Emotion emotion, int *frames)
{
    return render(&VOICES[emotion], SFX_CAT_VOICE, 0x701CE000u + (uint32_t)emotion, frames);
}

// ---- his little language ----

// each letters step on the scale (0..5), vowels sit higher and consonants lower so words
// get a lilt
static const unsigned char LETTER_STEP[26] = {
//  a  b  c  d  e  f  g  h  i  j  k  l  m  n  o  p  q  r  s  t  u  v  w  x  y  z
    3, 1, 2, 0, 4, 1, 0, 2, 5, 3, 1, 2, 0, 3, 2, 1, 3, 2, 0, 3, 2, 1, 0, 2, 4, 1,
};

// pentatonic steps in semitones from his base pitch. sad moods talk in minor, love coos in
// a narrower sweeter band that barely goes under his base (a minor third under it, then up
// through the major triad with a soft fourth)
static const float MAJOR_STEPS[6] = { -5, -3, 0, 2, 4, 7 };
static const float MINOR_STEPS[6] = { -7, -5, -2, 0, 3, 5 };
static const float LOVE_STEPS[6]  = { -3, 0, 2, 4, 5, 7 };
#define QUESTION_LIFT 4.0f        // semitones the syllable before a '?' goes up

float synth_voice_pitch(Emotion emotion, int codepoint, int next)
{
    if (codepoint >= 'A' && codepoint <= 'Z') codepoint += 'a' - 'A';
    if (codepoint < 'a' || codepoint > 'z') return 0;
    bool minor = emotion == EMOTION_SAD || emotion == EMOTION_WORRIED || emotion == EMOTION_ANNOYED;
    const float *steps = emotion == EMOTION_LOVE ? LOVE_STEPS : minor ? MINOR_STEPS : MAJOR_STEPS;
    float semis = steps[LETTER_STEP[codepoint - 'a']];
    if (next == '?') semis += QUESTION_LIFT;
    return powf(2.0f, semis / 12.0f);
}

// the tune he whistles, a note every ~1.3 s, lazy major pentatonic loop
static const float WHISTLE_TUNE[8] = { 0, 4, 7, 4, 9, 7, 4, 2 };

float synth_whistle_pitch(int n)
{
    if (n < 0) n = 0;
    return powf(2.0f, WHISTLE_TUNE[n % 8] / 12.0f);
}
