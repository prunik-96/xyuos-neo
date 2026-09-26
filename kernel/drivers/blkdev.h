#ifndef BLKDEV_H
#define BLKDEV_H

#include <stdint.h>

// The block device the filesystem sits on. Three backends:
//
//   virtio-blk  -- in QEMU with a -drive; the real path during development.
//   boot stick  -- the ext2 partition the build appends to the ISO, on the
//                  USB stick the machine booted from. Writes survive a reboot.
//                  Found by the UUID the build hands over as the "diskid"
//                  GRUB module; nothing outside that one partition is touched.
//   RAM disk    -- the disk image loaded as a GRUB module (initrd). The
//                  fallback when the stick cannot be used. Writes live only in
//                  RAM (lost on reboot).
//
// There is no driver for the machine's internal disk (NVMe or SATA), and so
// no code here can ever reach it -- a structural guarantee, not carefulness.
//
// blkdev_init tries them in that order, so the same kernel boots in QEMU and
// on hardware unchanged. It must run after xhci_init, which finds the sticks.
int blkdev_init(uint32_t multiboot_addr);

// `count` sectors starting at `lba`, in one request to the device. `buf` must
// be kernel memory that is contiguous physically as well as virtually -- a
// static buffer, or a single kmalloc block -- since the device writes to it
// directly. Asking for a whole block this way instead of sector by sector is
// what a virtual disk notices, and a USB stick far more: each request is a
// command, its data and a status, three transfers on the bus.
int blkdev_read_sectors(uint64_t lba, uint32_t count, void *buf);
int blkdev_write_sectors(uint64_t lba, uint32_t count, const void *buf);
int blkdev_read_sector(uint64_t lba, void *buf512);
int blkdev_write_sector(uint64_t lba, const void *buf512);

#define BACKEND_NONE   0
#define BACKEND_VIRTIO 1
#define BACKEND_RAM    2
#define BACKEND_STICK  3

// Which backend, for the boot banner and the device list.
int blkdev_backend(void);

// The USB disk the root filesystem is on, or -1: the FAT32 data-stick code
// must never mistake it for a stick of its own.
int blkdev_usb_dev(void);

#endif
