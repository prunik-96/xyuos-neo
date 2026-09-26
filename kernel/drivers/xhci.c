// xHCI driver for a single boot-protocol USB keyboard. See xhci.h.
//
// The controller does DMA to physical addresses. The kernel identity-maps the
// low 4 GiB, so for any static buffer here physical == virtual, and xHCI BARs
// sit below 4 GiB -- no special mapping is needed.

#include "xhci.h"
#include "pci.h"
#include "keyboard.h"
#include "framebuffer.h"
#include "../kernel/kio.h"
#include "mouse.h"
#include "hid.h"
#include "../arch/x86_64/pit.h"
#include "../mm/paging.h"
#include <stdint.h>
#include <stddef.h>

#define XHCI_DEBUG 1
#if XHCI_DEBUG
#define DBG(...) kprintf(__VA_ARGS__)
#else
#define DBG(...)
#endif

// --- MMIO ------------------------------------------------------------------

static volatile uint8_t *mmio;      // capability register base
static volatile uint8_t *op;        // operational registers
static volatile uint8_t *rt;        // runtime registers
static volatile uint32_t *db;       // doorbell array
static uint32_t ctx_size;           // 32 or 64
static uint32_t max_ports;

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
#define PORTSC_PRC (1u << 21)
// All the RW1-to-clear status-change bits (CSC..CEC, bits 17..23). Writing a 1
// to any of them acks/clears it. PED (bit 1) is also RW1C. So a read-modify-
// write of PORTSC must mask ALL of these off, or it silently clears them.
#define PORTSC_RW1C (PORTSC_PED | 0x00FE0000u)

// Runtime: interrupter 0
#define RT_IMAN 0x20
#define RT_IMOD 0x24
#define RT_ERSTSZ 0x28
#define RT_ERSTBA 0x30
#define RT_ERDP 0x38

// --- TRBs and rings --------------------------------------------------------

#define TRB_NORMAL 1
#define TRB_SETUP 2
#define TRB_DATA 3
#define TRB_STATUS 4
#define TRB_LINK 6
#define TRB_ENABLE_SLOT 9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEVICE 11
#define TRB_CONFIGURE_ENDPOINT 12
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
#define CC_SHORT_PKT 13

#define RING_SIZE 64

// Each TRB is 4 dwords. Rings are page-aligned so they never cross a 64 KiB
// boundary (an xHCI requirement) and are identity-mapped for DMA.
struct ring { uint32_t *trb; int enq; int cycle; };

// A board can present several keyboard-class devices (an internal HID gadget AND
// the real USB keyboard). We can't reliably tell which is the one you type on,
// so we configure and poll EVERY keyboard interface, up to MAX_KBD.
#define MAX_KBD 4

__attribute__((aligned(4096))) static uint32_t cmd_ring[RING_SIZE * 4];
__attribute__((aligned(4096))) static uint32_t evt_ring[RING_SIZE * 4];
__attribute__((aligned(4096))) static uint32_t ep0_ring[RING_SIZE * 4];
__attribute__((aligned(4096))) static uint32_t intr_rings[MAX_KBD][RING_SIZE * 4];

__attribute__((aligned(64))) static uint64_t dcbaa[256];
__attribute__((aligned(64))) static uint32_t erst[4];             // one segment
__attribute__((aligned(64))) static uint8_t  dev_ctxs[MAX_KBD][2048]; // per-kbd device ctx
__attribute__((aligned(64))) static uint8_t  in_ctx[2048];        // input context (transient)
__attribute__((aligned(4096))) static uint64_t scratchpad_arr[64];
__attribute__((aligned(4096))) static uint8_t scratchpad_bufs[8][4096];

// Descriptors (transient). 1 KiB for a HID report descriptor, which is often
// longer than the 255 bytes a configuration is read in; aligned so that a
// control transfer into it never crosses a 64 KiB line.
__attribute__((aligned(1024))) static uint8_t xfer_buf[1024];

// Where a HID report lands. A boot report is 8 bytes at most, but the buffer
// must hold a whole packet of the endpoint's size: a device that sends more
// than the transfer asked for is a babble error to the controller, and the
// endpoint stops for good. A wireless receiver's keyboard interface does that
// with its media-key reports.
#define HID_BUF 64
__attribute__((aligned(64))) static uint8_t  report_bufs[MAX_KBD][HID_BUF]; // per-kbd HID report DMA

// One live keyboard: its slot, its interrupt-IN endpoint DCI, its interrupt-ring
// producer state, how much each transfer asks for, and its own key-repeat
// de-dup memory (so two keyboards don't cancel each other's held keys).
struct kbdev {
    int slot;
    int dci;
    struct ring intr;
    uint32_t len;
    uint8_t prev[6];
    uint8_t prevmod;
};
static struct kbdev kbds[MAX_KBD];
static int nkbds;

static struct ring cmd, ep0;
static int evt_deq;
static int evt_cycle;

static int slot_id;        // slot of the device currently being probed
static int kbd_dci;        // device context index of the interrupt IN endpoint
static int kbd_ep_addr;    // endpoint address (e.g. 0x81)
static int kbd_ready;

// Diagnostic breadcrumbs, filled by xhci_try, read by xhci_init to print a
// clean per-controller report on screen (the only way to debug on real HW).
static const char *g_stage;      // where the last try stopped
static int         g_ports_seen; // number of root-hub ports on that controller
static uint32_t    g_conn_mask;  // bitmap of ports that reported a device
static int         e_addressed;  // devices successfully addressed this controller
static int         e_kbd_seen;   // keyboard interfaces found this controller

// Per-port breakdown for the on-screen report: what each connected device is,
// so we can see (on real hardware) exactly what the keyboard presents.
struct pdiag {
    uint8_t port, speed, addressed, nif;
    uint8_t cls[6], proto[6];     // interface class + protocol, first 6 interfaces
    const char *stage;
};
static struct pdiag g_pd[16];
static int g_npd;
static int g_kbd_port = -1;   // port the keyboard was found on, -1 if none

// --- USB Mass Storage: up to MAX_MSC Bulk-Only-Transport disks --------------
// A board can have several (the boot stick AND a data stick), so we configure
// and expose them all, indexed 0..usb_disk_count()-1.
#define MAX_MSC 4
__attribute__((aligned(4096))) static uint32_t msc_in_trbs[MAX_MSC][RING_SIZE * 4];
__attribute__((aligned(4096))) static uint32_t msc_out_trbs[MAX_MSC][RING_SIZE * 4];
__attribute__((aligned(64)))   static uint8_t  msc_dev_ctxs[MAX_MSC][2048];
__attribute__((aligned(64)))   static uint8_t  msc_cbw[31];         // shared (one xfer at a time)
__attribute__((aligned(64)))   static uint8_t  msc_csw[13];
struct msc_dev {
    struct ring in, out;
    int slot, in_dci, out_dci, port;
    uint32_t mps, bsize, blocks;
};
static struct msc_dev mscs[MAX_MSC];
static int nmsc;

static int port_is_msc(int port) {
    for (int i = 0; i < nmsc; i++) if (mscs[i].port == port) return 1;
    return 0;
}

// --- USB RNDIS: one network adapter (phone USB-tethering, or QEMU usb-net) ---
// A CDC-composite: a communications interface (class 0x02 or 0xE0) that takes
// RNDIS control messages as encapsulated commands over EP0, and a CDC-data
// interface (class 0x0A) with a bulk IN + bulk OUT pair carrying framed
// packets. We drive the control handshake during enumeration (while slot_id/ep0
// still point at it) and then the data path is pure bulk, exactly like MSC.
__attribute__((aligned(4096))) static uint32_t rndis_in_trbs[RING_SIZE * 4];
__attribute__((aligned(4096))) static uint32_t rndis_out_trbs[RING_SIZE * 4];
__attribute__((aligned(64)))   static uint8_t  rndis_dev_ctx[2048];
__attribute__((aligned(64)))   static uint8_t  rndis_ctrl[512];     // control OUT data
__attribute__((aligned(64)))   static uint8_t  rndis_rxbuf[2048];   // one RX transfer
struct rndis_dev {
    struct ring in, out;
    int slot, in_dci, out_dci, comm_if, port;
    uint32_t mps;
    int ready;
    uint8_t mac[6];
};
static struct rndis_dev rndis;
static int rndis_found = 0;
static const char *rndis_stage = "none";

static int port_is_rndis(int port) {
    return rndis_found && rndis.port == port;
}

// --- USB HID mouse ----------------------------------------------------------
// Same shape as the keyboard: one interrupt IN endpoint delivering fixed-size
// boot-protocol reports (buttons, dx, dy, wheel). Kept separate from the
// keyboard tables so a machine can have both.
__attribute__((aligned(4096))) static uint32_t mouse_intr_ring[RING_SIZE * 4];
__attribute__((aligned(64)))   static uint8_t  mouse_dev_ctx[2048];
__attribute__((aligned(64)))   static uint8_t  mouse_report[HID_BUF];
static struct {
    struct ring intr;
    int slot, dci, port, ready;
    uint32_t len;
    hid_mouse_fmt fmt;          // where its reports keep what (see hid.h)
} mdev;

