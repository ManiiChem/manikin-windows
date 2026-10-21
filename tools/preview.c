// draws him to a png so i can check the art without running the overlay
//   preview.exe out.png             the built in moods
//   preview.exe out.png <module>    a modules scenario, emotion | gaze | sleep | physics | bye | wheel
//   preview.exe out.png point       walking over and pointing (fake trips from point_sim.h)
// out.png is every frame on dark and light, out_zoom.png one frame at 3x (point and wheel
// crop the whole overlay window instead and put the light ones in out_light.png)

#include <stdio.h>
#include <string.h>

#include "raylib.h"
#include "mascot.h"
#include "mascot_kit.h"
#include "point_sim.h"
#include "drive_sim.h"

#define MAX_FRAMES 64
#define COLUMNS 8
#define SPRITE_W (MASCOT_W * MASCOT_PX)
#define SPRITE_H (MASCOT_H * MASCOT_PX)
#define CELL_W (SPRITE_W + 24)
#define CELL_H (SPRITE_H + 36)
#define STEP (1.0f / 60.0f)

// ---- built in moods, no simulating just set the fields ----

typedef struct {
    const char *label;
    Mood mood;
    float time;
    bool blink;
    int typed;
} MoodFrame;

static const MoodFrame MOODS[] = {
    { "idle",     MOOD_IDLE,     0.10f, false, 0 },
    { "idle bob", MOOD_IDLE,     1.20f, false, 0 },
    { "blink",    MOOD_IDLE,     0.10f, true,  0 },
    { "think 1",  MOOD_THINKING, 0.50f, false, 0 },
    { "think 2",  MOOD_THINKING, 1.00f, false, 0 },
    { "talk 1",   MOOD_TALKING,  0.05f, false, 0 },
    { "talk 2",   MOOD_TALKING,  0.17f, false, 0 },
    { "talk 3",   MOOD_TALKING,  0.30f, false, 0 },
    { "locked",   MOOD_SHY,      0.10f, false, 0 },
    { "locked 3", MOOD_SHY,      0.50f, false, 3 },
    { "ask",      MOOD_ASK,      0.20f, false, 0 },
    { "happy",    MOOD_HAPPY,    0.10f, false, 0 },
};

static Mascot mood_frame(int i)
{
    Mascot m;
    mascot_init(&m);
    m.mood = MOODS[i].mood;
    m.time = MOODS[i].time;
    m.blink_left = MOODS[i].blink ? 0.1f : 0.0f;
    m.typed = MOODS[i].typed;
    return m;
}

// ---- module scenarios, simulated at 60 fps ----

static Mascot simulate(const PreviewFrame *f)
{
    Mascot m;
    mascot_init(&m);
    SetRandomSeed(42);                       // same blinks every run
    int steps = (int)(f->seconds / STEP + 0.5f);
    for (int i = 0; i < steps; i++) {
        if (f->drive) f->drive(&m, i * STEP);
        mascot_update(&m, STEP);
    }
    if (f->drive) f->drive(&m, steps * STEP);
    return m;
}

// ---- pointing, crops of the overlay window with a fake control ----

typedef struct {
    const char *label;
    const PointTrip *trip;
    float t;
} PointFrame;

#define T_OUT(p) (SIM_LEAD_IN)
#define T_AT(p) (SIM_LEAD_IN + (p)->out_s)
#define T_RES(p) (SIM_LEAD_IN + (p)->out_s + (p)->at_s)

