#pragma once

#include <stdbool.h>

// talks to the python brain over its stdin/stdout, one JSON object per line (brain/manikin/protocol.py)

#define BRAIN_TEXT_MAX 2048          // utf-8 bytes of an event's text, longer gets cut

typedef enum {
    BRAIN_EV_PASSWORD,      // mode "create" | "confirm" | "unlock", text = the error or ""
    BRAIN_EV_UNLOCKED,      // ok
    BRAIN_EV_READY,
    BRAIN_EV_THINKING,
    BRAIN_EV_STATUS,        // text = label for the thinking bubble ("searching the web")
    BRAIN_EV_ASK,           // id, text = the question
    BRAIN_EV_ASK_CANCEL,    // id
    BRAIN_EV_SAY,           // text, mood (an emotion name or ""), proactive
    BRAIN_EV_REMIND,        // text
    BRAIN_EV_ERROR,         // text
    BRAIN_EV_CANCELLED,     // the reply we were waiting on got dropped (after a cancel)
    BRAIN_EV_POINT,         // id, x, y, box, window, text. go point at something on screen
    BRAIN_EV_POINT_CLEAR,   // id. stop pointing at it and walk home
    BRAIN_EV_SOUND,         // sound settings changed (also sent once at the start)
    BRAIN_EV_DRIVE,         // id, drive (+ text for "type"). one action with the real pointer
    BRAIN_EV_DRIVE_END,     // text "done" | "stopped" | "failed". let go and walk home
    BRAIN_EV_NARRATE,       // text, mood. a passing line while he drives
    BRAIN_EV_DIED,          // the process exited or closed its stdout
} BrainEventType;

// one "drive" action, he takes the wheel
typedef enum {
    BRAIN_DRIVE_NONE,       // unknown "do", answer it "failed"
    BRAIN_DRIVE_MOVE,       // x, y. hover there
    BRAIN_DRIVE_CLICK,      // x, y, button, count, mods
    BRAIN_DRIVE_DRAG,       // x, y to to_x, to_y, mods
    BRAIN_DRIVE_SCROLL,     // x, y, dx, dy
    BRAIN_DRIVE_TYPE,       // the event's text, at the focus
    BRAIN_DRIVE_KEY,        // keys pressed together, repeat times
} BrainDriveDo;

#define BRAIN_DRIVE_KEYS_MAX 6
#define BRAIN_KEY_NAME_MAX 12   // fits "backspace", the longest in KEY_NAMES

typedef struct {
    BrainDriveDo action;
    int x, y;               // desktop px (MOVE, CLICK, DRAG start, SCROLL)
    int to_x, to_y;         // DRAG end
    bool right;             // CLICK with the right button
    int count;              // CLICK 1..3
    int dx, dy;             // SCROLL notches, dy > 0 down, dx > 0 right
    int repeat;             // KEY 1..100
    int nkeys;              // the keys for KEY, the held modifiers for CLICK/DRAG
    char keys[BRAIN_DRIVE_KEYS_MAX][BRAIN_KEY_NAME_MAX];
} BrainDrive;

// sound settings, same as the brain keeps in data/settings.json
typedef struct {
    int volume;             // 0..100
    bool muted;
    bool voice;             // his babble while a line types out
} BrainSound;

typedef struct {
    BrainEventType type;
    int id;
    bool ok;
    bool proactive;         // SAY: he said it on his own, its not a reply
    bool session;           // ASK: show the third "yes for this session" button
    char mode[16];
    char mood[16];
    int x, y;               // POINT: the spot to touch, desktop px (can be negative)
    int box[4];             // POINT: the control's left, top, width, height
    long long window;       // POINT: HWND of the window its in (0 = unknown)
    BrainSound sound;
    BrainDrive drive;
    char text[BRAIN_TEXT_MAX];
} BrainEvent;

// finds brain/ and its .venv next to the exe (or one folder up) and starts it.
// if it fails `err` gets a one sentence reason for the bubble
bool brain_start(char *err, int err_cap);
void brain_stop(void);
bool brain_running(void);

// next event if one came in. never blocks, call it till it returns false
bool brain_poll(BrainEvent *ev);

// any length, cJSON sizes the line itself so nothing gets cut
void brain_send_user(const char *text);
void brain_send_answer(int id, bool allow);
// the third button on an ask that offered it, yes and dont ask again this session
void brain_send_answer_session(int id);
void brain_send_password(const char *password);
void brain_send_password_skip(void);

// Esc while a reply is pending, the brain stops and answers "cancelled"
void brain_send_cancel(void);
// every few secs so the brain can pick a moment to chime in. pet_idle = secs since the user
// last touched him, pc_idle = secs since any input at all (-1 = unknown), typing = chat box has text
void brain_send_presence(float pet_idle, float pc_idle, bool typing, bool asleep);
// the user poked him awake after slept_s secs of real sleep
void brain_send_woke(float slept_s);
// a "point" ended, exactly one per id. outcome is "clicked" | "moved" | "dismissed" | "cleared",
// x, y (desktop px) and inside only mean something for "clicked", otherwise pass 0, 0, false
void brain_send_point_done(int id, const char *outcome, int x, int y, bool inside);
// the user toggled mute (Ctrl+M), the brain remembers it and answers with "sound"
void brain_send_sound_set(bool muted);
// a "drive" action ended, exactly one per id. outcome is "done" | "grabbed" | "cancelled"
// | "failed". x, y is where the pointer is now (desktop px)
void brain_send_drive_done(int id, const char *outcome, int x, int y);

// sound settings read straight from data/settings.json so they apply from the first frame,
// before the brain is up. defaults (30, not muted, voice on) for anything missing or unreadable
BrainSound brain_saved_sound(void);
