#pragma once

// anim_bye.c state. when the app quits he waves bye then switches off like an old CRT

#define BYE_WAVE   0.6f     // secs waving while "Bye!" types out
#define BYE_FLASH  0.07f    // secs the screen flares white when the power cuts
#define BYE_SQUASH 0.15f    // secs for the picture to collapse into a bright line
#define BYE_LINE   0.12f    // secs for the line to shrink to a dot
#define BYE_DOT    0.28f    // secs the dot glows before it goes out
#define BYE_LEN    (BYE_WAVE + BYE_FLASH + BYE_SQUASH + BYE_LINE + BYE_DOT)

typedef struct {
    float t;                // secs since m->leaving went on (0 while its off)
} ByeState;
