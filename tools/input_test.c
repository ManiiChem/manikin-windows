// tests the input sending half of src/platform_win32.c (key names, layouts, combos, unicode, wheel,
// swapped buttons, esc edge). it only builds INPUTs and looks at them, SendInput and SetCursorPos
// are tripwires so it never touches your real mouse or keyboard. make ctest runs it

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- tripwires, nothing in here is allowed to reach the real desktop ----

static int injected;

static UINT fake_send_input(UINT n, INPUT *in, int size)
{
    (void)in; (void)size;
    injected++;
    printf("  FAIL: SendInput called with %u inputs\n", n);
    exit(2);
}

static BOOL fake_set_cursor_pos(int x, int y)
{
    injected++;
    printf("  FAIL: SetCursorPos(%d, %d) called\n", x, y);
    exit(2);
}

#define SendInput fake_send_input
#define SetCursorPos fake_set_cursor_pos
#include "../src/platform_win32.c"
#undef SendInput
#undef SetCursorPos

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
                                 printf(__VA_ARGS__); printf("\n"); } } while (0)

// copy of KEY_NAMES from protocol.py, the C table has to match it exactly
static const char *const KEY_NAMES[] = {
    "ctrl", "shift", "alt", "super",
    "enter", "tab", "escape", "backspace", "delete", "insert", "space",
    "up", "down", "left", "right", "home", "end", "pageup", "pagedown",
    "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
};
#define KEY_NAME_COUNT ((int)(sizeof KEY_NAMES / sizeof KEY_NAMES[0]))

// key sequence as text like "ctrl+ A+ A- ctrl-" (+ down, - up) so the checks are readable
static const char *seq(const INPUT *in, int n)
{
    static char buf[512];
    size_t len = 0;
    buf[0] = 0;
    for (int i = 0; i < n && len < sizeof buf - 32; i++) {
        const char *name = NULL;
        for (int k = 0; k < NAMED_KEY_COUNT; k++)
            if (NAMED_KEYS[k].key.vk == in[i].ki.wVk) name = NAMED_KEYS[k].name;
        char one[16];
        if (!name) {
            snprintf(one, sizeof one, "%c", in[i].ki.wVk);
            name = one;
        }
        len += snprintf(buf + len, sizeof buf - len, "%s%s%c", len ? " " : "", name,
                        (in[i].ki.dwFlags & KEYEVENTF_KEYUP) ? '-' : '+');
    }
    return buf;
}

static Press named(const char *name)
{
    const NamedKey *k = named_key(name);
    return (Press){ k->key, k->mod, 0 };
}

// a characters key the way a layout would give it, vk is the keys letter ('S') and needs
// is what it needs on top (shift for "S", nothing for "s")
static Press letter(char vk, unsigned needs)
{
    return (Press){ { (WORD)vk, 0x10, false }, 0, needs };
}

