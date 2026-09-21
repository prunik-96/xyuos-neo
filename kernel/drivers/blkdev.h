#ifndef BLKDEV_H
#define BLKDEV_H

#include <stdint.h>

// The block device the filesystem sits on. Two backends:
//
//   virtio-blk  -- in QEMU with a -drive; the real path during development.
//   RAM disk    -- the disk image loaded as a GRUB module (initrd). Used on
//                  bare metal, where virtio does not exist and we have no
//                  AHCI/NVMe driver. Writes live only in RAM (lost on reboot),
//                  which is exactly why no code here can ever touch a real
//                  disk -- a structural guarantee, not carefulness.
//
// blkdev_init prefers virtio and falls back to the module, so the same kernel
// boots in QEMU and on hardware unchanged.
int blkdev_init(uint32_t multiboot_addr);
int blkdev_read_sector(uint64_t lba, void *buf512);
int blkdev_write_sector(uint64_t lba, const void *buf512);

// 0 none, 1 virtio, 2 RAM disk. For the boot banner.
int blkdev_backend(void);

#endif
