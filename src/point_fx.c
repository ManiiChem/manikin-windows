// everything outside his 40x48 canvas while he points or drives (cable arm and plug, the ring,
// marks over his head, dust and speed streaks). drawn like the mascot, palette letters on his
// 3 px grid with a dark outline and white halo so it lines up pixel for pixel. the halo and
// outline never paint over his body so the cable comes straight out of his port. drawing
// only, the moving parts live in anim_point.c and anim_wheel.c

#include <math.h>
#include <string.h>

#include "point_fx.h"
#include "mascot_kit.h"

// ---- tuning

#define BIG_BOX       POINTER_BIG_BOX    // px, a box bigger than this gets brackets around the point
#define FOCUS_W       POINTER_FOCUS_W     // in a patch this big (or the box if its smaller)
#define FOCUS_H       POINTER_FOCUS_H
#define MIN_BOX       18.0f     // px, tiny controls still get a ring at least this big
#define EDGE          6.0f      // px the ring stays inside the window
#define BODY_GAP      9.0f      // px it stays off his body
#define TIP_GAP       3         // cells between the ring line and the plug tip
#define OVERSHOOT_MAX 2.0f      // cells the plug can overshoot into that gap
#define RING_REACT    0.34f     // how long the ring reacts to a result
#define STREAK_SPEED  950.0f    // px/s where speed streaks start

// ---- the layer grid

#define LW 200
#define LH 230
static char layer[LH][LW];
static bool layer_ready;
static int ox, oy, cpx;          // window px of layer cell (0, 0), and the cell size
static int cox, coy;             // layer cell of canvas cell (0, 0)
static int bx0, by0, bx1, by1;   // bounding box of whats been drawn (cells)

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int roundi(float v) { return (int)floorf(v + 0.5f); }

// line the layer up with a canvas drawn at (x, y) so canvas cells and layer cells match
static void layer_begin(int x, int y, int px)
{
    if (!layer_ready) {
        memset(layer, '.', sizeof layer);
        layer_ready = true;
    }
    cpx = px;
    cox = (x + px - 1) / px + 2;     // enough cells to reach the windows left edge
    coy = (y + px - 1) / px + 2;
    ox = x - cox * px;
    oy = y - coy * px;
    bx0 = LW; by0 = LH; bx1 = -1; by1 = -1;
}

static void lput(int x, int y, char c)
{
    if (x < 1 || y < 1 || x >= LW - 1 || y >= LH - 1) return;   // keep a margin for the outline
    layer[y][x] = c;
    if (x < bx0) bx0 = x;
    if (y < by0) by0 = y;
    if (x > bx1) bx1 = x;
    if (y > by1) by1 = y;
}

static char lget(int x, int y)
{
    if (x < 0 || y < 0 || x >= LW || y >= LH) return '.';
    return layer[y][x];
}

static void cput(int cx, int cy, char c) { lput(cx + cox, cy + coy, c); }

static void lstamp(int x, int y, const char *rows[], int h, bool flip)
{
    for (int j = 0; j < h; j++) {
        int w = (int)strlen(rows[j]);
        for (int i = 0; i < w; i++) {
            char c = rows[j][flip ? w - 1 - i : i];
            if (c != '.') lput(x + i, y + j, c);
        }
    }
}

// window px -> layer cell coords (float, cell (i, j) spans i..i+1)
static float cell_x(float wx) { return (wx - (float)ox) / (float)cpx; }
static float cell_y(float wy) { return (wy - (float)oy) / (float)cpx; }

// is there body art under layer cell (x, y)? (the mascot gets drawn right before this)
static bool body_at(int x, int y)
{
    char c = mk_get(x - cox, y - coy);
    return c != '.';
}

// outline, halo, paint, clear. front = drawn over the mascot so keep off his body.
// soft = no outline or halo (for dust, a cloud with an ink line around it looks like a pebble)
static void layer_end_ex(bool front, bool soft)
{
    if (bx1 < bx0) return;
    int x0 = bx0 - 1, y0 = by0 - 1, x1 = bx1 + 1, y1 = by1 + 1;

    // dark outline around everything (same rule as the mascots auto_outline)
    static char ring_k[LH][LW];
    for (int y = soft ? y1 + 1 : y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            ring_k[y][x] = 0;
            if (layer[y][x] != '.') continue;
            static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
            for (int k = 0; k < 4; k++) {
                char c = lget(x + dx[k], y + dy[k]);
                if (c != '.' && c != 'K') { ring_k[y][x] = 1; break; }
            }
        }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (!soft && ring_k[y][x]) layer[y][x] = 'K';

    // white halo on every empty cell touching art, except over his body
    const Color halo = { 255, 255, 255, 255 };
    for (int y = soft ? y1 + 2 : y0 - 1; y <= y1 + 1; y++)
        for (int x = x0 - 1; x <= x1 + 1; x++) {
            if (lget(x, y) != '.') continue;
            bool near = false;
            for (int j = -1; j <= 1 && !near; j++)
                for (int i = -1; i <= 1 && !near; i++)
                    if (lget(x + i, y + j) != '.') near = true;
            if (!near || (front && body_at(x, y))) continue;
            DrawRectangle(ox + x * cpx, oy + y * cpx, cpx, cpx, halo);
        }

    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            char c = layer[y][x];
            if (c == '.') continue;
            if (front && c == 'K' && body_at(x, y)) continue;     // dont cut into his body
            DrawRectangle(ox + x * cpx, oy + y * cpx, cpx, cpx, mk_color(c));
        }

    for (int y = y0; y <= y1; y++) memset(&layer[y][x0], '.', (size_t)(x1 - x0 + 1));
    bx0 = LW; by0 = LH; bx1 = -1; by1 = -1;
}

