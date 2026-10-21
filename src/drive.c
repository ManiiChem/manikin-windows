#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "drive.h"
#include "platform.h"

// see drive.h. every input goes out through the "hands" helpers below so it gets timestamped
// for drive_injected_within, and anything held down can always be let go

#define GRAB_SLOP 3                  // px the pointer can drift off his plug before its the users
#define PRESS_QUIET 0.15f            // a press this soon (secs) after his own button is his
#define ESC_QUIET 0.15f              // same for an Esc right after he pressed "escape"
#define FETCH_SLOP 24                // px it can wander while he walks over before he re-aims
#define SPOT_W 40.0f                 // px around a spot kept clear of him ("drive" has no box)
#define SPOT_H 24.0f
#define SETTLE 0.06f                 // wait at the spot before pressing so the app sees the hover
#define CLICK_HOLD 0.05f             // how long each click is held down
#define CLICK_GAP 0.06f              // between double click clicks (windows allows 500 ms)
#define DRAG_GRIP 0.1f               // pause after pressing so the app sees a drag
#define DRAG_SLOW 1.3f               // drags get hauled this much slower than carries
#define SCROLL_EVERY 0.06f           // between wheel notches
#define SCROLL_MAX 100               // max notches either way
#define KEY_EVERY 0.08f              // between key repeats
#define TYPE_CPS 15.0f               // chars a second, for the show
#define TYPE_MAX_S 4.0f              // but no text takes longer than this
#define HAUL_FROM 300.0f             // px, carries longer than this start counting as a haul
#define HAUL_FULL 1200.0f            // full strength haul from here
#define LINGER_VICTORY 1.2f          // he celebrates (or looks sheepish) this long before walking home
#define LINGER_SHEEPISH 0.7f
#define TEXT_MAX BRAIN_TEXT_MAX      // codepoints, never more than the bytes they came in

static PointerLayout lay;
static long long own_win;
static double now;
static double inject_t = -1e9;       // when he last sent any input
static double button_t = -1e9;       // last mouse button he sent
static double escape_t = -1e9;       // last time he pressed "escape"

// what the animation sees (DriveInputs)
static DriveAnimPhase phase;
static float phase_t;
static bool latched, steering;
static float haul;
static int clicks, keys;
static unsigned last_key;
static DriveResult result;

static float win_x, win_y;           // the window as of the last update
static float home_x, home_y;
static bool home_set;
static PointerTrip trip;
static bool trip_on;                 // false = plan it from here the next frame he can move
static float pin_from_x, pin_from_y, pin_to_x, pin_to_y;   // the pointers own trip (CARRY or a drag)
static Vector2 vel;
static int pin_x, pin_y;             // where he put the pointer while steering
static int cur_x, cur_y;             // the pointer as of the last look
static int fetch_x, fetch_y;         // where it was when he set off to fetch it

// the action in progress, still owes one drive_done for act_id
static bool busy;
static int act_id;
static BrainDrive act;
static unsigned text[TEXT_MAX];
static int text_n;
static float type_every;             // secs per char
static int step;                     // next input (ACT / TYPE)
static float step_wait;              // secs until it
static bool hauling;                 // a drag, walking it over with the button down
static bool button_down;             // holding a mouse button (act.right says which)
static int mods_down;                // act.keys[0..mods_down) are held (CLICK / DRAG)

static bool grab_pending;            // grabbed between actions, the next action answers "grabbed"
static bool refusing;                // after Esc the stopped drives late actions get "cancelled"
static bool esc_ended;               // Esc ended it, sheepish no matter what drive_end says
static bool esc_note;                // for DriveFrame.cancelled
static float linger;                 // DONE, time left before he walks home
static bool chat_saved, chat_was_open;

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void set_phase(DriveAnimPhase p)
{
    phase = p;
    phase_t = 0;
}

static void look(void)
{
    platform_get_cursor_screen(&cur_x, &cur_y);
}

static void answer(int id, const char *outcome)
{
    look();
    brain_send_drive_done(id, outcome, cur_x, cur_y);
}

// the action in progress is over, tell the brain (once)
static void finish(const char *outcome)
{
    if (!busy) return;
    busy = hauling = false;
    haul = 0;
    answer(act_id, outcome);
}

