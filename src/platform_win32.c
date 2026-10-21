#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "platform.h"

void platform_get_cursor_screen(int *x, int *y)
{
    POINT p;
    GetCursorPos(&p);
    *x = p.x;
    *y = p.y;
}

void platform_get_work_area(int *x, int *y, int *w, int *h)
{
    RECT r;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);   // desktop minus the taskbar
    *x = r.left;
    *y = r.top;
    *w = r.right - r.left;
    *h = r.bottom - r.top;
}

void platform_get_work_area_at(int px, int py, int *x, int *y, int *w, int *h)
{
    POINT p = { px, py };
    MONITORINFO mi = { sizeof mi };
    if (!GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi)) {
        platform_get_work_area(x, y, w, h);
        return;
    }
    *x = mi.rcWork.left;
    *y = mi.rcWork.top;
    *w = mi.rcWork.right - mi.rcWork.left;
    *h = mi.rcWork.bottom - mi.rcWork.top;
}

// other apps windows

// dwmapi isnt in make.bat's link line so look it up at runtime. if that fails we fall back
// to GetWindowRect, which includes the invisible resize borders (about 7px a side)
#define DWMA_EXTENDED_FRAME_BOUNDS 9
#define DWMA_CLOAKED 14
typedef HRESULT (WINAPI *DwmGetAttrFn)(HWND, DWORD, PVOID, DWORD);

static DwmGetAttrFn dwm_get_attr(void)
{
    static bool tried;
    static DwmGetAttrFn fn;
    if (!tried) {
        tried = true;
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm) fn = (DwmGetAttrFn)(void *)GetProcAddress(dwm, "DwmGetWindowAttribute");
    }
    return fn;
}

static HWND to_hwnd(long long win)
{
    return (HWND)(INT_PTR)win;
}

static long long root_id(HWND h)
{
    if (!h) return 0;
    HWND root = GetAncestor(h, GA_ROOT);
    return (long long)(INT_PTR)(root ? root : h);
}

