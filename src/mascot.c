// manikin himself, an old all in one computer thats alive. the big teal screen is his face
// and the two glowing eyes do all the emoting, the rest is a keyboard base on stubby feet,
// cable arms plugged into side ports, a floppy drive, a speaker and a network card on top.
// he gets drawn into a char grid every frame, outlined, then painted with the palette

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "mascot.h"
#include "mascot_kit.h"
#include "ui.h"

static char grid[MASCOT_H][MASCOT_W];

#define C(r, g, b) { r, g, b, 255 }
static const Color palette[128] = {
    ['K'] = C(22, 24, 32),     // outline
    ['H'] = C(246, 247, 249),  // casing highlight / eye shine
    ['W'] = C(214, 217, 221),  // casing light
    ['G'] = C(168, 173, 180),  // casing mid
    ['S'] = C(118, 124, 134),  // casing shade
    ['M'] = C(72, 78, 90),     // dark metal
    ['D'] = C(9, 34, 40),      // screen dark
    ['p'] = C(14, 22, 30),     // pupils
    ['T'] = C(20, 64, 70),     // screen teal
    ['U'] = C(52, 118, 120),   // screen glint
    ['d'] = C(15, 50, 56),     // scanline
    ['g'] = C(66, 76, 54),     // orange glow bleed on the screen
    ['O'] = C(255, 146, 28),   // orange glow
    ['Y'] = C(255, 226, 130),  // hot centre
    ['L'] = C(255, 70, 60),    // LED red (lit)
    ['l'] = C(104, 34, 36),    // LED red (off)
    ['E'] = C(80, 222, 110),   // LED / wire green
    ['e'] = C(30, 84, 52),     // LED green (off)
    ['B'] = C(64, 140, 236),   // wire blue
    ['R'] = C(216, 62, 54),    // wire red
    ['P'] = C(236, 124, 40),   // wire orange
    // only the anim modules use these
    ['k'] = C(255, 118, 150),  // blush pink
    ['h'] = C(240, 52, 84),    // heart red
    ['w'] = C(255, 255, 255),  // white sparkle
    ['q'] = C(110, 196, 255),  // tear / sweat blue
    ['v'] = C(170, 110, 240),  // dizzy violet
    ['z'] = C(200, 226, 255),  // sleep z's
};

// primitives

static void put(int x, int y, char c)
{
    if (x >= 0 && x < MASCOT_W && y >= 0 && y < MASCOT_H) grid[y][x] = c;
}

static char get(int x, int y)
{
    if (x < 0 || x >= MASCOT_W || y < 0 || y >= MASCOT_H) return '.';
    return grid[y][x];
}

static void fill(int x, int y, int w, int h, char c)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) put(x + i, y + j, c);
}

static void hline(int x, int y, int w, char c) { fill(x, y, w, 1, c); }
static void vline(int x, int y, int h, char c) { fill(x, y, 1, h, c); }

// filled box with a 1 cell edge, cut notches the corners so it looks rounded
static void box(int x, int y, int w, int h, char body, char edge, bool cut)
{
    fill(x, y, w, h, edge);
    fill(x + 1, y + 1, w - 2, h - 2, body);
    if (cut) {
        put(x, y, '.');
        put(x + w - 1, y, '.');
        put(x, y + h - 1, '.');
        put(x + w - 1, y + h - 1, '.');
    }
}

