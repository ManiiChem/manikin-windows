#pragma once

#include <stdbool.h>

// the windows layer. only platform_win32.c includes <windows.h> because it clashes
// with raylib.h (Rectangle, CloseWindow, DrawText...)

void platform_get_cursor_screen(int *x, int *y);
void platform_get_work_area(int *x, int *y, int *w, int *h);

// work area (desktop minus the taskbar) of the monitor that has the desktop point (x, y),
// or of the nearest monitor if none has it. desktop coords span every monitor so they
// can be negative
void platform_get_work_area_at(int px, int py, int *x, int *y, int *w, int *h);

// other apps windows
// a window is its HWND widened to long long (what the brain sends in "point"), 0 means
// no window / unknown

typedef enum {
    PLATFORM_WIN_GONE,              // closed, or never was a window
    PLATFORM_WIN_HIDDEN,            // exists but isnt shown (on another virtual desktop counts)
    PLATFORM_WIN_MINIMIZED,
    PLATFORM_WIN_SHOWN,
} PlatformWinState;

// state of a top level window, and when its SHOWN its visible bounds in desktop px
// (without the invisible resize borders and the drop shadow)
PlatformWinState platform_window_state(long long win, int *x, int *y, int *w, int *h);

// the window that has keyboard focus, as its top level window (0 if none)
long long platform_foreground_window(void);

// our own window (GetWindowHandle()) as the same kind of id, top level
long long platform_window_id(void *hwnd);
// the top level window `win` belongs to (itself if it is one, 0 if its gone) so it
// compares equal to platform_foreground_window()
long long platform_window_root(long long win);

// true once for each press of the primary mouse button anywhere on the desktop since the
// last call, even over other apps and while our window is click-through. call it every
// frame, it keeps state between calls and a press from before the first call doesnt count.
// it polls, so a press can be reported up to a frame late (read the cursor when it fires),
// and a really short click between two frames is only caught by GetAsyncKeyState's
// "pressed since the last check" bit, which any other GetAsyncKeyState call in our
// process can eat first
bool platform_mouse_pressed(void);

// call once after InitWindow so click-through can be toggled cheaply
void platform_make_overlay(void *hwnd);

// enabled = mouse events fall through to whatever is behind the window
void platform_set_click_through(void *hwnd, bool enabled);

// enabled = clicking the window doesnt take keyboard focus away from the app the user was
// in. while he drives, answering his bubble cant pull the focus out of the app hes typing into
void platform_set_no_activate(void *hwnd, bool enabled);

// the px sizes windows shows this window's icon at (for SetWindowIcons) so the pixel art
// gets drawn at exactly those sizes instead of scaled. fills up to `cap`, returns how many
int platform_icon_sizes(void *hwnd, int *sizes, int cap);

// driving the real pointer and keyboard (he takes the wheel)
// input as if it came from the user's own mouse and keyboard, going to whatever is under
// the pointer or has focus. desktop px. each one returns false if windows refused it.
// windows silently drops input to a window running as admin so we check that first (the
// window under the pointer for the mouse, the focused one for keys) and a press gets
// refused with nothing sent. a release always gets sent so nothing stays held down, but
// it still returns false then, so false on a release doesnt mean "still down"

// moves the pointer. no admin window check here, only presses get refused for those
bool platform_set_cursor(int x, int y);
// presses (down) or releases a mouse button where the pointer is. thats the user's primary
// button, or the secondary one if `right`, even if they swapped them
bool platform_mouse_button(bool right, bool down);
// wheel notches where the pointer is, dy > 0 scrolls down and dx > 0 right. one wheel
// event per notch, 50 max each way
bool platform_scroll(int dx, int dy);
// types one unicode character at the focus, whatever the keyboard layout is. it shows up
// as a typed char with no real key behind it so games and remote desktops might miss it.
// '\n' presses Enter and '\t' Tab, '\r' sends nothing (so "\r\n" is one Enter), other
// control chars and invalid codepoints return false
bool platform_type_codepoint(unsigned codepoint);
// a key by its name in protocol.py's KEY_NAMES ("ctrl", "enter", "f5") or a single
// printable ascii char ("a", "/"), pressed or released. unknown names return false.
// a char is the key that types it on the focused app's layout, plus the Shift or AltGr it
// needs there unless thats already held. so on US "A" is Shift+A and "?" is Shift+/, thats
// why shortcuts use lowercase ({"ctrl", "S"} is Ctrl+Shift+S)
bool platform_key(const char *name, bool down);
// presses names[0..n) in order then releases them in reverse, {"ctrl", "s"} is Ctrl+S.
// sent as one batch so the user's own keys cant land in the middle. modifiers already held
// through platform_key are left alone. 1-16 names, any unknown one returns false with
// nothing pressed
bool platform_keys(const char *const *names, int n);
// whether the name can be pressed, so its in KEY_NAMES or its a char on the focused app's
// layout (letters and digits always count, for shortcuts on non latin layouts)
bool platform_key_known(const char *name);

typedef enum { PLATFORM_KEY_ESC } PlatformKey;
// true once for each press of the key anywhere since the last call (polled like
// platform_mouse_pressed, same caveats). its how Esc stops him while another app has the
// focus. it also sees an Escape he sends himself
bool platform_key_pressed(PlatformKey key);

// secs since the last keyboard or mouse input anywhere in the user's session, not just in
// our window. -1 means unknown (the call failed), callers should treat that as "no idea"
// and never as "active"
double platform_pc_idle_seconds(void);

// the focused app covers its whole monitor (a game, a fullscreen video, a slideshow).
// false for the desktop, the taskbar/shell, our own window, and when it cant tell.
// cheap enough to call about once a second
bool platform_foreground_fullscreen(void);

// child process with pipes (the python brain)

typedef struct PlatformProcess PlatformProcess;

// argv[0] is the program and argv ends with NULL, strings are utf-8. the childs stdin and
// stdout are pipes to us, its stderr gets appended to stderr_path and it gets no console
// window. it gets killed if we exit. NULL on failure
PlatformProcess *platform_spawn(const char *const *argv, const char *cwd, const char *stderr_path);

// never blocks, just copies up to cap bytes that are already waiting.
// returns the byte count, 0 if nothing yet, -1 once the child closed its stdout
int platform_proc_read(PlatformProcess *p, char *buf, int cap);

bool platform_proc_write(PlatformProcess *p, const char *buf, int len);
bool platform_proc_alive(PlatformProcess *p);

// closes the childs stdin so it can exit cleanly, waits up to 1.5s, then kills it
void platform_proc_close(PlatformProcess *p);
