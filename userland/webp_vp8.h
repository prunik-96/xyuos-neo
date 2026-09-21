#ifndef WEBP_VP8_H
#define WEBP_VP8_H

/* VP8 -- the lossy half of WebP, which is a video keyframe.
 *
 * The shape of it: an arithmetic ("boolean") coder carries every syntax
 * element; each macroblock picks an intra prediction mode and is predicted
 * from the pixels above and to its left; the difference is carried as
 * quantised coefficients of a small integer transform; and a deblocking
 * filter runs over the finished frame to hide the block edges the quantiser
 * leaves behind.
 *
 * Only keyframes exist in a still image, so there is no motion compensation
 * here and no reference frames -- which removes perhaps half the format.
 *
 * The constants come from tools/genvp8.py, which lifts them out of RFC 6386
 * rather than having them retyped. Everything else is written from the
 * specification's own description, and checked against libwebp pixel for
 * pixel by tools/webpcheck.py -- an integer format has exactly one right
 * answer, so "close enough" is a bug that has not been found yet.
 */

#include "vp8_tables.h"

/* ==========================================================================
 * the boolean decoder
 * ========================================================================== */

typedef struct {
    const unsigned char *buf;
    unsigned long        len, pos;
    unsigned             value;
    int                  range, count;
} vp8_bool;

static unsigned vp8_byte(vp8_bool *d) {
    return d->pos < d->len ? d->buf[d->pos++] : 0;
}

static void vp8_bool_init(vp8_bool *d, const unsigned char *b, unsigned long n) {
    d->buf = b; d->len = n; d->pos = 0;
    d->range = 255;
    d->count = 0;
    d->value = (vp8_byte(d) << 8) | vp8_byte(d);
}

static int vp8_read(vp8_bool *d, int prob) {
    unsigned split = 1u + (unsigned)(((d->range - 1) * prob) >> 8);
    unsigned big = split << 8;
    int ret;
    if (d->value >= big) { ret = 1; d->range -= (int)split; d->value -= big; }
    else                 { ret = 0; d->range = (int)split; }
    while (d->range < 128) {
        d->value <<= 1;
        d->range <<= 1;
        if (++d->count == 8) { d->count = 0; d->value |= vp8_byte(d); }
    }
    return ret;
}

static int vp8_bit(vp8_bool *d) { return vp8_read(d, 128); }

static int vp8_literal(vp8_bool *d, int n) {
    int v = 0;
    while (n--) v = (v << 1) | vp8_bit(d);
    return v;
}

/* A value with its sign written after it, which is how the header carries
 * every delta. */
static int vp8_signed(vp8_bool *d, int n) {
    int v = vp8_literal(d, n);
    return vp8_bit(d) ? -v : v;
}

/* Walk a decision tree. Positive entries are the index of the next pair,
 * anything else is a leaf holding its own negation. `start` lets a caller
 * enter below the root, which is how the coefficient decoder skips the
 * end-of-block branch when it cannot occur. */
static int vp8_tree(vp8_bool *d, const signed char *t, const unsigned char *p,
                    int start) {
    int i = start;
    do { i = t[i + vp8_read(d, p[i >> 1])]; } while (i > 0);
    return -i;
}

/* ==========================================================================
 * the frame
 * ========================================================================== */

enum { VP8_DC_PRED = 0, VP8_V_PRED, VP8_H_PRED, VP8_TM_PRED, VP8_B_PRED };

#define VP8_MAX_PARTS 8

typedef struct {
    int  w, h, mb_w, mb_h;

    /* segmentation */
    int  seg_on, seg_update_map, seg_abs;
    int  seg_quant[4], seg_lf[4];
    unsigned char seg_prob[3];

    /* the loop filter */
    int  simple_filter, filter_level, sharpness;
    int  lf_delta_on;
    int  ref_lf_delta[4], mode_lf_delta[4];

    /* quantisation */
    int  qindex, dq_y_dc, dq_y2_dc, dq_y2_ac, dq_uv_dc, dq_uv_ac;

    unsigned char coeff_probs[4][8][3][11];
    int  use_skip, prob_skip;

    /* the residual partitions */
    vp8_bool part[VP8_MAX_PARTS];
    int      nparts;

    /* the planes, with a border so prediction never reads off the end */
    unsigned char *ybuf, *ubuf, *vbuf;
    int  ys, uvs;
    unsigned char *Y, *U, *V;

    /* what the loop filter needs to know about each macroblock */
    unsigned char *mb_level;    /* per macroblock */
    unsigned char *mb_inner;    /* filter the subblock edges too */

    /* nonzero-coefficient context */
    unsigned char *top_nz;      /* 9 per macroblock column */
    unsigned char  left_nz[9];

    /* the 4x4 mode of every subblock, for the mode contexts */
    unsigned char *top_bmode;   /* 4 per macroblock column */
    unsigned char  left_bmode[4];
} vp8_dec;

#define VP8_BORDER 16

static int vp8_clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* Six fractional bits, then clamped: the last step of the colour conversion
 * at the bottom of this file. */
