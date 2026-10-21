// getting dragged around and shaken
//   pick up   flinch (crouch), eyes pop wide with pin pupils, '!' and flailing arms
//   held      dangling stretched out, nervous, looking down, wobbly mouth, sweat drop
//   dragging  body sways behind the drag on a spring, pupils lag the other way and the
//             trailing arm lifts. fast drags squint into the wind with speed lines
//   shaking   fast direction flips pump up a "shake energy" that decays. past a point
//             hes dizzy (spiral eyes, stars, drunk wobble) for a while, even after hes
//             let go, then he shakes it off with a quick head shake
//   drop      landing squash that bounces back, dust puffs, relieved ^ ^
// physics_update runs all the springs, the draw hooks only read PhysicsState and m->time

#include <math.h>
#include <stdio.h>

#include "mascot_kit.h"

// tuning (px = screen pixels, art px = his own pixels)

#define VEL_CLAMP     6000.0f   // px/s, raw velocity spikes past this get clipped
#define VEL_TAU       0.06f     // secs, velocity smoothing (bigger = smoother but laggier)
#define STILL_SPEED   90.0f     // px/s, under this counts as held still
#define FAST_ON       1100.0f   // px/s, fast drag look turns on above this...
#define FAST_OFF      800.0f    // ...and off below this

#define LEAN_PX       420.0f    // px/s of drag per art px of body lag (lower = leans more)
#define LEAN_MAX      3.0f      // art px
#define LEAN_W        13.0f     // sway spring in rad/s (about 2 Hz)
#define LEAN_Z        0.28f     // damping, under 1 so it overshoots and swings through
#define DROP_PX       650.0f    // same as LEAN_PX but vertical
#define DROP_MAX      2.0f

#define SQ_W          17.0f     // squash/stretch spring, rad/s
#define SQ_Z          0.30f
#define HELD_STRETCH  (-1.0f)   // bob while dangling (body pulled up off the base)
#define LAND_KICK     62.0f     // art px/s downward kick when let go, makes the landing squash

#define PUPIL_PX      340.0f    // px/s of drag per art px of pupil lag
#define PUPIL_W       24.0f
#define PUPIL_Z       0.55f
#define PUPIL_DOWN    1.7f      // art px he looks down at the ground while held

#define ARM_PX        1500.0f   // px/s of drag for a fully raised trailing arm
#define ARM_HELD      0.12f     // arms dangle just off the keys while held
#define ARM_W         15.0f
#define ARM_Z         0.35f

#define SHAKE_SPEED   700.0f    // px/s, a direction flip only counts between motions this fast
#define SHAKE_QUIET   0.35f     // secs without fast motion before it forgets the last direction
#define SHAKE_KICK    0.34f     // energy per flip
#define SHAKE_DECAY   0.8f      // energy lost per sec
#define SHAKE_MAX     1.8f
#define SHAKE_DIZZY   1.0f      // energy where he gets dizzy (about 3-4 quick flips)
#define DIZZY_HOLD    2.6f      // secs of dizziness after the last hard shaking...
#define DIZZY_AFTER_RELEASE 2.2f  // ...and at least this long after hes let go
#define SHAKEOFF_TIME 0.62f     // secs of shaking it off
#define SHAKEOFF_HEAD 0.36f     // secs of that spent shaking his head

#define PICK_POP      0.40f     // secs of the startled pick up
#define LAND_TIME     1.10f     // secs of the landing squash + relieved look
#define LAND_IMPACT   0.18f     // secs of that with his eyes squeezed shut

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int roundi(float v) { return (int)floorf(v + 0.5f); }
static float maxf(float a, float b) { return a > b ? a : b; }

// one damped spring step (semi implicit euler)
static void spring(float *x, float *v, float target, float w, float z, float dt)
{
    float a = w * w * (target - *x) - 2.0f * z * w * *v;
    *v += a * dt;
    *x += *v * dt;
}

// counts a direction flip on one axis (fast motion changed sign)
static int reversal(int *dir, float *quiet, float v, float dt)
{
    if (fabsf(v) > SHAKE_SPEED) {
        int sg = v > 0 ? 1 : -1;
        int rev = *dir != 0 && sg != *dir;
        *dir = sg;
        *quiet = 0;
        return rev;
    }
    *quiet += dt;
    if (*quiet > SHAKE_QUIET) *dir = 0;
    return 0;
}

