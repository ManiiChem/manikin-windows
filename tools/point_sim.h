#pragma once

// fake pointing trips for preview and showreel, standing in for pointer.c. a trip walks the
// window from home to beside the target, points, gets a result and walks back, making the
// same PointerView and Mascot inputs the real thing would

#include <math.h>

#include "raylib.h"
#include "mascot.h"
#include "pointer.h"
#include "point_fx.h"

#define SIM_WIN_W 420
#define SIM_WIN_H 582
#define SIM_MASCOT_X ((SIM_WIN_W - MASCOT_W * MASCOT_PX) / 2)
#define SIM_MASCOT_Y 270
#define SIM_FACE_X (SIM_MASCOT_X + 20 * MASCOT_PX)    // middle of his screen face, window px
#define SIM_FACE_Y (SIM_MASCOT_Y + 16 * MASCOT_PX)
#define SIM_LEAD_IN 0.4f

typedef enum { CTL_BUTTON, CTL_CHECKBOX, CTL_ROW } ControlKind;

typedef struct {
    const char *name;
    Vector2 screen;         // fake screen size
    Vector2 home;           // window top left at home, screen px
    Rectangle box;          // the control, screen px
    Vector2 spot;           // where to touch, from the box middle
    ControlKind kind;
    const char *label;
    float out_s;            // secs walking out
    float at_s;             // secs pointing before the result
    PointerResult result;   // NONE keeps pointing till the end
    float linger;           // secs still AT after the result (0 means the result comes with BACK)
    float back_s;           // secs walking home
    float talk_from, talk_to;   // secs into AT where hes talking ("there it is!")
    float stand;            // px from his face to the target (0 means 150)
} PointTrip;

static Vector2 sim_target(const PointTrip *p)
{
    return (Vector2){ p->box.x + p->box.width * 0.5f + p->spot.x, p->box.y + p->box.height * 0.5f + p->spot.y };
}

// where the window ends up, his face p->stand px (150 if 0) beside the target, kept on screen
static Vector2 sim_dest(const PointTrip *p)
{
    Vector2 t = sim_target(p);
    bool right = t.x > p->home.x + SIM_FACE_X;
    float off = p->stand > 0.0f ? p->stand : 150.0f;
    float fx = t.x + (right ? -off : off), fy = t.y - 4.0f;
    float x = fx - SIM_FACE_X, y = fy - SIM_FACE_Y;
    x = fminf(fmaxf(x, 0.0f), p->screen.x - SIM_WIN_W);
    y = fminf(fmaxf(y, 0.0f), p->screen.y - SIM_WIN_H);
    return (Vector2){ x, y };
}

static float sim_ease(float k) { return k * k * (3.0f - 2.0f * k); }
static float sim_ease_d(float k) { return 6.0f * k * (1.0f - k); }

static float sim_length(const PointTrip *p)
{
    return SIM_LEAD_IN + p->out_s + p->at_s + p->linger + p->back_s + 1.0f;
}

// the view at time t, win gets where the window is (screen px)
static PointerView sim_view(const PointTrip *p, float t, Vector2 *win)
{
    PointerView v = { 0 };
    Vector2 a = p->home, b = sim_dest(p);
    const float t1 = SIM_LEAD_IN, t2 = t1 + p->out_s, t3 = t2 + p->at_s;
    const float t4 = t3 + (p->result ? p->linger : 1e9f), t5 = t4 + p->back_s;
    *win = a;
    if (t < t1) {
        v.phase = POINTER_HOME;
        v.phase_t = t;
    } else if (t < t2) {
        float k = (t - t1) / p->out_s;
        v.phase = POINTER_OUT;
        v.phase_t = t - t1;
        v.travel = sim_ease(k);
        win->x = a.x + (b.x - a.x) * v.travel;
        win->y = a.y + (b.y - a.y) * v.travel;
        v.vel = (Vector2){ (b.x - a.x) * sim_ease_d(k) / p->out_s, (b.y - a.y) * sim_ease_d(k) / p->out_s };
    } else if (t < t4) {
        v.phase = POINTER_AT;
        v.phase_t = t - t2;
        v.travel = 1.0f;
        *win = b;
    } else if (t < t5) {
        float k = (t - t4) / p->back_s;
        v.phase = POINTER_BACK;
        v.phase_t = t - t4;
        v.travel = sim_ease(k);
        win->x = b.x + (a.x - b.x) * v.travel;
        win->y = b.y + (a.y - b.y) * v.travel;
        v.vel = (Vector2){ (a.x - b.x) * sim_ease_d(k) / p->back_s, (a.y - b.y) * sim_ease_d(k) / p->back_s };
    } else {
        v.phase = POINTER_HOME;
        v.phase_t = t - t5;
    }
    if (v.phase != POINTER_HOME || t < t1) {
        Vector2 tg = sim_target(p);
        v.target = (Vector2){ tg.x - win->x, tg.y - win->y };
        v.box = (Rectangle){ p->box.x - win->x, p->box.y - win->y, p->box.width, p->box.height };
        v.target_right = v.target.x > SIM_FACE_X;
    }
    if (p->result && t >= t3 && t < t5) {
        v.result = p->result;
        v.result_t = t - t3;
    }
    if (v.phase == POINTER_HOME && t < t1) v.box = (Rectangle){ 0 };
    return v;
}

