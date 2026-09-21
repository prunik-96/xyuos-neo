#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

/* Heap grows on demand via sbrk() instead of living in a fixed static arena.
 * The old 2 MiB BSS array capped every program at 2 MiB and consumed that much
 * of the 16 MiB user pool whether or not it was used; a compiler needs far
 * more than that.
 *
 * First-fit free list. Because sbrk hands back contiguous memory and blocks
 * are only ever appended, neighbours in the list are also neighbours in
 * memory -- which is what makes the simple next-block coalescing valid. The
 * adjacency is still asserted before merging rather than assumed. */

#define CHUNK_SIZE (256 * 1024)   /* grow the heap in 256 KiB steps */

typedef struct block {
    size_t        size;    /* payload bytes, not counting this header */
    int           free;
    struct block *next;
} block_t;

/* THE HEADER SIZE USED FOR ADDRESSING IS *NOT* sizeof(block_t).
 *
 * malloc must return memory aligned for any type, which on x86-64 means 16
 * bytes: the compiler emits movaps/movdqa for struct copies and those FAULT
 * (#GP) on a misaligned address -- they do not merely run slow.
 *
 * sizeof(block_t) is 24, so a payload placed at block+24 lands 8 bytes off
 * every single time. Rounding the header up to 32 keeps every payload
 * 16-aligned, given a 16-aligned heap base and 16-aligned block sizes. */
#define HDR (((sizeof(block_t)) + 15u) & ~(size_t)15u)

static block_t *head = NULL;
static block_t *tail = NULL;

static size_t align16(size_t n) { return (n + 15) & ~(size_t)15; }

/* True when `b` is immediately followed in memory by `b->next`. */
static int adjacent(block_t *b) {
    return b->next && (char *)b + HDR + b->size == (char *)b->next;
}

static block_t *request_space(size_t want) {
    size_t need = HDR + want;
    size_t chunk = (need + CHUNK_SIZE - 1) & ~(size_t)(CHUNK_SIZE - 1);

    void *p = sbrk((long)chunk);
    if (p == (void *)-1) return NULL;

    /* Belt and braces: the break starts page-aligned and only ever moves by
     * whole chunks, but a header placed on an odd address would silently
     * misalign every payload after it. */
    if ((uintptr_t)p & 15u) {
        size_t pad = 16u - ((uintptr_t)p & 15u);
        p = (char *)p + pad;
        chunk -= pad;
    }

    block_t *b = (block_t *)p;
    b->size = chunk - HDR;
    b->free = 1;
    b->next = NULL;

    if (tail) tail->next = b;
    else      head = b;
    tail = b;
    return b;
}

/* Split `b` if the leftover is big enough to be worth its own header. */
static void split(block_t *b, size_t want) {
    size_t rest = b->size - want;
    if (rest <= HDR + 16) return;

    block_t *nb = (block_t *)((char *)b + HDR + want);
    nb->size = rest - HDR;
    nb->free = 1;
    nb->next = b->next;
    b->next = nb;
    b->size = want;
    if (tail == b) tail = nb;
}

void *malloc(size_t size) {
    if (size == 0) return NULL;
    size_t want = align16(size);

    for (block_t *b = head; b; b = b->next) {
        if (b->free && b->size >= want) {
            split(b, want);
            b->free = 0;
            return (char *)b + HDR;
        }
    }

    block_t *b = request_space(want);
    if (!b) return NULL;
    split(b, want);
    b->free = 0;
    return (char *)b + HDR;
}

void free(void *ptr) {
    if (!ptr) return;
    block_t *b = (block_t *)((char *)ptr - HDR);
    b->free = 1;

    /* Merge every run of free, physically adjacent blocks. */
    for (block_t *cur = head; cur; cur = cur->next) {
        while (cur->free && adjacent(cur) && cur->next->free) {
            block_t *dead = cur->next;
            cur->size += HDR + dead->size;
            cur->next = dead->next;
            if (tail == dead) tail = cur;
        }
    }
}

void *calloc(size_t count, size_t size) {
    if (count && size > (size_t)-1 / count) return NULL;   /* overflow */
    size_t total = count * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }

    block_t *b = (block_t *)((char *)ptr - HDR);
    size_t want = align16(size);
    if (b->size >= want) return ptr;

    /* Grow in place when the next block is free, adjacent and big enough. */
    if (adjacent(b) && b->next->free &&
        b->size + HDR + b->next->size >= want) {
        block_t *dead = b->next;
        b->size += HDR + dead->size;
        b->next = dead->next;
        if (tail == dead) tail = b;
        split(b, want);
        return ptr;
    }

    void *np = malloc(size);
    if (!np) return NULL;
    memcpy(np, ptr, b->size);      /* old payload size: always <= new size */
    free(ptr);
    return np;
}

/* Defined in posixbits.c, next to atexit itself. */
void __libc_run_atexit(void);

void exit(int code) { __libc_run_atexit(); _exit(code); }
void abort(void)    { _exit(134); }

int atoi(const char *s) {
    int sign = 1;
    int result = 0;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    return result * sign;
}
