// The desktop's drawing primitives (see ui.h). Integers only.

#include "ui.h"
#include "../drivers/framebuffer.h"
#include "../gfx/icons.h"
#include "../mm/heap.h"
#include "../arch/x86_64/smp.h"

static int cx0, cy0, cx1, cy1, clip_ok;

// Where drawing goes: the screen's back buffer, or a picture in memory.
static uint32_t *tg_px;              // 0: the screen
static int tg_w, tg_h;

static int target_w(void) { return tg_px ? tg_w : (int)fb_get_width(); }
static int target_h(void) { return tg_px ? tg_h : (int)fb_get_height(); }

void ui_target(uint32_t *px, int w, int h) {
    tg_px = px; tg_w = w; tg_h = h;
    ui_noclip();
}

void ui_target_screen(void) {
    tg_px = 0;
    ui_noclip();
}

// --- many cores ----------------------------------------------------------------------
//
// Almost everything here walks rows, and every row is its own business. A big
// enough piece of work is cut into bands of rows and every idle core takes one
// (smp_run); a small one is simply done, since waking the other cores costs
// more than a few thousand pixels. A band never reports to the screen and
// never allocates: the caller does both, before and after.
#define PAR_MIN_PX 20000

struct band_job { ui_band_fn fn; void *ctx; int y0, y1; };
static volatile int bands_busy;      // a job is out: anything inside it runs inline

static void band_share(int share, int n, void *arg) {
    const struct band_job *j = (const struct band_job *)arg;
    int rows = j->y1 - j->y0;
    int a = j->y0 + (int)((int64_t)rows * share / n);
    int b = j->y0 + (int)((int64_t)rows * (share + 1) / n);
    if (b > a) j->fn(a, b, share, j->ctx);
}

void ui_bands(ui_band_fn fn, void *ctx, int y0, int y1, int px_per_row) {
    if (y1 <= y0) return;
    if (bands_busy || smp_cpu_count() <= 1 ||
        (int64_t)(y1 - y0) * px_per_row < PAR_MIN_PX) {
        fn(y0, y1, 0, ctx);
        return;
    }
    struct band_job j = { fn, ctx, y0, y1 };
    bands_busy = 1;
    smp_run(band_share, &j);
    bands_busy = 0;
}

int ui_band_count(void) {
    int n = smp_cpu_count();
    return n < 1 ? 1 : n;
}

// Only what lands on the screen has to be reported to it.
static inline void mark(int x, int y, int w, int h) {
    if (!tg_px) fb_mark_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
}

void ui_noclip(void) {
    cx0 = 0; cy0 = 0;
    cx1 = target_w(); cy1 = target_h();
    clip_ok = 1;
}

void ui_clip(int x, int y, int w, int h) {
    int W = target_w(), H = target_h();
    cx0 = x < 0 ? 0 : x;
    cy0 = y < 0 ? 0 : y;
    cx1 = x + w > W ? W : x + w;
    cy1 = y + h > H ? H : y + h;
    if (cx1 < cx0) cx1 = cx0;
    if (cy1 < cy0) cy1 = cy0;
    clip_ok = 1;
}

void ui_clip_get(int r[4]) {
    if (!clip_ok) ui_noclip();
    r[0] = cx0; r[1] = cy0; r[2] = cx1; r[3] = cy1;
}

void ui_clip_set(const int r[4]) {
    cx0 = r[0]; cy0 = r[1]; cx1 = r[2]; cy1 = r[3];
    clip_ok = 1;
}

// Cut (x, y, w, h) down to the clip. 0 if nothing is left.
static int cut(int *x, int *y, int *w, int *h) {
    if (!clip_ok) ui_noclip();
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x1 <= x0 || y1 <= y0) return 0;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return 1;
}

static inline uint32_t *row_at(int y) {
    if (tg_px) return tg_px + (uint64_t)y * tg_w;
    return (uint32_t *)(fb_get_base() + (uint64_t)y * fb_get_pitch());
}

// dst toward src by a (0..255).
static inline uint32_t over(uint32_t dst, uint32_t src, uint32_t a) {
    if (a >= 255) return src & 0xFFFFFF;
    return ui_mix(dst, src, (int)(a + (a >> 7)));
}

struct fill_ctx { int x, w, t; uint32_t rgb; };

static void fill_band(int y0, int y1, int share, void *cv) {
    (void)share;
    const struct fill_ctx *c = (const struct fill_ctx *)cv;
    for (int y = y0; y < y1; y++) {
        uint32_t *d = row_at(y) + c->x;
        if (c->t < 0) for (int i = 0; i < c->w; i++) d[i] = c->rgb;
        else          for (int i = 0; i < c->w; i++) d[i] = ui_mix(d[i], c->rgb, c->t);
    }
}

void ui_fill(int x, int y, int w, int h, uint32_t rgb) {
    if (!cut(&x, &y, &w, &h)) return;
    struct fill_ctx c = { x, w, -1, rgb };
    ui_bands(fill_band, &c, y, y + h, w);
    mark(x, y, w, h);
}

void ui_blend(int x, int y, int w, int h, uint32_t rgb, int alpha) {
    if (alpha <= 0) return;
    if (alpha >= 255) { ui_fill(x, y, w, h, rgb); return; }
    if (!cut(&x, &y, &w, &h)) return;
    struct fill_ctx c = { x, w, alpha + (alpha >> 7), rgb };
    ui_bands(fill_band, &c, y, y + h, w);
    mark(x, y, w, h);
}

// --- corners -----------------------------------------------------------------
//
// How much of each pixel of a quarter circle is inside it, for every radius
// asked for so far: 64 samples a pixel, worked out once.

#define RMAX 40
static uint8_t *corner[RMAX + 1];

static const uint8_t *corner_tab(int r) {
    if (r <= 0) return 0;
    if (r > RMAX) r = RMAX;
    if (corner[r]) return corner[r];
    uint8_t *t = (uint8_t *)kmalloc((size_t)r * r);
    if (!t) return 0;
    const int S = 8;
    int C = 2 * r * S;                       // centre and radius, in half-samples
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            int n = 0;
            for (int sy = 0; sy < S; sy++)
                for (int sx = 0; sx < S; sx++) {
                    int dx = (i * S + sx) * 2 + 1 - C;
                    int dy = (j * S + sy) * 2 + 1 - C;
                    if (dx * dx + dy * dy <= C * C) n++;
                }
            t[j * r + i] = (uint8_t)(n * 255 / (S * S));
        }
    corner[r] = t;
    return t;
}

