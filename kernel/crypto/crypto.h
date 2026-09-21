#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdint.h>

/* The cryptography a TLS 1.3 client needs, and nothing else.
 *
 * There is no general-purpose crypto library here on purpose: every function
 * below exists because the handshake in kernel/net/tls.c calls it. The choice
 * of algorithms is not ours either -- TLS_AES_128_GCM_SHA256 with X25519 key
 * exchange is the suite every server must implement, so implementing exactly
 * that guarantees we can talk to all of them.
 *
 * None of this is hardened against an attacker who can measure how long it
 * takes. The scalar multiplication and the AES round function are written the
 * plain way, and a machine sitting on the same network could in principle
 * learn something from the timing. That is an acceptable trade here and a bad
 * one anywhere that matters; it is written down so nobody has to guess. */

/* --- SHA-256 ------------------------------------------------------------ */

typedef struct {
    uint32_t h[8];
    uint64_t len;          /* total bytes fed in */
    uint8_t  buf[64];
    int      n;            /* bytes currently in buf */
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, uint32_t len);
void sha256_final(sha256_t *s, uint8_t out[32]);
void sha256(const void *data, uint32_t len, uint8_t out[32]);

/* The handshake needs the hash of everything sent so far, repeatedly, while
 * still adding to it -- so the state is copied rather than finished. */
void sha256_peek(const sha256_t *s, uint8_t out[32]);

/* --- SHA-512 and SHA-384 -------------------------------------------------
 * Not used by TLS itself, which is all SHA-256 here, but certificate
 * authorities sign with SHA-384 constantly and a chain cannot be checked
 * without it. */

typedef struct {
    uint64_t h[8];
    uint64_t len;
    uint8_t  buf[128];
    int      n;
    int      outlen;       /* 64 for SHA-512, 48 for SHA-384 */
} sha512_t;

void sha512_init(sha512_t *s);
void sha384_init(sha512_t *s);
void sha512_update(sha512_t *s, const void *data, uint32_t len);
void sha512_final(sha512_t *s, uint8_t *out);
void sha512(const void *data, uint32_t len, uint8_t out[64]);
void sha384(const void *data, uint32_t len, uint8_t out[48]);

/* --- HMAC and HKDF (RFC 5869), plus TLS 1.3's labelled form -------------- */

void hmac_sha256(const uint8_t *key, uint32_t keylen,
                 const void *data, uint32_t datalen, uint8_t out[32]);

void hkdf_extract(const uint8_t *salt, uint32_t saltlen,
                  const uint8_t *ikm, uint32_t ikmlen, uint8_t out[32]);
void hkdf_expand(const uint8_t prk[32], const uint8_t *info, uint32_t infolen,
                 uint8_t *out, uint32_t outlen);

/* HKDF-Expand-Label from RFC 8446 5.3: the label is prefixed with "tls13 "
 * and packed with the context into the info string. */
void hkdf_expand_label(const uint8_t secret[32], const char *label,
                       const uint8_t *ctx, uint32_t ctxlen,
                       uint8_t *out, uint32_t outlen);

/* Derive-Secret(secret, label, transcript-hash) */
void tls13_derive_secret(const uint8_t secret[32], const char *label,
                         const uint8_t hash[32], uint8_t out[32]);

/* --- X25519 (RFC 7748) --------------------------------------------------- */

/* out = scalar * point. Both are 32 bytes, little endian. */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
/* out = scalar * basepoint(9) -- the public half of a key pair. */
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

/* --- AES-128-GCM --------------------------------------------------------- */

typedef struct {
    uint32_t rk[44];       /* 11 round keys of 4 words */
    uint8_t  h[16];        /* GHASH subkey = E(0) */
} aes128gcm_t;

void aes128gcm_init(aes128gcm_t *c, const uint8_t key[16]);

/* Encrypt in place and append nothing -- the caller owns the tag buffer.
 * `nonce` is 12 bytes, as TLS always uses. */
void aes128gcm_seal(aes128gcm_t *c, const uint8_t nonce[12],
                    const uint8_t *aad, uint32_t aadlen,
                    uint8_t *data, uint32_t len, uint8_t tag[16]);

/* Decrypt in place. Returns 1 if the tag matched, 0 if it did not -- in which
 * case the plaintext must be treated as if it had never arrived. */
