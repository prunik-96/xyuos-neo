#include "crypto.h"

/* ECDSA over the two NIST curves the web is signed with: P-256 and P-384.
 *
 * Server certificates are overwhelmingly P-256, but the authorities above them
 * sign with P-384 and SHA-384, so a client that only knows the smaller curve
 * gets one link up the chain and stops. Both are here, and because both have
 * a = -3 the point arithmetic is identical -- only the numbers differ.
 *
 * The arithmetic reuses the Montgomery code written for RSA rather than the
 * fast reduction these primes were chosen to allow. Every serious
 * implementation does the fast thing; this one pays perhaps a factor of three
 * for a few hundred lines it does not have to get right, and a signature check
 * is a millisecond either way.
 *
 * Points live in Jacobian coordinates (x = X/Z^2, y = Y/Z^3) so the inner loop
 * needs no division -- only the final answer is converted back, which costs
 * one modular inverse instead of one per step. */

/* --- the curves ----------------------------------------------------------- */

static const char *P256_P = "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff";
static const char *P256_N = "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551";
static const char *P256_GX= "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296";
static const char *P256_GY= "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5";

static const char *P384_P =
    "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe"
    "ffffffff0000000000000000ffffffff";
static const char *P384_N =
    "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf"
    "581a0db248b0a77aecec196accc52973";
static const char *P384_GX=
    "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a38"
    "5502f25dbf55296c3a545e3872760ab7";
static const char *P384_GY=
    "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c0"
    "0a60b1ce1d7e819d7a431d7c90ea0e5f";

typedef struct {
    int      ready;
    int      bytes;              /* the width of a coordinate */
    bn_t     p, n;
    bn_mont_t mp, mn;
    bn_t     one_m, three_m;     /* 1 and 3, in Montgomery form modulo p */
    bn_t     gx_m, gy_m;
    uint8_t  p2[64], n2[64];     /* p-2 and n-2, the inversion exponents */
} curve_t;

static curve_t CURVES[2];        /* [0] = P-256, [1] = P-384 */
static curve_t *CU;              /* the one in use; one call runs at a time */

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int hexbytes(const char *h, uint8_t *out, int max) {
    int n = 0;
    while (h[0] && h[1] && n < max) {
        int a = hexval(h[0]), b = hexval(h[1]);
        if (a < 0 || b < 0) return 0;
        out[n++] = (uint8_t)((a << 4) | b);
        h += 2;
    }
    return n;
}

/* v -= 2, in place, on a big-endian byte string. Both moduli end well above 2,
 * so this never borrows past the last byte. */
static void minus2(uint8_t *v, int len) { v[len - 1] -= 2; }

static void curve_setup(curve_t *c, const char *ps, const char *ns,
                        const char *gxs, const char *gys) {
    uint8_t buf[64];
    bn_t t;
    int len;

    if (c->ready) return;

    len = hexbytes(ps, buf, sizeof buf);
    c->bytes = len;
    bn_from_bytes(&c->p, buf, len);
    for (int i = 0; i < len; i++) c->p2[i] = buf[i];
    minus2(c->p2, len);

    int nl = hexbytes(ns, buf, sizeof buf);
    bn_from_bytes(&c->n, buf, nl);
    for (int i = 0; i < nl; i++) c->n2[i] = buf[i];
    minus2(c->n2, nl);

    bn_mont_init(&c->mp, &c->p);
    bn_mont_init(&c->mn, &c->n);

    bn_set_u64(&t, 1); bn_mont_in(&c->one_m, &t, &c->mp);
    bn_set_u64(&t, 3); bn_mont_in(&c->three_m, &t, &c->mp);

    hexbytes(gxs, buf, sizeof buf);
    bn_from_bytes(&t, buf, len); bn_mont_in(&c->gx_m, &t, &c->mp);
    hexbytes(gys, buf, sizeof buf);
    bn_from_bytes(&t, buf, len); bn_mont_in(&c->gy_m, &t, &c->mp);

    c->ready = 1;
}

static void curves_init(void) {
    curve_setup(&CURVES[0], P256_P, P256_N, P256_GX, P256_GY);
    curve_setup(&CURVES[1], P384_P, P384_N, P384_GX, P384_GY);
}

/* Field operations, on Montgomery-form values modulo the current p. */
#define FMUL(r, a, b) bn_mont_mul(&(r), &(a), &(b), &CU->mp)
#define FADD(r, a, b) bn_mod_add(&(r), &(a), &(b), &CU->p)
#define FSUB(r, a, b) bn_mod_sub(&(r), &(a), &(b), &CU->p)

