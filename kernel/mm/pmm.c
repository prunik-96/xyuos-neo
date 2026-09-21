#include "pmm.h"
#include "vmm.h"
#include "../include/multiboot2.h"
#include "../kernel/spinlock.h"
#include <stddef.h>

// Guards the frame bitmap. Physical frames are handed out from IRQ context
// (paging_map_mmio during device polling) as well as mainline code, and every
// core will allocate once SMP is up -- so this must be irq-safe.
static spinlock_t pmm_lock = SPINLOCK_INIT;

// Bitmap-based physical frame allocator, covering the first 4GiB
// (matches the identity-mapped range set up at boot). One bit per 4KiB frame.
#define MAX_FRAMES (4ULL * 1024 * 1024 * 1024 / PAGE_SIZE)
#define BITMAP_BYTES (MAX_FRAMES / 8)

static uint8_t bitmap[BITMAP_BYTES];
static uint64_t free_frames = 0;
// Frames the firmware reported as usable RAM. NOT the size of the range the
// bitmap covers (4 GiB): on a 512 MiB machine reporting the latter as "total"
// would claim 3.5 GiB is in use.
static uint64_t usable_frames = 0;

extern uint8_t kernel_start[];
extern uint8_t kernel_end[];

static void mark_used(uint64_t frame) {
    if (frame >= MAX_FRAMES) return;
    uint64_t byte = frame / 8;
    uint8_t bit = frame % 8;
    if (!(bitmap[byte] & (1 << bit))) {
        return;
    }
    bitmap[byte] &= ~(1 << bit);
    free_frames--;
}

static void mark_free(uint64_t frame) {
    if (frame >= MAX_FRAMES) return;
    uint64_t byte = frame / 8;
    uint8_t bit = frame % 8;
    if (bitmap[byte] & (1 << bit)) {
        return;
    }
    bitmap[byte] |= (1 << bit);
    free_frames++;
}

static void reserve_range(uint64_t start, uint64_t end) {
    uint64_t first_frame = start / PAGE_SIZE;
    uint64_t last_frame = (end + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t f = first_frame; f < last_frame; f++) {
        mark_used(f);
    }
}

void pmm_init(uint32_t multiboot_addr) {
    // start fully reserved; mmap entries below free up usable RAM.
    for (uint64_t i = 0; i < BITMAP_BYTES; i++) {
        bitmap[i] = 0x00;
    }
    free_frames = 0;

    struct multiboot_info *info = (struct multiboot_info *)(uintptr_t)multiboot_addr;
    uint8_t *tag_ptr = (uint8_t *)info + 8;

    for (;;) {
        struct multiboot_tag *tag = (struct multiboot_tag *)tag_ptr;
        if (tag->type == MULTIBOOT2_TAG_TYPE_END) {
            break;
        }
        if (tag->type == MULTIBOOT2_TAG_TYPE_MMAP) {
            struct multiboot_tag_mmap *mmap = (struct multiboot_tag_mmap *)tag;
            uint8_t *entry_ptr = (uint8_t *)mmap->entries;
            uint8_t *entries_end = (uint8_t *)tag + tag->size;
            while (entry_ptr < entries_end) {
                struct multiboot_mmap_entry *e = (struct multiboot_mmap_entry *)entry_ptr;
                if (e->type == MULTIBOOT2_MEMORY_AVAILABLE) {
                    uint64_t start = (e->addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                    uint64_t end = (e->addr + e->len) & ~(PAGE_SIZE - 1);
                    uint64_t first_frame = start / PAGE_SIZE;
                    uint64_t last_frame = end / PAGE_SIZE;
                    for (uint64_t f = first_frame; f < last_frame && f < MAX_FRAMES; f++) {
                        mark_free(f);
                    }
                }
                entry_ptr += mmap->entry_size;
            }
        }
        tag_ptr += (tag->size + 7) & ~7u;
    }

    // Everything marked free so far is real RAM; the reservations below carve
    // pieces out of it, so record the total before they run.
    usable_frames = free_frames;

    // never hand out physical frame 0 (keeps NULL meaningfully invalid).
    reserve_range(0, PAGE_SIZE);

    // the kernel image itself.
    reserve_range((uint64_t)(uintptr_t)kernel_start, (uint64_t)(uintptr_t)kernel_end);

    // the multiboot info structure and every tag/module it points to.
    reserve_range(multiboot_addr, multiboot_addr + info->total_size);

    // The virtual window reserved for per-process user space (see paging.h).
    // These frames are withheld from the allocator entirely -- not because
    // user pages come from here (they can come from anywhere), but because the
    // kernel reaches any frame through its identity address, and inside a
    // process address space this virtual range is NOT the identity map. A
    // kernel pointer into it would quietly refer to the running program's
    // memory instead.
    reserve_range(USER_VIRT_BASE, USER_VIRT_END);

    tag_ptr = (uint8_t *)info + 8;
    for (;;) {
        struct multiboot_tag *tag = (struct multiboot_tag *)tag_ptr;
        if (tag->type == MULTIBOOT2_TAG_TYPE_END) {
            break;
        }
        if (tag->type == MULTIBOOT2_TAG_TYPE_MODULE) {
            struct multiboot_tag_module *mod = (struct multiboot_tag_module *)tag;
            reserve_range(mod->mod_start, mod->mod_end);
        }
        tag_ptr += (tag->size + 7) & ~7u;
    }
}

void pmm_reserve(uint64_t phys, uint64_t len) {
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    reserve_range(phys, phys + len);
    spin_unlock_irqrestore(&pmm_lock, flags);
}

// Is this frame free? Caller holds the lock.
static int frame_free(uint64_t f) {
    if (f >= MAX_FRAMES) return 0;
    return (bitmap[f / 8] & (1 << (f % 8))) != 0;
}

uint64_t pmm_alloc_contig(uint64_t count) {
    if (count == 0) return 0;
    if (count == 1) return pmm_alloc_frame();

    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    uint64_t run = 0, start = 0;
    for (uint64_t f = 0; f < MAX_FRAMES; f++) {
        if (!frame_free(f)) { run = 0; continue; }
        if (run == 0) start = f;
        if (++run == count) {
            for (uint64_t i = 0; i < count; i++) mark_used(start + i);
            spin_unlock_irqrestore(&pmm_lock, flags);
            uint8_t *p = (uint8_t *)(uintptr_t)(start * PAGE_SIZE);
            for (uint64_t i = 0; i < count * PAGE_SIZE; i++) p[i] = 0;
            return start * PAGE_SIZE;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return 0;
}

uint64_t pmm_alloc_frame(void) {
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    for (uint64_t byte = 0; byte < BITMAP_BYTES; byte++) {
        if (bitmap[byte] == 0) continue;
        for (int bit = 0; bit < 8; bit++) {
            if (bitmap[byte] & (1 << bit)) {
                uint64_t frame = byte * 8 + bit;
                mark_used(frame);
                spin_unlock_irqrestore(&pmm_lock, flags);
                return frame * PAGE_SIZE;
            }
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return 0; // out of memory
}

void pmm_free_frame(uint64_t phys_addr) {
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    mark_free(phys_addr / PAGE_SIZE);
    spin_unlock_irqrestore(&pmm_lock, flags);
}

uint64_t pmm_free_frame_count(void) {
    return free_frames;
}

uint64_t pmm_total_frame_count(void) {
    return usable_frames;
}
