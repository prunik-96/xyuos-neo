/* The rasteriser: edges into cells, cells into coverage, coverage into
 * paint. See rast.h for the idea in a paragraph; the details are here.
 *
 * Units. Device coordinates are 24.8 -- a pixel is 256. For each pixel an
 * edge passes through, `cover` sums how far it descends inside the pixel
 * (downwards positive: a full crossing is +/-256) and `area` sums that
 * descent times twice its mean distance from the pixel's left side. Summing
 * cover along a row from the left gives the winding number times 256 at any
 * point; for the pixel an edge is in, cover*512 - area is the part of the
 * pixel to the edge's right, times 512. So a pixel's coverage is exact to
 * 1/256 whatever the edges in it do.
 */

#include "rast_int.h"
#include <limits.h>

/* --- cells ------------------------------------------------------------------------ */

typedef struct { int32_t x, y, cover, area; } rcell;

static rcell  *cells;
static int     ncells, capcells;
static int     cell_err;
static int32_t cur_x, cur_y, cur_cover, cur_area;
static int     have_cur;
static int32_t box_x0, box_y0, box_x1, box_y1;   /* the clip, 24.8 */
static int32_t row_lo, row_hi;                   /* rows with cells */

static void cells_begin(const rast_target *t) {
    ncells = 0;
    cell_err = 0;
    have_cur = 0;
    box_x0 = t->cx0 * 256;
    box_y0 = t->cy0 * 256;
    box_x1 = t->cx1 * 256;
    box_y1 = t->cy1 * 256;
    row_lo = INT_MAX;
    row_hi = INT_MIN;
}

static void cell_flush(void) {
    if (!have_cur) return;
    have_cur = 0;
    if (!(cur_cover | cur_area)) return;
    if (ncells == capcells) {
        int cap = capcells ? capcells * 2 : 4096;
        rcell *n = (rcell *)realloc(cells, (size_t)cap * sizeof *n);
        if (!n) { cell_err = 1; return; }
        cells = n;
        capcells = cap;
    }
    rcell *c = &cells[ncells++];
    c->x = cur_x; c->y = cur_y; c->cover = cur_cover; c->area = cur_area;
    if (cur_y < row_lo) row_lo = cur_y;
    if (cur_y > row_hi) row_hi = cur_y;
}

static inline void cell_add(int32_t x, int32_t y, int32_t cover, int32_t area) {
    if (!have_cur || x != cur_x || y != cur_y) {
        cell_flush();
        cur_x = x; cur_y = y; cur_cover = 0; cur_area = 0;
        have_cur = 1;
    }
    cur_cover += cover;
    cur_area += area;
}

/* The part of an edge inside one row: from (xa, fya) to (xb, fyb), with the
 * y values 0..256 inside the row and fya < fyb. `dir` is +1 for an edge that
 * runs down the page and -1 for one that runs up. */
static void row_piece(int32_t ey, int32_t xa, int32_t fya, int32_t xb, int32_t fyb, int dir) {
    int32_t exa = xa >> 8, exb = xb >> 8;
    if (exa == exb) {
        int32_t dy = (fyb - fya) * dir;
        cell_add(exa, ey, dy, dy * ((xa - exa * 256) + (xb - exa * 256)));
        return;
    }
    /* Across several pixels: cut at each boundary. Every cut is worked out
     * from the ends of the piece, so rounding never accumulates. */
    int64_t ddx = (int64_t)xb - xa, ddy = fyb - fya;
    int32_t x = xa, y = fya, ex = exa;
    if (xb > xa) {
        while (ex != exb) {
            int32_t bound = (ex + 1) * 256;
            int32_t yn = fya + (int32_t)((ddy * (bound - xa)) / ddx);
            int32_t dy = (yn - y) * dir;
            cell_add(ex, ey, dy, dy * ((x - ex * 256) + 256));
            x = bound; y = yn; ex++;
        }
    } else {
        while (ex != exb) {
            int32_t bound = ex * 256;
            int32_t yn = fya + (int32_t)((ddy * (bound - xa)) / ddx);
            int32_t dy = (yn - y) * dir;
            cell_add(ex, ey, dy, dy * (x - ex * 256));
            x = bound; y = yn; ex--;
        }
    }
    int32_t dy = (fyb - y) * dir;
    cell_add(exb, ey, dy, dy * ((x - exb * 256) + (xb - exb * 256)));
}