static int port_is_mouse(int port) {
    return mdev.ready && mdev.port == port;
}

// RX is non-blocking: one bulk-IN transfer is kept posted, and whichever event
// drainer (wait_event during a TX, xhci_poll from the timer, or rndis_recv's own
// scan) observes its completion records it here. rndis_recv then consumes it.
static volatile int      rndis_rx_done = 0;
static volatile uint32_t rndis_rx_resid = 0;
static int               rndis_rx_posted = 0;

// Called by every event-ring drainer for each transfer event it consumes, so a
// RNDIS bulk-IN completion is never silently dropped no matter who saw it.
static inline void rndis_note_event(uint32_t ctrl, uint32_t status) {
    if (!rndis_found) return;
    if (TRB_TYPE_OF(ctrl) != TRB_TRANSFER_EVENT) return;
    uint32_t slot = (ctrl >> 24) & 0xFF;
    uint32_t ep   = (ctrl >> 16) & 0x1F;
    if ((int)slot == rndis.slot && (int)ep == rndis.in_dci) {
        rndis_rx_resid = status & 0xFFFFFF;
        rndis_rx_done = 1;
    }
}

static void delay_ms(uint32_t ms) {
    uint64_t start = pit_get_ticks();
    // PIT is 100 Hz -> 10 ms/tick. Round up.
    uint64_t want = (ms + 9) / 10;
    if (want == 0) want = 1;
    while (pit_get_ticks() - start < want) { __asm__ volatile ("pause"); }
}

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

static void ring_doorbell(int slot, uint32_t target) {
    db[slot] = target;
    (void)r32(op, OP_USBSTS);   // posting read to flush
}

static int hid_event(uint32_t slot, uint32_t epid, uint32_t status);

// Wait for a completion, returning its completion code, or -1 on timeout.
//
// What is waited for: the completion of the TRB at `expect_trb`; or, when that
// is 0 and `ep_slot` is not, the next transfer event from endpoint `ep_dci` of
// slot `ep_slot` -- a transfer of several TRBs can finish early, at whichever
// of them saw a short packet; or, with both 0, the first completion of any
// kind.
//
// Everything else on the one shared event ring is dealt with on the way,
// never dropped: a keyboard or mouse report is taken and its endpoint armed
// again (hid_event). A disk transfer can take long enough for a key to be
// pressed in the middle of it, and a report thrown away here used to leave
// that keyboard with nothing armed -- silent for good.
static int wait_completion(uint64_t expect_trb, int ep_slot, int ep_dci,
                           uint32_t *out_slot) {
    for (uint32_t spin = 0; spin < 3000000; spin++) {
        uint32_t *e = &evt_ring[evt_deq * 4];
        uint32_t ctrl = e[3];
        if ((ctrl & TRB_CYCLE ? 1 : 0) != evt_cycle) { __asm__ volatile ("pause"); continue; }
        __asm__ volatile ("" ::: "memory");

        uint32_t type = TRB_TYPE_OF(ctrl);
        uint64_t ptr = ((uint64_t)e[1] << 32) | e[0];
        uint32_t status = e[2];
        uint32_t slot = (ctrl >> 24) & 0xFF;
        rndis_note_event(ctrl, status);   // capture a RNDIS RX completion seen here

        // advance dequeue
        evt_deq++;
        if (evt_deq == RING_SIZE) { evt_deq = 0; evt_cycle ^= 1; }
        w64(rt, RT_ERDP, ((uint64_t)(uintptr_t)&evt_ring[evt_deq * 4]) | (1u << 3));

        if (type == TRB_CMD_COMPLETION || type == TRB_TRANSFER_EVENT) {
            uint32_t epid = (ctrl >> 16) & 0x1F;
            int mine = expect_trb ? ptr == expect_trb
                     : ep_slot   ? (type == TRB_TRANSFER_EVENT &&
                                    (int)slot == ep_slot && (int)epid == ep_dci)
                     :             1;
            if (mine) {
                if (out_slot) *out_slot = slot;
                return (int)TRB_CC(status);
            }
            // a completion for something else: a HID report is handled
            if (type == TRB_TRANSFER_EVENT) hid_event(slot, epid, status);
        }
        // port status change etc. -> ignore, keep draining
    }
    return -1;
}

static int wait_event(uint64_t expect_trb, uint32_t *out_slot) {
    return wait_completion(expect_trb, 0, 0, out_slot);
}

// A control transfer on EP0. setup is the 8-byte SETUP packet. If len>0 and
// `in` is true, data is read into xfer_buf; write direction is not needed here.
static int control_in(const uint8_t *setup, int len) {
    uint64_t sp = ((uint64_t)((uint32_t *)setup)[1] << 32) | ((uint32_t *)setup)[0];
    (void)sp;
    uint32_t s0 = ((uint32_t *)setup)[0];
    uint32_t s1 = ((uint32_t *)setup)[1];

    // Setup Stage: immediate data, TRT = IN (3) if len>0 else no-data (0)
    uint32_t trt = (len > 0) ? 3u : 0u;
    ring_push(&ep0, s0, s1, 8 /*transfer length=8*/,
              TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));

    if (len > 0) {
        uint64_t buf = (uint64_t)(uintptr_t)xfer_buf;
        ring_push(&ep0, (uint32_t)buf, (uint32_t)(buf >> 32), (uint32_t)len,
                  TRB_TYPE(TRB_DATA) | (1u << 16) /*DIR=IN*/);
    }

    // Status Stage: opposite direction, IOC
    uint32_t dir = (len > 0) ? 0u : (1u << 16);   // if IN data, status is OUT
    uint64_t last = ring_push(&ep0, 0, 0, 0,
                              TRB_TYPE(TRB_STATUS) | dir | TRB_IOC);

    ring_doorbell(slot_id, 1);   // EP0 doorbell = DCI 1
    return wait_event(last, NULL);
}

// A control transfer with an OUT data stage (or no data). `data`/`len` is the
// payload sent to the device; it is staged through rndis_ctrl (a DMA buffer).
// Used for RNDIS SEND_ENCAPSULATED_COMMAND. Runs on EP0 of the current slot_id.
static int control_out(const uint8_t *setup, const void *data, int len) {
    uint32_t s0 = ((uint32_t *)setup)[0];
    uint32_t s1 = ((uint32_t *)setup)[1];

    uint32_t trt = (len > 0) ? 2u : 0u;             // TRT: OUT data = 2, none = 0
    ring_push(&ep0, s0, s1, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));

    if (len > 0) {
        if (len > (int)sizeof(rndis_ctrl)) len = sizeof(rndis_ctrl);
        for (int i = 0; i < len; i++) rndis_ctrl[i] = ((const uint8_t *)data)[i];
        uint64_t b = (uint64_t)(uintptr_t)rndis_ctrl;
        ring_push(&ep0, (uint32_t)b, (uint32_t)(b >> 32), (uint32_t)len,
                  TRB_TYPE(TRB_DATA) /* DIR = OUT = 0 */);
    }
    // Status stage is always IN for an OUT/no-data control transfer.
    uint64_t last = ring_push(&ep0, 0, 0, 0,
                              TRB_TYPE(TRB_STATUS) | (1u << 16) | TRB_IOC);
    ring_doorbell(slot_id, 1);
    return wait_event(last, NULL);
}

static void make_setup(uint8_t *s, uint8_t type, uint8_t req, uint16_t val,
                       uint16_t idx, uint16_t len) {
    s[0] = type; s[1] = req;
    s[2] = val & 0xFF; s[3] = val >> 8;
    s[4] = idx & 0xFF; s[5] = idx >> 8;
    s[6] = len & 0xFF; s[7] = len >> 8;
}

// --- context helpers -------------------------------------------------------

static uint32_t *slot_ctx_of(uint8_t *base) {
    return (uint32_t *)(base + ctx_size);   // input control ctx is [0], slot [1]
}
static uint32_t *ep_ctx_of(uint8_t *base, int dci) {
    return (uint32_t *)(base + ctx_size * (dci + 1));
}

// --- init ------------------------------------------------------------------

static int reset_controller(void) {
    // halt
    uint32_t cmd = r32(op, OP_USBCMD);
    w32(op, OP_USBCMD, cmd & ~USBCMD_RUN);
    for (int i = 0; i < 100; i++) {
        if (r32(op, OP_USBSTS) & USBSTS_HCH) break;
        delay_ms(1);
    }
    // reset
    w32(op, OP_USBCMD, USBCMD_HCRST);
    for (int i = 0; i < 200; i++) {
        if (!(r32(op, OP_USBCMD) & USBCMD_HCRST) &&
            !(r32(op, OP_USBSTS) & USBSTS_CNR)) return 1;
        delay_ms(1);
    }
    return 0;
}

