// manikin, a pixel art pet that sits on top of every window. this overlay is his body,
// the python brain (brain/) does the thinking and runs the tools

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#include "brain.h"
#include "chat.h"
#include "cues.h"
#include "drive.h"
#include "icon.h"
#include "mascot.h"
#include "platform.h"
#include "point_fx.h"
#include "pointer.h"
#include "sfx.h"
#include "speech.h"
#include "ui.h"

#define WIN_W 420
#define WIN_H 582                    // bottom 72px is room for the chat box to grow to 4 lines

#define MASCOT_X ((WIN_W - MASCOT_W * MASCOT_PX) / 2)
#define MASCOT_Y 270
#define BUBBLE_BOTTOM 256            // bottom edge of the bubble box, the tail hangs below
#define CHAT_Y (MASCOT_Y + MASCOT_H * MASCOT_PX + 6)
#define CHAT_MAX_H (48 + 72)         // chat box at its full 4 lines
#define ARROW_GAP 10

#define FACE_X (MASCOT_X + 20 * MASCOT_PX)   // middle of his screen face
#define FACE_Y (MASCOT_Y + 16 * MASCOT_PX)
#define ARM_Y (MASCOT_Y + 25 * MASCOT_PX)    // where the cable arm comes out
#define ARM_REACH 140                // how far to his side the thing hes pointing at should sit

#define DRAG_THRESHOLD 4             // px of movement before a click turns into a drag
#define ANSWER_QUIET 0.3f            // secs after his own input before a click on him counts as yours

static Mascot mascot;

#define PRESENCE_EVERY 5.0
#define HUSH_CHECK_EVERY 1.0

static bool brain_ok;
static bool brain_ready;             // past the password prompt, taking chat
static int pending;                  // messages sent that havent been answered yet
static bool pw_active, pw_wait;      // password prompt is up / waiting for the brain to check it
static char pw_mode[16];
static bool ask_active;
static int ask_id;
static float happy_timer;
static BrainSound sound;

// the point's line ("Click this one") stays in the bubble while hes standing there, but a
// reply or a prompt can take the bubble and the line comes back once its free again
static char point_text[BRAIN_TEXT_MAX];
static bool point_text_up;           // its the sticky line in the bubble right now

static void point_text_yield(void)
{
    if (!point_text_up) return;
    speech_hide();
    point_text_up = false;
}

static void point_text_update(const PointerView *v)
{
    bool want = v->phase == POINTER_AT && point_text[0];
    if (!want) {
        point_text_yield();
    } else if (!point_text_up && !speech_sticky() && (!speech_busy() || speech_thinking())) {
        speech_prompt(point_text, UI_BUTTONS_NONE);   // goes over the thinking dots if theyre up
        point_text_up = true;
    }
}

static const char *password_prompt(const char *mode)
{
    if (strcmp(mode, "create") == 0)
        return "Let's lock my memory. Pick a password (6+ characters). Esc skips.";
    if (strcmp(mode, "confirm") == 0)
        return "One more time, so I know you mean it.";
    return "Password, please! I'm not looking. (Esc skips)";
}

// pan his sounds by where he stands on his monitor (by the whole desktop hed sit hard left
// or right with two screens), only 60% of the way since hes one small pet. -1 left .. 1 right
static float his_pan(void)
{
    Vector2 here = GetWindowPosition();
    int sx, sy, sw, sh;
    platform_get_work_area_at((int)here.x + FACE_X, (int)here.y + FACE_Y, &sx, &sy, &sw, &sh);
    return sw > 0 ? 0.6f * (2.0f * (here.x + FACE_X - sx) / sw - 1.0f) : 0.0f;
}

static void toggle_mute(void)
{
    sound.muted = !sound.muted;
    sfx_set(sound.volume, sound.muted, sound.voice);
    if (brain_ok) brain_send_sound_set(sound.muted);
    if (!sound.muted) cues_play(SFX_SOUND_ON);
    speech_say(sound.muted ? "Shh... I'll be quiet. Ctrl+M brings my sound back." : "Sound's back on!",
               EMOTION_NONE, false);
}

