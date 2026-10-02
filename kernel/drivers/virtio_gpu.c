// The virtio GPU (see virtio_gpu.h): the modern virtio PCI transport, one
// control queue polled for completion, and the handful of 2D commands that
// put a picture in memory on the screen.

#include "virtio_gpu.h"
#include "pci.h"
#include "../mm/pmm.h"
#include "../mm/paging.h"
#include "../kernel/kio.h"
#include "../arch/x86_64/pit.h"

#define VIRTIO_VENDOR   0x1AF4
#define VGPU_DEVICE     0x1050       // virtio 1.x, device type 16

// --- the transport -----------------------------------------------------------------
//
// A modern virtio device describes where its register blocks are with
// vendor-specific PCI capabilities: which BAR, at what offset.
#define CAP_COMMON  1
#define CAP_NOTIFY  2
#define CAP_ISR     3
#define CAP_DEVICE  4

// The common configuration block.
#define CC_DFSELECT   0x00
#define CC_DF         0x04
#define CC_GFSELECT   0x08
#define CC_GF         0x0C
#define CC_NUMQ       0x12
#define CC_STATUS     0x14
#define CC_QSELECT    0x16
#define CC_QSIZE      0x18
#define CC_QENABLE    0x1C
#define CC_QNOTIFYOFF 0x1E
#define CC_QDESC      0x20
#define CC_QDRIVER    0x28
#define CC_QDEVICE    0x30

#define ST_ACK        1
#define ST_DRIVER     2
#define ST_DRIVER_OK  4
#define ST_FEATURES   8
#define ST_FAILED     128

static volatile uint8_t *common, *notify_base;
static uint32_t notify_mult;
static uint16_t notify_off;

