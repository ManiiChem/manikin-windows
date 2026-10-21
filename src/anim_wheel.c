// driving the real pointer. his cable plugs into it and he walks it around, clicks with a
// clack, types on his own keyboard and sweats on long hauls. reads m->drive_in, the walk is
// from anim_point and point_fx.c draws the cable. only wheel_update changes WheelState

#include <math.h>
#include <stddef.h>

#include "mascot_kit.h"
#include "pointer.h"

// ---- tuning

#define REACH_W       24.0f     // latching, the arm shoots out a touch underdamped
#define REACH_Z       0.45f
#define RETRACT_W     22.0f     // letting go, back into the port critically damped
#define SNAP_W        46.0f     // grabbed from him, the arm snaps back
#define ARM_OUT_MIN   0.04f     // reach where the hand gets swapped for the cable
#define GRIP_AT       0.9f      // reach where the plug counts as on the pointer
#define SIDE_SWAP     60.0f     // px past the middle of his face before the other arm takes over
#define TENSION_W     12.0f     // slack <-> taut

#define DROOP_TAUT_W  26.0f     // spring on the cables middle, stiff when taut
#define DROOP_SLACK_W 7.0f      // floppy when slack so it trails and swings
#define DROOP_SLACK_Z 0.28f
#define SAG_SLACK     0.32f     // how much of the cable length hangs down when slack

#define CLICK_GAP     0.09f     // between the jabs of a double click
#define KEY_GLOW      0.16f     // how long a struck key glows
#define KEY_HOT       0.06f     // white hot for this long first
#define STRIKE        0.05f     // fingers stay down on a key this long
#define TYPE_HOLD     0.35f     // hand stays over the keys this long after the last key
#define HAND_W        55.0f     // typing hand darting between keys
#define HAND_Z        0.8f

#define HAUL_ON       0.55f     // haul above this starts the strain (MEV_HAUL)
#define HAUL_OFF      0.3f      // and below this ends it

#define PAUSE_LEN     WHEEL_PAUSE
#define VICTORY_LEN   WHEEL_VICTORY
#define VICTORY_HOP   WHEEL_VICTORY_HOP    // hop (and jingle) starts here, the arm safely back
#define SHEEPISH_LEN  WHEEL_SHEEPISH

#define PI_F 3.14159265f

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int roundi(float v) { return (int)floorf(v + 0.5f); }
static bool pulse(float t, float period, float duty) { return fmodf(t, period) < period * duty; }

static void spring(float *x, float *v, float target, float w, float z, float dt)
{
    *v += (w * w * (target - *x) - 2.0f * z * w * *v) * dt;
    *x += *v * dt;
}

static bool driving(const Mascot *m) { return m->drive_in.phase != DRIVE_ANIM_NONE; }

// anything to show at all? (everything is a no-op if not)
static bool active(const Mascot *m)
{
    const WheelState *w = &m->wheel;
    return driving(m) || w->out || w->reach > 0.01f || w->result_left > 0.0f ||
           w->pause_left > 0.0f || w->typing;
}

static bool steering(const Mascot *m) { return m->drive_in.steering && m->wheel.latched; }

// pointing as usual (pointer.c), during a handover he points while you do it
static bool pointing(const Mascot *m)
{
    return m->point_phase != POINTER_HOME || m->point.out || m->point.reach > 0.01f;
}

// ---- the keyboard

// where each char sits on a qwerty keyboard (row, key widths along). his has 2 rows of 14 so
// the number and top rows go on the back one and home and bottom on the front, words look typed
static int key_of(unsigned cp)
{
    static const char *const ROWS[4] = { "1234567890-=", "qwertyuiop[]", "asdfghjkl;'", "zxcvbnm,./" };
    static const char *const SHIFTED[4] = { "!@#$%^&*()_+", "QWERTYUIOP{}", "ASDFGHJKL:\"", "ZXCVBNM<>?" };
    static const float ROW_X[4] = { 0.0f, 0.5f, 0.8f, 1.3f };
    if (cp == ' ') return WHEEL_KEY_SPACE;
    if (cp == '\n' || cp == '\r') return WHEEL_KEY_ENTER;
    if (cp == '\t') return 1;                             // far left, front row
    if (cp == '\b' || cp == 127) return 26;               // far right, back row
    if (cp == 27) return 0;                               // esc
    for (int r = 0; r < 4; r++)
        for (int i = 0; ROWS[r][i]; i++)
            if ((unsigned char)ROWS[r][i] == cp || (unsigned char)SHIFTED[r][i] == cp) {
                int k = clampi((int)((ROW_X[r] + (float)i) * 14.0f / 12.5f), 0, 13);
                return 2 * k + (r < 2 ? 0 : 1);
            }
    return (int)((cp * 2654435761u >> 16) % 28u);         // anything else goes somewhere, always the same key
}

// note for the keys clack (MEV_KEY ev.note), 0..27 left to right
static int key_note(int key)
{
    if (key == WHEEL_KEY_SPACE) return 13;
    if (key == WHEEL_KEY_ENTER) return 27;
    return key;
}

// cell the fingers aim for (left column) and its row
static void key_cell(int key, int *x, int *row)
{
    if (key == WHEEL_KEY_SPACE) { *x = 19; *row = 38; return; }
    if (key == WHEEL_KEY_ENTER) { *x = 32; *row = 37; return; }
    *x = 6 + key;
    *row = 36 + (key & 1);
}

char wheel_key_glow(const WheelState *w, int key)
{
    char best = 0;
    for (int i = 0; i < WHEEL_LIT; i++) {
        if (w->key[i] != key || w->key_left[i] <= 0.0f) continue;
        if (KEY_GLOW - w->key_left[i] < KEY_HOT) return 'Y';
        best = 'O';
    }
    return best;
}

static int free_side(const WheelState *w) { return w->side ? -w->side : 1; }
static float rest_x(int side) { return side < 0 ? 2.0f : (float)(MASCOT_W - 1 - 5); }

// newest struck key, or -1
static int last_key(const WheelState *w)
{
    int i = (w->key_next + WHEEL_LIT - 1) % WHEEL_LIT;
    return w->key_left[i] > 0.0f || w->type_left > 0.0f ? w->key[i] : -1;
}

bool wheel_typing_hand(const WheelState *w, int *side, int *x, int *y)
{
    if (!w->typing) return false;
    *side = free_side(w);
    *x = roundi(w->hand_x);
    int k = last_key(w), kx, row = 35;
    if (k >= 0) key_cell(k, &kx, &row);
    bool down = w->strike_left > 0.0f && fabsf(w->hand_v) < 40.0f;
    *y = down ? row - 2 : 33;                             // fingers on the key, or just above the keys
    return true;
}

// typing hands left column so its pecking finger (col 2 left hand, 1 right) lands on key `k`
static float hand_target(int side, int k)
{
    int kx, row;
    if (k < 0) k = side < 0 ? 12 : 15;                   // waiting to type, hover over the middle
    key_cell(k, &kx, &row);
    return (float)(side < 0 ? kx - 2 : kx - 1);
}

