// saying bye when the app quits (ctrl+q or closing the window)
//   wave    ^ ^ eyes, blushing, one hand waving while "Bye!" types out
//   flash   power cuts and the screen flares white (the power down sound starts here)
//   squash  the whole picture collapses into a bright line like an old CRT turning off
//   line    the line shrinks to a dot
//   dot     the dot glows for a sec and goes out
// wave and flash get drawn in the grid like any face. squash, line and dot warp the
// finished sprite so mascot_draw does those from bye_crt(). main.c sets m->leaving and
// only closes once bye_done()

#include "mascot_kit.h"

#define WAVE_BEAT 0.14f     // secs between the waving hand going up and down

static float off_t(const Mascot *m)  // secs since the power cut (negative before it)
{
    return m->bye.t - BYE_WAVE;
}

bool bye_done(const Mascot *m)
{
    return m->leaving && m->bye.t >= BYE_LEN;
}

void bye_update(Mascot *m, float dt)
{
    if (!m->leaving) {
        m->bye.t = 0.0f;
        return;
    }
    float prev = m->bye.t;
    m->bye.t += dt;
    if (prev < BYE_WAVE && m->bye.t >= BYE_WAVE) m->ev.bits |= MEV_SWITCH_OFF;
}

void bye_pose(const Mascot *m, Pose *p)
{
    if (!m->leaving) return;
    // stand still and straight so the collapse has a steady picture to squash
    p->bob = 0;
    p->lean = 0;
    p->lift = 0;
    p->shift_x = p->shift_y = 0;
    p->foot_dx[0] = p->foot_dx[1] = p->foot_dy[0] = p->foot_dy[1] = 0;
}

bool bye_face(const Mascot *m, Face *f)
{
    if (!m->leaving) return false;
    f->lh = f->rh = 30;
    if (off_t(m) >= 0.0f) {                          // power cut, the glass flares white
        mk_fill(f->glass_x, f->glass_y, f->glass_w, f->glass_h, 'H');
        return true;
    }
    mk_eye(f->lx, f->ly, EYE_HAPPY, 0, 0, 0, 0);
    mk_eye(f->rx, f->ly, EYE_HAPPY, 0, 0, 0, 0);
    const int my = f->my, by = f->ly + 9;
    mk_hline(12, by, 2, 'k');                        // blush
    mk_hline(26, by, 2, 'k');
    mk_put(16, my, 'O');                             // big smile
    mk_put(23, my, 'O');
    mk_hline(17, my + 1, 6, 'O');
    mk_hline(18, my + 1, 4, 'Y');
    bool up = (int)(m->bye.t / WAVE_BEAT) % 2 == 0;
    f->lh = f->hy + (up ? -1 : 5);                   // wave with the free hand (the cable loop is on the right)
    return true;
}

void bye_overlay(const Mascot *m, const Face *f)
{
    (void)m;                                         // nothing to draw on top
    (void)f;
}

static float ease_in(float k)
{
    k = k < 0 ? 0 : (k > 1 ? 1 : k);
    return k * k;
}

ByeCrt bye_crt(const Mascot *m)
{
    ByeCrt c = { .squash = 1.0f, .line = 1.0f, .dot = 1.0f };
    if (!m->leaving) return c;
    float t = off_t(m) - BYE_FLASH;                  // the flash is drawn in the grid
    if (t < 0.0f) return c;
    if (t < BYE_SQUASH) {
        float k = t / BYE_SQUASH;
        c.stage = 1;
        c.squash = 1.0f - ease_in(k);
        c.white = k * k;                             // gets whiter as it thins out to a line
        return c;
    }
    t -= BYE_SQUASH;
    if (t < BYE_LINE) {
        c.stage = 2;
        c.line = 1.0f - ease_in(t / BYE_LINE);
        return c;
    }
    t -= BYE_LINE;
    c.stage = 3;
    c.dot = t < BYE_DOT ? 1.0f - t / BYE_DOT : 0.0f;
    return c;
}

// preview (make anim bye)

static void d_bye(Mascot *m, float t)
{
    (void)t;
    m->mood = MOOD_IDLE;
    m->cursor_dx = 2000.0f;
    m->cursor_dy = 0.0f;
    m->leaving = true;
}

int bye_preview(PreviewFrame *out, int cap)
{
    static const struct { const char *label; float t; } FRAMES[] = {
        { "wave up", 0.05f },       { "wave down", 0.2f },      { "wave up", 0.33f },
        { "power cut", BYE_WAVE + 0.03f },
        { "squash 1/3", BYE_WAVE + BYE_FLASH + BYE_SQUASH * 0.33f },
        { "squash 2/3", BYE_WAVE + BYE_FLASH + BYE_SQUASH * 0.66f },
        { "line", BYE_WAVE + BYE_FLASH + BYE_SQUASH + 0.01f },
        { "line shrinking", BYE_WAVE + BYE_FLASH + BYE_SQUASH + BYE_LINE * 0.6f },
        { "dot", BYE_WAVE + BYE_FLASH + BYE_SQUASH + BYE_LINE + 0.02f },
        { "dot fading", BYE_WAVE + BYE_FLASH + BYE_SQUASH + BYE_LINE + BYE_DOT * 0.6f },
    };
    int n = 0;
    for (int i = 0; i < (int)(sizeof FRAMES / sizeof FRAMES[0]) && n < cap; i++)
        out[n++] = (PreviewFrame){ FRAMES[i].label, FRAMES[i].t, d_bye };
    return n;
}
