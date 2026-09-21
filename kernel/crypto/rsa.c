#include "crypto.h"

/* RSA signature verification, in the two paddings TLS actually presents.
 *
 * A certificate chain is signed with PKCS#1 v1.5 more often than not, because
 * roots and intermediates are long-lived and were issued that way. The
 * CertificateVerify message in a TLS 1.3 handshake, by contrast, may only use
 * PSS -- RFC 8446 removed v1.5 from the handshake entirely. So both are here,
 * and each is used in exactly one place.
 *
 * Verification never reveals anything: the key is public and the signature is
 * public. What matters is that nothing is accepted by accident, so both
 * routines check every byte of the padding rather than skipping to the hash. */

static void rmemcpy(void *d, const void *s, int n) {
    uint8_t *a = d; const uint8_t *b = s;
    for (int i = 0; i < n; i++) a[i] = b[i];
}

/* s^e mod n, with the result written out to exactly `outlen` bytes. */
static int rsa_public(const uint8_t *n_be, int nlen,
                      const uint8_t *e_be, int elen,
                      const uint8_t *sig, int siglen,
                      uint8_t *out, int outlen) {
    /* Kilobytes of scratch, and a kernel stack measured in a few of them.
     * Only one verification is ever in flight. */
    static bn_t n, s, r;
    static bn_mont_t m;

    if (!bn_from_bytes(&n, n_be, nlen)) return 0;
    if (!bn_from_bytes(&s, sig, siglen)) return 0;
    if (bn_cmp(&s, &n) >= 0) return 0;               /* signature out of range */
    if (!bn_mont_init(&m, &n)) return 0;

    bn_modexp(&r, &s, e_be, elen, &m);
    bn_to_bytes(&r, out, outlen);
    return 1;
}

/* --- PKCS#1 v1.5 --------------------------------------------------------- */

/* EM = 0x00 || 0x01 || 0xFF...FF || 0x00 || DigestInfo(hash algorithm, hash).
 * The DigestInfo is a fixed nineteen-byte preamble naming the hash; the only
 * difference between the three sizes is two bytes inside it. */
static const uint8_t di_sha256[19] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
    0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20,
};
static const uint8_t di_sha384[19] = {
    0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
    0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30,
};
static const uint8_t di_sha512[19] = {
    0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
    0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40,
};

int rsa_verify_pkcs1(const uint8_t *n, int nlen, const uint8_t *e, int elen,
                     const uint8_t *sig, int siglen,
                     const uint8_t *hash, int hashlen) {
    static uint8_t em[BN_MAX_LIMBS * 8];
    const uint8_t *di;

    if      (hashlen == 32) di = di_sha256;
    else if (hashlen == 48) di = di_sha384;
    else if (hashlen == 64) di = di_sha512;
    else return 0;

    int k = nlen;
    while (k > 0 && n[0] == 0) { n++; k--; }         /* the modulus size in bytes */
    if (k < 11 + 19 + hashlen || k > (int)sizeof em) return 0;
    if (siglen != k) return 0;
    if (!rsa_public(n, k, e, elen, sig, siglen, em, k)) return 0;

    if (em[0] != 0x00 || em[1] != 0x01) return 0;
    int i = 2;
    while (i < k && em[i] == 0xFF) i++;
    if (i - 2 < 8) return 0;                         /* the pad has a minimum */
    if (i >= k || em[i] != 0x00) return 0;
    i++;

    if (k - i != 19 + hashlen) return 0;
    if (!crypto_equal(em + i, di, 19)) return 0;
    return crypto_equal(em + i + 19, hash, hashlen);
}

int rsa_verify_pkcs1_sha256(const uint8_t *n, int nlen, const uint8_t *e, int elen,
                            const uint8_t *sig, int siglen, const uint8_t hash[32]) {
    return rsa_verify_pkcs1(n, nlen, e, elen, sig, siglen, hash, 32);
}

/* --- PSS ----------------------------------------------------------------- */

/* MGF1 with SHA-256: the mask is the hash of the seed with a counter appended,
 * repeated until there is enough of it. */
static void mgf1(const uint8_t *seed, int seedlen, uint8_t *mask, int masklen) {
    uint8_t buf[64 + 4], digest[32];
    int done = 0;
    for (uint32_t counter = 0; done < masklen; counter++) {
        int n = 0;
        for (int i = 0; i < seedlen && i < 64; i++) buf[n++] = seed[i];
        buf[n++] = (uint8_t)(counter >> 24);
        buf[n++] = (uint8_t)(counter >> 16);
        buf[n++] = (uint8_t)(counter >> 8);
        buf[n++] = (uint8_t)counter;
        sha256(buf, (uint32_t)n, digest);
        for (int i = 0; i < 32 && done < masklen; i++) mask[done++] = digest[i];
    }
}

/* RSASSA-PSS with SHA-256 everywhere and a salt the same length as the hash,
 * which is what rsa_pss_rsae_sha256 means. */
int rsa_verify_pss_sha256(const uint8_t *n, int nlen, const uint8_t *e, int elen,
                          const uint8_t *sig, int siglen, const uint8_t hash[32]) {
    static uint8_t em[BN_MAX_LIMBS * 8], db_mask[BN_MAX_LIMBS * 8];
    uint8_t mprime[8 + 32 + 32], h2[32];
    const int hlen = 32, slen = 32;

    int k = nlen;
    while (k > 0 && n[0] == 0) { n++; k--; }
    if (k < hlen + slen + 2 || k > (int)sizeof em) return 0;
    if (siglen != k) return 0;
    if (!rsa_public(n, k, e, elen, sig, siglen, em, k)) return 0;

    if (em[k - 1] != 0xBC) return 0;

    int emlen = k;
    int embits = 0;
    {   /* The encoded message is one bit shorter than the modulus, so the top
         * byte carries a bit that must be zero. */
        uint8_t top = n[0];
        int bits = 0;
        while (top) { bits++; top >>= 1; }
        embits = (k - 1) * 8 + bits - 1;
    }
    int maskedlen = emlen - hlen - 1;
    int spare = emlen * 8 - embits;                  /* leading bits to be zero */
    if (spare < 0 || spare > 7) return 0;
    if (spare && (em[0] >> (8 - spare))) return 0;

    const uint8_t *h = em + maskedlen;
    mgf1(h, hlen, db_mask, maskedlen);
    for (int i = 0; i < maskedlen; i++) db_mask[i] ^= em[i];
    if (spare) db_mask[0] &= (uint8_t)(0xFF >> spare);

    /* DB is zeros, then a single 0x01, then the salt. */
    int i = 0;
    while (i < maskedlen - slen - 1 && db_mask[i] == 0) i++;
    if (i != maskedlen - slen - 1) return 0;
    if (db_mask[i] != 0x01) return 0;
    const uint8_t *salt = db_mask + i + 1;

    /* H is the hash of eight zero bytes, the message hash, and the salt. */
    for (int j = 0; j < 8; j++) mprime[j] = 0;
    rmemcpy(mprime + 8, hash, hlen);
    rmemcpy(mprime + 8 + hlen, salt, slen);
    sha256(mprime, (uint32_t)(8 + hlen + slen), h2);

    return crypto_equal(h2, h, hlen);
}
