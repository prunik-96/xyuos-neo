#ifndef IMG_H
#define IMG_H

/* Image decoding for xyuOS Neo: PNG, JPEG, BMP, WebP and SVG, from the file
 * format up.
 *
 * Nothing here is vendored. That is not purity for its own sake -- the two
 * interesting formats are interesting for opposite reasons, and writing them
 * out is the only way the system ends up owning them:
 *
 *   PNG is a container problem. The pixels are just DEFLATE, so most of the
 *   work is a correct RFC 1951 inflate plus the five row filters that make
 *   the data compressible in the first place.
 *
 *   JPEG is a signal problem. Entropy-coded coefficients come out of a
 *   Huffman stream in zig-zag order, get scaled by a quantisation table, and
 *   turn back into pixels through an inverse DCT.
 *
 * The IDCT here is the plain separable definition against a fixed-point
 * cosine table, not one of the clever factorisations. It costs about eight
 * times the multiplies of a Loeffler butterfly and it is worth it: it is
 * correct by construction, and decoding a photo is already dominated by the
 * Huffman bit loop.
 *
*   WebP is two formats behind one name. The lossless one is a Huffman and
 *   back-reference scheme with four reversible transforms layered on top; the
 *   lossy one is a video keyframe, and is decoded as such -- an arithmetic
 *   coder, intra prediction, and an inverse transform per block. Both live in
 *   webp.h.
 *
 *   SVG is not pixels at all but a drawing, so it gets a rasteriser rather
 *   than a decoder. That is in svg.h.
 *
 * All integer -- these programs build under -mgeneral-regs-only. */

#include <stdlib.h>
#include <string.h>

typedef struct {
    int           w, h;
    unsigned int *px;        /* ARGB 0x00RRGGBB, w*h */
} image_t;

static const char *img_err = "";

static void img_free(image_t *im) {
    free(im->px);
    im->px = 0;
    im->w = im->h = 0;
}

/* Refuse absurd dimensions before allocating: a corrupt header should fail,
 * not take the machine down with it. */
#define IMG_MAX_DIM   16384
#define IMG_MAX_PIXEL (40u * 1024u * 1024u)

/* What transparency is flattened onto.
 *
 * The surface has no alpha channel, so a picture that has one has to be laid
 * on something. White is the safe default, and it is also wrong half the time:
 * a site's logo is very often white itself, drawn on the dark panel the site
 * would have painted behind it -- flatten that onto white and the logo simply
 * disappears. A caller that knows the colour behind the picture should say so
 * before decoding. */
static unsigned img_background = 0xFFFFFF;

static unsigned img_flatten_px(unsigned r, unsigned g, unsigned b, unsigned a) {
    if (a != 255) {
        unsigned br = (img_background >> 16) & 0xFF;
        unsigned bg = (img_background >> 8) & 0xFF;
        unsigned bb = img_background & 0xFF;
        r = (r * a + br * (255 - a)) / 255;
        g = (g * a + bg * (255 - a)) / 255;
        b = (b * a + bb * (255 - a)) / 255;
    }
    return (r << 16) | (g << 8) | b;
}

static int img_dims_ok(long w, long h) {
    if (w <= 0 || h <= 0 || w > IMG_MAX_DIM || h > IMG_MAX_DIM) return 0;
    if ((unsigned long)w * (unsigned long)h > IMG_MAX_PIXEL) return 0;
    return 1;
}

#include "inflate.h"

/* ==========================================================================
 * PNG
 * ========================================================================== */

static unsigned png_be32(const unsigned char *p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8) | (unsigned)p[3];
}

/* Extract sample `i` of a row stored at `depth` bits per sample. */
static unsigned png_sample(const unsigned char *row, int i, int depth) {
    if (depth == 8)  return row[i];
    if (depth == 16) return row[i * 2];                 /* keep the high byte */
    int per = 8 / depth;
    unsigned b = row[i / per];
    int shift = 8 - depth * (i % per + 1);
    return (b >> shift) & (unsigned)((1 << depth) - 1);
}

