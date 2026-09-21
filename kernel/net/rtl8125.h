#ifndef RTL8125_H
#define RTL8125_H

#include <stdint.h>

// Realtek RTL8125 2.5GbE (PCI 10EC:8125) -- the NIC on the target board.
// A descriptor-ring driver in the RTL8169 family's basic mode (the RTL8125 is
// backward compatible at the descriptor level for plain TX/RX).
//
// NOTE: QEMU has no RTL8125 model, so this path is exercised only on real
// hardware; keep every hardware wait bounded so a wrong assumption degrades to
// "no NIC" instead of a hang.

int rtl8125_init(void);
const uint8_t *rtl8125_mac(void);
int rtl8125_send(const void *frame, uint16_t len);
void rtl8125_poll(void);

#endif
