#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brain.h"
#include "cJSON.h"
#include "platform.h"
#include "raylib.h"

#define LINE_MAX_BYTES (1 << 16)
#define PATH_CAP 1024

#define VENV_PYTHON ".venv/Scripts/python.exe"

static PlatformProcess *proc;
static char line[LINE_MAX_BYTES];
static int line_len;
static bool skipping_long_line;
static bool died_reported;

bool brain_running(void)
{
    return proc != NULL && !died_reported;
}

// the exe's folder without the trailing slash, thats the brain's --home (files go in <home>/data)
static void home_dir(char *home, int cap)
{
    snprintf(home, cap, "%s", GetApplicationDirectory());
    size_t n = strlen(home);
    if (n > 1 && (home[n - 1] == '/' || home[n - 1] == '\\')) home[n - 1] = 0;
}

bool brain_start(char *err, int err_cap)
{
    char app[PATH_CAP], home[PATH_CAP], brain_dir[PATH_CAP] = "", python[PATH_CAP];
    char data_dir[PATH_CAP], stderr_path[PATH_CAP], probe[PATH_CAP];
    snprintf(app, sizeof app, "%s", GetApplicationDirectory());
    home_dir(home, sizeof home);

    // brain/ ships next to the exe, in dev the exe is in build/
    const char *candidates[] = { "brain", "../brain" };
    for (int i = 0; i < 2 && !brain_dir[0]; i++) {
        snprintf(probe, sizeof probe, "%s%s/manikin/__main__.py", app, candidates[i]);
        if (FileExists(probe)) snprintf(brain_dir, sizeof brain_dir, "%s%s", app, candidates[i]);
    }
    if (!brain_dir[0]) {
        snprintf(err, err_cap, "I can't find my brain folder next to the app.");
        return false;
    }

    const char *override = getenv("MANIKIN_PYTHON");
    if (override && override[0]) snprintf(python, sizeof python, "%s", override);
    else snprintf(python, sizeof python, "%s/%s", brain_dir, VENV_PYTHON);
    if ((!override || !override[0]) && !FileExists(python)) {
        snprintf(err, err_cap, "My brain has no Python environment yet. Run: make setup");
        return false;
    }

    snprintf(data_dir, sizeof data_dir, "%s/data", home);
    MakeDirectory(data_dir);
    snprintf(stderr_path, sizeof stderr_path, "%s/brain_stderr.log", data_dir);

    const char *backend = getenv("MANIKIN_BACKEND");   // "echo" to test without the API
    const char *argv[] = { python, "-m", "manikin", "--home", home,
                           backend ? "--backend" : NULL, backend, NULL };
    proc = platform_spawn(argv, brain_dir, stderr_path);
    if (!proc) {
        snprintf(err, err_cap, "My brain wouldn't start. Check data/brain_stderr.log.");
        return false;
    }
    line_len = 0;
    skipping_long_line = false;
    died_reported = false;
    return true;
}

void brain_stop(void)
{
    platform_proc_close(proc);
    proc = NULL;
}

static void copy_str(char *dst, int cap, const cJSON *item)
{
    const char *s = cJSON_IsString(item) ? item->valuestring : "";
    snprintf(dst, cap, "%s", s);
    int len = (int)strlen(s);
    if (len >= cap && cap > 4) {         // keep it obvious it was cut
        int at = cap - 4;
        while (at > 0 && ((unsigned char)dst[at] & 0xC0) == 0x80) at--;   // dont cut a utf-8 char in half
        memcpy(dst + at, "...", 4);
    }
}

static int int_of(const cJSON *item)
{
    return cJSON_IsNumber(item) ? (int)lround(item->valuedouble) : 0;
}

// same rules as config.Settings in the brain, wrong types keep the default, volume gets clamped
static BrainSound sound_of(const cJSON *obj)
{
    BrainSound s = { 30, false, true };
    const cJSON *volume = cJSON_GetObjectItemCaseSensitive(obj, "volume");
    const cJSON *muted = cJSON_GetObjectItemCaseSensitive(obj, "muted");
    const cJSON *voice = cJSON_GetObjectItemCaseSensitive(obj, "voice");
    if (cJSON_IsNumber(volume)) {
        int v = int_of(volume);
        s.volume = v < 0 ? 0 : v > 100 ? 100 : v;
    }
    if (cJSON_IsBool(muted)) s.muted = cJSON_IsTrue(muted);
    if (cJSON_IsBool(voice)) s.voice = cJSON_IsTrue(voice);
    return s;
}

BrainSound brain_saved_sound(void)
{
    char home[PATH_CAP], path[PATH_CAP];
    home_dir(home, sizeof home);
    snprintf(path, sizeof path, "%s/data/settings.json", home);
    char *text = FileExists(path) ? LoadFileText(path) : NULL;
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    BrainSound s = sound_of(cJSON_GetObjectItemCaseSensitive(root, "sound"));
    cJSON_Delete(root);
    if (text) UnloadFileText(text);
    return s;
}

