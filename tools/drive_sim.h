#pragma once

// fake desktop for filming wheel_preview's scripted drives in preview and showreel. a little
// sign up form, your pointer drawn on top, his ask bubble and the text showing up as he types.
// the scripts only set the Mascot inputs and this works out the rest (window from walk_vx/vy,
// pointer from cursor_dx/dy, focus and text from the click and key counters)

#include <stdio.h>
#include <string.h>

#include "raylib.h"
#include "mascot.h"
#include "point_sim.h"
#include "ui.h"

// keep in sync with the scripts in src/anim_wheel.c, they start the window here and click on these
#define DSIM_HOME_X 240.0f
#define DSIM_HOME_Y 330.0f
#define DSIM_SCREEN_W 1920.0f          // the fake screen
#define DSIM_SCREEN_H 1080.0f
#define DSIM_BUBBLE_BOTTOM 256.0f      // main.c's BUBBLE_BOTTOM
#define DSIM_FIELDS 3
#define DSIM_TEXT 40

typedef struct {
    Rectangle box;
    const char *label;
} DsimField;

static const DsimField DSIM_FORM[DSIM_FIELDS] = {
    { { 940, 470, 230, 28 }, "Name" },
    { { 940, 532, 230, 28 }, "Email" },
    { { 940, 594, 230, 28 }, "Password" },
};
static const Rectangle DSIM_PAGE = { 890, 400, 730, 460 };
static const Rectangle DSIM_SUBMIT = { 1460, 800, 110, 32 };

typedef struct {
    Vector2 win;                    // overlay window top left, desktop px
    float vx, vy;                   // last steps walk velocity (for trapezoid integration)
    bool started;
    int clicks, keys;               // drive counters as of last step
    int focus;                      // field with the caret, -1 for none
    char text[DSIM_FIELDS][DSIM_TEXT];
    bool submitted;
} DriveSim;

static void dsim_init(DriveSim *s)
{
    memset(s, 0, sizeof *s);
    s->win = (Vector2){ DSIM_HOME_X, DSIM_HOME_Y };
    s->focus = -1;
}

// your pointer in desktop px
static Vector2 dsim_pointer(const DriveSim *s, const Mascot *m)
{
    return (Vector2){ s->win.x + SIM_FACE_X + m->cursor_dx, s->win.y + SIM_FACE_Y + m->cursor_dy };
}

// once a step right after the script sets the inputs, moves the window and works the form
static void dsim_step(DriveSim *s, const Mascot *m, float dt)
{
    if (s->started) {
        s->win.x += (s->vx + m->walk_vx) * 0.5f * dt;
        s->win.y += (s->vy + m->walk_vy) * 0.5f * dt;
    }
    s->started = true;
    s->vx = m->walk_vx;
    s->vy = m->walk_vy;

    const DriveInputs *d = &m->drive_in;
    if (d->clicks > s->clicks) {
        Vector2 p = dsim_pointer(s, m);
        s->focus = -1;
        for (int i = 0; i < DSIM_FIELDS; i++)
            if (CheckCollisionPointRec(p, DSIM_FORM[i].box)) s->focus = i;
        if (CheckCollisionPointRec(p, DSIM_SUBMIT)) s->submitted = true;
    }
    if (d->keys > s->keys && s->focus >= 0) {
        unsigned c = d->last_key;
        char *t = s->text[s->focus];
        int n = (int)strlen(t);
        if (c == '\t') s->focus = (s->focus + 1) % DSIM_FIELDS;
        else if (c == '\b' && n > 0) t[n - 1] = 0;
        else if (c >= 32 && c < 127 && n < DSIM_TEXT - 1) { t[n] = (char)c; t[n + 1] = 0; }
    }
    s->clicks = d->clicks;
    s->keys = d->keys;
}

// ---- drawing ----

static void dsim_text(const char *t, float x, float y, int size, Color c)
{
    DrawText(t, (int)x, (int)y, size, c);
}

// the desktop with the form, (ox, oy) is the desktop px at the render targets top left
static void dsim_draw_desktop(const DriveSim *s, const SimLook *L, float ox, float oy, int w, int h, float time)
{
    DrawRectangle(0, 0, w, h, L->bg);
    int x0 = (int)floorf(ox / 40.0f) * 40, y0 = (int)floorf(oy / 40.0f) * 40;
    for (int y = y0; y < oy + h + 40; y += 40)
        for (int x = x0; x < ox + w + 40; x += 40)
            DrawRectangle(x - (int)ox, y - (int)oy, 2, 2, L->dots);

    Rectangle pg = { DSIM_PAGE.x - ox, DSIM_PAGE.y - oy, DSIM_PAGE.width, DSIM_PAGE.height };
    DrawRectangleRec(pg, L->panel);
    DrawRectangleLinesEx(pg, 1.0f, L->panel_edge);
    DrawRectangle((int)pg.x, (int)pg.y, (int)pg.width, 24, L->panel_edge);
    dsim_text("Sign up for the newsletter", pg.x + 10, pg.y + 7, 10, L->ink);
    for (int i = 0; i < DSIM_FIELDS; i++) {
        Rectangle b = { DSIM_FORM[i].box.x - ox, DSIM_FORM[i].box.y - oy, DSIM_FORM[i].box.width, DSIM_FORM[i].box.height };
        dsim_text(DSIM_FORM[i].label, b.x - 44, b.y + 9, 10, L->ink);
        DrawRectangleRec(b, L->bg);
        DrawRectangleLinesEx(b, 1.0f, s->focus == i ? (Color){ 60, 140, 240, 255 } : L->panel_edge);
        dsim_text(s->text[i], b.x + 6, b.y + 9, 10, L->ink);
        if (s->focus == i && fmodf(time, 1.0f) < 0.5f) {
            int tw = MeasureText(s->text[i], 10);
            DrawRectangle((int)(b.x + 7 + tw), (int)b.y + 6, 1, 16, L->ink);
        }
    }
    Rectangle sb = { DSIM_SUBMIT.x - ox, DSIM_SUBMIT.y - oy, DSIM_SUBMIT.width, DSIM_SUBMIT.height };
    Color fill = s->submitted ? (Color){ 60, 170, 90, 255 } : (Color){ 40, 120, 220, 255 };
    DrawRectangleRounded(sb, 0.3f, 4, fill);
    const char *label = s->submitted ? "Sent!" : "Submit";
    dsim_text(label, sb.x + (sb.width - MeasureText(label, 10)) / 2, sb.y + 11, 10, WHITE);
}

