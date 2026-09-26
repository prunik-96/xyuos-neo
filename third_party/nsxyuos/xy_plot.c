/* The plotters: NetSurf's drawing, done in software into xyuOS's pixels.
 *
 * Everything here writes straight into the ARGB block the window manager
 * blits, because that is the only drawing this system has. That makes the
 * operations plain -- a rectangle is two loops -- and puts the whole cost of
 * a page redraw in this file, which is the honest place for it.
 *
 * Filled shapes go through one scanline filler. A polygon is the general
 * case, a circle is a polygon with enough sides not to look like one, and a
 * Bezier path is flattened to line segments and then filled the same way.
 * Writing three fillers would mean three places for the edge cases to be
 * wrong in different ways.
 */

#include <stdlib.h>
#include <string.h>

#include "utils/log.h"
#include "netsurf/plotters.h"
#include "netsurf/bitmap.h"

#include "xy_front.h"
#include "xy_bitmap.h"

int xy_cx0, xy_cy0, xy_cx1, xy_cy1;

void xy_clip_reset(void) {
    xy_cx0 = 0;
    xy_cy0 = 0;
    xy_cx1 = xy_gui.w;
    xy_cy1 = xy_gui.h;
}

/* --- the pixel ----------------------------------------------------------- */

static inline void put(int x, int y, unsigned c) {
    if (x < xy_cx0 || x >= xy_cx1 || y < xy_cy0 || y >= xy_cy1) return;
    xy_gui.px[(size_t)y * xy_gui.w + x] = c;
}

static inline void blend(int x, int y, unsigned c, unsigned a) {
    if (a == 0) return;
    if (a == 255) { put(x, y, c); return; }
    if (x < xy_cx0 || x >= xy_cx1 || y < xy_cy0 || y >= xy_cy1) return;
    unsigned *p = &xy_gui.px[(size_t)y * xy_gui.w + x];
    *p = gui_mix(*p, c, (int)a);
}

static void fill_rect(int x0, int y0, int x1, int y1, unsigned c) {
    if (x0 < xy_cx0) x0 = xy_cx0;
    if (y0 < xy_cy0) y0 = xy_cy0;
    if (x1 > xy_cx1) x1 = xy_cx1;
    if (y1 > xy_cy1) y1 = xy_cy1;
    for (int y = y0; y < y1; y++) {
        unsigned *row = xy_gui.px + (size_t)y * xy_gui.w;
        for (int x = x0; x < x1; x++) row[x] = c;
    }
}

/* --- lines --------------------------------------------------------------- */

/* Bresenham, with the dotted and dashed patterns NetSurf asks for. `phase`
 * counts pixels along the line so the gaps stay evenly spaced however the
 * line slopes. */
