#include <math.h>
#include <stdlib.h>

#include "pointer.h"
#include "brain.h"
#include "platform.h"

// pointing at things, where to stand, the walk there and back, and what ends it.
// no raylib calls (just its types) so tools/pointer_test.c can drive it with a fake clock
// and a fake desktop

// ---- placement

#define PLACE_STEP 4                 // window px between candidate spots on the first pass
#define SOLID_GAP 4                  // never closer than this to his solid UI
#define MARGIN 24                    // room the plug and the ring would like around the target
#define BOX_REACH 160                // how much of a big box around the point we try to keep clear
#define ARM_ROOM 30                  // px beside him the plug needs between his body and the ring

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

// distance from (x, y) to the rect, 0 if its inside
static float rect_dist(Rectangle r, float x, float y)
{
    float dx = fmaxf(fmaxf(r.x - x, x - (r.x + r.width)), 0);
    float dy = fmaxf(fmaxf(r.y - y, y - (r.y + r.height)), 0);
    return sqrtf(dx * dx + dy * dy);
}

static float overlap_area(Rectangle a, Rectangle b)
{
    float w = fminf(a.x + a.width, b.x + b.width) - fmaxf(a.x, b.x);
    float h = fminf(a.y + a.height, b.y + b.height) - fmaxf(a.y, b.y);
    return w > 0 && h > 0 ? w * h : 0;
}

// how far the window at (x, y) sticks out of the work area, both axes added up
static float overshoot(const PointerLayout *l, int wa_x, int wa_y, int wa_w, int wa_h, float x, float y)
{
    float o = 0;
    o += fmaxf(wa_x - x, 0) + fmaxf(x + l->win_w - (wa_x + wa_w), 0);
    o += fmaxf(wa_y - y, 0) + fmaxf(y + l->win_h - (wa_y + wa_h), 0);
    return o;
}

typedef struct {
    const PointerLayout *l;
    int wa_x, wa_y, wa_w, wa_h;
    Vector2 target;
    Rectangle around;                // desktop px, the part of the control around the point
    Rectangle box;                   // desktop px, all of it
    int from_x, from_y;
} Placing;

// cost of putting the target at window px (px, py), INFINITY if thats on his UI.
// heaviest first, the window stays in the work area, the target gets MARGIN px of air, the
// control isnt hidden under him, its around arm height on one side, and the walk is short
static float place_cost(const Placing *pl, int px, int py)
{
    const PointerLayout *l = pl->l;
    float ui = INFINITY;
    for (int i = 0; i < l->solid_count; i++) ui = fminf(ui, rect_dist(l->solid[i], (float)px, (float)py));
    if (ui < SOLID_GAP) return INFINITY;     // his UI is a hard no, the window edge just costs more
    float clear = fminf(ui, fminf(fminf((float)px, (float)(l->win_w - 1 - px)),
                                  fminf((float)py, (float)(l->win_h - 1 - py))));
    float wx = pl->target.x - px, wy = pl->target.y - py;
    float cost = 1000.0f * overshoot(l, pl->wa_x, pl->wa_y, pl->wa_w, pl->wa_h, wx, wy);
    cost += 40.0f * fmaxf(MARGIN - clear, 0);

    // standing on the control is way worse than letting it run off the windows edge,
    // off the edge you can still see and click it and only the ring gets cut short
    Rectangle k = { pl->around.x - wx, pl->around.y - wy, pl->around.width, pl->around.height };   // window px
    float off = k.width * k.height - overlap_area(k, (Rectangle){ 0, 0, (float)l->win_w, (float)l->win_h });
    // his body (solid[0]) counts a bit wider, the plug needs room between him and the ring
    float under = 0;
    for (int i = 0; i < l->solid_count; i++) {
        Rectangle r = l->solid[i];
        if (i == 0) { r.x -= ARM_ROOM; r.width += 2 * ARM_ROOM; }
        under += overlap_area(k, r);
    }
    cost += 2.0f * sqrtf(fmaxf(off, 0)) + 30.0f * sqrtf(under);
    // the rest of a big control can go under him, but if the other side is free hed rather
    // stand there (not on a dropdowns arrow say)
    Rectangle whole = { pl->box.x - wx, pl->box.y - wy, pl->box.width, pl->box.height };
    cost += 6.0f * sqrtf(overlap_area(whole, l->solid[0]));

    float dl = hypotf(px - l->spot_left.x, py - l->spot_left.y);
    float dr = hypotf(px - l->spot_right.x, py - l->spot_right.y);
    cost += fminf(dl, dr);
    cost += 0.05f * hypotf(wx - pl->from_x, wy - pl->from_y);
    return cost;
}