// bresenham, if c2 isnt 0 it swaps colour every seg cells for the ribbed look
static void line2(int x0, int y0, int x1, int y1, char c, char c2, int seg)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, n = 0;
    for (;;) {
        put(x0, y0, (c2 && (n / seg) & 1) ? c2 : c);
        n++;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void line(int x0, int y0, int x1, int y1, char c) { line2(x0, y0, x1, y1, c, 0, 1); }

// paint a little ascii stamp, '.' is see through and flip mirrors it
static void stamp(int x, int y, const char *rows[], int h, bool flip)
{
    for (int j = 0; j < h; j++) {
        int w = (int)strlen(rows[j]);
        for (int i = 0; i < w; i++) {
            char c = rows[j][flip ? w - 1 - i : i];
            if (c != '.') put(x + i, y + j, c);
        }
    }
}

// any empty cell touching art becomes outline so his silhouette always stands out
static void auto_outline(void)
{
    static char src[MASCOT_H][MASCOT_W];
    memcpy(src, grid, sizeof grid);
    for (int y = 0; y < MASCOT_H; y++)
        for (int x = 0; x < MASCOT_W; x++) {
            if (src[y][x] != '.') continue;
            bool near = false;
            for (int k = 0; k < 4 && !near; k++) {
                static const int ox[4] = { 1, -1, 0, 0 }, oy[4] = { 0, 0, 1, -1 };
                int nx = x + ox[k], ny = y + oy[k];
                if (nx < 0 || nx >= MASCOT_W || ny < 0 || ny >= MASCOT_H) continue;
                char c = src[ny][nx];
                if (c != '.' && c != 'K') near = true;
            }
            if (near) grid[y][x] = 'K';
        }
}

// glow bleed, only ever tints the screen glass and never the bezel
static void glow_cell(int x, int y)
{
    char c = get(x, y);
    if (c == 'T' || c == 'd' || c == 'D' || c == 'U') put(x, y, 'g');
}

// every lit cell on the glass bleeds a soft glow onto its neighbours
static void glow_pass(int x, int y, int w, int h)
{
    static char src[MASCOT_H][MASCOT_W];
    memcpy(src, grid, sizeof grid);
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) {
            char c = src[j][i];
            if (c != 'O' && c != 'Y') continue;
            glow_cell(i - 1, j);
            glow_cell(i + 1, j);
            glow_cell(i, j - 1);
            glow_cell(i, j + 1);
        }
}

// eyes

#define EYE_W 7
#define EYE_H 9

// rounded 7x9 eye mask, lid cuts rows off the top (flat skeptical lid), low off the bottom
static bool eye_mask(int i, int j, int lid, int low)
{
    static const int span[EYE_H] = { 3, 5, 7, 7, 7, 7, 7, 5, 3 };
    if (i < 0 || i >= EYE_W || j < lid || j >= EYE_H - low) return false;
    int o = (EYE_W - span[j]) / 2;
    return i >= o && i < o + span[j];
}

// the big glowing eye (orange rim, cream fill, dark pupil with a shine)
static void eye(int x, int y, EyeShape shape, int pdx, int pdy, int lid, int low)
{
    if (shape == EYE_SHUT || shape == EYE_HAPPY) {
        // shut is a soft downward curve, happy is an arch (^)
        int top = shape == EYE_SHUT ? 5 : 3, dir = shape == EYE_SHUT ? 1 : -1;
        int edge = shape == EYE_SHUT ? top : top + 2;    // row of the outer ends
        put(x, y + edge, 'O');
        put(x + 6, y + edge, 'O');
        put(x + 1, y + edge + dir, 'O');
        put(x + 5, y + edge + dir, 'O');
        hline(x + 2, y + edge + 2 * dir, 3, 'O');
        put(x + 3, y + edge + 2 * dir, 'Y');
        return;
    }
    for (int j = 0; j < EYE_H; j++)
        for (int i = 0; i < EYE_W; i++) {
            if (!eye_mask(i, j, lid, low)) continue;
            bool rim = !eye_mask(i - 1, j, lid, low) || !eye_mask(i + 1, j, lid, low) ||
                       !eye_mask(i, j - 1, lid, low) || !eye_mask(i, j + 1, lid, low);
            put(x + i, y + j, rim ? 'O' : 'Y');
        }
    // 3x4 pupil, clipped to the inside of the eye
    int px0 = 2 + pdx, py0 = 3 + pdy;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 3; i++)
            if (get(x + px0 + i, y + py0 + j) == 'Y') put(x + px0 + i, y + py0 + j, 'p');
    // shine on the pupils top left plus a tiny second sparkle
    if (get(x + px0, y + py0) == 'p') put(x + px0, y + py0, 'H');
    if (get(x + px0 + 2, y + py0 + 2) == 'p') put(x + px0 + 2, y + py0 + 2, 'U');
}

// arms

static const char *HAND[] = {      // fingers down
    "SWWS",
    "WHWS",
    "M.M.",
};
static const char *HAND_UP[] = {   // fingers up, palm out
    "M.M.",
    "WHWS",
    "SWWS",
};