static int vp8_clip6(int v) {
    v >>= 6;
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* ==========================================================================
 * the inverse transforms
 * ========================================================================== */

/* The Walsh-Hadamard transform that carries the sixteen luma DC values. */
static void vp8_iwht(const short *in, short *out) {
    int a1, b1, c1, d1, a2, b2, c2, d2;
    short tmp[16];
    for (int i = 0; i < 4; i++) {
        a1 = in[i + 0]  + in[i + 12];
        b1 = in[i + 4]  + in[i + 8];
        c1 = in[i + 4]  - in[i + 8];
        d1 = in[i + 0]  - in[i + 12];
        tmp[i + 0]  = (short)(a1 + b1);
        tmp[i + 4]  = (short)(c1 + d1);
        tmp[i + 8]  = (short)(a1 - b1);
        tmp[i + 12] = (short)(d1 - c1);
    }
    for (int i = 0; i < 4; i++) {
        const short *ip = tmp + i * 4;
        a1 = ip[0] + ip[3];
        b1 = ip[1] + ip[2];
        c1 = ip[1] - ip[2];
        d1 = ip[0] - ip[3];
        a2 = a1 + b1; b2 = c1 + d1; c2 = a1 - b1; d2 = d1 - c1;
        out[i * 4 + 0] = (short)((a2 + 3) >> 3);
        out[i * 4 + 1] = (short)((b2 + 3) >> 3);
        out[i * 4 + 2] = (short)((c2 + 3) >> 3);
        out[i * 4 + 3] = (short)((d2 + 3) >> 3);
    }
}

/* The 4x4 transform. The two constants are cos(pi/8)*sqrt(2)-1 and
 * sin(pi/8)*sqrt(2), in 16.16 -- fixed by the specification, so that every
 * decoder produces the same pixels rather than merely similar ones. */
#define VP8_COS 20091
#define VP8_SIN 35468

static void vp8_idct_add(unsigned char *dst, int stride, short *in) {
    short tmp[16];
    for (int i = 0; i < 4; i++) {
        int a1 = in[i + 0] + in[i + 8];
        int b1 = in[i + 0] - in[i + 8];
        int t1 = (in[i + 4] * VP8_SIN) >> 16;
        int t2 = in[i + 12] + ((in[i + 12] * VP8_COS) >> 16);
        int c1 = t1 - t2;
        t1 = in[i + 4] + ((in[i + 4] * VP8_COS) >> 16);
        t2 = (in[i + 12] * VP8_SIN) >> 16;
        int d1 = t1 + t2;
        tmp[i + 0]  = (short)(a1 + d1);
        tmp[i + 12] = (short)(a1 - d1);
        tmp[i + 4]  = (short)(b1 + c1);
        tmp[i + 8]  = (short)(b1 - c1);
    }
    for (int i = 0; i < 4; i++) {
        const short *ip = tmp + i * 4;
        int a1 = ip[0] + ip[2];
        int b1 = ip[0] - ip[2];
        int t1 = (ip[1] * VP8_SIN) >> 16;
        int t2 = ip[3] + ((ip[3] * VP8_COS) >> 16);
        int c1 = t1 - t2;
        t1 = ip[1] + ((ip[1] * VP8_COS) >> 16);
        t2 = (ip[3] * VP8_SIN) >> 16;
        int d1 = t1 + t2;
        unsigned char *row = dst + i * stride;
        row[0] = (unsigned char)vp8_clamp255(row[0] + ((a1 + d1 + 4) >> 3));
        row[3] = (unsigned char)vp8_clamp255(row[3] + ((a1 - d1 + 4) >> 3));
        row[1] = (unsigned char)vp8_clamp255(row[1] + ((b1 + c1 + 4) >> 3));
        row[2] = (unsigned char)vp8_clamp255(row[2] + ((b1 - c1 + 4) >> 3));
    }
}

/* ==========================================================================
 * intra prediction
 * ========================================================================== */

/* The four whole-block modes, used at 16x16 for luma and 8x8 for chroma.
 * `have_up` and `have_left` say whether the neighbours are real pixels or the
 * 127/129 borders, which changes what DC_PRED averages. */
static void vp8_pred_block(unsigned char *dst, int stride, int size, int mode,
                           int have_up, int have_left) {
    const unsigned char *A = dst - stride;
    int i, j;

    switch (mode) {
    case VP8_DC_PRED: {
        int sum = 0, count = 0;
        if (have_up)   { for (i = 0; i < size; i++) sum += A[i]; count += size; }
        if (have_left) { for (i = 0; i < size; i++) sum += dst[i * stride - 1]; count += size; }
        int dc = 128;
        if (count) {
            int shift = 0;
            for (int c = count; c > 1; c >>= 1) shift++;
            dc = (sum + (count >> 1)) >> shift;
        }
        for (j = 0; j < size; j++)
            for (i = 0; i < size; i++) dst[j * stride + i] = (unsigned char)dc;
        break;
    }
    case VP8_V_PRED:
        for (j = 0; j < size; j++)
            for (i = 0; i < size; i++) dst[j * stride + i] = A[i];
        break;
    case VP8_H_PRED:
        for (j = 0; j < size; j++) {
            unsigned char v = dst[j * stride - 1];
            for (i = 0; i < size; i++) dst[j * stride + i] = v;
        }
        break;
    default: {   /* TM_PRED: propagate the differences from the corner */
        int p = A[-1];
        for (j = 0; j < size; j++) {
            int l = dst[j * stride - 1];
            for (i = 0; i < size; i++)
                dst[j * stride + i] = (unsigned char)vp8_clamp255(l + A[i] - p);
        }
        break;
    }
    }
}

#define AVG3(x, y, z) (unsigned char)(((x) + 2 * (y) + (z) + 2) >> 2)
#define AVG2(x, y)    (unsigned char)(((x) + (y) + 1) >> 1)

/* One 4x4 subblock. `A` points at the eight pixels above (four of them above
 * and to the right) with A[-1] the corner, `L` at the four to the left. */
static void vp8_pred4(unsigned char *dst, int stride, int mode,
                      const unsigned char *A, const unsigned char *L) {
    unsigned char B[4][4];
    unsigned char E[9];
    E[0] = L[3]; E[1] = L[2]; E[2] = L[1]; E[3] = L[0];
    E[4] = A[-1];
    E[5] = A[0]; E[6] = A[1]; E[7] = A[2]; E[8] = A[3];

    switch (mode) {
    case 0: {           /* B_DC_PRED */
        int v = 4;
        for (int i = 0; i < 4; i++) v += A[i] + L[i];
        v >>= 3;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) B[r][c] = (unsigned char)v;
        break;
    }
    case 1:             /* B_TM_PRED */
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                B[r][c] = (unsigned char)vp8_clamp255(L[r] + A[c] - A[-1]);
        break;
    case 2:             /* B_VE_PRED: the smoothed row above */
        for (int c = 0; c < 4; c++) {
            unsigned char v = AVG3(A[c - 1], A[c], A[c + 1]);
            B[0][c] = B[1][c] = B[2][c] = B[3][c] = v;
        }
        break;
    case 3: {           /* B_HE_PRED: the smoothed column to the left */
        /* Each row is the average of three neighbours centred on it. The top
         * row reaches up to the corner pixel, and the bottom row has nothing
         * below it so it repeats L[3]. */
        unsigned char r0 = AVG3(A[-1], L[0], L[1]);
        unsigned char r1 = AVG3(L[0],  L[1], L[2]);
        unsigned char r2 = AVG3(L[1],  L[2], L[3]);
        unsigned char r3 = AVG3(L[2],  L[3], L[3]);
        unsigned char rows[4] = { r0, r1, r2, r3 };
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) B[r][c] = rows[r];
        break;
    }
    case 4:             /* B_LD_PRED: down and to the left */
        B[0][0] = AVG3(A[0], A[1], A[2]);
        B[0][1] = B[1][0] = AVG3(A[1], A[2], A[3]);
        B[0][2] = B[1][1] = B[2][0] = AVG3(A[2], A[3], A[4]);
        B[0][3] = B[1][2] = B[2][1] = B[3][0] = AVG3(A[3], A[4], A[5]);
        B[1][3] = B[2][2] = B[3][1] = AVG3(A[4], A[5], A[6]);
        B[2][3] = B[3][2] = AVG3(A[5], A[6], A[7]);
        B[3][3] = AVG3(A[6], A[7], A[7]);
        break;
    case 5:             /* B_RD_PRED: down and to the right */
        B[3][0] = AVG3(E[0], E[1], E[2]);
        B[3][1] = B[2][0] = AVG3(E[1], E[2], E[3]);
        B[3][2] = B[2][1] = B[1][0] = AVG3(E[2], E[3], E[4]);
        B[3][3] = B[2][2] = B[1][1] = B[0][0] = AVG3(E[3], E[4], E[5]);
        B[2][3] = B[1][2] = B[0][1] = AVG3(E[4], E[5], E[6]);
        B[1][3] = B[0][2] = AVG3(E[5], E[6], E[7]);
        B[0][3] = AVG3(E[6], E[7], E[8]);
        break;
    case 6:             /* B_VR_PRED */
        B[3][0] = AVG3(E[1], E[2], E[3]);
        B[2][0] = AVG3(E[2], E[3], E[4]);
        B[3][1] = B[1][0] = AVG3(E[3], E[4], E[5]);
        B[2][1] = B[0][0] = AVG2(E[4], E[5]);
        B[3][2] = B[1][1] = AVG3(E[4], E[5], E[6]);
        B[2][2] = B[0][1] = AVG2(E[5], E[6]);
        B[3][3] = B[1][2] = AVG3(E[5], E[6], E[7]);
        B[2][3] = B[0][2] = AVG2(E[6], E[7]);
        B[1][3] = AVG3(E[6], E[7], E[8]);
        B[0][3] = AVG2(E[7], E[8]);
        break;
    case 7:             /* B_VL_PRED */
        B[0][0] = AVG2(A[0], A[1]);
        B[1][0] = AVG3(A[0], A[1], A[2]);
        B[2][0] = B[0][1] = AVG2(A[1], A[2]);
        B[1][1] = B[3][0] = AVG3(A[1], A[2], A[3]);
        B[2][1] = B[0][2] = AVG2(A[2], A[3]);
        B[3][1] = B[1][2] = AVG3(A[2], A[3], A[4]);
        B[2][2] = B[0][3] = AVG2(A[3], A[4]);
        B[3][2] = B[1][3] = AVG3(A[3], A[4], A[5]);
        B[2][3] = AVG3(A[4], A[5], A[6]);
        B[3][3] = AVG3(A[5], A[6], A[7]);
        break;
    case 8:             /* B_HD_PRED */
        B[3][0] = AVG2(E[0], E[1]);
        B[3][1] = AVG3(E[0], E[1], E[2]);
        B[2][0] = B[3][2] = AVG2(E[1], E[2]);
        B[2][1] = B[3][3] = AVG3(E[1], E[2], E[3]);
        B[2][2] = B[1][0] = AVG2(E[2], E[3]);
        B[2][3] = B[1][1] = AVG3(E[2], E[3], E[4]);
        B[1][2] = B[0][0] = AVG2(E[3], E[4]);
        B[1][3] = B[0][1] = AVG3(E[3], E[4], E[5]);
        B[0][2] = AVG3(E[4], E[5], E[6]);
        B[0][3] = AVG3(E[5], E[6], E[7]);
        break;
    default:            /* B_HU_PRED */
        B[0][0] = AVG2(L[0], L[1]);
        B[0][1] = AVG3(L[0], L[1], L[2]);
        B[0][2] = B[1][0] = AVG2(L[1], L[2]);
        B[0][3] = B[1][1] = AVG3(L[1], L[2], L[3]);
        B[1][2] = B[2][0] = AVG2(L[2], L[3]);
        B[1][3] = B[2][1] = AVG3(L[2], L[3], L[3]);
        B[2][2] = B[2][3] = B[3][0] = B[3][1] = B[3][2] = B[3][3] = L[3];
        break;
    }

    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) dst[r * stride + c] = B[r][c];
}

