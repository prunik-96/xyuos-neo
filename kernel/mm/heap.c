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

static uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

static heap_block_t *grow_heap(uint64_t min_data_size) {
    uint64_t needed = align_up(HEADER_SIZE + min_data_size, PAGE_SIZE);
    uint64_t frame_count = needed / PAGE_SIZE;

    uint64_t start = pmm_alloc_frame();
    if (start == 0) return NULL;
    uint64_t end = start + PAGE_SIZE;

    for (uint64_t i = 1; i < frame_count; i++) {
        uint64_t f = pmm_alloc_frame();
        if (f == 0) return NULL;
        if (f == end) {
            end += PAGE_SIZE;
        } else {
            // non-contiguous: stop growing this run, use what we have so far.
            // (the extra frame is simply left mapped-but-unused for now.)
            break;
        }
    }

    heap_block_t *block = (heap_block_t *)(uintptr_t)start;
    block->size = (end - start) - HEADER_SIZE;
    block->free = 1;
    block->next = NULL;
    block->prev = NULL;

    if (last_block != NULL && (uint64_t)(uintptr_t)last_block + HEADER_SIZE + last_block->size == start) {
        // physically contiguous with the tail block: merge instead of appending.
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

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    uint64_t want = ALIGN16((uint64_t)size);

    uint64_t flags = spin_lock_irqsave(&heap_lock);
    for (heap_block_t *b = first_block; b != NULL; b = b->next) {
        if (b->free && b->size >= want) {
            split_block(b, want);
            b->free = 0;
            void *r = (void *)((uint8_t *)b + HEADER_SIZE);
            spin_unlock_irqrestore(&heap_lock, flags);
            return r;
        }
    }

    heap_block_t *b = grow_heap(want);
    while (b != NULL && b->size < want) {
        b = grow_heap(want);
    }
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
