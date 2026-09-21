#ifndef E1000_H
#define E1000_H

#include <stdint.h>

// Intel 82540EM ("e1000" in QEMU). A legacy-descriptor driver: enough to move
// Ethernet frames in and out, which is all the network stack asks of it.

// Bring the card up: find it on PCI, map its BAR, reset, learn the MAC, and arm
// the RX/TX rings. Returns 1 on success, 0 if there is no e1000.
int e1000_init(void);

// The card's MAC address (6 bytes), valid after e1000_init() succeeds.
const uint8_t *e1000_mac(void);

// Queue one Ethernet frame for transmit (blocks until the descriptor is taken).
// Returns 0 on success, -1 on error. `len` is the full frame length.
int e1000_send(const void *frame, uint16_t len);

// Drain the RX ring, handing each received frame to net_rx(). Safe to call from
// the timer IRQ; a no-op if the card was never initialised.
void e1000_poll(void);

#endif
