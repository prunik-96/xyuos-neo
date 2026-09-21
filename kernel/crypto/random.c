#include "crypto.h"
#include "../arch/x86_64/pit.h"

/* Where key material comes from.
 *
 * The processor's own generator is asked first: RDSEED and RDRAND are the only
 * sources on this machine that are actually unpredictable, and both the Ryzen
 * this runs on and KVM provide them. When neither is there, the fallback mixes
 * the timestamp counter into a hash chain -- honest entropy on a desktop that
 * has just booted is thin, so this is a fallback and not a design. */

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static int have_rd = -1;                 /* -1 unknown, 0 no, 1 rdrand, 2 rdseed */

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    __asm__ volatile ("cpuid"
                      : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
                      : "a"(leaf), "c"(sub));
}

static void detect(void) {
    uint32_t r[4];
    have_rd = 0;
    cpuid(0, 0, r);
    uint32_t max_leaf = r[0];
    if (max_leaf >= 1) {
        cpuid(1, 0, r);
        if (r[2] & (1u << 30)) have_rd = 1;          /* ECX.RDRAND */
    }
    if (max_leaf >= 7) {
        cpuid(7, 0, r);
        if (r[1] & (1u << 18)) have_rd = 2;          /* EBX.RDSEED */
    }
}

static int hw_word(uint64_t *out) {
    unsigned char ok = 0;
    uint64_t v = 0;
    /* Both instructions can legitimately decline; the manual says retry a
     * bounded number of times and then treat it as unavailable. */
    for (int i = 0; i < 32; i++) {
        if (have_rd == 2)
            __asm__ volatile ("rdseed %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
        else
            __asm__ volatile ("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
        if (ok) { *out = v; return 1; }
    }
    return 0;
}

void crypto_random(void *out, uint32_t len) {
    uint8_t *p = (uint8_t *)out;
    if (have_rd < 0) detect();

    while (len) {
        uint64_t v = 0;
        if (!have_rd || !hw_word(&v)) {
            /* Chain the counter through SHA-256 so that even a poor source is
             * not handed to the protocol raw. */
            static uint8_t pool[32];
            static uint64_t counter;
            uint8_t mix[48];
            uint64_t t = rdtsc(), u = pit_now_us(), c = ++counter;
            for (int i = 0; i < 32; i++) mix[i] = pool[i];
            for (int i = 0; i < 8; i++) {
                mix[32 + i] = (uint8_t)(t >> (i * 8));
                mix[40 + i] = (uint8_t)((u ^ c) >> (i * 8));
            }
            sha256(mix, sizeof mix, pool);
            for (int i = 0; i < 8; i++) v = (v << 8) | pool[i];
        }
        for (int i = 0; i < 8 && len; i++, len--) *p++ = (uint8_t)(v >> (i * 8));
    }
}