/* ==========================================================================
 * coefficients
 * ========================================================================== */

static const unsigned char vp8_zigzag[16] = {
    0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};

static const short vp8_cat_base[6] = { 5, 7, 11, 19, 35, 67 };

static const unsigned char *vp8_cat_probs(int c) {
    switch (c) {
    case 0: return vp8_pcat1;
    case 1: return vp8_pcat2;
    case 2: return vp8_pcat3;
    case 3: return vp8_pcat4;
    case 4: return vp8_pcat5;
    default: return vp8_pcat6;
    }
}

/* One block of coefficients, dequantised into `out` in raster order.
 * Returns the number of coefficients decoded, which is what the neighbouring
 * blocks use as their context. */
static int vp8_coeffs(vp8_bool *d, short *out,
                      const unsigned char probs[8][3][11],
                      int type, int ctx, int dq_dc, int dq_ac) {
    int i = (type == 0) ? 1 : 0;      /* type 0 has its DC in the Y2 block */
    int last = 0;
    int skip_eob = 0;
    int c3 = ctx;

    while (i < 16) {
        const unsigned char *p = probs[vp8_coeff_bands[i]][c3];
        /* Entering the tree at node 2 skips the end-of-block branch, which
         * cannot follow a zero. */
        int tok = vp8_tree(d, vp8_coeff_tree, p, skip_eob ? 2 : 0);
        if (tok == 11) break;                    /* dct_eob */

        int v;
        if (tok == 0) {
            v = 0;
            c3 = 0;
            skip_eob = 1;
        } else {
            skip_eob = 0;
            if (tok <= 4) { v = tok; c3 = (tok == 1) ? 1 : 2; }
            else {
                int cat = tok - 5;
                const unsigned char *ep = vp8_cat_probs(cat);
                int extra = 0;
                for (int k = 0; ep[k]; k++) extra += extra + vp8_read(d, ep[k]);
                v = vp8_cat_base[cat] + extra;
                c3 = 2;
            }
            if (vp8_bit(d)) v = -v;
            last = i + 1;
        }
        out[vp8_zigzag[i]] = (short)(v * (i == 0 ? dq_dc : dq_ac));
        i++;
    }
    return last;
}

