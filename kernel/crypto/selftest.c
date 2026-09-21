#include "crypto.h"
#include "../kernel/kio.h"
#include "vectors.h"

/* Known-answer tests, run once at boot.
 *
 * A cipher that is subtly wrong does not announce itself: the handshake simply
 * fails, or worse, succeeds against nothing. Every answer below was generated
 * by a reference implementation (tools/genvec.py), so this checks the code and
 * not somebody's memory of the standards. */

static int fails;

static void check(int ok, const char *what) {
    if (!ok) { fails++; kprintf("crypto: FAILED %s\n", what); }
}

int crypto_selftest(void) {
    uint8_t buf[64], tag[16], work[128];
    fails = 0;

    /* SHA-256 over a message that spans several blocks and ends part way
     * through one, which is where padding goes wrong. */
    sha256(v_sha_msg, sizeof v_sha_msg, buf);
    check(crypto_equal(buf, v_sha_out, 32), "sha256");

    /* The same message fed in awkward pieces must hash identically -- this is
     * what the handshake does with the running transcript. */
    {
        sha256_t s;
        sha256_init(&s);
        uint32_t off = 0, step = 1;
        while (off < sizeof v_sha_msg) {
            uint32_t n = step;
            if (off + n > sizeof v_sha_msg) n = (uint32_t)sizeof v_sha_msg - off;
            sha256_update(&s, v_sha_msg + off, n);
            off += n;
            step = step * 3 + 1;
            if (step > 200) step = 1;
        }
        sha256_final(&s, buf);
        check(crypto_equal(buf, v_sha_out, 32), "sha256 streaming");
    }

    hmac_sha256(v_hmac_key, sizeof v_hmac_key, v_hmac_msg, sizeof v_hmac_msg, buf);
    check(crypto_equal(buf, v_hmac_out, 32), "hmac-sha256");

    hkdf_expand_label(v_hkdf_secret, "key", v_hkdf_ctx, 32, buf, 16);
    check(crypto_equal(buf, v_hkdf_key, 16), "hkdf-expand-label 16");

    /* Longer than one hash output, so the counter loop is exercised. */
    hkdf_expand_label(v_hkdf_secret, "c ap traffic", v_hkdf_ctx, 32, buf, 48);
    check(crypto_equal(buf, v_hkdf_long, 48), "hkdf-expand-label 48");

    /* X25519 both ways round: each side's public key, and the shared secret
     * they must agree on. */
    x25519_base(buf, v_x_apriv);
    check(crypto_equal(buf, v_x_apub, 32), "x25519 public a");
    x25519_base(buf, v_x_bpriv);
    check(crypto_equal(buf, v_x_bpub, 32), "x25519 public b");
    x25519(buf, v_x_apriv, v_x_bpub);
    check(crypto_equal(buf, v_x_shared, 32), "x25519 shared a*B");
    x25519(buf, v_x_bpriv, v_x_apub);
    check(crypto_equal(buf, v_x_shared, 32), "x25519 shared b*A");

    /* AES-128-GCM against a reference ciphertext, with associated data and a
     * plaintext whose length is not a multiple of the block size. */
    {
        aes128gcm_t c;
        aes128gcm_init(&c, v_gcm_key);

        for (unsigned i = 0; i < sizeof v_gcm_pt; i++) work[i] = v_gcm_pt[i];
        aes128gcm_seal(&c, v_gcm_nonce, v_gcm_aad, sizeof v_gcm_aad,
                       work, sizeof v_gcm_pt, tag);
        check(crypto_equal(work, v_gcm_ct, sizeof v_gcm_ct), "aes-gcm ciphertext");
        check(crypto_equal(tag, v_gcm_tag, 16), "aes-gcm tag");

        /* And back again. */
        check(aes128gcm_open(&c, v_gcm_nonce, v_gcm_aad, sizeof v_gcm_aad,
                             work, sizeof v_gcm_pt, v_gcm_tag), "aes-gcm open");
        check(crypto_equal(work, v_gcm_pt, sizeof v_gcm_pt), "aes-gcm plaintext");

        /* A tag that does not match must be refused. If this ever passes, the
         * channel is unauthenticated and anyone can rewrite what we receive. */
        uint8_t bad[16];
        for (int i = 0; i < 16; i++) bad[i] = v_gcm_tag[i];
        bad[7] ^= 0x40;
        for (unsigned i = 0; i < sizeof v_gcm_pt; i++) work[i] = v_gcm_ct[i];
        check(!aes128gcm_open(&c, v_gcm_nonce, v_gcm_aad, sizeof v_gcm_aad,
                              work, sizeof v_gcm_pt, bad), "aes-gcm rejects a bad tag");

        /* Associated data is authenticated even though it is not encrypted. */
        uint8_t aad2[sizeof v_gcm_aad];
        for (unsigned i = 0; i < sizeof v_gcm_aad; i++) aad2[i] = v_gcm_aad[i];
        aad2[0] ^= 1;
        for (unsigned i = 0; i < sizeof v_gcm_pt; i++) work[i] = v_gcm_ct[i];
        check(!aes128gcm_open(&c, v_gcm_nonce, aad2, sizeof aad2,
                              work, sizeof v_gcm_pt, v_gcm_tag),
              "aes-gcm rejects altered aad");
    }

    /* The empty message: only a tag, and the one case where the length block
     * is all that GHASH sees. */
    {
        uint8_t key[16] = { 0 }, nonce[12] = { 0 };
        aes128gcm_t c;
        aes128gcm_init(&c, key);
        aes128gcm_seal(&c, nonce, 0, 0, work, 0, tag);
        check(crypto_equal(tag, v_gcm_empty_tag, 16), "aes-gcm empty");
    }

    /* --- signatures ------------------------------------------------------
     * Two things have to be true of a verifier, and only the first is
     * obvious: it must accept a genuine signature, and it must reject
     * everything else. A verifier that returns 1 unconditionally passes half
     * of any test written carelessly, so each check below has a matching one
     * with a signature that belongs to a different message. */
    {
        static uint8_t sig[512];

        check(rsa_verify_pkcs1_sha256(v_rsa_n, sizeof v_rsa_n, v_rsa_e, sizeof v_rsa_e,
                                      v_rsa_pkcs1, sizeof v_rsa_pkcs1, v_sig_hash),
              "rsa pkcs1 accepts a good signature");
        check(!rsa_verify_pkcs1_sha256(v_rsa_n, sizeof v_rsa_n, v_rsa_e, sizeof v_rsa_e,
                                       v_rsa_pkcs1, sizeof v_rsa_pkcs1, v_sig_hash2),
              "rsa pkcs1 rejects the wrong message");

        for (unsigned i = 0; i < sizeof v_rsa_pkcs1; i++) sig[i] = v_rsa_pkcs1[i];
        sig[100] ^= 0x01;
        check(!rsa_verify_pkcs1_sha256(v_rsa_n, sizeof v_rsa_n, v_rsa_e, sizeof v_rsa_e,
                                       sig, sizeof v_rsa_pkcs1, v_sig_hash),
              "rsa pkcs1 rejects a tampered signature");

        check(rsa_verify_pss_sha256(v_rsa_n, sizeof v_rsa_n, v_rsa_e, sizeof v_rsa_e,
                                    v_rsa_pss, sizeof v_rsa_pss, v_sig_hash),
              "rsa pss accepts a good signature");
        check(rsa_verify_pss_sha256(v_rsa_n, sizeof v_rsa_n, v_rsa_e, sizeof v_rsa_e,
                                    v_rsa_pss2, sizeof v_rsa_pss2, v_sig_hash2),
              "rsa pss accepts a second one");
        check(!rsa_verify_pss_sha256(v_rsa_n, sizeof v_rsa_n, v_rsa_e, sizeof v_rsa_e,
                                     v_rsa_pss, sizeof v_rsa_pss, v_sig_hash2),
              "rsa pss rejects the wrong message");

        check(ecdsa_p256_verify_sha256(v_ec_pub, sizeof v_ec_pub,
                                       v_ec_sig, sizeof v_ec_sig, v_sig_hash),
              "ecdsa p256 accepts a good signature");
        check(ecdsa_p256_verify_sha256(v_ec_pub, sizeof v_ec_pub,
                                       v_ec_sig2, sizeof v_ec_sig2, v_sig_hash2),
              "ecdsa p256 accepts a second one");
        check(!ecdsa_p256_verify_sha256(v_ec_pub, sizeof v_ec_pub,
                                        v_ec_sig, sizeof v_ec_sig, v_sig_hash2),
              "ecdsa p256 rejects the wrong message");
        check(!ecdsa_p256_verify_sha256(v_ec_pub, sizeof v_ec_pub,
                                        v_ec_sig2, sizeof v_ec_sig2, v_sig_hash),
              "ecdsa p256 rejects a swapped signature");
    }

    if (!fails) kprintf("crypto: self-test ok\n");
    return fails == 0;
}
