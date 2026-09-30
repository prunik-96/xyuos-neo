/* The plotters: NetSurf's drawing, done in software into xyuOS's pixels.
 *
 * Everything here writes straight into the ARGB block the window manager
 * blits, because that is the only drawing this system has. That makes the
 * operations plain -- a rectangle is two loops -- and puts the whole cost of
 * a page redraw in this file, which is the honest place for it.
 *
 * Shapes -- polygons, discs, arcs, paths, and any line that is neither level
 * nor upright -- are librast's: filled and stroked anti-aliased by exact
 * area, so a bullet is round and a diagonal is a line rather than a stair.
 * The level and upright solid lines that make up almost every border and
 * rule stay what NetSurf means by them, whole pixels, filled as the
 * rectangles they are.
 */

#include <stdlib.h>
#include <string.h>

#include "utils/log.h"
#include "netsurf/plotters.h"
#include "netsurf/bitmap.h"

#include "xy_front.h"
#include "xy_bitmap.h"
#include "rast.h"

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

/* --- librast ------------------------------------------------------------ */

/* The surface, clipped as NetSurf last asked. */
static rast_target surface(void) {
    rast_target t;
    rast_target_init(&t, (uint32_t *)xy_gui.px, xy_gui.w, xy_gui.h, xy_gui.w, RAST_XRGB);
    t.cx0 = xy_cx0; t.cy0 = xy_cy0;
    t.cx1 = xy_cx1; t.cy1 = xy_cy1;
    return t;
}

static void paint_of(rast_paint *pt, colour c) {
    rast_paint_solid(pt, 0xFF000000u | xy_colour(c));
}

/* One path, reused: NetSurf plots shapes one after another, never two at once. */
static rast_path shape;

#define PX(v)    RAST_INT(v)
#define CENTRE(v) (RAST_INT(v) + RAST_ONE / 2)

/* A stroke along the path in `shape`, in NetSurf's patterns. */
static void stroke_shape(unsigned c, plot_operation_type_t type, rast_fx width,
                         int cap, int join) {
    rast_stroke s;
    rast_stroke_init(&s, width < RAST_ONE ? RAST_ONE : width);
    s.cap = cap;
    s.join = join;
    rast_fx dash[2];
    if (type == PLOT_OP_TYPE_DOT) {
        dash[0] = dash[1] = 2 * s.width;
        s.dash = dash; s.ndash = 2;
        s.cap = RAST_CAP_BUTT;
    } else if (type == PLOT_OP_TYPE_DASH) {
        dash[0] = 5 * s.width; dash[1] = 3 * s.width;
        s.dash = dash; s.ndash = 2;
        s.cap = RAST_CAP_BUTT;
    }
    rast_target t = surface();
    rast_paint pt;
    rast_paint_solid(&pt, 0xFF000000u | c);
    rast_draw_stroke(&t, &shape, NULL, &s, &pt);
}

/* --- lines --------------------------------------------------------------- */

/* A level or upright solid line covers whole pixels, the ones NetSurf named,
 * `width` of them across. Dotted and dashed ones keep the pixel pattern
 * they always had. Anything at an angle goes through the pixel centres to
 * librast, with square ends so that both named end pixels are covered as
 * they would have been. */