static int png_decode(const unsigned char *d, unsigned long n, image_t *out) {
    static const unsigned char sig[8] = { 137,80,78,71,13,10,26,10 };
    if (n < 8 || memcmp(d, sig, 8) != 0) { img_err = "not a PNG"; return 0; }

    unsigned long p = 8;
    int w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
    unsigned char pal[256][3];
    unsigned char palA[256];
    int npal = 0;
    for (int i = 0; i < 256; i++) palA[i] = 255;

    unsigned char *idat = 0;
    unsigned long idat_len = 0, idat_cap = 0;

    while (p + 8 <= n) {
        unsigned long clen = png_be32(d + p);
        const unsigned char *type = d + p + 4;
        const unsigned char *data = d + p + 8;
        if (p + 12 + clen > n) break;

        if (memcmp(type, "IHDR", 4) == 0 && clen >= 13) {
            w = (int)png_be32(data);
            h = (int)png_be32(data + 4);
            depth = data[8];
            ctype = data[9];
            interlace = data[12];
        } else if (memcmp(type, "PLTE", 4) == 0) {
            npal = (int)(clen / 3);
            if (npal > 256) npal = 256;
            for (int i = 0; i < npal; i++) {
                pal[i][0] = data[i * 3];
                pal[i][1] = data[i * 3 + 1];
                pal[i][2] = data[i * 3 + 2];
            }
        } else if (memcmp(type, "tRNS", 4) == 0 && ctype == 3) {
            for (unsigned long i = 0; i < clen && i < 256; i++) palA[i] = data[i];
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (idat_len + clen > idat_cap) {
                unsigned long nc = idat_cap ? idat_cap * 2 : 65536;
                while (nc < idat_len + clen) nc *= 2;
                unsigned char *nn = (unsigned char *)realloc(idat, nc);
                if (!nn) { free(idat); img_err = "out of memory"; return 0; }
                idat = nn;
                idat_cap = nc;
            }
            memcpy(idat + idat_len, data, clen);
            idat_len += clen;
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        p += 12 + clen;
    }

    if (!img_dims_ok(w, h)) { free(idat); img_err = "bad PNG size"; return 0; }
    if (!idat_len)          { free(idat); img_err = "PNG has no data"; return 0; }
    if (interlace)          { free(idat); img_err = "interlaced PNG unsupported"; return 0; }
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) {
        free(idat); img_err = "bad PNG bit depth"; return 0;
    }

    int chan;
    switch (ctype) {
        case 0: chan = 1; break;   /* grey        */
        case 2: chan = 3; break;   /* truecolour  */
        case 3: chan = 1; break;   /* palette     */
        case 4: chan = 2; break;   /* grey+alpha  */
        case 6: chan = 4; break;   /* RGBA        */
        default: free(idat); img_err = "bad PNG colour type"; return 0;
    }
    if (ctype == 3 && npal == 0) { free(idat); img_err = "PNG palette missing"; return 0; }

    /* The zlib wrapper is 2 header bytes and a 4-byte Adler checksum. */
    unsigned long rawlen = 0;
    unsigned char *raw = inflate_raw(idat + 2, idat_len - 2, &rawlen);
    free(idat);
    if (!raw) { img_err = "PNG stream corrupt"; return 0; }

    int bpp = (chan * depth + 7) / 8;             /* filter unit, >= 1 */
    unsigned long stride = ((unsigned long)w * chan * depth + 7) / 8;
    if (rawlen < (stride + 1) * (unsigned long)h) {
        free(raw);
        img_err = "PNG truncated";
        return 0;
    }

    unsigned char *lines = (unsigned char *)malloc(stride * (unsigned long)h);
    if (!lines) { free(raw); img_err = "out of memory"; return 0; }

    /* Undo the per-row filters. Each row predicts from the pixel to its left
     * (a), the row above (b) and above-left (c). */
    for (int y = 0; y < h; y++) {
        const unsigned char *src = raw + (stride + 1) * (unsigned long)y;
        int filter = src[0];
        src++;
        unsigned char *cur = lines + stride * (unsigned long)y;
        const unsigned char *up = y ? lines + stride * (unsigned long)(y - 1) : 0;

        for (unsigned long i = 0; i < stride; i++) {
            int a = (i >= (unsigned long)bpp) ? cur[i - bpp] : 0;
            int b = up ? up[i] : 0;
            int c = (up && i >= (unsigned long)bpp) ? up[i - bpp] : 0;
            int v = src[i];
            switch (filter) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: {
                    int pp = a + b - c;
                    int pa = pp > a ? pp - a : a - pp;
                    int pb = pp > b ? pp - b : b - pp;
                    int pc = pp > c ? pp - c : c - pp;
                    v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    break;
                }
                default: break;
            }
            cur[i] = (unsigned char)v;
        }
    }
    free(raw);

    unsigned int *px = (unsigned int *)malloc((unsigned long)w * h * 4);
    if (!px) { free(lines); img_err = "out of memory"; return 0; }

    int maxv = (1 << depth) - 1;
    if (depth == 16) maxv = 255;

    for (int y = 0; y < h; y++) {
        const unsigned char *row = lines + stride * (unsigned long)y;
        for (int x = 0; x < w; x++) {
            unsigned r, g, b, a = 255;
            if (ctype == 3) {
                unsigned idx = png_sample(row, x, depth);
                if ((int)idx >= npal) idx = 0;
                r = pal[idx][0]; g = pal[idx][1]; b = pal[idx][2];
                a = palA[idx];
            } else if (ctype == 0 || ctype == 4) {
                unsigned v = png_sample(row, x * chan, depth);
                if (depth < 8) v = v * 255 / (unsigned)maxv;
                r = g = b = v;
                if (ctype == 4) a = png_sample(row, x * chan + 1, depth);
            } else {
                r = png_sample(row, x * chan + 0, depth);
                g = png_sample(row, x * chan + 1, depth);
                b = png_sample(row, x * chan + 2, depth);
                if (ctype == 6) a = png_sample(row, x * chan + 3, depth);
            }
            /* The surface has no alpha channel, so this lands on whatever
             * the caller said was behind it. */
            px[(unsigned long)y * w + x] = img_flatten_px(r, g, b, a);
        }
    }
    free(lines);

    out->w = w;
    out->h = h;
    out->px = px;
    return 1;
}