// ---- update

// pupils on (dx, dy) (screen px from the middle of the face), same as anim_point
static void look(float dx, float dy, float *px, float *py)
{
    float r = sqrtf(dx * dx + dy * dy);
    *px = *py = 0.0f;
    if (r <= 1.0f) return;
    float big = fmaxf(fabsf(dx), fabsf(dy)) / r;
    float mag = 2.4f * r / (r + 25.0f) * (1.0f + (1.0f / big - 1.0f) * 0.55f);
    *px = clampf(dx / r * mag, -2.0f, 2.0f);
    *py = clampf(dy / r * mag, -2.0f, 2.0f);
}

void wheel_update(Mascot *m, float dt)
{
    WheelState *w = &m->wheel;
    const DriveInputs *d = &m->drive_in;
    w->oops = false;
    if (!(dt > 0.0f)) return;
    if (dt > 0.1f) dt = 0.1f;
    const int phase = d->phase;
    const bool on = phase != DRIVE_ANIM_NONE;

    // new drive, the counters start over (nothing to clack for)
    if (on && !w->driving) {
        w->last_clicks = d->clicks;
        w->last_keys = d->keys;
    }
    w->driving = on;

    // phase edges
    if (phase != w->last_phase) {
        w->phase_t = 0.0f;
        if (phase == DRIVE_ANIM_PAUSED) {
            w->pause_left = PAUSE_LEN;
            w->oops = true;
        }
        if (phase == DRIVE_ANIM_DONE) {
            w->result = d->result;
            w->result_left = d->result == DRIVE_RESULT_SHEEPISH ? SHEEPISH_LEN :
                             d->result == DRIVE_RESULT_VICTORY ? VICTORY_LEN : 0.0f;
            if (d->result == DRIVE_RESULT_SHEEPISH) w->oops = true;
        }
        if (phase == DRIVE_ANIM_FETCH) w->result_left = 0.0f;   // a new drive (or carrying on)
        w->last_phase = phase;
    } else {
        w->phase_t += dt;
    }
    if (phase == DRIVE_ANIM_DONE && d->result != w->result && w->result == DRIVE_RESULT_NONE) {
        w->result = d->result;                         // the result came a beat after DONE
        w->result_left = d->result == DRIVE_RESULT_SHEEPISH ? SHEEPISH_LEN : VICTORY_LEN;
        if (d->result == DRIVE_RESULT_SHEEPISH) w->oops = true;
    }
    if (w->result_left > 0.0f) {
        float len = w->result == DRIVE_RESULT_VICTORY ? VICTORY_LEN : SHEEPISH_LEN;
        float was = len - w->result_left;
        w->result_left = fmaxf(0.0f, w->result_left - dt);
        float now = len - w->result_left;
        if (w->result == DRIVE_RESULT_VICTORY && was < VICTORY_HOP && now >= VICTORY_HOP)
            m->ev.bits |= MEV_VICTORY;                 // the hop, play the jingle
    }
    if (w->pause_left > 0.0f) w->pause_left = phase == DRIVE_ANIM_PAUSED || phase == DRIVE_ANIM_WAIT
                                                  ? fmaxf(0.0f, w->pause_left - dt) : 0.0f;

    // the latch
    const bool latched = on && d->latched, was_latched = w->latched;
    const float aim_was_x = w->aim_x, aim_was_y = w->aim_y;
    if (latched) {
        w->aim_x = m->point_dx;
        w->aim_y = m->point_dy;
    }
    if (latched && !w->latched) {                      // plugging on, the arm shoots out
        if (!w->out || w->side == 0) w->side = w->aim_x >= 0.0f ? 1 : -1;
        w->gripped = false;
        w->snap = false;
    }
    if (!latched && w->latched) {                      // letting go
        m->ev.bits |= MEV_LETGO;
        w->snap = phase == DRIVE_ANIM_PAUSED;
    }
    w->latched = latched;
    // the grab is when he takes hold (steering starts), the clunk once the plug is on. plugged
    // in but not steering (trailing your hand, or walking back over to it) isnt a grab yet
    const bool hold = latched && d->steering;
    if (hold && !w->held) w->gripped = false;
    w->held = hold;
    if (latched && w->out && w->aim_x * (float)w->side < -SIDE_SWAP) {
        w->side = -w->side;                            // it went around him, the other arm takes it
        w->hand_x = rest_x(free_side(w));
        w->hand_v = 0.0f;
    }

    // the arm
    {
        float rt = latched ? 1.0f : 0.0f, aw = REACH_W, az = REACH_Z;
        if (!latched) { aw = w->snap ? SNAP_W : RETRACT_W; az = 1.0f; }
        int n = (int)ceilf(dt * 240.0f);
        float h = dt / (float)n;
        for (int i = 0; i < n; i++) spring(&w->reach, &w->reach_v, rt, aw, az, h);
        if (w->reach < 0.0f) { w->reach = 0.0f; w->reach_v = 0.0f; }
        w->out = w->side != 0 && w->reach > ARM_OUT_MIN;
        if (!latched && !w->out && w->reach < 0.01f) w->side = 0;
        if (hold && !w->gripped && w->reach >= GRIP_AT) {
            w->gripped = true;
            w->grip_t = 0.0f;
            m->ev.bits |= MEV_GRAB;                    // the plug clunks onto the pointer
        } else if (w->gripped) {
            w->grip_t += dt;
        }

        // taut while he steers, slack while it trails your hand
        bool taut = latched && d->steering;
        for (int i = 0; i < n; i++) spring(&w->tension, &w->tension_v, taut ? 1.0f : 0.0f, TENSION_W, 1.0f, h);

        // the cables middle hangs (slack) or pulls straight (taut), and for an instant stays
        // where it was in the world when the window or pointer moves so it trails and swings back
        float lx = w->aim_x - (float)w->side * 46.0f, ly = w->aim_y - 6.0f;   // port to plug
        float len = sqrtf(lx * lx + ly * ly);
        float sag = len * SAG_SLACK * (1.0f - w->tension);
        float dw = DROOP_SLACK_W + (DROOP_TAUT_W - DROOP_SLACK_W) * w->tension;
        float dz = DROOP_SLACK_Z + (1.0f - DROOP_SLACK_Z) * w->tension;
        float drag = 1.0f - 0.85f * w->tension;        // (a taut cable hardly lags)
        w->droop_x -= m->walk_vx * dt * drag;
        w->droop_y -= m->walk_vy * dt * drag;
        if (latched && was_latched && w->out) {        // the plug end moved with the pointer
            w->droop_x -= (w->aim_x - aim_was_x) * 0.5f;
            w->droop_y -= (w->aim_y - aim_was_y) * 0.5f;
        }
        for (int i = 0; i < n; i++) {
            spring(&w->droop_x, &w->droop_vx, 0.0f, dw, dz, h);
            spring(&w->droop_y, &w->droop_vy, sag, dw, dz, h);
        }
        float lim = len * 0.6f + 20.0f;
        w->droop_x = clampf(w->droop_x, -lim, lim);
        w->droop_y = clampf(w->droop_y, -lim, lim);
        if (!w->out) w->droop_x = w->droop_y = w->droop_vx = w->droop_vy = 0.0f;
    }

    // clicks, a jab each (a double click is 2 a beat apart)
    if (on && d->clicks > w->last_clicks) w->clicks_due += d->clicks - w->last_clicks;
    w->last_clicks = d->clicks;
    if (w->clicks_due > 4) w->clicks_due = 4;
    w->click_left = fmaxf(0.0f, w->click_left - dt);
    w->click_wait = fmaxf(0.0f, w->click_wait - dt);
    if (w->clicks_due > 0 && w->click_wait <= 0.0f) {
        w->clicks_due--;
        w->click_left = WHEEL_CLICK;
        w->click_wait = CLICK_GAP;
        m->ev.bits |= MEV_CLACK;
    }

    // keys, one lights up per char and the free hand strikes it
    for (int i = 0; i < WHEEL_LIT; i++) w->key_left[i] = fmaxf(0.0f, w->key_left[i] - dt);
    w->strike_left = fmaxf(0.0f, w->strike_left - dt);
    w->type_left = fmaxf(0.0f, w->type_left - dt);
    if (on && d->keys > w->last_keys) {
        int k = key_of(d->last_key);
        w->key[w->key_next] = k;
        w->key_left[w->key_next] = KEY_GLOW;
        w->key_next = (w->key_next + 1) % WHEEL_LIT;
        w->strike_left = STRIKE;
        w->type_left = TYPE_HOLD;
        m->ev.bits |= MEV_KEY;
        m->ev.note = key_note(k);
    }
    w->last_keys = d->keys;
    {
        int fs = free_side(w);
        bool on_keys = (phase == DRIVE_ANIM_TYPE || w->type_left > 0.0f) && on;
        if (!w->typing && on_keys) { w->hand_x = rest_x(fs); w->hand_v = 0.0f; }
        float target = on_keys ? hand_target(fs, last_key(w)) : rest_x(fs);
        int n = (int)ceilf(dt * 240.0f);
        float h = dt / (float)n;
        for (int i = 0; i < n; i++) spring(&w->hand_x, &w->hand_v, target, HAND_W, HAND_Z, h);
        w->typing = on_keys || (w->typing && fabsf(w->hand_x - rest_x(fs)) > 0.6f);
        if (!w->typing) { w->hand_x = rest_x(fs); w->hand_v = 0.0f; }
    }

    // hauling, strain once it gets heavy
    w->haul += (d->haul - w->haul) * (1.0f - expf(-dt / 0.12f));
    if (!on) w->haul = 0.0f;
    if (!w->hauling && steering(m) && d->haul > HAUL_ON) {
        w->hauling = true;
        w->haul_t = 0.0f;
        m->ev.bits |= MEV_HAUL;
    } else if (w->hauling && (d->haul < HAUL_OFF || !steering(m))) {
        w->hauling = false;
    }
    if (w->hauling) w->haul_t += dt;

    // pupils
    float px = 0.0f, py = 0.0f;
    if (phase == DRIVE_ANIM_TYPE || (w->typing && w->type_left > 0.0f)) {
        // down at the keys with a glance up at what hes typing into now and then
        if (fmodf(w->phase_t, 1.6f) > 1.25f) look(w->aim_x, w->aim_y, &px, &py);
        else { px = (w->hand_x - 18.0f) / 9.0f; py = 2.0f; }
    } else if (phase == DRIVE_ANIM_WAIT && m->mood == MOOD_ASK) {
        px = 0.0f;                                     // up at the question
        py = -2.0f;
    } else if (phase == DRIVE_ANIM_FETCH || phase == DRIVE_ANIM_WAIT || phase == DRIVE_ANIM_PAUSED) {
        look(m->point_dx, m->point_dy, &px, &py);      // the pointer, wherever it is
    } else if (w->out) {
        look(w->aim_x, w->aim_y, &px, &py);            // the plug
    }
    float pk = 1.0f - expf(-dt * 30.0f);
    w->pux += (clampf(px, -2.0f, 2.0f) - w->pux) * pk;
    w->puy += (py - w->puy) * pk;
}

