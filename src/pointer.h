#pragma once

#include <stdbool.h>
#include "raylib.h"

// pointing (BRAIN_EV_POINT). he walks over to something on screen, points at it and walks
// home once the user clicks or the window changes. point_fx.c and the mascot only read PointerView

typedef enum {
    POINTER_HOME,           // not pointing (he might be wherever the user dragged him)
    POINTER_OUT,            // walking to the target
    POINTER_AT,             // standing beside it, arm out
    POINTER_BACK,           // walking home
} PointerPhase;

typedef enum {
    POINTER_RESULT_NONE,        // still pointing (or never was)
    POINTER_RESULT_CLICKED_IN,  // user clicked the target
    POINTER_RESULT_CLICKED_OUT, // user clicked somewhere else
    POINTER_RESULT_MOVED,       // target window moved, resized, closed or lost focus
    POINTER_RESULT_DISMISSED,   // user grabbed or poked him, or hit Esc
    POINTER_RESULT_CLEARED,     // brain called it off (timeout, turn cancelled, replaced)
} PointerResult;

// everything the visuals need for one frame, in window px (same space as mascot_draw) so it
// shifts as the window walks. the target can even be above or below him at a screen edge
typedef struct {
    PointerPhase phase;
    float phase_t;          // secs since this phase started
    float travel;           // eased 0..1 progress of the trip in OUT/BACK, 1 in AT
    Vector2 vel;            // window velocity this frame px/s (0 unless walking)
    Vector2 target;         // the spot to touch (OUT/AT/BACK)
    Rectangle box;          // the controls bounds, can be way bigger than the window
    bool target_right;      // target is to his right (else left)
    PointerResult result;   // how the point ended, set when BACK starts (or late in AT)
    float result_t;         // secs since result was set
} PointerView;

PointerView pointer_view(void);

// a control bigger than this either way (a settings row, a toolbar) only gets a FOCUS_W x
// FOCUS_H patch around the target marked and kept clear of him (placement and drawing)
#define POINTER_BIG_BOX 240.0f
#define POINTER_FOCUS_W 84.0f
#define POINTER_FOCUS_H 54.0f

// ---- driving it (main.c)
// pointer.c only does the math and asks platform.h about other windows, main.c moves the
// window and runs the chat box and bubble. desktop px below unless it says window px

#define POINTER_SOLID_MAX 8

// the overlays layout so pointer.c doesnt have to repeat the main.c numbers
typedef struct {
    int win_w, win_h;
    // window px, everything that can catch the mouse at its biggest (bubble full width, chat
    // box full height). the target stays off these so the user can click it through the window
    Rectangle solid[POINTER_SOLID_MAX];
    int solid_count;
    // window px, where the target ideally sits (beside him at arm height)
    Vector2 spot_left, spot_right;
} PointerLayout;

// own_window is platform_window_id() of the overlay, (win_x, win_y) is where it is now
void pointer_init(const PointerLayout *layout, long long own_window, int win_x, int win_y);

// the brains "point", walk over so (x, y) sits beside him. box is the controls bounds,
// window is its window which gets watched while hes there (0 = unknown, not watched).
// replaces a point still in progress (that one gets reported "cleared")
void pointer_point(int id, int x, int y, Rectangle box, long long window);
// the brains "point_clear", ignored unless `id` is the point still in progress
void pointer_clear(int id);
// calls off whatever point is in progress (the brain went away)
void pointer_clear_active(void);
// Esc, the user doesnt need it. does nothing unless a point is in progress
void pointer_dismiss(void);
// a point is in progress, hes on his way or there and nothing has ended it yet
bool pointer_pointing(void);
// he isnt home (OUT, AT or BACK)
bool pointer_away(void);

typedef struct {
    // in
    int win_x, win_y;       // where the window is now
    int cursor_x, cursor_y; // the mouse
    bool pressed;           // platform_mouse_pressed() this frame (call it every frame) but never
                            // a press he might have made himself (drive_injected_within)
    bool on_ui;             // cursor is over his solid UI, a press there is a poke
    bool held;              // mouse down on him but not dragging yet, hold still
    bool dragging;          // user is dragging him, he stays where hes dropped
    bool keep_chat;         // chat box has to stay open (password prompt)
    // in/out
    bool chat_open;         // closed while hes away, reopened once hes home
    // out
    bool move;              // SetWindowPosition(win_x, win_y) this frame
} PointerFrame;

void pointer_update(float dt, PointerFrame *f);

// the placement on its own (pure, pointer_point uses it). gives the window position that
// puts `target` beside him, off his solid UI and inside the work area (wa_*), on whichever
// side is the shorter walk from (from_x, from_y) when both fit
void pointer_place(const PointerLayout *l, int wa_x, int wa_y, int wa_w, int wa_h,
                   Vector2 target, Rectangle box, int from_x, int from_y, int *x, int *y);

// ---- the walk (drive.c uses it too), eased in and out, longer trips take longer up to a
// limit. the caller adds each frames dt to `t`
typedef struct {
    float from_x, from_y, to_x, to_y;
    float t, dur;                // secs so far, secs it takes
} PointerTrip;

// dist is the distance to time it by (0 = the windows own, a carry passes the pointers
// when thats further)
PointerTrip pointer_trip(float from_x, float from_y, float to_x, float to_y, float dist);
// eased progress (0..1) plus where the window is and how fast its going (px/s) right now.
// x, y and vel can be NULL
float pointer_trip_at(const PointerTrip *trip, float *x, float *y, Vector2 *vel);
bool pointer_trip_done(const PointerTrip *trip);