static inline void w8(volatile uint8_t *b, int o, uint8_t v)   { *(volatile uint8_t *)(b + o) = v; }
static inline void w16(volatile uint8_t *b, int o, uint16_t v) { *(volatile uint16_t *)(b + o) = v; }
static inline void w32(volatile uint8_t *b, int o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static inline uint8_t  r8(volatile uint8_t *b, int o)  { return *(volatile uint8_t *)(b + o); }
static inline uint16_t r16(volatile uint8_t *b, int o) { return *(volatile uint16_t *)(b + o); }
static inline uint32_t r32(volatile uint8_t *b, int o) { return *(volatile uint32_t *)(b + o); }
static inline void w64(volatile uint8_t *b, int o, uint64_t v) {
    w32(b, o, (uint32_t)v);
    w32(b, o + 4, (uint32_t)(v >> 32));
}

// --- the control queue ------------------------------------------------------------
#define QMAX 64
struct desc  { uint64_t addr; uint32_t len; uint16_t flags, next; };
struct avail { uint16_t flags, idx, ring[QMAX], used_event; };
struct uelem { uint32_t id, len; };
struct used  { uint16_t flags, idx; struct uelem ring[QMAX]; uint16_t avail_event; };
#define D_NEXT  1
#define D_WRITE 2

static struct desc  *dt;
static struct avail *av;
static struct used  *us;
static uint16_t qsz, used_seen;

// Requests and their answers: one slot each per command in flight, in memory
// the device can reach (the kernel's memory is mapped at its physical address).
#define SLOT 64
static uint8_t req[QMAX / 2][SLOT] __attribute__((aligned(64)));
static uint8_t rsp[QMAX / 2][SLOT] __attribute__((aligned(64)));
static uint8_t big_rsp[512] __attribute__((aligned(64)));   // the display info

// --- the commands --------------------------------------------------------------------
#define CMD_GET_DISPLAY_INFO  0x0100
#define CMD_RESOURCE_CREATE_2D 0x0101
#define CMD_SET_SCANOUT       0x0103
#define CMD_RESOURCE_FLUSH    0x0104
#define CMD_TRANSFER_TO_HOST  0x0105
#define CMD_ATTACH_BACKING    0x0106
#define RESP_OK_NODATA        0x1100
#define RESP_OK_DISPLAY_INFO  0x1101
#define FMT_B8G8R8X8          2      // bytes B, G, R, X: our 0x00RRGGBB words

struct hdr { uint32_t type, flags; uint64_t fence; uint32_t ctx; uint8_t ring, pad[3]; };
struct rect { uint32_t x, y, w, h; };

static int found, active;
static uint32_t res_id = 1, res_w, res_h, scr_w, scr_h, res_pitch;

// Put commands 0..n-1 (request i in req[i], `rlen[i]` bytes; answer `alen[i]`
// bytes into rsp[i], or into `big` for one large one) on the queue, ring the
// bell once, and wait for all of them. 1 if every answer was a success.
static int submit(int n, const uint32_t *rlen, const uint32_t *alen, void *big) {
    if (n <= 0) return 1;
    uint16_t at = av->idx;
    for (int i = 0; i < n; i++) {
        int d = (i * 2) % qsz;
        dt[d].addr = (uint64_t)(uintptr_t)req[i];
        dt[d].len = rlen[i];
        dt[d].flags = D_NEXT;
        dt[d].next = (uint16_t)(d + 1);
        dt[d + 1].addr = (uint64_t)(uintptr_t)(big && i == 0 ? big : rsp[i]);
        dt[d + 1].len = alen[i];
        dt[d + 1].flags = D_WRITE;
        dt[d + 1].next = 0;
        av->ring[(uint16_t)(at + i) % qsz] = (uint16_t)d;
    }
    __asm__ volatile ("mfence" ::: "memory");
    av->idx = (uint16_t)(at + n);
    __asm__ volatile ("mfence" ::: "memory");
    w16(notify_base, notify_off * notify_mult, 0);          // queue 0

    // Wait for the device -- briefly: it answers in well under a millisecond.
    uint64_t give_up = pit_get_ticks() + 50;                   // half a second
    uint16_t want = (uint16_t)(used_seen + n);
    while (*(volatile uint16_t *)&us->idx != want) {
        if (pit_get_ticks() > give_up) {
            kprintf("vgpu: the device stopped answering\n");
            active = 0;
            return 0;
        }
        __asm__ volatile ("pause");
    }
    used_seen = want;
    int ok = 1;
    for (int i = 0; i < n; i++) {
        const struct hdr *h = (const struct hdr *)(big && i == 0 ? big : rsp[i]);
        if (h->type != RESP_OK_NODATA && h->type != RESP_OK_DISPLAY_INFO) ok = 0;
    }
    return ok;
}

static void *zero(void *p, int n) {
    for (int i = 0; i < n; i++) ((uint8_t *)p)[i] = 0;
    return p;
}

int vgpu_init(void) {
    pci_device_t dev;
    if (!pci_find_device(VIRTIO_VENDOR, VGPU_DEVICE, &dev)) return 0;

    // Walk the capability list for the register blocks.
    uint32_t sc = pci_config_read32(dev.bus, dev.slot, dev.func, 0x04);
    if (!((sc >> 16) & 0x10)) return 0;                      // no capability list
    uint8_t cp = (uint8_t)(pci_config_read32(dev.bus, dev.slot, dev.func, 0x34) & 0xFC);
    volatile uint8_t *blocks[5] = { 0 };
    for (int guard = 0; cp && guard < 48; guard++) {
        uint32_t c0 = pci_config_read32(dev.bus, dev.slot, dev.func, cp);
        uint8_t id = c0 & 0xFF, next = (c0 >> 8) & 0xFC, type = (c0 >> 24) & 0xFF;
        if (id == 0x09 && type >= 1 && type <= 4) {
            uint32_t c1 = pci_config_read32(dev.bus, dev.slot, dev.func, cp + 4);
            uint32_t off = pci_config_read32(dev.bus, dev.slot, dev.func, cp + 8);
            uint32_t len = pci_config_read32(dev.bus, dev.slot, dev.func, cp + 12);
            int bar = c1 & 0xFF;
            if (bar < 6) {
                uint64_t base = dev.bar[bar] & ~0xFULL;
                if ((dev.bar[bar] & 0x6) == 0x4 && bar < 5) base |= (uint64_t)dev.bar[bar + 1] << 32;
                if (base) {
                    paging_map_mmio(base + off, len < 0x1000 ? 0x1000 : len);
                    blocks[type] = (volatile uint8_t *)(uintptr_t)(base + off);
                    if (type == CAP_NOTIFY)
                        notify_mult = pci_config_read32(dev.bus, dev.slot, dev.func, cp + 16);
                }
            }
        }
        cp = next;
    }
    common = blocks[CAP_COMMON];
    notify_base = blocks[CAP_NOTIFY];
    if (!common || !notify_base) { kprintf("vgpu: registers not found\n"); return 0; }
    pci_enable_bus_master(&dev);

    // Reset, say hello, agree on features: version 1 and nothing else.
    w8(common, CC_STATUS, 0);
    for (int i = 0; i < 1000 && r8(common, CC_STATUS); i++) __asm__ volatile ("pause");
    w8(common, CC_STATUS, ST_ACK);
    w8(common, CC_STATUS, ST_ACK | ST_DRIVER);
    w32(common, CC_GFSELECT, 0); w32(common, CC_GF, 0);
    w32(common, CC_GFSELECT, 1); w32(common, CC_GF, 1);      // VIRTIO_F_VERSION_1 (bit 32)
    w8(common, CC_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES);
    if (!(r8(common, CC_STATUS) & ST_FEATURES)) {
        kprintf("vgpu: features refused\n");
        w8(common, CC_STATUS, ST_FAILED);
        return 0;
    }

    // The control queue, number 0.
    w16(common, CC_QSELECT, 0);
    qsz = r16(common, CC_QSIZE);
    if (!qsz) { kprintf("vgpu: no control queue\n"); return 0; }
    if (qsz > QMAX) qsz = QMAX;
    w16(common, CC_QSIZE, qsz);
    dt = (struct desc *)(uintptr_t)pmm_alloc_contig(1);
    av = (struct avail *)(uintptr_t)pmm_alloc_contig(1);
    us = (struct used *)(uintptr_t)pmm_alloc_contig(1);
    if (!dt || !av || !us) { kprintf("vgpu: out of memory\n"); return 0; }
    zero(dt, 4096); zero(av, 4096); zero(us, 4096);
    w64(common, CC_QDESC, (uint64_t)(uintptr_t)dt);
    w64(common, CC_QDRIVER, (uint64_t)(uintptr_t)av);
    w64(common, CC_QDEVICE, (uint64_t)(uintptr_t)us);
    notify_off = r16(common, CC_QNOTIFYOFF);
    w16(common, CC_QENABLE, 1);
    w8(common, CC_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES | ST_DRIVER_OK);

    // What the display looks like, for the log.
    struct hdr *h = (struct hdr *)zero(req[0], SLOT);
    h->type = CMD_GET_DISPLAY_INFO;
    uint32_t rl = sizeof(struct hdr), al = sizeof big_rsp;
    if (submit(1, &rl, &al, big_rsp)) {
        const uint32_t *m = (const uint32_t *)(big_rsp + sizeof(struct hdr));
        kprintf("vgpu: ready, display 0 %ux%u%s\n", m[2], m[3], m[4] ? "" : " (off)");
    }
    found = 1;
    return 1;
}

int vgpu_take_screen(void *px, uint32_t pitch, uint32_t w, uint32_t h) {
    if (!found || !px || pitch % 4) return 0;
    // The resource is as wide as a row of memory, so its rows and the back
    // buffer's line up; the screen shows the first w columns of it.
    res_w = pitch / 4; res_h = h; res_pitch = pitch;
    scr_w = w; scr_h = h;

    uint32_t rl[3], al[3];
    uint32_t *c = (uint32_t *)zero(req[0], SLOT);
    ((struct hdr *)c)->type = CMD_RESOURCE_CREATE_2D;
    c[6] = res_id; c[7] = FMT_B8G8R8X8; c[8] = res_w; c[9] = res_h;
    rl[0] = 40; al[0] = sizeof(struct hdr);

    c = (uint32_t *)zero(req[1], SLOT);
    ((struct hdr *)c)->type = CMD_ATTACH_BACKING;
    c[6] = res_id; c[7] = 1;                                  // one stretch of memory
    *(uint64_t *)&c[8] = (uint64_t)(uintptr_t)px;
    c[10] = pitch * h; c[11] = 0;
    rl[1] = 48; al[1] = sizeof(struct hdr);

    c = (uint32_t *)zero(req[2], SLOT);
    ((struct hdr *)c)->type = CMD_SET_SCANOUT;
    c[6] = 0; c[7] = 0; c[8] = scr_w; c[9] = scr_h;          // rect
    c[10] = 0; c[11] = res_id;                                // scanout 0, the resource
    rl[2] = 48; al[2] = sizeof(struct hdr);

    if (!submit(3, rl, al, 0)) { kprintf("vgpu: could not set the screen up\n"); return 0; }
    active = 1;
    kprintf("vgpu: the screen is a %ux%u picture in memory now\n", scr_w, scr_h);
    return 1;
}

int vgpu_active(void) { return active; }

void vgpu_update(const uint32_t (*rects)[4], int n) {
    if (!active || n <= 0) return;
    // A copy for each rectangle, then one flush over all of them.
    uint32_t rl[QMAX / 2], al[QMAX / 2];
    uint32_t fx0 = 0xFFFFFFFF, fy0 = 0xFFFFFFFF, fx1 = 0, fy1 = 0;
    int k = 0;
    for (int i = 0; i < n && k < QMAX / 2 - 1; i++) {
        uint32_t x0 = rects[i][0], y0 = rects[i][1], x1 = rects[i][2], y1 = rects[i][3];
        if (x1 > scr_w) x1 = scr_w;
        if (y1 > scr_h) y1 = scr_h;
        if (x1 <= x0 || y1 <= y0) continue;
        uint32_t *c = (uint32_t *)zero(req[k], SLOT);
        ((struct hdr *)c)->type = CMD_TRANSFER_TO_HOST;
        c[6] = x0; c[7] = y0; c[8] = x1 - x0; c[9] = y1 - y0;
        *(uint64_t *)&c[10] = (uint64_t)y0 * res_pitch + (uint64_t)x0 * 4;
        c[12] = res_id;
        rl[k] = 56; al[k] = sizeof(struct hdr);
        k++;
        if (x0 < fx0) fx0 = x0;
        if (y0 < fy0) fy0 = y0;
        if (x1 > fx1) fx1 = x1;
        if (y1 > fy1) fy1 = y1;
    }
    if (!k) return;
    uint32_t *c = (uint32_t *)zero(req[k], SLOT);
    ((struct hdr *)c)->type = CMD_RESOURCE_FLUSH;
    c[6] = fx0; c[7] = fy0; c[8] = fx1 - fx0; c[9] = fy1 - fy0;
    c[10] = res_id;
    rl[k] = 48; al[k] = sizeof(struct hdr);
    submit(k + 1, rl, al, 0);
}