static void setup_scratchpad(void) {
    uint32_t hcs2 = r32(mmio, CAP_HCSPARAMS2);
    uint32_t hi = (hcs2 >> 21) & 0x1F;
    uint32_t lo = (hcs2 >> 27) & 0x1F;
    uint32_t n = (hi << 5) | lo;
    if (n == 0) { dcbaa[0] = 0; return; }
    if (n > 8) n = 8;
    for (uint32_t i = 0; i < n; i++)
        scratchpad_arr[i] = (uint64_t)(uintptr_t)scratchpad_bufs[i];
    dcbaa[0] = (uint64_t)(uintptr_t)scratchpad_arr;
    DBG("xhci: %u scratchpad buffers\n", n);
}

// Take ownership of the controller from the firmware. On real UEFI hardware the
// BIOS/SMM owns the xHCI (for legacy USB support) and will not let the OS drive
// it until we set the OS-Owned semaphore and it clears BIOS-Owned. We also
// disable the firmware's SMIs so it stops touching the controller behind us.
// QEMU has no such capability, so this is a no-op there. hcc1 = HCCPARAMS1.
static void bios_handoff(uint32_t hcc1) {
    uint32_t off = ((hcc1 >> 16) & 0xFFFF) * 4;   // byte offset of first xECP
    for (int guard = 0; off && guard < 64; guard++) {
        uint32_t cap = r32(mmio, off);
        uint8_t id = cap & 0xFF;
        if (id == 1) {                            // USB Legacy Support capability
            if (cap & (1u << 16)) {               // currently BIOS-owned
                w32(mmio, off, cap | (1u << 24)); // request HC OS Owned
                for (int i = 0; i < 1000; i++) {  // wait up to ~1 s
                    if (!(r32(mmio, off) & (1u << 16))) break;
                    delay_ms(1);
                }
                DBG("xhci: bios handoff legsup=%x\n", r32(mmio, off));
            }
            // USBLEGCTLSTS (off+4): disable all SMI sources, clear SMI status.
            uint32_t ctl = r32(mmio, off + 4);
            ctl &= ~((1u<<0)|(1u<<4)|(1u<<13)|(1u<<14)|(1u<<15));  // SMI enables off
            ctl |=  (1u<<29)|(1u<<30)|(1u<<31)|(1u<<20);           // W1C status bits
            w32(mmio, off + 4, ctl);
            return;
        }
        uint8_t next = (cap >> 8) & 0xFF;
        off = next ? off + next * 4 : 0;
    }
}

// Release a slot back to the controller so its resources are free for the next
// port we probe (a board has several devices; only one is the keyboard).
static void disable_slot(void) {
    if (slot_id <= 0) return;
    ring_push(&cmd, 0, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)slot_id << 24));
    ring_doorbell(0, 0);
    wait_event(0, NULL);      // best-effort; ignore the result
    dcbaa[slot_id] = 0;
    slot_id = 0;
}

// Reset one root-hub port, address the device on it, read its config
// descriptor, and -- only if it presents a HID *keyboard* interface -- fully
// configure its interrupt IN endpoint and arm the first transfer. Returns 1 if
// this port is now a live keyboard; 0 otherwise (any slot it took is released).
// Configure the two bulk endpoints of a Bulk-Only-Transport mass-storage device
// (already reset/addressed by enumerate_port) and register it as the USB disk.
// Its device context must persist (dcbaa keeps pointing at it), so it is copied
// out of the reused scratch context into msc_dev_ctx. Returns 1 on success.
static int configure_msc(int port, uint32_t speed, int in_addr, int out_addr) {
    if (nmsc >= MAX_MSC) return 0;
    int k = nmsc;
    uint8_t setup[8];
    make_setup(setup, 0x00, 9, 1, 0, 0);            // SET_CONFIGURATION(1)
    if (control_in(setup, 0) != CC_SUCCESS) return 0;

    int in_dci  = (in_addr  & 0x0F) * 2 + 1;
    int out_dci = (out_addr & 0x0F) * 2;
    int max_dci = in_dci > out_dci ? in_dci : out_dci;
    uint32_t mps = (speed >= 4) ? 1024 : (speed == 3 ? 512 : 64);

    uint8_t *src = dev_ctxs[nkbds];
    for (int i = 0; i < 2048; i++) msc_dev_ctxs[k][i] = src[i];
    dcbaa[slot_id] = (uint64_t)(uintptr_t)msc_dev_ctxs[k];

    for (int i = 0; i < 2048; i++) in_ctx[i] = 0;
    uint32_t *icc = (uint32_t *)in_ctx;
    icc[1] = (1u << 0) | (1u << in_dci) | (1u << out_dci);
    uint32_t *sctx = slot_ctx_of(in_ctx);
    sctx[0] = ((uint32_t)max_dci << 27) | ((speed & 0xF) << 20);
    sctx[1] = ((uint32_t)(port + 1) << 16);

    ring_init(&mscs[k].in, msc_in_trbs[k]);
    uint32_t *ei = ep_ctx_of(in_ctx, in_dci);
    ei[1] = (6u << 3) | (mps << 16) | (3u << 1);    // Bulk IN, CErr 3
    uint64_t ir = (uint64_t)(uintptr_t)msc_in_trbs[k];
    ei[2] = (uint32_t)ir | 1; ei[3] = (uint32_t)(ir >> 32); ei[4] = mps;

    ring_init(&mscs[k].out, msc_out_trbs[k]);
    uint32_t *eo = ep_ctx_of(in_ctx, out_dci);
    eo[1] = (2u << 3) | (mps << 16) | (3u << 1);    // Bulk OUT, CErr 3
    uint64_t orr = (uint64_t)(uintptr_t)msc_out_trbs[k];
    eo[2] = (uint32_t)orr | 1; eo[3] = (uint32_t)(orr >> 32); eo[4] = mps;

    uint64_t c = ring_push(&cmd, (uint32_t)(uintptr_t)in_ctx,
                  (uint32_t)((uint64_t)(uintptr_t)in_ctx >> 32), 0,
                  TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | ((uint32_t)slot_id << 24));
    ring_doorbell(0, 0);
    if (wait_event(c, NULL) != CC_SUCCESS) return 0;

    mscs[k].slot = slot_id; mscs[k].in_dci = in_dci; mscs[k].out_dci = out_dci;
    mscs[k].mps = mps; mscs[k].port = port; mscs[k].bsize = 512; mscs[k].blocks = 0;
    nmsc++;
    DBG("xhci: mass-storage #%d on port %d slot %d in-dci %d out-dci %d\n",
        k, port, slot_id, in_dci, out_dci);
    return 1;
}

static int rndis_control_init(void);   // fwd (defined after the msg helpers)

