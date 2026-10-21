#include <math.h>
#include <string.h>

#include "ui.h"
#include "anim_emotion.h"

const Color UI_INK   = { 24, 20, 37, 255 };
const Color UI_PAPER = { 255, 255, 255, 255 };
const Color UI_HALO  = { 255, 255, 255, 255 };
const Color UI_HINT  = { 150, 148, 160, 255 };

#define ARROW_W 9
#define ARROW_H 5
static const char ARROW_DOWN[] =
    "#########"
    ".#ooooo#."
    "..#ooo#.."
    "...#o#..."
    "....#....";
static const char ARROW_UP[] =
    "....#...."
    "...#o#..."
    "..#ooo#.."
    ".#ooooo#."
    "#########";

#define TAIL_W 7
#define TAIL_H 4
static const char TAIL[] =
    "#ooooo#"
    ".#ooo#."
    "..#o#.."
    "...#...";

#define THINK_W 88
#define THINK_H 48
#define THINK_DOTS_W 28              // room for the "..." after a status label

#define NOTE_SIZE 10                 // corner notes ("2/3") in the default font's native size
#define NOTE_SPACING 1

#define CHECK_W 7
#define CHECK_H 5
static const char CHECK[] =
    "......#"
    ".....##"
    "#...##."
    "##.##.."
    ".###...";

#define CHECK2_W 12
#define CHECK2_H 5
static const char CHECK2[] =    // double tick, yes for the rest of this session
    "......#....#"
    ".....##...##"
    "#...##...##."
    "##.##...##.."
    ".###...##...";

#define CROSS_W 5
#define CROSS_H 5
static const char CROSS[] =
    "#...#"
    ".#.#."
    "..#.."
    ".#.#."
    "#...#";

#define MORE_W 5
#define MORE_H 3
static const char MORE[] =      // "theres another page" marker
    "#####"
    ".###."
    "..#..";

#define BUTTON_W 64
#define BUTTON_H 30
#define BUTTON_GAP 16
#define BUTTON_ROW_H (BUTTON_H + 12)

void ui_draw_cells_pal(const char *cells, int w, int h, int x, int y, int px,
                       const Color palette[128], bool halo)
{
    if (halo) {
        for (int cy = 0; cy < h; cy++)
            for (int cx = 0; cx < w; cx++) {
                unsigned char c = (unsigned char)cells[cy * w + cx];
                if (c >= 128 || palette[c].a == 0) continue;
                DrawRectangle(x + (cx - 1) * px, y + (cy - 1) * px, 3 * px, 3 * px, UI_HALO);
            }
    }
    for (int cy = 0; cy < h; cy++)
        for (int cx = 0; cx < w; cx++) {
            unsigned char c = (unsigned char)cells[cy * w + cx];
            if (c >= 128 || palette[c].a == 0) continue;
            DrawRectangle(x + cx * px, y + cy * px, px, px, palette[c]);
        }
}

void ui_draw_cells(const char *cells, int w, int h, int x, int y, int px, bool halo)
{
    static Color palette[128];
    palette['#'] = UI_INK;
    palette['o'] = UI_PAPER;
    ui_draw_cells_pal(cells, w, h, x, y, px, palette, halo);
}

// rectangle with 1 cell notched corners so it looks pixel rounded
static void box_shape(int x, int y, int w, int h, Color fill, Color line)
{
    const int p = UI_PX;
    DrawRectangle(x + p, y + p, w - 2 * p, h - 2 * p, fill);
    DrawRectangle(x + 2 * p, y, w - 4 * p, p, line);
    DrawRectangle(x + 2 * p, y + h - p, w - 4 * p, p, line);
    DrawRectangle(x, y + 2 * p, p, h - 4 * p, line);
    DrawRectangle(x + w - p, y + 2 * p, p, h - 4 * p, line);
    DrawRectangle(x + p, y + p, p, p, line);                   // corner pixels
    DrawRectangle(x + w - 2 * p, y + p, p, p, line);
    DrawRectangle(x + p, y + h - 2 * p, p, p, line);
    DrawRectangle(x + w - 2 * p, y + h - 2 * p, p, p, line);
}

void ui_box_draw(Rectangle r)
{
    int x = (int)r.x, y = (int)r.y, w = (int)r.width, h = (int)r.height, p = UI_PX;
    box_shape(x - p, y - p, w + 2 * p, h + 2 * p, UI_HALO, UI_HALO);
    box_shape(x, y, w, h, UI_PAPER, UI_INK);
}