static void test_table(void)
{
    printf("key names\n");
    CHECK(NAMED_KEY_COUNT == KEY_NAME_COUNT, "table has %d names, KEY_NAMES %d", NAMED_KEY_COUNT, KEY_NAME_COUNT);
    for (int i = 0; i < KEY_NAME_COUNT; i++) {
        CHECK(named_key(KEY_NAMES[i]) != NULL, "\"%s\" missing from the table", KEY_NAMES[i]);
        CHECK(platform_key_known(KEY_NAMES[i]), "\"%s\" not known", KEY_NAMES[i]);
    }
    for (int i = 0; i < NAMED_KEY_COUNT; i++)
        for (int j = i + 1; j < NAMED_KEY_COUNT; j++) {
            CHECK(strcmp(NAMED_KEYS[i].name, NAMED_KEYS[j].name) != 0, "\"%s\" twice", NAMED_KEYS[i].name);
            CHECK(NAMED_KEYS[i].key.vk != NAMED_KEYS[j].key.vk, "%s and %s share a VK", NAMED_KEYS[i].name, NAMED_KEYS[j].name);
        }

    // press_inputs counts on the first three being shift, ctrl, alt
    static const unsigned MODS[] = { KMOD_SHIFT, KMOD_CTRL, KMOD_ALT, KMOD_SUPER };
    for (int i = 0; i < 4; i++) CHECK(NAMED_KEYS[i].mod == MODS[i], "entry %d should be modifier %u", i, MODS[i]);
    for (int i = 4; i < NAMED_KEY_COUNT; i++) CHECK(NAMED_KEYS[i].mod == 0, "%s isn't a modifier", NAMED_KEYS[i].name);
    CHECK(named_key("super")->key.vk == VK_LWIN, "super should be the Windows key");

    // the E0 keys need KEYEVENTF_EXTENDEDKEY or anything reading scan codes sees the numpad
    // instead (arrows with numlock on)
    static const char *const EXT[] = { "super", "delete", "insert", "up", "down", "left", "right",
                                       "home", "end", "pageup", "pagedown" };
    for (int i = 0; i < NAMED_KEY_COUNT; i++) {
        bool want = false;
        for (int j = 0; j < (int)(sizeof EXT / sizeof EXT[0]); j++)
            if (strcmp(EXT[j], NAMED_KEYS[i].name) == 0) want = true;
        CHECK(NAMED_KEYS[i].key.ext == want, "%s: extended should be %d", NAMED_KEYS[i].name, want);
        INPUT in = key_input(NAMED_KEYS[i].key, true);
        CHECK(((in.ki.dwFlags & KEYEVENTF_EXTENDEDKEY) != 0) == want, "%s: flag not set right", NAMED_KEYS[i].name);
    }

    // set 1 scan codes
    static const struct { const char *name; WORD scan; } SCANS[] = {
        { "enter", 0x1C }, { "tab", 0x0F }, { "escape", 0x01 }, { "backspace", 0x0E }, { "space", 0x39 },
        { "ctrl", 0x1D }, { "shift", 0x2A }, { "alt", 0x38 }, { "super", 0x5B },
        { "insert", 0x52 }, { "delete", 0x53 }, { "home", 0x47 }, { "end", 0x4F },
        { "pageup", 0x49 }, { "pagedown", 0x51 }, { "up", 0x48 }, { "down", 0x50 }, { "left", 0x4B },
        { "right", 0x4D }, { "f1", 0x3B }, { "f10", 0x44 }, { "f11", 0x57 }, { "f12", 0x58 },
    };
    for (int i = 0; i < (int)(sizeof SCANS / sizeof SCANS[0]); i++)
        CHECK(named_key(SCANS[i].name)->key.scan == SCANS[i].scan, "%s: scan 0x%02X, want 0x%02X",
              SCANS[i].name, named_key(SCANS[i].name)->key.scan, SCANS[i].scan);

    // not key names, is_key_name in protocol.py says no to all of these too
    static const char *const BAD[] = { "", " ", "ab", "Return", "ctrl+s", "F1", "esc", "Ctrl", "f13",
                                       "\x7f", "\x01", "\t", "\xc3\xa9" /* e acute in utf-8 */, "spacebar" };
    for (int i = 0; i < (int)(sizeof BAD / sizeof BAD[0]); i++)
        CHECK(!platform_key_known(BAD[i]), "\"%s\" should be unknown", BAD[i]);
    CHECK(!platform_key_known(NULL), "NULL should be unknown");
}

