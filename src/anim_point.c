// walking over to something on screen and pointing at it (phases come from pointer.c via
// the point_* inputs). OUT crouches and scuttles off, AT springs the arm out to the target,
// then a reaction to how it ended (hop, "hm?" or '!') and BACK walks home relaxed.
// driving (anim_wheel.c) reuses the walk, the stride, lean, squash and dust run whenever the
// window moves. point_update integrates everything and raises the MEV_* moments in m->ev,
// the other hooks only read PointState

#include <math.h>

#include "mascot_kit.h"
#include "pointer.h"

// ---- tuning

#define VEL_TAU       0.05f     // secs, travel velocity low pass
#define WALK_MIN      15.0f     // px/s, slower than this is standing
#define STEP_MIN      4.5f      // steps/s at a crawl
#define STEP_PX       260.0f    // +1 step/s for every this many px/s
#define STEP_MAX      11.0f     // step rate cap, past it the speed shows in the lean and streaks
#define FAST          1300.0f   // px/s where he squints into the wind, arms streaming back
#define PUFF_MAX      2200.0f   // px/s, any faster and the dust is gone before it shows

#define LEAN_PX       650.0f    // px/s of travel per art px of lean
#define LEAN_MAX      1.8f      // (2 = the whole sprite pitches a pixel further)
#define LEAN_W        11.0f     // lean spring about 1.7 Hz
#define LEAN_Z        0.32f     // underdamped so stopping swings through
#define LEAN_KICK     9.0f      // art px/s forward on arrival (momentum)
#define REACH_LEAN    0.7f      // leaning in toward what hes pointing at

#define SQ_W          18.0f     // squash spring
#define SQ_Z          0.34f
#define LAND_KICK     30.0f     // rows/s downward on arrival
#define CROUCH_TIME   0.12f     // crouch before setting off

#define REACH_DELAY   0.13f     // wind up before the arm shoots out
#define REACH_W       21.0f     // extend fast and underdamped (overshoots ~20%)
#define REACH_Z       0.42f
#define RETRACT_W     19.0f     // retract, critically damped
#define SNAP_W        46.0f     // target moved, the arm snaps back
#define SLOW_W        12.0f     // clicked elsewhere, a slower deflated retract
#define ARM_OUT_MIN   0.04f     // reach where the hand gets swapped for the cable
#define LOCK_AT       0.9f      // reach where the plug counts as arrived
#define CLICK_HOLD    0.32f     // plug stays in, lit up, this long after a click
#define CLICK_HOLD_WALKING 0.12f  // same if hes already walking home
#define REACT_LEN     0.50f     // how long the reaction lasts
#define SETTLE_TIME   0.45f     // arrival settle
#define HAND_DROP     0.22f     // secs with the hand back at the port before it drops to the keys
#define WAVE_TIME     0.9f      // free hand waves this long after the plug arrives

#define PUFF_LIFE     0.36f
#define LAND_LIFE     0.42f

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int roundi(float v) { return (int)floorf(v + 0.5f); }

static void spring(float *x, float *v, float target, float w, float z, float dt)
{
    *v += (w * w * (target - *x) - 2.0f * z * w * *v) * dt;
    *x += *v * dt;
}

#define DRIVE_STILL   0.12f     // secs the window has to stand still before a drive walk ends

static bool on_trip(int phase) { return phase == POINTER_OUT || phase == POINTER_BACK; }

static float speed_of(const PointState *s) { return sqrtf(s->vx * s->vx + s->vy * s->vy); }

// on a pointing trip or walking the real pointer around
static bool travelling(const Mascot *m) { return on_trip(m->point_phase) || m->point.drive_walk; }

bool point_walking(const PointState *s)
{
    return (on_trip(s->phase) || s->drive_walk) && speed_of(s) > WALK_MIN;
}

static bool walking(const Mascot *m) { return point_walking(&m->point); }

static bool reacting(const Mascot *m)
{
    return m->point.result != POINTER_RESULT_NONE && m->point.result_t < REACT_LEN;
}