/* ==========================================================================
 * the loop filter
 * ========================================================================== */

static int vp8_c(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }
static int vp8_u2s(int v) { return v - 128; }
static int vp8_s2u(int v) { return vp8_c(v) + 128; }

/* The two-or-four tap adjustment both filters are built from. `step` is the
 * distance between the pixels, so the same code filters a vertical edge and a
 * horizontal one. */
static int vp8_adjust(int outer, unsigned char *P, int step) {
    int p1 = vp8_u2s(P[-2 * step]), p0 = vp8_u2s(P[-step]);
    int q0 = vp8_u2s(P[0]),         q1 = vp8_u2s(P[step]);
    int a = vp8_c((outer ? vp8_c(p1 - q1) : 0) + 3 * (q0 - p0));
    int b = vp8_c(a + 3) >> 3;
    a = vp8_c(a + 4) >> 3;
    P[0]     = (unsigned char)vp8_s2u(q0 - a);
    P[-step] = (unsigned char)vp8_s2u(p0 + b);
    return a;
}

static int vp8_absd(int a, int b) { return a > b ? a - b : b - a; }

static int vp8_needs_filter(const unsigned char *P, int step, int I, int E) {
    int p3 = P[-4*step], p2 = P[-3*step], p1 = P[-2*step], p0 = P[-step];
    int q0 = P[0], q1 = P[step], q2 = P[2*step], q3 = P[3*step];
    return (vp8_absd(p0, q0) * 2 + vp8_absd(p1, q1) / 2) <= E &&
           vp8_absd(p3, p2) <= I && vp8_absd(p2, p1) <= I &&
           vp8_absd(p1, p0) <= I && vp8_absd(q3, q2) <= I &&
           vp8_absd(q2, q1) <= I && vp8_absd(q1, q0) <= I;
}

static int vp8_hev(const unsigned char *P, int step, int t) {
    return vp8_absd(P[-2*step], P[-step]) > t || vp8_absd(P[step], P[0]) > t;
}

static void vp8_filter_sub(unsigned char *P, int step, int hevt, int I, int E) {
    if (!vp8_needs_filter(P, step, I, E)) return;
    int hv = vp8_hev(P, step, hevt);
    int a = (vp8_adjust(hv, P, step) + 1) >> 1;
    if (!hv) {
        P[step]      = (unsigned char)vp8_s2u(vp8_u2s(P[step]) - a);
        P[-2 * step] = (unsigned char)vp8_s2u(vp8_u2s(P[-2 * step]) + a);
    }
}

static void vp8_filter_mb(unsigned char *P, int step, int hevt, int I, int E) {
    if (!vp8_needs_filter(P, step, I, E)) return;
    if (vp8_hev(P, step, hevt)) { vp8_adjust(1, P, step); return; }

    int p2 = vp8_u2s(P[-3*step]), p1 = vp8_u2s(P[-2*step]), p0 = vp8_u2s(P[-step]);
    int q0 = vp8_u2s(P[0]), q1 = vp8_u2s(P[step]), q2 = vp8_u2s(P[2*step]);
    int w = vp8_c(vp8_c(p1 - q1) + 3 * (q0 - p0));
    int a = vp8_c((27 * w + 63) >> 7);
    P[0]       = (unsigned char)vp8_s2u(q0 - a);
    P[-step]   = (unsigned char)vp8_s2u(p0 + a);
    a = vp8_c((18 * w + 63) >> 7);
    P[step]    = (unsigned char)vp8_s2u(q1 - a);
    P[-2*step] = (unsigned char)vp8_s2u(p1 + a);
    a = vp8_c((9 * w + 63) >> 7);
    P[2*step]  = (unsigned char)vp8_s2u(q2 - a);
    P[-3*step] = (unsigned char)vp8_s2u(p2 + a);
}

/* The simple filter touches luma only, and only the two pixels either side. */
static void vp8_filter_simple(unsigned char *P, int step, int E) {
    int p1 = P[-2*step], p0 = P[-step], q0 = P[0], q1 = P[step];
    if ((vp8_absd(p0, q0) * 2 + vp8_absd(p1, q1) / 2) <= E)
        vp8_adjust(1, P, step);
}

