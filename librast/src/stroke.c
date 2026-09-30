/* Strokes: dashes, and the outline of a pen of some width drawn along a
 * polyline, with joins at its corners and caps at its ends.
 *
 * The outline is built as polygons to be filled under the nonzero rule. For
 * an open line it is one loop: out along the left edge, round the end cap,
 * back along the right edge, round the start cap. A closed line is two loops
 * -- the left edge forwards and the right edge backwards -- whose opposite
 * windings leave the inside empty.
 *
 * At a corner the two edges behave differently. On the outside they part,
 * and the gap is closed by the join: a miter, a round, or a bevel. On the
 * inside they cross, and there each edge simply runs on to the corner point
 * and back out again. That leaves small loops inside the stroke; every one of
 * them winds the same way as the stroke around it, so the nonzero fill covers
 * them and they never show. It is what keeps the inside of a sharp turn
 * right however short the segments either side are, which cutting the
 * edges off at their crossing does not.
 */

#include "rast_int.h"

void rast_stroke_init(rast_stroke *s, rast_fx width) {
    memset(s, 0, sizeof *s);
    s->width = width;
    s->join = RAST_JOIN_MITER;
    s->cap = RAST_CAP_BUTT;
    s->miter_limit = 4 * RAST_ONE;
}

/* --- dashes --------------------------------------------------------------------- */

void rast_dash(const rpoly *in, const rast_stroke *s, rpoly *out) {
    /* An odd-length list repeats to make an even one: "5" is "5 5". */
    int n = s->ndash, count = (n & 1) ? 2 * n : n;
    int64_t total = 0;
    for (int i = 0; i < count; i++) {
        rast_fx d = s->dash[i % n];
        if (d < 0) count = 0;
        total += d;
    }
    /* No pattern, or one so fine it would be millions of pieces: solid. */
    int64_t length = 0;
    for (int sp = 0; sp < in->ns; sp++) {
        int m = rpoly_len(in, sp), b = in->start[sp];
        for (int i = 0; i + 1 < m + (in->closed[sp] ? 1 : 0) && m > 1; i++) {
            int j = (i + 1) % m;
            length += rast_len(in->xy[(b + j) * 2] - in->xy[(b + i) * 2],
                               in->xy[(b + j) * 2 + 1] - in->xy[(b + i) * 2 + 1]);
        }
    }
    if (count == 0 || total <= 0 || length / total > 100000) {
        for (int sp = 0; sp < in->ns; sp++) {
            int m = rpoly_len(in, sp), b = in->start[sp];
            for (int i = 0; i < m; i++) {
                if (i == 0) rpoly_move(out, in->xy[b * 2], in->xy[b * 2 + 1]);
                else rpoly_line(out, in->xy[(b + i) * 2], in->xy[(b + i) * 2 + 1]);
            }
            if (in->closed[sp]) rpoly_close(out);
        }
        return;
    }

    for (int sp = 0; sp < in->ns; sp++) {
        int m = rpoly_len(in, sp), b = in->start[sp];
        if (m < 2) continue;

        /* Where in the pattern the subpath starts. */
        int64_t off = s->dash_offset % total;
        if (off < 0) off += total;
        int idx = 0;
        while (off >= s->dash[idx % n]) {
            off -= s->dash[idx % n];
            idx = (idx + 1) % count;
        }
        int64_t rem = s->dash[idx % n] - off;     /* left of the current dash or gap */
        int on = !(idx & 1);
        if (on) rpoly_move(out, in->xy[b * 2], in->xy[b * 2 + 1]);

        int segs = in->closed[sp] ? m : m - 1;
        for (int i = 0; i < segs; i++) {
            int j = (i + 1) % m;
            int32_t ax = in->xy[(b + i) * 2], ay = in->xy[(b + i) * 2 + 1];
            int32_t bx = in->xy[(b + j) * 2], by = in->xy[(b + j) * 2 + 1];
            int64_t len = rast_len(bx - ax, by - ay), pos = 0;
            if (!len) continue;
            while (len - pos > rem) {
                pos += rem;
                int32_t px = ax + (int32_t)(((int64_t)(bx - ax) * pos) / len);
                int32_t py = ay + (int32_t)(((int64_t)(by - ay) * pos) / len);
                if (on) rpoly_line(out, px, py);
                else rpoly_move(out, px, py);
                on = !on;
                idx = (idx + 1) % count;
                rem = s->dash[idx % n];
            }
            rem -= len - pos;
            if (on) rpoly_line(out, bx, by);
        }
    }
}