// ---- his hands

static bool move_pointer(float x, float y)
{
    inject_t = now;
    bool ok = platform_set_cursor((int)lroundf(x), (int)lroundf(y));
    look();                          // windows keeps it on screen so pin it where it actually went
    pin_x = cur_x;
    pin_y = cur_y;
    return ok;
}

static bool press(bool down)
{
    inject_t = button_t = now;
    bool ok = platform_mouse_button(act.right, down);
    button_down = down && ok;
    return ok;
}

static bool mods_press(void)
{
    for (; mods_down < act.nkeys; mods_down++) {
        inject_t = now;
        if (!platform_key(act.keys[mods_down], true)) return false;
    }
    return true;
}

// let go of whatevers held down, last modifier first. false if windows refused
static bool release_held(void)
{
    bool ok = true;
    if (button_down) ok = press(false);
    while (mods_down > 0) {
        inject_t = now;
        ok = platform_key(act.keys[--mods_down], false) && ok;
    }
    return ok;
}

static void let_go(void)
{
    release_held();
    latched = steering = hauling = false;
    haul = 0;
}

// windows refused an input, let go of everything and report it. lost_grip = it wouldnt
// even move the pointer so hes not holding it anymore
static void fail(bool lost_grip)
{
    release_held();
    if (lost_grip) latched = steering = false;
    trip_on = false;
    finish("failed");
    set_phase(DRIVE_ANIM_HOLD);
}

// the user took the pointer back ("oh its yours!")
static void grabbed(void)
{
    let_go();
    trip_on = false;
    if (busy) finish("grabbed");
    else grab_pending = true;        // between actions, the next one gets answered for it
    set_phase(DRIVE_ANIM_PAUSED);
}

static void enter_done(DriveResult r)
{
    let_go();
    finish("cancelled");             // if an action is still going
    trip_on = false;
    grab_pending = false;
    result = r;
    linger = r == DRIVE_RESULT_VICTORY ? LINGER_VICTORY : LINGER_SHEEPISH;
    set_phase(DRIVE_ANIM_DONE);
}

static void stop_driving(void)
{
    set_phase(DRIVE_ANIM_NONE);
    latched = steering = hauling = false;
    haul = 0;
    clicks = keys = 0;
    last_key = 0;
    result = DRIVE_RESULT_NONE;
    home_set = trip_on = esc_ended = false;
    vel = (Vector2){ 0 };
}

void drive_init(const PointerLayout *layout, long long own_window, int wx, int wy)
{
    lay = *layout;
    own_win = own_window;
    win_x = (float)wx;
    win_y = (float)wy;
    inject_t = button_t = escape_t = now - 1e9;
    busy = grab_pending = refusing = esc_note = chat_saved = false;
    button_down = false;
    mods_down = 0;
    stop_driving();
}

// ---- getting there

// window position that puts desktop (x, y) at his arm, clear of his UI
static void place(int x, int y, float *wx, float *wy)
{
    int ax, ay, aw, ah, px, py;
    platform_get_work_area_at(x, y, &ax, &ay, &aw, &ah);
    Rectangle spot = { x - SPOT_W / 2, y - SPOT_H / 2, SPOT_W, SPOT_H };
    pointer_place(&lay, ax, ay, aw, ah, (Vector2){ (float)x, (float)y }, spot,
                  (int)lroundf(win_x), (int)lroundf(win_y), &px, &py);
    *wx = (float)px;
    *wy = (float)py;
}

// plan a window trip. with near, if its already under 2 px away the trip ends right away
static void plan_trip(float x, float y, bool near)
{
    trip = pointer_trip(win_x, win_y, x, y, 0);
    if (near && fabsf(x - win_x) < 2 && fabsf(y - win_y) < 2) trip.t = trip.dur;
    trip_on = true;
}