// ---- pose

// parabolic hop, 0 outside [t0, t0+dur] and up to `height` rows up in the middle
static int hop(float a, float t0, float dur, float height)
{
    if (a < t0 || a > t0 + dur) return 0;
    float u = (a - t0) / dur;
    return -roundi(height * 4.0f * u * (1.0f - u));
}

void wheel_pose(const Mascot *m, Pose *p)
{
    if (!active(m)) return;
    const WheelState *w = &m->wheel;
    const int phase = m->drive_in.phase;
    const bool walk = point_walking(&m->point);

    if (w->out) p->arm_out = w->side;

    // lean toward the pointer hes holding (the walks lean wins while walking)
    if (w->out && w->tension > 0.5f && !walk) p->lean = w->side;

    // a click, a little lunge at it
    if (w->click_left > 0.0f) {
        float u = WHEEL_CLICK - w->click_left;
        if (u < 0.07f) { p->lean = w->side; p->shift_x += w->side; }
        else if (u < 0.12f) p->bob = 1;
    }

    // straining, hunched and trembling
    if (w->hauling) {
        if (!walk) p->bob = 2;
        p->shift_x += ((int)(m->time / 0.05f) & 1) ? 1 : 0;
    }

    // typing, bent over the keys bouncing with each stroke
    if (w->typing && phase != DRIVE_ANIM_DONE) {
        p->bob = w->strike_left > 0.0f ? 1 : 0;
        p->lean = 0;
    }

    // "oh its yours!", a jolt then frozen stiff
    if (w->pause_left > 0.0f) {
        float u = PAUSE_LEN - w->pause_left;
        if (u < 0.06f) p->bob = 2;
        else if (u < 0.3f) { p->bob = -1; p->shift_y = hop(u, 0.06f, 0.24f, 2.0f); }
        else if (u < 0.38f) p->bob = 2;
        else p->bob = 0;
        p->lean = 0;
    }

    // how it ended
    if (w->result_left > 0.0f) {
        if (w->result == DRIVE_RESULT_VICTORY) {
            float u = VICTORY_LEN - w->result_left;
            if (u < VICTORY_HOP) p->bob = u < VICTORY_HOP - 0.07f ? 0 : 3;      // crouch
            else if (u < 0.5f) { p->bob = -1; p->shift_y = hop(u, VICTORY_HOP, 0.5f - VICTORY_HOP, 5.0f); }
            else if (u < 0.58f) p->bob = 3;                                    // land
            else if (u < 0.82f) { p->bob = 0; p->shift_y = hop(u, 0.58f, 0.24f, 2.0f); }
            else if (u < 0.9f) p->bob = 2;                                     // and a little one
            // and a wiggle at the top of the big hop
            if (u > 0.24f && u < 0.44f) p->shift_x += ((int)((u - 0.24f) / 0.05f) & 1) ? 1 : -1;
        } else {
            float u = SHEEPISH_LEN - w->result_left;
            p->bob = u < 0.3f ? 1 : 2;                                         // shrinking
            p->shift_x += u > 0.3f && pulse(u, 0.8f, 0.5f) ? -1 : 0;           // shuffling
        }
        if (!walk) p->lean = 0;
    }
}