static void layer_end(bool front) { layer_end_ex(front, false); }

// ---- stamps

// the plug "hand" pointing right (+x), collar, metal shell, dark face plate and 2 prongs.
// rows 1..2 meet the cable, '#' is the arms colour and '1' / '2' the glowing prong tips
#define PLUG_W 7
#define PLUG_H 4
static const char *PLUG[PLUG_H] = {
    "MWHHWS1",
    "#HWWGM.",
    "#WGGSM.",
    "MGSSSS2",
};

static const char *STAR[] = {
    ".w.",
    "wYw",
    ".w.",
};

static const char *STAR_BIG[] = {
    "..w..",
    "..w..",
    "wwYww",
    "..w..",
    "..w..",
};

static const char *HUH[] = {
    ".YYY.",
    "YO.OO",
    "...OO",
    "..OO.",
    "..OO.",
    ".....",
    "..OO.",
};

static const char *BANG[] = {
    "YO",
    "YO",
    "YO",
    "YO",
    "..",
    "YO",
};

static const char *NOTE[] = {
    "..YYYY",
    "..OYYY",
    "..O..Y",
    "..O..Y",
    "YOO..Y",
    "OOO.YY",
    ".O..YY",
};

// plug with its pin tips at cell boundary (tx, ty) (the cables centre line), facing axis (dx, dy)
static void plug(int tx, int ty, int dx, int dy, char arm, char tip1, char tip2)
{
    for (int j = 0; j < PLUG_H; j++)
        for (int i = 0; i < PLUG_W; i++) {
            char c = PLUG[j][i];
            if (c == '.') continue;
            if (c == '#') c = arm;
            else if (c == '1') c = tip1;
            else if (c == '2') c = tip2;
            int x, y;
            if (dx > 0)      { x = tx - PLUG_W + i; y = ty - 2 + j; }
            else if (dx < 0) { x = tx + PLUG_W - 1 - i; y = ty - 2 + j; }
            else if (dy > 0) { x = tx - 2 + j; y = ty - PLUG_W + i; }
            else             { x = tx - 2 + j; y = ty + PLUG_W - 1 - i; }
            lput(x, y, c);
        }
}

// ---- the cable

typedef struct { float x, y; } V2;

static V2 bez(V2 a, V2 b, V2 c, V2 d, float t)
{
    float u = 1.0f - t;
    float k0 = u * u * u, k1 = 3.0f * u * u * t, k2 = 3.0f * u * t * t, k3 = t * t * t;
    return (V2){ k0 * a.x + k1 * b.x + k2 * c.x + k3 * d.x, k0 * a.y + k1 * b.y + k2 * c.y + k3 * d.y };
}

// 2 cell ribbed cable like his arms. lit side (W/S ribs) on top, or on the left when it
// runs vertical, and the arms colour (M ribs) on the other
static void cable(V2 p0, V2 p1, V2 p2, V2 p3, char arm)
{
    float len = 0.0f;
    V2 prev = p0;
    for (int i = 1; i <= 16; i++) {
        V2 q = bez(p0, p1, p2, p3, (float)i / 16.0f);
        len += sqrtf((q.x - prev.x) * (q.x - prev.x) + (q.y - prev.y) * (q.y - prev.y));
        prev = q;
    }
    int n = (int)(len * 4.0f) + 2;
    float s = 0.0f;
    prev = p0;
    for (int i = 0; i <= n; i++) {
        float t = (float)i / (float)n;
        V2 q = bez(p0, p1, p2, p3, t);
        V2 q2 = bez(p0, p1, p2, p3, fminf(1.0f, t + 0.02f));
        V2 q1 = bez(p0, p1, p2, p3, fmaxf(0.0f, t - 0.02f));
        float tx = q2.x - q1.x, ty = q2.y - q1.y;
        s += sqrtf((q.x - prev.x) * (q.x - prev.x) + (q.y - prev.y) * (q.y - prev.y));
        prev = q;
        bool rib = ((int)floorf(s) & 1) != 0;
        char lit = rib ? 'S' : 'W', col = rib ? 'M' : arm;
        if (fabsf(tx) >= fabsf(ty)) {
            int cx = (int)floorf(q.x), r = roundi(q.y);
            lput(cx, r - 1, lit);
            lput(cx, r, col);
        } else {
            int cy = (int)floorf(q.y), c = roundi(q.x);
            lput(c - 1, cy, lit);
            lput(c, cy, col);
        }
    }
}