static bool active(const Mascot *m)
{
    const PhysicsState *s = &m->phys;
    return m->held || s->dizzy_left > 0 || s->shakeoff_left > 0 || s->land_left > 0 ||
           fabsf(s->ox) >= 0.5f || fabsf(s->oy) >= 0.5f || fabsf(s->sq) >= 0.5f;
}

static float speed_of(const PhysicsState *s) { return sqrtf(s->svx * s->svx + s->svy * s->svy); }

void physics_update(Mascot *m, float dt)
{
    PhysicsState *s = &m->phys;
    if (dt <= 0) return;
    if (dt > 0.1f) dt = 0.1f;
    const bool held = m->held;

    // picked up / let go
    if (held && !s->was_held) {
        s->held_t = 0;
        s->land_left = 0;
        m->ev.bits |= MEV_PICKUP;
    }
    if (!held && s->was_held) {
        // how hard he lands, from how fast he was going when let go (still .. fast drag)
        m->ev.bits |= MEV_DROP;
        m->ev.land = clampf((speed_of(s) - STILL_SPEED) / (FAST_ON - STILL_SPEED), 0.0f, 1.0f);
        s->land_left = LAND_TIME;
        s->sqv += LAND_KICK;
        if (s->dizzy_left > 0) s->dizzy_left = maxf(s->dizzy_left, DIZZY_AFTER_RELEASE);
    }
    s->was_held = held;
    if (held) s->held_t += dt;

    // smoothed velocity
    float vx = held ? clampf(m->vel_x, -VEL_CLAMP, VEL_CLAMP) : 0.0f;
    float vy = held ? clampf(m->vel_y, -VEL_CLAMP, VEL_CLAMP) : 0.0f;
    float k = 1.0f - expf(-dt / VEL_TAU);
    s->svx += (vx - s->svx) * k;
    s->svy += (vy - s->svy) * k;
    float speed = speed_of(s);
    if (speed > FAST_ON) s->fast = true;
    else if (speed < FAST_OFF) s->fast = false;

    // shake detection, fast direction flips pump up an energy that decays
    int revs = reversal(&s->dir_x, &s->quiet_x, s->svx, dt) + reversal(&s->dir_y, &s->quiet_y, s->svy, dt);
    s->energy = clampf(s->energy + revs * SHAKE_KICK - SHAKE_DECAY * dt, 0.0f, SHAKE_MAX);
    if (held && revs > 0) m->ev.bits |= MEV_SHAKE;
    if (held && revs > 0 && s->energy >= SHAKE_DIZZY) {   // refreshed only while still shaking
        if (s->dizzy_left <= 0) m->ev.bits |= MEV_DIZZY;
        s->dizzy_left = DIZZY_HOLD;
        s->shakeoff_left = 0;
    }

    if (s->dizzy_left > 0) {
        s->dizzy_left -= dt;
        if (s->dizzy_left <= 0) {
            s->dizzy_left = 0;
            s->shakeoff_left = SHAKEOFF_TIME;
            m->ev.bits |= MEV_SHAKEOFF;
        }
    } else if (s->shakeoff_left > 0) {
        s->shakeoff_left = maxf(0.0f, s->shakeoff_left - dt);
    }
    if (s->land_left > 0) s->land_left = maxf(0.0f, s->land_left - dt);

    // spring targets
    float tx = 0, ty = 0, tsq = 0, tpx = 0, tpy = 0, tla = 0, tra = 0;
    if (held) {
        tx = clampf(-s->svx / LEAN_PX, -LEAN_MAX, LEAN_MAX);
        ty = clampf(-s->svy / DROP_PX, -DROP_MAX, DROP_MAX);
        tsq = HELD_STRETCH;
        tpx = clampf(-s->svx / PUPIL_PX, -2.0f, 2.0f);
        tpy = clampf(PUPIL_DOWN - s->svy / PUPIL_PX, -2.0f, 2.0f);
        // trailing arm streams back and up, moving down lifts both and moving up lets them hang
        float right = maxf(0.0f, s->svx) / ARM_PX, left = maxf(0.0f, -s->svx) / ARM_PX;
        float down = maxf(0.0f, s->svy) / ARM_PX, up = maxf(0.0f, -s->svy) / ARM_PX;
        tla = clampf(ARM_HELD + right - 0.3f * left + down - up, 0.0f, 1.0f);
        tra = clampf(ARM_HELD + left - 0.3f * right + down - up, 0.0f, 1.0f);
    }

    int n = (int)ceilf(dt * 120.0f);
    float h = dt / (float)n;
    for (int i = 0; i < n; i++) {
        spring(&s->ox, &s->oxv, tx, LEAN_W, LEAN_Z, h);
        spring(&s->oy, &s->oyv, ty, LEAN_W, LEAN_Z, h);
        spring(&s->sq, &s->sqv, tsq, SQ_W, SQ_Z, h);
        spring(&s->pux, &s->puxv, tpx, PUPIL_W, PUPIL_Z, h);
        spring(&s->puy, &s->puyv, tpy, PUPIL_W, PUPIL_Z, h);
        spring(&s->la, &s->lav, tla, ARM_W, ARM_Z, h);
        spring(&s->ra, &s->rav, tra, ARM_W, ARM_Z, h);
    }
    if (held && s->held_t < PICK_POP) {     // startled flinch, hunched down and held there
        s->sq = 3.0f;
        s->sqv = 0;
    }
}