// walk the pointer to (x, y) with the window along, timed by whichever goes further
static void plan_carry(int x, int y, bool drag)
{
    float wx, wy;
    place(x, y, &wx, &wy);
    pin_from_x = (float)pin_x;
    pin_from_y = (float)pin_y;
    pin_to_x = (float)x;
    pin_to_y = (float)y;
    float d = hypotf(pin_to_x - pin_from_x, pin_to_y - pin_from_y);
    trip = pointer_trip(win_x, win_y, wx, wy, fmaxf(d, hypotf(wx - win_x, wy - win_y)));
    if (drag) trip.dur *= DRAG_SLOW;
    trip_on = true;
    float h = clampf((d - HAUL_FROM) / (HAUL_FULL - HAUL_FROM), 0, 1);
    haul = drag ? 0.4f + 0.6f * h : h;
}

// one frame of the trip, dragging the pointer along on his plug if carrying.
// returns 1 once there, 0 on the way, -1 if windows wouldnt move the pointer
static int walk(float dt, DriveFrame *f, bool carrying)
{
    trip.t += dt;
    float e = pointer_trip_at(&trip, &win_x, &win_y, &vel);
    f->win_x = (int)lroundf(win_x);
    f->win_y = (int)lroundf(win_y);
    f->move = true;
    if (carrying && !move_pointer(pin_from_x + (pin_to_x - pin_from_x) * e,
                                  pin_from_y + (pin_to_y - pin_from_y) * e))
        return -1;
    return pointer_trip_done(&trip) ? 1 : 0;
}

// ---- the actions

enum { STEP_MORE, STEP_DONE, STEP_FAIL };

static const char *const ENTER[] = { "enter" };
static const char *const TAB[] = { "tab" };

// CLICK goes settle, mods down, (down hold up gap) per click, mods up
static int click_step(int k, float *wait)
{
    int n = act.count;
    if (k == 0) { *wait = SETTLE; return STEP_MORE; }
    if (k == 1) return mods_press() ? STEP_MORE : STEP_FAIL;
    k -= 2;
    if (k < 2 * n) {
        bool down = k % 2 == 0;
        if (!press(down)) return STEP_FAIL;
        if (down) clicks++;
        *wait = down ? CLICK_HOLD : k / 2 + 1 < n ? CLICK_GAP : 0;
        return STEP_MORE;
    }
    return release_held() ? STEP_DONE : STEP_FAIL;
}

// DRAG goes settle, mods down, press, short wait, haul (drive_update walks it), settle, let go
static int drag_step(int k, float *wait)
{
    switch (k) {
    case 0: *wait = SETTLE; return STEP_MORE;
    case 1: return mods_press() ? STEP_MORE : STEP_FAIL;
    case 2:
        if (!press(true)) return STEP_FAIL;
        clicks++;
        *wait = DRAG_GRIP;
        return STEP_MORE;
    case 3: hauling = true; trip_on = false; return STEP_MORE;
    case 4: *wait = SETTLE; return STEP_MORE;
    default: return release_held() ? STEP_DONE : STEP_FAIL;
    }
}

// SCROLL settles then does a notch at a time, up/down first then sideways
static int scroll_step(int k, float *wait)
{
    int ny = abs(act.dy) < SCROLL_MAX ? abs(act.dy) : SCROLL_MAX;
    int nx = abs(act.dx) < SCROLL_MAX ? abs(act.dx) : SCROLL_MAX;
    if (k == 0) { *wait = SETTLE; return STEP_MORE; }
    if (--k >= nx + ny) return STEP_DONE;
    inject_t = now;
    bool ok = k < ny ? platform_scroll(0, act.dy > 0 ? 1 : -1) : platform_scroll(act.dx > 0 ? 1 : -1, 0);
    *wait = k + 1 < nx + ny ? SCROLL_EVERY : 0;
    return ok ? STEP_MORE : STEP_FAIL;
}

// TYPE, a char at a time. newline and tab go out as Enter/Tab, most apps ignore them as chars
static int type_step(int k, float *wait)
{
    if (k >= text_n) return STEP_DONE;
    unsigned c = text[k];
    inject_t = now;
    bool ok = c == '\n' ? platform_keys(ENTER, 1) : c == '\t' ? platform_keys(TAB, 1)
                                                               : platform_type_codepoint(c);
    if (!ok) return STEP_FAIL;
    keys++;
    last_key = c;
    *wait = k + 1 < text_n ? type_every : 0;
    return STEP_MORE;
}

