#include "crypto.h"

/* AES-128 in GCM mode -- the one cipher suite every TLS 1.3 implementation is
 * required to support, which is why it is the one we implement.
 *
 * The AES here is the textbook byte-oriented version: a substitution table, a
 * row shift and a column mix, done a round at a time. No T-tables, because
 * they trade a table lookup pattern (which leaks through the cache) for speed
 * we do not need. GHASH multiplies bit by bit for the same reason -- it is
 * 128 shifts per block and still far faster than the network it protects. */

/* --- AES ----------------------------------------------------------------- */

static const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static const uint8_t rcon[10] = { 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };

static inline uint32_t sub_word(uint32_t w) {
    return ((uint32_t)sbox[(w >> 24) & 0xff] << 24) |
           ((uint32_t)sbox[(w >> 16) & 0xff] << 16) |
           ((uint32_t)sbox[(w >> 8) & 0xff] << 8) |
           (uint32_t)sbox[w & 0xff];
}
static inline uint32_t rot_word(uint32_t w) { return (w << 8) | (w >> 24); }

static void key_expand(uint32_t rk[44], const uint8_t key[16]) {
    for (int i = 0; i < 4; i++)
        rk[i] = ((uint32_t)key[4 * i] << 24) | ((uint32_t)key[4 * i + 1] << 16) |
                ((uint32_t)key[4 * i + 2] << 8) | (uint32_t)key[4 * i + 3];
    for (int i = 4; i < 44; i++) {
        uint32_t t = rk[i - 1];
        if (i % 4 == 0) t = sub_word(rot_word(t)) ^ ((uint32_t)rcon[i / 4 - 1] << 24);
        rk[i] = rk[i - 4] ^ t;
    }
}

/* Multiply by x in GF(2^8) with the AES polynomial. */
static inline uint8_t xtime(uint8_t a) {
    return (uint8_t)((a << 1) ^ ((a >> 7) * 0x1b));
}

static void encrypt_block(const uint32_t rk[44], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i];

    /* AddRoundKey, in the column-major order AES states are written in. */
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            s[c * 4 + r] ^= (uint8_t)(rk[c] >> (24 - r * 8));

    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];

        /* ShiftRows: row r rotates left by r. */
        uint8_t t[16];
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                t[c * 4 + r] = s[((c + r) & 3) * 4 + r];
        for (int i = 0; i < 16; i++) s[i] = t[i];

        if (round != 10) {                      /* MixColumns, all but the last */
            for (int c = 0; c < 4; c++) {
                uint8_t *p = s + c * 4;
                uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                uint8_t x = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                p[0] = (uint8_t)(a0 ^ x ^ xtime((uint8_t)(a0 ^ a1)));
                p[1] = (uint8_t)(a1 ^ x ^ xtime((uint8_t)(a1 ^ a2)));
                p[2] = (uint8_t)(a2 ^ x ^ xtime((uint8_t)(a2 ^ a3)));
                p[3] = (uint8_t)(a3 ^ x ^ xtime((uint8_t)(a3 ^ a0)));
            }
        }

        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                s[c * 4 + r] ^= (uint8_t)(rk[round * 4 + c] >> (24 - r * 8));
    }

    for (int i = 0; i < 16; i++) out[i] = s[i];
}

/* --- GHASH --------------------------------------------------------------- */

/* Multiplication in GF(2^128) with the reversed-bit convention GCM uses:
 * bit 0 of byte 0 is the highest-order coefficient. */
