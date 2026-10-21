// acting out the emotion Claude tags each reply with. each one gets its own face, a
// talking mouth that flaps with the speaker grille, an entrance when the reply starts,
// arm gestures and some effects on top (hearts, tears, sparkles, steam...). once the reply
// is done typing it lingers for LINGER secs, eases out and hands back with a blink.
// its all worked out from m->emotion_age / m->time, the only state is the linger clock.
// emotion_update also fires the entrance beats (MEV_BEAT) for sounds

#include <math.h>
#include <stdio.h>

#include "mascot_kit.h"

// tuning

#define LINGER      3.5f    // secs the emotion hangs around after the text is done typing
#define EASE_OUT    0.6f    // last secs of the linger, the arms settle and he calms down
#define FX_MARGIN   0.15f   // effects finish this long before the face hands over
#define BLINK_SWAP  0.12f   // the very end, eyes shut so swapping to the idle face looks like a blink
#define NO_BLINK    0.6f    // secs with no blinking at the start (the entrance)
#define TALK_PERIOD 0.36f   // same mouth cycle as the built in face and the speaker grille

#define PI_F 3.14159265f

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int roundi(float v) { return (int)floorf(v + 0.5f); }

// square wave, true for the first duty fraction of every period secs
static bool pulse(float t, float period, float duty)
{
    return fmodf(t, period) < period * duty;
}

// parabola hop, 0 outside [t0, t0+dur] and -height (rows up) in the middle
static int hop(float a, float t0, float dur, float height)
{
    if (a < t0 || a > t0 + dur) return 0;
    float u = (a - t0) / dur;
    return -roundi(height * 4.0f * u * (1.0f - u));
}

static bool in(float a, float t0, float t1) { return a >= t0 && a < t1; }

static bool valid(Emotion e) { return e > EMOTION_NONE && e < EMOTION_COUNT; }

static bool active(const Mascot *m)
{
    if (!valid(m->emotion)) return false;
    if (m->mood == MOOD_TALKING) return true;
    return m->mood == MOOD_IDLE && m->emo.quiet < LINGER;
}

// secs left before it hands back to the idle face (huge while talking)
static float remaining(const Mascot *m)
{
    return m->mood == MOOD_TALKING ? 1000.0f : LINGER - m->emo.quiet;
}

// 1 while acting, drops to 0 over the last EASE_OUT secs
static float strength(const Mascot *m)
{
    return clampf(remaining(m) / EASE_OUT, 0.0f, 1.0f);
}

// mouth phase while talking, 0 small, 1 open, 2 in between (-1 when not talking)
static int talk_phase(const Mascot *m)
{
    if (m->mood != MOOD_TALKING) return -1;
    return (int)(fmodf(m->time, TALK_PERIOD) / (TALK_PERIOD / 3.0f)) % 3;
}

// emotion_age where the face hands back to idle (way off while talking). effects that
// couldnt finish by then never spawn so they wind down on their own
static float end_age(const Mascot *m)
{
    if (m->mood == MOOD_TALKING) return 1e9f;
    return m->emotion_age + remaining(m);
}

static const int SPAN[9] = { 3, 5, 7, 7, 7, 7, 7, 5, 3 };

// lid depth at column i (0..6) for a lid sloping from l0 (left) to l1 (right)
static int lid_at(int i, int l0, int l1)
{
    return (l0 * (6 - i) + l1 * i + 3) / 6;
}

static bool emask(int i, int j, int l0, int l1, int low)
{
    if (i < 0 || i >= 7 || j < 0 || j >= 9 - low) return false;
    if (j < lid_at(i, l0, l1)) return false;
    int o = (7 - SPAN[j]) / 2;
    return i >= o && i < o + SPAN[j];
}

// mk_eye look but with a sloped lid (l0 left, l1 right) and a pw x ph pupil
static void eye(int x, int y, int l0, int l1, int low, int pdx, int pdy, int pw, int ph)
{
    for (int j = 0; j < 9; j++)
        for (int i = 0; i < 7; i++) {
            if (!emask(i, j, l0, l1, low)) continue;
            bool rim = !emask(i - 1, j, l0, l1, low) || !emask(i + 1, j, l0, l1, low) ||
                       !emask(i, j - 1, l0, l1, low) || !emask(i, j + 1, l0, l1, low);
            mk_put(x + i, y + j, rim ? 'O' : 'Y');
        }
    int px0 = (7 - pw) / 2 + pdx, py0 = 3 + (4 - ph) / 2 + pdy;
    for (int j = 0; j < ph; j++)
        for (int i = 0; i < pw; i++)
            if (mk_get(x + px0 + i, y + py0 + j) == 'Y') mk_put(x + px0 + i, y + py0 + j, 'p');
    if (pw * ph >= 4 && mk_get(x + px0, y + py0) == 'p') mk_put(x + px0, y + py0, 'H');
    if (pw >= 3 && ph >= 3 && mk_get(x + px0 + 2, y + py0 + 2) == 'p')
        mk_put(x + px0 + 2, y + py0 + 2, 'U');
}

// brow that follows a sloped lid, gap rows above it
static void brow(int x, int y, int l0, int l1, int gap)
{
    for (int i = 0; i < 7; i++) mk_put(x + i, y + lid_at(i, l0, l1) - gap, 'O');
}

static void stamp(int x, int y, const char *rows[], int h) { mk_stamp(x, y, rows, h, false); }

// mouths are 8x4 stamps at (16, my-1), rows my-1 .. my+2
#define MOUTH(name, ...) static const char *name[4] = { __VA_ARGS__ }
static void mouth(const Face *f, const char *rows[4]) { stamp(16, f->my - 1, rows, 4); }