/* --- the outline ----------------------------------------------------------------- */

typedef struct {
    rpoly  *out;
    rast_fx hw;                 /* half the width */
    int     join, cap;
    rast_fx limit;
    rast_fx tol;
} sctx;

/* The points of an arc of radius hw round (cx,cy), from vector a to vector b
 * (both of that length, less than half a turn apart), after a and up to and
 * including b. Halved until each chord is within the tolerance of the
 * circle -- no angles, no trigonometry. */
static void arc_to(sctx *S, rpoly *o, int32_t cx, int32_t cy, rast_fx ax, rast_fx ay,
                   rast_fx bx, rast_fx by, int depth) {
    rast_fx mx = ax / 2 + bx / 2, my = ay / 2 + by / 2;
    rast_fx ml = rast_len(mx, my);
    if (depth >= 12 || ml == 0 || S->hw - ml <= S->tol) {
        rpoly_line(o, cx + bx, cy + by);
        return;
    }
    rast_fx nx = (rast_fx)(((int64_t)mx * S->hw) / ml);
    rast_fx ny = (rast_fx)(((int64_t)my * S->hw) / ml);
    arc_to(S, o, cx, cy, ax, ay, nx, ny, depth + 1);
    arc_to(S, o, cx, cy, nx, ny, bx, by, depth + 1);
}

/* An arc that may be a half turn or more: split at `mid` (a vector of length
 * hw pointing the way the arc bulges) first. */
static void arc_via(sctx *S, rpoly *o, int32_t cx, int32_t cy, rast_fx ax, rast_fx ay,
                    rast_fx mx, rast_fx my, rast_fx bx, rast_fx by) {
    arc_to(S, o, cx, cy, ax, ay, mx, my, 0);
    arc_to(S, o, cx, cy, mx, my, bx, by, 0);
}

/* Unit vector along (dx,dy), 16.16. */
static void unit(int32_t dx, int32_t dy, rast_fx *ux, rast_fx *uy) {
    rast_fx l = rast_len(dx, dy);
    if (!l) { *ux = RAST_ONE; *uy = 0; return; }
    *ux = (rast_fx)(((int64_t)dx * RAST_ONE) / l);
    *uy = (rast_fx)(((int64_t)dy * RAST_ONE) / l);
}

/* One side of a corner at (px,py), from the offset a (end of the segment
 * before) to b (start of the segment after): vectors from the corner, both
 * of length hw. `outer` says which kind of side it is. (dx,dy) is the
 * direction of the segment before, for a turn so sharp it doubles back. */
