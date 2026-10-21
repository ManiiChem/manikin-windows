// eyes that follow the mouse while hes idle. each pupil aims from its own eye (far
// pointer = parallel eyes, right between them = cross eyed) and rides a spring, a smooth
// follow normally and a quick overshooting jump (saccade) once the target gets over a
// pixel ahead. the drawn offsets round with hysteresis so they dont flicker
//   far      calm tracking, a 1 px head turn / body lean when its way off to the side
//   close    sparkly eyes and a bigger smile
//   on face  quick happy squeeze (^ ^) then blush
//   between  cross eyed with a wobbly mouth
//   still    after a while he gets bored and looks around, snaps back once it moves

#include <math.h>
#include <stdio.h>

#include "mascot_kit.h"
#include "pointer.h"

// tuning
#define BORED_AFTER     4.5f    // secs of a still pointer before he gets bored and looks around
#define BORED_GLANCE_LEAD 1.3f  // secs skipped into the glance cycle so the first glance is ~1.2 s in
#define MOVE_PX         3.0f   // screen px the pointer has to move to count as moving
#define SAT_PX          45.0f   // screen px where the pupil is about halfway to the rim (bigger = lazier)
#define PUPIL_GAIN      2.35f   // max pupil offset in art px, the drawn one clamps to 2 anyway
#define SACCADE_TRIGGER 1.1f    // art px the target can get ahead of the pupil before it jumps
#define SACCADE_W       34.0f   // saccade spring in rad/s, ~50 ms to land (higher = snappier)
#define SACCADE_ZETA    0.45f   // damping, under 1 so it overshoots ~20% then settles
#define PURSUIT_W_NEAR  16.0f   // smooth follow spring in rad/s with the pointer close
#define PURSUIT_W_FAR   9.0f    // ...and far away (calmer)
#define QUANT_HYST      0.18f   // extra art px past the half pixel before a drawn pupil moves
#define HEAD_PX         220.0f  // |dx| in screen px past which his face turns a pixel
#define LEAN_PX         520.0f  // |dx| past which his whole body leans a pixel
#define FLINCH_TIME     0.45f   // secs of the happy squeeze when the pointer lands on his face
#define FLINCH_MAX_SPEED 900.0f // px/s, any faster and its just whizzing past

// eye centres from the middle of his screen face (where cursor_dx/dy start), screen px
#define EYE_X 16.5f
#define EYE_Y (-4.5f)

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float approach(float v, float target, float rate, float dt)
{
    return v + (target - v) * (1.0f - expf(-rate * dt));
}

// rounding with hysteresis, q only changes once v is clearly past the halfway point
static int quantise(float v, int q, float h, int lo, int hi)
{
    while (q < hi && v > (float)q + 0.5f + h) q++;
    while (q > lo && v < (float)q - 0.5f - h) q--;
    return q;
}

// one damped spring step (semi implicit euler)
static void spring(float *x, float *v, float target, float w, float zeta, float dt)
{
    *v += (w * w * (target - *x) - 2.0f * zeta * w * *v) * dt;
    *x += *v * dt;
}

// where one eye wants its pupil for a pointer at (dx, dy) from that eye
static void aim(float dx, float dy, float *tx, float *ty)
{
    float r = sqrtf(dx * dx + dy * dy);
    if (r < 0.5f) { *tx = 0.0f; *ty = 0.0f; return; }
    float ux = dx / r, uy = dy / r;
    // push diagonals toward the corners of the pupils square range but not all the
    // way, the eye is round and a pupil right in the corner gets clipped
    float big = fmaxf(fabsf(ux), fabsf(uy));
    float square = 1.0f + (1.0f / big - 1.0f) * 0.55f;
    float mag = PUPIL_GAIN * r / (r + SAT_PX) * square;
    *tx = clampf(ux * mag, -2.4f, 2.4f);
    *ty = clampf(uy * mag, -2.4f, 2.4f);
}