/* ==========================================================================
 * JPEG (baseline sequential)
 * ========================================================================== */

/* T[u][x] = C(u) * cos((2x+1) u pi / 16) * 4096, C(0) = 1/sqrt(2).
 *
 * The 1/2 in front of each 1-D IDCT is NOT in the table -- it is paid for by
 * the shifts below, two of them, one per pass. Getting that wrong scales the
 * whole image by four, which flat blocks and hard edges both survive (one has
 * no AC energy, the other clips back to the right answer) and every gradient
 * does not. */
static const int idct_tab[8][8] = {
    {   2896,   2896,   2896,   2896,   2896,   2896,   2896,   2896 },
    {   4017,   3406,   2276,    799,   -799,  -2276,  -3406,  -4017 },
    {   3784,   1567,  -1567,  -3784,  -3784,  -1567,   1567,   3784 },
    {   3406,   -799,  -4017,  -2276,   2276,   4017,    799,  -3406 },
    {   2896,  -2896,  -2896,   2896,   2896,  -2896,  -2896,   2896 },
    {   2276,  -4017,    799,   3406,  -3406,   -799,   4017,  -2276 },
    {   1567,  -3784,   3784,  -1567,  -1567,   3784,  -3784,   1567 },
    {    799,  -2276,   3406,  -4017,   4017,  -3406,   2276,   -799 },
};

static const unsigned char jz[64] = {
     0, 1, 8,16, 9, 2, 3,10, 17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34, 27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36, 29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46, 53,60,61,54,47,55,62,63 };

/* The accumulators are 64-bit on purpose. A dequantised coefficient is
 * bounded by 2047*255, and eight of those against a 4096-scaled cosine
 * overflow a 32-bit sum -- rarely, only on blocks with a hard edge in them,
 * which is exactly the case where the wraparound is most visible. */
static void idct8x8(const int *in, unsigned char *out, int outstride) {
    int tmp[64];
    for (int y = 0; y < 8; y++) {                     /* rows */
        const int *r = in + y * 8;
        for (int x = 0; x < 8; x++) {
            long long s = 0;
            for (int u = 0; u < 8; u++) s += (long long)r[u] * idct_tab[u][x];
            tmp[y * 8 + x] = (int)((s + 128) >> 8);   /* keep 4 spare bits */
        }
    }
    for (int x = 0; x < 8; x++) {                     /* columns */
        for (int y = 0; y < 8; y++) {
            long long s = 0;
            for (int v = 0; v < 8; v++) s += (long long)tmp[v * 8 + x] * idct_tab[v][y];
            /* 18 = 8 (undone from the row pass) + 12 + 12 (the two table
              * scales) - 2 - 12, i.e. both 1/2 factors land here. */
            int p = (int)((s + (1 << 17)) >> 18) + 128;
            out[y * outstride + x] = (unsigned char)(p < 0 ? 0 : (p > 255 ? 255 : p));
        }
    }
}

