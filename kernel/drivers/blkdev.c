#include "blkdev.h"
#include "virtio_blk.h"
#include "../include/multiboot2.h"
#include "../kernel/kio.h"
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

int blkdev_init(uint32_t multiboot_addr) {
    // virtio-blk first: it is the real device in QEMU. On bare metal there is
    // no PCI virtio device, so this fails fast and we fall back to the module.
    if (virtio_blk_init()) {
        backend = BACKEND_VIRTIO;
        kprintf("blkdev: virtio-blk\n");
        return 1;
    }

    uint8_t *base = NULL;
    uint64_t size = 0;
    if (find_module(multiboot_addr, "disk", &base, &size) && size >= 1024) {
        ram_base = base;
        ram_sectors = size / 512;
        backend = BACKEND_RAM;
        kprintf("blkdev: RAM disk from GRUB module, %u KiB\n",
                (unsigned int)(size / 1024));
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