// copies the view into the Mascot like the real overlay does
static void sim_apply(const PointTrip *p, Mascot *m, const PointerView *v, float t)
{
    m->point_phase = v->phase;
    m->point_t = v->phase_t;
    m->point_result = v->result;
    m->point_dx = v->phase == POINTER_HOME ? 0.0f : v->target.x - SIM_FACE_X;
    m->point_dy = v->phase == POINTER_HOME ? 0.0f : v->target.y - SIM_FACE_Y;
    m->walk_vx = v->vel.x;
    m->walk_vy = v->vel.y;
    if (v->phase == POINTER_OUT || v->phase == POINTER_AT) {
        m->cursor_dx = m->point_dx;
        m->cursor_dy = m->point_dy;
    } else {
        m->cursor_dx = 40.0f;
        m->cursor_dy = 260.0f;     // your mouse, somewhere below him
    }
    float at = t - (SIM_LEAD_IN + p->out_s);
    m->mood = v->phase == POINTER_AT && at >= p->talk_from && at < p->talk_to ? MOOD_TALKING : MOOD_IDLE;
}

// ---- the fake desktop ----

typedef struct {
    Color bg, dots, panel, panel_edge, ink;
} SimLook;

static const SimLook SIM_DARK = { { 32, 32, 40, 255 }, { 52, 52, 64, 255 }, { 44, 46, 56, 255 }, { 70, 72, 86, 255 }, { 220, 222, 230, 255 } };
static const SimLook SIM_LIGHT = { { 225, 225, 230, 255 }, { 200, 200, 208, 255 }, { 243, 243, 246, 255 }, { 190, 190, 200, 255 }, { 30, 30, 40, 255 } };

// dotted desktop (so you can see him travel) with the control on it, (ox, oy) is the
// screen px at the render targets top left
static void sim_draw_desktop(const PointTrip *p, const SimLook *L, float ox, float oy, int w, int h)
{
    DrawRectangle(0, 0, w, h, L->bg);
    int x0 = (int)floorf(ox / 40.0f) * 40, y0 = (int)floorf(oy / 40.0f) * 40;
    for (int y = y0; y < oy + h + 40; y += 40)
        for (int x = x0; x < ox + w + 40; x += 40)
            DrawRectangle(x - (int)ox, y - (int)oy, 2, 2, L->dots);

    Rectangle b = { p->box.x - ox, p->box.y - oy, p->box.width, p->box.height };
    Vector2 t = sim_target(p);
    switch (p->kind) {
    case CTL_BUTTON:
        DrawRectangleRounded(b, 0.25f, 4, L->panel);
        DrawRectangleRoundedLines(b, 0.25f, 4, L->panel_edge);
        if (p->label) {
            int tw = MeasureText(p->label, 10);
            DrawText(p->label, (int)(b.x + (b.width - tw) / 2), (int)(b.y + b.height / 2 - 5), 10, L->ink);
        }
        break;
    case CTL_CHECKBOX:
        DrawRectangleRec(b, L->panel);
        DrawRectangleLinesEx(b, 1.0f, L->ink);
        if (p->label) DrawText(p->label, (int)(b.x + b.width + 6), (int)(b.y + b.height / 2 - 5), 10, L->ink);
        break;
    case CTL_ROW:
        DrawRectangleRec(b, L->panel);
        DrawRectangleLinesEx(b, 1.0f, L->panel_edge);
        if (p->label) DrawText(p->label, (int)(b.x + 14), (int)(b.y + b.height / 2 - 5), 10, L->ink);
        // a toggle at the spot
        DrawRectangleRounded((Rectangle){ t.x - ox - 18, t.y - oy - 9, 36, 18 }, 1.0f, 8, (Color){ 40, 120, 220, 255 });
        DrawCircle((int)(t.x - ox + 9), (int)(t.y - oy), 6, WHITE);
        break;
    }
}

// whats inside the overlay window, top left at the current origin
static void sim_draw_window(const PointerView *v, const Mascot *m)
{
    point_fx_draw_behind(v, m, SIM_MASCOT_X, SIM_MASCOT_Y, MASCOT_PX, m->time);
    mascot_draw(m, SIM_MASCOT_X, SIM_MASCOT_Y, MASCOT_PX);
    point_fx_draw(v, m, SIM_MASCOT_X, SIM_MASCOT_Y, MASCOT_PX, m->time);
}

// ---- the trips ----

#define SCREEN { 1920.0f, 1080.0f }