typedef struct {
    const unsigned char *d;
    unsigned long        len, pos;
    unsigned             buf;
    int                  cnt;
    int                  stop;      /* hit a marker or ran out */
} jbits;

static int jb_bit(jbits *b) {
    if (b->cnt == 0) {
        if (b->stop || b->pos >= b->len) { b->stop = 1; return 0; }
        unsigned char c = b->d[b->pos++];
        if (c == 0xFF) {
            unsigned char nx = (b->pos < b->len) ? b->d[b->pos] : 0xD9;
            if (nx == 0x00) b->pos++;         /* stuffed byte */
            else { b->stop = 1; b->pos--; return 0; }
        }
        b->buf = c;
        b->cnt = 8;
    }
    b->cnt--;
    return (int)((b->buf >> b->cnt) & 1u);
}

static int jb_receive(jbits *b, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | jb_bit(b);
    return v;
}

/* Sign-extend an n-bit magnitude the way JPEG stores differences. */
static int jb_extend(int v, int n) {
    return (n && v < (1 << (n - 1))) ? v - (1 << n) + 1 : v;
}

typedef struct {
    unsigned char bits[17];
    unsigned char vals[256];
    int  mincode[17], maxcode[18], valptr[17];
    int  present;
} jhuff;

static void jhuff_build(jhuff *t) {
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        t->valptr[l] = k;
        t->mincode[l] = code;
        code += t->bits[l];
        k += t->bits[l];
        t->maxcode[l] = t->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    t->maxcode[17] = 0x7FFFFFFF;
    t->present = 1;
}

static int jhuff_decode(jbits *b, const jhuff *t) {
    int code = jb_bit(b), l = 1;
    while (code > t->maxcode[l]) {
        code = (code << 1) | jb_bit(b);
        if (++l > 16) return 0;
    }
    int i = t->valptr[l] + code - t->mincode[l];
    return (i >= 0 && i < 256) ? t->vals[i] : 0;
}

typedef struct {
    int id, hs, vs, tq;
    int td, ta;
    int pred;
    int bw, bh;               /* plane size in blocks */
    int cw, ch;               /* live pixels; the rest is block padding */
    unsigned char *plane;
    int stride;
} jcomp;

/* Sample a component plane at a 1/256-pixel position, bilinearly.
 *
 * Nearest-neighbour is a lot simpler and looks it: on 4:2:0, which is what
 * nearly every photograph in the world uses, each chroma sample covers a 2x2
 * block of pixels, and picking one of them puts a visible coloured staircase
 * along every sharp colour edge. Interpolating instead is what libjpeg calls
 * "fancy upsampling", and it is a handful of multiplies per pixel. */
static int jsample(const jcomp *c, int xf, int yf) {
    int x0 = xf >> 8, y0 = yf >> 8;
    int fx = xf & 255, fy = yf & 255;
    int x1 = x0 + 1, y1 = y0 + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x0 > c->cw - 1) x0 = c->cw - 1;
    if (x1 > c->cw - 1) x1 = c->cw - 1;
    if (y0 > c->ch - 1) y0 = c->ch - 1;
    if (y1 > c->ch - 1) y1 = c->ch - 1;

    const unsigned char *r0 = c->plane + (unsigned long)y0 * c->stride;
    const unsigned char *r1 = c->plane + (unsigned long)y1 * c->stride;
    int top = r0[x0] * (256 - fx) + r0[x1] * fx;
    int bot = r1[x0] * (256 - fx) + r1[x1] * fx;
    return (top * (256 - fy) + bot * fy + 32768) >> 16;
}

/* Where output pixel `p` falls in a plane subsampled by samp/max, in 1/256
 * of a plane pixel. The half-pixel offsets put sample centres, not corners,
 * in correspondence -- without them the chroma drifts half a pixel. */
static int jmap(int p, int samp, int max) {
    return ((2 * p + 1) * samp * 128) / max - 128;
}

