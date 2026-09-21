#include "e1000.h"
#include "net.h"
#include "nic.h"
#include "../drivers/pci.h"
#include "../mm/pmm.h"
#include "../mm/paging.h"
#include "../kernel/kio.h"
#include "../arch/x86_64/pit.h"
#include <stddef.h>

// --- register offsets (byte addresses into BAR0 MMIO) ---------------------
#define E1000_CTRL   0x0000
#define E1000_STATUS 0x0008
#define E1000_EERD   0x0014   // EEPROM read
#define E1000_ICR    0x00C0   // interrupt cause (read)
#define E1000_IMS    0x00D0   // interrupt mask set
#define E1000_IMC    0x00D8   // interrupt mask clear
#define E1000_RCTL   0x0100
#define E1000_TCTL   0x0400
#define E1000_TIPG   0x0410
#define E1000_RDBAL  0x2800
#define E1000_RDBAH  0x2804
#define E1000_RDLEN  0x2808
#define E1000_RDH    0x2810
#define E1000_RDT    0x2818
#define E1000_TDBAL  0x3800
#define E1000_TDBAH  0x3804
#define E1000_TDLEN  0x3808
#define E1000_TDH    0x3810
#define E1000_TDT    0x3818
#define E1000_MTA    0x5200   // 128-entry multicast table
#define E1000_RAL0   0x5400
#define E1000_RAH0   0x5404

#define CTRL_RST     (1u << 26)
#define CTRL_ASDE    (1u << 5)
#define CTRL_SLU     (1u << 6)

// RCTL bits
#define RCTL_EN      (1u << 1)
#define RCTL_BAM     (1u << 15)  // accept broadcast
#define RCTL_SECRC   (1u << 26)  // strip Ethernet CRC
#define RCTL_BSIZE_2048 0        // bits 16:17 = 00, and no BSEX

// TCTL bits
#define TCTL_EN      (1u << 1)
#define TCTL_PSP     (1u << 3)   // pad short packets
#define TCTL_CT_SHIFT   4
#define TCTL_COLD_SHIFT 12

// RX descriptor status
#define RXD_STAT_DD  0x01        // descriptor done
#define RXD_STAT_EOP 0x02        // end of packet

// TX descriptor command / status
#define TXD_CMD_EOP  0x01
#define TXD_CMD_IFCS 0x02        // insert FCS/CRC
#define TXD_CMD_RS   0x08        // report status
#define TXD_STAT_DD  0x01

// A receive ring is measured in PACKETS, not bytes, and the window we
// advertise lets the peer put roughly 45 segments in flight at once. Thirty-two
// descriptors could not hold that: the ring overflowed mid-burst, one segment
// was lost, and a receiver that only accepts data in order then threw away
// every segment behind it until the retransmission arrived -- turning a 400KB
// download into a ten-second stall. 256 descriptors hold a whole window with
// room to spare, at the cost of half a megabyte.
#define NUM_RX 256
#define NUM_TX 32
#define BUF_SIZE 2048

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} rx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} tx_desc_t;

static volatile uint8_t *mmio = NULL;
static uint8_t mac[6];

static rx_desc_t *rx_ring = NULL;
static tx_desc_t *tx_ring = NULL;
static uint8_t   *rx_bufs = NULL;   // NUM_RX * BUF_SIZE, contiguous
static uint8_t   *tx_bufs = NULL;   // NUM_TX * BUF_SIZE, contiguous
static uint16_t   tx_tail = 0;
static uint16_t   rx_tail = 0;

static inline void reg_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(mmio + reg) = val;
}
static inline uint32_t reg_read(uint32_t reg) {
    return *(volatile uint32_t *)(mmio + reg);
}

// Contiguous physical pages, used directly as a pointer (low RAM is identity
// mapped, so physical == virtual here -- same assumption as virtio-blk).
static void *alloc_contig(uint32_t bytes) {
    uint64_t start = pmm_alloc_contig((bytes + 4095) / 4096);
    return start ? (void *)(uintptr_t)start : NULL;
}