int aes128gcm_open(aes128gcm_t *c, const uint8_t nonce[12],
                   const uint8_t *aad, uint32_t aadlen,
                   uint8_t *data, uint32_t len, const uint8_t tag[16]);

/* --- big integers, for RSA ----------------------------------------------- */

/* Wide enough for a 4096-bit modulus, which is the largest anybody issues. */
#define BN_MAX_LIMBS 64

typedef struct {
    int      n;                  /* limbs in use, least significant first */
    uint64_t v[BN_MAX_LIMBS];
} bn_t;

typedef struct {
    bn_t     n;                  /* the modulus */
    bn_t     rr;                 /* R^2 mod n, for entering Montgomery form */
    uint64_t n0;                 /* -n^-1 mod 2^64 */
} bn_mont_t;

int  bn_from_bytes(bn_t *a, const uint8_t *be, int len);   /* 0 if too large */
void bn_to_bytes(const bn_t *a, uint8_t *be, int len);
int  bn_bytes(const bn_t *a);
int  bn_cmp(const bn_t *a, const bn_t *b);
int  bn_mont_init(bn_mont_t *m, const bn_t *n);            /* 0 if n is even */
void bn_modexp(bn_t *r, const bn_t *base, const uint8_t *exp, int explen,
               const bn_mont_t *m);
void bn_mont_mul(bn_t *r, const bn_t *a, const bn_t *b, const bn_mont_t *m);
void bn_mont_in(bn_t *r, const bn_t *a, const bn_mont_t *m);
void bn_mont_out(bn_t *r, const bn_t *a, const bn_mont_t *m);
void bn_mod_add(bn_t *r, const bn_t *a, const bn_t *b, const bn_t *n);
void bn_mod_sub(bn_t *r, const bn_t *a, const bn_t *b, const bn_t *n);
void bn_set_u64(bn_t *a, uint64_t v);
int  bn_is_zero(const bn_t *a);
int  bn_bit(const bn_t *a, int i);

/* --- RSA signature verification ------------------------------------------ */

/* Both take the modulus and exponent as they appear in a certificate: big
 * endian, possibly with a leading zero byte. `hash` is a SHA-256 digest of
 * whatever was signed. Return 1 only if the signature is genuine. */
int rsa_verify_pkcs1(const uint8_t *n, int nlen, const uint8_t *e, int elen,
                     const uint8_t *sig, int siglen,
                     const uint8_t *hash, int hashlen);   /* 32, 48 or 64 */
int rsa_verify_pkcs1_sha256(const uint8_t *n, int nlen, const uint8_t *e, int elen,
                            const uint8_t *sig, int siglen, const uint8_t hash[32]);
int rsa_verify_pss_sha256(const uint8_t *n, int nlen, const uint8_t *e, int elen,
                          const uint8_t *sig, int siglen, const uint8_t hash[32]);

/* --- ECDSA over P-256 and P-384 ------------------------------------------ */

/* `pub` is the uncompressed point from a certificate: 0x04 then X then Y.
 * `sig` is the DER SEQUENCE of two INTEGERs that X.509 and TLS both carry.
 * `hash` may be any length; only as much of it as the curve's order is wide
 * gets used, which is what the standard calls for. Returns 1 only if the
 * signature is genuine. */
#define ECDSA_P256 0
#define ECDSA_P384 1

int ecdsa_verify(int curve, const uint8_t *pub, int publen,
                 const uint8_t *sig, int siglen,
                 const uint8_t *hash, int hashlen);

/* The common case, spelled out. */
int ecdsa_p256_verify_sha256(const uint8_t *pub, int publen,
                             const uint8_t *sig, int siglen,
                             const uint8_t hash[32]);

/* --- odds and ends ------------------------------------------------------- */

/* Comparison that does not stop at the first difference. */
int crypto_equal(const void *a, const void *b, uint32_t len);

/* Random bytes for key material. */
void crypto_random(void *out, uint32_t len);

/* Run every known-answer test. Returns 1 if they all pass. Called once at
 * boot: a cipher that is subtly wrong fails in ways that look like network
 * problems, and this turns that into one line in the log. */
int crypto_selftest(void);

#endif
