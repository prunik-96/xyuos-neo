#include "blkdev.h"
#include "virtio_blk.h"
#include "../include/multiboot2.h"
#include "../kernel/kio.h"
#include "../mm/paging.h"
#include "../mm/pmm.h"
#include <stddef.h>

#define BACKEND_NONE   0
#define BACKEND_VIRTIO 1
#define BACKEND_RAM    2

static int      backend = BACKEND_NONE;
static uint8_t *ram_base = NULL;
static uint64_t ram_sectors = 0;

static int name_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

// Find a GRUB module by its cmdline name. Its memory is already reserved by the
// PMM (pmm_init walks the module tags), so the RAM disk is not clobbered.
static int find_module(uint32_t multiboot_addr, const char *name,
                       uint8_t **out_base, uint64_t *out_size) {
    struct multiboot_info *info = (struct multiboot_info *)(uintptr_t)multiboot_addr;
    uint8_t *tag_ptr = (uint8_t *)info + 8;

    for (;;) {
        struct multiboot_tag *tag = (struct multiboot_tag *)tag_ptr;
        if (tag->type == MULTIBOOT2_TAG_TYPE_END) break;
        if (tag->type == MULTIBOOT2_TAG_TYPE_MODULE) {
            struct multiboot_tag_module *mod = (struct multiboot_tag_module *)tag;
            if (name_eq(mod->cmdline, name)) {
                *out_base = (uint8_t *)(uintptr_t)mod->mod_start;
                *out_size = (uint64_t)mod->mod_end - mod->mod_start;
                return 1;
            }
        }
        tag_ptr += (tag->size + 7) & ~7u;
    }
    return 0;
}

// Give a module's frames back to the allocator: only whole pages, and only
// those outside the program window. GRUB may put its information structure in
// the page where the module ends, so a partial page is never freed.
static void release(uint64_t lo, uint64_t hi) {
    for (uint64_t a = (lo + 4095) & ~0xFFFULL; a + 4096 <= hi; a += 4096)
        if (a < USER_VIRT_BASE || a >= USER_VIRT_END) pmm_free_frame(a);
}

// GRUB puts a module wherever it finds room, and the room it finds grows with
// the module. A small image lands just above the kernel; a big one can land
// anywhere below 4 GiB -- including [1 GiB, 2 GiB), which is the one range the
// kernel cannot use (see paging.h). Inside a process address space that range
// is the program's own memory, so a disk read made during a system call would
// return whatever the program had there, and a write would scribble on it.
//
// So a module that touches that range is moved out of it, once, here. This
// runs from kmain before the first process exists, on the boot page tables,
// which still map the whole low 4 GiB one to one -- so both copies are
// reachable. The frames it leaves behind outside the window go back to the
// allocator; the ones inside it were never the allocator's to have.
static uint8_t *settle(uint8_t *base, uint64_t size) {
    uint64_t lo = (uint64_t)(uintptr_t)base, hi = lo + size;
    if (hi <= USER_VIRT_BASE || lo >= USER_VIRT_END) return base;

    uint64_t frames = (size + 4095) / 4096;
    uint64_t to = pmm_alloc_contig(frames);
    if (!to) {
        kprintf("blkdev: the RAM disk sits in the program window and there is "
                "no room to move it: %u KiB wanted\n", (unsigned)(size / 1024));
        return NULL;
    }
    const uint64_t *src = (const uint64_t *)(uintptr_t)lo;
    uint64_t *dst = (uint64_t *)(uintptr_t)to;
    for (uint64_t i = 0; i < size / 8; i++) dst[i] = src[i];
    for (uint64_t i = size & ~7ULL; i < size; i++)
        ((uint8_t *)dst)[i] = ((const uint8_t *)src)[i];
    release(lo, hi);

    kprintf("blkdev: moved it out of the program window, to 0x%x\n",
            (unsigned)to);
    return (uint8_t *)(uintptr_t)to;
}

int blkdev_init(uint32_t multiboot_addr) {
    // virtio-blk first: it is the real device in QEMU. On bare metal there is
    // no PCI virtio device, so this fails fast and we fall back to the module.
    uint8_t *base = NULL;
    uint64_t size = 0;
    int have = find_module(multiboot_addr, "disk", &base, &size) && size >= 1024;

    if (virtio_blk_init()) {
        backend = BACKEND_VIRTIO;
        kprintf("blkdev: virtio-blk\n");
        // GRUB loaded the image anyway, since the menu cannot know. With a
        // real device it is dead weight the size of the whole disk.
        if (have) release((uint64_t)(uintptr_t)base, (uint64_t)(uintptr_t)base + size);
        return 1;
    }

    if (have) {
        kprintf("blkdev: RAM disk from GRUB module, %u KiB at 0x%x\n",
                (unsigned int)(size / 1024), (unsigned int)(uintptr_t)base);
        base = settle(base, size);
        if (!base) return 0;
        ram_base = base;
        ram_sectors = size / 512;
        backend = BACKEND_RAM;
        return 1;
    }

    kprintf("blkdev: no disk (no virtio device, no 'disk' module)\n");
    return 0;
}

int blkdev_backend(void) { return backend; }

int blkdev_read_sector(uint64_t lba, void *buf512) {
    if (backend == BACKEND_VIRTIO) return virtio_blk_read_sector(lba, buf512);
    if (backend == BACKEND_RAM) {
        if (lba >= ram_sectors) return 0;
        const uint8_t *src = ram_base + lba * 512;
        uint8_t *dst = (uint8_t *)buf512;
        for (int i = 0; i < 512; i++) dst[i] = src[i];
        return 1;
    }
    return 0;
}

int blkdev_read_sectors(uint64_t lba, uint32_t count, void *buf) {
    if (backend == BACKEND_VIRTIO) return virtio_blk_read_sectors(lba, count, buf);
    if (backend == BACKEND_RAM) {
        if (lba + count > ram_sectors) return 0;
        const uint64_t *src = (const uint64_t *)(ram_base + lba * 512);
        uint64_t *dst = (uint64_t *)buf;
        for (uint32_t i = 0; i < count * 64; i++) dst[i] = src[i];
        return 1;
    }
    return 0;
}

int blkdev_write_sector(uint64_t lba, const void *buf512) {
    if (backend == BACKEND_VIRTIO) return virtio_blk_write_sector(lba, buf512);
    if (backend == BACKEND_RAM) {
        if (lba >= ram_sectors) return 0;
        uint8_t *dst = ram_base + lba * 512;
        const uint8_t *src = (const uint8_t *)buf512;
        for (int i = 0; i < 512; i++) dst[i] = src[i];
        return 1;   // RAM only: not written back to any real disk on reboot
    }
    return 0;
}