// ---- face

static void mouth_set(int my, int sx)           // determined, flat with one corner up
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

static void mouth_grin(int my, int sx)
{
    mk_put(16 + sx, my, 'O');
    mk_put(23 + sx, my, 'O');
    mk_hline(17 + sx, my + 1, 6, 'O');
    mk_hline(18 + sx, my + 2, 4, 'O');
    mk_hline(18 + sx, my + 1, 4, 'Y');
}

// concentrating, lips pressed and the tip of the tongue poking out a corner
static void mouth_tongue(int my, int sx, int side, bool wag)
{
    int c = side > 0 ? 22 : 17;                     // corner on the side hes working
    mk_hline(18 + sx, my + 1, 4, 'O');
    mk_put(c + sx, my + 1, 'k');
    mk_put(c + sx + (wag ? side : 0), my + 2, 'k');
}

// effort, teeth gritted and corners pulled down twitching
static void mouth_grit(int my, int sx, bool twitch)
{
    mk_hline(17 + sx, my, 6, 'O');
    mk_hline(17 + sx, my + 1, 6, 'H');
    mk_put(19 + sx, my + 1, 'S');
    mk_put(21 + sx, my + 1, 'S');
    mk_hline(17 + sx, my + 2, 6, 'O');
    mk_put(16 + sx, my + (twitch ? 2 : 1), 'O');
    mk_put(23 + sx, my + (twitch ? 1 : 2), 'O');
}

static void mouth_gasp(int my, int sx)
{
    mk_hline(19 + sx, my - 1, 2, 'O');
    mk_vline(18 + sx, my, 2, 'O');
    mk_vline(21 + sx, my, 2, 'O');
    mk_hline(19 + sx, my + 2, 2, 'O');
    mk_fill(19 + sx, my, 2, 2, 'p');
}

// sheepish, wobbly embarrassed little smile
static void mouth_sheepish(int my, int sx)
{
    mk_put(17 + sx, my, 'O');
    mk_hline(18 + sx, my + 1, 2, 'O');
    mk_put(20 + sx, my, 'O');
    mk_hline(21 + sx, my + 1, 2, 'O');
}

// both eyes looking at (pdx, pdy), a pupil in a bottom corner under a lid gets lifted
static void eyes(const Face *f, int sx, EyeShape shape, int pdx, int pdy, int lid, int low)
{
    pdx = clampi(pdx, -2, 2);
    pdy = clampi(pdy, -2, 2);
    if (lid > 0 && pdy >= 2 && (pdx >= 2 || pdx <= -2)) pdy = 1;
    if (lid >= 3 && pdy < 0) pdy = 0;
    mk_eye(f->lx + sx, f->ly, shape, pdx, pdy, lid, low);
    mk_eye(f->rx + sx, f->ly, shape, pdx, pdy, lid, low);
}

// squeezed shut with effort, '>' for the left eye and '<' for the right
static void eyes_squeeze(const Face *f, int sx)
{
    static const char *SQ[] = { "OO.....", "..OO...", "....YO.", "..OO...", "OO....." };
    mk_stamp(f->lx + sx, f->ly + 2, SQ, 5, false);
    mk_stamp(f->rx + sx, f->ly + 2, SQ, 5, true);
}

// wide with shock, no lids and tiny pupils
static void eyes_wide(const Face *f, int pdx)
{
    for (int e = 0; e < 2; e++) {
        int x = e ? f->rx : f->lx;
        mk_eye(x, f->ly - 1, EYE_OPEN, 9, 9, 0, 0);
        mk_put(x + 3 + clampi(pdx, -1, 1), f->ly + 3, 'p');
        mk_put(x + 3 + clampi(pdx, -1, 1), f->ly + 4, 'p');
    }
}

static void blush(const Face *f, int sx)
{
    mk_hline(f->lx + sx, f->ly + 9, 2, 'k');
    mk_hline(f->rx + sx + 5, f->ly + 9, 2, 'k');
}

// apologetic brows (inner ends up)
static void brows_sorry(const Face *f)
{
    mk_hline(f->lx + 1, f->ly - 1, 2, 'O');
    mk_hline(f->lx + 3, f->ly - 2, 2, 'O');
    mk_hline(f->rx + 4, f->ly - 1, 2, 'O');
    mk_hline(f->rx + 2, f->ly - 2, 2, 'O');
}

// hand rows (see Face), resting on the keys or raised to about the port
#define HAND_REST 30
static int hand_port(const Face *f) { return f->hy + 11; }

