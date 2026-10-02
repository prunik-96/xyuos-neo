// xHCI: the USB host controllers, the hubs on them, and the devices behind
// those. See xhci.h.
//
// The controller does DMA to physical addresses. The kernel identity-maps the
// low 4 GiB and every page handed to a controller here comes from below that,
// so for any buffer here physical == virtual. The registers are mapped by
// paging_map_mmio (on real hardware they sit far above 4 GiB).
//
// Everything a controller says arrives on one event ring per controller, and
// whoever drains it deals with whatever it finds (other_event): the timer's
// xhci_poll, a transfer waiting for its own completion, the network's receive.
// Devices are set up and let go of only in usb_service, from the idle loop --
// it waits on the devices, which an interrupt must never do.

#include "xhci.h"
#include "pci.h"
#include "keyboard.h"
#include "framebuffer.h"
#include "../kernel/kio.h"
#include "mouse.h"
#include "hid.h"
#include "../arch/x86_64/pit.h"
#include "../mm/paging.h"
#include "../mm/pmm.h"
#include <stdint.h>
#include <stddef.h>

#define XHCI_DEBUG 1
#if XHCI_DEBUG
#define DBG(...) kprintf(__VA_ARGS__)
#else
#define DBG(...)
#endif

// --- registers ---------------------------------------------------------------

static inline uint32_t r32(volatile uint8_t *p, uint32_t off) {
    return *(volatile uint32_t *)(p + off);
}
static inline void w32(volatile uint8_t *p, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(p + off) = v;
}
static inline void w64(volatile uint8_t *p, uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(p + off) = (uint32_t)v;
    *(volatile uint32_t *)(p + off + 4) = (uint32_t)(v >> 32);
}

// Capability register offsets
#define CAP_CAPLENGTH 0x00
#define CAP_HCSPARAMS1 0x04
#define CAP_HCSPARAMS2 0x08
#define CAP_HCCPARAMS1 0x10
#define CAP_DBOFF 0x14
#define CAP_RTSOFF 0x18

// Operational register offsets
#define OP_USBCMD 0x00
#define OP_USBSTS 0x04
#define OP_CRCR 0x18
#define OP_DCBAAP 0x30
#define OP_CONFIG 0x38
#define OP_PORTSC(n) (0x400 + (n) * 0x10)   // n is 0-based here

#define USBCMD_RUN  (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBSTS_HCH  (1u << 0)
#define USBSTS_CNR  (1u << 11)

#define PORTSC_CCS (1u << 0)
#define PORTSC_PED (1u << 1)     // RW1C: writing 1 DISABLES the port
#define PORTSC_PR  (1u << 4)
#define PORTSC_PP  (1u << 9)
#define PORTSC_CSC (1u << 17)
#define PORTSC_PEC (1u << 18)
#define PORTSC_OCC (1u << 20)
#define PORTSC_PRC (1u << 21)
#define PORTSC_PLC (1u << 22)
#define PORTSC_CEC (1u << 23)
// All the RW1-to-clear status-change bits (CSC..CEC, bits 17..23). Writing a 1
// to any of them acks/clears it. PED (bit 1) is also RW1C. So a read-modify-
// write of PORTSC must mask ALL of these off, or it silently clears them.
#define PORTSC_RW1C (PORTSC_PED | 0x00FE0000u)

// Runtime: interrupter 0
#define RT_ERSTSZ 0x28
#define RT_ERSTBA 0x30
#define RT_ERDP 0x38

// --- TRBs and rings ----------------------------------------------------------

#define TRB_NORMAL 1
#define TRB_SETUP 2
#define TRB_DATA 3
#define TRB_STATUS 4
#define TRB_LINK 6
#define TRB_ENABLE_SLOT 9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEVICE 11
#define TRB_CONFIGURE_ENDPOINT 12
#define TRB_EVALUATE_CONTEXT 13
#define TRB_RESET_ENDPOINT 14
#define TRB_SET_TR_DEQUEUE 16
#define TRB_TRANSFER_EVENT 32
#define TRB_CMD_COMPLETION 33
#define TRB_PORT_STATUS_CHANGE 34

#define TRB_TYPE(t) ((uint32_t)(t) << 10)
#define TRB_CYCLE (1u << 0)
#define TRB_ISP (1u << 2)     // interrupt on short packet
#define TRB_CHAIN (1u << 4)   // the TD goes on in the next TRB
#define TRB_IOC (1u << 5)     // interrupt on completion
#define TRB_IDT (1u << 6)     // immediate data
#define TRB_TYPE_OF(ctrl) (((ctrl) >> 10) & 0x3F)
#define TRB_CC(status) (((status) >> 24) & 0xFF)
#define CC_SUCCESS 1
#define CC_STALL 6
#define CC_SHORT_PKT 13

#define RING_SIZE 64          // TRBs in a command or transfer ring
#define EVT_SIZE 256          // TRBs in an event ring: one page

// Each TRB is 4 dwords. Rings never cross a 64 KiB boundary (an xHCI
// requirement): each lives inside one page.
struct ring { uint32_t *trb; int enq; int cycle; };

// Endpoint types, as an endpoint context names them.
#define EP_BULK_OUT 2
#define EP_CONTROL  4
#define EP_BULK_IN  6
#define EP_INTR_IN  7

// --- pages for the controllers -----------------------------------------------

// One zeroed page below 4 GiB, or NULL.
static void *page(void) {
    uint64_t f = pmm_alloc_frame();
    if (!f) return NULL;
    if (f + PAGE_SIZE > 0x100000000ULL) { pmm_free_frame(f); return NULL; }
    uint64_t *p = (uint64_t *)(uintptr_t)f;
    for (int i = 0; i < 512; i++) p[i] = 0;
    return p;
}
static void unpage(void *p) { if (p) pmm_free_frame((uint64_t)(uintptr_t)p); }

// The time, for waits that must also run with interrupts off (the timer's
// tick count stands still then; the timestamp counter does not).
static void delay_ms(uint32_t ms) {
    uint64_t until = pit_now_us() + (uint64_t)ms * 1000;
    while (pit_now_us() < until) __asm__ volatile ("pause");
}

static inline uint64_t cpu_flags(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0" : "=r"(f) :: "memory");
    return f;
}

// --- controllers -------------------------------------------------------------

#define MAX_HC 4
struct hc {
    int up;
    uint8_t bus, dev, fn;
    volatile uint8_t *mmio, *op, *rt;
    volatile uint32_t *db;
    uint32_t ctx_size, max_ports, max_slots;
    uint64_t *dcbaa;
    uint32_t *evt;              // event ring
    int evt_deq, evt_cycle;
    struct ring cmd;
    uint32_t *erst;
    uint8_t *in_ctx;            // input context, built afresh for each command
    volatile uint64_t pending;  // root ports (bit p for port p) with news
    uint32_t conn_mask;         // ports with a device at start-up (the report)
    const char *stage;          // how far it got (the report)
};
static struct hc hcs[MAX_HC];
static int nhc;

__attribute__((aligned(4096))) static uint32_t hc_cmd[MAX_HC][1024];   // a page each
__attribute__((aligned(4096))) static uint32_t hc_evt[MAX_HC][1024];   // a page each
__attribute__((aligned(4096))) static uint64_t hc_dcbaa[MAX_HC][512];
__attribute__((aligned(4096))) static uint8_t  hc_in[MAX_HC][4096];
__attribute__((aligned(64)))   static uint32_t hc_erst[MAX_HC][16];   // 64 bytes each: an ERST is 64-aligned

// Control transfers go one at a time, so they share one buffer for what comes
// in and one for what goes out. 1 KiB: a HID report descriptor and a big
// configuration both fit. Aligned so a transfer never crosses a 64 KiB line.
__attribute__((aligned(1024))) static uint8_t xfer_buf[1024];
__attribute__((aligned(512)))  static uint8_t xfer_out[512];

static void ring_init(struct ring *r, uint32_t *trb) {
    r->trb = trb;
    r->enq = 0;
    r->cycle = 1;
    for (int i = 0; i < RING_SIZE * 4; i++) trb[i] = 0;
    // Link TRB in the last slot, back to the start, with Toggle Cycle.
    uint32_t *link = &trb[(RING_SIZE - 1) * 4];
    uint64_t base = (uint64_t)(uintptr_t)trb;
    link[0] = (uint32_t)base;
    link[1] = (uint32_t)(base >> 32);
    link[2] = 0;
    link[3] = TRB_TYPE(TRB_LINK) | (1u << 1) /*Toggle Cycle*/;
}

// Enqueue a TRB (4 dwords). control's cycle bit is set from the ring's state.
static uint64_t ring_push(struct ring *r, uint32_t d0, uint32_t d1,
                          uint32_t d2, uint32_t control) {
    uint32_t *t = &r->trb[r->enq * 4];
    t[0] = d0; t[1] = d1; t[2] = d2;
    control = (control & ~TRB_CYCLE) | (r->cycle ? TRB_CYCLE : 0);
    __asm__ volatile ("" ::: "memory");
    t[3] = control;
    uint64_t addr = (uint64_t)(uintptr_t)t;

    r->enq++;
    if (r->enq == RING_SIZE - 1) {   // reached the Link TRB
        // set the Link TRB's cycle to the producer cycle, then wrap+toggle.
        // A TD that runs on past the end of the ring runs on through the
        // Link TRB too, and the controller must be told so: its Chain bit
        // is the Chain bit of the TRB just written.
        uint32_t *link = &r->trb[(RING_SIZE - 1) * 4];
        link[3] = (link[3] & ~(TRB_CYCLE | TRB_CHAIN)) | (control & TRB_CHAIN) |
                  (r->cycle ? TRB_CYCLE : 0);
        r->enq = 0;
        r->cycle ^= 1;
    }
    return addr;
}

static void ring_doorbell(struct hc *h, int slot, uint32_t target) {
    h->db[slot] = target;
    (void)r32(h->op, OP_USBSTS);   // posting read to flush
}

// --- devices -----------------------------------------------------------------

#define MAX_DEV 32
struct udev {
    int used;
    struct hc *hc;
    int slot;                   // 0 once let go of by the controller
    int parent;                 // the hub it hangs off (index), -1 on a root port
    int port;                   // its port there: 1-based on a hub, 0-based on the root
    int root;                   // the controller port its chain starts at, 0-based
    uint32_t route;             // the hub ports down to it, 4 bits a tier
    int depth;                  // hubs between it and the controller
    int speed;                  // 1 full, 2 low, 3 high, 4 super
    int tt_slot, tt_port;       // a slow device behind a high-speed hub: whose translator
    int entries;                // highest endpoint the slot context counts
    uint8_t *ctx;               // its device context, the controller's to write
    uint32_t *ep0_trb;
    struct ring ep0;
    uint16_t lang, vid, pid;
    int kind;                   // USB_KIND_*
    int generic_hid;            // passed over: an HID that is no keyboard or mouse
    char name[48];
    // A hub.
    int nports, usb3hub, hub_dci;
    uint32_t hub_len;
    uint32_t *hub_trb;          // its status-change ring, and the buffer after it
    struct ring hub_intr;
    volatile uint32_t hub_pending;  // ports (bit n for port n) with news
    int hub_stalled;
};
static struct udev devs[MAX_DEV];

static volatile int busy;       // devices being set up: the timer keeps off the rings
static volatile int work;       // a port or a hub has news for usb_service
static int booting;             // xhci_init running: no news for the desktop

// --- news for the desktop ------------------------------------------------------

#define NEWS_MAX 8
static struct usb_news news[NEWS_MAX];
static int news_head, news_n;

static void scopy(char *d, const char *s, int max) {
    int i = 0;
    for (; s && s[i] && i < max - 1; i++) d[i] = s[i];
    d[i] = 0;
}

static void news_push(int attached, int kind, const char *name, uint32_t mib) {
    if (booting) return;
    if (news_n == NEWS_MAX) { news_head = (news_head + 1) % NEWS_MAX; news_n--; }
    struct usb_news *n = &news[(news_head + news_n) % NEWS_MAX];
    n->attached = attached;
    n->kind = kind;
    n->mib = mib;
    scopy(n->name, name, sizeof n->name);
    news_n++;
}

int xhci_news(struct usb_news *out) {
    if (!news_n) return 0;
    *out = news[news_head];
    news_head = (news_head + 1) % NEWS_MAX;
    news_n--;
    return 1;
}

// --- keyboards, mice, disks, the network --------------------------------------

// Where a HID report lands: a whole packet of the endpoint's size. A device
// that sends more than the transfer asked for is a babble error to the
// controller, and the endpoint stops for good. A wireless receiver's keyboard
// interface does that with its media-key reports.
#define HID_BUF 64

// One live keyboard. Its page holds its interrupt ring, and its report buffer
// at +2048. Its own key memory, so two keyboards don't cancel each other's
// held keys.
#define MAX_KBD 8
struct kbdev {
    int used, dev, dci;
    uint32_t *trb;
    uint8_t *buf;
    struct ring intr;
    uint32_t len;
    uint8_t prev[6];
    uint8_t prevmod;
    int stalled;                // its endpoint failed: usb_service brings it back
};
static struct kbdev kbds[MAX_KBD];
static int nkbds;