static int clamp_r(int r, int w, int h) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r > RMAX) r = RMAX;
    return r < 0 ? 0 : r;
}

// A w x h rectangle whose top corners are rounded by rt and bottom ones by
// rb -- a window is rounder at the top than at the bottom, as Windows 7's
// were -- with the radii cut down to what fits.
typedef struct {
    int w, h, rt, rb, corners;
    const uint8_t *tt, *tb;
} shape;

static void shape_init(shape *s, int w, int h, int rt, int rb, int corners) {
    int lim = w / 2 < h / 2 ? w / 2 : h / 2;
    if (lim > RMAX) lim = RMAX;
    if (rt > lim) rt = lim;
    if (rb > lim) rb = lim;
    if (rt < 0) rt = 0;
    if (rb < 0) rb = 0;
    if (!(corners & (UI_TL | UI_TR))) rt = 0;
    if (!(corners & (UI_BL | UI_BR))) rb = 0;
    s->w = w; s->h = h; s->rt = rt; s->rb = rb; s->corners = corners;
    s->tt = rt ? corner_tab(rt) : 0;
    s->tb = rb ? corner_tab(rb) : 0;
    if (rt && !s->tt) s->rt = 0;
    if (rb && !s->tb) s->rb = 0;
}

// Coverage of pixel (i, j) of the shape.
static inline int shape_cov(const shape *s, int i, int j) {
    if (i < 0 || j < 0 || i >= s->w || j >= s->h) return 0;
    int r = s->rt;
    if (j < r) {
        if (i < r && (s->corners & UI_TL)) return s->tt[j * r + i];
        if (i >= s->w - r && (s->corners & UI_TR)) return s->tt[j * r + (s->w - 1 - i)];
    }
    r = s->rb;
    if (j >= s->h - r) {
        int jj = s->h - 1 - j;
        if (i < r && (s->corners & UI_BL)) return s->tb[jj * r + i];
        if (i >= s->w - r && (s->corners & UI_BR)) return s->tb[jj * r + (s->w - 1 - i)];
    }
    return 255;
}

// How far in from the left and right edges row j has curved pixels.
static inline int shape_band(const shape *s, int j) {
    if (j < s->rt) return s->rt;
    if (j >= s->h - s->rb) return s->rb;
    return 0;
}

// --- the backdrop glass looks through -----------------------------------------

static const uint32_t *bd;
static int bd_w, bd_h;

void ui_set_backdrop(const uint32_t *px, int w, int h) {
    bd = px; bd_w = w; bd_h = h;
}

// --- rounded rectangles ----------------------------------------------------------

struct rr_ctx {
    shape s;
    const ui_mat *m;
    int x, y, w, vx, vw, gy0, gspan;
};

static void rr_band(int y0, int y1, int share, void *cv) {
    (void)share;
    const struct rr_ctx *c = (const struct rr_ctx *)cv;
    const ui_mat *m = c->m;
    int x = c->x, y = c->y, w = c->w;
    for (int py = y0; py < y1; py++) {
        int j = py - y;
        uint32_t col = m->c0;
        int a = m->a0;
        if (m->kind == UI_GRAD) {
            int k = (py - c->gy0) * 256 / c->gspan;
            if (k < 0) k = 0;
            if (k > 256) k = 256;
            col = ui_mix(m->c0, m->c1, k);
            a = m->a0 + (m->a1 - m->a0) * k / 256;
            if (a <= 0) continue;
        }
        int band = shape_band(&c->s, j);
        uint32_t *d = row_at(py);
        const uint32_t *b = 0;
        if (m->kind == UI_GLASS && bd && py < bd_h) b = bd + (uint64_t)py * bd_w;
        int tint = m->a0 + (m->a0 >> 7);
        for (int px = c->vx; px < c->vx + c->vw; px++) {
            int i = px - x;
            int cvg = 255;
            if (band && (i < band || i >= w - band)) {
                cvg = shape_cov(&c->s, i, j);
                if (!cvg) continue;
            }
            uint32_t col2;
            int aa;
            if (m->kind == UI_GLASS) {
                uint32_t under = (b && px < bd_w) ? b[px] : m->c0;
                col2 = ui_mix(under, m->c0, tint);
                aa = cvg;
            } else {
                col2 = col;
                aa = a * cvg / 255;
            }
            if (aa <= 0) continue;
            d[px] = over(d[px], col2, (uint32_t)aa);
        }
    }
}

static void rr_fill(int x, int y, int w, int h, int rt, int rb, int corners,
                    const ui_mat *m) {
    if (w <= 0 || h <= 0) return;
    struct rr_ctx c;
    shape_init(&c.s, w, h, rt, rb, corners);
    int vx = x, vy = y, vw = w, vh = h;
    if (!cut(&vx, &vy, &vw, &vh)) return;
    int gy0 = m->gy0, gy1 = m->gy1;
    if (gy0 == 0 && gy1 == 0) { gy0 = y; gy1 = y + h; }
    c.m = m; c.x = x; c.y = y; c.w = w; c.vx = vx; c.vw = vw;
    c.gy0 = gy0; c.gspan = gy1 - gy0 > 0 ? gy1 - gy0 : 1;
    ui_bands(rr_band, &c, vy, vy + vh, vw);
    mark(vx, vy, vw, vh);
}

void ui_rrect(int x, int y, int w, int h, int r, int corners, const ui_mat *m) {
    rr_fill(x, y, w, h, r, r, corners, m);
}

void ui_rrect2(int x, int y, int w, int h, int rt, int rb, const ui_mat *m) {
    rr_fill(x, y, w, h, rt, rb, UI_ALL, m);
}

// The ring between the shape and the same shape one pixel in: coverage of
// the outer less coverage of the inner, so the curve is as smooth as the
// fill's.
static void rr_line(int x, int y, int w, int h, int rt, int rb, int corners,
                    uint32_t rgb, int alpha) {
    if (w <= 2 || h <= 2 || alpha <= 0) return;
    shape so, si;
    shape_init(&so, w, h, rt, rb, corners);
    shape_init(&si, w - 2, h - 2, so.rt > 1 ? so.rt - 1 : 0, so.rb > 1 ? so.rb - 1 : 0, corners);
    int vx = x, vy = y, vw = w, vh = h;
    if (!cut(&vx, &vy, &vw, &vh)) return;

    for (int py = vy; py < vy + vh; py++) {
        int j = py - y;
        uint32_t *d = row_at(py);
        int full = (j == 0 || j == h - 1);
        int band = shape_band(&so, j);
        if (j <= so.rt && band < so.rt) band = so.rt;
        if (j >= h - 1 - so.rb && band < so.rb) band = so.rb;
        for (int px = vx; px < vx + vw; px++) {
            int i = px - x;
            // Away from the corners the ring is the outermost column only.
            if (!full && i > band && i < w - 1 - band) {
                px = x + w - 2 - band;
                continue;
            }
            int c = shape_cov(&so, i, j) - shape_cov(&si, i - 1, j - 1);
            if (c <= 0) continue;
            d[px] = over(d[px], rgb, (uint32_t)(c * alpha / 255));
        }
    }
    mark(vx, vy, vw, vh);
}

