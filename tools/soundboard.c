// for tuning his sounds (src/synth.c) by ear
//   soundboard.exe              window with every sound, click or enter plays one,
//                               V says a sample line in the picked emotion
//   soundboard.exe --check      renders everything and checks lengths and levels
//   soundboard.exe --export DIR writes every sound, voice and sample line as a .wav
// check and export dont open a window or the audio device

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#include "synth.h"

#define TOLERANCE_DB 1.5f       // how far a sound can be off its categorys loudness
#define PEAK_MAX 0.98f
#define COMMA_PAUSE 0.12f       // same typing pauses as the bubble (speech.c)
#define STOP_PAUSE 0.3f
#define LINE_TAIL 0.15f         // secs of room after the last letter

static const char *const CATEGORY_NAMES[SFX_CAT_COUNT] = { "ui", "voice", "body", "ambient" };

// a line per emotion to hear the babble in context
static const char *const LINES[EMOTION_COUNT] = {
    "Hi there, I'm Manikin. Need a hand with anything?",
    "Yay, that worked! Nice one, you did it.",
    "Ooh ooh, look at this! It's so cool!",
    "Haha, that's hilarious! Stop it, haha.",
    "Aww, thank you. You're the best, really.",
    "Whoa! Wait, where did that come from?",
    "Hmm, which button was it again? This one?",
    "Heh, told you so. I'm kind of a genius.",
    "Oh no, it didn't work. I'm sorry.",
    "Um, are you sure about this? It looks risky.",
    "Hey, stop poking me. I mean it.",
};

// chars per second, same speeds the bubble types at
static float type_speed(Emotion e)
{
    if (e == EMOTION_SAD || e == EMOTION_WORRIED) return 35.0f;
    if (e == EMOTION_EXCITED || e == EMOTION_LAUGHING) return 70.0f;
    return 55.0f;
}

// pause after text[i], the bubble takes a breath after punctuation
static float pause_after(const char *text, int i)
{
    char c = text[i], next = text[i + 1];
    if (next != ' ' && next != 0) return 0;
    if (c == ',' || c == ';' || c == ':') return COMMA_PAUSE;
    if (c == '.' || c == '!' || c == '?') return STOP_PAUSE;
    return 0;
}

static Wave to_wave(int16_t *pcm, int frames)
{
    return (Wave){ .frameCount = (unsigned)frames, .sampleRate = SYNTH_RATE,
                   .sampleSize = 16, .channels = 1, .data = pcm };
}

static float peak_of(const int16_t *pcm, int frames)
{
    int peak = 0;
    for (int i = 0; i < frames; i++) {
        int v = abs(pcm[i]);
        if (v > peak) peak = v;
    }
    return peak / 32767.0f;
}

// a whole spoken line offline, the syllable gets resampled per letter and mixed into one
// buffer then scaled down if the overlaps add up past full scale
static int16_t *render_line(Emotion e, const char *text, int *frames)
{
    int syl_n;
    int16_t *syl = synth_render_voice(e, &syl_n);
    if (!syl) { *frames = 0; return NULL; }

    float t = 0, step = 1.0f / type_speed(e);
    for (int i = 0; text[i]; i++) t += step + pause_after(text, i);
    int n = (int)((t + LINE_TAIL) * SYNTH_RATE) + (int)(syl_n / 0.5f);
    float *mix = calloc((size_t)n, sizeof *mix);

    t = 0;
    for (int i = 0; text[i]; i++) {
        float pitch = synth_voice_pitch(e, (unsigned char)text[i], (unsigned char)text[i + 1]);
        if (pitch > 0) {
            int at = (int)(t * SYNTH_RATE);
            for (int j = 0; at + j < n; j++) {
                float src = j * pitch;               // linear resampling, like SetSoundPitch
                int k = (int)src;
                if (k + 1 >= syl_n) break;
                float frac = src - k;
                mix[at + j] += (syl[k] + (syl[k + 1] - syl[k]) * frac) / 32767.0f;
            }
        }
        t += step + pause_after(text, i);
    }

    float peak = 0;
    for (int i = 0; i < n; i++) if (fabsf(mix[i]) > peak) peak = fabsf(mix[i]);
    float gain = peak > 0.97f ? 0.97f / peak : 1.0f;
    int16_t *pcm = malloc((size_t)n * sizeof *pcm);
    for (int i = 0; i < n; i++) pcm[i] = (int16_t)lrintf(mix[i] * gain * 32767.0f);
    free(mix);
    free(syl);
    *frames = n;
    return pcm;
}

// ---- --check ----