static void handle_event(const BrainEvent *ev, bool *chat_open)
{
    char text[BRAIN_TEXT_MAX + 64];
    switch (ev->type) {                  // anything that talks gets the bubble first
    case BRAIN_EV_PASSWORD: case BRAIN_EV_UNLOCKED: case BRAIN_EV_ASK: case BRAIN_EV_SAY:
    case BRAIN_EV_ERROR: case BRAIN_EV_REMIND: case BRAIN_EV_DIED: case BRAIN_EV_NARRATE:
        point_text_yield();
        break;
    default:
        break;
    }
    switch (ev->type) {
    case BRAIN_EV_PASSWORD:
        speech_interrupt();
        pw_active = true;
        pw_wait = false;
        snprintf(pw_mode, sizeof pw_mode, "%s", ev->mode);
        snprintf(text, sizeof text, "%s%s%s", ev->text, ev->text[0] ? " " : "", password_prompt(ev->mode));
        speech_prompt(text, UI_BUTTONS_NONE);
        cues_play(ev->text[0] ? SFX_WRONG : SFX_LOCK);
        *chat_open = true;
        break;
    case BRAIN_EV_UNLOCKED:
        pw_active = pw_wait = false;
        speech_hide();
        if (ev->ok) {
            happy_timer = 2.0f;
            speech_say("Memory unlocked!", EMOTION_NONE, false);   // the brain says hi right after
            cues_lead(SFX_UNLOCK);
        } else {
            speech_say("Okay, no memory this time. I'll forget everything when you close me.", EMOTION_NONE, false);
        }
        break;
    case BRAIN_EV_READY:
        brain_ready = true;
        break;
    case BRAIN_EV_ASK:
        speech_interrupt();
        ask_active = true;
        ask_id = ev->id;
        speech_prompt(ev->text, ev->session ? UI_BUTTONS_SESSION : UI_BUTTONS_YES_NO);
        cues_play(SFX_ASK);
        break;
    case BRAIN_EV_ASK_CANCEL:
        if (ask_active && ask_id == ev->id) {
            ask_active = false;
            speech_hide();
        }
        break;
    case BRAIN_EV_SAY:
        if (ev->proactive) {
            // not a reply so it doesnt touch pending or the status label. if youre typing
            // to him he doesnt talk over you, the line just gets dropped
            if (!(*chat_open && chat_len() > 0)) {
                speech_say(ev->text, emotion_from_name(ev->mood), false);
                cues_lead(SFX_HEY);      // "hey!" so you notice without staring at him
            }
            break;
        }
        if (pending > 0) pending--;
        speech_set_status(NULL);         // the turn is over, dont let the label hang into the next one
        drive_turn_over();               // drive_end normally came first, this is in case it didnt
        speech_say(ev->text, emotion_from_name(ev->mood), false);
        break;
    case BRAIN_EV_NARRATE:               // a passing line while he drives, the turn isnt over
        speech_narrate(ev->text, emotion_from_name(ev->mood));
        break;
    case BRAIN_EV_ERROR:
        if (pending > 0) pending--;
        speech_set_status(NULL);
        drive_turn_over();
        speech_say(ev->text, EMOTION_NONE, false);
        cues_lead(SFX_ERROR);
        break;
    case BRAIN_EV_STATUS:
        speech_set_status(ev->text[0] ? ev->text : NULL);
        break;
    case BRAIN_EV_CANCELLED:
        pending = 0;
        speech_set_status(NULL);
        drive_turn_over();
        break;
    case BRAIN_EV_REMIND:
        speech_say(ev->text, EMOTION_EXCITED, false);
        cues_lead(SFX_REMIND);
        break;
    case BRAIN_EV_POINT:
        snprintf(point_text, sizeof point_text, "%s", ev->text);
        pointer_point(ev->id, ev->x, ev->y,
                      (Rectangle){ (float)ev->box[0], (float)ev->box[1], (float)ev->box[2], (float)ev->box[3] },
                      ev->window);
        break;
    case BRAIN_EV_POINT_CLEAR:
        pointer_clear(ev->id);
        break;
    case BRAIN_EV_DRIVE:
        drive_action(ev);
        break;
    case BRAIN_EV_DRIVE_END:
        drive_end(ev->text);
        break;
    case BRAIN_EV_SOUND: {
        bool unmuted = sound.muted && !ev->sound.muted;   // you asked him to ("sound back on")
        sound = ev->sound;
        sfx_set(sound.volume, sound.muted, sound.voice);
        if (unmuted) cues_play(SFX_SOUND_ON);
        break;
    }
    case BRAIN_EV_DIED:
        brain_ok = brain_ready = false;
        pending = 0;
        pw_active = pw_wait = ask_active = false;
        pointer_clear_active();          // nobody left to point for
        drive_turn_over();               // ...or drive for
        speech_set_status(NULL);
        if (speech_sticky()) speech_hide();
        speech_say("My brain stopped working (see data/brain.log). I'll just echo you for now.", EMOTION_NONE, false);
        cues_lead(SFX_BRAIN_DIED);
        break;
    default:                             // thinking, pending already covers it
        break;
    }
}

