#include "virtio_blk.h"
#include "pci.h"
#include "../include/port_io.h"
#include "../kernel/kio.h"
#include "../mm/pmm.h"
#include <stddef.h>

#define VIRTIO_VENDOR_ID 0x1AF4
#define VIRTIO_BLK_DEVICE_ID_LEGACY 0x1001
#define VIRTIO_BLK_DEVICE_ID_MODERN 0x1042

#define REG_DEVICE_FEATURES 0x00
#define REG_GUEST_FEATURES  0x04
#define REG_QUEUE_ADDRESS   0x08
#define REG_QUEUE_SIZE      0x0C
#define REG_QUEUE_SELECT    0x0E
#define REG_QUEUE_NOTIFY    0x10
#define REG_DEVICE_STATUS   0x12
#define REG_ISR_STATUS      0x13

#define STATUS_ACKNOWLEDGE 1
#define STATUS_DRIVER      2
#define STATUS_DRIVER_OK   4

#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

#define VIRTIO_BLK_T_IN  0
#define VIRTIO_BLK_T_OUT 1

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} virtio_blk_req_hdr_t;

static uint16_t io_base = 0;
static uint16_t queue_size = 0;

static uint8_t *vq_mem = NULL;
static virtq_desc_t *desc_table = NULL;
static uint8_t *avail_base = NULL;
static uint8_t *used_base = NULL;
static uint16_t next_avail_idx = 0;
static uint16_t last_used_idx = 0;

static volatile virtio_blk_req_hdr_t req_hdr __attribute__((aligned(16)));
static volatile uint8_t req_status __attribute__((aligned(16)));

static uint32_t align_up32(uint32_t v, uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}

static void *alloc_contig_pages(uint32_t count) {
    uint64_t start = pmm_alloc_frame();
    if (start == 0) return NULL;
    uint64_t expect = start + PAGE_SIZE;
    for (uint32_t i = 1; i < count; i++) {
        uint64_t f = pmm_alloc_frame();
        if (f != expect) return NULL; // not contiguous; simple driver gives up
        expect += PAGE_SIZE;
    }
    return (void *)(uintptr_t)start;
}

int virtio_blk_init(void) {
    pci_device_t dev;
    if (!pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_BLK_DEVICE_ID_LEGACY, &dev) &&
        !pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_BLK_DEVICE_ID_MODERN, &dev)) {
        kprintf("virtio-blk: no device found\n");
        return 0;
    }

    if (!(dev.bar[0] & 1)) {
        kprintf("virtio-blk: BAR0 is not I/O space\n");
        return 0;
    }
    io_base = (uint16_t)(dev.bar[0] & 0xFFFC);

    // enable I/O space + bus mastering
    uint32_t cmd = pci_config_read32(dev.bus, dev.slot, dev.func, 0x04);
    cmd |= 0x5;
    pci_config_write32(dev.bus, dev.slot, dev.func, 0x04, cmd);

    outb(io_base + REG_DEVICE_STATUS, 0); // reset
    outb(io_base + REG_DEVICE_STATUS, STATUS_ACKNOWLEDGE);
    outb(io_base + REG_DEVICE_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

    // negotiate no optional features -- plain sector I/O, one buffer per
    // request (a sector, or the sectors of one filesystem block).
    __asm__ volatile ("outl %0, %1" : : "a"((uint32_t)0), "Nd"((uint16_t)(io_base + REG_GUEST_FEATURES)));

    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0), "Nd"((uint16_t)(io_base + REG_QUEUE_SELECT)));
    uint16_t qsz;
    __asm__ volatile ("inw %1, %0" : "=a"(qsz) : "Nd"((uint16_t)(io_base + REG_QUEUE_SIZE)));
    queue_size = qsz;
    if (queue_size == 0) {
        kprintf("virtio-blk: queue 0 unavailable (io=0x%x bar0=0x%x dev=0x%x)\n",
                (unsigned)io_base, (unsigned)dev.bar[0], (unsigned)dev.device_id);
        return 0;
    }

    uint32_t desc_avail_size = (uint32_t)queue_size * sizeof(virtq_desc_t)
        + 4 + (uint32_t)queue_size * 2 + 2;
    uint32_t used_offset = align_up32(desc_avail_size, 4096);
    uint32_t used_size = 4 + (uint32_t)queue_size * 8 + 2;
    uint32_t total = used_offset + used_size;
    uint32_t pages = (total + 4095) / 4096;

    vq_mem = (uint8_t *)alloc_contig_pages(pages);
    if (!vq_mem) {
        kprintf("virtio-blk: failed to allocate contiguous virtqueue memory\n");
        return 0;
    }
    for (uint32_t i = 0; i < pages * 4096; i++) vq_mem[i] = 0;

    desc_table = (virtq_desc_t *)vq_mem;
    avail_base = vq_mem + (uint32_t)queue_size * sizeof(virtq_desc_t);
    used_base = vq_mem + used_offset;

    uint32_t pfn = (uint32_t)((uintptr_t)vq_mem / 4096);
    __asm__ volatile ("outl %0, %1" : : "a"(pfn), "Nd"((uint16_t)(io_base + REG_QUEUE_ADDRESS)));

    outb(io_base + REG_DEVICE_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_DRIVER_OK);

    kprintf("virtio-blk: ready, io_base=0x%x queue_size=%u\n", io_base, queue_size);
    return 1;
}