static void draw_line(int x0, int y0, int x1, int y1, unsigned c,
                      plot_operation_type_t type, int width) {
    if (width < 1) width = 1;

    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int phase = 0;

    for (;;) {
        int show = 1;
        if (type == PLOT_OP_TYPE_DOT)  show = (phase & 2) == 0;
        if (type == PLOT_OP_TYPE_DASH) show = (phase % 8) < 5;

        if (show) {
            if (width == 1) {
                put(x0, y0, c);
            } else {
                int h = width / 2;
                for (int oy = -h; oy <= h; oy++)
                    for (int ox = -h; ox <= h; ox++)
                        put(x0 + ox, y0 + oy, c);
            }
        }
        phase++;

        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* --- the one filler ------------------------------------------------------ */

/* Scanline fill of a closed polygon, non-zero winding, which is the rule
 * NetSurf's documentation asks for. Crossings are gathered per row with the
 * direction each edge runs, and the span is inside wherever the running sum
 * is not zero.
 */
typedef struct { int x; int dir; } crossing;

static void fill_poly(const int *pts, unsigned n, unsigned c) {
    if (n < 3) return;

    int ymin = pts[1], ymax = pts[1];
    for (unsigned i = 1; i < n; i++) {
        int y = pts[i * 2 + 1];
        if (y < ymin) ymin = y;
        if (y > ymax) ymax = y;
    }
    if (ymin < xy_cy0) ymin = xy_cy0;
    if (ymax > xy_cy1) ymax = xy_cy1;
    if (ymin >= ymax) return;

    crossing *xs = malloc(sizeof *xs * n);
    if (xs == NULL) return;

    for (int y = ymin; y < ymax; y++) {
        unsigned k = 0;
        int cy = y * 2 + 1;                 /* sample down the row's middle */

        for (unsigned i = 0; i < n; i++) {
            unsigned j = (i + 1) % n;
            int ay = pts[i * 2 + 1] * 2, by = pts[j * 2 + 1] * 2;
            if (ay == by) continue;
            int lo = ay < by ? ay : by, hi = ay < by ? by : ay;
            if (cy < lo || cy >= hi) continue;

            int ax = pts[i * 2], bx = pts[j * 2];
            /* Where this edge is at the sample row. */
            int x = ax + (int)((long)(bx - ax) * (cy - ay) / (by - ay));
            xs[k].x = x;
            xs[k].dir = (by > ay) ? 1 : -1;
            k++;
        }
        if (k < 2) continue;

        for (unsigned a = 1; a < k; a++) {   /* few crossings; insertion */
            crossing t = xs[a];
            unsigned b = a;
            while (b > 0 && xs[b - 1].x > t.x) { xs[b] = xs[b - 1]; b--; }
            xs[b] = t;
        }

        int wind = 0;
        for (unsigned a = 0; a + 1 < k; a++) {
            wind += xs[a].dir;
            if (wind != 0) fill_rect(xs[a].x, y, xs[a + 1].x, y + 1, c);
        }
    }
    free(xs);
}

/* --- the operations ------------------------------------------------------ */

static nserror xyp_clip(const struct redraw_context *ctx,
                        const struct rect *clip) {
    (void)ctx;
    xy_cx0 = clip->x0 < 0 ? 0 : clip->x0;
    xy_cy0 = clip->y0 < 0 ? 0 : clip->y0;
    xy_cx1 = clip->x1 > xy_gui.w ? xy_gui.w : clip->x1;
    xy_cy1 = clip->y1 > xy_gui.h ? xy_gui.h : clip->y1;
    if (xy_cx1 < xy_cx0) xy_cx1 = xy_cx0;
    if (xy_cy1 < xy_cy0) xy_cy1 = xy_cy0;
    return NSERROR_OK;
}

static nserror xyp_rectangle(const struct redraw_context *ctx,
                             const plot_style_t *st, const struct rect *r) {
    (void)ctx;
    if (st->fill_type != PLOT_OP_TYPE_NONE)
        fill_rect(r->x0, r->y0, r->x1, r->y1, xy_colour(st->fill_colour));

    if (st->stroke_type != PLOT_OP_TYPE_NONE) {
        unsigned c = xy_colour(st->stroke_colour);
        int w = plot_style_fixed_to_int(st->stroke_width);
        if (w < 1) w = 1;
        draw_line(r->x0, r->y0, r->x1 - 1, r->y0, c, st->stroke_type, w);
        draw_line(r->x1 - 1, r->y0, r->x1 - 1, r->y1 - 1, c, st->stroke_type, w);
        draw_line(r->x1 - 1, r->y1 - 1, r->x0, r->y1 - 1, c, st->stroke_type, w);
        draw_line(r->x0, r->y1 - 1, r->x0, r->y0, c, st->stroke_type, w);
    }
    return NSERROR_OK;
}

static nserror xyp_line(const struct redraw_context *ctx,
                        const plot_style_t *st, const struct rect *l) {
    (void)ctx;
    if (st->stroke_type == PLOT_OP_TYPE_NONE) return NSERROR_OK;
    int w = plot_style_fixed_to_int(st->stroke_width);
    draw_line(l->x0, l->y0, l->x1, l->y1,
              xy_colour(st->stroke_colour), st->stroke_type, w);
    return NSERROR_OK;
}

static nserror xyp_polygon(const struct redraw_context *ctx,
                           const plot_style_t *st, const int *p, unsigned n) {
    (void)ctx;
    if (st->fill_type != PLOT_OP_TYPE_NONE)
        fill_poly(p, n, xy_colour(st->fill_colour));
    return NSERROR_OK;
}

/* A circle is a polygon with enough sides. Sixteen is too few at any size a
 * page uses, so the count follows the radius and stops where more would not
 * show. */
static int circle_points(int *out, int cap, int cx, int cy, int r,
                         int a0, int a1) {
    int sides = r * 2;
    if (sides < 12) sides = 12;
    if (sides > cap) sides = cap;

    int span = a1 - a0;
    while (span < 0) span += 360;
    if (span == 0) span = 360;

    for (int i = 0; i < sides; i++) {
        /* Fixed point, because there is no floating point trigonometry here
         * and none is needed: a table of the unit circle would be the same
         * thing with a lookup. */
        long deg = a0 + (long)span * i / (sides - 1);
        long rad10000 = deg * 174533 / 10000;     /* degrees -> radians e4 */
        /* Taylor is fine once the angle is folded into one quadrant. */
        long t = rad10000 % 62832;
        if (t < 0) t += 62832;
        int quad = (int)(t / 15708);
        long u = t % 15708;
        long s = u - (u * u / 10000) * u / 60000;              /* sin, e4 */
        long v = 15708 - u;
        long co = v - (v * v / 10000) * v / 60000;
        long sx, cxv;
        switch (quad) {
        case 0: sx =  s; cxv =  co; break;
        case 1: sx =  co; cxv = -s; break;
        case 2: sx = -s; cxv = -co; break;
        default: sx = -co; cxv =  s; break;
        }
        out[i * 2]     = cx + (int)((long)r * cxv / 10000);
        out[i * 2 + 1] = cy - (int)((long)r * sx / 10000);
    }
    return sides;
}

static nserror xyp_disc(const struct redraw_context *ctx,
                        const plot_style_t *st, int x, int y, int radius) {
    (void)ctx;
    if (radius <= 0) return NSERROR_OK;

    if (st->fill_type != PLOT_OP_TYPE_NONE) {
        /* Straight from the definition, which for a filled circle is both
         * simpler and more exact than going round the edge. */
        unsigned c = xy_colour(st->fill_colour);
        for (int dy = -radius; dy <= radius; dy++) {
            int half = 0;
            while ((half + 1) * (half + 1) + dy * dy <= radius * radius) half++;
            fill_rect(x - half, y + dy, x + half + 1, y + dy + 1, c);
        }
    }
    if (st->stroke_type != PLOT_OP_TYPE_NONE) {
        int cap = 512;
        int *pts = malloc(sizeof(int) * 2 * cap);
        if (pts != NULL) {
            int n = circle_points(pts, cap, x, y, radius, 0, 360);
            unsigned c = xy_colour(st->stroke_colour);
            for (int i = 0; i < n; i++) {
                int j = (i + 1) % n;
                draw_line(pts[i * 2], pts[i * 2 + 1],
                          pts[j * 2], pts[j * 2 + 1], c, st->stroke_type, 1);
            }
            free(pts);
        }
    }
    return NSERROR_OK;
}

static nserror xyp_arc(const struct redraw_context *ctx,
                       const plot_style_t *st, int x, int y, int radius,
                       int angle1, int angle2) {
    (void)ctx;
    if (radius <= 0) return NSERROR_OK;
    int cap = 512;
    int *pts = malloc(sizeof(int) * 2 * cap);
    if (pts == NULL) return NSERROR_NOMEM;
    int n = circle_points(pts, cap, x, y, radius, angle1, angle2);
    unsigned c = xy_colour(st->stroke_type != PLOT_OP_TYPE_NONE
                           ? st->stroke_colour : st->fill_colour);
    for (int i = 0; i + 1 < n; i++)
        draw_line(pts[i * 2], pts[i * 2 + 1],
                  pts[(i + 1) * 2], pts[(i + 1) * 2 + 1], c,
                  PLOT_OP_TYPE_SOLID, 1);
    free(pts);
    return NSERROR_OK;
}

/* --- paths --------------------------------------------------------------- */

/* NetSurf hands paths over as a stream of MOVE / LINE / CUBIC / CLOSE
 * commands with a transform. Curves are cut into straight pieces and the
 * result goes through the same filler as a polygon. The number of pieces
 * follows the size of the curve, so a small one does not pay for smoothness
 * nobody can see. */
static void tx(const float m[6], float x, float y, int *ox, int *oy) {
    *ox = (int)(m[0] * x + m[2] * y + m[4]);
    *oy = (int)(m[1] * x + m[3] * y + m[5]);
}

static nserror xyp_path(const struct redraw_context *ctx,
                        const plot_style_t *st, const float *p, unsigned n,
                        const float transform[6]) {
    (void)ctx;
    if (n == 0) return NSERROR_OK;

    unsigned cap = 256, used = 0;
    int *pts = malloc(sizeof(int) * 2 * cap);
    if (pts == NULL) return NSERROR_NOMEM;

    float cx = 0, cy = 0;

#define PUSH(fx, fy) do {                                               \
        if (used == cap) {                                              \
            unsigned nc = cap * 2;                                      \
            int *np = realloc(pts, sizeof(int) * 2 * nc);               \
            if (np == NULL) { free(pts); return NSERROR_NOMEM; }        \
            pts = np; cap = nc;                                         \
        }                                                               \
        tx(transform, (fx), (fy), &pts[used * 2], &pts[used * 2 + 1]);  \
        used++;                                                         \
    } while (0)

    unsigned i = 0;
    while (i < n) {
        int op = (int)p[i];
        if (op == PLOTTER_PATH_MOVE && i + 2 < n) {
            cx = p[i + 1]; cy = p[i + 2];
            PUSH(cx, cy);
            i += 3;
        } else if (op == PLOTTER_PATH_LINE && i + 2 < n) {
            cx = p[i + 1]; cy = p[i + 2];
            PUSH(cx, cy);
            i += 3;
        } else if (op == PLOTTER_PATH_BEZIER && i + 6 < n) {
            float x1 = p[i + 1], y1 = p[i + 2];
            float x2 = p[i + 3], y2 = p[i + 4];
            float x3 = p[i + 5], y3 = p[i + 6];

            float dx = x3 - cx, dy = y3 - cy;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            int steps = (int)((dx + dy) / 3.0f) + 4;
            if (steps > 64) steps = 64;

            for (int s = 1; s <= steps; s++) {
                float t = (float)s / steps, u = 1.0f - t;
                float bx = u*u*u*cx + 3*u*u*t*x1 + 3*u*t*t*x2 + t*t*t*x3;
                float by = u*u*u*cy + 3*u*u*t*y1 + 3*u*t*t*y2 + t*t*t*y3;
                PUSH(bx, by);
            }
            cx = x3; cy = y3;
            i += 7;
        } else if (op == PLOTTER_PATH_CLOSE) {
            i += 1;
        } else {
            break;                        /* not a shape we can read */
        }
    }
#undef PUSH

    if (used >= 3) {
        if (st->fill_type != PLOT_OP_TYPE_NONE)
            fill_poly(pts, used, xy_colour(st->fill_colour));
        if (st->stroke_type != PLOT_OP_TYPE_NONE) {
            unsigned c = xy_colour(st->stroke_colour);
            int w = plot_style_fixed_to_int(st->stroke_width);
            for (unsigned k = 0; k + 1 < used; k++)
                draw_line(pts[k * 2], pts[k * 2 + 1],
                          pts[(k + 1) * 2], pts[(k + 1) * 2 + 1],
                          c, st->stroke_type, w);
        }
    }
    free(pts);
    return NSERROR_OK;
}

/* --- images -------------------------------------------------------------- */

static nserror xyp_bitmap(const struct redraw_context *ctx,
                          struct bitmap *bitmap, int x, int y,
                          int width, int height, colour bg,
                          bitmap_flags_t flags) {
    (void)ctx;
    struct xy_bitmap *bm = (struct xy_bitmap *)bitmap;
    if (bm == NULL || width <= 0 || height <= 0) return NSERROR_OK;

    unsigned back = xy_colour(bg);

    /* Tiling: work out which copies can touch the clip, and draw only
     * those. Asking for a tiled background on a tall page otherwise means
     * drawing the whole page whatever is visible. */
    int fx = (flags & BITMAPF_REPEAT_X) ? 1 : 0;
    int fy = (flags & BITMAPF_REPEAT_Y) ? 1 : 0;

    int tx0 = 0, tx1 = 0, ty0 = 0, ty1 = 0;
    if (fx) {
        tx0 = (xy_cx0 - x - width + 1) / width;
        tx1 = (xy_cx1 - x) / width;
        if ((xy_cx0 - x) % width && xy_cx0 < x) tx0--;
    }
    if (fy) {
        ty0 = (xy_cy0 - y - height + 1) / height;
        ty1 = (xy_cy1 - y) / height;
        if ((xy_cy0 - y) % height && xy_cy0 < y) ty0--;
    }

    for (int ty = ty0; ty <= ty1; ty++) {
        for (int txi = tx0; txi <= tx1; txi++) {
            int ox = x + txi * width;
            int oy = y + ty * height;

            int px0 = ox > xy_cx0 ? ox : xy_cx0;
            int py0 = oy > xy_cy0 ? oy : xy_cy0;
            int px1 = (ox + width)  < xy_cx1 ? (ox + width)  : xy_cx1;
            int py1 = (oy + height) < xy_cy1 ? (oy + height) : xy_cy1;

            for (int py = py0; py < py1; py++) {
                int sv = (py - oy) * bm->h / height;
                const unsigned char *srow = bm->data + (size_t)sv * bm->stride;
                unsigned *drow = xy_gui.px + (size_t)py * xy_gui.w;

                for (int px = px0; px < px1; px++) {
                    int su = (px - ox) * bm->w / width;
                    const unsigned char *s = srow + su * 4;
                    unsigned a = s[3];
                    unsigned col = ((unsigned)s[0] << 16) |
                                   ((unsigned)s[1] << 8) | s[2];
                    if (a == 255 || bm->opaque) {
                        drow[px] = col;
                    } else if (a > 0) {
                        /* The background NetSurf named, then what is
                         * already there: a half-transparent image over a
                         * coloured box has to see the box. */
                        unsigned under = (bg == NS_TRANSPARENT)
                                       ? drow[px] : back;
                        drow[px] = gui_mix(under, col, (int)a);
                    }
                }
            }
        }
    }
    return NSERROR_OK;
}

/* --- text ---------------------------------------------------------------- */

/* What follows is for the bitmap font only, when there are no fonts on the
 * disk; with them, libtext draws every one of these characters as itself.
 *
 * The window manager's font covers Latin, Latin-1, Latin Extended-A and
 * Cyrillic. Typography lives above all of that: the quotation marks a word
 * processor inserts, the dash between a range of numbers, the ellipsis, the
 * bullet in front of every item of every list. A page is full of them, and
 * gui_glyph draws anything it does not have as a question mark -- which is
 * why the welcome page appeared to have a broken image before each link and
 * did not.
 *
 * So the ones that have a plain equivalent are given it. This is the same
 * substitution a terminal does, and it is far better than a row of question
 * marks: an em dash drawn as a hyphen reads correctly, and a hyphen is what
 * the writer meant before their editor was clever.
 *
 * Returns 0 for a character to be drawn as a shape instead. */
static int plain_form(int cp) {
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
    case 0x2013: case 0x2014: case 0x2015: case 0x2212: return '-';
    case 0x2026: return '.';          /* one dot; three would shift the line */
    case 0x00A0: case 0x2002: case 0x2003: case 0x2009: return ' ';
    case 0x2039: return '<';
    case 0x203A: return '>';
    case 0x00AB: case 0x00BB: return '"';
    case 0x2022: case 0x2023: case 0x25CF: case 0x25AA: return 0;  /* a dot */
    case 0x25CB: case 0x25E6: case 0x25A1: return 0;
    case 0x2192: return '>';
    case 0x2190: return '<';
    case 0x00D7: return 'x';
    case 0x2122: return 'T';
    default: return -1;               /* leave it to the font */
    }
}

