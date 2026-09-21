#ifndef WEBP_H
#define WEBP_H

/* WebP for xyuOS Neo.
 *
 * One name, two entirely different formats behind it, plus a container that
 * decides which one you got:
 *
 *   VP8L is the lossless one. Canonical Huffman codes over a five-tree
 *   alphabet, LZ77 back-references across a two-dimensional image, a small
 *   cache of recently used colours, and four reversible transforms layered
 *   underneath that the decoder has to undo in the opposite order. It is a
 *   compression format and nothing else -- every pixel comes back exactly.
 *
 *   VP8 is the lossy one, and it is a video keyframe wearing a still image's
 *   clothing: an arithmetic coder, per-block intra prediction, a quantised
 *   integer transform, and a deblocking filter to hide the block edges the
 *   quantiser leaves behind. It lives in the second half of this file.
 *
 * The container is RIFF. A plain file is one VP8 or VP8L chunk; an extended
 * one begins with VP8X and may carry a separate alpha plane in an ALPH chunk,
 * which is itself either raw or a VP8L image standing in for the alpha values.
 *
 * All integer, like the rest of img.h -- these programs build under
 * -mgeneral-regs-only, so there is no floating point available even where the
 * specification writes one. Nothing here needs it: the transforms are defined
 * on integers precisely so that every decoder agrees to the last bit.
 */

#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * bit reading, least significant bit first (what VP8L uses)
 * ========================================================================== */

typedef struct {
    const unsigned char *buf;
    unsigned long        len, pos;
    unsigned long long   acc;
    int                  bits;
    int                  eof;
} wl_br;

static void wl_init(wl_br *b, const unsigned char *d, unsigned long n) {
    b->buf = d; b->len = n; b->pos = 0; b->acc = 0; b->bits = 0; b->eof = 0;
}

static void wl_fill(wl_br *b) {
    while (b->bits <= 56) {
        unsigned long long byte = 0;
        if (b->pos < b->len) byte = b->buf[b->pos++];
        else { b->eof = 1; byte = 0; }
        b->acc |= byte << b->bits;
        b->bits += 8;
    }
}

static unsigned wl_get(wl_br *b, int n) {
    if (n <= 0) return 0;
    if (b->bits < n) wl_fill(b);
    unsigned v = (unsigned)(b->acc & (((unsigned long long)1 << n) - 1));
    b->acc >>= n;
    b->bits -= n;
    return v;
}

static unsigned wl_peek(wl_br *b, int n) {
    if (b->bits < n) wl_fill(b);
    return (unsigned)(b->acc & (((unsigned long long)1 << n) - 1));
}

static void wl_skip(wl_br *b, int n) {
    if (b->bits < n) wl_fill(b);
    b->acc >>= n;
    b->bits -= n;
}

/* ==========================================================================
 * canonical Huffman, decoded through a flat lookup table
 * ========================================================================== */

/* Codes are at most 15 bits. A single table of 2^15 entries per tree would be
 * 64KB each and there are five per group, so the table is capped at 8 bits and
 * anything longer falls back to walking the code bit by bit. Real images spend
 * nearly all their symbols in the short half. */
#define WL_ROOT_BITS 8
#define WL_MAX_LEN   15

typedef struct {
    /* root[code] = (len << 16) | symbol, or 0 when the code is longer than the
     * root covers and the slow path is needed. */
    unsigned  *root;
    /* The canonical tables, for the slow path. */
    unsigned short counts[WL_MAX_LEN + 1];
    unsigned short *symbols;      /* in canonical order */
    int             nsym;
    int             one_symbol;   /* >= 0 when the tree is a single value */
} wl_huff;

static void wl_huff_free(wl_huff *h) {
    free(h->root); free(h->symbols);
    h->root = 0; h->symbols = 0;
}

