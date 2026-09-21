#ifndef SVG_H
#define SVG_H

/* SVG rendering for xyuOS Neo.
 *
 * The other formats in img.h hand over pixels. This one hands over a drawing,
 * so the work is different in kind: parse the markup, turn every shape into
 * straight-line segments, and fill those with a scanline rasteriser. A logo
 * arrives as a few hundred cubic curves and leaves as a bitmap.
 *
 * WHAT IS HERE: path, rect, circle, ellipse, line, polyline, polygon, and <g>
 * nesting with transforms; the full path grammar including elliptical arcs;
 * fill and stroke with either winding rule; colours by name, hex or rgb();
 * opacity; viewBox scaling.
 *
 * WHAT IS NOT: text (it needs a font, and the one we have is a terminal face
 * that would misrepresent the drawing rather than approximate it), gradients,
 * patterns, filters, clipping and masking. A shape that depends on any of
 * those is drawn without it rather than dropped -- half a logo beats a hole.
 *
 * ALL INTEGER. web.c is built with -mgeneral-regs-only, so there is no
 * floating point to be had: coordinates are 16.16 fixed point throughout, and
 * the two transcendentals an arc needs -- sine and square root -- are a table
 * and a Newton iteration at the top of the file. Fixed point is the right
 * answer here anyway; a rasteriser wants exact, repeatable arithmetic on a
 * grid far more than it wants range.
 *
 * Anti-aliasing is coverage-based: four sub-scanlines per row, exact
 * horizontal coverage at the ends of every span. Curves flatten to a
 * tolerance of a third of a pixel, which is below what the coverage can
 * show.
 */

#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * fixed point: 16.16 throughout
 * ========================================================================== */

typedef int svg_fx;
#define FX_ONE   65536
#define FX_HALF  32768

static svg_fx fx_mul(svg_fx a, svg_fx b) {
    return (svg_fx)(((long long)a * (long long)b) >> 16);
}

static svg_fx fx_div(svg_fx a, svg_fx b) {
    if (!b) return 0;
    return (svg_fx)(((long long)a << 16) / b);
}

/* Square root of a 16.16 value, by Newton from a bit-length estimate. Six
 * iterations is comfortably enough for the range we use it over. */
static svg_fx fx_sqrt(svg_fx v) {
    if (v <= 0) return 0;
    /* Start at 2^(bits/2), which is within a factor of two of the answer. */
    int bits = 0;
    for (svg_fx t = v; t; t >>= 1) bits++;
    svg_fx x = (svg_fx)1 << ((bits + 16) >> 1);
    if (x <= 0) x = FX_ONE;
    for (int i = 0; i < 8; i++) {
        svg_fx q = fx_div(v, x);
        x = (x + q) >> 1;
        if (x <= 0) { x = 1; break; }
    }
    return x;
}

/* Sine over a quarter turn, at one-degree steps, as 16.16. Everything else is
 * reflection. A degree is finer than the curve flattening below can show. */
static const unsigned int svg_sin_tab[91] = {
        0,  1144,  2287,  3430,  4572,  5712,  6850,  7987,  9121, 10252,
    11380, 12505, 13626, 14742, 15855, 16962, 18064, 19161, 20252, 21336,
    22415, 23486, 24550, 25607, 26656, 27697, 28729, 29753, 30767, 31772,
    32768, 33754, 34729, 35693, 36647, 37590, 38521, 39441, 40348, 41243,
    42126, 42995, 43852, 44695, 45525, 46341, 47143, 47930, 48703, 49461,
    50203, 50931, 51643, 52339, 53020, 53684, 54332, 54963, 55578, 56175,
    56756, 57319, 57865, 58393, 58903, 59396, 59870, 60326, 60764, 61183,
    61584, 61966, 62328, 62672, 62997, 63303, 63589, 63856, 64104, 64332,
    64540, 64729, 64898, 65048, 65177, 65287, 65376, 65446, 65496, 65526,
    65536
};

/* Angle in degrees, 16.16. */
static svg_fx fx_sin(svg_fx deg) {
    int neg = 0;
    long long d = deg;
    long long full = 360LL * FX_ONE;
    d %= full;
    if (d < 0) d += full;
    if (d >= 180LL * FX_ONE) { d -= 180LL * FX_ONE; neg = 1; }
    if (d > 90LL * FX_ONE) d = 180LL * FX_ONE - d;
    int i = (int)(d >> 16);
    if (i > 89) i = 89;
    int frac = (int)(d & 0xFFFF);
    int lo = svg_sin_tab[i], hi = svg_sin_tab[i + 1];
    svg_fx v = lo + (svg_fx)(((long long)(hi - lo) * frac) >> 16);
    return neg ? -v : v;
}

static svg_fx fx_cos(svg_fx deg) { return fx_sin(deg + 90 * FX_ONE); }

/* Angle of the vector (x,y) in degrees, 16.16. A bisection over the tangent
 * table: eleven steps get us inside a thousandth of a degree, which is far
 * below what any of this is drawn at. */
static svg_fx fx_atan2(svg_fx y, svg_fx x) {
    if (!x && !y) return 0;
    svg_fx lo = -180 * FX_ONE, hi = 180 * FX_ONE;
    /* sin(t)*x - cos(t)*y changes sign exactly at the answer, over the half
     * turn that contains it; pick that half first. */
    if (y >= 0) { lo = 0; hi = 180 * FX_ONE; }
    else        { lo = -180 * FX_ONE; hi = 0; }
    for (int i = 0; i < 24; i++) {
        svg_fx mid = lo + ((hi - lo) >> 1);
        /* cross product of (cos,sin) with (x,y) */
        long long cr = (long long)fx_cos(mid) * y - (long long)fx_sin(mid) * x;
        if ((y >= 0) ? (cr > 0) : (cr < 0)) lo = mid; else hi = mid;
    }
    return lo + ((hi - lo) >> 1);
}

/* ==========================================================================
 * transforms
 * ========================================================================== */

/* x' = a*x + c*y + e ;  y' = b*x + d*y + f  -- the same six numbers, in the
 * same order, as the matrix() form in the markup. */
typedef struct { svg_fx a, b, c, d, e, f; } svg_mat;

static svg_mat svg_ident(void) {
    svg_mat m = { FX_ONE, 0, 0, FX_ONE, 0, 0 };
    return m;
}

/* n applied first, then m. */
static svg_mat svg_mul(svg_mat m, svg_mat n) {
    svg_mat r;
    r.a = fx_mul(m.a, n.a) + fx_mul(m.c, n.b);
    r.b = fx_mul(m.b, n.a) + fx_mul(m.d, n.b);
    r.c = fx_mul(m.a, n.c) + fx_mul(m.c, n.d);
    r.d = fx_mul(m.b, n.c) + fx_mul(m.d, n.d);
    r.e = fx_mul(m.a, n.e) + fx_mul(m.c, n.f) + m.e;
    r.f = fx_mul(m.b, n.e) + fx_mul(m.d, n.f) + m.f;
    return r;
}

static void svg_apply(svg_mat m, svg_fx x, svg_fx y, svg_fx *ox, svg_fx *oy) {
    *ox = fx_mul(m.a, x) + fx_mul(m.c, y) + m.e;
    *oy = fx_mul(m.b, x) + fx_mul(m.d, y) + m.f;
}

/* How much the matrix scales lengths, for stroke widths. The geometric mean of
 * the two axis scales, which is what the specification asks for. */
static svg_fx svg_mat_scale(svg_mat m) {
    svg_fx sx = fx_sqrt(fx_mul(m.a, m.a) + fx_mul(m.b, m.b));
    svg_fx sy = fx_sqrt(fx_mul(m.c, m.c) + fx_mul(m.d, m.d));
    return fx_sqrt(fx_mul(sx, sy));
}

/* ==========================================================================
 * the edge list and the rasteriser
 * ========================================================================== */

typedef struct { svg_fx x0, y0, x1, y1; int dir; } svg_edge;

typedef struct {
    svg_edge *e;
    int       n, cap;
    svg_fx    minx, miny, maxx, maxy;
} svg_edges;