static void corner(sctx *S, rpoly *o, int outer, int32_t px, int32_t py,
                   rast_fx ax, rast_fx ay, rast_fx bx, rast_fx by,
                   rast_fx nax, rast_fx nay, rast_fx nbx, rast_fx nby,
                   rast_fx dx, rast_fx dy) {
    rpoly_line(o, px + ax, py + ay);
    if (!outer) {
        rpoly_line(o, px, py);
        rpoly_line(o, px + bx, py + by);
        return;
    }
    /* cos of the angle between the two normals */
    rast_fx dot = (rast_fx)(((int64_t)nax * nbx + (int64_t)nay * nby) >> 16);
    if (S->join == RAST_JOIN_MITER) {
        /* The miter is 1/cos(half that angle) widths long; allowed while that
         * stays within the limit: (1 + dot)/2 >= 1/limit^2. */
        int64_t lim2 = ((int64_t)S->limit * S->limit) >> 16;       /* 16.16 */
        if (((int64_t)(RAST_ONE + dot) * lim2) >= 2LL * RAST_ONE * RAST_ONE &&
            RAST_ONE + dot > 0) {
            int64_t k = RAST_ONE + dot;
            rpoly_line(o, px + (int32_t)(((int64_t)(ax + bx) * RAST_ONE) / k),
                          py + (int32_t)(((int64_t)(ay + by) * RAST_ONE) / k));
        }
    } else if (S->join == RAST_JOIN_ROUND) {
        rast_fx mx = ax / 2 + bx / 2, my = ay / 2 + by / 2;
        rast_fx ml = rast_len(mx, my);
        if (ml > S->hw / 8) {
            mx = (rast_fx)(((int64_t)mx * S->hw) / ml);
            my = (rast_fx)(((int64_t)my * S->hw) / ml);
        } else {                                  /* doubling back: bulge onward */
            mx = rast_mulfx(dx, S->hw);
            my = rast_mulfx(dy, S->hw);
        }
        arc_via(S, o, px, py, ax, ay, mx, my, bx, by);
        return;
    }
    rpoly_line(o, px + bx, py + by);
}

/* A cap: from the offset (ex,ey) round to its opposite, bulging along
 * (dx,dy), the direction the line was going. */
static void cap(sctx *S, rpoly *o, int32_t px, int32_t py, rast_fx ex, rast_fx ey,
                rast_fx dx, rast_fx dy) {
    rast_fx fx = rast_mulfx(dx, S->hw), fy = rast_mulfx(dy, S->hw);
    if (S->cap == RAST_CAP_SQUARE) {
        rpoly_line(o, px + ex + fx, py + ey + fy);
        rpoly_line(o, px - ex + fx, py - ey + fy);
    } else if (S->cap == RAST_CAP_ROUND) {
        arc_via(S, o, px, py, ex, ey, fx, fy, -ex, -ey);
        return;
    }
    rpoly_line(o, px - ex, py - ey);
}

/* A subpath with nothing to follow: a dot, if the cap makes one. */
static void dot(sctx *S, int32_t px, int32_t py) {
    rast_fx h = S->hw;
    if (S->cap == RAST_CAP_ROUND) {
        rpoly_move(S->out, px + h, py);
        arc_via(S, S->out, px, py, h, 0, 0, h, -h, 0);
        arc_via(S, S->out, px, py, -h, 0, 0, -h, h, 0);
        rpoly_close(S->out);
    } else if (S->cap == RAST_CAP_SQUARE) {
        rpoly_move(S->out, px - h, py - h);
        rpoly_line(S->out, px + h, py - h);
        rpoly_line(S->out, px + h, py + h);
        rpoly_line(S->out, px - h, py + h);
        rpoly_close(S->out);
    }
}