/* Build from a list of code lengths (0 meaning "unused"). */
static int wl_huff_build(wl_huff *h, const unsigned char *lens, int n) {
    memset(h, 0, sizeof *h);
    h->one_symbol = -1;

    int used = 0, last = -1;
    for (int i = 0; i < n; i++)
        if (lens[i]) { used++; last = i; }
    if (used == 0) return 0;
    if (used == 1) { h->one_symbol = last; return 1; }

    for (int i = 0; i < n; i++)
        if (lens[i]) h->counts[lens[i]]++;

    /* Check the code is neither over- nor under-subscribed. */
    int left = 1;
    for (int l = 1; l <= WL_MAX_LEN; l++) {
        left <<= 1;
        left -= h->counts[l];
        if (left < 0) return 0;
    }

    unsigned short offs[WL_MAX_LEN + 2];
    offs[1] = 0;
    for (int l = 1; l <= WL_MAX_LEN; l++) offs[l + 1] = offs[l] + h->counts[l];
    h->nsym = offs[WL_MAX_LEN + 1];
    h->symbols = (unsigned short *)malloc((size_t)h->nsym * sizeof(unsigned short));
    if (!h->symbols) return 0;
    {
        unsigned short o[WL_MAX_LEN + 2];
        memcpy(o, offs, sizeof o);
        for (int i = 0; i < n; i++)
            if (lens[i]) h->symbols[o[lens[i]]++] = (unsigned short)i;
    }

    /* The fast table: for every code no longer than the root, write the symbol
     * into every slot whose low bits match the code (the stream is LSB-first,
     * so the code sits reversed in the low bits). */
    h->root = (unsigned *)calloc((size_t)1 << WL_ROOT_BITS, sizeof(unsigned));
    if (!h->root) return 0;

    unsigned code = 0;
    int si = 0;
    for (int l = 1; l <= WL_MAX_LEN; l++) {
        for (int c = 0; c < h->counts[l]; c++, si++) {
            if (l <= WL_ROOT_BITS) {
                /* Reverse the l-bit code, since we match against low bits. */
                unsigned r = 0;
                for (int k = 0; k < l; k++) if (code & (1u << (l - 1 - k))) r |= 1u << k;
                unsigned step = 1u << l;
                for (unsigned j = r; j < (1u << WL_ROOT_BITS); j += step)
                    h->root[j] = ((unsigned)l << 16) | h->symbols[si];
            }
            code++;
        }
        code <<= 1;
    }
    return 1;
}

