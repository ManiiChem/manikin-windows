#include <math.h>
#include <string.h>

#include "chat.h"
#include "ui.h"

#define CHAT_PAD 16                  // border + inner margin, each side
#define CHAT_LINE_H 24               // line pitch, a multiple of UI_PX so highlights stay on the grid
#define CHAT_TOP 12                  // box top to the first line's highlight band
#define CHAT_H1 (2 * CHAT_TOP + CHAT_LINE_H)    // one line box is 48

typedef struct { int start, len; } ChatLine;

static char input[CHAT_MAX + 1];
static int input_len;
static int caret, anchor;            // selection is [min, max) of the two
static bool eol;                     // caret at a soft wrap shows at the end of the upper line
static float goal_x = -1;            // Up/Down stick to this px column, -1 = take it from the caret
static int top;                      // first visible line
static int pw_off;                   // password box, first visible char
static double blink_t0;
static bool interacted;
static bool last_password, last_can_type;

static float wrap_w = 344;           // set by chat_rect/chat_draw from the box width
static ChatLine lines[CHAT_MAX + 1];
static int line_count;

// raylib's default font isnt monospace. DrawTextEx moves each glyph by its width plus the
// spacing, so the width of a run is just the sum of these
static float adv[128];

static float char_adv(char c)
{
    if (adv[' '] == 0) {
        for (int i = 32; i < 127; i++) {
            char s[2] = { (char)i, 0 };
            adv[i] = MeasureTextEx(GetFontDefault(), s, UI_FONT_SIZE, UI_FONT_SPACING).x + UI_FONT_SPACING;
        }
    }
    return adv[(unsigned char)c & 127];
}

static char shown(int i) { return last_password ? '*' : input[i]; }

static float span_w(int a, int b)
{
    float w = 0;
    for (int i = a; i < b; i++) w += char_adv(shown(i));
    return w;
}

static bool is_space(int i) { return input[i] == ' '; }

// greedy word wrap. lines cover [start, start+len) back to back so every index is on
// exactly one line, and a space where a line overflows hangs off the end of it
static void layout(void)
{
    line_count = 0;
    if (last_password) {                         // one line, scrolled sideways
        lines[line_count++] = (ChatLine){ 0, input_len };
        return;
    }
    int s = 0;
    do {
        float x = -UI_FONT_SPACING;              // the last glyph's spacing doesnt count
        int j = s;
        while (j < input_len && x + char_adv(input[j]) <= wrap_w) x += char_adv(input[j++]);
        int end;
        if (j == input_len) {
            end = input_len;
        } else if (is_space(j)) {
            end = j;
            while (end < input_len && is_space(end)) end++;
        } else {
            int k = j - 1;
            while (k >= s && !is_space(k)) k--;
            end = k >= s ? k + 1 : (j > s ? j : s + 1);   // no space so break the word
        }
        lines[line_count++] = (ChatLine){ s, end - s };
        s = end;
    } while (s < input_len);
}

static int line_end(int i) { return lines[i].start + lines[i].len; }

static int line_of(int c)
{
    for (int i = 0; i < line_count; i++) {
        int en = line_end(i);
        if (eol && c == en && c > lines[i].start) return i;
        if (c < en || i == line_count - 1) return i;
    }
    return 0;
}

// the caret cant sit after a hanging space, that spot is the next line's start
static int line_last_pos(int i)
{
    int en = line_end(i);
    if (i < line_count - 1 && en > lines[i].start && is_space(en - 1)) return en - 1;
    return en;
}

// closest caret position to px column `x` on line `i`
static int pos_at_x(int i, float x)
{
    int st = lines[i].start, last = line_last_pos(i), best = st;
    float cx = 0;
    for (int k = st; k < last; k++) {
        float a = char_adv(shown(k));
        if (x < cx + a / 2) break;
        cx += a;
        best = k + 1;
    }
    eol = best == line_end(i) && i < line_count - 1;
    return best;
}