// Post one interrupt IN transfer for a HID report of up to `len` bytes.
static void arm_hid(struct ring *r, uint8_t *buf, uint32_t len, int slot, int dci) {
    for (uint32_t i = 0; i < len; i++) buf[i] = 0;
    ring_push(r, (uint32_t)(uintptr_t)buf, (uint32_t)((uint64_t)(uintptr_t)buf >> 32),
              len, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    ring_doorbell(slot, dci);
}

// How much a HID interrupt transfer asks for: the endpoint's packet, which the
// buffer has room for (see HID_BUF).
static uint32_t hid_len(int mps) {
    if (mps <= 0) mps = 8;
    return (uint32_t)(mps < HID_BUF ? mps : HID_BUF);
}

// Add a mouse's interrupt IN endpoint to the slot being probed. `rlen` is the
// length of its HID report descriptor, 0 if it gave none. `keep_dci` is an
// endpoint the slot already has -- the keyboard of a combo device -- or 0:
// the slot context must go on counting it.
static int add_mouse(int port, uint32_t speed, int ep_addr, int iface, int mps,
                     int rlen, int keep_dci) {
    uint8_t setup[8];

    // Where its reports keep the buttons, X, Y and wheel: from its own report
    // descriptor, and then the reports are taken as they are described. Only
    // when that cannot be read is the boot protocol asked for -- a request
    // some devices accept and then ignore (see hid.h).
    int parsed = 0;
    if (rlen > (int)sizeof xfer_buf) rlen = (int)sizeof xfer_buf;
    if (rlen > 0) {
        make_setup(setup, 0x81, 6, 0x2200, (uint16_t)iface, (uint16_t)rlen); // GET_DESCRIPTOR(report)
        if (control_in(setup, rlen) == CC_SUCCESS)
            parsed = hid_parse_mouse(xfer_buf, rlen, &mdev.fmt);
    }
    if (!parsed) hid_mouse_boot(&mdev.fmt);
    make_setup(setup, 0x21, 0x0B, parsed ? 1 : 0, (uint16_t)iface, 0); // SET_PROTOCOL
    control_in(setup, 0);                                 // some devices STALL
    DBG("xhci: mouse reports %s: id %d x %d/%d y %d/%d wheel %d/%d\n",
        parsed ? "as described" : "boot protocol", mdev.fmt.id,
        mdev.fmt.x, mdev.fmt.xs, mdev.fmt.y, mdev.fmt.ys, mdev.fmt.wheel, mdev.fmt.ws);

    int dci = (ep_addr & 0x0F) * 2 + 1;
    int entries = dci > keep_dci ? dci : keep_dci;
    if (mps <= 0) mps = 8;

    for (int i = 0; i < 2048; i++) in_ctx[i] = 0;
    uint32_t *icc = (uint32_t *)in_ctx;
    icc[1] = (1u << 0) | (1u << dci);      // the slot, and this endpoint only
    uint32_t *sctx = slot_ctx_of(in_ctx);
    sctx[0] = ((uint32_t)entries << 27) | ((speed & 0xF) << 20);
    sctx[1] = ((uint32_t)(port + 1) << 16);

    ring_init(&mdev.intr, mouse_intr_ring);
    uint32_t *epc = ep_ctx_of(in_ctx, dci);
    epc[0] = (6u << 16);                            // interval 2^6 * 125us = 8ms
    epc[1] = (7u << 3) | ((uint32_t)mps << 16) | (3u << 1);   // Interrupt IN
    uint64_t itr = (uint64_t)(uintptr_t)mouse_intr_ring;
    epc[2] = (uint32_t)itr | 1;
    epc[3] = (uint32_t)(itr >> 32);
    epc[4] = (uint32_t)mps;

    uint64_t c = ring_push(&cmd, (uint32_t)(uintptr_t)in_ctx,
                  (uint32_t)((uint64_t)(uintptr_t)in_ctx >> 32), 0,
                  TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | ((uint32_t)slot_id << 24));
    ring_doorbell(0, 0);
    if (wait_event(c, NULL) != CC_SUCCESS) return 0;

    mdev.slot = slot_id;
    mdev.dci  = dci;
    mdev.port = port;
    mdev.len  = hid_len(mps);
    mdev.ready = 1;
    mouse_set_present(1);

    arm_hid(&mdev.intr, mouse_report, mdev.len, slot_id, dci);
    DBG("xhci: mouse on port %d slot %d dci %d mps %d\n", port, slot_id, dci, mps);
    return 1;
}

// Configure a device that is only a mouse.
static int configure_mouse(int port, uint32_t speed, int ep_addr, int iface, int mps,
                           int rlen) {
    uint8_t setup[8];
    make_setup(setup, 0x00, 9, 1, 0, 0);                  // SET_CONFIGURATION(1)
    if (control_in(setup, 0) != CC_SUCCESS) return 0;

    // Take a private copy of the device context so the next keyboard probe can
    // reuse the shared scratch, exactly as the mass-storage path does.
    uint8_t *src = dev_ctxs[nkbds];
    for (int i = 0; i < 2048; i++) mouse_dev_ctx[i] = src[i];
    dcbaa[slot_id] = (uint64_t)(uintptr_t)mouse_dev_ctx;

    return add_mouse(port, speed, ep_addr, iface, mps, rlen, 0);
}

// Configure a RNDIS/CDC network adapter: SET_CONFIGURATION, add its bulk IN/OUT
// endpoints, then run the RNDIS control handshake (still on this slot's EP0).
static int configure_rndis(int port, uint32_t speed, int in_addr, int out_addr,
                           int comm_if, int cfg_val) {
    uint8_t setup[8];
    // Use the configuration's real bConfigurationValue: QEMU usb-net puts RNDIS
    // on config value 2 (CDC on 1), and picking the wrong one lands us in a
    // non-RNDIS config that STALLs SEND_ENCAPSULATED_COMMAND.
    make_setup(setup, 0x00, 9, (uint16_t)cfg_val, 0, 0);   // SET_CONFIGURATION
    if (control_in(setup, 0) != CC_SUCCESS) { rndis_stage = "set-config"; return 0; }

    int in_dci  = (in_addr  & 0x0F) * 2 + 1;
    int out_dci = (out_addr & 0x0F) * 2;
    int max_dci = in_dci > out_dci ? in_dci : out_dci;
    uint32_t mps = (speed >= 4) ? 1024 : (speed == 3 ? 512 : 64);

    // Configure the bulk endpoints INTO the live device context
    // (dcbaa[slot_id] still points at dev_ctxs[nkbds]); the RNDIS control
    // handshake below then runs on that same context's EP0. We copy the whole
    // thing to a private buffer only AFTERWARDS, so no stale EP0 dequeue
    // snapshot ever ends up active mid-control-transfer.
    for (int i = 0; i < 2048; i++) in_ctx[i] = 0;
    uint32_t *icc = (uint32_t *)in_ctx;
    icc[1] = (1u << 0) | (1u << in_dci) | (1u << out_dci);
    uint32_t *sctx = slot_ctx_of(in_ctx);
    sctx[0] = ((uint32_t)max_dci << 27) | ((speed & 0xF) << 20);
    sctx[1] = ((uint32_t)(port + 1) << 16);

    ring_init(&rndis.in, rndis_in_trbs);
    uint32_t *ei = ep_ctx_of(in_ctx, in_dci);
    ei[1] = (6u << 3) | (mps << 16) | (3u << 1);    // Bulk IN
    uint64_t ir = (uint64_t)(uintptr_t)rndis_in_trbs;
    ei[2] = (uint32_t)ir | 1; ei[3] = (uint32_t)(ir >> 32); ei[4] = mps;

    ring_init(&rndis.out, rndis_out_trbs);
    uint32_t *eo = ep_ctx_of(in_ctx, out_dci);
    eo[1] = (2u << 3) | (mps << 16) | (3u << 1);    // Bulk OUT
    uint64_t orr = (uint64_t)(uintptr_t)rndis_out_trbs;
    eo[2] = (uint32_t)orr | 1; eo[3] = (uint32_t)(orr >> 32); eo[4] = mps;

    uint64_t c = ring_push(&cmd, (uint32_t)(uintptr_t)in_ctx,
                  (uint32_t)((uint64_t)(uintptr_t)in_ctx >> 32), 0,
                  TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | ((uint32_t)slot_id << 24));
    ring_doorbell(0, 0);
    if (wait_event(c, NULL) != CC_SUCCESS) { rndis_stage = "config-ep"; return 0; }

    rndis.slot = slot_id; rndis.in_dci = in_dci; rndis.out_dci = out_dci;
    rndis.mps = mps; rndis.port = port; rndis.comm_if = comm_if; rndis.ready = 0;
    rndis_found = 1;
    DBG("xhci: rndis on port %d slot %d in-dci %d out-dci %d comm-if %d\n",
        port, slot_id, in_dci, out_dci, comm_if);

    // Run the RNDIS control handshake while EP0 is still on the live context.
    int ok = rndis_control_init();

    // Now snapshot the device context into a private buffer so the next
    // keyboard probe can reuse dev_ctxs[nkbds]. From here on we only touch the
    // bulk endpoints (data path), whose dequeue snapshot is at ring start.
    uint8_t *src = dev_ctxs[nkbds];
    for (int i = 0; i < 2048; i++) rndis_dev_ctx[i] = src[i];
    dcbaa[slot_id] = (uint64_t)(uintptr_t)rndis_dev_ctx;

    if (ok) rndis.ready = 1;
    return 1;
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
static int rndis_encap_out(const uint8_t *msg, int len) {
    uint8_t s[8];
    make_setup(s, 0x21, 0x00, 0, (uint16_t)rndis.comm_if, (uint16_t)len);
    rndis_cc = control_out(s, msg, len);
    return rndis_cc;
}
// GET_ENCAPSULATED_RESPONSE (class IN); response lands in xfer_buf.
static int rndis_encap_in(void) {
    uint8_t s[8];
    make_setup(s, 0xA1, 0x01, 0, (uint16_t)rndis.comm_if, sizeof(xfer_buf));
    rndis_cc = control_in(s, sizeof(xfer_buf));
    return (rndis_cc == CC_SUCCESS || rndis_cc == CC_SHORT_PKT) ? 0 : -1;
}
int rndis_last_cc(void) { return rndis_cc; }

static int rndis_control_init(void) {
    uint8_t m[64];

    // REMOTE_NDIS_INITIALIZE_MSG
    for (int i = 0; i < 64; i++) m[i] = 0;
    put32(m + 0, 0x00000002);      // MessageType
    put32(m + 4, 24);              // MessageLength
    put32(m + 8, 1);               // RequestId
    put32(m + 12, 1);              // MajorVersion
    put32(m + 16, 0);              // MinorVersion
    put32(m + 20, 0x4000);         // MaxTransferSize
    if (rndis_encap_out(m, 24) != CC_SUCCESS) { rndis_stage = "init-send"; return 0; }
    delay_ms(2);
    if (rndis_encap_in() != 0) { rndis_stage = "init-resp"; return 0; }
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
    if (rndis_encap_out(m, 28) != CC_SUCCESS) { rndis_stage = "query-send"; return 0; }
    delay_ms(2);
    if (rndis_encap_in() != 0) { rndis_stage = "query-resp"; return 0; }
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
    if (rndis_encap_out(m, 32) != CC_SUCCESS) { rndis_stage = "set-send"; return 0; }
    delay_ms(2);
    if (rndis_encap_in() != 0) { rndis_stage = "set-resp"; return 0; }
    if (get32(xfer_buf) != 0x80000005) { rndis_stage = "set-status"; return 0; }

    rndis_stage = "ready";
    return 1;
}

static int enumerate_port(int port, int strict) {
    // Don't re-probe a port that already hosts a USB disk or the RNDIS NIC --
    // resetting it would knock that device off its slot.
    if (port_is_msc(port)) return 0;
    if (port_is_rndis(port)) return 0;
    if (port_is_mouse(port)) return 0;

    // strict = accept ONLY a real boot keyboard (HID proto 1). Non-strict also
    // accepts a generic HID (proto 0, not a mouse). We scan all ports strict
    // first so a real keyboard is always preferred over some internal HID gadget
    // that happens to sit on an earlier port. Diagnostics are recorded on the
    // strict pass only, so each port shows once.
    struct pdiag *pd = (strict && g_npd < 16) ? &g_pd[g_npd++] : NULL;
    if (pd) { pd->port = (uint8_t)port; pd->speed = 0; pd->addressed = 0; pd->nif = 0; pd->stage = "?"; }
    int result = 0;
    if (nkbds >= MAX_KBD) { g_stage = "kbd table full"; if (pd) pd->stage = g_stage; return 0; }

    // Reset the port, then wait for it to ENABLE. Every PORTSC write masks off
    // PORTSC_RW1C first: PED (bit 1) is write-1-to-clear and real AMD xHCI sets
    // PED the instant reset completes, so writing the register back verbatim
    // would disable the port we just enabled (QEMU sets PED later, hiding it).
    uint32_t sc = r32(op, OP_PORTSC(port));
    w32(op, OP_PORTSC(port), (sc & ~PORTSC_RW1C) | PORTSC_PP | PORTSC_PR);   // start reset
    for (int i = 0; i < 500; i++) {
        sc = r32(op, OP_PORTSC(port));
        if (sc & PORTSC_PRC) break;
        delay_ms(1);
    }
    w32(op, OP_PORTSC(port), (r32(op, OP_PORTSC(port)) & ~PORTSC_RW1C) | PORTSC_PRC | PORTSC_CSC);
    for (int i = 0; i < 200; i++) {
        sc = r32(op, OP_PORTSC(port));
        if (sc & PORTSC_PED) break;
        delay_ms(1);
    }
    DBG("xhci: port %d after reset sc=%x\n", port, sc);
    if (!(sc & PORTSC_PED)) { g_stage = "port reset/enable failed"; goto done; }
    uint32_t speed = (sc >> 10) & 0xF;
    if (pd) pd->speed = (uint8_t)speed;

    // Enable Slot. NOTE: do NOT re-init the command ring or rewrite CRCR here.
    // The command ring is shared across all ports and set up once in xhci_try;
    // rewriting CRCR mid-flight is illegal (the ring isn't stopped) and desyncs
    // the controller's command dequeue pointer -- on real AMD hardware every
    // Enable Slot after the first then fails, though QEMU tolerates it.
    uint64_t c = ring_push(&cmd, 0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT));
    ring_doorbell(0, 0);
    uint32_t got_slot = 0;
    int cc = wait_event(c, &got_slot);
    if (cc != CC_SUCCESS) { g_stage = "enable-slot failed"; goto done; }
    slot_id = (int)got_slot;
    DBG("xhci: port %d slot %d speed %u\n", port, slot_id, speed);

    // Input context for Address Device: slot + EP0. The device context is the
    // slot we'd COMMIT this keyboard to (dev_ctxs[nkbds]); if it turns out not to
    // be a keyboard we disable the slot and reuse that scratch next probe.
    uint8_t *dev_ctx = dev_ctxs[nkbds];
    for (int i = 0; i < 2048; i++) { in_ctx[i] = 0; dev_ctx[i] = 0; }
    uint32_t *icc = (uint32_t *)in_ctx;
    icc[1] = (1u << 0) | (1u << 1);
    uint32_t *sctx = slot_ctx_of(in_ctx);
    sctx[0] = (1u << 27) | ((speed & 0xF) << 20);
    sctx[1] = ((uint32_t)(port + 1) << 16);

    ring_init(&ep0, ep0_ring);
    uint32_t *ep0c = ep_ctx_of(in_ctx, 1);
    uint32_t mps = (speed >= 4) ? 512 : (speed == 3 ? 64 : (speed == 2 ? 8 : 64));
    ep0c[1] = (4u << 3) | (mps << 16) | (3u << 1);
    uint64_t tr = (uint64_t)(uintptr_t)ep0_ring;
    ep0c[2] = (uint32_t)tr | 1;
    ep0c[3] = (uint32_t)(tr >> 32);
    dcbaa[slot_id] = (uint64_t)(uintptr_t)dev_ctx;

    c = ring_push(&cmd, (uint32_t)(uintptr_t)in_ctx,
                  (uint32_t)((uint64_t)(uintptr_t)in_ctx >> 32), 0,
                  TRB_TYPE(TRB_ADDRESS_DEVICE) | ((uint32_t)slot_id << 24));
    ring_doorbell(0, 0);
    cc = wait_event(c, NULL);
    if (cc != CC_SUCCESS) { g_stage = "address-device failed"; disable_slot(); goto done; }
    e_addressed++;
    if (pd) pd->addressed = 1;

    // For a full-speed device, EP0's real max packet size may be 8 (we guessed
    // 64). Read the first 8 bytes of the DEVICE descriptor to learn it; short
    // packets make this read work regardless, and it lets composite keyboards
    // enumerate reliably on real hardware.
    uint8_t setup[8];
    make_setup(setup, 0x80, 6, 0x0100, 0, 8);       // GET_DESCRIPTOR(device, 8)
    if (control_in(setup, 8) == CC_SUCCESS && speed < 3) {
        uint32_t real_mps = xfer_buf[7];
        if (real_mps == 8 || real_mps == 16 || real_mps == 32 || real_mps == 64) {
            // Evaluate Context to fix EP0 MPS.
            for (int i = 0; i < 2048; i++) in_ctx[i] = 0;
            ((uint32_t *)in_ctx)[1] = (1u << 1);    // A1 (EP0)
            uint32_t *e0 = ep_ctx_of(in_ctx, 1);
            e0[1] = (4u << 3) | (real_mps << 16) | (3u << 1);
            e0[2] = (uint32_t)tr | 1;
            e0[3] = (uint32_t)(tr >> 32);
            c = ring_push(&cmd, (uint32_t)(uintptr_t)in_ctx,
                          (uint32_t)((uint64_t)(uintptr_t)in_ctx >> 32), 0,
                          TRB_TYPE(13 /*EVALUATE_CONTEXT*/) | ((uint32_t)slot_id << 24));
            ring_doorbell(0, 0);
            wait_event(c, NULL);
        }
    }

    // GET_DESCRIPTOR(config, 255).
    make_setup(setup, 0x80, 6, 0x0200, 0, 255);
    cc = control_in(setup, 255);
    if (cc != CC_SUCCESS) { g_stage = "get-descriptor failed"; disable_slot(); goto done; }

    int total = xfer_buf[2] | (xfer_buf[3] << 8);
    if (total > 255) total = 255;
    int cfg_val = xfer_buf[5];   // bConfigurationValue of this configuration

    // Walk the descriptors. Pick the interrupt IN endpoint of a HID keyboard.
    // Prefer a real boot keyboard (class 3, protocol 1); fall back to any HID
    // (class 3) interrupt IN that is NOT a mouse (protocol 2) -- some keyboards
    // report protocol 0. Never take a mouse. Records each interface for the
    // report so we can see what the device actually is.
    int interface = 0;
    int cur_cls = -1, cur_sub = -1, cur_proto = -1, cur_iface = 0;
    int kbd_if = -1, kbd_ep = 0, kbd_mps = 0;      // best: proto 1
    int alt_if = -1, alt_ep = 0, alt_mps = 0;      // fallback: class 3, proto != 2
    int msc_in = 0, msc_out = 0;      // mass-storage bulk IN/OUT endpoint addrs
    int mouse_if = -1, mouse_ep = 0, mouse_mps = 0; // HID mouse (boot protocol 2)
    int rn_comm = -1, rn_in = 0, rn_out = 0;   // RNDIS comm interface + data bulk
    int hid_rlen[8] = { 0 };          // report descriptor length, per interface
    for (int i = 0; i + 2 < total; ) {
        int blen = xfer_buf[i];
        int btype = xfer_buf[i + 1];
        if (blen == 0) break;
        if (btype == 4) {                                     // interface
            cur_cls = xfer_buf[i + 5];
            cur_sub = xfer_buf[i + 6];
            cur_proto = xfer_buf[i + 7];
            cur_iface = xfer_buf[i + 2];
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
        } else if (btype == 5) {                              // endpoint
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
            }
        }
        i += blen;
    }

    // A RNDIS network adapter (USB tethering / usb-net): configure its bulk
    // endpoints, run the control handshake, and keep the slot. Checked before
    // mass-storage since it also uses bulk pairs.
    if (rn_comm >= 0 && rn_in && rn_out && !rndis_found) {
        if (configure_rndis(port, speed, rn_in, rn_out, rn_comm, cfg_val)) { g_stage = "rndis"; goto done; }
    }

    // A mass-storage device (flash drive): configure its bulk endpoints and keep
    // its slot as a USB disk. Not a keyboard, so we return without disabling.
    if (msc_in && msc_out) {
        if (configure_msc(port, speed, msc_in, msc_out)) { g_stage = "mass-storage"; goto done; }
    }

    // A device that is only a pointing device. One that also presents a
    // keyboard is configured as a keyboard below, and its mouse added to the
    // same slot after it.
    if (kbd_if < 0 && mouse_if >= 0 && !mdev.ready) {
        if (configure_mouse(port, speed, mouse_ep, mouse_if, mouse_mps, hid_rlen[mouse_if & 7])) {
            g_stage = "mouse";
            slot_id = 0;            // committed; don't let disable_slot free it
            goto done;
        }
    }
    int kbd_mps_use = 8;
    if (kbd_if >= 0)                 { interface = kbd_if; kbd_ep_addr = kbd_ep; kbd_mps_use = kbd_mps; }
    else if (!strict && alt_if >= 0) { interface = alt_if; kbd_ep_addr = alt_ep; kbd_mps_use = alt_mps; }
    else { kbd_ep_addr = 0; }
    if (kbd_mps_use <= 0) kbd_mps_use = 8;
    if (!kbd_ep_addr) { g_stage = strict ? "not a keyboard (proto!=1)" : "not a keyboard"; disable_slot(); goto done; }
    e_kbd_seen++;
    kbd_dci = (kbd_ep_addr & 0x0F) * 2 + 1;
    DBG("xhci: keyboard iface %d ep %x dci %d\n", interface, kbd_ep_addr, kbd_dci);

    // SET_CONFIGURATION(1).
    make_setup(setup, 0x00, 9, 1, 0, 0);
    cc = control_in(setup, 0);
    if (cc != CC_SUCCESS) { g_stage = "set-configuration failed"; disable_slot(); goto done; }

    // SET_PROTOCOL(boot) on the HID interface (some devices STALL -> ignore).
    make_setup(setup, 0x21, 0x0B, 0, (uint16_t)interface, 0);
    control_in(setup, 0);

    // Configure Endpoint: add the interrupt IN endpoint. Everything the
    // controller keeps DMAing to (interrupt ring, report buffer) is this
    // keyboard's own slot in the arrays, so multiple keyboards don't collide.
    int k = nkbds;
    for (int i = 0; i < 2048; i++) in_ctx[i] = 0;
    icc = (uint32_t *)in_ctx;
    icc[1] = (1u << 0) | (1u << kbd_dci);
    sctx = slot_ctx_of(in_ctx);
    sctx[0] = ((uint32_t)kbd_dci << 27) | ((speed & 0xF) << 20);
    sctx[1] = ((uint32_t)(port + 1) << 16);

    ring_init(&kbds[k].intr, intr_rings[k]);
    uint32_t *epc = ep_ctx_of(in_ctx, kbd_dci);
    uint32_t interval = 6;   // 2^6 * 125us = 8 ms
    epc[0] = (interval << 16);
    epc[1] = (7u << 3) | ((uint32_t)kbd_mps_use << 16) | (3u << 1);   // Interrupt IN, CErr 3
    uint64_t itr = (uint64_t)(uintptr_t)intr_rings[k];
    epc[2] = (uint32_t)itr | 1;
    epc[3] = (uint32_t)(itr >> 32);
    epc[4] = (uint32_t)kbd_mps_use;

    c = ring_push(&cmd, (uint32_t)(uintptr_t)in_ctx,
                  (uint32_t)((uint64_t)(uintptr_t)in_ctx >> 32), 0,
                  TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | ((uint32_t)slot_id << 24));
    ring_doorbell(0, 0);
    cc = wait_event(c, NULL);
    if (cc != CC_SUCCESS) { g_stage = "configure-endpoint failed"; disable_slot(); goto done; }

    // Commit this keyboard and arm its first interrupt IN transfer.
    kbds[k].slot = slot_id;
    kbds[k].dci = kbd_dci;
    kbds[k].len = hid_len(kbd_mps_use);
    for (int i = 0; i < 6; i++) kbds[k].prev[i] = 0;
    kbds[k].prevmod = 0;
    arm_hid(&kbds[k].intr, report_bufs[k], kbds[k].len, slot_id, kbd_dci);
    nkbds++;

    // A combo device: the mouse too, as a second endpoint on the same slot.
    // Wireless receivers are the usual case -- Logitech's LIGHTSPEED and
    // Unifying present the mouse and a keyboard (the media keys come through
    // it) on one plug, and taking only the keyboard left the mouse dead.
    if (mouse_if >= 0 && !mdev.ready &&
        !add_mouse(port, speed, mouse_ep, mouse_if, mouse_mps, hid_rlen[mouse_if & 7], kbd_dci))
        DBG("xhci: port %d: the mouse of this combo device did not configure\n", port);

    slot_id = 0;             // committed; don't let a later disable_slot free it
    g_stage = "OK";
    result = 1;

done:
    if (pd) pd->stage = g_stage;
    return result;
}