// same glance timing as builtin_face so bored him looks like his usual self
static void glance(float t, float *tx, float *ty)
{
    float g = fmodf(t, 7.0f);
    *tx = 0.0f;
    *ty = 0.0f;
    if (g > 2.5f && g < 3.4f) *tx = -2.0f;
    else if (g > 4.6f && g < 5.3f) { *tx = 2.0f; *ty = -1.0f; }
    else if (g > 6.0f && g < 6.5f) *ty = 1.0f;
}

static bool pointer_on_face(float dx, float dy)
{
    return fabsf(dx) < 31.0f && dy > -27.0f && dy < 22.0f;   // roughly the teal glass
}

// is the gaze face actually on screen? only then does a flinch or perk get a sound
// (dragged, pointing, asleep or waking up means some other face wins)
static bool face_shown(const Mascot *m)
{
    return m->mood == MOOD_IDLE && !m->held && m->point_phase == POINTER_HOME && !sleep_is_asleep(m) &&
           m->sleep.wake_left <= 0.0f;
}

void gaze_update(Mascot *m, float dt)
{
    GazeState *g = &m->gaze;
    const float cx = m->cursor_dx, cy = m->cursor_dy;
    if (!(dt > 0.0f)) return;
    if (dt > 0.1f) dt = 0.1f;                    // after a hitch, dont fling the pupils
    if (!g->inited) {
        g->inited = true;
        g->anchor_x = cx;
        g->anchor_y = cy;
        g->attention = 1.0f;
        g->last_x = cx;
        g->last_y = cy;
    }
    {
        float sx = cx - g->last_x, sy = cy - g->last_y;
        g->speed = approach(g->speed, sqrtf(sx * sx + sy * sy) / dt, 12.0f, dt);
        g->last_x = cx;
        g->last_y = cy;
    }

    float mdx = cx - g->anchor_x, mdy = cy - g->anchor_y;
    bool moved = mdx * mdx + mdy * mdy > MOVE_PX * MOVE_PX;
    if (moved) {
        g->anchor_x = cx;
        g->anchor_y = cy;
        if (g->attention < 0.5f) {               // interested again, snap straight back
            g->perk = 0.5f;
            g->saccade = 0.3f;
            if (face_shown(m)) m->ev.bits |= MEV_PERK;
        }
        g->still_for = 0.0f;
        g->attention = 1.0f;
    } else {
        g->still_for += dt;
        if (g->still_for > BORED_AFTER) g->attention = 0.0f;
    }
    bool watching = g->attention > 0.5f;
    g->bored_t = watching ? 0.0f : g->bored_t + dt;

    // how close is it (hovering, on his face, between his eyes)
    float r = sqrtf(cx * cx + cy * cy);
    float near_t = watching ? clampf((150.0f - r) / 80.0f, 0.0f, 1.0f) : 0.0f;
    bool on_face = pointer_on_face(cx, cy);
    float bx = cx, by = (cy - EYE_Y) * 1.3f;
    float cross_t = watching ? clampf((15.0f - sqrtf(bx * bx + by * by)) / 7.0f, 0.0f, 1.0f) : 0.0f;
    g->near = approach(g->near, near_t, 8.0f, dt);
    g->on_face = approach(g->on_face, watching && on_face ? 1.0f : 0.0f, 10.0f, dt);
    g->cross = approach(g->cross, cross_t, 12.0f, dt);

    // only a slow landing on his face counts, whizzing past doesnt
    if (on_face && !g->was_on_face && watching && g->flinch_cool <= 0.0f && g->speed < FLINCH_MAX_SPEED) {
        g->flinch = FLINCH_TIME;
        g->flinch_cool = 1.6f;
        if (face_shown(m)) m->ev.bits |= MEV_FLINCH;
    }
    g->was_on_face = on_face;
    g->flinch = fmaxf(0.0f, g->flinch - dt);
    g->flinch_cool = fmaxf(0.0f, g->flinch_cool - dt);
    g->perk = fmaxf(0.0f, g->perk - dt);

    float tx[2], ty[2];
    if (watching) {
        for (int e = 0; e < 2; e++) {
            float ex = e == 0 ? -EYE_X : EYE_X;
            aim(cx - ex, cy - EYE_Y, &tx[e], &ty[e]);
        }
        // right between the eyes, pull them all the way in
        tx[0] += 1.4f * g->cross;
        tx[1] -= 1.4f * g->cross;
    } else {
        glance(g->bored_t + BORED_GLANCE_LEAD, &tx[0], &ty[0]);
        tx[1] = tx[0];
        ty[1] = ty[0];
    }

    // saccade or smooth follow?
    float lead = 0.0f;
    for (int e = 0; e < 2; e++) {
        float ex = fabsf(clampf(tx[e], -2.0f, 2.0f) - g->px[e]);
        float ey = fabsf(clampf(ty[e], -2.0f, 2.0f) - g->py[e]);
        lead = fmaxf(lead, fmaxf(ex, ey));
    }
    if (g->saccade <= 0.0f && lead > SACCADE_TRIGGER) g->saccade = 0.3f;

    float far = clampf((r - 150.0f) / 450.0f, 0.0f, 1.0f);
    float w = g->saccade > 0.0f ? SACCADE_W : PURSUIT_W_NEAR + (PURSUIT_W_FAR - PURSUIT_W_NEAR) * far;
    float zeta = g->saccade > 0.0f ? SACCADE_ZETA : 1.0f;
    if (!watching && g->saccade <= 0.0f) w = 7.0f;   // bored, lazy drift back to the centre

    // substep so big frame times stay stable
    int steps = (int)ceilf(dt / (1.0f / 120.0f));
    float h = dt / (float)steps;
    for (int s = 0; s < steps; s++)
        for (int e = 0; e < 2; e++) {
            spring(&g->px[e], &g->vx[e], tx[e], w, zeta, h);
            spring(&g->py[e], &g->vy[e], ty[e], w, zeta, h);
        }
    if (g->saccade > 0.0f) {
        g->saccade -= dt;
        bool settled = lead < 0.2f;
        for (int e = 0; e < 2; e++)
            settled = settled && fabsf(g->vx[e]) < 2.0f && fabsf(g->vy[e]) < 2.0f;
        if (settled) g->saccade = 0.0f;
    }
    for (int e = 0; e < 2; e++) {
        g->qx[e] = quantise(g->px[e], g->qx[e], QUANT_HYST, -2, 2);
        g->qy[e] = quantise(g->py[e], g->qy[e], QUANT_HYST, -2, 2);
    }

    // head turn and body lean toward a pointer way off to the side. both lag behind the
    // eyes (eyes go first then the head) and overshoot a bit
    float side = cx > 0.0f ? 1.0f : -1.0f;
    float head_t = watching && fabsf(cx) > HEAD_PX && fabsf(cx) > fabsf(cy) * 0.8f ? side : 0.0f;
    float lean_t = watching && fabsf(cx) > LEAN_PX && fabsf(cx) > fabsf(cy) ? side : 0.0f;
    for (int s = 0; s < steps; s++) {
        spring(&g->head, &g->head_v, head_t, 9.0f, 0.6f, h);
        spring(&g->lean, &g->lean_v, lean_t, 6.0f, 0.7f, h);
    }
    g->qhead = quantise(g->head, g->qhead, 0.25f, -1, 1);
    g->qlean = quantise(g->lean, g->qlean, 0.25f, -1, 1);
}

