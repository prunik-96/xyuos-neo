/* Paths: building them, the standard shapes, and turning curves into the
 * straight segments the rasteriser and the stroker work on. */

#include "rast_int.h"

/* --- the path ------------------------------------------------------------------ */

void rast_path_init(rast_path *p) { memset(p, 0, sizeof *p); }

void rast_path_free(rast_path *p) {
    free(p->ops);
    free(p->pts);
    memset(p, 0, sizeof *p);
}

void rast_path_reset(rast_path *p) {
    p->nops = p->npts = 0;
    p->cx = p->cy = p->sx = p->sy = 0;
    p->open = 0;
    p->err = 0;
}

static int room(rast_path *p, int ops, int pts) {
    if (p->err) return 0;
    if (p->nops + ops > p->capops) {
        int cap = p->capops ? p->capops * 2 : 64;
        while (cap < p->nops + ops) cap *= 2;
        uint8_t *n = (uint8_t *)realloc(p->ops, (size_t)cap);
        if (!n) { p->err = 1; return 0; }
        p->ops = n;
        p->capops = cap;
    }
    if (p->npts + pts > p->cappts) {
        int cap = p->cappts ? p->cappts * 2 : 128;
        while (cap < p->npts + pts) cap *= 2;
        rast_fx *n = (rast_fx *)realloc(p->pts, (size_t)cap * 2 * sizeof *n);
        if (!n) { p->err = 1; return 0; }
        p->pts = n;
        p->cappts = cap;
    }
    return 1;
}

static void put(rast_path *p, rast_fx x, rast_fx y) {
    p->pts[p->npts * 2] = x;
    p->pts[p->npts * 2 + 1] = y;
    p->npts++;
}

void rast_move_to(rast_path *p, rast_fx x, rast_fx y) {
    /* A move straight after a move drew nothing: the second replaces it. */
    if (p->nops && p->ops[p->nops - 1] == RAST_MOVE) {
        p->pts[(p->npts - 1) * 2] = x;
        p->pts[(p->npts - 1) * 2 + 1] = y;
    } else {
        if (!room(p, 1, 1)) return;
        p->ops[p->nops++] = RAST_MOVE;
        put(p, x, y);
    }
    p->cx = p->sx = x;
    p->cy = p->sy = y;
    p->open = 1;
}

/* Drawing with no subpath under way starts one at the current point -- which
 * after a close is where the closed subpath began, as SVG has it. */
static void ensure_open(rast_path *p) {
    if (!p->open) rast_move_to(p, p->cx, p->cy);
}

void rast_line_to(rast_path *p, rast_fx x, rast_fx y) {
    ensure_open(p);
    if (!room(p, 1, 1)) return;
    p->ops[p->nops++] = RAST_LINE;
    put(p, x, y);
    p->cx = x;
    p->cy = y;
}

void rast_quad_to(rast_path *p, rast_fx cx, rast_fx cy, rast_fx x, rast_fx y) {
    ensure_open(p);
    if (!room(p, 1, 2)) return;
    p->ops[p->nops++] = RAST_QUAD;
    put(p, cx, cy);
    put(p, x, y);
    p->cx = x;
    p->cy = y;
}

void rast_cubic_to(rast_path *p, rast_fx c1x, rast_fx c1y,
                   rast_fx c2x, rast_fx c2y, rast_fx x, rast_fx y) {
    ensure_open(p);
    if (!room(p, 1, 3)) return;
    p->ops[p->nops++] = RAST_CUBIC;
    put(p, c1x, c1y);
    put(p, c2x, c2y);
    put(p, x, y);
    p->cx = x;
    p->cy = y;
}

void rast_close(rast_path *p) {
    if (!p->open) return;
    if (!room(p, 1, 0)) return;
    p->ops[p->nops++] = RAST_CLOSE;
    p->cx = p->sx;
    p->cy = p->sy;
    p->open = 0;
}

/* --- shapes --------------------------------------------------------------------- */

/* Sums of coordinates saturate: a shape that reaches past the 16.16 range is
 * drawn cut off at its edge rather than wrapped round to the other side. */
static inline rast_fx S(int64_t v) { return rast_clamp32(v); }

/* A cubic with its control points this fraction of the radius along the
 * tangents matches a quarter circle to within 0.03% of the radius. */
