#pragma once

// his drawing kit and the interface the anim modules plug into. only mascot.c and the
// anim_*.c files use this, the rest of the app just needs mascot.h

#include <stdbool.h>
#include "raylib.h"
#include "mascot.h"

// canvas
// MASCOT_W x MASCOT_H art pixels (40 x 48), each one MASCOT_PX (3) screen pixels wide.
// leave column/row 0 and the last column/row empty, the auto outline and the white halo
// need that margin. x goes right, y goes down
//
// palette letters (the actual RGB is in mascot.c):
//   K outline        H highlight/white   W casing light   G casing mid   S casing shade
//   M dark metal     D screen dark       p pupil          T screen teal  U screen glint
//   d scanline       g glow bleed        O orange glow    Y hot yellow
//   L red LED        l red LED off       E green          e green off
//   B blue wire      R red wire          P orange wire
// extra colours only the anim modules use:
//   k blush pink     h heart red         w white sparkle  q tear/sweat blue
//   v dizzy violet   z pale blue-white (sleep z's)
// '.' is empty. once the face is drawn every 'O'/'Y' cell on the glass tints its teal
// neighbours to 'g' so anything glowing on the screen gets a soft halo for free.
// any empty cell touching art turns into dark outline automatically

void mk_put(int x, int y, char c);          // clipped to the canvas
char mk_get(int x, int y);                  // gives '.' outside the canvas
void mk_fill(int x, int y, int w, int h, char c);
void mk_hline(int x, int y, int w, char c);
void mk_vline(int x, int y, int h, char c);
void mk_line(int x0, int y0, int x1, int y1, char c);                // Bresenham
void mk_stamp(int x, int y, const char *rows[], int h, bool flip);  // '.' is see through

typedef enum { EYE_OPEN, EYE_SHUT, EYE_HAPPY } EyeShape;

// the standard 7x9 glowing eye with its top left at (x, y). orange rim, cream fill and a
// 3x4 pupil pushed (pdx, pdy) from the centre (about -2..2 each way). lid cuts that many
// rows off the top (flat sleepy/skeptical lid), low cuts them off the bottom.
// EYE_SHUT is a soft closed curve, EYE_HAPPY an upside down U (^)
void mk_eye(int x, int y, EyeShape shape, int pdx, int pdy, int lid, int low);

// what the hooks get

// whole body tweaks, decided before anything gets drawn
typedef struct {
    int bob;                // rows the upper body sinks (-1..3), the keyboard base stays put
    int shift_x, shift_y;   // moves the whole sprite in art px (shake, hop, tremble)
    int lean;               // upper body (rows above the keyboard) leans this many art px
                            // sideways over the base (-1..1), for walking and reaching
    int lift;               // rows everything but the feet goes up (0..2), the walking bounce
    int foot_dx[2], foot_dy[2];   // left/right foot offsets in art px (dy < 0 = lifted)
    int arm_out;            // -1/+1 = that arm is unplugged and stretched out, point_fx draws
                            // the cable from its port and the body only keeps the socket
} Pose;

// where the face goes this frame, in art px
typedef struct {
    float t;                // m->time
    int hy;                 // casing top row (bob already in)
    int lx, rx, ly;         // 7x9 eye boxes, the left one at (lx, ly) and the right at (rx, ly)
    int my;                 // mouth row (mouth area is about x 15..24, rows my-1..my+2)
    int glass_x, glass_y, glass_w, glass_h;   // inside of the teal screen, where the face lives
    int lh, rh;             // in/out, left/right hand top row. 30 = resting on the keyboard,
                            // smaller = raised (clamped to hy-1), the cable arm follows
} Face;

// the frame mascot_draw just drew, for stuff drawn outside the canvas right after it
// (point_fx.c). where the canvas ended up and where the stretched out arm leaves the body.
// mk_get reads the finished grid (outline included) until the next mascot_draw
typedef struct {
    int x, y, px;           // window px of canvas cell (0, 0), pose shift included
    int hy;                 // casing top row as drawn (lift included)
    int port_y;             // canvas row of the top one of the two plug cells in the side ports
    int lean, lift;         // as drawn
    int arm_out;            // Pose.arm_out as drawn
} MkFrame;

