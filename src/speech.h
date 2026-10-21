#pragma once

#include <stdbool.h>
#include "raylib.h"
#include "anim_emotion.h"
#include "ui.h"

// the speech bubble over his head, so the queue of lines to say, the typewriter, paging
// and the thinking bubble. prompts (password, permission) are sticky till speech_hide()

// anchored at the bottom centre of the box, the tail hangs below that
void speech_init(float cx, float bottom);

// queue a line. `front` puts it first in line (an interrupted line going back)
void speech_say(const char *text, Emotion emotion, bool front);
// a passing line while he drives. it swaps in as soon as the narration on screen is done
// typing instead of waiting its turn, skips the history, and drops any older one waiting
void speech_narrate(const char *text, Emotion emotion);
// show a sticky prompt right away, with answer buttons under it (or none)
void speech_prompt(const char *text, UiButtons buttons);
void speech_hide(void);
// a prompt is about to take the bubble, put an unfinished line back in the queue
void speech_interrupt(void);
// the user started a new exchange, hide a line thats done typing
void speech_dismiss(void);
// a click on a non sticky bubble finishes the typing, then turns the page, then dismisses it
void speech_click(void);

// waiting = the brain is working (thinking bubble if nothing is queued). mouse is in
// window coords, wheel is GetMouseWheelMove()
void speech_update(float dt, bool waiting, Vector2 mouse, float wheel);
void speech_draw(UiHover hover);

// wheel over him while the bubble is hidden, wheel up reopens the newest old line
// (over the bubble itself speech_update handles the wheel)
void speech_recall(float wheel);

// label for the thinking bubble ("searching the web"), NULL for just the dots
void speech_set_status(const char *label);

Rectangle speech_rect(void);                    // zero size while hidden
// false unless answer buttons are up. `session` stays zero size unless thats offered
bool speech_buttons(Rectangle *yes, Rectangle *session, Rectangle *no);
bool speech_busy(void);                         // anything on screen, thinking too
bool speech_talking(void);                      // typing a line out
bool speech_thinking(void);
bool speech_sticky(void);

// true once each time a new line starts showing, `out` is the emotion the face should
// act out from now (EMOTION_NONE for prompts)
bool speech_emotion_changed(Emotion *out);

// the face is replaying a line it already acted (an old one from the wheel history, or
// the live one again on the way back). stays true till a new line
bool speech_face_replayed(void);

// what the bubble did since the last call, for the sounds. call once a frame after
// speech_update, it resets everything
typedef struct {
    int typed;              // glyphs of a live line that showed up (never history)
    int last;               // codepoint of the newest one (0 if typed == 0)
    int next;               // codepoint right after it on the page (0 at the end)
    Emotion emotion;
    bool popped;            // a new line's bubble popped open
    bool prompt;            // a sticky prompt showed up (password, yes/no, point text)
    bool thinking;          // the thinking bubble showed up
    bool page;              // turned to the next page of the live line
    bool closed;            // the bubble went away (shrank or got hidden)
    bool history;           // stepped to another old line, or reopened one
} SpeechEvents;

SpeechEvents speech_events(void);
