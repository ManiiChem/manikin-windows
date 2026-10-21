#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "anim_emotion.h"

// his sounds, each one made from a few numbers (sfxr style chiptune) so theres no audio files,
// same as his sprites. plain C with no raylib so it can be checked without an audio device

#define SYNTH_RATE 44100             // samples per sec, everything is mono 16 bit
#define SYNTH_VOICE_MAX_MS 90        // a voice syllable at base pitch stays under this

// each category gets normalised to its own loudness and has its own gain in sfx.c
typedef enum {
    SFX_CAT_UI,                      // you did something (clicks, prompts, jingles)
    SFX_CAT_VOICE,                   // his babble while the bubble types
    SFX_CAT_BODY,                    // his body (hops, landings, the plug, emotion beats)
    SFX_CAT_AMBIENT,                 // background stuff like footsteps, whistling, sleep noises, the ring
    SFX_CAT_COUNT
} SfxCategory;

// every sound he can make, each one needs a recipe in synth.c (soundboard --check fails otherwise)
typedef enum {
    // system
    SFX_BOOT,
    SFX_QUIT,
    SFX_BRAIN_DIED,

    // you and him
    SFX_POKE,                        // you clicked him (pitch goes up for a combo)
    SFX_POKE_GRUMBLE,                // poked too many times in a row
    SFX_PICKUP,                      // the drag starts
    SFX_SHAKE,                       // each time a shake changes direction
    SFX_DIZZY,
    SFX_LAND,                        // dropped (louder and lower the harder he lands)
    SFX_CHAT_OPEN,
    SFX_CHAT_CLOSE,
    SFX_SUBMIT,                      // your message is sent
    SFX_ASK,                         // a yes/no permission prompt
    SFX_YES,
    SFX_NO,
    SFX_LOCK,                        // the password prompt (his memory is locked)
    SFX_UNLOCK,                      // memory unlocked
    SFX_WRONG,                       // wrong password
    SFX_CANCEL,                      // esc while hes thinking
    SFX_SOUND_ON,                    // unmuted with ctrl+m

    // the bubble
    SFX_BUBBLE,                      // a new line pops up
    SFX_PAGE,                        // next page
    SFX_HISTORY,                     // tick per step back through old lines
    SFX_HEY,                         // before he speaks up on his own
    SFX_REMIND,                      // a reminder went off (a bell so it stands out)
    SFX_ERROR,
    SFX_THINK,                       // the thinking bubble shows up

    // emotion beats, timed to his entrance animations
    SFX_HOP,                         // a happy hop lands
    SFX_SPARKLE,                     // excited
    SFX_LAUGH,
    SFX_HEART,                       // love, a heart floats up
    SFX_STING,                       // surprised
    SFX_HUH,                         // confused
    SFX_TING,                        // smug
    SFX_SIGH,                        // sad
    SFX_QUIVER,                      // worried
    SFX_STOMP,                       // annoyed, the foot stomp
    SFX_HMPH,                        // annoyed, the grumble

    // sleep
    SFX_YAWN,
    SFX_NOD,                         // his head snaps up as he nods off
    SFX_SETTLE,                      // hes asleep now
    SFX_SNORE_POP,                   // the snot bubble pops
    SFX_WAKE_JOLT,                   // woken from deep sleep
    SFX_WAKE_SOFT,                   // woken while just drowsy

    // pointing at something on screen
    SFX_STEP,                        // a foot comes down (panned as he walks)
    SFX_ZIP_OUT,                     // the cable arm unplugs and stretches out
    SFX_ZIP_IN,                      // ...and snaps back
    SFX_PLUG,                        // the plug locks onto the target
    SFX_RING,                        // shimmer as the ring appears round it
    SFX_SUCCESS,                     // you clicked the right thing
    SFX_UNPLUG,                      // you clicked somewhere else, it moved, or never mind
    SFX_WHISTLE,                     // one note at a time as he strolls home happy (tune in synth_whistle_pitch)

    // driving the real pointer (he takes the wheel)
    SFX_GRAB,                        // his cable clunks onto your pointer
    SFX_CLACK,                       // he clicks
    SFX_KEY,                         // one key on his keyboard (fast, pitch changes per key)
    SFX_HAUL,                        // he strains to haul the pointer a long way
    SFX_LETGO,
    SFX_VICTORY,                     // the drive went well, victory lap
    SFX_OOPS,                        // a miss, or "oh its yours!" when you grab the mouse

    SFX_COUNT
} SfxId;

typedef struct {
    const char *name;                // like "poke", soundboard label and export file name
    SfxCategory category;
    int max_ms;                     // longest it can be (soundboard --check checks)
} SfxInfo;

const SfxInfo *synth_info(SfxId id);

// loudness every sound in cat gets normalised to (RMS dBFS, a full scale sine is -3)
float synth_target_db(SfxCategory cat);

// RMS of a rendered buffer in dBFS, measured the same way the normaliser does it
float synth_rms_db(const int16_t *samples, int frames);

// emotion name like "happy" for the soundboards file names. same as EMOTION_NAMES in
// mascot.c except EMOTION_NONE is "none" here (mascot.c uses "")
const char *synth_emotion_name(Emotion emotion);

// renders sound id into a malloced buffer (free it after) of mono 16 bit samples at
// SYNTH_RATE, already normalised to its categorys loudness. *frames gets the length,
// NULL and *frames = 0 if theres no recipe or malloc fails
int16_t *synth_render(SfxId id, int *frames);

// one syllable of his voice for that emotion at its base pitch. sfx.c plays it once per
// letter pitched by synth_voice_pitch. same deal as synth_render otherwise
int16_t *synth_render_voice(Emotion emotion, int *frames);

// pitch multiplier for letter codepoint of a line in that emotion (1 = base). the same letter
// always gets the same pitch so a word sounds the same every time. next is the codepoint
// after it (0 at the end) and a '?' there lifts it. 0 means silent (spaces, punctuation, digits...)
float synth_voice_pitch(Emotion emotion, int codepoint, int next);

// pitch multiplier for note n (0, 1, 2...) of the tune he whistles walking home after you
// clicked what he pointed at, it loops
float synth_whistle_pitch(int n);
