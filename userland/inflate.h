#ifndef INFLATE_H
#define INFLATE_H

/* DEFLATE, and the two wrappers it usually arrives in.
 *
 * This was written for PNG, whose pixels are a raw RFC 1951 stream. It turned
 * out to be the same code the browser needs for a gzipped response and the
 * same code NetSurf's message catalogue needs, so it lives here rather than
 * in any one of them.
 *
 * Everything is static: this is a header of definitions, included by whoever
 * needs it, and the compiler drops what a given program does not call.
 */

#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * DEFLATE (RFC 1951)
 * ========================================================================== */

typedef struct {
    const unsigned char *in;
    unsigned long        inlen, pos;
    unsigned             bitbuf;
    int                  bitcnt;
    unsigned char       *out;
    unsigned long        cap, len;
    int                  err;
} inf_t;

static int inf_bit(inf_t *s) {
    if (s->bitcnt == 0) {
        if (s->pos >= s->inlen) { s->err = 1; return 0; }
        s->bitbuf = s->in[s->pos++];
        s->bitcnt = 8;
    }
    int b = (int)(s->bitbuf & 1u);
    s->bitbuf >>= 1;
    s->bitcnt--;
    return b;
}

static int inf_bits(inf_t *s, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) v |= inf_bit(s) << i;
    return v;
}

typedef struct {
    short count[16];
    short symbol[288];
} huff_t;

static void huff_build(huff_t *h, const unsigned char *lengths, int n) {
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    short offs[16];
    offs[0] = offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (short)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++)
        if (lengths[i]) h->symbol[offs[lengths[i]]++] = (short)i;
}

/* Canonical Huffman, LSB-first as DEFLATE stores it. Walks code lengths from
 * the shortest up, which needs no decoding table at all. */
static int huff_decode(inf_t *s, const huff_t *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= inf_bit(s);
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    s->err = 1;
    return -1;
}

static int inf_put(inf_t *s, unsigned char c) {
    if (s->len >= s->cap) {
        unsigned long nc = s->cap ? s->cap * 2 : 16384;
        unsigned char *n = (unsigned char *)realloc(s->out, nc);
        if (!n) { s->err = 1; return 0; }
        s->out = n;
        s->cap = nc;
    }
    s->out[s->len++] = c;
    return 1;
}

