#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "speech.h"

#define QUEUE_MAX 8
#define STATUS_MAX 64
#define HIST_MAX 10                  // old lines the wheel can scroll back through

#define POP_T 0.15f                  // pop in, grows out of the tail with a bit of overshoot
#define SHRINK_T 0.1f                // shrink back into the tail
#define COMMA_PAUSE 0.12f            // typing pauses a bit after , ; :
#define STOP_PAUSE 0.3f              // and after . ! ?

typedef enum { BUBBLE_HIDDEN, BUBBLE_THINKING, BUBBLE_TALKING, BUBBLE_SHOWN } BubbleState;

static float anchor_x, anchor_bottom;
static UiBubble bubble;
static BubbleState state = BUBBLE_HIDDEN;
static bool sticky;                  // prompts stay till answered
static Emotion emotion;              // emotion the current line was tagged with
static Emotion face;                 // what the face acts out, `emotion` or an old line's
static bool face_new;                // speech_emotion_changed() hasnt reported it yet
static bool face_replay;             // and its an old line replaying
static float timer, shown, pause;
static double born[UI_TEXT_MAX];     // GetTime() when each char on the page showed up
static float pop = POP_T;            // secs since the bubble popped in
static float shrink;                 // secs left of the shrink away
static Rectangle shrink_r;
static bool shrink_old;
static bool hovered;
static float wheel_acc;
static char status[STATUS_MAX];
static SpeechEvents ev;              // what happened since speech_events() last asked

// lines waiting for the bubble, oldest first
typedef struct {
    char text[UI_TEXT_MAX];
    Emotion emotion;
    bool narration;
} Queued;
static Queued queue[QUEUE_MAX];
static int queue_head, queue_count;
static bool narrating;               // the line up now is narration so the next one swaps in

#define NARRATE_HOLD 0.35f           // secs a typed out narration line stays once a newer one is waiting

// last lines said, oldest first (the newest is usually the one on screen). browsing shows
// an old one in its own bubble and freezes the live one meanwhile
static Queued hist[HIST_MAX];
static int hist_head, hist_count;
static bool browsing;
static int hist_pos;
static UiBubble hist_bubble;
static float hist_timer;
static int browse_queued;            // queue length when browsing started

void speech_init(float cx, float bottom)
{
    anchor_x = cx;
    anchor_bottom = bottom;
}

void speech_say(const char *text, Emotion e, bool front)
{
    if (queue_count == QUEUE_MAX) {
        queue_head = (queue_head + 1) % QUEUE_MAX;
        queue_count--;
    }
    int slot = front ? (queue_head = (queue_head + QUEUE_MAX - 1) % QUEUE_MAX)
                     : (queue_head + queue_count) % QUEUE_MAX;
    snprintf(queue[slot].text, UI_TEXT_MAX, "%s", text);
    queue[slot].emotion = e;
    queue[slot].narration = false;
    queue_count++;
}

void speech_narrate(const char *text, Emotion e)
{
    // narration still waiting is already stale, his hands moved on
    int kept = 0;
    for (int i = 0; i < queue_count; i++) {
        Queued *q = &queue[(queue_head + i) % QUEUE_MAX];
        if (!q->narration) queue[(queue_head + kept++) % QUEUE_MAX] = *q;
    }
    queue_count = kept;
    speech_say(text, e, false);
    queue[(queue_head + queue_count - 1) % QUEUE_MAX].narration = true;
}

static Queued *hist_at(int i)
{
    return &hist[(hist_head + i) % HIST_MAX];
}

static void remember(const char *text, Emotion e)
{
    // an interrupted line coming back around is already the newest
    if (hist_count > 0 && strcmp(hist_at(hist_count - 1)->text, text) == 0) return;
    if (hist_count == HIST_MAX) {
        hist_head = (hist_head + 1) % HIST_MAX;
        hist_count--;
    }
    Queued *q = hist_at(hist_count++);
    snprintf(q->text, UI_TEXT_MAX, "%s", text);
    q->emotion = e;
}

static void set_face(Emotion e, bool replay)
{
    face = e;
    face_new = true;
    face_replay = replay;
}

// the face acts out a line from when its text starts showing
static void set_emotion(Emotion e)
{
    emotion = e;
    set_face(e, false);
}