// checks one rendered sound and prints its row, false if it FAILs
static bool check_row(const char *name, SfxCategory cat, int max_ms, int16_t *pcm, int frames)
{
    if (!name) {
        printf("%-16s %-8s FAIL: no name in the info table\n", "?", CATEGORY_NAMES[cat]);
        return false;
    }
    if (!pcm) {
        printf("%-16s %-8s %6s %6s %8s  FAIL: no recipe\n", name, CATEGORY_NAMES[cat], "--", "--", "--");
        return false;
    }
    float ms = frames * 1000.0f / SYNTH_RATE;
    float peak = peak_of(pcm, frames);
    float db = synth_rms_db(pcm, frames);
    float off = db - synth_target_db(cat);
    const char *why = NULL;
    if (peak == 0) why = "silent";
    else if (peak > PEAK_MAX) why = "peak too high";
    else if (ms > max_ms) why = "too long";
    else if (fabsf(off) > TOLERANCE_DB) why = "loudness off target";
    printf("%-16s %-8s %6.0f %6.3f %8.1f  %s%s\n", name, CATEGORY_NAMES[cat],
           ms, peak, db, why ? "FAIL: " : "OK", why ? why : "");
    return !why;
}

static int check(void)
{
    int fails = 0, ok = 0, missing = 0;
    printf("%-16s %-8s %6s %6s %8s  %s\n", "sound", "category", "ms", "peak", "rms dB", "result");
    for (int id = 0; id < SFX_COUNT; id++) {
        const SfxInfo *info = synth_info((SfxId)id);
        int frames;
        int16_t *pcm = synth_render((SfxId)id, &frames);
        if (!pcm) missing++;
        if (!check_row(info->name, info->category, info->max_ms, pcm, frames)) fails++;
        else ok++;
        free(pcm);
    }
    for (int e = 0; e < EMOTION_COUNT; e++) {
        int frames;
        int16_t *pcm = synth_render_voice((Emotion)e, &frames);
        char name[32];
        snprintf(name, sizeof name, "voice_%s", synth_emotion_name((Emotion)e));
        if (!pcm) missing++;
        if (!check_row(name, SFX_CAT_VOICE, SYNTH_VOICE_MAX_MS, pcm, frames)) fails++;
        else ok++;
        free(pcm);
    }

    // his voice, letters have to land on the scale and everything else stays silent
    bool pitches_ok = true;
    for (int e = 0; e < EMOTION_COUNT; e++)
        for (int c = 32; c < 127; c++) {
            bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            float p = synth_voice_pitch((Emotion)e, c, 0), q = synth_voice_pitch((Emotion)e, c, '?');
            if (letter ? (p < 0.6f || p > 1.6f || q <= p) : (p != 0 || q != 0)) pitches_ok = false;
        }
    if (!pitches_ok) { printf("voice pitches     FAIL: a letter off the scale or a non-letter voiced\n"); fails++; }

    printf("\n%d ok, %d FAIL (%d of them with no recipe)\n", ok, fails, missing);
    return fails ? 1 : 0;
}

// ---- --export ----

static int export_all(const char *dir)
{
    if (!DirectoryExists(dir) && MakeDirectory(dir) != 0) {
        fprintf(stderr, "can't create %s\n", dir);
        return 1;
    }
    int written = 0, failed = 0;
    for (int id = 0; id < SFX_COUNT; id++) {
        int frames;
        int16_t *pcm = synth_render((SfxId)id, &frames);
        if (pcm && ExportWave(to_wave(pcm, frames), TextFormat("%s/%s.wav", dir, synth_info((SfxId)id)->name))) written++;
        else failed++;
        free(pcm);
    }
    for (int e = 0; e < EMOTION_COUNT; e++) {
        const char *name = synth_emotion_name((Emotion)e);
        int frames;
        int16_t *pcm = synth_render_voice((Emotion)e, &frames);
        if (pcm && ExportWave(to_wave(pcm, frames), TextFormat("%s/voice_%s.wav", dir, name))) written++;
        else failed++;
        free(pcm);
        pcm = render_line((Emotion)e, LINES[e], &frames);
        if (pcm && ExportWave(to_wave(pcm, frames), TextFormat("%s/line_%s.wav", dir, name))) written++;
        else failed++;
        free(pcm);
    }
    printf("wrote %d files to %s%s\n", written, dir, failed ? " (some failed)" : "");
    return failed ? 1 : 0;
}

// ---- the window ----

#define WIN_W 760
#define WIN_H 700
#define ROWS 30                 // per column
#define ROW_H 19
#define LIST_Y 92
#define COL_W 370
#define ALIASES 4               // copies per sound so fast repeats can overlap
#define VOICE_ALIASES 6

static Sound sounds[SFX_COUNT][ALIASES + 1];      // [0] owns the samples, the rest alias it
static bool have[SFX_COUNT];
static int next_alias[SFX_COUNT];
static Sound voices[EMOTION_COUNT][VOICE_ALIASES + 1];
static int next_voice[EMOTION_COUNT];

static Sound load(int16_t *pcm, int frames, Sound *slots, int aliases)
{
    slots[0] = LoadSoundFromWave(to_wave(pcm, frames));
    for (int i = 1; i <= aliases; i++) slots[i] = LoadSoundAlias(slots[0]);
    return slots[0];
}

static void play(int id)
{
    if (!have[id] || !IsAudioDeviceReady()) return;
    next_alias[id] = (next_alias[id] + 1) % (ALIASES + 1);
    PlaySound(sounds[id][next_alias[id]]);
}