static int point_frames(PointFrame *f, int cap)
{
    int n = 0;
#define PF(l, trip, time) do { if (n < cap) f[n++] = (PointFrame){ (l), &(trip), (time) }; } while (0)
    const PointTrip *r = &TRIP_RIGHT;
    PF("home", TRIP_RIGHT, 0.2f);
    PF("out: crouch", TRIP_RIGHT, T_OUT(r) + 0.06f);
    PF("out: step", TRIP_RIGHT, T_OUT(r) + 0.25f);
    PF("out: step", TRIP_RIGHT, T_OUT(r) + 0.30f);
    PF("out: mid", TRIP_RIGHT, T_OUT(r) + 0.38f);
    PF("out: easing", TRIP_RIGHT, T_OUT(r) + 0.62f);
    PF("arrive +0.05", TRIP_RIGHT, T_AT(r) + 0.05f);
    PF("arrive +0.10", TRIP_RIGHT, T_AT(r) + 0.10f);
    PF("reach +0.17", TRIP_RIGHT, T_AT(r) + 0.17f);
    PF("reach +0.21", TRIP_RIGHT, T_AT(r) + 0.21f);
    PF("reach +0.26", TRIP_RIGHT, T_AT(r) + 0.26f);
    PF("lock-on +0.32", TRIP_RIGHT, T_AT(r) + 0.32f);
    PF("point R +0.6", TRIP_RIGHT, T_AT(r) + 0.6f);
    PF("point R talking", TRIP_RIGHT, T_AT(r) + 1.33f);
    PF("point far L", TRIP_FAR, T_AT(&TRIP_FAR) + 1.3f);
    PF("point L", TRIP_LEFT, T_AT(&TRIP_LEFT) + 1.3f);
    PF("point up-left (clamped)", TRIP_UPLEFT, T_AT(&TRIP_UPLEFT) + 1.3f);
    PF("point down (clamped)", TRIP_DOWN, T_AT(&TRIP_DOWN) + 1.3f);
    PF("tiny target", TRIP_TINY, T_AT(&TRIP_TINY) + 1.3f);
    PF("huge row", TRIP_HUGE, T_AT(&TRIP_HUGE) + 1.3f);
    PF("click +0.03", TRIP_CLICK, T_RES(&TRIP_CLICK) + 0.03f);
    PF("click +0.10", TRIP_CLICK, T_RES(&TRIP_CLICK) + 0.10f);
    PF("click +0.18", TRIP_CLICK, T_RES(&TRIP_CLICK) + 0.18f);
    PF("click +0.30", TRIP_CLICK, T_RES(&TRIP_CLICK) + 0.30f);
    PF("click +0.45", TRIP_CLICK, T_RES(&TRIP_CLICK) + 0.45f);
    PF("clicked out +0.1", TRIP_CLICK_OUT, T_RES(&TRIP_CLICK_OUT) + 0.1f);
    PF("clicked out +0.3", TRIP_CLICK_OUT, T_RES(&TRIP_CLICK_OUT) + 0.3f);
    PF("moved +0.05", TRIP_MOVED, T_RES(&TRIP_MOVED) + 0.05f);
    PF("moved +0.2", TRIP_MOVED, T_RES(&TRIP_MOVED) + 0.2f);
    PF("click+go +0.06", TRIP_CLICK_GO, T_RES(&TRIP_CLICK_GO) + 0.06f);
    PF("click+go +0.15", TRIP_CLICK_GO, T_RES(&TRIP_CLICK_GO) + 0.15f);
    PF("moved+go +0.05", TRIP_MOVED_GO, T_RES(&TRIP_MOVED_GO) + 0.05f);
    PF("moved+go +0.15", TRIP_MOVED_GO, T_RES(&TRIP_MOVED_GO) + 0.15f);
    PF("dash (fast)", TRIP_FAST, T_OUT(&TRIP_FAST) + 0.31f);
    PF("dash (fast)", TRIP_FAST, T_OUT(&TRIP_FAST) + 0.36f);
    PF("back home (happy)", TRIP_CLICK, T_RES(&TRIP_CLICK) + 0.4f + 0.45f);
    PF("home: settle", TRIP_CLICK, T_RES(&TRIP_CLICK) + 0.4f + 0.9f + 0.08f);
#undef PF
    return n;
}