bool wheel_face(const Mascot *m, Face *f)
{
    if (!active(m)) return false;
    const WheelState *w = &m->wheel;
    const float t = f->t;
    const int phase = m->drive_in.phase, my = f->my, hy = f->hy;
    const bool shut = m->blink_left > 0.0f;
    const bool talking = m->mood == MOOD_TALKING;
    const int side = w->side ? w->side : 1;
    int pdx = roundi(w->pux), pdy = roundi(w->puy);
    int *free_h = side > 0 ? &f->lh : &f->rh, *arm_h = side > 0 ? &f->rh : &f->lh;
    *free_h = *arm_h = HAND_REST;
    if (!w->out && w->reach < 0.01f && w->latched) *arm_h = hand_port(f);   // about to shoot out

    // how it ended
    if (w->result_left > 0.0f) {
        if (w->result == DRIVE_RESULT_VICTORY) {
            float u = VICTORY_LEN - w->result_left;
            eyes(f, 0, u < 0.06f ? EYE_SHUT : EYE_HAPPY, 0, 0, 0, 0);
            if (talking) mouth_talk(my, 0, t);
            else mouth_grin(my, 0);
            blush(f, 0);
            if (u >= VICTORY_HOP && u < 1.2f) {             // fists in the air
                bool up = pulse(u, 0.2f, 0.5f);
                f->lh = hy - 1 + (up ? 0 : 3);
                f->rh = hy - 1 + (up ? 3 : 0);
                if (w->out) *free_h = hy - 1;
            }
        } else {
            float u = SHEEPISH_LEN - w->result_left;
            if (shut || u < 0.08f) eyes(f, 0, EYE_SHUT, 0, 0, 0, 0);
            else eyes(f, 0, EYE_OPEN, -side, 2, 1, 0);         // cant quite look at you
            brows_sorry(f);
            if (talking) mouth_talk(my, 0, t);
            else mouth_sheepish(my, 0);
            blush(f, 0);
            *free_h = hy + 1 + (pulse(u, 0.24f, 0.5f) ? 0 : 1);                // scratching his casing
        }
        return true;
    }

    // "oh its yours!"
    if (w->pause_left > 0.0f) {
        float u = PAUSE_LEN - w->pause_left;
        if (u < 0.06f) eyes(f, 0, EYE_SHUT, 0, 0, 0, 0);
        else if (u < 0.55f) eyes_wide(f, pdx);
        else if (shut) eyes(f, 0, EYE_SHUT, 0, 0, 0, 0);
        else eyes(f, 0, EYE_OPEN, pdx, pdy, 0, 1);
        if (u < 0.55f) mouth_gasp(my, 0);
        else if (talking) mouth_talk(my, 0, t);
        else mouth_sheepish(my, 0);
        if (u >= 0.55f) {
            blush(f, 0);
            brows_sorry(f);
        }
        if (u >= 0.12f) {                                  // hands up, "all yours!"
            int j = u < 0.55f && ((int)(u / 0.06f) & 1) ? 1 : 0;
            f->lh = hy + 2 + j;
            f->rh = hy + 3 - j;
        }
        return true;
    }

    if (!driving(m)) {                                     // the arm still coming home, the hand still on the keys
        if (shut) eyes(f, 0, EYE_SHUT, 0, 0, 0, 0);
        else eyes(f, 0, EYE_OPEN, pdx, pdy, 0, 0);
        if (talking) mouth_talk(my, 0, t);
        else mouth_set(my, 0);
        return true;
    }

    // a handover (password, payment..), he points like always and anim_point has the arm and face
    if (pointing(m) && !w->out) return false;

    // a narration line while carrying, holding, waiting or fetching gets the emotions own face
    // (not while typing, clicking or hauling)
    if (talking && m->emotion > EMOTION_NONE && m->emotion < EMOTION_COUNT && !w->typing &&
        w->click_left <= 0.0f && !w->hauling &&
        (phase == DRIVE_ANIM_CARRY || phase == DRIVE_ANIM_HOLD || phase == DRIVE_ANIM_WAIT ||
         phase == DRIVE_ANIM_FETCH))
        return false;

    const bool walk = point_walking(&m->point);
    const int sx = w->out && w->tension > 0.5f && !walk ? side : 0;   // the face turns to the work

    // arms pump while he walks (only the free one once the other has the pointer)
    if (walk) {
        int fr = point_walk_frame(&m->point), d = m->point.dir ? m->point.dir : 1;
        int *lead = d > 0 ? &f->rh : &f->lh, *trail = d > 0 ? &f->lh : &f->rh;
        *lead = fr == 2 ? hy + 14 : (fr == 0 ? HAND_REST : hy + 18);
        *trail = fr == 0 ? hy + 14 : (fr == 2 ? HAND_REST : hy + 18);
    }

    // a click, "hup!"
    if (w->click_left > 0.0f && WHEEL_CLICK - w->click_left < 0.12f) {
        eyes(f, sx, EYE_OPEN, pdx, pdy, 3, 1);
        mk_fill(19 + sx, my, 2, 2, 'O');
        return true;
    }

    // straining
    if (w->hauling) {
        bool twitch = pulse(t, 0.16f, 0.5f);
        if (pulse(w->haul_t, 1.4f, 0.8f)) eyes_squeeze(f, sx);
        else eyes(f, sx, EYE_OPEN, pdx, pdy, 3, 0);          // a peek at where its going
        if (talking) mouth_talk(my, sx, t);
        else mouth_grit(my, sx, twitch);
        if (!walk) *free_h = hy + 10 + (twitch ? 1 : 0);     // clenched fist
        return true;
    }

    // typing, eyes on the keys and tongue out
    if (w->typing) {
        if (shut) eyes(f, 0, EYE_SHUT, 0, 0, 0, 0);
        else eyes(f, 0, EYE_OPEN, pdx, pdy, pdy >= 2 ? 3 : 2, 0);
        if (talking) mouth_talk(my, 0, t);
        else mouth_tongue(my, 0, free_side(w), w->strike_left > 0.0f);
        return true;
    }

    switch (phase) {
    case DRIVE_ANIM_WAIT:
        if (m->mood == MOOD_ASK) {                         // the question, eyes up at the bubble, hand up
            if (shut) eyes(f, 0, EYE_SHUT, 0, 0, 0, 0);
            else eyes(f, 0, EYE_OPEN, pulse(t, 2.0f, 0.5f) ? 0 : 1, -2, 0, 1);
            mk_fill(19, my, 2, 2, 'O');
            *free_h = hy - 1 + (pulse(t, 1.0f, 0.5f) ? 0 : 1);
        } else {                                           // watching your hand on the mouse
            if (shut) eyes(f, 0, EYE_SHUT, 0, 0, 0, 0);
            else eyes(f, 0, EYE_OPEN, pdx, pdy, 0, 0);
            if (talking) mouth_talk(my, 0, t);
            else {
                mk_put(17, my, 'O');
                mk_put(22, my, 'O');
                mk_hline(18, my + 1, 4, 'O');
                mk_hline(19, my + 1, 2, 'Y');
            }
        }
        return true;
    case DRIVE_ANIM_DONE: {                                // strolling home after the reaction
        bool good = w->result == DRIVE_RESULT_VICTORY;
        if (shut || (good && pulse(t, 1.6f, 0.3f))) eyes(f, 0, good ? EYE_HAPPY : EYE_SHUT, 0, 0, 0, 0);
        else eyes(f, 0, EYE_OPEN, walk ? 2 * m->point.dir : 0, good ? 0 : 1, good ? 0 : 1, 0);
        if (talking) mouth_talk(my, 0, t);
        else if (good) mouth_grin(my, 0);
        else mouth_sheepish(my, 0);
        return true;
    }
    case DRIVE_ANIM_FETCH: {                               // on a mission
        bool fast = sqrtf(m->point.vx * m->point.vx + m->point.vy * m->point.vy) > 1300.0f;
        if (shut) eyes(f, 0, EYE_SHUT, 0, 0, 0, 0);
        else eyes(f, 0, EYE_OPEN, pdx, pdy, fast ? 2 : 1, 0);
        if (talking) mouth_talk(my, 0, t);
        else mouth_set(my, 0);
        return true;
    }
    default: {                                             // holding or carrying it, determined
        bool grab = w->gripped && w->grip_t < 0.16f;
        if (shut && !grab) eyes(f, sx, EYE_SHUT, 0, 0, 0, 0);
        else if (grab)                                     // "got it!" eyes pop up a row
            for (int e = 0; e < 2; e++) mk_eye((e ? f->rx : f->lx) + sx, f->ly - 1, EYE_OPEN, pdx, pdy, 0, 0);
        else eyes(f, sx, EYE_OPEN, pdx, pdy, 2, 0);
        if (talking) mouth_talk(my, sx, t);
        else if (grab) mouth_grin(my, sx);
        else mouth_set(my, sx);
        if (phase == DRIVE_ANIM_HOLD && !walk && !w->typing)     // drumming his fingers
            *free_h = pulse(t, 0.5f, 0.25f) ? HAND_REST - 2 : HAND_REST;
        return true;
    }
    }
}