PlatformWinState platform_window_state(long long win, int *x, int *y, int *w, int *h)
{
    HWND hw = to_hwnd(win);
    if (!win || !IsWindow(hw)) return PLATFORM_WIN_GONE;
    if (IsIconic(hw)) return PLATFORM_WIN_MINIMIZED;
    if (!IsWindowVisible(hw)) return PLATFORM_WIN_HIDDEN;
    DwmGetAttrFn attr = dwm_get_attr();
    DWORD cloaked = 0;                   // on another virtual desktop, or a suspended store app
    if (attr && SUCCEEDED(attr(hw, DWMA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked)
        return PLATFORM_WIN_HIDDEN;
    RECT r;
    if (!attr || FAILED(attr(hw, DWMA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)))
        if (!GetWindowRect(hw, &r)) return PLATFORM_WIN_GONE;
    *x = r.left;
    *y = r.top;
    *w = r.right - r.left;
    *h = r.bottom - r.top;
    return PLATFORM_WIN_SHOWN;
}

long long platform_foreground_window(void)
{
    return root_id(GetForegroundWindow());
}

long long platform_window_id(void *hwnd)
{
    return root_id((HWND)hwnd);
}

long long platform_window_root(long long win)
{
    HWND h = to_hwnd(win);
    return win && IsWindow(h) ? root_id(h) : 0;
}

bool platform_mouse_pressed(void)
{
    // GetAsyncKeyState reads the physical buttons, "left" is the secondary one if the user
    // swapped them in mouse settings
    static bool was_down;
    static bool primed;
    int vk = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    SHORT s = GetAsyncKeyState(vk);
    bool down = (s & 0x8000) != 0;
    // bit 0 = pressed since the last GetAsyncKeyState call, catches a click that went down
    // and up between two frames. the first call only syncs up
    bool pressed = primed && ((down && !was_down) || (s & 1));
    primed = true;
    was_down = down;
    return pressed;
}

void platform_make_overlay(void *hwnd)
{
    HWND h = (HWND)hwnd;
    // WS_EX_TRANSPARENT only lets clicks through on layered windows. set layered once here
    // so toggling click-through never recreates the surface
    SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
}

void platform_set_click_through(void *hwnd, bool enabled)
{
    HWND h = (HWND)hwnd;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    ex = enabled ? (ex | WS_EX_TRANSPARENT) : (ex & ~(LONG_PTR)WS_EX_TRANSPARENT);
    SetWindowLongPtrW(h, GWL_EXSTYLE, ex);
}

void platform_set_no_activate(void *hwnd, bool enabled)
{
    HWND h = (HWND)hwnd;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    ex = enabled ? (ex | WS_EX_NOACTIVATE) : (ex & ~(LONG_PTR)WS_EX_NOACTIVATE);
    SetWindowLongPtrW(h, GWL_EXSTYLE, ex);
}

int platform_icon_sizes(void *hwnd, int *sizes, int cap)
{
    // small icon is 16px and the taskbar's 24, times the display scale. GLFW gives the window
    // the images closest to its small and big (32px) sizes and the taskbar would scale the
    // big one down, so with just these two each one lands on whole pixels
    UINT dpi = GetDpiForWindow((HWND)hwnd);
    if (!dpi) dpi = USER_DEFAULT_SCREEN_DPI;
    const int want[2] = { MulDiv(16, dpi, USER_DEFAULT_SCREEN_DPI), MulDiv(24, dpi, USER_DEFAULT_SCREEN_DPI) };
    int n = cap < 2 ? cap : 2;
    for (int i = 0; i < n; i++) sizes[i] = want[i];
    return n;
}

double platform_pc_idle_seconds(void)
{
    LASTINPUTINFO li = { sizeof li };
    if (!GetLastInputInfo(&li)) return -1.0;
    // 32 bit ms ticks that wrap every ~49.7 days, unsigned subtraction still gets it right
    DWORD idle_ms = GetTickCount() - li.dwTime;
    return idle_ms / 1000.0;
}

bool platform_foreground_fullscreen(void)
{
    HWND h = GetForegroundWindow();
    if (!h || IsIconic(h)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    if (h == GetShellWindow() || h == GetDesktopWindow()) return false;
    // the desktop (clicking the wallpaper focuses Progman/WorkerW) and the taskbar can cover
    // the whole monitor too, theyre the shell so skip them
    static const wchar_t *const SHELL[] = { L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd" };
    wchar_t cls[64];
    if (GetClassNameW(h, cls, 64))
        for (int i = 0; i < 4; i++)
            if (wcscmp(cls, SHELL[i]) == 0) return false;

    // fullscreen covers rcMonitor. a maximized window stops at rcWork (the taskbar) and its
    // GetWindowRect only pokes past that by the invisible borders
    MONITORINFO mi = { sizeof mi };
    RECT r;
    if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi) || !GetWindowRect(h, &r))
        return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
           r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

// driving the real pointer and keyboard. the helpers that only build INPUTs send nothing, so
// tools/input_test.c can check them. windows silently drops input to a window with a higher
// integrity level than ours (UIPI), like an admin app or task manager, and SendInput wont tell
// you. so check first and refuse, the window under the pointer for the mouse, the foreground one for keys

#define INJECT_TAG 0x4D4E4B21   // dwExtraInfo on everything we send ("MNK!") so a hook can tell its him
#define WHEEL_NOTCHES_MAX 50    // per axis per call
#define COMBO_MAX 16            // names in one platform_keys
#define PRESS_INPUTS_MAX 4      // one key down or up plus up to 3 modifiers it needs

// same bits as VkKeyScan's shift state byte, plus super
enum { KMOD_SHIFT = 1, KMOD_CTRL = 2, KMOD_ALT = 4, KMOD_SUPER = 8 };

typedef struct {
    WORD vk;
    WORD scan;              // set 1 scan code for apps that read those (games, remote desktops)
    bool ext;               // KEYEVENTF_EXTENDEDKEY, the E0 prefixed keys
} KeyCode;

typedef struct {
    const char *name;
    KeyCode key;
    unsigned mod;           // KMOD_* if its a modifier, else 0
} NamedKey;

// same list as KEY_NAMES in protocol.py, keep them in sync. modifiers first in KMOD_* order
static const NamedKey NAMED_KEYS[] = {
    { "shift",     { VK_SHIFT,   0x2A, false }, KMOD_SHIFT },
    { "ctrl",      { VK_CONTROL, 0x1D, false }, KMOD_CTRL },
    { "alt",       { VK_MENU,    0x38, false }, KMOD_ALT },
    { "super",     { VK_LWIN,    0x5B, true  }, KMOD_SUPER },
    { "enter",     { VK_RETURN,  0x1C, false }, 0 },
    { "tab",       { VK_TAB,     0x0F, false }, 0 },
    { "escape",    { VK_ESCAPE,  0x01, false }, 0 },
    { "backspace", { VK_BACK,    0x0E, false }, 0 },
    { "delete",    { VK_DELETE,  0x53, true  }, 0 },
    { "insert",    { VK_INSERT,  0x52, true  }, 0 },
    { "space",     { VK_SPACE,   0x39, false }, 0 },
    { "up",        { VK_UP,      0x48, true  }, 0 },
    { "down",      { VK_DOWN,    0x50, true  }, 0 },
    { "left",      { VK_LEFT,    0x4B, true  }, 0 },
    { "right",     { VK_RIGHT,   0x4D, true  }, 0 },
    { "home",      { VK_HOME,    0x47, true  }, 0 },
    { "end",       { VK_END,     0x4F, true  }, 0 },
    { "pageup",    { VK_PRIOR,   0x49, true  }, 0 },
    { "pagedown",  { VK_NEXT,    0x51, true  }, 0 },
    { "f1",        { VK_F1,      0x3B, false }, 0 },
    { "f2",        { VK_F2,      0x3C, false }, 0 },
    { "f3",        { VK_F3,      0x3D, false }, 0 },
    { "f4",        { VK_F4,      0x3E, false }, 0 },
    { "f5",        { VK_F5,      0x3F, false }, 0 },
    { "f6",        { VK_F6,      0x40, false }, 0 },
    { "f7",        { VK_F7,      0x41, false }, 0 },
    { "f8",        { VK_F8,      0x42, false }, 0 },
    { "f9",        { VK_F9,      0x43, false }, 0 },
    { "f10",       { VK_F10,     0x44, false }, 0 },
    { "f11",       { VK_F11,     0x57, false }, 0 },
    { "f12",       { VK_F12,     0x58, false }, 0 },
};
#define NAMED_KEY_COUNT ((int)(sizeof NAMED_KEYS / sizeof NAMED_KEYS[0]))

static const NamedKey *named_key(const char *name)
{
    for (int i = 0; i < NAMED_KEY_COUNT; i++)
        if (strcmp(NAMED_KEYS[i].name, name) == 0) return &NAMED_KEYS[i];
    return NULL;
}

// a printable ascii char (33..126, so no ' ') as the key that types it. `scan` is what
// VkKeyScanEx said, low byte is the key and high byte the shift state it needs (ctrl+alt is
// AltGr). a layout without the char (latin on cyrillic) still has the letter and digit keys
// where shortcuts like Ctrl+A live, so those fall back to their own VK codes
static bool char_key(int ch, SHORT scan, KeyCode *k, unsigned *needs)
{
    if (ch <= ' ' || ch >= 127) return false;
    unsigned vk = (unsigned)scan & 0xFF, state = ((unsigned)scan >> 8) & 0xFF;
    k->scan = 0;
    k->ext = false;
    if (scan != -1 && vk && !(state & ~(unsigned)(KMOD_SHIFT | KMOD_CTRL | KMOD_ALT))) {
        k->vk = (WORD)vk;
        *needs = state;
    } else if (ch >= 'a' && ch <= 'z') {
        k->vk = (WORD)(ch - 'a' + 'A');
        *needs = 0;
    } else if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
        k->vk = (WORD)ch;
        *needs = ch >= 'A' ? KMOD_SHIFT : 0;
    } else {
        return false;           // punctuation this layout cant type
    }
    return true;
}

// keyboard layout of the app that gets our keys (each thread can have its own)
static HKL target_layout(void)
{
    HWND fg = GetForegroundWindow();
    return GetKeyboardLayout(fg ? GetWindowThreadProcessId(fg, NULL) : 0);
}

// a key name as the key, the modifier it is (if any) and the modifiers it needs (a char's
// Shift or AltGr on this layout)
typedef struct { KeyCode key; unsigned mod, needs; } Press;

static bool resolve_key(const char *name, HKL layout, Press *p)
{
    if (!name) return false;
    const NamedKey *nk = named_key(name);
    if (nk) {
        p->key = nk->key;
        p->mod = nk->mod;
        p->needs = 0;
        return true;
    }
    if (!name[0] || name[1]) return false;
    int ch = (unsigned char)name[0];
    if (!char_key(ch, VkKeyScanExW((WCHAR)ch, layout), &p->key, &p->needs)) return false;
    p->key.scan = (WORD)MapVirtualKeyExW(p->key.vk, MAPVK_VK_TO_VSC, layout);
    p->mod = 0;
    return true;
}

static INPUT key_input(KeyCode k, bool down)
{
    INPUT in = { .type = INPUT_KEYBOARD };
    in.ki.wVk = k.vk;
    in.ki.wScan = k.scan;
    in.ki.dwFlags = (k.ext ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
    in.ki.dwExtraInfo = INJECT_TAG;
    return in;
}

// one key down (after the modifiers in `add`) or up (then `add` released in reverse)
static int press_inputs(KeyCode k, unsigned add, bool down, INPUT *out)
{
    int n = 0;
    if (!down) out[n++] = key_input(k, false);
    for (int i = 0; i < 3; i++) {                  // shift ctrl alt going down, alt ctrl shift going up
        int m = down ? i : 2 - i;
        if (add & NAMED_KEYS[m].mod) out[n++] = key_input(NAMED_KEYS[m].key, down);
    }
    if (down) out[n++] = key_input(k, true);
    return n;
}

// a whole combo, pressed in order and released in reverse. modifiers in `held` (from platform_key)
// dont get pressed or released again, and a char's Shift/AltGr is only added if nothing holds it
static int combo_inputs(const Press *ps, int n, unsigned held, INPUT *out)
{
    unsigned add[COMBO_MAX];
    bool skip[COMBO_MAX];
    int count = 0;
    for (int i = 0; i < n; i++) {
        skip[i] = ps[i].mod && (held & ps[i].mod);
        add[i] = ps[i].needs & ~held;
        if (!skip[i]) count += press_inputs(ps[i].key, add[i], true, out + count);
        held |= ps[i].mod | ps[i].needs;
    }
    for (int i = n - 1; i >= 0; i--)
        if (!skip[i]) count += press_inputs(ps[i].key, add[i], false, out + count);
    return count;
}

// the INPUTs that type `cp`. KEYEVENTF_UNICODE gets to the app as WM_CHAR whatever the
// layout, one utf-16 unit at a time (so a surrogate pair past U+FFFF). apps ignore control
// chars sent that way so '\n' and '\t' press Enter and Tab. returns the count (4 max), 0 for
// '\r' or -1 if it cant be typed (other control chars, lone surrogates, past U+10FFFF)
static int char_inputs(unsigned cp, INPUT *out)
{
    if (cp == '\n' || cp == '\t') {
        KeyCode k = named_key(cp == '\n' ? "enter" : "tab")->key;
        out[0] = key_input(k, true);
        out[1] = key_input(k, false);
        return 2;
    }
    if (cp == '\r') return 0;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0) || (cp >= 0xD800 && cp < 0xE000) || cp > 0x10FFFF)
        return -1;
    WORD units[2];
    int nu = 0;
    if (cp >= 0x10000) {
        units[nu++] = (WORD)(0xD800 + ((cp - 0x10000) >> 10));
        units[nu++] = (WORD)(0xDC00 + ((cp - 0x10000) & 0x3FF));
    } else {
        units[nu++] = (WORD)cp;
    }
    int n = 0;
    for (int i = 0; i < nu; i++)
        for (int up = 0; up < 2; up++) {
            INPUT in = { .type = INPUT_KEYBOARD };
            in.ki.wScan = units[i];
            in.ki.dwFlags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0);
            in.ki.dwExtraInfo = INJECT_TAG;
            out[n++] = in;
        }
    return n;
}