void ui_rrect_line(int x, int y, int w, int h, int r, int corners,
                   uint32_t rgb, int alpha) {
    rr_line(x, y, w, h, r, r, corners, rgb, alpha);
}

void ui_rrect2_line(int x, int y, int w, int h, int rt, int rb,
                    uint32_t rgb, int alpha) {
    rr_line(x, y, w, h, rt, rb, UI_ALL, rgb, alpha);
}

// --- glass over what is really there ---------------------------------------------

static inline uint32_t grain_at(int x, int y) {
    uint32_t n = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u;
    n ^= n >> 13;
    n *= 0x5bd1e995u;
    return (n >> 24) & 7;                      // 0..7
}

struct live_ctx {
    shape s;
    const uint32_t *tmp;
    int x, y, w, sx, sy, sw, vx, vw, t;
    uint32_t tint;
};

static void live_copy_band(int y0, int y1, int share, void *cv) {
    (void)share;
    const struct live_ctx *c = (const struct live_ctx *)cv;
    uint32_t *tmp = (uint32_t *)c->tmp;
    for (int j = y0; j < y1; j++) {
        const uint32_t *src = row_at(c->sy + j) + c->sx;
        for (int i = 0; i < c->sw; i++) tmp[j * c->sw + i] = src[i];
    }
}

static void live_band(int y0, int y1, int share, void *cv) {
    (void)share;
    const struct live_ctx *c = (const struct live_ctx *)cv;
    for (int py = y0; py < y1; py++) {
        int j = py - c->y;
        int band = shape_band(&c->s, j);
        uint32_t *d = row_at(py);
        const uint32_t *b = c->tmp + (py - c->sy) * c->sw;
        for (int px = c->vx; px < c->vx + c->vw; px++) {
            int i = px - c->x;
            int cvg = 255;
            if (band && (i < band || i >= c->w - band)) {
                cvg = shape_cov(&c->s, i, j);
                if (!cvg) continue;
            }
            uint32_t col = ui_mix(b[px - c->sx], c->tint, c->t);
            // Grain: a few levels of noise, so the glass reads as a
            // material and wide flat areas do not band.
            uint32_t g = grain_at(px, py);
            uint32_t rr = ((col >> 16) & 255) + g, gg = ((col >> 8) & 255) + g, bb = (col & 255) + g;
            rr = rr > 258 ? 255 : (rr < 4 ? 0 : rr - 3);
            gg = gg > 258 ? 255 : (gg < 4 ? 0 : gg - 3);
            bb = bb > 258 ? 255 : (bb < 4 ? 0 : bb - 3);
            col = (rr << 16) | (gg << 8) | bb;
            d[px] = over(d[px], col, (uint32_t)cvg);
        }
    }
}

void ui_glass_live(int x, int y, int w, int h, int r, uint32_t tint, int tint_a, int blur) {
    if (w <= 0 || h <= 0) return;
    // Take a margin round the rectangle, so the blur at its edge sees what
    // lies beyond it rather than a hard border.
    int pad = blur * 2;
    int sx = x - pad, sy = y - pad, sw = w + 2 * pad, sh = h + 2 * pad;
    int TW = target_w(), TH = target_h();
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sx + sw > TW) sw = TW - sx;
    if (sy + sh > TH) sh = TH - sy;
    if (sw <= 0 || sh <= 0) return;
    uint32_t *tmp = (uint32_t *)kmalloc((size_t)sw * sh * 4);
    if (!tmp) return;
    struct live_ctx c;
    c.tmp = tmp; c.sx = sx; c.sy = sy; c.sw = sw;
    ui_bands(live_copy_band, &c, 0, sh, sw);
    if (blur > 0) ui_blur(tmp, sw, sh, blur);

    shape_init(&c.s, w, h, r, r, UI_ALL);
    int vx = x, vy = y, vw = w, vh = h;
    if (cut(&vx, &vy, &vw, &vh)) {
        c.x = x; c.y = y; c.w = w; c.vx = vx; c.vw = vw;
        c.t = tint_a + (tint_a >> 7);
        c.tint = tint;
        ui_bands(live_band, &c, vy, vy + vh, vw);
        mark(vx, vy, vw, vh);
    }
    kfree(tmp);
    ui_glass_rim(x, y, w, h, r);
}

// Light along the edge: brightest at the top, where it catches the light,
// fading down the sides.
void ui_glass_rim(int x, int y, int w, int h, int r) {
    int sh = h < 60 ? h : 60;
    ui_mat sheen = { UI_GRAD, 0x00FFFFFF, 0x00FFFFFF, 60, 0, y, y + sh };
    ui_rrect(x, y, w, sh, r, UI_TOP, &sheen);
    ui_rrect_line(x, y, w, h, r, UI_ALL, 0x00FFFFFF, 140);
    ui_rrect_line(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, UI_ALL, 0x00FFFFFF, 36);
}

// --- glass with a memory --------------------------------------------------------

static int floor4(int v) { return v >= 0 ? v & ~3 : -((-v + 3) & ~3); }

void ui_glass_place(ui_glass *g, int x, int y, int w, int h, int pad) {
    int ux = floor4(x - pad), uy = floor4(y - pad);
    int ux1 = floor4(x + w + pad + 3), uy1 = floor4(y + h + pad + 3);
    int uw = ux1 - ux, uh = uy1 - uy;
    if (g->have && g->ux == ux && g->uy == uy && g->uw == uw && g->uh == uh) return;
    g->ux = ux; g->uy = uy; g->uw = uw; g->uh = uh;
    g->sw = uw / 4; g->sh = uh / 4;
    int need = g->sw * g->sh;
    if (need > g->cap) {
        if (g->small) kfree(g->small);
        if (g->soft) kfree(g->soft);
        g->small = (uint32_t *)kmalloc((size_t)need * 4);
        g->soft = (uint32_t *)kmalloc((size_t)need * 4);
        g->cap = (g->small && g->soft) ? need : 0;
    }
    g->have = 0;
    g->soft_ok = 0;
}