static void vp8_loop_filter(vp8_dec *V) {
    if (V->filter_level == 0) return;

    for (int mb_y = 0; mb_y < V->mb_h; mb_y++) {
        for (int mb_x = 0; mb_x < V->mb_w; mb_x++) {
            int level = V->mb_level[mb_y * V->mb_w + mb_x];
            if (!level) continue;
            int inner = V->mb_inner[mb_y * V->mb_w + mb_x];

            int I = level;
            if (V->sharpness) {
                I >>= (V->sharpness > 4) ? 2 : 1;
                if (I > 9 - V->sharpness) I = 9 - V->sharpness;
            }
            if (!I) I = 1;
            int hevt = (level >= 40) ? 2 : (level >= 15) ? 1 : 0;
            int E_mb  = ((level + 2) * 2) + I;
            int E_sub = (level * 2) + I;

            unsigned char *y = V->Y + (long)mb_y * 16 * V->ys + mb_x * 16;
            unsigned char *u = V->U + (long)mb_y * 8 * V->uvs + mb_x * 8;
            unsigned char *v = V->V + (long)mb_y * 8 * V->uvs + mb_x * 8;

            if (V->simple_filter) {
                if (mb_x) for (int i = 0; i < 16; i++)
                    vp8_filter_simple(y + i * V->ys, 1, E_mb);
                if (inner) for (int c = 4; c < 16; c += 4)
                    for (int i = 0; i < 16; i++)
                        vp8_filter_simple(y + i * V->ys + c, 1, E_sub);
                if (mb_y) for (int i = 0; i < 16; i++)
                    vp8_filter_simple(y + i, V->ys, E_mb);
                if (inner) for (int r = 4; r < 16; r += 4)
                    for (int i = 0; i < 16; i++)
                        vp8_filter_simple(y + r * V->ys + i, V->ys, E_sub);
                continue;
            }

            /* left edge of the macroblock */
            if (mb_x) {
                for (int i = 0; i < 16; i++) vp8_filter_mb(y + i * V->ys, 1, hevt, I, E_mb);
                for (int i = 0; i < 8; i++) {
                    vp8_filter_mb(u + i * V->uvs, 1, hevt, I, E_mb);
                    vp8_filter_mb(v + i * V->uvs, 1, hevt, I, E_mb);
                }
            }
            /* the vertical edges inside it */
            if (inner) {
                for (int c = 4; c < 16; c += 4)
                    for (int i = 0; i < 16; i++)
                        vp8_filter_sub(y + i * V->ys + c, 1, hevt, I, E_sub);
                for (int i = 0; i < 8; i++) {
                    vp8_filter_sub(u + i * V->uvs + 4, 1, hevt, I, E_sub);
                    vp8_filter_sub(v + i * V->uvs + 4, 1, hevt, I, E_sub);
                }
            }
            /* top edge */
            if (mb_y) {
                for (int i = 0; i < 16; i++) vp8_filter_mb(y + i, V->ys, hevt, I, E_mb);
                for (int i = 0; i < 8; i++) {
                    vp8_filter_mb(u + i, V->uvs, hevt, I, E_mb);
                    vp8_filter_mb(v + i, V->uvs, hevt, I, E_mb);
                }
            }
            /* and the horizontal ones inside */
            if (inner) {
                for (int r = 4; r < 16; r += 4)
                    for (int i = 0; i < 16; i++)
                        vp8_filter_sub(y + r * V->ys + i, V->ys, hevt, I, E_sub);
                for (int i = 0; i < 8; i++) {
                    vp8_filter_sub(u + 4 * V->uvs + i, V->uvs, hevt, I, E_sub);
                    vp8_filter_sub(v + 4 * V->uvs + i, V->uvs, hevt, I, E_sub);
                }
            }
        }
    }
}

/* --- chroma at full resolution --------------------------------------------
 *
 * Each output pixel sits between four chroma samples and is weighted 9:3:3:1
 * towards the nearest of them. At the edges the missing samples are mirrored,
 * which the index clamping below does on its own.
 *
 * The bitstream does not define this step -- how to stretch chroma back is a
 * decoder's own business -- but the weights and the exact order of the two
 * roundings are the ones libwebp uses, so our pixels come out identical to
 * every other decoder's rather than merely similar.
 */
static int vp8_uv_sample(const unsigned char *P, int stride, int cw, int ch,
                         int X, int Y) {
    int x = (X + (X & 1)) >> 1, y = (Y + (Y & 1)) >> 1;
    int lc = x - 1, rc = x, tr = y - 1, br = y;
    if (lc < 0) lc = 0;
    if (tr < 0) tr = 0;
    if (rc > cw - 1) rc = cw - 1;
    if (br > ch - 1) br = ch - 1;

    int tl = P[(long)tr * stride + lc], t = P[(long)tr * stride + rc];
    int l  = P[(long)br * stride + lc], c = P[(long)br * stride + rc];

    int avg = tl + t + l + c + 8;
    int d12 = (avg + 2 * (t + l)) >> 3;      /* along one diagonal */
    int d03 = (avg + 2 * (tl + c)) >> 3;     /* along the other */

    if (Y & 1) return (X & 1) ? ((d12 + tl) >> 1) : ((d03 + t) >> 1);
    return (X & 1) ? ((d03 + l) >> 1) : ((d12 + c) >> 1);
}

/* ==========================================================================
 * the frame header
 * ========================================================================== */

static int vp8_header(vp8_dec *V, vp8_bool *b, const unsigned char *data,
                      unsigned long len, unsigned long part0_end) {
    vp8_literal(b, 1);            /* colour space   */
    vp8_literal(b, 1);            /* clamping type  */

    V->seg_on = vp8_bit(b);
    if (V->seg_on) {
        V->seg_update_map = vp8_bit(b);
        int update_data = vp8_bit(b);
        if (update_data) {
            V->seg_abs = vp8_bit(b);
            for (int i = 0; i < 4; i++) V->seg_quant[i] = vp8_bit(b) ? vp8_signed(b, 7) : 0;
            for (int i = 0; i < 4; i++) V->seg_lf[i]    = vp8_bit(b) ? vp8_signed(b, 6) : 0;
        }
        if (V->seg_update_map)
            for (int i = 0; i < 3; i++)
                V->seg_prob[i] = vp8_bit(b) ? (unsigned char)vp8_literal(b, 8) : 255;
    }

    V->simple_filter = vp8_bit(b);
    V->filter_level  = vp8_literal(b, 6);
    V->sharpness     = vp8_literal(b, 3);
    V->lf_delta_on   = vp8_bit(b);
    if (V->lf_delta_on && vp8_bit(b)) {
        for (int i = 0; i < 4; i++) if (vp8_bit(b)) V->ref_lf_delta[i]  = vp8_signed(b, 6);
        for (int i = 0; i < 4; i++) if (vp8_bit(b)) V->mode_lf_delta[i] = vp8_signed(b, 6);
    }

    /* The residuals live in their own partitions, whose sizes are written as
     * three-byte lengths just after the first one. */
    V->nparts = 1 << vp8_literal(b, 2);
    {
        const unsigned char *sizes = data + part0_end;
        unsigned long p = part0_end + (unsigned long)(V->nparts - 1) * 3;
        if (p > len) return 0;
        for (int i = 0; i < V->nparts; i++) {
            unsigned long sz;
            if (i < V->nparts - 1) {
                sz = (unsigned long)sizes[i*3] | ((unsigned long)sizes[i*3+1] << 8) |
                     ((unsigned long)sizes[i*3+2] << 16);
            } else {
                sz = len - p;
            }
            if (p + sz > len) sz = len - p;
            vp8_bool_init(&V->part[i], data + p, sz);
            p += sz;
        }
    }

    V->qindex   = vp8_literal(b, 7);
    V->dq_y_dc  = vp8_bit(b) ? vp8_signed(b, 4) : 0;
    V->dq_y2_dc = vp8_bit(b) ? vp8_signed(b, 4) : 0;
    V->dq_y2_ac = vp8_bit(b) ? vp8_signed(b, 4) : 0;
    V->dq_uv_dc = vp8_bit(b) ? vp8_signed(b, 4) : 0;
    V->dq_uv_ac = vp8_bit(b) ? vp8_signed(b, 4) : 0;

    vp8_bit(b);                   /* refresh_entropy_probs */

    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            for (int k = 0; k < 3; k++)
                for (int t = 0; t < 11; t++)
                    if (vp8_read(b, vp8_coeff_update_probs[i][j][k][t]))
                        V->coeff_probs[i][j][k][t] = (unsigned char)vp8_literal(b, 8);

    V->use_skip = vp8_bit(b);
    V->prob_skip = V->use_skip ? vp8_literal(b, 8) : 0;
    return 1;
}

