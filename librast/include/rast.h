#ifndef RAST_H
#define RAST_H

/* librast: shapes into pixels.
 *
 * A path -- straight lines, quadratic and cubic Beziers, elliptical arcs --
 * is filled or stroked into a block of 32-bit pixels, anti-aliased by the
 * exact area each pixel has inside the shape, under either winding rule,
 * with a solid colour or a linear or radial gradient, through any affine
 * transform, clipped to a rectangle and optionally to a coverage mask.
 * Strokes have the three joins and three caps SVG and canvas know, a miter
 * limit and dashes.
 *
 * ALL INTEGER. Programs built with -mgeneral-regs-only (web, and anything in
 * the kernel) have no floating point, so every number here is fixed point:
 * rast_fx is 16.16 for coordinates, matrix entries, widths and offsets.
 * Inside, the rasteriser works in 24.8 device units -- 1/256 of a pixel.
 *
 * How it works, in one paragraph: each edge is cut at every pixel boundary it
 * crosses, and for each pixel it passes through two numbers are summed -- how
 * far down the edge went inside the pixel (cover) and the area to its left
 * (area). Walking a row from the left, the running sum of cover says how
 * many edges have been crossed, which is the winding number, and cover minus
 * the area of the pixel it is in says how much of that pixel the edges
 * leave inside. That is exact, not sampled: a pixel half inside the shape
 * gets half the colour whichever way the edge runs through it.
 *
 * Nothing here is thread-safe: scratch memory is kept between calls.
 */

#include <stdint.h>

typedef int32_t rast_fx;                    /* 16.16 */
#define RAST_ONE        65536
#define RAST_INT(i)     ((rast_fx)(i) * RAST_ONE)
#define RAST_FRAC(n, d) ((rast_fx)(((int64_t)(n) * RAST_ONE) / (d)))

/* --- numbers ---------------------------------------------------------------- */

rast_fx  rast_mulfx(rast_fx a, rast_fx b);
rast_fx  rast_divfx(rast_fx a, rast_fx b);
rast_fx  rast_sqrtfx(rast_fx v);
rast_fx  rast_sin(rast_fx degrees);
rast_fx  rast_cos(rast_fx degrees);
rast_fx  rast_atan2(rast_fx y, rast_fx x);  /* degrees, -180..180 */
uint64_t rast_isqrt64(uint64_t v);

/* --- transforms ---------------------------------------------------------------
 *
 * x' = a*x + c*y + e,  y' = b*x + d*y + f: the six numbers in the order SVG's
 * matrix() and canvas's setTransform() give them. */
typedef struct { rast_fx a, b, c, d, e, f; } rast_mat;

rast_mat rast_identity(void);
rast_mat rast_mul(rast_mat m, rast_mat n);            /* n first, then m */
rast_mat rast_translate(rast_fx x, rast_fx y);
rast_mat rast_scale(rast_fx sx, rast_fx sy);
rast_mat rast_rotate(rast_fx degrees);
rast_mat rast_skew(rast_fx x_degrees, rast_fx y_degrees);
int      rast_invert(rast_mat m, rast_mat *out);      /* 0 if it has no inverse */
void     rast_apply(const rast_mat *m, rast_fx x, rast_fx y, rast_fx *ox, rast_fx *oy);
rast_fx  rast_mat_scale(const rast_mat *m);           /* how much lengths grow */

/* --- paths ------------------------------------------------------------------- */

enum { RAST_MOVE, RAST_LINE, RAST_QUAD, RAST_CUBIC, RAST_CLOSE };

typedef struct {
    uint8_t *ops;
    int      nops, capops;
    rast_fx *pts;              /* x,y pairs: 1 for MOVE/LINE, 2 QUAD, 3 CUBIC */
    int      npts, cappts;
    rast_fx  cx, cy;           /* the current point */
    rast_fx  sx, sy;           /* where the current subpath started */
    int      open;             /* a subpath is under way */
    int      err;              /* ran out of memory; the path is incomplete */
} rast_path;

void rast_path_init(rast_path *p);
void rast_path_free(rast_path *p);
void rast_path_reset(rast_path *p);             /* empty, memory kept */

void rast_move_to(rast_path *p, rast_fx x, rast_fx y);
void rast_line_to(rast_path *p, rast_fx x, rast_fx y);
void rast_quad_to(rast_path *p, rast_fx cx, rast_fx cy, rast_fx x, rast_fx y);
void rast_cubic_to(rast_path *p, rast_fx c1x, rast_fx c1y,
                   rast_fx c2x, rast_fx c2y, rast_fx x, rast_fx y);
/* An elliptical arc in the endpoint form of SVG's A command. */
void rast_arc_to(rast_path *p, rast_fx rx, rast_fx ry, rast_fx rotation,
                 int large, int sweep, rast_fx x, rast_fx y);
