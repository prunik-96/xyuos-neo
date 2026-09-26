/* The xyuOS heap (libc/src/malloc.c), built for the host and hammered.
 *
 *   gcc -O2 -Wall -pthread -o /tmp/malloctest tools/malloctest.c && /tmp/malloctest
 *
 * The allocator is compiled in as source, under other names so the host's
 * own malloc is left alone, and given a break of its own to move through: a
 * static arena. Random allocations, frees, reallocations and callocs run
 * against it with every block's contents checked, and every so often the
 * whole heap is walked and every rule it depends on checked -- sizes, flags,
 * the size at the end of each free block, no two free blocks side by side,
 * every free block in exactly the bin its size says, the bitmap agreeing with
 * the bins. The test also moves the break itself now and then, as a program
 * calling sbrk would, leaving the heap gaps it does not own; what was written
 * into those gaps must still be there at the end.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <time.h>

/* --- a break to move ------------------------------------------------------ */

#define ARENA (768ull << 20)
static char arena[ARENA] __attribute__((aligned(4096)));
static char *brk_now = arena + 5;        /* deliberately misaligned to start */
static int foreign;                      /* the test, not the heap, is asking */

/* The stretches of arena the heap was given. A grant straight after the last
 * one extends it, as it does in the allocator. */
static struct { char *start, *end; } regions[4096];
static int nregions;

static void *test_sbrk(long inc) {
    if (inc < 0 || (size_t)(brk_now - arena) + (size_t)inc > ARENA) return (void *)-1;
    char *old = brk_now;
    brk_now += inc;
    if (!foreign) {
        if (nregions && regions[nregions - 1].end == old) regions[nregions - 1].end = brk_now;
        else { regions[nregions].start = old; regions[nregions].end = brk_now; nregions++; }
    }
    return old;
}

#define malloc  t_malloc
#define free    t_free
#define calloc  t_calloc
#define realloc t_realloc
#define sbrk    test_sbrk
#define sleep_ms(ms) sched_yield()
#include "../libc/src/malloc.c"
#undef malloc
#undef free
#undef calloc
#undef realloc
#undef sbrk
#undef sleep_ms

/* --- the checks ------------------------------------------------------------ */

#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); printf("\n"); exit(1); } while (0)

static int cmp_ptr(const void *a, const void *b) {
    uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
    return x < y ? -1 : x > y;
}

static uintptr_t *binned;
static size_t binned_cap;

/* Walks the bins and every region; returns how many blocks are in use. */
static size_t check_heap(void) {
    size_t nb = 0;
    for (unsigned i = 0; i < NBINS; i++) {
        int bit = (int)((bin_map[i >> 6] >> (i & 63)) & 1);
        if (bit != (bins[i] != NULL)) FAIL("bitmap says %d for bin %u", bit, i);
        block_t *prev = NULL;
        for (block_t *b = bins[i]; b; b = b->next) {
            if (b->prev != prev) FAIL("bin %u: broken back link", i);
            if (b->head & USED) FAIL("bin %u: a block in use", i);
            if (bin_for(size_of(b)) != i) FAIL("bin %u: size %zu belongs in %u",
                                               i, size_of(b), bin_for(size_of(b)));
            if (b == top) FAIL("top is in a bin");
            if (nb == binned_cap) {
                binned_cap = binned_cap ? binned_cap * 2 : 65536;
                binned = realloc(binned, binned_cap * sizeof *binned);
            }
            binned[nb++] = (uintptr_t)b;
            prev = b;
        }
    }
    qsort(binned, nb, sizeof *binned, cmp_ptr);

    size_t used = 0, nfree = 0;
    int tops = 0;
    for (int r = 0; r < nregions; r++) {
        char *start = regions[r].start + ((8 - (uintptr_t)regions[r].start) & 15);
        char *fence = fence_for(regions[r].end);
        block_t *b = (block_t *)start;
        int prev_used = 1;
        while ((char *)b < fence) {
            size_t s = size_of(b);
            if (((uintptr_t)b & 15) != 8) FAIL("block %p misaligned", (void *)b);
            if (s < MIN_BLOCK || (s & 15)) FAIL("block %p has size %zu", (void *)b, s);
            if ((char *)b + s > fence) FAIL("block %p runs past its fence", (void *)b);
            if (!!(b->head & PREV_USED) != prev_used)
                FAIL("block %p: PREV_USED is %d, the block before is %s", (void *)b,
                     !!(b->head & PREV_USED), prev_used ? "in use" : "free");
            if (b->head & USED) {
                used++;
                prev_used = 1;
            } else {
                if (!prev_used) FAIL("two free blocks side by side at %p", (void *)b);
                if (b == top) {
                    tops++;
                    if ((char *)b + s != fence) FAIL("top is not last before its fence");
                } else {
                    if (*(size_t *)((char *)b + s - HEAD) != s)
                        FAIL("free block %p: tail says %zu, head %zu", (void *)b,
                             *(size_t *)((char *)b + s - HEAD), s);
                    uintptr_t key = (uintptr_t)b;
                    if (!bsearch(&key, binned, nb, sizeof *binned, cmp_ptr))
                        FAIL("free block %p (size %zu) is in no bin", (void *)b, s);
                    nfree++;
                }
                prev_used = 0;
            }
            b = at(b, s);
        }
        if ((char *)b != fence) FAIL("region %d: walk ends at %p, fence at %p",
                                     r, (void *)b, (void *)fence);
        if (!(b->head & USED) || size_of(b) != 0) FAIL("region %d: no fence", r);
        if (!!(b->head & PREV_USED) != prev_used) FAIL("region %d: fence flag wrong", r);
    }
    if (nregions && tops != 1) FAIL("%d tops", tops);
    if (nfree != nb) FAIL("%zu free blocks in the heap, %zu in bins", nfree, nb);
    return used;
}