const MkFrame *mk_frame(void);
Color mk_color(char c);     // a palette letters colour (alpha 0 for '.' and unknown letters)

// anim modules
// every anim_*.c has these four hooks. *_update owns the modules state inside Mascot (its
// *State struct, defined in its own header), the other three have to be pure functions of
// the Mascot. a face hook returns true if it drew the eyes and mouth (then the built in
// face gets skipped), it can also move the hands by changing f->lh / f->rh
//
// order every frame (mascot.c):
//   pose:    sleep, gaze, emotion, point, wheel, physics, bye (later ones win)
//   face:    bye > lock screen (password) > physics > wheel (while driving, but it steps
//            aside for a handovers pointing and for a narration lines emotion) > point
//            (while pointing) > built in ask/happy > emotion (only when IDLE or TALKING) >
//            built in thinking/talking > sleep (only when IDLE) > gaze (only when IDLE) >
//            built in idle
//   overlay: sleep, emotion, gaze, point, wheel, physics, bye (drawn on top of everything)
// mascot.c also asks anim_wheel.h which keys glow on his keyboard and where the typing
// hand is, then draws both itself like the other arms

void emotion_update(Mascot *m, float dt);
void emotion_pose(const Mascot *m, Pose *p);
bool emotion_face(const Mascot *m, Face *f);
void emotion_overlay(const Mascot *m, const Face *f);

void gaze_update(Mascot *m, float dt);
void gaze_pose(const Mascot *m, Pose *p);
bool gaze_face(const Mascot *m, Face *f);
void gaze_overlay(const Mascot *m, const Face *f);

void sleep_update(Mascot *m, float dt);
void sleep_pose(const Mascot *m, Pose *p);
bool sleep_face(const Mascot *m, Face *f);
void sleep_overlay(const Mascot *m, const Face *f);

void point_update(Mascot *m, float dt);
void point_pose(const Mascot *m, Pose *p);
bool point_face(const Mascot *m, Face *f);
void point_overlay(const Mascot *m, const Face *f);

void wheel_update(Mascot *m, float dt);
void wheel_pose(const Mascot *m, Pose *p);
bool wheel_face(const Mascot *m, Face *f);
void wheel_overlay(const Mascot *m, const Face *f);

void physics_update(Mascot *m, float dt);
void physics_pose(const Mascot *m, Pose *p);
bool physics_face(const Mascot *m, Face *f);
void physics_overlay(const Mascot *m, const Face *f);

void bye_update(Mascot *m, float dt);
void bye_pose(const Mascot *m, Pose *p);
bool bye_face(const Mascot *m, Face *f);
void bye_overlay(const Mascot *m, const Face *f);

// how far the CRT switch off has gotten, mascot_draw does it as its last step. the
// finished sprite squashes into a line, the line shrinks to a dot, then the dot goes out
typedef struct {
    int stage;              // 0 drawn as usual, 1 squashing, 2 shrinking line, 3 fading dot
    float squash;           // 1 = full height .. 0 = one line
    float white;            // 0 = true colours .. 1 = all glowing white
    float line;             // 1 = as wide as him .. 0 = a dot
    float dot;              // 1 = glowing .. 0 = gone
} ByeCrt;

ByeCrt bye_crt(const Mascot *m);

// preview scenarios for make anim <module>
// each frame starts from mascot_init() and simulates `seconds` at 60 fps, calling
// drive(m, t) before every step so it can script the inputs (move the cursor, start a
// drag, shake the window...), then the frame gets drawn
typedef struct {
    const char *label;
    float seconds;
    void (*drive)(Mascot *m, float t);
} PreviewFrame;

int emotion_preview(PreviewFrame *out, int cap);
int gaze_preview(PreviewFrame *out, int cap);
int sleep_preview(PreviewFrame *out, int cap);
int physics_preview(PreviewFrame *out, int cap);
int bye_preview(PreviewFrame *out, int cap);
int wheel_preview(PreviewFrame *out, int cap);
