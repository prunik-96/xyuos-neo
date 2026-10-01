// The desktop's drawing primitives (see ui.h). Integers only.

#include "ui.h"
#include "../drivers/framebuffer.h"
#include "../mm/heap.h"

static int cx0, cy0, cx1, cy1, clip_ok;

void ui_noclip(void) {
    cx0 = 0; cy0 = 0;
    cx1 = (int)fb_get_width(); cy1 = (int)fb_get_height();
    clip_ok = 1;
}

void ui_clip(int x, int y, int w, int h) {
    int W = (int)fb_get_width(), H = (int)fb_get_height();
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
    return (uint32_t *)(fb_get_base() + (uint64_t)y * fb_get_pitch());
}

// dst toward src by a (0..255).
static inline uint32_t over(uint32_t dst, uint32_t src, uint32_t a) {
    if (a >= 255) return src & 0xFFFFFF;
    return ui_mix(dst, src, (int)(a + (a >> 7)));
}

void ui_fill(int x, int y, int w, int h, uint32_t rgb) {
    if (!cut(&x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint32_t *d = row_at(y + j) + x;
        for (int i = 0; i < w; i++) d[i] = rgb;
    }
    fb_mark_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
}

void ui_blend(int x, int y, int w, int h, uint32_t rgb, int alpha) {
    if (alpha <= 0) return;
    if (alpha >= 255) { ui_fill(x, y, w, h, rgb); return; }
    if (!cut(&x, &y, &w, &h)) return;
    int t = alpha + (alpha >> 7);
    for (int j = 0; j < h; j++) {
        uint32_t *d = row_at(y + j) + x;
        for (int i = 0; i < w; i++) d[i] = ui_mix(d[i], rgb, t);
    }
    fb_mark_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
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

static void rr_fill(int x, int y, int w, int h, int rt, int rb, int corners,
                    const ui_mat *m) {
    if (w <= 0 || h <= 0) return;
    shape s;
    shape_init(&s, w, h, rt, rb, corners);
    int vx = x, vy = y, vw = w, vh = h;
    if (!cut(&vx, &vy, &vw, &vh)) return;

    int gy0 = m->gy0, gy1 = m->gy1;
    if (gy0 == 0 && gy1 == 0) { gy0 = y; gy1 = y + h; }
    int gspan = gy1 - gy0 > 0 ? gy1 - gy0 : 1;

    for (int py = vy; py < vy + vh; py++) {
        int j = py - y;
        uint32_t col = m->c0;
        int a = m->a0;
        if (m->kind == UI_GRAD) {
            int k = (py - gy0) * 256 / gspan;
            if (k < 0) k = 0;
            if (k > 256) k = 256;
            col = ui_mix(m->c0, m->c1, k);
            a = m->a0 + (m->a1 - m->a0) * k / 256;
            if (a <= 0) continue;
        }
        int band = shape_band(&s, j);
        uint32_t *d = row_at(py);
        const uint32_t *b = 0;
        if (m->kind == UI_GLASS && bd && py < bd_h) b = bd + (uint64_t)py * bd_w;
        int tint = m->a0 + (m->a0 >> 7);
        for (int px = vx; px < vx + vw; px++) {
            int i = px - x;
            int cv = 255;
            if (band && (i < band || i >= w - band)) {
                cv = shape_cov(&s, i, j);
                if (!cv) continue;
            }
            uint32_t c;
            int aa;
            if (m->kind == UI_GLASS) {
                uint32_t under = (b && px < bd_w) ? b[px] : m->c0;
                c = ui_mix(under, m->c0, tint);
                aa = cv;
            } else {
                c = col;
                aa = a * cv / 255;
            }
            if (aa <= 0) continue;
            d[px] = over(d[px], c, (uint32_t)aa);
        }
    }
    fb_mark_rect((uint32_t)vx, (uint32_t)vy, (uint32_t)vw, (uint32_t)vh);
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
    fb_mark_rect((uint32_t)vx, (uint32_t)vy, (uint32_t)vw, (uint32_t)vh);
}

void ui_rrect_line(int x, int y, int w, int h, int r, int corners,
                   uint32_t rgb, int alpha) {
    rr_line(x, y, w, h, r, r, corners, rgb, alpha);
}

void ui_rrect2_line(int x, int y, int w, int h, int rt, int rb,
                    uint32_t rgb, int alpha) {
    rr_line(x, y, w, h, rt, rb, UI_ALL, rgb, alpha);
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

void ui_shadow(int x, int y, int w, int h, int r, int size, int alpha, int dy,
               const int limit[4]) {
    if (size <= 0 || alpha <= 0 || w <= 0 || h <= 0) return;
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
    // less r.
    int ix0 = x + r, iy0 = y + r, ix1 = x + w - 1 - r, iy1 = y + h - 1 - r;
    for (int py = vy; py < vy + vh; py++) {
        uint32_t *d = row_at(py);
        int ddy = py < iy0 ? iy0 - py : (py > iy1 ? py - iy1 : 0);
        int inside_rows = (py >= y && py < y + h);
        for (int px = vx; px < vx + vw; px++) {
            // The middle is the caster's own: skip straight across it.
            if (inside_rows && px >= x + r && px < x + w - r && py >= y + r && py < y + h - r) {
                px = x + w - r - 1;
                continue;
            }
            int ddx = px < ix0 ? ix0 - px : (px > ix1 ? px - ix1 : 0);
            int dist16;
            if (!ddx) dist16 = ddy * 16;
            else if (!ddy) dist16 = ddx * 16;
            else dist16 = (int)isqrt32((uint32_t)(ddx * ddx + ddy * ddy) * 256);
            dist16 -= r * 16;
            if (dist16 <= 0) {
                // Inside the shape. Only the corners of the bounding box get
                // here, and they are the caster's to cover.
                continue;
            }
            if (dist16 >= n) continue;
            int a = alpha * prof[dist16] >> 8;
            if (a <= 0) continue;
            d[px] = ui_mix(d[px], 0, a + (a >> 7));
        }
    }
    fb_mark_rect((uint32_t)vx, (uint32_t)vy, (uint32_t)vw, (uint32_t)vh);
    ui_clip_set(saved);
}

// --- shapes, by librast, straight onto the screen ----------------------------------

static void fb_target(rast_target *t) {
    if (!clip_ok) ui_noclip();
    rast_target_init(t, (uint32_t *)fb_get_base(), (int)fb_get_width(), (int)fb_get_height(),
                     (int)(fb_get_pitch() / 4), RAST_XRGB);
    t->cx0 = cx0; t->cy0 = cy0; t->cx1 = cx1; t->cy1 = cy1;
}

static void mark_path(const rast_path *p, rast_fx grow) {
    rast_fx b[4];
    if (!rast_path_bounds(p, b)) return;
    int x = (int)((b[0] - grow) >> 16) - 1, y = (int)((b[1] - grow) >> 16) - 1;
    int w = (int)((b[2] + grow) >> 16) + 2 - x, h = (int)((b[3] + grow) >> 16) + 2 - y;
    if (cut(&x, &y, &w, &h))
        fb_mark_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
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
    fb_mark_rect((uint32_t)vx, (uint32_t)vy, (uint32_t)vw, (uint32_t)vh);
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
    fb_mark_rect((uint32_t)vx, (uint32_t)vy, (uint32_t)vw, (uint32_t)vh);
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
        fb_mark_rect((uint32_t)vx, (uint32_t)vy, (uint32_t)vw, (uint32_t)vh);
    }
    return ui_text(x, y, s, face, rgb);
}

// --- blur ---------------------------------------------------------------------------
//
// Three box passes each way. The horizontal one walks each row with a
// running sum; the vertical one keeps a running sum for EVERY column and
// walks the rows top to bottom, so both read memory in order -- a column at a
// time would touch a new cache line on every pixel of a 1080-row image.

static void hpass(uint32_t *px, int w, int h, int r, uint32_t *line) {
    int n = 2 * r + 1;
    uint32_t inv = (65536u + (uint32_t)n / 2) / (uint32_t)n;
    for (int y = 0; y < h; y++) {
        uint32_t *row = px + (uint64_t)y * w;
        for (int x = 0; x < w; x++) line[x] = row[x];
        uint32_t sr = 0, sg = 0, sb = 0;
        for (int k = -r; k <= r; k++) {
            uint32_t c = line[k < 0 ? 0 : (k >= w ? w - 1 : k)];
            sr += (c >> 16) & 255; sg += (c >> 8) & 255; sb += c & 255;
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

static void vpass(uint32_t *px, int w, int h, int r, uint32_t *sums, uint32_t *ring) {
    // ring holds the original rows still inside the window: 2r+2 of them.
    int n = 2 * r + 1, cap = 2 * r + 2;
    uint32_t inv = (65536u + (uint32_t)n / 2) / (uint32_t)n;
    uint32_t *sr = sums, *sg = sums + w, *sb = sums + 2 * w;
    for (int x = 0; x < w; x++) sr[x] = sg[x] = sb[x] = 0;
    // Prime with rows -r..r, the ones above the top standing in as row 0.
    for (int k = -r; k <= r; k++) {
        const uint32_t *row = px + (uint64_t)(k < 0 ? 0 : (k >= h ? h - 1 : k)) * w;
        for (int x = 0; x < w; x++) {
            uint32_t c = row[x];
            sr[x] += (c >> 16) & 255; sg[x] += (c >> 8) & 255; sb[x] += c & 255;
        }
    }
    for (int k = 0; k <= r && k < h; k++) {
        uint32_t *slot = ring + (uint64_t)(k % cap) * w;
        const uint32_t *row = px + (uint64_t)k * w;
        for (int x = 0; x < w; x++) slot[x] = row[x];
    }
    for (int y = 0; y < h; y++) {
        uint32_t *row = px + (uint64_t)y * w;
        // Before row y is overwritten, the row that will enter the window
        // later must be safe: it is y+r+1, still untouched below.
        int add = y + r + 1, sub = y - r;
        if (add < h) {
            uint32_t *slot = ring + (uint64_t)(add % cap) * w;
            const uint32_t *src = px + (uint64_t)add * w;
            for (int x = 0; x < w; x++) slot[x] = src[x];
        }
        for (int x = 0; x < w; x++) {
            uint32_t R = (sr[x] * inv) >> 16, G = (sg[x] * inv) >> 16, B = (sb[x] * inv) >> 16;
            row[x] = ((R > 255 ? 255 : R) << 16) | ((G > 255 ? 255 : G) << 8) | (B > 255 ? 255 : B);
        }
        const uint32_t *ra = ring + (uint64_t)((add < h ? add : h - 1) % cap) * w;
        const uint32_t *rs = ring + (uint64_t)((sub < 0 ? 0 : sub) % cap) * w;
        for (int x = 0; x < w; x++) {
            uint32_t ca = ra[x], cs = rs[x];
            sr[x] += ((ca >> 16) & 255) - ((cs >> 16) & 255);
            sg[x] += ((ca >> 8) & 255) - ((cs >> 8) & 255);
            sb[x] += (ca & 255) - (cs & 255);
        }
    }
}

void ui_blur(uint32_t *px, int w, int h, int r) {
    if (!px || w <= 0 || h <= 0 || r <= 0) return;
    uint32_t *line = (uint32_t *)kmalloc((size_t)(w > h ? w : h) * 4);
    uint32_t *sums = (uint32_t *)kmalloc((size_t)w * 3 * 4);
    uint32_t *ring = (uint32_t *)kmalloc((size_t)w * (2 * r + 2) * 4);
    if (line && sums && ring) {
        for (int pass = 0; pass < 3; pass++) {
            hpass(px, w, h, r, line);
            vpass(px, w, h, r, sums, ring);
        }
    }
    if (line) kfree(line);
    if (sums) kfree(sums);
    if (ring) kfree(ring);
}