static int xhci_try(const pci_device_t *devp) {
    pci_device_t dev = *devp;
    kbd_ready = 0;
    nkbds = 0;
    pci_enable_bus_master(&dev);

    uint64_t bar = (dev.bar[0] & ~0xFULL);
    if (dev.bar[0] & 0x4) bar |= ((uint64_t)dev.bar[1] << 32);  // 64-bit BAR
    DBG("xhci: at %x:%x MMIO=%x:%x\n", dev.bus, dev.slot,
        (uint32_t)(bar >> 32), (uint32_t)bar);

    // The BAR sits far above RAM on real hardware (768 GiB on q35), and the
    // boot page tables only cover the low 4 GiB. Map it before the first
    // register read, or that read triple-faults the machine.
    paging_map_mmio(bar, 0x10000);
    mmio = (volatile uint8_t *)(uintptr_t)bar;

    uint8_t caplen = (uint8_t)(r32(mmio, CAP_CAPLENGTH) & 0xFF);
    op = mmio + caplen;
    rt = mmio + (r32(mmio, CAP_RTSOFF) & ~0x1Fu);
    db = (volatile uint32_t *)(mmio + (r32(mmio, CAP_DBOFF) & ~0x3u));

    uint32_t hcs1 = r32(mmio, CAP_HCSPARAMS1);
    uint32_t max_slots = hcs1 & 0xFF;
    max_ports = (hcs1 >> 24) & 0xFF;
    uint32_t hcc1 = r32(mmio, CAP_HCCPARAMS1);
    ctx_size = (hcc1 & (1u << 2)) ? 64 : 32;
    DBG("xhci: slots=%u ports=%u ctx=%u\n", max_slots, max_ports, ctx_size);

    // Wrest the controller from the firmware before touching it (real HW).
    bios_handoff(hcc1);

    if (!reset_controller()) { g_stage = "controller reset failed"; DBG("xhci: reset failed\n"); return 0; }

    // Program max slots enabled.
    w32(op, OP_CONFIG, max_slots);

    // DCBAA
    for (int i = 0; i < 256; i++) dcbaa[i] = 0;
    setup_scratchpad();
    w64(op, OP_DCBAAP, (uint64_t)(uintptr_t)dcbaa);

    // Command ring
    ring_init(&cmd, cmd_ring);
    w64(op, OP_CRCR, (uint64_t)(uintptr_t)cmd_ring | 1 /*RCS*/);

    // Event ring: one segment, ERST -> evt_ring
    for (int i = 0; i < RING_SIZE * 4; i++) evt_ring[i] = 0;
    evt_deq = 0; evt_cycle = 1;
    erst[0] = (uint32_t)(uintptr_t)evt_ring;
    erst[1] = (uint32_t)((uint64_t)(uintptr_t)evt_ring >> 32);
    erst[2] = RING_SIZE;   // segment size
    erst[3] = 0;
    w32(rt, RT_ERSTSZ, 1);
    w64(rt, RT_ERDP, (uint64_t)(uintptr_t)evt_ring);
    w64(rt, RT_ERSTBA, (uint64_t)(uintptr_t)erst);

    // Run.
    w32(op, OP_USBCMD, USBCMD_RUN);
    for (int i = 0; i < 100; i++) {
        if (!(r32(op, OP_USBSTS) & USBSTS_HCH)) break;
        delay_ms(1);
    }
    DBG("xhci: running, usbsts=%x\n", r32(op, OP_USBSTS));

    // Power and reset ports; find one with a device.
    int port = -1;
    for (uint32_t p = 0; p < max_ports; p++) {
        uint32_t sc = r32(op, OP_PORTSC(p));
        // Turn power on without disturbing PED or acking change bits.
        if (!(sc & PORTSC_PP)) { w32(op, OP_PORTSC(p), (sc & ~PORTSC_RW1C) | PORTSC_PP); delay_ms(20); }
    }
    delay_ms(100);
    // Record EVERY connected port, not just the first -- the mask tells us on
    // real hardware whether the keyboard's port is even seen by this controller.
    g_ports_seen = (int)max_ports;
    g_conn_mask = 0;
    for (uint32_t p = 0; p < max_ports; p++) {
        uint32_t sc = r32(op, OP_PORTSC(p));
        if (sc & PORTSC_CCS) {
            if (p < 32) g_conn_mask |= (1u << p);
            if (port < 0) { port = (int)p; DBG("xhci: device on port %u sc=%x\n", p, sc); }
        }
    }
    if (port < 0) { g_stage = "no device on any port"; DBG("xhci: no device connected\n"); return 0; }

    // A board can have several devices plugged in (mask has many bits set on
    // real hardware: hubs, RGB/keyboard/mouse, internal gadgets). The keyboard
    // is rarely the first, so try EVERY connected port and keep the one that
    // actually presents a HID keyboard interface.
    e_addressed = 0;
    e_kbd_seen = 0;
    // Configure EVERY real boot keyboard (proto 1) on any port -- we can't tell
    // which one you actually type on, so poll them all. Only if none looks like a
    // real keyboard do we fall back to a generic HID (proto != 2, i.e. not a
    // mouse).
    for (uint32_t p = 0; p < max_ports; p++) {
        if (!(g_conn_mask & (1u << p))) continue;
        enumerate_port((int)p, 1);
    }
    if (nkbds == 0) {
        for (uint32_t p = 0; p < max_ports; p++) {
            if (!(g_conn_mask & (1u << p))) continue;
            enumerate_port((int)p, 0);
        }
    }
    if (nkbds > 0) {
        kbd_ready = 1;
        g_stage = "OK";
        g_kbd_port = nkbds;   // report shows the count
        DBG("xhci: %d keyboard(s) ready\n", nkbds);
        return 1;
    }
    if (e_addressed) g_stage = "devices addressed, none a keyboard";
    else             g_stage = "devices seen, none addressed";
    return 0;
}