// anything to show at all? (everything is a no-op if not)
static bool active(const Mascot *m)
{
    const PointState *s = &m->point;
    return m->point_phase != POINTER_HOME || s->drive_walk || s->out || s->reach > 0.01f || s->settle_left > 0.0f ||
           reacting(m) || (s->side != 0 && s->in_t < HAND_DROP) ||
           fabsf(s->lean) > 0.3f || fabsf(s->sq) > 0.3f;
}

static void puff(PointState *s, float x, float y, float life, int big, int drift)
{
    PointPuff *p = &s->puffs[s->next_puff];
    s->next_puff = (s->next_puff + 1) % POINT_PUFFS;
    *p = (PointPuff){ x, y, s->trip_x, s->trip_y, life, life, big, drift };
}

PointMark point_head_mark(int phase, int result, float result_t, int *note)
{
    const float u = result_t;
    if (result == POINTER_RESULT_CLICKED_OUT && u > 0.05f && u < 0.75f) return POINT_MARK_HUH;
    if (result == POINTER_RESULT_MOVED && u > 0.03f && u < 0.5f) return POINT_MARK_BANG;
    if (phase == POINTER_BACK && result == POINTER_RESULT_CLICKED_IN && u > POINT_NOTE_FROM &&
        fmodf(u - POINT_NOTE_FROM, POINT_NOTE_EVERY) < POINT_NOTE_LEN) {
        if (note) *note = (int)((u - POINT_NOTE_FROM) / POINT_NOTE_EVERY);
        return POINT_MARK_NOTE;
    }
    return POINT_MARK_NONE;
}

// ---- update

