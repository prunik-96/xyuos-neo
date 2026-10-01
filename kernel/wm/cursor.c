// The pointer's pictures (see cursor.h).
//
// In the manner of Windows 7's Aero set: white, a thin black edge, and a
// shadow that falls a little down and to the right. The ring of the busy
// pointer is blue glass with a bright arc running round it.

#include "cursor.h"
#include "../../librast/include/rast.h"

#define FRAMED (CUR_FRAMES)

static uint32_t imgs[CUR_KINDS][FRAMED][CUR_SZ * CUR_SZ];
static int hot[CUR_KINDS][2];
static int ready;

static void clear(uint32_t *px) {
    for (int i = 0; i < CUR_SZ * CUR_SZ; i++) px[i] = 0;
}

static void target(rast_target *t, uint32_t *px) {
    rast_target_init(t, px, CUR_SZ, CUR_SZ, CUR_SZ, RAST_ARGB);
}

// The shadow of path p, moved (dx, dy) and softened: its coverage into a
// mask, two box blurs, laid down black at `alpha`.
static void shadow(uint32_t *px, const rast_path *p, rast_fx dx, rast_fx dy, int alpha) {
    static uint8_t m[CUR_SZ * CUR_SZ], t[CUR_SZ * CUR_SZ];
    for (int i = 0; i < CUR_SZ * CUR_SZ; i++) m[i] = 0;
    rast_mat mv = rast_translate(dx, dy);
    rast_fill_mask(m, CUR_SZ, CUR_SZ, CUR_SZ, p, &mv, RAST_NONZERO);
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < CUR_SZ; y++)
            for (int x = 0; x < CUR_SZ; x++) {
                int s = 0, n = 0;
                for (int k = -1; k <= 1; k++)
                    if (x + k >= 0 && x + k < CUR_SZ) { s += m[y * CUR_SZ + x + k]; n++; }
                t[y * CUR_SZ + x] = (uint8_t)(s / n);
            }
        for (int y = 0; y < CUR_SZ; y++)
            for (int x = 0; x < CUR_SZ; x++) {
                int s = 0, n = 0;
                for (int k = -1; k <= 1; k++)
                    if (y + k >= 0 && y + k < CUR_SZ) { s += t[(y + k) * CUR_SZ + x]; n++; }
                m[y * CUR_SZ + x] = (uint8_t)(s / n);
            }
    }
    for (int i = 0; i < CUR_SZ * CUR_SZ; i++) {
        uint32_t a = (uint32_t)m[i] * (uint32_t)alpha / 255;
        if (a) px[i] = a << 24;                   // black, premultiplied: just alpha
    }
}

// The shape over its shadow: a black edge (the path stroked two pixels wide,
// half of which the fill then covers) and a white fill.
static void shape(uint32_t *px, const rast_path *p, uint32_t fill) {
    rast_target t;
    target(&t, px);
    shadow(px, p, RAST_FRAC(6, 5), RAST_FRAC(9, 5), 110);
    rast_stroke s;
    rast_stroke_init(&s, RAST_FRAC(9, 5));
    s.join = RAST_JOIN_ROUND;          // a mitre at the arrow's tip would overshoot it
    rast_paint pt;
    rast_paint_solid(&pt, 0xFF000000);
    rast_draw_stroke(&t, p, 0, &s, &pt);
    rast_paint_solid(&pt, fill);
    rast_fill(&t, p, 0, RAST_NONZERO, &pt);
}

static void arrow_path(rast_path *p, rast_fx ox, rast_fx oy, int scale_pct) {
    // The classic arrow, tip at the origin, in pixels.
    static const int pts[][2] = {           // tenths of a pixel
        { 0, 0 }, { 0, 172 }, { 42, 132 }, { 72, 196 }, { 100, 184 },
        { 70, 122 }, { 126, 122 },
    };
    int n = (int)(sizeof pts / sizeof pts[0]);
    for (int i = 0; i < n; i++) {
        rast_fx x = ox + RAST_FRAC(pts[i][0] * scale_pct, 1000);
        rast_fx y = oy + RAST_FRAC(pts[i][1] * scale_pct, 1000);
        if (i == 0) rast_move_to(p, x, y);
        else rast_line_to(p, x, y);
    }
    rast_close(p);
}