static void finv(bn_t *r, const bn_t *a) {
    /* a^(p-2) = a^-1, by Fermat. The exponentiation works on ordinary values,
     * so step out of Montgomery form and back. */
    static bn_t plain, inv;
    bn_mont_out(&plain, a, &CU->mp);
    bn_modexp(&inv, &plain, CU->p2, CU->bytes, &CU->mp);
    bn_mont_in(r, &inv, &CU->mp);
}

/* --- points --------------------------------------------------------------- */

typedef struct { bn_t x, y, z; int inf; } jpt;

static void jzero(jpt *P) {
    bn_set_u64(&P->x, 0); bn_set_u64(&P->y, 0); bn_set_u64(&P->z, 0);
    P->inf = 1;
}

/* Double a point. a = -3 lets this be done in eight multiplies. */
static void jdouble(jpt *R, const jpt *P) {
    if (P->inf || bn_is_zero(&P->y)) { jzero(R); return; }

    static bn_t delta, gamma, beta, alpha, t1, t2, t3;

    FMUL(delta, P->z, P->z);              /* Z^2 */
    FMUL(gamma, P->y, P->y);              /* Y^2 */
    FMUL(beta, P->x, gamma);              /* X*Y^2 */

    FSUB(t1, P->x, delta);
    FADD(t2, P->x, delta);
    FMUL(t3, t1, t2);                     /* (X-Z^2)(X+Z^2) */
    FMUL(alpha, CU->three_m, t3);

    FMUL(t1, alpha, alpha);
    FADD(t2, beta, beta);
    FADD(t2, t2, t2);                     /* 4*beta */
    FADD(t3, t2, t2);                     /* 8*beta */
    FSUB(R->x, t1, t3);

    FADD(t1, P->y, P->z);
    FMUL(t1, t1, t1);
    FSUB(t1, t1, gamma);
    FSUB(R->z, t1, delta);                /* (Y+Z)^2 - Y^2 - Z^2 = 2YZ */

    FSUB(t1, t2, R->x);                   /* 4*beta - X3 */
    FMUL(t1, alpha, t1);
    FMUL(t3, gamma, gamma);
    FADD(t3, t3, t3);
    FADD(t3, t3, t3);
    FADD(t3, t3, t3);                     /* 8*gamma^2 */
    FSUB(R->y, t1, t3);

    R->inf = 0;
}

/* Add two points. Falls back to doubling when they are the same point, which
 * the addition formula cannot express. */
static void jadd(jpt *R, const jpt *P, const jpt *Q) {
    if (P->inf) { *R = *Q; return; }
    if (Q->inf) { *R = *P; return; }

    static bn_t z1z1, z2z2, u1, u2, s1, s2, h, i, j, r, v, t1, t2;

    FMUL(z1z1, P->z, P->z);
    FMUL(z2z2, Q->z, Q->z);
    FMUL(u1, P->x, z2z2);
    FMUL(u2, Q->x, z1z1);
    FMUL(t1, Q->z, z2z2);
    FMUL(s1, P->y, t1);
    FMUL(t1, P->z, z1z1);
    FMUL(s2, Q->y, t1);

    FSUB(h, u2, u1);
    FSUB(r, s2, s1);

    if (bn_is_zero(&h)) {
        if (bn_is_zero(&r)) { jdouble(R, P); return; }
        jzero(R);                          /* P + (-P) */
        return;
    }

    FADD(t1, h, h);
    FMUL(i, t1, t1);                       /* (2H)^2 */
    FMUL(j, h, i);
    FADD(r, r, r);                         /* 2*(S2-S1) */
    FMUL(v, u1, i);

    FMUL(t1, r, r);
    FSUB(t1, t1, j);
    FADD(t2, v, v);
    FSUB(R->x, t1, t2);

    FSUB(t1, v, R->x);
    FMUL(t1, r, t1);
    FMUL(t2, s1, j);
    FADD(t2, t2, t2);
    FSUB(R->y, t1, t2);

    FADD(t1, P->z, Q->z);
    FMUL(t1, t1, t1);
    FSUB(t1, t1, z1z1);
    FSUB(t1, t1, z2z2);
    FMUL(R->z, t1, h);

    R->inf = 0;
}

/* R = k * P, plainly, since everything here is public. */
static void jmul(jpt *R, const bn_t *k, const jpt *P) {
    static jpt acc, t;
    jzero(&acc);
    for (int i = CU->bytes * 8 - 1; i >= 0; i--) {
        jdouble(&t, &acc);
        acc = t;
        if (bn_bit(k, i)) { jadd(&t, &acc, P); acc = t; }
    }
    *R = acc;
}

/* --- DER, just enough for a signature ------------------------------------- */

