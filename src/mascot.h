#pragma once

#include <stdbool.h>

#include "anim_bye.h"
#include "anim_emotion.h"
#include "anim_gaze.h"
#include "anim_physics.h"
#include "anim_point.h"
#include "anim_sleep.h"
#include "anim_wheel.h"

// canvas size in art pixels (with room for the halo and the idle bob), on screen its
// MASCOT_W*MASCOT_PX by MASCOT_H*MASCOT_PX = 120x144
#define MASCOT_W 40
#define MASCOT_H 48
#define MASCOT_PX 3

typedef enum {
    MOOD_IDLE,
    MOOD_THINKING,
    MOOD_TALKING,
    MOOD_SHY,       // typing a password, his screen turns into a lock screen ("not looking!")
    MOOD_ASK,       // waiting on a yes/no permission answer, hand up and eyes on the bubble
    MOOD_HAPPY,     // memory unlocked, happy eyes and arms up
} Mood;

// stuff that happened this update that deserves a sound. the anim modules set them,
// mascot_update clears them first so they only ever cover one update. cues.c plays them
// (preview and showreel just ignore them)
enum {
    // anim_point.c, walking over to something and pointing at it
    MEV_STEP        = 1u << 0,   // a foot came down while walking (ev.foot says which)
    MEV_DEPART      = 1u << 1,   // set off on a trip
    MEV_ARRIVE      = 1u << 2,   // trip over, either home or next to the target
    MEV_ARM_OUT     = 1u << 3,   // cable arm unplugged and starts stretching out
    MEV_PLUG        = 1u << 4,   // plug reached the target (the ring locks on)
    MEV_ARM_IN      = 1u << 5,   // arm plugged back into him
    MEV_POINT_HIT   = 1u << 6,   // user clicked the thing, he celebrates
    MEV_POINT_MISS  = 1u << 7,   // clicked somewhere else, it moved, or never mind
    MEV_WHISTLE     = 1u << 8,   // a whistled note pops up on his way home (ev.note)
    MEV_POINT_HUH   = 1u << 9,   // the "?" over his head while pointing
    MEV_POINT_BANG  = 1u << 10,  // the "!" over his head while pointing

    // anim_physics.c, getting dragged around
    MEV_PICKUP      = 1u << 11,
    MEV_DROP        = 1u << 12,  // let go of him (ev.land is how hard he landed)
    MEV_SHAKE       = 1u << 13,  // a fast direction flip while being shaken
    MEV_DIZZY       = 1u << 14,
    MEV_SHAKEOFF    = 1u << 15,  // shaking it off after being dizzy

    // anim_sleep.c
    MEV_YAWN        = 1u << 16,
    MEV_NOD         = 1u << 17,  // his head snaps up while nodding off
    MEV_ASLEEP      = 1u << 18,  // fell fast asleep
    MEV_SNORE_POP   = 1u << 19,  // the snot bubble pops
    MEV_WAKE_JOLT   = 1u << 20,  // woken from deep sleep
    MEV_WAKE_SOFT   = 1u << 21,  // woken while only drowsy

    // anim_gaze.c
    MEV_FLINCH      = 1u << 22,  // the pointer landed on his face
    MEV_PERK        = 1u << 23,  // "oh, there you are!" after losing interest

    // anim_emotion.c
    MEV_BEAT        = 1u << 24,  // a beat of the replys emotion (ev.beat says which)

    // anim_bye.c
    MEV_SWITCH_OFF  = 1u << 25,  // done waving bye, the power cuts (the power down sound)

    // anim_wheel.c, driving the real pointer (m->drive_in, filled from drive.c)
    MEV_GRAB        = 1u << 26,  // the cable clunks onto the pointer
    MEV_CLACK       = 1u << 27,  // he clicked
    MEV_KEY         = 1u << 28,  // he typed a character or pressed a key (ev.note is which key lit)
    MEV_HAUL        = 1u << 29,  // a long haul starts and he strains
    MEV_LETGO       = 1u << 30,  // he unplugged from the pointer
    MEV_VICTORY     = 1u << 31,  // the drive went well, victory lap
};

// emotion beats, the moments in each emotions entrance (emotion_pose)
typedef enum {
    BEAT_NONE,
    BEAT_HOP,           // a hop lands (happy, excited, love...)
    BEAT_SPARKLE,       // excited sparkles
    BEAT_LAUGH,         // one laughing pulse
    BEAT_HEART,         // a heart floats up
    BEAT_BANG,          // surprised "!"
    BEAT_HUH,           // confused "?"
    BEAT_TING,          // the smug glint
    BEAT_SIGH,          // sad sigh
    BEAT_QUIVER,        // worried tremble
    BEAT_STOMP,         // annoyed stomp
    BEAT_HMPH,          // annoyed huff
} MascotBeat;

