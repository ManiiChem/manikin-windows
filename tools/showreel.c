// records his animations at real speed as pngs for make_gifs.py to turn into gifs
//   showreel.exe out_dir         -> out_dir/<scene>_0000.png ...
//   showreel.exe out_dir point   -> only the pointing scenes
//   showreel.exe out_dir drive   -> only the driving scenes
// each scene fakes the overlays inputs at 60 fps and saves every 3rd frame (20 fps).
// point_*, walk_* and drive* film the whole window walking across a fake desktop

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "raylib.h"
#include "mascot.h"
#include "mascot_kit.h"
#include "point_sim.h"
#include "drive_sim.h"

#define STEP (1.0f / 60.0f)
#define KEEP_EVERY 3
#define FRAME_W 168
#define FRAME_H 200
#define SPRITE_X 24
#define SPRITE_Y 26

typedef struct {
    const char *name;
    float seconds;
    const char *(*drive)(Mascot *m, float t);   // sets inputs, returns the caption
} Scene;

// ---- emotions, each one talks for 1 s then hangs around ----

static const char *const EMOTIONS[] = { "happy", "excited", "laughing", "love", "surprised",
                                        "confused", "smug", "sad", "worried", "annoyed" };
#define EMO_SEG 2.0f
#define EMO_TALK 1.0f

static const char *drive_emotions(Mascot *m, float t)
{
    static int last = -1;
    int seg = (int)(t / EMO_SEG);
    if (t == 0.0f) last = -1;
    if (seg > 9) seg = 9;
    if (seg != last) {
        m->emotion = emotion_from_name(EMOTIONS[seg]);
        m->emotion_age = 0;
        last = seg;
    }
    m->mood = fmodf(t, EMO_SEG) < EMO_TALK ? MOOD_TALKING : MOOD_IDLE;
    return EMOTIONS[seg];
}

// ---- gaze, circle far away then come in close and sit between his eyes ----

static float lerpf(float a, float b, float k) { return a + (b - a) * (k < 0 ? 0 : (k > 1 ? 1 : k)); }

static const char *drive_gaze(Mascot *m, float t)
{
    m->mood = MOOD_IDLE;
    if (t < 4.0f) {
        float a = t * 1.6f;
        m->cursor_dx = cosf(a) * 650.0f;
        m->cursor_dy = sinf(a) * 400.0f;
        return "following the cursor";
    }
    if (t < 5.2f) {
        float k = (t - 4.0f) / 1.2f;
        m->cursor_dx = lerpf(650.0f, 70.0f, k);
        m->cursor_dy = lerpf(0.0f, -20.0f, k);
        return "coming closer";
    }
    if (t < 6.8f) {
        float k = (t - 5.2f) / 0.6f;
        m->cursor_dx = lerpf(70.0f, 12.0f, k);
        m->cursor_dy = lerpf(-20.0f, 8.0f, k);
        return "on his face";
    }
    if (t < 8.2f) {
        m->cursor_dx = 0.0f;
        m->cursor_dy = -6.0f;
        return "between his eyes";
    }
    float k = (t - 8.2f) / 0.5f;
    m->cursor_dx = lerpf(0.0f, -900.0f, k);
    m->cursor_dy = lerpf(-6.0f, 300.0f, k);
    return "gone again";
}

// ---- sleep, a time lapse that skips the boring minutes ----

static const char *drive_sleep(Mascot *m, float t)
{
    m->mood = MOOD_IDLE;
    m->cursor_dx = 2000.0f;          // cursor way off so its not hovering him
    m->cursor_dy = 0.0f;
    if (t < 2.0f)  { m->idle_for = t;                      return "awake"; }
    if (t < 6.0f)  { m->idle_for = 122.0f + (t - 2.0f);    return "2 min ignored: drowsy"; }
    if (t < 10.0f) { m->idle_for = 149.5f + (t - 6.0f);    return "yawn"; }
    if (t < 16.0f) { m->idle_for = 160.0f + (t - 10.0f);   return "nodding off"; }
    if (t < 22.0f) { m->idle_for = 181.0f + (t - 16.0f);   return "3 min: asleep"; }
    m->idle_for = t - 22.0f;                                return "poked awake";
}