/* ==========================================================================
 * the macroblocks
 * ========================================================================== */

static int vp8_qclamp(int v) { return v < 0 ? 0 : v > 127 ? 127 : v; }

static unsigned char *vp8_lossy_planes(vp8_dec *V) {
    V->ys  = V->mb_w * 16 + 2 * VP8_BORDER;
    V->uvs = V->mb_w * 8  + 2 * VP8_BORDER;
    long yh = (long)V->mb_h * 16 + 2 * VP8_BORDER;
    long ch = (long)V->mb_h * 8  + 2 * VP8_BORDER;

    V->ybuf = (unsigned char *)malloc((size_t)V->ys * yh);
    V->ubuf = (unsigned char *)malloc((size_t)V->uvs * ch);
    V->vbuf = (unsigned char *)malloc((size_t)V->uvs * ch);
    if (!V->ybuf || !V->ubuf || !V->vbuf) return 0;

    V->Y = V->ybuf + (long)VP8_BORDER * V->ys + VP8_BORDER;
    V->U = V->ubuf + (long)VP8_BORDER * V->uvs + VP8_BORDER;
    V->V = V->vbuf + (long)VP8_BORDER * V->uvs + VP8_BORDER;

    /* The row above the picture is 127 and the column to its left is 129.
     * Those two numbers are in the specification, and they are what makes the
     * prediction of the very first macroblock well defined. */
    memset(V->Y - V->ys - VP8_BORDER, 127, (size_t)V->ys);
    memset(V->U - V->uvs - VP8_BORDER, 127, (size_t)V->uvs);
    memset(V->V - V->uvs - VP8_BORDER, 127, (size_t)V->uvs);
    for (long y = 0; y < (long)V->mb_h * 16; y++) V->Y[y * V->ys - 1] = 129;
    for (long y = 0; y < (long)V->mb_h * 8; y++) {
        V->U[y * V->uvs - 1] = 129;
        V->V[y * V->uvs - 1] = 129;
    }
    return V->ybuf;
}