#define MAX_MOUSE 4
struct mousedev {
    int used, dev, dci;
    uint32_t *trb;
    uint8_t *buf;
    struct ring intr;
    uint32_t len;
    hid_mouse_fmt fmt;          // where its reports keep what (see hid.h)
    int stalled;
};
static struct mousedev mice[MAX_MOUSE];
static int nmice;

// USB Mass Storage: Bulk-Only-Transport disks. A disk keeps its index while
// it is plugged in; `id` tells one plugging-in from the next.
#define MAX_MSC 8
__attribute__((aligned(64))) static uint8_t msc_cbw[31];   // shared (one xfer at a time)
__attribute__((aligned(64))) static uint8_t msc_csw[13];
struct msc_dev {
    int used, dev;
    uint32_t id;
    uint32_t *in_trb, *out_trb;
    struct ring in, out;
    int in_dci, out_dci;
    uint32_t mps, bsize, blocks;
};
static struct msc_dev mscs[MAX_MSC];
static uint32_t msc_next_id = 1;

// --- USB RNDIS: one network adapter (phone USB-tethering, or QEMU usb-net) ---
// A CDC-composite: a communications interface (class 0x02 or 0xE0) that takes
// RNDIS control messages as encapsulated commands over EP0, and a CDC-data
// interface (class 0x0A) with a bulk IN + bulk OUT pair carrying framed
// packets. The control handshake runs while the device is set up, and then the
// data path is pure bulk, exactly like MSC.
__attribute__((aligned(4096))) static uint32_t rndis_in_trbs[RING_SIZE * 4];
__attribute__((aligned(4096))) static uint32_t rndis_out_trbs[RING_SIZE * 4];
// Receiving: RNDIS_RX_BUFS transfers are kept with the controller at once,
// each RNDIS_RX_SIZE bytes -- which is also the MaxTransferSize the phone is
// told in INITIALIZE, so it is the most one transfer from it can hold.
//
// Both numbers matter. A phone sends as soon as it has packets; with one
// small buffer posted only when the browser next polled, whatever came in
// between waited in the phone's short queue or was dropped, and every drop
// cost a TCP retransmission timeout -- pages took minutes. And many Android
// phones put several packets into one transfer when the host allows it,
// which the INITIALIZE here always did: into a 2 KiB buffer, of which only
// the first packet was read.
#define RNDIS_RX_BUFS 8
#define RNDIS_RX_SIZE 16384
__attribute__((aligned(RNDIS_RX_SIZE)))
static uint8_t rndis_rxbufs[RNDIS_RX_BUFS][RNDIS_RX_SIZE];
struct rndis_dev {
    struct ring in, out;
    int dev, in_dci, out_dci, comm_if;
    uint32_t mps;
    int ready;
    uint8_t mac[6];
};
static struct rndis_dev rndis;
static int rndis_found = 0;
static const char *rndis_stage = "none";

// RX is non-blocking. The buffers go round in order: each is free, with the
// controller, or full and waiting to be read; bulk IN transfers on one
// endpoint complete in the order they were posted, so the next completion is
// always rndis_rx_fill's. Whichever event drainer sees a completion records
// it here; rndis_recv reads the packets out.
#define RX_FREE   0
#define RX_POSTED 1
#define RX_FULL   2
static volatile uint8_t  rndis_rx_state[RNDIS_RX_BUFS];
static volatile uint32_t rndis_rx_got[RNDIS_RX_BUFS];   // bytes in a full one
static volatile int      rndis_rx_fill;   // the next to complete
static int               rndis_rx_post;   // the next to hand to the controller
static int               rndis_rx_read;   // the next to read packets from
static uint32_t          rndis_rx_off;    // where in it

static int rndis_note_event(struct hc *h, uint32_t ctrl, uint32_t status) {
    if (!rndis_found || devs[rndis.dev].hc != h) return 0;
    uint32_t slot = (ctrl >> 24) & 0xFF;
    uint32_t ep   = (ctrl >> 16) & 0x1F;
    if ((int)slot != devs[rndis.dev].slot || (int)ep != rndis.in_dci) return 0;
    int i = rndis_rx_fill;
    uint32_t cc = (status >> 24) & 0xFF, missing = status & 0xFFFFFF;
    rndis_rx_got[i] = (cc == CC_SUCCESS || cc == CC_SHORT_PKT) && missing <= RNDIS_RX_SIZE
                      ? RNDIS_RX_SIZE - missing : 0;
    rndis_rx_state[i] = RX_FULL;
    rndis_rx_fill = (i + 1) % RNDIS_RX_BUFS;
    return 1;
}

// --- events ------------------------------------------------------------------

// Take the next event off a controller's ring, if there is one.
static int next_event(struct hc *h, uint32_t ev[4]) {
    uint32_t *e = &h->evt[h->evt_deq * 4];
    uint32_t ctrl = e[3];
    if (((ctrl & TRB_CYCLE) ? 1 : 0) != h->evt_cycle) return 0;
    __asm__ volatile ("" ::: "memory");
    ev[0] = e[0]; ev[1] = e[1]; ev[2] = e[2]; ev[3] = ctrl;
    if (++h->evt_deq == EVT_SIZE) { h->evt_deq = 0; h->evt_cycle ^= 1; }
    w64(h->rt, RT_ERDP, ((uint64_t)(uintptr_t)&h->evt[h->evt_deq * 4]) | (1u << 3));
    return 1;
}

// A device came or went on a root port. Acknowledged here -- a port reports no
// further change until it is -- and left for usb_service. The reset's own
// change bit is not touched: a reset in progress is waiting for it.
static void port_event(struct hc *h, uint32_t param_lo) {
    int port = (int)((param_lo >> 24) & 0xFF);
    if (port < 1 || port > (int)h->max_ports) return;
    uint32_t v = r32(h->op, OP_PORTSC(port - 1));
    w32(h->op, OP_PORTSC(port - 1), (v & ~PORTSC_RW1C) |
        (v & (PORTSC_CSC | PORTSC_PEC | PORTSC_OCC | PORTSC_PLC | PORTSC_CEC)));
    if ((v & (PORTSC_CSC | PORTSC_PEC)) && port <= 64) {
        h->pending |= 1ull << (port - 1);
        work = 1;
    }
}

static int hid_event(struct hc *h, uint32_t slot, uint32_t epid, uint32_t status);
static int hub_event(struct hc *h, uint32_t slot, uint32_t epid, uint32_t status);

// An event nobody in particular is waiting for: a key, a mouse moved, a hub
// or a port saying something changed, a packet for the network.
static void other_event(struct hc *h, const uint32_t ev[4]) {
    uint32_t type = TRB_TYPE_OF(ev[3]);
    if (type == TRB_PORT_STATUS_CHANGE) { port_event(h, ev[0]); return; }
    if (type != TRB_TRANSFER_EVENT) return;
    if (rndis_note_event(h, ev[3], ev[2])) return;
    uint32_t slot = (ev[3] >> 24) & 0xFF, epid = (ev[3] >> 16) & 0x1F;
    if (hid_event(h, slot, epid, ev[2])) return;
    hub_event(h, slot, epid, ev[2]);
}

// Wait for a completion, returning its completion code, or -1 on timeout.
//
// What is waited for: the completion of the TRB at `expect_trb`; or, when that
// is 0 and `ep_slot` is not, the next transfer event from endpoint `ep_dci` of
// slot `ep_slot` -- a transfer of several TRBs can finish early, at whichever
// of them saw a short packet; or, with both 0, the first completion of any
// kind.
//
// Everything else on the ring is dealt with on the way, never dropped: a
// keyboard or mouse report is taken and its endpoint armed again. A disk
// transfer can take long enough for a key to be pressed in the middle of it,
// and a report thrown away here used to leave that keyboard with nothing
// armed -- silent for good.
static int wait_completion(struct hc *h, uint64_t expect_trb, int ep_slot, int ep_dci,
                           uint32_t *out_slot) {
    uint64_t until = pit_now_us() + 1500000;
    for (uint32_t spin = 0;; spin++) {
        uint32_t ev[4];
        if (!next_event(h, ev)) {
            if ((spin & 255) == 0 && pit_now_us() > until) return -1;
            __asm__ volatile ("pause");
            continue;
        }
        uint32_t type = TRB_TYPE_OF(ev[3]);
        if (type == TRB_CMD_COMPLETION || type == TRB_TRANSFER_EVENT) {
            uint64_t ptr = ((uint64_t)ev[1] << 32) | ev[0];
            uint32_t slot = (ev[3] >> 24) & 0xFF, epid = (ev[3] >> 16) & 0x1F;
            int mine = expect_trb ? ptr == expect_trb
                     : ep_slot   ? (type == TRB_TRANSFER_EVENT &&
                                    (int)slot == ep_slot && (int)epid == ep_dci)
                     :             1;
            if (mine) {
                if (out_slot) *out_slot = slot;
                return (int)TRB_CC(ev[2]);
            }
        }
        other_event(h, ev);
    }
}

// One command on the controller's command ring.
static int command(struct hc *h, uint32_t d0, uint32_t d1, uint32_t ctrl, uint32_t *out_slot) {
    uint64_t c = ring_push(&h->cmd, d0, d1, 0, ctrl);
    ring_doorbell(h, 0, 0);
    return wait_completion(h, c, 0, 0, out_slot);
}

static int ctx_command(struct hc *h, int type, int slot) {
    uint64_t in = (uint64_t)(uintptr_t)h->in_ctx;
    return command(h, (uint32_t)in, (uint32_t)(in >> 32),
                   TRB_TYPE(type) | ((uint32_t)slot << 24), NULL);
}

// --- context helpers -----------------------------------------------------------

static uint32_t *in_slot(struct hc *h) {
    return (uint32_t *)(h->in_ctx + h->ctx_size);   // input control ctx is [0], slot [1]
}
static uint32_t *in_ep(struct hc *h, int dci) {
    return (uint32_t *)(h->in_ctx + h->ctx_size * (dci + 1));
}
static void in_clear(struct hc *h) {
    for (int i = 0; i < 4096; i++) h->in_ctx[i] = 0;
}

// --- control transfers -----------------------------------------------------------

// Bring an endpoint back after a transfer on it failed. The controller halts
// an endpoint whose transfer the device refused (a STALL) or that went wrong,
// and it takes nothing more until it is reset; one that simply never answered
// is stopped instead. Either way it is then pointed past what is left of the
// failed transfer, at the next free place on its ring.
#define TRB_STOP_ENDPOINT 15
static void ep_recover(int d, int dci, struct ring *r, int halted) {
    struct udev *u = &devs[d];
    struct hc *h = u->hc;
    uint32_t who = ((uint32_t)dci << 16) | ((uint32_t)u->slot << 24);
    command(h, 0, 0, TRB_TYPE(halted ? TRB_RESET_ENDPOINT : TRB_STOP_ENDPOINT) | who, NULL);
    uint64_t deq = (uint64_t)(uintptr_t)&r->trb[r->enq * 4] | (r->cycle ? 1 : 0);
    command(h, (uint32_t)deq, (uint32_t)(deq >> 32), TRB_TYPE(TRB_SET_TR_DEQUEUE) | who, NULL);
}

// A control transfer on device d's EP0. A request with the IN bit (0x80 in
// `type`) reads up to `len` bytes into xfer_buf; one without sends `len`
// bytes of `out`. Returns the completion code.
static int ctrl(int d, uint8_t type, uint8_t req, uint16_t val, uint16_t idx,
                uint16_t len, const void *out) {
    struct udev *u = &devs[d];
    struct hc *h = u->hc;
    int in = (type & 0x80) != 0;
    uint32_t s0 = type | ((uint32_t)req << 8) | ((uint32_t)val << 16);
    uint32_t s1 = idx | ((uint32_t)len << 16);
    // Transfer type: no data 0, OUT data 2, IN data 3.
    uint32_t trt = len ? (in ? 3u : 2u) : 0u;
    ring_push(&u->ep0, s0, s1, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));
    if (len) {
        uint8_t *b = in ? xfer_buf : xfer_out;
        if (!in) {
            if (len > sizeof xfer_out) len = sizeof xfer_out;
            for (int i = 0; i < len; i++) xfer_out[i] = ((const uint8_t *)out)[i];
        }
        uint64_t a = (uint64_t)(uintptr_t)b;
        ring_push(&u->ep0, (uint32_t)a, (uint32_t)(a >> 32), len,
                  TRB_TYPE(TRB_DATA) | (in ? (1u << 16) : 0));
    }
    // Status stage: the other direction from the data, IN when there is none.
    uint32_t dir = (len && in) ? 0u : (1u << 16);
    ring_push(&u->ep0, 0, 0, 0, TRB_TYPE(TRB_STATUS) | dir | TRB_IOC);
    ring_doorbell(h, u->slot, 1);
    // The next event from this EP0 is the transfer's end: the status stage
    // completing, or the stage the device refused (a STALL is reported on the
    // TRB the controller was at, not on the last one).
    int cc = wait_completion(h, 0, u->slot, 1, NULL);
    if (cc != CC_SUCCESS) ep_recover(d, 1, &u->ep0, cc != -1);
    return cc;
}

static int ok(int cc) { return cc == CC_SUCCESS || cc == CC_SHORT_PKT; }

// --- controller start-up ---------------------------------------------------------

