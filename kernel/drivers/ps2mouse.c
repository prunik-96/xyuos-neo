#include "ps2mouse.h"
#include "mouse.h"
#include "../include/port_io.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/apic.h"
#include "../kernel/kio.h"

/* The i8042 auxiliary port.
 *
 * Everything here is one controller shared with the keyboard, which is the
 * whole difficulty: commands for the mouse are sent by prefixing them with
 * 0xD4 ("the next byte goes to the aux device"), replies come back on the
 * same data port the keyboard uses, and the only thing distinguishing a mouse
 * byte from a key byte is bit 5 of the status register. Every read below
 * therefore checks that bit, and every wait is bounded -- a machine with no
 * PS/2 mouse must not hang here, it must fall through and leave the keyboard
 * exactly as it found it. */

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define ST_OUTPUT_FULL 0x01
#define ST_INPUT_FULL  0x02
#define ST_FROM_AUX    0x20

#define ACK 0xFA

static int present;
static int has_wheel;

/* Packet assembly. A PS/2 mouse streams 3 (or 4 with a wheel) byte packets
 * with no framing of its own, so byte 0 is identified by bit 3, which the
 * protocol guarantees is always set there. */
static unsigned char pkt[4];
static int pkt_idx;

static int wait_writable(void) {
    for (int i = 0; i < 200000; i++)
        if (!(inb(PS2_STATUS) & ST_INPUT_FULL)) return 1;
    return 0;
}

static int wait_readable(void) {
    for (int i = 0; i < 200000; i++)
        if (inb(PS2_STATUS) & ST_OUTPUT_FULL) return 1;
    return 0;
}

static void ctl_cmd(unsigned char c) {
    if (wait_writable()) outb(PS2_CMD, c);
}

static int ctl_data(unsigned char c) {
    if (!wait_writable()) return 0;
    outb(PS2_DATA, c);
    return 1;
}

static int read_data(unsigned char *out) {
    if (!wait_readable()) return 0;
    *out = inb(PS2_DATA);
    return 1;
}

/* Send one command to the mouse and collect its acknowledgement. */
static int aux_cmd(unsigned char c) {
    ctl_cmd(0xD4);
    if (!ctl_data(c)) return 0;
    unsigned char r;
    if (!read_data(&r)) return 0;
    return r == ACK;
}

static int set_rate(unsigned char hz) {
    return aux_cmd(0xF3) && aux_cmd(hz);
}

/* --- the interrupt ------------------------------------------------------- */

void ps2mouse_byte(unsigned char b) {
    /* Resynchronise on the framing bit rather than trusting the stream. One
     * dropped byte would otherwise turn every later packet into nonsense --
     * the pointer flying off to a corner is the classic symptom. */
    if (pkt_idx == 0 && !(b & 0x08)) return;

    pkt[pkt_idx++] = b;
    int want = has_wheel ? 4 : 3;
    if (pkt_idx < want) return;
    pkt_idx = 0;

    unsigned char flags = pkt[0];
    if (flags & 0xC0) return;               /* movement overflowed: drop it */

    /* The sign lives in the flags byte, not in the movement byte: bit 4 for
     * X, bit 5 for Y. Shifting it up to bit 8 turns it into the 256 to
     * subtract, which is exactly two's complement by hand. */
    int dx = (int)pkt[1] - (int)((flags << 4) & 0x100);
    int dy = (int)pkt[2] - (int)((flags << 3) & 0x100);

    int wheel = 0;
    if (has_wheel) {
        int z = pkt[3] & 0x0F;              /* 4-bit signed */
        if (z & 0x08) z -= 16;
        wheel = -z;                         /* PS/2 counts down as positive */
    }

    /* PS/2 Y grows upwards, screen Y grows downwards. */
    mouse_inject(flags & 0x07, dx, -dy, wheel);
}

static void mouse_irq(struct interrupt_frame *frame) {
    (void)frame;
    unsigned char st = inb(PS2_STATUS);
    if (!(st & ST_OUTPUT_FULL)) return;
    /* Not ours: leave the byte in the port for the keyboard's own IRQ rather
     * than consuming it here. */
    if (!(st & ST_FROM_AUX)) return;
    ps2mouse_byte(inb(PS2_DATA));
}

/* --- bring-up ------------------------------------------------------------ */

int ps2mouse_present(void) { return present; }

int ps2mouse_init(void) {
    /* Remember the configuration byte so a machine with no mouse is left
     * untouched -- getting this wrong takes the keyboard down with it. */
    ctl_cmd(0x20);
    unsigned char cfg_saved;
    if (!read_data(&cfg_saved)) return 0;

    ctl_cmd(0xA8);                          /* enable the auxiliary port */

    unsigned char cfg = cfg_saved;
    cfg |= 0x02;                            /* IRQ12 on                    */
    cfg &= (unsigned char)~0x20;            /* aux clock enabled           */
    ctl_cmd(0x60);
    ctl_data(cfg);

    /* Reset. A mouse answers ACK, then 0xAA (self-test passed), then its
     * device id. Anything else means the port is empty. */
    if (!aux_cmd(0xFF)) goto no_mouse;
    unsigned char bat = 0, id = 0;
    if (!read_data(&bat) || bat != 0xAA) goto no_mouse;
    read_data(&id);                         /* 0x00 for a plain mouse */

    aux_cmd(0xF6);                          /* defaults */

    /* The knock that turns a plain mouse into a wheel mouse: three sample
     * rates in this exact order, after which the device reports id 3. */
    if (set_rate(200) && set_rate(100) && set_rate(80)) {
        if (aux_cmd(0xF2)) {
            unsigned char nid = 0;
            if (read_data(&nid) && (nid == 3 || nid == 4)) has_wheel = 1;
        }
    }
    set_rate(100);                          /* 100 reports per second */

    if (!aux_cmd(0xF4)) goto no_mouse;      /* start reporting */

    pkt_idx = 0;
    irq_register_handler(12, mouse_irq);
    /* IRQ12 lives on the SLAVE 8259, which reaches the CPU through the
     * cascade on IRQ2 -- but only when the 8259s are the ones delivering
     * interrupts. With an IO-APIC there is no cascade, and "IRQ2" is not an
     * interrupt at all: firmware routes the timer onto GSI 2 through an ACPI
     * source override, so unmasking it here rewrites the timer's redirection
     * entry to a vector with no handler. The clock stops, and the first
     * driver that waits on it (xHCI, during port power-up) hangs forever. */
    if (!apic_active()) irq_unmask(2);
    irq_unmask(12);

    present = 1;
    mouse_set_present(1);
    kprintf("ps2: mouse ready%s\n", has_wheel ? " (wheel)" : "");
    return 1;

no_mouse:
    ctl_cmd(0x60);
    ctl_data(cfg_saved);
    ctl_cmd(0xA7);                          /* disable the aux port again */
    return 0;
}
