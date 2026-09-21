#!/usr/bin/env python3
"""Stop the receive poll from spinning without ever leaving the machine.

Measured, not guessed: with a packet capture outside the guest, every TCP
handshake after the first took 490 to 500 milliseconds, and the delay was
entirely on the answering side -- our SYN left on time. The answers landed on
a half-second grid, which is the fallback timeout of the emulator's own event
loop.

The cause is here. e1000_poll reads a descriptor the card filled by DMA,
which is an ordinary memory read: a wait loop calling it runs at sixty
million iterations a second and never touches the device, so the virtual
machine never exits and the emulator's loop -- the thing that actually
carries the packets -- is starved. Reading a device register does exit, and
with one such read the same ten handshakes took 0 ms each and the page
arrived in 0.099s instead of 4.085s.

Doing it on every poll would be the wrong fix. On real hardware that read is
a transaction across the bus, and sixty million of them a second would be a
catastrophe where there was never a problem: a real card has no emulator loop
to starve. So it is done at most every two hundred microseconds, which bounds
it to five thousand a second -- negligible on real hardware, and far below
the half second that was being lost on emulated.
"""
import sys
import os

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))

R = XYUOS + "/"
p = R + "kernel/net/e1000.c"
s = open(p).read()

MARK = "/* See the note on letting the host run, below. */"

# Remove the diagnostic version first if it is still in place.
DIAG = """// --- temporary: does letting the host run fix it ---
void e1000_poll(void) {
    if (!mmio) return;
    (void)reg_read(E1000_STATUS);
    for (;;) {"""
PLAIN = """void e1000_poll(void) {
    if (!mmio) return;
    for (;;) {"""

if MARK in s:
    print("already done")
    sys.exit(0)

if DIAG in s:
    s = s.replace(DIAG, PLAIN, 1)
    print("removed the diagnostic version")

A = """void e1000_poll(void) {
    if (!mmio) return;
    for (;;) {
        rx_desc_t *d = &rx_ring[rx_tail];
        if (!(d->status & RXD_STAT_DD)) break;"""

B = """/* See the note on letting the host run, below. */
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
        if (!(d->status & RXD_STAT_DD)) break;"""

assert A in s, "e1000_poll is not the shape expected"
s = s.replace(A, B, 1)

INC = '#include "../kernel/kio.h"'
if '#include "../arch/x86_64/pit.h"' not in s:
    assert INC in s
    s = s.replace(INC, INC + '\n#include "../arch/x86_64/pit.h"', 1)

open(p, "w").write(s)
print("ok: the poll lets the host run, at most every 200us")