static int jpeg_decode(const unsigned char *d, unsigned long n, image_t *out) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) { img_err = "not a JPEG"; return 0; }

    unsigned short qt[4][64];
    jhuff hdc[4], hac[4];
    memset(qt, 0, sizeof qt);
    memset(hdc, 0, sizeof hdc);
    memset(hac, 0, sizeof hac);

    jcomp comp[4];
    memset(comp, 0, sizeof comp);
    int ncomp = 0, width = 0, height = 0, restart = 0;
    int ok = 0;

    unsigned long p = 2;
    while (p + 4 <= n) {
        if (d[p] != 0xFF) { p++; continue; }
        unsigned char m = d[p + 1];
        p += 2;
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (m == 0xD9) break;
        if (p + 2 > n) break;
        unsigned long seglen = ((unsigned long)d[p] << 8) | d[p + 1];
        const unsigned char *seg = d + p + 2;
        unsigned long slen = seglen >= 2 ? seglen - 2 : 0;
        if (p + seglen > n) break;

        if (m == 0xDB) {                                     /* quant tables */
            unsigned long i = 0;
            while (i < slen) {
                int prec = seg[i] >> 4, id = seg[i] & 15;
                i++;
                if (id > 3) break;
                for (int k = 0; k < 64 && i < slen; k++) {
                    qt[id][k] = prec ? (unsigned short)((seg[i] << 8) | seg[i + 1])
                                     : (unsigned short)seg[i];
                    i += prec ? 2 : 1;
                }
            }
        } else if (m == 0xC4) {                              /* Huffman tables */
            unsigned long i = 0;
            while (i + 17 <= slen) {
                int cls = seg[i] >> 4, id = seg[i] & 15;
                i++;
                if (id > 3) break;
                jhuff *t = cls ? &hac[id] : &hdc[id];
                int total = 0;
                t->bits[0] = 0;
                for (int l = 1; l <= 16; l++) { t->bits[l] = seg[i + l - 1]; total += t->bits[l]; }
                i += 16;
                if (total > 256 || i + (unsigned long)total > slen) break;
                for (int k = 0; k < total; k++) t->vals[k] = seg[i + k];
                i += (unsigned long)total;
                jhuff_build(t);
            }
        } else if (m == 0xDD) {                              /* restart interval */
            if (slen >= 2) restart = (seg[0] << 8) | seg[1];
        } else if (m == 0xC0 || m == 0xC1) {                 /* baseline frame */
            if (slen < 6) break;
            height = (seg[1] << 8) | seg[2];
            width  = (seg[3] << 8) | seg[4];
            ncomp  = seg[5];
            if (ncomp > 4 || ncomp < 1) { img_err = "odd JPEG component count"; return 0; }
            for (int c = 0; c < ncomp; c++) {
                comp[c].id = seg[6 + c * 3];
                comp[c].hs = seg[7 + c * 3] >> 4;
                comp[c].vs = seg[7 + c * 3] & 15;
                comp[c].tq = seg[8 + c * 3];
                if (comp[c].hs < 1 || comp[c].vs < 1) { img_err = "bad JPEG sampling"; return 0; }
            }
        } else if (m == 0xC2) {
            img_err = "progressive JPEG unsupported";
            return 0;
        } else if (m >= 0xC3 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            img_err = "unsupported JPEG variant";
            return 0;
        } else if (m == 0xDA) {                              /* start of scan */
            if (!width || !height || !ncomp) { img_err = "JPEG header missing"; return 0; }
            if (!img_dims_ok(width, height)) { img_err = "bad JPEG size"; return 0; }

            int ns = seg[0];
            for (int i = 0; i < ns; i++) {
                int cid = seg[1 + i * 2], tt = seg[2 + i * 2];
                for (int c = 0; c < ncomp; c++)
                    if (comp[c].id == cid) { comp[c].td = tt >> 4; comp[c].ta = tt & 15; }
            }

            int hmax = 1, vmax = 1;
            for (int c = 0; c < ncomp; c++) {
                if (comp[c].hs > hmax) hmax = comp[c].hs;
                if (comp[c].vs > vmax) vmax = comp[c].vs;
            }
            int mcux = (width + 8 * hmax - 1) / (8 * hmax);
            int mcuy = (height + 8 * vmax - 1) / (8 * vmax);

            for (int c = 0; c < ncomp; c++) {
                comp[c].bw = mcux * comp[c].hs;
                comp[c].bh = mcuy * comp[c].vs;
                comp[c].cw = (width * comp[c].hs + hmax - 1) / hmax;
                comp[c].ch = (height * comp[c].vs + vmax - 1) / vmax;
                comp[c].stride = comp[c].bw * 8;
                comp[c].plane = (unsigned char *)malloc((unsigned long)comp[c].stride *
                                                        comp[c].bh * 8);
                if (!comp[c].plane) { img_err = "out of memory"; goto cleanup; }
                comp[c].pred = 0;
            }

            jbits b;
            memset(&b, 0, sizeof b);
            b.d = d;
            b.len = n;
            b.pos = p + seglen;

            int blk[64];
            int mcu_count = 0;
            for (int my = 0; my < mcuy; my++) {
                for (int mx = 0; mx < mcux; mx++) {
                    if (restart && mcu_count && mcu_count % restart == 0) {
                        /* Resynchronise: byte-align, step over the RSTn
                         * marker and forget the DC predictors. */
                        b.cnt = 0;
                        b.stop = 0;
                        while (b.pos + 1 < b.len &&
                               !(b.d[b.pos] == 0xFF && b.d[b.pos + 1] >= 0xD0 &&
                                 b.d[b.pos + 1] <= 0xD7)) b.pos++;
                        if (b.pos + 1 < b.len) b.pos += 2;
                        for (int c = 0; c < ncomp; c++) comp[c].pred = 0;
                    }
                    mcu_count++;

                    for (int c = 0; c < ncomp; c++) {
                        for (int by = 0; by < comp[c].vs; by++) {
                            for (int bx = 0; bx < comp[c].hs; bx++) {
                                memset(blk, 0, sizeof blk);
                                const unsigned short *q = qt[comp[c].tq & 3];

                                int t = jhuff_decode(&b, &hdc[comp[c].td & 3]);
                                int diff = t ? jb_extend(jb_receive(&b, t), t) : 0;
                                comp[c].pred += diff;
                                blk[0] = comp[c].pred * q[0];

                                for (int k = 1; k < 64; ) {
                                    int rs = jhuff_decode(&b, &hac[comp[c].ta & 3]);
                                    int s = rs & 15, r = rs >> 4;
                                    if (s == 0) {
                                        if (r != 15) break;
                                        k += 16;
                                    } else {
                                        k += r;
                                        if (k > 63) break;
                                        blk[jz[k]] = jb_extend(jb_receive(&b, s), s) * q[k];
                                        k++;
                                    }
                                }

                                int px0 = (mx * comp[c].hs + bx) * 8;
                                int py0 = (my * comp[c].vs + by) * 8;
                                idct8x8(blk, comp[c].plane +
                                        (unsigned long)py0 * comp[c].stride + px0,
                                        comp[c].stride);
                            }
                        }
                    }
                }
            }

            /* --- colour conversion + chroma upsampling --- */
            unsigned int *px = (unsigned int *)malloc((unsigned long)width * height * 4);
            if (!px) { img_err = "out of memory"; goto cleanup; }

            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    int Y, Cb = 128, Cr = 128;
                    Y = comp[0].plane[(unsigned long)(y * comp[0].vs / vmax) *
                                      comp[0].stride + (x * comp[0].hs / hmax)];
                    if (ncomp >= 3) {
                        Cb = jsample(&comp[1], jmap(x, comp[1].hs, hmax),
                                     jmap(y, comp[1].vs, vmax));
                        Cr = jsample(&comp[2], jmap(x, comp[2].hs, hmax),
                                     jmap(y, comp[2].vs, vmax));
                    }
                    int cb = Cb - 128, cr = Cr - 128;
                    int r = Y + ((91881 * cr) >> 16);
                    int g = Y - ((22554 * cb + 46802 * cr) >> 16);
                    int bl = Y + ((116130 * cb) >> 16);
                    if (r < 0) r = 0; else if (r > 255) r = 255;
                    if (g < 0) g = 0; else if (g > 255) g = 255;
                    if (bl < 0) bl = 0; else if (bl > 255) bl = 255;
                    px[(unsigned long)y * width + x] =
                        ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)bl;
                }
            }

            out->w = width;
            out->h = height;
            out->px = px;
            ok = 1;
            goto cleanup;
        }
        p += seglen;
    }

    if (!ok) img_err = "JPEG has no image data";

