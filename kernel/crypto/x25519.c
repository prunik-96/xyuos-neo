#include "crypto.h"

/* X25519, RFC 7748.
 *
 * Field elements are sixteen signed limbs of sixteen bits, base 2^16. That is
 * not the fast representation -- the fast ones pack 51 bits into a 64-bit limb
 * and need 128-bit products -- but it is the one that is hard to get wrong,
 * because every intermediate stays far inside int64 and the carry chain is a
 * single obvious loop. A handshake does one scalar multiplication; the
 * difference costs microseconds nobody will see.
 *
 * The curve is chosen so that this is safe with no special cases: every
 * 32-byte string is a valid public key, and the ladder below runs the same
 * sequence of operations whatever the scalar bits are. */

typedef int64_t gf[16];

static const gf gf0   = { 0 };
static const gf gf1   = { 1 };
static const gf gf121665 = { 0xDB41, 1 };

static void fcopy(gf o, const gf a) { for (int i = 0; i < 16; i++) o[i] = a[i]; }
static void fadd(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fsub(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

/* Propagate carries so every limb is back inside sixteen bits. The top limb
 * folds around by 38 = 2 * 19, because 2^256 = 38 mod (2^255 - 19). */
static void carry(gf o) {
    for (int i = 0; i < 16; i++) {
        int64_t c = o[i] >> 16;
        o[i] -= c << 16;
        if (i < 15) o[i + 1] += c;
        else        o[0] += 38 * c;
    }
}

static void fmul(gf o, const gf a, const gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    carry(o);
    carry(o);
}

static void fsq(gf o, const gf a) { fmul(o, a, a); }

/* 1/a, by raising to the power p-2. The exponent's bit pattern is all ones
 * except for bits 2 and 4, which is why those two rounds skip the multiply. */
static void finv(gf o, const gf a) {
    gf c;
    fcopy(c, a);
    for (int i = 253; i >= 0; i--) {
        fsq(c, c);
        if (i != 2 && i != 4) fmul(c, c, a);
    }
    fcopy(o, c);
}

/* Swap p and q if b is 1, without branching on b. */
static void cswap(gf p, gf q, int64_t b) {
    int64_t mask = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = mask & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void unpack(gf o, const uint8_t *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;                       /* the top bit is ignored, RFC 7748 */
}

static void pack(uint8_t *o, const gf n) {
    gf m, t;
    fcopy(t, n);
    carry(t); carry(t); carry(t);
    /* Two conditional subtractions of p bring the value into [0, p). */
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int64_t b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        cswap(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i]     = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t z[32];
    gf x, a, b, c, d, e, f;

    for (int i = 0; i < 32; i++) z[i] = scalar[i];
    /* Clamping, RFC 7748 5: clear the low three bits, clear the top bit, set
     * the second-highest. This is what makes every scalar a member of the
     * right subgroup and keeps the ladder's timing uniform. */
    z[0] &= 248;
    z[31] &= 127;
    z[31] |= 64;

    unpack(x, point);
    fcopy(b, x);
    fcopy(a, gf1); fcopy(c, gf0); fcopy(d, gf1);

    for (int i = 254; i >= 0; i--) {
        int64_t r = (z[i >> 3] >> (i & 7)) & 1;
        cswap(a, b, r);
        cswap(c, d, r);
        fadd(e, a, c);
        fsub(a, a, c);
        fadd(c, b, d);
        fsub(b, b, d);
        fsq(d, e);
        fsq(f, a);
        fmul(a, c, a);
        fmul(c, b, e);
        fadd(e, a, c);
        fsub(a, a, c);
        fsq(b, a);
        fsub(c, d, f);
        fmul(a, c, gf121665);
        fadd(a, a, d);
        fmul(c, c, a);
        fmul(a, d, f);
        fmul(d, b, x);
        fsq(b, e);
        cswap(a, b, r);
        cswap(c, d, r);
    }

    finv(c, c);
    fmul(a, a, c);
    pack(out, a);
}

void x25519_base(uint8_t out[32], const uint8_t scalar[32]) {
    uint8_t base[32] = { 9 };
    x25519(out, scalar, base);
}
