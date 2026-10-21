// tests src/drive.c on a fake desktop with a fake mouse and keyboard that just write down
// what theyre told, on a fake 60 fps clock. wired up like main.c does it, pointer.c too for
// the handover. nothing real gets moved, clicked or typed. make ctest runs it

#include <stdio.h>
#include <string.h>

#include "../src/drive.c"

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
                                 printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- the fake desktop ----

// work areas, the primary (taskbar at the bottom) and one to its left. the pointer can go
// anywhere on both screens (1080 tall) taskbar included
void platform_get_work_area_at(int px, int py, int *x, int *y, int *w, int *h)
{
    (void)py;
    *x = px < 0 ? -1920 : 0;
    *y = 0;
    *w = 1920;
    *h = px < 0 ? 1032 : 1040;
}

static int fx, fy;                       // the real pointer
void platform_get_cursor_screen(int *x, int *y) { *x = fx; *y = fy; }

#define OWN 999                          // his own window
static long long fake_fg = 500;
long long platform_foreground_window(void) { return fake_fg; }
long long platform_window_root(long long w) { return w; }
PlatformWinState platform_window_state(long long w, int *x, int *y, int *ww, int *hh)
{
    (void)w;
    *x = 100; *y = 100; *ww = 800; *hh = 600;
    return PLATFORM_WIN_SHOWN;
}

// ---- the fake mouse and keyboard ----

typedef enum { IN_MOVE, IN_BUTTON, IN_SCROLL, IN_CHAR, IN_KEY, IN_KEYS } InKind;

typedef struct {
    InKind kind;
    double t;                // fake clock time
    int x, y;                // where the pointer was (for IN_MOVE where it got put)
    int a, b;                // IN_BUTTON right, down / IN_SCROLL dx, dy / IN_KEY down
    unsigned cp;             // IN_CHAR
    char name[64];           // IN_KEY the key, IN_KEYS the combo like "ctrl+s"
} Input;

#define INPUT_MAX 20000
static Input ins[INPUT_MAX];
static int nin;
static double t_now;
static bool fail_set, fail_button, fail_scroll, fail_type, fail_key;
static bool button_is_down[2];
static int keys_held;
static bool os_pressed;                  // what platform_mouse_pressed() says next frame
static bool esc_flag;                    // what platform_key_pressed() says next

static Input *rec(InKind k)
{
    static bool full;
    if (nin == INPUT_MAX - 1 && !full) {
        full = true;
        printf("  input log is full, the last slot gets overwritten from here (raise INPUT_MAX)\n");
    }
    Input *i = &ins[nin < INPUT_MAX - 1 ? nin++ : nin];
    memset(i, 0, sizeof *i);
    i->kind = k;
    i->t = t_now;
    i->x = fx;
    i->y = fy;
    return i;
}

bool platform_set_cursor(int x, int y)
{
    if (fail_set) return false;
    fx = x < -1920 ? -1920 : x > 1919 ? 1919 : x;     // windows keeps it on the screens
    fy = y < 0 ? 0 : y > 1079 ? 1079 : y;
    rec(IN_MOVE);
    return true;
}

bool platform_mouse_button(bool right, bool down)
{
    if (fail_button) return false;
    Input *i = rec(IN_BUTTON);
    i->a = right;
    i->b = down;
    button_is_down[right] = down;
    if (down && !right) os_pressed = true;   // GetAsyncKeyState sees his clicks too
    return true;
}

bool platform_scroll(int dx, int dy)
{
    if (fail_scroll) return false;
    Input *i = rec(IN_SCROLL);
    i->a = dx;
    i->b = dy;
    return true;
}

bool platform_type_codepoint(unsigned cp)
{
    if (fail_type) return false;
    rec(IN_CHAR)->cp = cp;
    return true;
}

static const char *const KEY_NAMES[] = {
    "ctrl", "shift", "alt", "super", "enter", "tab", "escape", "backspace", "delete", "insert",
    "space", "up", "down", "left", "right", "home", "end", "pageup", "pagedown",
    "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
};

bool platform_key_known(const char *name)
{
    if (strlen(name) == 1) return name[0] > 32 && name[0] < 127;
    for (int i = 0; i < (int)(sizeof KEY_NAMES / sizeof KEY_NAMES[0]); i++)
        if (strcmp(name, KEY_NAMES[i]) == 0) return true;
    return false;
}

bool platform_key(const char *name, bool down)
{
    if (fail_key || !platform_key_known(name)) return false;
    Input *i = rec(IN_KEY);
    snprintf(i->name, sizeof i->name, "%s", name);
    i->a = down;
    keys_held += down ? 1 : -1;
    return true;
}

bool platform_keys(const char *const *names, int n)
{
    if (fail_key) return false;
    for (int k = 0; k < n; k++)
        if (!platform_key_known(names[k])) return false;
    Input *i = rec(IN_KEYS);
    for (int k = 0; k < n; k++) {
        size_t len = strlen(i->name);
        snprintf(i->name + len, sizeof i->name - len, "%s%s", k ? "+" : "", names[k]);
        if (strcmp(names[k], "escape") == 0) esc_flag = true;   // the poll sees his own esc too
    }
    return true;
}

bool platform_key_pressed(PlatformKey key)
{
    (void)key;
    bool p = esc_flag;
    esc_flag = false;
    return p;
}

// ---- the fake brain ----

typedef struct { int id; char outcome[16]; int x, y; double t; } Done;
static Done dones[256];
static int ndone;
#define ID_MAX 1000
static int answers[ID_MAX];              // drive_done count per id, across all the tests
static bool sent_id[ID_MAX];
static int ncancel;

void brain_send_drive_done(int id, const char *outcome, int x, int y)
{
    CHECK(id > 0 && id < ID_MAX, "drive_done for a bad id %d", id);
    if (id <= 0 || id >= ID_MAX) return;
    CHECK(sent_id[id], "drive_done for id %d, never sent", id);
    CHECK(++answers[id] == 1, "id %d answered %d times (now %s)", id, answers[id], outcome);
    Done *d = &dones[ndone < 255 ? ndone++ : ndone];
    d->id = id;
    snprintf(d->outcome, sizeof d->outcome, "%s", outcome);
    d->x = x;
    d->y = y;
    d->t = t_now;
}