static void play_voice(Emotion e, float pitch)
{
    next_voice[e] = (next_voice[e] + 1) % (VOICE_ALIASES + 1);
    Sound s = voices[e][next_voice[e]];
    SetSoundPitch(s, pitch);
    PlaySound(s);
}

static int interactive(void)
{
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(WIN_W, WIN_H, "Manikin soundboard");
    SetTargetFPS(60);
    InitAudioDevice();
    bool audio = IsAudioDeviceReady();

    for (int id = 0; id < SFX_COUNT; id++) {
        int frames;
        int16_t *pcm = synth_render((SfxId)id, &frames);
        if (!pcm) continue;
        have[id] = true;
        if (audio) load(pcm, frames, sounds[id], ALIASES);
        free(pcm);
    }
    if (audio) {
        for (int e = 0; e < EMOTION_COUNT; e++) {
            int frames;
            int16_t *pcm = synth_render_voice((Emotion)e, &frames);
            load(pcm, frames, voices[e], VOICE_ALIASES);
            free(pcm);
        }
    }

    int sel = 0;
    Emotion emotion = EMOTION_NONE;
    const char *line = NULL;     // the line being spoken, NULL when quiet
    int letter = 0;
    float wait = 0;
    char status[160] = "";

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_DOWN)) sel = (sel + 1) % SFX_COUNT;
        if (IsKeyPressed(KEY_UP)) sel = (sel + SFX_COUNT - 1) % SFX_COUNT;
        if (IsKeyPressed(KEY_RIGHT)) emotion = (Emotion)((emotion + 1) % EMOTION_COUNT);
        if (IsKeyPressed(KEY_LEFT)) emotion = (Emotion)((emotion + EMOTION_COUNT - 1) % EMOTION_COUNT);
        if (IsKeyPressed(KEY_ENTER)) play(sel);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            Vector2 m = GetMousePosition();
            int col = (int)(m.x - 10) / COL_W, row = (int)(m.y - LIST_Y) / ROW_H;
            int id = col * ROWS + row;
            if (m.y >= LIST_Y && col >= 0 && col < 2 && row < ROWS && id < SFX_COUNT) {
                sel = id;
                play(id);
            }
        }
        if (IsKeyPressed(KEY_V) && audio) {
            line = LINES[emotion];
            letter = 0;
            wait = 0;
        }
        if (IsKeyPressed(KEY_E)) {
            int failed = export_all("build/sounds");
            snprintf(status, sizeof status, "%s", failed ? "export to build\\sounds failed" : "exported to build\\sounds");
        }

        // say the line like the bubble types it, a blip per letter and a pause after punctuation
        if (line) {
            wait -= GetFrameTime();
            while (line && wait <= 0) {
                float pitch = synth_voice_pitch(emotion, (unsigned char)line[letter], (unsigned char)line[letter + 1]);
                if (pitch > 0) play_voice(emotion, pitch);
                wait += 1.0f / type_speed(emotion) + pause_after(line, letter);
                if (!line[++letter]) line = NULL;
            }
        }

        BeginDrawing();
        ClearBackground((Color){ 24, 26, 34, 255 });
        DrawText("Manikin soundboard", 10, 10, 20, RAYWHITE);
        DrawText("up/down pick   enter or click play   left/right emotion   V speak   E export",
                 10, 38, 10, GRAY);
        DrawText(TextFormat("emotion: %s", synth_emotion_name(emotion)), 10, 58, 20, GOLD);
        if (!audio) DrawText("no audio device: InitAudioDevice failed", 300, 58, 20, RED);
        else if (status[0]) DrawText(status, 300, 64, 10, LIME);

        for (int id = 0; id < SFX_COUNT; id++) {
            const SfxInfo *info = synth_info((SfxId)id);
            int x = 10 + (id / ROWS) * COL_W, y = LIST_Y + (id % ROWS) * ROW_H;
            if (id == sel) DrawRectangle(x - 4, y - 2, COL_W - 8, ROW_H, (Color){ 60, 70, 100, 255 });
            DrawText(info->name, x, y, 20, have[id] ? RAYWHITE : GRAY);
            DrawText(CATEGORY_NAMES[info->category], x + 220, y + 6, 10, GRAY);
            if (!have[id]) DrawText("--", x + 310, y, 20, GRAY);
        }
        EndDrawing();
    }

    if (audio) {
        for (int id = 0; id < SFX_COUNT; id++) {
            if (!have[id]) continue;
            for (int i = 1; i <= ALIASES; i++) UnloadSoundAlias(sounds[id][i]);
            UnloadSound(sounds[id][0]);
        }
        for (int e = 0; e < EMOTION_COUNT; e++) {
            for (int i = 1; i <= VOICE_ALIASES; i++) UnloadSoundAlias(voices[e][i]);
            UnloadSound(voices[e][0]);
        }
        CloseAudioDevice();
    }
    CloseWindow();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--check") == 0) return check();
    if (argc >= 3 && strcmp(argv[1], "--export") == 0) {
        SetTraceLogLevel(LOG_WARNING);
        return export_all(argv[2]);
    }
    if (argc >= 2) {
        fprintf(stderr, "usage: soundboard [--check | --export DIR]\n");
        return 2;
    }
    return interactive();
}