void point_update(Mascot *m, float dt)
{
    PointState *s = &m->point;
    if (!(dt > 0.0f)) return;
    if (dt > 0.1f) dt = 0.1f;
    const int phase = m->point_phase, result = m->point_result;
    int note_was = -1, note = -1;                      // what was over his head last update
    const PointMark mark_was = point_head_mark(s->phase, s->result, s->result_t, &note_was);

    // driving, walking whenever the window moves (a beat of hysteresis so one still frame doesnt end it)
    {
        bool moving = m->drive_in.phase != DRIVE_ANIM_NONE && !on_trip(phase) &&   // (a handover points as usual)
                      (m->walk_vx != 0.0f || m->walk_vy != 0.0f);
        bool was = s->drive_walk;
        s->drive_still = moving ? 0.0f : s->drive_still + dt;
        if (moving && !was) {
            s->drive_walk = true;
            s->dir = m->walk_vx > 0.0f ? 1 : (m->walk_vx < 0.0f ? -1 : (s->dir ? s->dir : 1));
            s->stride = 0.0f;
            m->ev.bits |= MEV_DEPART;
        } else if (was && (m->drive_in.phase == DRIVE_ANIM_NONE || s->drive_still >= DRIVE_STILL)) {
            s->drive_walk = false;
            if (!on_trip(phase)) {                         // pulled up, momentum squash and dust
                m->ev.bits |= MEV_ARRIVE;
                s->settle_left = SETTLE_TIME;
                s->settle_home = false;
                s->sq_v += LAND_KICK * 0.7f;
                s->lean_v += (float)s->dir * LEAN_KICK * 0.8f;
                puff(s, 7.0f, 42.0f, LAND_LIFE, 1, -1);
                puff(s, 32.0f, 42.0f, LAND_LIFE, 1, 1);
            }
        }
    }

    // edges
    if (result != s->result) {
        if (result == POINTER_RESULT_CLICKED_IN) m->ev.bits |= MEV_POINT_HIT;
        else if (result == POINTER_RESULT_CLICKED_OUT || result == POINTER_RESULT_MOVED ||
                 result == POINTER_RESULT_DISMISSED) m->ev.bits |= MEV_POINT_MISS;
        s->result = result;
        s->result_t = 0.0f;
    } else if (result) {
        s->result_t += dt;
    }
    if (phase != s->phase) {
        if (!on_trip(s->phase) && on_trip(phase)) m->ev.bits |= MEV_DEPART;   // out, or home
        if (on_trip(s->phase) && !on_trip(phase)) {       // arrived, same momentum squash and dust
            m->ev.bits |= MEV_ARRIVE;
            s->settle_left = SETTLE_TIME;
            s->settle_home = phase == POINTER_HOME;
            s->sq_v += LAND_KICK;
            s->lean_v += (float)s->dir * LEAN_KICK;
            puff(s, 7.0f, 42.0f, LAND_LIFE, 1, -1);
            puff(s, 32.0f, 42.0f, LAND_LIFE, 1, 1);
        }
        if (phase == POINTER_OUT) {
            s->dir = m->point_dx >= 0.0f ? 1 : -1;
            s->stride = 0.0f;
        }
        if (phase == POINTER_AT) {
            s->side = m->point_dx >= 0.0f ? 1 : -1;
            s->locked = false;
            s->latched = false;
        }
        s->phase = phase;
    }

    // the arm aim follows the target while pointing, frozen after
    if (phase == POINTER_AT && result == POINTER_RESULT_NONE) {
        s->aim_x = m->point_dx;
        s->aim_y = m->point_dy;
        s->latched = false;
    } else if (!s->latched) {
        s->latched = true;
        s->aim_trip_x = s->trip_x;
        s->aim_trip_y = s->trip_y;
    }

    // the arm
    float rt = 0.0f, w = RETRACT_W, z = 1.0f;
    if (phase == POINTER_AT && result == POINTER_RESULT_NONE) {
        if (m->point_t >= REACH_DELAY) { rt = 1.0f; w = REACH_W; z = REACH_Z; }
    } else if (result == POINTER_RESULT_CLICKED_IN && s->reach > ARM_OUT_MIN &&
               s->result_t < (on_trip(phase) ? CLICK_HOLD_WALKING : CLICK_HOLD)) {
        rt = 1.0f; w = 30.0f; z = 0.8f;
    } else if (result == POINTER_RESULT_MOVED) {
        w = SNAP_W;
    } else if (result == POINTER_RESULT_CLICKED_OUT) {
        w = SLOW_W;
    }
    int n = (int)ceilf(dt * 240.0f);
    float h = dt / (float)n;
    for (int i = 0; i < n; i++) spring(&s->reach, &s->reach_v, rt, w, z, h);
    if (s->reach < 0.0f) { s->reach = 0.0f; s->reach_v = 0.0f; }

    bool was_out = s->out;
    s->out = s->side != 0 && s->reach > ARM_OUT_MIN;
    if (!was_out && s->out) m->ev.bits |= MEV_ARM_OUT;
    if (was_out && !s->out) { s->in_t = 0.0f; m->ev.bits |= MEV_ARM_IN; }
    else s->in_t += dt;
    if (!s->locked && phase == POINTER_AT && s->reach >= LOCK_AT) {
        s->locked = true;
        s->lock_t = 0.0f;
        m->ev.bits |= MEV_PLUG;
    } else if (s->locked) s->lock_t += dt;

    // a sound when a head mark shows up or the next note starts
    const PointMark mark = point_head_mark(phase, s->result, s->result_t, &note);
    if (mark != POINT_MARK_NONE && (mark != mark_was || note != note_was)) {
        if (mark == POINT_MARK_HUH) m->ev.bits |= MEV_POINT_HUH;
        if (mark == POINT_MARK_BANG) m->ev.bits |= MEV_POINT_BANG;
        if (mark == POINT_MARK_NOTE) { m->ev.bits |= MEV_WHISTLE; m->ev.note = note; }
    }
    if (phase != POINTER_AT && !s->out && s->in_t > HAND_DROP && !(result && s->result_t < 0.8f))
        s->side = 0;                                   // (kept while the reaction marks still show)

    // travelling
    const bool trip = travelling(m);
    float vx = trip ? m->walk_vx : 0.0f, vy = trip ? m->walk_vy : 0.0f;
    float k = 1.0f - expf(-dt / VEL_TAU);
    s->vx += (vx - s->vx) * k;
    s->vy += (vy - s->vy) * k;
    s->trip_x += vx * dt;
    s->trip_y += vy * dt;
    float speed = speed_of(s);
    if (trip && fabsf(s->vx) > 40.0f) s->dir = s->vx > 0.0f ? 1 : -1;
    if (trip && speed > WALK_MIN) {
        float prev = s->stride;
        s->stride += dt * clampf(STEP_MIN + speed / STEP_PX, STEP_MIN, STEP_MAX);
        if (floorf(s->stride) != floorf(prev)) {      // a foot came down (heard even when too fast for dust)
            m->ev.bits |= MEV_STEP;
            m->ev.foot = ((int)s->stride & 1) ? 1 : -1;
            if (speed < PUFF_MAX) {
                int back = s->dir > 0 ? 0 : 1;
                float x = back == 0 ? 7.0f : 32.0f;
                puff(s, x - (float)s->dir * (((int)s->stride & 1) ? 1.0f : 0.0f), 42.0f, PUFF_LIFE, 0, -s->dir);
            }
        }
    }

    // lean into the motion, or toward the target while pointing
    float lt = 0.0f;
    if (trip) lt = clampf(s->vx / LEAN_PX, -LEAN_MAX, LEAN_MAX);
    else if (s->out && phase == POINTER_AT) lt = (float)s->side * REACH_LEAN;
    if (result == POINTER_RESULT_CLICKED_OUT && s->result_t < REACT_LEN) lt = -(float)s->side * 0.8f;
    float sqt = 0.0f;
    if (phase == POINTER_OUT && m->point_t < CROUCH_TIME) sqt = 2.2f;
    if (phase == POINTER_AT && result == POINTER_RESULT_NONE && m->point_t < REACH_DELAY) sqt = 1.6f;
    for (int i = 0; i < n; i++) {
        spring(&s->lean, &s->lean_v, lt, LEAN_W, LEAN_Z, h);
        spring(&s->sq, &s->sq_v, sqt, SQ_W, SQ_Z, h);
    }
    if (s->settle_left > 0.0f) s->settle_left = fmaxf(0.0f, s->settle_left - dt);

    for (int i = 0; i < POINT_PUFFS; i++)
        if (s->puffs[i].left > 0.0f) s->puffs[i].left = fmaxf(0.0f, s->puffs[i].left - dt);

    // pupils on the target going out and while pointing, looking ahead walking home
    float px = 0.0f, py = 0.0f;
    if (phase == POINTER_OUT || phase == POINTER_AT || s->out) {
        float dx = s->out || phase == POINTER_AT ? s->aim_x : m->point_dx;
        float dy = s->out || phase == POINTER_AT ? s->aim_y : m->point_dy;
        float r = sqrtf(dx * dx + dy * dy);
        if (r > 1.0f) {
            float big = fmaxf(fabsf(dx), fabsf(dy)) / r;
            float mag = 2.4f * r / (r + 25.0f) * (1.0f + (1.0f / big - 1.0f) * 0.55f);
            px = clampf(dx / r * mag, -2.0f, 2.0f);
            py = clampf(dy / r * mag, -2.0f, 2.0f);
        }
    } else if (phase == POINTER_BACK) {
        px = 2.0f * (float)s->dir;
        py = s->vy > 300.0f ? 1.0f : (s->vy < -300.0f ? -1.0f : 0.0f);
    }
    float pk = 1.0f - expf(-dt * 30.0f);
    s->pux += (px - s->pux) * pk;
    s->puy += (py - s->puy) * pk;
}