static void test_chars(void)
{
    printf("characters\n");
    KeyCode k;
    unsigned needs;
    // made up VkKeyScanEx answers, low byte is the key and high byte the shift state
    CHECK(char_key('A', 0x0141, &k, &needs) && k.vk == 'A' && needs == KMOD_SHIFT, "'A' should be Shift+A");
    CHECK(char_key('a', 0x0041, &k, &needs) && k.vk == 'A' && needs == 0, "'a' should be plain A");
    CHECK(char_key('@', 0x0651, &k, &needs) && k.vk == 'Q' && needs == (KMOD_CTRL | KMOD_ALT),
          "'@' on a German layout should be AltGr+Q");
    CHECK(char_key('/', 0x00BF, &k, &needs) && k.vk == VK_OEM_2 && needs == 0, "'/' on US is OEM_2");
    CHECK(char_key('?', 0x01BF, &k, &needs) && k.vk == VK_OEM_2 && needs == KMOD_SHIFT, "'?' on US is Shift+OEM_2");
    // not on the layout (latin on a cyrillic one), letters and digits keep their VKs
    CHECK(char_key('a', -1, &k, &needs) && k.vk == 'A' && needs == 0, "'a' fallback");
    CHECK(char_key('Z', -1, &k, &needs) && k.vk == 'Z' && needs == KMOD_SHIFT, "'Z' fallback");
    CHECK(char_key('5', -1, &k, &needs) && k.vk == '5' && needs == 0, "'5' fallback");
    CHECK(!char_key('[', -1, &k, &needs), "'[' not on the layout: unknown");
    // a shift state with more than shift/ctrl/alt in it (hankaku, 8) cant be used
    CHECK(char_key('x', 0x0858, &k, &needs) && k.vk == 'X' && needs == 0, "'x' with Hankaku: fallback");
    CHECK(!char_key(';', 0x08BA, &k, &needs), "';' with Hankaku: unknown");
    CHECK(!char_key(' ', 0x0020, &k, &needs), "' ' isn't a character name (\"space\" is)");
    CHECK(!char_key(127, 0x002E, &k, &needs) && !char_key(31, 0x0000, &k, &needs), "DEL and controls are unknown");

    // this machines own layout, letters and digits work on any layout but punctuation
    // depends on it so thats only counted
    HKL layout = target_layout();
    Press p;
    for (int ch = 'a'; ch <= 'z'; ch++) {
        char name[2] = { (char)ch, 0 };
        CHECK(resolve_key(name, layout, &p) && p.key.vk == ch - 'a' + 'A' && p.mod == 0 && !p.key.ext,
              "'%c' should be its letter key", ch);
        CHECK(p.needs == 0, "'%c' shouldn't need modifiers", ch);
        name[0] = (char)(ch - 'a' + 'A');
        CHECK(resolve_key(name, layout, &p) && p.key.vk == name[0] && p.needs == KMOD_SHIFT,
              "'%c' should be Shift+%c", name[0], name[0]);
    }
    for (int ch = '0'; ch <= '9'; ch++) {
        char name[2] = { (char)ch, 0 };
        CHECK(resolve_key(name, layout, &p) && p.key.vk == ch, "'%c' should be its digit key", ch);
    }
    int known = 0, total = 0;
    for (int ch = 33; ch < 127; ch++) {
        char name[2] = { (char)ch, 0 };
        total++;
        known += platform_key_known(name);
    }
    printf("  layout %p: %d of %d printable characters on it\n", (void *)layout, known, total);
    CHECK(resolve_key("a", layout, &p) && p.key.scan != 0, "'a' should get a scan code from the layout");
}

static void check_seq(const Press *ps, int n, unsigned held, const char *want, const char *what)
{
    INPUT in[2 * PRESS_INPUTS_MAX * COMBO_MAX];
    int count = combo_inputs(ps, n, held, in);
    const char *got = seq(in, count);
    CHECK(strcmp(got, want) == 0, "%s: got \"%s\", want \"%s\"", what, got, want);
    for (int i = 0; i < count; i++) {
        CHECK(in[i].type == INPUT_KEYBOARD && in[i].ki.dwExtraInfo == INJECT_TAG &&
              !(in[i].ki.dwFlags & KEYEVENTF_UNICODE) && in[i].ki.time == 0, "%s: input %d malformed", what, i);
    }
}