void brain_send_cancel(void) { ncancel++; }

typedef struct { int id; char outcome[16]; } PointDone;
static PointDone pdones[16];
static int npdone;

void brain_send_point_done(int id, const char *outcome, int x, int y, bool inside)
{
    (void)x; (void)y; (void)inside;
    PointDone *d = &pdones[npdone < 15 ? npdone++ : npdone];
    d->id = id;
    snprintf(d->outcome, sizeof d->outcome, "%s", outcome);
}

static const Done *done_of(int id)
{
    for (int i = 0; i < ndone; i++)
        if (dones[i].id == id) return &dones[i];
    return NULL;
}

static bool outcome_is(int id, const char *outcome)
{
    const Done *d = done_of(id);
    return d && strcmp(d->outcome, outcome) == 0;
}

static const char *outcome_of(int id)
{
    const Done *d = done_of(id);
    return d ? d->outcome : "(none)";
}

// ---- main.c, as far as driving goes ----

#define DT (1.0f / 60.0f)
#define HOME_X 1484
#define HOME_Y 458

static int at_x, at_y;
static bool chat_open, asking, held, dragging;
static DriveFrame df;
static int max_step;                     // biggest jump the window made in one frame
static int moved_while_away;             // frames drive.c moved him while pointer.c had him
static int drive_moves;                  // frames drive.c moved him

// same layout as pointer_test.c (labels are over there)
static PointerLayout test_layout(void)
{
    PointerLayout l = { 420, 582 };
    l.solid[l.solid_count++] = (Rectangle){ 162, 270, 96, 138 };
    l.solid[l.solid_count++] = (Rectangle){ 16, 0, 388, 276 };
    l.solid[l.solid_count++] = (Rectangle){ 16, 416, 388, 128 };
    l.solid[l.solid_count++] = (Rectangle){ 184, 416, 52, 36 };
    l.solid[l.solid_count++] = (Rectangle){ 184, 542, 52, 36 };
    l.spot_left = (Vector2){ 70, 345 };
    l.spot_right = (Vector2){ 350, 345 };
    return l;
}

static void frame(void)
{
    bool pressed = os_pressed;           // platform_mouse_pressed(), once a frame
    os_pressed = false;

    PointerFrame pf = { 0 };
    pf.win_x = at_x;
    pf.win_y = at_y;
    pf.cursor_x = fx;
    pf.cursor_y = fy;
    pf.pressed = pressed && !drive_injected_within(0.3f);
    pf.held = held;
    pf.dragging = dragging;
    pf.chat_open = chat_open;
    pointer_update(DT, &pf);
    chat_open = pf.chat_open;
    if (pf.move) {
        at_x = pf.win_x;
        at_y = pf.win_y;
    }

    memset(&df, 0, sizeof df);
    df.win_x = at_x;
    df.win_y = at_y;
    df.pressed = pressed;
    df.held = held;
    df.dragging = dragging;
    df.asking = asking;
    df.pointer_away = pointer_away();
    df.chat_open = chat_open;
    drive_update(DT, &df);
    chat_open = df.chat_open;
    if (df.move) {
        int step = abs(df.win_x - at_x) + abs(df.win_y - at_y);
        if (step > max_step) max_step = step;
        at_x = df.win_x;
        at_y = df.win_y;
        drive_moves++;
    }
    // drive.c must never move him while pointer.c has him (pointer.c landing him home and
    // drive.c setting off in the same frame is fine)
    if (df.move && df.pointer_away) moved_while_away++;
    t_now += DT;

    // every frame, while he steers the pointer is on his plug and between actions nothing is held down
    DriveView v = drive_view();
    if (drive_steering())
        CHECK(abs(v.plug_x - fx) <= GRAB_SLOP && abs(v.plug_y - fy) <= GRAB_SLOP,
              "steering, but the pointer (%d,%d) isn't on his plug (%d,%d)", fx, fy, v.plug_x, v.plug_y);
    if (!busy)
        CHECK(!button_is_down[0] && !button_is_down[1] && keys_held == 0,
              "something left held down (buttons %d %d, keys %d)", button_is_down[0], button_is_down[1], keys_held);
}

static void run_for(float s)
{
    for (float t = 0; t < s; t += DT) frame();
}

static DriveAnimPhase phase_now(void) { return (DriveAnimPhase)drive_view().in.phase; }

// frames till the phase is p, returns the secs it took
static float run_until_phase(DriveAnimPhase p, float limit)
{
    float t = 0;
    while (phase_now() != p && t < limit) { frame(); t += DT; }
    CHECK(phase_now() == p, "never got to phase %d (at %d)", p, phase_now());
    return t;
}

static float run_while_phase(DriveAnimPhase p, float limit)
{
    float t = 0;
    while (phase_now() == p && t < limit) { frame(); t += DT; }
    CHECK(phase_now() != p, "stuck in phase %d", p);
    return t;
}

static float run_until_answered(int id, float limit)
{
    float t = 0;
    while (!done_of(id) && t < limit) { frame(); t += DT; }
    CHECK(done_of(id) != NULL, "id %d never answered", id);
    return t;
}

static void reset(void)
{
    PointerLayout l = test_layout();
    pointer_init(&l, OWN, HOME_X, HOME_Y);
    drive_init(&l, OWN, HOME_X, HOME_Y);
    at_x = HOME_X;
    at_y = HOME_Y;
    fx = 300;
    fy = 200;
    chat_open = asking = held = dragging = false;
    fail_set = fail_button = fail_scroll = fail_type = fail_key = false;
    fake_fg = 500;
    nin = ndone = npdone = ncancel = 0;
    os_pressed = esc_flag = false;
    max_step = moved_while_away = drive_moves = 0;
    frame();                             // primes the polls
}

// ---- the brains "drive" messages ----

static int next_id = 1;