static void scroll_to_caret(void)
{
    layout();
    int i = line_of(caret);
    if (i < top) top = i;
    if (i >= top + CHAT_VISIBLE_LINES) top = i - CHAT_VISIBLE_LINES + 1;
    int max_top = line_count > CHAT_VISIBLE_LINES ? line_count - CHAT_VISIBLE_LINES : 0;
    if (top > max_top) top = max_top;
    if (top < 0) top = 0;

    // password box, keep the caret in view and dont leave empty room on the right
    float star = char_adv('*');
    int fit = (int)((wrap_w + UI_FONT_SPACING) / star);
    if (caret < pw_off) pw_off = caret;
    if (caret - pw_off > fit) pw_off = caret - fit;
    if (input_len - pw_off < fit) pw_off = input_len > fit ? input_len - fit : 0;
}

static bool has_text(void)
{
    for (const char *s = input; *s; s++)
        if (*s != ' ') return true;
    return false;
}

static int sel_lo(void) { return caret < anchor ? caret : anchor; }
static int sel_hi(void) { return caret < anchor ? anchor : caret; }

static void touch(void) { blink_t0 = GetTime(); }

static void move_to(int c, bool extend)
{
    caret = c < 0 ? 0 : c > input_len ? input_len : c;
    if (!extend) anchor = caret;
    touch();
}

static void delete_range(int a, int b)
{
    if (b <= a) return;
    memmove(input + a, input + b, input_len - b + 1);
    memset(input + input_len - (b - a), 0, b - a);   // no stale bytes past the end
    input_len -= b - a;
    caret = anchor = a;
    eol = false;
    goal_x = -1;
    touch();
}

static bool delete_selection(void)
{
    if (caret == anchor) return false;
    delete_range(sel_lo(), sel_hi());
    return true;
}

static void insert(const char *s, int n)
{
    delete_selection();
    if (n > CHAT_MAX - input_len) n = CHAT_MAX - input_len;
    if (n <= 0) return;
    memmove(input + caret + n, input + caret, input_len - caret + 1);
    memcpy(input + caret, s, n);
    input_len += n;
    caret = anchor = caret + n;
    eol = false;
    goal_x = -1;
    touch();
}

static int word_left(int c)
{
    while (c > 0 && is_space(c - 1)) c--;
    while (c > 0 && !is_space(c - 1)) c--;
    return c;
}

static int word_right(int c)
{
    while (c < input_len && !is_space(c)) c++;
    while (c < input_len && is_space(c)) c++;
    return c;
}

static void copy_selection(void)
{
    char buf[CHAT_MAX + 1];
    int n = sel_hi() - sel_lo();
    memcpy(buf, input + sel_lo(), n);
    buf[n] = 0;
    SetClipboardText(buf);
}

static void paste(void)
{
    const char *clip = GetClipboardText();
    char buf[CHAT_MAX + 1];
    int n = 0;
    for (; clip && *clip && n < CHAT_MAX; clip++) {
        char c = *clip == '\n' || *clip == '\t' ? ' ' : *clip;
        if (c >= 32 && c < 127) buf[n++] = c;
    }
    insert(buf, n);
}

// Up/Down, same px column on the line above or below
static void move_vertical(int dir, bool extend)
{
    layout();
    int i = line_of(caret);
    float x = goal_x >= 0 ? goal_x : span_w(lines[i].start, caret);
    int t = i + dir;
    if (t < 0) { eol = false; move_to(0, extend); }
    else if (t >= line_count) { eol = false; move_to(input_len, extend); }
    else move_to(pos_at_x(t, x), extend);
    goal_x = x;
}

