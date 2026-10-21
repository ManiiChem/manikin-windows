#pragma once

#include <stdbool.h>

// state for anim_point.c (walking over to something on screen and pointing at it).
// all zero = inactive since mascot_init() memsets the Mascot

#define POINT_PUFFS 12

// dust puff kicked up by a foot. it stays put on screen while the window walks away, so
// its window position is where it got kicked up minus how far the window went since
typedef struct {
    float x, y;             // where it got kicked up, canvas art px
    float trip_x, trip_y;   // PointState.trip_x/y at that moment
    float left;             // secs of life left (0 = unused)
    float life;
    int big;                // landing puff (bigger, drifts outward)
    int drift;              // -1 / +1, which way it drifts
} PointPuff;

typedef struct {
    int phase;              // m->point_phase seen last update (a PointerPhase)
    int result;             // m->point_result seen last update
    float result_t;         // secs since point_result went non zero

    // the stretched out arm
    int side;               // arm thats out or going out, -1 left, +1 right, 0 none
    float reach, reach_v;   // extension spring, 0 plugged in .. 1 at the target (overshoots)
    bool locked;            // plug reached the target this time
    float lock_t;           // secs since it did (the ring locks on)
    bool out;               // arm is unplugged (Pose.arm_out)
    float in_t;             // secs since it plugged back in (hand comes down)
    bool latched;           // aim frozen, the points over and the arm retracts along it
    float aim_x, aim_y;     // where the arm points from the middle of the face, screen px
    float aim_trip_x, aim_trip_y;   // trip_x/y when the aim froze

    // travelling
    bool drive_walk;        // walking the real pointer around (m->drive_in), same walk
    float drive_still;      // how long the window has been still (that walk ends after a beat)
    float trip_x, trip_y;   // window movement added up over every trip, screen px
    float vx, vy;           // smoothed window velocity px/s
    float stride;           // foot cycle, +1 per step
    int dir;                // facing while travelling, -1 left, +1 right
    float lean, lean_v;     // body lean spring, art px (+ = to his right)
    float sq, sq_v;         // squash spring, rows (+ = squashed down)
    float settle_left;      // > 0 while settling after a trip (counts down)
    bool settle_home;       // that trip ended at home (else beside the target)

    float pux, puy;         // smoothed pupil offset, art px

    PointPuff puffs[POINT_PUFFS];
    int next_puff;
} PointState;

// marks over his head (point_fx.c draws them). one function decides them for the drawing and
// the sounds (MEV_POINT_HUH, MEV_POINT_BANG, MEV_WHISTLE) so both start on the same frame
typedef enum { POINT_MARK_NONE, POINT_MARK_HUH, POINT_MARK_BANG, POINT_MARK_NOTE } PointMark;

#define POINT_NOTE_FROM  0.35f   // secs after a click before he starts whistling (walking home)
#define POINT_NOTE_EVERY 1.3f    // a new note this often
#define POINT_NOTE_LEN   0.9f    // each one floats up for this long

// from the PointerPhase, the PointerResult and secs since that result came in.
// for POINT_MARK_NOTE, `note` (if not NULL) gets which note it is, 0, 1, 2..
PointMark point_head_mark(int phase, int result, float result_t, int *note);

// the scuttle, shared with anim_wheel.c (driving walks the same walk and point_pose moves
// the feet and body for both). walking this frame, and which of the 4 walk frames it is,
// 0 contact (lead foot ahead), 1 passing, 2 contact (feet swapped), 3 passing
bool point_walking(const PointState *s);
int point_walk_frame(const PointState *s);