static int wl_huff_read(wl_br *b, const wl_huff *h) {
    if (h->one_symbol >= 0) return h->one_symbol;

    unsigned v = wl_peek(b, WL_ROOT_BITS);
    unsigned e = h->root[v];
    if (e) { wl_skip(b, (int)(e >> 16)); return (int)(e & 0xFFFF); }

    /* Longer than the table: walk the canonical code one bit at a time. */
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= WL_MAX_LEN; l++) {
        code |= (int)wl_get(b, 1);
        int count = h->counts[l];
        if (code - first < count) return h->symbols[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

/* ==========================================================================
 * VP8L
 * ========================================================================== */

#define WL_NUM_TREES   5
#define WL_GREEN_SYMS  (256 + 24 + 40)   /* literals, lengths, cache entries */

typedef struct { wl_huff t[WL_NUM_TREES]; } wl_group;

static const unsigned char WL_CODE_ORDER[19] = {
    17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

/* A single Huffman tree: either two symbols written out, or a full set of code
 * lengths that are themselves Huffman-coded. */
static int wl_read_tree(wl_br *b, wl_huff *h, int alphabet) {
    if (wl_get(b, 1)) {                       /* the simple form */
        int nsym = (int)wl_get(b, 1) + 1;
        int first_bits = wl_get(b, 1) ? 8 : 1;
        unsigned char *lens = (unsigned char *)calloc((size_t)alphabet, 1);
        if (!lens) return 0;
        int s0 = (int)wl_get(b, first_bits);
        if (s0 >= alphabet) { free(lens); return 0; }
        lens[s0] = 1;
        if (nsym == 2) {
            int s1 = (int)wl_get(b, 8);
            if (s1 >= alphabet) { free(lens); return 0; }
            lens[s1] = 1;
        }
        int ok = wl_huff_build(h, lens, alphabet);
        free(lens);
        return ok;
    }

    /* The general form: nineteen code-length codes, in their fixed order. */
    unsigned char clen[19];
    memset(clen, 0, sizeof clen);
    int ncodes = (int)wl_get(b, 4) + 4;
    if (ncodes > 19) return 0;
    for (int i = 0; i < ncodes; i++) clen[WL_CODE_ORDER[i]] = (unsigned char)wl_get(b, 3);

    wl_huff ch;
    if (!wl_huff_build(&ch, clen, 19)) return 0;

    int max = alphabet;
    if (wl_get(b, 1)) {                       /* only the first few are coded */
        int lbits = 2 + 2 * (int)wl_get(b, 3);
        max = 2 + (int)wl_get(b, lbits);
        if (max > alphabet) max = alphabet;
    }

    unsigned char *lens = (unsigned char *)calloc((size_t)alphabet, 1);
    if (!lens) { wl_huff_free(&ch); return 0; }

    int prev = 8, i = 0, used = 0;
    while (i < alphabet && used < max) {
        int s = wl_huff_read(b, &ch);
        if (s < 0) break;
        used++;
        if (s < 16) {
            lens[i++] = (unsigned char)s;
            if (s) prev = s;
        } else if (s == 16) {                 /* repeat the previous, 3..6 */
            int n = 3 + (int)wl_get(b, 2);
            while (n-- && i < alphabet) lens[i++] = (unsigned char)prev;
        } else if (s == 17) {                 /* zeros, 3..10 */
            int n = 3 + (int)wl_get(b, 3);
            while (n-- && i < alphabet) lens[i++] = 0;
        } else {                              /* zeros, 11..138 */
            int n = 11 + (int)wl_get(b, 7);
            while (n-- && i < alphabet) lens[i++] = 0;
        }
    }
    wl_huff_free(&ch);

    int ok = wl_huff_build(h, lens, alphabet);
    free(lens);
    return ok;
}

static void wl_group_free(wl_group *g, int n) {
    if (!g) return;
    for (int i = 0; i < n; i++)
        for (int t = 0; t < WL_NUM_TREES; t++) wl_huff_free(&g[i].t[t]);
    free(g);
}

/* Back-reference distances are coded so that short two-dimensional offsets --
 * the pixel above, the one up and left -- get the smallest numbers. */
static const signed char WL_DIST_MAP[120][2] = {
    {0,1},{1,0},{1,1},{-1,1},{0,2},{2,0},{1,2},{-1,2},{2,1},{-2,1},
    {2,2},{-2,2},{0,3},{3,0},{1,3},{-1,3},{3,1},{-3,1},{2,3},{-2,3},
    {3,2},{-3,2},{0,4},{4,0},{1,4},{-1,4},{4,1},{-4,1},{3,3},{-3,3},
    {2,4},{-2,4},{4,2},{-4,2},{0,5},{3,4},{-3,4},{4,3},{-4,3},{5,0},
    {1,5},{-1,5},{5,1},{-5,1},{2,5},{-2,5},{5,2},{-5,2},{4,4},{-4,4},
    {3,5},{-3,5},{5,3},{-5,3},{0,6},{6,0},{1,6},{-1,6},{6,1},{-6,1},
    {2,6},{-2,6},{6,2},{-6,2},{4,5},{-4,5},{5,4},{-5,4},{3,6},{-3,6},
    {6,3},{-6,3},{0,7},{7,0},{1,7},{-1,7},{5,5},{-5,5},{7,1},{-7,1},
    {4,6},{-4,6},{6,4},{-6,4},{2,7},{-2,7},{7,2},{-7,2},{3,7},{-3,7},
    {7,3},{-7,3},{5,6},{-5,6},{6,5},{-6,5},{8,0},{4,7},{-4,7},{7,4},
    {-7,4},{8,1},{8,2},{6,6},{-6,6},{8,3},{5,7},{-5,7},{7,5},{-7,5},
    {8,4},{6,7},{-6,7},{7,6},{-7,6},{8,5},{7,7},{-7,7},{8,6},{8,7}
};

static int wl_dist(int code, int xsize) {
    if (code > 120) return code - 120;
    int dx = WL_DIST_MAP[code - 1][0], dy = WL_DIST_MAP[code - 1][1];
    int d = dy * xsize + dx;
    return d < 1 ? 1 : d;
}

/* A length or distance is a prefix code followed by that many extra bits. */
static int wl_prefix(wl_br *b, int code) {
    if (code < 4) return code + 1;
    int extra = (code - 2) >> 1;
    int offset = (2 + (code & 1)) << extra;
    return offset + (int)wl_get(b, extra) + 1;
}

static unsigned wl_cache_hash(unsigned argb, int bits) {
    return (argb * 0x1e35a7bdu) >> (32 - bits);
}

/* --- the transforms ------------------------------------------------------- */

enum { WL_PREDICTOR = 0, WL_CROSS_COLOR = 1, WL_SUBTRACT_GREEN = 2, WL_COLOR_INDEX = 3 };

typedef struct {
    int       type;
    int       bits;          /* block size, as a shift */
    int       xsize, ysize;  /* of the image the transform was measured on */
    unsigned *data;          /* the predictor / cross-colour / palette image */
    int       ncolors;
} wl_xform;

static int wl_add8(int a, int b) { return (a + b) & 0xFF; }

/* The fourteen predictors, all of them cheap combinations of the three
 * neighbours already decoded. */
static unsigned wl_predict(int mode, unsigned L, unsigned T, unsigned TL, unsigned TR) {
    switch (mode) {
    case 0: return 0xFF000000u;
    case 1: return L;
    case 2: return T;
    case 3: return TR;
    case 4: return TL;
    case 5: {   /* average of (average of L and TR) and T */
        unsigned r = 0;
        for (int s = 0; s < 32; s += 8) {
            int a = ((L >> s) & 0xFF), c = ((TR >> s) & 0xFF), t = ((T >> s) & 0xFF);
            int v = (((a + c) >> 1) + t) >> 1;
            r |= (unsigned)v << s;
        }
        return r;
    }
    case 6: case 7: case 8: case 9: {
        unsigned A = (mode == 6) ? L  : (mode == 7) ? L : (mode == 8) ? TL : T;
        unsigned B = (mode == 6) ? TL : (mode == 7) ? T : (mode == 8) ? T  : TR;
        unsigned r = 0;
        for (int s = 0; s < 32; s += 8) {
            int v = ((((A >> s) & 0xFF) + ((B >> s) & 0xFF)) >> 1);
            r |= (unsigned)v << s;
        }
        return r;
    }
    case 10: {  /* average of the average of L,TL and the average of T,TR */
        unsigned r = 0;
        for (int s = 0; s < 32; s += 8) {
            int a = (((L >> s) & 0xFF) + ((TL >> s) & 0xFF)) >> 1;
            int c = (((T >> s) & 0xFF) + ((TR >> s) & 0xFF)) >> 1;
            r |= (unsigned)((a + c) >> 1) << s;
        }
        return r;
    }
    case 11: {  /* whichever of L and T the gradient says is closer */
        int pl = 0, pt = 0;
        for (int s = 0; s < 32; s += 8) {
            int l = (L >> s) & 0xFF, t = (T >> s) & 0xFF, tl = (TL >> s) & 0xFF;
            int p = l + t - tl;
            int dl = p - l, dt = p - t;
            pl += dl < 0 ? -dl : dl;
            pt += dt < 0 ? -dt : dt;
        }
        return pl < pt ? L : T;
    }
    case 12: {  /* the gradient itself, clamped */
        unsigned r = 0;
        for (int s = 0; s < 32; s += 8) {
            int v = ((L >> s) & 0xFF) + ((T >> s) & 0xFF) - ((TL >> s) & 0xFF);
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            r |= (unsigned)v << s;
        }
        return r;
    }
    default: {  /* 13: the gradient, halved towards the average of L and T */
        unsigned r = 0;
        for (int s = 0; s < 32; s += 8) {
            int l = (L >> s) & 0xFF, t = (T >> s) & 0xFF, tl = (TL >> s) & 0xFF;
            int a = (l + t) >> 1;
            int v = a + (a - tl) / 2;
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            r |= (unsigned)v << s;
        }
        return r;
    }
    }
}

static void wl_undo_predictor(const wl_xform *x, unsigned *px, int w, int h) {
    int bits = x->bits, bw = (w + (1 << bits) - 1) >> bits;

    /* The first pixel is fixed, the first row is left-predicted, the first
     * column is top-predicted -- the general rule has no neighbours there. */
    /* The very first pixel has no neighbours at all, so it is predicted from
     * opaque black. Only the alpha byte is non-zero there, and the carry out
     * of the top of the word is the wrap that byte wants. */
    px[0] = px[0] + 0xFF000000u;
    for (int i = 1; i < w; i++) {
        unsigned L = px[i - 1], v = px[i], r = 0;
        for (int s = 0; s < 32; s += 8)
            r |= (unsigned)wl_add8((L >> s) & 0xFF, (v >> s) & 0xFF) << s;
        px[i] = r;
    }
    for (int y = 1; y < h; y++) {
        unsigned *row = px + (long)y * w, *up = px + (long)(y - 1) * w;
        {
            unsigned T = up[0], v = row[0], r = 0;
            for (int s = 0; s < 32; s += 8)
                r |= (unsigned)wl_add8((T >> s) & 0xFF, (v >> s) & 0xFF) << s;
            row[0] = r;
        }
        for (int i = 1; i < w; i++) {
            int mode = (int)((x->data[(long)(y >> bits) * bw + (i >> bits)] >> 8) & 0xFF);
            if (mode > 13) mode = 13;
            unsigned L = row[i - 1], T = up[i], TL = up[i - 1];
            /* The rightmost column has no pixel above and to its right. The
             * specification does not clamp or repeat there: it names the
             * LEFTMOST pixel of the current row as the top-right neighbour,
             * which is what a flat buffer gives you if you read one past the
             * end of the row above. Substituting the pixel above instead is
             * wrong in the last column of every row, and the error then
             * spreads inwards through everything predicted from it. */
            unsigned TR = (i + 1 < w) ? up[i + 1] : row[0];
            unsigned P = wl_predict(mode, L, T, TL, TR);
            unsigned v = row[i], r = 0;
            for (int s = 0; s < 32; s += 8)
                r |= (unsigned)wl_add8((P >> s) & 0xFF, (v >> s) & 0xFF) << s;
            row[i] = r;
        }
    }
}

static void wl_undo_cross(const wl_xform *x, unsigned *px, int w, int h) {
    int bits = x->bits, bw = (w + (1 << bits) - 1) >> bits;
    for (int y = 0; y < h; y++) {
        unsigned *row = px + (long)y * w;
        for (int i = 0; i < w; i++) {
            /* The three multipliers are packed into one pixel of the
             * transform image, and NOT in the order the names suggest:
             * green-to-red is the blue byte, green-to-blue the green byte,
             * red-to-blue the red byte. Each is signed. */
            unsigned m = x->data[(long)(y >> bits) * bw + (i >> bits)];
            int g2r = (signed char)(m & 0xFF);
            int g2b = (signed char)((m >> 8) & 0xFF);
            int r2b = (signed char)((m >> 16) & 0xFF);
            unsigned v = row[i];
            int red = (int)((v >> 16) & 0xFF);
            int grn = (int)((v >> 8) & 0xFF);
            int blu = (int)(v & 0xFF);
            red = (red + ((g2r * (signed char)grn) >> 5)) & 0xFF;
            blu = (blu + ((g2b * (signed char)grn) >> 5)) & 0xFF;
            blu = (blu + ((r2b * (signed char)red) >> 5)) & 0xFF;
            row[i] = (v & 0xFF000000u) | ((unsigned)red << 16) |
                     ((unsigned)grn << 8) | (unsigned)blu;
        }
    }
}

static void wl_undo_subgreen(unsigned *px, long n) {
    for (long i = 0; i < n; i++) {
        unsigned v = px[i];
        unsigned g = (v >> 8) & 0xFF;
        unsigned r = ((v >> 16) + g) & 0xFF;
        unsigned b = ((v      ) + g) & 0xFF;
        px[i] = (v & 0xFF00FF00u) | (r << 16) | b;
    }
}

/* The colour-indexing transform also packs several pixels into one when the
 * palette is small, so undoing it makes the image wider again. */
static unsigned *wl_undo_palette(const wl_xform *x, unsigned *px, int packed_w,
                                 int w, int h) {
    int bits = x->bits;              /* the shift: 1 << bits pixels per byte */
    int per = 1 << bits;
    unsigned *out = (unsigned *)malloc((size_t)w * h * sizeof(unsigned));
    if (!out) return 0;
    int mask = (1 << (8 >> bits)) - 1;
    for (int y = 0; y < h; y++) {
        const unsigned *src = px + (long)y * packed_w;
        unsigned *dst = out + (long)y * w;
        for (int i = 0; i < w; i++) {
            int sx = i / per;
            int sub = i % per;
            unsigned g = (src[sx] >> 8) & 0xFF;
            int idx = (int)((g >> (sub * (8 >> bits))) & (unsigned)mask);
            if (idx >= x->ncolors) idx = x->ncolors ? x->ncolors - 1 : 0;
            dst[i] = x->ncolors ? x->data[idx] : 0xFF000000u;
        }
    }
    return out;
}

/* --- the image stream ------------------------------------------------------ */

static unsigned *wl_image(wl_br *b, int xsize, int ysize, int level0,
                          int *out_w);

/* Decode the pixels themselves, given the trees. */
static unsigned *wl_pixels(wl_br *b, int w, int h, int cache_bits,
                           const unsigned *huff_img, int huff_bits, int huff_w,
                           wl_group *groups, int ngroups) {
    long n = (long)w * h;
    unsigned *px = (unsigned *)malloc((size_t)n * sizeof(unsigned));
    if (!px) return 0;

    unsigned *cache = 0;
    if (cache_bits > 0) {
        cache = (unsigned *)calloc((size_t)1 << cache_bits, sizeof(unsigned));
        if (!cache) { free(px); return 0; }
    }

    long i = 0;
    int x = 0, y = 0;
    while (i < n) {
        if (b->eof && b->bits <= 0) break;

        wl_group *g = &groups[0];
        if (huff_img) {
            int gi = (int)((huff_img[(long)(y >> huff_bits) * huff_w +
                                     (x >> huff_bits)] >> 8) & 0xFFFF);
            if (gi >= ngroups) gi = 0;
            g = &groups[gi];
        }

        int s = wl_huff_read(b, &g->t[0]);
        if (s < 0) break;

        if (s < 256) {                       /* a literal, green first */
            int red = wl_huff_read(b, &g->t[1]);
            int blu = wl_huff_read(b, &g->t[2]);
            int alp = wl_huff_read(b, &g->t[3]);
            if (red < 0 || blu < 0 || alp < 0) break;
            unsigned v = ((unsigned)alp << 24) | ((unsigned)red << 16) |
                         ((unsigned)s << 8) | (unsigned)blu;
            px[i++] = v;
            if (cache) cache[wl_cache_hash(v, cache_bits)] = v;
            if (++x == w) { x = 0; y++; }
        } else if (s < 256 + 24) {           /* a back-reference */
            int length = wl_prefix(b, s - 256);
            int dcode = wl_huff_read(b, &g->t[4]);
            if (dcode < 0) break;
            int dist = wl_dist(wl_prefix(b, dcode), w);
            if (dist > i || length <= 0) break;
            if (i + length > n) length = (int)(n - i);
            for (int k = 0; k < length; k++) {
                unsigned v = px[i - dist];
                px[i++] = v;
                if (cache) cache[wl_cache_hash(v, cache_bits)] = v;
                if (++x == w) { x = 0; y++; }
            }
        } else {                             /* an entry from the colour cache */
            if (!cache) break;
            int idx = s - 256 - 24;
            if (idx >= (1 << cache_bits)) break;
            unsigned v = cache[idx];
            px[i++] = v;
            if (++x == w) { x = 0; y++; }
        }
    }

    /* A stream that ran out leaves the tail black rather than uninitialised. */
    for (; i < n; i++) px[i] = 0xFF000000u;

    free(cache);
    return px;
}

/* Read the Huffman groups and then the pixels. `level0` streams may carry the
 * meta-Huffman image that says which group each block uses. */
static unsigned *wl_entropy(wl_br *b, int w, int h, int level0) {
    int cache_bits = 0;
    if (wl_get(b, 1)) {
        cache_bits = (int)wl_get(b, 4);
        if (cache_bits < 1 || cache_bits > 11) return 0;
    }

    unsigned *huff_img = 0;
    int huff_bits = 0, huff_w = 0, ngroups = 1;
    if (level0 && wl_get(b, 1)) {
        huff_bits = (int)wl_get(b, 3) + 2;
        int hw = (w + (1 << huff_bits) - 1) >> huff_bits;
        int hh = (h + (1 << huff_bits) - 1) >> huff_bits;
        int got_w = hw;
        huff_img = wl_image(b, hw, hh, 0, &got_w);
        if (!huff_img) return 0;
        huff_w = hw;
        for (long i = 0; i < (long)hw * hh; i++) {
            int gi = (int)((huff_img[i] >> 8) & 0xFFFF);
            if (gi + 1 > ngroups) ngroups = gi + 1;
        }
    }

    if (ngroups <= 0 || ngroups > 100000) { free(huff_img); return 0; }
    wl_group *groups = (wl_group *)calloc((size_t)ngroups, sizeof(wl_group));
    if (!groups) { free(huff_img); return 0; }

    int alpha0 = WL_GREEN_SYMS - 40 + (cache_bits ? (1 << cache_bits) : 0);
    for (int i = 0; i < ngroups; i++) {
        static const int sizes[WL_NUM_TREES] = { 0, 256, 256, 256, 40 };
        for (int t = 0; t < WL_NUM_TREES; t++) {
            int alphabet = t == 0 ? alpha0 : sizes[t];
            if (!wl_read_tree(b, &groups[i].t[t], alphabet)) {
                wl_group_free(groups, ngroups);
                free(huff_img);
                return 0;
            }
        }
    }

    unsigned *px = wl_pixels(b, w, h, cache_bits, huff_img, huff_bits, huff_w,
                             groups, ngroups);
    wl_group_free(groups, ngroups);
    free(huff_img);
    return px;
}

/* One image stream: any transforms it declares, then the entropy-coded data,
 * then those transforms undone in the reverse order. */
static unsigned *wl_image(wl_br *b, int xsize, int ysize, int level0,
                          int *out_w) {
    wl_xform xf[4];
    int nxf = 0;
    int w = xsize;

    if (level0) {
        while (wl_get(b, 1)) {
            if (nxf == 4) return 0;
            wl_xform *x = &xf[nxf];
            memset(x, 0, sizeof *x);
            x->type = (int)wl_get(b, 2);

            /* The same transform twice would mean undoing it twice. */
            for (int i = 0; i < nxf; i++)
                if (xf[i].type == x->type) {
                    for (int k = 0; k < nxf; k++) free(xf[k].data);
                    return 0;
                }

            if (x->type == WL_PREDICTOR || x->type == WL_CROSS_COLOR) {
                x->bits = (int)wl_get(b, 3) + 2;
                int bw = (w + (1 << x->bits) - 1) >> x->bits;
                int bh = (ysize + (1 << x->bits) - 1) >> x->bits;
                int gw = bw;
                x->data = wl_image(b, bw, bh, 0, &gw);
                if (!x->data) {
                    for (int k = 0; k < nxf; k++) free(xf[k].data);
                    return 0;
                }
            } else if (x->type == WL_COLOR_INDEX) {
                x->ncolors = (int)wl_get(b, 8) + 1;
                int gw = x->ncolors;
                x->data = wl_image(b, x->ncolors, 1, 0, &gw);
                if (!x->data) {
                    for (int k = 0; k < nxf; k++) free(xf[k].data);
                    return 0;
                }
                /* The palette is stored as differences along its length. */
                for (int i = 1; i < x->ncolors; i++) {
                    unsigned p = x->data[i - 1], v = x->data[i], r = 0;
                    for (int s = 0; s < 32; s += 8)
                        r |= (unsigned)wl_add8((p >> s) & 0xFF, (v >> s) & 0xFF) << s;
                    x->data[i] = r;
                }
                x->bits = x->ncolors <= 2 ? 3 : x->ncolors <= 4 ? 2
                        : x->ncolors <= 16 ? 1 : 0;
                if (x->bits) {
                    int per = 1 << x->bits;
                    w = (w + per - 1) / per;
                }
            }
            nxf++;
        }
    }

    unsigned *px = wl_entropy(b, w, ysize, level0);
    if (!px) {
        for (int k = 0; k < nxf; k++) free(xf[k].data);
        return 0;
    }

    for (int i = nxf - 1; i >= 0; i--) {
        wl_xform *x = &xf[i];
        if (x->type == WL_SUBTRACT_GREEN) {
            wl_undo_subgreen(px, (long)w * ysize);
        } else if (x->type == WL_PREDICTOR) {
            wl_undo_predictor(x, px, w, ysize);
        } else if (x->type == WL_CROSS_COLOR) {
            wl_undo_cross(x, px, w, ysize);
        } else if (x->type == WL_COLOR_INDEX) {
            unsigned *n = wl_undo_palette(x, px, w, xsize, ysize);
            free(px);
            if (!n) {
                for (int k = 0; k < nxf; k++) free(xf[k].data);
                return 0;
            }
            px = n;
            w = xsize;
        }
    }

    for (int k = 0; k < nxf; k++) free(xf[k].data);
    if (out_w) *out_w = w;
    return px;
}

/* Decode a VP8L chunk into ARGB. */
static unsigned *webp_lossless(const unsigned char *d, unsigned long n,
                               int *pw, int *ph) {
    if (n < 5 || d[0] != 0x2F) { img_err = "not a VP8L stream"; return 0; }
    wl_br b;
    wl_init(&b, d + 1, n - 1);
    int w = (int)wl_get(&b, 14) + 1;
    int h = (int)wl_get(&b, 14) + 1;
    wl_get(&b, 1);                       /* alpha_is_used, only a hint */
    int version = (int)wl_get(&b, 3);
    if (version != 0) { img_err = "unsupported VP8L version"; return 0; }
    if (!img_dims_ok(w, h)) { img_err = "bad WebP size"; return 0; }

    int gw = w;
    unsigned *px = wl_image(&b, w, h, 1, &gw);
    if (!px) { img_err = "corrupt VP8L stream"; return 0; }
    *pw = w; *ph = h;
    return px;
}

/* ==========================================================================
 * VP8 -- the lossy half
 * ========================================================================== */

/* Written out in webp_vp8.h purely to keep this file readable: it is a video
 * keyframe decoder and it is longer than everything above put together. */
#include "webp_vp8.h"

/* ==========================================================================
 * the container
 * ========================================================================== */

static unsigned webp_u32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* Lay the picture on whatever the caller said was behind it, the same way the
 * rest of img.h treats alpha. */
static void webp_flatten(unsigned *px, long n) {
    for (long i = 0; i < n; i++) {
        unsigned v = px[i];
        px[i] = img_flatten_px((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, v >> 24);
    }
}

static int webp_decode(const unsigned char *d, unsigned long len, image_t *out) {
    if (len < 20) { img_err = "WebP too short"; return 0; }

    unsigned long p = 12;                 /* past "RIFF" size "WEBP" */
    const unsigned char *vp8 = 0, *vp8l = 0, *alph = 0;
    unsigned long vp8n = 0, vp8ln = 0, alphn = 0;

    while (p + 8 <= len) {
        const unsigned char *tag = d + p;
        unsigned long sz = webp_u32(d + p + 4);
        unsigned long body = p + 8;
        if (body + sz > len) sz = len - body;

        if (!memcmp(tag, "VP8 ", 4))      { vp8  = d + body; vp8n  = sz; }
        else if (!memcmp(tag, "VP8L", 4)) { vp8l = d + body; vp8ln = sz; }
        else if (!memcmp(tag, "ALPH", 4)) { alph = d + body; alphn = sz; }
        /* VP8X carries the canvas size, which the frame repeats; ANIM and the
         * rest describe things a still viewer has no use for. */

        p = body + sz + (sz & 1);          /* chunks are padded to even */
    }

    int w = 0, h = 0;
    unsigned *px = 0;

    if (vp8l) {
        px = webp_lossless(vp8l, vp8ln, &w, &h);
    } else if (vp8) {
        px = webp_lossy(vp8, vp8n, &w, &h);
        if (px && alph) webp_apply_alpha(px, w, h, alph, alphn);
    } else {
        img_err = "WebP has no image in it";
        return 0;
    }
    if (!px) return 0;

#ifdef WEBP_ALPHA_ONLY
    /* A seam for the host check: hand back the alpha plane as a grey picture,
     * so it can be compared on its own rather than through the flattening. */
    for (long i = 0; i < (long)w * h; i++) {
        unsigned a = px[i] >> 24;
        px[i] = (a << 16) | (a << 8) | a;
    }
#else
    webp_flatten(px, (long)w * h);
#endif
    out->w = w;
    out->h = h;
    out->px = px;
    return 1;
}

#endif /* WEBP_H */