static float text_width(const char *s, int len)
{
    char buf[UI_TEXT_MAX];
    if (len >= (int)sizeof buf) len = (int)sizeof buf - 1;
    memcpy(buf, s, len);
    buf[len] = 0;
    return MeasureTextEx(GetFontDefault(), buf, UI_FONT_SIZE, UI_FONT_SPACING).x;
}

static void draw_text_n(const char *s, int len, float x, float y, Color color)
{
    char buf[UI_TEXT_MAX];
    if (len >= (int)sizeof buf) len = (int)sizeof buf - 1;
    memcpy(buf, s, len);
    buf[len] = 0;
    DrawTextEx(GetFontDefault(), buf, (Vector2){ x, y }, UI_FONT_SIZE, UI_FONT_SPACING, color);
}

void ui_bubble_set(UiBubble *b, const char *text, UiButtons buttons)
{
    b->buttons = buttons;
    b->page = 0;
    const float max_w = UI_BUBBLE_MAX_W - 2 * UI_BUBBLE_PAD;
    int len = (int)strlen(text);
    if (len >= UI_TEXT_MAX) len = UI_TEXT_MAX - 1;
    memcpy(b->text, text, len);
    b->text[len] = 0;

    b->line_count = 0;
    float widest = 0;
    int start = 0;
    while (start < len && b->line_count < UI_BUBBLE_MAX_LINES) {
        while (start < len && b->text[start] == ' ') start++;
        if (start >= len) break;
        if (b->text[start] == '\n') {         // explicit line break (blank line if doubled)
            if (start > 0 && b->text[start - 1] == '\n')
                b->lines[b->line_count++] = (UiLine){ start, 0 };
            start++;
            continue;
        }

        // take whole words while they fit, a newline ends the line
        int end = -1, i = start;
        for (;;) {
            int j = i;
            while (j < len && b->text[j] != ' ' && b->text[j] != '\n') j++;
            if (text_width(b->text + start, j - start) > max_w) break;
            end = j;
            if (j >= len || b->text[j] == '\n') break;
            i = j + 1;
        }
        // a single word wider than the bubble gets split mid word
        if (end < 0) {
            end = start + 1;
            while (end < len && text_width(b->text + start, end + 1 - start) <= max_w) end++;
        }

        float w = text_width(b->text + start, end - start);
        if (w > widest) widest = w;
        b->lines[b->line_count++] = (UiLine){ start, end - start };
        start = end;
    }
    b->page_count = b->line_count > 0 ? (b->line_count + UI_PAGE_LINES - 1) / UI_PAGE_LINES : 1;

    int w = (int)widest + 2 * UI_BUBBLE_PAD;
    int min_w = buttons == UI_BUTTONS_SESSION ? 3 * BUTTON_W + 2 * BUTTON_GAP + 2 * UI_BUBBLE_PAD
              : buttons ? 2 * BUTTON_W + BUTTON_GAP + 2 * UI_BUBBLE_PAD : 64;
    if (w < min_w) w = min_w;
    w = (w + 2 * UI_PX - 1) / (2 * UI_PX) * (2 * UI_PX);   // even cell count keeps the tail centred
    b->w = (float)w;
}

static int page_first(const UiBubble *b)
{
    return b->page * UI_PAGE_LINES;
}

static int page_lines(const UiBubble *b)
{
    int n = b->line_count - page_first(b);
    return n < UI_PAGE_LINES ? (n > 0 ? n : 1) : UI_PAGE_LINES;
}

int ui_bubble_page_chars(const UiBubble *b)
{
    int total = 0;
    for (int i = page_first(b); i < page_first(b) + page_lines(b) && i < b->line_count; i++)
        total += b->lines[i].len;
    return total;
}

bool ui_bubble_next_page(UiBubble *b)
{
    if (b->page + 1 >= b->page_count) return false;
    b->page++;
    return true;
}

int ui_bubble_text_index(const UiBubble *b, int i)
{
    for (int l = page_first(b); l < page_first(b) + page_lines(b) && l < b->line_count; l++) {
        if (i < b->lines[l].len) return b->lines[l].start + i;
        i -= b->lines[l].len;
    }
    return -1;
}

Rectangle ui_bubble_rect(const UiBubble *b, float cx, float bottom)
{
    float h = (float)(page_lines(b) * UI_LINE_H - (UI_LINE_H - UI_FONT_SIZE) + 2 * UI_BUBBLE_PAD);
    if (b->buttons) h += BUTTON_ROW_H;
    return (Rectangle){ (float)((int)cx - (int)b->w / 2), bottom - h, b->w, h };
}