static void test_combos(void)
{
    printf("combos\n");
    Press ctrl_s[] = { named("ctrl"), letter('S', 0) };
    check_seq(ctrl_s, 2, 0, "ctrl+ S+ S- ctrl-", "ctrl+s");
    Press ctrl_A[] = { named("ctrl"), letter('A', KMOD_SHIFT) };
    check_seq(ctrl_A, 2, 0, "ctrl+ shift+ A+ A- shift- ctrl-", "ctrl+A adds Shift");
    Press shift_A[] = { named("shift"), letter('A', KMOD_SHIFT) };
    check_seq(shift_A, 2, 0, "shift+ A+ A- shift-", "shift+A: one Shift");
    Press A_shift[] = { letter('A', KMOD_SHIFT), named("shift") };
    check_seq(A_shift, 2, 0, "shift+ A+ A- shift-", "A+shift: one Shift");
    Press AB[] = { letter('A', KMOD_SHIFT), letter('B', KMOD_SHIFT) };
    check_seq(AB, 2, 0, "shift+ A+ B+ B- A- shift-", "A+B share one Shift");
    Press cst[] = { named("ctrl"), named("shift"), letter('T', 0) };
    check_seq(cst, 3, 0, "ctrl+ shift+ T+ T- shift- ctrl-", "ctrl+shift+t");
    Press at[] = { letter('Q', KMOD_CTRL | KMOD_ALT) };
    check_seq(at, 1, 0, "ctrl+ alt+ Q+ Q- alt- ctrl-", "AltGr character");
    Press all[] = { named("super"), named("alt"), named("ctrl"), named("shift"), named("f5") };
    check_seq(all, 5, 0, "super+ alt+ ctrl+ shift+ f5+ f5- shift- ctrl- alt- super-", "four modifiers");
    Press single[] = { named("enter") };
    check_seq(single, 1, 0, "enter+ enter-", "enter");

    // already held through platform_key (like a ctrl+click) so its left alone
    check_seq(ctrl_s, 2, KMOD_CTRL, "S+ S-", "ctrl+s with Ctrl held");
    check_seq(ctrl_A, 2, KMOD_SHIFT, "ctrl+ A+ A- ctrl-", "ctrl+A with Shift held");

    // extended flag and scan codes come along
    Press cad[] = { named("ctrl"), named("alt"), named("delete") };
    INPUT in[2 * PRESS_INPUTS_MAX * COMBO_MAX];
    int n = combo_inputs(cad, 3, 0, in);
    CHECK(n == 6 && (in[2].ki.dwFlags & KEYEVENTF_EXTENDEDKEY) && (in[3].ki.dwFlags & KEYEVENTF_EXTENDEDKEY) &&
          !(in[0].ki.dwFlags & KEYEVENTF_EXTENDEDKEY) && in[2].ki.wScan == 0x53 && in[0].ki.wScan == 0x1D,
          "delete in a combo should be extended, with its scan code");

    // press_inputs by itself, what platform_key sends for a character down and up
    n = press_inputs(letter('A', KMOD_SHIFT).key, KMOD_SHIFT, true, in);
    CHECK(strcmp(seq(in, n), "shift+ A+") == 0, "key down with Shift: %s", seq(in, n));
    n = press_inputs(letter('A', KMOD_SHIFT).key, KMOD_SHIFT, false, in);
    CHECK(strcmp(seq(in, n), "A- shift-") == 0, "key up with Shift: %s", seq(in, n));
    n = press_inputs(named("left").key, 0, true, in);
    CHECK(n == 1 && in[0].ki.wVk == VK_LEFT && in[0].ki.dwFlags == KEYEVENTF_EXTENDEDKEY, "left down");
    n = press_inputs(named("left").key, 0, false, in);
    CHECK(n == 1 && in[0].ki.dwFlags == (KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP), "left up");

    // refused before anything gets built, too many, too few, unknown name
    const char *too_many[COMBO_MAX + 1];
    for (int i = 0; i <= COMBO_MAX; i++) too_many[i] = "a";
    CHECK(!platform_keys(too_many, COMBO_MAX + 1), "more than COMBO_MAX names");
    CHECK(!platform_keys(too_many, 0), "no names");
    const char *bad[] = { "ctrl", "Return" };
    CHECK(!platform_keys(bad, 2), "an unknown name refuses the combo");
    const char *bad_first[] = { "nope", "s" };
    CHECK(!platform_keys(bad_first, 2), "an unknown first name refuses the combo");
    CHECK(!platform_key("Return", true) && !platform_key("", false) && !platform_key(NULL, true),
          "platform_key refuses unknown names");
}