#define KAPPA 36195                 /* 0.5522847 */

void rast_rect(rast_path *p, rast_fx x, rast_fx y, rast_fx w, rast_fx h) {
    rast_move_to(p, x, y);
    rast_line_to(p, S((int64_t)x + w), y);
    rast_line_to(p, S((int64_t)x + w), S((int64_t)y + h));
    rast_line_to(p, x, S((int64_t)y + h));
    rast_close(p);
}

void rast_ellipse(rast_path *p, rast_fx cx, rast_fx cy, rast_fx rx, rast_fx ry) {
    int64_t kx = rast_mulfx(rx, KAPPA), ky = rast_mulfx(ry, KAPPA);
    int64_t X = cx, Y = cy;
    /* Where SVG starts one, and the way it goes: clockwise on screen. */
    rast_move_to(p, S(X + rx), cy);
    rast_cubic_to(p, S(X + rx), S(Y + ky), S(X + kx), S(Y + ry), cx, S(Y + ry));
    rast_cubic_to(p, S(X - kx), S(Y + ry), S(X - rx), S(Y + ky), S(X - rx), cy);
    rast_cubic_to(p, S(X - rx), S(Y - ky), S(X - kx), S(Y - ry), cx, S(Y - ry));
    rast_cubic_to(p, S(X + kx), S(Y - ry), S(X + rx), S(Y - ky), S(X + rx), cy);
    rast_close(p);
}

void rast_round_rect(rast_path *p, rast_fx x, rast_fx y, rast_fx w, rast_fx h,
                     rast_fx rx, rast_fx ry) {
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx > w / 2) rx = w / 2;
    if (ry > h / 2) ry = h / 2;
    if (!rx || !ry) { rast_rect(p, x, y, w, h); return; }
    int64_t kx = rast_mulfx(rx, KAPPA), ky = rast_mulfx(ry, KAPPA);
    int64_t X = x, Y = y, r = X + w, b = Y + h;
    rast_move_to(p, S(X + rx), y);
    rast_line_to(p, S(r - rx), y);
    rast_cubic_to(p, S(r - rx + kx), y, S(r), S(Y + ry - ky), S(r), S(Y + ry));
    rast_line_to(p, S(r), S(b - ry));
    rast_cubic_to(p, S(r), S(b - ry + ky), S(r - rx + kx), S(b), S(r - rx), S(b));
    rast_line_to(p, S(X + rx), S(b));
    rast_cubic_to(p, S(X + rx - kx), S(b), x, S(b - ry + ky), x, S(b - ry));
    rast_line_to(p, x, S(Y + ry));
    rast_cubic_to(p, x, S(Y + ry - ky), S(X + rx - kx), y, S(X + rx), y);
    rast_close(p);
}

/* One piece of an ellipse, at most a quarter turn, as a cubic: the ends at
 * angles t0 and t0+dt on the unit circle, the control points along the
 * tangents by 4/3 tan(dt/4), then out through the radii, the rotation and
 * the centre. */
typedef struct { rast_fx cx, cy, rx, ry, cosr, sinr; } ell;

static void ell_pt(const ell *E, rast_fx ux, rast_fx uy, rast_fx *x, rast_fx *y) {
    int64_t ex = ((int64_t)E->rx * ux) >> 16, ey = ((int64_t)E->ry * uy) >> 16;
    *x = S(E->cx + ((E->cosr * ex - E->sinr * ey) >> 16));
    *y = S(E->cy + ((E->sinr * ex + E->cosr * ey) >> 16));
}

static void ell_arc(rast_path *p, const ell *E, rast_fx t0, rast_fx dt,
                    int last, rast_fx lx, rast_fx ly) {
    int n = 1;
    rast_fx adt = dt < 0 ? -dt : dt;
    while (adt > (rast_fx)n * 90 * RAST_ONE) n++;
    rast_fx step = dt / n;
    rast_fx q = step / 4;
    rast_fx k = rast_divfx(rast_sin(q), rast_cos(q));      /* tan(step/4) */
    k = (rast_fx)(((int64_t)k * 4) / 3);
    for (int i = 0; i < n; i++) {
        rast_fx a = t0 + step * i, b = a + step;
        if (i == n - 1) b = t0 + dt;
        rast_fx ca = rast_cos(a), sa = rast_sin(a), cb = rast_cos(b), sb = rast_sin(b);
        rast_fx x1, y1, x2, y2, x3, y3;
        ell_pt(E, ca - rast_mulfx(k, sa), sa + rast_mulfx(k, ca), &x1, &y1);
        ell_pt(E, cb + rast_mulfx(k, sb), sb - rast_mulfx(k, cb), &x2, &y2);
        ell_pt(E, cb, sb, &x3, &y3);
        /* The last point exactly where it was asked to be, not where the
         * arithmetic came out: the next segment starts from it. */
        if (last && i == n - 1) { x3 = lx; y3 = ly; }
        rast_cubic_to(p, x1, y1, x2, y2, x3, y3);
    }
}