// SendInput's buttons are physical and windows applies the user's swap on top, so the
// primary button is the physical right one when theyre swapped
static INPUT button_input(bool right, bool swapped, bool down)
{
    INPUT in = { .type = INPUT_MOUSE };
    bool phys_right = right != swapped;
    in.mi.dwFlags = phys_right ? (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP)
                               : (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
    in.mi.dwExtraInfo = INJECT_TAG;
    return in;
}

static INPUT wheel_input(DWORD flag, int delta)
{
    INPUT in = { .type = INPUT_MOUSE };
    in.mi.dwFlags = flag;
    in.mi.mouseData = (DWORD)delta;
    in.mi.dwExtraInfo = INJECT_TAG;
    return in;
}

// one event per notch like a real wheel (some apps only look at the sign), dy first. the wheel
// counts up and right as positive so dy > 0 (down) goes negative. clamped to WHEEL_NOTCHES_MAX
static int wheel_inputs(int dx, int dy, INPUT *out)
{
    int n = 0;
    int ny = dy < -WHEEL_NOTCHES_MAX ? -WHEEL_NOTCHES_MAX : dy > WHEEL_NOTCHES_MAX ? WHEEL_NOTCHES_MAX : dy;
    int nx = dx < -WHEEL_NOTCHES_MAX ? -WHEEL_NOTCHES_MAX : dx > WHEEL_NOTCHES_MAX ? WHEEL_NOTCHES_MAX : dx;
    for (int i = 0; i < abs(ny); i++) out[n++] = wheel_input(MOUSEEVENTF_WHEEL, ny > 0 ? -WHEEL_DELTA : WHEEL_DELTA);
    for (int i = 0; i < abs(nx); i++) out[n++] = wheel_input(MOUSEEVENTF_HWHEEL, nx > 0 ? WHEEL_DELTA : -WHEEL_DELTA);
    return n;
}

// same edge detection as platform_mouse_pressed, for one GetAsyncKeyState reading
typedef struct { bool primed, was_down; } KeyEdge;

static bool edge_pressed(KeyEdge *e, SHORT state)
{
    bool down = (state & 0x8000) != 0;
    // bit 0 catches a tap between two polls, the first call only syncs up
    bool pressed = e->primed && ((down && !e->was_down) || (state & 1));
    e->primed = true;
    e->was_down = down;
    return pressed;
}

// the part that actually checks windows and sends input (the token calls need advapi32.lib)

static DWORD token_integrity(HANDLE token)
{
    DWORD_PTR buf[8];   // TOKEN_MANDATORY_LABEL plus a SID with one subauthority, aligned
    DWORD len = 0;
    if (!GetTokenInformation(token, TokenIntegrityLevel, buf, sizeof buf, &len)) return 0;
    PSID sid = ((TOKEN_MANDATORY_LABEL *)buf)->Label.Sid;
    return *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1u);
}