// ribbed cable arm plugged into a side port. coords are for the left side and side > 0
// mirrors them. hand top left is (2, hy), the cable runs down columns 3..4 from the port
// to the wrist. out = the arm is stretched out pointing at something (point_fx.c draws
// it from column 4 outward) so only the socket and plug stay on the body
static void arm(int side, int port_y, int hy, char plug, bool out)
{
#define AX(v) (side < 0 ? (v) : MASCOT_W - 1 - (v))
    const int hx = side < 0 ? 2 : MASCOT_W - 1 - 5;
    const bool up = hy < port_y;
    // port socket + coloured plug
    vline(AX(6), port_y - 1, 3, 'M');
    put(AX(5), port_y, plug);
    put(AX(5), port_y + 1, plug);
    if (out) return;
    // the cable
    int wy = up ? hy + 3 : hy - 1;            // wrist row
    int y0 = up ? wy : port_y, y1 = up ? port_y + 1 : wy;
    for (int yy = y0; yy <= y1; yy++) {
        bool rib = ((yy - port_y) & 1) != 0;
        put(AX(3), yy, rib ? 'S' : 'W');
        put(AX(4), yy, rib ? 'M' : plug);
    }
    // cuff + hand
    hline(hx, wy, 4, 'M');
    put(AX(3), wy, plug);
    stamp(hx, hy, up ? HAND_UP : HAND, 3, side > 0);
#undef AX
}

// the free hand typing on the keyboard while the other one holds the pointer (anim_wheel.c
// says where). the arm comes down his side like normal then the forearm goes across in
// front of the stand to the hand over the keys, top left at (hx, hy). he hunts and pecks
// with one finger (column 2 of the left hand, 1 of the right). it goes into a mask first
// so it gets its own dark outline where it crosses him
#define FOREARM_Y 32                       // its two rows, the lit side on top then the coloured one
static const char *PECK[] = {              // left hand a shade darker than the keys, mirrored for the right
    "SGGS",
    "GWGS",
    "..M.",
};
static void typing_arm(int side, int port_y, int hx, int hy, char plug)
{
#define AX(v) (side < 0 ? (v) : MASCOT_W - 1 - (v))
    static char mask[MASCOT_H][MASCOT_W];
    memset(mask, '.', sizeof mask);
    vline(AX(6), port_y - 1, 3, 'M');      // port socket + coloured plug
    put(AX(5), port_y, plug);
    put(AX(5), port_y + 1, plug);
    for (int yy = port_y; yy <= FOREARM_Y + 1; yy++) {      // down the side...
        bool rib = ((yy - port_y) & 1) != 0;
        mask[yy][AX(3)] = rib ? 'S' : 'W';
        mask[yy][AX(4)] = rib ? 'M' : plug;
    }
    int x0 = AX(3), x1 = side < 0 ? hx + 3 : hx;             // ...and across to the hand
    for (int x = x0; side < 0 ? x <= x1 : x >= x1; x -= side) {
        bool rib = ((x - x0) & 1) != 0;
        if (x == x0 || x == AX(4)) continue;                 // the elbow keeps its ribs
        mask[FOREARM_Y][x] = rib ? 'S' : 'W';
        mask[FOREARM_Y + 1][x] = rib ? 'M' : plug;
    }
    for (int yy = FOREARM_Y + 2; yy < hy; yy++) {            // wrist down to the key its hitting
        mask[yy][hx + 1] = 'M';
        mask[yy][hx + 2] = 'M';
    }
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 4; i++) {
            char c = PECK[j][side > 0 ? 3 - i : i];
            if (c != '.' && hy + j >= 0 && hy + j < MASCOT_H && hx + i >= 0 && hx + i < MASCOT_W)
                mask[hy + j][hx + i] = c;
        }
    // outline it where it crosses him, but not next to or under the fingertip since the
    // key glows there
    const int tip = side < 0 ? hx + 2 : hx + 1;
    for (int y = 0; y < MASCOT_H; y++)
        for (int x = 0; x < MASCOT_W; x++) {
            if (mask[y][x] != '.' || y >= hy + 3) continue;
            if (y == hy + 2 && (x == tip - 1 || x == tip + 1)) continue;
            static const int ox[4] = { 1, -1, 0, 0 }, oy[4] = { 0, 0, 1, -1 };
            for (int k = 0; k < 4; k++) {
                int nx = x + ox[k], ny = y + oy[k];
                if (nx < 0 || nx >= MASCOT_W || ny < 0 || ny >= MASCOT_H || mask[ny][nx] == '.') continue;
                if (get(x, y) != '.') put(x, y, 'K');
                break;
            }
        }
    for (int y = 0; y < MASCOT_H; y++)
        for (int x = 0; x < MASCOT_W; x++)
            if (mask[y][x] != '.') grid[y][x] = mask[y][x];