/* SVG's endpoint arc, turned into a centre and two angles as the
 * specification's appendix does it -- but in the space where the ellipse is
 * the unit circle, which keeps every intermediate number near one. */
void rast_arc_to(rast_path *p, rast_fx rx, rast_fx ry, rast_fx rot,
                 int large, int sweep, rast_fx x, rast_fx y) {
    ensure_open(p);
    rast_fx x0 = p->cx, y0 = p->cy;
    if (x0 == x && y0 == y) return;               /* the specification: omit */
    if (rx < 0) rx = -rx;
    if (ry < 0) ry = -ry;
    if (!rx || !ry) { rast_line_to(p, x, y); return; }

    rast_fx cosr = rast_cos(rot), sinr = rast_sin(rot);
    rast_fx dx2 = x0 / 2 - x / 2, dy2 = y0 / 2 - y / 2;
    rast_fx x1p =  rast_mulfx(cosr, dx2) + rast_mulfx(sinr, dy2);
    rast_fx y1p = -rast_mulfx(sinr, dx2) + rast_mulfx(cosr, dy2);

    /* The start point, halfway chord, in unit-circle space. */
    rast_fx ux = rast_divfx(x1p, rx), uy = rast_divfx(y1p, ry);
    rast_fx L = rast_len(ux, uy);
    if (L > RAST_ONE) {                           /* radii too small: grow them */
        rx = rast_mulfx(rx, L);
        ry = rast_mulfx(ry, L);
        ux = rast_divfx(x1p, rx);
        uy = rast_divfx(y1p, ry);
        L = RAST_ONE;
    }
    if (!L) { rast_line_to(p, x, y); return; }

    /* The centre sits sqrt(1 - L^2) from the chord's middle, across it. */
    rast_fx h = rast_sqrtfx(RAST_ONE - rast_mulfx(L, L));
    rast_fx cux = rast_divfx(rast_mulfx(h, uy), L);
    rast_fx cuy = rast_divfx(rast_mulfx(h, -ux), L);
    if (large == sweep) { cux = -cux; cuy = -cuy; }

    rast_fx t0 = rast_atan2(uy - cuy, ux - cux);
    rast_fx t1 = rast_atan2(-uy - cuy, -ux - cux);
    rast_fx dt = t1 - t0;
    if (!sweep && dt > 0) dt -= 360 * RAST_ONE;
    if (sweep && dt < 0) dt += 360 * RAST_ONE;

    ell E;
    E.rx = rx; E.ry = ry; E.cosr = cosr; E.sinr = sinr;
    rast_fx cxp = rast_mulfx(rx, cux), cyp = rast_mulfx(ry, cuy);
    E.cx = S((int64_t)rast_mulfx(cosr, cxp) - rast_mulfx(sinr, cyp) + x0 / 2 + x / 2);
    E.cy = S((int64_t)rast_mulfx(sinr, cxp) + rast_mulfx(cosr, cyp) + y0 / 2 + y / 2);
    ell_arc(p, &E, t0, dt, 1, x, y);
}

void rast_arc(rast_path *p, rast_fx cx, rast_fx cy, rast_fx r,
              rast_fx a0, rast_fx a1) {
    if (r <= 0) return;
    ell E = { cx, cy, r, r, RAST_ONE, 0 };
    rast_fx sx, sy;
    ell_pt(&E, rast_cos(a0), rast_sin(a0), &sx, &sy);
    if (p->open) rast_line_to(p, sx, sy);
    else rast_move_to(p, sx, sy);
    if (a1 != a0) ell_arc(p, &E, a0, a1 - a0, 0, 0, 0);
}