MOUTH(M_SMILE,      "........", "O......O", ".OYYYYO.", "........");
MOUTH(M_SMILE_OPEN, "........", "OOOOOOOO", ".OppppO.", "..OOOO..");
MOUTH(M_SMILE_MID,  "........", "O......O", ".OOppOO.", "...OO...");
MOUTH(M_CAT,        "........", ".O.YY.O.", "..O..O..", "........");
MOUTH(M_CAT_OPEN,   "........", "..OOOO..", ".OppppO.", "..OOOO..");
MOUTH(M_CAT_MID,    "........", "...OO...", "..OppO..", "...OO...");
MOUTH(M_OH,         "...OO...", "..OppO..", "..OppO..", "...OO...");
MOUTH(M_OH_SMALL,   "........", "...OO...", "...OO...", "........");
MOUTH(M_OH_MID,     "........", "...OO...", "..OppO..", "...OO...");
MOUTH(M_SQUIGGLE,   "........", ".OO..OO.", "O..OO..O", "........");
MOUTH(M_SQUIGGLE2,  "........", "O..OO..O", ".OO..OO.", "........");
MOUTH(M_HUH,        "........", "....OOO.", "...OpppO", "....OOO.");
MOUTH(M_SMIRK,      ".......O", "......O.", ".OYYOO..", "........");
MOUTH(M_SMIRK_OPEN, "........", ".....OOO", ".OOOOppO", ".....OO.");
MOUTH(M_FROWN,      "........", "..OYYO..", ".O....O.", "........");
MOUTH(M_FROWN_OPEN, "........", "..OOOO..", ".OppppO.", ".O....O.");
MOUTH(M_GRUMPY,     "........", "........", ".OOOOOO.", "O......O");
MOUTH(M_SNARL,      ".OOOOOO.", "OHHHHHHO", "OppppppO", ".OOOOOO.");
MOUTH(M_SNARL_MID,  "........", ".OOOOOO.", "OHHHHHHO", ".OOOOOO.");
MOUTH(M_IDLE,       "........", ".O....O.", "..OYYO..", "........");

// big mouths are 10x5 stamps at (15, my-1), rows my-1 .. my+3 (the last glass row)
#define BIG_MOUTH(name, ...) static const char *name[5] = { __VA_ARGS__ }
static void big_mouth(const Face *f, const char *rows[5]) { stamp(15, f->my - 1, rows, 5); }

BIG_MOUTH(M_GRIN,      "OOOOOOOOOO", "OHHHHHHHHO", "OppppppppO", ".OppkkppO.", "..OOOOOO..");
BIG_MOUTH(M_GRIN_MID,  "..........", "OOOOOOOOOO", "OHHHHHHHHO", ".OppkkppO.", "..OOOOOO..");
BIG_MOUTH(M_GRIN_FLAT, "..........", "O........O", ".OHHHHHHO.", "..OOOOOO..", "..........");
BIG_MOUTH(M_LAUGH,     ".OOOOOOOO.", "OppppppppO", "OppppppppO", ".OpkkkkpO.", "..OOOOOO..");
BIG_MOUTH(M_LAUGH_MID, "..........", ".OOOOOOOO.", "OppppppppO", ".OpkkkkpO.", "..OOOOOO..");

// worried mouth, a clenched grimace with the corners twitching up and down
static void grimace(const Face *f, int wobble, bool open)
{
    const int my = f->my;
    mk_hline(17, my, 6, 'O');
    mk_hline(17, my + 1, 6, 'H');
    if (open) {
        mk_hline(17, my + 2, 6, 'p');
        mk_hline(17, my + 3, 6, 'O');
    } else {
        mk_hline(17, my + 2, 6, 'O');
    }
    int c = wobble ? 1 : -1;
    mk_put(16, my + 1 + c, 'O');
    mk_put(23, my + 1 - c, 'O');
}

static const char *HEART5[] = { ".h.h.", "hkhkh", "hkkkh", ".hkh.", "..h.." };
static const char *HEART7[] = {
    ".hh.hh.", "hkkhkkh", "hwkkkkh", "hkkkkkh", ".hkkkh.", "..hkh..", "...h...",
};
static const char *HEART9[] = {
    ".hhh.hhh.", "hkkkhkkkh", "hwwkkkkkh", "hwkkkkkkh", "hkkkkkkkh",
    ".hkkkkkh.", "..hkkkh..", "...hkh...", "....h....",
};
static const char *STAR[] = {
    "...O...", "..OYO..", "OOOYOOO", ".OYYYO.", "..OYO..", ".OYOYO.", "OO...OO",
};

// squeezed shut laughing, '>' for the left eye and a mirrored '<' for the right
static void squeeze(int x, int y, bool right, int dy)
{
    static const char *SQ[] = {
        "OO.....", "..OO...", "....OO.", ".....YO", "....OO.", "..OO...", "OO.....",
    };
    mk_stamp(x, y + 1 + dy, SQ, 7, right);
}

static const char *HEART_S[] = { "hh.hh", "hwhhh", ".hhh.", "..h.." };
static const char *HEART_T[] = { "h.h", "hhh", ".h." };
static const char *SPARK_B[] = { ".w.", "www", ".w." };
static const char *SPARK_L[] = { "..w..", "..w..", "wwHww", "..w..", "..w.." };
static const char *QMARK[] = { ".YYY.", "YO.OY", "...OY", "..YO.", "..Y..", ".....", "..Y.." };
static const char *BANG[] = { "YY", "YY", "YY", "OO", "..", "OO" };
static const char *ANGER_A[] = {
    "..h.h..", ".hh.hh.", "hh...hh", ".......", "hh...hh", ".hh.hh.", "..h.h..",
};
static const char *ANGER_B[] = { ".h.h.", "hh.hh", ".....", "hh.hh", ".h.h." };
static const char *DROP[] = { ".q.", "qwq", "qqq", ".q." };
static const char *PUFF[] = { "zz", "zz" };