// ---- overlay

static const char *DROP[] = { ".q.", "qwq", "qqq", ".q." };

static void fx_stamp(int x, int y, const char *rows[], int w, int h)
{
    x = clampi(x, 1, MASCOT_W - 1 - w);
    y = clampi(y, 1, MASCOT_H - 1 - h);
    mk_stamp(x, y, rows, h, false);
}

static void fx_put(int x, int y, char c)
{
    if (x >= 1 && x < MASCOT_W - 1 && y >= 1 && y < MASCOT_H - 1) mk_put(x, y, c);
}

// sweat drop flung off his casing, up and out then falling
static void fling(const Face *f, int dir, float u)
{
    int x = (dir < 0 ? 6 : 33) + dir * roundi(u * 4.0f);
    int y = f->hy + 3 - roundi(4.0f * sinf(u * PI_F)) + roundi(u * u * 6.0f);
    fx_put(x, y, 'q');
    fx_put(x, y + 1, 'q');
    if (u < 0.6f) fx_put(x - dir, y, 'w');
}

void wheel_overlay(const Mascot *m, const Face *f)
{
    if (!active(m)) return;
    const WheelState *w = &m->wheel;

    // straining, sweat flying off one side then the other
    if (w->hauling) {
        float a = w->haul_t;
        for (int n = 0; n < 4; n++) {
            float born = 0.15f + n * 0.35f, u = (a - born) / 0.5f;
            if (u >= 0.0f && u < 1.0f) fling(f, (n & 1) ? 1 : -1, u);
        }
        a = fmodf(a - 0.15f, 1.4f);                       // keeps going on a long haul
        if (w->haul_t > 1.55f && a < 0.5f) fling(f, -1, a / 0.5f);
    }

    // "oh its yours" and the sheepish end get a nervous drop sliding down
    float u = -1.0f;
    if (w->pause_left > 0.0f) u = (PAUSE_LEN - w->pause_left - 0.5f) / 0.8f;
    if (w->result_left > 0.0f && w->result == DRIVE_RESULT_SHEEPISH)
        u = (SHEEPISH_LEN - w->result_left - 0.15f) / 0.55f;
    if (u >= 0.0f && u < 1.0f) {
        int side = w->side ? -w->side : -1;              // free hand side, same as wheel_face (left once the arm is back in)
        fx_stamp(side > 0 ? 33 : 4, f->hy - 1 + roundi(u * 5.0f), DROP, 3, 4);
    }

    // victory sparkles
    if (w->result_left > 0.0f && w->result == DRIVE_RESULT_VICTORY) {
        float a = VICTORY_LEN - w->result_left - VICTORY_HOP;
        static const int X[4] = { 3, 36, 8, 31 }, Y[4] = { 8, 6, 2, 1 };
        for (int k = 0; k < 4; k++) {
            float s = a - k * 0.12f;
            if (s < 0.0f || s > 0.45f) continue;
            int y = Y[k] > 5 ? f->hy + Y[k] - 5 : Y[k];
            if (s < 0.1f || s > 0.35f) fx_put(X[k], y + 1, 'w');
            else {
                static const char *SPARK[] = { ".w.", "wYw", ".w." };
                fx_stamp(X[k] - 1, y, SPARK, 3, 3);
            }
        }
    }
}

// ---- preview (make anim wheel)
// 3 scripted drives standing in for drive.c, desktop px. keep these numbers in step with
// tools/drive_sim.h which films them (window starts at DSIM_HOME (240, 330), fake form, ask
// bubble yes at (170, 229) and no at (250, 229) in the window). the window is 420 x 582, his
// face middle at (210, 318), a steered pointer rides 140 px out, 27 down (ARM_REACH, ARM_Y)

#define FACE_X 210.0f
#define FACE_Y 318.0f
#define SPOT_X 140.0f
#define SPOT_Y 27.0f
#define STAY -1.0f

typedef struct {
    float len;              // secs
    int phase;              // a DriveAnimPhase
    bool latched, steering;
    float haul;
    float wx, wy;           // window walks here (desktop px of its top left, STAY = it doesnt)
    float w_at, w_len;      // starting this far in, taking this long (0 = the rest of the beat)
    float px, py;           // your pointer moves here (desktop px, STAY = it doesnt or it rides
    float p_at, p_len;      // along while he steers), from p_at over p_len secs
    int clicks;             // clicks at click_at (2 = double click)
    float click_at;
    const char *text;       // typed from type_at at 15 chars a second
    float type_at;
    int result;
    int mood;               // a Mood from talk_at to talk_to (IDLE otherwise)
    float talk_at, talk_to;
    int emotion;
    bool hand_over;         // he points at (tx, ty) (desktop px) while you do it
    float tx, ty;
} Beat;

// fill the form. grab, carry to Name, click, type, tab, type, haul to Submit, ask, you click
// yes (the cable trails your hand to the bubble), grab it again, click Submit, victory, home
static const Beat GOOD[] = {
    { .len = 0.5f, .phase = DRIVE_ANIM_NONE, .wx = STAY, .px = 760, .py = 690 },
    { .len = 0.7f, .phase = DRIVE_ANIM_FETCH, .wx = 760 - SPOT_X - FACE_X, .wy = 690 - SPOT_Y - FACE_Y, .px = STAY },
    { .len = 0.35f, .phase = DRIVE_ANIM_HOLD, .latched = true, .steering = true, .wx = STAY, .px = STAY },
    { .len = 1.1f, .phase = DRIVE_ANIM_CARRY, .latched = true, .steering = true, .haul = 0.25f,
      .wx = 1000 - SPOT_X - FACE_X, .wy = 484 - SPOT_Y - FACE_Y, .px = STAY,
      .mood = MOOD_TALKING, .talk_at = 0.2f, .talk_to = 0.9f },
    { .len = 0.4f, .phase = DRIVE_ANIM_ACT, .latched = true, .steering = true, .wx = STAY, .px = STAY,
      .clicks = 1, .click_at = 0.08f },
    { .len = 1.0f, .phase = DRIVE_ANIM_TYPE, .latched = true, .steering = true, .wx = STAY, .px = STAY,
      .text = "Ada Lovelace", .type_at = 0.12f },
    { .len = 0.25f, .phase = DRIVE_ANIM_TYPE, .latched = true, .steering = true, .wx = STAY, .px = STAY,
      .text = "\t", .type_at = 0.06f },
    { .len = 1.3f, .phase = DRIVE_ANIM_TYPE, .latched = true, .steering = true, .wx = STAY, .px = STAY,
      .text = "ada@engine.org", .type_at = 0.1f },
    { .len = 0.9f, .phase = DRIVE_ANIM_HOLD, .latched = true, .steering = true, .wx = STAY, .px = STAY,
      .mood = MOOD_TALKING, .talk_at = 0.05f, .talk_to = 0.8f, .emotion = EMOTION_HAPPY },
    { .len = 1.6f, .phase = DRIVE_ANIM_CARRY, .latched = true, .steering = true, .haul = 0.9f,
      .wx = 1515 - SPOT_X - FACE_X, .wy = 816 - SPOT_Y - FACE_Y, .px = STAY },
    { .len = 0.3f, .phase = DRIVE_ANIM_HOLD, .latched = true, .steering = true, .wx = STAY, .px = STAY },
    // "Press Submit?", you take the mouse up to his bubble and click yes
    { .len = 2.4f, .phase = DRIVE_ANIM_WAIT, .latched = true, .wx = STAY,
      .px = 1165 + 170, .py = 471 + 229, .p_at = 0.5f, .p_len = 0.8f,
      .mood = MOOD_ASK, .talk_at = 0.0f, .talk_to = 2.0f },
    { .len = 0.6f, .phase = DRIVE_ANIM_FETCH, .latched = true, .wx = 1335 - SPOT_X - FACE_X, .wy = 700 - SPOT_Y - FACE_Y,
      .px = STAY },
    { .len = 0.7f, .phase = DRIVE_ANIM_CARRY, .latched = true, .steering = true, .haul = 0.3f,
      .wx = 1515 - SPOT_X - FACE_X, .wy = 816 - SPOT_Y - FACE_Y, .px = STAY },
    { .len = 0.4f, .phase = DRIVE_ANIM_ACT, .latched = true, .steering = true, .wx = STAY, .px = STAY,
      .clicks = 1, .click_at = 0.08f },
    { .len = 2.6f, .phase = DRIVE_ANIM_DONE, .result = DRIVE_RESULT_VICTORY, .wx = 240, .wy = 330,
      .w_at = WHEEL_VICTORY, .w_len = 1.2f, .px = STAY },
    { .len = 0.6f, .phase = DRIVE_ANIM_NONE, .wx = STAY, .px = STAY },
};

