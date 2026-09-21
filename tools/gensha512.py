#!/usr/bin/env python3
"""Emit the SHA-512 tables. Generated rather than typed: eighty 64-bit
constants copied by hand is eighty chances to introduce a bug that shows up as
'the signature does not verify' and nothing else."""
from decimal import Decimal, getcontext
getcontext().prec = 80

def primes(n):
    out, x = [], 2
    while len(out) < n:
        if all(x % p for p in out if p * p <= x):
            out.append(x)
        x += 1
    return out

def frac64(v, root):
    """First 64 bits of the fractional part of v ** (1/root)."""
    x = Decimal(v) ** (Decimal(1) / Decimal(root))
    f = x - int(x)
    return int(f * (Decimal(2) ** 64))

P = primes(80)
K = [frac64(p, 3) for p in P]
H512 = [frac64(p, 2) for p in P[:8]]
H384 = [frac64(p, 2) for p in P[8:16]]

def table(name, vals, per=2):
    lines = ["static const uint64_t %s[%d] = {" % (name, len(vals))]
    row = "   "
    for v in vals:
        tok = " 0x%016xULL," % v
        if len(row) + len(tok) > 76:
            lines.append(row); row = "   "
        row += tok
    lines.append(row)
    lines.append("};")
    return "\n".join(lines)

body = """#include "crypto.h"

/* SHA-512, and SHA-384 which is the same function with a different starting
 * point and a shorter answer.
 *
 * Needed because certificate authorities sign with SHA-384far more often than
 * the hash TLS itself uses. The structure is SHA-256's with sixty-four-bit
 * words, different rotation amounts and eighty rounds instead of sixty-four.
 *
 * The tables below are generated (tools/gensha512.py) from the definition --
 * the fractional parts of the cube and square roots of the small primes --
 * rather than copied, because a single mistyped digit would show up only as
 * signatures that mysteriously fail to verify. */

%s

%s

%s

static inline uint64_t ror64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static void block512(sha512_t *s, const uint8_t *p) {
    uint64_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = 0;
        for (int j = 0; j < 8; j++) w[i] = (w[i] << 8) | p[i * 8 + j];
    }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ror64(w[i-15], 1) ^ ror64(w[i-15], 8) ^ (w[i-15] >> 7);
        uint64_t s1 = ror64(w[i-2], 19) ^ ror64(w[i-2], 61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    uint64_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint64_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];

    for (int i = 0; i < 80; i++) {
        uint64_t S1 = ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + K512[i] + w[i];
        uint64_t S0 = ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39);
        uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void init_with(sha512_t *s, const uint64_t iv[8], int outlen) {
    for (int i = 0; i < 8; i++) s->h[i] = iv[i];
    s->len = 0;
    s->n = 0;
    s->outlen = outlen;
}

void sha512_init(sha512_t *s) { init_with(s, H512, 64); }
void sha384_init(sha512_t *s) { init_with(s, H384, 48); }

void sha512_update(sha512_t *s, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    s->len += len;
    while (len) {
        if (s->n == 0 && len >= 128) { block512(s, p); p += 128; len -= 128; continue; }
        uint32_t take = 128 - (uint32_t)s->n;
        if (take > len) take = len;
        for (uint32_t i = 0; i < take; i++) s->buf[s->n + (int)i] = p[i];
        s->n += (int)take; p += take; len -= take;
        if (s->n == 128) { block512(s, s->buf); s->n = 0; }
    }
}

void sha512_final(sha512_t *s, uint8_t *out) {
    /* The length is a 128-bit field; nothing here is anywhere near 2^64 bytes,
     * so the top half is always zero. */
    uint64_t bits = s->len * 8;
    int outlen = s->outlen;
    uint8_t pad = 0x80, zero = 0;
    sha512_update(s, &pad, 1);
    while (s->n != 112) sha512_update(s, &zero, 1);
    uint8_t len_be[16];
    for (int i = 0; i < 8; i++) len_be[i] = 0;
    for (int i = 0; i < 8; i++) len_be[8 + i] = (uint8_t)(bits >> (56 - i * 8));
    sha512_update(s, len_be, 16);

    for (int i = 0; i < 8 && i * 8 < outlen; i++)
        for (int j = 0; j < 8 && i * 8 + j < outlen; j++)
            out[i * 8 + j] = (uint8_t)(s->h[i] >> (56 - j * 8));
}

void sha512(const void *data, uint32_t len, uint8_t out[64]) {
    sha512_t s;
    sha512_init(&s);
    sha512_update(&s, data, len);
    sha512_final(&s, out);
}

void sha384(const void *data, uint32_t len, uint8_t out[48]) {
    sha512_t s;
    sha384_init(&s);
    sha512_update(&s, data, len);
    sha512_final(&s, out);
}
""" % (table("K512", K), table("H512", H512), table("H384", H384))

open(XYUOS + "/kernel/crypto/sha512.c", "w").write(body)
print("wrote sha512.c")

# A quick sanity check against the host's own implementation.
import hashlib
import os

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))
print("sha384('abc') =", hashlib.sha384(b"abc").hexdigest()[:32], "...")