static void sedge_reset(svg_edges *E) {
    E->n = 0;
    E->minx = E->miny = 0x7FFFFFFF;
    E->maxx = E->maxy = -0x7FFFFFFF;
}

#define SVG_MAX_EDGES 400000

static int sedge_add(svg_edges *E, svg_fx x0, svg_fx y0, svg_fx x1, svg_fx y1) {
    if (y0 == y1) return 1;                    /* horizontal: no crossings */
    if (E->n == E->cap) {
        int cap = E->cap ? E->cap * 2 : 1024;
        if (cap > SVG_MAX_EDGES) cap = SVG_MAX_EDGES;
        if (cap == E->cap) return 0;
        svg_edge *ne = (svg_edge *)realloc(E->e, (size_t)cap * sizeof *ne);
        if (!ne) return 0;
        E->e = ne; E->cap = cap;
    }
    svg_edge *ed = &E->e[E->n++];
    if (y0 < y1) { ed->x0 = x0; ed->y0 = y0; ed->x1 = x1; ed->y1 = y1; ed->dir = 1; }
    else         { ed->x0 = x1; ed->y0 = y1; ed->x1 = x0; ed->y1 = y0; ed->dir = -1; }
    if (ed->x0 < E->minx) E->minx = ed->x0;
    if (ed->x1 < E->minx) E->minx = ed->x1;
    if (ed->x0 > E->maxx) E->maxx = ed->x0;
    if (ed->x1 > E->maxx) E->maxx = ed->x1;
    if (ed->y0 < E->miny) E->miny = ed->y0;
    if (ed->y1 > E->maxy) E->maxy = ed->y1;
    return 1;
}

/* Four sub-scanlines a row. Each contributes a quarter of full coverage, and
 * a full pixel therefore accumulates 4*64 = 256, one over the 255 we want --
 * clamped at the end rather than scaled, which keeps the arithmetic exact
 * everywhere except the very top. */
#define SVG_SUBS   4
#define SVG_WEIGHT (256 / SVG_SUBS)

static void svg_span(unsigned *acc, int w, svg_fx xa, svg_fx xb) {
    if (xb <= xa) return;
    if (xa < 0) xa = 0;
    svg_fx wide = (svg_fx)(w) << 16;
    if (xb > wide) xb = wide;
    if (xb <= xa) return;

    int ia = xa >> 16, ib = (xb - 1) >> 16;
    if (ia >= w) return;
    if (ib >= w) ib = w - 1;

    if (ia == ib) {
        acc[ia] += (unsigned)(((long long)(xb - xa) * SVG_WEIGHT) >> 16);
        return;
    }
    acc[ia] += (unsigned)(((long long)(((ia + 1) << 16) - xa) * SVG_WEIGHT) >> 16);
    for (int i = ia + 1; i < ib; i++) acc[i] += SVG_WEIGHT;
    acc[ib] += (unsigned)(((long long)(xb - ((svg_fx)ib << 16)) * SVG_WEIGHT) >> 16);
}

/* Sort the edge list by the row each edge starts on, so the active set can be
 * grown by walking forwards instead of rescanning everything every row. */
static int svg_edge_cmp(const void *A, const void *B) {
    const svg_edge *a = (const svg_edge *)A, *b = (const svg_edge *)B;
    return (a->y0 > b->y0) - (a->y0 < b->y0);
}

typedef struct { svg_fx x; int dir; } svg_cross;

/* Fill the accumulated edges into `px` with `color` (0xRRGGBB) at `alpha`. */
static void svg_fill(unsigned *px, int w, int h, svg_edges *E,
                     unsigned color, int alpha, int evenodd) {
    if (E->n <= 0 || alpha <= 0) return;

    qsort(E->e, (size_t)E->n, sizeof(svg_edge), svg_edge_cmp);

    int y0 = E->miny >> 16, y1 = (E->maxy >> 16) + 1;
    if (y0 < 0) y0 = 0;
    if (y1 > h) y1 = h;
    if (y0 >= y1) return;

    unsigned  *acc    = (unsigned *)calloc((size_t)w, sizeof(unsigned));
    int       *active = (int *)malloc((size_t)E->n * sizeof(int));
    svg_cross *cross  = (svg_cross *)malloc((size_t)E->n * sizeof(svg_cross));
    if (!acc || !active || !cross) { free(acc); free(active); free(cross); return; }

    int next = 0, nact = 0;
    /* Everything that starts above the first row we draw is already active. */
    while (next < E->n && (E->e[next].y0 >> 16) < y0) active[nact++] = next++;

    unsigned cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;

    for (int y = y0; y < y1; y++) {
        memset(acc, 0, (size_t)w * sizeof(unsigned));

        /* Admit the edges that begin on this row. */
        while (next < E->n && (E->e[next].y0 >> 16) <= y) active[nact++] = next++;

        for (int s = 0; s < SVG_SUBS; s++) {
            svg_fx sy = ((svg_fx)y << 16)
                      + (svg_fx)((s * FX_ONE + FX_ONE / 2) / SVG_SUBS);
            int nc = 0;
            for (int k = 0; k < nact; k++) {
                svg_edge *ed = &E->e[active[k]];
                if (sy < ed->y0 || sy >= ed->y1) continue;
                svg_fx dy = ed->y1 - ed->y0;
                svg_fx t  = fx_div(sy - ed->y0, dy);
                cross[nc].x   = ed->x0 + fx_mul(ed->x1 - ed->x0, t);
                cross[nc].dir = ed->dir;
                nc++;
            }
            if (nc < 2) continue;

            for (int i = 1; i < nc; i++) {          /* insertion sort by x */
                svg_cross v = cross[i];
                int j = i - 1;
                while (j >= 0 && cross[j].x > v.x) { cross[j + 1] = cross[j]; j--; }
                cross[j + 1] = v;
            }

            int wind = 0;
            for (int i = 0; i + 1 < nc; i++) {
                wind += evenodd ? 1 : cross[i].dir;
                int inside = evenodd ? (wind & 1) : (wind != 0);
                if (inside) svg_span(acc, w, cross[i].x, cross[i + 1].x);
            }
        }

        /* Drop the edges that ended above the next row. */
        int keep = 0;
        svg_fx rowend = ((svg_fx)(y + 1) << 16);
        for (int k = 0; k < nact; k++)
            if (E->e[active[k]].y1 > rowend) active[keep++] = active[k];
        nact = keep;

        unsigned *row = px + (long)y * w;
        for (int x = 0; x < w; x++) {
            unsigned c = acc[x];
            if (!c) continue;
            if (c > 255) c = 255;
            unsigned a = (c * (unsigned)alpha) / 255;
            if (!a) continue;
            unsigned d = row[x];
            unsigned dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            dr = (cr * a + dr * (255 - a)) / 255;
            dg = (cg * a + dg * (255 - a)) / 255;
            db = (cb * a + db * (255 - a)) / 255;
            row[x] = (dr << 16) | (dg << 8) | db;
        }
    }

    free(acc); free(active); free(cross);
}

/* ==========================================================================
 * building shapes: a path is a list of subpaths of points
 * ========================================================================== */

typedef struct {
    svg_fx *p;            /* x,y pairs in USER space */
    int     n, cap;       /* n = number of points */
    int    *starts;       /* index of the first point of each subpath */
    int     ns, scap;
    int    *closed;       /* whether each subpath was closed with Z */
} svg_path;

static void spath_init(svg_path *P) { memset(P, 0, sizeof *P); }

static void spath_free(svg_path *P) {
    free(P->p); free(P->starts); free(P->closed);
    memset(P, 0, sizeof *P);
}

static int spath_pt(svg_path *P, svg_fx x, svg_fx y) {
    if (P->n == P->cap) {
        int cap = P->cap ? P->cap * 2 : 256;
        svg_fx *np = (svg_fx *)realloc(P->p, (size_t)cap * 2 * sizeof *np);
        if (!np) return 0;
        P->p = np; P->cap = cap;
    }
    P->p[P->n * 2] = x;
    P->p[P->n * 2 + 1] = y;
    P->n++;
    return 1;
}

