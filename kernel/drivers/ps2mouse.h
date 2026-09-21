#ifndef PS2MOUSE_H
#define PS2MOUSE_H

// The PS/2 (i8042 auxiliary port) mouse.
//
// The USB HID driver covers real hardware, where the pointer is on a USB
// port. This covers everything else: a virtual machine started without an
// explicit USB tablet or mouse presents an emulated PS/2 pair, and a laptop
// touchpad is usually PS/2 behind the scenes too. Both feed the same
// driver-agnostic pointer state in mouse.c, so the compositor never has to
// know which one moved.
//
// Returns 1 if a mouse answered, 0 if the port is empty (in which case the
// controller is left exactly as it was found, so the keyboard keeps working).
int ps2mouse_init(void);

// 1 once a PS/2 mouse has been found and enabled.
int ps2mouse_present(void);

// Feed one byte that arrived on the auxiliary port. Exposed so the keyboard
// IRQ can hand over a byte it finds is not its own.
void ps2mouse_byte(unsigned char b);

#endif