// ---- where to point

// his body as just drawn, in window px (same rect the overlay treats as him)
static Rectangle body_rect(void)
{
    const MkFrame *fr = mk_frame();
    return (Rectangle){ (float)(fr->x + 4 * fr->px), (float)fr->y,
                        (float)((MASCOT_W - 8) * fr->px), (float)((MASCOT_H - 2) * fr->px) };
}

// the part of the control to ring in window px, the box itself (at least MIN_BOX) or a
// patch around the target for a huge one, clipped to the window. `big` says which
static Rectangle focus_rect(Rectangle box, Vector2 target, Rectangle body, bool *big)
{
    const float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
    Rectangle r = box;
    // a control hes partly standing on gets brackets too (a ring around it would cross his face),
    // same for one cut off by the window edge (a ring line there would look like its real edge)
    bool cut = box.x < EDGE || box.y < EDGE || box.x + box.width > W - EDGE || box.y + box.height > H - EDGE;
    *big = box.width > BIG_BOX || box.height > BIG_BOX || cut || CheckCollisionRecs(box, body);
    if (*big) {
        float w = fminf(box.width, FOCUS_W), h = fminf(box.height, FOCUS_H);
        float x = clampf(target.x - w * 0.5f, box.x, box.x + box.width - w);
        float y = clampf(target.y - h * 0.5f, box.y, box.y + box.height - h);
        // a button thats only wide (not a whole row) is one target, so bracket the end
        // nearest him and the plug stops at its edge instead of lying across its label
        if (box.width <= BIG_BOX && box.height <= BIG_BOX && box.height <= FOCUS_H * 1.5f) {
            if (box.x + box.width <= body.x) x = fmaxf(box.x, box.x + box.width - w);
            else if (box.x >= body.x + body.width) x = box.x;
        }
        r = (Rectangle){ x, y, w, h };
        // pull it back off him if the patch still reaches under him
        if (CheckCollisionRecs(r, body)) {
            if (target.x < body.x) r.width = fminf(r.width, body.x - BODY_GAP - r.x);
            else if (target.x > body.x + body.width) {
                float x1 = r.x + r.width;
                r.x = fmaxf(r.x, body.x + body.width + BODY_GAP);
                r.width = x1 - r.x;
            } else if (target.y < body.y) r.height = fminf(r.height, body.y - BODY_GAP - r.y);
            else {
                float y1 = r.y + r.height;
                r.y = fmaxf(r.y, body.y + body.height + BODY_GAP);
                r.height = y1 - r.y;
            }
        }
    }
    if (r.width < MIN_BOX) { r.x += (r.width - MIN_BOX) * 0.5f; r.width = MIN_BOX; }
    if (r.height < MIN_BOX) { r.y += (r.height - MIN_BOX) * 0.5f; r.height = MIN_BOX; }
    float x0 = fmaxf(r.x, EDGE), y0 = fmaxf(r.y, EDGE);
    float x1 = fminf(r.x + r.width, W - EDGE), y1 = fminf(r.y + r.height, H - EDGE);
    if (x1 - x0 < MIN_BOX) {           // mostly off the window, ring whats left of it
        float cx = clampf(target.x, EDGE + MIN_BOX * 0.5f, W - EDGE - MIN_BOX * 0.5f);
        x0 = cx - MIN_BOX * 0.5f; x1 = cx + MIN_BOX * 0.5f;
    }
    if (y1 - y0 < MIN_BOX) {
        float cy = clampf(target.y, EDGE + MIN_BOX * 0.5f, H - EDGE - MIN_BOX * 0.5f);
        y0 = cy - MIN_BOX * 0.5f; y1 = cy + MIN_BOX * 0.5f;
    }
    return (Rectangle){ x0, y0, x1 - x0, y1 - y0 };
}

// the ring line in layer cells, one gap cell (the rings inner outline) between it and r
typedef struct { int l, t, r, b; } Cells;

static Cells ring_cells(Rectangle r)
{
    Cells c;
    c.l = (int)floorf(cell_x(r.x)) - 2;
    c.t = (int)floorf(cell_y(r.y)) - 2;
    c.r = (int)ceilf(cell_x(r.x + r.width)) + 1;
    c.b = (int)ceilf(cell_y(r.y + r.height)) + 1;
    return c;
}

// box and target (window px) as they were when the aim froze, the window has moved on since
static void aim_frame(const PointerView *v, const PointState *s, Rectangle *box, Vector2 *target)
{
    *box = v->box;
    *target = v->target;
    if (s->latched) {
        float dx = s->trip_x - s->aim_trip_x, dy = s->trip_y - s->aim_trip_y;
        box->x += dx;
        box->y += dy;
        target->x += dx;
        target->y += dy;
    }
}

// ---- the ring