/* An edge already inside the clip box, 24.8. */
static void cells_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (y0 == y1) return;                  /* level: it crosses no row */
    int dir = 1;
    if (y0 > y1) {
        int32_t t;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
        dir = -1;
    }
    int64_t dx = (int64_t)x1 - x0, dy = (int64_t)y1 - y0;
    int32_t ey0 = y0 >> 8, ey1 = (y1 - 1) >> 8;
    int32_t xa = x0, ya = y0;
    for (int32_t ey = ey0; ey <= ey1; ey++) {
        int32_t yb = (ey + 1) * 256;
        if (yb > y1) yb = y1;
        int32_t xb = (yb == y1) ? x1 : x0 + (int32_t)((dx * (yb - y0)) / dy);
        row_piece(ey, xa, ya - ey * 256, xb, yb - ey * 256, dir);
        xa = xb; ya = yb;
    }
}

/* An edge anywhere, 24.8: cut to the clip box. Above and below the box it is
 * dropped -- it crosses no row there. Left of the box it becomes a vertical
 * edge on the box's side, which crosses the same rows the same way and so
 * leaves every pixel inside exactly as it was; right of the box the same. */
static int32_t x_at(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t y) {
    return x0 + (int32_t)((((int64_t)x1 - x0) * ((int64_t)y - y0)) / ((int64_t)y1 - y0));
}

static void clip_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (y0 == y1) return;
    if ((y0 <= box_y0 && y1 <= box_y0) || (y0 >= box_y1 && y1 >= box_y1)) return;
    /* Both ends moved along the original line, never along a moved one. */
    int32_t ox0 = x0, oy0 = y0, ox1 = x1, oy1 = y1;
    if (y0 < box_y0)      { x0 = x_at(ox0, oy0, ox1, oy1, box_y0); y0 = box_y0; }
    else if (y0 > box_y1) { x0 = x_at(ox0, oy0, ox1, oy1, box_y1); y0 = box_y1; }
    if (y1 < box_y0)      { x1 = x_at(ox0, oy0, ox1, oy1, box_y0); y1 = box_y0; }
    else if (y1 > box_y1) { x1 = x_at(ox0, oy0, ox1, oy1, box_y1); y1 = box_y1; }
    if (y0 == y1) return;

    /* Where it crosses the two sides, in order along it. */
    int32_t px[4], py[4];
    int n = 0;
    px[n] = x0; py[n] = y0; n++;
    int64_t ex = (int64_t)x1 - x0, ey = (int64_t)y1 - y0;
    int32_t cx[2], cy[2];
    int nc = 0;
    if ((x0 < box_x0) != (x1 < box_x0)) {
        cx[nc] = box_x0;
        cy[nc] = y0 + (int32_t)((ey * (box_x0 - x0)) / ex);
        nc++;
    }
    if ((x0 > box_x1) != (x1 > box_x1)) {
        cx[nc] = box_x1;
        cy[nc] = y0 + (int32_t)((ey * (box_x1 - x0)) / ex);
        nc++;
    }
    if (nc == 2) {
        /* the nearer to (x0,y0) first */
        int64_t d0 = (int64_t)cx[0] - x0, d1 = (int64_t)cx[1] - x0;
        if (d0 < 0) d0 = -d0;
        if (d1 < 0) d1 = -d1;
        if (d1 < d0) {
            int32_t t = cx[0]; cx[0] = cx[1]; cx[1] = t;
            t = cy[0]; cy[0] = cy[1]; cy[1] = t;
        }
    }
    for (int i = 0; i < nc; i++) { px[n] = cx[i]; py[n] = cy[i]; n++; }
    px[n] = x1; py[n] = y1; n++;

    for (int i = 0; i + 1 < n; i++) {
        int32_t ax = px[i], bx = px[i + 1];
        if (ax < box_x0) ax = box_x0;
        if (ax > box_x1) ax = box_x1;
        if (bx < box_x0) bx = box_x0;
        if (bx > box_x1) bx = box_x1;
        cells_line(ax, py[i], bx, py[i + 1]);
    }
}