// you grab the mouse halfway through a carry, he lets go ("oh its yours!") and asks whether
// to carry on, you say no, sheepish, home
static const Beat OOPS[] = {
    { .len = 0.4f, .phase = DRIVE_ANIM_NONE, .wx = STAY, .px = 760, .py = 690 },
    { .len = 0.7f, .phase = DRIVE_ANIM_FETCH, .wx = 760 - SPOT_X - FACE_X, .wy = 690 - SPOT_Y - FACE_Y, .px = STAY },
    { .len = 0.35f, .phase = DRIVE_ANIM_HOLD, .latched = true, .steering = true, .wx = STAY, .px = STAY },
    { .len = 0.55f, .phase = DRIVE_ANIM_CARRY, .latched = true, .steering = true, .haul = 0.2f,
      .wx = 410 + 330, .wy = 345 - 120, .w_len = 1.1f, .px = STAY },     // cut off halfway
    { .len = 1.4f, .phase = DRIVE_ANIM_PAUSED, .wx = STAY, .px = 1010, .py = 760, .p_at = 0.0f, .p_len = 0.25f },
    { .len = 1.6f, .phase = DRIVE_ANIM_WAIT, .wx = STAY, .px = 575 + 250, .py = 285 + 229, .p_at = 0.3f, .p_len = 0.8f,
      .mood = MOOD_ASK, .talk_at = 0.0f, .talk_to = 1.4f },
    { .len = 1.8f, .phase = DRIVE_ANIM_DONE, .result = DRIVE_RESULT_SHEEPISH, .wx = 240, .wy = 330,
      .w_at = WHEEL_SHEEPISH, .w_len = 0.9f, .px = STAY },
    { .len = 0.5f, .phase = DRIVE_ANIM_NONE, .wx = STAY, .px = STAY },
};

// a password field. he carries the pointer over, lets go and points at it while you type
// it ("your turn"), then youre done, victory, home
static const Beat HANDOVER[] = {
    { .len = 0.3f, .phase = DRIVE_ANIM_NONE, .wx = STAY, .px = 760, .py = 690 },
    { .len = 0.7f, .phase = DRIVE_ANIM_FETCH, .wx = 760 - SPOT_X - FACE_X, .wy = 690 - SPOT_Y - FACE_Y, .px = STAY },
    { .len = 0.35f, .phase = DRIVE_ANIM_HOLD, .latched = true, .steering = true, .wx = STAY, .px = STAY },
    { .len = 0.9f, .phase = DRIVE_ANIM_CARRY, .latched = true, .steering = true, .haul = 0.2f,
      .wx = 1000 - SPOT_X - FACE_X, .wy = 608 - SPOT_Y - FACE_Y, .px = STAY },
    { .len = 2.2f, .phase = DRIVE_ANIM_WAIT, .wx = STAY, .px = 1080, .py = 612, .p_at = 0.7f, .p_len = 0.4f,
      .mood = MOOD_ASK, .talk_at = 0.5f, .talk_to = 2.2f, .hand_over = true, .tx = 1000, .ty = 608 },
    { .len = 2.5f, .phase = DRIVE_ANIM_DONE, .result = DRIVE_RESULT_VICTORY, .wx = 240, .wy = 330,
      .w_at = WHEEL_VICTORY, .w_len = 1.1f, .px = STAY },
    { .len = 0.5f, .phase = DRIVE_ANIM_NONE, .wx = STAY, .px = STAY },
};

#define HOME_X 240.0f
#define HOME_Y 330.0f
#define TYPE_CPS 15.0f

static float ease(float k) { k = clampf(k, 0.0f, 1.0f); return k * k * (3.0f - 2.0f * k); }
static float ease_d(float k) { return k <= 0.0f || k >= 1.0f ? 0.0f : 6.0f * k * (1.0f - k); }