// ---- pose

// the 4 frame scuttle (see anim_point.h), 1 lifts the trailing foot and 3 the lead one
static int walk_frame(const PointState *s) { return (int)floorf(s->stride * 2.0f) & 3; }

int point_walk_frame(const PointState *s) { return walk_frame(s); }

void point_pose(const Mascot *m, Pose *p)
{
    if (!active(m)) return;
    const PointState *s = &m->point;
    const float u = s->result_t;

    if (walking(m)) {
        static const int BOB[4] = { 1, 0, 1, 0 }, LIFT[4] = { 0, 1, 0, 1 };
        int f = walk_frame(s), d = s->dir;
        int a = d > 0 ? 1 : 0, b = 1 - a;          // lead foot, trailing foot
        p->bob = BOB[f];
        p->lift = LIFT[f];
        if (f == 0) { p->foot_dx[a] = d; p->foot_dx[b] = -d; }
        if (f == 1) p->foot_dy[b] = -2;
        if (f == 2) { p->foot_dx[b] = d; p->foot_dx[a] = -d; }
        if (f == 3) p->foot_dy[a] = -2;
        if (speed_of(s) > FAST) p->foot_dy[f < 2 ? b : a] = -1;   // blur of feet, never both down
    }

    // squash and stretch (crouches, landing)
    if (fabsf(s->sq) >= 0.5f) p->bob = clampi(roundi(s->sq), -1, 3);

    // lean, past one pixel the whole sprite tips over
    int lean = roundi(s->lean);
    p->lean = clampi(lean, -1, 1);
    if (lean >= 2) p->shift_x += 1;
    if (lean <= -2) p->shift_x -= 1;

    if (s->out) p->arm_out = s->side;

    if (reacting(m)) {
        switch (m->point.result) {
        case POINTER_RESULT_CLICKED_IN:            // a little hop for joy
            if (u < 0.06f) p->bob = 2;
            else if (u < 0.30f) {
                p->bob = -1;
                p->shift_y = -roundi(3.0f * sinf(3.1415927f * (u - 0.06f) / 0.24f));
            } else if (u < 0.38f) p->bob = 2;
            else if (u < 0.44f) p->bob = 1;
            break;
        case POINTER_RESULT_MOVED:                 // jolted upright, a shiver
            if (u < 0.10f) p->bob = -1;
            if (u < 0.24f) p->shift_x += ((int)(u / 0.04f) & 1) ? 1 : -1;
            break;
        default:
            break;
        }
    }
}