static inline int32_t to24(int32_t v16) { return (v16 + 128) >> 8; }

/* Every subpath of P as a closed polygon, through m if given. */
static void cells_poly(const rpoly *P, const rast_mat *m) {
    for (int s = 0; s < P->ns; s++) {
        int n = rpoly_len(P, s), b = P->start[s];
        if (n < 2) continue;
        int32_t fx = 0, fy = 0, lx = 0, ly = 0;
        for (int i = 0; i <= n; i++) {
            int k = b + (i % n);
            int32_t x = P->xy[k * 2], y = P->xy[k * 2 + 1];
            if (m) rast_apply(m, x, y, &x, &y);
            x = to24(x); y = to24(y);
            if (i == 0) { fx = x; fy = y; }
            else clip_line(lx, ly, x, y);
            lx = x; ly = y;
        }
        (void)fx; (void)fy;
    }
    cell_flush();
}

/* --- paint ---------------------------------------------------------------------------- */

typedef struct {
    int      type;
    uint32_t solid;             /* premultiplied */
    uint32_t lut[256];          /* premultiplied, t = i/255 */
    int      spread;
    int64_t  t0, tx, ty;        /* LINEAR: t at pixel (x,y), 26 fraction bits */
    int64_t  u0x, u0y, uxx, uxy, uyx, uyy;   /* RADIAL: (p - focus)/r, 32.32 */
    rast_fx  vx, vy;            /* RADIAL: (focus - centre)/r */
    rast_fx  A;                 /*         1 - |v|^2 */
    int      mask;              /* writing coverage into a mask */
} painter;

static inline uint32_t div255(uint32_t v) {
    v += 128;
    return (v + (v >> 8)) >> 8;
}

uint32_t rast_premul(uint32_t c) {
    uint32_t a = c >> 24;
    if (a == 255) return c;
    return (a << 24) | (div255(((c >> 16) & 255) * a) << 16) |
           (div255(((c >> 8) & 255) * a) << 8) | div255((c & 255) * a);
}

uint32_t rast_unpremul(uint32_t c) {
    uint32_t a = c >> 24;
    if (a == 255 || a == 0) return a ? c : 0;
    uint32_t r = (((c >> 16) & 255) * 255 + a / 2) / a;
    uint32_t g = (((c >> 8) & 255) * 255 + a / 2) / a;
    uint32_t b = ((c & 255) * 255 + a / 2) / a;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static uint32_t with_opacity(uint32_t argb, int opacity) {
    if (opacity >= 255) return argb;
    if (opacity <= 0) return argb & 0xFFFFFF;
    return (div255((argb >> 24) * (uint32_t)opacity) << 24) | (argb & 0xFFFFFF);
}

/* The 256 colours t = 0..1 passes through, blended premultiplied so that a
 * fade to transparent does not darken on the way. */
static void build_lut(painter *P, const rast_paint *pt) {
    int ns = pt->nstops;
    const rast_stop *st = pt->stops;
    rast_fx last_off = 0;
    for (int i = 0; i < 256; i++) {
        rast_fx t = (rast_fx)(((int64_t)i * RAST_ONE) / 255);
        uint32_t c;
        if (t <= st[0].offset) {
            c = rast_premul(with_opacity(st[0].color, pt->opacity));
        } else if (t >= st[ns - 1].offset) {
            c = rast_premul(with_opacity(st[ns - 1].color, pt->opacity));
        } else {
            int k = 1;
            last_off = st[0].offset;
            while (k < ns - 1 && t > st[k].offset) k++;
            rast_fx o0 = st[k - 1].offset, o1 = st[k].offset;
            uint32_t a = rast_premul(with_opacity(st[k - 1].color, pt->opacity));
            uint32_t b = rast_premul(with_opacity(st[k].color, pt->opacity));
            uint32_t f = o1 > o0 ? (uint32_t)(((int64_t)(t - o0) * 256) / (o1 - o0)) : 256;
            if (f > 256) f = 256;
            c = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                uint32_t ca = (a >> sh) & 255, cb = (b >> sh) & 255;
                c |= ((ca * (256 - f) + cb * f + 128) >> 8) << sh;
            }
        }
        P->lut[i] = c;
    }
    (void)last_off;
}