static float hold_time(const UiBubble *b)
{
    int chars = ui_bubble_page_chars(b);
    return b->page + 1 >= b->page_count ? 5.0f + 0.04f * chars : 1.5f + 0.035f * chars;
}

// chars per sec, his mood changes how fast he talks
static float type_speed(void)
{
    switch (emotion) {
    case EMOTION_SAD: case EMOTION_WORRIED: return 35.0f;
    case EMOTION_EXCITED: case EMOTION_LAUGHING: return 70.0f;
    default: return 55.0f;
    }
}

static void show(const char *text, UiButtons buttons, bool is_sticky)
{
    ui_bubble_set(&bubble, text, buttons);
    state = BUBBLE_TALKING;
    sticky = is_sticky;
    narrating = false;
    shown = pause = 0;
    pop = shrink = 0;
    browsing = false;                        // a new line jumps back to the present
    if (is_sticky) set_emotion(EMOTION_NONE);    // prompts use their own moods
    if (is_sticky) ev.prompt = true;
    else ev.popped = true;
}

// swap = replace a narration line in place, no shrink and pop in between
static void show_next_queued(bool swap)
{
    const Queued *q = &queue[queue_head];
    float popped = pop;
    show(q->text, UI_BUTTONS_NONE, false);
    if (swap) {                              // same bubble new words, so no pop and no pop sound
        pop = popped;
        ev.popped = false;
    }
    narrating = q->narration;
    set_emotion(q->emotion);
    if (!q->narration) remember(q->text, q->emotion);    // narration stays out of the history
    queue_head = (queue_head + 1) % QUEUE_MAX;
    queue_count--;
}

void speech_prompt(const char *text, UiButtons buttons)
{
    show(text, buttons, true);
}

void speech_hide(void)
{
    Rectangle r = speech_rect();
    if (r.width > 0) {
        shrink = SHRINK_T;
        shrink_r = r;
        shrink_old = browsing;
        ev.closed = true;
    }
    state = BUBBLE_HIDDEN;
    sticky = false;
    browsing = false;
}

static void enter_history(int pos)
{
    if (!browsing) browse_queued = queue_count;
    browsing = true;
    hist_pos = pos;
    ui_bubble_set(&hist_bubble, hist_at(pos)->text, UI_BUTTONS_NONE);
    hist_timer = hold_time(&hist_bubble);
    set_face(hist_at(pos)->emotion, true);
    ev.history = true;                       // a wheel step back, or a recall
}

// back to the live line (or away if nothing was up when browsing started)
static void leave_history(void)
{
    if (!browsing) return;
    if (state == BUBBLE_HIDDEN) {
        speech_hide();
        return;
    }
    browsing = false;
    set_face(emotion, true);
}

static bool live_done(void)
{
    return state == BUBBLE_HIDDEN || (state == BUBBLE_SHOWN && bubble.page + 1 >= bubble.page_count);
}

// an old line had its time, go away unless the live one still has more to say
static void close_history(void)
{
    if (live_done()) speech_hide();
    else leave_history();
}

// dir < 0 goes further back, dir > 0 toward the present
static void history_step(int dir)
{
    if (dir < 0) {
        if (browsing) {
            if (hist_pos > 0) enter_history(hist_pos - 1);
        } else if (state != BUBBLE_HIDDEN && hist_count >= 2) {
            enter_history(hist_count - 2);   // the newest is the live line itself
        }
    } else if (browsing) {
        int newest_old = state == BUBBLE_HIDDEN ? hist_count - 1 : hist_count - 2;
        if (hist_pos < newest_old) enter_history(hist_pos + 1);
        else {
            leave_history();
            ev.history = true;               // the wheel stepped back onto the live line
        }
    }
}

void speech_recall(float wheel)
{
    if (wheel <= 0 || state != BUBBLE_HIDDEN || browsing || hist_count == 0) return;
    enter_history(hist_count - 1);
    pop = shrink = 0;
}

void speech_interrupt(void)
{
    leave_history();
    // an interrupted reply comes back after, interrupted narration would be stale by then
    if (!sticky && !narrating && (state == BUBBLE_TALKING ||
                                  (state == BUBBLE_SHOWN && bubble.page + 1 < bubble.page_count)))
        speech_say(bubble.text, emotion, true);
}