static void handle_key(int key, bool ctrl, bool shift, bool password)
{
    bool word = ctrl && !password;               // word boundaries would leak the password's shape
    int lo = sel_lo(), hi = sel_hi();
    if (key != KEY_UP && key != KEY_DOWN) goal_x = -1;

    switch (key) {
    case KEY_LEFT:
        eol = false;
        if (!shift && lo != hi && !ctrl) move_to(lo, false);
        else move_to(word ? word_left(caret) : ctrl ? 0 : caret - 1, shift);
        break;
    case KEY_RIGHT:
        eol = false;
        if (!shift && lo != hi && !ctrl) move_to(hi, false);
        else move_to(word ? word_right(caret) : ctrl ? input_len : caret + 1, shift);
        break;
    case KEY_HOME:
        if (ctrl) { eol = false; move_to(0, shift); break; }
        layout();
        { int i = line_of(caret); eol = false; move_to(lines[i].start, shift); }
        break;
    case KEY_END:
        if (ctrl) { eol = false; move_to(input_len, shift); break; }
        layout();
        { int i = line_of(caret), p = line_last_pos(i);
          eol = p == line_end(i) && i < line_count - 1;
          move_to(p, shift); }
        break;
    case KEY_UP:   move_vertical(-1, shift); break;
    case KEY_DOWN: move_vertical(1, shift); break;
    case KEY_BACKSPACE:
        if (!delete_selection() && caret > 0) delete_range(word ? word_left(caret) : caret - 1, caret);
        break;
    case KEY_DELETE:
        if (!delete_selection() && caret < input_len) delete_range(caret, word ? word_right(caret) : caret + 1);
        break;
    case KEY_A:
        if (ctrl) { anchor = 0; caret = input_len; eol = false; touch(); }
        break;
    case KEY_C:
        if (ctrl && !password && lo != hi) copy_selection();
        break;
    case KEY_X:
        if (ctrl && !password && lo != hi) { copy_selection(); delete_selection(); }
        break;
    case KEY_V:
        if (ctrl) paste();
        break;
    default: break;
    }
}

// keys that repeat while held. not Enter, that would send twice
static const int repeat_keys[] = {
    KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END,
    KEY_BACKSPACE, KEY_DELETE, KEY_V,
};

ChatAction chat_update(bool can_type, bool password)
{
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (password != last_password) eol = false;
    last_password = password;
    last_can_type = can_type;

    // first presses come from raylib's queue, IsKeyPressed misses a tap that goes down and up
    // within one frame. held keys come back through IsKeyPressedRepeat
    int keys[32], nkeys = 0;
    bool enter = false, escape = false, quit = false, mute = false;
    interacted = false;
    int key;
    while ((key = GetKeyPressed()) != 0) {
        interacted = true;
        switch (key) {
        case KEY_ENTER: case KEY_KP_ENTER: enter = true; break;
        case KEY_ESCAPE: escape = true; break;
        case KEY_Q: if (ctrl) quit = true; break;
        case KEY_M: if (ctrl) mute = true; break;
        default: if (nkeys < 32) keys[nkeys++] = key; break;
        }
    }
    for (int i = 0; i < (int)(sizeof repeat_keys / sizeof repeat_keys[0]); i++) {
        if (IsKeyPressedRepeat(repeat_keys[i]) && nkeys < 32) {
            keys[nkeys++] = repeat_keys[i];
            interacted = true;
        }
    }

    // typed text goes in before the keys since the two queues lose their order within a
    // frame, and "text then Enter" is the common case
    char typed[64];
    int ntyped = 0, ch;
    while ((ch = GetCharPressed()) != 0) {           // always drain the queue
        if (!ctrl && ch >= 32 && ch < 127 && ntyped < (int)sizeof typed) typed[ntyped++] = (char)ch;
    }
    if (quit) return CHAT_QUIT;

    if (can_type) {
        if (ntyped > 0) insert(typed, ntyped);
        for (int i = 0; i < nkeys; i++) handle_key(keys[i], ctrl, shift, password);
        scroll_to_caret();
        if (enter && (password ? input_len > 0 : has_text())) return CHAT_SUBMIT;
    }
    if (mute) return CHAT_TOGGLE_MUTE;
    return escape ? CHAT_ESCAPE : CHAT_NONE;
}

const char *chat_text(void) { return input; }
int chat_len(void) { return input_len; }
bool chat_interacted(void) { return interacted; }

void chat_clear(void)
{
    memset(input, 0, sizeof input);
    input_len = caret = anchor = 0;
    eol = false;
    goal_x = -1;
    top = pw_off = 0;
    touch();
}

static float box_height(void)
{
    layout();
    int n = line_count < CHAT_VISIBLE_LINES ? line_count : CHAT_VISIBLE_LINES;
    return (float)(CHAT_H1 + (n - 1) * CHAT_LINE_H);
}