static void test_typing(void)
{
    printf("typing\n");
    INPUT in[4];

    int n = char_inputs('a', in);
    CHECK(n == 2 && in[0].type == INPUT_KEYBOARD && in[0].ki.wVk == 0 && in[0].ki.wScan == 'a' &&
          in[0].ki.dwFlags == KEYEVENTF_UNICODE && in[1].ki.wScan == 'a' &&
          in[1].ki.dwFlags == (KEYEVENTF_UNICODE | KEYEVENTF_KEYUP) && in[0].ki.dwExtraInfo == INJECT_TAG,
          "'a' should be one Unicode down and up");
    CHECK(char_inputs(' ', in) == 2 && in[0].ki.wScan == ' ', "space types as a character");
    CHECK(char_inputs(0xE9, in) == 2 && in[0].ki.wScan == 0xE9, "U+00E9");
    CHECK(char_inputs(0x20AC, in) == 2 && in[0].ki.wScan == 0x20AC, "U+20AC");
    CHECK(char_inputs(0xFFFD, in) == 2 && in[1].ki.wScan == 0xFFFD, "U+FFFD");

    // past U+FFFF its a surrogate pair, down and up for each half, high one first
    static const struct { unsigned cp; WORD hi, lo; } PAIRS[] = {
        { 0x1F600, 0xD83D, 0xDE00 },        // grinning face
        { 0x10000, 0xD800, 0xDC00 },
        { 0x10FFFF, 0xDBFF, 0xDFFF },
        { 0x1D11E, 0xD834, 0xDD1E },        // musical G clef
    };
    for (int i = 0; i < (int)(sizeof PAIRS / sizeof PAIRS[0]); i++) {
        n = char_inputs(PAIRS[i].cp, in);
        CHECK(n == 4, "U+%X: %d inputs", PAIRS[i].cp, n);
        if (n != 4) continue;
        CHECK(in[0].ki.wScan == PAIRS[i].hi && in[1].ki.wScan == PAIRS[i].hi &&
              in[2].ki.wScan == PAIRS[i].lo && in[3].ki.wScan == PAIRS[i].lo,
              "U+%X: got %04X %04X %04X %04X", PAIRS[i].cp, in[0].ki.wScan, in[1].ki.wScan, in[2].ki.wScan, in[3].ki.wScan);
        for (int j = 0; j < 4; j++)
            CHECK(in[j].ki.wVk == 0 && in[j].ki.dwFlags == (KEYEVENTF_UNICODE | (j & 1 ? KEYEVENTF_KEYUP : 0)),
                  "U+%X: input %d flags 0x%lX", PAIRS[i].cp, j, in[j].ki.dwFlags);
    }

    // control chars press the real key instead, or nothing
    n = char_inputs('\n', in);
    CHECK(n == 2 && in[0].ki.wVk == VK_RETURN && in[0].ki.wScan == 0x1C && in[0].ki.dwFlags == 0 &&
          in[1].ki.wVk == VK_RETURN && in[1].ki.dwFlags == KEYEVENTF_KEYUP, "'\\n' should press Enter");
    n = char_inputs('\t', in);
    CHECK(n == 2 && in[0].ki.wVk == VK_TAB && in[0].ki.dwFlags == 0 && in[1].ki.dwFlags == KEYEVENTF_KEYUP,
          "'\\t' should press Tab");
    CHECK(char_inputs('\r', in) == 0, "'\\r' sends nothing");
    static const unsigned BAD[] = { 0, 7, 8, 0x1B, 0x1F, 0x7F, 0x85, 0x9F, 0xD800, 0xDBFF, 0xDC00, 0xDFFF, 0x110000, 0xFFFFFFFF };
    for (int i = 0; i < (int)(sizeof BAD / sizeof BAD[0]); i++)
        CHECK(char_inputs(BAD[i], in) == -1, "U+%X can't be typed", BAD[i]);
    CHECK(!platform_type_codepoint(0x1B) && !platform_type_codepoint(0xD800), "refused before sending");
}