// ---- face

static void mouth_smile(int my, int sx)
{
    mk_put(17 + sx, my, 'O');
    mk_put(22 + sx, my, 'O');
    mk_hline(18 + sx, my + 1, 4, 'O');
    mk_hline(19 + sx, my + 1, 2, 'Y');
}

static void mouth_grin(int my, int sx)       // big open "here!" / "yay"
{
    mk_put(16 + sx, my, 'O');
    mk_put(23 + sx, my, 'O');
    mk_hline(17 + sx, my + 1, 6, 'O');
    mk_hline(18 + sx, my + 2, 4, 'O');
    mk_hline(18 + sx, my + 1, 4, 'Y');
}

static void mouth_set(int my, int sx)        // determined, flat with one corner up
{
    mk_hline(18 + sx, my + 1, 4, 'O');
    mk_put(22 + sx, my, 'O');
    mk_put(20 + sx, my + 1, 'Y');
}

static void mouth_talk(int my, int sx, float t)
{
    int phase = (int)(fmodf(t, 0.36f) / 0.12f) % 3;
    if (phase == 0) {
        mk_fill(19 + sx, my, 2, 2, 'O');
    } else if (phase == 1) {
        mk_fill(17 + sx, my, 6, 3, 'O');
        mk_hline(18 + sx, my + 1, 4, 'p');
    } else {
        mk_hline(17 + sx, my + 1, 6, 'O');
    }
}

static void mouth_hm(int my, int sx)         // lopsided "hm?"
{
    mk_put(17 + sx, my + 1, 'O');
    mk_hline(18 + sx, my, 3, 'O');
    mk_put(21 + sx, my + 1, 'O');
    mk_put(22 + sx, my + 1, 'O');
}

static void mouth_gasp(int my, int sx)
{
    mk_hline(19 + sx, my - 1, 2, 'O');
    mk_vline(18 + sx, my, 2, 'O');
    mk_vline(21 + sx, my, 2, 'O');
    mk_hline(19 + sx, my + 2, 2, 'O');
    mk_fill(19 + sx, my, 2, 2, 'p');
}

static void mouth_aah(int my, int sx)        // into the wind
{
    mk_fill(17 + sx, my, 6, 3, 'O');
    mk_hline(18 + sx, my + 1, 4, 'p');
}