#undef AX
}

// lock screen (password entry)

// no face while you type a password, just a big glowing padlock and a dot per character
// (up to five, after that the last one keeps blinking)
static void lock_screen(int hy, float t, int typed)
{
    int rattle = fmodf(t, 1.5f) < 0.15f ? -1 : 0;     // the shackle rattles now and then
    hline(17, hy + 4 + rattle, 6, 'O');
    vline(16, hy + 5 + rattle, 3, 'O');
    vline(23, hy + 5 + rattle, 3, 'O');
    fill(15, hy + 8, 10, 6, 'O');
    hline(16, hy + 8, 8, 'Y');
    fill(19, hy + 10, 2, 2, 'p');                      // keyhole
    put(19, hy + 12, 'p');
    put(20, hy + 12, 'p');

    bool blink = typed > 5 && fmodf(t, 0.5f) < 0.25f;
    for (int i = 0; i < 5; i++) {
        bool lit = i < typed && !(blink && i == 4);
        hline(11 + i * 4, hy + 16, 2, lit ? 'Y' : 'U');
    }
}

// built in face and arms

// hand positions per mood, before any anim module gets a say
static void default_arms(const Mascot *m, Face *f, int phase)
{
    const float t = f->t;
    const int hy = f->hy;
    switch (m->mood) {
    case MOOD_SHY:          // both hands up by the screen, nervous jitter
        f->lh = hy + 2 + (fmodf(t, 0.4f) < 0.2f ? 0 : 1);
        f->rh = hy + 2 + (fmodf(t, 0.4f) < 0.2f ? 1 : 0);
        break;
    case MOOD_ASK:          // hand raised, like in class
        f->rh = hy - 1 + (fmodf(t, 1.0f) < 0.5f ? 0 : 1);
        break;
    case MOOD_HAPPY: {
        bool up = fmodf(t, 0.5f) < 0.25f;
        f->lh = hy + (up ? -1 : 3);
        f->rh = hy + (up ? 3 : -1);
        break;
    }
    case MOOD_TALKING: {
        static const int L[3] = { 13, 7, 19 }, R[3] = { 19, 26, 9 };
        f->lh = hy + L[phase] - 5;
        f->rh = hy + R[phase] - 5;
        break;
    }
    case MOOD_THINKING:     // scratching his "head" and tapping a key
        f->lh = hy + (fmodf(t, 0.6f) < 0.3f ? 1 : 2);
        f->rh = fmodf(t, 0.3f) < 0.15f ? 28 : 30;
        if (f->rh == 30 && m->drive_in.phase == DRIVE_ANIM_NONE) put(33, 36, 'E');   // not while driving, his keys are busy
        break;
    default:
        break;
    }
}

static void builtin_face(const Mascot *m, const Face *f, int phase)
{
    const float t = f->t;
    const bool thinking = m->mood == MOOD_THINKING;
    const bool talking = m->mood == MOOD_TALKING;
    const bool shut = m->blink_left > 0;
    int pdx = 0, pdy = 0, lid = 0, low = 0;
    EyeShape shape = shut ? EYE_SHUT : EYE_OPEN;
    if (m->mood == MOOD_ASK) {
        pdy = -2;                                        // looking up at the question
        pdx = fmodf(t, 2.0f) < 1.0f ? 0 : 1;
    } else if (m->mood == MOOD_HAPPY) {
        shape = EYE_HAPPY;
    } else if (thinking) {
        // look up and to the side with a flat skeptical lid, pupils drift slowly
        pdx = fmodf(t, 3.0f) < 1.5f ? 2 : 1;
        pdy = -2;
        lid = 2;
    } else if (talking) {
        static const int PDY[3] = { 0, -1, 0 };
        pdy = PDY[phase];
        if (!shut && fmodf(t, 2.4f) > 1.9f) shape = EYE_HAPPY;
        if (phase == 2) low = 1;
    } else {
        // idle, glance around now and then
        float g = fmodf(t, 7.0f);
        if (g > 2.5f && g < 3.4f) pdx = -2;
        else if (g > 4.6f && g < 5.3f) { pdx = 2; pdy = -1; }
        else if (g > 6.0f && g < 6.5f) pdy = 1;
    }
    eye(f->lx, f->ly, shape, pdx, pdy, lid, thinking ? 1 : low);
    eye(f->rx, f->ly, shape, pdx, pdy, thinking ? 0 : lid, low);

    // mouth (centred between cells 19 and 20)
    const int my = f->my;
    if (m->mood == MOOD_ASK) {
        fill(19, my, 2, 2, 'O');                         // a small "oh?"
    } else if (m->mood == MOOD_HAPPY) {
        put(16, my, 'O');
        put(23, my, 'O');
        hline(17, my + 1, 6, 'O');
        hline(18, my + 1, 4, 'Y');
    } else if (talking) {
        if (phase == 0) {
            fill(19, my, 2, 2, 'O');
        } else if (phase == 1) {
            fill(17, my, 6, 3, 'O');
            hline(18, my + 1, 4, 'p');
        } else {
            hline(17, my + 1, 6, 'O');
        }
    } else if (thinking) {
        int on = (int)(t * 4.0f) % 3;                    // loading dots
        for (int i = 0; i < 3; i++) hline(15 + i * 4, my + 1, 2, i == on ? 'Y' : 'O');
    } else {
        put(17, my, 'O');
        put(22, my, 'O');
        hline(18, my + 1, 4, 'O');
        put(19, my + 1, 'Y');
        put(20, my + 1, 'Y');
    }
}