// the char a key name stands for on his keyboard base (anim_wheel.c lights that key)
static unsigned key_char(const char *name)
{
    static const struct { const char *name; unsigned c; } KEYS[] = {
        { "enter", '\n' }, { "space", ' ' }, { "tab", '\t' }, { "backspace", '\b' }, { "escape", 27 },
    };
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
        if (strcmp(name, KEYS[i].name) == 0) return KEYS[i].c;
    return (unsigned char)name[0];
}

// KEY, the combo `repeat` times
static int key_step(int k, float *wait)
{
    if (k >= act.repeat) return STEP_DONE;
    const char *names[BRAIN_DRIVE_KEYS_MAX];
    for (int i = 0; i < act.nkeys; i++) {
        names[i] = act.keys[i];
        if (strcmp(names[i], "escape") == 0) escape_t = now;   // so the poll knows the next Esc is his
    }
    inject_t = now;
    if (!platform_keys(names, act.nkeys)) return STEP_FAIL;
    keys++;
    last_key = key_char(act.keys[act.nkeys - 1]);
    *wait = k + 1 < act.repeat ? KEY_EVERY : 0;
    return STEP_MORE;
}

static int act_step(int k, float *wait)
{
    switch (act.action) {
    case BRAIN_DRIVE_CLICK:  return click_step(k, wait);
    case BRAIN_DRIVE_DRAG:   return drag_step(k, wait);
    case BRAIN_DRIVE_SCROLL: return scroll_step(k, wait);
    case BRAIN_DRIVE_TYPE:   return type_step(k, wait);
    case BRAIN_DRIVE_KEY:    return key_step(k, wait);
    default:                 return STEP_DONE;
    }
}

// inputs in order. typing catches up after a slow frame but a clicks hold never gets cut short
static void run_steps(float dt)
{
    step_wait -= dt;
    while (busy && !hauling && step_wait <= 1e-4f) {
        float wait = 0;
        int r = act_step(step++, &wait);
        if (r == STEP_FAIL) { fail(false); return; }
        if (r == STEP_DONE) { finish("done"); set_phase(DRIVE_ANIM_HOLD); return; }
        if (phase != DRIVE_ANIM_TYPE && step_wait < 0) step_wait = 0;
        step_wait += wait;
    }
}

static void arrive(void)
{
    trip_on = false;
    haul = 0;
    step = 0;
    step_wait = 0;
    if (act.action == BRAIN_DRIVE_MOVE) {
        finish("done");
        set_phase(DRIVE_ANIM_HOLD);
    } else {
        set_phase(DRIVE_ANIM_ACT);
    }
}

// hes got hold of the pointer, start the action
static void begin_action(void)
{
    step = 0;
    step_wait = 0;
    trip_on = false;
    if (act.action == BRAIN_DRIVE_TYPE || act.action == BRAIN_DRIVE_KEY)
        set_phase(DRIVE_ANIM_TYPE);  // no spot, he types where he stands
    else if (abs(act.x - pin_x) <= 1 && abs(act.y - pin_y) <= 1)
        arrive();
    else
        set_phase(DRIVE_ANIM_CARRY);
}

// utf-8 to codepoints. broken sequences get skipped so we never type junk into someones form.
// \r\n and \r become \n, other control chars get dropped (U+0008 would act as a backspace)
static int decode(const char *s, unsigned *out, int cap)
{
    static const unsigned LEAST[] = { 0, 0, 0x80, 0x800, 0x10000 };   // shortest form per length
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (*p && n < cap) {
        unsigned c = *p, cp;
        int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!len) { p++; continue; }
        cp = len == 1 ? c : c & (0x7F >> len);
        int i = 1;
        for (; i < len && (p[i] & 0xC0) == 0x80; i++) cp = cp << 6 | (p[i] & 0x3F);
        p += i;
        if (i < len || cp < LEAST[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) continue;
        if (cp == '\r') {
            if (*p == '\n') continue;    // the '\n' comes next
            cp = '\n';
        }
        if ((cp < 0x20 && cp != '\n' && cp != '\t') || cp == 0x7F) continue;
        out[n++] = cp;
    }
    return n;
}