/* (num << shift) / den without overflowing: the two are halved together
 * until the shifted numerator fits, which costs only bits nobody needs. */
static int64_t div_scaled(int64_t num, int64_t den, int shift) {
    int64_t lim = (int64_t)1 << (62 - shift);
    while (num >= lim || num <= -lim) { num /= 2; den /= 2; }
    if (!den) return num >= 0 ? ((int64_t)1 << 50) : -((int64_t)1 << 50);
    return (num * ((int64_t)1 << shift)) / den;
}

/* Everything a paint needs per pixel, worked out once. 0 if it paints
 * nothing at all. */
static int prep_paint(painter *P, const rast_paint *pt, const rast_mat *ctm) {
    memset(P, 0, sizeof *P);
    P->type = pt->type;
    P->spread = pt->spread;
    if (pt->opacity <= 0) return 0;

    if (pt->type == RAST_SOLID) {
        uint32_t c = with_opacity(pt->color, pt->opacity);
        if (!(c >> 24)) return 0;
        P->solid = rast_premul(c);
        return 1;
    }
    if (pt->nstops <= 0 || !pt->stops) return 0;

    /* Gradient space -> device, and back. */
    rast_mat I = rast_identity();
    rast_mat G = rast_mul(ctm ? *ctm : I, pt->m), Gi;
    int degenerate = !rast_invert(G, &Gi);

    if (pt->nstops == 1 || degenerate) {
        /* One stop, or a gradient squashed flat: its last colour, as SVG
         * says for the first and near enough for the second. */
        uint32_t c = with_opacity(pt->stops[pt->nstops - 1].color, pt->opacity);
        if (!(c >> 24)) return 0;
        P->type = RAST_SOLID;
        P->solid = rast_premul(c);
        return 1;
    }
    build_lut(P, pt);

    if (pt->type == RAST_LINEAR) {
        int64_t dx = (int64_t)pt->x2 - pt->x1, dy = (int64_t)pt->y2 - pt->y1;
        int64_t L2 = dx * dx + dy * dy;                          /* 32.32 */
        if (L2 <= 0) {
            P->type = RAST_SOLID;
            P->solid = P->lut[255];
            return 1;
        }
        /* t is affine in the pixel: coefficients straight from the inverse
         * matrix, to 26 bits of fraction. */
        int64_t nx = (int64_t)Gi.a * dx + (int64_t)Gi.b * dy;    /* per device x */
        int64_t ny = (int64_t)Gi.c * dx + (int64_t)Gi.d * dy;    /* per device y */
        int64_t n0 = ((int64_t)Gi.e - pt->x1) * dx + ((int64_t)Gi.f - pt->y1) * dy;
        P->tx = div_scaled(nx, L2, 26);
        P->ty = div_scaled(ny, L2, 26);
        P->t0 = div_scaled(n0, L2, 26) + P->tx / 2 + P->ty / 2;  /* pixel centres */
        return 1;
    }

    /* RADIAL: work in units of the radius, measured from the focus. */
    if (pt->r <= 0) {
        P->type = RAST_SOLID;
        P->solid = P->lut[255];
        return 1;
    }
    rast_fx vx = rast_divfx(pt->fx - pt->cx, pt->r), vy = rast_divfx(pt->fy - pt->cy, pt->r);
    rast_fx vl = rast_len(vx, vy);
    rast_fx maxv = RAST_ONE - RAST_ONE / 100;                    /* 0.99 */
    if (vl > maxv) {                  /* focus outside: onto the edge, as SVG 1.1 */
        vx = (rast_fx)(((int64_t)vx * maxv) / vl);
        vy = (rast_fx)(((int64_t)vy * maxv) / vl);
    }
    P->vx = vx; P->vy = vy;
    P->A = RAST_ONE - (rast_fx)((((int64_t)vx * vx) + ((int64_t)vy * vy)) >> 16);
    rast_fx fxp = pt->cx + rast_mulfx(vx, pt->r), fyp = pt->cy + rast_mulfx(vy, pt->r);
    P->uxx = div_scaled(Gi.a, pt->r, 32);
    P->uxy = div_scaled(Gi.b, pt->r, 32);
    P->uyx = div_scaled(Gi.c, pt->r, 32);
    P->uyy = div_scaled(Gi.d, pt->r, 32);
    P->u0x = div_scaled((int64_t)Gi.e - fxp, pt->r, 32) + P->uxx / 2 + P->uyx / 2;
    P->u0y = div_scaled((int64_t)Gi.f - fyp, pt->r, 32) + P->uxy / 2 + P->uyy / 2;
    return 1;
}

