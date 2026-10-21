#pragma once

#include <stdbool.h>

// state for anim_wheel.c (driving the real pointer, the cable on it, clicking, typing on
// his keyboard, the victory lap). inputs are m->drive_in, all zero (mascot_init) means not
// driving. timers count down to 0 = inactive so the zeroed state is a quiet one

#define WHEEL_LIT 4             // keys glowing at once (the latest brightest)

typedef struct {
    int last_phase;         // m->drive_in.phase last update, to catch edges
    int last_clicks;        // m->drive_in.clicks / keys last update, to raise MEV_CLACK / MEV_KEY
    int last_keys;
    bool driving;           // m->drive_in.phase wasnt NONE last update
    float phase_t;          // secs in the current drive phase (own clock)
    bool oops;              // this update only, grabbed or sheepish (cues.c plays SFX_OOPS, no MEV bit left)

    // the cable arm on the pointer (point_fx.c draws it)
    int side;               // arm thats out or going out, -1 left, +1 right, 0 none
    bool latched;           // m->drive_in.latched seen last update
    bool held;              // latched while steering too (the grab is its rising edge)
    bool snap;              // arm is snapping back (user grabbed the mouse)
    float reach, reach_v;   // extension spring, 0 plugged into him .. 1 at the pointer
    bool out;               // arm is unplugged (Pose.arm_out)
    bool gripped;           // hes taken hold (plug on the pointer, steering)
    float grip_t;           // secs since he did
    float aim_x, aim_y;     // plug (pointer) from the middle of the face, screen px, frozen when he lets go
    float tension, tension_v;   // 0 slack and trailing your pointer .. 1 taut, hes steering
    float droop_x, droop_y;     // cables middle off the straight line, screen px (sags, lags, swings)
    float droop_vx, droop_vy;
    float click_left;       // > 0 right after a click, the jab (counts down from WHEEL_CLICK)
    int clicks_due;         // clicks still to show (a double click is 2 jabs a beat apart)
    float click_wait;       // > 0, the next queued click waits this long
    float haul;             // m->drive_in.haul smoothed
    bool hauling;           // a big haul is on (MEV_HAUL once per haul)
    float haul_t;           // secs into it

    // typing on his keyboard base, the free hand works the keys
    int key[WHEEL_LIT];     // recently struck keys (WHEEL_KEY_*), newest at key_next - 1
    float key_left[WHEEL_LIT];   // secs each one keeps glowing
    int key_next;
    float strike_left;      // > 0 while the fingers are down on a key
    float type_left;        // > 0 keeps the hand on the keys a bit after the last key
    float hand_x, hand_v;   // typing hands left column on the canvas (spring)
    bool typing;            // hand is off its rest, over the keys

    // how it ended
    int result;             // m->drive_in.result when DONE started (a DriveResult)
    float result_left;      // > 0 while the victory / sheepish reaction plays (counts down)
    float pause_left;       // > 0 during "oh its yours!" (counts down)

    float pux, puy;         // smoothed pupil offset, art px
} WheelState;

// keys on his keyboard base, as anim_wheel.c lights them and mascot.c draws them.
// 0..27 go left to right over the 2 rows of key caps (key i is cells 6+i and 7+i of row
// 36 + (i & 1)), then the space bar and Enter
#define WHEEL_KEY_SPACE 28
#define WHEEL_KEY_ENTER 29
#define WHEEL_KEYS 30

#define WHEEL_CLICK 0.22f       // how long a clicks jab lasts (WheelState.click_left)
#define WHEEL_PAUSE 1.3f        // secs of "oh its yours!" (WheelState.pause_left)
#define WHEEL_VICTORY 1.2f      // secs of the victory reaction, drive.c lingers in DONE this long too
#define WHEEL_VICTORY_HOP 0.16f // the hop (and jingle) starts this far into it
#define WHEEL_SHEEPISH 0.7f     // the sheepish one (the walk home stays sheepish)

// how `key` glows right now, 'Y' just struck, 'O' fading, 0 not lit
char wheel_key_glow(const WheelState *w, int key);

// the free hand typing (mascot.c draws that arm reaching over the keys). gives which hand
// (-1 left, +1 right) and the canvas cell of its top left, the fingers are its bottom row
// (on a key while striking, hovering over the keys otherwise). false when no hand is on them
bool wheel_typing_hand(const WheelState *w, int *side, int *x, int *y);