void ui_bubble_buttons(const UiBubble *b, float cx, float bottom, Rectangle *yes, Rectangle *session,
                       Rectangle *no)
{
    Rectangle r = ui_bubble_rect(b, cx, bottom);
    float y = r.y + r.height - UI_BUBBLE_PAD - BUTTON_H + 4;
    if (session) *session = (Rectangle){ 0 };
    if (b->buttons == UI_BUTTONS_SESSION) {     // the session one in the middle
        int mid = (int)cx - BUTTON_W / 2;
        *yes = (Rectangle){ (float)(mid - BUTTON_GAP - BUTTON_W), y, BUTTON_W, BUTTON_H };
        if (session) *session = (Rectangle){ (float)mid, y, BUTTON_W, BUTTON_H };
        *no = (Rectangle){ (float)(mid + BUTTON_W + BUTTON_GAP), y, BUTTON_W, BUTTON_H };
        return;
    }
    *yes = (Rectangle){ (float)((int)cx - BUTTON_GAP / 2 - BUTTON_W), y, BUTTON_W, BUTTON_H };
    *no = (Rectangle){ (float)((int)cx + BUTTON_GAP / 2), y, BUTTON_W, BUTTON_H };
}

static void draw_button(Rectangle r, const char *icon, int iw, int ih, bool hot)
{
    const int px = 3;
    box_shape((int)r.x, (int)r.y, (int)r.width, (int)r.height, hot ? UI_INK : UI_PAPER, UI_INK);
    // icon in paper on a hovered (ink filled) button, ink otherwise
    int x0 = (int)(r.x + (r.width - iw * px) / 2), y0 = (int)(r.y + (r.height - ih * px) / 2);
    for (int cy = 0; cy < ih; cy++)
        for (int cx = 0; cx < iw; cx++)
            if (icon[cy * iw + cx] == '#')
                DrawRectangle(x0 + cx * px, y0 + cy * px, px, px, hot ? UI_PAPER : UI_INK);
}

// `line` is the outline colour, ink or hint grey for an old reply
static void draw_bubble_frame(Rectangle r, float cx, Color line)
{
    Color palette[128] = { 0 };
    palette['#'] = line;
    palette['o'] = UI_PAPER;
    int tx = (int)cx - TAIL_W * UI_PX / 2;
    int ty = (int)(r.y + r.height) - UI_PX;   // first tail row sits on the box's bottom border
    int x = (int)r.x, y = (int)r.y, w = (int)r.width, h = (int)r.height, p = UI_PX;
    ui_draw_cells_pal(TAIL, TAIL_W, TAIL_H, tx, ty, UI_PX, palette, true);
    box_shape(x - p, y - p, w + 2 * p, h + 2 * p, UI_HALO, UI_HALO);
    box_shape(x, y, w, h, UI_PAPER, line);
    ui_draw_cells_pal(TAIL, TAIL_W, TAIL_H, tx, ty, UI_PX, palette, false);   // punches the opening through the border
}

void ui_bubble_pop_draw(Rectangle r, float cx, float scale, bool old)
{
    if (scale <= 0) return;
    // whole cells (even count across so the tail stays centred), never smaller than the
    // notched corners need
    int w = (int)(r.width * scale / (2 * UI_PX) + 0.5f) * 2 * UI_PX;
    int h = (int)(r.height * scale / UI_PX + 0.5f) * UI_PX;
    if (w < 10 * UI_PX) w = 10 * UI_PX;
    if (h < 6 * UI_PX) h = 6 * UI_PX;
    Rectangle s = { (float)((int)cx - w / 2), r.y + r.height - h, (float)w, (float)h };
    draw_bubble_frame(s, cx, old ? UI_HINT : UI_INK);
}

