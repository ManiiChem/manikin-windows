#pragma once

#include "raylib.h"

// his app icon at size x size, drawn from his sprite in whole art pixels using as much of
// him as fills it (all of him, his head, or just his screen face). RGBA, free it with
// UnloadImage. needs the window, the first call draws him once off screen (his own Mascot
// isnt touched)
Image icon_image(int size);

// the sizes tools/make_icon.c puts in the exe's .ico, the usual windows ones at 100 to
// 200% scaling so the shell hardly ever has to scale his pixels
#define ICON_COUNT 15
extern const int ICON_SIZE[ICON_COUNT];
