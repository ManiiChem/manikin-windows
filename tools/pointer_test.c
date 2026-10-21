// tests src/pointer.c, where he stands on a fake 3 monitor desktop and the life of a point
// (travel, outcomes, exactly one point_done per id) on a fake clock. no window needed since
// pointer.c only uses raylibs types. make ctest runs it

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../src/pointer.c"

// ---- the fake desktop ----

typedef struct { int x, y, w, h; } Box;

// work areas, the primary (taskbar at the bottom), one to its left and one above it
static const Box MONITORS[] = {
    { 0, 0, 1920, 1040 },
    { -1920, 0, 1920, 1032 },
    { 0, -1080, 1920, 1080 },
};

static bool in_box(Box b, int x, int y)
{
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

void platform_get_work_area_at(int px, int py, int *x, int *y, int *w, int *h)
{
    Box b = MONITORS[0];                 // primary as the "nearest" is good enough here
    for (int i = 0; i < 3; i++)
        if (in_box(MONITORS[i], px, py)) b = MONITORS[i];
    // so a point in the primarys taskbar still counts as the primary
    *x = b.x; *y = b.y; *w = b.w; *h = b.h;
}

static PlatformWinState fake_state = PLATFORM_WIN_SHOWN;
static int fake_rect[4] = { 100, 100, 800, 600 };
static long long fake_fg = 500;

PlatformWinState platform_window_state(long long w, int *x, int *y, int *ww, int *hh)
{
    (void)w;
    *x = fake_rect[0]; *y = fake_rect[1]; *ww = fake_rect[2]; *hh = fake_rect[3];
    return fake_state;
}
long long platform_foreground_window(void) { return fake_fg; }
long long platform_window_root(long long w) { return w; }

// ---- the fake brain ----

typedef struct { int id; char outcome[16]; int x, y; bool inside; } Done;
static Done sent[64];
static int sent_count;
static int failures, checks;

void brain_send_point_done(int pid, const char *outcome, int x, int y, bool inside)
{
    if (sent_count == (int)(sizeof sent / sizeof sent[0])) {
        failures++;
        printf("  FAIL more point_done messages than the test keeps\n");
        return;
    }
    Done *d = &sent[sent_count++];
    d->id = pid;
    snprintf(d->outcome, sizeof d->outcome, "%s", outcome);
    d->x = x; d->y = y; d->inside = inside;
}

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
                                 printf(__VA_ARGS__); printf("\n"); } } while (0)

// the layout from main.c with the numbers written out (main.c builds it from its #defines)
static PointerLayout test_layout(void)
{
    PointerLayout l = { 420, 582 };
    l.solid[l.solid_count++] = (Rectangle){ 162, 270, 96, 138 };    // mascot
    l.solid[l.solid_count++] = (Rectangle){ 16, 0, 388, 276 };      // widest bubble, tail, halo
    l.solid[l.solid_count++] = (Rectangle){ 16, 416, 388, 128 };    // tallest chat box + halo
    l.solid[l.solid_count++] = (Rectangle){ 184, 416, 52, 36 };     // arrow, chat closed
    l.solid[l.solid_count++] = (Rectangle){ 184, 542, 52, 36 };     // arrow under the tallest chat
    l.spot_left = (Vector2){ 70, 345 };
    l.spot_right = (Vector2){ 350, 345 };
    return l;
}

typedef struct {
    const char *name;
    int x, y;
    Rectangle box;           // left, top, w, h. 0 width means a 20x20 box round the point
    bool expect_arm_height;  // nothing in the way so it should be beside him at arm height
} PlaceCase;