// rounded rect with the bubbles notched corners, or just corner brackets that long if
// `brackets` > 0. `dash` > 0 leaves gaps, `chase` runs 2 sparks around, `corners_hot` lights the corners
static void ring_shape(Cells c, int brackets, int dash, char col, char hot, float chase, bool corners_hot)
{
    int w = c.r - c.l - 3, h = c.b - c.t - 3;   // straight cells per side
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    int per = 2 * (w + h) + 4;
    int head = ((int)floorf(chase)) % per;
    for (int k = 0; k < per; k++) {
        int x, y, i, len;
        bool corner = false;
        int q = k;
        if (q < w) { x = c.l + 2 + q; y = c.t; i = q; len = w; }
        else if ((q -= w) == 0) { x = c.r - 1; y = c.t + 1; corner = true; i = len = 0; }
        else if (--q < h) { x = c.r; y = c.t + 2 + q; i = q; len = h; }
        else if ((q -= h) == 0) { x = c.r - 1; y = c.b - 1; corner = true; i = len = 0; }
        else if (--q < w) { x = c.r - 2 - q; y = c.b; i = q; len = w; }
        else if ((q -= w) == 0) { x = c.l + 1; y = c.b - 1; corner = true; i = len = 0; }
        else if (--q < h) { x = c.l; y = c.b - 2 - q; i = q; len = h; }
        else { x = c.l + 1; y = c.t + 1; corner = true; i = len = 0; }
        if (!corner && brackets > 0 && i >= brackets && i < len - brackets) continue;
        if (!corner && dash > 0 && ((k / dash) & 1)) continue;
        int d = (k - head + per) % per, d2 = (k - head + per / 2 + per) % per;
        char ch = (d < 3 || d2 < 3 || (corner && corners_hot)) ? hot : col;
        lput(x, y, ch);
    }
}

static Cells grow(Cells c, int d) { return (Cells){ c.l - d, c.t - d, c.r + d, c.b + d }; }

static void draw_ring(const PointerView *v, const PointState *s, float time)
{
    if (v->box.width <= 0.0f || v->box.height <= 0.0f) return;
    bool big;
    Rectangle r = focus_rect(v->box, v->target, body_rect(), &big);
    Cells c = ring_cells(r);
    int w = c.r - c.l, h = c.b - c.t;
    int brackets = big ? clampi((w < h ? w : h) / 3, 2, 5) : 0;
    const float u = s->result_t;

    if (s->result == POINTER_RESULT_NONE) {
        if (!s->locked || v->phase != POINTER_AT) return;
        // lock on, snaps in from big with a little overshoot
        static const int SNAP[6] = { 6, 4, 2, 1, -1, 0 };
        int f = (int)(s->lock_t / 0.035f);
        int off = f < 6 ? SNAP[f] : 0;
        // then a slow breath (one cell out every 1.4 s) and 2 sparks chasing around
        if (f >= 6 && fmodf(s->lock_t, 1.4f) > 1.0f) off = 1;
        Cells g = grow(c, off);
        ring_shape(g, brackets, 0, 'O', 'Y', time * 16.0f, true);
        return;
    }
    if (u > RING_REACT) return;
    switch (s->result) {
    case POINTER_RESULT_CLICKED_IN: {            // flash green, then burst outward
        if (u < 0.09f) ring_shape(c, brackets, 0, 'E', 'w', 0.0f, true);
        else if (u < 0.17f) ring_shape(grow(c, u < 0.13f ? 1 : 2), brackets, 2, 'E', 'E', 0.0f, false);
        // sparkles flying off the corners
        float d = 1.0f + (u - 0.07f) * 26.0f;
        int sx[4] = { c.l, c.r, c.l, c.r }, sy[4] = { c.t, c.t, c.b, c.b };
        int ex[4] = { -1, 1, -1, 1 }, ey[4] = { -1, -1, 1, 1 };
        for (int i = 0; i < 4 && u >= 0.07f; i++) {
            int x = sx[i] + roundi(ex[i] * d), y = sy[i] + roundi(ey[i] * d * 0.7f);
            if (u < 0.17f) lstamp(x - 2, y - 2, STAR_BIG, 5, false);
            else if (u < 0.26f) lstamp(x - 1, y - 1, STAR, 3, false);
            else lput(x, y, 'w');
        }
        break;
    }
    case POINTER_RESULT_CLICKED_OUT:             // a dim double blink, gone
        if (((int)(u / 0.07f) & 1) == 0 && u < 0.28f) ring_shape(c, brackets, 0, 'S', 'G', 0.0f, false);
        break;
    case POINTER_RESULT_MOVED:                   // red flicker, gone
        if (u < 0.12f && ((int)(u / 0.04f) & 1) == 0) ring_shape(c, brackets, 0, 'L', 'L', 0.0f, false);
        break;
    default:                                     // fades out through dashes
        if (u < 0.12f) ring_shape(c, brackets, 2, 'O', 'O', 0.0f, false);
        else if (u < 0.24f) ring_shape(c, 2, 0, 'O', 'O', 0.0f, false);
        break;
    }
}