// his ask bubble drawn with the real ui code, the scripts aim your pointer at its yes and no
static void dsim_bubble(const Mascot *m, UiBubble *b)
{
    const char *q = m->point_phase != POINTER_HOME ? "Your turn: the password."
                  : m->drive_in.latched ? "Press Submit?" : "Carry on?";
    ui_bubble_set(b, q, UI_BUTTONS_YES_NO);
}

static void dsim_draw_bubble(const Mascot *m, Vector2 ptr_win)
{
    if (m->mood != MOOD_ASK) return;
    static UiBubble b;
    dsim_bubble(m, &b);
    Rectangle yes, session, no;
    ui_bubble_buttons(&b, SIM_WIN_W / 2.0f, DSIM_BUBBLE_BOTTOM, &yes, &session, &no);
    UiHover hover = CheckCollisionPointRec(ptr_win, yes) ? UI_HOVER_YES
                  : CheckCollisionPointRec(ptr_win, no) ? UI_HOVER_NO : UI_HOVER_NONE;
    UiBubbleLook look = { 0 };
    ui_bubble_draw(&b, SIM_WIN_W / 2.0f, DSIM_BUBBLE_BOTTOM, ui_bubble_page_chars(&b), hover, &look);
}

// whats inside the overlay window, top left at the current origin
static void dsim_draw_window(const DriveSim *s, const Mascot *m)
{
    PointerView v = { 0 };
    if (m->point_phase != POINTER_HOME) {             // handover, he points at a field like normal
        v.phase = (PointerPhase)m->point_phase;
        v.phase_t = m->point_t;
        v.travel = 1.0f;
        v.target = (Vector2){ SIM_FACE_X + m->point_dx, SIM_FACE_Y + m->point_dy };
        v.box = (Rectangle){ v.target.x - 12, v.target.y - 12, 24, 24 };
        for (int i = 0; i < DSIM_FIELDS; i++) {
            Rectangle b = DSIM_FORM[i].box;
            b.x -= s->win.x;
            b.y -= s->win.y;
            if (CheckCollisionPointRec(v.target, b)) v.box = b;
        }
        v.target_right = v.target.x > SIM_FACE_X;
        v.result = (PointerResult)m->point_result;
    }
    point_fx_draw_behind(&v, m, SIM_MASCOT_X, SIM_MASCOT_Y, MASCOT_PX, m->time);
    mascot_draw(m, SIM_MASCOT_X, SIM_MASCOT_Y, MASCOT_PX);
    point_fx_draw(&v, m, SIM_MASCOT_X, SIM_MASCOT_Y, MASCOT_PX, m->time);
    dsim_draw_bubble(m, (Vector2){ SIM_FACE_X + m->cursor_dx, SIM_FACE_Y + m->cursor_dy });
}

// your pointer, the usual white arrow with a black edge and its tip (the hotspot) at (x, y)
static void dsim_draw_pointer(float x, float y)
{
    static const char *ARROW[] = {
        "#...........",
        "##..........",
        "#o#.........",
        "#oo#........",
        "#ooo#.......",
        "#oooo#......",
        "#ooooo#.....",
        "#oooooo#....",
        "#ooooooo#...",
        "#oooooooo#..",
        "#ooooo#####.",
        "#oo#oo#.....",
        "#o#.#oo#....",
        "##..#oo#....",
        "#....#oo#...",
        ".....#oo#...",
        "......##....",
    };
    for (int j = 0; j < 17; j++)
        for (int i = 0; i < 12; i++) {
            char c = ARROW[j][i];
            if (c == '.') continue;
            DrawRectangle((int)x + i, (int)y + j, 1, 1, c == '#' ? BLACK : WHITE);
        }
}

// showreel caption for whats going on
static const char *dsim_caption(const Mascot *m)
{
    const DriveInputs *d = &m->drive_in;
    const WheelState *w = &m->wheel;
    if (w->pause_left > 0.0f) return "you grabbed the mouse: \"oh, it's yours!\"";
    if (w->result_left > 0.0f) return w->result == DRIVE_RESULT_VICTORY ? "done: the victory hop" : "stopped: sheepish";
    switch (d->phase) {
    case DRIVE_ANIM_FETCH: return d->latched ? "walking to your pointer to grab it again" : "fetching your pointer";
    case DRIVE_ANIM_CARRY: return w->hauling ? "carrying it: a long haul" : "carrying it";
    case DRIVE_ANIM_ACT: return "click!";
    case DRIVE_ANIM_TYPE: return "typing on his keyboard";
    case DRIVE_ANIM_HOLD: return w->gripped && w->grip_t < 0.4f ? "grab!" : "holding on";
    case DRIVE_ANIM_WAIT: return m->point_phase != POINTER_HOME ? "a handover: he points, you do it"
                               : d->latched ? "waiting on you: the cable trails your hand" : "waiting on you";
    case DRIVE_ANIM_PAUSED: return "you have it";
    case DRIVE_ANIM_DONE: return "walking home";
    default: return "";
    }
}