// keep effects inside the canvas with a 1 cell margin
static void fx_stamp(int x, int y, const char *rows[], int w, int h)
{
    x = clampi(x, 1, MASCOT_W - 1 - w);
    y = clampi(y, 1, MASCOT_H - 1 - h);
    stamp(x, y, rows, h);
}

static void fx_put(int x, int y, char c)
{
    if (x >= 1 && x < MASCOT_W - 1 && y >= 1 && y < MASCOT_H - 1) mk_put(x, y, c);
}

// calls fn for every particle still alive. one spawns every `every` secs from start and
// lives for life secs, u goes 0..1 over its life and n is its index
typedef void (*ParticleFn)(const Mascot *m, const Face *f, int n, float u);
static void particles(const Mascot *m, const Face *f, float start, float every, float life,
                      ParticleFn fn)
{
    float a = m->emotion_age, cut = end_age(m) - FX_MARGIN - life;
    if (a < start) return;
    int last = (int)((a - start) / every);
    int first = (int)((a - start - life) / every);
    if (first < 0) first = 0;
    for (int n = first; n <= last; n++) {
        float born = start + n * every;
        float u = (a - born) / life;
        if (u < 0.0f || u >= 1.0f || born > cut) continue;
        fn(m, f, n, u);
    }
}

static void blush(const Face *f)
{
    mk_hline(f->lx, f->my - 1, 3, 'k');
    mk_hline(f->rx + 4, f->my - 1, 3, 'k');
}

// beats (sounds). the visible moments of each entrance, emotion_age crossing the same
// times emotion_pose and emotion_overlay use. loops only make noise the first pass or so
// so a reply that lingers doesnt keep making noise

static bool crossed(float prev, float now, float at) { return prev < at && now >= at; }

// a beat at start + phase + k*period for k = first..last (inclusive) in (prev, now]?
static bool crossed_every(float prev, float now, float start, float period, float phase, int first, int last)
{
    int k = (int)floorf((now - start - phase) / period);
    return k >= first && k <= last && crossed(prev, now, start + phase + (float)k * period);
}

// the more noticeable of two beats landing in the same update
static int louder(int a, int b)
{
    static const int RANK[] = {
        [BEAT_NONE] = 0, [BEAT_LAUGH] = 1, [BEAT_QUIVER] = 1, [BEAT_SIGH] = 1, [BEAT_HMPH] = 1,
        [BEAT_SPARKLE] = 2, [BEAT_TING] = 2, [BEAT_HUH] = 2, [BEAT_HEART] = 2,
        [BEAT_HOP] = 3, [BEAT_STOMP] = 3, [BEAT_BANG] = 3,
    };
    return RANK[b] > RANK[a] ? b : a;
}

static int beat_between(const Mascot *m, float p, float a)
{
    const bool talking = m->mood == MOOD_TALKING;
    int b = BEAT_NONE;
    switch (m->emotion) {
    case EMOTION_HAPPY:
        if (crossed(p, a, 0.40f)) b = BEAT_HOP;                               // lands the hop
        break;
    case EMOTION_EXCITED: {
        float per = talking ? 0.5f : 0.7f;                                    // same as emotion_pose
        if (crossed(p, a, 0.12f)) b = louder(b, BEAT_SPARKLE);                // star eyes, the jump
        if (crossed(p, a, 0.44f)) b = louder(b, BEAT_HOP);
        if (a < 1.5f && crossed_every(p, a, 0.62f, per, 0.6f * per, 0, 2))   // bounces landing, first 1.5 s only
            b = louder(b, BEAT_HOP);
        break;
    }
    case EMOTION_LAUGHING:
        if ((talking || m->emo.quiet < 1.5f) &&                              // ha-ha-ha, the first 5
            crossed_every(p, a, 0.0f, talking ? 0.2f : 0.3f, 0.0f, 1, 5))
            b = BEAT_LAUGH;
        break;
    case EMOTION_LOVE:
        if (crossed_every(p, a, 0.25f, 0.6f, 0.0f, 0, 1) && a <= end_age(m) - FX_MARGIN - 1.7f)
            b = BEAT_HEART;                                                   // same timing particles() spawns them at
        break;
    case EMOTION_SURPRISED:
        if (crossed(p, a, 0.5f)) b = BEAT_BANG;
        break;
    case EMOTION_CONFUSED:
        if (crossed(p, a, 0.25f)) b = BEAT_HUH;
        break;
    case EMOTION_SMUG:
        if (crossed(p, a, 0.55f)) b = BEAT_TING;                             // the glint at its brightest
        break;
    case EMOTION_SAD:
        if (crossed(p, a, 0.55f)) b = BEAT_SIGH;                             // the slump
        break;
    case EMOTION_WORRIED:
        if (crossed(p, a, 0.28f)) b = BEAT_QUIVER;                           // the shiver starts
        break;
    case EMOTION_ANNOYED:
        if (crossed(p, a, 0.16f)) b = BEAT_STOMP;
        if (crossed(p, a, 0.42f)) b = louder(b, BEAT_HMPH);                  // first huff, puffs up
        break;
    default:
        break;
    }
    return b;
}