static int spath_move(svg_path *P, svg_fx x, svg_fx y) {
    if (P->ns == P->scap) {
        int cap = P->scap ? P->scap * 2 : 32;
        int *ns = (int *)realloc(P->starts, (size_t)cap * sizeof *ns);
        int *nc = (int *)realloc(P->closed, (size_t)cap * sizeof *nc);
        if (ns) P->starts = ns;
        if (nc) P->closed = nc;
        if (!ns || !nc) return 0;
        P->scap = cap;
    }
    P->starts[P->ns] = P->n;
    P->closed[P->ns] = 0;
    P->ns++;
    return spath_pt(P, x, y);
}

static int spath_len(const svg_path *P, int i) {
    int end = (i + 1 < P->ns) ? P->starts[i + 1] : P->n;
    return end - P->starts[i];
}

/* --- curve flattening ------------------------------------------------------
 * Subdivision count from the control polygon's length: a curve that spans
 * n pixels gets about n/3 segments, which puts the chord error below the
 * third of a pixel the coverage can resolve. */

static int svg_steps(svg_fx d) {
    int px = d >> 16;
    if (px < 0) px = -px;
    int n = px / 3 + 4;
    if (n > 120) n = 120;
    return n;
}

static svg_fx svg_dist(svg_fx x0, svg_fx y0, svg_fx x1, svg_fx y1) {
    svg_fx dx = x1 - x0, dy = y1 - y0;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    /* An octagonal approximation: within 4% of the true length, and this only
     * decides how many segments to spend. */
    return (dx > dy) ? dx + (dy >> 1) : dy + (dx >> 1);
}

static int spath_cubic(svg_path *P, svg_fx x0, svg_fx y0, svg_fx x1, svg_fx y1,
                       svg_fx x2, svg_fx y2, svg_fx x3, svg_fx y3) {
    int n = svg_steps(svg_dist(x0, y0, x1, y1) + svg_dist(x1, y1, x2, y2)
                    + svg_dist(x2, y2, x3, y3));
    for (int i = 1; i <= n; i++) {
        svg_fx t  = (svg_fx)(((long long)i << 16) / n);
        svg_fx it = FX_ONE - t;
        svg_fx a = fx_mul(fx_mul(it, it), it);
        svg_fx b = fx_mul(fx_mul(it, it), t) * 3;
        svg_fx c = fx_mul(fx_mul(it, t), t) * 3;
        svg_fx d = fx_mul(fx_mul(t, t), t);
        if (!spath_pt(P, fx_mul(a, x0) + fx_mul(b, x1) + fx_mul(c, x2) + fx_mul(d, x3),
                         fx_mul(a, y0) + fx_mul(b, y1) + fx_mul(c, y2) + fx_mul(d, y3)))
            return 0;
    }
    return 1;
}

static int spath_quad(svg_path *P, svg_fx x0, svg_fx y0, svg_fx cx, svg_fx cy,
                      svg_fx x1, svg_fx y1) {
    /* Raise to a cubic: the two control points sit two thirds of the way from
     * each end towards the quadratic's single one. */
    svg_fx c1x = x0 + (svg_fx)(((long long)(cx - x0) * 2) / 3);
    svg_fx c1y = y0 + (svg_fx)(((long long)(cy - y0) * 2) / 3);
    svg_fx c2x = x1 + (svg_fx)(((long long)(cx - x1) * 2) / 3);
    svg_fx c2y = y1 + (svg_fx)(((long long)(cy - y1) * 2) / 3);
    return spath_cubic(P, x0, y0, c1x, c1y, c2x, c2y, x1, y1);
}

/* An elliptical arc, in the endpoint form the markup uses. Converted to a
 * centre and a sweep the way the specification's appendix says, then walked. */
static int spath_arc(svg_path *P, svg_fx x0, svg_fx y0, svg_fx rx, svg_fx ry,
                     svg_fx rot, int large, int sweep, svg_fx x1, svg_fx y1) {
    if (rx < 0) rx = -rx;
    if (ry < 0) ry = -ry;
    if (!rx || !ry) return spath_pt(P, x1, y1);      /* degenerate: a line */

    svg_fx cosr = fx_cos(rot), sinr = fx_sin(rot);
    svg_fx dx2 = (x0 - x1) >> 1, dy2 = (y0 - y1) >> 1;
    svg_fx x1p =  fx_mul(cosr, dx2) + fx_mul(sinr, dy2);
    svg_fx y1p = -fx_mul(sinr, dx2) + fx_mul(cosr, dy2);

    /* Grow the radii if they cannot span the two points. */
    svg_fx lam = fx_div(fx_mul(x1p, x1p), fx_mul(rx, rx))
               + fx_div(fx_mul(y1p, y1p), fx_mul(ry, ry));
    if (lam > FX_ONE) {
        svg_fx s = fx_sqrt(lam);
        rx = fx_mul(rx, s);
        ry = fx_mul(ry, s);
    }

    svg_fx rx2 = fx_mul(rx, rx), ry2 = fx_mul(ry, ry);
    svg_fx num = rx2 * 0 + fx_mul(rx2, ry2)
               - fx_mul(rx2, fx_mul(y1p, y1p)) - fx_mul(ry2, fx_mul(x1p, x1p));
    svg_fx den = fx_mul(rx2, fx_mul(y1p, y1p)) + fx_mul(ry2, fx_mul(x1p, x1p));
    if (num < 0) num = 0;
    svg_fx co = den ? fx_sqrt(fx_div(num, den)) : 0;
    if (large == sweep) co = -co;

    svg_fx cxp =  fx_mul(co, fx_div(fx_mul(rx, y1p), ry));
    svg_fx cyp = -fx_mul(co, fx_div(fx_mul(ry, x1p), rx));

    svg_fx cx = fx_mul(cosr, cxp) - fx_mul(sinr, cyp) + ((x0 + x1) >> 1);
    svg_fx cy = fx_mul(sinr, cxp) + fx_mul(cosr, cyp) + ((y0 + y1) >> 1);

    svg_fx a0 = fx_atan2(fx_div(y1p - cyp, ry), fx_div(x1p - cxp, rx));
    svg_fx a1 = fx_atan2(fx_div(-y1p - cyp, ry), fx_div(-x1p - cxp, rx));
    svg_fx sweepdeg = a1 - a0;
    if (!sweep && sweepdeg > 0) sweepdeg -= 360 * FX_ONE;
    if (sweep && sweepdeg < 0)  sweepdeg += 360 * FX_ONE;

    svg_fx big = rx > ry ? rx : ry;
    int steps = svg_steps(fx_mul(big, sweepdeg < 0 ? -sweepdeg : sweepdeg) / 57);
    for (int i = 1; i <= steps; i++) {
        svg_fx t = a0 + (svg_fx)(((long long)sweepdeg * i) / steps);
        svg_fx ct = fx_cos(t), st = fx_sin(t);
        svg_fx ex = fx_mul(rx, ct), ey = fx_mul(ry, st);
        if (!spath_pt(P, cx + fx_mul(cosr, ex) - fx_mul(sinr, ey),
                         cy + fx_mul(sinr, ex) + fx_mul(cosr, ey)))
            return 0;
    }
    return 1;
}

/* ==========================================================================
 * numbers, colours and the rest of the small parsing
 * ========================================================================== */

static int svg_isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}
static int svg_isdigit(int c) { return c >= '0' && c <= '9'; }

static void svg_skip_sep(const char **pp) {
    const char *p = *pp;
    while (*p && (svg_isspace(*p) || *p == ',')) p++;
    *pp = p;
}