// coarse grid, every PLACE_STEP px and always the last one (the windows edge)
static int next_step(int v, int n)
{
    return v == n - 1 ? n : v + PLACE_STEP < n ? v + PLACE_STEP : n - 1;
}

// try spots all over the window for the target (every few px, then every px around the best)
// and keep the cheapest. it never goes under his solid UI, so near the top of the screen where
// the bubble band spans the window it ends up in the strip beside the bubble
void pointer_place(const PointerLayout *l, int wa_x, int wa_y, int wa_w, int wa_h,
                   Vector2 target, Rectangle box, int from_x, int from_y, int *x, int *y)
{
    Placing pl = { l, wa_x, wa_y, wa_w, wa_h, target, { 0 }, box, from_x, from_y };
    // what has to stay in view. a big control only gets a patch around the point marked
    // (see POINTER_BIG_BOX), anything else the part within BOX_REACH of it
    bool big = box.width > POINTER_BIG_BOX || box.height > POINTER_BIG_BOX;
    float reach_x = big ? POINTER_FOCUS_W / 2 + SOLID_GAP : BOX_REACH;
    float reach_y = big ? POINTER_FOCUS_H / 2 + SOLID_GAP : BOX_REACH;
    pl.around.x = fmaxf(box.x, target.x - reach_x);
    pl.around.y = fmaxf(box.y, target.y - reach_y);
    pl.around.width = fminf(box.x + box.width, target.x + reach_x) - pl.around.x;
    pl.around.height = fminf(box.y + box.height, target.y + reach_y) - pl.around.y;
    if (pl.around.width <= 0 || pl.around.height <= 0) pl.around = (Rectangle){ target.x, target.y, 0, 0 };

    float best = INFINITY;
    int bx = (int)l->spot_right.x, by = (int)l->spot_right.y;
    for (int py = 0; py < l->win_h; py = next_step(py, l->win_h)) {
        for (int px = 0; px < l->win_w; px = next_step(px, l->win_w)) {
            float c = place_cost(&pl, px, py);
            if (c < best) { best = c; bx = px; by = py; }
        }
    }
    int cx = bx, cy = by;                    // then every px around the best
    for (int py = cy - PLACE_STEP + 1; py < cy + PLACE_STEP; py++) {
        for (int px = cx - PLACE_STEP + 1; px < cx + PLACE_STEP; px++) {
            if (px < 0 || py < 0 || px >= l->win_w || py >= l->win_h) continue;
            float c = place_cost(&pl, px, py);
            if (c < best) { best = c; bx = px; by = py; }
        }
    }
    *x = (int)lroundf(target.x) - bx;
    *y = (int)lroundf(target.y) - by;
}

// ---- the walk

#define TRIP_MIN 0.45f               // shortest a trip takes (secs)
#define TRIP_MAX 1.1f                // longest
#define TRIP_BASE 0.3f
#define TRIP_SPEED 1600.0f           // px/s on top of the base
#define CHECK_EVERY 0.15f            // how often he checks the target window
#define WINDOW_SLOP 4                // px it can shift before it counts as moved
#define CLICK_SLOP 6                 // px around the box that still counts as inside
#define LINGER_HIT 0.5f              // he celebrates a hit this long before walking home
#define LINGER_OTHER 0.25f

static PointerLayout lay;
static long long own_win;
static float cur_x, cur_y;           // the window as of the last update

static PointerPhase phase = POINTER_HOME;
static float phase_t;
static PointerResult result;
static float result_t;

static int id;
static bool resolved;                // point_done already went out for `id`
static Vector2 target;
static Rectangle box;
static long long win, win_root;      // 0 = unknown, not watched
static int win_r[4];                 // its bounds when the point came in
static bool win_shown;
static long long fg_base;            // the foreground window when he arrived
static float check_in;
static float linger;                 // resolved in AT, secs before he heads home

static float home_x, home_y;
static PointerTrip trip;
static Vector2 vel;
static bool chat_saved, chat_was_open;