static int count_text(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

// the inputs drive.c and main.c would give at time t of a story
static void play(const Beat *b, int count, Mascot *m, float t)
{
    float wx = HOME_X, wy = HOME_Y, px = 0.0f, py = 0.0f, start = 0.0f;
    int clicks = 0, keys = 0;
    unsigned last = 0;
    for (int i = 0; i < count; i++) {
        const Beat *e = &b[i];
        const bool last_beat = i == count - 1;
        const float u = t - start;
        const bool now = u < e->len || last_beat;
        // the window
        float wx0 = wx, wy0 = wy, vx = 0.0f, vy = 0.0f;
        if (e->wx != STAY) {
            float wl = e->w_len > 0.0f ? e->w_len : e->len - e->w_at;
            float k = ((now ? u : e->len) - e->w_at) / wl;      // (a walk can be cut off)
            wx = wx0 + (e->wx - wx0) * ease(k);
            wy = wy0 + (e->wy - wy0) * ease(k);
            if (now) { vx = (e->wx - wx0) * ease_d(k) / wl; vy = (e->wy - wy0) * ease_d(k) / wl; }
        }
        // the pointer rides along while he steers, or you move it
        if (e->latched && e->steering) {
            px += wx - wx0;
            py += wy - wy0;
        } else if (e->px != STAY) {
            float pl = e->p_len > 0.0f ? e->p_len : 1e-4f;
            float k = ((now ? u : e->len) - e->p_at) / pl;
            float px0 = i == 0 ? e->px : px, py0 = i == 0 ? e->py : py;
            px = px0 + (e->px - px0) * ease(k);
            py = py0 + (e->py - py0) * ease(k);
        }
        // clicks and keys so far
        if (e->clicks && (!now || u >= e->click_at)) clicks += e->clicks;
        int n = count_text(e->text);
        int typed = now ? clampi((int)floorf((u - e->type_at) * TYPE_CPS) + 1, 0, n) : n;
        if (typed > 0) last = (unsigned char)e->text[typed - 1];
        keys += typed;
        if (!now) { start += e->len; continue; }

        DriveInputs *d = &m->drive_in;
        d->phase = e->phase;
        d->t = u;
        d->latched = e->latched;
        d->steering = e->steering;
        d->haul = e->phase == DRIVE_ANIM_CARRY ? e->haul * ease_d(u / e->len) / 1.5f : 0.0f;
        d->clicks = e->phase == DRIVE_ANIM_NONE ? 0 : clicks;
        d->keys = e->phase == DRIVE_ANIM_NONE ? 0 : keys;
        d->last_key = last;
        d->result = e->phase == DRIVE_ANIM_DONE ? e->result : DRIVE_RESULT_NONE;
        if (e->phase == DRIVE_ANIM_CARRY && e->haul > 0.5f) d->haul = e->haul;   // a long one stays heavy throughout
        m->walk_vx = vx;
        m->walk_vy = vy;
        float dx = px - (wx + FACE_X), dy = py - (wy + FACE_Y);
        m->cursor_dx = dx;
        m->cursor_dy = dy;
        m->point_dx = e->phase == DRIVE_ANIM_NONE ? 0.0f : dx;
        m->point_dy = e->phase == DRIVE_ANIM_NONE ? 0.0f : dy;
        m->point_phase = e->hand_over ? POINTER_AT : POINTER_HOME;   // a handover, pointer.c points
        m->point_t = e->hand_over ? u : 0.0f;
        m->point_result = POINTER_RESULT_NONE;
        if (e->hand_over) {
            m->point_dx = e->tx - (wx + FACE_X);
            m->point_dy = e->ty - (wy + FACE_Y);
        }
        bool talk = e->mood != MOOD_IDLE && u >= e->talk_at && u < e->talk_to;
        m->mood = talk ? (Mood)e->mood : MOOD_IDLE;
        Emotion emo = talk && e->mood == MOOD_TALKING ? (Emotion)e->emotion : EMOTION_NONE;
        if (emo != m->emotion) { m->emotion = emo; m->emotion_age = 0.0f; }
        return;
    }
}

static void d_good(Mascot *m, float t) { play(GOOD, (int)(sizeof GOOD / sizeof GOOD[0]), m, t); }
static void d_oops(Mascot *m, float t) { play(OOPS, (int)(sizeof OOPS / sizeof OOPS[0]), m, t); }
static void d_hand(Mascot *m, float t) { play(HANDOVER, (int)(sizeof HANDOVER / sizeof HANDOVER[0]), m, t); }

// when beat `i` of a story starts
static float at(const Beat *b, int i)
{
    float s = 0.0f;
    for (int k = 0; k < i; k++) s += b[k].len;
    return s;
}

int wheel_preview(PreviewFrame *out, int cap)
{
    static const struct { const char *label; int story; int beat; float u; } F[] = {
        { "home", 0, 0, 0.3f },
        { "fetch", 0, 1, 0.3f },
        { "fetch: arriving", 0, 1, 0.62f },
        { "latch +0.04", 0, 2, 0.04f },
        { "grab! +0.10", 0, 2, 0.10f },
        { "grab +0.25", 0, 2, 0.25f },
        { "carry: off", 0, 3, 0.25f },
        { "carry (talking)", 0, 3, 0.55f },
        { "carry: arriving", 0, 3, 1.0f },
        { "click +0.01", 0, 4, 0.09f },
        { "click +0.05", 0, 4, 0.13f },
        { "click +0.14", 0, 4, 0.22f },
        { "type: hand out", 0, 5, 0.08f },
        { "type A", 0, 5, 0.145f },
        { "type d", 0, 5, 0.21f },
        { "type space", 0, 5, 0.345f },
        { "type v", 0, 5, 0.545f },
        { "type e", 0, 5, 0.61f },
        { "tab", 0, 6, 0.085f },
        { "type @", 0, 7, 0.325f },
        { "type .", 0, 7, 0.79f },
        { "hold: \"got it!\" (happy)", 0, 8, 0.4f },
        { "hold", 0, 8, 0.85f },
        { "haul: heave", 0, 9, 0.12f },
        { "haul", 0, 9, 0.5f },
        { "haul", 0, 9, 0.8f },
        { "haul: almost", 0, 9, 1.35f },
        { "wait: ask", 0, 11, 0.3f },
        { "wait: you take it", 0, 11, 0.8f },
        { "wait: trailing", 0, 11, 1.1f },
        { "wait: on yes", 0, 11, 1.6f },
        { "fetch it again", 0, 12, 0.35f },
        { "carry back", 0, 13, 0.4f },
        { "click Submit", 0, 14, 0.1f },
        { "done: let go", 0, 15, 0.06f },
        { "victory: crouch", 0, 15, 0.13f },
        { "victory: hop", 0, 15, 0.3f },
        { "victory: land", 0, 15, 0.54f },
        { "victory: hop 2", 0, 15, 0.7f },
        { "victory: cheer", 0, 15, 1.1f },
        { "walking home", 0, 15, 1.8f },
        { "oops: carrying", 1, 3, 0.4f },
        { "grabbed +0.03", 1, 4, 0.03f },
        { "grabbed +0.15", 1, 4, 0.15f },
        { "grabbed +0.4", 1, 4, 0.4f },
        { "it's yours +0.9", 1, 4, 0.9f },
        { "carry on?", 1, 5, 0.9f },
        { "sheepish +0.3", 1, 6, 0.3f },
        { "sheepish: walking home", 1, 6, 1.1f },
        { "sheepish: home", 1, 6, 1.75f },
        { "handover: carried", 2, 3, 0.85f },
        { "handover: let go", 2, 4, 0.08f },
        { "handover: he points", 2, 4, 0.6f },
        { "handover: your turn", 2, 4, 1.5f },
        { "handover: done", 2, 5, 0.3f },
    };
    int n = 0;
    for (int i = 0; i < (int)(sizeof F / sizeof F[0]) && n < cap; i++) {
        static const Beat *const STORY[3] = { GOOD, OOPS, HANDOVER };
        static void (*const DRIVE[3])(Mascot *m, float t) = { d_good, d_oops, d_hand };
        const Beat *b = STORY[F[i].story];
        out[n++] = (PreviewFrame){ F[i].label, at(b, F[i].beat) + F[i].u, DRIVE[F[i].story] };
    }
    return n;
}