/* A number in the markup's grammar: sign, digits, fraction, exponent. */
static svg_fx svg_number(const char **pp) {
    const char *p = *pp;
    svg_skip_sep(&p);
    int neg = 0;
    if (*p == '+') p++;
    else if (*p == '-') { neg = 1; p++; }

    long long ip = 0;
    while (svg_isdigit(*p)) { if (ip < 100000) ip = ip * 10 + (*p - '0'); p++; }

    long long frac = 0, fdiv = 1;
    if (*p == '.') {
        p++;
        while (svg_isdigit(*p)) {
            if (fdiv < 100000000LL) { frac = frac * 10 + (*p - '0'); fdiv *= 10; }
            p++;
        }
    }

    long long v = (ip << 16) + (fdiv > 1 ? ((frac << 16) / fdiv) : 0);

    if (*p == 'e' || *p == 'E') {
        const char *save = p;
        p++;
        int eneg = 0;
        if (*p == '+') p++;
        else if (*p == '-') { eneg = 1; p++; }
        if (svg_isdigit(*p)) {
            int ex = 0;
            while (svg_isdigit(*p)) { if (ex < 40) ex = ex * 10 + (*p - '0'); p++; }
            for (int i = 0; i < ex; i++) {
                if (eneg) v /= 10;
                else if (v < (1LL << 46)) v *= 10;
            }
        } else {
            p = save;                     /* an 'e' that began something else */
        }
    }

    *pp = p;
    return (svg_fx)(neg ? -v : v);
}

/* A flag in an arc command is a single character, with no separator required
 * after it -- "a1 1 0 011 1" is legal and means large=0 sweep=1. */
static int svg_flag(const char **pp) {
    svg_skip_sep(pp);
    int v = (**pp == '1');
    if (**pp == '0' || **pp == '1') (*pp)++;
    return v;
}

static int svg_ci_eq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        int x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

/* 0xAARRGGBB, with A==0 meaning "do not paint". The named colours are the
 * handful that turn up in real drawings; anything unrecognised is black,
 * which is what the specification says an invalid paint falls back to. */
