#include "rtl8125.h"
#include "net.h"
#include "nic.h"
#include "../drivers/pci.h"
#include "../mm/pmm.h"
#include "../mm/paging.h"
#include "../kernel/kio.h"
#include <stddef.h>

// --- register offsets (MMIO) ----------------------------------------------
#define R_IDR0    0x00   // MAC address, 6 bytes
#define R_MAR0    0x08   // multicast, 8 bytes
#define R_TNPDS   0x20   // TX normal-priority descriptor start (qword, 256-aln)
#define R_CR      0x37   // command (byte)
#define R_TPPOLL  0x38   // transmit poll (byte)
#define R_IMR     0x3C   // interrupt mask (word)
#define R_ISR     0x3E   // interrupt status (word)
#define R_TCR     0x40   // TX config (dword)
#define R_RCR     0x44   // RX config (dword)
#define R_9346CR  0x50   // 93C46 (EEPROM) command / config lock (byte)
#define R_PHYSTAT 0x6C   // PHY status (byte)
#define R_RMS     0xDA   // RX max packet size (word)
#define R_CPCR    0xE0   // C+ command (word)
#define R_RDSAR   0xE4   // RX descriptor start (qword, 256-aln)
#define R_MTPS    0xEC   // max TX packet size (byte)

// CR bits
#define CR_RST    0x10
#define CR_RE     0x08
#define CR_TE     0x04
// TPPoll
#define TPPOLL_NPQ 0x40
// 9346CR
#define CFG_UNLOCK 0xC0
#define CFG_LOCK   0x00

// descriptor opts1 bits
#define D_OWN 0x80000000u
#define D_EOR 0x40000000u
#define D_FS  0x20000000u
#define D_LS  0x10000000u
#define D_RES 0x00200000u   // RX: receive error
#define D_LEN 0x00003FFFu   // buffer size / received length field

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

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t opts1;
    uint32_t opts2;
    uint64_t addr;
} rtl_desc_t;

static volatile uint8_t *mmio = NULL;
static uint8_t mac[6];
static rtl_desc_t *rx_ring = NULL, *tx_ring = NULL;
static uint8_t *rx_bufs = NULL, *tx_bufs = NULL;
static int rx_cur = 0, tx_cur = 0;

static inline void  w8(uint32_t r, uint8_t v)  { *(volatile uint8_t  *)(mmio + r) = v; }
static inline void  w16(uint32_t r, uint16_t v){ *(volatile uint16_t *)(mmio + r) = v; }
static inline void  w32(uint32_t r, uint32_t v){ *(volatile uint32_t *)(mmio + r) = v; }
static inline uint8_t  r8(uint32_t r)  { return *(volatile uint8_t  *)(mmio + r); }
static inline uint32_t r32(uint32_t r) { return *(volatile uint32_t *)(mmio + r); }

static void *alloc_contig(uint32_t bytes) {
    uint32_t pages = (bytes + 4095) / 4096;
    uint64_t start = pmm_alloc_frame();
    if (!start) return NULL;
    uint64_t expect = start + PAGE_SIZE;
    for (uint32_t i = 1; i < pages; i++) {
        uint64_t f = pmm_alloc_frame();
        if (f != expect) return NULL;
        expect += PAGE_SIZE;
    }
    uint8_t *p = (uint8_t *)(uintptr_t)start;
    for (uint32_t i = 0; i < pages * 4096; i++) p[i] = 0;
    return p;
}

// Pick the first memory BAR (RTL8125 keeps its MMIO on BAR2, but scan so a
// board that lays the BARs out differently still works). Returns 0 if none.
static uint64_t find_mmio_bar(const pci_device_t *dev) {
    for (int i = 0; i < 6; i++) {
        uint32_t b = dev->bar[i];
        if (b & 0x1) continue;                 // I/O space, skip
        uint64_t base = b & ~0xFULL;
        if ((b & 0x6) == 0x4) {                // 64-bit: high half in next BAR
            if (i + 1 < 6) base |= ((uint64_t)dev->bar[i + 1] << 32);
            i++;
        }
        if (base) return base;
    }
    return 0;
}

static void setup_rings(void) {
    rx_ring = (rtl_desc_t *)alloc_contig(NUM_RX * sizeof(rtl_desc_t));
    tx_ring = (rtl_desc_t *)alloc_contig(NUM_TX * sizeof(rtl_desc_t));
    rx_bufs = (uint8_t *)alloc_contig(NUM_RX * BUF_SIZE);
    tx_bufs = (uint8_t *)alloc_contig(NUM_TX * BUF_SIZE);

    for (int i = 0; i < NUM_RX; i++) {
        rx_ring[i].addr = (uint64_t)(uintptr_t)(rx_bufs + i * BUF_SIZE);
        rx_ring[i].opts2 = 0;
        rx_ring[i].opts1 = D_OWN | ((i == NUM_RX - 1) ? D_EOR : 0) | BUF_SIZE;
    }
    for (int i = 0; i < NUM_TX; i++) {
        tx_ring[i].addr = (uint64_t)(uintptr_t)(tx_bufs + i * BUF_SIZE);
        tx_ring[i].opts1 = (i == NUM_TX - 1) ? D_EOR : 0;   // not owned = free
        tx_ring[i].opts2 = 0;
    }
    rx_cur = tx_cur = 0;
}