// Ctrl+Q or the window closing. he waves while "Bye!" types out then switches off like an
// old CRT, and only then does the window close. clicks go through to the desktop meanwhile
static void goodbye(void *hwnd)
{
    drive_end("failed");                 // let go of anything hes holding down
    mascot.drive_in = (DriveInputs){ 0 };
    platform_set_click_through(hwnd, true);
    point_text_yield();
    speech_prompt("Bye!", UI_BUTTONS_NONE);
    mascot.leaving = true;
    PointerView pv = pointer_view();     // he stays wherever he is, no walking home
    while (!bye_done(&mascot)) {
        float dt = GetFrameTime();
        speech_update(dt, false, (Vector2){ -1000.0f, -1000.0f }, 0.0f);
        mascot.mood = MOOD_IDLE;
        mascot.held = false;
        mascot.vel_x = mascot.vel_y = 0.0f;
        mascot.idle_for = 0.0f;
        mascot_update(&mascot, dt);
        if (mascot.ev.bits & MEV_SWITCH_OFF) speech_hide();   // the bubble goes off with the power
        sfx_update(dt);
        cues_frame(&mascot, speech_events(), his_pan(), false);

        BeginDrawing();
        ClearBackground(BLANK);
        float t = (float)GetTime();
        bool on = mascot.bye.t < BYE_WAVE;   // a cable still out goes off with it too
        if (on) point_fx_draw_behind(&pv, &mascot, MASCOT_X, MASCOT_Y, MASCOT_PX, t);
        mascot_draw(&mascot, MASCOT_X, MASCOT_Y, MASCOT_PX);
        if (on) point_fx_draw(&pv, &mascot, MASCOT_X, MASCOT_Y, MASCOT_PX, t);
        speech_draw(UI_HOVER_NONE);
        EndDrawing();
    }
}