void physics_pose(const Mascot *m, Pose *p)
{
    if (!active(m)) return;
    const PhysicsState *s = &m->phys;
    const float t = m->time;
    float sx = s->ox, sy = s->oy, b = s->sq;

    if (s->dizzy_left > 0) {
        // drunk figure eight wobble, stronger once hes back on his feet
        float amt = m->held ? 0.6f : 1.0f;
        sx += 1.5f * amt * sinf(t * 5.5f);
        if (!m->held) b = maxf(b, 0.0f) + 0.5f + 0.6f * sinf(t * 11.0f + 1.0f);
    } else if (s->shakeoff_left > 0) {
        float u = SHAKEOFF_TIME - s->shakeoff_left;
        if (u < SHAKEOFF_HEAD) sx = ((int)(u / 0.06f) & 1) ? 1.0f : -1.0f;
    }

    p->shift_x = clampi(roundi(sx), -3, 3);
    p->shift_y = clampi(roundi(sy), -2, 2);
    p->bob = clampi(roundi(b), (s->dizzy_left > 0 && !m->held) ? 0 : -1, 3);
}

// hand row for an arm raised by raise (0 = resting on the keys, 1 = all the way up)
static int arm_row(const Face *f, float raise)
{
    raise = clampf(raise, 0.0f, 1.0f);
    int top = f->hy - 1;
    return roundi(30.0f - raise * (float)(30 - top));
}

// spinning violet spiral eye, dir flips which way it spins
static void spiral_eye(int x, int y, float t, int dir)
{
    mk_eye(x, y, EYE_OPEN, 9, 9, 0, 0);     // pupil pushed off the eye so its just rim + fill
    for (int j = 0; j < 9; j++)
        for (int i = 0; i < 7; i++) {
            if (mk_get(x + i, y + j) != 'Y') continue;
            float dx = (float)(i - 3), dy = (float)(j - 4) * 0.78f;
            float r = sqrtf(dx * dx + dy * dy);
            float a = atan2f(dy, dx) * (float)dir;
            float ph = a / 6.2831853f + r * 0.42f - t * 1.8f;
            ph -= floorf(ph);
            mk_put(x + i, y + j, ph < 0.5f ? 'v' : 'Y');
        }
    mk_put(x + 3, y + 4, 'p');
}

// wide open startled eye with a pin pupil
static void pop_eye(int x, int y)
{
    mk_eye(x, y, EYE_OPEN, 9, 9, 0, 0);
    mk_put(x + 3, y + 4, 'p');
    mk_put(x + 3, y + 5, 'p');
}

static void mouth_o(int my)                 // small "oh"
{
    mk_fill(19, my, 2, 2, 'O');
}

static void mouth_shock(int my)             // big round gasp
{
    mk_hline(19, my - 1, 2, 'O');
    mk_vline(18, my, 2, 'O');
    mk_vline(21, my, 2, 'O');
    mk_hline(19, my + 2, 2, 'O');
    mk_fill(19, my, 2, 2, 'p');
}

static void mouth_wobble(int my, float t, int w)   // nervous / dizzy squiggle
{
    int phase = (int)(t * 6.0f);
    for (int i = 0; i < w; i++) {
        int x = 20 - w / 2 + i;
        mk_put(x, my + (((i + phase) / 2) & 1), 'O');
    }
}