// ---- physics, pick him up, drag, shake, drop ----

static const char *drive_physics(Mascot *m, float t)
{
    m->mood = MOOD_IDLE;
    m->cursor_dx = 0.0f;
    m->cursor_dy = -300.0f;
    m->vel_x = m->vel_y = 0.0f;
    m->held = t >= 0.8f && t < 5.6f;
    if (t < 0.8f) return "idle";
    if (t < 2.0f) return "picked up";
    if (t < 3.0f) { m->vel_x = 450.0f;   return "dragged slowly"; }
    if (t < 3.6f) { m->vel_x = -2200.0f; return "dragged fast"; }
    if (t < 4.2f) return "stop";
    if (t < 5.6f) { m->vel_x = sinf(t * 25.0f) > 0 ? 2600.0f : -2600.0f; return "shaken!"; }
    return "dropped";
}

// ---- bye, ctrl+q then a wave then off like an old crt ----

static const char *drive_bye(Mascot *m, float t)
{
    m->mood = MOOD_IDLE;
    m->cursor_dx = 0.0f;
    m->cursor_dy = -300.0f;
    m->leaving = t >= 0.8f;
    if (!m->leaving) return "idle";
    return t < 0.8f + BYE_WAVE ? "Ctrl+Q: bye!" : "switching off";
}

static const Scene SCENES[] = {
    { "emotions", 10 * EMO_SEG, drive_emotions },
    { "gaze",     9.0f,         drive_gaze },
    { "sleep",    25.5f,        drive_sleep },
    { "physics",  10.0f,        drive_physics },
    { "bye",      0.8f + BYE_LEN + 0.5f, drive_bye },
};

// ---- pointing, the window walking across a fake desktop ----

#define DESK_W 640
#define DESK_H 480

static const PointTrip *const TRIPS[] = {
    &TRIP_SLOW, &TRIP_FAST, &TRIP_RIGHT, &TRIP_LEFT, &TRIP_FAR, &TRIP_UPLEFT, &TRIP_DOWN,
    &TRIP_TINY, &TRIP_HUGE, &TRIP_CLICK, &TRIP_CLICK_OUT, &TRIP_MOVED, &TRIP_CLICK_GO, &TRIP_MOVED_GO,
};

static const char *trip_caption(const PointerView *v)
{
    static const char *const PHASE[4] = { "home", "walking over", "pointing", "walking home" };
    static const char *const RESULT[6] = { "", "clicked it!", "clicked elsewhere", "target moved",
                                           "dismissed", "cleared" };
    if (v->result && v->result_t < 0.8f) return RESULT[v->result];
    return PHASE[v->phase];
}

// screen px of the frames top left, stays put if the whole trip fits, otherwise follows him
static Vector2 trip_camera(const PointTrip *p, Vector2 win)
{
    Vector2 a = p->home, b = sim_dest(p);
    float fx = SIM_FACE_X, fy = SIM_FACE_Y;
    Vector2 c;
    if (fabsf(a.x - b.x) < DESK_W - 300 && fabsf(a.y - b.y) < DESK_H - 300) {
        Vector2 t = sim_target(p);
        float cx = (a.x + b.x) * 0.5f + fx, cy = (a.y + b.y) * 0.5f + fy;
        cx = cx * 0.7f + t.x * 0.3f;
        cy = cy * 0.7f + t.y * 0.3f;
        c = (Vector2){ cx - DESK_W / 2, cy - DESK_H / 2 };
    } else {
        c = (Vector2){ win.x + fx - DESK_W / 2, win.y + fy - DESK_H / 2 };
    }
    c.x = fminf(fmaxf(c.x, 0.0f), p->screen.x - DESK_W);
    c.y = fminf(fmaxf(c.y, 0.0f), p->screen.y - DESK_H);
    return (Vector2){ floorf(c.x), floorf(c.y) };
}

