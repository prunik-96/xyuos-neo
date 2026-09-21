#include "crypto.h"

/* Just enough big-integer arithmetic to check an RSA signature.
 *
 * Verification is s^e mod n with a public exponent that is almost always
 * 65537, so the only operation that matters is modular multiplication of
 * numbers a few thousand bits wide. That is done in Montgomery form: the
 * reduction after each multiply becomes a shift and a couple of multiplies
 * instead of a division, and dividing numbers this size is the one genuinely
 * unpleasant thing to write.
 *
 * Nothing here is secret -- a public key and a signature are both public -- so
 * unlike the rest of kernel/crypto this code has nothing to hide from timing.
 */

#define LIMBS BN_MAX_LIMBS

typedef unsigned __int128 u128;

static void bn_zero(bn_t *a) {
    a->n = 0;
    for (int i = 0; i < LIMBS; i++) a->v[i] = 0;
}

static void bn_trim(bn_t *a) {
    while (a->n > 0 && a->v[a->n - 1] == 0) a->n--;
}

int bn_from_bytes(bn_t *a, const uint8_t *be, int len) {
    bn_zero(a);
    while (len > 0 && be[0] == 0) { be++; len--; }      /* leading zeros */
    if (len > LIMBS * 8) return 0;
    for (int i = 0; i < len; i++) {
        int bit = (len - 1 - i) * 8;
        a->v[bit / 64] |= (uint64_t)be[i] << (bit % 64);
    }
    a->n = (len * 8 + 63) / 64;
    bn_trim(a);
    return 1;
}

void bn_to_bytes(const bn_t *a, uint8_t *be, int len) {
    for (int i = 0; i < len; i++) {
        int bit = (len - 1 - i) * 8;
        int limb = bit / 64;
        be[i] = (limb < a->n) ? (uint8_t)(a->v[limb] >> (bit % 64)) : 0;
    }
}

int bn_bytes(const bn_t *a) {
    if (a->n == 0) return 0;
    uint64_t top = a->v[a->n - 1];
    int b = 0;
    while (top) { b++; top >>= 8; }
    return (a->n - 1) * 8 + b;
}

int bn_cmp(const bn_t *a, const bn_t *b) {
    int n = a->n > b->n ? a->n : b->n;
    for (int i = n - 1; i >= 0; i--) {
        uint64_t x = i < a->n ? a->v[i] : 0;
        uint64_t y = i < b->n ? b->v[i] : 0;
        if (x != y) return x > y ? 1 : -1;
    }
    return 0;
}

/* a -= b over exactly `w` limbs, wrapping. Used both for a genuine subtraction
 * (where a >= b) and for the "subtract the modulus after an overflow" case,
 * where the borrow out of the top cancels the bit that did not fit. */
static void sub_w(bn_t *a, const bn_t *b, int w) {
    uint64_t borrow = 0;
    for (int i = 0; i < w; i++) {
        uint64_t bi = (i < b->n) ? b->v[i] : 0;
        u128 d = (u128)a->v[i] - bi - borrow;
        a->v[i] = (uint64_t)d;
        borrow = (uint64_t)(d >> 64) ? 1 : 0;
    }
    a->n = w;
    bn_trim(a);
}

/* The inverse of -n mod 2^64, by Newton's method: x <- x * (2 - n*x) doubles
 * the number of correct low bits each round, and five rounds from a seed good
 * to four bits covers all sixty-four. */
static uint64_t inv64(uint64_t n) {
    uint64_t x = n;                      /* correct to 4 bits for odd n */
    for (int i = 0; i < 5; i++) x *= 2 - n * x;
    return (uint64_t)0 - x;              /* we want -n^-1, not n^-1 */
}

int bn_mont_init(bn_mont_t *m, const bn_t *n) {
    if (n->n == 0 || !(n->v[0] & 1)) return 0;       /* modulus must be odd */
    m->n = *n;
    m->n0 = inv64(n->v[0]);

    /* rr = R^2 mod n, with R = 2^(64*len). Reached by doubling 128*len times
     * from 1, which needs no division. */
    int w = n->n;
    bn_zero(&m->rr);
    m->rr.v[0] = 1;
    m->rr.n = w;
    for (int i = 0; i < 128 * w; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < w; j++) {
            uint64_t hi = m->rr.v[j] >> 63;
            m->rr.v[j] = (m->rr.v[j] << 1) | carry;
            carry = hi;
        }
        m->rr.n = w;
        /* Doubling something below n leaves something below 2n, so at most one
         * subtraction is ever needed. */
        if (carry || bn_cmp(&m->rr, n) >= 0) sub_w(&m->rr, n, w);
        m->rr.n = w;
    }
    bn_trim(&m->rr);
    return 1;
}

/* r = a * b * R^-1 mod n. Interleaved: each round adds one limb of a times b,
 * then cancels the low limb by adding a multiple of n and shifting down. */