void emotion_update(Mascot *m, float dt)
{
    EmotionState *s = &m->emo;
    const bool fresh = (int)m->emotion != s->last_emotion || m->emotion_age < s->last_age;
    const float prev = fresh ? -1.0f : s->last_age;
    if (fresh) s->quiet = 0.0f;
    s->last_emotion = (int)m->emotion;
    s->last_age = m->emotion_age;
    if (m->mood == MOOD_TALKING) s->quiet = 0.0f;
    else if (m->mood == MOOD_IDLE) s->quiet += dt;
    else s->quiet = LINGER;   // thinking, asking etc, this replys emotion is over

    // only while the face is acting it out and not once its easing out
    if (active(m) && remaining(m) >= EASE_OUT) {
        int b = beat_between(m, prev, m->emotion_age);
        if (b != BEAT_NONE) {
            m->ev.bits |= MEV_BEAT;
            m->ev.beat = b;
        }
    }
}

void emotion_pose(const Mascot *m, Pose *p)
{
    if (!active(m)) return;
    const float a = m->emotion_age;
    const bool talking = m->mood == MOOD_TALKING;
    const float s = strength(m);
    if (s < 0.5f) return;              // easing out, back to the idle bob
    const int breathe = sinf(a * 3.0f) > 0.0f ? 1 : 0;

    switch (m->emotion) {
    case EMOTION_HAPPY:
        if (a < 0.10f) p->bob = 2;                                   // squash
        else if (a < 0.40f) { p->bob = 0; p->shift_y = hop(a, 0.10f, 0.30f, 2.0f); }
        else if (a < 0.50f) p->bob = 2;                              // land
        else p->bob = talking ? (pulse(a, 0.5f, 0.5f) ? 1 : 0) : breathe;
        break;
    case EMOTION_EXCITED:
        if (a < 0.12f) p->bob = 3;                                   // deep crouch
        else if (a < 0.44f) { p->bob = -1; p->shift_y = hop(a, 0.12f, 0.32f, 4.0f); }
        else if (a < 0.54f) p->bob = 3;                              // land
        else if (a < 0.62f) p->bob = -1;                             // rebound
        else {                                                       // keeps bouncing
            float per = talking ? 0.5f : 0.7f, u = fmodf(a - 0.62f, per) / per;
            if (u < 0.15f) p->bob = 2;
            else if (u < 0.6f) { p->bob = 0; p->shift_y = hop(u, 0.15f, 0.45f, talking ? 2.0f : 1.0f); }
            else p->bob = 0;
        }
        break;
    case EMOTION_LAUGHING:
        if (a < 0.10f) p->bob = -1;                                  // big inhale
        else if (talking || m->emo.quiet < 1.5f)                     // ha-ha-ha
            p->bob = pulse(a, talking ? 0.2f : 0.3f, 0.5f) ? 2 : 0;
        else p->bob = breathe;
        break;
    case EMOTION_LOVE: {
        if (a < 0.10f) p->bob = 1;
        else if (a < 0.38f) p->shift_y = hop(a, 0.10f, 0.28f, 2.0f);
        else {
            float sw = sinf((a - 0.38f) * 2.4f);                     // dreamy sway
            p->shift_x = sw > 0.55f ? 1 : (sw < -0.55f ? -1 : 0);
            p->bob = sinf(a * 2.0f) > 0.0f ? 1 : 0;
        }
        break;
    }
    case EMOTION_SURPRISED:
        if (a < 0.08f) p->bob = 2;                                   // flinch
        else if (a < 0.36f) { p->bob = -1; p->shift_y = hop(a, 0.08f, 0.28f, 3.0f); }
        else if (a < 0.46f) p->bob = 3;                              // land hard
        else if (a < 0.56f) p->bob = 1;
        else p->bob = talking ? breathe : 0;                         // frozen stiff
        break;
    case EMOTION_CONFUSED:
        p->bob = sinf(a * 2.0f) > 0.0f ? 1 : 0;
        if (a > 0.2f) p->shift_x = pulse(a - 0.2f, 2.4f, 0.5f) ? -1 : 0;   // lean toward the '?'
        break;
    case EMOTION_SMUG:
        if (a < 0.25f) p->bob = -1;                                  // chin up
        else p->bob = sinf(a * 1.6f) > 0.0f ? 1 : 0;
        break;
    case EMOTION_SAD:
        if (a < 0.25f) p->bob = 0;
        else if (a < 0.55f) p->bob = 1;
        else if (a < 0.75f) p->bob = 3;                              // slump
        else {
            float u = fmodf(a - 0.75f, 3.0f);                        // a sigh every 3 s
            p->bob = u < 0.4f ? 1 : (u < 0.7f ? 3 : 2);
        }
        break;
    case EMOTION_WORRIED:
        if (a < 0.10f) p->bob = 0;
        else if (a < 0.28f) p->bob = 2;                              // gulp
        else p->bob = 1;
        {
            float u = fmodf(a, 2.5f);                                 // shiver bursts
            if ((a > 0.28f && a < 0.9f) || (a > 2.0f && u < 0.35f))
                p->shift_x = pulse(a, 0.12f, 0.5f) ? 1 : 0;
        }
        break;
    case EMOTION_ANNOYED:
        if (a < 0.16f) p->bob = -1;                                  // inhale, puff up
        else if (a < 0.30f) p->bob = 3;                              // stomp
        else if (a < 0.42f) p->bob = 2;
        else {
            float u = fmodf(a - 0.42f, 2.4f);                        // a huff now and then
            p->bob = u < 0.15f ? -1 : (u < 0.3f ? 2 : 1);
        }
        break;
    default:
        break;
    }
}