typedef struct {
    unsigned bits;      // MEV_*
    int beat;           // for MEV_BEAT, a MascotBeat
    int foot;           // for MEV_STEP, -1 left foot, +1 right
    int note;           // for MEV_WHISTLE 0, 1, 2... counting up through the wait,
                        // for MEV_KEY which key on his keyboard lit (0..27, left to right)
    float land;         // for MEV_DROP, 0 = set down gently .. 1 = slammed
} MascotEvents;

// driving the real pointer (drive.c) the way the animation sees it. main.c fills it every
// frame and its all zero when he isnt driving. the counters only go up during a drive,
// anim_wheel.c fires MEV_CLACK / MEV_KEY when they change
typedef enum {
    DRIVE_ANIM_NONE,
    DRIVE_ANIM_FETCH,       // walking over to wherever the pointer is to grab it
    DRIVE_ANIM_CARRY,       // walking the pointer to the next spot (point_dx/dy, walk_vx/vy)
    DRIVE_ANIM_ACT,         // clicking, dragging, scrolling
    DRIVE_ANIM_TYPE,        // typing on his keyboard
    DRIVE_ANIM_HOLD,        // holding the pointer while the brain picks the next action
    DRIVE_ANIM_WAIT,        // waiting on the user (an ask, a handover), not steering
    DRIVE_ANIM_PAUSED,      // user grabbed the mouse ("oh, its yours!")
    DRIVE_ANIM_DONE,        // over, let go, react to the result, walk home
} DriveAnimPhase;

typedef enum {
    DRIVE_RESULT_NONE,
    DRIVE_RESULT_VICTORY,   // went well, hop and jingle
    DRIVE_RESULT_SHEEPISH,  // stopped, declined, handed over or failed
} DriveResult;

typedef struct {
    int phase;              // a DriveAnimPhase
    float t;                // seconds in that phase
    bool latched;           // his cable is plugged into the pointer
    bool steering;          // ...and hes moving it (false = it trails the users hand)
    float haul;             // 0..1 how hard hes hauling (long carries, drags)
    int clicks;             // +1 per click (a double click is 2)
    int keys;               // +1 per character typed or key combo pressed
    unsigned last_key;      // codepoint of the last character (or the first letter of a key name)
    int result;             // a DriveResult, set in DONE
} DriveInputs;

typedef struct {
    Mood mood;
    float time;
    MascotEvents ev;    // what happened in the last mascot_update (MEV_*)
    float blink_in;     // secs till the next blink
    float blink_left;   // > 0 while the eyes are shut
    int typed;          // MOOD_SHY password characters typed so far (lights the dots)

    // inputs from the overlay, set every frame before mascot_update
    float cursor_dx, cursor_dy;   // mouse pointer from the middle of his screen face in screen px
                                  // (+x right, +y down), works anywhere on screen
    float idle_for;               // secs since the user last did something (typed, clicked,
                                  // dragged) or Manikin last showed something
    bool held;                    // being dragged around with the mouse
    float vel_x, vel_y;           // window velocity in screen px/s (0 when not held)
    Emotion emotion;              // emotion of the reply on screen (EMOTION_NONE = none)
    float emotion_age;            // secs since that reply started showing (mascot_update adds dt)

    // pointing at something on screen, copied from pointer_view() every frame (pointer.h)
    int point_phase;              // a PointerPhase, 0 home, 1 out, 2 at, 3 back
    float point_t;                // seconds in that phase
    int point_result;             // a PointerResult (0 while still pointing)
    float point_dx, point_dy;     // target from the middle of his screen face, screen px
    float walk_vx, walk_vy;       // window velocity while walking, screen px/s

    // driving the real pointer, his cable plugs into it and he walks it around. while
    // driving point_dx/dy is where the pointer is and walk_vx/vy is how fast hes walking
    DriveInputs drive_in;

    bool leaving;                 // app is quitting, wave bye then switch off (anim_bye.c)

    // per module animation state, each anim_*.c owns its own
    EmotionState emo;
    GazeState gaze;
    SleepState sleep;
    PhysicsState phys;
    PointState point;
    WheelState wheel;
    ByeState bye;
} Mascot;

void mascot_init(Mascot *m);
void mascot_update(Mascot *m, float dt);
void mascot_draw(const Mascot *m, int x, int y, int px);

// "happy" -> EMOTION_HAPPY, unknown or NULL -> EMOTION_NONE
Emotion emotion_from_name(const char *name);

// fast asleep (past nodding off and not mid wake up), lives in anim_sleep.c
bool sleep_is_asleep(const Mascot *m);

// hes done saying bye (m->leaving) and gone dark so the app can close now (anim_bye.c)
bool bye_done(const Mascot *m);