static inline int spread_index(int64_t t, int spread) {
    if (spread == RAST_REPEAT) {
        t &= 0xFFFF;
    } else if (spread == RAST_REFLECT) {
        t &= 0x1FFFF;
        if (t > 0x10000) t = 0x20000 - t;
    } else {
        if (t < 0) t = 0;
        if (t > 0x10000) t = 0x10000;
    }
    return (int)((t * 255 + 0x8000) >> 16);
}

static inline int64_t clamp_big(int64_t v) {
    const int64_t lim = (int64_t)1 << 27;
    return v > lim ? lim : v < -lim ? -lim : v;
}

static inline uint32_t grad_at(const painter *P, int x, int y) {
    if (P->type == RAST_LINEAR) {
        int64_t t = (P->t0 + P->tx * x + P->ty * y) >> 10;          /* 16.16 */
        return P->lut[spread_index(t, P->spread)];
    }
    int64_t ux = clamp_big((P->u0x + P->uxx * x + P->uyx * y) >> 16);   /* 16.16 */
    int64_t uy = clamp_big((P->u0y + P->uxy * x + P->uyy * y) >> 16);
    int64_t vu = ((int64_t)P->vx * ux + (int64_t)P->vy * uy) >> 16;   /* 16.16 */
    int64_t uu = ux * ux + uy * uy;                                   /* 32.32 */
    int64_t disc = vu * vu + (uu >> 16) * P->A;                       /* 32.32 */
    int64_t sq = (int64_t)rast_isqrt64((uint64_t)(disc < 0 ? 0 : disc));
    int64_t t = ((vu + sq) * RAST_ONE) / P->A;
    return P->lut[spread_index(t, P->spread)];
}

/* src (premultiplied) over *d at coverage c. */
static inline void blend(uint32_t *d, uint32_t s, uint32_t c, int format) {
    uint32_t sa = s >> 24;
    if (c == 255 && sa == 255) {
        *d = format == RAST_XRGB ? (*d & 0xFF000000u) | (s & 0xFFFFFF) : s;
        return;
    }
    if (c != 255) {
        sa = div255(sa * c);
        if (!sa) return;
        s = (sa << 24) | (div255(((s >> 16) & 255) * c) << 16) |
            (div255(((s >> 8) & 255) * c) << 8) | div255((s & 255) * c);
    }
    uint32_t inv = 255 - sa, v = *d;
    uint32_t r = ((s >> 16) & 255) + div255(((v >> 16) & 255) * inv);
    uint32_t g = ((s >> 8) & 255) + div255(((v >> 8) & 255) * inv);
    uint32_t b = (s & 255) + div255((v & 255) * inv);
    uint32_t a = format == RAST_XRGB ? (v >> 24) : sa + div255((v >> 24) * inv);
    *d = (a << 24) | (r << 16) | (g << 8) | b;
}

static void paint_row(const rast_target *t, const painter *P, int y, int x0, int x1,
                      const uint8_t *cov) {
    if (P->mask) {
        uint8_t *mrow = (uint8_t *)t->px + (size_t)y * t->stride;
        for (int x = x0; x < x1; x++)
            if (cov[x] > mrow[x]) mrow[x] = cov[x];
        return;
    }
    uint32_t *row = t->px + (size_t)y * t->stride;
    const uint8_t *m = t->mask ? t->mask + (size_t)y * t->mask_stride : NULL;
    for (int x = x0; x < x1; x++) {
        uint32_t c = cov[x];
        if (m) c = div255(c * m[x]);
        if (!c) continue;
        uint32_t s = P->type == RAST_SOLID ? P->solid : grad_at(P, x, y);
        if (s >> 24) blend(&row[x], s, c, t->format);
    }
}

/* --- the sweep ---------------------------------------------------------------------------- */

