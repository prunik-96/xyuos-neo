#include "heap.h"
#include "pmm.h"
#include "../kernel/spinlock.h"
#include <stdint.h>
#include <stddef.h>

// Serialises the free list. kmalloc/kfree run from many contexts (and every
// core, once SMP is up); irq-safe so an allocation inside an interrupt handler
// cannot corrupt a list the interrupted code was mid-way through editing.
static spinlock_t heap_lock = SPINLOCK_INIT;

// Simple first-fit free-list allocator. Backing memory comes from the
// (identity-mapped) physical frame allocator, grabbed a page at a time and
// merged into the block list when contiguous with the previous grab.

typedef struct heap_block {
    uint64_t size;   // size of the usable data area, excludes this header
    uint64_t free;
    struct heap_block *next;
    struct heap_block *prev;
} heap_block_t;

#define HEADER_SIZE sizeof(heap_block_t)
#define ALIGN16(x) (((x) + 15) & ~15ULL)

static heap_block_t *first_block = NULL;
static heap_block_t *last_block = NULL;

// What the list began as, kept only so a corruption report can say whether
// the head itself was overwritten or something further along was.
static heap_block_t *first_at_init;
static uint64_t kheap_allocs;

static uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

static heap_block_t *grow_heap(uint64_t min_data_size) {
    uint64_t needed = align_up(HEADER_SIZE + min_data_size, PAGE_SIZE);
    uint64_t frame_count = needed / PAGE_SIZE;

    // Ask for the whole run at once. Taking frames one at a time and hoping
    // they come out adjacent works only while the bitmap is nearly empty --
    // which is exactly how a megabyte came to be unallocatable here.
    uint64_t start = pmm_alloc_contig(frame_count);
    if (start == 0) {
        // Not available in one piece. Half of it still serves a smaller
        // request, and the caller asks again for what it is short of.
        while (frame_count > 1) {
            frame_count /= 2;
            start = pmm_alloc_contig(frame_count);
            if (start) break;
        }
        if (start == 0) return NULL;
    }
    uint64_t end = start + frame_count * PAGE_SIZE;

    heap_block_t *block = (heap_block_t *)(uintptr_t)start;
    block->size = (end - start) - HEADER_SIZE;
    block->free = 1;
    block->next = NULL;
    block->prev = NULL;

    // Physically contiguous with the tail block: merge instead of appending
    // -- but ONLY if that block is free. Growing a block somebody is using
    // and then handing it to kmalloc gives its bytes a second owner, and the
    // two of them quietly overwrite each other. That is the whole bug this
    // condition exists to prevent, and it cost an afternoon to find, because
    // the fault appears in a later kmalloc rather than anywhere near here.
    if (last_block != NULL && last_block->free &&
        (uint64_t)(uintptr_t)last_block + HEADER_SIZE + last_block->size == start) {
        last_block->size += HEADER_SIZE + block->size;
        return last_block;
    }

    if (first_block == NULL) {
        first_block = block;
        last_block = block;
    } else {
        block->prev = last_block;
        last_block->next = block;
        last_block = block;
    }
    return block;
}

void heap_init(void) {
    first_block = NULL;
    last_block = NULL;
    grow_heap(1024 * 1024); // pre-warm with ~1MB
    first_at_init = first_block;
}

static void split_block(heap_block_t *block, uint64_t size) {
    uint64_t remaining = block->size - size;
    if (remaining <= HEADER_SIZE + 16) {
        return; // not worth splitting
    }

    heap_block_t *new_block = (heap_block_t *)((uint8_t *)block + HEADER_SIZE + size);
    new_block->size = remaining - HEADER_SIZE;
    new_block->free = 1;
    new_block->next = block->next;
    new_block->prev = block;

    if (block->next) {
        block->next->prev = new_block;
    } else {
        last_block = new_block;
    }
    block->next = new_block;
    block->size = size;
}

// Everything this allocator hands out comes from identity-mapped physical
// frames, so a block header is only ever a modest address, 16-byte aligned,
// carrying a size no larger than the machine. Anything else is not a block:
// it is whatever overran the one before it.
static int block_sane(const heap_block_t *b) {
    uint64_t a = (uint64_t)(uintptr_t)b;
    if (a < 0x1000 || a > 0x100000000ULL) return 0;   // below the first page,
    if (a & 15) return 0;                             // or past 4 GiB, or odd
    if (b->free > 1) return 0;                        // a flag, nothing else
    if (b->size > 0x100000000ULL) return 0;
    return 1;
}

void kheap_corrupt(const heap_block_t *prev, const heap_block_t *bad, int n);

void serial_write(const char *s);

static void hex64(char *o, int *n, uint64_t v) {
    static const char d[] = "0123456789abcdef";
    o[(*n)++] = '0'; o[(*n)++] = 'x';
    int started = 0;
    for (int sh = 60; sh >= 0; sh -= 4) {
        int nib = (int)((v >> sh) & 15);
        if (nib || started || sh == 0) { o[(*n)++] = d[nib]; started = 1; }
    }
}