static void point_simulate(const PointFrame *f, Mascot *m, PointerView *v)
{
    mascot_init(m);
    SetRandomSeed(42);
    int steps = (int)(f->t / STEP + 0.5f);
    Vector2 win;
    for (int i = 0; i <= steps; i++) {
        *v = sim_view(f->trip, i * STEP, &win);
        sim_apply(f->trip, m, v, i * STEP);
        if (i < steps) mascot_update(m, STEP);
    }
}

#define PCOLS 4
#define PCROP_Y 70
#define PCROP_H 500
#define PCELL_W SIM_WIN_W
#define PCELL_H (PCROP_H + 20)

static int point_main(const char *out)
{
    static PointFrame frames[64];
    int count = point_frames(frames, 64);
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(SIM_WIN_W, SIM_WIN_H, "preview");     // point_fx clamps to the window size

    const int rows = (count + PCOLS - 1) / PCOLS;
    const int w = PCOLS * PCELL_W, h = rows * PCELL_H;
    RenderTexture2D win = LoadRenderTexture(SIM_WIN_W, SIM_WIN_H);
    RenderTexture2D sheet = LoadRenderTexture(w, h);
    bool ok = true;

    for (int bg = 0; bg < 2; bg++) {
        const SimLook *L = bg == 0 ? &SIM_DARK : &SIM_LIGHT;
        BeginTextureMode(sheet);
        ClearBackground(L->bg);
        EndTextureMode();
        for (int i = 0; i < count; i++) {
            Mascot m;
            PointerView v;
            point_simulate(&frames[i], &m, &v);
            Vector2 wp;
            sim_view(frames[i].trip, (int)(frames[i].t / STEP + 0.5f) * STEP, &wp);

            BeginTextureMode(win);
            ClearBackground(BLANK);
            sim_draw_desktop(frames[i].trip, L, wp.x, wp.y, SIM_WIN_W, SIM_WIN_H);
            sim_draw_window(&v, &m);
            EndTextureMode();

            int cx = (i % PCOLS) * PCELL_W, cy = (i / PCOLS) * PCELL_H;
            BeginTextureMode(sheet);
            DrawTextureRec(win.texture, (Rectangle){ 0, (float)(SIM_WIN_H - PCROP_Y - PCROP_H), SIM_WIN_W, -(float)PCROP_H },
                           (Vector2){ (float)cx, (float)cy }, WHITE);
            DrawRectangle(cx, cy + PCROP_H, PCELL_W, 20, L->bg);
            DrawRectangle(cx + PCELL_W - 1, cy, 1, PCELL_H, L->dots);
            DrawText(frames[i].label, cx + 8, cy + PCROP_H + 5, 10, bg == 0 ? LIGHTGRAY : DARKGRAY);
            EndTextureMode();
        }
        Image img = LoadImageFromTexture(sheet.texture);
        ImageFlipVertical(&img);
        char path[512];
        if (bg == 0) snprintf(path, sizeof path, "%s", out);
        else snprintf(path, sizeof path, "%.*s_light.png", (int)(strlen(out) - 4), out);
        ok = ExportImage(img, path) && ok;
        if (bg == 0) {      // zoom on the pointing right frame
            char zoom_path[512];
            snprintf(zoom_path, sizeof zoom_path, "%.*s_zoom.png", (int)(strlen(out) - 4), out);
            int zi = 12;
            Image zoom = ImageFromImage(img, (Rectangle){ (float)((zi % PCOLS) * PCELL_W), (float)((zi / PCOLS) * PCELL_H + 140), PCELL_W, 200 });
            ImageResizeNN(&zoom, zoom.width * 3, zoom.height * 3);
            ok = ExportImage(zoom, zoom_path) && ok;
            UnloadImage(zoom);
        }
        UnloadImage(img);
    }
    UnloadRenderTexture(sheet);
    UnloadRenderTexture(win);
    CloseWindow();
    printf("%s %s (%d frames)\n", ok ? "wrote" : "FAILED writing", out, count);
    return ok ? 0 : 1;
}