static unsigned *webp_lossy(const unsigned char *d, unsigned long n,
                            int *pw, int *ph) {
    if (n < 10) { img_err = "VP8 frame too short"; return 0; }

    unsigned tag = (unsigned)d[0] | ((unsigned)d[1] << 8) | ((unsigned)d[2] << 16);
    int keyframe = !(tag & 1);
    int show     = (tag >> 4) & 1;
    unsigned long part0 = (tag >> 5) & 0x7FFFF;
    (void)show;
    if (!keyframe) { img_err = "WebP frame is not a keyframe"; return 0; }
    if (d[3] != 0x9D || d[4] != 0x01 || d[5] != 0x2A) {
        img_err = "bad VP8 start code"; return 0;
    }

    vp8_dec V;
    memset(&V, 0, sizeof V);
    V.w = ((int)d[6] | ((int)d[7] << 8)) & 0x3FFF;
    V.h = ((int)d[8] | ((int)d[9] << 8)) & 0x3FFF;
    if (!img_dims_ok(V.w, V.h)) { img_err = "bad WebP size"; return 0; }
    V.mb_w = (V.w + 15) >> 4;
    V.mb_h = (V.h + 15) >> 4;

    unsigned long hdr = 10;
    if (hdr + part0 > n) { img_err = "truncated VP8 frame"; return 0; }

    memcpy(V.coeff_probs, vp8_default_coeff_probs, sizeof V.coeff_probs);
    for (int i = 0; i < 3; i++) V.seg_prob[i] = 255;

    vp8_bool b;
    vp8_bool_init(&b, d + hdr, part0);
    if (!vp8_header(&V, &b, d, n, hdr + part0)) {
        img_err = "bad VP8 header"; return 0;
    }

    if (!vp8_lossy_planes(&V)) {
        free(V.ybuf); free(V.ubuf); free(V.vbuf);
        img_err = "out of memory"; return 0;
    }
    V.mb_level = (unsigned char *)calloc((size_t)V.mb_w * V.mb_h, 1);
    V.mb_inner = (unsigned char *)calloc((size_t)V.mb_w * V.mb_h, 1);
    V.top_nz    = (unsigned char *)calloc((size_t)V.mb_w * 9, 1);
    V.top_bmode = (unsigned char *)calloc((size_t)V.mb_w * 4, 1);
    if (!V.mb_level || !V.mb_inner || !V.top_nz || !V.top_bmode) {
        free(V.ybuf); free(V.ubuf); free(V.vbuf);
        free(V.mb_level); free(V.mb_inner); free(V.top_nz); free(V.top_bmode);
        img_err = "out of memory"; return 0;
    }

    short coeffs[25][16];
    unsigned char bmodes[16];

    for (int mb_y = 0; mb_y < V.mb_h; mb_y++) {
        vp8_bool *res = &V.part[mb_y & (V.nparts - 1)];
        memset(V.left_nz, 0, sizeof V.left_nz);
        memset(V.left_bmode, 0, sizeof V.left_bmode);

        for (int mb_x = 0; mb_x < V.mb_w; mb_x++) {
            /* --- what kind of macroblock is this ------------------------- */
            int segment = 0;
            if (V.seg_on && V.seg_update_map)
                segment = vp8_tree(&b, vp8_segment_tree, V.seg_prob, 0);
            int skip = V.use_skip ? vp8_read(&b, V.prob_skip) : 0;

            int ymode = vp8_tree(&b, vp8_kf_ymode_tree, vp8_kf_ymode_prob, 0);
            if (ymode == VP8_B_PRED) {
                for (int j = 0; j < 4; j++) {
                    for (int i = 0; i < 4; i++) {
                        int above = (j == 0) ? V.top_bmode[mb_x * 4 + i]
                                             : bmodes[(j - 1) * 4 + i];
                        int left  = (i == 0) ? V.left_bmode[j]
                                             : bmodes[j * 4 + i - 1];
                        bmodes[j * 4 + i] = (unsigned char)
                            vp8_tree(&b, vp8_bmode_tree,
                                     vp8_kf_bmode_prob[above][left], 0);
                    }
                }
            } else {
                /* The whole-block modes have an equivalent 4x4 mode, which is
                 * what the neighbours see when they look this way. */
                static const unsigned char equiv[4] = { 0, 2, 3, 1 };
                unsigned char e = equiv[ymode];
                for (int i = 0; i < 16; i++) bmodes[i] = e;
            }
            for (int i = 0; i < 4; i++) {
                V.top_bmode[mb_x * 4 + i] = bmodes[12 + i];
                V.left_bmode[i] = bmodes[i * 4 + 3];
            }

            int uvmode = vp8_tree(&b, vp8_uv_mode_tree, vp8_kf_uv_mode_prob, 0);

            /* --- quantisers, possibly per segment ------------------------ */
            int q = V.qindex;
            if (V.seg_on) q = V.seg_abs ? V.seg_quant[segment]
                                        : q + V.seg_quant[segment];
            q = vp8_qclamp(q);
            int y_dc  = vp8_dc_qlookup[vp8_qclamp(q + V.dq_y_dc)];
            int y_ac  = vp8_ac_qlookup[q];
            int y2_dc = vp8_dc_qlookup[vp8_qclamp(q + V.dq_y2_dc)] * 2;
            int y2_ac = vp8_ac_qlookup[vp8_qclamp(q + V.dq_y2_ac)] * 155 / 100;
            if (y2_ac < 8) y2_ac = 8;
            int uv_dc = vp8_dc_qlookup[vp8_qclamp(q + V.dq_uv_dc)];
            if (uv_dc > 132) uv_dc = 132;
            int uv_ac = vp8_ac_qlookup[vp8_qclamp(q + V.dq_uv_ac)];

            /* --- the residuals -------------------------------------------- */
            memset(coeffs, 0, sizeof coeffs);
            int nonzero = 0;
            int has_y2 = (ymode != VP8_B_PRED);

            if (skip) {
                /* Nothing was written for this block, so every context it
                 * would have set becomes zero -- except Y2, which keeps its
                 * neighbour's value when the block has no Y2 at all. */
                memset(V.left_nz, 0, 8);
                memset(V.top_nz + mb_x * 9, 0, 8);
                if (has_y2) { V.left_nz[8] = 0; V.top_nz[mb_x * 9 + 8] = 0; }
            } else {
                unsigned char *tnz = V.top_nz + mb_x * 9;
                if (has_y2) {
                    int ctx = V.left_nz[8] + tnz[8];
                    int nz = vp8_coeffs(res, coeffs[24], V.coeff_probs[1],
                                        1, ctx, y2_dc, y2_ac);
                    V.left_nz[8] = tnz[8] = (unsigned char)(nz > 0);
                    if (nz) nonzero = 1;
                }
                int ytype = has_y2 ? 0 : 3;
                for (int j = 0; j < 4; j++) {
                    for (int i = 0; i < 4; i++) {
                        int ctx = V.left_nz[j] + tnz[i];
                        int nz = vp8_coeffs(res, coeffs[j * 4 + i],
                                            V.coeff_probs[ytype], ytype, ctx,
                                            y_dc, y_ac);
                        V.left_nz[j] = tnz[i] = (unsigned char)(nz > 0);
                        if (nz) nonzero = 1;
                    }
                }
                for (int pl = 0; pl < 2; pl++) {
                    for (int j = 0; j < 2; j++) {
                        for (int i = 0; i < 2; i++) {
                            int ctx = V.left_nz[4 + pl * 2 + j] + tnz[4 + pl * 2 + i];
                            int nz = vp8_coeffs(res, coeffs[16 + pl * 4 + j * 2 + i],
                                                V.coeff_probs[2], 2, ctx,
                                                uv_dc, uv_ac);
                            V.left_nz[4 + pl * 2 + j] = tnz[4 + pl * 2 + i] =
                                (unsigned char)(nz > 0);
                            if (nz) nonzero = 1;
                        }
                    }
                }
            }

            /* The Y2 block holds the sixteen luma DC values, transformed
             * again; undoing that puts each one back where it belongs. */
            if (has_y2 && !skip) {
                short dc[16];
                vp8_iwht(coeffs[24], dc);
                for (int i = 0; i < 16; i++) coeffs[i][0] = dc[i];
            }

            /* --- predict, then add the residual --------------------------- */
            unsigned char *y = V.Y + (long)mb_y * 16 * V.ys + mb_x * 16;
            unsigned char *u = V.U + (long)mb_y * 8 * V.uvs + mb_x * 8;
            unsigned char *v = V.V + (long)mb_y * 8 * V.uvs + mb_x * 8;

            if (ymode == VP8_B_PRED) {
                /* Every right-edge subblock uses the same four pixels above
                 * and to the right of the macroblock, whatever has been
                 * reconstructed since. */
                unsigned char tr[4];
                for (int i = 0; i < 4; i++) tr[i] = y[-V.ys + 16 + i];

                for (int j = 0; j < 4; j++) {
                    for (int i = 0; i < 4; i++) {
                        unsigned char *dst = y + j * 4 * V.ys + i * 4;
                        unsigned char A[9], L[4];
                        A[0] = dst[-V.ys - 1];        /* the corner, at A[-1] */
                        for (int k = 0; k < 4; k++) A[1 + k] = dst[-V.ys + k];
                        if (i == 3) for (int k = 0; k < 4; k++) A[5 + k] = tr[k];
                        else        for (int k = 0; k < 4; k++) A[5 + k] = dst[-V.ys + 4 + k];
                        for (int k = 0; k < 4; k++) L[k] = dst[k * V.ys - 1];
                        vp8_pred4(dst, V.ys, bmodes[j * 4 + i], A + 1, L);

                        short *c = coeffs[j * 4 + i];
                        vp8_idct_add(dst, V.ys, c);
                    }
                }
            } else {
                vp8_pred_block(y, V.ys, 16, ymode, mb_y > 0, mb_x > 0);
                for (int j = 0; j < 4; j++)
                    for (int i = 0; i < 4; i++)
                        vp8_idct_add(y + j * 4 * V.ys + i * 4, V.ys,
                                     coeffs[j * 4 + i]);
            }

            vp8_pred_block(u, V.uvs, 8, uvmode, mb_y > 0, mb_x > 0);
            vp8_pred_block(v, V.uvs, 8, uvmode, mb_y > 0, mb_x > 0);
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    vp8_idct_add(u + j * 4 * V.uvs + i * 4, V.uvs, coeffs[16 + j * 2 + i]);
                    vp8_idct_add(v + j * 4 * V.uvs + i * 4, V.uvs, coeffs[20 + j * 2 + i]);
                }

            /* --- what the loop filter will need --------------------------- */
            {
                int level = V.filter_level;
                if (V.seg_on) level = V.seg_abs ? V.seg_lf[segment]
                                                : level + V.seg_lf[segment];
                if (V.lf_delta_on) {
                    level += V.ref_lf_delta[0];             /* intra */
                    if (ymode == VP8_B_PRED) level += V.mode_lf_delta[0];
                }
                if (level < 0) level = 0;
                if (level > 63) level = 63;
                V.mb_level[mb_y * V.mb_w + mb_x] = (unsigned char)level;
                /* The specification skips the interior edges of a block that
                 * carried no coefficients -- there are no block boundaries
                 * inside it to hide -- unless it was predicted in 4x4 pieces,
                 * which have edges of their own regardless. */
                int had_coeffs = !skip && nonzero;
                V.mb_inner[mb_y * V.mb_w + mb_x] =
                    (unsigned char)(had_coeffs || ymode == VP8_B_PRED);
            }
        }

        /* The four pixels past the right edge of this row are what the
         * rightmost macroblock of the NEXT row predicts from. */
        for (int i = 0; i < 4; i++) {
            long last = (long)(mb_y * 16 + 15) * V.ys;
            V.Y[last + V.mb_w * 16 + i] = V.Y[last + V.mb_w * 16 - 1];
        }
    }