// both eyes looking at (pdx, pdy), a pupil in a bottom corner under a lid gets lifted
static void eyes(const Face *f, int sx, EyeShape shape, int pdx, int pdy, int lid, int low)
{
    pdx = clampi(pdx, -2, 2);
    pdy = clampi(pdy, -2, 2);
    if (lid > 0 && pdy >= 2 && (pdx >= 2 || pdx <= -2)) pdy = 1;
    mk_eye(f->lx + sx, f->ly, shape, pdx, pdy, lid, low);
    mk_eye(f->rx + sx, f->ly, shape, pdx, pdy, lid, low);
}

// hand rows (see Face), resting on the keys or raised to about the port
static int hand_rest(void) { return 30; }
static int hand_port(const Face *f) { return f->hy + 11; }

bool point_face(const Mascot *m, Face *f)
{
    if (!active(m)) return false;
    const PointState *s = &m->point;
    const float t = f->t, u = s->result_t;
    const int phase = m->point_phase, my = f->my;
    const bool shut = m->blink_left > 0.0f;
    const bool talking = m->mood == MOOD_TALKING;
    int pdx = roundi(s->pux), pdy = roundi(s->puy);
    int side = s->side;
    int sx = 0;                                   // the face turns a pixel toward the target
    if (s->out && phase == POINTER_AT) sx = side;

    // free hand (both while walking), the pointing one is the cable
    int *free_h = side > 0 ? &f->lh : &f->rh, *arm_h = side > 0 ? &f->rh : &f->lh;

    if (walking(m)) {
        int fr = walk_frame(s), d = s->dir;
        int *lead = d > 0 ? &f->rh : &f->lh, *trail = d > 0 ? &f->lh : &f->rh;
        if (speed_of(s) > FAST) {                 // arms streaming back
            *trail = f->hy + 3 + (fr & 1);
            *lead = f->hy + 16 + (fr & 1);
        } else {                                   // pumping, opposite to the feet
            *lead = fr == 2 ? f->hy + 14 : (fr == 0 ? hand_rest() : f->hy + 18);
            *trail = fr == 0 ? f->hy + 14 : (fr == 2 ? hand_rest() : f->hy + 18);
        }
    }

    // reacting to how it ended
    if (reacting(m)) {
        switch (s->result) {
        case POINTER_RESULT_CLICKED_IN:
            eyes(f, sx, EYE_HAPPY, 0, 0, 0, 0);
            mouth_grin(my, sx);
            mk_hline(f->lx + sx, f->ly + 9, 2, 'k');      // a pleased blush
            mk_hline(f->rx + sx + 5, f->ly + 9, 2, 'k');
            if (side != 0) {
                *free_h = f->hy - 1 + (((int)(u / 0.1f) & 1) ? 2 : 0);   // fist pump
                if (!s->out) *arm_h = u < 0.42f ? f->hy + 2 : hand_port(f);
            }
            return true;
        case POINTER_RESULT_CLICKED_OUT: {            // "hm? not that one?"
            int look = -side;                           // glances at his own hand, then away
            mk_eye(f->lx + sx, f->ly, EYE_OPEN, look, u < 0.25f ? 1 : -1, side < 0 ? 0 : 2, 0);
            mk_eye(f->rx + sx, f->ly, EYE_OPEN, look, u < 0.25f ? 1 : -1, side < 0 ? 2 : 0, 0);
            mouth_hm(my, sx);
            if (side != 0) *free_h = f->hy + 4;           // scratching his casing
            if (side != 0 && !s->out) *arm_h = hand_port(f) + 4;
            return true;
        }
        case POINTER_RESULT_MOVED:                    // whoa!
            if (u < 0.04f) {
                eyes(f, sx, EYE_SHUT, 0, 0, 0, 0);
            } else {
                for (int e = 0; e < 2; e++) {
                    int x = e ? f->rx : f->lx;
                    mk_eye(x, f->ly - 1, EYE_OPEN, 9, 9, 0, 0);
                    mk_put(x + 3, f->ly + 3, 'p');
                    mk_put(x + 3, f->ly + 4, 'p');
                }
            }
            mouth_gasp(my, 0);
            f->lh = f->hy + 1 + (((int)(u / 0.06f) & 1) ? 0 : 3);
            f->rh = f->hy + 1 + (((int)(u / 0.06f) & 1) ? 3 : 0);
            return true;
        default:                                      // called off, all good, back home
            if (u < 0.12f || shut) eyes(f, sx, EYE_SHUT, 0, 0, 0, 0);
            else eyes(f, sx, EYE_OPEN, pdx, pdy, 0, 0);
            if (talking) mouth_talk(my, sx, t);
            else mouth_smile(my, sx);
            if (side != 0 && !s->out) *arm_h = hand_port(f);
            return true;
        }
    }

    // pointing
    if (phase == POINTER_AT || s->out) {
        bool windup = !s->out && s->reach < ARM_OUT_MIN;
        if (shut) eyes(f, sx, EYE_SHUT, 0, 0, 0, 0);
        else if (s->locked && s->lock_t < 0.16f)       // "there!" eyes pop up a row
            for (int e = 0; e < 2; e++) mk_eye((e ? f->rx : f->lx) + sx, f->ly - 1, EYE_OPEN, pdx, pdy, 0, 0);
        else eyes(f, sx, EYE_OPEN, pdx, pdy, 0, windup ? 0 : 1);
        if (talking) mouth_talk(my, sx, t);
        else if (windup) mk_fill(19 + sx, my, 2, 2, 'O');     // "oh!"
        else mouth_grin(my, sx);
        if (side != 0) {
            if (windup) *arm_h = hand_port(f) - (m->point_t < REACH_DELAY * 0.5f ? 0 : 1);
            if (s->locked && s->lock_t < WAVE_TIME)        // "over here!"
                *free_h = f->hy + 4 + (((int)(s->lock_t / 0.15f) & 1) ? 3 : 0);
            else if (!s->locked)
                *free_h = hand_rest();
        }
        return true;
    }

    // travelling
    if (on_trip(phase)) {
        bool fast = speed_of(s) > FAST;
        if (phase == POINTER_OUT) {                   // on a mission
            if (shut) eyes(f, 0, EYE_SHUT, 0, 0, 0, 0);
            else eyes(f, 0, EYE_OPEN, pdx, pdy, fast ? 2 : 1, 0);
            if (fast) mouth_aah(my, 0);
            else if (m->point_t < CROUCH_TIME) mk_fill(19, my, 2, 2, 'O');
            else mouth_set(my, 0);
        } else {                                      // strolling home, pleased
            bool good = s->result == POINTER_RESULT_CLICKED_IN;
            if (shut || (good && fmodf(t, 1.6f) < 0.45f)) eyes(f, 0, good ? EYE_HAPPY : EYE_SHUT, 0, 0, 0, 0);
            else eyes(f, 0, EYE_OPEN, pdx, pdy, fast ? 2 : 0, fast ? 0 : 1);
            if (fast) mouth_aah(my, 0);
            else if (talking) mouth_talk(my, 0, t);
            else mouth_smile(my, 0);
        }
        if (side != 0 && !s->out && s->in_t < HAND_DROP) *arm_h = hand_port(f);
        return true;
    }

    // just got home (or a hand still coming down)
    if (s->settle_left > 0.0f && s->settle_home) {
        float a = SETTLE_TIME - s->settle_left;
        if (a < 0.1f) eyes(f, 0, EYE_SHUT, 0, 0, 0, 0);
        else eyes(f, 0, EYE_HAPPY, 0, 0, 0, 0);
        mouth_smile(my, 0);
        return true;
    }
    if (side != 0 && !s->out && s->in_t < HAND_DROP) *arm_h = hand_port(f);
    return false;                                     // the usual face (springs settling)
}

// ---- overlay

// the "hm?", '!' and whistled note go above his head outside the canvas, point_fx.c draws them
void point_overlay(const Mascot *m, const Face *f)
{
    (void)m;
    (void)f;
}