void ui_glass_forget(ui_glass *g) { g->have = 0; g->soft_ok = 0; }

void ui_glass_release(ui_glass *g) {
    if (g->small) kfree(g->small);
    if (g->soft) kfree(g->soft);
    g->small = g->soft = 0;
    g->cap = 0;
    g->have = g->soft_ok = 0;
}

// Average the 4x4 block (bx, by) of the remembered area from the target,
// reading the nearest on-screen pixel for any that are off it.
static uint32_t block_avg(const ui_glass *g, int bx, int by) {
    int TW = target_w(), TH = target_h();
    uint32_t r = 0, gg = 0, b = 0;
    for (int j = 0; j < 4; j++) {
        int py = g->uy + by * 4 + j;
        py = py < 0 ? 0 : (py >= TH ? TH - 1 : py);
        const uint32_t *row = row_at(py);
        for (int i = 0; i < 4; i++) {
            int px = g->ux + bx * 4 + i;
            px = px < 0 ? 0 : (px >= TW ? TW - 1 : px);
            uint32_t c = row[px];
            r += (c >> 16) & 255; gg += (c >> 8) & 255; b += c & 255;
        }
    }
    return ((r >> 4) << 16) | ((gg >> 4) << 8) | (b >> 4);
}

struct take_ctx { ui_glass *g; int bx0, bx1; };

static void take_band(int by0, int by1, int share, void *cv) {
    (void)share;
    const struct take_ctx *c = (const struct take_ctx *)cv;
    for (int by = by0; by < by1; by++)
        for (int bx = c->bx0; bx < c->bx1; bx++)
            c->g->small[by * c->g->sw + bx] = block_avg(c->g, bx, by);
}

void ui_glass_take(ui_glass *g, const int (*dmg)[4], int n) {
    if (!g->cap) return;
    if (!g->have) {
        struct take_ctx c = { g, 0, g->sw };
        ui_bands(take_band, &c, 0, g->sh, g->sw * 16);
        g->have = 1;
        g->soft_ok = 0;
        return;
    }
    for (int k = 0; k < n; k++) {
        int x0 = dmg[k][0] > g->ux ? dmg[k][0] : g->ux;
        int y0 = dmg[k][1] > g->uy ? dmg[k][1] : g->uy;
        int x1 = dmg[k][2] < g->ux + g->uw ? dmg[k][2] : g->ux + g->uw;
        int y1 = dmg[k][3] < g->uy + g->uh ? dmg[k][3] : g->uy + g->uh;
        if (x1 <= x0 || y1 <= y0) continue;
        int bx0 = (x0 - g->ux) / 4, by0 = (y0 - g->uy) / 4;
        int bx1 = (x1 - g->ux + 3) / 4, by1 = (y1 - g->uy + 3) / 4;
        struct take_ctx c = { g, bx0, bx1 };
        ui_bands(take_band, &c, by0, by1, (bx1 - bx0) * 16);
        g->soft_ok = 0;
    }
}

static void glass_soften(ui_glass *g) {
    if (g->soft_ok || !g->cap || !g->have) return;
    for (int i = 0; i < g->sw * g->sh; i++) g->soft[i] = g->small[i];
    ui_blur(g->soft, g->sw, g->sh, 3);
    g->soft_ok = 1;
}

// What the glass shows at screen pixel (px, py): the softened copy,
// sampled between its quarter-size pixels.
static inline uint32_t glass_sample(const ui_glass *g, int px, int py) {
    int fx = (px - g->ux) * 64 - 96, fy = (py - g->uy) * 64 - 96;
    int i0 = fx >> 8, j0 = fy >> 8;
    int ax = fx & 255, ay = fy & 255;
    if (i0 < 0) { i0 = 0; ax = 0; }
    if (j0 < 0) { j0 = 0; ay = 0; }
    int i1 = i0 + 1 < g->sw ? i0 + 1 : g->sw - 1;
    int j1 = j0 + 1 < g->sh ? j0 + 1 : g->sh - 1;
    if (i0 >= g->sw) i0 = g->sw - 1;
    if (j0 >= g->sh) j0 = g->sh - 1;
    const uint32_t *r0 = g->soft + j0 * g->sw, *r1 = g->soft + j1 * g->sw;
    uint32_t top = ui_mix(r0[i0], r0[i1], ax), bot = ui_mix(r1[i0], r1[i1], ax);
    return ui_mix(top, bot, ay);
}

struct gd_ctx {
    shape so, si;
    const ui_glass *g;
    int x, y, w, bx, by, bw, bh, vx, vw, t;
    uint32_t tint;
};

static void gd_band(int y0, int y1, int share, void *cv) {
    (void)share;
    const struct gd_ctx *c = (const struct gd_ctx *)cv;
    int x = c->x, y = c->y, w = c->w, bx = c->bx, by = c->by, bw = c->bw, bh = c->bh;
    for (int py = y0; py < y1; py++) {
        int j = py - y;
        int band = shape_band(&c->so, j);
        int in_body_rows = bw > 0 && py >= by && py < by + bh;
        int ibandv = in_body_rows ? shape_band(&c->si, py - by) : 0;
        uint32_t *d = row_at(py);
        for (int px = c->vx; px < c->vx + c->vw; px++) {
            int i = px - x;
            // The body's middle is not glass: skip across it. Only its
            // rounded corners share pixels with the frame.
            if (in_body_rows && px >= bx + ibandv && px < bx + bw - ibandv) {
                px = bx + bw - ibandv - 1;
                continue;
            }
            int cvg = 255;
            if (band && (i < band || i >= w - band)) {
                cvg = shape_cov(&c->so, i, j);
                if (!cvg) continue;
            }
            if (in_body_rows && px >= bx && px < bx + bw) {
                int ci = shape_cov(&c->si, px - bx, py - by);
                cvg = cvg - ci;
                if (cvg <= 0) continue;
            }
            uint32_t col = ui_mix(glass_sample(c->g, px, py), c->tint, c->t);
            uint32_t gr = grain_at(px, py);
            uint32_t rr = ((col >> 16) & 255) + gr, gg = ((col >> 8) & 255) + gr, bb = (col & 255) + gr;
            rr = rr > 258 ? 255 : (rr < 4 ? 0 : rr - 3);
            gg = gg > 258 ? 255 : (gg < 4 ? 0 : gg - 3);
            bb = bb > 258 ? 255 : (bb < 4 ? 0 : bb - 3);
            d[px] = over(d[px], (rr << 16) | (gg << 8) | bb, (uint32_t)cvg);
        }
    }
}

