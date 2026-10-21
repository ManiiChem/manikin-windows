// his app icon, cut from his idle frame so it always looks like him. pixel art blurs at in
// between scales so each size is drawn at a whole scale, centred, using the most of him that
// still fills 70% of it. so all of him with his halo (48, 64, 96, 128, 256), his head with the
// card on top but no arms or keyboard (32-40, 72, 80), his screen in its frame (24, 30), and
// that screen with the gap between his eyes squeezed so both eyes still fit (16, 20)

#include <string.h>

#include "icon.h"
#include "mascot.h"
#include "mascot_kit.h"
#include "ui.h"

const int ICON_SIZE[ICON_COUNT] = { 16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 128, 256 };

#define CAP 64

typedef struct { char c[CAP][CAP]; int w, h; } Art;
typedef struct { int from, to; } Span;      // inclusive

static char sprite[MASCOT_H][MASCOT_W];    // his finished idle frame, outline included
static int top;                            // top row of his casing
static Art body, face, screen, small, tiny;
static bool built;

static char sprite_at(int x, int y)
{
    return x < 0 || x >= MASCOT_W || y < 0 || y >= MASCOT_H ? '.' : sprite[y][x];
}

// the sprite's cells at these columns and rows in order, skipping some squeezes the cut
static void cut(Art *a, const Span *cols, int nc, const Span *rows, int nr)
{
    int xs[CAP], ys[CAP];
    a->w = a->h = 0;
    for (int i = 0; i < nc; i++)
        for (int x = cols[i].from; x <= cols[i].to && a->w < CAP; x++) xs[a->w++] = x;
    for (int i = 0; i < nr; i++)
        for (int y = rows[i].from; y <= rows[i].to && a->h < CAP; y++) ys[a->h++] = y;
    memset(a->c, '.', sizeof a->c);
    for (int j = 0; j < a->h; j++)
        for (int i = 0; i < a->w; i++) a->c[j][i] = sprite_at(xs[i], ys[j]);
}

// just mascot_draw's casing (7, top, 26x27) and the network card on it (23, top-3, 7x4),
// the arms, cable loop and keyboard go and it gets a fresh outline
static void head(Art *a)
{
    const int x0 = 6, y0 = top - 4;
    cut(a, &(Span){ x0, 33 }, 1, &(Span){ y0, top + 27 }, 1);
    for (int j = 0; j < a->h; j++)
        for (int i = 0; i < a->w; i++) {
            int x = x0 + i, y = y0 + j;
            bool casing = x >= 7 && x <= 32 && y >= top && y <= top + 26;
            bool card = x >= 23 && x <= 29 && y >= top - 3 && y <= top;
            if (!casing && !card) a->c[j][i] = '.';
        }
    static char src[CAP][CAP];              // same rule as mascot.c's auto_outline
    memcpy(src, a->c, sizeof src);
    for (int j = 0; j < a->h; j++)
        for (int i = 0; i < a->w; i++) {
            if (src[j][i] != '.') continue;
            static const int ox[4] = { 1, -1, 0, 0 }, oy[4] = { 0, 0, 1, -1 };
            for (int k = 0; k < 4; k++) {
                int ni = i + ox[k], nj = j + oy[k];
                if (ni < 0 || ni >= a->w || nj < 0 || nj >= a->h) continue;
                if (src[nj][ni] != '.' && src[nj][ni] != 'K') { a->c[j][i] = 'K'; break; }
            }
        }
}

static void build(void)
{
    // his idle frame at t = 0, eyes open looking straight ahead, no bob. mascot_draw also
    // draws while it builds so that goes to a texture nobody sees
    static Mascot m;
    mascot_init(&m);
    RenderTexture2D rt = LoadRenderTexture(MASCOT_W, MASCOT_H);
    BeginTextureMode(rt);
    mascot_draw(&m, 0, 0, 1);
    EndTextureMode();
    UnloadRenderTexture(rt);
    for (int y = 0; y < MASCOT_H; y++)
        for (int x = 0; x < MASCOT_W; x++) sprite[y][x] = mk_get(x, y);
    top = mk_frame()->hy;

    // the face, eyes at columns 11..17 and 22..28, bezel at 9 and 30, casing inner edge at
    // 8 and 31, and the glass is rows top+3 .. top+18
    static const Span ALL = { 0, CAP - 1 };
    static const Span FRAMED[1] = { { 8, 31 } };
    static const Span SQUEEZED[5] = { { 8, 9 }, { 11, 17 }, { 19, 20 }, { 22, 28 }, { 30, 31 } };
    static const Span EYES[3] = { { 11, 17 }, { 19, 20 }, { 22, 28 } };
    const Span framed_rows = { top + 1, top + 20 }, glass_rows = { top + 3, top + 18 };
    cut(&body, &ALL, 1, &ALL, 1);
    head(&face);
    cut(&screen, FRAMED, 1, &framed_rows, 1);
    cut(&small, SQUEEZED, 5, &framed_rows, 1);
    cut(&tiny, EYES, 3, &glass_rows, 1);
    built = true;
}

// the art's bounding box, 1 cell bigger all round with the halo
static void extent(const Art *a, bool halo, int *x0, int *y0, int *w, int *h)
{
    int l = CAP, t = CAP, r = -1, b = -1;
    for (int j = 0; j < a->h; j++)
        for (int i = 0; i < a->w; i++) {
            if (a->c[j][i] == '.') continue;
            if (i < l) l = i;
            if (i > r) r = i;
            if (j < t) t = j;
            if (j > b) b = j;
        }
    if (halo) { l--; t--; r++; b++; }
    *x0 = l;
    *y0 = t;
    *w = r - l + 1;
    *h = b - t + 1;
}

Image icon_image(int size)
{
    if (!built) build();
    static const struct { const Art *art; bool halo; } CHOICES[] = {
        { &body, true }, { &face, true }, { &face, false }, { &screen, false }, { &small, false }, { &tiny, false },
    };
    const int n = sizeof CHOICES / sizeof CHOICES[0];
    int pick = n - 1, x0, y0, w, h, k;
    for (int i = 0; i < n; i++) {
        extent(CHOICES[i].art, CHOICES[i].halo, &x0, &y0, &w, &h);
        int big = w > h ? w : h;
        k = size / big;
        if (k >= 1 && k * big * 10 >= size * 7) { pick = i; break; }
    }
    const Art *a = CHOICES[pick].art;
    const bool halo = CHOICES[pick].halo;
    extent(a, halo, &x0, &y0, &w, &h);
    k = size / (w > h ? w : h);
    if (k < 1) k = 1;                       // smaller than the smallest cut, it gets clipped
    int ox = (size - w * k) / 2 - x0 * k, oy = (size - h * k) / 2 - y0 * k;

    Image img = GenImageColor(size, size, BLANK);
    for (int pass = halo ? 0 : 1; pass < 2; pass++)
        for (int j = 0; j < a->h; j++)
            for (int i = 0; i < a->w; i++) {
                if (a->c[j][i] == '.') continue;
                if (pass == 0) ImageDrawRectangle(&img, ox + (i - 1) * k, oy + (j - 1) * k, 3 * k, 3 * k, UI_HALO);
                else ImageDrawRectangle(&img, ox + i * k, oy + j * k, k, k, mk_color(a->c[j][i]));
            }
    return img;
}