// walking, lean and bounce

// slide rows y0..y1 sideways by dx (upper body leaning over the base)
static void shift_rows_x(int y0, int y1, int dx)
{
    if (dx == 0) return;
    for (int y = y0; y <= y1; y++) {
        char row[MASCOT_W];
        for (int x = 0; x < MASCOT_W; x++) {
            int sx = x - dx;
            row[x] = sx >= 0 && sx < MASCOT_W ? grid[y][sx] : '.';
        }
        memcpy(grid[y], row, MASCOT_W);
    }
}

// move the whole grid up dy rows (the feet get drawn after so they stay put)
static void shift_up(int dy)
{
    if (dy <= 0) return;
    memmove(grid[0], grid[dy], (size_t)(MASCOT_H - dy) * MASCOT_W);
    memset(grid[MASCOT_H - dy], '.', (size_t)dy * MASCOT_W);
}

// the stubby feet, behind the keyboard (only into empty cells)
static void feet(const Pose *p)
{
    for (int s = 0; s < 2; s++) {
        int fx = (s == 0 ? 8 : 26) + p->foot_dx[s], fy = 40 + p->foot_dy[s];
        static const char *FOOT[4] = { ".SSSS.", "SWWWWS", "SGGGGS", ".MMMM." };
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 6; i++)
                if (FOOT[j][i] != '.' && get(fx + i, fy + j) == '.') put(fx + i, fy + j, FOOT[j][i]);
    }
}

// the kit for the anim modules (mascot_kit.h)

void mk_put(int x, int y, char c) { put(x, y, c); }
char mk_get(int x, int y) { return get(x, y); }
void mk_fill(int x, int y, int w, int h, char c) { fill(x, y, w, h, c); }
void mk_hline(int x, int y, int w, char c) { hline(x, y, w, c); }
void mk_vline(int x, int y, int h, char c) { vline(x, y, h, c); }
void mk_line(int x0, int y0, int x1, int y1, char c) { line(x0, y0, x1, y1, c); }
void mk_stamp(int x, int y, const char *rows[], int h, bool flip) { stamp(x, y, rows, h, flip); }
void mk_eye(int x, int y, EyeShape shape, int pdx, int pdy, int lid, int low)
{
    eye(x, y, shape, pdx, pdy, lid, low);
}

static MkFrame last_frame;

const MkFrame *mk_frame(void) { return &last_frame; }

Color mk_color(char c)
{
    unsigned char u = (unsigned char)c;
    return u < 128 ? palette[u] : (Color){ 0, 0, 0, 0 };
}

static const char *const EMOTION_NAMES[EMOTION_COUNT] = {
    "", "happy", "excited", "laughing", "love", "surprised",
    "confused", "smug", "sad", "worried", "annoyed",
};

Emotion emotion_from_name(const char *name)
{
    for (int i = 1; name && i < EMOTION_COUNT; i++)
        if (strcmp(name, EMOTION_NAMES[i]) == 0) return (Emotion)i;
    return EMOTION_NONE;
}