static int reset_controller(struct hc *h) {
    // halt
    uint32_t cmd = r32(h->op, OP_USBCMD);
    w32(h->op, OP_USBCMD, cmd & ~USBCMD_RUN);
    for (int i = 0; i < 100; i++) {
        if (r32(h->op, OP_USBSTS) & USBSTS_HCH) break;
        delay_ms(1);
    }
    // reset
    w32(h->op, OP_USBCMD, USBCMD_HCRST);
    for (int i = 0; i < 200; i++) {
        if (!(r32(h->op, OP_USBCMD) & USBCMD_HCRST) &&
            !(r32(h->op, OP_USBSTS) & USBSTS_CNR)) return 1;
        delay_ms(1);
    }
    return 0;
}

// The pages the controller asks for to keep its own state in. Every one it
// asks for: a controller given fewer reads the missing entries as address 0.
static int setup_scratchpad(struct hc *h) {
    uint32_t hcs2 = r32(h->mmio, CAP_HCSPARAMS2);
    uint32_t n = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
    h->dcbaa[0] = 0;
    if (n == 0) return 1;
    uint64_t arr_pages = (n * 8 + 4095) / 4096;
    uint64_t arr = pmm_alloc_contig(arr_pages);
    if (!arr || arr + arr_pages * 4096 > 0x100000000ULL) return 0;
    uint64_t *a = (uint64_t *)(uintptr_t)arr;
    for (uint32_t i = 0; i < n; i++) {
        void *p = page();
        if (!p) return 0;
        a[i] = (uint64_t)(uintptr_t)p;
    }
    h->dcbaa[0] = arr;
    DBG("xhci: %u scratchpad buffers\n", n);
    return 1;
}

// Take ownership of the controller from the firmware. On real UEFI hardware the
// BIOS/SMM owns the xHCI (for legacy USB support) and will not let the OS drive
// it until we set the OS-Owned semaphore and it clears BIOS-Owned. We also
// disable the firmware's SMIs so it stops touching the controller behind us.
// QEMU has no such capability, so this is a no-op there. hcc1 = HCCPARAMS1.
static void bios_handoff(struct hc *h, uint32_t hcc1) {
    uint32_t off = ((hcc1 >> 16) & 0xFFFF) * 4;   // byte offset of first xECP
    for (int guard = 0; off && guard < 64; guard++) {
        uint32_t cap = r32(h->mmio, off);
        uint8_t id = cap & 0xFF;
        if (id == 1) {                            // USB Legacy Support capability
            if (cap & (1u << 16)) {               // currently BIOS-owned
                w32(h->mmio, off, cap | (1u << 24)); // request HC OS Owned
                for (int i = 0; i < 1000; i++) {  // wait up to ~1 s
                    if (!(r32(h->mmio, off) & (1u << 16))) break;
                    delay_ms(1);
                }
                DBG("xhci: bios handoff legsup=%x\n", r32(h->mmio, off));
            }
            // USBLEGCTLSTS (off+4): disable all SMI sources, clear SMI status.
            uint32_t ctl = r32(h->mmio, off + 4);
            ctl &= ~((1u<<0)|(1u<<4)|(1u<<13)|(1u<<14)|(1u<<15));  // SMI enables off
            ctl |=  (1u<<29)|(1u<<30)|(1u<<31)|(1u<<20);           // W1C status bits
            w32(h->mmio, off + 4, ctl);
            return;
        }
        uint8_t next = (cap >> 8) & 0xFF;
        off = next ? off + next * 4 : 0;
    }
}

static int hc_start(struct hc *h, const pci_device_t *devp) {
    pci_device_t dev = *devp;
    int k = (int)(h - hcs);
    pci_enable_bus_master(&dev);
    h->bus = dev.bus; h->dev = dev.slot; h->fn = dev.func;

    uint64_t bar = (dev.bar[0] & ~0xFULL);
    if (dev.bar[0] & 0x4) bar |= ((uint64_t)dev.bar[1] << 32);  // 64-bit BAR
    DBG("xhci: at %x:%x.%x MMIO=%x:%x\n", dev.bus, dev.slot, dev.func,
        (uint32_t)(bar >> 32), (uint32_t)bar);

    // The BAR sits far above RAM on real hardware (768 GiB on q35), and the
    // boot page tables only cover the low 4 GiB. Map it before the first
    // register read, or that read triple-faults the machine.
    paging_map_mmio(bar, 0x10000);
    h->mmio = (volatile uint8_t *)(uintptr_t)bar;

    uint8_t caplen = (uint8_t)(r32(h->mmio, CAP_CAPLENGTH) & 0xFF);
    h->op = h->mmio + caplen;
    h->rt = h->mmio + (r32(h->mmio, CAP_RTSOFF) & ~0x1Fu);
    h->db = (volatile uint32_t *)(h->mmio + (r32(h->mmio, CAP_DBOFF) & ~0x3u));

    uint32_t hcs1 = r32(h->mmio, CAP_HCSPARAMS1);
    h->max_slots = hcs1 & 0xFF;
    h->max_ports = (hcs1 >> 24) & 0xFF;
    uint32_t hcc1 = r32(h->mmio, CAP_HCCPARAMS1);
    h->ctx_size = (hcc1 & (1u << 2)) ? 64 : 32;
    DBG("xhci: slots=%u ports=%u ctx=%u\n", h->max_slots, h->max_ports, h->ctx_size);

    // Wrest the controller from the firmware before touching it (real HW).
    bios_handoff(h, hcc1);

    if (!reset_controller(h)) { h->stage = "controller reset failed"; return 0; }

    // Program max slots enabled.
    w32(h->op, OP_CONFIG, h->max_slots);

    h->dcbaa = hc_dcbaa[k];
    for (int i = 0; i < 512; i++) h->dcbaa[i] = 0;
    if (!setup_scratchpad(h)) { h->stage = "no memory for scratchpad"; return 0; }
    w64(h->op, OP_DCBAAP, (uint64_t)(uintptr_t)h->dcbaa);
    h->in_ctx = hc_in[k];

    // Command ring
    ring_init(&h->cmd, hc_cmd[k]);
    w64(h->op, OP_CRCR, (uint64_t)(uintptr_t)hc_cmd[k] | 1 /*RCS*/);

    // Event ring: one segment, ERST -> evt
    h->evt = hc_evt[k];
    for (int i = 0; i < EVT_SIZE * 4; i++) h->evt[i] = 0;
    h->evt_deq = 0; h->evt_cycle = 1;
    h->erst = hc_erst[k];
    h->erst[0] = (uint32_t)(uintptr_t)h->evt;
    h->erst[1] = (uint32_t)((uint64_t)(uintptr_t)h->evt >> 32);
    h->erst[2] = EVT_SIZE;   // segment size
    h->erst[3] = 0;
    w32(h->rt, RT_ERSTSZ, 1);
    w64(h->rt, RT_ERDP, (uint64_t)(uintptr_t)h->evt);
    w64(h->rt, RT_ERSTBA, (uint64_t)(uintptr_t)h->erst);

    // Run.
    w32(h->op, OP_USBCMD, USBCMD_RUN);
    for (int i = 0; i < 100; i++) {
        if (!(r32(h->op, OP_USBSTS) & USBSTS_HCH)) break;
        delay_ms(1);
    }
    DBG("xhci: running, usbsts=%x\n", r32(h->op, OP_USBSTS));

    // Power on every port without disturbing PED or acking change bits.
    for (uint32_t p = 0; p < h->max_ports; p++) {
        uint32_t sc = r32(h->op, OP_PORTSC(p));
        if (!(sc & PORTSC_PP)) { w32(h->op, OP_PORTSC(p), (sc & ~PORTSC_RW1C) | PORTSC_PP); delay_ms(20); }
    }
    h->up = 1;
    h->stage = "running";
    return 1;
}

// --- diagnostics -------------------------------------------------------------------

// Per-device breakdown for the on-screen report when no keyboard came up: what
// each connected device is, so we can see (on real hardware) exactly what the
// keyboard presents.
struct pdiag {
    uint8_t hc, port, depth, speed, addressed, nif;
    uint8_t cls[6], proto[6];     // interface class + protocol, first 6 interfaces
    const char *stage;
};
static struct pdiag g_pd[24];
static int g_npd;

// --- setting a device up -------------------------------------------------------------

static int attach(struct hc *h, int parent, int port, int strict);
static void detach(int d, int quiet);
static int hid_translate(uint8_t u, int shift, uint8_t *code, char *ascii);

// Whether an HID that is no boot keyboard is passed over (see attach): while
// the machine starts, and whenever a real keyboard is there already.
static int strict_now(void) { return booting || nkbds > 0; }

static int dev_alloc(void) {
    for (int i = 0; i < MAX_DEV; i++) if (!devs[i].used) return i;
    return -1;
}

static void disable_slot(int d) {
    struct udev *u = &devs[d];
    if (u->slot <= 0) return;
    command(u->hc, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)u->slot << 24), NULL);
    u->hc->dcbaa[u->slot] = 0;
    u->slot = 0;
}

// Add endpoints to device d's slot. `ep` holds up to three as {dci, type,
// max packet, interval exponent, ring}. With `hub_ports`, the slot is also
// made a hub's: the controller then routes to the ports below it, and for a
// high-speed hub `hub_ttt` is its transaction translator's think time.
struct epdef { int dci, type, mps, interval; uint32_t *trb; };
static int add_endpoints(int d, const struct epdef *ep, int n, int hub_ports, int hub_ttt) {
    struct udev *u = &devs[d];
    struct hc *h = u->hc;
    in_clear(h);
    uint32_t *icc = (uint32_t *)h->in_ctx;
    icc[1] = 1u << 0;                       // the slot context
    int entries = u->entries;
    for (int i = 0; i < n; i++) {
        icc[1] |= 1u << ep[i].dci;
        if (ep[i].dci > entries) entries = ep[i].dci;
        uint32_t *e = in_ep(h, ep[i].dci);
        uint32_t mps = (uint32_t)ep[i].mps;
        e[0] = (uint32_t)ep[i].interval << 16;
        e[1] = ((uint32_t)ep[i].type << 3) | (mps << 16) | (3u << 1);   // CErr 3
        uint64_t r = (uint64_t)(uintptr_t)ep[i].trb;
        e[2] = (uint32_t)r | 1;                                         // DCS
        e[3] = (uint32_t)(r >> 32);
        // Average TRB length; and for a periodic endpoint, what it may move
        // in one interval.
        e[4] = mps | (ep[i].type == EP_INTR_IN ? mps << 16 : 0);
    }
    // The slot as the controller has it, with the new count of endpoints.
    uint32_t *s = in_slot(h);
    const uint32_t *o = (const uint32_t *)u->ctx;
    s[0] = o[0]; s[1] = o[1]; s[2] = o[2]; s[3] = 0;
    s[0] = (s[0] & ~(0x1Fu << 27)) | ((uint32_t)entries << 27);
    if (hub_ports) {
        s[0] |= 1u << 26;
        s[1] = (s[1] & 0x00FFFFFFu) | ((uint32_t)hub_ports << 24);
        s[2] = (s[2] & ~(3u << 16)) | ((uint32_t)(hub_ttt & 3) << 16);
    }
    if (ctx_command(h, TRB_CONFIGURE_ENDPOINT, u->slot) != CC_SUCCESS) return 0;
    u->entries = entries;
    return 1;
}