bool emotion_face(const Mascot *m, Face *f)
{
    if (!active(m)) return false;
    const float a = m->emotion_age;
    const float t = f->t;
    const bool talking = m->mood == MOOD_TALKING;
    const int ph = talk_phase(m);
    const bool shut = m->blink_left > 0.0f && a > NO_BLINK;
    const int lx = f->lx, rx = f->rx, ly = f->ly;
    const float s = strength(m);

    // last moment, eyes shut and the idle mouth so the handover looks like a blink
    if (remaining(m) < BLINK_SWAP) {
        mk_eye(lx, ly, EYE_SHUT, 0, 0, 0, 0);
        mk_eye(rx, ly, EYE_SHUT, 0, 0, 0, 0);
        mouth(f, M_IDLE);
        return true;
    }

    int lh = f->lh, rh = f->rh;   // talking gestures from the built in arms by default
    const int hy = f->hy;
    if (!talking) lh = rh = 30;

    switch (m->emotion) {
    case EMOTION_HAPPY: {
        mk_eye(lx, ly, EYE_HAPPY, 0, 0, 0, 0);
        mk_eye(rx, ly, EYE_HAPPY, 0, 0, 0, 0);
        mouth(f, ph == 1 ? M_SMILE_OPEN : (ph == 2 ? M_SMILE_MID : M_SMILE));
        if (a < 0.4f) lh = rh = hy + 1;                              // little cheer on the hop
        else if (!talking && m->emo.quiet < 1.8f)                    // a friendly wave
            rh = hy + (pulse(a, 0.36f, 0.5f) ? 0 : 2);
        break;
    }
    case EMOTION_EXCITED: {
        if (a > 0.12f && a < 0.62f) {                                // star eyes at the jump
            stamp(lx, ly + 1, STAR, 7);
            stamp(rx, ly + 1, STAR, 7);
        } else if (shut) {
            mk_eye(lx, ly, EYE_SHUT, 0, 0, 0, 0);
            mk_eye(rx, ly, EYE_SHUT, 0, 0, 0, 0);
        } else {
            eye(lx, ly, 0, 0, 0, 0, -1, 3, 4);
            eye(rx, ly, 0, 0, 0, 0, -1, 3, 4);
            bool tw = pulse(t, 0.5f, 0.5f);                          // twinkling shine
            mk_put(lx + 4, ly + 3, tw ? 'w' : 'H');
            mk_put(rx + 4, ly + 3, tw ? 'w' : 'H');
            if (tw) { mk_put(lx + 2, ly + 2, 'w'); mk_put(rx + 2, ly + 2, 'w'); }
        }
        if (a < 0.12f) big_mouth(f, M_GRIN_FLAT);
        else big_mouth(f, ph == 0 ? M_GRIN_MID : (ph == 2 ? M_GRIN_FLAT : M_GRIN));
        if (a < 0.12f) lh = rh = 30;                                 // arms down in the crouch
        else {
            bool up = pulse(a, 0.3f, 0.5f);                          // pumping fists
            lh = hy - 1 + (up ? 0 : 2);
            rh = hy - 1 + (up ? 2 : 0);
        }
        break;
    }
    case EMOTION_LAUGHING: {
        bool ha = talking || m->emo.quiet < 1.5f
                      ? pulse(a, talking ? 0.2f : 0.3f, 0.5f) : false;
        squeeze(lx, ly, false, ha ? 1 : 0);
        squeeze(rx, ly, true, ha ? 1 : 0);
        if (talking) big_mouth(f, ph == 1 ? M_LAUGH : M_LAUGH_MID);
        else big_mouth(f, ha ? M_LAUGH_MID : M_LAUGH);
        lh = hy + 15 + (ha ? 1 : 0);                                  // holding his belly
        rh = ha ? 30 : 27;                                           // slapping the keyboard
        break;
    }
    case EMOTION_LOVE: {
        if (a < 0.08f) {
            mk_eye(lx, ly, EYE_SHUT, 0, 0, 0, 0);
            mk_eye(rx, ly, EYE_SHUT, 0, 0, 0, 0);
        } else if (a < 0.30f) {                                      // pop, big hearts
            stamp(lx - 1, ly, HEART9, 9);
            stamp(rx - 1, ly, HEART9, 9);
        } else {
            float b = fmodf(a - 0.30f, 1.0f);                         // lub-dub heartbeat
            bool small = in(b, 0.0f, 0.12f) || in(b, 0.24f, 0.36f);
            if (small) {
                stamp(lx + 1, ly + 2, HEART5, 5);
                stamp(rx + 1, ly + 2, HEART5, 5);
            } else {
                stamp(lx, ly + 1, HEART7, 7);
                stamp(rx, ly + 1, HEART7, 7);
            }
        }
        mouth(f, ph == 1 ? M_CAT_OPEN : (ph == 2 ? M_CAT_MID : M_CAT));
        int sway = sinf(a * 2.4f) > 0.0f ? 1 : 0;                    // hands clasped, swaying
        lh = hy + 9 + sway;
        rh = hy + 10 - sway;
        break;
    }
    case EMOTION_SURPRISED: {
        bool pop = a > 0.08f && a < 0.46f;
        if (a < 0.08f) {
            mk_eye(lx, ly, EYE_SHUT, 0, 0, 0, 0);
            mk_eye(rx, ly, EYE_SHUT, 0, 0, 0, 0);
        } else if (shut) {
            mk_eye(lx, ly, EYE_SHUT, 0, 0, 0, 0);
            mk_eye(rx, ly, EYE_SHUT, 0, 0, 0, 0);
        } else {
            eye(lx, ly, 0, 0, 0, 1, 0, 2, 2);                        // wide, tiny pupils
            eye(rx, ly, 0, 0, 0, 0, 0, 2, 2);
            int by = ly - 2;
            mk_hline(lx + 2, by, 3, 'O');                            // brows way up
            mk_put(lx + 1, by + 1, 'O');
            mk_hline(rx + 2, by, 3, 'O');
            mk_put(rx + 5, by + 1, 'O');
        }
        if (pop) {
            static const char *BIG_OH[] = {
                "..OOOO..", ".OppppO.", ".OppppO.", ".OppppO.", "..OOOO..",
            };
            stamp(16, f->my - 1, BIG_OH, 5);
        } else if (a < 0.08f) {
            mouth(f, M_OH_SMALL);
        } else {
            mouth(f, ph == 0 ? M_OH_SMALL : (ph == 2 ? M_OH_MID : M_OH));
        }
        if (a < 0.08f) lh = rh = 30;
        else if (a < 0.5f) lh = rh = hy - 1;                         // arms fly up
        else if (!talking) lh = rh = hy + 8;                         // hands out, "whoa"
        break;
    }
    case EMOTION_CONFUSED: {
        if (shut) {
            mk_eye(lx, ly, EYE_SHUT, 0, 0, 0, 0);
            mk_eye(rx, ly, EYE_SHUT, 0, 0, 0, 0);
        } else {
            int look = pulse(a, 2.4f, 0.5f) ? 0 : 1;                 // eyes wander up
            eye(lx, ly, 0, 0, 0, -1 + look, -2, 3, 4);               // big, brow way up
            eye(rx, ly, 3, 3, 1, -1 + look, 0, 3, 4);                // squinting
            mk_hline(lx + 2, ly - 2, 3, 'O');
            mk_put(lx + 1, ly - 1, 'O');
            mk_put(lx + 5, ly - 1, 'O');
            mk_line(rx, ly + 1, rx + 6, ly + 1, 'O');                // flat brow on the squint
        }
        if (talking) mouth(f, ph == 1 ? M_HUH : (ph == 2 ? M_SQUIGGLE2 : M_SQUIGGLE));
        else mouth(f, pulse(a, 1.2f, 0.5f) ? M_SQUIGGLE : M_SQUIGGLE2);
        if (!talking || ph != 1) rh = hy + 12 + (pulse(a, 0.8f, 0.5f) ? 0 : 1);  // hand on chin
        if (!talking) lh = 30;
        break;
    }
    case EMOTION_SMUG: {
        // lids slide down into a half lidded look at the start
        int lid = a < 0.10f ? 0 : (a < 0.2f ? 2 : 4);
        bool wiggle = in(a, 0.35f, 0.5f) || in(a, 0.65f, 0.8f) ||
                      in(fmodf(a, 4.0f), 2.6f, 2.75f);
        if (shut) {
            mk_eye(lx, ly, EYE_SHUT, 0, 0, 0, 0);
            mk_eye(rx, ly, EYE_SHUT, 0, 0, 0, 0);
        } else {
            eye(lx, ly, lid, lid, 0, 1, 1, 3, 4);
            eye(rx, ly, lid, lid, 0, 1, 1, 3, 4);
        }
        // eyebrow waggle on the right, the left brow sits on the lid
        mk_hline(lx + 1, ly + lid - 1, 5, 'O');
        mk_hline(rx + 1, wiggle ? ly - 2 : ly + lid - 2, 5, 'O');
        mouth(f, ph == 1 ? M_SMIRK_OPEN : M_SMIRK);
        if (!talking) {
            lh = 30;
            rh = in(a, 0.2f, 1.0f) ? hy + 4 : 30;                    // "ta-da"
        }
        break;
    }
    case EMOTION_SAD: {
        if (shut) {
            mk_eye(lx, ly, EYE_SHUT, 0, 1, 0, 0);
            mk_eye(rx, ly, EYE_SHUT, 0, 1, 0, 0);
        } else {
            int droop = a < 0.3f ? 1 : 3;
            eye(lx, ly, droop, 0, 0, 0, 1, 3, 4);                    // outer corners droop
            eye(rx, ly, 0, droop, 0, 0, 1, 3, 4);
            brow(lx, ly, droop, 0, 2);
            brow(rx, ly, 0, droop, 2);
        }
        mouth(f, ph == 1 ? M_FROWN_OPEN : M_FROWN);
        lh = rh = 30;                                                // arms hang
        break;
    }
    case EMOTION_WORRIED: {
        if (shut) {
            mk_eye(lx, ly, EYE_SHUT, 0, 0, 0, 0);
            mk_eye(rx, ly, EYE_SHUT, 0, 0, 0, 0);
        } else {
            float g = fmodf(a, 1.2f);                                // darting glances
            int dx = g < 0.4f ? -1 : (g < 0.6f ? 0 : (g < 1.0f ? 1 : 0));
            eye(lx, ly, 1, 0, 0, dx + 1, 0, 2, 2);
            eye(rx, ly, 0, 1, 0, dx, 0, 2, 2);
            brow(lx, ly, 2, 0, 2);                                   // inner ends up
            brow(rx, ly, 0, 2, 2);
        }
        grimace(f, (int)(a / 0.15f) & 1, ph == 1);
        bool w = pulse(a, 0.5f, 0.5f);                               // wringing hands
        lh = hy + 7 + (w ? 0 : 1);
        rh = hy + 7 + (w ? 1 : 0);
        break;
    }
    case EMOTION_ANNOYED: {
        if (shut) {
            mk_eye(lx, ly, EYE_SHUT, 0, 0, 0, 0);
            mk_eye(rx, ly, EYE_SHUT, 0, 0, 0, 0);
        } else {
            int glare = a < 0.16f ? 1 : 3;
            eye(lx, ly, 1, glare + 1, 0, 0, 0, 3, 4);                 // inner ends down
            eye(rx, ly, glare + 1, 1, 0, 0, 0, 3, 4);
            brow(lx, ly, 1, glare + 1, 2);
            brow(rx, ly, glare + 1, 1, 2);
        }
        if (talking) mouth(f, ph == 1 ? M_SNARL : (ph == 2 ? M_SNARL_MID : M_GRUMPY));
        else mouth(f, M_GRUMPY);
        if (a > 0.16f && a < 0.9f) {                                 // shakes a fist
            lh = 30;
            rh = hy + 2 + (pulse(a, 0.12f, 0.5f) ? 0 : 1);
        } else if (!talking) {
            lh = 30;
            rh = pulse(a, 0.4f, 0.5f) ? 28 : 30;                      // impatient tapping
        }
        break;
    }
    default:
        return false;
    }

    // ease out, hands settle back onto the keyboard
    f->lh = 30 + roundi((lh - 30) * s);
    f->rh = 30 + roundi((rh - 30) * s);
    return true;
}