// api

void mascot_init(Mascot *m)
{
    memset(m, 0, sizeof *m);
    m->blink_in = 2.5f;
}

void mascot_update(Mascot *m, float dt)
{
    m->ev = (MascotEvents){ 0 };
    m->time += dt;
    m->emotion_age += dt;
    if (m->blink_left > 0) {
        m->blink_left -= dt;
    } else if ((m->blink_in -= dt) <= 0) {
        m->blink_left = 0.12f;
        m->blink_in = 2.0f + GetRandomValue(0, 300) / 100.0f;
    }
    emotion_update(m, dt);
    gaze_update(m, dt);
    sleep_update(m, dt);
    point_update(m, dt);
    wheel_update(m, dt);
    physics_update(m, dt);
    bye_update(m, dt);
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// switching off, the CRT collapse (anim_bye.c says how far along it is)

static Color toward_white(Color c, float k)
{
    return (Color){ (unsigned char)(c.r + (255 - c.r) * k), (unsigned char)(c.g + (255 - c.g) * k),
                    (unsigned char)(c.b + (255 - c.b) * k), c.a };
}

// squash the finished grid toward the middle of his screen like a dying beam, then a
// line, then a dot. (x, y) is window px of canvas cell (0, 0), hy is the casing top row
static void switch_off(const ByeCrt *crt, int x, int y, int px, int hy)
{
    const float cx = x + 20.0f * px;                 // middle of the screen glass
    const float cy = y + (hy + 11.0f) * px;
    const Color glow = palette['U'];
    if (crt->stage == 1) {
        const float s = crt->squash;
        const float h = px * s < 1.0f ? 1.0f : px * s;   // never thinner than a pixel
        for (int pass = 0; pass < 2; pass++)              // halo first like usual
            for (int r = 0; r < MASCOT_H; r++) {
                float top = cy + (y + r * px - cy) * s;
                for (int c = 0; c < MASCOT_W; c++) {
                    unsigned char k = (unsigned char)grid[r][c];
                    if (k >= 128 || palette[k].a == 0) continue;
                    if (pass == 0)
                        DrawRectangleRec((Rectangle){ (float)(x + (c - 1) * px), top - h, 3.0f * px, 3.0f * h }, UI_HALO);
                    else                                   // the outline goes white last
                        DrawRectangleRec((Rectangle){ (float)(x + c * px), top, (float)px, h },
                                         toward_white(palette[k], k == 'K' ? crt->white * crt->white : crt->white));
                }
            }
        return;
    }
    // a glow, the dark outline everything of his has (so it shows on a light desktop too)
    // and a white hot core. first a line that shrinks from about his width to a dot...
    const Color ink = palette['K'];
    if (crt->stage == 2) {
        float w = 30.0f * px * crt->line + px;
        DrawRectangleRec((Rectangle){ cx - w / 2 - px, cy - 1.5f * px, w + 2.0f * px, 3.0f * px }, Fade(glow, 0.45f));
        DrawRectangleRec((Rectangle){ cx - w / 2 - 1, cy - 2, w + 2, 4 }, ink);
        DrawRectangleRec((Rectangle){ cx - w / 2, cy - 1, w, 2 }, WHITE);
        return;
    }
    // ...then the dot fading out
    if (crt->dot <= 0) return;
    float g = 2.0f * px, d = (float)px;
    DrawRectangleRec((Rectangle){ cx - g, cy - g, 2 * g, 2 * g }, Fade(glow, 0.4f * crt->dot));
    DrawRectangleRec((Rectangle){ cx - d / 2 - 1, cy - d / 2 - 1, d + 2, d + 2 }, Fade(ink, crt->dot));
    DrawRectangleRec((Rectangle){ cx - d / 2, cy - d / 2, d, d }, Fade(WHITE, crt->dot));
}

void mascot_draw(const Mascot *m, int x, int y, int px)
{
    memset(grid, '.', sizeof grid);

    const float t = m->time;
    const bool thinking = m->mood == MOOD_THINKING;
    const bool talking = m->mood == MOOD_TALKING;
    const bool shy = m->mood == MOOD_SHY;
    const bool asking = m->mood == MOOD_ASK;
    const bool idle = m->mood == MOOD_IDLE;
    const int phase = talking ? (int)(fmodf(t, 0.36f) / 0.12f) % 3 : 0;

    // whole body pose, the modules can change the bob or move the whole sprite
    Pose pose = { .bob = sinf(t * 3.0f) > 0.0f ? 1 : 0 };   // idle bob, whole pixels
    sleep_pose(m, &pose);
    gaze_pose(m, &pose);
    emotion_pose(m, &pose);
    point_pose(m, &pose);
    wheel_pose(m, &pose);
    physics_pose(m, &pose);
    bye_pose(m, &pose);
    pose.lift = clampi(pose.lift, 0, 2);
    pose.lean = clampi(pose.lean, -1, 1);
    const int hy = 5 + clampi(pose.bob, pose.lift > 0 ? 0 : -1, 3);   // casing top (room for a lift)

    // LED on the top card, slow heartbeat when idle and fast while thinking
    float period = thinking || shy ? 0.3f : (talking || asking ? 0.8f : 2.0f);
    bool led_on = fmodf(t, period) < period * (thinking || shy ? 0.5f : 0.3f);

    // looping cable from the top card into the back of the casing (drawn first so its behind)
    {
        static const int X[5] = { 29, 33, 35, 35, 33 }, Y[5] = { -2, -2, 0, 4, 6 };
        for (int k = 0; k < 2; k++)       // two parallel wires, inner one offset
            for (int i = 0; i < 4; i++)
                line(X[i] - (i ? k : 0), hy + Y[i] + k, X[i + 1] - k, hy + Y[i + 1] + k,
                     k == 0 ? 'B' : 'R');
    }

    // keyboard base, planted so it doesnt bob. the stubby feet go in last, behind it
    box(4, 34, 32, 7, 'W', 'S', true);
    hline(5, 35, 30, 'H');
    for (int r = 0; r < 2; r++)                      // key rows
        for (int k = 0; k < 14; k++) {
            int kx = 6 + r + k * 2;
            if (kx > 33) continue;
            put(kx, 36 + r, 'H');
            put(kx + 1, 36 + r, 'G');
        }
    hline(13, 38, 14, 'H');                          // space bar
    put(12, 38, 'G');
    put(27, 38, 'G');
    put(6, 36, 'R');                                 // esc
    put(32, 37, 'P');                                // enter
    put(33, 37, 'P');
    hline(5, 39, 30, 'G');                           // front lip
    for (int k = 0; k < WHEEL_KEYS; k++) {           // keys hes typing on light up (anim_wheel.c)
        char c = wheel_key_glow(&m->wheel, k);
        if (!c) continue;
        bool hot = c == 'Y';
        if (k == WHEEL_KEY_SPACE) {
            hline(13, 38, 14, hot ? 'O' : 'Y');
            put(12, 38, 'O');
            put(27, 38, 'O');
        } else if (k == WHEEL_KEY_ENTER) {
            put(32, 37, hot ? 'w' : 'Y');
            put(33, 37, 'Y');
        } else {
            put(6 + k, 36 + (k & 1), hot ? 'O' : 'Y');
            put(7 + k, 36 + (k & 1), hot ? 'O' : 'G');
        }
    }

    // stand (stretches with the bob)
    fill(16, hy + 26, 8, 34 - (hy + 26), 'M');
    vline(18, hy + 26, 34 - (hy + 26), 'S');
    hline(14, 33, 12, 'S');

    // casing, the whole computer is his body
    box(7, hy, 26, 27, 'W', 'S', true);
    put(8, hy + 1, 'S');
    put(31, hy + 1, 'S');
    put(8, hy + 25, 'S');
    put(31, hy + 25, 'S');
    hline(9, hy + 1, 22, 'H');
    vline(8, hy + 2, 22, 'H');
    hline(9, hy + 25, 22, 'G');
    vline(31, hy + 2, 23, 'G');

    // screen, dark bezel and teal glass with rounded corners, glint, scanlines
    box(9, hy + 2, 22, 18, 'T', 'K', true);
    for (int r = hy + 4; r < hy + 19; r += 2) hline(10, r, 20, 'd');
    put(10, hy + 3, 'D');
    put(29, hy + 3, 'D');
    put(10, hy + 18, 'D');
    put(29, hy + 18, 'D');
    hline(11, hy + 3, 3, 'U');
    put(11, hy + 4, 'U');
    hline(9, hy + 20, 22, 'G');                      // bezel shadow

    // lower panel (rainbow badge, speaker grille, floppy drive)
    put(10, hy + 22, 'R');
    put(11, hy + 22, 'P');
    put(10, hy + 23, 'E');
    put(11, hy + 23, 'B');
    for (int i = 0; i < 3; i++) {
        static const int LVL[3][3] = { { 1, 3, 2 }, { 3, 2, 3 }, { 2, 1, 1 } };
        int gx = 13 + i * 2;
        vline(gx, hy + 21, 3, 'M');
        if (talking)
            for (int k = 0; k < LVL[phase][i]; k++) put(gx, hy + 23 - k, k == 2 ? 'Y' : 'O');
    }
    box(20, hy + 21, 11, 4, 'G', 'M', false);        // floppy drive
    hline(21, hy + 22, 7, 'K');
    hline(28, hy + 23, 2, 'S');                      // eject button
    {
        bool drive = thinking ? fmodf(t, 0.24f) < 0.12f
                              : (talking ? phase == 1 : fmodf(t, 5.0f) < 0.25f);
        put(21, hy + 23, drive ? 'E' : 'e');
    }

    // network card on top with its LED
    box(23, hy - 3, 7, 4, 'W', 'S', true);
    hline(24, hy - 2, 5, 'H');
    put(25, hy - 1, led_on ? 'L' : 'l');
    if (led_on) put(25, hy - 2, 'Y');
    put(27, hy - 1, 'E');

    // face, the first hook that wants it gets to draw it (order is in mascot_kit.h)
    Face f = {
        .t = t, .hy = hy, .lx = 11, .rx = 22, .ly = hy + 5, .my = hy + 15,
        .glass_x = 10, .glass_y = hy + 3, .glass_w = 20, .glass_h = 16,
        .lh = 30, .rh = 30,
    };
    default_arms(m, &f, phase);
    if (bye_face(m, &f)) {}
    else if (shy) lock_screen(hy, t, m->typed);
    else if (physics_face(m, &f)) {}
    else if (wheel_face(m, &f)) {}
    else if (point_face(m, &f)) {}
    else if (asking || m->mood == MOOD_HAPPY) builtin_face(m, &f, phase);
    else if ((idle || talking) && emotion_face(m, &f)) {}
    else if (!idle) builtin_face(m, &f, phase);
    else if (sleep_face(m, &f)) {}
    else if (gaze_face(m, &f)) {}
    else builtin_face(m, &f, phase);

    glow_pass(f.glass_x, f.glass_y, f.glass_w, f.glass_h);

    // arms, plugged into the side ports at mid screen height
    const int port = hy + 12;
    int type_side = 0, type_x = 0, type_y = 0;       // a hand typing on the keys (anim_wheel.c)
    if (!wheel_typing_hand(&m->wheel, &type_side, &type_x, &type_y)) type_side = 0;
    if (type_side < 0 && pose.arm_out >= 0) typing_arm(-1, port, type_x, type_y, 'B');
    else arm(-1, port, clampi(f.lh, hy - 1, 30), 'B', pose.arm_out < 0);
    if (type_side > 0 && pose.arm_out <= 0) typing_arm(+1, port, type_x, type_y, 'P');
    else arm(+1, port, clampi(f.rh, hy - 1, 30), 'P', pose.arm_out > 0);

    // extras on top (z's, sparkles, sweat drops, stars...)
    sleep_overlay(m, &f);
    emotion_overlay(m, &f);
    gaze_overlay(m, &f);
    point_overlay(m, &f);
    wheel_overlay(m, &f);
    physics_overlay(m, &f);
    bye_overlay(m, &f);

    // walking, the upper body leans over the base and everything bounces off the feet
    shift_rows_x(0, 32, pose.lean);
    shift_up(pose.lift);
    feet(&pose);

    auto_outline();
    last_frame = (MkFrame){
        .x = x + pose.shift_x * px, .y = y + pose.shift_y * px, .px = px,
        .hy = hy - pose.lift, .port_y = port - pose.lift,
        .lean = pose.lean, .lift = pose.lift, .arm_out = pose.arm_out,
    };
    ByeCrt crt = bye_crt(m);
    if (crt.stage > 0) switch_off(&crt, x + pose.shift_x * px, y + pose.shift_y * px, px, hy);
    else ui_draw_cells_pal(&grid[0][0], MASCOT_W, MASCOT_H, x + pose.shift_x * px, y + pose.shift_y * px,
                           px, palette, true);
}