cleanup:
    for (int c = 0; c < 4; c++) free(comp[c].plane);
    return ok;
}

/* ==========================================================================
 * BMP (uncompressed 24/32-bit, and 8-bit palette)
 * ========================================================================== */

static int bmp_decode(const unsigned char *d, unsigned long n, image_t *out) {
    if (n < 54 || d[0] != 'B' || d[1] != 'M') { img_err = "not a BMP"; return 0; }
    unsigned off = (unsigned)d[10] | ((unsigned)d[11] << 8) |
                   ((unsigned)d[12] << 16) | ((unsigned)d[13] << 24);
    int w = (int)((unsigned)d[18] | ((unsigned)d[19] << 8) |
                  ((unsigned)d[20] << 16) | ((unsigned)d[21] << 24));
    int h = (int)((unsigned)d[22] | ((unsigned)d[23] << 8) |
                  ((unsigned)d[24] << 16) | ((unsigned)d[25] << 24));
    int bpp = (int)((unsigned)d[28] | ((unsigned)d[29] << 8));
    unsigned comp = (unsigned)d[30] | ((unsigned)d[31] << 8);
    int flip = 1;
    if (h < 0) { h = -h; flip = 0; }

    if (comp != 0) { img_err = "compressed BMP unsupported"; return 0; }
    if (bpp != 24 && bpp != 32 && bpp != 8) { img_err = "unsupported BMP depth"; return 0; }
    if (!img_dims_ok(w, h)) { img_err = "bad BMP size"; return 0; }

    unsigned char pal[256][4];
    if (bpp == 8) {
        unsigned hdr = (unsigned)d[14] | ((unsigned)d[15] << 8);
        unsigned long pp = 14 + hdr;
        for (int i = 0; i < 256; i++) {
            if (pp + 4 <= n) { memcpy(pal[i], d + pp, 4); pp += 4; }
            else { pal[i][0] = pal[i][1] = pal[i][2] = 0; }
        }
    }

    unsigned long stride = ((unsigned long)w * bpp / 8 + 3) & ~3UL;
    if (off + stride * (unsigned long)h > n) { img_err = "BMP truncated"; return 0; }

    unsigned int *px = (unsigned int *)malloc((unsigned long)w * h * 4);
    if (!px) { img_err = "out of memory"; return 0; }

    for (int y = 0; y < h; y++) {
        const unsigned char *row = d + off + stride * (unsigned long)(flip ? h - 1 - y : y);
        for (int x = 0; x < w; x++) {
            unsigned r, g, b;
            if (bpp == 8) {
                const unsigned char *c = pal[row[x]];
                b = c[0]; g = c[1]; r = c[2];
            } else {
                const unsigned char *c = row + (unsigned long)x * (bpp / 8);
                b = c[0]; g = c[1]; r = c[2];
            }
            px[(unsigned long)y * w + x] = (r << 16) | (g << 8) | b;
        }
    }
    out->w = w;
    out->h = h;
    out->px = px;
    return 1;
}

