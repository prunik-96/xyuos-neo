#!/usr/bin/env python3
"""Test one hypothesis: the guest never lets the host run.

e1000_poll reads a descriptor out of ordinary memory, which the card filled
by DMA. That costs nothing and touches no device -- so a tight loop calling
it spins the processor at full speed without ever leaving the virtual
machine. QEMU's own loop, which is what carries the packets, then only gets
to run when something else forces it to, and its fallback timeout is half a
second. That is the half second.

Reading a device register instead is an access to the card, which does leave
the virtual machine. If the delay collapses, the hypothesis is right and the
real fix is to stop spinning; if it does not, the hypothesis is wrong and
this comes straight back out.

Run with `off` to remove it.
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

MARK = "// --- temporary: does letting the host run fix it ---"

A = """void e1000_poll(void) {
    if (!mmio) return;
    for (;;) {
        rx_desc_t *d = &rx_ring[rx_tail];
        if (!(d->status & RXD_STAT_DD)) break;"""

B = MARK + """
void e1000_poll(void) {
    if (!mmio) return;
    (void)reg_read(E1000_STATUS);
    for (;;) {
        rx_desc_t *d = &rx_ring[rx_tail];
        if (!(d->status & RXD_STAT_DD)) break;"""

if len(sys.argv) > 1 and sys.argv[1] == "off":
    if MARK not in s:
        print("already off")
        sys.exit(0)
    open(p, "w").write(s.replace(B, A, 1))
    print("removed")
else:
    if MARK in s:
        print("already on")
        sys.exit(0)
    assert A in s, "e1000_poll is not the shape expected"
    open(p, "w").write(s.replace(A, B, 1))
    print("added: one register read per poll")