// ---- the arm

static void draw_arm(const PointerView *v, const Mascot *m, int mascot_x, int mascot_y, float time)
{
    const PointState *s = &m->point;
    const MkFrame *fr = mk_frame();
    if (!s->out || fr->arm_out == 0) return;
    const int side = fr->arm_out;
    const char arm = side < 0 ? 'B' : 'P';

    // the cable leaves the port horizontally from the column next to the plug
    V2 p0 = { (float)((side < 0 ? 4 : MASCOT_W - 1 - 4) + fr->lean + cox) + 0.5f, (float)(fr->port_y + 1 + coy) };

    // the target as hes aiming at it
    Rectangle box;
    Vector2 tgt;
    aim_frame(v, s, &box, &tgt);
    tgt.x = (float)mascot_x + 20.0f * (float)fr->px + s->aim_x;
    tgt.y = (float)mascot_y + 16.0f * (float)fr->px + s->aim_y;
    bool big;
    Rectangle r = box.width > 0.0f ? focus_rect(box, tgt, body_rect(), &big)
                                   : (Rectangle){ tgt.x - MIN_BOX / 2, tgt.y - MIN_BOX / 2, MIN_BOX, MIN_BOX };
    Cells c = ring_cells(r);
    float tcx = cell_x(tgt.x), tcy = cell_y(tgt.y);

    // come in from the side facing him, horizontal if theres room, else from below/above
    float gx = side > 0 ? (float)c.l - p0.x : p0.x - (float)(c.r + 1);
    float gy = p0.y > (float)c.b ? p0.y - (float)c.b : ((float)c.t > p0.y ? (float)c.t - p0.y : 0.0f);
    int dx = 0, dy = 0, tx, ty;
    if (gx > 5.0f && gx >= gy * 0.6f) {
        dx = side;
        tx = side > 0 ? c.l - TIP_GAP : c.r + 1 + TIP_GAP;
        ty = clampi(roundi(tcy), c.t + 2, c.b - 1);
        if (c.b - c.t < 4) ty = (c.t + c.b + 1) / 2;
    } else {
        dy = p0.y > tcy ? -1 : 1;
        ty = dy < 0 ? c.b + 1 + TIP_GAP : c.t - TIP_GAP;
        tx = clampi(roundi(tcx), c.l + 2, c.r - 1);
        if (c.r - c.l < 4) tx = (c.l + c.r + 1) / 2;
    }

    // the cable end (back of the plug), extended by the reach spring
    V2 end = { (float)(tx - dx * PLUG_W), (float)(ty - dy * PLUG_W) };
    float lx = end.x - p0.x, ly = end.y - p0.y;
    float len = sqrtf(lx * lx + ly * ly);
    float e = s->reach;
    V2 pe;
    if (e <= 1.0f) {
        pe = (V2){ p0.x + lx * e, p0.y + ly * e };
    } else {
        float over = fminf((e - 1.0f) * len, OVERSHOOT_MAX);
        pe = (V2){ end.x + (float)dx * over, end.y + (float)dy * over };
    }
    int ptx = roundi(pe.x) + dx * PLUG_W, pty = roundi(pe.y) + dy * PLUG_W;
    pe.x = (float)(ptx - dx * PLUG_W);
    pe.y = (float)(pty - dy * PLUG_W);
    if (dx != 0) pe.x -= 0.5f * (float)dx;        // horizontal sampling covers cells left of x
    else pe.y -= 0.5f * (float)dy;

    // lazy curve, straight out of the port with a bit of sag and sway, straight into the plug
    float ex = pe.x - p0.x, ey = pe.y - p0.y;
    float el = sqrtf(ex * ex + ey * ey);
    float k1 = clampf(el * 0.45f, 1.0f, 18.0f), k2 = clampf(el * 0.35f, 1.0f, 14.0f);
    float sag = clampf(el * 0.10f, 0.0f, 6.0f) * clampf(1.4f - fabsf(s->reach_v) * 0.1f, 0.3f, 1.0f);
    float sway = (el > 12.0f ? 0.8f : 0.0f) * sinf(time * 2.3f);
    V2 p1 = { p0.x + (float)side * k1, p0.y + sag + sway };
    V2 p2 = { pe.x - (float)dx * k2, pe.y - (float)dy * k2 + sag * (dy == 0 ? 1.0f : 0.0f) - sway * 0.5f };
    if (el > 1.0f) cable(p0, p1, p2, pe, arm);

    // tips pulse while pointing, go green when it worked and red when it didnt
    char t1 = 'Y', t2 = 'O';
    if (fmodf(time, 0.5f) < 0.25f) { t1 = 'O'; t2 = 'Y'; }
    if (s->result == POINTER_RESULT_CLICKED_IN) { t1 = 'w'; t2 = 'E'; }
    else if (s->result == POINTER_RESULT_MOVED) { t1 = 'L'; t2 = 'l'; }
    else if (s->result == POINTER_RESULT_CLICKED_OUT) { t1 = 'S'; t2 = 'S'; }
    plug(ptx, pty, dx, dy, arm, t1, t2);
}