static void mouth_smile(int my)
{
    mk_put(17, my, 'O');
    mk_put(22, my, 'O');
    mk_hline(18, my + 1, 4, 'O');
    mk_hline(19, my + 1, 2, 'Y');
}

static void mouth_aah(int my)               // wide open, into the wind
{
    mk_fill(17, my, 6, 3, 'O');
    mk_hline(18, my + 1, 4, 'p');
}

bool physics_face(const Mascot *m, Face *f)
{
    const PhysicsState *s = &m->phys;
    if (!(m->held || s->dizzy_left > 0 || s->shakeoff_left > 0 || s->land_left > 0)) return false;
    const float t = f->t;
    const int my = f->my;

    if (s->dizzy_left > 0) {
        spiral_eye(f->lx, f->ly, t, 1);
        spiral_eye(f->rx, f->ly, t, -1);
        mouth_wobble(my, t, 8);
        // limp arms swaying out of step
        if (m->held) {
            f->lh = arm_row(f, s->la + 0.12f * sinf(t * 5.5f));
            f->rh = arm_row(f, s->ra + 0.12f * sinf(t * 5.5f + 2.0f));
        } else {
            f->lh = arm_row(f, 0.15f + 0.12f * sinf(t * 5.5f));
            f->rh = arm_row(f, 0.15f + 0.12f * sinf(t * 5.5f + 2.5f));
        }
        return true;
    }

    if (s->shakeoff_left > 0) {
        float u = SHAKEOFF_TIME - s->shakeoff_left;
        if (u < SHAKEOFF_HEAD) {              // brrr, eyes screwed shut and head shaking
            mk_eye(f->lx, f->ly, EYE_SHUT, 0, 0, 0, 0);
            mk_eye(f->rx, f->ly, EYE_SHUT, 0, 0, 0, 0);
            mk_hline(17, my + 1, 6, 'O');
            f->lh = arm_row(f, ((int)(u / 0.06f) & 1) ? 0.35f : 0.2f);
            f->rh = arm_row(f, ((int)(u / 0.06f) & 1) ? 0.2f : 0.35f);
        } else {                              // ...and back, eyes open, all better
            mk_eye(f->lx, f->ly - 1, EYE_OPEN, 0, 0, 0, 0);
            mk_eye(f->rx, f->ly - 1, EYE_OPEN, 0, 0, 0, 0);
            mouth_smile(my);
        }
        return true;
    }

    if (m->held && s->held_t < PICK_POP) {    // startled pick up
        if (s->held_t < 0.05f) {
            mk_eye(f->lx, f->ly, EYE_SHUT, 0, 0, 0, 0);
            mk_eye(f->rx, f->ly, EYE_SHUT, 0, 0, 0, 0);
        } else {
            pop_eye(f->lx, f->ly - 1);
            pop_eye(f->rx, f->ly - 1);
        }
        mouth_shock(my);
        bool a = ((int)(s->held_t / 0.07f) & 1) != 0;   // flailing
        f->lh = f->hy - 1 + (a ? 0 : 7);
        f->rh = f->hy - 1 + (a ? 7 : 0);
        return true;
    }

    if (m->held) {
        float speed = speed_of(s);
        int pdx = clampi(roundi(s->pux), -2, 2), pdy = clampi(roundi(s->puy), -2, 2);
        float flutter = s->fast ? 0.1f * sinf(t * 40.0f) : 0.0f;
        f->lh = arm_row(f, s->la + (s->svx > 0 ? flutter : 0.0f));
        f->rh = arm_row(f, s->ra + (s->svx < 0 ? flutter : 0.0f));
        if (s->fast) {                        // squinting into the wind
            mk_eye(f->lx, f->ly, EYE_OPEN, pdx, pdy, 2, 0);
            mk_eye(f->rx, f->ly, EYE_OPEN, pdx, pdy, 2, 0);
            mouth_aah(my);
        } else if (speed > STILL_SPEED) {     // carried along, watching where hes going
            mk_eye(f->lx, f->ly, EYE_OPEN, pdx, pdy, 0, 0);
            mk_eye(f->rx, f->ly, EYE_OPEN, pdx, pdy, 0, 0);
            mouth_o(my);
        } else {                              // dangling, nervous glances at the ground
            EyeShape shape = m->blink_left > 0 ? EYE_SHUT : EYE_OPEN;
            float g = fmodf(t, 2.4f);
            if (g > 1.2f && g < 1.5f) pdx = clampi(pdx - 1, -2, 2);
            else if (g > 1.9f && g < 2.2f) pdx = clampi(pdx + 1, -2, 2);
            mk_eye(f->lx, f->ly, shape, pdx, pdy, 1, 0);
            mk_eye(f->rx, f->ly, shape, pdx, pdy, 1, 0);
            mouth_wobble(my, t * 0.5f, 6);
            float tremble = fmodf(t, 0.5f) < 0.25f ? 0.0f : 0.06f;
            f->lh = arm_row(f, s->la + tremble);
            f->rh = arm_row(f, s->ra + 0.06f - tremble);
        }
        return true;
    }

    // landed
    float u = LAND_TIME - s->land_left;
    if (u < LAND_IMPACT) {                    // oof
        mk_eye(f->lx, f->ly + 1, EYE_SHUT, 0, 0, 0, 0);
        mk_eye(f->rx, f->ly + 1, EYE_SHUT, 0, 0, 0, 0);
        mk_hline(17, my + 1, 6, 'O');
    } else {                                  // phew
        mk_eye(f->lx, f->ly, EYE_HAPPY, 0, 0, 0, 0);
        mk_eye(f->rx, f->ly, EYE_HAPPY, 0, 0, 0, 0);
        mouth_smile(my);
    }
    f->lh = arm_row(f, s->la);
    f->rh = arm_row(f, s->ra);
    return true;
}

