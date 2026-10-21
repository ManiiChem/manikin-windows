#pragma once

#include <stdbool.h>

// anim_gaze.c state (eyes following the cursor), index 0 is the left eye and 1 the right
typedef struct {
    bool inited;
    float px[2], py[2];         // simulated pupil offsets, art px
    float vx[2], vy[2];         // pupil velocity, art px/s
    int qx[2], qy[2];           // the offsets that actually get drawn (rounded with hysteresis)
    float saccade;              // > 0 during a quick jump that overshoots a bit
    float head, head_v;         // face shifts a pixel toward a pointer way off to the side
    int qhead;
    float lean, lean_v;         // whole body leans a pixel toward a really far pointer
    int qlean;
    float anchor_x, anchor_y;   // where the pointer was when it last moved
    float still_for;            // secs since the pointer last moved
    float last_x, last_y;
    float speed;                // smoothed pointer speed, screen px/s
    float attention;            // 1 = watching the pointer, 0 = bored (doing his own glances)
    float bored_t;              // secs since he lost interest
    float near;                 // 0..1 pointer hovering close (smoothed)
    float on_face;              // 0..1 pointer on his screen face (smoothed)
    float cross;                // 0..1 pointer right between his eyes (smoothed)
    bool was_on_face;
    float flinch;               // happy squeeze timer, pointer landed on his face
    float flinch_cool;          // so brushing back and forth doesnt keep setting it off
    float perk;                 // "oh, there you are!" timer once he gets interested again
} GazeState;
