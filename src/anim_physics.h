#pragma once

#include <stdbool.h>

// anim_physics.c state (getting dragged and shaken). all zero = not held, settled and
// nothing playing. the *_left timers count down, 0 = off
typedef struct {
    bool was_held;          // m->held last update, for catching the edges
    float held_t;           // secs since he got picked up (only valid while held)

    float svx, svy;         // smoothed window velocity, screen px/s
    bool fast;              // fast drag look (with hysteresis)

    // damped springs in art px (value, velocity)
    float ox, oxv;          // body sway / lean, horizontal -> Pose.shift_x
    float oy, oyv;          // body sway, vertical         -> Pose.shift_y
    float sq, sqv;          // squash / stretch            -> Pose.bob
    float pux, puxv;        // pupil lag, horizontal
    float puy, puyv;        // pupil lag, vertical
    float la, lav;          // left arm raise, 0 = resting on the keys, 1 = all the way up
    float ra, rav;          // right arm raise

    // shake detection
    float energy;           // goes up on every fast direction flip, decays over time
    int dir_x, dir_y;       // sign of the last fast motion on each axis (0 = none)
    float quiet_x, quiet_y; // secs since the last fast motion on each axis

    float dizzy_left;       // > 0 while dizzy (spiral eyes, stars)
    float shakeoff_left;    // > 0 while hes shaking it off after being dizzy
    float land_left;        // > 0 during the landing squash and the relieved look
} PhysicsState;