static void stroke_sub(sctx *S, const int32_t *xy, int n, int closed, rpoly *right) {
    if (closed && n > 1 && xy[0] == xy[(n - 1) * 2] && xy[1] == xy[(n - 1) * 2 + 1])
        n--;
    if (n == 1) { dot(S, xy[0], xy[1]); return; }

    int segs = closed ? n : n - 1;
    /* Direction and left normal of segment i, computed as needed. */
    #define SEG(i, dx_, dy_) unit(xy[(((i) + 1) % n) * 2] - xy[(i) * 2], \
                                  xy[(((i) + 1) % n) * 2 + 1] - xy[(i) * 2 + 1], dx_, dy_)
    rast_fx hw = S->hw;
    rpoly *L = S->out;
    rpoly_reset(right);

    rast_fx d0x, d0y;
    SEG(0, &d0x, &d0y);

    if (!closed) {
        rast_fx nx = -d0y, ny = d0x;
        rast_fx ox = rast_mulfx(nx, hw), oy = rast_mulfx(ny, hw);
        rpoly_move(L, xy[0] + ox, xy[1] + oy);
        rpoly_move(right, xy[0] - ox, xy[1] - oy);
    }

    rast_fx pdx = d0x, pdy = d0y;
    int first = closed ? 0 : 1, last = closed ? n - 1 : n - 2;
    if (closed) SEG(segs - 1, &pdx, &pdy);          /* the segment into point 0 */

    for (int i = first; i <= last; i++) {
        rast_fx ndx, ndy;
        SEG(i, &ndx, &ndy);
        rast_fx pnx = -pdy, pny = pdx, nnx = -ndy, nny = ndx;
        rast_fx pax = rast_mulfx(pnx, hw), pay = rast_mulfx(pny, hw);
        rast_fx nbx = rast_mulfx(nnx, hw), nby = rast_mulfx(nny, hw);
        int32_t px = xy[i * 2], py = xy[i * 2 + 1];
        /* Turning towards the left (positive normal) side makes it the
         * inner side of the corner. */
        int64_t cr = ((int64_t)pdx * ndy - (int64_t)pdy * ndx) >> 16;
        int64_t dt = ((int64_t)pdx * ndx + (int64_t)pdy * ndy) >> 16;
        if (i == first && closed) {
            rpoly_move(L, px + pax, py + pay);
            rpoly_move(right, px - pax, py - pay);
        }
        if (cr == 0 && dt > 0) {                  /* straight on */
            rpoly_line(L, px + nbx, py + nby);
            rpoly_line(right, px - nbx, py - nby);
        } else {
            int left_outer = cr < 0;
            corner(S, L, left_outer, px, py, pax, pay, nbx, nby,
                   pnx, pny, nnx, nny, pdx, pdy);
            corner(S, right, !left_outer, px, py, -pax, -pay, -nbx, -nby,
                   -pnx, -pny, -nnx, -nny, pdx, pdy);
        }
        pdx = ndx; pdy = ndy;
    }

    if (closed) {
        /* Two loops: the left edge as it is, the right edge reversed. */
        rpoly_close(L);
        rpoly_move(L, right->xy[(right->n - 1) * 2], right->xy[(right->n - 1) * 2 + 1]);
        for (int k = right->n - 2; k >= 0; k--)
            rpoly_line(L, right->xy[k * 2], right->xy[k * 2 + 1]);
        rpoly_close(L);
    } else {
        int32_t ex = xy[(n - 1) * 2], ey = xy[(n - 1) * 2 + 1];
        rast_fx nx = -pdy, ny = pdx;
        rast_fx ox = rast_mulfx(nx, hw), oy = rast_mulfx(ny, hw);
        rpoly_line(L, ex + ox, ey + oy);
        cap(S, L, ex, ey, ox, oy, pdx, pdy);      /* round the far end */
        rpoly_line(L, ex - ox, ey - oy);
        for (int k = right->n - 1; k >= 0; k--)
            rpoly_line(L, right->xy[k * 2], right->xy[k * 2 + 1]);
        rast_fx sx = rast_mulfx(-d0y, hw), sy = rast_mulfx(d0x, hw);
        cap(S, L, xy[0], xy[1], -sx, -sy, -d0x, -d0y);   /* and the near one */
        rpoly_close(L);
    }
    #undef SEG
    if (right->err) L->err = 1;
}

void rast_stroke_outline(const rpoly *in, const rast_stroke *s, rast_fx tol, rpoly *out) {
    static rpoly right;
    sctx S;
    S.out = out;
    S.hw = s->width / 2;
    S.join = s->join;
    S.cap = s->cap;
    S.limit = s->miter_limit < RAST_ONE ? RAST_ONE : s->miter_limit;
    S.tol = tol < 1 ? 1 : tol;
    if (S.hw <= 0) return;
    for (int sp = 0; sp < in->ns; sp++) {
        int m = rpoly_len(in, sp);
        if (m < 1) continue;
        stroke_sub(&S, in->xy + in->start[sp] * 2, m, in->closed[sp], &right);
    }
}