static void test_mouse(void)
{
    printf("mouse\n");
    INPUT in[2 * WHEEL_NOTCHES_MAX];

    // dy > 0 is down which is negative for the wheel, dx > 0 is right which is positive
    int n = wheel_inputs(0, 3, in);
    CHECK(n == 3, "3 notches down: %d events", n);
    for (int i = 0; i < n; i++)
        CHECK(in[i].type == INPUT_MOUSE && in[i].mi.dwFlags == MOUSEEVENTF_WHEEL && (int)in[i].mi.mouseData == -WHEEL_DELTA &&
              in[i].mi.dx == 0 && in[i].mi.dy == 0 && in[i].mi.dwExtraInfo == INJECT_TAG, "down notch %d", i);
    n = wheel_inputs(0, -2, in);
    CHECK(n == 2 && in[0].mi.dwFlags == MOUSEEVENTF_WHEEL && (int)in[0].mi.mouseData == WHEEL_DELTA &&
          (int)in[1].mi.mouseData == WHEEL_DELTA, "2 notches up");
    n = wheel_inputs(2, 0, in);
    CHECK(n == 2 && in[0].mi.dwFlags == MOUSEEVENTF_HWHEEL && (int)in[0].mi.mouseData == WHEEL_DELTA &&
          (int)in[1].mi.mouseData == WHEEL_DELTA, "2 notches right");
    n = wheel_inputs(-1, 0, in);
    CHECK(n == 1 && in[0].mi.dwFlags == MOUSEEVENTF_HWHEEL && (int)in[0].mi.mouseData == -WHEEL_DELTA, "1 notch left");
    n = wheel_inputs(-2, 1, in);
    CHECK(n == 3 && in[0].mi.dwFlags == MOUSEEVENTF_WHEEL && (int)in[0].mi.mouseData == -WHEEL_DELTA &&
          in[1].mi.dwFlags == MOUSEEVENTF_HWHEEL && (int)in[2].mi.mouseData == -WHEEL_DELTA,
          "down then left: vertical first");
    CHECK(wheel_inputs(0, 0, in) == 0, "no notches, no events");
    CHECK(wheel_inputs(1000, -1000, in) == 2 * WHEEL_NOTCHES_MAX, "clamped to %d a side", WHEEL_NOTCHES_MAX);

    // "left" means the primary button, so the physical right one if you swapped them
    static const struct { bool right, swapped, down; DWORD want; } BUTTONS[] = {
        { false, false, true,  MOUSEEVENTF_LEFTDOWN },  { false, false, false, MOUSEEVENTF_LEFTUP },
        { true,  false, true,  MOUSEEVENTF_RIGHTDOWN }, { true,  false, false, MOUSEEVENTF_RIGHTUP },
        { false, true,  true,  MOUSEEVENTF_RIGHTDOWN }, { false, true,  false, MOUSEEVENTF_RIGHTUP },
        { true,  true,  true,  MOUSEEVENTF_LEFTDOWN },  { true,  true,  false, MOUSEEVENTF_LEFTUP },
    };
    for (int i = 0; i < (int)(sizeof BUTTONS / sizeof BUTTONS[0]); i++) {
        INPUT b = button_input(BUTTONS[i].right, BUTTONS[i].swapped, BUTTONS[i].down);
        CHECK(b.type == INPUT_MOUSE && b.mi.dwFlags == BUTTONS[i].want && b.mi.dx == 0 && b.mi.dy == 0 &&
              b.mi.mouseData == 0, "button %d: flags 0x%lX, want 0x%lX (no move: where the pointer is)",
              i, b.mi.dwFlags, BUTTONS[i].want);
    }
}

static void test_edge(void)
{
    printf("esc edge\n");
    KeyEdge e = { 0 };
    CHECK(!edge_pressed(&e, (SHORT)0x8001), "held before the first call doesn't count");
    CHECK(!edge_pressed(&e, (SHORT)0x8000), "still held");
    CHECK(!edge_pressed(&e, 0), "released");
    CHECK(edge_pressed(&e, (SHORT)0x8001), "pressed");
    CHECK(!edge_pressed(&e, (SHORT)0x8000), "once per press");
    CHECK(!edge_pressed(&e, 0), "released again");
    CHECK(edge_pressed(&e, 0x0001), "a tap between two polls");
    CHECK(!edge_pressed(&e, 0), "nothing since");
    KeyEdge fresh = { 0 };
    CHECK(!edge_pressed(&fresh, 0x0001), "a tap before the first call doesn't count");
}

int main(void)
{
    test_table();
    test_chars();
    test_combos();
    test_typing();
    test_mouse();
    test_edge();
    CHECK(injected == 0, "something was injected");
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