static const char *const OUTCOME[] = {
    [POINTER_RESULT_CLICKED_IN] = "clicked", [POINTER_RESULT_CLICKED_OUT] = "clicked",
    [POINTER_RESULT_MOVED] = "moved", [POINTER_RESULT_DISMISSED] = "dismissed",
    [POINTER_RESULT_CLEARED] = "cleared",
};

void pointer_init(const PointerLayout *layout, long long own_window, int win_x, int win_y)
{
    lay = *layout;
    own_win = own_window;
    cur_x = home_x = (float)win_x;
    cur_y = home_y = (float)win_y;
    phase = POINTER_HOME;
    phase_t = result_t = 0;
    result = POINTER_RESULT_NONE;
    resolved = true;                     // nothing in progress
    vel = (Vector2){ 0 };
    chat_saved = false;
}

static void set_phase(PointerPhase p)
{
    phase = p;
    phase_t = 0;
}

PointerTrip pointer_trip(float from_x, float from_y, float to_x, float to_y, float dist)
{
    PointerTrip t = { from_x, from_y, to_x, to_y, 0, 0 };
    if (dist <= 0) dist = hypotf(to_x - from_x, to_y - from_y);
    t.dur = clampf(TRIP_BASE + dist / TRIP_SPEED, TRIP_MIN, TRIP_MAX);
    return t;
}

float pointer_trip_at(const PointerTrip *t, float *x, float *y, Vector2 *vel_out)
{
    float u = clampf(t->t / t->dur, 0, 1);
    float e = u * u * (3 - 2 * u);           // smoothstep, eases in and out
    if (x) *x = t->from_x + (t->to_x - t->from_x) * e;
    if (y) *y = t->from_y + (t->to_y - t->from_y) * e;
    if (vel_out) {
        float de = 6 * u * (1 - u) / t->dur;
        *vel_out = (Vector2){ (t->to_x - t->from_x) * de, (t->to_y - t->from_y) * de };
    }
    return e;
}

bool pointer_trip_done(const PointerTrip *t)
{
    return t->t >= t->dur;
}

static void start_trip(PointerPhase p, float x, float y)
{
    set_phase(p);
    trip = pointer_trip(cur_x, cur_y, x, y, 0);
}

// the points over, tell the brain (once) and head home
static void resolve(PointerResult r, int x, int y, bool inside)
{
    if (resolved || phase == POINTER_HOME || phase == POINTER_BACK) return;
    resolved = true;
    result = r;
    result_t = 0;
    brain_send_point_done(id, OUTCOME[r], x, y, inside);
    if (phase == POINTER_AT) linger = r == POINTER_RESULT_CLICKED_IN ? LINGER_HIT : LINGER_OTHER;
    else start_trip(POINTER_BACK, home_x, home_y);   // turn around mid walk
}

void pointer_point(int new_id, int x, int y, Rectangle b, long long window)
{
    resolve(POINTER_RESULT_CLEARED, 0, 0, false);   // the one in progress, if any
    if (phase == POINTER_HOME) {                     // wherever the user last left him
        home_x = cur_x;
        home_y = cur_y;
    }
    id = new_id;
    resolved = false;
    result = POINTER_RESULT_NONE;
    result_t = 0;
    target = (Vector2){ (float)x, (float)y };
    box = b;

    win = window == own_win ? 0 : window;
    win_root = win ? platform_window_root(win) : 0;
    win_shown = win && platform_window_state(win, &win_r[0], &win_r[1], &win_r[2], &win_r[3]) == PLATFORM_WIN_SHOWN;

    int wa_x, wa_y, wa_w, wa_h, wx, wy;
    platform_get_work_area_at(x, y, &wa_x, &wa_y, &wa_w, &wa_h);
    pointer_place(&lay, wa_x, wa_y, wa_w, wa_h, target, box, (int)cur_x, (int)cur_y, &wx, &wy);
    start_trip(POINTER_OUT, (float)wx, (float)wy);
}

void pointer_clear(int clear_id)
{
    if (clear_id == id) resolve(POINTER_RESULT_CLEARED, 0, 0, false);
}

void pointer_clear_active(void) { resolve(POINTER_RESULT_CLEARED, 0, 0, false); }
void pointer_dismiss(void)      { resolve(POINTER_RESULT_DISMISSED, 0, 0, false); }