static const PointTrip TRIP_RIGHT = {
    "point_right", SCREEN, { 300, 380 }, { 1080, 690, 92, 30 }, { 0, 0 }, CTL_BUTTON, "Save",
    0.75f, 2.4f, POINTER_RESULT_CLEARED, 0.0f, 0.75f, 0.9f, 1.8f, 0.0f,
};
static const PointTrip TRIP_LEFT = {
    "point_left", SCREEN, { 900, 380 }, { 560, 700, 110, 30 }, { 0, 0 }, CTL_BUTTON, "Settings",
    0.75f, 2.4f, POINTER_RESULT_DISMISSED, 0.0f, 0.75f, 0, 0,
};
static const PointTrip TRIP_UPLEFT = {      // screen corner, he gets clamped below right of it
    "point_up_left", SCREEN, { 500, 300 }, { 14, 118, 90, 28 }, { 0, 0 }, CTL_BUTTON, "File",
    0.8f, 2.4f, POINTER_RESULT_CLEARED, 0.0f, 0.8f, 0, 0,
};
static const PointTrip TRIP_DOWN = {        // bottom edge, target under him
    "point_down", SCREEN, { 900, 300 }, { 1180, 1040, 64, 28 }, { 0, 0 }, CTL_BUTTON, "Start",
    0.8f, 2.4f, POINTER_RESULT_CLEARED, 0.0f, 0.8f, 0, 0,
};
static const PointTrip TRIP_TINY = {
    "point_tiny", SCREEN, { 600, 380 }, { 1110, 700, 14, 14 }, { 0, 0 }, CTL_CHECKBOX, "Remember me",
    0.75f, 2.4f, POINTER_RESULT_CLEARED, 0.0f, 0.75f, 0, 0,
};
static const PointTrip TRIP_HUGE = {        // a settings row much wider than the window
    "point_huge", SCREEN, { 500, 380 }, { 200, 690, 1500, 44 }, { 420, 0 }, CTL_ROW, "Bluetooth",
    0.75f, 2.4f, POINTER_RESULT_CLEARED, 0.0f, 0.75f, 0, 0,
};
static const PointTrip TRIP_CLICK = {
    "point_click", SCREEN, { 600, 380 }, { 1080, 690, 92, 30 }, { 0, 0 }, CTL_BUTTON, "Save",
    0.75f, 1.6f, POINTER_RESULT_CLICKED_IN, 0.4f, 0.9f, 0, 0,
};
static const PointTrip TRIP_CLICK_OUT = {
    "point_click_out", SCREEN, { 600, 380 }, { 1080, 690, 92, 30 }, { 0, 0 }, CTL_BUTTON, "Save",
    0.75f, 1.6f, POINTER_RESULT_CLICKED_OUT, 0.4f, 0.9f, 0, 0,
};
static const PointTrip TRIP_MOVED = {
    "point_moved", SCREEN, { 900, 380 }, { 560, 700, 110, 30 }, { 0, 0 }, CTL_BUTTON, "Settings",
    0.75f, 1.6f, POINTER_RESULT_MOVED, 0.3f, 0.8f, 0, 0,
};
static const PointTrip TRIP_SLOW = {        // a short hop (~300 px/s)
    "walk_short", SCREEN, { 500, 380 }, { 1080, 690, 92, 30 }, { 0, 0 }, CTL_BUTTON, "Save",
    1.0f, 1.2f, POINTER_RESULT_CLICKED_IN, 0.4f, 1.0f, 0, 0,
};
static const PointTrip TRIP_FAST = {        // a long dash (peaks ~3000 px/s)
    "walk_long", SCREEN, { 1450, 300 }, { 60, 690, 110, 30 }, { 0, 0 }, CTL_BUTTON, "Back",
    0.62f, 1.4f, POINTER_RESULT_CLICKED_IN, 0.4f, 0.75f, 0, 0,
};
static const PointTrip TRIP_FAR = {         // stands further off, long reach
    "point_far", SCREEN, { 900, 380 }, { 400, 650, 80, 30 }, { 0, 0 }, CTL_BUTTON, "Undo",
    0.75f, 2.4f, POINTER_RESULT_CLEARED, 0.0f, 0.75f, 0, 0, 190.0f,
};
static const PointTrip TRIP_CLICK_GO = {    // result comes with BACK, no lingering
    "point_click_go", SCREEN, { 600, 380 }, { 1080, 690, 92, 30 }, { 0, 0 }, CTL_BUTTON, "Save",
    0.75f, 1.6f, POINTER_RESULT_CLICKED_IN, 0.0f, 0.9f, 0, 0, 0,
};
static const PointTrip TRIP_MOVED_GO = {
    "point_moved_go", SCREEN, { 900, 380 }, { 560, 700, 110, 30 }, { 0, 0 }, CTL_BUTTON, "Settings",
    0.75f, 1.6f, POINTER_RESULT_MOVED, 0.0f, 0.8f, 0, 0, 0,
};