Rectangle chat_rect(float x, float y, float w)
{
    wrap_w = w - 2 * CHAT_PAD - UI_PX;           // keep room for the caret after a full line
    return (Rectangle){ x, y, w, box_height() };
}

void chat_click(Rectangle r, Vector2 mouse)
{
    if (!last_can_type) return;
    wrap_w = r.width - 2 * CHAT_PAD - UI_PX;
    layout();
    int row = (int)floorf((mouse.y - r.y - CHAT_TOP) / CHAT_LINE_H);
    if (row < 0) row = 0;
    if (row > CHAT_VISIBLE_LINES - 1) row = CHAT_VISIBLE_LINES - 1;
    int i = top + row;
    if (i > line_count - 1) i = line_count - 1;
    float x = mouse.x - r.x - CHAT_PAD;
    if (last_password) x += span_w(0, pw_off);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    goal_x = -1;
    move_to(pos_at_x(i, x), shift);
    scroll_to_caret();
}

static void draw_run(int a, int b, float x, float y, Color color)
{
    char buf[CHAT_MAX + 1];
    int n = 0;
    for (int i = a; i < b; i++) buf[n++] = shown(i);
    buf[n] = 0;
    DrawTextEx(GetFontDefault(), buf, (Vector2){ x, y }, UI_FONT_SIZE, UI_FONT_SPACING, color);
}

// chars [a, b) at x, the selected part gets drawn inverted (paper text on ink)
static void draw_line(int a, int b, float x, float band_y, float right)
{
    float ty = band_y + (CHAT_LINE_H - UI_FONT_SIZE) / 2.0f;
    int lo = sel_lo(), hi = sel_hi();
    int s0 = lo > a ? lo : a, s1 = hi < b ? hi : b;
    if (s0 >= s1) {
        draw_run(a, b, x, ty, UI_INK);
        return;
    }
    float x0 = x + span_w(a, s0), x1 = x0 + span_w(s0, s1);
    float l = x0 - UI_FONT_SPACING, rgt = x1 < right ? x1 : right;   // hanging spaces stop at the edge
    if (rgt > l) DrawRectangle((int)l, (int)band_y, (int)(rgt - l), CHAT_LINE_H, UI_INK);
    draw_run(a, s0, x, ty, UI_INK);
    draw_run(s0, s1, x0, ty, UI_PAPER);
    draw_run(s1, b, x1, ty, UI_INK);
}

void chat_draw(Rectangle r, bool cursor_on, bool password, const char *placeholder)
{
    last_password = password;
    wrap_w = r.width - 2 * CHAT_PAD - UI_PX;
    r.height = box_height();                     // text may have changed since chat_rect
    ui_box_draw(r);

    float tx = r.x + CHAT_PAD, right = tx + wrap_w + UI_FONT_SPACING;
    float band0 = r.y + CHAT_TOP;
    bool caret_on = cursor_on && fmod(GetTime() - blink_t0, 1.0) < 0.5;

    if (input_len == 0) {
        DrawTextEx(GetFontDefault(), placeholder,
                   (Vector2){ tx + 2 * UI_PX, band0 + (CHAT_LINE_H - UI_FONT_SIZE) / 2.0f },
                   UI_FONT_SIZE, UI_FONT_SPACING, UI_HINT);
    }

    int caret_line = line_of(caret);
    for (int row = 0; row < CHAT_VISIBLE_LINES && top + row < line_count; row++) {
        int i = top + row;
        int a = lines[i].start, b = line_end(i);
        if (password) {                              // only the part that fits
            a = pw_off;
            float w = 0;
            b = a;
            while (b < input_len && w + char_adv('*') <= wrap_w + UI_FONT_SPACING) {
                w += char_adv('*');
                b++;
            }
        }
        float band_y = band0 + row * CHAT_LINE_H;
        draw_line(a, b, tx, band_y, right);

        // the caret sits in the 2px gap between glyphs, hidden while selecting
        if (i == caret_line && caret_on && caret == anchor) {
            float cx = tx + span_w(a, caret);
            if (cx > right) cx = right;
            DrawRectangle((int)cx - UI_FONT_SPACING, (int)band_y, UI_FONT_SPACING, CHAT_LINE_H, UI_INK);
        }
    }
}