bool pointer_pointing(void)
{
    return !resolved && (phase == POINTER_OUT || phase == POINTER_AT);
}

bool pointer_away(void)
{
    return phase != POINTER_HOME;
}

// did the target window close, hide, move, or lose the foreground to another app?
static bool target_window_changed(void)
{
    if (!win) return false;
    int r[4];
    if (!win_shown || platform_window_state(win, &r[0], &r[1], &r[2], &r[3]) != PLATFORM_WIN_SHOWN) return true;
    for (int i = 0; i < 4; i++)
        if (abs(r[i] - win_r[i]) > WINDOW_SLOP) return true;
    long long fg = platform_foreground_window();
    return fg && fg != own_win && fg != fg_base && fg != win_root;
}

static void arrive_home(void)
{
    set_phase(POINTER_HOME);
    vel = (Vector2){ 0 };
}

void pointer_update(float dt, PointerFrame *f)
{
    f->move = false;
    cur_x = (float)f->win_x;
    cur_y = (float)f->win_y;
    phase_t += dt;
    result_t += dt;

    // grabbed, the points over and he stays where hes dropped
    if (f->dragging && phase != POINTER_HOME) {
        resolve(POINTER_RESULT_DISMISSED, 0, 0, false);
        arrive_home();
    }

    // the press gets read every frame so one made during the walk is used up there and
    // doesnt count as the click when he arrives
    if (f->pressed && (phase == POINTER_OUT || phase == POINTER_AT)) {
        if (f->on_ui) {
            resolve(POINTER_RESULT_DISMISSED, 0, 0, false);   // a poke
        } else if (phase == POINTER_AT) {
            float s = CLICK_SLOP;
            bool inside = f->cursor_x >= box.x - s && f->cursor_x <= box.x + box.width + s &&
                          f->cursor_y >= box.y - s && f->cursor_y <= box.y + box.height + s;
            resolve(inside ? POINTER_RESULT_CLICKED_IN : POINTER_RESULT_CLICKED_OUT,
                    f->cursor_x, f->cursor_y, inside);
        }
    }

    if (phase == POINTER_AT) {
        if (resolved) {
            if ((linger -= dt) <= 0) start_trip(POINTER_BACK, home_x, home_y);
        } else if ((check_in -= dt) <= 0) {
            check_in = CHECK_EVERY;
            if (target_window_changed()) resolve(POINTER_RESULT_MOVED, 0, 0, false);
        }
    }

    vel = (Vector2){ 0 };
    if (phase == POINTER_OUT || phase == POINTER_BACK) {
        if (!f->held) trip.t += dt;          // mouse is down on him, wait and see
        Vector2 v;
        pointer_trip_at(&trip, &cur_x, &cur_y, &v);
        f->win_x = (int)lroundf(cur_x);
        f->win_y = (int)lroundf(cur_y);
        f->move = true;
        if (!f->held) vel = v;
        if (pointer_trip_done(&trip)) {
            if (phase == POINTER_BACK) {
                arrive_home();
            } else {
                set_phase(POINTER_AT);
                fg_base = platform_foreground_window();
                check_in = CHECK_EVERY;
            }
        }
    }

    // the chat box would sit in someone elses window, shut it while hes out and reopen it if it was
    if (phase != POINTER_HOME) {
        if (!chat_saved) {
            chat_saved = true;
            chat_was_open = f->chat_open;
        }
        if (!f->keep_chat) f->chat_open = false;
    } else if (chat_saved) {
        chat_saved = false;
        if (chat_was_open) f->chat_open = true;
    }
}

PointerView pointer_view(void)
{
    PointerView v = { 0 };
    v.phase = phase;
    v.phase_t = phase_t;
    v.result = result;
    v.result_t = result_t;
    v.vel = vel;
    if (phase == POINTER_HOME && result == POINTER_RESULT_NONE) return v;
    v.target = (Vector2){ target.x - cur_x, target.y - cur_y };
    v.box = (Rectangle){ box.x - cur_x, box.y - cur_y, box.width, box.height };
    v.target_right = v.target.x > (lay.spot_left.x + lay.spot_right.x) / 2;
    if (phase == POINTER_AT) {
        v.travel = 1;
    } else if (phase == POINTER_OUT || phase == POINTER_BACK) {
        v.travel = pointer_trip_at(&trip, NULL, NULL, NULL);
    }
    return v;
}
