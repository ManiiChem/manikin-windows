// dozing off when hes ignored, waking up when poked. it all runs off m->idle_for
//   awake    < DROWSY_AT             nothing, the other idle faces run
//   drowsy   DROWSY_AT .. YAWN_AT    heavy lids, slow lazy blinks, slower bob
//   yawn     YAWN_AT .. YAWN_END     a big yawn and a stretch
//   nodding  YAWN_END .. SLEEP_AT    eyes creep shut then snap open, the last time he loses
//   asleep   SLEEP_AT ..             screen dims, z's, snore bubbles
// when idle_for drops back to 0 he wakes up, a big jolt out of real sleep or just a little
// start if he was only drowsy. hover the pointer on him while he sleeps and he stirs (one
// eye cracks open at it). pose, face and overlay all go through look() so they agree

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mascot_kit.h"

// timing, in seconds unless it says otherwise

#define DROWSY_AT        120.0f   // idle secs till his lids start getting heavy
#define LAZY_BLINK_EVERY   4.2f   // a slow lazy blink this often while drowsy
#define DROWSY_BOB_RATE    1.5f   // rad/s of the drowsy bob (the awake bob is 3.0)

#define YAWN_AT          150.0f   // the big yawn
#define YAWN_LEN           3.2f
#define YAWN_END         (YAWN_AT + YAWN_LEN)

#define NOD_EVERY          5.5f   // eyes creep shut and snap open this often
#define NOD_CREEP          3.4f   //   creeping shut up to here in the cycle
#define NOD_SNAP           4.0f   //   then the snap (after a short doze)
#define NOD_FINAL_MIN      3.0f   // the last losing fight lasts at least this long

#define SLEEP_AT         180.0f   // fast asleep
#define DIM_FADE           2.5f   // screen dims over this long
#define BREATH             3.4f   // one slow breath
#define BUBBLE_EVERY          3   // a snore bubble every Nth breath
#define TWITCH_EVERY      11.0f   // secs between mouth twitches
#define Z_DELAY            1.0f   // first z appears this long after falling asleep
#define Z_EVERY            1.5f   // a new z this often
#define Z_LIFE             3.2f   // each z floats this long
#define Z_X0                 24   // where a z starts (its bottom left, art px), the screens upper right
#define Z_Y0                 13
#define Z_DX              18.0f   // how far it drifts left over its life
#define Z_DY               8.0f   // how far it rises...
#define Z_RISE            0.55f   // ...in this fraction of its life (then it drifts)

#define WAKE_DROP          1.0f   // idle_for falling by more than this = woken up
#define WAKE_DEEP_LEN      2.6f   // jolt + grogginess after real sleep
#define WAKE_LIGHT_LEN     1.1f   // little start after just dozing
#define WAKE_HOP_PX           4   // jolt hop height in art px