int rast_path_bounds(const rast_path *p, rast_fx box[4]) {
    if (!p->npts) return 0;
    box[0] = box[2] = p->pts[0];
    box[1] = box[3] = p->pts[1];
    for (int i = 1; i < p->npts; i++) {
        rast_fx x = p->pts[i * 2], y = p->pts[i * 2 + 1];
        if (x < box[0]) box[0] = x;
        if (x > box[2]) box[2] = x;
        if (y < box[1]) box[1] = y;
        if (y > box[3]) box[3] = y;
    }
    return 1;
}

/* --- polylines -------------------------------------------------------------------- */

void rpoly_init(rpoly *P) { memset(P, 0, sizeof *P); }

void rpoly_free(rpoly *P) {
    free(P->xy);
    free(P->start);
    free(P->closed);
    memset(P, 0, sizeof *P);
}

void rpoly_reset(rpoly *P) { P->n = P->ns = 0; P->err = 0; }

static int pt_room(rpoly *P) {
    if (P->n < P->cap) return 1;
    int cap = P->cap ? P->cap * 2 : 256;
    int32_t *n = (int32_t *)realloc(P->xy, (size_t)cap * 2 * sizeof *n);
    if (!n) { P->err = 1; return 0; }
    P->xy = n;
    P->cap = cap;
    return 1;
}

void rpoly_move(rpoly *P, int32_t x, int32_t y) {
    if (P->err) return;
    if (P->ns == P->scap) {
        int cap = P->scap ? P->scap * 2 : 16;
        int *s = (int *)realloc(P->start, (size_t)cap * sizeof *s);
        if (s) P->start = s;
        uint8_t *c = (uint8_t *)realloc(P->closed, (size_t)cap);
        if (c) P->closed = c;
        if (!s || !c) { P->err = 1; return; }
        P->scap = cap;
    }
    if (!pt_room(P)) return;
    P->start[P->ns] = P->n;
    P->closed[P->ns] = 0;
    P->ns++;
    P->xy[P->n * 2] = x;
    P->xy[P->n * 2 + 1] = y;
    P->n++;
}

void rpoly_line(rpoly *P, int32_t x, int32_t y) {
    if (P->err) return;
    if (!P->ns) { rpoly_move(P, x, y); return; }
    int last = P->n - 1;
    if (P->xy[last * 2] == x && P->xy[last * 2 + 1] == y) return;
    if (!pt_room(P)) return;
    P->xy[P->n * 2] = x;
    P->xy[P->n * 2 + 1] = y;
    P->n++;
}

void rpoly_close(rpoly *P) {
    if (P->ns) P->closed[P->ns - 1] = 1;
}

int rpoly_len(const rpoly *P, int s) {
    int end = (s + 1 < P->ns) ? P->start[s + 1] : P->n;
    return end - P->start[s];
}

/* --- flattening --------------------------------------------------------------------
 *
 * A Bezier cut into n equal steps of its parameter strays from the chords by
 * at most (1/8) * max|B''| / n^2, and |B''| is bounded by the second
 * differences of the control points -- so n comes straight from the
 * tolerance, with no recursion. The points are then generated by forward
 * differences in 64-bit integers scaled by n^2 (or n^3), which makes every
 * one of them exact: nothing accumulates, and the last lands on the end. */

static int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }

static int32_t div_round(int64_t v, int64_t d) {
    return (int32_t)((v >= 0 ? v + d / 2 : v - d / 2) / d);
}

static int steps_for(int64_t dd, int64_t num, int64_t den, rast_fx tol, int max) {
    if (tol < 1) tol = 1;
    int64_t n2 = (dd * num + den * tol - 1) / (den * tol);
    int n = (int)rast_isqrt64((uint64_t)n2) + 1;
    return n > max ? max : n;
}

static void flat_quad(rpoly *o, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                      int32_t x2, int32_t y2, rast_fx tol) {
    int64_t ax = (int64_t)x0 - 2 * (int64_t)x1 + x2;
    int64_t ay = (int64_t)y0 - 2 * (int64_t)y1 + y2;
    /* error <= |A| / (4 n^2) */
    int n = steps_for(iabs64(ax) + iabs64(ay), 1, 4, tol, 512);
    int64_t n2 = (int64_t)n * n;
    int64_t Fx = (int64_t)x0 * n2, Fy = (int64_t)y0 * n2;
    int64_t d1x = ax + 2 * ((int64_t)x1 - x0) * n;
    int64_t d1y = ay + 2 * ((int64_t)y1 - y0) * n;
    for (int i = 1; i < n; i++) {
        Fx += d1x; Fy += d1y;
        d1x += 2 * ax; d1y += 2 * ay;
        rpoly_line(o, div_round(Fx, n2), div_round(Fy, n2));
    }
    rpoly_line(o, x2, y2);
}