static int submit_request(uint64_t lba, void *buf, uint32_t len, int write) {
    req_hdr.type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    req_hdr.reserved = 0;
    req_hdr.sector = lba;
    req_status = 0xFF;

    desc_table[0].addr = (uint64_t)(uintptr_t)&req_hdr;
    desc_table[0].len = sizeof(virtio_blk_req_hdr_t);
    desc_table[0].flags = VIRTQ_DESC_F_NEXT;
    desc_table[0].next = 1;

    desc_table[1].addr = (uint64_t)(uintptr_t)buf;
    desc_table[1].len = len;
    desc_table[1].flags = VIRTQ_DESC_F_NEXT | (write ? 0 : VIRTQ_DESC_F_WRITE);
    desc_table[1].next = 2;

    desc_table[2].addr = (uint64_t)(uintptr_t)&req_status;
    desc_table[2].len = 1;
    desc_table[2].flags = VIRTQ_DESC_F_WRITE;
    desc_table[2].next = 0;

    uint16_t *avail_idx_ptr = (uint16_t *)(avail_base + 2);
    uint16_t *avail_ring = (uint16_t *)(avail_base + 4);
    avail_ring[next_avail_idx % queue_size] = 0;
    __asm__ volatile ("" ::: "memory");
    next_avail_idx++;
    *avail_idx_ptr = next_avail_idx;
    __asm__ volatile ("" ::: "memory");

    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0), "Nd"((uint16_t)(io_base + REG_QUEUE_NOTIFY)));

    volatile uint16_t *used_idx_ptr = (volatile uint16_t *)(used_base + 2);
    uint32_t spins = 0;
    while (*used_idx_ptr == last_used_idx) {
        spins++;
        if (spins > 100000000u) {
            kprintf("virtio-blk: request timed out\n");
            return 0;
        }
    }
    last_used_idx = *used_idx_ptr;

    return req_status == 0;
}

int virtio_blk_read_sector(uint64_t lba, void *buf512) {
    return submit_request(lba, buf512, 512, 0);
}

int virtio_blk_read_sectors(uint64_t lba, uint32_t count, void *buf) {
    return submit_request(lba, buf, count * 512, 0);
}

int virtio_blk_write_sector(uint64_t lba, const void *buf512) {
    return submit_request(lba, (void *)(uintptr_t)buf512, 512, 1);
}
