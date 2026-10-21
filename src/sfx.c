#include <stdlib.h>

#include "raylib.h"
#include "sfx.h"

#define COPIES 4                     // per sound, so a quick repeat overlaps instead of restarting
#define VOICE_COPIES 6               // syllables overlap a lot more
#define MAX_COPIES 8
#define MAX_PLAYING 6                // past this many at once its just mush
#define VOICE_GAP 0.065f             // ~15 syllables a sec max however fast he types
#define DUCK_TIME 0.35f              // background sounds stay ducked this long after a syllable
#define DUCK_GAIN 0.4f
#define HUSH_VOLUME 0.1f             // volume cap while a fullscreen app is in front

typedef struct {
    Sound copy[MAX_COPIES];          // [0] owns the samples, the rest are aliases of it
    int count, next;
    float length;                    // secs
    double last;                     // when it last played, for its cooldown
} Slot;

static Slot sounds[SFX_COUNT];
static Slot voices[EMOTION_COUNT];
static bool ready;

static float volume = 0.3f;
static bool muted, voice_on = true, hushed;
static float duck_left, hold_left;
static double voice_last;
static bool logging;                 // MANIKIN_SFX_LOG=1 prints every sound (see it with make console)

// on top of synth.c's per category loudness, he talks a bit under the UI and his footsteps
// and whistling stay in the background
static const float CATEGORY_GAIN[SFX_CAT_COUNT] = {
    [SFX_CAT_UI] = 0.9f, [SFX_CAT_VOICE] = 0.6f, [SFX_CAT_BODY] = 0.8f, [SFX_CAT_AMBIENT] = 0.6f,
};

// min secs between two plays of the same sound so a shake or a fast walk cant machine gun it
static float cooldown(SfxId id)
{
    switch (id) {
    case SFX_STEP:      return 0.07f;
    case SFX_KEY:       return 0.03f;    // typing, at most a key each frame if it gets that fast
    case SFX_CLACK:     return 0.03f;    // a triple click is three clacks
    case SFX_SHAKE:     return 0.12f;
    case SFX_LAUGH:     return 0.15f;
    case SFX_HEART:     return 0.3f;
    case SFX_NOD:       return 3.0f;
    case SFX_SNORE_POP: return 180.0f;   // hes asleep, keep it rare
    default:            return 0.05f;
    }
}

// how many overlapping copies a sound gets, his keys clatter 15+ a sec and a double or
// triple click comes quick so those get more
static int copies(SfxId id)
{
    switch (id) {
    case SFX_KEY:   return 8;
    case SFX_CLACK: return 6;
    default:        return COPIES;
    }
}

static void load(Slot *s, int16_t *samples, int frames, int n)
{
    if (!samples) return;                    // no recipe yet, stays silent
    Wave w = { (unsigned)frames, SYNTH_RATE, 16, 1, samples };
    s->copy[0] = LoadSoundFromWave(w);
    free(samples);
    if (!IsSoundValid(s->copy[0])) return;
    s->count = 1;
    while (s->count < n) s->copy[s->count++] = LoadSoundAlias(s->copy[0]);
    s->length = (float)frames / SYNTH_RATE;
}

static void unload(Slot *s)
{
    for (int i = s->count - 1; i >= 1; i--) UnloadSoundAlias(s->copy[i]);
    if (s->count) UnloadSound(s->copy[0]);
    s->count = 0;
}

void sfx_init(void)
{
    logging = getenv("MANIKIN_SFX_LOG") != NULL;
    InitAudioDevice();
    ready = IsAudioDeviceReady();
    if (!ready) return;
    for (int id = 0; id < SFX_COUNT; id++) {
        int frames = 0;
        int16_t *samples = synth_render((SfxId)id, &frames);
        load(&sounds[id], samples, frames, copies((SfxId)id));
    }
    for (int e = 0; e < EMOTION_COUNT; e++) {
        int frames = 0;
        int16_t *samples = synth_render_voice((Emotion)e, &frames);
        load(&voices[e], samples, frames, VOICE_COPIES);
    }
}

void sfx_shutdown(void)
{
    if (!ready) return;
    for (int id = 0; id < SFX_COUNT; id++) unload(&sounds[id]);
    for (int e = 0; e < EMOTION_COUNT; e++) unload(&voices[e]);
    CloseAudioDevice();
    ready = false;
}