void bn_mont_mul(bn_t *r, const bn_t *a, const bn_t *b, const bn_mont_t *m) {
    int n = m->n.n;
    uint64_t t[LIMBS + 2];
    for (int i = 0; i < n + 2; i++) t[i] = 0;

    for (int i = 0; i < n; i++) {
        uint64_t ai = i < a->n ? a->v[i] : 0;

        u128 carry = 0;
        for (int j = 0; j < n; j++) {
            u128 s = (u128)ai * (j < b->n ? b->v[j] : 0) + t[j] + carry;
            t[j] = (uint64_t)s;
            carry = s >> 64;
        }
        u128 s = (u128)t[n] + carry;
        t[n] = (uint64_t)s;
        t[n + 1] += (uint64_t)(s >> 64);

        uint64_t u = t[0] * m->n0;
        carry = 0;
        for (int j = 0; j < n; j++) {
            u128 x = (u128)u * m->n.v[j] + t[j] + carry;
            t[j] = (uint64_t)x;
            carry = x >> 64;
        }
        s = (u128)t[n] + carry;
        t[n] = (uint64_t)s;
        t[n + 1] += (uint64_t)(s >> 64);

        for (int j = 0; j <= n; j++) t[j] = t[j + 1];   /* the divide by 2^64 */
        t[n + 1] = 0;
    }

    bn_zero(r);
    for (int i = 0; i < n; i++) r->v[i] = t[i];
    r->n = n;

    if (t[n] || bn_cmp(r, &m->n) >= 0) sub_w(r, &m->n, n);
    bn_trim(r);
}

/* r = base^exp mod n, exponent big-endian. Square and multiply, left to right,
 * entirely in Montgomery form. */
void bn_modexp(bn_t *r, const bn_t *base, const uint8_t *exp, int explen,
               const bn_mont_t *m) {
    bn_t x, acc;

    bn_mont_mul(&x, base, &m->rr, m);          /* into Montgomery form */

    int started = 0;
    bn_zero(&acc);
    for (int i = 0; i < explen; i++) {
        for (int b = 7; b >= 0; b--) {
            if (started) bn_mont_mul(&acc, &acc, &acc, m);
            if ((exp[i] >> b) & 1) {
                if (!started) { acc = x; started = 1; }
                else bn_mont_mul(&acc, &acc, &x, m);
            }
        }
    }
    if (!started) {                         /* exponent zero: the answer is 1 */
        bn_zero(r); r->v[0] = 1; r->n = 1;
        return;
    }

    bn_t one;
    bn_zero(&one); one.v[0] = 1; one.n = 1;
    bn_mont_mul(r, &acc, &one, m);             /* back out of Montgomery form */
}

/* --- the rest of what a curve needs -------------------------------------- */

void bn_set_u64(bn_t *a, uint64_t v) {
    bn_zero(a);
    a->v[0] = v;
    a->n = v ? 1 : 0;
}

int bn_is_zero(const bn_t *a) {
    for (int i = 0; i < a->n; i++) if (a->v[i]) return 0;
    return 1;
}

int bn_bit(const bn_t *a, int i) {
    if (i < 0 || i >= a->n * 64) return 0;
    return (int)((a->v[i / 64] >> (i % 64)) & 1);
}

/* r = a + b mod n. Both inputs are already reduced, so the sum is below 2n and
 * one conditional subtraction finishes it. */
void bn_mod_add(bn_t *r, const bn_t *a, const bn_t *b, const bn_t *n) {
    /* Built in a temporary, because callers write bn_mod_add(&t, &t, &t) to
     * double a value and clearing the destination first would read back the
     * zeros it had just written. */
    int w = n->n;
    uint64_t carry = 0;
    bn_t t;
    bn_zero(&t);
    for (int i = 0; i < w; i++) {
        u128 s2 = (u128)(i < a->n ? a->v[i] : 0) + (i < b->n ? b->v[i] : 0) + carry;
        t.v[i] = (uint64_t)s2;
        carry = (uint64_t)(s2 >> 64);
    }
    t.n = w;
    if (carry || bn_cmp(&t, n) >= 0) sub_w(&t, n, w);
    t.n = w;
    bn_trim(&t);
    *r = t;
}

/* r = a - b mod n. */
void bn_mod_sub(bn_t *r, const bn_t *a, const bn_t *b, const bn_t *n) {
    int w = n->n;
    bn_t t = *a;
    t.n = w;
    if (bn_cmp(a, b) < 0) {
        /* Add the modulus first so the subtraction cannot go negative. */
        uint64_t carry = 0;
        for (int i = 0; i < w; i++) {
            u128 s2 = (u128)t.v[i] + n->v[i] + carry;
            t.v[i] = (uint64_t)s2;
            carry = (uint64_t)(s2 >> 64);
        }
        t.n = w;
        (void)carry;      /* a + n < 2^(64w) + n; the borrow below cancels it */
    }
    sub_w(&t, b, w);
    *r = t;
    bn_trim(r);
}

/* Into and out of Montgomery form, so callers need not know what rr is for. */
void bn_mont_in(bn_t *r, const bn_t *a, const bn_mont_t *m) {
    bn_mont_mul(r, a, &m->rr, m);
}
void bn_mont_out(bn_t *r, const bn_t *a, const bn_mont_t *m) {
    bn_t one;
    bn_zero(&one); one.v[0] = 1; one.n = 1;
    bn_mont_mul(r, a, &one, m);
}