static unsigned svg_color(const char *s, unsigned inherit) {
    while (svg_isspace(*s)) s++;
    if (!*s) return inherit;

    if (svg_ci_eq(s, "none", 4)) return 0;
    if (svg_ci_eq(s, "transparent", 11)) return 0;
    if (svg_ci_eq(s, "currentcolor", 12)) return inherit;
    /* A gradient or pattern reference: we cannot paint it, but something is
     * better placed there than nothing, so use a mid grey. */
    if (svg_ci_eq(s, "url(", 4)) return 0xFF808080u;

    if (*s == '#') {
        s++;
        unsigned v = 0; int n = 0;
        while (n < 8) {
            int c = s[n], d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = (v << 4) | (unsigned)d;
            n++;
        }
        if (n == 3) return 0xFF000000u | ((v & 0xF00) * 0x1100) |
                            ((v & 0xF0) * 0x110) | ((v & 0xF) * 0x11);
        if (n == 6) return 0xFF000000u | v;
        if (n == 8) return ((v & 0xFF) << 24) | (v >> 8);   /* #rrggbbaa */
        return 0xFF000000u;
    }

    if (svg_ci_eq(s, "rgb", 3)) {
        const char *p = s + 3;
        while (*p && *p != '(') p++;
        if (*p == '(') {
            p++;
            svg_fx c[4] = { 0, 0, 0, FX_ONE };
            for (int i = 0; i < 4 && *p && *p != ')'; i++) {
                c[i] = svg_number(&p);
                svg_skip_sep(&p);
                if (*p == '%') { p++; c[i] = fx_mul(c[i], 255 * FX_ONE / 100); }
                svg_skip_sep(&p);
                if (*p == '/') { p++; }
            }
            unsigned a = (unsigned)((c[3] > FX_ONE ? c[3] >> 16 : (c[3] * 255) >> 16));
            if (a > 255) a = 255;
            unsigned r = (unsigned)(c[0] >> 16), g = (unsigned)(c[1] >> 16),
                     b = (unsigned)(c[2] >> 16);
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            return (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    static const struct { const char *n; int len; unsigned v; } named[] = {
        { "black",   5, 0x000000 }, { "white",   5, 0xFFFFFF },
        { "red",     3, 0xFF0000 }, { "green",   5, 0x008000 },
        { "blue",    4, 0x0000FF }, { "yellow",  6, 0xFFFF00 },
        { "gray",    4, 0x808080 }, { "grey",    4, 0x808080 },
        { "silver",  6, 0xC0C0C0 }, { "maroon",  6, 0x800000 },
        { "olive",   5, 0x808000 }, { "lime",    4, 0x00FF00 },
        { "aqua",    4, 0x00FFFF }, { "cyan",    4, 0x00FFFF },
        { "teal",    4, 0x008080 }, { "navy",    4, 0x000080 },
        { "fuchsia", 7, 0xFF00FF }, { "magenta", 7, 0xFF00FF },
        { "purple",  6, 0x800080 }, { "orange",  6, 0xFFA500 },
        { "pink",    4, 0xFFC0CB }, { "brown",   5, 0xA52A2A },
        { "gold",    4, 0xFFD700 }, { "darkgray",8, 0xA9A9A9 },
        { "lightgray",9,0xD3D3D3 },
    };
    for (unsigned i = 0; i < sizeof named / sizeof named[0]; i++)
        if (svg_ci_eq(s, named[i].n, named[i].len)) return 0xFF000000u | named[i].v;

    return 0xFF000000u;
}

/* ==========================================================================
 * attributes
 * ========================================================================== */

#define SVG_ATTR_MAX 24

typedef struct {
    const char *name; int nlen;
    const char *val;  int vlen;
} svg_attr;

/* Copy an attribute value out as a NUL-terminated string. */
static void svg_attr_str(const svg_attr *a, char *out, int max) {
    int n = a ? a->vlen : 0;
    if (n > max - 1) n = max - 1;
    for (int i = 0; i < n; i++) out[i] = a->val[i];
    out[n] = 0;
}

static const svg_attr *svg_find(const svg_attr *at, int n, const char *name) {
    int len = (int)strlen(name);
    for (int i = 0; i < n; i++)
        if (at[i].nlen == len && svg_ci_eq(at[i].name, name, len)) return &at[i];
    return 0;
}

/* A length, resolved against a reference for percentages. */
static svg_fx svg_len(const svg_attr *a, svg_fx ref, svg_fx dflt) {
    if (!a) return dflt;
    char buf[64];
    svg_attr_str(a, buf, sizeof buf);
    const char *p = buf;
    svg_fx v = svg_number(&p);
    while (svg_isspace(*p)) p++;
    if (*p == '%') return fx_mul(v, ref) / 100;
    /* px is the only unit the drawings we meet actually carry; pt and friends
     * are close enough at these sizes that guessing would be worse than the
     * error from ignoring them. */
    return v;
}

/* transform="translate(..) scale(..) rotate(..) matrix(..) skewX(..) skewY(..)" */
static svg_mat svg_transform(const char *s) {
    svg_mat m = svg_ident();
    while (*s) {
        while (*s && (svg_isspace(*s) || *s == ',')) s++;
        if (!*s) break;
        const char *name = s;
        while (*s && *s != '(' && !svg_isspace(*s)) s++;
        int nlen = (int)(s - name);
        while (svg_isspace(*s)) s++;
        if (*s != '(') break;
        s++;

        svg_fx v[6] = { 0, 0, 0, 0, 0, 0 };
        int nv = 0;
        while (*s && *s != ')' && nv < 6) {
            v[nv++] = svg_number(&s);
            svg_skip_sep(&s);
        }
        while (*s && *s != ')') s++;
        if (*s == ')') s++;

        svg_mat t = svg_ident();
        if (nlen == 9 && svg_ci_eq(name, "translate", 9)) {
            t.e = v[0]; t.f = nv > 1 ? v[1] : 0;
        } else if (nlen == 5 && svg_ci_eq(name, "scale", 5)) {
            t.a = v[0]; t.d = nv > 1 ? v[1] : v[0];
        } else if (nlen == 6 && svg_ci_eq(name, "rotate", 6)) {
            svg_fx c = fx_cos(v[0]), sn = fx_sin(v[0]);
            t.a = c; t.b = sn; t.c = -sn; t.d = c;
            if (nv >= 3) {           /* rotate about a point */
                svg_mat to = svg_ident(), back = svg_ident();
                to.e = v[1]; to.f = v[2];
                back.e = -v[1]; back.f = -v[2];
                t = svg_mul(to, svg_mul(t, back));
            }
        } else if (nlen == 6 && svg_ci_eq(name, "matrix", 6)) {
            t.a = v[0]; t.b = v[1]; t.c = v[2]; t.d = v[3]; t.e = v[4]; t.f = v[5];
        } else if (nlen == 5 && svg_ci_eq(name, "skewX", 5)) {
            t.c = fx_div(fx_sin(v[0]), fx_cos(v[0]));
        } else if (nlen == 5 && svg_ci_eq(name, "skewY", 5)) {
            t.b = fx_div(fx_sin(v[0]), fx_cos(v[0]));
        } else {
            continue;
        }
        m = svg_mul(m, t);
    }
    return m;
}

/* ==========================================================================
 * the drawing state, inherited down the tree
 * ========================================================================== */

typedef struct {
    svg_mat  m;
    unsigned fill, stroke;      /* 0xAARRGGBB; A == 0 means not painted */
    svg_fx   swidth;
    int      evenodd;
    int      opacity;           /* 0..255, multiplied down the tree */
} svg_state;

#define SVG_DEPTH 32

typedef struct {
    unsigned  *px;
    int        w, h;
    svg_state  st[SVG_DEPTH];
    int        depth;
    svg_edges  edges;
    int        skip_depth;      /* >0 while inside something we cannot draw */
} svg_ctx;

/* --- turning a path into edges -------------------------------------------- */

static void svg_emit_fill(svg_ctx *C, svg_path *P, const svg_state *st) {
    if (!(st->fill >> 24)) return;
    sedge_reset(&C->edges);
    for (int s = 0; s < P->ns; s++) {
        int n = spath_len(P, s), base = P->starts[s];
        if (n < 2) continue;
        svg_fx px0 = 0, py0 = 0, fx0 = 0, fy0 = 0;
        for (int i = 0; i < n; i++) {
            svg_fx x, y;
            svg_apply(st->m, P->p[(base + i) * 2], P->p[(base + i) * 2 + 1], &x, &y);
            if (i == 0) { fx0 = x; fy0 = y; }
            else sedge_add(&C->edges, px0, py0, x, y);
            px0 = x; py0 = y;
        }
        sedge_add(&C->edges, px0, py0, fx0, fy0);   /* fills are always closed */
    }
    int alpha = (int)((st->fill >> 24) * (unsigned)st->opacity / 255);
    svg_fill(C->px, C->w, C->h, &C->edges, st->fill & 0xFFFFFF, alpha, st->evenodd);
}

/* A stroke is drawn as the union of one quad per segment and one square per
 * joint, all wound the same way so the nonzero rule merges them. It is not a
 * true offset curve -- the joins are square rather than mitred -- but at the
 * widths a drawing actually uses the difference is under a pixel. */
/* A closed polygon, wound consistently.
 *
 * Every piece of a stroke must wind the same way. Under the nonzero rule two
 * overlapping shapes wound oppositely cancel where they meet, and a stroke is
 * nothing but overlapping pieces -- one wound backwards turns a solid line
 * into a stippled one. Rather than ask each caller to get the order right, the
 * orientation is measured here (the shoelace sum is twice the signed area) and
 * corrected. */
static void svg_poly(svg_edges *E, const svg_fx *p, int n) {
    long long a = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        a += (long long)p[i * 2] * p[j * 2 + 1] - (long long)p[j * 2] * p[i * 2 + 1];
    }
    if (a > 0)
        for (int i = n; i > 0; i--) {
            int j = i % n;
            sedge_add(E, p[j * 2], p[j * 2 + 1], p[(i - 1) * 2], p[(i - 1) * 2 + 1]);
        }
    else
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            sedge_add(E, p[i * 2], p[i * 2 + 1], p[j * 2], p[j * 2 + 1]);
        }
}

static void svg_quad(svg_edges *E, svg_fx x0, svg_fx y0, svg_fx x1, svg_fx y1,
                     svg_fx x2, svg_fx y2, svg_fx x3, svg_fx y3) {
    svg_fx p[8] = { x0, y0, x1, y1, x2, y2, x3, y3 };
    svg_poly(E, p, 4);
}

/* A round join, near enough: a regular octagon that just contains the circle
 * of radius h. A square there would stick out past the line wherever the line
 * is not axis-aligned, which is exactly what makes a stroke look like stairs.
 * 0.41421 is tan(22.5 degrees), the offset of the octagon's flat sides. */
#define SVG_OCT 27146          /* 0.41421 in 16.16 */

static void svg_joint(svg_edges *E, svg_fx cx, svg_fx cy, svg_fx h) {
    svg_fx d = fx_mul(h, SVG_OCT);
    svg_fx p[16] = {
        cx + h, cy - d,  cx + h, cy + d,
        cx + d, cy + h,  cx - d, cy + h,
        cx - h, cy + d,  cx - h, cy - d,
        cx - d, cy - h,  cx + d, cy - h,
    };
    svg_poly(E, p, 8);
}

static void svg_emit_stroke(svg_ctx *C, svg_path *P, const svg_state *st) {
    if (!(st->stroke >> 24)) return;
    svg_fx w = fx_mul(st->swidth, svg_mat_scale(st->m));
    if (w < FX_ONE) w = FX_ONE;              /* never thinner than a pixel */
    svg_fx h = w >> 1;

    sedge_reset(&C->edges);
    for (int s = 0; s < P->ns; s++) {
        int n = spath_len(P, s), base = P->starts[s];
        if (n < 1) continue;
        int segs = P->closed[s] ? n : n - 1;

        for (int i = 0; i < segs; i++) {
            int j = (i + 1) % n;
            svg_fx ax, ay, bx, by;
            svg_apply(st->m, P->p[(base + i) * 2], P->p[(base + i) * 2 + 1], &ax, &ay);
            svg_apply(st->m, P->p[(base + j) * 2], P->p[(base + j) * 2 + 1], &bx, &by);
            svg_fx dx = bx - ax, dy = by - ay;
            svg_fx len = fx_sqrt(fx_mul(dx, dx) + fx_mul(dy, dy));
            if (len < 64) continue;                        /* a duplicate point */
            svg_fx nx = fx_div(-dy, len), ny = fx_div(dx, len);
            nx = fx_mul(nx, h); ny = fx_mul(ny, h);
            svg_quad(&C->edges, ax + nx, ay + ny, bx + nx, by + ny,
                                bx - nx, by - ny, ax - nx, ay - ny);
        }
        /* A rounded cap at every vertex closes the gaps the quads leave
         * between one segment and the next. */
        for (int i = 0; i < n; i++) {
            svg_fx vx, vy;
            svg_apply(st->m, P->p[(base + i) * 2], P->p[(base + i) * 2 + 1], &vx, &vy);
            svg_joint(&C->edges, vx, vy, h);
        }
    }
    int alpha = (int)((st->stroke >> 24) * (unsigned)st->opacity / 255);
    /* Nonzero, always: the overlapping pieces are a union, not a xor. */
    svg_fill(C->px, C->w, C->h, &C->edges, st->stroke & 0xFFFFFF, alpha, 0);
}

static void svg_draw(svg_ctx *C, svg_path *P, const svg_state *st) {
    svg_emit_fill(C, P, st);
    svg_emit_stroke(C, P, st);
}

/* ==========================================================================
 * the path grammar
 * ========================================================================== */

static void svg_parse_path(svg_path *P, const char *d) {
    svg_fx cx = 0, cy = 0;         /* current point            */
    svg_fx sx = 0, sy = 0;         /* start of the subpath     */
    svg_fx rx = 0, ry = 0;         /* reflection of the last control point */
    int    have_ctrl = 0;
    char   cmd = 0, prev = 0;
    int    open = 0;

    const char *p = d;
    for (;;) {
        svg_skip_sep(&p);
        if (!*p) break;

        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            cmd = *p++;
        } else if (!cmd) {
            p++;                    /* junk before any command */
            continue;
        } else if (cmd == 'M') {
            cmd = 'L';              /* extra pairs after a moveto are linetos */
        } else if (cmd == 'm') {
            cmd = 'l';
        }

        int rel = (cmd >= 'a' && cmd <= 'z');
        char c = rel ? (char)(cmd - 32) : cmd;
        svg_fx ox = rel ? cx : 0, oy = rel ? cy : 0;

        switch (c) {
        case 'M': {
            svg_fx x = svg_number(&p) + ox, y = svg_number(&p) + oy;
            if (!spath_move(P, x, y)) return;
            cx = sx = x; cy = sy = y;
            open = 1; have_ctrl = 0;
            break;
        }
        case 'L': {
            svg_fx x = svg_number(&p) + ox, y = svg_number(&p) + oy;
            if (!open) { spath_move(P, cx, cy); open = 1; }
            spath_pt(P, x, y);
            cx = x; cy = y; have_ctrl = 0;
            break;
        }
        case 'H': {
            svg_fx x = svg_number(&p) + ox;
            if (!open) { spath_move(P, cx, cy); open = 1; }
            spath_pt(P, x, cy);
            cx = x; have_ctrl = 0;
            break;
        }
        case 'V': {
            svg_fx y = svg_number(&p) + oy;
            if (!open) { spath_move(P, cx, cy); open = 1; }
            spath_pt(P, cx, y);
            cy = y; have_ctrl = 0;
            break;
        }
        case 'C': {
            svg_fx x1 = svg_number(&p) + ox, y1 = svg_number(&p) + oy;
            svg_fx x2 = svg_number(&p) + ox, y2 = svg_number(&p) + oy;
            svg_fx x  = svg_number(&p) + ox, y  = svg_number(&p) + oy;
            if (!open) { spath_move(P, cx, cy); open = 1; }
            spath_cubic(P, cx, cy, x1, y1, x2, y2, x, y);
            rx = x2; ry = y2; have_ctrl = 1;
            cx = x; cy = y;
            break;
        }
        case 'S': {
            svg_fx x2 = svg_number(&p) + ox, y2 = svg_number(&p) + oy;
            svg_fx x  = svg_number(&p) + ox, y  = svg_number(&p) + oy;
            svg_fx x1 = have_ctrl ? 2 * cx - rx : cx;
            svg_fx y1 = have_ctrl ? 2 * cy - ry : cy;
            if (!open) { spath_move(P, cx, cy); open = 1; }
            spath_cubic(P, cx, cy, x1, y1, x2, y2, x, y);
            rx = x2; ry = y2; have_ctrl = 1;
            cx = x; cy = y;
            break;
        }
        case 'Q': {
            svg_fx x1 = svg_number(&p) + ox, y1 = svg_number(&p) + oy;
            svg_fx x  = svg_number(&p) + ox, y  = svg_number(&p) + oy;
            if (!open) { spath_move(P, cx, cy); open = 1; }
            spath_quad(P, cx, cy, x1, y1, x, y);
            rx = x1; ry = y1; have_ctrl = 2;
            cx = x; cy = y;
            break;
        }
        case 'T': {
            svg_fx x = svg_number(&p) + ox, y = svg_number(&p) + oy;
            svg_fx x1 = (have_ctrl == 2) ? 2 * cx - rx : cx;
            svg_fx y1 = (have_ctrl == 2) ? 2 * cy - ry : cy;
            if (!open) { spath_move(P, cx, cy); open = 1; }
            spath_quad(P, cx, cy, x1, y1, x, y);
            rx = x1; ry = y1; have_ctrl = 2;
            cx = x; cy = y;
            break;
        }
        case 'A': {
            svg_fx arx = svg_number(&p), ary = svg_number(&p);
            svg_fx rot = svg_number(&p);
            int large = svg_flag(&p), sweep = svg_flag(&p);
            svg_fx x = svg_number(&p) + ox, y = svg_number(&p) + oy;
            if (!open) { spath_move(P, cx, cy); open = 1; }
            spath_arc(P, cx, cy, arx, ary, rot, large, sweep, x, y);
            cx = x; cy = y; have_ctrl = 0;
            break;
        }
        case 'Z':
            if (open && P->ns > 0) P->closed[P->ns - 1] = 1;
            cx = sx; cy = sy;
            open = 0; have_ctrl = 0;
            break;
        default:
            /* An unknown letter: skip to the next one rather than spin. */
            while (*p && !((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) p++;
            cmd = 0;
            break;
        }
        prev = c;
        (void)prev;
    }
}

/* ==========================================================================
 * the markup
 * ========================================================================== */

/* Read the attributes of one tag. `p` points just past the tag name; on
 * return it points at the '>' (or at the '/' of a self-closing tag). */
static int svg_attrs(const char **pp, svg_attr *at, int max) {
    const char *p = *pp;
    int n = 0;
    for (;;) {
        while (svg_isspace(*p)) p++;
        if (!*p || *p == '>' || *p == '/') break;
        const char *name = p;
        while (*p && *p != '=' && *p != '>' && *p != '/' && !svg_isspace(*p)) p++;
        int nlen = (int)(p - name);
        while (svg_isspace(*p)) p++;
        if (*p != '=') { if (nlen == 0) p++; continue; }
        p++;
        while (svg_isspace(*p)) p++;
        char q = 0;
        if (*p == '"' || *p == '\'') { q = *p; p++; }
        const char *val = p;
        if (q) { while (*p && *p != q) p++; }
        else   { while (*p && !svg_isspace(*p) && *p != '>' && *p != '/') p++; }
        int vlen = (int)(p - val);
        if (q && *p) p++;
        if (n < max) {
            at[n].name = name; at[n].nlen = nlen;
            at[n].val  = val;  at[n].vlen = vlen;
            n++;
        }
    }
    *pp = p;
    return n;
}

/* Look inside style="fill:#fff;stroke:none" for one property. Returns 1 and
 * fills `out` when found. The style attribute wins over the presentation
 * attribute of the same name, which is what the cascade says. */
static int svg_style_get(const svg_attr *style, const char *prop,
                         char *out, int max) {
    if (!style) return 0;
    int plen = (int)strlen(prop);
    const char *s = style->val, *end = style->val + style->vlen;
    while (s < end) {
        while (s < end && (svg_isspace(*s) || *s == ';')) s++;
        const char *name = s;
        while (s < end && *s != ':' && *s != ';') s++;
        int nlen = (int)(s - name);
        while (nlen > 0 && svg_isspace(name[nlen - 1])) nlen--;
        if (s < end && *s == ':') {
            s++;
            while (s < end && svg_isspace(*s)) s++;
            const char *val = s;
            while (s < end && *s != ';') s++;
            int vlen = (int)(s - val);
            while (vlen > 0 && svg_isspace(val[vlen - 1])) vlen--;
            if (nlen == plen && svg_ci_eq(name, prop, plen)) {
                if (vlen > max - 1) vlen = max - 1;
                for (int i = 0; i < vlen; i++) out[i] = val[i];
                out[vlen] = 0;
                return 1;
            }
        }
    }
    return 0;
}

/* One property, from the style attribute if it is there and the presentation
 * attribute otherwise. */
static int svg_prop(const svg_attr *at, int nat, const char *name,
                    char *out, int max) {
    const svg_attr *style = svg_find(at, nat, "style");
    if (svg_style_get(style, name, out, max)) return 1;
    const svg_attr *a = svg_find(at, nat, name);
    if (!a) return 0;
    svg_attr_str(a, out, max);
    return 1;
}

static void svg_state_apply(svg_state *st, const svg_attr *at, int nat) {
    char buf[128];

    const svg_attr *tr = svg_find(at, nat, "transform");
    if (tr) {
        char t[512];
        svg_attr_str(tr, t, sizeof t);
        st->m = svg_mul(st->m, svg_transform(t));
    }

    if (svg_prop(at, nat, "fill", buf, sizeof buf))
        st->fill = svg_color(buf, st->fill);
    if (svg_prop(at, nat, "stroke", buf, sizeof buf))
        st->stroke = svg_color(buf, st->stroke);

    if (svg_prop(at, nat, "stroke-width", buf, sizeof buf)) {
        const char *p = buf;
        st->swidth = svg_number(&p);
    }
    if (svg_prop(at, nat, "fill-rule", buf, sizeof buf))
        st->evenodd = svg_ci_eq(buf, "evenodd", 7);

    /* The three opacities all end up multiplied into the paint. */
    if (svg_prop(at, nat, "opacity", buf, sizeof buf)) {
        const char *p = buf;
        svg_fx v = svg_number(&p);
        if (*p == '%') v = fx_mul(v, FX_ONE / 100);
        int o = (int)((v * 255) >> 16);
        if (o < 0) o = 0;
        if (o > 255) o = 255;
        st->opacity = st->opacity * o / 255;
    }
    if (svg_prop(at, nat, "fill-opacity", buf, sizeof buf)) {
        const char *p = buf;
        svg_fx v = svg_number(&p);
        int o = (int)((v * 255) >> 16);
        if (o < 0) o = 0;
        if (o > 255) o = 255;
        st->fill = (st->fill & 0xFFFFFF) | ((unsigned)((st->fill >> 24) * o / 255) << 24);
    }
    if (svg_prop(at, nat, "stroke-opacity", buf, sizeof buf)) {
        const char *p = buf;
        svg_fx v = svg_number(&p);
        int o = (int)((v * 255) >> 16);
        if (o < 0) o = 0;
        if (o > 255) o = 255;
        st->stroke = (st->stroke & 0xFFFFFF) | ((unsigned)((st->stroke >> 24) * o / 255) << 24);
    }
}

/* The four corner arcs of a rounded rectangle, as cubics. 0.5523 is the
 * constant that makes a cubic match a quarter circle to within a thousandth
 * of its radius. */
#define SVG_KAPPA 36204          /* 0.5523 in 16.16 */

static void svg_rounded_rect(svg_path *P, svg_fx x, svg_fx y, svg_fx w, svg_fx h,
                             svg_fx rx, svg_fx ry) {
    svg_fx kx = fx_mul(rx, SVG_KAPPA), ky = fx_mul(ry, SVG_KAPPA);
    spath_move(P, x + rx, y);
    spath_pt(P, x + w - rx, y);
    spath_cubic(P, x + w - rx, y, x + w - rx + kx, y, x + w, y + ry - ky, x + w, y + ry);
    spath_pt(P, x + w, y + h - ry);
    spath_cubic(P, x + w, y + h - ry, x + w, y + h - ry + ky,
                   x + w - rx + kx, y + h, x + w - rx, y + h);
    spath_pt(P, x + rx, y + h);
    spath_cubic(P, x + rx, y + h, x + rx - kx, y + h, x, y + h - ry + ky, x, y + h - ry);
    spath_pt(P, x, y + ry);
    spath_cubic(P, x, y + ry, x, y + ry - ky, x + rx - kx, y, x + rx, y);
    P->closed[P->ns - 1] = 1;
}

static void svg_ellipse_path(svg_path *P, svg_fx cx, svg_fx cy, svg_fx rx, svg_fx ry) {
    svg_fx kx = fx_mul(rx, SVG_KAPPA), ky = fx_mul(ry, SVG_KAPPA);
    spath_move(P, cx + rx, cy);
    spath_cubic(P, cx + rx, cy, cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
    spath_cubic(P, cx, cy + ry, cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
    spath_cubic(P, cx - rx, cy, cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
    spath_cubic(P, cx, cy - ry, cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
    P->closed[P->ns - 1] = 1;
}

static void svg_points_path(svg_path *P, const char *s, int close) {
    int first = 1;
    for (;;) {
        svg_skip_sep(&s);
        if (!*s) break;
        if (!svg_isdigit(*s) && *s != '-' && *s != '+' && *s != '.') break;
        svg_fx x = svg_number(&s), y = svg_number(&s);
        if (first) { spath_move(P, x, y); first = 0; }
        else spath_pt(P, x, y);
    }
    if (!first && close && P->ns) P->closed[P->ns - 1] = 1;
}

/* ==========================================================================
 * the front door
 * ========================================================================== */

/* Elements whose contents describe something other than what is drawn. Their
 * whole subtree is stepped over. */
static int svg_is_skipped(const char *n, int len) {
    static const char *skip[] = { "defs", "clipPath", "mask", "pattern",
                                  "linearGradient", "radialGradient", "filter",
                                  "style", "text", "title", "desc", "metadata",
                                  "symbol", "marker", "switch", "foreignObject" };
    for (unsigned i = 0; i < sizeof skip / sizeof skip[0]; i++) {
        int l = (int)strlen(skip[i]);
        if (l == len && svg_ci_eq(n, skip[i], l)) return 1;
    }
    return 0;
}

/* How big should this drawing be rendered? The width and height attributes if
 * it has them, the viewBox if not, and a default if neither -- then clamped so
 * a drawing that claims to be ten thousand pixels wide does not try to be. */
#define SVG_MIN_SIDE 1
#define SVG_MAX_SIDE 2048

static int svg_decode(const unsigned char *data, unsigned long len, image_t *out) {
    /* The parser wants a NUL to stop at, and the buffer it is handed is not
     * guaranteed to have one. */
    char *doc = (char *)malloc(len + 1);
    if (!doc) { img_err = "out of memory"; return 0; }
    memcpy(doc, data, len);
    doc[len] = 0;

    svg_ctx C;
    memset(&C, 0, sizeof C);

    /* --- find the root and work out the size ----------------------------- */
    const char *p = doc;
    const char *root = 0;
    while (*p) {
        if (p[0] == '<' && (p[1] == 's' || p[1] == 'S') &&
            svg_ci_eq(p + 1, "svg", 3) &&
            (svg_isspace(p[4]) || p[4] == '>' || p[4] == '/')) { root = p + 4; break; }
        p++;
    }
    if (!root) { free(doc); img_err = "not an SVG"; return 0; }

    svg_attr rat[SVG_ATTR_MAX];
    const char *rp = root;
    int nrat = svg_attrs(&rp, rat, SVG_ATTR_MAX);

    svg_fx vbx = 0, vby = 0, vbw = 0, vbh = 0;
    const svg_attr *vb = svg_find(rat, nrat, "viewBox");
    if (vb) {
        char b[160];
        svg_attr_str(vb, b, sizeof b);
        const char *q = b;
        vbx = svg_number(&q); vby = svg_number(&q);
        vbw = svg_number(&q); vbh = svg_number(&q);
    }

    svg_fx aw = svg_len(svg_find(rat, nrat, "width"),  vbw ? vbw : 300 * FX_ONE, 0);
    svg_fx ah = svg_len(svg_find(rat, nrat, "height"), vbh ? vbh : 150 * FX_ONE, 0);
    if (aw <= 0) aw = vbw;
    if (ah <= 0) ah = vbh;
    if (aw <= 0) aw = 300 * FX_ONE;
    if (ah <= 0) ah = 150 * FX_ONE;
    if (vbw <= 0 || vbh <= 0) { vbx = vby = 0; vbw = aw; vbh = ah; }

    int W = (aw + FX_HALF) >> 16, H = (ah + FX_HALF) >> 16;
    if (W < SVG_MIN_SIDE) W = SVG_MIN_SIDE;
    if (H < SVG_MIN_SIDE) H = SVG_MIN_SIDE;
    /* Small drawings are rendered larger than asked and shrunk by the caller,
     * which is cheap here and much sharper than scaling up afterwards. */
    int up = 1;
    while (W * up < 96 && H * up < 96 && up < 8) up *= 2;
    W *= up; H *= up;
    if (W > SVG_MAX_SIDE) { H = (int)((long)H * SVG_MAX_SIDE / W); W = SVG_MAX_SIDE; }
    if (H > SVG_MAX_SIDE) { W = (int)((long)W * SVG_MAX_SIDE / H); H = SVG_MAX_SIDE; }
    if (W < 1) W = 1;
    if (H < 1) H = 1;
    if (!img_dims_ok(W, H)) { free(doc); img_err = "bad SVG size"; return 0; }

    C.px = (unsigned *)malloc((size_t)W * H * sizeof(unsigned));
    if (!C.px) { free(doc); img_err = "out of memory"; return 0; }
    /* A drawing has no background of its own, so it is painted straight onto
     * whatever the caller said was behind it -- the same choice the other
     * decoders make for a transparent image. */
    for (long i = 0; i < (long)W * H; i++) C.px[i] = img_background;
    C.w = W; C.h = H;

    /* viewBox -> pixels, preserving the aspect ratio and centring, which is
     * what the default preserveAspectRatio asks for. */
    svg_fx sx = fx_div((svg_fx)W << 16, vbw);
    svg_fx sy = fx_div((svg_fx)H << 16, vbh);
    svg_fx s  = sx < sy ? sx : sy;
    svg_mat root_m = svg_ident();
    root_m.a = s; root_m.d = s;
    root_m.e = (((svg_fx)W << 16) - fx_mul(vbw, s)) / 2 - fx_mul(vbx, s);
    root_m.f = (((svg_fx)H << 16) - fx_mul(vbh, s)) / 2 - fx_mul(vby, s);

    C.depth = 0;
    C.st[0].m       = root_m;
    C.st[0].fill    = 0xFF000000u;      /* the initial fill really is black */
    C.st[0].stroke  = 0;
    C.st[0].swidth  = FX_ONE;
    C.st[0].evenodd = 0;
    C.st[0].opacity = 255;
    svg_state_apply(&C.st[0], rat, nrat);

    /* --- walk the tree ---------------------------------------------------- */
    p = rp;
    while (*p && *p != '>') p++;
    if (*p) p++;

    while (*p) {
        if (*p != '<') { p++; continue; }

        if (p[1] == '!') {                      /* comment, doctype, CDATA */
            if (!strncmp(p, "<!--", 4)) {
                const char *e = strstr(p + 4, "-->");
                p = e ? e + 3 : p + strlen(p);
            } else {
                while (*p && *p != '>') p++;
                if (*p) p++;
            }
            continue;
        }
        if (p[1] == '?') {
            while (*p && *p != '>') p++;
            if (*p) p++;
            continue;
        }

        if (p[1] == '/') {                      /* a closing tag */
            const char *n = p + 2;
            int nl = 0;
            while (n[nl] && n[nl] != '>' && !svg_isspace(n[nl])) nl++;
            if (C.skip_depth > 0) {
                C.skip_depth--;
            } else if (C.depth > 0 &&
                       !(nl == 3 && svg_ci_eq(n, "svg", 3))) {
                C.depth--;
            }
            while (*p && *p != '>') p++;
            if (*p) p++;
            continue;
        }

        const char *name = p + 1;
        int nl = 0;
        while (name[nl] && name[nl] != '>' && name[nl] != '/' && !svg_isspace(name[nl])) nl++;

        const char *ap = name + nl;
        svg_attr at[SVG_ATTR_MAX];
        int nat = svg_attrs(&ap, at, SVG_ATTR_MAX);
        int selfclose = (*ap == '/');
        while (*ap && *ap != '>') ap++;
        if (*ap) ap++;
        const char *after = ap;

        if (C.skip_depth > 0) {
            if (!selfclose) C.skip_depth++;
            p = after;
            continue;
        }
        if (svg_is_skipped(name, nl)) {
            if (!selfclose) C.skip_depth = 1;
            p = after;
            continue;
        }

        /* Everything inherits, so the child's state starts as a copy. */
        svg_state st = C.st[C.depth];
        svg_state_apply(&st, at, nat);

        int container = (nl == 1 && (name[0] == 'g' || name[0] == 'G')) ||
                        (nl == 3 && svg_ci_eq(name, "svg", 3)) ||
                        (nl == 1 && (name[0] == 'a' || name[0] == 'A'));

        if (container) {
            if (!selfclose && C.depth + 1 < SVG_DEPTH) C.st[++C.depth] = st;
            else if (!selfclose) C.skip_depth = 1;      /* nested too deep */
            p = after;
            continue;
        }

        svg_path P;
        spath_init(&P);
        char buf[64];

        if (nl == 4 && svg_ci_eq(name, "path", 4)) {
            const svg_attr *d = svg_find(at, nat, "d");
            if (d) {
                char *dd = (char *)malloc((size_t)d->vlen + 1);
                if (dd) {
                    memcpy(dd, d->val, (size_t)d->vlen);
                    dd[d->vlen] = 0;
                    svg_parse_path(&P, dd);
                    free(dd);
                }
            }
        } else if (nl == 4 && svg_ci_eq(name, "rect", 4)) {
            svg_fx x = svg_len(svg_find(at, nat, "x"), vbw, 0);
            svg_fx y = svg_len(svg_find(at, nat, "y"), vbh, 0);
            svg_fx w = svg_len(svg_find(at, nat, "width"),  vbw, 0);
            svg_fx h = svg_len(svg_find(at, nat, "height"), vbh, 0);
            const svg_attr *arx = svg_find(at, nat, "rx"), *ary = svg_find(at, nat, "ry");
            svg_fx r1 = svg_len(arx, vbw, -1), r2 = svg_len(ary, vbh, -1);
            if (r1 < 0) r1 = r2 < 0 ? 0 : r2;
            if (r2 < 0) r2 = r1;
            if (r1 > w / 2) r1 = w / 2;
            if (r2 > h / 2) r2 = h / 2;
            if (w > 0 && h > 0) {
                if (r1 > 0 && r2 > 0) svg_rounded_rect(&P, x, y, w, h, r1, r2);
                else {
                    spath_move(&P, x, y);
                    spath_pt(&P, x + w, y);
                    spath_pt(&P, x + w, y + h);
                    spath_pt(&P, x, y + h);
                    P.closed[P.ns - 1] = 1;
                }
            }
        } else if (nl == 6 && svg_ci_eq(name, "circle", 6)) {
            svg_fx cx = svg_len(svg_find(at, nat, "cx"), vbw, 0);
            svg_fx cy = svg_len(svg_find(at, nat, "cy"), vbh, 0);
            svg_fx r  = svg_len(svg_find(at, nat, "r"),  vbw, 0);
            if (r > 0) svg_ellipse_path(&P, cx, cy, r, r);
        } else if (nl == 7 && svg_ci_eq(name, "ellipse", 7)) {
            svg_fx cx = svg_len(svg_find(at, nat, "cx"), vbw, 0);
            svg_fx cy = svg_len(svg_find(at, nat, "cy"), vbh, 0);
            svg_fx r1 = svg_len(svg_find(at, nat, "rx"), vbw, 0);
            svg_fx r2 = svg_len(svg_find(at, nat, "ry"), vbh, 0);
            if (r1 > 0 && r2 > 0) svg_ellipse_path(&P, cx, cy, r1, r2);
        } else if (nl == 4 && svg_ci_eq(name, "line", 4)) {
            svg_fx x1 = svg_len(svg_find(at, nat, "x1"), vbw, 0);
            svg_fx y1 = svg_len(svg_find(at, nat, "y1"), vbh, 0);
            svg_fx x2 = svg_len(svg_find(at, nat, "x2"), vbw, 0);
            svg_fx y2 = svg_len(svg_find(at, nat, "y2"), vbh, 0);
            spath_move(&P, x1, y1);
            spath_pt(&P, x2, y2);
            st.fill = 0;                         /* a line is never filled */
        } else if ((nl == 7 && svg_ci_eq(name, "polygon", 7)) ||
                   (nl == 8 && svg_ci_eq(name, "polyline", 8))) {
            const svg_attr *pts = svg_find(at, nat, "points");
            if (pts) {
                char *s2 = (char *)malloc((size_t)pts->vlen + 1);
                if (s2) {
                    memcpy(s2, pts->val, (size_t)pts->vlen);
                    s2[pts->vlen] = 0;
                    svg_points_path(&P, s2, nl == 7);
                    free(s2);
                }
            }
            if (nl == 8) st.fill = 0;            /* a polyline is not filled */
        } else {
            (void)buf;
            spath_free(&P);
            p = after;
            continue;
        }

        if (P.ns > 0) svg_draw(&C, &P, &st);
        spath_free(&P);
        p = after;
    }

    free(C.edges.e);
    free(doc);

    out->w = W;
    out->h = H;
    out->px = C.px;
    return 1;
}

#endif /* SVG_H */