// can the action run at all? needs a known "do", keys platform_key_known() knows, and no
// typing into his own window (the brain should click into the target first)
static bool runnable(const BrainDrive *d)
{
    if (d->action == BRAIN_DRIVE_NONE) return false;
    if (d->action == BRAIN_DRIVE_KEY && d->nkeys == 0) return false;
    if (d->action == BRAIN_DRIVE_KEY || d->action == BRAIN_DRIVE_CLICK || d->action == BRAIN_DRIVE_DRAG)
        for (int i = 0; i < d->nkeys; i++)
            if (!platform_key_known(d->keys[i])) return false;
    bool keyboard = d->action == BRAIN_DRIVE_TYPE || d->action == BRAIN_DRIVE_KEY;
    return !(keyboard && own_win && platform_foreground_window() == own_win);
}

void drive_action(const BrainEvent *ev)
{
    if (busy) { answer(ev->id, "failed"); return; }          // one at a time
    if (refusing) { answer(ev->id, "cancelled"); return; }    // Esc stopped this drive
    if (phase == DRIVE_ANIM_NONE || phase == DRIVE_ANIM_DONE) {
        // a drive starts (or a new one while hes walking home, home stays where it was)
        if (phase == DRIVE_ANIM_NONE) home_set = false;
        result = DRIVE_RESULT_NONE;
        esc_ended = false;
        trip_on = false;
        set_phase(DRIVE_ANIM_HOLD);
    }
    if (pointer_pointing()) pointer_clear_active();           // a handover the brain moved on from
    if (grab_pending) {
        grab_pending = false;
        answer(ev->id, "grabbed");
        return;
    }
    if (!runnable(&ev->drive)) { answer(ev->id, "failed"); return; }

    act = ev->drive;
    if (act.action == BRAIN_DRIVE_TYPE) {
        text_n = decode(ev->text, text, TEXT_MAX);
        if (text_n == 0) { answer(ev->id, "done"); return; }
        type_every = 1.0f / fmaxf(TYPE_CPS, text_n / TYPE_MAX_S);
    }
    busy = true;
    act_id = ev->id;
    hauling = false;
    if (steering) {
        begin_action();
    } else {                         // pointers loose, go get it first
        trip_on = false;
        set_phase(DRIVE_ANIM_FETCH);
    }
}

void drive_end(const char *r)
{
    bool victory = r && strcmp(r, "done") == 0;
    grab_pending = refusing = false;
    if (phase == DRIVE_ANIM_NONE) return;            // never started (the plan was declined)
    if (phase == DRIVE_ANIM_DONE) {                  // already letting go (the turn ended first)
        if (!esc_ended) result = victory ? DRIVE_RESULT_VICTORY : DRIVE_RESULT_SHEEPISH;
        return;
    }
    enter_done(victory ? DRIVE_RESULT_VICTORY : DRIVE_RESULT_SHEEPISH);
}

void drive_cancel(void)
{
    if (!drive_driving()) return;
    enter_done(DRIVE_RESULT_SHEEPISH);  // the action in progress gets "cancelled" first
    brain_send_cancel();                // then the cancel
    refusing = esc_ended = esc_note = true;
}

void drive_turn_over(void)
{
    refusing = grab_pending = false;
    if (drive_driving()) enter_done(DRIVE_RESULT_SHEEPISH);
}

bool drive_driving(void)  { return phase != DRIVE_ANIM_NONE && phase != DRIVE_ANIM_DONE; }
bool drive_away(void)     { return phase != DRIVE_ANIM_NONE; }
bool drive_steering(void) { return steering; }

bool drive_injected_within(float seconds)
{
    return now - inject_t < seconds;
}

// ---- every frame

static void fetch(float dt, DriveFrame *f)
{
    if (!trip_on) {                  // set off for wherever the pointer is now
        float wx, wy;
        fetch_x = cur_x;
        fetch_y = cur_y;
        place(cur_x, cur_y, &wx, &wy);
        plan_trip(wx, wy, true);
    }
    if (walk(dt, f, false) == 0) return;
    trip_on = false;
    if (abs(cur_x - fetch_x) > FETCH_SLOP || abs(cur_y - fetch_y) > FETCH_SLOP) return;   // moved, go after it
    latched = steering = true;       // clunk
    pin_x = cur_x;
    pin_y = cur_y;
    begin_action();
}