// Called with the lock already released: this does not return, and holding a
// lock into a halt would be the last unhelpful thing it could do.
void kheap_corrupt(const heap_block_t *prev, const heap_block_t *bad, int n) {
    char o[220]; int k = 0;
    const char *m = "\n*** HEAP: the block list is broken ***\n  after ";
    while (*m) o[k++] = *m++;
    hex64(o, &k, (uint64_t)(uintptr_t)prev);
    m = " size ";
    while (*m) o[k++] = *m++;
    hex64(o, &k, prev ? prev->size : 0);
    m = prev && prev->free ? " (free)" : " (in use)";
    while (*m) o[k++] = *m++;
    m = "\n  next is ";
    while (*m) o[k++] = *m++;
    hex64(o, &k, (uint64_t)(uintptr_t)bad);
    m = ", which is not a block. blocks walked: ";
    while (*m) o[k++] = *m++;
    hex64(o, &k, (uint64_t)n);
    m = "\n  its header reads free=";
    while (*m) o[k++] = *m++;
    hex64(o, &k, bad ? bad->free : 0);
    m = " size=";
    while (*m) o[k++] = *m++;
    hex64(o, &k, bad ? bad->size : 0);
    m = "\n  list head is ";
    while (*m) o[k++] = *m++;
    hex64(o, &k, (uint64_t)(uintptr_t)first_block);
    m = ", was ";
    while (*m) o[k++] = *m++;
    hex64(o, &k, (uint64_t)(uintptr_t)first_at_init);
    m = " at init; allocations so far ";
    while (*m) o[k++] = *m++;
    hex64(o, &k, kheap_allocs);
    o[k++] = '\n';
    o[k] = 0;
    serial_write(o);
    for (;;) __asm__ volatile("cli; hlt");
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    uint64_t want = ALIGN16((uint64_t)size);

    uint64_t flags = spin_lock_irqsave(&heap_lock);
    heap_block_t *prev = NULL;
    int walked = 0;
    for (heap_block_t *b = first_block; b != NULL; prev = b, b = b->next) {
        if (!block_sane(b)) {
            spin_unlock_irqrestore(&heap_lock, flags);
            kheap_corrupt(prev, b, walked);
            return NULL;
        }
        walked++;
        if (b->free && b->size >= want) {
            kheap_allocs++;
            split_block(b, want);
            b->free = 0;
            void *r = (void *)((uint8_t *)b + HEADER_SIZE);
            spin_unlock_irqrestore(&heap_lock, flags);
            return r;
        }
    }

    // grow_heap may come back with less than was asked for, when the frames
    // it got were not contiguous. Asking again is reasonable -- the next run
    // may be better placed -- but not forever: without a bound this walks
    // through every frame in the machine looking for a lucky one.
    heap_block_t *b = grow_heap(want);
    for (int tries = 0; b != NULL && b->size < want && tries < 16; tries++)
        b = grow_heap(want);
    if (b != NULL && b->size < want) b = NULL;
    if (b == NULL) { spin_unlock_irqrestore(&heap_lock, flags); return NULL; }

    split_block(b, want);
    b->free = 0;
    void *r = (void *)((uint8_t *)b + HEADER_SIZE);
    spin_unlock_irqrestore(&heap_lock, flags);
    return r;
}

// Two blocks are neighbours in the free LIST, but that does not make them
// neighbours in MEMORY: grow_heap links separate frame runs together, and a
// reserved frame (a device MMIO page table, say) can sit in the gap between
// them. Merging across such a gap would produce one block whose byte range
// straddles memory the heap does not own -- and the next write into it would
// corrupt that memory. So only ever merge blocks that are physically adjacent.
static int phys_adjacent(const heap_block_t *lo, const heap_block_t *hi) {
    return (const uint8_t *)lo + HEADER_SIZE + lo->size == (const uint8_t *)hi;
}

static void coalesce(heap_block_t *b) {
    if (b->next && b->next->free && phys_adjacent(b, b->next)) {
        b->size += HEADER_SIZE + b->next->size;
        b->next = b->next->next;
        if (b->next) {
            b->next->prev = b;
        } else {
            last_block = b;
        }
    }
    if (b->prev && b->prev->free && phys_adjacent(b->prev, b)) {
        coalesce(b->prev);
    }
}

void kfree(void *ptr) {
    if (ptr == NULL) return;
    uint64_t flags = spin_lock_irqsave(&heap_lock);
    heap_block_t *b = (heap_block_t *)((uint8_t *)ptr - HEADER_SIZE);
    b->free = 1;
    coalesce(b);
    spin_unlock_irqrestore(&heap_lock, flags);
}