static void ghash_mul(uint8_t x[16], const uint8_t h[16]) {
    uint8_t z[16] = { 0 }, v[16];
    for (int i = 0; i < 16; i++) v[i] = h[i];

    for (int i = 0; i < 128; i++) {
        if ((x[i >> 3] >> (7 - (i & 7))) & 1)
            for (int j = 0; j < 16; j++) z[j] ^= v[j];

        int lsb = v[15] & 1;
        for (int j = 15; j > 0; j--) v[j] = (uint8_t)((v[j] >> 1) | (v[j - 1] << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xe1;                  /* the reduction polynomial */
    }
    for (int i = 0; i < 16; i++) x[i] = z[i];
}

static void ghash_blocks(uint8_t y[16], const uint8_t h[16],
                         const uint8_t *data, uint32_t len) {
    uint32_t off = 0;
    while (off < len) {
        uint32_t n = len - off;
        if (n > 16) n = 16;
        for (uint32_t i = 0; i < n; i++) y[i] ^= data[off + i];
        /* A short final block is padded with zeros, which the xor above
         * already leaves untouched. */
        ghash_mul(y, h);
        off += n;
    }
}

/* --- GCM ----------------------------------------------------------------- */

void aes128gcm_init(aes128gcm_t *c, const uint8_t key[16]) {
    uint8_t zero[16] = { 0 };
    key_expand(c->rk, key);
    encrypt_block(c->rk, zero, c->h);
}

/* The counter block for a 12-byte nonce is nonce || counter, big endian. */
static void ctr_block(uint8_t out[16], const uint8_t nonce[12], uint32_t counter) {
    for (int i = 0; i < 12; i++) out[i] = nonce[i];
    out[12] = (uint8_t)(counter >> 24);
    out[13] = (uint8_t)(counter >> 16);
    out[14] = (uint8_t)(counter >> 8);
    out[15] = (uint8_t)counter;
}

static void gcm_crypt(aes128gcm_t *c, const uint8_t nonce[12],
                      uint8_t *data, uint32_t len) {
    uint8_t cb[16], ks[16];
    uint32_t counter = 2;                       /* 1 is reserved for the tag */
    for (uint32_t off = 0; off < len; off += 16) {
        ctr_block(cb, nonce, counter++);
        encrypt_block(c->rk, cb, ks);
        uint32_t n = len - off;
        if (n > 16) n = 16;
        for (uint32_t i = 0; i < n; i++) data[off + i] ^= ks[i];
    }
}

static void gcm_tag(aes128gcm_t *c, const uint8_t nonce[12],
                    const uint8_t *aad, uint32_t aadlen,
                    const uint8_t *ct, uint32_t ctlen, uint8_t tag[16]) {
    uint8_t y[16] = { 0 }, cb[16], ek[16], lenblk[16];

    ghash_blocks(y, c->h, aad, aadlen);
    ghash_blocks(y, c->h, ct, ctlen);

    /* The length block is the two bit counts, each 64 bits big endian. */
    uint64_t abits = (uint64_t)aadlen * 8, cbits = (uint64_t)ctlen * 8;
    for (int i = 0; i < 8; i++) lenblk[i]     = (uint8_t)(abits >> (56 - i * 8));
    for (int i = 0; i < 8; i++) lenblk[8 + i] = (uint8_t)(cbits >> (56 - i * 8));
    for (int i = 0; i < 16; i++) y[i] ^= lenblk[i];
    ghash_mul(y, c->h);

    ctr_block(cb, nonce, 1);
    encrypt_block(c->rk, cb, ek);
    for (int i = 0; i < 16; i++) tag[i] = (uint8_t)(y[i] ^ ek[i]);
}

void aes128gcm_seal(aes128gcm_t *c, const uint8_t nonce[12],
                    const uint8_t *aad, uint32_t aadlen,
                    uint8_t *data, uint32_t len, uint8_t tag[16]) {
    gcm_crypt(c, nonce, data, len);
    gcm_tag(c, nonce, aad, aadlen, data, len, tag);
}

int aes128gcm_open(aes128gcm_t *c, const uint8_t nonce[12],
                   const uint8_t *aad, uint32_t aadlen,
                   uint8_t *data, uint32_t len, const uint8_t tag[16]) {
    uint8_t want[16];
    /* The tag covers the ciphertext, so it has to be checked before the data
     * is turned back into plaintext -- and the caller must ignore the buffer
     * entirely when this returns 0. */
    gcm_tag(c, nonce, aad, aadlen, data, len, want);
    if (!crypto_equal(want, tag, 16)) return 0;
    gcm_crypt(c, nonce, data, len);
    return 1;
}