void gaze_pose(const Mascot *m, Pose *p)
{
    if (m->mood != MOOD_IDLE) return;
    p->shift_x += m->gaze.qlean;
}

// the idle mouth, or one that fits what hes looking at
static void mouth(const GazeState *g, int my, int sx)
{
    if (g->flinch > 0.0f || g->on_face > 0.5f || g->near > 0.75f) {
        if (g->cross > 0.5f) {                   // cross eyed gets a goofy wobbly mouth
            mk_put(17 + sx, my + 1, 'O');
            mk_put(18 + sx, my, 'O');
            mk_put(19 + sx, my, 'O');
            mk_put(20 + sx, my + 1, 'O');
            mk_put(21 + sx, my + 1, 'O');
            mk_put(22 + sx, my, 'O');
            return;
        }
        mk_put(16 + sx, my, 'O');                // big happy grin
        mk_put(23 + sx, my, 'O');
        mk_hline(17 + sx, my + 1, 6, 'O');
        mk_hline(18 + sx, my + 1, 4, 'Y');
        return;
    }
    if (g->perk > 0.0f) {                        // "oh! there you are"
        mk_fill(19 + sx, my, 2, 2, 'O');
        return;
    }
    mk_put(17 + sx, my, 'O');                    // the usual idle smile
    mk_put(22 + sx, my, 'O');
    mk_hline(18 + sx, my + 1, 4, 'O');
    mk_put(19 + sx, my + 1, 'Y');
    mk_put(20 + sx, my + 1, 'Y');
}