static BrainEvent action(BrainDriveDo what, int x, int y)
{
    BrainEvent e;
    memset(&e, 0, sizeof e);
    e.type = BRAIN_EV_DRIVE;
    e.id = next_id++;
    e.drive.action = what;
    e.drive.x = x;
    e.drive.y = y;
    e.drive.count = 1;
    e.drive.repeat = 1;
    return e;
}

static void with_key(BrainEvent *e, const char *name)
{
    snprintf(e->drive.keys[e->drive.nkeys++], BRAIN_KEY_NAME_MAX, "%s", name);
}

static int send(const BrainEvent *e)
{
    sent_id[e->id] = true;
    drive_action(e);
    return e->id;
}

static int click_at(int x, int y)
{
    BrainEvent e = action(BRAIN_DRIVE_CLICK, x, y);
    return send(&e);
}

static int type_text(const char *s)
{
    BrainEvent e = action(BRAIN_DRIVE_TYPE, 0, 0);
    snprintf(e.text, sizeof e.text, "%s", s);
    return send(&e);
}

static int count_of(InKind k, int from)
{
    int n = 0;
    for (int i = from; i < nin; i++) n += ins[i].kind == k;
    return n;
}

// every input since from except pointer moves, as words like "+ctrl +L -L -ctrl s0,1 c48 k:ctrl+s"
// (keys and L/R buttons down/up, scroll notches, characters, combos)
static const char *inputs_since(int from)
{
    static char buf[4096];
    buf[0] = 0;
    for (int i = from; i < nin && strlen(buf) < sizeof buf - 64; i++) {
        const Input *in = &ins[i];
        char w[80];
        switch (in->kind) {
        case IN_BUTTON: snprintf(w, sizeof w, "%c%c", in->b ? '+' : '-', in->a ? 'R' : 'L'); break;
        case IN_KEY:    snprintf(w, sizeof w, "%c%s", in->a ? '+' : '-', in->name); break;
        case IN_SCROLL: snprintf(w, sizeof w, "s%d,%d", in->a, in->b); break;
        case IN_CHAR:   snprintf(w, sizeof w, "c%x", in->cp); break;
        case IN_KEYS:   snprintf(w, sizeof w, "k:%s", in->name); break;
        default: continue;
        }
        if (buf[0]) strcat(buf, " ");
        strcat(buf, w);
    }
    return buf;
}

// the i-th input of kind k since from, NULL if theres none
static const Input *nth(InKind k, int i, int from)
{
    for (int j = from; j < nin; j++)
        if (ins[j].kind == k && i-- == 0) return &ins[j];
    return NULL;
}

// fresh drive where hes already holding the pointer at (x, y)
static void holding_at(int x, int y)
{
    reset();
    fx = x;
    fy = y;
    int id = click_at(x, y);
    run_until_answered(id, 4);
    CHECK(phase_now() == DRIVE_ANIM_HOLD && drive_steering(), "should be holding the pointer");
}

static void test_fetch_carry_click(void)
{
    printf("fetch, carry, click\n");
    reset();
    chat_open = true;
    int id = click_at(1000, 600);
    frame();
    CHECK(!chat_open, "the chat box should shut as the drive starts");
    DriveView v = drive_view();
    CHECK(v.in.phase == DRIVE_ANIM_FETCH && !v.in.latched && !v.in.steering, "should set off to fetch the pointer");
    CHECK(v.plug_x == 300 && v.plug_y == 200, "fetching: point_dx/dy should be the pointer");
    float fetch_t = run_while_phase(DRIVE_ANIM_FETCH, 3) + DT;
    CHECK(count_of(IN_MOVE, 0) == 0, "moved the pointer before he had it");
    v = drive_view();
    CHECK(v.in.phase == DRIVE_ANIM_CARRY && v.in.latched && v.in.steering, "should have latched and be carrying");
    int px = 300 - at_x, py = 200 - at_y;
    CHECK(px >= 0 && px < 420 && py >= 0 && py < 582, "latched with the pointer outside his window (%d,%d)", px, py);
    CHECK(!(px >= 162 - 8 && px < 258 + 8 && py >= 270 && py < 408), "latched with the pointer on his body (%d,%d)", px, py);
    CHECK(fetch_t > 0.4f && fetch_t < 1.2f, "fetching took %.2f s", fetch_t);

    float carry_t = run_until_answered(id, 4);
    CHECK(outcome_is(id, "done"), "expected done, got %s", outcome_of(id));
    const Done *d = done_of(id);
    CHECK(d && d->x == 1000 && d->y == 600, "done should say where the pointer is");
    CHECK(fx == 1000 && fy == 600, "the pointer should be at the spot, at (%d,%d)", fx, fy);
    CHECK(count_of(IN_MOVE, 0) >= 20, "the pointer should travel with him (%d moves)", count_of(IN_MOVE, 0));
    CHECK(strcmp(inputs_since(0), "+L -L") == 0, "one left click, got \"%s\"", inputs_since(0));
    const Input *dn = nth(IN_BUTTON, 0, 0), *up = nth(IN_BUTTON, 1, 0);
    CHECK(dn && dn->x == 1000 && dn->y == 600 && up && up->x == 1000, "clicked away from the spot");
    if (dn && up) {
        CHECK(up->t - dn->t > 0.04 && up->t - dn->t < 0.07, "held the click %.3f s", up->t - dn->t);
        const Input *last_move = nth(IN_MOVE, count_of(IN_MOVE, 0) - 1, 0);
        CHECK(last_move && dn->t - last_move->t >= 0.05, "pressed before the pointer settled");
    }
    v = drive_view();
    CHECK(v.in.phase == DRIVE_ANIM_HOLD && v.in.steering && v.in.clicks == 1, "should hold on after one click");
    CHECK(max_step < 60, "jerky walk: %d px in a frame", max_step);
    printf("  fetch %.2f s, carry and click %.2f s, biggest step %d px\n", fetch_t, carry_t, max_step);

    CHECK(drive_injected_within(0.3f), "he just clicked");
    run_for(0.35f);
    CHECK(!drive_injected_within(0.3f), "holding still isn't input");
    CHECK(fx == 1000 && fy == 600 && phase_now() == DRIVE_ANIM_HOLD, "should still be holding it there");
}