// Try every xHCI controller in the machine until one yields a working keyboard.
// A B650M-class board has more than one (CPU-side + chipset), and the keyboard
// may be on any of them -- initializing only the first (as before) misses it.
// Records a per-controller verdict and prints a clean, photographable report.
int xhci_init(void) {
    pci_device_t dev;
    int found = 0;
    int result = 0;
    static uint8_t     d_bus[8], d_slot[8], d_func[8], d_ports[8];
    static uint32_t    d_mask[8];
    static const char *d_stage[8];
    g_npd = 0;
    g_kbd_port = -1;

    for (int n = 0; pci_find_class_n(0x0C, 0x03, 0x30, n, &dev); n++) {
        DBG("xhci: trying controller #%d at %x:%x.%x\n", n, dev.bus, dev.slot, dev.func);
        g_stage = "init failed early"; g_ports_seen = 0; g_conn_mask = 0;
        int ok = xhci_try(&dev);
        if (found < 8) {
            d_bus[found] = dev.bus; d_slot[found] = dev.slot; d_func[found] = dev.func;
            d_ports[found] = (uint8_t)g_ports_seen; d_mask[found] = g_conn_mask;
            d_stage[found] = g_stage;
        }
        found++;
        if (ok) { result = 1; break; }
    }

    // On success just return quietly. On FAILURE print a clean, photographable
    // report (one line per controller + a per-device breakdown), after wiping
    // the boot log so only the report is on screen.
    if (result) return 1;
    if (fb_available()) fb_console_reset();
    kprintf("\n==== USB xHCI report: %d controller(s) found ====\n", found);
    for (int i = 0; i < found && i < 8; i++) {
        kprintf("  ctrl%d  %x:%x.%x  ports=%d  connected-mask=%x  -> %s\n",
                i, d_bus[i], d_slot[i], d_func[i], d_ports[i], d_mask[i], d_stage[i]);
    }
    // Per-device breakdown: speed + class/protocol of each interface
    // (HID class=3; keyboard proto=1, mouse proto=2).
    for (int i = 0; i < g_npd; i++) {
        struct pdiag *p = &g_pd[i];
        kprintf("   port %d sp=%d addr=%d if=", p->port, p->speed, p->addressed);
        if (p->nif == 0) kprintf("none");
        for (int k = 0; k < p->nif; k++) kprintf("[c%d/p%d]", p->cls[k], p->proto[k]);
        kprintf(" -> %s\n", p->stage);
    }
    if (result) kprintf("  ==> %d keyboard(s) configured (polling all)\n", g_kbd_port);
    else        kprintf("  ==> NO keyboard found\n");
    kprintf("=================================================\n");
    if (!found) DBG("xhci: no controller\n");
    return result;
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

// One mouse report, read by the layout its own descriptor gave (mdev.fmt).
static void process_mouse_report(const uint8_t *r, int len) {
    int b, dx, dy, wheel;
    if (!hid_mouse_read(&mdev.fmt, r, len, &b, &dx, &dy, &wheel)) return;
    uint8_t buttons = 0;
    if (b & 1) buttons |= MOUSE_LEFT;
    if (b & 2) buttons |= MOUSE_RIGHT;
    if (b & 4) buttons |= MOUSE_MIDDLE;
    mouse_inject(buttons, dx, dy, wheel);
}

int xhci_present(void) { return kbd_ready || mdev.ready; }

int xhci_keyboard_count(void) { return nkbds; }

int xhci_mouse_present(void) { return mdev.ready ? 1 : 0; }

void xhci_poll(void) {
    if (!kbd_ready && !mdev.ready) return;

    // Drain completed interrupt transfers from ANY of the configured keyboards.
    // Each transfer event carries the slot it came from; route it to that
    // keyboard's report buffer + de-dup memory, then re-arm that keyboard.
    for (int guard = 0; guard < RING_SIZE; guard++) {
        uint32_t *e = &evt_ring[evt_deq * 4];
        uint32_t ctrl = e[3];
        if ((ctrl & TRB_CYCLE ? 1 : 0) != evt_cycle) return;   // nothing new
        __asm__ volatile ("" ::: "memory");

        uint32_t type = TRB_TYPE_OF(ctrl);
        uint32_t slot = (ctrl >> 24) & 0xFF;
        uint32_t status = e[2];
        evt_deq++;
        if (evt_deq == RING_SIZE) { evt_deq = 0; evt_cycle ^= 1; }
        w64(rt, RT_ERDP, ((uint64_t)(uintptr_t)&evt_ring[evt_deq * 4]) | (1u << 3));

        if (type != TRB_TRANSFER_EVENT) continue;
        rndis_note_event(ctrl, status);   // don't drop a RNDIS RX completion
        hid_event(slot, (ctrl >> 16) & 0x1F, status);
    }
}

// A transfer event that is a keyboard's or the mouse's report: hand the report
// on and arm the endpoint for the next one. 1 if it was one of theirs. Called
// from xhci_poll, and from wait_completion for events that arrive while it is
// waiting for something else.
//
// The event says how many bytes did not arrive, so the report's real length
// is known, and it decides what the report is. A boot keyboard report is
// exactly 8 bytes; a receiver's keyboard interface also sends the 2- and
// 3-byte reports of its media keys, which read as a boot report would be a
// stream of wrong keys. A mouse report is read by the layout its own
// descriptor gave (hid.h).
static int hid_event(uint32_t slot, uint32_t epid, uint32_t status) {
    uint32_t missing = status & 0xFFFFFF;

    if (mdev.ready && mdev.slot == (int)slot && mdev.dci == (int)epid) {
        uint32_t got = missing < mdev.len ? mdev.len - missing : 0;
        if (got > 0) process_mouse_report(mouse_report, (int)got);
        arm_hid(&mdev.intr, mouse_report, mdev.len, mdev.slot, mdev.dci);
        return 1;
    }

    for (int i = 0; i < nkbds; i++) {
        if (kbds[i].slot != (int)slot || kbds[i].dci != (int)epid) continue;
        uint32_t got = missing < kbds[i].len ? kbds[i].len - missing : 0;
        if (got == 8) process_report(&kbds[i], report_bufs[i]);
        arm_hid(&kbds[i].intr, report_bufs[i], kbds[i].len, kbds[i].slot, kbds[i].dci);
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
static int msc_bulk(int slot, int dci, struct ring *r, void *buf, uint32_t len,
                    uint32_t mps, int in) {
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
    ring_doorbell(slot, dci);
    int cc = wait_completion(0, slot, dci, NULL);
    return (cc == CC_SUCCESS || cc == CC_SHORT_PKT) ? 0 : -1;
}

// One BOT command on device `d`: CBW (bulk OUT) -> optional data -> CSW (bulk
// IN). Returns the SCSI status (0 = good), or -1 on a transport error. Runs with
// interrupts off so the timer's xhci_poll can't drain our completion events off
// the shared event ring mid-transfer.
static int bot_xfer(struct msc_dev *d, const uint8_t *cdb, int cdb_len,
                    int dir_in, void *data, uint32_t data_len) {
    static uint32_t tag = 0;
    int result = -1;
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

        if (msc_bulk(d->slot, d->out_dci, &d->out, msc_cbw, 31, d->mps, 0) != 0) break;
        if (data_len) {
            int dci = dir_in ? d->in_dci : d->out_dci;
            struct ring *r = dir_in ? &d->in : &d->out;
            if (msc_bulk(d->slot, dci, r, data, data_len, d->mps, dir_in) != 0) break;
        }
        if (msc_bulk(d->slot, d->in_dci, &d->in, msc_csw, 13, d->mps, 1) != 0) break;
        if (*(uint32_t *)(msc_csw + 0) != 0x53425355) break;   // 'USBS'
        result = msc_csw[12];                                  // bCSWStatus
    } while (0);
    __asm__ volatile ("sti");
    return result;
}

int usb_disk_count(void) { return nmsc; }
int usb_disk_present(void) { return nmsc > 0; }

int usb_disk_capacity(int dev, uint32_t *blocks, uint32_t *bsize) {
    if (dev < 0 || dev >= nmsc) return 0;
    struct msc_dev *d = &mscs[dev];
    uint8_t cdb[10] = { 0x25 };   // READ CAPACITY(10)
    uint8_t cap[8];
    // Sticks often answer UNIT ATTENTION on the first command after power-up;
    // retry a few times.
    for (int t = 0; t < 5; t++) {
        int st = bot_xfer(d, cdb, 10, 1, cap, 8);
        if (st == 0) {
            uint32_t last = ((uint32_t)cap[0]<<24)|((uint32_t)cap[1]<<16)|((uint32_t)cap[2]<<8)|cap[3];
            uint32_t bs   = ((uint32_t)cap[4]<<24)|((uint32_t)cap[5]<<16)|((uint32_t)cap[6]<<8)|cap[7];
            d->blocks = last + 1;
            if (bs == 512 || bs == 1024 || bs == 2048 || bs == 4096) d->bsize = bs;
            if (blocks) *blocks = d->blocks;
            if (bsize)  *bsize  = d->bsize;
            return 1;
        }
    }
    return 0;
}

int usb_disk_read(int dev, uint32_t lba, uint32_t count, void *buf) {
    if (dev < 0 || dev >= nmsc) return 0;
    struct msc_dev *d = &mscs[dev];
    uint8_t cdb[10] = {0};
    cdb[0] = 0x28;                              // READ(10)
    cdb[2] = lba >> 24; cdb[3] = lba >> 16; cdb[4] = lba >> 8; cdb[5] = lba;
    cdb[7] = count >> 8; cdb[8] = count;
    return bot_xfer(d, cdb, 10, 1, buf, count * d->bsize) == 0;
}

int usb_disk_write(int dev, uint32_t lba, uint32_t count, const void *buf) {
    if (dev < 0 || dev >= nmsc) return 0;
    struct msc_dev *d = &mscs[dev];
    uint8_t cdb[10] = {0};
    cdb[0] = 0x2A;                              // WRITE(10)
    cdb[2] = lba >> 24; cdb[3] = lba >> 16; cdb[4] = lba >> 8; cdb[5] = lba;
    cdb[7] = count >> 8; cdb[8] = count;
    return bot_xfer(d, cdb, 10, 0, (void *)buf, count * d->bsize) == 0;
}

// --- RNDIS network adapter: data path + accessors ---------------------------
int rndis_present(void) { return rndis_found && rndis.ready; }
const uint8_t *rndis_mac(void) { return rndis.mac; }
const char *rndis_status(void) { return rndis_stage; }

__attribute__((aligned(64))) static uint8_t rndis_txbuf[2048];

// Send one Ethernet frame: wrap it in a REMOTE_NDIS_PACKET_MSG and bulk-OUT it.
int rndis_send(const void *frame, int len) {
    if (!rndis.ready || len < 0) return -1;
    if (len > (int)sizeof(rndis_txbuf) - 44) len = sizeof(rndis_txbuf) - 44;
    for (int i = 0; i < 44; i++) rndis_txbuf[i] = 0;
    put32(rndis_txbuf + 0, 0x00000001);       // REMOTE_NDIS_PACKET_MSG
    put32(rndis_txbuf + 4, 44 + len);         // MessageLength
    put32(rndis_txbuf + 8, 36);               // DataOffset (from byte 8)
    put32(rndis_txbuf + 12, len);             // DataLength
    const uint8_t *f = frame;
    for (int i = 0; i < len; i++) rndis_txbuf[44 + i] = f[i];
    __asm__ volatile ("cli");
    int r = msc_bulk(rndis.slot, rndis.out_dci, &rndis.out, rndis_txbuf, 44 + len,
                     rndis.mps, 0);
    __asm__ volatile ("sti");
    return r;
}

// Receive one framed packet, non-blocking. Keeps exactly one bulk-IN transfer
// posted; returns the Ethernet frame length into buf when it completes, else 0.
// The completion may be observed here or by any other event drainer (see
// rndis_note_event), so nothing is lost while a TX or the timer runs.
int rndis_recv(void *buf, int max) {
    if (!rndis.ready) return 0;
    int n = 0;
    __asm__ volatile ("cli");

    if (!rndis_rx_posted) {
        uint64_t b = (uint64_t)(uintptr_t)rndis_rxbuf;
        ring_push(&rndis.in, (uint32_t)b, (uint32_t)(b >> 32),
                  sizeof(rndis_rxbuf), TRB_TYPE(TRB_NORMAL) | TRB_IOC);
        ring_doorbell(rndis.slot, rndis.in_dci);
        rndis_rx_posted = 1;
    }

    // Consume any events currently on the ring (records our RX completion).
    for (int g = 0; g < RING_SIZE; g++) {
        uint32_t *e = &evt_ring[evt_deq * 4];
        uint32_t ctrl = e[3];
        if ((ctrl & TRB_CYCLE ? 1 : 0) != evt_cycle) break;
        __asm__ volatile ("" ::: "memory");
        uint32_t status = e[2];
        evt_deq++;
        if (evt_deq == RING_SIZE) { evt_deq = 0; evt_cycle ^= 1; }
        w64(rt, RT_ERDP, ((uint64_t)(uintptr_t)&evt_ring[evt_deq * 4]) | (1u << 3));
        rndis_note_event(ctrl, status);
    }

    if (rndis_rx_done) {
        rndis_rx_done = 0;
        rndis_rx_posted = 0;
        uint32_t got = sizeof(rndis_rxbuf) - rndis_rx_resid;
        if (get32(rndis_rxbuf) == 0x00000001) {          // REMOTE_NDIS_PACKET_MSG
            uint32_t doff = get32(rndis_rxbuf + 8);
            uint32_t dlen = get32(rndis_rxbuf + 12);
            uint32_t start = 8 + doff;
            if (dlen > 0 && start + dlen <= got && start + dlen <= sizeof(rndis_rxbuf)) {
                if ((int)dlen > max) dlen = max;
                uint8_t *out = buf;
                for (uint32_t i = 0; i < dlen; i++) out[i] = rndis_rxbuf[start + i];
                n = (int)dlen;
            }
        }
    }
    __asm__ volatile ("sti");
    return n;
}
