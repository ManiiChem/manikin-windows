#pragma once

#include <stdbool.h>

#include "anim_emotion.h"
#include "synth.h"

// plays his sounds (rendered once by synth.c) through raylib audio. everything is a no-op
// without an audio device so he just stays quiet. what plays when is up to cues.c

// after InitWindow. opens the audio device and renders every sound
void sfx_init(void);
void sfx_shutdown(void);

// volume 0..100, voice = his babble while a line types
void sfx_set(int volume, bool muted, bool voice);
bool sfx_muted(void);

// a fullscreen app is in front (game, video), everything plays at 10% volume max so hes
// still around but never louder than what youre watching
void sfx_hush(bool on);

// gain and pitch 1 = as designed (pitch gets a bit of random jitter so repeats dont sound
// canned), pan -1 left .. 1 right
void sfx_play(SfxId id, float gain, float pitch, float pan);

// one letter of a line typing out in `emotion`'s voice. spaces, punctuation and letters
// coming faster than he can say them get skipped
void sfx_voice(Emotion emotion, int codepoint, int next, float pan);
// no babble for this many secs (a chirp is playing)
void sfx_voice_hold(float seconds);

// how long sound `id` is in secs (0 if it has none yet)
float sfx_length(SfxId id);

void sfx_update(float dt);