static const char *BANG[] = {
    "YO",
    "YO",
    "YO",
    "..",
    "YO",
};

static const char *STAR[] = {   // dark corners so it shows up over the light casing
    "KwK",
    "wYw",
    "KwK",
};

static const char *DROP[] = {
    ".q",
    "qw",
};

// stars orbiting over his screen, spread > 0 flings them outward (shaking it off)
static void stars(const Face *f, float t, float spread)
{
    for (int k = 0; k < 3; k++) {
        float a = t * 3.2f + (float)k * 2.0943951f;
        float rx = 13.0f + spread * 12.0f, ry = 2.2f + spread * 3.0f;
        int x = roundi(20.0f + rx * cosf(a) - 0.5f);
        int y = roundi((float)f->hy - 0.5f + ry * sinf(a) - spread * 3.0f);
        if (y < 3) y = 3;
        if (x < 3 || x > 36) continue;
        if (sinf(a) > -0.2f && spread == 0) mk_stamp(x - 1, y - 1, STAR, 3, false);  // in front, big
        else mk_put(x, y, (k & 1) ? 'w' : 'Y');                                       // behind, small
    }
}

void physics_overlay(const Mascot *m, const Face *f)
{
    if (!active(m)) return;
    const PhysicsState *s = &m->phys;
    const float t = f->t;

    if (m->held && s->held_t >= 0.03f && s->held_t < PICK_POP && f->hy >= 8) {
        // '!' with little ticks flashing on both sides at first
        mk_stamp(13, f->hy - 7, BANG, 5, false);
        if (s->held_t < 0.25f) {
            mk_put(10, f->hy - 6, 'w');
            mk_put(11, f->hy - 5, 'w');
            mk_put(17, f->hy - 6, 'w');
            mk_put(16, f->hy - 5, 'w');
        }
    }

    if (s->dizzy_left > 0) stars(f, t, 0.0f);
    else if (s->shakeoff_left > 0) {
        float u = SHAKEOFF_TIME - s->shakeoff_left;
        if (u < 0.22f) stars(f, t, u / 0.22f);
    }

    // speed lines at the feet, trailing behind
    if (m->held && s->fast && fabsf(s->svx) > fabsf(s->svy) * 0.5f) {
        bool right = s->svx > 0;                 // moving right puts the lines on the left
        int off = (int)(t * 20.0f) % 2;
        for (int r = 0; r < 2; r++) {
            int len = 3 - r;
            int x0 = right ? 1 + off + r : 38 - off - r - len + 1;
            mk_hline(x0, 41 + r * 2, len, 'w');
        }
    }

    // nervous sweat drop while held (and not going fast), a relieved one after landing
    if (m->held && s->held_t >= PICK_POP && !s->fast && s->dizzy_left <= 0) {
        float g = fmodf(s->held_t, 3.0f);
        if (g > 1.0f && g < 2.2f) mk_stamp(29, f->ly + (int)((g - 1.0f) * 3.0f), DROP, 2, false);
    } else if (!m->held && s->land_left > 0 && s->dizzy_left <= 0) {
        float u = LAND_TIME - s->land_left;
        if (u > LAND_IMPACT && u < 0.9f) mk_stamp(29, f->ly - 1 + (int)((u - LAND_IMPACT) * 6.0f), DROP, 2, false);
        if (u < 0.35f) {                         // dust puffs at the feet
            int d = (int)(u / 0.1f);
            mk_hline(6 - d, 43, 2, 'w');
            mk_put(7 - d, 42, 'W');
            mk_hline(32 + d, 43, 2, 'w');
            mk_put(32 + d, 42, 'W');
        }
    }
}

