/* The heap: malloc, free, calloc and realloc.
 *
 * WHY THIS REPLACED THE FIRST ONE. The first allocator kept every block, used
 * or not, on one list in address order. malloc walked it from the front for a
 * free block big enough, and free walked ALL of it to merge neighbours. Both
 * cost in proportion to the number of blocks in the heap, and a browser has
 * hundreds of thousands of them: every DOM node, every string, every box.
 * Profiled on the Wikipedia front page, 96% of NetSurf's time was spent in
 * those two loops, and the page took 25 seconds. With the heap out of the way
 * the same page takes three.
 *
 * So nothing here depends on how much is allocated:
 *
 *   - Free blocks are sorted into bins by size. Below 1 KiB every multiple of
 *     16 has a bin of its own; above that each power of two is split into
 *     eight. A bitmap records which bins hold anything, so the first bin that
 *     can serve a request is found with one bit scan, not a search.
 *   - Every block knows its size, and a free block writes its size at its end
 *     as well, so free() reaches both neighbours by arithmetic and merges
 *     with them there and then. Two free blocks are never left side by side.
 *   - Memory never handed out is one block at the end of the heap, the top.
 *     Requests no bin can serve are cut from it, and a freed block next to it
 *     goes back into it. When it runs short the break moves a megabyte or
 *     more at a time: the kernel maps pages only as they are touched, so
 *     asking early costs nothing.
 *
 * The threads of one program share the heap, so it sits behind a lock. The
 * first allocator had none, and thread_create frees on the new thread what it
 * allocated on the old one -- a race as soon as the two ran at once.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

/* --- blocks ----------------------------------------------------------------
 *
 * Every piece of the heap, handed out or not, is a block with one word of
 * header in front of the bytes the caller gets:
 *
 *     in use:  [ size|flags ][ the caller's bytes ......................... ]
 *     free:    [ size|flags ][ next ][ prev ] ................... [ size ]
 *
 * The size includes the header and is a multiple of 16. Two flags live in its
 * low bits: USED, this block is handed out; PREV_USED, the block just before
 * it in memory is. A free block keeps the links of its bin in its first bytes
 * and its size again in its last word, which is how the block after it,
 * seeing PREV_USED clear, finds where it starts. An allocated block has no
 * such word at its end: the caller may use the whole block, header aside.
 *
 * The caller's bytes must be 16-byte aligned -- the compiler copies structs
 * with SSE moves that fault on less -- and the header is 8 bytes, so every
 * block STARTS 8 bytes past a multiple of 16.
 */

typedef struct block {
    size_t        head;          /* size | USED | PREV_USED */
    struct block *next, *prev;   /* free blocks only: the rest of their bin */
} block_t;

#define PREV_USED ((size_t)1)
#define USED      ((size_t)2)
#define FLAGS     (PREV_USED | USED)
#define HEAD      sizeof(size_t)
#define MIN_BLOCK ((size_t)32)     /* header, two links, the size at the end */

/* Refused outright, before the arithmetic below can wrap. The whole heap is
 * under a gigabyte in any case. */
#define MAX_REQUEST ((size_t)1 << 40)

static size_t   size_of(const block_t *b) { return b->head & ~FLAGS; }
static block_t *at(void *p, size_t off)   { return (block_t *)((char *)p + off); }
static void    *user(block_t *b)          { return (char *)b + HEAD; }
static block_t *block_of(void *p)         { return (block_t *)((char *)p - HEAD); }

static void set_tail(block_t *b, size_t s) { *(size_t *)((char *)b + s - HEAD) = s; }
static block_t *before(block_t *b) {
    return (block_t *)((char *)b - *(size_t *)((char *)b - HEAD));
}

/* The block that holds n bytes: n and the header, rounded up to 16. */
static size_t block_for(size_t n) {
    size_t s = (n + HEAD + 15) & ~(size_t)15;
    return s < MIN_BLOCK ? MIN_BLOCK : s;
}

/* --- bins ------------------------------------------------------------------
 *
 * Below 1 KiB a bin holds one size, so anything in it fits a request of that
 * size exactly. From 1 KiB up each power of two is cut in eight -- 1024-1151,
 * 1152-1279, ... 1792-2047, then 2048-2303 and on -- and a block in the bin a
 * request falls into may be too small for it. That one bin is searched; every
 * block in any LATER bin is big enough by construction.
 */

#define SMALL_BINS  64
#define SMALL_LIMIT ((size_t)SMALL_BINS * 16)     /* 1024 */
#define SPLITS      8
#define NBINS       (SMALL_BINS + SPLITS * 31)    /* up to 2^41 */
#define MAP_WORDS   ((NBINS + 63) / 64)

static block_t *bins[NBINS];
static uint64_t bin_map[MAP_WORDS];