static nserror xyp_text(const struct redraw_context *ctx,
                        const plot_font_style_t *fstyle, int x, int y,
                        const char *text, size_t length) {
    (void)ctx;
    if (xy_text) {
        /* Real fonts: every script, every size, the same shaping the layout
         * measured with. y is the baseline, which is what libtext wants. */
        txt_style st;
        xy_text_style(fstyle, &st);
        txt_target t = { xy_gui.px, xy_gui.w, xy_cx0, xy_cy0, xy_cx1, xy_cy1 };
        txt_draw(&t, &st, x, y, xy_colour(fstyle->foreground), text, length);
        return NSERROR_OK;
    }

    int scale = xy_font_scale(fstyle);
    unsigned c = xy_colour(fstyle->foreground);
    int top = y - xy_font_ascent(scale);
    int step = xy_gui.fw * scale;

    /* Nothing of this line can show: stop before decoding it. */
    if (top >= xy_cy1 || top + xy_gui.fh * scale <= xy_cy0)
        return NSERROR_OK;

    for (size_t i = 0; i < length && text[i]; ) {
        int cp;
        i += (size_t)gui_utf8(text + i, &cp);
        if (x >= xy_cx1) break;

        if (x + step > xy_cx0 && cp != ' ') {
            int sub = plain_form(cp);
            if (sub == 0) {
                /* A bullet: a small filled square in the middle of the cell.
                 * Round would be prettier and at this size indistinguishable
                 * from square, which is two loops rather than a circle. */
                int d = scale * 2;
                if (d < 2) d = 2;
                for (int dy = 0; dy < d; dy++)
                    for (int dx = 0; dx < d; dx++)
                        put(x + (step - d) / 2 + dx,
                            top + (xy_gui.fh * scale - d) / 2 + dy, c);
            } else {
                gui_glyph(&xy_gui, x, top, sub > 0 ? sub : cp, c, scale);
            }
        }
        x += step;
    }
    return NSERROR_OK;
}

const struct plotter_table xy_plotters = {
    .clip       = xyp_clip,
    .arc        = xyp_arc,
    .disc       = xyp_disc,
    .line       = xyp_line,
    .rectangle  = xyp_rectangle,
    .polygon    = xyp_polygon,
    .path       = xyp_path,
    .bitmap     = xyp_bitmap,
    .text       = xyp_text,
    /* group_start, group_end and flush are for plotters that write to a
     * file; this one draws on a screen. flush must be NULL for a display
     * plotter -- the knockout code owns it. */
    .option_knockout = true,
};