// preview (make anim physics)

static void drive_pickup(Mascot *m, float t)
{
    m->held = t >= 0.3f;
    m->vel_x = m->vel_y = 0;
}

static void drive_slow_right(Mascot *m, float t)
{
    m->held = t >= 0.2f;
    // whole pixel mouse steps, 0, 10, 0, 10 px per frame -> ~300 px/s but spiky
    m->vel_x = m->held && t >= 0.6f ? (((int)(t * 60.0f + 0.5f) & 1) ? 600.0f : 0.0f) : 0.0f;
    m->vel_y = 0;
}

static void drive_fast_left(Mascot *m, float t)
{
    m->held = t >= 0.2f;
    m->vel_x = m->held && t >= 0.6f ? (((int)(t * 60.0f + 0.5f) & 1) ? -2200.0f : -1400.0f) : 0.0f;
    m->vel_y = 0;
}

static void drive_fast_stop(Mascot *m, float t)   // fast drag left that halts at 1.3 s
{
    drive_fast_left(m, t);
    if (t >= 1.3f) m->vel_x = 0;
}

static void drive_shake(Mascot *m, float t)
{
    m->held = t >= 0.2f && t < 3.2f;
    m->vel_x = m->held && t >= 0.5f && t < 3.0f ? (sinf((t - 0.5f) * 25.0f) >= 0 ? 2500.0f : -2500.0f) : 0.0f;
    m->vel_y = 0;
}

static void drive_drop(Mascot *m, float t)
{
    m->held = t >= 0.2f && t < 1.5f;
    m->vel_x = m->vel_y = 0;
}

static char labels[64][19];

static int add(PreviewFrame *out, int n, int cap, const char *label, float seconds, void (*drive)(Mascot *, float))
{
    if (n >= cap || n >= 64) return n;
    snprintf(labels[n], sizeof labels[n], "%s", label);
    out[n] = (PreviewFrame){ labels[n], seconds, drive };
    return n + 1;
}

static int add_seq(PreviewFrame *out, int n, int cap, const char *name, float t0, float step, int count,
                   float origin, void (*drive)(Mascot *, float))
{
    for (int i = 0; i < count; i++) {
        char label[32];
        float t = t0 + step * (float)i;
        snprintf(label, sizeof label, "%s %+.2f", name, t - origin);
        n = add(out, n, cap, label, t, drive);
    }
    return n;
}

int physics_preview(PreviewFrame *out, int cap)
{
    int n = 0;
    n = add(out, n, cap, "pick-up!", 0.42f, drive_pickup);
    n = add(out, n, cap, "held still", 2.3f, drive_pickup);
    n = add(out, n, cap, "drag slow right", 1.8f, drive_slow_right);
    n = add(out, n, cap, "drag fast left", 1.3f, drive_fast_left);
    n = add(out, n, cap, "stop: swing thru", 1.45f, drive_fast_stop);
    n = add(out, n, cap, "mid-shake", 1.1f, drive_shake);
    n = add(out, n, cap, "dizzy after drop", 3.6f, drive_shake);
    n = add(out, n, cap, "shake it off", 5.7f, drive_shake);
    n = add(out, n, cap, "landing squash", 1.55f, drive_drop);
    n = add(out, n, cap, "relieved", 2.1f, drive_drop);
    n = add_seq(out, n, cap, "pick", 0.26f, 0.08f, 12, 0.3f, drive_pickup);
    n = add_seq(out, n, cap, "shake", 0.45f, 0.1f, 12, 0.5f, drive_shake);
    n = add_seq(out, n, cap, "drop", 1.44f, 0.09f, 12, 1.5f, drive_drop);
    n = add_seq(out, n, cap, "shake-off", 5.45f, 0.08f, 8, 5.6f, drive_shake);
    return n;
}
