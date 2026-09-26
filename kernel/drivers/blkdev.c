#include "blkdev.h"
#include "virtio_blk.h"
#include "xhci.h"
#include "../include/multiboot2.h"
#include "../kernel/kio.h"
#include "../mm/paging.h"
#include "../mm/pmm.h"
#include <stddef.h>

static int      backend = BACKEND_NONE;
static uint8_t *ram_base = NULL;
static uint64_t ram_sectors = 0;

// The boot stick: which USB disk, and where on it the partition is. Every
// sector the filesystem asks for is relative to `stick_first` and checked
// against `stick_sectors`, so nothing outside that one partition can ever be
// read or written -- not the ISO that boots the machine, not the EFI
// partition, not the rest of the stick.
static int      stick_dev = -1;
static uint64_t stick_first, stick_sectors;

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

// --- the boot stick ----------------------------------------------------------
//
// The stick is written from the ISO the build makes, and the build appends to
// that ISO a partition holding the ext2 root filesystem. Which partition, on
// which of possibly several sticks, is settled by one thing only: the UUID of
// that filesystem, which the build also puts beside the kernel as a tiny GRUB
// module called "diskid". A stick with no partition carrying that UUID -- a
// data stick, a stick from an earlier build, a backup drive -- is never
// written to. And there is no driver for the machine's internal disk at all.

// Sector buffers the controller writes into directly: static, so physically
// contiguous, and aligned so that none crosses a 64 KiB line.
static uint8_t sec[1024] __attribute__((aligned(1024)));

