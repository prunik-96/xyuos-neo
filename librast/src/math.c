/* Fixed-point arithmetic, the few functions of an angle librast needs, and
 * affine matrices. */

#include "rast_int.h"

rast_fx rast_mulfx(rast_fx a, rast_fx b) {
    return rast_clamp32(((int64_t)a * b) >> 16);
}

rast_fx rast_divfx(rast_fx a, rast_fx b) {
    if (!b) return 0;
    return rast_clamp32(((int64_t)a * RAST_ONE) / b);
}

/* Bit by bit: the square root rounded down, exact for every 64-bit value. */
uint64_t rast_isqrt64(uint64_t v) {
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

rast_fx rast_sqrtfx(rast_fx v) {
    if (v <= 0) return 0;
    return (rast_fx)rast_isqrt64((uint64_t)v << 16);
}

/* Sine over a quarter turn at every degree, 16.16; the rest by symmetry and
 * straight-line interpolation between the degrees, which is good to about
 * four parts in a hundred thousand -- below what 16.16 can hold anyway. */
static const int32_t sin_tab[91] = {
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

rast_fx rast_sin(rast_fx deg) {
    const int64_t full = 360LL * RAST_ONE, half = 180LL * RAST_ONE,
                  quarter = 90LL * RAST_ONE;
    int64_t d = deg % full;
    if (d < 0) d += full;
    int neg = 0;
    if (d >= half) { d -= half; neg = 1; }
    if (d > quarter) d = half - d;
    if (d >= quarter) return neg ? -RAST_ONE : RAST_ONE;   /* the table's end */
    int i = (int)(d >> 16);
    int32_t frac = (int32_t)(d & 0xFFFF);
    int32_t v = sin_tab[i] + (int32_t)(((int64_t)(sin_tab[i + 1] - sin_tab[i]) * frac) >> 16);
    return neg ? -v : v;
}

rast_fx rast_cos(rast_fx deg) { return rast_sin(deg + 90 * RAST_ONE); }

/* By bisection on the sign of a cross product: a thousandth of a degree in
 * twenty-four steps, which is plenty for turning an arc into curves. The
 * half turn that holds the answer is picked by the sign of y; inside it,
 * (x,y) lying counterclockwise of the midpoint's direction -- a positive
 * cross product -- means the answer is above the midpoint. */
rast_fx rast_atan2(rast_fx y, rast_fx x) {
    if (!x && !y) return 0;
    rast_fx lo = y >= 0 ? 0 : -180 * RAST_ONE;
    rast_fx hi = y >= 0 ? 180 * RAST_ONE : 0;
    for (int i = 0; i < 24; i++) {
        rast_fx mid = lo + ((hi - lo) >> 1);
        int64_t cr = (int64_t)rast_cos(mid) * y - (int64_t)rast_sin(mid) * x;
        if (cr > 0) lo = mid; else hi = mid;
    }
    return lo + ((hi - lo) >> 1);
}

/* --- matrices ------------------------------------------------------------------ */

rast_mat rast_identity(void) {
    rast_mat m = { RAST_ONE, 0, 0, RAST_ONE, 0, 0 };
    return m;
}

rast_mat rast_mul(rast_mat m, rast_mat n) {
    rast_mat r;
    r.a = rast_mulfx(m.a, n.a) + rast_mulfx(m.c, n.b);
    r.b = rast_mulfx(m.b, n.a) + rast_mulfx(m.d, n.b);
    r.c = rast_mulfx(m.a, n.c) + rast_mulfx(m.c, n.d);
    r.d = rast_mulfx(m.b, n.c) + rast_mulfx(m.d, n.d);
    r.e = rast_mulfx(m.a, n.e) + rast_mulfx(m.c, n.f) + m.e;
    r.f = rast_mulfx(m.b, n.e) + rast_mulfx(m.d, n.f) + m.f;
    return r;
}

rast_mat rast_translate(rast_fx x, rast_fx y) {
    rast_mat m = { RAST_ONE, 0, 0, RAST_ONE, x, y };
    return m;
}

rast_mat rast_scale(rast_fx sx, rast_fx sy) {
    rast_mat m = { sx, 0, 0, sy, 0, 0 };
    return m;
}

rast_mat rast_rotate(rast_fx deg) {
    rast_fx c = rast_cos(deg), s = rast_sin(deg);
    rast_mat m = { c, s, -s, c, 0, 0 };
    return m;
}

rast_mat rast_skew(rast_fx xdeg, rast_fx ydeg) {
    rast_mat m = rast_identity();
    rast_fx cx = rast_cos(xdeg), cy = rast_cos(ydeg);
    if (cx) m.c = rast_divfx(rast_sin(xdeg), cx);
    if (cy) m.b = rast_divfx(rast_sin(ydeg), cy);
    return m;
}

int rast_invert(rast_mat m, rast_mat *out) {
    int64_t det = (int64_t)m.a * m.d - (int64_t)m.b * m.c;     /* 32.32 */
    if (det > -256 && det < 256) return 0;                    /* next to none */
    rast_mat r;
    /* (x * 2^32) / det, with x 16.16 and det 32.32, is 16.16. Multiplied
     * rather than shifted: a negative number shifted left is undefined. */
    const int64_t k = (int64_t)1 << 32;
    r.a = rast_clamp32(( (int64_t)m.d * k) / det);
    r.b = rast_clamp32((-(int64_t)m.b * k) / det);
    r.c = rast_clamp32((-(int64_t)m.c * k) / det);
    r.d = rast_clamp32(( (int64_t)m.a * k) / det);
    r.e = -(rast_mulfx(r.a, m.e) + rast_mulfx(r.c, m.f));
    r.f = -(rast_mulfx(r.b, m.e) + rast_mulfx(r.d, m.f));
    *out = r;
    return 1;
}

void rast_apply(const rast_mat *m, rast_fx x, rast_fx y, rast_fx *ox, rast_fx *oy) {
    *ox = rast_clamp32((((int64_t)m->a * x + (int64_t)m->c * y) >> 16) + m->e);
    *oy = rast_clamp32((((int64_t)m->b * x + (int64_t)m->d * y) >> 16) + m->f);
}

/* The square root of the determinant: exactly the scale for a uniform scale
 * or a rotation, and the geometric mean of the two stretches otherwise. */
rast_fx rast_mat_scale(const rast_mat *m) {
    int64_t det = (int64_t)m->a * m->d - (int64_t)m->b * m->c;
    if (det < 0) det = -det;
    return (rast_fx)rast_isqrt64((uint64_t)det);
}