static void draw_line(int x0, int y0, int x1, int y1, unsigned c,
                      plot_operation_type_t type, int width) {
    if (width < 1) width = 1;

    if ((x0 == x1 || y0 == y1) && type == PLOT_OP_TYPE_SOLID) {
        int h = width / 2;
        if (y0 == y1)
            fill_rect(x0 < x1 ? x0 : x1, y0 - h, (x0 < x1 ? x1 : x0) + 1, y0 - h + width, c);
        else
            fill_rect(x0 - h, y0 < y1 ? y0 : y1, x0 - h + width, (y0 < y1 ? y1 : y0) + 1, c);
        return;
    }

    if (x0 == x1 || y0 == y1) {
        int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int n = dx > dy ? dx : dy, h = width / 2;
        for (int i = 0, phase = 0; i <= n; i++, phase++) {
            int show = (type == PLOT_OP_TYPE_DOT) ? (phase & 2) == 0 : (phase % 8) < 5;
            if (!show) continue;
            int x = x0 + (dx ? sx * i : 0), y = y0 + (dy ? sy * i : 0);
            if (y0 == y1) fill_rect(x, y - h, x + 1, y - h + width, c);
            else          fill_rect(x - h, y, x - h + width, y + 1, c);
        }
        return;
    }

    rast_path_reset(&shape);
    rast_move_to(&shape, CENTRE(x0), CENTRE(y0));
    rast_line_to(&shape, CENTRE(x1), CENTRE(y1));
    stroke_shape(c, type, PX(width), RAST_CAP_SQUARE, RAST_JOIN_MITER);
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

/* The points are pixel corners, so a polygon with level and upright sides
 * lands exactly on pixels, and a slanted side is smoothed. */
static nserror xyp_polygon(const struct redraw_context *ctx,
                           const plot_style_t *st, const int *p, unsigned n) {
    (void)ctx;
    if (st->fill_type == PLOT_OP_TYPE_NONE || n < 3) return NSERROR_OK;
    rast_path_reset(&shape);
    for (unsigned i = 0; i < n; i++) {
        if (i == 0) rast_move_to(&shape, PX(p[0]), PX(p[1]));
        else rast_line_to(&shape, PX(p[i * 2]), PX(p[i * 2 + 1]));
    }
    rast_close(&shape);
    rast_target t = surface();
    rast_paint pt;
    paint_of(&pt, st->fill_colour);
    rast_fill(&t, &shape, NULL, RAST_NONZERO, &pt);
    return NSERROR_OK;
}

/* A disc is centred on the middle of the pixel NetSurf names. Filled, it
 * reaches half a pixel past the radius -- the pixels the old one lit, now
 * with a smooth edge; outlined, the ring runs through the pixels at that
 * radius. */
static nserror xyp_disc(const struct redraw_context *ctx,
                        const plot_style_t *st, int x, int y, int radius) {
    (void)ctx;
    if (radius <= 0) return NSERROR_OK;
    rast_target t = surface();
    rast_paint pt;
    if (st->fill_type != PLOT_OP_TYPE_NONE) {
        rast_path_reset(&shape);
        rast_ellipse(&shape, CENTRE(x), CENTRE(y), PX(radius) + RAST_ONE / 2,
                     PX(radius) + RAST_ONE / 2);
        paint_of(&pt, st->fill_colour);
        rast_fill(&t, &shape, NULL, RAST_NONZERO, &pt);
    }
    if (st->stroke_type != PLOT_OP_TYPE_NONE) {
        rast_path_reset(&shape);
        rast_ellipse(&shape, CENTRE(x), CENTRE(y), PX(radius), PX(radius));
        stroke_shape(xy_colour(st->stroke_colour), st->stroke_type,
                     (rast_fx)st->stroke_width << 6, RAST_CAP_BUTT, RAST_JOIN_ROUND);
    }
    return NSERROR_OK;
}

/* NetSurf's angles run anticlockwise from three o'clock, the way they do on
 * paper; librast's run clockwise on the screen, the way y does. So they are
 * turned over. */
static nserror xyp_arc(const struct redraw_context *ctx,
                       const plot_style_t *st, int x, int y, int radius,
                       int angle1, int angle2) {
    (void)ctx;
    if (radius <= 0) return NSERROR_OK;
    if (angle2 < angle1) angle2 += 360;
    rast_path_reset(&shape);
    rast_arc(&shape, CENTRE(x), CENTRE(y), PX(radius), PX(-angle1), PX(-angle2));
    colour c = st->stroke_type != PLOT_OP_TYPE_NONE ? st->stroke_colour : st->fill_colour;
    stroke_shape(xy_colour(c), PLOT_OP_TYPE_SOLID, RAST_ONE, RAST_CAP_BUTT, RAST_JOIN_ROUND);
    return NSERROR_OK;
}

/* --- paths --------------------------------------------------------------- */

/* NetSurf hands paths over as a stream of MOVE / LINE / BEZIER / CLOSE with a
 * transform. The transform is applied to the points here, so that the
 * stroke width stays what it was given in: pixels. Curves stay curves --
 * librast flattens them after the transform, to a tenth of a pixel. */
static rast_fx fx_of(float v) {
    float s = v * 65536.0f;
    if (s > 1073741823.0f) return 0x3FFFFFFF;
    if (s < -1073741823.0f) return -0x3FFFFFFF;
    return (rast_fx)s;
}

static void tx(const float m[6], float x, float y, rast_fx *ox, rast_fx *oy) {
    *ox = fx_of(m[0] * x + m[2] * y + m[4]);
    *oy = fx_of(m[1] * x + m[3] * y + m[5]);
}

static nserror xyp_path(const struct redraw_context *ctx,
                        const plot_style_t *st, const float *p, unsigned n,
                        const float transform[6]) {
    (void)ctx;
    if (n == 0) return NSERROR_OK;
    rast_path_reset(&shape);

    unsigned i = 0;
    while (i < n) {
        int op = (int)p[i];
        rast_fx x1, y1, x2, y2, x3, y3;
        if (op == PLOTTER_PATH_MOVE && i + 2 < n) {
            tx(transform, p[i + 1], p[i + 2], &x1, &y1);
            rast_move_to(&shape, x1, y1);
            i += 3;
        } else if (op == PLOTTER_PATH_LINE && i + 2 < n) {
            tx(transform, p[i + 1], p[i + 2], &x1, &y1);
            rast_line_to(&shape, x1, y1);
            i += 3;
        } else if (op == PLOTTER_PATH_BEZIER && i + 6 < n) {
            tx(transform, p[i + 1], p[i + 2], &x1, &y1);
            tx(transform, p[i + 3], p[i + 4], &x2, &y2);
            tx(transform, p[i + 5], p[i + 6], &x3, &y3);
            rast_cubic_to(&shape, x1, y1, x2, y2, x3, y3);
            i += 7;
        } else if (op == PLOTTER_PATH_CLOSE) {
            rast_close(&shape);
            i += 1;
        } else {
            break;                        /* not a shape we can read */
        }
    }

    if (st->fill_type != PLOT_OP_TYPE_NONE) {
        rast_target t = surface();
        rast_paint pt;
        paint_of(&pt, st->fill_colour);
        rast_fill(&t, &shape, NULL, RAST_NONZERO, &pt);
    }
    if (st->stroke_type != PLOT_OP_TYPE_NONE)
        stroke_shape(xy_colour(st->stroke_colour), st->stroke_type,
                     (rast_fx)st->stroke_width << 6, RAST_CAP_BUTT, RAST_JOIN_MITER);
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
