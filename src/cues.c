#include "cues.h"
#include "raylib.h"
#include "sfx.h"

#define COMBO_GAP 1.2                // pokes closer together than this are one combo
#define COMBO_GRUMBLE 5              // and the 5th in a row gets grumbled at

static float pan;
static bool chat_was_open, chat_known;
static bool poked;                   // this frame's click on him also toggled the chat box
static bool led;                     // a lead in sound replaces the next bubble pop
static double last_poke = -10.0;
static int combo;

static void play(SfxId id)
{
    sfx_play(id, 1.0f, 1.0f, pan);
}

void cues_play(SfxId id)
{
    play(id);
}

void cues_lead(SfxId id)
{
    float len = sfx_length(id);
    if (len <= 0 || sfx_muted()) return;     // nothing to hear, the line pops like normal
    play(id);
    led = true;
    sfx_voice_hold(len * 0.8f);
}

void cues_poke(void)
{
    double now = GetTime();
    combo = now - last_poke < COMBO_GAP ? combo + 1 : 0;
    last_poke = now;
    poked = true;
    if (combo + 1 >= COMBO_GRUMBLE && sfx_length(SFX_POKE_GRUMBLE) > 0) {
        play(SFX_POKE_GRUMBLE);
        combo = 0;
    } else {
        sfx_play(SFX_POKE, 1.0f, 1.0f + 0.08f * (combo < 6 ? combo : 6), pan);
    }
}

// a key on his keyboard (ev.note 0..27 left to right), low on the left and high on the
// right, about a fifth apart end to end
static float key_pitch(int note)
{
    if (note < 0) note = 0;
    if (note > 27) note = 27;
    return 0.86f + 0.42f * (float)note / 27.0f;
}

static SfxId beat_sound(int beat)
{
    switch (beat) {
    case BEAT_HOP:     return SFX_HOP;
    case BEAT_SPARKLE: return SFX_SPARKLE;
    case BEAT_LAUGH:   return SFX_LAUGH;
    case BEAT_HEART:   return SFX_HEART;
    case BEAT_BANG:    return SFX_STING;
    case BEAT_HUH:     return SFX_HUH;
    case BEAT_TING:    return SFX_TING;
    case BEAT_SIGH:    return SFX_SIGH;
    case BEAT_QUIVER:  return SFX_QUIVER;
    case BEAT_STOMP:   return SFX_STOMP;
    case BEAT_HMPH:    return SFX_HMPH;
    default:           return SFX_COUNT;
    }
}

void cues_frame(const Mascot *m, SpeechEvents se, float where, bool chat_open)
{
    pan = where;

    // chat box, a click on him that opened it already booped
    if (chat_known && chat_open != chat_was_open && !poked)
        play(chat_open ? SFX_CHAT_OPEN : SFX_CHAT_CLOSE);
    chat_was_open = chat_open;
    chat_known = true;
    poked = false;

    if (se.popped) {
        if (!led) play(SFX_BUBBLE);
        led = false;
    }
    if (se.thinking) play(SFX_THINK);
    if (se.page) play(SFX_PAGE);
    if (se.history) play(SFX_HISTORY);
    if (se.typed > 0) sfx_voice(se.emotion, se.last, se.next, pan);

    // "whoops!", you grabbed the mouse off him or a drive ended in a shrug. theres no MEV
    // bit left for it so anim_wheel.c flags it in its own state
    if (m->wheel.oops) play(SFX_OOPS);

    unsigned b = m->ev.bits;
    if (!b) return;

    // walking over to something and pointing at it. steps are the quietest thing he does
    // and the walk home starts under the success jingle, so a bit extra to still hear them
    if (b & MEV_STEP) sfx_play(SFX_STEP, 1.5f, m->ev.foot < 0 ? 0.96f : 1.04f, pan);
    if (b & MEV_ARM_OUT) play(SFX_ZIP_OUT);
    if (b & MEV_PLUG) {
        play(SFX_PLUG);
        play(SFX_RING);
    }
    if (b & MEV_ARM_IN) play(SFX_ZIP_IN);
    if (b & MEV_POINT_HIT) play(SFX_SUCCESS);
    if (b & MEV_POINT_MISS) play(SFX_UNPLUG);
    if (b & MEV_WHISTLE) sfx_play(SFX_WHISTLE, 1.0f, synth_whistle_pitch(m->ev.note), pan);
    if (b & MEV_POINT_HUH) play(SFX_HUH);
    if (b & MEV_POINT_BANG) play(SFX_STING);

    // driving. his keys go up in pitch left to right, space is a lower thock and Enter a
    // chunkier one, all a bit quieter than the rest since a line of text is a lot of keys
    if (b & MEV_GRAB) play(SFX_GRAB);
    if (b & MEV_CLACK) play(SFX_CLACK);
    if (b & MEV_KEY) {
        unsigned c = m->drive_in.last_key;
        float pitch = key_pitch(m->ev.note), gain = 0.7f;
        if (c == ' ') { pitch = 0.74f; gain = 0.85f; }
        else if (c == '\n' || c == '\r') { pitch = 0.8f; gain = 1.0f; }
        sfx_play(SFX_KEY, gain, pitch, pan);
    }
    if (b & MEV_HAUL) play(SFX_HAUL);
    if (b & MEV_LETGO) play(SFX_LETGO);
    if (b & MEV_VICTORY) play(SFX_VICTORY);

    // getting dragged around, a gentle set down is a softer higher tap
    if (b & MEV_PICKUP) play(SFX_PICKUP);
    if (b & MEV_DROP) sfx_play(SFX_LAND, 0.4f + 0.6f * m->ev.land, 1.1f - 0.25f * m->ev.land, pan);
    if (b & MEV_SHAKE) play(SFX_SHAKE);
    if (b & MEV_DIZZY) play(SFX_DIZZY);

    if (b & MEV_YAWN) play(SFX_YAWN);
    if (b & MEV_NOD) play(SFX_NOD);
    if (b & MEV_ASLEEP) play(SFX_SETTLE);
    if (b & MEV_SNORE_POP) play(SFX_SNORE_POP);
    if (b & MEV_WAKE_JOLT) play(SFX_WAKE_JOLT);
    if (b & MEV_WAKE_SOFT) play(SFX_WAKE_SOFT);

    // quitting, the power cuts once hes done waving
    if (b & MEV_SWITCH_OFF) play(SFX_QUIT);

    // scrolling through old lines acts their emotions out again, but silently
    if ((b & MEV_BEAT) && !speech_face_replayed()) {
        SfxId id = beat_sound(m->ev.beat);
        if (id != SFX_COUNT) play(id);
    }
}