static uint8_t *covrow;
static int      covcap;
static rcell   *sorted;
static int      sortcap;
static int     *rowstart;
static int      rowcap;

/* A row's cells by x. Shell sort rather than qsort: the kernel has no C
 * library to take qsort from, and a row holds a few hundred cells at most,
 * already in nearly the right order -- edges are walked left to right as
 * often as not -- which is the case Shell sort is good at. */
static void sort_x(rcell *c, int n) {
    static const int gaps[] = { 701, 301, 132, 57, 23, 10, 4, 1 };
    for (int g = 0; g < (int)(sizeof gaps / sizeof gaps[0]); g++) {
        int gap = gaps[g];
        for (int i = gap; i < n; i++) {
            rcell v = c[i];
            int j = i;
            while (j >= gap && c[j - gap].x > v.x) { c[j] = c[j - gap]; j -= gap; }
            c[j] = v;
        }
    }
}

static inline int coverage(int32_t v, int rule) {
    if (v < 0) v = -v;
    v >>= 9;                                   /* 256 is one full layer */
    if (rule == RAST_EVENODD) {
        v &= 511;
        if (v > 256) v = 512 - v;
    } else if (v > 256) {
        v = 256;
    }
    return v - (v >> 8);
}

static int sweep(const rast_target *t, const painter *P, int rule) {
    if (cell_err) return 0;
    if (!ncells) return 1;

    if (covcap < t->w + 2) {
        uint8_t *n = (uint8_t *)realloc(covrow, (size_t)t->w + 2);
        if (!n) return 0;
        covrow = n;
        covcap = t->w + 2;
        memset(covrow, 0, (size_t)covcap);
    }
    if (sortcap < ncells) {
        rcell *n = (rcell *)realloc(sorted, (size_t)ncells * sizeof *n);
        if (!n) return 0;
        sorted = n;
        sortcap = ncells;
    }
    int rows = row_hi - row_lo + 1;
    if (rowcap < rows + 1) {
        int *n = (int *)realloc(rowstart, (size_t)(rows + 1) * sizeof *n);
        if (!n) return 0;
        rowstart = n;
        rowcap = rows + 1;
    }

    /* By row, counting; then by x within each row. */
    memset(rowstart, 0, (size_t)(rows + 1) * sizeof *rowstart);
    for (int i = 0; i < ncells; i++) rowstart[cells[i].y - row_lo + 1]++;
    for (int r = 0; r < rows; r++) rowstart[r + 1] += rowstart[r];
    for (int i = 0; i < ncells; i++) sorted[rowstart[cells[i].y - row_lo]++] = cells[i];
    for (int r = rows; r > 0; r--) rowstart[r] = rowstart[r - 1];
    rowstart[0] = 0;

    for (int r = 0; r < rows; r++) {
        int s = rowstart[r], e = rowstart[r + 1];
        if (s == e) continue;
        int y = row_lo + r;
        if (y < t->cy0 || y >= t->cy1) continue;
        rcell *c = sorted;
        if (e - s <= 24) {
            for (int i = s + 1; i < e; i++) {
                rcell v = c[i];
                int j = i - 1;
                while (j >= s && c[j].x > v.x) { c[j + 1] = c[j]; j--; }
                c[j + 1] = v;
            }
        } else {
            sort_x(c + s, e - s);
        }

        int32_t cover = 0;
        int lo = INT_MAX, hi = INT_MIN;
        int i = s;
        while (i < e) {
            int32_t x = c[i].x, area = 0;
            while (i < e && c[i].x == x) { cover += c[i].cover; area += c[i].area; i++; }
            if (area) {
                int a = coverage(cover * 512 - area, rule);
                if (a && x >= t->cx0 && x < t->cx1) {
                    covrow[x] = (uint8_t)a;
                    if (x < lo) lo = x;
                    if (x + 1 > hi) hi = x + 1;
                }
                x++;
            }
            if (i < e && c[i].x > x) {
                int a = coverage(cover * 512, rule);
                int x0 = x < t->cx0 ? t->cx0 : x;
                int x1 = c[i].x > t->cx1 ? t->cx1 : c[i].x;
                if (a && x0 < x1) {
                    memset(covrow + x0, a, (size_t)(x1 - x0));
                    if (x0 < lo) lo = x0;
                    if (x1 > hi) hi = x1;
                }
            }
        }
        if (lo < hi) {
            paint_row(t, P, y, lo, hi, covrow);
            memset(covrow + lo, 0, (size_t)(hi - lo));
        }
    }
    return 1;
}