static void test_clicks(void)
{
    printf("double, triple and right clicks, with modifiers\n");
    holding_at(800, 500);
    int from = nin;
    BrainEvent e = action(BRAIN_DRIVE_CLICK, 860, 520);
    e.drive.count = 2;
    with_key(&e, "ctrl");
    with_key(&e, "shift");
    int id = send(&e);
    run_until_answered(id, 4);
    CHECK(outcome_is(id, "done"), "his own clicks must not count as a grab (got %s)", outcome_of(id));
    CHECK(strcmp(inputs_since(from), "+ctrl +shift +L -L +L -L -shift -ctrl") == 0,
          "double click with ctrl+shift, got \"%s\"", inputs_since(from));
    const Input *b[4];
    for (int i = 0; i < 4; i++) b[i] = nth(IN_BUTTON, i, from);
    if (b[3]) {
        CHECK(b[1]->t - b[0]->t > 0.04 && b[1]->t - b[0]->t < 0.07, "first click held %.3f s", b[1]->t - b[0]->t);
        CHECK(b[2]->t - b[1]->t > 0.04 && b[2]->t - b[1]->t < 0.1, "gap between clicks %.3f s", b[2]->t - b[1]->t);
        CHECK(b[3]->t - b[0]->t < 0.3, "a double click must fit the OS's double-click time (%.3f s)", b[3]->t - b[0]->t);
        CHECK(b[0]->x == 860 && b[3]->x == 860 && b[3]->y == 520, "the pointer moved during the double click");
        const Input *mods = nth(IN_KEY, 0, from);
        CHECK(mods && mods->t <= b[0]->t, "modifiers should be down before the click");
    }
    CHECK(drive_view().in.clicks == 3, "clicks should count 1 + 2, got %d", drive_view().in.clicks);

    from = nin;
    e = action(BRAIN_DRIVE_CLICK, 860, 520);
    e.drive.count = 3;
    id = send(&e);
    run_until_answered(id, 2);
    CHECK(strcmp(inputs_since(from), "+L -L +L -L +L -L") == 0, "triple click in place, got \"%s\"", inputs_since(from));
    CHECK(count_of(IN_MOVE, from) == 0, "a click on the spot he's holding shouldn't carry");

    from = nin;
    e = action(BRAIN_DRIVE_CLICK, 700, 300);
    e.drive.right = true;
    id = send(&e);
    run_until_answered(id, 3);
    CHECK(outcome_is(id, "done") && strcmp(inputs_since(from), "+R -R") == 0, "right click, got \"%s\"", inputs_since(from));
    CHECK(drive_view().in.clicks == 7, "clicks should be 7, got %d", drive_view().in.clicks);
}

static void test_drag(void)
{
    printf("drag\n");
    reset();
    fx = 500;
    fy = 500;
    BrainEvent e = action(BRAIN_DRIVE_DRAG, 500, 500);
    e.drive.to_x = 900;
    e.drive.to_y = 700;
    with_key(&e, "shift");
    int id = send(&e);
    float max_haul = 0;
    int moves_down = 0, from_moves = 0;
    while (!done_of(id) && t_now < 1e6) {
        int before = nin;
        frame();
        DriveView v = drive_view();
        if (v.in.phase == DRIVE_ANIM_ACT && v.in.haul > max_haul) max_haul = v.in.haul;
        for (int i = before; i < nin; i++)
            if (ins[i].kind == IN_MOVE && button_is_down[0]) moves_down++;
        if (++from_moves > 600) break;
    }
    CHECK(outcome_is(id, "done"), "drag should be done, got %s", outcome_of(id));
    CHECK(strcmp(inputs_since(0), "+shift +L -L -shift") == 0, "drag with shift, got \"%s\"", inputs_since(0));
    const Input *dn = nth(IN_BUTTON, 0, 0), *up = nth(IN_BUTTON, 1, 0);
    CHECK(dn && dn->x == 500 && dn->y == 500, "pressed away from the start");
    CHECK(up && up->x == 900 && up->y == 700, "released away from the end");
    CHECK(moves_down >= 20, "should haul it over with the button down (%d moves)", moves_down);
    CHECK(max_haul >= 0.4f, "a drag is a haul (haul %.2f)", max_haul);
    CHECK(drive_view().in.haul == 0, "haul should drop once it's over");
    const Done *d = done_of(id);
    CHECK(d && d->x == 900 && d->y == 700, "done at the drop");
}

static void test_scroll_and_move(void)
{
    printf("scroll, move\n");
    reset();
    fx = 700;
    fy = 400;
    BrainEvent e = action(BRAIN_DRIVE_SCROLL, 700, 400);
    e.drive.dy = 3;
    e.drive.dx = -2;
    int id = send(&e);
    run_until_answered(id, 4);
    CHECK(strcmp(inputs_since(0), "s0,1 s0,1 s0,1 s-1,0 s-1,0") == 0, "3 down then 2 left, got \"%s\"", inputs_since(0));
    for (int i = 1; i < 5; i++) {
        const Input *a = nth(IN_SCROLL, i - 1, 0), *b = nth(IN_SCROLL, i, 0);
        if (a && b) CHECK(b->t - a->t > 0.05 && b->t - a->t < 0.075, "notches %.3f s apart", b->t - a->t);
        if (b) CHECK(b->x == 700 && b->y == 400, "scrolled away from the spot");
    }

    int from = nin;
    e = action(BRAIN_DRIVE_MOVE, 1200, 300);
    id = send(&e);
    run_until_answered(id, 4);
    CHECK(outcome_is(id, "done") && fx == 1200 && fy == 300, "move should just carry it there");
    CHECK(inputs_since(from)[0] == 0, "a move presses nothing, got \"%s\"", inputs_since(from));
    CHECK(count_of(IN_MOVE, from) > 10, "a move carries the pointer");
}