int rtl8125_init(void) {
    pci_device_t dev;
    if (!pci_find_device(0x10EC, 0x8125, &dev)) return 0;

    uint64_t bar = find_mmio_bar(&dev);
    if (!bar) return 0;
    paging_map_mmio(bar, 0x10000);
    mmio = (volatile uint8_t *)(uintptr_t)bar;

    pci_enable_bus_master(&dev);

    // Soft reset, bounded so a wrong BAR can't hang the machine.
    w8(R_CR, CR_RST);
    int spins = 0;
    while ((r8(R_CR) & CR_RST) && ++spins < 1000000) { __asm__ volatile ("pause"); }
    if (r8(R_CR) & CR_RST) { mmio = NULL; return 0; }   // never came back

    for (int i = 0; i < 6; i++) mac[i] = r8(R_IDR0 + i);
    // A NIC with an all-zero / all-ones MAC did not really initialise.
    int allz = 1, allf = 1;
    for (int i = 0; i < 6; i++) { if (mac[i]) allz = 0; if (mac[i] != 0xFF) allf = 0; }
    if (allz || allf) { mmio = NULL; return 0; }

    setup_rings();

    w8(R_9346CR, CFG_UNLOCK);                 // unlock config registers
    w16(R_CPCR, 0x0000);                      // plain C+ mode, no offloads
    w16(R_RMS, 0x1FFF);                       // accept full-size frames
    w8(R_MTPS, 0x3B);

    uint64_t rp = (uint64_t)(uintptr_t)rx_ring;
    uint64_t tp = (uint64_t)(uintptr_t)tx_ring;
    w32(R_RDSAR, (uint32_t)rp); w32(R_RDSAR + 4, (uint32_t)(rp >> 32));
    w32(R_TNPDS, (uint32_t)tp); w32(R_TNPDS + 4, (uint32_t)(tp >> 32));

    w32(R_TCR, 0x03000700);                   // IFG normal, unlimited DMA burst
    // RX: accept broadcast + multicast + our own MAC, unlimited burst/threshold.
    w32(R_RCR, 0x0000E70E);

    w8(R_CR, CR_TE | CR_RE);                  // enable TX + RX
    w8(R_9346CR, CFG_LOCK);                   // relock config

    w16(R_IMR, 0x0000);                       // we poll -- mask all interrupts
    w16(R_ISR, 0xFFFF);                       // clear any pending status

    uint8_t ps = r8(R_PHYSTAT);
    kprintf("rtl8125: up, mac %x:%x:%x:%x:%x:%x phystat=0x%x link=%s\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            ps, (ps & 0x02) ? "up" : "down");
    return 1;
}

// PHYStatus (0x6C): bit1 = LinkStatus. On RTL8125 the speed bits also include
// 2500M (bit6); any of the speed/link indications means the PHY has a link.
static int rtl8125_link(void) {
    if (!mmio) return 0;
    return (r8(R_PHYSTAT) & 0x02) ? 1 : 0;
}

const uint8_t *rtl8125_mac(void) { return mac; }

int rtl8125_send(const void *frame, uint16_t len) {
    if (!mmio) return -1;
    if (len > BUF_SIZE) len = BUF_SIZE;

    rtl_desc_t *d = &tx_ring[tx_cur];
    int spins = 0;
    while ((d->opts1 & D_OWN) && ++spins < 100000000) { __asm__ volatile ("pause"); }
    if (d->opts1 & D_OWN) return -1;

    uint8_t *buf = tx_bufs + tx_cur * BUF_SIZE;
    const uint8_t *src = (const uint8_t *)frame;
    for (uint16_t i = 0; i < len; i++) buf[i] = src[i];

    uint32_t eor = (tx_cur == NUM_TX - 1) ? D_EOR : 0;
    d->addr = (uint64_t)(uintptr_t)buf;
    d->opts2 = 0;
    __asm__ volatile ("" ::: "memory");
    d->opts1 = D_OWN | D_FS | D_LS | eor | (len & 0xFFFF);
    __asm__ volatile ("" ::: "memory");

    w8(R_TPPOLL, TPPOLL_NPQ);                  // kick the transmitter
    tx_cur = (tx_cur + 1) % NUM_TX;
    return 0;
}

void rtl8125_poll(void) {
    if (!mmio) return;
    for (int guard = 0; guard < NUM_RX; guard++) {
        rtl_desc_t *d = &rx_ring[rx_cur];
        if (d->opts1 & D_OWN) break;           // still owned by NIC -> empty

        uint32_t o1 = d->opts1;
        if ((o1 & (D_FS | D_LS)) == (D_FS | D_LS) && !(o1 & D_RES)) {
            int len = (int)(o1 & D_LEN) - 4;   // strip the 4-byte Ethernet CRC
            if (len > 0) net_rx(rx_bufs + rx_cur * BUF_SIZE, (uint16_t)len);
        }
        uint32_t eor = (rx_cur == NUM_RX - 1) ? D_EOR : 0;
        __asm__ volatile ("" ::: "memory");
        d->opts1 = D_OWN | eor | BUF_SIZE;     // hand the descriptor back
        rx_cur = (rx_cur + 1) % NUM_RX;
    }
}

const nic_driver_t rtl8125_driver = {
    .name = "rtl8125",
    .init = rtl8125_init,
    .mac  = rtl8125_mac,
    .send = rtl8125_send,
    .poll = rtl8125_poll,
    .link = rtl8125_link,
};
