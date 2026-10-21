#pragma once

#include <stdbool.h>
#include "raylib.h"

#define UI_PX 4                 // screen px per art pixel for all the UI
#define UI_FONT_SIZE 20         // raylib's 10px default font at 2x stays crisp
#define UI_FONT_SPACING 2
#define UI_LINE_H 22

#define UI_TEXT_MAX 2048
#define UI_BUBBLE_MAX_W 380
#define UI_BUBBLE_PAD 16        // border + inner margin
#define UI_BUBBLE_MAX_LINES 96
#define UI_PAGE_LINES 8         // longer text shows a page at a time

extern const Color UI_INK;
extern const Color UI_PAPER;
extern const Color UI_HALO;
extern const Color UI_HINT;

// draws a pixel sprite, '#' is ink, 'o' paper, anything else empty. `cells` is w*h chars
// row by row. `halo` adds a 1 cell white outline so it still reads on dark wallpapers
void ui_draw_cells(const char *cells, int w, int h, int x, int y, int px, bool halo);

// same but each char picks its colour from `palette` (indexed by the char itself, 128
// entries), alpha 0 = empty
void ui_draw_cells_pal(const char *cells, int w, int h, int x, int y, int px,
                       const Color palette[128], bool halo);

// white box with a pixel rounded ink border and a halo
void ui_box_draw(Rectangle r);

typedef struct { int start, len; } UiLine;

typedef enum {
    UI_BUTTONS_NONE,
    UI_BUTTONS_YES_NO,          // [check] [cross]
    UI_BUTTONS_SESSION,         // [check] [double check = yes for the rest of this session] [cross]
} UiButtons;

typedef struct {
    char text[UI_TEXT_MAX];
    UiLine lines[UI_BUBBLE_MAX_LINES];
    int line_count;
    int page, page_count;
    UiButtons buttons;
    float w;                    // same on every page so the bubble doesnt jump around
} UiBubble;

typedef enum { UI_HOVER_NONE, UI_HOVER_YES, UI_HOVER_NO, UI_HOVER_SESSION } UiHover;

// word wraps `text` once up front so the bubble doesnt resize while it types out
void ui_bubble_set(UiBubble *b, const char *text, UiButtons buttons);

// chars on the current page, what the typewriter counts up to
int ui_bubble_page_chars(const UiBubble *b);

// false if this was the last page
bool ui_bubble_next_page(UiBubble *b);

// index into b->text of the current page's i-th char, -1 past the end
int ui_bubble_text_index(const UiBubble *b, int i);

// how the text gets drawn, all zero = plain ink
typedef struct {
    int effect;                 // an Emotion, the mood wobble on each glyph (0 = none)
    double time;                // GetTime(), drives the effect
    const double *born;         // per page char, the `time` it showed up (NULL = long ago)
    int hist_pos, hist_count;   // hist_count > 0 means an old reply, dimmed and marked "<pos/count"
} UiBubbleLook;

// bubbles are anchored at the bottom centre of the box, the tail hangs below
Rectangle ui_bubble_rect(const UiBubble *b, float cx, float bottom);
void ui_bubble_draw(const UiBubble *b, float cx, float bottom, int shown_chars, UiHover hover,
                    const UiBubbleLook *look);
// the answer buttons. `session` can be NULL and stays zero size unless the bubble offers it
void ui_bubble_buttons(const UiBubble *b, float cx, float bottom, Rectangle *yes, Rectangle *session,
                       Rectangle *no);

// empty frame at `scale` of the final box `r` growing out of the tail, for the pop in and
// shrink away. sizes snap to whole art pixels so it stays crisp
void ui_bubble_pop_draw(Rectangle r, float cx, float scale, bool old);

// `label` ("searching the web") goes before the animated dots, NULL or "" for just dots
Rectangle ui_thinking_rect(float cx, float bottom, const char *label);
void ui_thinking_draw(float cx, float bottom, const char *label, double time);

Rectangle ui_arrow_rect(float cx, float y);
void ui_arrow_draw(float cx, float y, bool up);