#define STIR_AFTER         2.0f   // secs asleep before he can stir
#define STIR_HOVER         0.5f   // pointer has to hover this long
#define STIR_LEN           1.9f
#define STIR_COOLDOWN      5.0f
#define STIR_RX           45.0f   // half size of the hover box around his face, screen px
#define STIR_RY           48.0f

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static float seg(float u, float a, float b) { return clamp01((u - a) / (b - a)); }
static float ease_in(float t) { t = clamp01(t); return t * t; }
static float ease_out(float t) { t = clamp01(t); return 1.0f - (1.0f - t) * (1.0f - t); }
static float ease_io(float t)
{
    t = clamp01(t);
    return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
}
static float ease_out_back(float t)
{
    const float c = 1.9f;
    t = clamp01(t) - 1.0f;
    return 1.0f + (c + 1.0f) * t * t * t + c * t * t;
}
static int ri(float v) { return (int)floorf(v + 0.5f); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// how shut (0 open .. 1 shut) a blink is q secs after it started
static float blink_curve(float q, float close, float hold, float open)
{
    if (q < 0.0f) return 0.0f;
    if (q < close) return ease_in(q / close);
    if (q < close + hold) return 1.0f;
    if (q < close + hold + open) return 1.0f - ease_out((q - close - hold) / open);
    return 0.0f;
}

// how dim the screen is after s secs of sleep
static float dim_at(float s) { return ease_io(s / DIM_FADE); }

// what one frame looks like

typedef enum {
    MOUTH_SMILE, MOUTH_FLAT, MOUTH_SMALL_O, MOUTH_WIDE_O, MOUTH_YAWN,
    MOUTH_SLEEP_IN, MOUTH_SLEEP_OUT, MOUTH_TWITCH, MOUTH_WAVY,
} Mouth;

typedef struct {
    EyeShape shape;
    int lid, pdx, pdy, dy;
    bool squeeze;               // yawn, squeezed shut > <
} EyeLook;

typedef struct {
    bool face, pose;            // we drive the face / the pose this frame
    int bob, sx, sy;
    float lift[2];              // hands, 0 resting on the keyboard .. 1 all the way up
    int tremble[2];             // extra rows on top of lift
    EyeLook eye[2];
    Mouth mouth;
    int mouth_size;             // for MOUTH_YAWN, 1..4
    float dim;                  // 0..1 how dim the screen is
    float zclock;               // secs since the z's started (< 0 = no z's)
    int bubble;                 // snore bubble, 0 none, 1..3 size, 4 pop
    bool tear;
    bool bang;                  // '!' over his head
} Look;

static void set_eyes(Look *L, EyeShape shape, int lid, int pdx, int pdy, int dy)
{
    for (int e = 0; e < 2; e++) L->eye[e] = (EyeLook){ shape, lid, pdx, pdy, dy, false };
}

// eyes from how shut they are, -1 wide open, 0 at the base lid, 1 shut
static void set_closure(Look *L, float c, int base, int pdx, int pdy)
{
    if (c >= 0.9f) { set_eyes(L, EYE_SHUT, 0, 0, 0, 1); return; }
    int lid = c < 0.0f ? ri(base * (1.0f + c)) : base + ri(c * (7 - base));
    set_eyes(L, EYE_OPEN, clampi(lid, 0, 7), pdx, pdy, 0);
}

// drowsy, the lids keep getting heavier, lazy blinks, eyes wander low
static void drowsy_look(const Mascot *m, Look *L)
{
    float d = m->idle_for - DROWSY_AT;
    int base = d < 2.0f ? 0 : (d < 7.0f ? 1 : 2);
    float c = d < 1.5f ? 0.0f : blink_curve(fmodf(d - 1.5f, LAZY_BLINK_EVERY), 0.35f, 0.2f, 0.5f);
    set_closure(L, c, base, ri(sinf(d * 0.35f) * 1.4f), base >= 2 ? 1 : 0);
    L->bob = sinf(m->idle_for * DROWSY_BOB_RATE) > 0.0f ? 1 : 0;
    L->mouth = d < 5.0f ? MOUTH_SMILE : MOUTH_FLAT;
}

// the yawn. dips first, stretches tall with his arms up and trembles, then the arms
// drop, he settles and the lids come back heavy
static void yawn_look(const Mascot *m, Look *L)
{
    float u = m->idle_for - YAWN_AT;

    L->mouth = MOUTH_YAWN;
    if (u < 0.4f) L->mouth_size = 1;
    else if (u < 1.0f) L->mouth_size = 1 + ri(ease_out(seg(u, 0.4f, 1.0f)) * 3.0f);
    else if (u < 2.0f) L->mouth_size = (u > 1.4f && fmodf(u, 0.3f) < 0.1f) ? 3 : 4;   // jaw shiver
    else if (u < 2.45f) L->mouth_size = 4 - ri(ease_in(seg(u, 2.0f, 2.45f)) * 4.0f);
    else L->mouth_size = 0;
    if (L->mouth_size <= 0) L->mouth = (u > 2.75f && u < 2.95f) ? MOUTH_TWITCH : MOUTH_FLAT;

    if (u < 0.4f) set_closure(L, 0.2f, 2, 0, -1);                 // inhale, glance up
    else if (u < 0.55f) set_closure(L, seg(u, 0.4f, 0.55f), 2, 0, 0);
    else if (u < 2.2f) { set_eyes(L, EYE_SHUT, 0, 0, 0, 0); L->eye[0].squeeze = L->eye[1].squeeze = true; }
    else if (u < 2.6f) set_eyes(L, EYE_SHUT, 0, 0, 0, 1);
    else set_closure(L, 1.0f - ease_out(seg(u, 2.6f, YAWN_LEN)), 3, 0, 1);
    L->tear = u > 1.3f && u < 2.6f;

    // body dips, stretches tall, drops with a squash and settles
    if (u < 0.12f) L->bob = 1;
    else if (u < 0.4f) L->bob = 2;
    else if (u < 0.5f) L->bob = 0;
    else if (u < 2.0f) L->bob = -1;
    else if (u < 2.3f) L->bob = 0;
    else if (u < 2.65f) L->bob = 2;
    else L->bob = 1;

    // arms stretch up (overshooting), tremble at the top, fall and bounce once
    float lift;
    if (u < 0.4f) lift = 0.0f;
    else if (u < 1.0f) lift = ease_out_back(seg(u, 0.4f, 1.0f));
    else if (u < 2.0f) lift = 1.0f;
    else if (u < 2.4f) lift = 1.0f - ease_in(seg(u, 2.0f, 2.4f));
    else lift = (u < 2.55f) ? 0.1f : 0.0f;
    L->lift[0] = L->lift[1] = lift;
    if (u > 1.0f && u < 2.0f) {
        int k = (int)(u / 0.1f) & 1;
        L->tremble[0] = k;
        L->tremble[1] = 1 - k;
    }
}

// nodding off, eyes creep shut as his head sinks, short doze, then he snaps awake.
// the last cycle before SLEEP_AT doesnt snap, just a weak flutter
static void nod_look(const Mascot *m, Look *L)
{
    const float idle = m->idle_for;
    const int n = (int)((SLEEP_AT - YAWN_END - NOD_FINAL_MIN) / NOD_EVERY);
    const float last = YAWN_END + n * NOD_EVERY;
    const int base = 3;
    L->mouth = MOUTH_FLAT;
    if (idle >= last) {
        float v = seg(idle, last, SLEEP_AT), c;
        if (v < 0.5f) c = 0.85f * ease_in(v / 0.5f);
        else if (v < 0.6f) c = 0.85f - 0.45f * ease_out(seg(v, 0.5f, 0.6f));   // flutter
        else c = 0.4f + 0.6f * ease_io(seg(v, 0.6f, 0.95f));
        set_closure(L, c, base, 0, 1);
        L->bob = c > 0.6f ? 2 : 1;
        if (v > 0.85f) L->mouth = MOUTH_SLEEP_IN;
        return;
    }
    float q = fmodf(idle - YAWN_END, NOD_EVERY);
    if (q < NOD_CREEP) {
        float c = ease_in(q / NOD_CREEP);
        set_closure(L, c, base, 0, 1);
        L->bob = c > 0.55f ? 2 : 1;
    } else if (q < NOD_SNAP) {
        set_closure(L, 1.0f, base, 0, 1);
        L->bob = q > NOD_SNAP - 0.3f ? 3 : 2;                         // head droops lower
    } else if (q < NOD_SNAP + 0.15f) {
        set_closure(L, -1.0f, base, 0, -2);                          // snap!
        L->eye[0].dy = L->eye[1].dy = -1;
        L->bob = -1;
        L->lift[0] = L->lift[1] = 0.12f;
        L->mouth = MOUTH_SMALL_O;
    } else if (q < NOD_SNAP + 0.5f) {
        set_closure(L, -1.0f, base, 0, -1);
        L->bob = 0;
        L->lift[0] = L->lift[1] = q < NOD_SNAP + 0.25f ? 0.05f : 0.0f;
        L->mouth = MOUTH_SMALL_O;
    } else {
        set_closure(L, -1.0f + ease_io(seg(q, NOD_SNAP + 0.5f, NOD_EVERY - 0.2f)), base, 0,
                    q < NOD_SNAP + 1.0f ? 0 : 1);
        L->bob = 1;
    }
}

// fast asleep (dim screen, breathing, z's, snore bubbles, stirring)
static void asleep_look(const Mascot *m, Look *L)
{
    const SleepState *s = &m->sleep;
    float t = m->idle_for - SLEEP_AT;
    set_eyes(L, EYE_SHUT, 0, 0, 0, 1);
    L->dim = dim_at(t);
    float ph = fmodf(t, BREATH) / BREATH;
    bool exhale = ph < 0.5f;
    L->bob = exhale ? 2 : 1;
    L->mouth = exhale ? MOUTH_SLEEP_OUT : MOUTH_SLEEP_IN;
    if (fmodf(t, TWITCH_EVERY) > TWITCH_EVERY - 0.5f)
        L->mouth = fmodf(t, 0.25f) < 0.125f ? MOUTH_TWITCH : MOUTH_SLEEP_IN;
    L->zclock = t - Z_DELAY;

    int breath = (int)(t / BREATH);
    if (t > DIM_FADE && breath % BUBBLE_EVERY == BUBBLE_EVERY - 1 && exhale) {
        float b = ph / 0.5f;
        L->bubble = b > 0.92f ? 4 : clampi(1 + (int)(b * 3.5f), 1, 3);
        L->mouth = MOUTH_SLEEP_IN;
    }

    if (s->stir_left > 0.0f) {
        float v = STIR_LEN - s->stir_left;
        int e = m->cursor_dx < 0.0f ? 0 : 1;          // the eye nearer the pointer
        if (v < 0.3f) L->mouth = MOUTH_TWITCH;
        else L->mouth = MOUTH_SLEEP_IN;
        int lid = -1;
        if (v > 0.3f && v < 0.45f) lid = 7;
        else if (v >= 0.45f && v < 1.35f) lid = 5;
        else if (v >= 1.35f && v < 1.55f) lid = 7;
        if (lid >= 0) {
            int pdx = clampi(ri(m->cursor_dx / 20.0f), -2, 2);
            L->eye[e] = (EyeLook){ EYE_OPEN, lid, pdx, 2, 0, false };
        }
        L->bubble = 0;
    }
}

// waking up out of real sleep (deep) or a doze (light), w = secs since he woke
static void wake_look(const Mascot *m, Look *L)
{
    const SleepState *s = &m->sleep;
    float w = s->wake_len - s->wake_left;
    L->face = L->pose = true;
    L->bob = sinf(m->time * 3.0f) > 0.0f ? 1 : 0;
    L->mouth = MOUTH_SMILE;

    if (!s->wake_deep) {
        if (w < 0.12f) {
            set_closure(L, -1.0f, 2, 0, -1);
            L->bob = -1;
            L->sy = -1;
            L->lift[0] = L->lift[1] = 0.1f;
            L->mouth = MOUTH_SMALL_O;
        } else if (w < 0.45f) {
            set_closure(L, -1.0f, 2, 0, 0);
            L->bob = 0;
            L->mouth = MOUTH_SMALL_O;
        } else if (w < 0.8f) {
            set_closure(L, blink_curve(w - 0.45f, 0.1f, 0.06f, 0.16f), 0, 0, 0);
            L->mouth = MOUTH_FLAT;
        } else {
            set_eyes(L, EYE_OPEN, 0, 0, 0, 0);
        }
        return;
    }

    if (w < 0.1f) {                                   // squash first, still asleep
        set_eyes(L, EYE_SHUT, 0, 0, 0, 1);
        L->bob = 3;
        L->mouth = MOUTH_SLEEP_IN;
        L->dim = s->wake_dim;
    } else if (w < 0.5f) {                            // the jolt, hops with his eyes popped and arms flung up
        float h = sinf(3.14159265f * seg(w, 0.1f, 0.5f));
        L->sy = -ri(h * WAKE_HOP_PX);
        L->bob = -1;
        set_eyes(L, EYE_OPEN, 0, 0, 0, -1);
        L->mouth = MOUTH_WIDE_O;
        L->lift[0] = L->lift[1] = 0.8f * ease_out(seg(w, 0.1f, 0.22f));
        L->bang = true;
        L->dim = w < 0.16f ? s->wake_dim * 0.5f : 0.0f;
    } else if (w < 0.62f) {                           // lands with a squash, arms come down late
        L->bob = w < 0.56f ? 3 : 2;
        set_eyes(L, EYE_OPEN, 0, 0, 1, 0);
        L->mouth = MOUTH_WIDE_O;
        L->lift[0] = L->lift[1] = 0.8f * (1.0f - ease_in(seg(w, 0.5f, 0.62f)));
        L->bang = true;
    } else if (w < 0.8f) {                            // rebound
        L->bob = 0;
        set_eyes(L, EYE_OPEN, 0, 0, 0, 0);
        L->mouth = MOUTH_SMALL_O;
        L->bang = w < 0.75f;
    } else {                                          // groggy, heavy lids lifting and a couple blinks
        int base = ri(4.0f * (1.0f - ease_io(seg(w, 0.8f, WAKE_DEEP_LEN - 0.1f))));
        float c = blink_curve(w - 1.0f, 0.25f, 0.15f, 0.35f) +
                  blink_curve(w - 1.95f, 0.07f, 0.05f, 0.1f);
        int pdx = (w > 1.3f && w < 1.6f) ? -2 : ((w >= 1.6f && w < 1.85f) ? 2 : 0);
        set_closure(L, c, base, pdx, base >= 2 ? 1 : 0);
        L->bob = w < 1.2f ? 1 : L->bob;
        L->mouth = w < 1.9f ? MOUTH_WAVY : MOUTH_SMILE;
    }
}

static void look(const Mascot *m, Look *L)
{
    memset(L, 0, sizeof *L);
    L->zclock = -1.0f;
    set_eyes(L, EYE_OPEN, 0, 0, 0, 0);
    if (m->sleep.wake_left > 0.0f) { wake_look(m, L); return; }
    if (m->mood != MOOD_IDLE || m->idle_for < DROWSY_AT) return;
    L->face = L->pose = true;
    if (m->idle_for < YAWN_AT) drowsy_look(m, L);
    else if (m->idle_for < YAWN_END) yawn_look(m, L);
    else if (m->idle_for < SLEEP_AT) nod_look(m, L);
    else asleep_look(m, L);
}

// same test look() uses to pick asleep_look() so it matches whats on screen
bool sleep_is_asleep(const Mascot *m)
{
    return m->mood == MOOD_IDLE && m->idle_for >= SLEEP_AT && m->sleep.wake_left <= 0.0f;
}

// sounds. the look only depends on idle_for so each moment is just idle_for crossing a time

static bool crossed(float prev, float now, float at) { return prev < at && now >= at; }

// did a nod off snap (eyes pop open, head jerks up) land in (prev, now]? same schedule as
// nod_look, every NOD_EVERY from YAWN_END except the last losing cycle
static bool nod_snapped(float prev, float now)
{
    const int n = (int)((SLEEP_AT - YAWN_END - NOD_FINAL_MIN) / NOD_EVERY);
    if (now < YAWN_END + NOD_SNAP) return false;
    int k = (int)floorf((now - YAWN_END - NOD_SNAP) / NOD_EVERY);
    return k < n && crossed(prev, now, YAWN_END + k * NOD_EVERY + NOD_SNAP);
}

// did a snore bubble pop in (prev, now]? same schedule as asleep_look, it pops (bubble 4)
// at 0.92 of the exhale on every BUBBLE_EVERY-th breath once the screens dimmed
static bool bubble_popped(float prev, float now)
{
    float t = now - SLEEP_AT;
    if (t <= DIM_FADE) return false;
    int breath = (int)floorf(t / BREATH);
    if (breath % BUBBLE_EVERY != BUBBLE_EVERY - 1) return false;
    return crossed(prev, now, SLEEP_AT + ((float)breath + 0.5f * 0.92f) * BREATH);
}

void sleep_update(Mascot *m, float dt)
{
    SleepState *s = &m->sleep;
    const float idle = m->idle_for, prev = s->prev_idle;

    if (s->wake_left > 0.0f) s->wake_left = fmaxf(0.0f, s->wake_left - dt);
    if (s->stir_left > 0.0f) s->stir_left = fmaxf(0.0f, s->stir_left - dt);
    if (s->stir_cool > 0.0f) s->stir_cool = fmaxf(0.0f, s->stir_cool - dt);

    if (idle + WAKE_DROP < s->prev_idle && s->prev_idle >= DROWSY_AT) {
        s->wake_deep = s->prev_idle >= SLEEP_AT;
        s->wake_len = s->wake_deep ? WAKE_DEEP_LEN : WAKE_LIGHT_LEN;
        s->wake_left = s->wake_len;
        s->wake_dim = s->wake_deep ? dim_at(s->prev_idle - SLEEP_AT) : 0.0f;
        s->stir_left = 0.0f;
        s->hover_t = 0.0f;
        m->ev.bits |= s->wake_deep ? MEV_WAKE_JOLT : MEV_WAKE_SOFT;
    }
    s->prev_idle = idle;

    // dozing off moments, only while look() is actually showing them
    if (m->mood == MOOD_IDLE && s->wake_left <= 0.0f) {
        if (crossed(prev, idle, YAWN_AT)) m->ev.bits |= MEV_YAWN;
        if (nod_snapped(prev, idle)) m->ev.bits |= MEV_NOD;
        if (s->stir_left <= 0.0f && bubble_popped(prev, idle)) m->ev.bits |= MEV_SNORE_POP;
    }
    bool asleep_now = sleep_is_asleep(m);
    if (asleep_now && !s->was_asleep) m->ev.bits |= MEV_ASLEEP;
    s->was_asleep = asleep_now;

    // stir when the pointer hovers over him while hes asleep
    bool asleep = m->mood == MOOD_IDLE && idle >= SLEEP_AT + STIR_AFTER && s->wake_left <= 0.0f;
    bool over = fabsf(m->cursor_dx) < STIR_RX && fabsf(m->cursor_dy) < STIR_RY;
    s->hover_t = asleep && over ? s->hover_t + dt : 0.0f;
    if (s->hover_t >= STIR_HOVER && s->stir_cool <= 0.0f) {
        s->stir_left = STIR_LEN;
        s->stir_cool = STIR_LEN + STIR_COOLDOWN;
    }
}

void sleep_pose(const Mascot *m, Pose *p)
{
    Look L;
    look(m, &L);
    if (!L.pose) return;
    p->bob = L.bob;
    p->shift_x += L.sx;
    p->shift_y += L.sy;
}

// ordered dither so the screen fades out smoothly instead of all at once
static void dim_glass(const Face *f, float dim)
{
    static const int BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    int level = ri(dim * 16.0f);
    if (level <= 0) return;
    for (int y = f->glass_y; y < f->glass_y + f->glass_h; y++)
        for (int x = f->glass_x; x < f->glass_x + f->glass_w; x++) {
            if (BAYER[y & 3][x & 3] >= level) continue;
            char c = mk_get(x, y);
            if (c == 'T' || c == 'U') mk_put(x, y, 'd');
            else if (c == 'd') mk_put(x, y, 'D');
        }
}

static const char *SQUEEZE[] = {   // left eye squeezed shut (>), mirrored for the right
    "OO.....",
    "..OOO..",
    ".....OY",
    "..OOO..",
    "OO.....",
};

static void draw_eye(int x, int y, const EyeLook *e, bool right)
{
    if (e->squeeze) mk_stamp(x, y + e->dy + 2, SQUEEZE, 5, right);
    else mk_eye(x, y + e->dy, e->shape, e->pdx, e->pdy, e->lid, 0);
}

static const char *YAWN1[] = { "OO", "OO" };
static const char *YAWN2[] = { ".OO.", "OppO", ".OO." };
static const char *YAWN3[] = { ".OOOO.", "OppppO", "OppppO", ".OOOO." };
static const char *YAWN4[] = {
    "..OOOO..",
    ".OppppO.",
    "OppppppO",
    "OppkkppO",
    ".OOOOOO.",
};

static void draw_mouth(const Look *L, int my)
{
    switch (L->mouth) {
    case MOUTH_SMILE:
        mk_put(17, my, 'O');
        mk_put(22, my, 'O');
        mk_hline(18, my + 1, 4, 'O');
        mk_put(19, my + 1, 'Y');
        mk_put(20, my + 1, 'Y');
        break;
    case MOUTH_FLAT:
        mk_hline(18, my + 1, 4, 'O');
        break;
    case MOUTH_SMALL_O:
        mk_fill(19, my, 2, 2, 'O');
        break;
    case MOUTH_WIDE_O:
        mk_stamp(18, my - 1, YAWN2, 3, false);
        break;
    case MOUTH_YAWN:
        switch (L->mouth_size) {
        case 1: mk_stamp(19, my, YAWN1, 2, false); break;
        case 2: mk_stamp(18, my, YAWN2, 3, false); break;
        case 3: mk_stamp(17, my - 1, YAWN3, 4, false); break;
        default: mk_stamp(16, my - 1, YAWN4, 5, false); break;
        }
        break;
    case MOUTH_SLEEP_IN:
        mk_hline(19, my + 1, 2, 'O');
        break;
    case MOUTH_SLEEP_OUT:
        mk_fill(19, my, 2, 2, 'O');
        break;
    case MOUTH_TWITCH:
        mk_hline(18, my + 1, 2, 'O');
        mk_hline(20, my, 2, 'O');
        break;
    case MOUTH_WAVY:
        mk_put(17, my + 1, 'O');
        mk_put(18, my, 'O');
        mk_hline(19, my + 1, 2, 'O');
        mk_put(21, my, 'O');
        mk_put(22, my + 1, 'O');
        break;
    }
}

static void draw_bubble(int size, int my)
{
    static const char *B1[] = { "q" };
    static const char *B2[] = { ".q.", "q.q", ".q." };
    static const char *B3[] = { ".qq.", "qw.q", "q..q", ".qq." };
    static const char *POP[] = { "w.w", "...", "w.w" };
    switch (size) {
    case 1: mk_stamp(21, my + 1, B1, 1, false); break;
    case 2: mk_stamp(21, my, B2, 3, false); break;
    case 3: mk_stamp(21, my - 1, B3, 4, false); break;
    case 4: mk_stamp(21, my - 1, POP, 3, false); break;
    default: break;
    }
}

bool sleep_face(const Mascot *m, Face *f)
{
    Look L;
    look(m, &L);
    if (!L.face) return false;

    dim_glass(f, L.dim);
    draw_eye(f->lx, f->ly, &L.eye[0], false);
    draw_eye(f->rx, f->ly, &L.eye[1], true);
    if (L.tear) {
        mk_put(f->lx, f->ly + 6, 'q');
        mk_put(f->rx + 6, f->ly + 6, 'q');
    }
    draw_mouth(&L, f->my);
    draw_bubble(L.bubble, f->my);

    // hands, lift 0 rests on the keyboard (row 30) and 1 is all the way up (hy - 1)
    int *hand[2] = { &f->lh, &f->rh };
    for (int i = 0; i < 2; i++)
        if (L.lift[i] > 0.0f || L.tremble[i])
            *hand[i] = ri(30.0f + L.lift[i] * (float)((f->hy - 1) - 30)) + L.tremble[i];
    return true;
}

static const char *Z4[] = { "zzzz", "..z.", ".z..", "zzzz" };
static const char *Z5[] = { "zzzzz", "...z.", "..z..", ".z...", "zzzzz" };

// one z thats a secs old. floats up and to the left, sways, grows and fades out
static void draw_z(float a, int k)
{
    float p = a / Z_LIFE;
    if (p < 0.0f || p >= 1.0f) return;
    int size = p < 0.35f ? 4 : 5;
    // (x, y) is its bottom left. it rises off the screens upper right over the bezel then
    // drifts left through the empty space above his head while it fades
    int x = Z_X0 - ri(p * Z_DX + sinf(a * 2.2f + (float)k) * 0.8f);
    int y = Z_Y0 - ri(ease_io(p / Z_RISE) * Z_DY);
    if (p >= 0.92f) return;
    char col = p < 0.74f ? 'z' : 'G';                // fades to grey, then gone
    const char **glyph = size == 4 ? Z4 : Z5;
    x = clampi(x, 1, MASCOT_W - 1 - size);
    y = y - size + 1;
    // dark rim wherever it crosses the casing so you can still see it on the light plastic
    for (int j = 0; j < size; j++)
        for (int i = 0; i < size; i++) {
            if (glyph[j][i] == '.') continue;
            static const int OX[4] = { 1, -1, 0, 0 }, OY[4] = { 0, 0, 1, -1 };
            for (int n = 0; n < 4; n++) {
                int ii = i + OX[n], jj = j + OY[n];
                if (ii >= 0 && ii < size && jj >= 0 && jj < size && glyph[jj][ii] != '.') continue;
                char c = mk_get(x + ii, y + jj);
                if (c != '.' && c != 'T' && c != 'd' && c != 'D' && c != 'U' && c != 'g' && c != 'z')
                    mk_put(x + ii, y + jj, 'K');
            }
        }
    for (int j = 0; j < size; j++)
        for (int i = 0; i < size; i++)
            if (glyph[j][i] != '.') mk_put(x + i, y + j, col);
}

static const char *BANG[] = { "YY", "YY", "YY", "OO", "..", "OO" };

void sleep_overlay(const Mascot *m, const Face *f)
{
    Look L;
    look(m, &L);
    if (L.zclock >= 0.0f) {
        int newest = (int)(L.zclock / Z_EVERY);
        for (int k = newest; k >= 0 && k > newest - 4; k--) draw_z(L.zclock - k * Z_EVERY, k);
    }
    if (L.bang) mk_stamp(3, f->hy - 5 > 1 ? f->hy - 5 : 1, BANG, 6, false);
}

// preview (make anim sleep)

static void base_inputs(Mascot *m)
{
    m->mood = MOOD_IDLE;
    m->cursor_dx = 400.0f;
    m->cursor_dy = -300.0f;
}

static void d_awake(Mascot *m, float t) { base_inputs(m); m->idle_for = 10.0f + t; }
static void d_drowsy(Mascot *m, float t) { base_inputs(m); m->idle_for = DROWSY_AT + 10.0f + t; }
static void d_yawn(Mascot *m, float t) { base_inputs(m); m->idle_for = YAWN_AT - 0.2f + t; }
static void d_nod(Mascot *m, float t) { base_inputs(m); m->idle_for = YAWN_END + t; }
static void d_sleep(Mascot *m, float t) { base_inputs(m); m->idle_for = SLEEP_AT + 10.0f + t; }
static void d_snore(Mascot *m, float t) { base_inputs(m); m->idle_for = SLEEP_AT + t; }
static void d_stir(Mascot *m, float t)
{
    base_inputs(m);
    m->idle_for = SLEEP_AT + 10.0f + t;
    if (t > 1.0f) { m->cursor_dx = -20.0f; m->cursor_dy = 10.0f; }
}
static void d_wake(Mascot *m, float t)   // asleep for 3 s, then poked
{
    base_inputs(m);
    m->idle_for = t < 3.0f ? SLEEP_AT + 20.0f + t : t - 3.0f;
}
static void d_wake_light(Mascot *m, float t)
{
    base_inputs(m);
    m->idle_for = t < 3.0f ? DROWSY_AT + 20.0f + t : t - 3.0f;
}

int sleep_preview(PreviewFrame *out, int cap)
{
    static char labels[64][19];
    int n = 0;
#define ADD(lbl, secs, fn) do { if (n < cap) out[n++] = (PreviewFrame){ (lbl), (secs), (fn) }; } while (0)
#define ADDF(fmt, v, secs, fn) do { if (n < cap && n < 64) { \
        snprintf(labels[n], sizeof labels[n], fmt, (double)(v)); \
        out[n] = (PreviewFrame){ labels[n], (secs), (fn) }; n++; } } while (0)

    ADD("asleep", 5.0f, d_sleep);
    ADD("awake", 2.0f, d_awake);
    ADD("drowsy", 4.0f, d_drowsy);
    ADD("drowsy blink", 4.55f, d_drowsy);
    ADD("mid-yawn", 1.7f, d_yawn);
    ADD("nodding off", NOD_CREEP * 0.8f, d_nod);
    ADD("stirring", 2.2f, d_stir);
    ADD("light wake", 3.06f, d_wake_light);
    ADD("snore bubble", 7.7f, d_snore);
    ADD("bubble pops", 8.4f, d_snore);

    for (int k = 0; k < 12; k++) ADDF("yawn %.2f", k * 0.15f, 0.2f + k * 0.15f, d_yawn);
    for (int k = 0; k < 8; k++) ADDF("yawn %.2f", 1.8f + k * 0.2f, 2.0f + k * 0.2f, d_yawn);
    for (int k = 0; k < 6; k++) {
        static const float Q[6] = { 3.6f, 3.9f, 4.05f, 4.2f, 4.4f, 4.9f };
        ADDF("nod %.2f", Q[k], Q[k], d_nod);
    }
    for (int k = 0; k < 12; k++) ADDF("zzz %.1f", 3.0f + k * 0.1f, 3.0f + k * 0.1f, d_sleep);
    for (int k = 0; k < 12; k++) ADDF("wake %.2f", k * 0.1f + 0.02f, 3.02f + k * 0.1f, d_wake);
    for (int k = 0; k < 4; k++) {
        static const float W[4] = { 1.3f, 1.6f, 1.9f, 2.4f };
        ADDF("wake %.2f", W[k], 3.0f + W[k], d_wake);
    }
#undef ADD
#undef ADDF
    return n;
}