// Post one interrupt IN transfer of up to `len` bytes into buf.
static void arm_intr(struct hc *h, struct ring *r, uint8_t *buf, uint32_t len, int slot, int dci) {
    for (uint32_t i = 0; i < len; i++) buf[i] = 0;
    ring_push(r, (uint32_t)(uintptr_t)buf, (uint32_t)((uint64_t)(uintptr_t)buf >> 32),
              len, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    ring_doorbell(h, slot, (uint32_t)dci);
}

// How much a HID interrupt transfer asks for: the endpoint's packet, which the
// buffer has room for (see HID_BUF).
static uint32_t hid_len(int mps) {
    if (mps <= 0) mps = 8;
    return (uint32_t)(mps < HID_BUF ? mps : HID_BUF);
}

static int add_keyboard(int d, int iface, int ep_addr, int mps) {
    int k = -1;
    for (int i = 0; i < MAX_KBD; i++) if (!kbds[i].used) { k = i; break; }
    if (k < 0) return 0;
    struct kbdev *kb = &kbds[k];
    struct udev *u = &devs[d];

    // SET_PROTOCOL(boot) on the HID interface (some devices STALL -> ignore).
    ctrl(d, 0x21, 0x0B, 0, (uint16_t)iface, 0, NULL);

    uint32_t *pg = page();
    if (!pg) return 0;
    if (mps <= 0) mps = 8;
    int dci = (ep_addr & 0x0F) * 2 + 1;
    ring_init(&kb->intr, pg);
    struct epdef e = { dci, EP_INTR_IN, mps, 6 /* 2^6 x 125 us = 8 ms */, pg };
    if (!add_endpoints(d, &e, 1, 0, 0)) { unpage(pg); return 0; }

    kb->used = 1;
    kb->dev = d;
    kb->dci = dci;
    kb->trb = pg;
    kb->buf = (uint8_t *)pg + 2048;
    kb->len = hid_len(mps);
    for (int i = 0; i < 6; i++) kb->prev[i] = 0;
    kb->prevmod = 0;
    nkbds++;
    arm_intr(u->hc, &kb->intr, kb->buf, kb->len, u->slot, dci);
    DBG("xhci: keyboard iface %d ep %x dci %d on slot %d\n", iface, ep_addr, dci, u->slot);
    return 1;
}

// A mouse's interrupt IN endpoint, on its own slot or beside a keyboard's.
// `rlen` is the length of its HID report descriptor, 0 if it gave none.
static int add_mouse(int d, int iface, int ep_addr, int mps, int rlen) {
    int m = -1;
    for (int i = 0; i < MAX_MOUSE; i++) if (!mice[i].used) { m = i; break; }
    if (m < 0) return 0;
    struct mousedev *ms = &mice[m];
    struct udev *u = &devs[d];

    // Where its reports keep the buttons, X, Y and wheel: from its own report
    // descriptor, and then the reports are taken as they are described. Only
    // when that cannot be read is the boot protocol asked for -- a request
    // some devices accept and then ignore (see hid.h).
    int parsed = 0;
    if (rlen > (int)sizeof xfer_buf) rlen = (int)sizeof xfer_buf;
    if (rlen > 0 && ctrl(d, 0x81, 6, 0x2200, (uint16_t)iface, (uint16_t)rlen, NULL) == CC_SUCCESS)
        parsed = hid_parse_mouse(xfer_buf, rlen, &ms->fmt);
    if (!parsed) hid_mouse_boot(&ms->fmt);
    ctrl(d, 0x21, 0x0B, parsed ? 1 : 0, (uint16_t)iface, 0, NULL);   // SET_PROTOCOL; some STALL
    DBG("xhci: mouse reports %s: id %d x %d/%d y %d/%d wheel %d/%d\n",
        parsed ? "as described" : "boot protocol", ms->fmt.id,
        ms->fmt.x, ms->fmt.xs, ms->fmt.y, ms->fmt.ys, ms->fmt.wheel, ms->fmt.ws);

    uint32_t *pg = page();
    if (!pg) return 0;
    if (mps <= 0) mps = 8;
    int dci = (ep_addr & 0x0F) * 2 + 1;
    ring_init(&ms->intr, pg);
    struct epdef e = { dci, EP_INTR_IN, mps, 6, pg };
    if (!add_endpoints(d, &e, 1, 0, 0)) { unpage(pg); return 0; }

    ms->used = 1;
    ms->dev = d;
    ms->dci = dci;
    ms->trb = pg;
    ms->buf = (uint8_t *)pg + 2048;
    ms->len = hid_len(mps);
    nmice++;
    mouse_set_present(1);
    arm_intr(u->hc, &ms->intr, ms->buf, ms->len, u->slot, dci);
    DBG("xhci: mouse on slot %d dci %d mps %d\n", u->slot, dci, mps);
    return 1;
}

static int bulk_mps(int speed) { return speed >= 4 ? 1024 : (speed == 3 ? 512 : 64); }

static int add_disk(int d, int in_addr, int out_addr) {
    int k = -1;
    for (int i = 0; i < MAX_MSC; i++) if (!mscs[i].used) { k = i; break; }
    if (k < 0) return 0;
    struct msc_dev *m = &mscs[k];
    struct udev *u = &devs[d];
    uint32_t *pi = page(), *po = page();
    if (!pi || !po) { unpage(pi); unpage(po); return 0; }
    int mps = bulk_mps(u->speed);
    int in_dci = (in_addr & 0x0F) * 2 + 1, out_dci = (out_addr & 0x0F) * 2;
    ring_init(&m->in, pi);
    ring_init(&m->out, po);
    struct epdef e[2] = { { in_dci, EP_BULK_IN, mps, 0, pi }, { out_dci, EP_BULK_OUT, mps, 0, po } };
    if (!add_endpoints(d, e, 2, 0, 0)) { unpage(pi); unpage(po); return 0; }
    m->dev = d;
    m->in_trb = pi; m->out_trb = po;
    m->in_dci = in_dci; m->out_dci = out_dci;
    m->mps = (uint32_t)mps; m->bsize = 512; m->blocks = 0;
    m->id = msc_next_id++;
    m->used = 1;
    DBG("xhci: mass-storage #%d on slot %d in-dci %d out-dci %d\n", k, u->slot, in_dci, out_dci);
    return 1;
}

static int rndis_control_init(int d);

// A RNDIS/CDC network adapter: its bulk endpoints, then the RNDIS control
// handshake on EP0.
static int add_rndis(int d, int in_addr, int out_addr, int comm_if) {
    struct udev *u = &devs[d];
    int mps = bulk_mps(u->speed);
    int in_dci = (in_addr & 0x0F) * 2 + 1, out_dci = (out_addr & 0x0F) * 2;
    ring_init(&rndis.in, rndis_in_trbs);
    ring_init(&rndis.out, rndis_out_trbs);
    struct epdef e[2] = { { in_dci, EP_BULK_IN, mps, 0, rndis_in_trbs },
                          { out_dci, EP_BULK_OUT, mps, 0, rndis_out_trbs } };
    if (!add_endpoints(d, e, 2, 0, 0)) { rndis_stage = "config-ep"; return 0; }
    rndis.dev = d; rndis.in_dci = in_dci; rndis.out_dci = out_dci;
    rndis.mps = (uint32_t)mps; rndis.comm_if = comm_if; rndis.ready = 0;
    for (int i = 0; i < RNDIS_RX_BUFS; i++) rndis_rx_state[i] = RX_FREE;
    rndis_rx_fill = rndis_rx_post = rndis_rx_read = 0;
    rndis_rx_off = 0;
    rndis_found = 1;
    DBG("xhci: rndis on slot %d in-dci %d out-dci %d comm-if %d\n", u->slot, in_dci, out_dci, comm_if);
    if (rndis_control_init(d)) rndis.ready = 1;
    return 1;
}

// --- hubs --------------------------------------------------------------------------

#define HUB_GET_STATUS   0
#define HUB_CLEAR_FEAT   1
#define HUB_SET_FEAT     3
#define HUB_SET_DEPTH    12
#define PORT_RESET       4
#define PORT_POWER       8
#define C_PORT_CONNECTION 16
#define C_PORT_ENABLE    17
#define C_PORT_SUSPEND   18
#define C_PORT_OVERCURRENT 19
#define C_PORT_RESET     20
#define C_PORT_LINK_STATE 25
#define C_PORT_CONFIG_ERROR 26
#define C_BH_PORT_RESET  29

// A hub port's status and what changed, or 0 if the hub did not answer.
static int hub_port_status(int d, int port, uint16_t *st, uint16_t *ch) {
    if (!ok(ctrl(d, 0xA3, HUB_GET_STATUS, 0, (uint16_t)port, 4, NULL))) return 0;
    *st = (uint16_t)(xfer_buf[0] | (xfer_buf[1] << 8));
    *ch = (uint16_t)(xfer_buf[2] | (xfer_buf[3] << 8));
    return 1;
}

// Acknowledge every change a hub port reports, so it reports the next.
static void hub_port_ack(int d, int port, uint16_t ch) {
    static const uint8_t usb2[5] = { C_PORT_CONNECTION, C_PORT_ENABLE, C_PORT_SUSPEND,
                                     C_PORT_OVERCURRENT, C_PORT_RESET };
    static const uint8_t usb3[4] = { C_BH_PORT_RESET, C_PORT_LINK_STATE, C_PORT_CONFIG_ERROR, 0 };
    for (int i = 0; i < 5; i++)
        if (ch & (1u << i)) ctrl(d, 0x23, HUB_CLEAR_FEAT, usb2[i], (uint16_t)port, 0, NULL);
    if (devs[d].usb3hub)
        for (int i = 0; i < 3; i++)
            if (ch & (1u << (5 + i))) ctrl(d, 0x23, HUB_CLEAR_FEAT, usb3[i], (uint16_t)port, 0, NULL);
}

// Reset the device on a hub's port and say how fast it runs. 0 if none came up.
static int hub_reset(int d, int port, int *speed) {
    uint16_t st = 0, ch = 0;
    if (!ok(ctrl(d, 0x23, HUB_SET_FEAT, PORT_RESET, (uint16_t)port, 0, NULL))) return 0;
    int done = 0;
    for (int i = 0; i < 50 && !done; i++) {
        delay_ms(10);
        if (!hub_port_status(d, port, &st, &ch)) return 0;
        done = (ch & (1u << 4)) && !(st & (1u << 4));
    }
    hub_port_ack(d, port, ch);
    if (!done) return 0;
    delay_ms(10);                               // reset recovery
    if (!hub_port_status(d, port, &st, &ch)) return 0;
    if (!(st & 1) || !(st & 2)) return 0;       // connected and enabled
    *speed = devs[d].usb3hub ? 4 : (st & (1u << 9)) ? 2 : (st & (1u << 10)) ? 3 : 1;
    return 1;
}

// Device d is a hub: tell the controller so, power its ports, and set up
// whatever is already plugged into them.
static int add_hub(int d, int ep_addr, int mps, int bint) {
    struct udev *u = &devs[d];
    if (u->depth >= 5) return 0;               // USB allows five tiers of hubs
    u->usb3hub = u->speed >= 4;
    // A SuperSpeed hub has to know how deep it is before it can route.
    if (u->usb3hub && !ok(ctrl(d, 0x20, HUB_SET_DEPTH, (uint16_t)u->depth, 0, 0, NULL))) return 0;
    if (!ok(ctrl(d, 0xA0, 6, u->usb3hub ? 0x2A00 : 0x2900, 0, 12, NULL))) return 0;
    int nports = xfer_buf[2];
    uint16_t chars = (uint16_t)(xfer_buf[3] | (xfer_buf[4] << 8));
    int pwr_ms = xfer_buf[5] * 2;
    if (nports < 1 || nports > 15) return 0;
    int ttt = u->speed == 3 ? (chars >> 5) & 3 : 0;

    uint32_t *pg = page();
    if (!pg) return 0;
    int dci = (ep_addr & 0x0F) * 2 + 1;
    // How often it is asked what changed: its own interval, but no rarer than
    // 2^10 x 125 us = 128 ms, so a plug is noticed quickly.
    int interval = u->speed >= 3 ? bint - 1 : 7;
    if (interval < 3) interval = 3;
    if (interval > 10) interval = 10;
    ring_init(&u->hub_intr, pg);
    struct epdef e = { dci, EP_INTR_IN, mps > 0 ? mps : 1, interval, pg };
    if (!add_endpoints(d, &e, 1, nports, ttt)) { unpage(pg); return 0; }
    u->hub_trb = pg;
    u->hub_dci = dci;
    u->hub_len = (uint32_t)(mps > 0 && mps < 8 ? mps : 8);
    u->nports = nports;
    u->kind = USB_KIND_HUB;
    DBG("xhci: hub on slot %d, %d ports%s, depth %d\n", u->slot, nports,
        u->usb3hub ? " (SuperSpeed)" : "", u->depth);

    for (int p = 1; p <= nports; p++)
        ctrl(d, 0x23, HUB_SET_FEAT, PORT_POWER, (uint16_t)p, 0, NULL);
    delay_ms(pwr_ms > 100 ? pwr_ms : 100);

    // What is plugged in already. A port with a device to set up is noted as
    // news and handled like any other plug, by usb_service -- or right here
    // while the machine starts, so start-up sees the whole tree at once.
    for (int p = 1; p <= nports; p++) {
        uint16_t st, ch;
        if (!hub_port_status(d, p, &st, &ch)) continue;
        hub_port_ack(d, p, ch);
        if (st & 1) u->hub_pending |= 1u << p;
    }
    arm_intr(u->hc, &u->hub_intr, (uint8_t *)pg + 2048, u->hub_len, u->slot, dci);
    if (u->hub_pending) work = 1;
    return 1;
}

// A hub's status-change endpoint answered: a bit for every port with news.
static int hub_event(struct hc *h, uint32_t slot, uint32_t epid, uint32_t status) {
    for (int d = 0; d < MAX_DEV; d++) {
        struct udev *u = &devs[d];
        if (!u->used || u->kind != USB_KIND_HUB || u->hc != h || u->slot != (int)slot ||
            u->hub_dci != (int)epid) continue;
        uint8_t *b = (uint8_t *)u->hub_trb + 2048;
        uint32_t cc = TRB_CC(status);
        if (cc == CC_SUCCESS || cc == CC_SHORT_PKT) {
            uint32_t bits = b[0] | ((uint32_t)b[1] << 8);
            bits &= ~1u;                         // bit 0 is the hub itself
            if (bits) u->hub_pending |= bits;
            arm_intr(h, &u->hub_intr, b, u->hub_len, u->slot, u->hub_dci);
        } else {
            // The endpoint stopped. Usually the hub is gone, and its port
            // above says so; if not, usb_service starts it again.
            u->hub_stalled = 1;
        }
        work = 1;
        return 1;
    }
    return 0;
}

// --- the device's own descriptions --------------------------------------------------

// A string descriptor as UTF-8, spaces trimmed; "" if it has none.
static void read_string(int d, int idx, char *out, int max) {
    struct udev *u = &devs[d];
    out[0] = 0;
    if (!idx) return;
    if (!u->lang) {
        if (!ok(ctrl(d, 0x80, 6, 0x0300, 0, 4, NULL)) || xfer_buf[0] < 4) return;
        u->lang = (uint16_t)(xfer_buf[2] | (xfer_buf[3] << 8));
    }
    if (!ok(ctrl(d, 0x80, 6, (uint16_t)(0x0300 | idx), u->lang, 255, NULL))) return;
    int n = xfer_buf[0], k = 0;
    if (n > 255) n = 255;
    for (int i = 2; i + 1 < n; i += 2) {
        uint32_t c = xfer_buf[i] | ((uint32_t)xfer_buf[i + 1] << 8);
        if (c < 0x20) continue;
        if (c < 0x80) { if (k + 1 >= max) break; out[k++] = (char)c; }
        else if (c < 0x800) {
            if (k + 2 >= max) break;
            out[k++] = (char)(0xC0 | (c >> 6)); out[k++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (k + 3 >= max) break;
            out[k++] = (char)(0xE0 | (c >> 12)); out[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[k++] = (char)(0x80 | (c & 0x3F));
        }
    }
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = 0;
    int s = 0;
    while (out[s] == ' ') s++;
    if (s) { int i = 0; while (out[s + i]) { out[i] = out[s + i]; i++; } out[i] = 0; }
}

// --- reset, address, configure --------------------------------------------------------

// Reset a controller port and say how fast its device runs. 0 if none came up.
//
// Every PORTSC write masks off PORTSC_RW1C first: PED (bit 1) is write-1-to-
// clear and real AMD xHCI sets PED the instant reset completes, so writing the
// register back verbatim would disable the port we just enabled (QEMU sets PED
// later, hiding it).
static int root_reset(struct hc *h, int port, int *speed) {
    uint32_t sc = r32(h->op, OP_PORTSC(port));
    w32(h->op, OP_PORTSC(port), (sc & ~PORTSC_RW1C) | PORTSC_PP | PORTSC_PR);   // start reset
    for (int i = 0; i < 500; i++) {
        sc = r32(h->op, OP_PORTSC(port));
        if (sc & PORTSC_PRC) break;
        delay_ms(1);
    }
    w32(h->op, OP_PORTSC(port), (r32(h->op, OP_PORTSC(port)) & ~PORTSC_RW1C) | PORTSC_PRC | PORTSC_CSC);
    for (int i = 0; i < 200; i++) {
        sc = r32(h->op, OP_PORTSC(port));
        if (sc & PORTSC_PED) break;
        delay_ms(1);
    }
    DBG("xhci: port %d after reset sc=%x\n", port, sc);
    if (!(sc & PORTSC_PED)) return 0;
    *speed = (int)((sc >> 10) & 0xF);
    return 1;
}

// Set up the device on a port: of the controller (parent -1, port 0-based) or
// of hub `parent` (port 1-based). `strict` passes over an HID that presents
// neither a boot keyboard nor a mouse -- at start-up such a thing is far more
// likely a board's lighting controller than the keyboard -- remembering it
// in case no real keyboard turns up. Returns the device's index, or -1.
static int attach(struct hc *h, int parent, int port, int strict) {
    struct pdiag *pd = g_npd < 24 ? &g_pd[g_npd++] : NULL;
    if (pd) {
        pd->hc = (uint8_t)(h - hcs); pd->port = (uint8_t)port; pd->speed = 0; pd->addressed = 0;
        pd->nif = 0; pd->stage = "?";
        pd->depth = (uint8_t)(parent < 0 ? 0 : devs[parent].depth + 1);
    }
    const char *stage = "?";
    int d = dev_alloc();
    if (d < 0) { stage = "device table full"; goto fail_early; }

    int speed = 0;
    if (parent < 0 ? !root_reset(h, port, &speed) : !hub_reset(parent, port, &speed)) {
        stage = "port reset/enable failed"; goto fail_early;
    }
    if (pd) pd->speed = (uint8_t)speed;

    struct udev *u = &devs[d];
    for (size_t i = 0; i < sizeof *u; i++) ((uint8_t *)u)[i] = 0;
    u->used = 1;
    u->hc = h;
    u->parent = parent;
    u->port = port;
    u->speed = speed;
    if (parent < 0) {
        u->root = port;
    } else {
        struct udev *p = &devs[parent];
        u->root = p->root;
        u->depth = p->depth + 1;
        u->route = p->route | ((uint32_t)(port > 15 ? 15 : port) << (4 * p->depth));
        // A full- or low-speed device behind a high-speed hub talks through
        // that hub's transaction translator, and the controller must know
        // whose and on which port -- the nearest such hub up the chain.
        if (speed == 1 || speed == 2) {
            if (p->speed == 3) { u->tt_slot = p->slot; u->tt_port = port; }
            else { u->tt_slot = p->tt_slot; u->tt_port = p->tt_port; }
        }
    }

    // Enable Slot. NOTE: the command ring is never re-initialised or CRCR
    // rewritten after the controller runs -- rewriting CRCR mid-flight is
    // illegal and desyncs the controller's dequeue pointer; on real AMD
    // hardware every Enable Slot after the first then fails.
    uint32_t got_slot = 0;
    if (command(h, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &got_slot) != CC_SUCCESS || !got_slot) {
        stage = "enable-slot failed"; u->used = 0; goto fail_early;
    }
    u->slot = (int)got_slot;
    u->ctx = page();
    u->ep0_trb = page();
    if (!u->ctx || !u->ep0_trb) { stage = "out of memory"; goto fail; }
    h->dcbaa[u->slot] = (uint64_t)(uintptr_t)u->ctx;
    DBG("xhci: hc %d port %d (depth %d, route %x) slot %d speed %d\n",
        (int)(h - hcs), port, u->depth, u->route, u->slot, speed);

    // Address Device: the slot and EP0.
    in_clear(h);
    ((uint32_t *)h->in_ctx)[1] = (1u << 0) | (1u << 1);
    uint32_t *s = in_slot(h);
    s[0] = u->route | ((uint32_t)(speed & 0xF) << 20) | (1u << 27);
    s[1] = (uint32_t)(u->root + 1) << 16;
    s[2] = (uint32_t)u->tt_slot | ((uint32_t)u->tt_port << 8);
    ring_init(&u->ep0, u->ep0_trb);
    uint32_t *e0 = in_ep(h, 1);
    uint32_t mps0 = speed >= 4 ? 512 : speed == 2 ? 8 : 64;
    uint64_t tr = (uint64_t)(uintptr_t)u->ep0_trb;
    e0[1] = ((uint32_t)EP_CONTROL << 3) | (mps0 << 16) | (3u << 1);
    e0[2] = (uint32_t)tr | 1;
    e0[3] = (uint32_t)(tr >> 32);
    e0[4] = 8;                                   // average TRB length of a control endpoint
    if (ctx_command(h, TRB_ADDRESS_DEVICE, u->slot) != CC_SUCCESS) { stage = "address-device failed"; goto fail; }
    u->entries = 1;
    if (pd) pd->addressed = 1;
    delay_ms(2);                                 // SET_ADDRESS recovery

    // For a full-speed device, EP0's real max packet size may be 8 (we guessed
    // 64). Read the first 8 bytes of the DEVICE descriptor to learn it; short
    // packets make this read work regardless.
    if (!ok(ctrl(d, 0x80, 6, 0x0100, 0, 8, NULL))) { stage = "get-descriptor failed"; goto fail; }
    if (speed == 1) {
        uint32_t real = xfer_buf[7];
        if ((real == 8 || real == 16 || real == 32 || real == 64) && real != mps0) {
            in_clear(h);
            ((uint32_t *)h->in_ctx)[1] = 1u << 1;    // A1: EP0
            uint32_t *f = in_ep(h, 1);
            f[1] = ((uint32_t)EP_CONTROL << 3) | (real << 16) | (3u << 1);
            f[2] = (uint32_t)tr | 1;
            f[3] = (uint32_t)(tr >> 32);
            f[4] = 8;
            ctx_command(h, TRB_EVALUATE_CONTEXT, u->slot);
        }
    }
    int dev_class = 0, i_product = 0, i_maker = 0;
    if (ok(ctrl(d, 0x80, 6, 0x0100, 0, 18, NULL)) && xfer_buf[0] >= 18) {
        dev_class = xfer_buf[4];
        u->vid = (uint16_t)(xfer_buf[8] | (xfer_buf[9] << 8));
        u->pid = (uint16_t)(xfer_buf[10] | (xfer_buf[11] << 8));
        i_maker = xfer_buf[14];
        i_product = xfer_buf[15];
    }

    // The configuration: its first 9 bytes say how long the whole is.
    if (!ok(ctrl(d, 0x80, 6, 0x0200, 0, 9, NULL))) { stage = "get-descriptor failed"; goto fail; }
    int total = xfer_buf[2] | (xfer_buf[3] << 8);
    if (total > (int)sizeof xfer_buf) total = (int)sizeof xfer_buf;
    if (total < 9) total = 9;
    int cfg_val = xfer_buf[5];   // bConfigurationValue of this configuration
    if (!ok(ctrl(d, 0x80, 6, 0x0200, 0, (uint16_t)total, NULL))) { stage = "get-descriptor failed"; goto fail; }

    // Walk the descriptors. Records each interface for the report so we can
    // see what the device actually is.
    int cur_cls = -1, cur_sub = -1, cur_proto = -1, cur_iface = 0;
    int kbd_if = -1, kbd_ep = 0, kbd_mps = 0;      // boot keyboard (HID protocol 1)
    int alt_if = -1, alt_ep = 0, alt_mps = 0;      // some other HID, not a mouse
    int mouse_if = -1, mouse_ep = 0, mouse_mps = 0; // HID mouse (protocol 2)
    int msc_in = 0, msc_out = 0;                   // mass-storage bulk IN/OUT
    int rn_comm = -1, rn_in = 0, rn_out = 0;       // RNDIS comm interface + data bulk
    int hub_ep = 0, hub_mps = 0, hub_int = 0, is_hub = dev_class == 9;
    int hid_rlen[8] = { 0 };                       // report descriptor length, per interface
    for (int i = 0; i + 2 < total; ) {
        int blen = xfer_buf[i];
        int btype = xfer_buf[i + 1];
        if (blen == 0) break;
        if (btype == 4 && i + 7 < total) {                    // interface
            cur_cls = xfer_buf[i + 5];
            cur_sub = xfer_buf[i + 6];
            cur_proto = xfer_buf[i + 7];
            cur_iface = xfer_buf[i + 2];
            if (cur_cls == 9) is_hub = 1;
            if (pd && pd->nif < 6) { pd->cls[pd->nif] = (uint8_t)cur_cls; pd->proto[pd->nif] = (uint8_t)cur_proto; pd->nif++; }
            // RNDIS communications interface: CDC/ACM (0x02/0x02) with a
            // vendor protocol, or the Wireless RNDIS triple (0xE0/0x01/0x03).
            if ((cur_cls == 0x02 && cur_sub == 0x02) ||
                (cur_cls == 0xE0 && cur_sub == 0x01 && cur_proto == 0x03))
                rn_comm = cur_iface;
        } else if (btype == 0x21 && cur_cls == 3 && blen >= 9 && i + 8 < total) {
            // HID descriptor: the length of the report descriptor, which is
            // fetched separately and says what the reports look like.
            if (xfer_buf[i + 6] == 0x22)
                hid_rlen[cur_iface & 7] = xfer_buf[i + 7] | (xfer_buf[i + 8] << 8);
        } else if (btype == 5 && i + 6 < total) {             // endpoint
            int addr = xfer_buf[i + 2];
            int attr = xfer_buf[i + 3];
            int mps = (xfer_buf[i + 4] | (xfer_buf[i + 5] << 8)) & 0x7FF;
            if (cur_cls == 3 && (attr & 3) == 3 && (addr & 0x80)) {   // HID interrupt IN
                if (cur_proto == 1 && kbd_if < 0) { kbd_if = cur_iface; kbd_ep = addr; kbd_mps = mps; }
                else if (cur_proto == 2 && mouse_if < 0) { mouse_if = cur_iface; mouse_ep = addr; mouse_mps = mps; }
                else if (cur_proto != 2 && alt_if < 0) { alt_if = cur_iface; alt_ep = addr; alt_mps = mps; }
            } else if (cur_cls == 8 && (attr & 3) == 2) {            // mass-storage bulk
                if (addr & 0x80) msc_in = addr; else msc_out = addr;
            } else if (cur_cls == 0x0A && (attr & 3) == 2) {         // CDC data bulk
                if (addr & 0x80) rn_in = addr; else rn_out = addr;
            } else if (cur_cls == 9 && (attr & 3) == 3 && (addr & 0x80) && !hub_ep) {
                hub_ep = addr; hub_mps = mps; hub_int = xfer_buf[i + 6];
            }
        }
        i += blen;
    }

    // Its name, for the news and the device list: "maker product", or the
    // product alone when it already starts with the maker.
    {
        char maker[24], product[48];
        read_string(d, i_product, product, sizeof product);
        read_string(d, i_maker, maker, sizeof maker);
        int k = 0, m = 0;
        while (maker[m] && product[m] && maker[m] == product[m]) m++;
        if (maker[0] && maker[m] != 0) {
            for (int i = 0; maker[i] && k < (int)sizeof u->name - 2; i++) u->name[k++] = maker[i];
            if (product[0]) u->name[k++] = ' ';
        }
        for (int i = 0; product[i] && k < (int)sizeof u->name - 1; i++) u->name[k++] = product[i];
        u->name[k] = 0;
    }

    // Which of its faces to drive: a hub, the network, a disk, a keyboard
    // (and a mouse beside it -- a wireless receiver), a mouse.
    int driven = 0;
    if (is_hub && hub_ep) {
        if (ok(ctrl(d, 0x00, 9, (uint16_t)cfg_val, 0, 0, NULL)) && add_hub(d, hub_ep, hub_mps, hub_int)) {
            stage = "hub"; driven = 1;
        }
    } else if (rn_comm >= 0 && rn_in && rn_out && !rndis_found) {
        // Its configuration's real bConfigurationValue: QEMU usb-net puts
        // RNDIS on value 2 (CDC on 1), and the wrong one lands in a non-RNDIS
        // configuration that STALLs SEND_ENCAPSULATED_COMMAND.
        if (ok(ctrl(d, 0x00, 9, (uint16_t)cfg_val, 0, 0, NULL)) && add_rndis(d, rn_in, rn_out, rn_comm)) {
            u->kind = USB_KIND_NET; stage = "rndis"; driven = 1;
        } else rndis_stage = "set-config";
    } else if (msc_in && msc_out) {
        if (ok(ctrl(d, 0x00, 9, (uint16_t)cfg_val, 0, 0, NULL)) && add_disk(d, msc_in, msc_out)) {
            u->kind = USB_KIND_DISK; stage = "mass-storage"; driven = 1;
        }
    } else if (kbd_if >= 0 || mouse_if >= 0 || (!strict && alt_if >= 0)) {
        if (ok(ctrl(d, 0x00, 9, (uint16_t)cfg_val, 0, 0, NULL))) {
            int kb = 0, ms = 0;
            if (kbd_if >= 0) kb = add_keyboard(d, kbd_if, kbd_ep, kbd_mps);
            else if (mouse_if < 0) kb = add_keyboard(d, alt_if, alt_ep, alt_mps);
            // A combo device: the mouse too, as a second endpoint on the same
            // slot. Logitech's LIGHTSPEED and Unifying receivers present the
            // mouse and a keyboard (the media keys come through it) on one
            // plug, and taking only the keyboard left the mouse dead.
            if (mouse_if >= 0) ms = add_mouse(d, mouse_if, mouse_ep, mouse_mps, hid_rlen[mouse_if & 7]);
            if (kb || ms) {
                u->kind = kb && ms ? USB_KIND_COMBO : kb ? USB_KIND_KBD : USB_KIND_MOUSE;
                stage = kb ? "OK" : "mouse";
                driven = 1;
            }
        }
    } else if (alt_if >= 0) {
        stage = "not a keyboard (proto!=1)";
        u->generic_hid = 1;
    } else {
        stage = "nothing to drive";
    }

    if (!driven) {
        // Kept in the list (and as the owner of its port), but the controller
        // lets go of it.
        disable_slot(d);
        unpage(u->ctx); u->ctx = NULL;
        unpage(u->ep0_trb); u->ep0_trb = NULL;
        u->kind = USB_KIND_OTHER;
    }
    if (pd) pd->stage = stage;
    {
        uint32_t mib = 0;
        if (u->kind == USB_KIND_DISK && !booting) {
            for (int k = 0; k < MAX_MSC; k++) {
                if (!mscs[k].used || mscs[k].dev != d) continue;
                uint32_t blocks, bsize;
                if (usb_disk_capacity(k, &blocks, &bsize))
                    mib = (uint32_t)((uint64_t)blocks * bsize >> 20);
            }
        }
        news_push(1, u->kind, u->name, mib);
    }
    return d;

fail:
    disable_slot(d);
    unpage(u->ctx); u->ctx = NULL;
    unpage(u->ep0_trb); u->ep0_trb = NULL;
    u->used = 0;
fail_early:
    if (pd) pd->stage = stage;
    DBG("xhci: hc %d port %d: %s\n", (int)(h - hcs), port, stage);
    return -1;
}

// Let go of device d and everything behind it. `quiet`: no news (it went with
// the hub above it, which says so).
static void detach(int d, int quiet) {
    struct udev *u = &devs[d];
    if (!u->used) return;
    for (int c = 0; c < MAX_DEV; c++)
        if (devs[c].used && devs[c].parent == d) detach(c, 1);

    for (int k = 0; k < MAX_KBD; k++) {
        struct kbdev *kb = &kbds[k];
        if (!kb->used || kb->dev != d) continue;
        // Whatever it held down comes up: a key held while the plug was
        // pulled would otherwise repeat for ever.
        static const uint8_t modmask[4] = { 0x22, 0x11, 0x44, 0x88 };
        static const uint8_t modcode[4] = { KEY_SHIFT, KEY_CTRL, KEY_ALT, KEY_SUPER };
        for (int i = 0; i < 6; i++) {
            uint8_t code; char ascii;
            if (kb->prev[i] > 1 && hid_translate(kb->prev[i], 0, &code, &ascii))
                keyboard_inject_ex(code, ascii, 0, 0);
        }
        for (int i = 0; i < 4; i++)
            if (kb->prevmod & modmask[i]) keyboard_inject_ex(modcode[i], 0, 0, 0);
        kb->used = 0;
        unpage(kb->trb);
        nkbds--;
    }
    for (int m = 0; m < MAX_MOUSE; m++) {
        struct mousedev *ms = &mice[m];
        if (!ms->used || ms->dev != d) continue;
        mouse_inject(0, 0, 0, 0);                // buttons up
        ms->used = 0;
        unpage(ms->trb);
        nmice--;
    }
    for (int k = 0; k < MAX_MSC; k++) {
        struct msc_dev *m = &mscs[k];
        if (!m->used || m->dev != d) continue;
        m->used = 0;
        m->id = 0;
        unpage(m->in_trb);
        unpage(m->out_trb);
    }
    if (rndis_found && rndis.dev == d) {
        rndis.ready = 0;
        rndis_found = 0;
        rndis_stage = "unplugged";
    }
    if (u->kind == USB_KIND_HUB) unpage(u->hub_trb);
    disable_slot(d);
    unpage(u->ctx);
    unpage(u->ep0_trb);
    DBG("xhci: let go of device %d (%s)\n", d, u->name);
    if (!quiet) news_push(0, u->kind, u->name, 0);
    u->used = 0;
}

// The device on a port: of the controller (parent -1) or of a hub.
static int on_port(struct hc *h, int parent, int port) {
    for (int d = 0; d < MAX_DEV; d++)
        if (devs[d].used && devs[d].hc == h && devs[d].parent == parent && devs[d].port == port)
            return d;
    return -1;
}

// A controller port said something changed.
static void root_change(struct hc *h, int port) {
    uint32_t sc = r32(h->op, OP_PORTSC(port));
    int d = on_port(h, -1, port);
    // Gone, or replaced faster than we looked (connected but no longer
    // enabled): let go of what was there.
    if (d >= 0 && (!(sc & PORTSC_CCS) || !(sc & PORTSC_PED))) { detach(d, 0); d = -1; }
    if (d < 0 && (sc & PORTSC_CCS)) {
        delay_ms(100);                           // let the contacts settle
        if (r32(h->op, OP_PORTSC(port)) & PORTSC_CCS) attach(h, -1, port, strict_now());
    }
}

// A hub port said something changed.
static void hub_change(int hub, int port) {
    uint16_t st, ch;
    if (!hub_port_status(hub, port, &st, &ch)) return;
    hub_port_ack(hub, port, ch);
    int d = on_port(devs[hub].hc, hub, port);
    if (d >= 0 && (!(st & 1) || (ch & 1))) { detach(d, 0); d = -1; }
    if (d < 0 && (st & 1)) {
        delay_ms(100);
        if (hub_port_status(hub, port, &st, &ch) && (st & 1)) attach(devs[hub].hc, hub, port, strict_now());
    }
}

// Endpoints that failed on devices still plugged in: started again. (One
// whose device went is let go of before this, with the device.)
static void restart_endpoints(void) {
    for (int k = 0; k < MAX_KBD; k++) {
        struct kbdev *kb = &kbds[k];
        if (!kb->used || !kb->stalled) continue;
        kb->stalled = 0;
        struct udev *u = &devs[kb->dev];
        ep_recover(kb->dev, kb->dci, &kb->intr, 1);
        arm_intr(u->hc, &kb->intr, kb->buf, kb->len, u->slot, kb->dci);
    }
    for (int m = 0; m < MAX_MOUSE; m++) {
        struct mousedev *ms = &mice[m];
        if (!ms->used || !ms->stalled) continue;
        ms->stalled = 0;
        struct udev *u = &devs[ms->dev];
        ep_recover(ms->dev, ms->dci, &ms->intr, 1);
        arm_intr(u->hc, &ms->intr, ms->buf, ms->len, u->slot, ms->dci);
    }
    for (int d = 0; d < MAX_DEV; d++) {
        struct udev *u = &devs[d];
        if (!u->used || u->kind != USB_KIND_HUB || !u->hub_stalled) continue;
        u->hub_stalled = 0;
        ep_recover(d, u->hub_dci, &u->hub_intr, 1);
        arm_intr(u->hc, &u->hub_intr, (uint8_t *)u->hub_trb + 2048, u->hub_len, u->slot, u->hub_dci);
    }
}

// Everything with news, until nothing has.
static void service_all(void) {
    for (int round = 0; round < 8 && work; round++) {
        work = 0;
        for (int k = 0; k < nhc; k++) {
            struct hc *h = &hcs[k];
            if (!h->up) continue;
            uint64_t p = h->pending;
            h->pending = 0;
            for (int i = 0; i < 64 && p; i++)
                if (p & (1ull << i)) { p &= ~(1ull << i); root_change(h, i); }
        }
        for (int d = 0; d < MAX_DEV; d++) {
            struct udev *u = &devs[d];
            if (!u->used || u->kind != USB_KIND_HUB) continue;
            uint32_t p = u->hub_pending;
            u->hub_pending = 0;
            for (int i = 1; i <= u->nports; i++)
                if (p & (1u << i) && devs[d].used) hub_change(d, i);
        }
        restart_endpoints();
    }
}

void usb_service(void) {
    if (!work || busy || booting) return;
    busy = 1;
    // Waiting on the devices needs the clock; the timer's poll keeps off
    // the rings meanwhile (busy), and the waits here deal with their events.
    uint64_t fl = cpu_flags();
    __asm__ volatile ("sti");
    service_all();
    if (!(fl & 0x200)) __asm__ volatile ("cli");
    busy = 0;
}

// --- start-up ------------------------------------------------------------------------

int xhci_init(void) {
    pci_device_t dev;
    booting = 1;
    busy = 1;
    g_npd = 0;
    for (int n = 0; nhc < MAX_HC && pci_find_class_n(0x0C, 0x03, 0x30, n, &dev); n++) {
        struct hc *h = &hcs[nhc];
        DBG("xhci: controller #%d at %x:%x.%x\n", n, dev.bus, dev.slot, dev.func);
        h->stage = "init failed early";
        nhc++;
        if (!hc_start(h, &dev)) { DBG("xhci: %s\n", h->stage); continue; }
    }
    delay_ms(100);

    // Everything plugged in now, on every controller, hubs and all. A boot
    // keyboard on any port is taken; an HID that is not one is passed over
    // for now (see attach).
    for (int k = 0; k < nhc; k++) {
        struct hc *h = &hcs[k];
        if (!h->up) continue;
        h->conn_mask = 0;
        for (uint32_t p = 0; p < h->max_ports; p++) {
            if (!(r32(h->op, OP_PORTSC(p)) & PORTSC_CCS)) continue;
            if (p < 32) h->conn_mask |= 1u << p;
            DBG("xhci: hc %d: device on port %u\n", k, p);
            attach(h, -1, (int)p, 1);
        }
        h->pending = 0;              // what the ports said about it is handled
    }
    service_all();                   // the hubs' ports
    // No real keyboard anywhere: then an HID that only might be one.
    if (nkbds == 0) {
        for (int d = 0; d < MAX_DEV; d++) {
            struct udev *u = &devs[d];
            if (!u->used || !u->generic_hid) continue;
            struct hc *h = u->hc;
            int parent = u->parent, port = u->port;
            u->used = 0;
            attach(h, parent, port, 0);
        }
    }
    for (int k = 0; k < nhc; k++) {
        struct hc *h = &hcs[k];
        if (h->up) h->stage = nkbds ? "OK" : "devices seen, none a keyboard";
    }
    busy = 0;
    booting = 0;

    if (nkbds > 0 || !nhc) {
        if (nkbds) DBG("xhci: %d keyboard(s) ready\n", nkbds);
        else DBG("xhci: no controller\n");
        return nkbds > 0;
    }

    // No keyboard: a clean, photographable report -- one line per controller
    // and one per device seen -- after wiping the boot log.
    if (fb_available()) fb_console_reset();
    kprintf("\n==== USB xHCI report: %d controller(s) found ====\n", nhc);
    for (int i = 0; i < nhc; i++) {
        struct hc *h = &hcs[i];
        kprintf("  ctrl%d  %x:%x.%x  ports=%d  connected-mask=%x  -> %s\n",
                i, h->bus, h->dev, h->fn, h->max_ports, h->conn_mask, h->stage);
    }
    // Per-device breakdown: speed + class/protocol of each interface
    // (HID class=3; keyboard proto=1, mouse proto=2; hub class=9).
    for (int i = 0; i < g_npd; i++) {
        struct pdiag *p = &g_pd[i];
        kprintf("   hc%d port %d depth %d sp=%d addr=%d if=", p->hc, p->port, p->depth,
                p->speed, p->addressed);
        if (p->nif == 0) kprintf("none");
        for (int k = 0; k < p->nif; k++) kprintf("[c%d/p%d]", p->cls[k], p->proto[k]);
        kprintf(" -> %s\n", p->stage);
    }
    kprintf("  ==> NO keyboard found\n");
    kprintf("=================================================\n");
    return 0;
}

// --- HID boot report -> key events -----------------------------------------

// HID usage id -> ASCII (unshifted, then shifted), for the boot keyboard set.
static char hid_ascii(uint8_t u, int shift) {
    if (u >= 0x04 && u <= 0x1D) {   // a..z
        char base = 'a' + (u - 0x04);
        return shift ? (base - 'a' + 'A') : base;
    }
    if (u >= 0x1E && u <= 0x26) {   // 1..9
        static const char *sh = "!@#$%^&*(";
        return shift ? sh[u - 0x1E] : ('1' + (u - 0x1E));
    }
    if (u == 0x27) return shift ? ')' : '0';
    switch (u) {
        case 0x2C: return ' ';
        case 0x2D: return shift ? '_' : '-';
        case 0x2E: return shift ? '+' : '=';
        case 0x2F: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
    }
    return 0;
}

static int was_down(const uint8_t *prev, uint8_t u) {
    for (int i = 0; i < 6; i++) if (prev[i] == u) return 1;
    return 0;
}

// One HID usage -> our (code, ascii). Returns 0 for usages we do not carry.
//
// Split out of process_report because a key now has to be translated TWICE:
// once when it appears in a report and once when it stops appearing. Two
// copies of this table would drift, and the copy that drifted would be the
// release path -- the one nobody looks at.
static int hid_translate(uint8_t u, int shift, uint8_t *code, char *ascii) {
    *ascii = 0;
    // Special keys FIRST: several of them (Enter 0x28, Backspace 0x2A, Tab
    // 0x2B) fall inside the printable-usage range below, so checking that
    // range first would swallow them.
    switch (u) {
        case 0x28: *code = KEY_ENTER; *ascii = '\n'; return 1;
        case 0x2A: *code = KEY_BKSP;  *ascii = '\b'; return 1;
        case 0x2B: *code = KEY_CHAR;  *ascii = '\t'; return 1;
        case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45:
            *code = (uint8_t)(KEY_F1 + (u - 0x3A)); return 1;
        case 0x4F: *code = KEY_RIGHT; return 1;
        case 0x50: *code = KEY_LEFT;  return 1;
        case 0x51: *code = KEY_DOWN;  return 1;
        case 0x52: *code = KEY_UP;    return 1;
        case 0x4B: *code = KEY_PGUP;  return 1;
        case 0x4E: *code = KEY_PGDN;  return 1;
        case 0x4C: *code = KEY_DEL;   return 1;
        case 0x4A: *code = KEY_HOME;  return 1;
        case 0x4D: *code = KEY_END;   return 1;
        case 0x29: *code = KEY_ESC;   return 1;
        default: break;
    }
    char a = hid_ascii(u, shift);
    if (!a) return 0;
    *code = KEY_CHAR;
    *ascii = a;
    return 1;
}

// Decode one HID boot report `r` from keyboard `kb` (its own prev-key memory, so
// several keyboards don't cancel each other's held keys).
//
// USB has no typematic: a held key is not re-sent, it simply keeps appearing in
// every report until it stops. So a report says WHICH KEYS ARE DOWN, and the
// events are the difference between this report and the last one -- in both
// directions. Reading only the appearances, which is what this did, means a
// program can learn that a key went down and never that it came up.
static void process_report(struct kbdev *kb, const uint8_t *r) {
    uint8_t mod = r[0];
    uint8_t kmods = 0;
    if (mod & 0x22) kmods |= KBD_MOD_SHIFT;   // L/R shift
    if (mod & 0x11) kmods |= KBD_MOD_CTRL;    // L/R ctrl
    if (mod & 0x44) kmods |= KBD_MOD_ALT;     // L/R alt
    if (mod & 0x88) kmods |= KBD_MOD_SUPER;   // L/R gui
    int shift = (kmods & KBD_MOD_SHIFT) != 0;

    // The modifiers arrive as a bitmap rather than as keys, so their edges come
    // from comparing bytes. DOOM's run key is Shift and its fire key is Ctrl,
    // and neither is reachable as a modifier BIT on some other letter.
    static const uint8_t modmask[4] = { 0x22, 0x11, 0x44, 0x88 };
    static const uint8_t modcode[4] = { KEY_SHIFT, KEY_CTRL, KEY_ALT, KEY_SUPER };
    for (int i = 0; i < 4; i++) {
        int now = (mod & modmask[i]) != 0;
        int was = (kb->prevmod & modmask[i]) != 0;
        if (now != was) keyboard_inject_ex(modcode[i], 0, kmods, (uint8_t)now);
    }

    // Gone from the report: came up.
    for (int i = 0; i < 6; i++) {
        uint8_t u = kb->prev[i];
        if (u == 0 || u == 1) continue;       // empty / rollover
        if (was_down(r + 2, u)) continue;     // still held
        uint8_t code; char ascii;
        if (hid_translate(u, shift, &code, &ascii))
            keyboard_inject_ex(code, ascii, kmods, 0);
    }

    // New in the report: went down.
    for (int i = 2; i < 8; i++) {
        uint8_t u = r[i];
        if (u == 0 || u == 1) continue;
        if (was_down(kb->prev, u)) continue;  // already held: not an edge
        uint8_t code; char ascii;
        if (hid_translate(u, shift, &code, &ascii))
            keyboard_inject_ex(code, ascii, kmods, 1);
    }

    for (int i = 0; i < 6; i++) kb->prev[i] = r[i + 2];
    kb->prevmod = mod;
}

// One mouse report, read by the layout its own descriptor gave.
static void process_mouse_report(struct mousedev *ms, const uint8_t *r, int len) {
    int b, dx, dy, wheel;
    if (!hid_mouse_read(&ms->fmt, r, len, &b, &dx, &dy, &wheel)) return;
    uint8_t buttons = 0;
    if (b & 1) buttons |= MOUSE_LEFT;
    if (b & 2) buttons |= MOUSE_RIGHT;
    if (b & 4) buttons |= MOUSE_MIDDLE;
    mouse_inject(buttons, dx, dy, wheel);
}

int xhci_present(void) { return nkbds > 0 || nmice > 0; }

int xhci_keyboard_count(void) { return nkbds; }

int xhci_mouse_present(void) { return nmice > 0; }

int usb_list(struct usb_info *out, int max) {
    int n = 0;
    for (int d = 0; d < MAX_DEV && n < max; d++) {
        struct udev *u = &devs[d];
        if (!u->used) continue;
        out[n].kind = u->kind;
        out[n].speed = u->speed;
        out[n].depth = u->depth;
        out[n].vid = u->vid;
        out[n].pid = u->pid;
        scopy(out[n].name, u->name, sizeof out[n].name);
        n++;
    }
    return n;
}

void xhci_poll(void) {
    // While devices are being set up, the waits there take the events.
    if (busy) return;
    for (int k = 0; k < nhc; k++) {
        struct hc *h = &hcs[k];
        if (!h->up) continue;
        uint32_t ev[4];
        for (int guard = 0; guard < EVT_SIZE && next_event(h, ev); guard++)
            other_event(h, ev);
    }
}

// A transfer event that is a keyboard's or a mouse's report: hand the report
// on and arm the endpoint for the next one. 1 if it was one of theirs.
//
// The event says how many bytes did not arrive, so the report's real length
// is known, and it decides what the report is. A boot keyboard report is
// exactly 8 bytes; a receiver's keyboard interface also sends the 2- and
// 3-byte reports of its media keys, which read as a boot report would be a
// stream of wrong keys. A mouse report is read by the layout its own
// descriptor gave (hid.h).
static int hid_event(struct hc *h, uint32_t slot, uint32_t epid, uint32_t status) {
    uint32_t missing = status & 0xFFFFFF;
    uint32_t cc = TRB_CC(status);
    for (int m = 0; m < MAX_MOUSE; m++) {
        struct mousedev *ms = &mice[m];
        if (!ms->used || devs[ms->dev].hc != h || devs[ms->dev].slot != (int)slot ||
            ms->dci != (int)epid) continue;
        uint32_t got = missing < ms->len ? ms->len - missing : 0;
        if (cc == CC_SUCCESS || cc == CC_SHORT_PKT) {
            if (got > 0) process_mouse_report(ms, ms->buf, (int)got);
            arm_intr(h, &ms->intr, ms->buf, ms->len, (int)slot, ms->dci);
        } else {
            // A failed endpoint is not simply armed again: if the device was
            // pulled out that would fail at once, for ever. usb_service lets
            // go of it, or brings the endpoint back if it is still there.
            ms->stalled = 1;
            work = 1;
        }
        return 1;
    }
    for (int k = 0; k < MAX_KBD; k++) {
        struct kbdev *kb = &kbds[k];
        if (!kb->used || devs[kb->dev].hc != h || devs[kb->dev].slot != (int)slot ||
            kb->dci != (int)epid) continue;
        uint32_t got = missing < kb->len ? kb->len - missing : 0;
        if (cc == CC_SUCCESS || cc == CC_SHORT_PKT) {
            if (got == 8) process_report(kb, kb->buf);
            arm_intr(h, &kb->intr, kb->buf, kb->len, (int)slot, kb->dci);
        } else {
            kb->stalled = 1;
            work = 1;
        }
        return 1;
    }
    return 0;
}

// --- USB Mass Storage: Bulk-Only Transport + SCSI ---------------------------

// One bulk transfer on `dci`, waiting for it to finish. Returns 0 on success
// (SUCCESS or a Short Packet, both fine), -1 otherwise.
//
// A TRB's buffer may not cross a 64 KiB boundary (xHCI 4.11.7.1), so the
// transfer is as many Normal TRBs as it touches such pieces, chained into one
// TD. QEMU does not mind a TRB that crosses one; a real controller may. Each
// TRB says how many packets are left after it (TD Size), and an IN transfer
// asks to hear about a short packet wherever it happens, because the
// controller then skips the rest of the TD -- the last TRB would never
// complete. So the wait is for the next event on this endpoint, not for one
// particular TRB.
static int msc_bulk(int d, int dci, struct ring *r, void *buf, uint32_t len,
                    uint32_t mps, int in) {
    struct udev *u = &devs[d];
    uint64_t b = (uint64_t)(uintptr_t)buf, end = b + len;
    if (!mps) mps = 512;
    do {
        uint64_t edge = (b | 0xFFFFull) + 1;
        uint32_t n = (uint32_t)((edge < end ? edge : end) - b);
        uint32_t left = (uint32_t)(end - b - n);
        uint32_t packets = (left + mps - 1) / mps;
        if (packets > 31) packets = 31;
        int more = left > 0;
        ring_push(r, (uint32_t)b, (uint32_t)(b >> 32), n | (packets << 17),
                  TRB_TYPE(TRB_NORMAL) | (more ? TRB_CHAIN : TRB_IOC) |
                  (in ? TRB_ISP : 0));
        b += n;
    } while (b < end);
    ring_doorbell(u->hc, u->slot, (uint32_t)dci);
    int cc = wait_completion(u->hc, 0, u->slot, dci, NULL);
    if (cc == CC_SUCCESS || cc == CC_SHORT_PKT) return 0;
    // Bring the endpoint back for the next transfer, on both sides: the
    // controller's (halted, or stopped if it never answered) and, after a
    // STALL, the device's own halt (CLEAR_FEATURE ENDPOINT_HALT).
    ep_recover(d, dci, r, cc != -1);
    if (cc == CC_STALL) {
        ctrl(d, 0x02, 1, 0, (uint16_t)((dci / 2) | (in ? 0x80 : 0)), 0, NULL);
        return -2;
    }
    return -1;
}

// One BOT command on device `m`: CBW (bulk OUT) -> optional data -> CSW (bulk
// IN). Returns the SCSI status (0 = good), or -1 on a transport error. Runs with
// interrupts off so the timer's xhci_poll can't drain our completion events off
// the event ring mid-transfer.
static int bot_xfer(struct msc_dev *m, const uint8_t *cdb, int cdb_len,
                    int dir_in, void *data, uint32_t data_len) {
    static uint32_t tag = 0;
    int result = -1;
    uint64_t fl = cpu_flags();
    __asm__ volatile ("cli");
    do {
        for (int i = 0; i < 31; i++) msc_cbw[i] = 0;
        *(uint32_t *)(msc_cbw + 0) = 0x43425355;   // 'USBC'
        *(uint32_t *)(msc_cbw + 4) = ++tag;
        *(uint32_t *)(msc_cbw + 8) = data_len;
        msc_cbw[12] = dir_in ? 0x80 : 0x00;
        msc_cbw[13] = 0;                           // LUN 0
        msc_cbw[14] = (uint8_t)cdb_len;
        for (int i = 0; i < cdb_len && i < 16; i++) msc_cbw[15 + i] = cdb[i];

        if (msc_bulk(m->dev, m->out_dci, &m->out, msc_cbw, 31, m->mps, 0) != 0) break;
        if (data_len) {
            int dci = dir_in ? m->in_dci : m->out_dci;
            struct ring *r = dir_in ? &m->in : &m->out;
            // A device that refuses the data (a STALL) still sends its
            // status, which says why: Bulk-Only Transport goes on to it.
            int rc = msc_bulk(m->dev, dci, r, data, data_len, m->mps, dir_in);
            if (rc == -1) break;
        }
        int rc = msc_bulk(m->dev, m->in_dci, &m->in, msc_csw, 13, m->mps, 1);
        if (rc == -2) rc = msc_bulk(m->dev, m->in_dci, &m->in, msc_csw, 13, m->mps, 1);
        if (rc != 0) break;
        if (*(uint32_t *)(msc_csw + 0) != 0x53425355) break;   // 'USBS'
        result = msc_csw[12];                                  // bCSWStatus
    } while (0);
    if (fl & 0x200) __asm__ volatile ("sti");
    return result;
}

int usb_disk_count(void) {
    int n = 0;
    for (int k = 0; k < MAX_MSC; k++) if (mscs[k].used) n = k + 1;
    return n;
}
int usb_disk_present(void) { return usb_disk_count() > 0; }

uint32_t usb_disk_id(int dev) {
    if (dev < 0 || dev >= MAX_MSC || !mscs[dev].used) return 0;
    return mscs[dev].id;
}

const char *usb_disk_name(int dev) {
    if (dev < 0 || dev >= MAX_MSC || !mscs[dev].used) return "";
    return devs[mscs[dev].dev].name;
}

static struct msc_dev *disk(int dev) {
    if (dev < 0 || dev >= MAX_MSC || !mscs[dev].used) return NULL;
    return &mscs[dev];
}

int usb_disk_capacity(int dev, uint32_t *blocks, uint32_t *bsize) {
    struct msc_dev *m = disk(dev);
    if (!m) return 0;
    uint8_t cdb[10] = { 0x25 };   // READ CAPACITY(10)
    uint8_t cap[8];
    // Sticks often answer UNIT ATTENTION on the first command after power-up;
    // retry a few times.
    for (int t = 0; t < 5; t++) {
        int st = bot_xfer(m, cdb, 10, 1, cap, 8);
        if (st == 0) {
            uint32_t last = ((uint32_t)cap[0]<<24)|((uint32_t)cap[1]<<16)|((uint32_t)cap[2]<<8)|cap[3];
            uint32_t bs   = ((uint32_t)cap[4]<<24)|((uint32_t)cap[5]<<16)|((uint32_t)cap[6]<<8)|cap[7];
            m->blocks = last + 1;
            if (bs == 512 || bs == 1024 || bs == 2048 || bs == 4096) m->bsize = bs;
            if (blocks) *blocks = m->blocks;
            if (bsize)  *bsize  = m->bsize;
            return 1;
        }
        if (!disk(dev)) return 0;                // pulled out meanwhile
    }
    return 0;
}

int usb_disk_read(int dev, uint32_t lba, uint32_t count, void *buf) {
    struct msc_dev *m = disk(dev);
    if (!m) return 0;
    uint8_t cdb[10] = {0};
    cdb[0] = 0x28;                              // READ(10)
    cdb[2] = lba >> 24; cdb[3] = lba >> 16; cdb[4] = lba >> 8; cdb[5] = lba;
    cdb[7] = count >> 8; cdb[8] = count;
    return bot_xfer(m, cdb, 10, 1, buf, count * m->bsize) == 0;
}

int usb_disk_write(int dev, uint32_t lba, uint32_t count, const void *buf) {
    struct msc_dev *m = disk(dev);
    if (!m) return 0;
    uint8_t cdb[10] = {0};
    cdb[0] = 0x2A;                              // WRITE(10)
    cdb[2] = lba >> 24; cdb[3] = lba >> 16; cdb[4] = lba >> 8; cdb[5] = lba;
    cdb[7] = count >> 8; cdb[8] = count;
    return bot_xfer(m, cdb, 10, 0, (void *)buf, count * m->bsize) == 0;
}

// --- RNDIS control protocol (encapsulated messages over EP0) ----------------
static void put32(uint8_t *p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static uint32_t get32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int rndis_cc = 0;   // last control-transfer completion code (diagnostics)

// SEND_ENCAPSULATED_COMMAND (class OUT to the comm interface).
static int rndis_encap_out(int d, const uint8_t *msg, int len) {
    rndis_cc = ctrl(d, 0x21, 0x00, 0, (uint16_t)rndis.comm_if, (uint16_t)len, msg);
    return rndis_cc;
}
// GET_ENCAPSULATED_RESPONSE (class IN); response lands in xfer_buf.
static int rndis_encap_in(int d) {
    rndis_cc = ctrl(d, 0xA1, 0x01, 0, (uint16_t)rndis.comm_if, sizeof(xfer_buf), NULL);
    return (rndis_cc == CC_SUCCESS || rndis_cc == CC_SHORT_PKT) ? 0 : -1;
}
int rndis_last_cc(void) { return rndis_cc; }

static int rndis_control_init(int d) {
    uint8_t m[64];

    // REMOTE_NDIS_INITIALIZE_MSG
    for (int i = 0; i < 64; i++) m[i] = 0;
    put32(m + 0, 0x00000002);      // MessageType
    put32(m + 4, 24);              // MessageLength
    put32(m + 8, 1);               // RequestId
    put32(m + 12, 1);              // MajorVersion
    put32(m + 16, 0);              // MinorVersion
    put32(m + 20, RNDIS_RX_SIZE);  // MaxTransferSize: one receive buffer's worth
    if (rndis_encap_out(d, m, 24) != CC_SUCCESS) { rndis_stage = "init-send"; return 0; }
    delay_ms(2);
    if (rndis_encap_in(d) != 0) { rndis_stage = "init-resp"; return 0; }
    if (get32(xfer_buf) != 0x80000002 || get32(xfer_buf + 12) != 0) {
        rndis_stage = "init-status"; return 0;     // status at +12
    }

    // REMOTE_NDIS_QUERY_MSG for OID_802_3_PERMANENT_ADDRESS
    for (int i = 0; i < 64; i++) m[i] = 0;
    put32(m + 0, 0x00000004);      // MessageType QUERY
    put32(m + 4, 28);              // MessageLength
    put32(m + 8, 2);               // RequestId
    put32(m + 12, 0x01010101);     // OID_802_3_PERMANENT_ADDRESS
    put32(m + 16, 0);              // InformationBufferLength
    put32(m + 20, 0);              // InformationBufferOffset
    put32(m + 24, 0);              // Reserved
    if (rndis_encap_out(d, m, 28) != CC_SUCCESS) { rndis_stage = "query-send"; return 0; }
    delay_ms(2);
    if (rndis_encap_in(d) != 0) { rndis_stage = "query-resp"; return 0; }
    if (get32(xfer_buf) != 0x80000004 || get32(xfer_buf + 12) != 0) {
        rndis_stage = "query-status"; return 0;    // QUERY_CMPLT status at +12
    }
    {
        uint32_t infolen = get32(xfer_buf + 16);
        uint32_t infooff = get32(xfer_buf + 20);   // from the RequestId field (byte 8)
        if (infolen >= 6 && 8 + infooff + 6 <= sizeof(xfer_buf)) {
            const uint8_t *mac = xfer_buf + 8 + infooff;
            for (int i = 0; i < 6; i++) rndis.mac[i] = mac[i];
        }
    }

    // REMOTE_NDIS_SET_MSG: OID_GEN_CURRENT_PACKET_FILTER = directed|multi|bcast
    for (int i = 0; i < 64; i++) m[i] = 0;
    put32(m + 0, 0x00000005);      // MessageType SET
    put32(m + 4, 32);              // MessageLength (28 + 4 data)
    put32(m + 8, 3);               // RequestId
    put32(m + 12, 0x0001010E);     // OID_GEN_CURRENT_PACKET_FILTER
    put32(m + 16, 4);              // InformationBufferLength
    put32(m + 20, 20);             // InformationBufferOffset (from byte 8)
    put32(m + 24, 0);              // Reserved
    put32(m + 28, 0x0000000F);     // DIRECTED|MULTICAST|ALL_MULTICAST|BROADCAST
    if (rndis_encap_out(d, m, 32) != CC_SUCCESS) { rndis_stage = "set-send"; return 0; }
    delay_ms(2);
    if (rndis_encap_in(d) != 0) { rndis_stage = "set-resp"; return 0; }
    if (get32(xfer_buf) != 0x80000005) { rndis_stage = "set-status"; return 0; }

    rndis_stage = "ready";
    return 1;
}

// --- RNDIS network adapter: data path + accessors ---------------------------
int rndis_present(void) { return rndis_found && rndis.ready; }
const uint8_t *rndis_mac(void) { return rndis.mac; }
const char *rndis_status(void) { return rndis_stage; }

__attribute__((aligned(64))) static uint8_t rndis_txbuf[2048];

// Send one Ethernet frame: wrap it in a REMOTE_NDIS_PACKET_MSG and bulk-OUT it.
int rndis_send(const void *frame, int len) {
    if (!rndis_present() || len < 0) return -1;
    if (len > (int)sizeof(rndis_txbuf) - 44) len = sizeof(rndis_txbuf) - 44;
    for (int i = 0; i < 44; i++) rndis_txbuf[i] = 0;
    put32(rndis_txbuf + 0, 0x00000001);       // REMOTE_NDIS_PACKET_MSG
    put32(rndis_txbuf + 4, 44 + len);         // MessageLength
    put32(rndis_txbuf + 8, 36);               // DataOffset (from byte 8)
    put32(rndis_txbuf + 12, len);             // DataLength
    const uint8_t *f = frame;
    for (int i = 0; i < len; i++) rndis_txbuf[44 + i] = f[i];
    uint64_t fl = cpu_flags();
    __asm__ volatile ("cli");
    int r = msc_bulk(rndis.dev, rndis.out_dci, &rndis.out, rndis_txbuf, 44 + len,
                     rndis.mps, 0);
    if (fl & 0x200) __asm__ volatile ("sti");
    return r;
}

// Hand every free receive buffer to the controller, in order.
static void rndis_rx_refill(void) {
    int posted = 0;
    while (rndis_rx_state[rndis_rx_post] == RX_FREE) {
        uint64_t b = (uint64_t)(uintptr_t)rndis_rxbufs[rndis_rx_post];
        rndis_rx_state[rndis_rx_post] = RX_POSTED;
        ring_push(&rndis.in, (uint32_t)b, (uint32_t)(b >> 32), RNDIS_RX_SIZE,
                  TRB_TYPE(TRB_NORMAL) | TRB_IOC);
        rndis_rx_post = (rndis_rx_post + 1) % RNDIS_RX_BUFS;
        posted = 1;
    }
    if (posted) ring_doorbell(devs[rndis.dev].hc, devs[rndis.dev].slot, (uint32_t)rndis.in_dci);
}

// Receive one Ethernet frame, non-blocking: its length, into buf, or 0 when
// nothing has come. One transfer may carry several REMOTE_NDIS_PACKET_MSGs
// back to back, each saying its own length; they are handed out one per call,
// and a buffer goes back to the controller once all of its are.
int rndis_recv(void *buf, int max) {
    if (!rndis_present()) return 0;
    int n = 0;
    uint64_t fl = cpu_flags();
    __asm__ volatile ("cli");

    rndis_rx_refill();

    // Consume the events on the ring: our completions are recorded, and
    // anything else is dealt with -- a keyboard's report thrown away here
    // would leave it silent for good (see wait_completion).
    if (!busy) {
        struct hc *h = devs[rndis.dev].hc;
        uint32_t ev[4];
        for (int g = 0; g < EVT_SIZE && next_event(h, ev); g++) other_event(h, ev);
    }

    while (!n && rndis_rx_state[rndis_rx_read] == RX_FULL) {
        const uint8_t *b = rndis_rxbufs[rndis_rx_read];
        uint32_t got = rndis_rx_got[rndis_rx_read], at = rndis_rx_off;
        if (at + 16 <= got && get32(b + at) == 0x00000001) {   // REMOTE_NDIS_PACKET_MSG
            uint32_t mlen  = get32(b + at + 4);
            uint32_t start = at + 8 + get32(b + at + 8);       // DataOffset, from +8
            uint32_t dlen  = get32(b + at + 12);
            if (dlen > 0 && start + dlen <= got) {
                if ((int)dlen > max) dlen = (uint32_t)max;
                uint8_t *out = buf;
                for (uint32_t i = 0; i < dlen; i++) out[i] = b[start + i];
                n = (int)dlen;
            }
            // A length that cannot be right ends the buffer rather than
            // sending the parse into the middle of a packet.
            rndis_rx_off = (mlen >= 16 && at + mlen > at) ? at + mlen : got;
            if (rndis_rx_off < got) continue;
        }
        // This buffer is done with: back to the controller.
        rndis_rx_state[rndis_rx_read] = RX_FREE;
        rndis_rx_read = (rndis_rx_read + 1) % RNDIS_RX_BUFS;
        rndis_rx_off = 0;
    }

    rndis_rx_refill();
    if (fl & 0x200) __asm__ volatile ("sti");
    return n;
}