void ui_glass_draw(ui_glass *g, int x, int y, int w, int h, int r,
                   int bx, int by, int bw, int bh, int br, int bcorners,
                   uint32_t tint, int tint_a) {
    if (w <= 0 || h <= 0 || !g->cap || !g->have) return;
    glass_soften(g);
    struct gd_ctx c;
    shape_init(&c.so, w, h, r, r, UI_ALL);
    if (bw > 0 && bh > 0) shape_init(&c.si, bw, bh, br, br, bcorners);
    int vx = x, vy = y, vw = w, vh = h;
    if (!cut(&vx, &vy, &vw, &vh)) return;
    c.g = g; c.x = x; c.y = y; c.w = w;
    c.bx = bx; c.by = by; c.bw = bw; c.bh = bh;
    c.vx = vx; c.vw = vw;
    c.t = tint_a + (tint_a >> 7);
    c.tint = tint;
    // Rows through the body are glass only at its two sides, the strip above
    // it and the edge below are glass all the way across: dealt separately,
    // or one core would get the strip and the others almost nothing.
    if (bw > 0 && bh > 0) {
        int m0 = by < vy ? vy : (by > vy + vh ? vy + vh : by);
        int m1 = by + bh < m0 ? m0 : (by + bh > vy + vh ? vy + vh : by + bh);
        ui_bands(gd_band, &c, vy, m0, vw);
        ui_bands(gd_band, &c, m0, m1, 64);
        ui_bands(gd_band, &c, m1, vy + vh, vw);
    } else {
        ui_bands(gd_band, &c, vy, vy + vh, vw);
    }
    mark(vx, vy, vw, vh);
}