/* --- random traffic -------------------------------------------------------- */

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd_r(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return *s = x;
}
static uint64_t rnd(void) { return rnd_r(&rng_state); }

static size_t rand_size(uint64_t *s) {
    unsigned r = (unsigned)(rnd_r(s) % 1000);
    if (r < 5)   return 0;
    if (r < 600) return 1 + rnd_r(s) % 64;
    if (r < 900) return 65 + rnd_r(s) % 960;
    if (r < 995) return 1025 + rnd_r(s) % 15000;
    return 16384 + rnd_r(s) % (256 * 1024);
}

typedef struct { unsigned char *p; size_t n; uint32_t seed; } slot_t;

/* The pattern a block holds: every byte for small ones, and for large ones
 * both ends and a byte in every page, which is where damage would show. */
static unsigned char pat(uint32_t seed, size_t i) { return (unsigned char)(seed + i * 131u + (i >> 8)); }
static int checked(size_t n, size_t i) { return n <= 4096 || i < 2048 || i >= n - 2048 || !(i & 4095); }

static void fill(slot_t *s) {
    for (size_t i = 0; i < s->n; i++) if (checked(s->n, i)) s->p[i] = pat(s->seed, i);
}
static void verify(const slot_t *s, size_t upto, const char *when) {
    for (size_t i = 0; i < upto; i++)
        if (checked(s->n, i) && s->p[i] != pat(s->seed, i))
            FAIL("%s: block %p of %zu bytes damaged at byte %zu", when, (void *)s->p, s->n, i);
}

static void one_op(slot_t *slots, int nslots, uint64_t *rs) {
    slot_t *s = &slots[rnd_r(rs) % (uint64_t)nslots];
    unsigned r = (unsigned)(rnd_r(rs) % 100);
    if (!s->p) {
        if (r < 85) {
            size_t n = rand_size(rs);
            s->p = t_malloc(n);
            if (!s->p) FAIL("malloc(%zu) returned NULL", n);
            s->n = n;
        } else {
            size_t count = 1 + rnd_r(rs) % 16, each = rand_size(rs) / 8 + 1;
            s->p = t_calloc(count, each);
            if (!s->p) FAIL("calloc(%zu, %zu) returned NULL", count, each);
            s->n = count * each;
            for (size_t i = 0; i < s->n; i++)
                if (s->p[i]) FAIL("calloc memory not zero at %zu", i);
        }
        if ((uintptr_t)s->p & 15) FAIL("pointer %p not 16-aligned", (void *)s->p);
        s->seed = (uint32_t)rnd_r(rs);
        fill(s);
    } else if (r < 60) {
        verify(s, s->n, "before free");
        t_free(s->p);
        s->p = NULL;
    } else {
        size_t n = rand_size(rs);
        verify(s, s->n, "before realloc");
        unsigned char *q = t_realloc(s->p, n);
        if (n == 0) {
            if (q) FAIL("realloc to 0 returned a pointer");
            s->p = NULL;
            return;
        }
        if (!q) FAIL("realloc(%zu) returned NULL", n);
        if ((uintptr_t)q & 15) FAIL("realloc pointer %p not 16-aligned", (void *)q);
        size_t keep = s->n < n ? s->n : n;
        slot_t moved = { q, s->n, s->seed };
        verify(&moved, keep, "after realloc");
        s->p = q;
        s->n = n;
        fill(s);
    }
}

/* The gaps the test itself took from the break, and what it wrote there. */
static struct { unsigned char *p; size_t n; } gaps[1024];
static int ngaps;

