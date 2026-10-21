#pragma once

// he takes the wheel. drives the real mouse and keyboard one brain "drive" action at a
// time, plugs his cable into the pointer, walks it to the spot and clicks, scrolls or types
// for real. no raylib calls so tools/drive_test.c can run it against a fake platform

// phases (DriveAnimPhase) go FETCH walk over and plug in, CARRY walk it to the spot, ACT
// click/drag/scroll, TYPE typing, HOLD wait on the brain, WAIT the users turn (ask or handover,
// the cable trails their hand and the next action fetches the pointer again), PAUSED the
// user grabbed the mouse back, DONE let go and go home

// while he steers (CARRY ACT TYPE HOLD) the pointer stays pinned to his plug. if it drifts
// more than 3 px or theres a click he didnt make, the user grabbed it and its theirs again

#include <stdbool.h>

#include "brain.h"
#include "mascot.h"
#include "pointer.h"

// same layout pointer.c gets so each spot lands at his arm, clear of his UI.
// own_window is platform_window_id() of the overlay, he never types into that
void drive_init(const PointerLayout *layout, long long own_window, int win_x, int win_y);

// the brains "drive", one action. always answered by exactly one drive_done, right away if
// it cant run (another ones still going, its unknown, it would type into his own window,
// the user grabbed the mouse while he held it, or Esc stopped this drive)
void drive_action(const BrainEvent *ev);
// the brains "drive_end", "done" (victory lap) or "stopped" / "failed" (sheepish).
// he lets go and walks back to where he was when the drive started
void drive_end(const char *result);
// Esc in his chat box (drive_update polls Esc globally for everywhere else). either way the
// action in progress gets "cancelled" and a cancel goes to the brain. no-op unless driving
void drive_cancel(void);
// the brains turn is over (reply, error, "cancelled", or it died). a drive still going
// ends sheepish, and a cancelled drives late actions are done arriving
void drive_turn_over(void);

bool drive_driving(void);       // first action until drive_end or Esc
bool drive_away(void);          // driving, or still walking home after
bool drive_steering(void);      // hes moving the pointer, keep the window click-through
// he sent some input (move, click, key) in the last `seconds`. a press or a click on his
// bubble that soon could be his own and not the users
bool drive_injected_within(float seconds);

typedef struct {
    // in
    int win_x, win_y;       // where the window is now
    bool pressed;           // platform_mouse_pressed() this frame
    bool held;              // mouse down on him but not dragging yet, hold still
    bool dragging;          // user is dragging him, he stays where hes dropped
    bool asking;            // an ask is up, users turn so he stops steering
    bool pointer_away;      // pointer.c has the window (handover point), he lets go
    // in/out
    bool chat_open;         // closed when a drive starts, reopened once hes home
    // out
    bool move;              // SetWindowPosition(win_x, win_y) this frame
    bool cancelled;         // Esc stopped him this frame (for the cancel sound)
} DriveFrame;

void drive_update(float dt, DriveFrame *f);

typedef struct {
    DriveInputs in;         // goes into Mascot.drive_in, all zero when hes not driving
    int plug_x, plug_y;     // desktop px of the pointer (his plug tip while he steers)
    Vector2 vel;            // window velocity while he walks, px/s
} DriveView;

DriveView drive_view(void);