static DWORD our_integrity(void)
{
    static DWORD il;
    if (!il) {
        il = SECURITY_MANDATORY_MEDIUM_RID;
        HANDLE t;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) {
            DWORD v = token_integrity(t);
            if (v) il = v;
            CloseHandle(t);
        }
    }
    return il;
}

// false when windows would drop our input to `h` because its process has a higher integrity
// level than ours (still right if we run elevated). a process we cant even open counts as
// higher, from a normal process thats what an admin app looks like
static bool can_reach(HWND h)
{
    DWORD pid = 0;
    if (!h || !GetWindowThreadProcessId(h, &pid) || pid == GetCurrentProcessId()) return true;
    DWORD ours = our_integrity();
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return GetLastError() != ERROR_ACCESS_DENIED || ours >= SECURITY_MANDATORY_HIGH_RID;
    HANDLE tok;
    BOOL opened = OpenProcessToken(proc, TOKEN_QUERY, &tok);
    DWORD err = opened ? 0 : GetLastError();
    CloseHandle(proc);
    if (!opened) return err != ERROR_ACCESS_DENIED || ours >= SECURITY_MANDATORY_HIGH_RID;
    DWORD theirs = token_integrity(tok);
    CloseHandle(tok);
    return !theirs || theirs <= ours;
}