bool gaze_face(const Mascot *m, Face *f)
{
    const GazeState *g = &m->gaze;
    if (!g->inited) return false;
    const int sx = g->qhead;
    const int ex[2] = { f->lx + sx, f->rx + sx };

    if (g->flinch > FLINCH_TIME * 0.3f) {        // happy squeeze ^ ^
        for (int e = 0; e < 2; e++) mk_eye(ex[e], f->ly, EYE_HAPPY, 0, 0, 0, 0);
    } else if (m->blink_left > 0.0f) {
        for (int e = 0; e < 2; e++) mk_eye(ex[e], f->ly, EYE_SHUT, 0, 0, 0, 0);
    } else {
        for (int e = 0; e < 2; e++) {
            int pdx = g->qx[e], pdy = g->qy[e];
            // upper lid follows the gaze down, looking way up narrows the bottom
            int lid = pdy >= 2 && g->near < 0.5f ? 1 : 0;
            int low = pdy <= -2 && g->near < 0.5f ? 1 : 0;
            // a pupil in a bottom corner under the lowered lid gets clipped to a sliver so
            // lift it a row (the lid still says looking down)
            if (pdy >= 2 && (pdx >= 2 || pdx <= -2)) pdy = 1;
            mk_eye(ex[e], f->ly, EYE_OPEN, pdx, pdy, lid, low);
            // up close his eyes light up, the small shine turns into a bright sparkle
            if (g->near > 0.5f) {
                int sxp = ex[e] + 4 + pdx, syp = f->ly + 5 + pdy;
                if (mk_get(sxp, syp) == 'U') mk_put(sxp, syp, 'w');
            }
        }
    }

    // blush while the pointer is on his face
    if (g->on_face > 0.5f || g->flinch > 0.0f) {
        int by = f->ly + 9;
        mk_hline(ex[0], by, 2, 'k');
        mk_hline(ex[1] + 5, by, 2, 'k');
    }

    mouth(g, f->my, sx);
    return true;
}

void gaze_overlay(const Mascot *m, const Face *f)
{
    (void)m;
    (void)f;
}

// preview (make anim gaze)

static void at(Mascot *m, float dx, float dy)
{
    m->mood = MOOD_IDLE;
    m->cursor_dx = dx;
    m->cursor_dy = dy;
}

#define FIXED(name, dx, dy) \
    static void name(Mascot *m, float t) { (void)t; at(m, dx, dy); }
FIXED(d_e, 700.0f, 0.0f)
FIXED(d_ne, 500.0f, -500.0f)
FIXED(d_n, 0.0f, -600.0f)
FIXED(d_nw, -500.0f, -500.0f)
FIXED(d_w, -700.0f, 0.0f)
FIXED(d_sw, -500.0f, 500.0f)
FIXED(d_s, 0.0f, 600.0f)
FIXED(d_se, 500.0f, 500.0f)
FIXED(d_hover_r, 85.0f, -5.0f)
FIXED(d_hover_ul, -60.0f, -70.0f)
FIXED(d_cheek, -24.0f, 12.0f)
FIXED(d_between, 0.0f, -4.0f)
FIXED(d_left_eye, -16.0f, -4.0f)
FIXED(d_mid, 220.0f, 60.0f)