int main(void)
{
    SetConfigFlags(FLAG_WINDOW_TRANSPARENT | FLAG_WINDOW_UNDECORATED |
                   FLAG_WINDOW_TOPMOST | FLAG_WINDOW_HIDDEN);
    InitWindow(WIN_W, WIN_H, "Manikin");
    SetExitKey(KEY_NULL);            // Esc is for the chat box, it shouldnt quit the app
    SetTargetFPS(60);

    // his face for the taskbar and alt+tab at the exact sizes windows uses, scaling blurs his pixels
    {
        int sizes[8], n = platform_icon_sizes(GetWindowHandle(), sizes, 8);
        Image icons[8];
        for (int i = 0; i < n; i++) icons[i] = icon_image(sizes[i]);
        SetWindowIcons(icons, n);
        for (int i = 0; i < n; i++) UnloadImage(icons[i]);
    }

    // saved settings first so a muted pet stays muted from the very first sound
    sound = brain_saved_sound();
    sfx_set(sound.volume, sound.muted, sound.voice);
    sfx_init();

    // park him in the bottom right corner, just above the taskbar
    int ax, ay, aw, ah;
    platform_get_work_area(&ax, &ay, &aw, &ah);
    SetWindowPosition(ax + aw - WIN_W - 16, ay + ah - WIN_H);
    ClearWindowState(FLAG_WINDOW_HIDDEN);

    void *hwnd = GetWindowHandle();
    platform_make_overlay(hwnd);
    bool click_through = false;
    bool no_activate = false;

    // what pointing has to keep clear of, each at its biggest (him, widest bubble, tallest chat
    // box, the arrow in both its spots). whatever he points at goes in the gaps around them
    PointerLayout layout = { WIN_W, WIN_H };
    layout.solid[layout.solid_count++] = (Rectangle){ MASCOT_X + 4 * MASCOT_PX, MASCOT_Y,
                                                      (MASCOT_W - 8) * MASCOT_PX, (MASCOT_H - 2) * MASCOT_PX };
    layout.solid[layout.solid_count++] = (Rectangle){ WIN_W / 2 - UI_BUBBLE_MAX_W / 2 - UI_PX, 0,
                                                      UI_BUBBLE_MAX_W + 2 * UI_PX, BUBBLE_BOTTOM + 5 * UI_PX };
    layout.solid[layout.solid_count++] = (Rectangle){ 20 - UI_PX, CHAT_Y - UI_PX,
                                                      WIN_W - 40 + 2 * UI_PX, CHAT_MAX_H + 2 * UI_PX };
    layout.solid[layout.solid_count++] = ui_arrow_rect(WIN_W / 2.0f, (float)(CHAT_Y + UI_PX));
    layout.solid[layout.solid_count++] = ui_arrow_rect(WIN_W / 2.0f, (float)(CHAT_Y + CHAT_MAX_H + ARROW_GAP));
    layout.spot_left = (Vector2){ FACE_X - ARM_REACH, ARM_Y };
    layout.spot_right = (Vector2){ FACE_X + ARM_REACH, ARM_Y };
    Vector2 start = GetWindowPosition();
    pointer_init(&layout, platform_window_id(hwnd), (int)start.x, (int)start.y);
    drive_init(&layout, platform_window_id(hwnd), (int)start.x, (int)start.y);

    speech_init(WIN_W / 2.0f, BUBBLE_BOTTOM);

    char err[256];
    brain_ok = brain_start(err, sizeof err);
    if (!brain_ok) speech_say(err, EMOTION_NONE, false);

    mascot_init(&mascot);
    int last_gx = 0, last_gy = 0;

    bool chat_open = false;
    bool pressing_mascot = false, dragging = false;
    int press_x = 0, press_y = 0, grab_x = 0, grab_y = 0;
    int cursor = MOUSE_CURSOR_DEFAULT;
    bool quit = false;

    // for presence. unlike mascot.idle_for, pet_idle only resets when the user touches him
    float pet_idle = 0.0f;
    double next_presence = 0.0;          // 0 = not scheduled till the brain is ready
    bool was_asleep = false;
    double asleep_since = 0.0;
    double next_hush_check = 0.0;
    bool booted = false;

    while (!WindowShouldClose() && !quit) {
        float dt = GetFrameTime();
        double now = GetTime();

        BrainEvent ev;
        while (brain_ok && brain_poll(&ev)) handle_event(&ev, &chat_open);
        bool typing_password = pw_active && !pw_wait;
        if (typing_password) chat_open = true;

        // ask windows where the cursor is, a click-through window gets no mouse events
        int gx, gy;
        platform_get_cursor_screen(&gx, &gy);
        Vector2 wpos = GetWindowPosition();
        Vector2 mouse = { (float)gx - wpos.x, (float)gy - wpos.y };

        Rectangle mascot_r = { MASCOT_X + 4 * MASCOT_PX, MASCOT_Y, (MASCOT_W - 8) * MASCOT_PX, (MASCOT_H - 2) * MASCOT_PX };
        Rectangle chat_r = chat_rect(20, CHAT_Y, WIN_W - 40);
        float arrow_y = chat_open ? chat_r.y + chat_r.height + ARROW_GAP : (float)(CHAT_Y + UI_PX);
        Rectangle arrow_r = ui_arrow_rect(WIN_W / 2.0f, arrow_y);
        Rectangle bubble_r = speech_rect(), yes_r = { 0 }, session_r = { 0 }, no_r = { 0 };
        bool buttons_up = speech_buttons(&yes_r, &session_r, &no_r);

        // while he steers the pointer is his, none of his UI catches it
        bool steering = drive_steering();
        bool over_mascot = !steering && CheckCollisionPointRec(mouse, mascot_r);
        bool over_arrow = !steering && !typing_password && CheckCollisionPointRec(mouse, arrow_r);
        bool over_chat = !steering && chat_open && CheckCollisionPointRec(mouse, chat_r);
        bool over_bubble = !steering && speech_busy() && CheckCollisionPointRec(mouse, bubble_r);
        UiHover hover = !buttons_up || steering ? UI_HOVER_NONE
                        : CheckCollisionPointRec(mouse, yes_r) ? UI_HOVER_YES
                        : CheckCollisionPointRec(mouse, session_r) ? UI_HOVER_SESSION
                        : CheckCollisionPointRec(mouse, no_r) ? UI_HOVER_NO : UI_HOVER_NONE;

        // only the drawn parts catch the mouse, empty space clicks through to the desktop.
        // while he steers all of him clicks through so his own clicks never land on his UI
        bool solid = !steering && (pressing_mascot || over_mascot || over_arrow || over_chat || over_bubble);
        if (solid == click_through) {
            click_through = !solid;
            platform_set_click_through(hwnd, click_through);
        }
        // while he drives, clicking his bubble leaves keyboard focus in the app hes working in
        if (drive_driving() != no_activate) {
            no_activate = !no_activate;
            platform_set_no_activate(hwnd, no_activate);
        }

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (hover != UI_HOVER_NONE) {
                // a click he might have sent himself doesnt answer anything
                if (!drive_injected_within(ANSWER_QUIET)) {
                    if (hover == UI_HOVER_SESSION) brain_send_answer_session(ask_id);
                    else brain_send_answer(ask_id, hover == UI_HOVER_YES);
                    cues_play(hover == UI_HOVER_NO ? SFX_NO : SFX_YES);
                    ask_active = false;
                    speech_hide();
                }
            } else if (over_arrow) {
                chat_open = !chat_open;
            } else if (over_bubble && !speech_sticky()) {
                speech_click();
            } else if (over_chat) {
                chat_click(chat_r, mouse);
            } else if (over_mascot) {
                pressing_mascot = true;
                press_x = gx;
                press_y = gy;
                grab_x = gx - (int)wpos.x;
                grab_y = gy - (int)wpos.y;
            }
        }
        if (pressing_mascot) {
            if (!dragging && (abs(gx - press_x) > DRAG_THRESHOLD || abs(gy - press_y) > DRAG_THRESHOLD))
                dragging = true;
            if (dragging) SetWindowPosition(gx - grab_x, gy - grab_y);
            if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                if (!dragging) cues_poke();
                if (!dragging && !typing_password) chat_open = !chat_open;
                pressing_mascot = dragging = false;
            }
        }

        // every frame, so a press made while he walks gets used up then and not later
        bool pressed_anywhere = platform_mouse_pressed();

        bool can_type = chat_open && !pw_wait;
        ChatAction action = chat_update(can_type, typing_password);
        bool interacted = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || pressing_mascot || chat_interacted();

        if (action == CHAT_QUIT) {
            quit = true;
        } else if (action == CHAT_TOGGLE_MUTE) {
            toggle_mute();
        } else if (typing_password) {
            if (action == CHAT_SUBMIT) {
                brain_send_password(chat_text());
                cues_play(SFX_SUBMIT);
                pw_wait = true;
                speech_hide();
            } else if (action == CHAT_ESCAPE) {
                brain_send_password_skip();
                pw_wait = true;
                speech_hide();
            }
            if (pw_wait) chat_clear();          // dont leave the password lying around
        } else if (action == CHAT_SUBMIT) {
            speech_dismiss();
            cues_play(SFX_SUBMIT);
            if (brain_ok && brain_ready) {
                brain_send_user(chat_text());
                pending++;
            } else if (brain_ok) {
                speech_say("Hang on, I'm still waking up.", EMOTION_NONE, false);
            } else {
                speech_say(chat_text(), EMOTION_NONE, false);   // no brain, just echo
            }
            chat_clear();
        } else if (action == CHAT_ESCAPE && drive_away()) {
            drive_cancel();                     // on his way home its already over
        } else if (action == CHAT_ESCAPE && pending > 0) {
            // stop waiting, the chat box stays open to retry and the dots go once "cancelled" comes back
            brain_send_cancel();
            speech_set_status(NULL);
            cues_play(SFX_CANCEL);
        } else if (action == CHAT_ESCAPE && pointer_pointing()) {
            pointer_dismiss();                  // "never mind", he walks home
        } else if (action == CHAT_ESCAPE && can_type) {
            chat_open = false;
        }

        Vector2 now_pos = GetWindowPosition();  // a drag might have moved it this frame
        PointerFrame pf = { 0 };
        pf.win_x = (int)now_pos.x;
        pf.win_y = (int)now_pos.y;
        pf.cursor_x = gx;
        pf.cursor_y = gy;
        // clicking an answer button isnt a poke, and neither is a click he sent himself
        pf.pressed = pressed_anywhere && hover == UI_HOVER_NONE && !drive_injected_within(ANSWER_QUIET);
        pf.on_ui = over_mascot || over_arrow || over_chat || over_bubble;
        pf.held = pressing_mascot && !dragging;
        pf.dragging = dragging;
        pf.keep_chat = typing_password;
        pf.chat_open = chat_open;
        pointer_update(dt, &pf);
        chat_open = pf.chat_open;
        if (pf.move) SetWindowPosition(pf.win_x, pf.win_y);
        PointerView pv = pointer_view();

        DriveFrame df = { 0 };
        df.win_x = pf.win_x;
        df.win_y = pf.win_y;
        df.pressed = pressed_anywhere;
        df.held = pressing_mascot && !dragging;
        df.dragging = dragging;
        df.asking = ask_active;
        df.pointer_away = pointer_away();     // a handover point has the window
        df.chat_open = chat_open;
        drive_update(dt, &df);
        chat_open = df.chat_open;
        if (df.move) SetWindowPosition(df.win_x, df.win_y);
        if (df.cancelled) {                   // Esc, from anywhere
            speech_set_status(NULL);
            cues_play(SFX_CANCEL);
        }
        if (drive_steering() && !click_through) {   // he took hold this frame
            click_through = true;
            platform_set_click_through(hwnd, true);
        }
        DriveView dv = drive_view();

        bool waiting = pending > 0 || pw_wait;
        float wheel = drive_injected_within(ANSWER_QUIET) ? 0.0f : GetMouseWheelMove();   // ignore his own scrolling
        speech_update(dt, waiting, mouse, wheel);
        if (over_mascot && !speech_busy()) speech_recall(wheel);   // scroll on him to see his last line again
        point_text_update(&pv);
        Emotion emotion;
        if (speech_emotion_changed(&emotion)) {
            mascot.emotion = emotion;
            mascot.emotion_age = 0;
        }
        if (happy_timer > 0) happy_timer -= dt;

        mascot.mood = typing_password ? MOOD_SHY
                    : ask_active ? MOOD_ASK
                    : happy_timer > 0 ? MOOD_HAPPY
                    : speech_talking() ? MOOD_TALKING
                    : speech_thinking() ? MOOD_THINKING : MOOD_IDLE;
        mascot.typed = typing_password ? chat_len() : 0;

        mascot.cursor_dx = mouse.x - FACE_X;
        mascot.cursor_dy = mouse.y - FACE_Y;
        bool point_shown = pv.phase != POINTER_HOME || pv.result != POINTER_RESULT_NONE;
        mascot.point_phase = pv.phase;
        mascot.point_t = pv.phase_t;
        mascot.point_result = pv.result;
        mascot.point_dx = point_shown ? pv.target.x - FACE_X : 0.0f;
        mascot.point_dy = point_shown ? pv.target.y - FACE_Y : 0.0f;
        mascot.walk_vx = pv.vel.x;
        mascot.walk_vy = pv.vel.y;
        if (pv.phase == POINTER_OUT || pv.phase == POINTER_AT) {   // eyes on the target instead of the mouse
            mascot.cursor_dx = mascot.point_dx;
            mascot.cursor_dy = mascot.point_dy;
        }
        // driving, his cable goes to the real pointer. unless hes off pointing at something
        // (a handover), then pointer.c has his arm and his walk
        mascot.drive_in = dv.in;
        if (dv.in.phase != DRIVE_ANIM_NONE && !pointer_away()) {
            mascot.point_dx = (float)(dv.plug_x - df.win_x - FACE_X);   // df.win_* is where he is now
            mascot.point_dy = (float)(dv.plug_y - df.win_y - FACE_Y);
            mascot.walk_vx = dv.vel.x;
            mascot.walk_vy = dv.vel.y;
            mascot.cursor_dx = mascot.point_dx;
            mascot.cursor_dy = mascot.point_dy;
        }
        bool busy = speech_busy() || pending > 0 || pw_active || ask_active || pointer_away() || drive_away();
        mascot.idle_for = interacted || busy ? 0.0f : mascot.idle_for + dt;
        mascot.held = dragging;
        mascot.vel_x = dragging && dt > 0 ? (gx - last_gx) / dt : 0.0f;
        mascot.vel_y = dragging && dt > 0 ? (gy - last_gy) / dt : 0.0f;
        last_gx = gx;
        last_gy = gy;
        mascot_update(&mascot, dt);

        if (now >= next_hush_check) {
            sfx_hush(platform_foreground_fullscreen());
            next_hush_check = now + HUSH_CHECK_EVERY;
        }
        sfx_update(dt);
        cues_frame(&mascot, speech_events(), his_pan(), chat_open);
        if (!booted) {                   // first frame, once hes on screen
            cues_play(SFX_BOOT);
            booted = true;
        }

        // only the user's own poke counts as "woke". a proactive line wakes him too (busy resets
        // idle_for) and reporting that makes him talk about waking up, which wakes him, which.. lol
        bool user_touched = interacted || dragging;
        pet_idle = user_touched ? 0.0f : pet_idle + dt;
        bool asleep = sleep_is_asleep(&mascot);
        if (asleep && !was_asleep) asleep_since = now;
        if (was_asleep && !asleep && user_touched && brain_ok && brain_ready)
            brain_send_woke((float)(now - asleep_since));
        was_asleep = asleep;

        if (brain_ok && brain_ready) {
            if (next_presence == 0.0) next_presence = now;
            if (now >= next_presence) {
                brain_send_presence(pet_idle, (float)platform_pc_idle_seconds(),
                                    chat_open && chat_len() > 0, asleep);
                next_presence = now + PRESENCE_EVERY;
            }
        }

        int want = dragging ? MOUSE_CURSOR_RESIZE_ALL :
                   over_chat ? MOUSE_CURSOR_IBEAM :
                   (over_arrow || hover != UI_HOVER_NONE || (over_bubble && !speech_sticky()))
                       ? MOUSE_CURSOR_POINTING_HAND : MOUSE_CURSOR_DEFAULT;
        if (want != cursor) {
            SetMouseCursor(want);
            cursor = want;
        }

        BeginDrawing();
        ClearBackground(BLANK);

        // bubble goes on top of the arm, when hes pinned to the top of the screen the arm
        // can reach up past it and it shouldnt cover the text
        float t = (float)GetTime();
        point_fx_draw_behind(&pv, &mascot, MASCOT_X, MASCOT_Y, MASCOT_PX, t);
        mascot_draw(&mascot, MASCOT_X, MASCOT_Y, MASCOT_PX);
        point_fx_draw(&pv, &mascot, MASCOT_X, MASCOT_Y, MASCOT_PX, t);
        speech_draw(hover);

        if (chat_open)
            chat_draw(chat_r, can_type && IsWindowFocused(),
                      typing_password, typing_password ? "password..." : "say something...");
        if (!typing_password) ui_arrow_draw(WIN_W / 2.0f, arrow_y, chat_open);

        EndDrawing();
    }

    goodbye(hwnd);
    brain_stop();
    sfx_shutdown();
    CloseWindow();
    return 0;
}