/* SEQUENCE { INTEGER r, INTEGER s }. Returns 1 and points at the two values. */
static int parse_sig(const uint8_t *p, int len,
                     const uint8_t **r, int *rlen,
                     const uint8_t **s, int *slen) {
    if (len < 8 || p[0] != 0x30) return 0;
    int i = 1, seqlen;
    if (p[i] & 0x80) {
        int nb = p[i] & 0x7F;
        if (nb < 1 || nb > 2 || i + nb >= len) return 0;
        seqlen = 0;
        for (int k = 0; k < nb; k++) seqlen = (seqlen << 8) | p[++i];
        i++;
    } else {
        seqlen = p[i++];
    }
    if (i + seqlen > len) return 0;
    int end = i + seqlen;

    for (int which = 0; which < 2; which++) {
        if (i + 2 > end || p[i] != 0x02) return 0;
        int l = p[i + 1];
        if (l & 0x80) return 0;              /* never long-form at this size */
        i += 2;
        if (i + l > end || l == 0) return 0;
        if (which == 0) { *r = p + i; *rlen = l; }
        else            { *s = p + i; *slen = l; }
        i += l;
    }
    return 1;
}

/* --- verification --------------------------------------------------------- */

int ecdsa_verify(int curve, const uint8_t *pub, int publen,
                 const uint8_t *sig, int siglen,
                 const uint8_t *hash, int hashlen) {
    curves_init();
    if (curve != ECDSA_P256 && curve != ECDSA_P384) return 0;
    CU = &CURVES[curve == ECDSA_P256 ? 0 : 1];

    int w = CU->bytes;
    if (publen != 1 + 2 * w || pub[0] != 0x04) return 0;   /* uncompressed only */

    const uint8_t *rb, *sb;
    int rlen, slen;
    if (!parse_sig(sig, siglen, &rb, &rlen, &sb, &slen)) return 0;

    static bn_t r, s, e, w_inv, u1, u2, t;
    if (!bn_from_bytes(&r, rb, rlen)) return 0;
    if (!bn_from_bytes(&s, sb, slen)) return 0;
    if (bn_is_zero(&r) || bn_is_zero(&s)) return 0;
    if (bn_cmp(&r, &CU->n) >= 0 || bn_cmp(&s, &CU->n) >= 0) return 0;

    /* Only as much of the digest as the order is wide. Both curve orders are a
     * whole number of bytes, so this is a plain truncation from the left. */
    int use = hashlen > w ? w : hashlen;
    bn_from_bytes(&e, hash, use);
    if (bn_cmp(&e, &CU->n) >= 0) bn_mod_sub(&e, &e, &CU->n, &CU->n);

    bn_modexp(&w_inv, &s, CU->n2, w, &CU->mn);      /* s^-1 mod the order */

    static bn_t wm, em, rm, prod;
    bn_mont_in(&wm, &w_inv, &CU->mn);
    bn_mont_in(&em, &e, &CU->mn);
    bn_mont_in(&rm, &r, &CU->mn);
    bn_mont_mul(&prod, &em, &wm, &CU->mn);
    bn_mont_out(&u1, &prod, &CU->mn);
    bn_mont_mul(&prod, &rm, &wm, &CU->mn);
    bn_mont_out(&u2, &prod, &CU->mn);

    static jpt G, Q, A, B, R;
    static bn_t tx, ty;

    G.x = CU->gx_m; G.y = CU->gy_m; G.z = CU->one_m; G.inf = 0;

    if (!bn_from_bytes(&tx, pub + 1, w)) return 0;
    if (!bn_from_bytes(&ty, pub + 1 + w, w)) return 0;
    if (bn_cmp(&tx, &CU->p) >= 0 || bn_cmp(&ty, &CU->p) >= 0) return 0;
    bn_mont_in(&Q.x, &tx, &CU->mp);
    bn_mont_in(&Q.y, &ty, &CU->mp);
    Q.z = CU->one_m; Q.inf = 0;

    jmul(&A, &u1, &G);
    jmul(&B, &u2, &Q);
    jadd(&R, &A, &B);
    if (R.inf) return 0;

    /* Back to an affine x, then compare it with r modulo the order. */
    static bn_t zinv, zinv2, x;
    finv(&zinv, &R.z);
    FMUL(zinv2, zinv, zinv);
    FMUL(x, R.x, zinv2);
    bn_mont_out(&t, &x, &CU->mp);
    if (bn_cmp(&t, &CU->n) >= 0) bn_mod_sub(&t, &t, &CU->n, &CU->n);

    return bn_cmp(&t, &r) == 0;
}

int ecdsa_p256_verify_sha256(const uint8_t *pub, int publen,
                             const uint8_t *sig, int siglen,
                             const uint8_t hash[32]) {
    return ecdsa_verify(ECDSA_P256, pub, publen, sig, siglen, hash, 32);
}