uint32_t ui_accent_from(const uint32_t *px, int w, int h) {
    uint64_t sr = 0, sg = 0, sb = 0, sw = 0;
    for (int y = 0; y < h; y += 8)
        for (int x = 0; x < w; x += 8) {
            uint32_t c = px[(uint64_t)y * w + x];
            int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
            int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
            int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
            int sat = mx ? (mx - mn) * 255 / mx : 0;
            uint64_t wgt = (uint64_t)sat * sat * (uint64_t)mx / 255;   // colourful and not dark
            sr += r * wgt; sg += g * wgt; sb += b * wgt; sw += wgt;
        }
    if (!sw) return 0x003C7FD8;
    int r = (int)(sr / sw), g = (int)(sg / sw), b = (int)(sb / sw);
    // To an accent's brightness and colourfulness: the strongest channel at
    // 210, and the spread between strongest and weakest at least 120, so a
    // pastel wallpaper still gives a colour that reads.
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (mx < 1) mx = 1;
    r = r * 210 / mx; g = g * 210 / mx; b = b * 210 / mx;
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int spread = 210 - mn;
    if (spread > 0 && spread < 120) {
        r = 210 - (210 - r) * 120 / spread;
        g = 210 - (210 - g) * 120 / spread;
        b = 210 - (210 - b) * 120 / spread;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void ui_round_fill(int x, int y, int w, int h, int r, uint32_t rgb, int alpha) {
    ui_mat m = { UI_SOLID, rgb, 0, alpha, 0, 0, 0 };
    ui_rrect(x, y, w, h, r, UI_ALL, &m);
}

void ui_round_grad(int x, int y, int w, int h, int r, int corners,
                   uint32_t top, int atop, uint32_t bot, int abot) {
    ui_mat m = { UI_GRAD, top, bot, atop, abot, 0, 0 };
    ui_rrect(x, y, w, h, r, corners, &m);
}

// --- shadows -----------------------------------------------------------------------

static uint32_t isqrt32(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

struct sh_ctx {
    const uint16_t *prof;
    int x, y, w, r, n, alpha, cy0, cy1, ix0, iy0, ix1, iy1, vx, vw;
};

static void sh_band(int y0, int y1, int share, void *cv) {
    (void)share;
    const struct sh_ctx *c = (const struct sh_ctx *)cv;
    int x = c->x, w = c->w, r = c->r;
    for (int py = y0; py < y1; py++) {
        uint32_t *d = row_at(py);
        int ddy = py < c->iy0 ? c->iy0 - py : (py > c->iy1 ? py - c->iy1 : 0);
        int caster_rows = (py >= c->cy0 + r && py < c->cy1 - r);
        for (int px = c->vx; px < c->vx + c->vw; px++) {
            // The caster's own middle: skip straight across it.
            if (caster_rows && px >= x + r && px < x + w - r) {
                px = x + w - r - 1;
                continue;
            }
            int ddx = px < c->ix0 ? c->ix0 - px : (px > c->ix1 ? px - c->ix1 : 0);
            int dist16;
            if (!ddx) dist16 = ddy * 16;
            else if (!ddy) dist16 = ddx * 16;
            else dist16 = (int)isqrt32((uint32_t)(ddx * ddx + ddy * ddy) * 256);
            dist16 -= r * 16;
            if (dist16 < 0) dist16 = 0;          // inside the shadow: at its darkest
            if (dist16 >= c->n) continue;
            int a = c->alpha * c->prof[dist16] >> 8;
            if (a <= 0) continue;
            d[px] = ui_mix(d[px], 0, a + (a >> 7));
        }
    }
}

void ui_shadow(int x, int y, int w, int h, int r, int size, int alpha, int dy,
               const int limit[4]) {
    if (size <= 0 || alpha <= 0 || w <= 0 || h <= 0) return;
    // The caster stays where it is; only its shadow moves down. What the
    // caster covers is skipped -- what the shadow covers below it is not:
    // skipping the shadow's own middle left a strip of plain desktop between
    // a window's bottom edge and the shadow under it.
    int cy0 = y, cy1 = y + h;
    y += dy;
    r = clamp_r(r, w, h);
    int saved[4];
    ui_clip_get(saved);
    if (limit) {
        int l[4] = { limit[0], limit[1], limit[0] + limit[2], limit[1] + limit[3] };
        int c[4] = { saved[0] > l[0] ? saved[0] : l[0], saved[1] > l[1] ? saved[1] : l[1],
                     saved[2] < l[2] ? saved[2] : l[2], saved[3] < l[3] ? saved[3] : l[3] };
        if (c[2] < c[0]) c[2] = c[0];
        if (c[3] < c[1]) c[3] = c[1];
        ui_clip_set(c);
    }

    // The falloff, in sixteenths of a pixel: a smoothstep, squared so the
    // tail fades out instead of ending at a line.
    int n = size * 16;
    static uint16_t prof[64 * 16 + 1];
    if (n > 64 * 16) n = 64 * 16;
    for (int k = 0; k <= n; k++) {
        int t = k * 256 / n;
        int ss = (t * t * (768 - 2 * t)) >> 16;
        int v = 256 - ss;
        prof[k] = (uint16_t)((v * v) >> 8);
    }

    int bx = x - size, by = y - size, bw = w + 2 * size, bh = h + 2 * size;
    int vx = bx, vy = by, vw = bw, vh = bh;
    if (!cut(&vx, &vy, &vw, &vh)) { ui_clip_set(saved); return; }

    // Distance from the rounded rectangle: from the rectangle shrunk by r,
    // less r. The rows beside the caster have only its two edges to do.
    struct sh_ctx c = { prof, x, y, w, r, n, alpha, cy0, cy1,
                        x + r, y + r, x + w - 1 - r, y + h - 1 - r, vx, vw };
    // Above and below the caster the shadow is the whole width; beside it,
    // two narrow edges. Dealt separately, so the cores share evenly.
    int m0 = cy0 + r, m1 = cy1 - r;
    if (m0 < vy) m0 = vy;
    if (m0 > vy + vh) m0 = vy + vh;
    if (m1 < m0) m1 = m0;
    if (m1 > vy + vh) m1 = vy + vh;
    ui_bands(sh_band, &c, vy, m0, vw);
    ui_bands(sh_band, &c, m0, m1, 4 * size + 2 * r);
    ui_bands(sh_band, &c, m1, vy + vh, vw);
    mark(vx, vy, vw, vh);
    ui_clip_set(saved);
}

// --- shapes, by librast, straight onto the screen ----------------------------------

static void fb_target(rast_target *t) {
    if (!clip_ok) ui_noclip();
    if (tg_px)
        rast_target_init(t, tg_px, tg_w, tg_h, tg_w, RAST_XRGB);
    else
        rast_target_init(t, (uint32_t *)fb_get_base(), (int)fb_get_width(),
                         (int)fb_get_height(), (int)(fb_get_pitch() / 4), RAST_XRGB);
    t->cx0 = cx0; t->cy0 = cy0; t->cx1 = cx1; t->cy1 = cy1;
}

static void mark_path(const rast_path *p, rast_fx grow) {
    rast_fx b[4];
    if (!rast_path_bounds(p, b)) return;
    int x = (int)((b[0] - grow) >> 16) - 1, y = (int)((b[1] - grow) >> 16) - 1;
    int w = (int)((b[2] + grow) >> 16) + 2 - x, h = (int)((b[3] + grow) >> 16) + 2 - y;
    if (cut(&x, &y, &w, &h))
        mark(x, y, w, h);
}

void ui_rast_fill(const rast_path *p, const rast_paint *paint) {
    rast_target t;
    fb_target(&t);
    rast_fill(&t, p, 0, RAST_NONZERO, paint);
    mark_path(p, 0);
}

void ui_rast_stroke(const rast_path *p, const rast_stroke *s, const rast_paint *paint) {
    rast_target t;
    fb_target(&t);
    rast_draw_stroke(&t, p, 0, s, paint);
    mark_path(p, s->width);
}

// --- pictures ------------------------------------------------------------------------

void ui_image(int x, int y, const uint32_t *px, int w, int h, int alpha) {
    if (!px || w <= 0 || h <= 0 || alpha <= 0) return;
    int vx = x, vy = y, vw = w, vh = h;
    if (!cut(&vx, &vy, &vw, &vh)) return;
    uint32_t ga = (uint32_t)(alpha + (alpha >> 7));
    for (int py = vy; py < vy + vh; py++) {
        uint32_t *d = row_at(py);
        const uint32_t *s = px + (uint64_t)(py - y) * w;
        for (int qx = vx; qx < vx + vw; qx++) {
            uint32_t c = s[qx - x];
            uint32_t a = c >> 24;
            if (!a) continue;
            if (ga < 256) {
                // Scale the premultiplied colour and its alpha together.
                uint32_t rb = ((c & 0xFF00FF) * ga >> 8) & 0xFF00FF;
                uint32_t g  = ((c & 0x00FF00) * ga >> 8) & 0x00FF00;
                a = a * ga >> 8;
                c = rb | g;
                if (!a) continue;
            }
            // Premultiplied: what is underneath, scaled by what the picture
            // leaves of it, plus the picture. No channel can pass 255.
            uint32_t ia = 256 - (a + (a >> 7));
            uint32_t o = d[qx];
            uint32_t rb = (((o & 0xFF00FF) * ia >> 8) & 0xFF00FF) + (c & 0xFF00FF);
            uint32_t g  = (((o & 0x00FF00) * ia >> 8) & 0x00FF00) + (c & 0x00FF00);
            d[qx] = (rb & 0xFF00FF) | (g & 0xFF00);
        }
    }
    mark(vx, vy, vw, vh);
}

// An icon from the icon set: straight (not premultiplied) alpha. 0 if the
// set has no such icon at that size, so the caller can draw something else.
int ui_icon(int x, int y, const char *name, int size) {
    const uint32_t *src = icon_get(name, size);
    if (!src) return 0;
    int vx = x, vy = y, vw = size, vh = size;
    if (!cut(&vx, &vy, &vw, &vh)) return 1;
    for (int py = vy; py < vy + vh; py++) {
        uint32_t *d = row_at(py);
        const uint32_t *s = src + (py - y) * size;
        for (int px = vx; px < vx + vw; px++) {
            uint32_t c = s[px - x], a = c >> 24;
            if (a) d[px] = over(d[px], c, a);
        }
    }
    mark(vx, vy, vw, vh);
    return 1;
}

// --- text ------------------------------------------------------------------------------

static int utf8_next(const char *s, int *cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int need, v;
    if ((c & 0xE0) == 0xC0) { need = 1; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { need = 2; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { need = 3; v = c & 0x07; }
    else { *cp = 0xFFFD; return 1; }
    for (int i = 1; i <= need; i++) {
        unsigned char k = (unsigned char)s[i];
        if ((k & 0xC0) != 0x80) { *cp = 0xFFFD; return i; }
        v = (v << 6) | (k & 0x3F);
    }
    *cp = v;
    return need + 1;
}

static const struct ui_glyph *glyph_of(const struct ui_face *F, int cp) {
    int s = ui_slot(cp);
    if (s < 0 || !F->g[s].present) s = '?';
    return F->g[s].present ? &F->g[s] : 0;
}

int ui_line_h(int face) {
    if (!ui_fonts_ready || face < 0 || face >= UI_FACES) return 16;
    return ui_faces[face].height;
}

int ui_text_w(const char *s, int face) {
    if (!ui_fonts_ready || !s || face < 0 || face >= UI_FACES) return 0;
    const struct ui_face *F = &ui_faces[face];
    int w = 0;
    for (int i = 0; s[i]; ) {
        int cp;
        i += utf8_next(s + i, &cp);
        const struct ui_glyph *g = glyph_of(F, cp);
        if (g) w += g->adv;
    }
    return w;
}

// One glyph's coverage, scaled by `a` (0..256), blended in `rgb`.
static void put_glyph(int pen, int base, const struct ui_face *F,
                      const struct ui_glyph *g, uint32_t rgb, int a256) {
    if (!g->w) return;
    int gx = pen + g->xoff, gy = base + g->yoff;
    int vx = gx, vy = gy, vw = g->w, vh = g->h;
    if (!cut(&vx, &vy, &vw, &vh)) return;
    const uint8_t *src = F->pool + g->off;
    for (int py = vy; py < vy + vh; py++) {
        uint32_t *d = row_at(py);
        const uint8_t *s = src + (py - gy) * g->w;
        for (int px = vx; px < vx + vw; px++) {
            uint32_t c = s[px - gx];
            if (!c) continue;
            c = (c * (uint32_t)a256) >> 8;
            d[px] = over(d[px], rgb, c);
        }
    }
    mark(vx, vy, vw, vh);
}

static int text_run(int x, int y, const char *s, int nbytes, int face,
                    uint32_t rgb, int a256) {
    if (!ui_fonts_ready || !s || face < 0 || face >= UI_FACES) return 0;
    const struct ui_face *F = &ui_faces[face];
    int pen = x, base = y + F->ascent;
    for (int i = 0; s[i] && (nbytes < 0 || i < nbytes); ) {
        int cp;
        i += utf8_next(s + i, &cp);
        const struct ui_glyph *g = glyph_of(F, cp);
        if (!g) continue;
        put_glyph(pen, base, F, g, rgb, a256);
        pen += g->adv;
    }
    return pen - x;
}

int ui_text(int x, int y, const char *s, int face, uint32_t rgb) {
    return text_run(x, y, s, -1, face, rgb, 256);
}

int ui_text_fit(int x, int y, int maxw, const char *s, int face, uint32_t rgb) {
    if (!ui_fonts_ready || !s) return 0;
    if (ui_text_w(s, face) <= maxw) return ui_text(x, y, s, face, rgb);
    const struct ui_face *F = &ui_faces[face];
    static const char ell[] = "\xE2\x80\xA6";          // U+2026
    int ew = ui_text_w(ell, face);
    int w = 0, i = 0, cut_at = 0;
    while (s[i]) {
        int cp, n = utf8_next(s + i, &cp);
        const struct ui_glyph *g = glyph_of(F, cp);
        int a = g ? g->adv : 0;
        if (w + a + ew > maxw) break;
        w += a;
        i += n;
        cut_at = i;
    }
    // Trailing spaces before the ellipsis read as a gap.
    while (cut_at > 0 && s[cut_at - 1] == ' ') { cut_at--; w -= ui_faces[face].g[' '].adv; }
    text_run(x, y, s, cut_at, face, rgb, 256);
    text_run(x + w, y, ell, -1, face, rgb, 256);
    return w + ew;
}

// The halo: the text's own coverage, spread by a small blur, laid down in
// the glow colour first. Rendered into a scratch mask so the blur sees the
// whole string at once.
int ui_text_glow(int x, int y, const char *s, int face, uint32_t rgb,
                 uint32_t glow, int glow_alpha) {
    if (!ui_fonts_ready || !s) return 0;
    const struct ui_face *F = &ui_faces[face];
    int tw = ui_text_w(s, face);
    const int R = 4;
    int mw = tw + 2 * R + 4, mh = F->height + 2 * R;
    static uint8_t *mask;
    static uint16_t *acc;
    static int mcap;
    if (mw * mh > mcap) {
        if (mask) kfree(mask);
        if (acc) kfree(acc);
        mcap = mw * mh;
        mask = (uint8_t *)kmalloc((size_t)mcap);
        acc = (uint16_t *)kmalloc((size_t)mcap * 2);
        if (!mask || !acc) { mcap = 0; return ui_text(x, y, s, face, rgb); }
    }
    for (int k = 0; k < mw * mh; k++) mask[k] = 0;
    int pen = R + 2, base = R + F->ascent;
    for (int i = 0; s[i]; ) {
        int cp;
        i += utf8_next(s + i, &cp);
        const struct ui_glyph *g = glyph_of(F, cp);
        if (!g) continue;
        for (int gy = 0; gy < g->h; gy++)
            for (int gx = 0; gx < g->w; gx++) {
                int mx = pen + g->xoff + gx, my = base + g->yoff + gy;
                if (mx < 0 || my < 0 || mx >= mw || my >= mh) continue;
                uint8_t c = F->pool[g->off + gy * g->w + gx];
                if (c > mask[my * mw + mx]) mask[my * mw + mx] = c;
            }
        pen += g->adv;
    }
    // Two box passes each way, radius 2: a soft halo about four pixels out.
    for (int pass = 0; pass < 2; pass++) {
        for (int j = 0; j < mh; j++) {
            int sum = 0;
            for (int i = -2; i <= 2; i++) sum += (i >= 0 && i < mw) ? mask[j * mw + i] : 0;
            for (int i = 0; i < mw; i++) {
                acc[j * mw + i] = (uint16_t)sum;
                int add = i + 3, sub = i - 2;
                if (add < mw) sum += mask[j * mw + add];
                if (sub >= 0) sum -= mask[j * mw + sub];
            }
        }
        for (int i = 0; i < mw; i++) {
            int sum = 0;
            for (int j = -2; j <= 2; j++) sum += (j >= 0 && j < mh) ? acc[j * mw + i] : 0;
            for (int j = 0; j < mh; j++) {
                int v = sum / 25 * 2;               // a little stronger than an average
                mask[j * mw + i] = (uint8_t)(v > 255 ? 255 : v);
                int add = j + 3, sub = j - 2;
                if (add < mh) sum += acc[add * mw + i];
                if (sub >= 0) sum -= acc[sub * mw + i];
            }
        }
    }
    int ox = x - R - 2, oy = y - R;
    int vx = ox, vy = oy, vw = mw, vh = mh;
    if (cut(&vx, &vy, &vw, &vh)) {
        for (int py = vy; py < vy + vh; py++) {
            uint32_t *d = row_at(py);
            const uint8_t *m = mask + (py - oy) * mw;
            for (int px = vx; px < vx + vw; px++) {
                uint32_t a = (uint32_t)m[px - ox] * (uint32_t)glow_alpha / 255;
                if (a) d[px] = over(d[px], glow, a);
            }
        }
        mark(vx, vy, vw, vh);
    }
    return ui_text(x, y, s, face, rgb);
}

// --- blur ---------------------------------------------------------------------------
//
// Three box passes each way. The horizontal one walks each row with a
// running sum; the vertical one keeps a running sum for EVERY column and
// walks the rows top to bottom, so both read memory in order -- a column at a
// time would touch a new cache line on every pixel of a 1080-row image.

struct blur_ctx { uint32_t *px, *lines, *sums, *ring; int w, h, r, stride; };

static void hpass_band(int y0, int y1, int share, void *cv) {
    const struct blur_ctx *c = (const struct blur_ctx *)cv;
    int w = c->w, r = c->r;
    uint32_t *line = c->lines + (uint64_t)share * c->stride;
    int n = 2 * r + 1;
    uint32_t inv = (65536u + (uint32_t)n / 2) / (uint32_t)n;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = c->px + (uint64_t)y * w;
        for (int x = 0; x < w; x++) line[x] = row[x];
        uint32_t sr = 0, sg = 0, sb = 0;
        for (int k = -r; k <= r; k++) {
            uint32_t col = line[k < 0 ? 0 : (k >= w ? w - 1 : k)];
            sr += (col >> 16) & 255; sg += (col >> 8) & 255; sb += col & 255;
        }
        for (int x = 0; x < w; x++) {
            uint32_t R = (sr * inv) >> 16, G = (sg * inv) >> 16, B = (sb * inv) >> 16;
            row[x] = ((R > 255 ? 255 : R) << 16) | ((G > 255 ? 255 : G) << 8) | (B > 255 ? 255 : B);
            int add = x + r + 1, sub = x - r;
            uint32_t ca = line[add >= w ? w - 1 : add];
            uint32_t cs = line[sub < 0 ? 0 : sub];
            sr += ((ca >> 16) & 255) - ((cs >> 16) & 255);
            sg += ((ca >> 8) & 255) - ((cs >> 8) & 255);
            sb += (ca & 255) - (cs & 255);
        }
    }
}

// Columns x0..x1 only: every column keeps its own sums and its own place in
// the ring, so bands of columns never meet.
static void vpass_band(int x0, int x1, int share, void *cv) {
    (void)share;
    const struct blur_ctx *c = (const struct blur_ctx *)cv;
    uint32_t *px = c->px;
    int w = c->w, h = c->h, r = c->r;
    // ring holds the original rows still inside the window: 2r+2 of them.
    int n = 2 * r + 1, cap = 2 * r + 2;
    uint32_t inv = (65536u + (uint32_t)n / 2) / (uint32_t)n;
    uint32_t *sr = c->sums, *sg = c->sums + w, *sb = c->sums + 2 * w;
    uint32_t *ring = c->ring;
    for (int x = x0; x < x1; x++) sr[x] = sg[x] = sb[x] = 0;
    // Prime with rows -r..r, the ones above the top standing in as row 0.
    for (int k = -r; k <= r; k++) {
        const uint32_t *row = px + (uint64_t)(k < 0 ? 0 : (k >= h ? h - 1 : k)) * w;
        for (int x = x0; x < x1; x++) {
            uint32_t col = row[x];
            sr[x] += (col >> 16) & 255; sg[x] += (col >> 8) & 255; sb[x] += col & 255;
        }
    }
    for (int k = 0; k <= r && k < h; k++) {
        uint32_t *slot = ring + (uint64_t)(k % cap) * w;
        const uint32_t *row = px + (uint64_t)k * w;
        for (int x = x0; x < x1; x++) slot[x] = row[x];
    }
    for (int y = 0; y < h; y++) {
        uint32_t *row = px + (uint64_t)y * w;
        // Before row y is overwritten, the row that will enter the window
        // later must be safe: it is y+r+1, still untouched below.
        int add = y + r + 1, sub = y - r;
        if (add < h) {
            uint32_t *slot = ring + (uint64_t)(add % cap) * w;
            const uint32_t *src = px + (uint64_t)add * w;
            for (int x = x0; x < x1; x++) slot[x] = src[x];
        }
        for (int x = x0; x < x1; x++) {
            uint32_t R = (sr[x] * inv) >> 16, G = (sg[x] * inv) >> 16, B = (sb[x] * inv) >> 16;
            row[x] = ((R > 255 ? 255 : R) << 16) | ((G > 255 ? 255 : G) << 8) | (B > 255 ? 255 : B);
        }
        const uint32_t *ra = ring + (uint64_t)((add < h ? add : h - 1) % cap) * w;
        const uint32_t *rs = ring + (uint64_t)((sub < 0 ? 0 : sub) % cap) * w;
        for (int x = x0; x < x1; x++) {
            uint32_t ca = ra[x], cs = rs[x];
            sr[x] += ((ca >> 16) & 255) - ((cs >> 16) & 255);
            sg[x] += ((ca >> 8) & 255) - ((cs >> 8) & 255);
            sb[x] += (ca & 255) - (cs & 255);
        }
    }
}

void ui_blur(uint32_t *px, int w, int h, int r) {
    if (!px || w <= 0 || h <= 0 || r <= 0) return;
    int bands = ui_band_count();
    struct blur_ctx c;
    c.px = px; c.w = w; c.h = h; c.r = r;
    c.stride = w;
    c.lines = (uint32_t *)kmalloc((size_t)w * bands * 4);       // one row each
    c.sums = (uint32_t *)kmalloc((size_t)w * 3 * 4);
    c.ring = (uint32_t *)kmalloc((size_t)w * (2 * r + 2) * 4);
    if (c.lines && c.sums && c.ring) {
        for (int pass = 0; pass < 3; pass++) {
            ui_bands(hpass_band, &c, 0, h, w);
            ui_bands(vpass_band, &c, 0, w, h);
        }
    }
    if (c.lines) kfree(c.lines);
    if (c.sums) kfree(c.sums);
    if (c.ring) kfree(c.ring);
}