// names too long for KEY_NAMES get skipped (platform_keys would refuse them anyway)
static int key_names(const cJSON *arr, char names[][BRAIN_KEY_NAME_MAX])
{
    int n = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (n == BRAIN_DRIVE_KEYS_MAX) break;
        if (!cJSON_IsString(item) || strlen(item->valuestring) >= BRAIN_KEY_NAME_MAX) continue;
        snprintf(names[n++], BRAIN_KEY_NAME_MAX, "%s", item->valuestring);
    }
    return n;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void parse_drive(const cJSON *msg, BrainDrive *d)
{
    static const char *const DO[] = { "move", "click", "drag", "scroll", "type", "key" };
    const cJSON *what = cJSON_GetObjectItemCaseSensitive(msg, "do");
    d->action = BRAIN_DRIVE_NONE;
    for (int i = 0; cJSON_IsString(what) && i < (int)(sizeof DO / sizeof DO[0]); i++)
        if (strcmp(what->valuestring, DO[i]) == 0) d->action = (BrainDriveDo)(BRAIN_DRIVE_MOVE + i);
    d->x = int_of(cJSON_GetObjectItemCaseSensitive(msg, "x"));
    d->y = int_of(cJSON_GetObjectItemCaseSensitive(msg, "y"));
    d->to_x = int_of(cJSON_GetObjectItemCaseSensitive(msg, "to_x"));
    d->to_y = int_of(cJSON_GetObjectItemCaseSensitive(msg, "to_y"));
    const cJSON *button = cJSON_GetObjectItemCaseSensitive(msg, "button");
    d->right = cJSON_IsString(button) && strcmp(button->valuestring, "right") == 0;
    const cJSON *count = cJSON_GetObjectItemCaseSensitive(msg, "count");
    d->count = cJSON_IsNumber(count) ? clampi(int_of(count), 1, 3) : 1;
    d->dx = int_of(cJSON_GetObjectItemCaseSensitive(msg, "dx"));
    d->dy = int_of(cJSON_GetObjectItemCaseSensitive(msg, "dy"));
    const cJSON *repeat = cJSON_GetObjectItemCaseSensitive(msg, "repeat");
    d->repeat = cJSON_IsNumber(repeat) ? clampi(int_of(repeat), 1, 100) : 1;
    d->nkeys = key_names(cJSON_GetObjectItemCaseSensitive(msg, d->action == BRAIN_DRIVE_KEY ? "keys" : "mods"),
                         d->keys);
}

static bool parse_event(const char *text, BrainEvent *ev)
{
    static const struct { const char *name; BrainEventType type; } TYPES[] = {
        { "password", BRAIN_EV_PASSWORD }, { "unlocked", BRAIN_EV_UNLOCKED },
        { "ready", BRAIN_EV_READY },       { "thinking", BRAIN_EV_THINKING },
        { "ask", BRAIN_EV_ASK },
        { "ask_cancel", BRAIN_EV_ASK_CANCEL }, { "say", BRAIN_EV_SAY },
        { "remind", BRAIN_EV_REMIND },     { "error", BRAIN_EV_ERROR },
        { "status", BRAIN_EV_STATUS },     { "cancelled", BRAIN_EV_CANCELLED },
        { "point", BRAIN_EV_POINT },       { "point_clear", BRAIN_EV_POINT_CLEAR },
        { "sound", BRAIN_EV_SOUND },       { "drive", BRAIN_EV_DRIVE },
        { "drive_end", BRAIN_EV_DRIVE_END }, { "narrate", BRAIN_EV_NARRATE },
    };
    cJSON *msg = cJSON_Parse(text);
    if (!msg) return false;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(msg, "type");
    bool known = false;
    memset(ev, 0, sizeof *ev);
    for (size_t i = 0; cJSON_IsString(type) && i < sizeof TYPES / sizeof TYPES[0]; i++) {
        if (strcmp(type->valuestring, TYPES[i].name) == 0) {
            ev->type = TYPES[i].type;
            known = true;
            break;
        }
    }
    if (known) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
        ev->id = cJSON_IsNumber(id) ? id->valueint : 0;
        ev->ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(msg, "ok"));
        ev->proactive = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(msg, "proactive"));
        ev->session = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(msg, "session"));
        copy_str(ev->mode, sizeof ev->mode, cJSON_GetObjectItemCaseSensitive(msg, "mode"));
        copy_str(ev->mood, sizeof ev->mood, cJSON_GetObjectItemCaseSensitive(msg, "mood"));
        const cJSON *body = cJSON_GetObjectItemCaseSensitive(msg, ev->type == BRAIN_EV_PASSWORD ? "error"
                                                             : ev->type == BRAIN_EV_DRIVE_END ? "result" : "text");
        copy_str(ev->text, sizeof ev->text, body);
        if (ev->type == BRAIN_EV_POINT) {
            ev->x = int_of(cJSON_GetObjectItemCaseSensitive(msg, "x"));
            ev->y = int_of(cJSON_GetObjectItemCaseSensitive(msg, "y"));
            const cJSON *box = cJSON_GetObjectItemCaseSensitive(msg, "box");
            for (int i = 0; i < 4; i++) ev->box[i] = int_of(cJSON_GetArrayItem(box, i));
            // a HWND can be wider than an int, valueint would clip it
            const cJSON *win = cJSON_GetObjectItemCaseSensitive(msg, "window");
            ev->window = cJSON_IsNumber(win) ? (long long)win->valuedouble : 0;
        }
        if (ev->type == BRAIN_EV_SOUND) ev->sound = sound_of(msg);
        if (ev->type == BRAIN_EV_DRIVE) parse_drive(msg, &ev->drive);
    }
    cJSON_Delete(msg);
    return known;
}