// A short spin. The card runs at real speed; these loops just give MMIO writes
// time to land during reset before we poke it again.
static void io_delay(volatile uint32_t n) { while (n--) __asm__ volatile(""); }

static int read_mac(void) {
    // In QEMU the receive-address registers come pre-loaded with the MAC, so
    // read them directly and skip the EEPROM dance.
    uint32_t low  = reg_read(E1000_RAL0);
    uint32_t high = reg_read(E1000_RAH0);
    if (low != 0 || (high & 0xFFFF) != 0) {
        mac[0] = low & 0xFF;
        mac[1] = (low >> 8) & 0xFF;
        mac[2] = (low >> 16) & 0xFF;
        mac[3] = (low >> 24) & 0xFF;
        mac[4] = high & 0xFF;
        mac[5] = (high >> 8) & 0xFF;
        return 1;
    }
    // Fallback: read words 0..2 from EEPROM.
    for (int i = 0; i < 3; i++) {
        reg_write(E1000_EERD, (uint32_t)(i << 8) | 1);
        uint32_t v;
        uint32_t spins = 0;
        do { v = reg_read(E1000_EERD); } while (!(v & 0x10) && ++spins < 1000000);
        uint16_t w = (v >> 16) & 0xFFFF;
        mac[i * 2]     = w & 0xFF;
        mac[i * 2 + 1] = (w >> 8) & 0xFF;
    }
    return 1;
}

static void setup_rx(void) {
    rx_ring = (rx_desc_t *)alloc_contig(NUM_RX * sizeof(rx_desc_t));
    rx_bufs = (uint8_t *)alloc_contig(NUM_RX * BUF_SIZE);
    for (int i = 0; i < NUM_RX; i++) {
        rx_ring[i].addr = (uint64_t)(uintptr_t)(rx_bufs + i * BUF_SIZE);
        rx_ring[i].status = 0;
    }
    uint64_t ring_phys = (uint64_t)(uintptr_t)rx_ring;
    reg_write(E1000_RDBAL, (uint32_t)ring_phys);
    reg_write(E1000_RDBAH, (uint32_t)(ring_phys >> 32));
    reg_write(E1000_RDLEN, NUM_RX * sizeof(rx_desc_t));
    reg_write(E1000_RDH, 0);
    reg_write(E1000_RDT, NUM_RX - 1);   // hand every descriptor to the card
    rx_tail = 0;
    reg_write(E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);
}

static void setup_tx(void) {
    tx_ring = (tx_desc_t *)alloc_contig(NUM_TX * sizeof(tx_desc_t));
    tx_bufs = (uint8_t *)alloc_contig(NUM_TX * BUF_SIZE);
    for (int i = 0; i < NUM_TX; i++) {
        tx_ring[i].addr = (uint64_t)(uintptr_t)(tx_bufs + i * BUF_SIZE);
        tx_ring[i].status = TXD_STAT_DD;   // mark free
    }
    uint64_t ring_phys = (uint64_t)(uintptr_t)tx_ring;
    reg_write(E1000_TDBAL, (uint32_t)ring_phys);
    reg_write(E1000_TDBAH, (uint32_t)(ring_phys >> 32));
    reg_write(E1000_TDLEN, NUM_TX * sizeof(tx_desc_t));
    reg_write(E1000_TDH, 0);
    reg_write(E1000_TDT, 0);
    tx_tail = 0;
    // full-duplex collision threshold 0x10, back-off distance 0x40, IPG legacy.
    reg_write(E1000_TCTL, TCTL_EN | TCTL_PSP |
              (0x10u << TCTL_CT_SHIFT) | (0x40u << TCTL_COLD_SHIFT));
    reg_write(E1000_TIPG, 0x0060200A);
}