static void test_type(void)
{
    printf("typing\n");
    reset();
    fx = 600;
    fy = 400;
    int id = type_text("Hi \xC3\xA9\xF0\x9F\x98\x80\n");   // "Hi ", e acute, a grin emoji, enter
    run_until_answered(id, 5);
    CHECK(outcome_is(id, "done"), "typing should be done, got %s", outcome_of(id));
    CHECK(strcmp(inputs_since(0), "c48 c69 c20 ce9 c1f600 k:enter") == 0, "got \"%s\"", inputs_since(0));
    CHECK(count_of(IN_MOVE, 0) == 0, "he types where he stands, holding the pointer still");
    DriveView v = drive_view();
    CHECK(v.in.keys == 6 && v.in.last_key == '\n', "keys %d, last %u", v.in.keys, v.in.last_key);
    const Input *first = nth(IN_CHAR, 0, 0), *last = nth(IN_KEYS, 0, 0);
    if (first && last)
        CHECK(last->t - first->t > 0.3 && last->t - first->t < 0.37, "6 keys took %.3f s (15 a second)", last->t - first->t);

    const struct { int n; float lo, hi; } longer[] = { { 30, 1.85f, 2.0f }, { 200, 3.7f, 4.05f }, { 500, 3.8f, 4.05f } };
    for (int c = 0; c < 3; c++) {
        char s[600];
        memset(s, 'x', longer[c].n);
        s[longer[c].n] = 0;
        int from = nin;
        id = type_text(s);
        run_until_answered(id, 6);
        int n = count_of(IN_CHAR, from);
        const Input *a = nth(IN_CHAR, 0, from), *b = nth(IN_CHAR, n - 1, from);
        float took = a && b ? (float)(b->t - a->t) : -1;
        CHECK(n == longer[c].n, "typed %d of %d", n, longer[c].n);
        CHECK(took > longer[c].lo && took < longer[c].hi, "%d characters took %.2f s", longer[c].n, took);
        printf("  %3d characters in %.2f s\n", longer[c].n, took);
    }

    int from = nin;
    id = type_text("a\r\nb\rc\td\x08\x1B\x7F");             // the other control chars get dropped
    run_until_answered(id, 3);
    CHECK(strcmp(inputs_since(from), "c61 k:enter c62 k:enter c63 k:tab c64") == 0, "line ends, got \"%s\"", inputs_since(from));
    from = nin;
    id = type_text("a\xC3(b\xF0\x9F" "c\x80");                // broken utf-8 gets skipped
    run_until_answered(id, 3);
    CHECK(strcmp(inputs_since(from), "c61 c28 c62 c63") == 0, "broken UTF-8, got \"%s\"", inputs_since(from));
    id = type_text("");
    CHECK(outcome_is(id, "done"), "nothing to type is done at once");
}

static void test_keys(void)
{
    printf("keys\n");
    holding_at(600, 400);
    int from = nin;
    BrainEvent e = action(BRAIN_DRIVE_KEY, 0, 0);
    with_key(&e, "ctrl");
    with_key(&e, "s");
    e.drive.repeat = 3;
    int id = send(&e);
    run_until_answered(id, 3);
    CHECK(outcome_is(id, "done"), "keys should be done, got %s", outcome_of(id));
    CHECK(strcmp(inputs_since(from), "k:ctrl+s k:ctrl+s k:ctrl+s") == 0, "got \"%s\"", inputs_since(from));
    for (int i = 1; i < 3; i++) {
        const Input *a = nth(IN_KEYS, i - 1, from), *b = nth(IN_KEYS, i, from);
        if (a && b) CHECK(b->t - a->t > 0.06 && b->t - a->t < 0.09, "repeats %.3f s apart", b->t - a->t);
    }
    DriveView v = drive_view();
    CHECK(v.in.keys == 3 && v.in.last_key == 's', "keys %d, last %c", v.in.keys, v.in.last_key);
    CHECK(count_of(IN_MOVE, from) == 0, "keys don't move the pointer");

    from = nin;
    e = action(BRAIN_DRIVE_KEY, 0, 0);
    with_key(&e, "hyper");
    id = send(&e);
    CHECK(outcome_is(id, "failed") && nin == from, "an unknown key fails at once, pressing nothing");
    e = action(BRAIN_DRIVE_KEY, 0, 0);
    id = send(&e);
    CHECK(outcome_is(id, "failed"), "no keys at all fails");
    e = action(BRAIN_DRIVE_CLICK, 700, 400);
    with_key(&e, "meta");
    id = send(&e);
    CHECK(outcome_is(id, "failed") && nin == from, "an unknown modifier fails at once");
    CHECK(phase_now() == DRIVE_ANIM_HOLD && drive_steering(), "a refused action leaves him holding on");
}

