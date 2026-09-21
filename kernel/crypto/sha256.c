#include "crypto.h"

/* SHA-256, FIPS 180-4, written the way the specification reads. */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void block(sha256_t *s, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void sha256_init(sha256_t *s) {
    s->h[0] = 0x6a09e667; s->h[1] = 0xbb67ae85;
    s->h[2] = 0x3c6ef372; s->h[3] = 0xa54ff53a;
    s->h[4] = 0x510e527f; s->h[5] = 0x9b05688c;
    s->h[6] = 0x1f83d9ab; s->h[7] = 0x5be0cd19;
    s->len = 0;
    s->n = 0;
}

void sha256_update(sha256_t *s, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    s->len += len;
    while (len) {
        if (s->n == 0 && len >= 64) {           /* straight through */
            block(s, p);
            p += 64; len -= 64;
            continue;
        }
        uint32_t take = 64 - (uint32_t)s->n;
        if (take > len) take = len;
        for (uint32_t i = 0; i < take; i++) s->buf[s->n + (int)i] = p[i];
        s->n += (int)take; p += take; len -= take;
        if (s->n == 64) { block(s, s->buf); s->n = 0; }
    }
}

void sha256_final(sha256_t *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    uint8_t zero = 0;
    while (s->n != 56) sha256_update(s, &zero, 1);
    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) len_be[i] = (uint8_t)(bits >> (56 - i * 8));
    sha256_update(s, len_be, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)s->h[i];
    }
}

void sha256_peek(const sha256_t *s, uint8_t out[32]) {
    sha256_t copy = *s;
    sha256_final(&copy, out);
}

void sha256(const void *data, uint32_t len, uint8_t out[32]) {
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, data, len);
    sha256_final(&s, out);
}

/* --- HMAC (RFC 2104) ----------------------------------------------------- */

void hmac_sha256(const uint8_t *key, uint32_t keylen,
                 const void *data, uint32_t datalen, uint8_t out[32]) {
    uint8_t k[64], pad[64], inner[32];
    uint32_t i;

    for (i = 0; i < 64; i++) k[i] = 0;
    if (keylen > 64) sha256(key, keylen, k);
    else for (i = 0; i < keylen; i++) k[i] = key[i];

    sha256_t s;
    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&s);
    sha256_update(&s, pad, 64);
    sha256_update(&s, data, datalen);
    sha256_final(&s, inner);

    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&s);
    sha256_update(&s, pad, 64);
    sha256_update(&s, inner, 32);
    sha256_final(&s, out);
}

/* --- HKDF (RFC 5869) ----------------------------------------------------- */

void hkdf_extract(const uint8_t *salt, uint32_t saltlen,
                  const uint8_t *ikm, uint32_t ikmlen, uint8_t out[32]) {
    /* Extract is HMAC with the salt as the key -- an all-zero key when there
     * is no salt, which RFC 5869 spells out and TLS relies on. */
    static const uint8_t zeros[32] = { 0 };
    if (!salt || !saltlen) { salt = zeros; saltlen = 32; }
    hmac_sha256(salt, saltlen, ikm, ikmlen, out);
}

void hkdf_expand(const uint8_t prk[32], const uint8_t *info, uint32_t infolen,
                 uint8_t *out, uint32_t outlen) {
    uint8_t t[32], buf[32 + 256 + 1];
    uint32_t tlen = 0, done = 0;
    uint8_t counter = 1;

    while (done < outlen) {
        uint32_t n = 0;
        for (uint32_t i = 0; i < tlen; i++) buf[n++] = t[i];
        for (uint32_t i = 0; i < infolen && i < 256; i++) buf[n++] = info[i];
        buf[n++] = counter++;
        hmac_sha256(prk, 32, buf, n, t);
        tlen = 32;
        for (uint32_t i = 0; i < 32 && done < outlen; i++) out[done++] = t[i];
    }
}

/* --- the TLS 1.3 wrappers (RFC 8446 7.1) --------------------------------- */

static uint32_t cstrlen(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

void hkdf_expand_label(const uint8_t secret[32], const char *label,
                       const uint8_t *ctx, uint32_t ctxlen,
                       uint8_t *out, uint32_t outlen) {
    /* struct {
     *     uint16 length;
     *     opaque label<7..255>;      -- always prefixed with "tls13 "
     *     opaque context<0..255>;
     * } HkdfLabel;                                                          */
    uint8_t info[2 + 1 + 6 + 64 + 1 + 64];
    uint32_t n = 0, ll = cstrlen(label);
    if (ll > 64) ll = 64;
    if (ctxlen > 64) ctxlen = 64;

    info[n++] = (uint8_t)(outlen >> 8);
    info[n++] = (uint8_t)outlen;
    info[n++] = (uint8_t)(6 + ll);
    const char *pre = "tls13 ";
    for (int i = 0; i < 6; i++) info[n++] = (uint8_t)pre[i];
    for (uint32_t i = 0; i < ll; i++) info[n++] = (uint8_t)label[i];
    info[n++] = (uint8_t)ctxlen;
    for (uint32_t i = 0; i < ctxlen; i++) info[n++] = ctx[i];

    hkdf_expand(secret, info, n, out, outlen);
}

void tls13_derive_secret(const uint8_t secret[32], const char *label,
                         const uint8_t hash[32], uint8_t out[32]) {
    hkdf_expand_label(secret, label, hash, 32, out, 32);
}

/* --- odds and ends ------------------------------------------------------- */

int crypto_equal(const void *a, const void *b, uint32_t len) {
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    uint8_t diff = 0;
    for (uint32_t i = 0; i < len; i++) diff |= (uint8_t)(x[i] ^ y[i]);
    return diff == 0;
}