// per glyph noise, same inputs give the same jitter so it holds still between frames
static unsigned hash2(unsigned a, unsigned b)
{
    unsigned h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

static int px_round(float v)
{
    return (int)floorf(v + 0.5f);
}

// a glyph's mood offset in whole px (2 max, except sad's sag). `i` is its index on the page,
// `col`/`len` its spot on its line, `age` secs since it showed up
static void mood_offset(int effect, float t, int i, int col, int len, float age, int *dx, int *dy)
{
    unsigned h;
    *dx = *dy = 0;
    switch (effect) {
    case EMOTION_ANNOYED:            // annoyed jitter, 12 times a sec
        h = hash2((unsigned)(t * 12.0f), (unsigned)i);
        *dx = (int)(h % 3) - 1;
        *dy = (int)((h >> 8) % 3) - 1;
        break;
    case EMOTION_HAPPY:              // little hops rolling along the line
        *dy = -px_round(1.4f * fabsf(sinf(t * 5.0f - i * 0.45f)));
        break;
    case EMOTION_EXCITED:            // bigger faster hops
        *dy = -px_round(2.0f * fabsf(sinf(t * 8.0f - i * 0.55f)));
        break;
    case EMOTION_LAUGHING:           // hopping with the giggles plus a shake
        *dy = -px_round(2.0f * fabsf(sinf(t * 9.0f - i * 0.7f)));
        h = hash2((unsigned)(t * 15.0f), (unsigned)i);
        *dx = (h & 3) != 0 ? 0 : (h & 4) ? 1 : -1;
        break;
    case EMOTION_CONFUSED:           // slow out of step wobble
        *dx = px_round(sinf(t * 1.7f + i * 0.9f));
        *dy = px_round(0.8f * sinf(t * 1.3f + i * 0.55f));
        break;
    case EMOTION_WORRIED:            // nervous tremble, a few glyphs at a time
        h = hash2((unsigned)(t * 20.0f), (unsigned)i);
        *dx = (h % 5 == 0) - (h % 5 == 1);
        break;
    case EMOTION_SAD: {              // each glyph wilts after it shows up, lines sag towards the end
        float k = len > 1 ? (float)col / (len - 1) : 0.0f;
        float wilt = age < 0.8f ? age / 0.8f : 1.0f;
        float sag = 1.0f + 4.0f * k * sqrtf(k);                 // 1px at the start, 5 at the end
        *dy = px_round(sag * wilt * (0.85f + 0.15f * sinf(t * 1.4f)));   // a slow sigh
        break;
    }
    case EMOTION_LOVE:               // gentle slow wave
        *dy = px_round(sinf(t * 2.5f - i * 0.3f));
        break;
    case EMOTION_SMUG: {             // lazy sway, and every so often an eyebrow raise rolls along
        *dx = px_round(sinf(t * 1.2f));
        float wave = fmodf(t, 2.5f) * 40.0f;               // glyphs per sec the raise travels
        *dy = fabsf(wave - i) < 2.0f ? -2 : 0;
        break;
    }
    case EMOTION_SURPRISED:          // each glyph pops up as it shows up then settles
        *dy = age < 0.05f ? -2 : age < 0.11f ? -1 : 0;
        break;
    default:
        break;
    }
}

// draws `n` bytes of a line one glyph at a time so each gets its own offset. `first` is the
// line's first page index. it advances like DrawTextEx so the MeasureTextEx wrap still fits
static void draw_glyphs(const char *s, int n, int len, float x, float y, int first,
                        const UiBubbleLook *look)
{
    Font font = GetFontDefault();
    float scale = (float)UI_FONT_SIZE / font.baseSize;
    for (int k = 0; k < n;) {
        int bytes = 1;
        int cp = GetCodepointNext(s + k, &bytes);
        if (k + bytes > n) break;            // a multibyte char thats only partly typed
        int g = GetGlyphIndex(font, cp);
        if (cp != ' ' && cp != '\t') {
            int dx = 0, dy = 0;
            if (look->effect) {
                float age = look->born ? (float)(look->time - look->born[first + k]) : 1e6f;
                // wrapped so the float sines keep their precision when hes been up for days
                float t = (float)fmod(look->time, 3600.0);
                mood_offset(look->effect, t, first + k, k, len, age, &dx, &dy);
            }
            DrawTextCodepoint(font, cp, (Vector2){ x + dx, y + dy }, UI_FONT_SIZE, UI_INK);
        }
        float adv = font.glyphs[g].advanceX ? (float)font.glyphs[g].advanceX : font.recs[g].width;
        x += adv * scale + UI_FONT_SPACING;
        k += bytes;
    }
}

static void draw_note(const char *s, int x, int y)
{
    DrawTextEx(GetFontDefault(), s, (Vector2){ (float)x, (float)y }, NOTE_SIZE, NOTE_SPACING, UI_HINT);
}

void ui_bubble_draw(const UiBubble *b, float cx, float bottom, int shown_chars, UiHover hover,
                    const UiBubbleLook *look)
{
    static const UiBubbleLook plain = { 0 };
    if (!look) look = &plain;
    bool old = look->hist_count > 0;
    Rectangle r = ui_bubble_rect(b, cx, bottom);
    draw_bubble_frame(r, cx, old ? UI_HINT : UI_INK);

    int left = shown_chars, first = page_first(b), at = 0;
    for (int i = 0; i < page_lines(b) && first + i < b->line_count && left > 0; i++) {
        const UiLine *line = &b->lines[first + i];
        int n = line->len < left ? line->len : left;
        draw_glyphs(b->text + line->start, n, line->len,
                    r.x + UI_BUBBLE_PAD, r.y + UI_BUBBLE_PAD + i * UI_LINE_H, at, look);
        at += line->len;
        left -= n;
    }

    // corner notes go in the bottom padding under the text, "<3/10" on the left for an old
    // reply and "2/3" pages on the right
    bool typed = shown_chars >= ui_bubble_page_chars(b);
    int note_y = (int)(r.y + r.height) - 15, right = (int)(r.x + r.width) - 12;
    if (old) draw_note(TextFormat("<%d/%d", look->hist_pos, look->hist_count), (int)r.x + 12, note_y);
    if (b->page_count > 1) {
        const char *pages = TextFormat("%d/%d", b->page + 1, b->page_count);
        right -= (int)MeasureTextEx(GetFontDefault(), pages, NOTE_SIZE, NOTE_SPACING).x;
        draw_note(pages, right, note_y);
        right -= 4;
    }
    if (typed && b->page + 1 < b->page_count && (int)(GetTime() * 2.0) % 2 == 0)
        ui_draw_cells(MORE, MORE_W, MORE_H, right - MORE_W * 2, note_y + 2, 2, false);
    if (b->buttons && typed) {
        Rectangle yes, session, no;
        ui_bubble_buttons(b, cx, bottom, &yes, &session, &no);
        draw_button(yes, CHECK, CHECK_W, CHECK_H, hover == UI_HOVER_YES);
        if (b->buttons == UI_BUTTONS_SESSION)
            draw_button(session, CHECK2, CHECK2_W, CHECK2_H, hover == UI_HOVER_SESSION);
        draw_button(no, CROSS, CROSS_W, CROSS_H, hover == UI_HOVER_NO);
    }
}

// how much of the label fits in the widest thinking bubble
static int label_fit(const char *label)
{
    if (!label) return 0;
    int n = (int)strlen(label);
    while (n > 0 && text_width(label, n) > UI_BUBBLE_MAX_W - 2 * UI_BUBBLE_PAD - THINK_DOTS_W) n--;
    return n;
}

Rectangle ui_thinking_rect(float cx, float bottom, const char *label)
{
    int w = THINK_W, n = label_fit(label);
    if (n > 0) {
        w = (int)text_width(label, n) + THINK_DOTS_W + 2 * UI_BUBBLE_PAD;
        w = (w + 2 * UI_PX - 1) / (2 * UI_PX) * (2 * UI_PX);   // even cells keep the tail centred
        if (w < THINK_W) w = THINK_W;
    }
    return (Rectangle){ (float)((int)cx - w / 2), bottom - THINK_H, (float)w, THINK_H };
}

void ui_thinking_draw(float cx, float bottom, const char *label, double time)
{
    Rectangle r = ui_thinking_rect(cx, bottom, label);
    draw_bubble_frame(r, cx, UI_INK);
    int dots = 1 + (int)(time * 3.0) % 3, n = label_fit(label);
    if (n == 0) {
        for (int i = 0; i < dots; i++)
            DrawRectangle((int)r.x + 20 + i * 20, (int)r.y + 20, 2 * UI_PX, 2 * UI_PX, UI_INK);
        return;
    }
    // "searching the web..." with smaller dots sitting on the baseline
    float ty = r.y + (THINK_H - UI_FONT_SIZE) / 2;
    draw_text_n(label, n, r.x + UI_BUBBLE_PAD, ty, UI_INK);
    int x = (int)(r.x + UI_BUBBLE_PAD + text_width(label, n)) + 4;
    for (int i = 0; i < dots; i++)
        DrawRectangle(x + i * 8, (int)ty + 14, UI_PX, UI_PX, UI_INK);
}

Rectangle ui_arrow_rect(float cx, float y)
{
    const float pad = 2 * UI_PX;   // a bit bigger than the art so its easy to hit
    return (Rectangle){ cx - ARROW_W * UI_PX / 2.0f - pad, y - pad,
                        ARROW_W * UI_PX + 2 * pad, ARROW_H * UI_PX + 2 * pad };
}

void ui_arrow_draw(float cx, float y, bool up)
{
    ui_draw_cells(up ? ARROW_UP : ARROW_DOWN, ARROW_W, ARROW_H,
                  (int)(cx - ARROW_W * UI_PX / 2.0f), (int)y, UI_PX, true);
}