static void test_grab(void)
{
    printf("grabbed\n");
    reset();                             // mid-carry
    int id = click_at(1500, 900);
    run_until_phase(DRIVE_ANIM_CARRY, 3);
    run_for(0.2f);
    int moves = count_of(IN_MOVE, 0);
    fx += 10;                            // the users hand
    frame();
    CHECK(outcome_is(id, "grabbed"), "a nudge mid-carry should be grabbed, got %s", outcome_of(id));
    const Done *d = done_of(id);
    CHECK(d && d->x == fx && d->y == fy, "grabbed should say where the user has it");
    DriveView v = drive_view();
    CHECK(v.in.phase == DRIVE_ANIM_PAUSED && !v.in.latched && !v.in.steering, "should let go: oh, it's yours");
    run_for(0.5f);
    CHECK(count_of(IN_MOVE, 0) == moves && count_of(IN_BUTTON, 0) == 0, "kept steering after the grab");

    holding_at(600, 400);                // while he holds it between actions
    int nd = ndone;
    fx += 3;                             // 3 px is still his
    frame();
    fx -= 3;
    frame();
    CHECK(phase_now() == DRIVE_ANIM_HOLD, "3 px of drift isn't a grab");
    fx += 4;
    frame();
    CHECK(phase_now() == DRIVE_ANIM_PAUSED && ndone == nd, "4 px is: let go, but nothing to answer yet");
    int from = nin;
    int next = click_at(800, 400);
    CHECK(outcome_is(next, "grabbed") && nin == from, "the next action answers for the grab, unrun");
    int after = click_at(800, 400);
    CHECK(phase_now() == DRIVE_ANIM_FETCH, "...and the one after runs, fetching it again");
    run_until_answered(after, 4);
    CHECK(outcome_is(after, "done"), "should be done, got %s", outcome_of(after));

    run_for(0.3f);                       // user clicks (no movement) while he holds it
    os_pressed = true;
    frame();
    CHECK(phase_now() == DRIVE_ANIM_PAUSED, "a click he didn't make is a grab");
    next = click_at(800, 400);
    CHECK(outcome_is(next, "grabbed"), "...answered on the next action");

    reset();                             // mid drag, has to let go of the button and the modifier
    fx = 500;
    fy = 500;
    BrainEvent e = action(BRAIN_DRIVE_DRAG, 500, 500);
    e.drive.to_x = 1200;
    e.drive.to_y = 800;
    with_key(&e, "ctrl");
    id = send(&e);
    for (int i = 0; i < 300 && !(button_is_down[0] && count_of(IN_MOVE, 0) > 5); i++) frame();
    CHECK(button_is_down[0], "should be hauling with the button down");
    fy += 20;
    frame();
    CHECK(outcome_is(id, "grabbed"), "a grab mid-drag, got %s", outcome_of(id));
    CHECK(!button_is_down[0] && keys_held == 0, "must let go of the button and ctrl");
    CHECK(strcmp(inputs_since(0), "+ctrl +L -L -ctrl") == 0, "got \"%s\"", inputs_since(0));

    holding_at(600, 400);                // mid-typing
    id = type_text("a long line that takes him a good couple of seconds to type out");
    run_for(0.5f);
    int chars = count_of(IN_CHAR, 0);
    fx -= 8;
    frame();
    CHECK(outcome_is(id, "grabbed"), "a grab mid-typing, got %s", outcome_of(id));
    run_for(0.5f);
    CHECK(count_of(IN_CHAR, 0) == chars, "kept typing after the grab");
}

static void test_esc(void)
{
    printf("Esc\n");
    holding_at(600, 400);
    int id = type_text("a long line that takes him a good couple of seconds to type out");
    run_for(0.5f);
    int chars = count_of(IN_CHAR, 0);
    esc_flag = true;                     // user hits esc, anywhere
    frame();
    CHECK(outcome_is(id, "cancelled"), "Esc mid-typing should cancel it, got %s", outcome_of(id));
    CHECK(ncancel == 1 && df.cancelled, "and send one cancel (%d)", ncancel);
    DriveView v = drive_view();
    CHECK(v.in.phase == DRIVE_ANIM_DONE && v.in.result == DRIVE_RESULT_SHEEPISH && !v.in.latched,
          "should let go, sheepish");
    CHECK(!drive_driving() && drive_away() && !drive_steering(), "over, but still on his way home");
    run_for(0.5f);
    CHECK(count_of(IN_CHAR, 0) == chars, "kept typing after Esc");
    int late = click_at(900, 900);
    CHECK(outcome_is(late, "cancelled") && phase_now() == DRIVE_ANIM_DONE, "a late action of the drive is cancelled, unrun");
    drive_turn_over();                   // the brain says "cancelled", the turn is over
    int fresh = click_at(900, 900);
    CHECK(!done_of(fresh) && phase_now() == DRIVE_ANIM_FETCH, "a new turn's drive runs again");
    run_until_answered(fresh, 4);
    drive_end("done");
    CHECK(drive_view().in.result == DRIVE_RESULT_VICTORY, "that one went fine");

    holding_at(600, 400);                // his own escape doesnt stop him
    BrainEvent e = action(BRAIN_DRIVE_KEY, 0, 0);
    with_key(&e, "escape");
    id = send(&e);
    run_until_answered(id, 2);
    frame();
    CHECK(outcome_is(id, "done") && ncancel == 0 && phase_now() == DRIVE_ANIM_HOLD, "his own Escape isn't the user's Esc");
    run_for(0.2f);
    esc_flag = true;                     // but the users one a moment later does
    frame();
    CHECK(ncancel == 1 && phase_now() == DRIVE_ANIM_DONE, "the user's Esc after his own counts");

    holding_at(600, 400);                // esc in his chat box
    drive_cancel();
    frame();
    CHECK(ncancel == 1 && df.cancelled && phase_now() == DRIVE_ANIM_DONE, "Esc from the chat box stops him");
    drive_cancel();
    esc_flag = true;
    frame();
    CHECK(ncancel == 1, "Esc once he's stopped does nothing more");

    holding_at(600, 400);                // while waiting on the user
    asking = true;
    frame();
    int nd = ndone;
    esc_flag = true;
    frame();
    CHECK(ncancel == 1 && ndone == nd && phase_now() == DRIVE_ANIM_DONE, "Esc during an ask stops the drive");

    reset();                             // not driving so none of his business
    esc_flag = true;
    frame();
    CHECK(ncancel == 0 && phase_now() == DRIVE_ANIM_NONE, "Esc when he isn't driving");
}