static unsigned bin_for(size_t s) {
    if (s < SMALL_LIMIT) return (unsigned)(s >> 4);
    unsigned top = 63u - (unsigned)__builtin_clzll((unsigned long long)s);  /* 10 and up */
    unsigned i = SMALL_BINS + (top - 10) * SPLITS +
                 (unsigned)((s >> (top - 3)) & (SPLITS - 1));
    return i < NBINS ? i : NBINS - 1;
}

static void bin_add(block_t *b, size_t s) {
    unsigned i = bin_for(s);
    b->prev = NULL;
    b->next = bins[i];
    if (b->next) b->next->prev = b;
    bins[i] = b;
    bin_map[i >> 6] |= 1ull << (i & 63);
}

/* Out of its bin. `s` must still be the size it was filed under. */
static void bin_take(block_t *b, size_t s) {
    unsigned i = bin_for(s);
    if (b->prev) b->prev->next = b->next;
    else         bins[i] = b->next;
    if (b->next) b->next->prev = b->prev;
    if (!bins[i]) bin_map[i >> 6] &= ~(1ull << (i & 63));
}

/* The first bin from i on that holds anything, or -1. */
static int bin_after(unsigned i) {
    unsigned w = i >> 6;
    if (w >= MAP_WORDS) return -1;
    uint64_t m = bin_map[w] & (~0ull << (i & 63));
    while (!m) {
        if (++w >= MAP_WORDS) return -1;
        m = bin_map[w];
    }
    return (int)(w * 64 + (unsigned)__builtin_ctzll(m));
}

/* --- the top ---------------------------------------------------------------
 *
 * The free space at the end of the heap is one block kept out of the bins.
 * Behind it sits a fence: a header that says USED and has no size, so that no
 * merge ever runs past the end of what the heap owns. A program that moves
 * the break itself leaves a gap the heap does not own; the next growth then
 * starts a region of its own, and the old top becomes an ordinary free block
 * with its fence still behind it.
 */

static block_t *top;             /* NULL until the first request */
static char    *brk_end;         /* the break, as the heap last left it */

#define GROW ((size_t)1 << 20)   /* the break moves a megabyte at a time, at least */

/* Where the fence of a region ending at `end` goes: the last address a block
 * could start at, with a word of room after it. */
static char *fence_for(char *end) {
    uintptr_t f = (uintptr_t)end - HEAD;
    return (char *)(f - ((f - 8) & 15));
}

/* Make the top at least need + MIN_BLOCK. 0 if the kernel has no more. */
static int grow(size_t need) {
    size_t want = need + MIN_BLOCK + 64;
    size_t step = (want + GROW - 1) & ~(GROW - 1);
    char *p = sbrk((long)step);
    if (p == (char *)-1) {
        /* Close to the ceiling a whole megabyte may be refused where the
         * request itself would fit. */
        step = (want + 4095) & ~(size_t)4095;
        p = sbrk((long)step);
        if (p == (char *)-1) return 0;
    }

    if (top && p == brk_end) {
        /* Straight on from before: the old fence and the new memory both
         * become top. */
        brk_end = p + step;
        char *fence = fence_for(brk_end);
        top->head = (size_t)(fence - (char *)top) | (top->head & PREV_USED);
        ((block_t *)fence)->head = USED;
        return 1;
    }

    if (top) {
        size_t s = size_of(top);
        set_tail(top, s);
        bin_add(top, s);
    }
    char *start = p + ((8 - (uintptr_t)p) & 15);
    brk_end = p + step;
    char *fence = fence_for(brk_end);
    top = (block_t *)start;
    top->head = (size_t)(fence - start) | PREV_USED;   /* nothing before it */
    ((block_t *)fence)->head = USED;
    return 1;
}

/* --- handing out and taking back ------------------------------------------ */

/* Cut a block of `need` off the front of the top. */
static void *from_top(size_t need) {
    if ((!top || size_of(top) < need + MIN_BLOCK) && !grow(need)) return NULL;
    block_t *b = top;
    size_t s = size_of(b);
    top = at(b, need);
    top->head = (s - need) | PREV_USED;
    b->head = need | USED | (b->head & PREV_USED);
    return user(b);
}

/* Hand out the free block b, already out of its bin, for `need` bytes. What
 * it has beyond that goes back as a free block of its own if it is big
 * enough to be one. */
static void *hand_out(block_t *b, size_t need) {
    size_t s = size_of(b);
    if (s - need >= MIN_BLOCK) {
        block_t *rest = at(b, need);
        rest->head = (s - need) | PREV_USED;
        set_tail(rest, s - need);
        bin_add(rest, s - need);
        b->head = need | USED | (b->head & PREV_USED);
    } else {
        b->head |= USED;
        at(b, s)->head |= PREV_USED;
    }
    return user(b);
}