static void fx_heart(const Mascot *m, const Face *f, int n, float u)
{
    (void)m;
    bool left = (n & 1) == 0;
    int x0 = left ? 2 : 34;
    int y0 = f->hy + 9, y1 = 1;
    int y = y0 + roundi((y1 - y0) * (0.6f * u + 0.4f * u * u));      // drifts up, faster as it goes
    int sway = roundi(sinf(u * 2.0f * PI_F + n) * 1.2f);
    if (u < 0.7f) fx_stamp(x0 + sway, y, HEART_S, 5, 4);
    else fx_stamp(x0 + 1 + sway, y, HEART_T, 3, 3);                   // shrinks as it fades
}

static void fx_joy_tear(const Mascot *m, const Face *f, int n, float u)
{
    (void)m;
    bool left = (n & 1) == 0;
    float dx = u * 6.0f, dy = -2.5f * sinf(u * PI_F) + u * 3.0f;      // arcs outward
    int x = left ? f->lx - 1 - roundi(dx) : f->rx + 7 + roundi(dx);
    int y = f->ly + 4 + roundi(dy);
    fx_put(x, y, 'q');                                                // a fat droplet
    fx_put(x, y + 1, 'q');
    if (u < 0.5f) fx_put(left ? x + 1 : x - 1, y, 'w');
}

