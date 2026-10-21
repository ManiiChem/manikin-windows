#pragma once

#include <stdbool.h>

// anim_sleep.c state (dozing off when ignored, waking up when poked). zero means
// nothing going on since mascot_init() memsets the whole Mascot
typedef struct {
    float prev_idle;    // m->idle_for last frame, a sudden drop means someone poked him
    float wake_left;    // > 0 while the wake up plays (counts down)
    float wake_len;     // how long the current wake up is
    bool wake_deep;     // he was fully asleep (big jolt) vs only drowsy
    float wake_dim;     // how dim the screen was when he woke
    float hover_t;      // secs the pointer has hovered over him while hes asleep
    float stir_left;    // > 0 while he stirs in his sleep (counts down)
    float stir_cool;    // > 0 = too soon to stir again
    bool was_asleep;    // sleep_is_asleep() last update, MEV_ASLEEP fires on the edge
} SleepState;