/* --- the entry points --------------------------------------------------------------------- */

void rast_target_init(rast_target *t, uint32_t *px, int w, int h, int stride, int format) {
    t->px = px;
    t->w = w;
    t->h = h;
    t->stride = stride;
    t->format = format;
    t->cx0 = 0; t->cy0 = 0; t->cx1 = w; t->cy1 = h;
    t->mask = NULL;
    t->mask_stride = 0;
}

void rast_paint_solid(rast_paint *pt, uint32_t argb) {
    memset(pt, 0, sizeof *pt);
    pt->type = RAST_SOLID;
    pt->color = argb;
    pt->m = rast_identity();
    pt->opacity = 255;
}

static void clip_to_target(rast_target *t) {
    if (t->cx0 < 0) t->cx0 = 0;
    if (t->cy0 < 0) t->cy0 = 0;
    if (t->cx1 > t->w) t->cx1 = t->w;
    if (t->cy1 > t->h) t->cy1 = t->h;
}

static rpoly flat, dashed, outline;

int rast_fill(rast_target *t, const rast_path *p, const rast_mat *m, int rule,
              const rast_paint *paint) {
    if (!p || !p->nops || !paint) return 1;
    rast_target tt = *t;
    clip_to_target(&tt);
    if (tt.cx0 >= tt.cx1 || tt.cy0 >= tt.cy1) return 1;
    painter P;
    if (!prep_paint(&P, paint, m)) return 1;
    rpoly_reset(&flat);
    rast_flatten(p, m, RAST_TOL, &flat);
    cells_begin(&tt);
    cells_poly(&flat, NULL);
    int ok = sweep(&tt, &P, rule);
    return ok && !flat.err && !p->err;
}

int rast_draw_stroke(rast_target *t, const rast_path *p, const rast_mat *m,
                     const rast_stroke *s, const rast_paint *paint) {
    if (!p || !p->nops || !paint || !s || s->width <= 0) return 1;
    rast_target tt = *t;
    clip_to_target(&tt);
    if (tt.cx0 >= tt.cx1 || tt.cy0 >= tt.cy1) return 1;
    painter P;
    if (!prep_paint(&P, paint, m)) return 1;

    rast_mat I = rast_identity();
    const rast_mat *M = m ? m : &I;
    rast_fx scale = rast_mat_scale(M);
    if (scale <= 0) return 1;
    /* The pen is built in the path's own space, so the tolerance is the
     * device one brought back through the transform. */
    rast_fx tol = rast_divfx(RAST_TOL, scale);
    if (tol < 1) tol = 1;

    rpoly_reset(&flat);
    rast_flatten(p, NULL, tol, &flat);
    const rpoly *src = &flat;
    if (s->dash && s->ndash > 0) {
        rpoly_reset(&dashed);
        rast_dash(&flat, s, &dashed);
        src = &dashed;
    }
    rpoly_reset(&outline);
    /* Round joins and caps at half that: they are nothing but curve, and
     * a dot drawn with them is all edge. */
    rast_stroke_outline(src, s, tol / 2 > 0 ? tol / 2 : 1, &outline);
    cells_begin(&tt);
    cells_poly(&outline, M);
    int ok = sweep(&tt, &P, RAST_NONZERO);
    return ok && !flat.err && !dashed.err && !outline.err && !p->err;
}

int rast_fill_mask(uint8_t *mask, int w, int h, int stride, const rast_path *p,
                   const rast_mat *m, int rule) {
    if (!p || !p->nops) return 1;
    rast_target tt;
    rast_target_init(&tt, (uint32_t *)mask, w, h, stride, RAST_XRGB);
    painter P;
    memset(&P, 0, sizeof P);
    P.mask = 1;
    rpoly_reset(&flat);
    rast_flatten(p, m, RAST_TOL, &flat);
    cells_begin(&tt);
    cells_poly(&flat, NULL);
    int ok = sweep(&tt, &P, rule);
    return ok && !flat.err && !p->err;
}