static const short inf_lbase[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const short inf_lext[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const short inf_dbase[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const short inf_dext[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static void inf_block(inf_t *s, const huff_t *lit, const huff_t *dist) {
    for (;;) {
        int sym = huff_decode(s, lit);
        if (s->err || sym < 0) return;
        if (sym < 256) {
            if (!inf_put(s, (unsigned char)sym)) return;
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) { s->err = 1; return; }
            int length = inf_lbase[sym] + inf_bits(s, inf_lext[sym]);
            int dsym = huff_decode(s, dist);
            if (s->err || dsym < 0 || dsym >= 30) { s->err = 1; return; }
            unsigned long d = (unsigned long)inf_dbase[dsym] +
                              (unsigned long)inf_bits(s, inf_dext[dsym]);
            if (d > s->len) { s->err = 1; return; }
            unsigned long from = s->len - d;
            for (int i = 0; i < length; i++)
                if (!inf_put(s, s->out[from + (unsigned long)i])) return;
        }
    }
}

static void inf_fixed(huff_t *lit, huff_t *dist) {
    unsigned char l[288], d[30];
    for (int i = 0;   i < 144; i++) l[i] = 8;
    for (int i = 144; i < 256; i++) l[i] = 9;
    for (int i = 256; i < 280; i++) l[i] = 7;
    for (int i = 280; i < 288; i++) l[i] = 8;
    for (int i = 0;   i < 30;  i++) d[i] = 5;
    huff_build(lit, l, 288);
    huff_build(dist, d, 30);
}

static int inf_dynamic(inf_t *s, huff_t *lit, huff_t *dist) {
    static const unsigned char order[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    int hlit  = inf_bits(s, 5) + 257;
    int hdist = inf_bits(s, 5) + 1;
    int hclen = inf_bits(s, 4) + 4;
    if (hlit > 286 || hdist > 30) { s->err = 1; return 0; }

    unsigned char cl[19];
    for (int i = 0; i < 19; i++) cl[i] = 0;
    for (int i = 0; i < hclen; i++) cl[order[i]] = (unsigned char)inf_bits(s, 3);

    huff_t clh;
    huff_build(&clh, cl, 19);

    unsigned char lengths[320];
    int n = 0;
    while (n < hlit + hdist) {
        int sym = huff_decode(s, &clh);
        if (s->err || sym < 0) return 0;
        if (sym < 16) {
            lengths[n++] = (unsigned char)sym;
        } else if (sym == 16) {
            if (n == 0) { s->err = 1; return 0; }
            unsigned char prev = lengths[n - 1];
            int rep = 3 + inf_bits(s, 2);
            while (rep-- && n < 320) lengths[n++] = prev;
        } else if (sym == 17) {
            int rep = 3 + inf_bits(s, 3);
            while (rep-- && n < 320) lengths[n++] = 0;
        } else {
            int rep = 11 + inf_bits(s, 7);
            while (rep-- && n < 320) lengths[n++] = 0;
        }
    }
    huff_build(lit, lengths, hlit);
    huff_build(dist, lengths + hlit, hdist);
    return 1;
}

/* Raw DEFLATE stream -> malloc'd bytes. Caller frees. */
static unsigned char *inflate_raw(const unsigned char *in, unsigned long inlen,
                                  unsigned long *outlen) {
    inf_t s;
    memset(&s, 0, sizeof s);
    s.in = in;
    s.inlen = inlen;

    for (;;) {
        int final = inf_bit(&s);
        int type  = inf_bits(&s, 2);
        if (s.err) break;

        if (type == 0) {
            s.bitcnt = 0;
            if (s.pos + 4 > s.inlen) { s.err = 1; break; }
            unsigned len = (unsigned)s.in[s.pos] | ((unsigned)s.in[s.pos + 1] << 8);
            s.pos += 4;
            if (s.pos + len > s.inlen) { s.err = 1; break; }
            for (unsigned i = 0; i < len; i++)
                if (!inf_put(&s, s.in[s.pos + i])) break;
            s.pos += len;
        } else if (type == 1 || type == 2) {
            huff_t lit, dist;
            if (type == 1) inf_fixed(&lit, &dist);
            else if (!inf_dynamic(&s, &lit, &dist)) break;
            inf_block(&s, &lit, &dist);
        } else {
            s.err = 1;
        }
        if (s.err || final) break;
    }

    if (s.err && s.len == 0) {
        free(s.out);
        return 0;
    }
    *outlen = s.len;
    return s.out;
}

/* ==========================================================================
 * The wrappers
 * ========================================================================== */

/* Where the deflate stream starts inside a gzip member, or -1 if this is not
 * one. The header is fixed at ten bytes plus whichever optional fields the
 * flag byte claims are there. */
__attribute__((unused)) static int gzip_body_at(const unsigned char *p, int n) {
    if (n < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) return -1;
    int flg = p[3], i = 10;
    if (flg & 4) {                              /* FEXTRA */
        if (i + 2 > n) return -1;
        i += 2 + (p[i] | (p[i + 1] << 8));
    }
    if (flg & 8)  { while (i < n && p[i]) i++; i++; }    /* FNAME */
    if (flg & 16) { while (i < n && p[i]) i++; i++; }    /* FCOMMENT */
    if (flg & 2)  i += 2;                                /* FHCRC */
    return (i < n) ? i : -1;
}

/* A `deflate` body is usually wrapped in zlib's two bytes and sometimes is
 * not, so the wrapper is recognised rather than assumed. */
__attribute__((unused)) static int zlib_body_at(const unsigned char *p, int n) {
    if (n >= 2 && (p[0] & 0x0f) == 8 && ((p[0] << 8) | p[1]) % 31 == 0) return 2;
    return 0;
}

#endif