static void test_placement(void)
{
    printf("placement\n");
    PointerLayout l = test_layout();
    const PlaceCase cases[] = {
        { "middle", 960, 520, { 0 }, true },
        { "left edge", 5, 520, { 0 }, false },
        { "right edge", 1914, 520, { 0 }, false },
        { "top edge", 960, 5, { 0 }, false },
        { "bottom edge", 960, 1034, { 0 }, false },
        { "top-left corner", 4, 4, { 0 }, false },
        { "top-right corner", 1915, 4, { 0 }, false },
        { "bottom-left corner", 4, 1035, { 0 }, false },
        { "bottom-right corner", 1915, 1035, { 0 }, false },
        { "exact top-left px", 0, 0, { 0 }, false },
        { "exact bottom-right px", 1919, 1039, { 0 }, false },
        { "left monitor middle", -960, 500, { 0 }, true },
        { "left monitor left edge", -1915, 500, { 0 }, false },
        { "left monitor seam", -3, 500, { 0 }, false },
        { "left monitor top-left", -1916, 3, { 0 }, false },
        { "monitor above middle", 960, -540, { 0 }, true },
        { "monitor above bottom", 960, -6, { 0 }, false },
        { "tiny box", 960, 520, { 959, 519, 2, 2 }, true },
        { "huge box", 960, 520, { 10, 10, 1900, 1020 }, false },
        { "wide toolbar", 960, 30, { 0, 10, 1920, 40 }, false },
        { "tall sidebar", 30, 520, { 0, 0, 60, 1040 }, false },
        { "box above point", 960, 520, { 900, 300, 120, 240 }, false },
        { "in the taskbar", 1500, 1060, { 1484, 1044, 32, 32 }, false },
    };
    int n = (int)(sizeof cases / sizeof cases[0]);
    for (int c = 0; c < n; c++) {
        PlaceCase pc = cases[c];
        Rectangle box = pc.box.width > 0 ? pc.box : (Rectangle){ pc.x - 10.0f, pc.y - 10.0f, 20, 20 };
        int wa_x, wa_y, wa_w, wa_h, wx, wy;
        platform_get_work_area_at(pc.x, pc.y, &wa_x, &wa_y, &wa_w, &wa_h);
        // home is the primarys bottom right corner where he starts
        pointer_place(&l, wa_x, wa_y, wa_w, wa_h, (Vector2){ (float)pc.x, (float)pc.y }, box,
                      1920 - 420 - 16, 1040 - 582, &wx, &wy);
        int px = pc.x - wx, py = pc.y - wy;
        float ui = INFINITY;
        for (int i = 0; i < l.solid_count; i++) ui = fminf(ui, rect_dist(l.solid[i], (float)px, (float)py));
        float edge = fminf(fminf((float)px, (float)(l.win_w - 1 - px)), fminf((float)py, (float)(l.win_h - 1 - py)));
        bool target_on_screen = pc.x >= wa_x && pc.x < wa_x + wa_w && pc.y >= wa_y && pc.y < wa_y + wa_h;
        bool window_in = wx >= wa_x && wy >= wa_y && wx + l.win_w <= wa_x + wa_w && wy + l.win_h <= wa_y + wa_h;
        printf("  %-24s target (%5d,%5d)  window (%5d,%5d)  in window (%3d,%3d)  %s  clear %3.0f\n",
               pc.name, pc.x, pc.y, wx, wy, px, py, px > 210 ? "right" : "left ", fminf(ui, edge));
        CHECK(px >= 0 && px < l.win_w && py >= 0 && py < l.win_h, "%s: target outside the window", pc.name);
        CHECK(ui >= SOLID_GAP, "%s: target %.1f px from solid UI", pc.name, ui);
        if (target_on_screen)
            CHECK(window_in, "%s: window sticks out of the work area", pc.name);
        else
            CHECK(overshoot(&l, wa_x, wa_y, wa_w, wa_h, (float)wx, (float)wy) <= 40,
                  "%s: window sticks out further than needed", pc.name);
        if (pc.expect_arm_height) {
            CHECK(abs(py - 345) <= 4, "%s: not at arm height (y %d)", pc.name, py);
            CHECK(fminf(ui, edge) >= MARGIN, "%s: less than %d px of air", pc.name, MARGIN);
        }
    }
    // in the middle both sides fit, he picks the one nearer home and stands between the
    // target and home (home is to the right so the target ends up on his left)
    int wx, wy;
    clock_t t0 = clock();
    for (int i = 0; i < 20; i++)
        pointer_place(&l, 0, 0, 1920, 1040, (Vector2){ 960, 520 }, (Rectangle){ 950, 510, 20, 20 }, 1484, 458, &wx, &wy);
    printf("  one placement takes %.1f ms\n", (clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 20);
    pointer_place(&l, 0, 0, 1920, 1040, (Vector2){ 960, 520 }, (Rectangle){ 950, 510, 20, 20 }, 1484, 458, &wx, &wy);
    CHECK(960 - wx < 210, "middle from the right: target should be on his left");
    pointer_place(&l, 0, 0, 1920, 1040, (Vector2){ 960, 520 }, (Rectangle){ 950, 510, 20, 20 }, 16, 458, &wx, &wy);
    CHECK(960 - wx > 210, "middle from the left: target should be on his right");

    // normal sized buttons wider than the room beside him (220x60 first). they can run off the
    // windows edge but never under him. if one is too wide to clear him with its middle in the
    // window then just the part the brackets go round (84 px across the point) has to clear him.
    // the last is a 359 wide dropdown, he should stand beside it at arm height not over it
    const Rectangle wide[] = { { 1211, 405, 220, 60 }, { 700, 500, 300, 40 }, { 60, 300, 200, 50 },
                               { 1500, 600, 359, 60 } };
    for (int i = 0; i < 4; i++) {
        Rectangle b = wide[i];
        Vector2 t = { b.x + b.width / 2, b.y + b.height / 2 };
        pointer_place(&l, 0, 0, 1920, 1040, t, b, 1484, 458, &wx, &wy);
        Rectangle in_win = { b.x - wx, b.y - wy, b.width, b.height };
        if (b.width > POINTER_BIG_BOX) in_win = (Rectangle){ t.x - 42 - wx, b.y - wy, 84, b.height };
        CHECK(overlap_area(in_win, l.solid[0]) == 0, "wide box %d: under him (window %d,%d)", i, wx, wy);
        Rectangle room = l.solid[0];
        room.x -= ARM_ROOM - 2; room.width += 2 * (ARM_ROOM - 2);
        CHECK(overlap_area(in_win, room) == 0, "wide box %d: no room for the plug (window %d,%d)", i, wx, wy);
        if (wy > 0)                      // skip when hes pinned to the top of the screen
            CHECK(abs((int)t.y - wy - 345) <= 4, "wide box %d: not at arm height (y %d)", i, (int)t.y - wy);
    }
}

// ---- the life of a point ----

#define DT (1.0f / 60.0f)
static PointerFrame fr;
static int max_step;

static void frame(void)
{
    int ox = fr.win_x, oy = fr.win_y;
    pointer_update(DT, &fr);
    if (fr.move) {
        int step = abs(fr.win_x - ox) + abs(fr.win_y - oy);
        if (step > max_step) max_step = step;
    }
    fr.pressed = false;                  // a press lasts one frame
}

// runs frames till the phase is p (or the limit), returns the secs it took
static float run_until(PointerPhase p, float limit)
{
    float t = 0;
    while (pointer_view().phase != p && t < limit) {
        frame();
        t += DT;
    }
    return t;
}

static void run_for(float s)
{
    for (float t = 0; t < s; t += DT) frame();
}

static void reset(void)
{
    PointerLayout l = test_layout();
    pointer_init(&l, 999, 1484, 458);
    memset(&fr, 0, sizeof fr);
    fr.win_x = 1484;
    fr.win_y = 458;
    fr.cursor_x = fr.cursor_y = 5000;
    sent_count = 0;
    fake_state = PLATFORM_WIN_SHOWN;
    fake_fg = 500;
    max_step = 0;
}

static Rectangle box_at(int x, int y) { return (Rectangle){ x - 30.0f, y - 18.0f, 60, 36 }; }

static void click(int x, int y, bool on_ui)
{
    fr.cursor_x = x;
    fr.cursor_y = y;
    fr.pressed = true;
    fr.on_ui = on_ui;
    frame();
    fr.on_ui = false;
}

static void test_click_inside(void)
{
    printf("click inside, walk home\n");
    reset();
    fr.chat_open = true;
    pointer_point(1, 400, 300, box_at(400, 300), 123);
    frame();
    CHECK(!fr.chat_open, "chat should close while he's away");
    click(400, 300, false);              // during the walk, doesnt count
    float out = run_until(POINTER_AT, 3) + DT;
    CHECK(sent_count == 0, "a click during the walk counted");
    CHECK(out >= TRIP_MIN - 0.05f && out <= TRIP_MAX + 0.05f, "trip took %.2f s", out);
    CHECK(max_step < 60, "jerky: %d px in one frame", max_step);
    PointerView v = pointer_view();
    CHECK(fabsf(v.target.x - (400 - fr.win_x)) < 1, "view target not in window px");
    CHECK(v.travel == 1 && v.vel.x == 0 && v.vel.y == 0, "AT should be still, travel 1");
    printf("  trip out %.2f s, biggest step %d px\n", out, max_step);

    click(420, 310, false);
    CHECK(sent_count == 1 && sent[0].id == 1 && strcmp(sent[0].outcome, "clicked") == 0 && sent[0].inside
          && sent[0].x == 420 && sent[0].y == 310, "expected one clicked/inside for id 1");
    CHECK(pointer_view().result == POINTER_RESULT_CLICKED_IN, "result should be CLICKED_IN");
    click(420, 310, false);              // a second click changes nothing
    float stay = run_until(POINTER_BACK, 2);
    CHECK(stay > 0.4f && stay < 0.6f, "should celebrate ~0.5 s, stayed %.2f", stay);
    run_until(POINTER_HOME, 3);
    CHECK(fr.win_x == 1484 && fr.win_y == 458, "should be home, at (%d,%d)", fr.win_x, fr.win_y);
    frame();
    CHECK(fr.chat_open, "chat should reopen at home");
    CHECK(sent_count == 1, "exactly one point_done, got %d", sent_count);
}

static void test_click_outside_and_ui(void)
{
    printf("click outside / poke\n");
    reset();
    pointer_point(2, 400, 300, box_at(400, 300), 0);
    run_until(POINTER_AT, 3);
    click(500, 300, false);
    CHECK(sent_count == 1 && strcmp(sent[0].outcome, "clicked") == 0 && !sent[0].inside, "expected clicked/outside");
    CHECK(pointer_view().result == POINTER_RESULT_CLICKED_OUT, "result CLICKED_OUT");
    float stay = run_until(POINTER_BACK, 2);
    CHECK(stay > 0.15f && stay < 0.35f, "should go back after ~0.25 s, stayed %.2f", stay);

    reset();
    pointer_point(3, 400, 300, box_at(400, 300), 0);
    run_until(POINTER_AT, 3);
    click(400, 300, true);               // on his own ui its a poke, even over the target
    CHECK(sent_count == 1 && strcmp(sent[0].outcome, "dismissed") == 0, "poke should dismiss");

    reset();                             // 6 px slop round the box
    pointer_point(4, 400, 300, box_at(400, 300), 0);
    run_until(POINTER_AT, 3);
    click(370 - 6, 282 - 6, false);
    CHECK(sent_count == 1 && sent[0].inside, "click 6 px outside the box should count as inside");
}

static void test_replace_and_clear(void)
{
    printf("replace, clear\n");
    reset();
    pointer_point(10, 400, 300, box_at(400, 300), 0);
    run_for(0.2f);                       // mid walk
    pointer_point(11, 1000, 600, box_at(1000, 600), 0);
    CHECK(sent_count == 1 && sent[0].id == 10 && strcmp(sent[0].outcome, "cleared") == 0, "10 should be cleared");
    run_until(POINTER_AT, 3);
    pointer_clear(10);                   // old id, ignored
    CHECK(sent_count == 1, "clear of an old id should do nothing");
    pointer_clear(11);
    CHECK(sent_count == 2 && sent[1].id == 11 && strcmp(sent[1].outcome, "cleared") == 0, "11 should be cleared");
    pointer_clear(11);
    CHECK(sent_count == 2, "a second clear should do nothing");
    run_until(POINTER_BACK, 2);
    run_for(0.2f);
    pointer_point(12, 300, 900, box_at(300, 900), 0);   // while walking home, nothing to clear
    CHECK(sent_count == 2, "a point during BACK sent %d", sent_count);
    run_until(POINTER_AT, 3);
    pointer_dismiss();                   // esc
    CHECK(sent_count == 3 && sent[2].id == 12 && strcmp(sent[2].outcome, "dismissed") == 0, "Esc should dismiss 12");
    run_until(POINTER_HOME, 3);
    CHECK(fr.win_x == 1484 && fr.win_y == 458, "home should still be the original spot");

    reset();                             // cleared mid walk, turns right around
    pointer_point(13, 400, 300, box_at(400, 300), 0);
    run_for(0.2f);
    pointer_clear(13);
    CHECK(pointer_view().phase == POINTER_BACK, "clear during OUT should head back at once");
    CHECK(!pointer_pointing(), "nothing in progress after a clear");
}

static void test_drag(void)
{
    printf("drag\n");
    reset();
    fr.chat_open = true;
    pointer_point(20, 400, 300, box_at(400, 300), 0);
    run_until(POINTER_AT, 3);
    fr.held = true;                      // pressed on him...
    frame();
    fr.held = false;
    fr.dragging = true;                  // ...and dragging
    fr.win_x = 700;
    fr.win_y = 200;
    frame();
    CHECK(sent_count == 1 && strcmp(sent[0].outcome, "dismissed") == 0, "drag should dismiss");
    CHECK(pointer_view().phase == POINTER_HOME, "drag should leave him home where dropped");
    CHECK(!fr.move, "must not move the window while dragged");
    CHECK(fr.chat_open, "chat should come back once he's home");
    fr.dragging = false;
    run_for(1);
    CHECK(!fr.move && fr.win_x == 700, "no trip back after a drag");
    pointer_point(21, 1500, 900, box_at(1500, 900), 0);
    run_until(POINTER_AT, 3);
    pointer_clear(21);
    run_until(POINTER_HOME, 3);
    CHECK(fr.win_x == 700 && fr.win_y == 200, "the drop spot is the new home, at (%d,%d)", fr.win_x, fr.win_y);

    reset();                             // held mid walk, he waits
    pointer_point(22, 400, 300, box_at(400, 300), 0);
    run_for(0.2f);
    int x0 = fr.win_x;
    fr.held = true;
    run_for(0.5f);
    CHECK(fr.win_x == x0 && pointer_view().vel.x == 0, "should hold still while pressed");
    fr.held = false;
    CHECK(run_until(POINTER_AT, 3) > 0.1f, "should carry on after release");
    CHECK(sent_count == 0, "a press that wasn't a drag mustn't end it by itself");
}

static void test_window_watch(void)
{
    printf("watching the window\n");
    reset();
    pointer_point(30, 400, 300, box_at(400, 300), 123);
    run_until(POINTER_AT, 3);
    fake_rect[0] += 3;                   // jitter within slop
    fake_fg = 999;                       // our own window
    run_for(0.5f);
    fake_fg = 0;
    run_for(0.5f);
    fake_fg = 123;                       // the target itself
    run_for(0.5f);
    CHECK(sent_count == 0, "small jitter / own window / target foreground mustn't count");
    fake_rect[0] += 5;
    run_for(0.2f);
    CHECK(sent_count == 1 && strcmp(sent[0].outcome, "moved") == 0, "window moved: expected moved");
    fake_rect[0] -= 8;

    const struct { PlatformWinState st; long long fg; const char *what; } changes[] = {
        { PLATFORM_WIN_GONE, 500, "closed" }, { PLATFORM_WIN_MINIMIZED, 500, "minimized" },
        { PLATFORM_WIN_HIDDEN, 500, "hidden" }, { PLATFORM_WIN_SHOWN, 777, "another app in front" },
    };
    for (int i = 0; i < 4; i++) {
        reset();
        pointer_point(31 + i, 400, 300, box_at(400, 300), 123);
        run_until(POINTER_AT, 3);
        fake_state = changes[i].st;
        fake_fg = changes[i].fg;
        float t = 0;
        while (sent_count == 0 && t < 1) { frame(); t += DT; }
        CHECK(sent_count == 1 && strcmp(sent[0].outcome, "moved") == 0, "%s: expected moved", changes[i].what);
        CHECK(t <= CHECK_EVERY + 2 * DT, "%s: noticed after %.2f s", changes[i].what, t);
    }

    reset();                             // window 0 never gets watched
    pointer_point(40, 400, 300, box_at(400, 300), 0);
    run_until(POINTER_AT, 3);
    fake_state = PLATFORM_WIN_GONE;
    fake_fg = 777;
    run_for(2);
    CHECK(sent_count == 0, "window 0 should not be watched");
    pointer_clear_active();
    CHECK(sent_count == 1 && strcmp(sent[0].outcome, "cleared") == 0, "clear_active should clear");
}

int main(void)
{
    test_placement();
    test_click_inside();
    test_click_outside_and_ui();
    test_replace_and_clear();
    test_drag();
    test_window_watch();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