void speech_dismiss(void)
{
    leave_history();
    if (!sticky && state == BUBBLE_SHOWN) speech_hide();
}

void speech_click(void)
{
    if (browsing) {
        if (ui_bubble_next_page(&hist_bubble)) hist_timer = hold_time(&hist_bubble);
        else close_history();
        return;
    }
    if (sticky || state == BUBBLE_HIDDEN || state == BUBBLE_THINKING) return;
    if (state == BUBBLE_TALKING) {
        int chars = ui_bubble_page_chars(&bubble);
        for (int i = (int)shown; i < chars; i++) born[i] = -1e6;
        shown = (float)chars;
        pause = 0;
    } else if (ui_bubble_next_page(&bubble)) {
        state = BUBBLE_TALKING;
        shown = pause = 0;
        ev.page = true;
    } else {
        speech_hide();
    }
}

// how long to pause after page char `i`, only for punctuation followed by a space or line
// end (so not "3.14", the middle of "..." or the last char)
static float pause_after(int i, int chars)
{
    if (i + 1 >= chars) return 0;
    int at = ui_bubble_text_index(&bubble, i);
    if (at < 0) return 0;
    char c = bubble.text[at], next = bubble.text[at + 1];
    if (next != ' ' && next != '\n' && next != 0) return 0;
    if (c == ',' || c == ';' || c == ':') return COMMA_PAUSE;
    if (c == '.' || c == '!' || c == '?') return STOP_PAUSE;
    return 0;
}

static void type_out(float dt)
{
    if (pause > 0) {
        if ((pause -= dt) > 0) return;
        dt = -pause;                         // spend the rest of the frame typing
        pause = 0;
    }
    int chars = ui_bubble_page_chars(&bubble);
    int before = (int)shown;
    shown += type_speed() * dt;
    if (shown > chars) shown = (float)chars;
    for (int i = before; i < (int)shown; i++) {
        born[i] = GetTime();
        // for the typing sounds (click to finish skips this). the newest letter beats a silent
        // space or comma typed after it in the same frame so it doesnt get lost
        int at = ui_bubble_text_index(&bubble, i);
        int c = at >= 0 ? (unsigned char)bubble.text[at] : 0;
        ev.typed++;
        if (isalpha(c) || !isalpha(ev.last)) {
            ev.last = c;
            ev.next = at >= 0 && i + 1 < chars ? (unsigned char)bubble.text[at + 1] : 0;
        }
        ev.emotion = emotion;
        float p = pause_after(i, chars);
        if (p > 0) {                         // stop right after it so nothing shows early
            shown = (float)(i + 1);
            pause = p;
            break;
        }
    }
}

void speech_update(float dt, bool waiting, Vector2 mouse, float wheel)
{
    if (pop < POP_T) pop += dt;
    if (shrink > 0) shrink -= dt;

    // a new line while browsing jumps back to the present
    if (browsing && queue_count > browse_queued) {
        bool done = live_done();
        browsing = false;
        if (done) state = BUBBLE_HIDDEN;     // the new line pops right in below
        else set_face(emotion, true);
    }

    if (!browsing && (state == BUBBLE_HIDDEN || state == BUBBLE_THINKING)) {
        if (queue_count > 0) show_next_queued(false);
        else if (waiting && state == BUBBLE_HIDDEN && shrink <= 0) {
            state = BUBBLE_THINKING;         // after the last bubble has shrunk away
            pop = 0;
            ev.thinking = true;
        } else if (!waiting && state == BUBBLE_THINKING) {
            speech_hide();
        }
    }

    bool bubble_up = browsing || state == BUBBLE_TALKING || state == BUBBLE_SHOWN;
    hovered = bubble_up && CheckCollisionPointRec(mouse, speech_rect());

    // wheel over the bubble scrolls through old lines, up goes back in time
    if (hovered && !sticky) {
        wheel_acc += wheel;
        while (wheel_acc >= 1) { wheel_acc -= 1; history_step(-1); }
        while (wheel_acc <= -1) { wheel_acc += 1; history_step(1); }
    } else {
        wheel_acc = 0;
    }

    if (browsing) {                          // the live line waits frozen
        if (!hovered && (hist_timer -= dt) <= 0) {
            if (ui_bubble_next_page(&hist_bubble)) hist_timer = hold_time(&hist_bubble);
            else close_history();
        }
        return;
    }

    switch (state) {
    case BUBBLE_TALKING:
        if (pop < POP_T) break;
        type_out(dt);
        if (shown >= ui_bubble_page_chars(&bubble)) {
            state = BUBBLE_SHOWN;
            timer = hold_time(&bubble);
        }
        break;
    case BUBBLE_SHOWN:
        if (sticky || hovered) break;
        if (queue_count > 0 && bubble.page + 1 >= bubble.page_count)
            timer = fminf(timer, narrating ? NARRATE_HOLD : 1.5f);
        if ((timer -= dt) <= 0) {
            if (narrating && queue_count > 0 && bubble.page + 1 >= bubble.page_count) {
                show_next_queued(true);
            } else if (ui_bubble_next_page(&bubble)) {
                state = BUBBLE_TALKING;
                shown = pause = 0;
                ev.page = true;
            } else {
                speech_hide();
            }
        }
        break;
    default:
        break;
    }
}