/* The two formats that are large enough to deserve their own files. Both are
 * included here rather than beside them so that everything a caller needs
 * still arrives with img.h alone. */
#include "svg.h"
#include "webp.h"

/* ==========================================================================
 * front door
 * ========================================================================== */

/* Sniff the magic rather than trust the extension: a file called .png that is
 * really a JPEG should still open. */
static int img_load(const void *data, unsigned long len, image_t *out) {
    const unsigned char *d = (const unsigned char *)data;
    img_err = "";
    out->px = 0;
    out->w = out->h = 0;
    if (len < 4) { img_err = "file too short"; return 0; }
    if (d[0] == 137 && d[1] == 'P')                  return png_decode(d, len, out);
    if (d[0] == 0xFF && d[1] == 0xD8)                return jpeg_decode(d, len, out);
    if (d[0] == 'B' && d[1] == 'M')                  return bmp_decode(d, len, out);
    if (len > 16 && d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F' &&
        d[8] == 'W' && d[9] == 'E' && d[10] == 'B' && d[11] == 'P')
        return webp_decode(d, len, out);

    /* SVG is text, and the tag is not always the first thing in the file --
     * an XML declaration, a doctype or a comment can come first. Look for the
     * root within the first stretch of the file rather than at byte zero. */
    {
        unsigned long lim = len < 1024 ? len : 1024;
        for (unsigned long i = 0; i + 4 < lim; i++)
            if (d[i] == '<' && (d[i+1] == 's' || d[i+1] == 'S') &&
                (d[i+2] == 'v' || d[i+2] == 'V') && (d[i+3] == 'g' || d[i+3] == 'G') &&
                (d[i+4] == ' ' || d[i+4] == '\t' || d[i+4] == '\n' ||
                 d[i+4] == '\r' || d[i+4] == '>'))
                return svg_decode(d, len, out);
    }

    img_err = "unknown image format";
    return 0;
}

#endif /* IMG_H */