static int hexval(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "11111111-2222-3333-4444-555555555555" -> 16 bytes, in the order ext2 keeps
// them (the order they are written in).
static int parse_uuid(const uint8_t *s, uint64_t len, uint8_t out[16]) {
    int n = 0;
    for (uint64_t i = 0; i < len && n < 32; i++) {
        if (s[i] == '-') continue;
        int v = hexval(s[i]);
        if (v < 0) break;
        if (n & 1) out[n / 2] |= (uint8_t)v;
        else out[n / 2] = (uint8_t)(v << 4);
        n++;
    }
    return n == 32;
}

// Is there an ext2 filesystem with this UUID at `first` on disk `dev`?
static int ext2_with_uuid(int dev, uint64_t first, const uint8_t want[16]) {
    if (!usb_disk_read(dev, (uint32_t)(first + 2), 2, sec)) return 0;  // superblock
    if (sec[56] != 0x53 || sec[57] != 0xEF) return 0;                  // s_magic
    for (int i = 0; i < 16; i++) if (sec[104 + i] != want[i]) return 0; // s_uuid
    return 1;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static int take(int dev, uint64_t first, uint64_t count, uint32_t disk_blocks,
                const uint8_t want[16]) {
    if (!first || !count || first + count > disk_blocks) return 0;
    if (!ext2_with_uuid(dev, first, want)) return 0;
    stick_dev = dev;
    stick_first = first;
    stick_sectors = count;
    return 1;
}

// Look at every partition on every USB disk. Both tables are read: the ISO
// grub-mkrescue writes carries a GPT and an MBR describing the same appended
// partition, and a tool that rewrote one of them should not lose the disk.
static int find_stick(const uint8_t want[16]) {
    for (int dev = 0; dev < usb_disk_count(); dev++) {
        uint32_t blocks = 0, bsize = 0;
        if (!usb_disk_capacity(dev, &blocks, &bsize) || bsize != 512) continue;
        if (!usb_disk_read(dev, 0, 1, sec)) continue;
        if (sec[510] != 0x55 || sec[511] != 0xAA) continue;

        int gpt = 0;
        uint64_t mbr_first[4], mbr_count[4];
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = sec + 446 + 16 * i;
            mbr_first[i] = mbr_count[i] = 0;
            if (e[4] == 0xEE) gpt = 1;
            else if (e[4]) { mbr_first[i] = le32(e + 8); mbr_count[i] = le32(e + 12); }
        }
        for (int i = 0; i < 4; i++)
            if (take(dev, mbr_first[i], mbr_count[i], blocks, want)) return 1;

        if (!gpt || !usb_disk_read(dev, 1, 1, sec)) continue;
        if (le64(sec) != 0x5452415020494645ULL) continue;         // "EFI PART"
        uint64_t table = le64(sec + 72);
        uint32_t entries = le32(sec + 80), esize = le32(sec + 84);
        if (esize < 128 || esize > 512 || entries > 256) continue;
        for (uint32_t i = 0; i < entries; i++) {
            uint64_t at = (uint64_t)i * esize;
            if (!usb_disk_read(dev, (uint32_t)(table + at / 512), 1, sec)) break;
            const uint8_t *e = sec + at % 512;
            int used = 0;
            for (int k = 0; k < 16; k++) used |= e[k];
            if (!used) continue;
            uint64_t first = le64(e + 32), last = le64(e + 40);
            if (last < first) continue;
            if (take(dev, first, last - first + 1, blocks, want)) return 1;
        }
    }
    return 0;
}

// --- choosing ----------------------------------------------------------------

int blkdev_init(uint32_t multiboot_addr) {
    uint8_t *base = NULL;
    uint64_t size = 0;
    int have = find_module(multiboot_addr, "disk", &base, &size) && size >= 1024;
    uint8_t *idtext = NULL;
    uint64_t idlen = 0;
    int want_stick = find_module(multiboot_addr, "diskid", &idtext, &idlen);

    // virtio-blk first: it is the real device in QEMU. On bare metal there is
    // no PCI virtio device, so this fails fast and we go on.
    if (virtio_blk_init()) {
        backend = BACKEND_VIRTIO;
        kprintf("blkdev: virtio-blk\n");
        // GRUB loaded the image anyway, since the menu cannot know. With a
        // real device it is dead weight the size of the whole disk.
        if (have) release((uint64_t)(uintptr_t)base, (uint64_t)(uintptr_t)base + size);
        return 1;
    }

    // Then the stick the machine booted from, when the menu entry asked for
    // it by naming the filesystem's UUID. Writes to it survive a reboot.
    uint8_t uuid[16];
    if (want_stick && parse_uuid(idtext, idlen, uuid)) {
        if (find_stick(uuid)) {
            backend = BACKEND_STICK;
            kprintf("blkdev: the boot stick, USB disk %d, partition at sector %u, %u MiB\n",
                    stick_dev, (unsigned)stick_first, (unsigned)(stick_sectors / 2048));
            if (have) release((uint64_t)(uintptr_t)base, (uint64_t)(uintptr_t)base + size);
            return 1;
        }
        kprintf("blkdev: the boot stick's disk was not found on any of %d USB disk(s)\n",
                usb_disk_count());
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

    if (want_stick)
        kprintf("blkdev: no disk. Restart and choose \"disk in memory\" in the "
                "boot menu.\n");
    else
        kprintf("blkdev: no disk (no virtio device, no 'disk' module)\n");
    return 0;
}

int blkdev_backend(void) { return backend; }
int blkdev_usb_dev(void) { return backend == BACKEND_STICK ? stick_dev : -1; }

int blkdev_read_sectors(uint64_t lba, uint32_t count, void *buf) {
    if (backend == BACKEND_VIRTIO) return virtio_blk_read_sectors(lba, count, buf);
    if (backend == BACKEND_STICK) {
        if (lba + count > stick_sectors) return 0;
        return usb_disk_read(stick_dev, (uint32_t)(stick_first + lba), count, buf);
    }
    if (backend == BACKEND_RAM) {
        if (lba + count > ram_sectors) return 0;
        const uint64_t *src = (const uint64_t *)(ram_base + lba * 512);
        uint64_t *dst = (uint64_t *)buf;
        for (uint32_t i = 0; i < count * 64; i++) dst[i] = src[i];
        return 1;
    }
    return 0;
}

int blkdev_write_sectors(uint64_t lba, uint32_t count, const void *buf) {
    if (backend == BACKEND_VIRTIO) {
        for (uint32_t i = 0; i < count; i++)
            if (!virtio_blk_write_sector(lba + i, (const uint8_t *)buf + i * 512))
                return 0;
        return 1;
    }
    if (backend == BACKEND_STICK) {
        if (lba + count > stick_sectors) return 0;
        return usb_disk_write(stick_dev, (uint32_t)(stick_first + lba), count, buf);
    }
    if (backend == BACKEND_RAM) {
        if (lba + count > ram_sectors) return 0;
        const uint64_t *src = (const uint64_t *)buf;
        uint64_t *dst = (uint64_t *)(ram_base + lba * 512);
        for (uint32_t i = 0; i < count * 64; i++) dst[i] = src[i];
        return 1;   // RAM only: not written back to any real disk on reboot
    }
    return 0;
}

int blkdev_read_sector(uint64_t lba, void *buf512) {
    return blkdev_read_sectors(lba, 1, buf512);
}

int blkdev_write_sector(uint64_t lba, const void *buf512) {
    return blkdev_write_sectors(lba, 1, buf512);
}