// pointer flies in from far away and lands on his face at t = 1.0
static void d_land(Mascot *m, float t)
{
    float k = t < 1.0f ? t : 1.0f;
    k = 1.0f - (1.0f - k) * (1.0f - k);          // slows down as it arrives
    at(m, 600.0f - 582.0f * k, -200.0f + 192.0f * k);
}

// steady sweep from far left to far right (1300 px/s) a bit above his head
static void d_sweep(Mascot *m, float t)
{
    float x = -600.0f + 1300.0f * (t - 0.5f);
    at(m, x < -600.0f ? -600.0f : x, -120.0f);
}

// pointer whizzes straight across his face, too fast to flinch
static void d_whizz(Mascot *m, float t)
{
    at(m, -600.0f + 1300.0f * t, -5.0f);
}

// pointer parked far left then jumps to just right of his face at t = 1.0
static void d_jump(Mascot *m, float t)
{
    if (t < 1.0f) at(m, -650.0f + 10.0f * sinf(t * 9.0f), 20.0f);
    else at(m, 70.0f, -10.0f);
}

// moves till t = 1.0, sits still till t = 10.0, then moves again
static void d_bored(Mascot *m, float t)
{
    if (t < 1.0f) at(m, 300.0f * t, -160.0f * t);
    else if (t < 10.0f) at(m, 300.0f, -160.0f);
    else at(m, 300.0f - 1500.0f * (t - 10.0f), -160.0f + 600.0f * (t - 10.0f));
}

int gaze_preview(PreviewFrame *out, int cap)
{
    static char labels[40][19];
    int n = 0;
#define ADD(l, s, d) do { if (n < cap) out[n++] = (PreviewFrame){ (l), (s), (d) }; } while (0)
    ADD("far E", 1.0f, d_e);
    ADD("far NE", 1.0f, d_ne);
    ADD("far N", 1.0f, d_n);
    ADD("far NW", 1.0f, d_nw);
    ADD("far W", 1.0f, d_w);
    ADD("far SW", 1.0f, d_sw);
    ADD("far S", 1.0f, d_s);
    ADD("far SE", 1.0f, d_se);

    ADD("mid right", 1.0f, d_mid);
    ADD("hover right", 1.0f, d_hover_r);
    ADD("hover up-left", 1.0f, d_hover_ul);
    ADD("lands: flinch", 1.08f, d_land);
    ADD("on face: blush", 1.8f, d_cheek);
    ADD("between eyes", 1.0f, d_between);
    ADD("on left eye", 1.0f, d_left_eye);
    ADD("blink (far E)", 2.56f, d_e);

    int li = 0;
    for (int i = 0; i < 12 && li < 40; i++, li++) {
        float s = 0.5f + 0.08f * (float)i;
        snprintf(labels[li], sizeof labels[li], "sweep %.2fs", s);
        ADD(labels[li], s, d_sweep);
    }
    static const float JUMP[8] = { 0.98f, 1.017f, 1.033f, 1.05f, 1.067f, 1.1f, 1.15f, 1.3f };
    for (int i = 0; i < 8 && li < 40; i++, li++) {
        if (i == 0) snprintf(labels[li], sizeof labels[li], "before jump");
        else snprintf(labels[li], sizeof labels[li], "jump +%.0fms", (JUMP[i] - 1.0f) * 1000.0f);
        ADD(labels[li], JUMP[i], d_jump);
    }
    static const float BORED[8] = { 2.0f, 5.4f, 6.0f, 7.0f, 8.6f, 10.05f, 10.15f, 10.4f };
    for (int i = 0; i < 8 && li < 40; i++, li++) {
        snprintf(labels[li], sizeof labels[li], "%s %.2fs", BORED[i] < 10.0f ? "still" : "moves", BORED[i]);
        ADD(labels[li], BORED[i], d_bored);
    }
    ADD("whizz past face", 0.47f, d_whizz);
#undef ADD
    return n;
}