static HWND under_pointer(void)
{
    POINT p;
    return GetCursorPos(&p) ? WindowFromPoint(p) : NULL;
}

static bool send_inputs(INPUT *in, int n)
{
    return n <= 0 || SendInput((UINT)n, in, sizeof *in) == (UINT)n;
}

bool platform_set_cursor(int x, int y)
{
    return SetCursorPos(x, y) != 0;     // physical px, we're per monitor DPI aware
}

// a release always goes out so nothing stays held down, but it still reports the refusal
bool platform_mouse_button(bool right, bool down)
{
    bool reach = can_reach(under_pointer());
    if (down && !reach) return false;
    INPUT in = button_input(right, GetSystemMetrics(SM_SWAPBUTTON) != 0, down);
    return send_inputs(&in, 1) && reach;
}

bool platform_scroll(int dx, int dy)
{
    if (!can_reach(under_pointer())) return false;
    INPUT in[2 * WHEEL_NOTCHES_MAX];
    return send_inputs(in, wheel_inputs(dx, dy, in));
}

bool platform_type_codepoint(unsigned codepoint)
{
    INPUT in[4];
    int n = char_inputs(codepoint, in);
    if (n < 0) return false;
    if (n > 0 && !can_reach(GetForegroundWindow())) return false;
    return send_inputs(in, n);
}