// A double-headed arrow through the middle, turned by `deg`.
static void resize_path(rast_path *p, int deg) {
    static const int pts[][2] = {           // tenths, about the centre
        { -110, 0 }, { -50, -55 }, { -50, -17 }, { 50, -17 }, { 50, -55 },
        { 110, 0 }, { 50, 55 }, { 50, 17 }, { -50, 17 }, { -50, 55 },
    };
    rast_fx c = rast_cos(RAST_INT(deg)), s = rast_sin(RAST_INT(deg));
    rast_fx cx = RAST_INT(CUR_SZ / 2), cy = RAST_INT(CUR_SZ / 2);
    for (int i = 0; i < 10; i++) {
        rast_fx x = RAST_FRAC(pts[i][0], 10), y = RAST_FRAC(pts[i][1], 10);
        rast_fx X = cx + rast_mulfx(x, c) - rast_mulfx(y, s);
        rast_fx Y = cy + rast_mulfx(x, s) + rast_mulfx(y, c);
        if (i == 0) rast_move_to(p, X, Y);
        else rast_line_to(p, X, Y);
    }
    rast_close(p);
}

// The ring: blue glass, a dark edge for any background, and a bright arc
// at angle `a0` -- frame k of CUR_FRAMES puts it k sixteenths round.
static void ring(uint32_t *px, rast_fx cx, rast_fx cy, rast_fx r, rast_fx w, int a0) {
    rast_target t;
    target(&t, px);
    rast_path p;
    rast_path_init(&p);
    rast_stroke s;
    rast_paint pt;

    rast_ellipse(&p, cx, cy, r, r);
    rast_stroke_init(&s, w + RAST_INT(2));
    rast_paint_solid(&pt, 0x70000000);
    rast_draw_stroke(&t, &p, 0, &s, &pt);
    rast_stroke_init(&s, w);
    rast_paint_solid(&pt, 0xD03C88D0);
    rast_draw_stroke(&t, &p, 0, &s, &pt);
    rast_path_reset(&p);

    rast_arc(&p, cx, cy, r, RAST_INT(a0), RAST_INT(a0 + 110));
    s.cap = RAST_CAP_ROUND;
    rast_paint_solid(&pt, 0xFF8CE0FF);
    rast_draw_stroke(&t, &p, 0, &s, &pt);
    rast_path_reset(&p);
    rast_arc(&p, cx, cy, r, RAST_INT(a0 + 80), RAST_INT(a0 + 110));
    rast_paint_solid(&pt, 0xFFFFFFFF);
    rast_draw_stroke(&t, &p, 0, &s, &pt);
    rast_path_free(&p);
}

void cursor_init(void) {
    if (ready) return;
    rast_path p;
    rast_path_init(&p);

    // The arrow, and the arrow with the small ring beside it.
    clear(imgs[CUR_ARROW][0]);
    arrow_path(&p, RAST_FRAC(3, 2), RAST_FRAC(3, 2), 100);
    shape(imgs[CUR_ARROW][0], &p, 0xFFFFFFFF);
    hot[CUR_ARROW][0] = 1; hot[CUR_ARROW][1] = 1;
    for (int f = 0; f < FRAMED; f++) {
        uint32_t *px = imgs[CUR_APPSTART][f];
        for (int i = 0; i < CUR_SZ * CUR_SZ; i++) px[i] = imgs[CUR_ARROW][0][i];
        ring(px, RAST_INT(19), RAST_INT(20), RAST_FRAC(9, 2), RAST_FRAC(5, 2), f * 360 / FRAMED);
    }
    hot[CUR_APPSTART][0] = 1; hot[CUR_APPSTART][1] = 1;
    rast_path_reset(&p);

    // The four resizing arrows.
    static const int kinds[4] = { CUR_EW, CUR_NS, CUR_NWSE, CUR_NESW };
    static const int degs[4] = { 0, 90, 45, -45 };
    for (int k = 0; k < 4; k++) {
        clear(imgs[kinds[k]][0]);
        resize_path(&p, degs[k]);
        shape(imgs[kinds[k]][0], &p, 0xFFFFFFFF);
        rast_path_reset(&p);
        hot[kinds[k]][0] = CUR_SZ / 2;
        hot[kinds[k]][1] = CUR_SZ / 2;
    }

    // The busy ring on its own.
    for (int f = 0; f < FRAMED; f++) {
        clear(imgs[CUR_BUSY][f]);
        ring(imgs[CUR_BUSY][f], RAST_INT(CUR_SZ / 2), RAST_INT(CUR_SZ / 2), RAST_INT(9),
             RAST_INT(4), f * 360 / FRAMED);
    }
    hot[CUR_BUSY][0] = CUR_SZ / 2;
    hot[CUR_BUSY][1] = CUR_SZ / 2;

    rast_path_free(&p);
    ready = 1;
}

const uint32_t *cursor_image(int kind, int frame, int *hx, int *hy) {
    if (!ready || kind < 0 || kind >= CUR_KINDS) return 0;
    int animated = (kind == CUR_BUSY || kind == CUR_APPSTART);
    *hx = hot[kind][0];
    *hy = hot[kind][1];
    return imgs[kind][animated ? (frame % FRAMED) : 0];
}
