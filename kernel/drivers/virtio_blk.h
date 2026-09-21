#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdint.h>

int virtio_blk_init(void);
int virtio_blk_read_sector(uint64_t lba, void *buf512);
int virtio_blk_write_sector(uint64_t lba, const void *buf512);

#endif
