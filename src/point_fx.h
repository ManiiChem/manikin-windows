#pragma once

#include "mascot.h"
#include "pointer.h"

// what you see outside his canvas while he points or drives. visual only, none of it
// catches the mouse (the window stays click-through there so the user can click the very
// thing hes pointing at)
// mascot_x, mascot_y = where mascot_draw put the canvas (window px), px = MASCOT_PX.
// both do nothing while v->phase is POINTER_HOME and theres nothing left to fade out

void point_fx_draw_behind(const PointerView *v, const Mascot *m, int mascot_x, int mascot_y, int px, float time);
void point_fx_draw(const PointerView *v, const Mascot *m, int mascot_x, int mascot_y, int px, float time);