static void flat_cubic(rpoly *o, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                       int32_t x2, int32_t y2, int32_t x3, int32_t y3, rast_fx tol) {
    int64_t e1 = iabs64((int64_t)x0 - 2 * (int64_t)x1 + x2) +
                 iabs64((int64_t)y0 - 2 * (int64_t)y1 + y2);
    int64_t e2 = iabs64((int64_t)x1 - 2 * (int64_t)x2 + x3) +
                 iabs64((int64_t)y1 - 2 * (int64_t)y2 + y3);
    /* error <= (3/4) * max second difference / n^2 */
    int n = steps_for(e1 > e2 ? e1 : e2, 3, 4, tol, 1024);
    int64_t nn = n, n3 = nn * nn * nn;
    int64_t Ax = -(int64_t)x0 + 3 * (int64_t)x1 - 3 * (int64_t)x2 + x3;
    int64_t Ay = -(int64_t)y0 + 3 * (int64_t)y1 - 3 * (int64_t)y2 + y3;
    int64_t Bx = 3 * (int64_t)x0 - 6 * (int64_t)x1 + 3 * (int64_t)x2;
    int64_t By = 3 * (int64_t)y0 - 6 * (int64_t)y1 + 3 * (int64_t)y2;
    int64_t Cx = 3 * ((int64_t)x1 - x0), Cy = 3 * ((int64_t)y1 - y0);
    int64_t Fx = (int64_t)x0 * n3, Fy = (int64_t)y0 * n3;
    int64_t d1x = Ax + Bx * nn + Cx * nn * nn, d1y = Ay + By * nn + Cy * nn * nn;
    int64_t d2x = 6 * Ax + 2 * Bx * nn, d2y = 6 * Ay + 2 * By * nn;
    int64_t d3x = 6 * Ax, d3y = 6 * Ay;
    for (int i = 1; i < n; i++) {
        Fx += d1x; Fy += d1y;
        d1x += d2x; d1y += d2y;
        d2x += d3x; d2y += d3y;
        rpoly_line(o, div_round(Fx, n3), div_round(Fy, n3));
    }
    rpoly_line(o, x3, y3);
}

void rast_flatten(const rast_path *p, const rast_mat *m, rast_fx tol, rpoly *out) {
    rast_mat I = rast_identity();
    if (!m) m = &I;
    int k = 0;
    int32_t cx = 0, cy = 0, mx = 0, my = 0;
    int pending = 0;       /* a move not yet followed by anything that draws */

    for (int i = 0; i < p->nops; i++) {
        int op = p->ops[i];
        int32_t x[3], y[3];
        int np = op == RAST_MOVE || op == RAST_LINE ? 1
               : op == RAST_QUAD ? 2 : op == RAST_CUBIC ? 3 : 0;
        for (int j = 0; j < np; j++, k++)
            rast_apply(m, p->pts[k * 2], p->pts[k * 2 + 1], &x[j], &y[j]);

        if (op == RAST_MOVE) {
            mx = cx = x[0];
            my = cy = y[0];
            pending = 1;
            continue;
        }
        if (pending) { rpoly_move(out, mx, my); pending = 0; }
        switch (op) {
        case RAST_LINE:
            rpoly_line(out, x[0], y[0]);
            cx = x[0]; cy = y[0];
            break;
        case RAST_QUAD:
            flat_quad(out, cx, cy, x[0], y[0], x[1], y[1], tol);
            cx = x[1]; cy = y[1];
            break;
        case RAST_CUBIC:
            flat_cubic(out, cx, cy, x[0], y[0], x[1], y[1], x[2], y[2], tol);
            cx = x[2]; cy = y[2];
            break;
        case RAST_CLOSE:
            rpoly_close(out);
            cx = mx; cy = my;
            break;
        }
    }
    if (p->err) out->err = 1;
}