static void carry(float dt, DriveFrame *f, int x, int y, bool drag)
{
    if (!trip_on) plan_carry(x, y, drag);
    int r = walk(dt, f, true);
    if (r < 0) {
        fail(true);
    } else if (r > 0) {
        trip_on = false;
        haul = 0;
        if (drag) hauling = false;   // the drags steps carry on from here
        else arrive();
    }
}

static void walk_home(float dt, DriveFrame *f)
{
    if (linger > 0) {
        linger -= dt;
        return;
    }
    if (!trip_on) plan_trip(home_x, home_y, true);
    if (walk(dt, f, false) > 0) stop_driving();
}

void drive_update(float dt, DriveFrame *f)
{
    now += dt;
    phase_t += dt;
    f->move = false;
    vel = (Vector2){ 0 };
    win_x = (float)f->win_x;
    win_y = (float)f->win_y;
    // polled every frame even when not driving, so an Esc from before a drive never counts
    bool esc = platform_key_pressed(PLATFORM_KEY_ESC) && now - escape_t >= ESC_QUIET;
    look();

    if (phase != DRIVE_ANIM_NONE) {
        // the window is his to move unless the user has hold of him or hes pointing
        bool can_move = !f->pointer_away && !f->held && !f->dragging;
        if (!home_set && !f->pointer_away) {
            home_x = win_x;
            home_y = win_y;
            home_set = true;
        }
        if (f->dragging) {           // he stays where hes dropped
            home_x = win_x;
            home_y = win_y;
        }
        if (esc) drive_cancel();

        // while he steers the pointer is exactly where he put it, if it isnt the user has it
        if (steering) {
            bool moved = abs(cur_x - pin_x) > GRAB_SLOP || abs(cur_y - pin_y) > GRAB_SLOP;
            bool clicked = f->pressed && now - button_t >= PRESS_QUIET;
            if (moved || clicked) grabbed();
        }
        // between actions an ask or a handover is the users turn. he stops steering so they
        // can reach his bubble, and lets go completely if his arms off pointing
        if (!busy && drive_driving() && (f->asking || f->pointer_away)) {
            if (phase != DRIVE_ANIM_WAIT) set_phase(DRIVE_ANIM_WAIT);
            steering = false;
            if (f->pointer_away) latched = false;
        }

        if (!can_move) trip_on = false;  // re-planned from wherever he ends up
        switch (phase) {
        case DRIVE_ANIM_FETCH:
            if (can_move) fetch(dt, f);
            break;
        case DRIVE_ANIM_CARRY:
            if (can_move) carry(dt, f, act.x, act.y, false);
            break;
        case DRIVE_ANIM_ACT:
            if (!hauling) run_steps(dt);
            else if (can_move) carry(dt, f, act.to_x, act.to_y, true);
            break;
        case DRIVE_ANIM_TYPE:
            if (own_win && platform_foreground_window() == own_win) fail(false);   // never type into himself
            else run_steps(dt);
            break;
        case DRIVE_ANIM_DONE:
            if (f->dragging) stop_driving();
            else if (can_move || linger > 0) walk_home(dt, f);
            break;
        default:
            break;
        }
    }

    // shut the chat box for the drive (keys would go to him), reopen it once hes home if it was open
    if (phase != DRIVE_ANIM_NONE) {
        if (!chat_saved) {
            chat_saved = true;
            chat_was_open = f->chat_open;
            f->chat_open = false;
        }
    } else if (chat_saved) {
        chat_saved = false;
        if (chat_was_open) f->chat_open = true;
    }
    f->cancelled = esc_note;
    esc_note = false;
}

DriveView drive_view(void)
{
    DriveView v = { 0 };
    if (phase == DRIVE_ANIM_NONE) return v;
    v.in.phase = phase;
    v.in.t = phase_t;
    v.in.latched = latched;
    v.in.steering = steering;
    v.in.haul = haul;
    v.in.clicks = clicks;
    v.in.keys = keys;
    v.in.last_key = last_key;
    v.in.result = result;
    v.plug_x = steering ? pin_x : cur_x;
    v.plug_y = steering ? pin_y : cur_y;
    v.vel = vel;
    return v;
}