// ---- over his head

static void draw_head_marks(const Mascot *m)
{
    const PointState *s = &m->point;
    const MkFrame *fr = mk_frame();
    const float u = s->result_t;
    const int top = fr->hy - 3;                       // top row of the network card
    const PointMark mark = point_head_mark(m->point_phase, s->result, u, NULL);
    if (mark == POINT_MARK_HUH) {
        int pop = u < 0.12f ? 1 : 0;
        int x = s->side > 0 ? 10 : 25;                // over the free hand
        if (s->side == 0) x = 17;
        lstamp(x + cox, top - 9 + pop + coy, HUH, 7, false);
    }
    if (mark == POINT_MARK_BANG) {
        int pop = u < 0.1f ? 1 : 0;
        lstamp(19 + cox, top - 8 + pop + coy, BANG, 6, false);
        if (u < 0.22f) {                              // emphasis ticks
            cput(15, top - 6, 'w');
            cput(16, top - 5, 'w');
            cput(24, top - 6, 'w');
            cput(23, top - 5, 'w');
        }
    }
    // whistling on the way home after a job well done
    if (mark == POINT_MARK_NOTE) {
        float a = fmodf(u - POINT_NOTE_FROM, POINT_NOTE_EVERY);
        int x = (s->dir > 0 ? 8 : 27) + (((int)(a / 0.3f) & 1) ? 1 : 0);
        lstamp(x + cox, top - 8 - (int)(a * 6.0f) + coy, NOTE, 7, s->dir < 0);
    }
}

// ---- driving, the cable on the real pointer