bool brain_poll(BrainEvent *ev)
{
    if (!proc) return false;
    for (;;) {
        char *nl = memchr(line, '\n', line_len);
        if (nl) {
            *nl = 0;
            int used = (int)(nl - line) + 1;
            bool got = !skipping_long_line && parse_event(line, ev);
            skipping_long_line = false;
            memmove(line, line + used, line_len - used);
            line_len -= used;
            if (got) return true;
            continue;
        }
        if (line_len == LINE_MAX_BYTES) {      // way too long, drop it
            skipping_long_line = true;
            line_len = 0;
        }
        int got = platform_proc_read(proc, line + line_len, LINE_MAX_BYTES - line_len);
        if (got > 0) {
            line_len += got;
            continue;
        }
        if ((got < 0 || !platform_proc_alive(proc)) && !died_reported) {
            died_reported = true;
            memset(ev, 0, sizeof *ev);
            ev->type = BRAIN_EV_DIED;
            return true;
        }
        return false;
    }
}

static void send_json(cJSON *msg)
{
    char *text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!text) return;
    if (proc && !died_reported) {
        platform_proc_write(proc, text, (int)strlen(text));
        platform_proc_write(proc, "\n", 1);
    }
    cJSON_free(text);
}

void brain_send_user(const char *text)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "user");
    cJSON_AddStringToObject(msg, "text", text);
    send_json(msg);
}

void brain_send_answer(int id, bool allow)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "answer");
    cJSON_AddNumberToObject(msg, "id", id);
    cJSON_AddBoolToObject(msg, "allow", allow);
    send_json(msg);
}

void brain_send_answer_session(int id)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "answer");
    cJSON_AddNumberToObject(msg, "id", id);
    cJSON_AddBoolToObject(msg, "allow", true);
    cJSON_AddStringToObject(msg, "scope", "session");
    send_json(msg);
}

void brain_send_password(const char *password)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "password");
    cJSON_AddStringToObject(msg, "text", password);
    // cJSON keeps its own copy, wipe that too before its freed
    char *text = cJSON_PrintUnformatted(msg);
    cJSON *field = cJSON_GetObjectItemCaseSensitive(msg, "text");
    if (field && field->valuestring) memset(field->valuestring, 0, strlen(field->valuestring));
    cJSON_Delete(msg);
    if (!text) return;
    if (proc && !died_reported) {
        platform_proc_write(proc, text, (int)strlen(text));
        platform_proc_write(proc, "\n", 1);
    }
    memset(text, 0, strlen(text));
    cJSON_free(text);
}

void brain_send_password_skip(void)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "password");
    cJSON_AddBoolToObject(msg, "skip", true);
    send_json(msg);
}

void brain_send_cancel(void)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "cancel");
    send_json(msg);
}

void brain_send_presence(float pet_idle, float pc_idle, bool typing, bool asleep)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "presence");
    // whole secs is plenty for "is now a good moment" and keeps the line short
    cJSON_AddNumberToObject(msg, "pet_idle", floor(pet_idle));
    cJSON_AddNumberToObject(msg, "pc_idle", pc_idle < 0 ? -1.0 : floor(pc_idle));
    cJSON_AddBoolToObject(msg, "typing", typing);
    cJSON_AddBoolToObject(msg, "asleep", asleep);
    send_json(msg);
}

void brain_send_woke(float slept_s)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "event");
    cJSON_AddStringToObject(msg, "name", "woke");
    cJSON_AddNumberToObject(msg, "slept_s", floor(slept_s));
    send_json(msg);
}

void brain_send_point_done(int id, const char *outcome, int x, int y, bool inside)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "point_done");
    cJSON_AddNumberToObject(msg, "id", id);
    cJSON_AddStringToObject(msg, "outcome", outcome);
    cJSON_AddNumberToObject(msg, "x", x);
    cJSON_AddNumberToObject(msg, "y", y);
    cJSON_AddBoolToObject(msg, "inside", inside);
    send_json(msg);
}

void brain_send_sound_set(bool muted)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "sound_set");
    cJSON_AddBoolToObject(msg, "muted", muted);
    send_json(msg);
}

void brain_send_drive_done(int id, const char *outcome, int x, int y)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "drive_done");
    cJSON_AddNumberToObject(msg, "id", id);
    cJSON_AddStringToObject(msg, "outcome", outcome);
    cJSON_AddNumberToObject(msg, "x", x);
    cJSON_AddNumberToObject(msg, "y", y);
    send_json(msg);
}