static void test_fail(void)
{
    printf("failures\n");
    holding_at(600, 400);
    fail_button = true;                  // windows refuses the click (an admin window)
    int from = nin;
    BrainEvent e = action(BRAIN_DRIVE_CLICK, 650, 420);
    with_key(&e, "ctrl");
    int id = send(&e);
    run_until_answered(id, 3);
    CHECK(outcome_is(id, "failed"), "refused click, got %s", outcome_of(id));
    CHECK(strcmp(inputs_since(from), "+ctrl -ctrl") == 0 && keys_held == 0, "ctrl must come back up, got \"%s\"", inputs_since(from));
    CHECK(phase_now() == DRIVE_ANIM_HOLD && drive_steering(), "he still holds the pointer");
    fail_button = false;

    fail_set = true;                     // wont even move the pointer
    id = click_at(1200, 800);
    run_until_answered(id, 3);
    DriveView v = drive_view();
    CHECK(outcome_is(id, "failed") && !v.in.latched && !v.in.steering, "lost his grip: failed, let go");
    fail_set = false;
    id = click_at(1200, 800);
    CHECK(phase_now() == DRIVE_ANIM_FETCH, "fetches it again after losing it");
    run_until_answered(id, 4);
    CHECK(outcome_is(id, "done"), "then it works, got %s", outcome_of(id));

    e = action(BRAIN_DRIVE_NONE, 10, 10);
    id = send(&e);
    CHECK(outcome_is(id, "failed"), "an unknown \"do\" fails at once");

    fail_type = true;
    id = type_text("hello");
    run_until_answered(id, 2);
    CHECK(outcome_is(id, "failed"), "refused typing, got %s", outcome_of(id));
    fail_type = false;
    fail_scroll = true;
    e = action(BRAIN_DRIVE_SCROLL, 1200, 800);
    e.drive.dy = 2;
    id = send(&e);
    run_until_answered(id, 2);
    CHECK(outcome_is(id, "failed"), "refused scroll, got %s", outcome_of(id));
    fail_scroll = false;

    reset();                             // never types into his own window
    fake_fg = OWN;
    id = type_text("hello");
    CHECK(outcome_is(id, "failed"), "typing into his own window must fail at once");
    e = action(BRAIN_DRIVE_KEY, 0, 0);
    with_key(&e, "enter");
    id = send(&e);
    CHECK(outcome_is(id, "failed"), "so must keys");
    run_for(1);
    CHECK(at_x == HOME_X && at_y == HOME_Y && nin == 0, "...without going anywhere or touching anything");
    fake_fg = 500;
    id = click_at(700, 500);             // the brain clicks into the target first
    run_until_answered(id, 4);
    id = type_text("a long line that takes him a good couple of seconds to type out");
    run_for(0.5f);
    fake_fg = OWN;                       // and if he somehow gets focus halfway he stops
    frame();
    CHECK(outcome_is(id, "failed"), "his window got the focus mid-typing, got %s", outcome_of(id));
    int chars = count_of(IN_CHAR, 0);
    run_for(0.3f);
    CHECK(count_of(IN_CHAR, 0) == chars, "kept typing into himself");
}

static void test_wait(void)
{
    printf("waiting on the user\n");
    holding_at(600, 400);
    asking = true;                       // "Press Submit?"
    frame();
    DriveView v = drive_view();
    CHECK(v.in.phase == DRIVE_ANIM_WAIT && v.in.latched && !v.in.steering, "waits, cable trailing");
    CHECK(!drive_steering(), "not steering: the user can reach his bubble");
    int moves = count_of(IN_MOVE, 0), nd = ndone;
    for (int i = 0; i < 30; i++) {       // the user goes to click yes
        fx += 10;
        fy -= 3;
        frame();
        v = drive_view();
        CHECK(v.plug_x == fx && v.plug_y == fy, "the cable should trail the user's pointer");
    }
    CHECK(ndone == nd && count_of(IN_MOVE, 0) == moves, "waiting isn't steering: no grab, no pin");
    CHECK(!drive_injected_within(0.3f), "nothing he sent could be the click on yes");
    asking = false;                      // answered
    run_for(0.5f);
    CHECK(phase_now() == DRIVE_ANIM_WAIT, "still waiting for the next action");
    int ux = fx, uy = fy;
    int id = click_at(1200, 700);
    frame();
    CHECK(phase_now() == DRIVE_ANIM_FETCH, "the next action fetches the pointer again");
    run_until_answered(id, 4);
    const Input *first = nth(IN_MOVE, moves, 0);   // first step of the carry
    CHECK(outcome_is(id, "done") && fx == 1200 && fy == 700, "and carries on");
    CHECK(first && abs(first->x - ux) < 60 && abs(first->y - uy) < 60,
          "should pick it up where the user left it (%d,%d), first move (%d,%d)", ux, uy,
          first ? first->x : 0, first ? first->y : 0);

    reset();                             // the pointer runs off while he walks over
    id = click_at(1000, 600);
    run_for(0.3f);
    fx = 700;
    fy = 800;
    run_while_phase(DRIVE_ANIM_FETCH, 4);
    CHECK(drive_view().in.latched, "should catch it in the end");
    int px = 700 - at_x, py = 800 - at_y;
    CHECK(px >= 0 && px < 420 && py >= 0 && py < 582, "latched with it out of reach (%d,%d)", px, py);
    run_until_answered(id, 4);
    CHECK(outcome_is(id, "done"), "got %s", outcome_of(id));
}