// what platform_key is holding, the modifiers and per key the Shift or AltGr it added for a
// char ("A") that gets let go with it
static unsigned held_mods;
static unsigned char added_mods[256];

bool platform_key(const char *name, bool down)
{
    Press p;
    if (!resolve_key(name, target_layout(), &p)) return false;
    bool reach = can_reach(GetForegroundWindow());
    if (down && !reach) return false;
    INPUT in[PRESS_INPUTS_MAX];
    int n;
    BYTE vk = (BYTE)p.key.vk;
    if (down) {
        unsigned add = p.needs & ~held_mods;
        n = press_inputs(p.key, add, true, in);
        if (!send_inputs(in, n)) return false;
        added_mods[vk] = (unsigned char)add;
        held_mods |= p.mod;
        return true;
    }
    held_mods &= ~p.mod;
    n = press_inputs(p.key, added_mods[vk] & ~held_mods, false, in);
    added_mods[vk] = 0;
    return send_inputs(in, n) && reach;
}

bool platform_keys(const char *const *names, int n)
{
    if (n < 1 || n > COMBO_MAX) return false;
    HKL layout = target_layout();
    Press ps[COMBO_MAX];
    for (int i = 0; i < n; i++)
        if (!resolve_key(names[i], layout, &ps[i])) return false;   // before anything gets pressed
    if (!can_reach(GetForegroundWindow())) return false;
    INPUT in[2 * PRESS_INPUTS_MAX * COMBO_MAX];
    return send_inputs(in, combo_inputs(ps, n, held_mods, in));
}

bool platform_key_known(const char *name)
{
    Press p;
    return resolve_key(name, target_layout(), &p);
}

bool platform_key_pressed(PlatformKey key)
{
    static const int VKS[] = { [PLATFORM_KEY_ESC] = VK_ESCAPE };
    static KeyEdge edges[sizeof VKS / sizeof VKS[0]];
    if ((unsigned)key >= sizeof VKS / sizeof VKS[0]) return false;
    return edge_pressed(&edges[key], GetAsyncKeyState(VKS[key]));
}

// child process

#define CMDLINE_MAX 32768

struct PlatformProcess {
    HANDLE process;
    HANDLE job;        // kill on close, the child cant outlive us even if we crash
    HANDLE in_w;       // our end of the child's stdin
    HANDLE out_r;      // our end of the child's stdout
};

static bool to_wide(const char *s, wchar_t *out, int cap)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) > 0;
}