void rast_close(rast_path *p);

void rast_rect(rast_path *p, rast_fx x, rast_fx y, rast_fx w, rast_fx h);
void rast_round_rect(rast_path *p, rast_fx x, rast_fx y, rast_fx w, rast_fx h,
                     rast_fx rx, rast_fx ry);
void rast_ellipse(rast_path *p, rast_fx cx, rast_fx cy, rast_fx rx, rast_fx ry);
/* Part of a circle, from angle a0 to a1 in degrees, clockwise on screen for
 * a positive difference; joined to the current point by a line if there is
 * one. */
void rast_arc(rast_path *p, rast_fx cx, rast_fx cy, rast_fx r,
              rast_fx a0, rast_fx a1);

/* The box around every point of the path, control points included. 0 for an
 * empty path. */
int rast_path_bounds(const rast_path *p, rast_fx box[4]);

/* --- what is painted ------------------------------------------------------------ */

enum { RAST_NONZERO, RAST_EVENODD };
enum { RAST_SOLID, RAST_LINEAR, RAST_RADIAL };
enum { RAST_PAD, RAST_REFLECT, RAST_REPEAT };

typedef struct {
    rast_fx  offset;           /* 0..RAST_ONE */
    uint32_t color;            /* 0xAARRGGBB, not premultiplied */
} rast_stop;

typedef struct {
    int      type;
    uint32_t color;            /* SOLID: 0xAARRGGBB, not premultiplied */
    /* Gradients live in a space of their own; m takes it to the space the
     * path is drawn in (for SVG: gradientTransform, and the bounding box
     * when gradientUnits is objectBoundingBox). */
    rast_mat m;
    rast_fx  x1, y1, x2, y2;   /* LINEAR: t runs 0 at (x1,y1) to 1 at (x2,y2) */
    rast_fx  cx, cy, r;        /* RADIAL: t is 1 on this circle */
    rast_fx  fx, fy;           /*         and 0 at this focal point */
    int      spread;
    const rast_stop *stops;
    int      nstops;
    int      opacity;          /* 0..255, multiplied into everything */
} rast_paint;

void rast_paint_solid(rast_paint *pt, uint32_t argb);

/* --- where it goes ---------------------------------------------------------------- */

enum {
    RAST_XRGB,                 /* 0x..RRGGBB, opaque; the top byte is left alone */
    RAST_ARGB                  /* 0xAARRGGBB with the colour premultiplied */
};

typedef struct {
    uint32_t      *px;
    int            w, h, stride;           /* stride in pixels */
    int            format;
    int            cx0, cy0, cx1, cy1;     /* clip: [cx0,cx1) x [cy0,cy1) */
    const uint8_t *mask;                   /* coverage 0..255, or NULL */
    int            mask_stride;
} rast_target;

void rast_target_init(rast_target *t, uint32_t *px, int w, int h, int stride,
                      int format);

/* --- strokes ------------------------------------------------------------------------ */

enum { RAST_JOIN_MITER, RAST_JOIN_ROUND, RAST_JOIN_BEVEL };
enum { RAST_CAP_BUTT, RAST_CAP_ROUND, RAST_CAP_SQUARE };

typedef struct {
    rast_fx        width;
    int            join, cap;
    rast_fx        miter_limit;            /* SVG's meaning; 4 by default */
    const rast_fx *dash;                   /* on, off, on, off ... or NULL */
    int            ndash;
    rast_fx        dash_offset;
} rast_stroke;

void rast_stroke_init(rast_stroke *s, rast_fx width);

/* --- drawing -------------------------------------------------------------------------- */

/* Fill p, transformed by m (NULL for none), under `rule`. 0 if memory ran
 * out -- nothing, or only part, is drawn then. */
int rast_fill(rast_target *t, const rast_path *p, const rast_mat *m, int rule,
              const rast_paint *paint);

/* Stroke p. The stroke is built in the path's own space and then transformed,
 * so a scale that stretches one way stretches the pen the same way. */
int rast_draw_stroke(rast_target *t, const rast_path *p, const rast_mat *m,
                     const rast_stroke *s, const rast_paint *paint);

/* The coverage of p, added into an 8-bit mask by taking the larger of the two
 * at each pixel: several calls build the union of several shapes. For
 * clipping to a shape, give the mask to a target. */
int rast_fill_mask(uint8_t *mask, int w, int h, int stride, const rast_path *p,
                   const rast_mat *m, int rule);

/* --- colour ----------------------------------------------------------------------------- */

uint32_t rast_premul(uint32_t argb);
uint32_t rast_unpremul(uint32_t pargb);

#endif