static void fx_sad_tear(const Mascot *m, const Face *f, int n, float u)
{
    (void)m;
    bool left = (n & 1) == 0;
    int x = left ? f->lx + 1 : f->rx + 5;
    int y = f->ly + 8 + roundi(u * u * 6.0f);                         // slow start, then runs
    fx_put(x, y, 'q');
    if (u > 0.2f) fx_put(x, y - 1, 'w');
}

static void fx_sparkle(const Mascot *m, const Face *f, int n, float u)
{
    (void)m;
    static const int X[4] = { 10, 37, 19, 2 };
    static const int Y[4] = { 2, 22, 1, 22 };
    int k = n % 4;
    int y = Y[k] > 5 ? f->hy + Y[k] - 5 : Y[k];
    if (u < 0.25f || u > 0.75f) fx_put(X[k], y + 1, 'w');
    else if (u < 0.4f || u > 0.6f) fx_stamp(X[k] - 1, y, SPARK_B, 3, 3);
    else fx_stamp(X[k] - 2, y - 1, SPARK_L, 5, 5);
}

static void fx_steam(const Mascot *m, const Face *f, int n, float u)
{
    (void)m;
    bool left = (n & 1) == 0;
    int x = left ? 6 - roundi(u * 3.0f) : 31 + roundi(u * 3.0f);
    int y = f->hy - roundi(u * 5.0f);
    if (!left) y += 4;                                                // clear of the network card
    if (u < 0.7f) fx_stamp(x, y, PUFF, 2, 2);
    else fx_put(x, y, 'z');
}

static void fx_drop(const Mascot *m, const Face *f, int n, float u)
{
    (void)m;
    (void)n;
    fx_stamp(4, f->hy - 2 + roundi(u * 5.0f), DROP, 3, 4);
}

void emotion_overlay(const Mascot *m, const Face *f)
{
    if (!active(m)) return;
    const float a = m->emotion_age;
    const bool talking = m->mood == MOOD_TALKING;
    const bool calm = remaining(m) < EASE_OUT;   // last moments, only the soft stuff

    switch (m->emotion) {
    case EMOTION_HAPPY:
        if (!calm && a > 0.4f) blush(f);
        particles(m, f, 0.5f, 1.4f, 0.6f, fx_sparkle);
        break;
    case EMOTION_EXCITED:
        particles(m, f, 0.2f, 0.3f, 0.5f, fx_sparkle);
        break;
    case EMOTION_LAUGHING:
        if (talking || m->emo.quiet < 1.5f) particles(m, f, 0.15f, 0.25f, 0.45f, fx_joy_tear);
        break;
    case EMOTION_LOVE:
        if (!calm && a > 0.3f) blush(f);
        particles(m, f, 0.25f, 0.6f, 1.7f, fx_heart);
        break;
    case EMOTION_SURPRISED:
        if (a > 0.5f && a < 2.4f && !calm) {                          // '!' pops up once the arms drop
            int y = clampi(f->hy - 3 + (a < 0.56f ? 2 : (a < 0.64f ? -1 : 0)), 1, 40);
            fx_stamp(3, y, BANG, 2, 6);
        }
        break;
    case EMOTION_CONFUSED:
        if (a > 0.25f && !calm) {
            int rise = a < 0.35f ? 2 : (a < 0.45f ? -1 : 0);          // pops in with overshoot
            int bob = sinf(a * 3.0f) > 0.0f ? 0 : 1;
            fx_stamp(1, f->hy - 4 + rise + bob, QMARK, 5, 7);
        }
        break;
    case EMOTION_SMUG:
        if (in(a, 0.45f, 0.75f)) {                                   // "ting!" on the smirk
            if (a < 0.55f || a > 0.68f) fx_put(25, f->my - 2, 'w');
            else fx_stamp(24, f->my - 3, SPARK_B, 3, 3);
        }
        break;
    case EMOTION_SAD:
        particles(m, f, 0.8f, 1.1f, 1.0f, fx_sad_tear);
        break;
    case EMOTION_WORRIED:
        particles(m, f, 0.3f, 1.6f, 1.2f, fx_drop);
        break;
    case EMOTION_ANNOYED:
        if (a > 0.2f && !calm) {
            if (pulse(a, 0.5f, 0.5f)) fx_stamp(1, f->hy - 4, ANGER_A, 7, 7);   // throbbing mark
            else fx_stamp(2, f->hy - 3, ANGER_B, 5, 5);
        }
        particles(m, f, 0.25f, 0.18f, 0.6f, fx_steam);                // initial huff
        break;
    default:
        break;
    }
}