// quote one arg the way CommandLineToArgvW / the C runtime parse it back
static bool append_arg(wchar_t *cmd, size_t *len, const wchar_t *arg)
{
    wchar_t buf[CMDLINE_MAX];
    size_t n = 0;
    bool plain = arg[0] != 0 && wcspbrk(arg, L" \t\"") == NULL;
    if (*len > 0) buf[n++] = L' ';
    if (plain) {
        for (const wchar_t *c = arg; *c && n < CMDLINE_MAX - 1; c++) buf[n++] = *c;
    } else {
        buf[n++] = L'"';
        for (const wchar_t *c = arg;; c++) {
            size_t slashes = 0;
            while (*c == L'\\') { slashes++; c++; }
            // backslashes are only special right before a quote (or the closing one)
            size_t emit = *c == 0 ? slashes * 2 : (*c == L'"' ? slashes * 2 + 1 : slashes);
            for (size_t i = 0; i < emit && n < CMDLINE_MAX - 4; i++) buf[n++] = L'\\';
            if (*c == 0) break;
            if (n < CMDLINE_MAX - 4) buf[n++] = *c;
        }
        buf[n++] = L'"';
    }
    if (*len + n >= CMDLINE_MAX) return false;
    wmemcpy(cmd + *len, buf, n);
    *len += n;
    cmd[*len] = 0;
    return true;
}

PlatformProcess *platform_spawn(const char *const *argv, const char *cwd, const char *stderr_path)
{
    static wchar_t cmd[CMDLINE_MAX], warg[CMDLINE_MAX];
    wchar_t wcwd[MAX_PATH * 2], werr[MAX_PATH * 2];
    size_t len = 0;
    cmd[0] = 0;
    for (const char *const *a = argv; *a; a++)
        if (!to_wide(*a, warg, CMDLINE_MAX) || !append_arg(cmd, &len, warg)) return NULL;
    if (!to_wide(cwd, wcwd, MAX_PATH * 2) || !to_wide(stderr_path, werr, MAX_PATH * 2)) return NULL;

    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL;
    if (!CreatePipe(&in_r, &in_w, &sa, 1 << 16)) return NULL;
    if (!CreatePipe(&out_r, &out_w, &sa, 1 << 16)) {
        CloseHandle(in_r);
        CloseHandle(in_w);
        return NULL;
    }
    // only the childs ends get inherited
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

    HANDLE err = CreateFileW(werr, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (err == INVALID_HANDLE_VALUE)
        err = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);

    STARTUPINFOW si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = err;
    PROCESS_INFORMATION pi = { 0 };
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                             NULL, wcwd, &si, &pi);
    CloseHandle(in_r);      // the child has its own copies now
    CloseHandle(out_w);
    if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
    if (!ok) {
        CloseHandle(in_w);
        CloseHandle(out_r);
        return NULL;
    }

    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = { 0 };
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    PlatformProcess *p = calloc(1, sizeof *p);
    if (!p) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(in_w);
        CloseHandle(out_r);
        if (job) CloseHandle(job);
        return NULL;
    }
    p->process = pi.hProcess;
    p->job = job;
    p->in_w = in_w;
    p->out_r = out_r;
    return p;
}

int platform_proc_read(PlatformProcess *p, char *buf, int cap)
{
    DWORD avail = 0, got = 0;
    if (!PeekNamedPipe(p->out_r, NULL, 0, NULL, &avail, NULL)) return -1;   // broken pipe
    if (avail == 0) return 0;
    if (!ReadFile(p->out_r, buf, avail < (DWORD)cap ? avail : (DWORD)cap, &got, NULL)) return -1;
    return (int)got;
}

bool platform_proc_write(PlatformProcess *p, const char *buf, int len)
{
    while (len > 0) {
        DWORD wrote = 0;
        if (!p->in_w || !WriteFile(p->in_w, buf, (DWORD)len, &wrote, NULL)) return false;
        buf += wrote;
        len -= (int)wrote;
    }
    return true;
}

bool platform_proc_alive(PlatformProcess *p)
{
    return WaitForSingleObject(p->process, 0) == WAIT_TIMEOUT;
}

void platform_proc_close(PlatformProcess *p)
{
    if (!p) return;
    CloseHandle(p->in_w);           // EOF on its stdin, the child shuts down by itself
    p->in_w = NULL;
    if (WaitForSingleObject(p->process, 1500) == WAIT_TIMEOUT) TerminateProcess(p->process, 1);
    CloseHandle(p->process);
    CloseHandle(p->out_r);
    if (p->job) CloseHandle(p->job);
    free(p);
}