// cable from his port to the plug, tips on the pointers hotspot. taut while he steers (shivers
// on a heavy haul), slack and swinging while it trails your hand (WheelState droop spring)
static void draw_drive_arm(const Mascot *m, int mascot_x, int mascot_y, float time)
{
    const WheelState *w = &m->wheel;
    const MkFrame *fr = mk_frame();
    if (!w->out || fr->arm_out == 0) return;
    const int side = fr->arm_out;
    const char arm = side < 0 ? 'B' : 'P';
    const float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();

    V2 p0 = { (float)((side < 0 ? 4 : MASCOT_W - 1 - 4) + fr->lean + cox) + 0.5f, (float)(fr->port_y + 1 + coy) };

    // the pointer (window px), kept on the window and off his body
    float hx = (float)mascot_x + 20.0f * (float)fr->px + w->aim_x;
    float hy = (float)mascot_y + 16.0f * (float)fr->px + w->aim_y;
    hx = clampf(hx, EDGE, W - EDGE);
    hy = clampf(hy, EDGE, H - EDGE);
    Rectangle body = body_rect();
    if (CheckCollisionPointRec((Vector2){ hx, hy }, body))
        hx = side > 0 ? body.x + body.width + BODY_GAP : body.x - BODY_GAP;
    int tx = roundi(cell_x(hx)), ty = roundi(cell_y(hy));

    // in from his side if theres room, else from below/above
    float gx = side > 0 ? (float)tx - p0.x : p0.x - (float)tx, gy = fabsf((float)ty - p0.y);
    int dx = 0, dy = 0;
    if (gx > 4.0f && gx >= gy * 0.5f) dx = side;
    else dy = (float)ty < p0.y ? -1 : 1;

    // on a click the plug strikes, recoils a cell and settles
    float cu = w->click_left > 0.0f ? WHEEL_CLICK - w->click_left : 1.0f;
    int jab = cu >= 0.04f && cu < 0.12f ? -1 : 0;

    // the cable end (back of the plug), extended by the reach spring
    V2 end = { (float)(tx - dx * PLUG_W), (float)(ty - dy * PLUG_W) };
    float lx = end.x - p0.x, ly = end.y - p0.y;
    float len = sqrtf(lx * lx + ly * ly);
    float e = w->reach;
    V2 pe;
    if (e <= 1.0f) {
        pe = (V2){ p0.x + lx * e, p0.y + ly * e };
    } else {
        float over = fminf((e - 1.0f) * len, OVERSHOOT_MAX);
        pe = (V2){ end.x + (float)dx * over, end.y + (float)dy * over };
    }
    int ptx = roundi(pe.x) + dx * (PLUG_W + jab), pty = roundi(pe.y) + dy * (PLUG_W + jab);
    pe.x = (float)(ptx - dx * PLUG_W);
    pe.y = (float)(pty - dy * PLUG_W);
    if (dx != 0) pe.x -= 0.5f * (float)dx;        // horizontal sampling covers cells left of x
    else pe.y -= 0.5f * (float)dy;

    // slack hangs and sways, taut pulls nearly straight
    const float T = w->tension;
    float ex = pe.x - p0.x, ey = pe.y - p0.y;
    float el = sqrtf(ex * ex + ey * ey);
    float k1 = clampf(el * 0.45f, 1.0f, 18.0f) * (1.0f - 0.6f * T);
    float k2 = clampf(el * 0.35f, 1.0f, 14.0f) * (1.0f - 0.6f * T);
    float sx = w->droop_x / (float)cpx * (4.0f / 3.0f), sy = w->droop_y / (float)cpx * (4.0f / 3.0f);
    float sway = (1.0f - T) * (el > 12.0f ? 0.9f : 0.0f) * sinf(time * 2.3f);
    if (w->hauling && el > 4.0f) {                 // the taut cable shivers with the strain
        float shiver = ((int)(time / 0.05f) & 1) ? 0.7f : -0.7f;
        sx += -ey / el * shiver;
        sy += ex / el * shiver;
    }
    V2 p1 = { p0.x + (float)side * k1 + sx, p0.y + sy + sway };
    V2 p2 = { pe.x - (float)dx * k2 + sx, pe.y - (float)dy * k2 + sy - sway * 0.5f };
    // your pointer above his head (the ask bubble), the cable goes up and over, not across his face
    const float head = (float)(fr->hy - 4 + coy);     // the row above his network card
    if (dy < 0 && pe.y < head + 2.0f) {
        p1 = (V2){ p0.x + (float)side * clampf(el * 0.3f, 5.0f, 10.0f), fminf(p0.y, head - 1.0f) + sway * 0.3f };
        p2 = (V2){ pe.x + sx * 0.3f, fminf(pe.y + k2, head - 1.0f) };
    }
    if (el > 1.0f) cable(p0, p1, p2, pe, arm);

    // tips pulse while he steers, stay steady while it trails you, flash on the grab and clicks
    char t1 = 'Y', t2 = 'O';
    if (T > 0.5f && fmodf(time, 0.5f) < 0.25f) { t1 = 'O'; t2 = 'Y'; }
    if (T <= 0.5f) t1 = t2 = 'O';
    if ((w->gripped && w->grip_t < 0.12f) || cu < 0.1f) t1 = t2 = 'w';
    plug(ptx, pty, dx, dy, arm, t1, t2);

    // the grab, 4 little strokes flying off the tip
    if (w->gripped && w->grip_t < 0.18f && w->latched) {
        int d = 2 + (int)(w->grip_t / 0.04f);
        for (int k = 0; k < 4; k++) {
            int sx4 = (k & 1) ? 1 : -1, sy4 = (k & 2) ? 1 : -1;
            int x = tx - (sx4 < 0 ? 1 : 0), y = ty - (sy4 < 0 ? 1 : 0);
            lput(x + sx4 * d, y + sy4 * d, 'w');
            if (w->grip_t < 0.1f) lput(x + sx4 * (d + 1), y + sy4 * (d + 1), 'w');
        }
    }

    // a click, 3 strokes burst off the tip away from the plug (up since the arrow hangs down
    // and right of its tip, unless the plug came from above)
    if (cu < 0.12f && w->latched) {
        int d = 2 + (int)(cu / 0.03f), up = dy > 0 ? 1 : -1;
        for (int k = -1; k <= 1; k++) {
            int x = tx + k * d - (k < 0 ? 1 : 0), y = ty + up * d - (up < 0 ? 1 : 0);
            lput(x, y, 'w');
            lput(x + k, y + up, 'w');
        }
    }
}

static const char *STAR_CROSS[] = {
    "..Y..",
    ".YwY.",
    "YwwwY",
    ".YwY.",
    "..Y..",
};

// the '!' when you grab the pointer back and the stars of his victory hop
static void draw_drive_marks(const Mascot *m)
{
    const WheelState *w = &m->wheel;
    const MkFrame *fr = mk_frame();
    const int top = fr->hy - 3;                       // top row of the network card
    if (w->pause_left > 0.0f) {
        float u = WHEEL_PAUSE - w->pause_left;
        if (u > 0.06f && u < 0.75f) {
            int pop = u < 0.12f ? 1 : 0;
            lstamp(19 + cox, top - 8 + pop + coy, BANG, 6, false);
            if (u < 0.24f) {                          // emphasis ticks
                cput(15, top - 6, 'w');
                cput(16, top - 5, 'w');
                cput(24, top - 6, 'w');
                cput(23, top - 5, 'w');
            }
        }
    }
    if (w->result_left > 0.0f && w->result == DRIVE_RESULT_VICTORY) {
        float u = WHEEL_VICTORY - w->result_left - WHEEL_VICTORY_HOP;
        static const float AX[5] = { -1.0f, 1.0f, -0.55f, 0.55f, 0.0f };
        static const float AY[5] = { -0.35f, -0.35f, -0.9f, -0.9f, -1.0f };
        for (int k = 0; k < 5; k++) {
            float s = u - 0.05f * (float)k;
            if (s < 0.0f || s > 0.45f) continue;
            float d = 6.0f + s * 34.0f;
            int x = roundi(19.5f + AX[k] * (d + 8.0f)), y = roundi((float)top + 6.0f + AY[k] * d);
            if (s < 0.22f) lstamp(x - 2 + cox, y - 2 + coy, STAR_CROSS, 5, false);
            else lstamp(x - 1 + cox, y - 1 + coy, STAR, 3, false);
        }
    }
}