static void take_gap(void) {
    size_t k = 1 + rnd() % 3000;
    foreign = 1;
    unsigned char *g = test_sbrk((long)k);
    foreign = 0;
    if (g == (void *)-1 || ngaps == 1024) return;
    memset(g, 0xA5, k);
    gaps[ngaps].p = g; gaps[ngaps].n = k; ngaps++;
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* --- threads --------------------------------------------------------------- */

#define TSLOTS 4000
struct worker { slot_t slots[TSLOTS]; uint64_t rs; long ops; };

static void *worker_main(void *arg) {
    struct worker *w = arg;
    for (long i = 0; i < w->ops; i++) one_op(w->slots, TSLOTS, &w->rs);
    return NULL;
}

/* --- the run ------------------------------------------------------------------ */

#define SLOTS 20000
static slot_t live[SLOTS];

int main(void) {
    /* The edges first. */
    void *z1 = t_malloc(0), *z2 = t_malloc(0);
    if (!z1 || !z2 || z1 == z2) FAIL("malloc(0) must give distinct blocks");
    t_free(z1); t_free(z2);
    t_free(NULL);
    if (t_malloc((size_t)-1)) FAIL("malloc(SIZE_MAX) succeeded");
    if (t_calloc((size_t)1 << 40, (size_t)1 << 40)) FAIL("calloc overflow succeeded");
    void *r0 = t_realloc(NULL, 100);
    if (!r0) FAIL("realloc(NULL, 100)");
    void *dbl = t_malloc(48);
    t_free(dbl);
    t_free(dbl);                         /* a second free must change nothing */
    t_free(r0);
    check_heap();
    printf("edges: ok\n");

    /* Random traffic, the heap checked as it goes. */
    long ops = 3000000;
    double t0 = now_s();
    for (long i = 1; i <= ops; i++) {
        one_op(live, SLOTS, &rng_state);
        if (rnd() % 5000 == 0) take_gap();
        if (i % 100000 == 0) {
            check_heap();
            for (int k = 0; k < SLOTS; k++) if (live[k].p) verify(&live[k], live[k].n, "sweep");
        }
    }
    size_t used = check_heap();
    size_t nlive = 0;
    for (int k = 0; k < SLOTS; k++) nlive += live[k].p != NULL;
    if (used != nlive) FAIL("%zu blocks in use, %zu live", used, nlive);
    printf("random: %ld operations in %.2f s, %zu live, %d regions, %d gaps: ok\n",
           ops, now_s() - t0, nlive, nregions, ngaps);

    /* Threads: four at once on the one heap. */
    struct worker *w = calloc(4, sizeof *w);
    pthread_t th[4];
    for (int i = 0; i < 4; i++) { w[i].rs = 0x1234567ull * (i + 1); w[i].ops = 400000; }
    t0 = now_s();
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, worker_main, &w[i]);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    used = check_heap();
    size_t tlive = 0;
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < TSLOTS; k++)
            if (w[i].slots[k].p) { tlive++; verify(&w[i].slots[k], w[i].slots[k].n, "thread"); }
    if (used != nlive + tlive) FAIL("threads: %zu in use, %zu live", used, nlive + tlive);
    printf("threads: 4 x %ld operations in %.2f s: ok\n", w[0].ops, now_s() - t0);
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < TSLOTS; k++)
            if (w[i].slots[k].p) t_free(w[i].slots[k].p);
    free(w);

    /* What a browser does: a great many small blocks alive at once, and
     * churn among them. The first allocator walked all of them on every
     * call; this must not care how many there are. */
    for (int k = 0; k < SLOTS; k++) if (live[k].p) { t_free(live[k].p); live[k].p = NULL; }
    enum { MANY = 400000 };
    void **many = calloc(MANY, sizeof *many);
    for (int i = 0; i < MANY; i++) many[i] = t_malloc(8 + rnd() % 120);
    t0 = now_s();
    long churn = 4000000;
    for (long i = 0; i < churn; i++) {
        int k = (int)(rnd() % MANY);
        t_free(many[k]);
        many[k] = t_malloc(8 + rnd() % 120);
    }
    double dt = now_s() - t0;
    printf("churn: %ld free+malloc pairs among %d live blocks in %.2f s (%.0f ns a pair)\n",
           churn, MANY, dt, dt * 1e9 / churn);
    check_heap();
    for (int i = 0; i < MANY; i++) t_free(many[i]);
    free(many);

    /* Everything back: it must all have merged. What free space is left is
     * the top and, at most, one retired top per region the heap left behind. */
    used = check_heap();
    if (used) FAIL("%zu blocks still in use after freeing everything", used);
    size_t nb = 0;
    for (unsigned i = 0; i < NBINS; i++) for (block_t *b = bins[i]; b; b = b->next) nb++;
    if ((int)nb > nregions - 1) FAIL("%zu free blocks left in %d regions: not merged", nb, nregions);
    printf("all freed: %zu free blocks besides the top, %d regions: ok\n", nb, nregions);

    /* Running out: the heap must say NULL, not fall over, and recover. */
    void *big[64];
    int got = 0;
    while (got < 64 && (big[got] = t_malloc(64u << 20)) != NULL) got++;
    if (got == 64) FAIL("the arena never ran out");
    check_heap();
    for (int i = 0; i < got; i++) t_free(big[i]);
    void *after = t_malloc(1000);
    if (!after) FAIL("no memory after the big blocks were freed");
    t_free(after);
    check_heap();
    printf("out of memory: NULL after %d x 64 MiB, and recovered: ok\n", got);

    for (int i = 0; i < ngaps; i++)
        for (size_t j = 0; j < gaps[i].n; j++)
            if (gaps[i].p[j] != 0xA5) FAIL("the heap wrote into a gap it did not own");
    printf("gaps: %d untouched\n", ngaps);
    printf("ALL OK\n");
    return 0;
}