// preview (make anim emotion)

#define TALK_FOR 1.5f   // linger frames talk this long then go idle

#define DRIVES(NAME, EMO)                                                      \
    static void talk_##NAME(Mascot *m, float t)                                \
    {                                                                          \
        m->emotion = EMO;                                                      \
        m->emotion_age = t;                                                    \
        m->mood = MOOD_TALKING;                                                \
    }                                                                          \
    static void linger_##NAME(Mascot *m, float t)                              \
    {                                                                          \
        m->emotion = EMO;                                                      \
        m->emotion_age = t;                                                    \
        m->mood = t < TALK_FOR ? MOOD_TALKING : MOOD_IDLE;                     \
    }

DRIVES(happy, EMOTION_HAPPY)
DRIVES(excited, EMOTION_EXCITED)
DRIVES(laughing, EMOTION_LAUGHING)
DRIVES(love, EMOTION_LOVE)
DRIVES(surprised, EMOTION_SURPRISED)
DRIVES(confused, EMOTION_CONFUSED)
DRIVES(smug, EMOTION_SMUG)
DRIVES(sad, EMOTION_SAD)
DRIVES(worried, EMOTION_WORRIED)
DRIVES(annoyed, EMOTION_ANNOYED)

typedef struct {
    const char *name;
    void (*talk)(Mascot *m, float t);
    void (*linger)(Mascot *m, float t);
} EmotionDrives;

static const EmotionDrives ALL[] = {
    { "happy", talk_happy, linger_happy },
    { "excited", talk_excited, linger_excited },
    { "laughing", talk_laughing, linger_laughing },
    { "love", talk_love, linger_love },
    { "surprised", talk_surprised, linger_surprised },
    { "confused", talk_confused, linger_confused },
    { "smug", talk_smug, linger_smug },
    { "sad", talk_sad, linger_sad },
    { "worried", talk_worried, linger_worried },
    { "annoyed", talk_annoyed, linger_annoyed },
};

static char labels[64][19];

static int add(PreviewFrame *out, int n, int cap, const char *label, float sec,
               void (*drive)(Mascot *m, float t))
{
    if (n >= cap || n >= 64) return n;
    int i = 0;
    for (; label[i] && i < 18; i++) labels[n][i] = label[i];
    labels[n][i] = 0;
    out[n] = (PreviewFrame){ labels[n], sec, drive };
    return n + 1;
}

static void cat_label(char *dst, const char *a, const char *b)
{
    int i = 0;
    for (; *a && i < 18; a++) dst[i++] = *a;
    for (; *b && i < 18; b++) dst[i++] = *b;
    dst[i] = 0;
}

int emotion_preview(PreviewFrame *out, int cap)
{
    int n = 0;
    char buf[32];
    // every emotion talking with its mouth open, then closed, then lingering after
    for (int e = 0; e < (int)(sizeof ALL / sizeof ALL[0]); e++) {
        cat_label(buf, ALL[e].name, " talk");
        n = add(out, n, cap, buf, 1.26f, ALL[e].talk);   // phase 1, mouth open
        cat_label(buf, ALL[e].name, " talk b");
        n = add(out, n, cap, buf, 1.14f, ALL[e].talk);   // phase 0
        cat_label(buf, ALL[e].name, " linger");
        n = add(out, n, cap, buf, TALK_FOR + 1.5f, ALL[e].linger);
    }
    // entrance beats, 0.08 s apart
    for (int k = 0; k < 12; k++) {
        snprintf(buf, sizeof buf, "surprised %.2f", 0.08f * k);
        n = add(out, n, cap, buf, 0.08f * k, talk_surprised);
    }
    for (int k = 0; k < 10; k++) {
        snprintf(buf, sizeof buf, "excited %.2f", 0.08f * k);
        n = add(out, n, cap, buf, 0.08f * k, talk_excited);
    }
    for (int k = 0; k < 10; k++) {
        snprintf(buf, sizeof buf, "love %.2f", 1.0f + 0.1f * k);
        n = add(out, n, cap, buf, 1.0f + 0.1f * k, talk_love);
    }
    // ease out at the end of the linger (quiet secs after talking)
    static const float QUIET[2] = { 3.1f, 3.42f };
    for (int k = 0; k < 2; k++) {
        snprintf(buf, sizeof buf, "love quiet %.2f", QUIET[k]);
        n = add(out, n, cap, buf, TALK_FOR + QUIET[k], linger_love);
    }
    return n;
}