static void record_trips(const char *out_dir)
{
    RenderTexture2D win_rt = LoadRenderTexture(SIM_WIN_W, SIM_WIN_H);
    RenderTexture2D rt = LoadRenderTexture(DESK_W, DESK_H);
    for (size_t s = 0; s < sizeof TRIPS / sizeof TRIPS[0]; s++) {
        const PointTrip *p = TRIPS[s];
        Mascot m;
        mascot_init(&m);
        SetRandomSeed(7);
        int steps = (int)(sim_length(p) / STEP), saved = 0;
        for (int i = 0; i < steps; i++) {
            Vector2 win;
            PointerView v = sim_view(p, i * STEP, &win);
            sim_apply(p, &m, &v, i * STEP);
            mascot_update(&m, STEP);
            if (i % KEEP_EVERY) continue;

            BeginTextureMode(win_rt);
            ClearBackground(BLANK);
            sim_draw_window(&v, &m);
            EndTextureMode();

            Vector2 cam = trip_camera(p, win);
            BeginTextureMode(rt);
            sim_draw_desktop(p, &SIM_DARK, cam.x, cam.y, DESK_W, DESK_H);
            DrawTextureRec(win_rt.texture, (Rectangle){ 0, 0, SIM_WIN_W, -SIM_WIN_H },
                           (Vector2){ floorf(win.x - cam.x), floorf(win.y - cam.y) }, WHITE);
            DrawRectangle(0, DESK_H - 20, DESK_W, 20, (Color){ 20, 20, 26, 255 });
            DrawText(trip_caption(&v), 8, DESK_H - 15, 10, LIGHTGRAY);
            EndTextureMode();

            Image img = LoadImageFromTexture(rt.texture);
            ImageFlipVertical(&img);
            char path[512];
            snprintf(path, sizeof path, "%s/%s_%04d.png", out_dir, p->name, saved++);
            ExportImage(img, path);
            UnloadImage(img);
        }
        printf("%s: %d frames\n", p->name, saved);
    }
    UnloadRenderTexture(rt);
    UnloadRenderTexture(win_rt);
}

// ---- driving, wheel_previews scripted drives on a fake desktop with a sign up form ----

