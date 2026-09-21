#ifndef NIC_H
#define NIC_H

#include <stdint.h>

// A network card, reduced to the four things the stack needs. Each driver
// exports one of these; nic_init() probes them in order and binds the first
// card that is present, so net.c never names a specific chip.
typedef struct {
    const char *name;
    int  (*init)(void);                       // 1 if this card is present + up
    const uint8_t *(*mac)(void);              // 6-byte MAC
    int  (*send)(const void *frame, uint16_t len);
    void (*poll)(void);                       // drain RX -> net_rx()
    int  (*link)(void);                       // 1 if the link is up (NULL = assume up)
} nic_driver_t;

// Probe every known driver; bind the first that initialises. Returns 1 on
// success. Idempotent.
int nic_init(void);
const uint8_t *nic_mac(void);
const char *nic_name(void);           // bound driver's name, NULL if none
int  nic_send(const void *frame, uint16_t len);
void nic_poll(void);
int  nic_link(void);                          // 1 if link up (or driver has no link check)

#endif