// ---- driving, wheel_previews scripted drives filmed the same way ----

static void wheel_simulate(const PreviewFrame *f, Mascot *m, DriveSim *ds)
{
    mascot_init(m);
    dsim_init(ds);
    SetRandomSeed(42);
    int steps = (int)(f->seconds / STEP + 0.5f);
    for (int i = 0; i <= steps; i++) {
        f->drive(m, i * STEP);
        dsim_step(ds, m, STEP);
        if (i < steps) mascot_update(m, STEP);
    }
}

static int wheel_main(const char *out)
{
    static PreviewFrame frames[MAX_FRAMES];
    int count = wheel_preview(frames, MAX_FRAMES);
    if (count <= 0) {
        fprintf(stderr, "scenario wheel has no preview frames yet\n");
        return 1;
    }
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(SIM_WIN_W, SIM_WIN_H, "preview");     // point_fx clamps to the window size

    const int rows = (count + PCOLS - 1) / PCOLS;
    const int w = PCOLS * PCELL_W, h = rows * PCELL_H;
    RenderTexture2D win = LoadRenderTexture(SIM_WIN_W, SIM_WIN_H);
    RenderTexture2D sheet = LoadRenderTexture(w, h);
    bool ok = true;
    int zi = 0;                                      // zoom on one of the typing frames
    for (int i = 0; i < count; i++)
        if (strcmp(frames[i].label, "type d") == 0) zi = i;

    for (int bg = 0; bg < 2; bg++) {
        const SimLook *L = bg == 0 ? &SIM_DARK : &SIM_LIGHT;
        BeginTextureMode(sheet);
        ClearBackground(L->bg);
        EndTextureMode();
        for (int i = 0; i < count; i++) {
            Mascot m;
            DriveSim ds;
            wheel_simulate(&frames[i], &m, &ds);

            BeginTextureMode(win);
            ClearBackground(BLANK);
            dsim_draw_desktop(&ds, L, ds.win.x, ds.win.y, SIM_WIN_W, SIM_WIN_H, m.time);
            dsim_draw_window(&ds, &m);
            dsim_draw_pointer(SIM_FACE_X + m.cursor_dx, SIM_FACE_Y + m.cursor_dy);
            EndTextureMode();

            int cx = (i % PCOLS) * PCELL_W, cy = (i / PCOLS) * PCELL_H;
            BeginTextureMode(sheet);
            DrawTextureRec(win.texture, (Rectangle){ 0, (float)(SIM_WIN_H - PCROP_Y - PCROP_H), SIM_WIN_W, -(float)PCROP_H },
                           (Vector2){ (float)cx, (float)cy }, WHITE);
            DrawRectangle(cx, cy + PCROP_H, PCELL_W, 20, L->bg);
            DrawRectangle(cx + PCELL_W - 1, cy, 1, PCELL_H, L->dots);
            DrawText(frames[i].label, cx + 8, cy + PCROP_H + 5, 10, bg == 0 ? LIGHTGRAY : DARKGRAY);
            EndTextureMode();
        }
        Image img = LoadImageFromTexture(sheet.texture);
        ImageFlipVertical(&img);
        char path[512];
        if (bg == 0) snprintf(path, sizeof path, "%s", out);
        else snprintf(path, sizeof path, "%.*s_light.png", (int)(strlen(out) - 4), out);
        ok = ExportImage(img, path) && ok;
        if (bg == 0) {
            char zoom_path[512];
            snprintf(zoom_path, sizeof zoom_path, "%.*s_zoom.png", (int)(strlen(out) - 4), out);
            Image zoom = ImageFromImage(img, (Rectangle){ (float)((zi % PCOLS) * PCELL_W + 90),
                                                          (float)((zi / PCOLS) * PCELL_H + 160), 300, 220 });
            ImageResizeNN(&zoom, zoom.width * 3, zoom.height * 3);
            ok = ExportImage(zoom, zoom_path) && ok;
            UnloadImage(zoom);
        }
        UnloadImage(img);
    }
    UnloadRenderTexture(sheet);
    UnloadRenderTexture(win);
    CloseWindow();
    printf("%s %s (%d frames)\n", ok ? "wrote" : "FAILED writing", out, count);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "preview.png";
    const char *scenario = argc > 2 ? argv[2] : "";
    if (strcmp(scenario, "point") == 0) return point_main(out);
    if (strcmp(scenario, "wheel") == 0) return wheel_main(out);

    PreviewFrame frames[MAX_FRAMES];
    int count = 0;
    if (strcmp(scenario, "emotion") == 0) count = emotion_preview(frames, MAX_FRAMES);
    else if (strcmp(scenario, "gaze") == 0) count = gaze_preview(frames, MAX_FRAMES);
    else if (strcmp(scenario, "sleep") == 0) count = sleep_preview(frames, MAX_FRAMES);
    else if (strcmp(scenario, "physics") == 0) count = physics_preview(frames, MAX_FRAMES);
    else if (strcmp(scenario, "bye") == 0) count = bye_preview(frames, MAX_FRAMES);
    else if (scenario[0]) {
        fprintf(stderr, "unknown scenario %s (emotion, gaze, sleep, physics, point, bye, wheel)\n", scenario);
        return 1;
    }
    bool moods = !scenario[0];
    if (moods) count = (int)(sizeof MOODS / sizeof MOODS[0]);
    if (count <= 0) {
        fprintf(stderr, "scenario %s has no preview frames yet\n", scenario);
        return 1;
    }
    if (count > MAX_FRAMES) count = MAX_FRAMES;

    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(64, 64, "preview");

    const int cols = count < COLUMNS ? count : COLUMNS;
    const int rows = (count + COLUMNS - 1) / COLUMNS;
    const int block_h = rows * CELL_H;
    const int w = cols * CELL_W, h = block_h * 2;
    const Color backgrounds[2] = { { 32, 32, 40, 255 }, { 225, 225, 230, 255 } };
    RenderTexture2D rt = LoadRenderTexture(w, h);

    BeginTextureMode(rt);
    for (int bg = 0; bg < 2; bg++) {
        DrawRectangle(0, bg * block_h, w, block_h, backgrounds[bg]);
        Color label = bg == 0 ? LIGHTGRAY : DARKGRAY;
        for (int i = 0; i < count; i++) {
            int cx = (i % COLUMNS) * CELL_W, cy = bg * block_h + (i / COLUMNS) * CELL_H;
            Mascot m = moods ? mood_frame(i) : simulate(&frames[i]);
            mascot_draw(&m, cx + 12, cy + 8, MASCOT_PX);
            DrawText(moods ? MOODS[i].label : frames[i].label, cx + 12, cy + SPRITE_H + 16, 10, label);
        }
    }
    EndTextureMode();

    Image sheet = LoadImageFromTexture(rt.texture);
    ImageFlipVertical(&sheet);   // render textures come out upside down
    bool ok = ExportImage(sheet, out);

    // first frame at 3x so the actual pixels are easy to see
    char zoom_path[512];
    snprintf(zoom_path, sizeof zoom_path, "%.*s_zoom.png", (int)(strlen(out) - 4), out);
    Image zoom = ImageFromImage(sheet, (Rectangle){ 0, 0, CELL_W, CELL_H - 20 });
    ImageResizeNN(&zoom, zoom.width * 3, zoom.height * 3);
    ok = ExportImage(zoom, zoom_path) && ok;

    UnloadImage(zoom);
    UnloadImage(sheet);
    UnloadRenderTexture(rt);
    CloseWindow();

    printf("%s %s (%d frames)\n", ok ? "wrote" : "FAILED writing", out, count);
    return ok ? 0 : 1;
}
