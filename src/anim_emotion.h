#pragma once

// the emotions Claude tags each reply with ("[happy] Hi!"). keep the list and the order
// in sync with MOODS in brain/manikin/protocol.py and EMOTION_NAMES in mascot.c
typedef enum {
    EMOTION_NONE,
    EMOTION_HAPPY,
    EMOTION_EXCITED,
    EMOTION_LAUGHING,
    EMOTION_LOVE,
    EMOTION_SURPRISED,
    EMOTION_CONFUSED,
    EMOTION_SMUG,
    EMOTION_SAD,
    EMOTION_WORRIED,
    EMOTION_ANNOYED,
    EMOTION_COUNT
} Emotion;

// anim_emotion.c state
typedef struct {
    float quiet;        // secs spent IDLE since the reply stopped typing (the linger clock)
    float last_age;     // emotion_age last update, if it drops a new reply started
    int last_emotion;   // emotion last update
} EmotionState;