static void stop_all(void)
{
    for (int id = 0; id < SFX_COUNT; id++)
        for (int i = 0; i < sounds[id].count; i++) StopSound(sounds[id].copy[i]);
    for (int e = 0; e < EMOTION_COUNT; e++)
        for (int i = 0; i < voices[e].count; i++) StopSound(voices[e].copy[i]);
}

void sfx_set(int vol, bool mute, bool voice)
{
    volume = (vol < 0 ? 0 : vol > 100 ? 100 : vol) / 100.0f;
    if (mute && !muted && ready) stop_all();     // muting cuts off whatever is playing right away
    muted = mute;
    voice_on = voice;
}

bool sfx_muted(void) { return muted; }
void sfx_hush(bool on) { hushed = on; }
void sfx_voice_hold(float seconds) { if (seconds > hold_left) hold_left = seconds; }

float sfx_length(SfxId id)
{
    return id >= 0 && id < SFX_COUNT ? sounds[id].length : 0.0f;
}

static int playing_count(void)
{
    int n = 0;
    for (int id = 0; id < SFX_COUNT; id++)
        for (int i = 0; i < sounds[id].count; i++) n += IsSoundPlaying(sounds[id].copy[i]);
    for (int e = 0; e < EMOTION_COUNT; e++)
        for (int i = 0; i < voices[e].count; i++) n += IsSoundPlaying(voices[e].copy[i]);
    return n;
}

static float jitter(int per_mille)
{
    return 1.0f + GetRandomValue(-per_mille, per_mille) / 1000.0f;
}

static void play_slot(Slot *s, float vol, float pitch, float pan)
{
    Sound snd = s->copy[s->next];
    s->next = (s->next + 1) % s->count;
    SetSoundVolume(snd, vol);
    SetSoundPitch(snd, pitch);
    SetSoundPan(snd, pan < -1 ? -1 : pan > 1 ? 1 : pan);
    PlaySound(snd);                          // restarts that copy if its still going
}

// the volume everything plays at, the user's but turned down while hushed
static float master(void)
{
    return hushed && volume > HUSH_VOLUME ? HUSH_VOLUME : volume;
}

void sfx_play(SfxId id, float gain, float pitch, float pan)
{
    if (!ready || muted || id < 0 || id >= SFX_COUNT || sounds[id].count == 0) return;
    SfxCategory cat = synth_info(id)->category;
    Slot *s = &sounds[id];
    double now = GetTime();
    if (now - s->last < cooldown(id)) return;
    if (cat == SFX_CAT_AMBIENT && playing_count() >= MAX_PLAYING) return;   // only background sounds give way
    s->last = now;
    float vol = master() * CATEGORY_GAIN[cat] * gain;
    if (cat == SFX_CAT_AMBIENT && duck_left > 0) vol *= DUCK_GAIN;
    int wobble = id == SFX_WHISTLE ? 0 : 40;   // no jitter on the whistle, its a tune
    play_slot(s, vol > 1 ? 1 : vol, pitch * jitter(wobble), pan);
    if (logging) TraceLog(LOG_INFO, "SFX: %s (vol %.2f, pitch %.2f, pan %+.2f)", synth_info(id)->name, vol, pitch, pan);
}

void sfx_voice(Emotion emotion, int codepoint, int next, float pan)
{
    if (!ready || muted || !voice_on || hold_left > 0) return;
    if (emotion < 0 || emotion >= EMOTION_COUNT || voices[emotion].count == 0) return;
    float pitch = synth_voice_pitch(emotion, codepoint, next);
    if (pitch <= 0) return;
    double now = GetTime();
    bool question = next == '?';             // the letter before a '?' gets the lift so never skip it
    if (!question && now - voice_last < VOICE_GAP) return;
    voice_last = now;
    duck_left = DUCK_TIME;
    float vol = master() * CATEGORY_GAIN[SFX_CAT_VOICE];
    play_slot(&voices[emotion], vol, pitch * jitter(10), pan);
    if (logging) TraceLog(LOG_INFO, "SFX: voice %c (%s, pitch %.2f)", codepoint, synth_emotion_name(emotion), pitch);
}

void sfx_update(float dt)
{
    if (duck_left > 0) duck_left -= dt;
    if (hold_left > 0) hold_left -= dt;
}