static void *alloc(size_t need) {
    unsigned i = bin_for(need);
    block_t *b = bins[i];
    if (i >= SMALL_BINS) {
        /* A range of sizes: the first that fits, looking at no more than a
         * few before moving to a bin where everything does. */
        int looked = 0;
        while (b && size_of(b) < need && ++looked < 16) b = b->next;
        if (b && size_of(b) < need) b = NULL;
    }
    if (!b) {
        int j = bin_after(i + 1);
        if (j < 0) return from_top(need);
        b = bins[j];
    }
    bin_take(b, size_of(b));
    return hand_out(b, need);
}

/* Free b, merging it with whichever neighbours are free. */
static void release(block_t *b) {
    size_t s = size_of(b);
    /* Cleared first, so that a header swallowed by the merge below no
     * longer claims to be in use -- which is what lets free() recognise a
     * second free of the same pointer. */
    b->head &= ~USED;

    if (!(b->head & PREV_USED)) {
        block_t *p = before(b);
        size_t ps = size_of(p);
        bin_take(p, ps);
        b = p;
        s += ps;
    }

    block_t *n = at(b, s);
    if (n == top) {
        top = b;
        b->head = (s + size_of(n)) | PREV_USED;
        return;
    }
    if (!(n->head & USED)) {
        size_t ns = size_of(n);
        bin_take(n, ns);
        s += ns;
    } else {
        n->head &= ~PREV_USED;
    }
    b->head = s | PREV_USED;
    set_tail(b, s);
    bin_add(b, s);
}

/* b is in use and holds at least `need`: whatever it has beyond that, if
 * enough to stand as a block, is freed. */
static void shrink(block_t *b, size_t need) {
    size_t s = size_of(b);
    if (s - need < MIN_BLOCK) return;
    block_t *rest = at(b, need);
    b->head = need | USED | (b->head & PREV_USED);
    rest->head = (s - need) | USED | PREV_USED;
    release(rest);
}

/* --- the lock --------------------------------------------------------------
 *
 * Held for a few hundred instructions at most. A thread that finds it taken
 * spins briefly -- the holder may be on another core and out in a moment --
 * and then sleeps, because a holder preempted on THIS core can only finish
 * once it gets the processor back.
 */

static volatile int heap_lock;

static void lock(void) {
    while (__atomic_exchange_n(&heap_lock, 1, __ATOMIC_ACQUIRE)) {
        for (int i = 0; i < 200 && heap_lock; i++) __asm__ volatile ("pause");
        if (heap_lock) sleep_ms(1);
    }
}

static void unlock(void) { __atomic_store_n(&heap_lock, 0, __ATOMIC_RELEASE); }

/* --- the interface --------------------------------------------------------- */

/* malloc(0) is a real block of its own, not NULL: code that treats NULL as
 * "out of memory" then does not fail on an empty string or list. */
void *malloc(size_t n) {
    if (n > MAX_REQUEST) return NULL;
    size_t need = block_for(n);
    lock();
    void *p = alloc(need);
    unlock();
    return p;
}

void free(void *p) {
    if (!p) return;
    block_t *b = block_of(p);
    lock();
    /* A block that is not in use is being freed a second time, or was never
     * the heap's. Freeing it would file it in a bin twice and later hand the
     * same memory to two callers, so it is left alone. */
    if ((b->head & USED) && size_of(b) >= MIN_BLOCK) release(b);
    unlock();
}

void *calloc(size_t count, size_t size) {
    if (count && size > (size_t)-1 / count) return NULL;   /* overflow */
    size_t total = count * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (n == 0) { free(p); return NULL; }
    if (n > MAX_REQUEST) return NULL;

    size_t need = block_for(n);
    block_t *b = block_of(p);

    lock();
    if (!(b->head & USED)) { unlock(); return NULL; }
    size_t s = size_of(b);

    /* No bigger than it already is: keep it, and give back a tail worth
     * having. */
    if (s >= need) {
        shrink(b, need);
        unlock();
        return p;
    }

    /* At the end of the heap: grow into the top, moving the break first if
     * the top is short. Buffers that are built up by appending -- strings,
     * the script engine's arrays -- end up here and never copy. */
    block_t *n1 = at(b, s);
    if (n1 == top && (s + size_of(top) >= need + MIN_BLOCK ||
                      (grow(need) && n1 == top))) {
        size_t all = s + size_of(top);
        top = at(b, need);
        top->head = (all - need) | PREV_USED;
        b->head = need | USED | (b->head & PREV_USED);
        unlock();
        return p;
    }

    /* A free block right after it with enough room: take it over. */
    if (n1 != top && !(n1->head & USED) && s + size_of(n1) >= need) {
        size_t ns = size_of(n1);
        bin_take(n1, ns);
        b->head = (s + ns) | USED | (b->head & PREV_USED);
        at(b, s + ns)->head |= PREV_USED;
        shrink(b, need);
        unlock();
        return p;
    }
    unlock();

    /* Anywhere else, then, and the contents come along. */
    void *q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, s - HEAD);
    free(p);
    return q;
}