static void test_end(void)
{
    printf("the end of a drive\n");
    const struct { const char *result; DriveResult want; } ends[] = {
        { "done", DRIVE_RESULT_VICTORY }, { "stopped", DRIVE_RESULT_SHEEPISH }, { "failed", DRIVE_RESULT_SHEEPISH },
    };
    for (int c = 0; c < 3; c++) {
        reset();
        chat_open = true;
        fx = 600;
        fy = 400;
        int id = click_at(600, 400);
        run_until_answered(id, 4);
        CHECK(at_x != HOME_X || at_y != HOME_Y, "should have walked off to fetch");
        drive_end(ends[c].result);
        frame();
        DriveView v = drive_view();
        CHECK(v.in.phase == DRIVE_ANIM_DONE && v.in.result == (int)ends[c].want && !v.in.latched && !v.in.steering,
              "%s: should let go with result %d (got phase %d result %d)", ends[c].result, ends[c].want, v.in.phase, v.in.result);
        int wx = at_x, wy = at_y;
        run_for(0.6f);
        CHECK(at_x == wx && at_y == wy, "%s: reacts on the spot before walking home", ends[c].result);
        float t = run_until_phase(DRIVE_ANIM_NONE, 4);
        CHECK(at_x == HOME_X && at_y == HOME_Y, "%s: should be home, at (%d,%d)", ends[c].result, at_x, at_y);
        v = drive_view();
        CHECK(v.in.phase == 0 && v.in.clicks == 0 && v.in.keys == 0 && v.in.result == 0 && !v.in.latched &&
              v.in.haul == 0 && v.vel.x == 0, "%s: DriveInputs should be all zero at home", ends[c].result);
        CHECK(chat_open, "%s: the chat box comes back", ends[c].result);
        CHECK(!drive_away(), "%s: over", ends[c].result);
        printf("  %-7s home %.2f s after the reaction\n", ends[c].result, t);
    }

    reset();                             // plan got declined so no drive at all
    drive_end("stopped");
    frame();
    CHECK(phase_now() == DRIVE_ANIM_NONE && at_x == HOME_X, "drive_end without a drive does nothing");

    reset();                             // mid action, gets answered and lets go
    fx = 500;
    fy = 500;
    BrainEvent e = action(BRAIN_DRIVE_DRAG, 500, 500);
    e.drive.to_x = 1200;
    e.drive.to_y = 800;
    int id = send(&e);
    for (int i = 0; i < 300 && !button_is_down[0]; i++) frame();
    drive_end("done");
    CHECK(outcome_is(id, "cancelled") && !button_is_down[0], "an action cut short by drive_end, got %s", outcome_of(id));

    reset();                             // another drive on his way home keeps the old home
    id = click_at(700, 500);
    run_until_answered(id, 4);
    drive_end("done");
    run_for(1.5f);                       // mid walk home
    CHECK(phase_now() == DRIVE_ANIM_DONE && (at_x != HOME_X || at_y != HOME_Y), "should be on the way home");
    id = click_at(300, 300);
    run_until_answered(id, 4);
    drive_end("done");
    run_until_phase(DRIVE_ANIM_NONE, 5);
    CHECK(at_x == HOME_X && at_y == HOME_Y, "home is still where the first drive began, at (%d,%d)", at_x, at_y);

    holding_at(600, 400);                // the turn ended without a drive_end
    drive_turn_over();
    CHECK(phase_now() == DRIVE_ANIM_DONE && drive_view().in.result == DRIVE_RESULT_SHEEPISH, "lets go when the turn's over");
    drive_end("done");                   // ...which turns up after all
    CHECK(drive_view().in.result == DRIVE_RESULT_VICTORY, "a late drive_end still decides the reaction");

    holding_at(600, 400);                // dropped somewhere new mid drive
    asking = true;
    frame();
    dragging = true;
    at_x = 700;
    at_y = 300;
    frame();
    dragging = false;
    asking = false;
    drive_end("done");
    run_until_phase(DRIVE_ANIM_NONE, 4);
    CHECK(at_x == 700 && at_y == 300, "the drop spot is home now, at (%d,%d)", at_x, at_y);
    CHECK(max_step < 60, "jerky walk: %d px in a frame", max_step);
}

static void test_one_at_a_time(void)
{
    printf("one action at a time\n");
    reset();
    int first = click_at(1000, 600);
    frame();
    int second = click_at(500, 500);
    CHECK(outcome_is(second, "failed"), "an action while one is going fails at once");
    run_until_answered(first, 4);
    CHECK(outcome_is(first, "done") && fx == 1000, "the first one still finishes, got %s", outcome_of(first));
}

static void test_handover(void)
{
    printf("a handover while he waits\n");
    holding_at(600, 400);
    // handover, he points at the password box ("your turn") with an ask
    pointer_point(70, 1200, 300, (Rectangle){ 1170, 285, 60, 30 }, 0);
    asking = true;
    int moves = drive_moves;
    float t = 0;
    while (pointer_view().phase != POINTER_AT && t < 3) { frame(); t += DT; }
    DriveView v = drive_view();
    CHECK(pointer_view().phase == POINTER_AT, "should be pointing at it");
    CHECK(v.in.phase == DRIVE_ANIM_WAIT && !v.in.latched && !v.in.steering, "let go: his arm's pointing");
    CHECK(drive_moves == moves && moved_while_away == 0, "drive.c moved him while pointer.c had him");
    int px = 1200 - at_x, py = 300 - at_y;
    CHECK(px >= 0 && px < 420 && py >= 0 && py < 582, "pointing from too far off");
    fx = 1200;                           // user goes to type the password, clicks into it first
    fy = 300;
    os_pressed = true;
    frame();
    CHECK(npdone == 1 && pdones[0].id == 70 && strcmp(pdones[0].outcome, "clicked") == 0,
          "pointer.c should see the user's click (%d)", npdone);
    t = 0;
    while (pointer_away() && t < 3) { frame(); t += DT; }
    CHECK(!pointer_away() && drive_moves == moves, "walked back by pointer.c alone");
    asking = false;                      // user hits the check, done
    frame();
    int id = click_at(900, 700);
    run_until_answered(id, 5);
    CHECK(outcome_is(id, "done") && fx == 900 && fy == 700, "the drive carries on, got %s", outcome_of(id));
    CHECK(moved_while_away == 0, "drive.c moved him while pointer.c had him");

    // brain moves on while hes still pointing, the point gets cleared and he walks back
    // before fetching
    pointer_point(71, 300, 800, (Rectangle){ 270, 785, 60, 30 }, 0);
    asking = true;
    t = 0;
    while (pointer_view().phase != POINTER_AT && t < 3) { frame(); t += DT; }
    asking = false;
    id = click_at(1300, 400);
    CHECK(npdone == 2 && pdones[1].id == 71 && strcmp(pdones[1].outcome, "cleared") == 0, "the point should be cleared");
    CHECK(phase_now() == DRIVE_ANIM_FETCH, "should be off to fetch it, once pointer.c is done");
    t = 0;
    while (pointer_away() && t < 3) { frame(); t += DT; }
    CHECK(moved_while_away == 0, "fetched while pointer.c still had him");
    run_until_answered(id, 5);
    CHECK(outcome_is(id, "done") && fx == 1300, "then carries on, got %s", outcome_of(id));
    CHECK(moved_while_away == 0, "drive.c moved him while pointer.c had him");
}

int main(void)
{
    test_fetch_carry_click();
    test_clicks();
    test_drag();
    test_scroll_and_move();
    test_type();
    test_keys();
    test_grab();
    test_esc();
    test_fail();
    test_wait();
    test_end();
    test_one_at_a_time();
    test_handover();

    int unanswered = 0;                  // exactly one drive_done per id, over everything
    for (int id = 1; id < next_id; id++)
        if (sent_id[id] && answers[id] != 1) unanswered++;
    CHECK(unanswered == 0, "%d ids without exactly one drive_done", unanswered);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