#ifndef VP8_NO_LOOP_FILTER
    vp8_loop_filter(&V);
#else
    (void)vp8_loop_filter;
#endif

    /* --- YCbCr to RGB ------------------------------------------------------
     * BT.601, in the exact fixed-point form WebP decoders use: the products
     * are taken to 8 bits, summed with the offsets that account for the 16..235
     * range the values live in, and only then shifted down by six. Rounding at
     * one place rather than three is what makes every decoder agree on the
     * last bit. */
    unsigned *px = (unsigned *)malloc((size_t)V.w * V.h * sizeof(unsigned));
    if (!px) {
        free(V.ybuf); free(V.ubuf); free(V.vbuf);
        free(V.mb_level); free(V.mb_inner); free(V.top_nz); free(V.top_bmode);
        img_err = "out of memory"; return 0;
    }
    int cw = (V.w + 1) >> 1, ch = (V.h + 1) >> 1;
    for (int j = 0; j < V.h; j++) {
        const unsigned char *yr = V.Y + (long)j * V.ys;
        unsigned *out = px + (long)j * V.w;
        for (int i = 0; i < V.w; i++) {
            int Yv = yr[i];
            int Cb = vp8_uv_sample(V.U, V.uvs, cw, ch, i, j);
            int Cr = vp8_uv_sample(V.V, V.uvs, cw, ch, i, j);
            int t  = (Yv * 19077) >> 8;
            int r  = t + ((Cr * 26149) >> 8) - 14234;
            int g  = t - ((Cb *  6419) >> 8) - ((Cr * 13320) >> 8) + 8708;
            int bl = t + ((Cb * 33050) >> 8) - 17685;
            out[i] = 0xFF000000u | ((unsigned)vp8_clip6(r) << 16) |
                     ((unsigned)vp8_clip6(g) << 8) | (unsigned)vp8_clip6(bl);
        }
    }

    free(V.ybuf); free(V.ubuf); free(V.vbuf);
    free(V.mb_level); free(V.mb_inner); free(V.top_nz); free(V.top_bmode);

    *pw = V.w; *ph = V.h;
    return px;
}

/* ==========================================================================
 * the separate alpha plane
 * ========================================================================== */

/* An extended WebP may carry alpha beside the lossy image, either as raw
 * bytes or as a lossless image whose green channel holds the values. Either
 * way a filter may have been applied along the rows, which is undone here. */
static void webp_apply_alpha(unsigned *px, int w, int h,
                             const unsigned char *d, unsigned long n) {
    if (n < 1) return;
    int method = d[0] & 3;
    int filter = (d[0] >> 2) & 3;

    unsigned char *a = (unsigned char *)malloc((size_t)w * h);
    if (!a) return;

    if (method == 0) {
        if (n - 1 < (unsigned long)w * h) { free(a); return; }
        memcpy(a, d + 1, (size_t)w * h);
    } else if (method == 1) {
        wl_br b;
        wl_init(&b, d + 1, n - 1);
        int gw = w;
        unsigned *img = wl_image(&b, w, h, 1, &gw);
        if (!img) { free(a); return; }
        for (long i = 0; i < (long)w * h; i++) a[i] = (unsigned char)((img[i] >> 8) & 0xFF);
        free(img);
    } else {
        free(a);
        return;
    }

    if (filter) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                if (!x && !y) continue;
                int left = x ? a[(long)y * w + x - 1] : a[(long)(y - 1) * w];
                int up   = y ? a[(long)(y - 1) * w + x] : a[(long)y * w + x - 1];
                int p;
                if (filter == 1) p = left;
                else if (filter == 2) p = up;
                else {
                    int ul = (x && y) ? a[(long)(y - 1) * w + x - 1]
                                      : (y ? up : left);
                    p = left + up - ul;
                    if (p < 0) p = 0;
                    if (p > 255) p = 255;
                }
                a[(long)y * w + x] = (unsigned char)((a[(long)y * w + x] + p) & 0xFF);
            }
        }
    }

    for (long i = 0; i < (long)w * h; i++)
        px[i] = (px[i] & 0xFFFFFF) | ((unsigned)a[i] << 24);
    free(a);
}

#endif /* WEBP_VP8_H */