int e1000_init(void) {
    pci_device_t dev;
    // 82540EM. (0x100E is the classic QEMU e1000; keep it simple and match it.)
    if (!pci_find_device(0x8086, 0x100E, &dev)) {
        return 0;   // no NIC; quietly do nothing
    }

    uint64_t bar = dev.bar[0] & ~0xFULL;
    if (dev.bar[0] & 0x4) bar |= ((uint64_t)dev.bar[1] << 32);  // 64-bit BAR
    paging_map_mmio(bar, 0x20000);   // e1000 register space is 128 KiB
    mmio = (volatile uint8_t *)(uintptr_t)bar;

    pci_enable_bus_master(&dev);

    // Reset the device, then wait for it to settle.
    reg_write(E1000_IMC, 0xFFFFFFFF);       // mask all interrupts
    reg_write(E1000_CTRL, reg_read(E1000_CTRL) | CTRL_RST);
    io_delay(1000000);
    reg_write(E1000_IMC, 0xFFFFFFFF);

    // Set link up + auto-speed detect.
    reg_write(E1000_CTRL, reg_read(E1000_CTRL) | CTRL_SLU | CTRL_ASDE);

    read_mac();

    // Clear the multicast table (128 dwords).
    for (int i = 0; i < 128; i++) reg_write(E1000_MTA + i * 4, 0);

    setup_rx();
    setup_tx();

    kprintf("e1000: up, mac %x:%x:%x:%x:%x:%x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 1;
}

const uint8_t *e1000_mac(void) { return mac; }

int e1000_send(const void *frame, uint16_t len) {
    if (!mmio) return -1;
    if (len > BUF_SIZE) len = BUF_SIZE;

    tx_desc_t *d = &tx_ring[tx_tail];
    // Wait for this slot to be free (card sets DD when it's done with it).
    uint32_t spins = 0;
    while (!(d->status & TXD_STAT_DD)) {
        if (++spins > 100000000u) return -1;   // wedged
    }

    uint8_t *buf = tx_bufs + tx_tail * BUF_SIZE;
    const uint8_t *src = (const uint8_t *)frame;
    for (uint16_t i = 0; i < len; i++) buf[i] = src[i];

    d->length = len;
    d->cso = 0;
    d->css = 0;
    d->special = 0;
    d->status = 0;
    d->cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;

    tx_tail = (tx_tail + 1) % NUM_TX;
    __asm__ volatile ("" ::: "memory");
    reg_write(E1000_TDT, tx_tail);
    return 0;
}

/* See the note on letting the host run, below. */
static uint64_t last_device_touch_us;

void e1000_poll(void) {
    if (!mmio) return;

    /* Touch the card itself now and then, even when there is nothing to
     * collect.
     *
     * Everything below reads descriptors the card wrote by DMA, which is
     * ordinary memory: a wait loop calling this runs flat out and never
     * leaves the virtual machine. The emulator's own loop is what moves the
     * packets, and starved of any chance to run it falls back to a half
     * second timer -- which is exactly the delay that was measured on every
     * connection after the first.
     *
     * A register read is an access to the device, so it hands control back.
     * Doing it every time would be wrong for a real card, where each one
     * crosses the bus and there is no emulator to starve; every two hundred
     * microseconds costs five thousand reads a second at worst and answered
     * the problem completely. */
    uint64_t now = pit_now_us();
    if (now - last_device_touch_us >= 200) {
        last_device_touch_us = now;
        (void)reg_read(E1000_STATUS);
    }

    for (;;) {
        rx_desc_t *d = &rx_ring[rx_tail];
        if (!(d->status & RXD_STAT_DD)) break;

        if (d->status & RXD_STAT_EOP) {
            uint8_t *buf = rx_bufs + rx_tail * BUF_SIZE;
            net_rx(buf, d->length);
        }
        d->status = 0;
        // Return the descriptor to the card and advance.
        reg_write(E1000_RDT, rx_tail);
        rx_tail = (rx_tail + 1) % NUM_RX;
    }
}

static int e1000_link(void) {
    if (!mmio) return 0;
    return (reg_read(E1000_STATUS) & 0x02) ? 1 : 0;   // STATUS.LU
}

const nic_driver_t e1000_driver = {
    .name = "e1000",
    .init = e1000_init,
    .mac  = e1000_mac,
    .send = e1000_send,
    .poll = e1000_poll,
    .link = e1000_link,
};