// ease out with a small overshoot (about 7%), in steps of 1/20 so it looks like frames
static float pop_scale(float t)
{
    const float c1 = 1.4f, c3 = c1 + 1.0f;
    float u = t - 1.0f;
    float s = 1.0f + c3 * u * u * u + c1 * u * u;
    return floorf(s * 20.0f + 0.5f) / 20.0f;
}

void speech_draw(UiHover hover)
{
    if (state == BUBBLE_HIDDEN && !browsing) {
        float s = shrink / SHRINK_T;
        if (shrink > 0 && s > 0.1f) ui_bubble_pop_draw(shrink_r, anchor_x, floorf(s * 20.0f) / 20.0f, shrink_old);
        return;
    }
    if (pop < POP_T) {
        ui_bubble_pop_draw(speech_rect(), anchor_x, pop_scale(pop / POP_T), browsing);
        return;
    }
    if (browsing) {
        UiBubbleLook look = { hist_at(hist_pos)->emotion, GetTime(), NULL, hist_pos + 1, hist_count };
        ui_bubble_draw(&hist_bubble, anchor_x, anchor_bottom, ui_bubble_page_chars(&hist_bubble),
                       UI_HOVER_NONE, &look);
    } else if (state == BUBBLE_THINKING) {
        ui_thinking_draw(anchor_x, anchor_bottom, status, GetTime());
    } else {
        UiBubbleLook look = { sticky ? EMOTION_NONE : emotion, GetTime(), born, 0, 0 };
        ui_bubble_draw(&bubble, anchor_x, anchor_bottom,
                       state == BUBBLE_TALKING ? (int)shown : ui_bubble_page_chars(&bubble), hover, &look);
    }
}

void speech_set_status(const char *label)
{
    snprintf(status, sizeof status, "%s", label ? label : "");
}

Rectangle speech_rect(void)
{
    if (browsing) return ui_bubble_rect(&hist_bubble, anchor_x, anchor_bottom);
    if (state == BUBBLE_THINKING) return ui_thinking_rect(anchor_x, anchor_bottom, status);
    if (state != BUBBLE_HIDDEN) return ui_bubble_rect(&bubble, anchor_x, anchor_bottom);
    return (Rectangle){ 0 };
}

bool speech_buttons(Rectangle *yes, Rectangle *session, Rectangle *no)
{
    if (browsing || state != BUBBLE_SHOWN || !bubble.buttons) return false;
    ui_bubble_buttons(&bubble, anchor_x, anchor_bottom, yes, session, no);
    return true;
}

bool speech_busy(void)     { return browsing || state != BUBBLE_HIDDEN; }
bool speech_talking(void)  { return !browsing && state == BUBBLE_TALKING; }
bool speech_thinking(void) { return !browsing && state == BUBBLE_THINKING; }
bool speech_sticky(void)   { return sticky; }

bool speech_emotion_changed(Emotion *out)
{
    if (!face_new) return false;
    face_new = false;
    *out = face;
    return true;
}

bool speech_face_replayed(void) { return face_replay; }

SpeechEvents speech_events(void)
{
    SpeechEvents out = ev;
    ev = (SpeechEvents){ 0 };
    return out;
}