// one scene per script in wheel_preview (frames that share a drive function), run a bit
// past its last frame. the camera drifts after him and your pointer
static void record_drives(const char *out_dir)
{
    static const char *const NAMES[4] = { "drive", "drive_oops", "drive_handover", "drive_4" };
    static PreviewFrame frames[64];
    int count = wheel_preview(frames, 64), scenes = 0;
    void (*seen[4])(Mascot *m, float t) = { 0 };
    RenderTexture2D win_rt = LoadRenderTexture(SIM_WIN_W, SIM_WIN_H);
    RenderTexture2D rt = LoadRenderTexture(DESK_W, DESK_H);
    for (int i = 0; i < count && scenes < 4; i++) {
        void (*drive)(Mascot *m, float t) = frames[i].drive;
        bool dup = false;
        for (int k = 0; k < scenes; k++) dup = dup || seen[k] == drive;
        if (dup) continue;
        const char *name = NAMES[scenes];
        seen[scenes++] = drive;
        float len = 0.0f;
        for (int j = 0; j < count; j++)
            if (frames[j].drive == drive && frames[j].seconds > len) len = frames[j].seconds;
        len += 1.2f;

        Mascot m;
        DriveSim ds;
        mascot_init(&m);
        dsim_init(&ds);
        SetRandomSeed(7);
        Vector2 cam = { 0 };
        int steps = (int)(len / STEP), saved = 0;
        for (int s = 0; s < steps; s++) {
            drive(&m, s * STEP);
            dsim_step(&ds, &m, STEP);
            mascot_update(&m, STEP);
            if (s % KEEP_EVERY) continue;

            Vector2 ptr = dsim_pointer(&ds, &m);
            Vector2 aim = { (ds.win.x + SIM_FACE_X) * 0.7f + ptr.x * 0.3f - DESK_W / 2,
                            (ds.win.y + SIM_FACE_Y) * 0.7f + ptr.y * 0.3f - DESK_H / 2 };
            aim.x = fminf(fmaxf(aim.x, 0.0f), DSIM_SCREEN_W - DESK_W);
            aim.y = fminf(fmaxf(aim.y, 0.0f), DSIM_SCREEN_H - DESK_H);
            if (s == 0) cam = aim;
            cam.x += (aim.x - cam.x) * 0.15f;
            cam.y += (aim.y - cam.y) * 0.15f;
            Vector2 c = { floorf(cam.x), floorf(cam.y) };

            BeginTextureMode(win_rt);
            ClearBackground(BLANK);
            dsim_draw_window(&ds, &m);
            EndTextureMode();

            BeginTextureMode(rt);
            dsim_draw_desktop(&ds, &SIM_DARK, c.x, c.y, DESK_W, DESK_H, m.time);
            DrawTextureRec(win_rt.texture, (Rectangle){ 0, 0, SIM_WIN_W, -SIM_WIN_H },
                           (Vector2){ floorf(ds.win.x - c.x), floorf(ds.win.y - c.y) }, WHITE);
            dsim_draw_pointer(floorf(ptr.x - c.x), floorf(ptr.y - c.y));
            DrawRectangle(0, DESK_H - 20, DESK_W, 20, (Color){ 20, 20, 26, 255 });
            DrawText(dsim_caption(&m), 8, DESK_H - 15, 10, LIGHTGRAY);
            EndTextureMode();

            Image img = LoadImageFromTexture(rt.texture);
            ImageFlipVertical(&img);
            char path[512];
            snprintf(path, sizeof path, "%s/%s_%04d.png", out_dir, name, saved++);
            ExportImage(img, path);
            UnloadImage(img);
        }
        printf("%s: %d frames\n", name, saved);
    }
    UnloadRenderTexture(rt);
    UnloadRenderTexture(win_rt);
}

int main(int argc, char **argv)
{
    const char *out_dir = argc > 1 ? argv[1] : ".";
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(SIM_WIN_W, SIM_WIN_H, "showreel");   // point_fx clamps to the window size
    RenderTexture2D rt = LoadRenderTexture(FRAME_W, FRAME_H);

    bool only_point = argc > 2 && strcmp(argv[2], "point") == 0;
    bool only_drive = argc > 2 && strcmp(argv[2], "drive") == 0;
    if (only_drive) {
        UnloadRenderTexture(rt);
        record_drives(out_dir);
        CloseWindow();
        return 0;
    }
    for (size_t s = 0; !only_point && s < sizeof SCENES / sizeof SCENES[0]; s++) {
        const Scene *sc = &SCENES[s];
        Mascot m;
        mascot_init(&m);
        SetRandomSeed(7);
        int steps = (int)(sc->seconds / STEP), saved = 0;
        for (int i = 0; i < steps; i++) {
            const char *caption = sc->drive(&m, i * STEP);
            mascot_update(&m, STEP);
            if (i % KEEP_EVERY) continue;

            BeginTextureMode(rt);
            ClearBackground((Color){ 32, 32, 40, 255 });
            mascot_draw(&m, SPRITE_X, SPRITE_Y, MASCOT_PX);
            DrawText(caption, 8, FRAME_H - 16, 10, LIGHTGRAY);
            EndTextureMode();

            Image img = LoadImageFromTexture(rt.texture);
            ImageFlipVertical(&img);
            char path[512];
            snprintf(path, sizeof path, "%s/%s_%04d.png", out_dir, sc->name, saved++);
            ExportImage(img, path);
            UnloadImage(img);
        }
        printf("%s: %d frames\n", sc->name, saved);
    }
    UnloadRenderTexture(rt);
    record_trips(out_dir);
    record_drives(out_dir);
    CloseWindow();
    return 0;
}
