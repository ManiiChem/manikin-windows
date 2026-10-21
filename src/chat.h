#pragma once

#include <stdbool.h>
#include "raylib.h"

// the chat box and the keyboard. chat_update() drains raylib's key and char queues every
// frame, open or not

#define CHAT_MAX 1000                // max chars you can type
#define CHAT_VISIBLE_LINES 4         // the box grows to this many lines then scrolls

typedef enum {
    CHAT_NONE,
    CHAT_SUBMIT,                     // Enter with something to send
    CHAT_ESCAPE,
    CHAT_QUIT,                       // Ctrl+Q
    CHAT_TOGGLE_MUTE,                // Ctrl+M
} ChatAction;

// can_type = the box is open and taking text. password = one masked line with no copy,
// cut or ctrl word delete
ChatAction chat_update(bool can_type, bool password);

const char *chat_text(void);
int chat_len(void);
// empties the box and wipes the buffer so a password doesnt hang around in memory
void chat_clear(void);
// a key got pressed this frame (keeps him awake)
bool chat_interacted(void);

// the box with its top left at (x, y), w wide. it grows down as the text wraps
Rectangle chat_rect(float x, float y, float w);
// mouse press in the box puts the caret there (shift extends the selection)
void chat_click(Rectangle r, Vector2 mouse);
// cursor_on = the caret can show (typing allowed, window focused). it blinks on its own
// clock that restarts on every edit or move
void chat_draw(Rectangle r, bool cursor_on, bool password, const char *placeholder);