static bool drive_shows(const Mascot *m)
{
    const WheelState *w = &m->wheel;
    return w->out || w->pause_left > 0.0f || w->result_left > 0.0f;
}

// ---- dust and speed

// dust, a little cloud that billows then breaks into wisps. drawn for drift > 0, flipped otherwise
static const char *PUFF_S[3][3] = {
    { ".W.", "WWG", "" },
    { ".WW.", "WWGG", ".GS." },
    { "W..G.", "..G.S", "" },
};
static const char *PUFF_B[3][3] = {
    { ".WW.", "WWGG", "" },
    { "..WW..", ".WWGG.", "WGGGSS" },
    { "W...G.", "..G..S", ".S...." },
};

static void draw_puffs(const PointState *s)
{
    for (int i = 0; i < POINT_PUFFS; i++) {
        const PointPuff *p = &s->puffs[i];
        if (p->left <= 0.0f) continue;
        float k = (p->life - p->left) / p->life;
        // it stays put on screen so back it off by however far the window moved since
        float x = p->x - (s->trip_x - p->trip_x) / (float)cpx + (float)p->drift * k * (p->big ? 4.0f : 2.0f);
        float y = p->y - (s->trip_y - p->trip_y) / (float)cpx - k * 2.0f;
        int stage = k < 0.3f ? 0 : (k < 0.7f ? 1 : 2);
        const char *const *rows = p->big ? PUFF_B[stage] : PUFF_S[stage];
        int h = rows[2][0] ? 3 : 2, w = (int)strlen(rows[0]);
        int cx = roundi(x) + cox - w / 2, cy = roundi(y) + coy - h + 1;
        lstamp(cx, cy, (const char **)rows, h, p->drift < 0);
    }
}

static void draw_streaks(const PointState *s, float time)
{
    float sp = sqrtf(s->vx * s->vx + s->vy * s->vy);
    if (sp < STREAK_SPEED) return;
    float bx = -s->vx / sp, by = -s->vy / sp;         // trailing direction
    float nx = -by, ny = bx;                           // across it
    int len = clampi((int)((sp - STREAK_SPEED) / 130.0f) + 5, 5, 17);
    static const float OFF[4] = { -13.0f, -5.0f, 3.0f, 13.0f };
    static const float LEN[4] = { 0.6f, 1.0f, 0.8f, 0.5f };
    static const int PHASE[4] = { 0, 5, 2, 8 };
    int scroll = (int)(time * 30.0f);
    for (int k = 0; k < 4; k++) {
        // one long line per row, sliding back off the body and growing again
        float reach = fabsf(bx) * 19.5f + fabsf(by) * 23.0f;
        float x0 = 19.5f + bx * reach + nx * OFF[k], y0 = 24.0f + by * reach + ny * OFF[k];
        int n = (int)((float)len * LEN[k] + 0.5f);
        int gap = (scroll + PHASE[k]) % 4;            // the lines front edge slips back then snaps in
        if (n - gap < 3) continue;                    // any shorter and it looks like a bubble
        for (int i = gap; i < n; i++) {
            int x = roundi(x0 + bx * (float)i), y = roundi(y0 + by * (float)i);
            cput(x, y, 'w');
        }
    }
}

// ---- API

static bool anything(const PointerView *v, const Mascot *m)
{
    const PointState *s = &m->point;
    if (v->phase != POINTER_HOME || s->out || s->drive_walk || drive_shows(m)) return true;
    if (s->result != POINTER_RESULT_NONE && s->result_t < 0.8f) return true;
    for (int i = 0; i < POINT_PUFFS; i++)
        if (s->puffs[i].left > 0.0f) return true;
    return false;
}

void point_fx_draw_behind(const PointerView *v, const Mascot *m, int mascot_x, int mascot_y, int px, float time)
{
    if (!anything(v, m)) return;
    layer_begin(mascot_x, mascot_y, px);
    draw_puffs(&m->point);
    layer_end_ex(false, true);
    if (v->phase == POINTER_OUT || v->phase == POINTER_BACK || m->point.drive_walk) draw_streaks(&m->point, time);
    layer_end(false);
}

void point_fx_draw(const PointerView *v, const Mascot *m, int mascot_x, int mascot_y, int px, float time)
{
    if (!anything(v, m)) return;
    const MkFrame *fr = mk_frame();
    layer_begin(fr->x, fr->y, px);
    if (v->phase == POINTER_AT || v->phase == POINTER_BACK) draw_ring(v, &m->point, time);
    draw_arm(v, m, mascot_x, mascot_y, time);
    draw_drive_arm(m, mascot_x, mascot_y, time);
    draw_head_marks(m);
    draw_drive_marks(m);
    layer_end(true);
}
