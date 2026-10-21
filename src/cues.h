#pragma once

#include <stdbool.h>

#include "mascot.h"
#include "speech.h"
#include "synth.h"

// which moment makes which sound. the anim modules and the bubble report what happened
// (Mascot.ev, speech_events) and this turns that plus the overlays own moments into sfx_*
// calls. every sound comes from him so they all get panned to where he stands

// once a frame after mascot_update. pan is where he stands on his monitor, -1 left edge
// .. 1 right edge
void cues_frame(const Mascot *m, SpeechEvents se, float pan, bool chat_open);

// one of the overlays own moments (a prompt, an answer, a submit..)
void cues_play(SfxId id);

void cues_poke(void);                // a click on him, pokes in a row go up in pitch then he grumbles
// a sound that leads into the next line (his "hey!" before he speaks up on his own, a
// reminder's bell). it replaces that line's pop and the babble waits till its done
void cues_lead(SfxId id);
