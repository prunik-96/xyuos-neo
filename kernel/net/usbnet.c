#include "nic.h"
#include "net.h"
#include "../drivers/xhci.h"
#include <stddef.h>

// USB-Ethernet (RNDIS) as a NIC driver. The heavy lifting -- enumeration, the
// RNDIS control handshake, and the bulk data path -- lives in the xHCI driver
// (kernel/drivers/xhci.c), configured during xhci_init. This is just the thin
// nic_driver_t wrapper that lets net.c treat a tethered phone like any card.

static int usbnet_init(void) {
    return rndis_present();      // already brought up during xhci_init
}
static const uint8_t *usbnet_mac(void) {
    return rndis_mac();
}
static int usbnet_send(const void *frame, uint16_t len) {
    return rndis_send(frame, len);
}
static void usbnet_poll(void) {
    uint8_t buf[1600];
    int n;
    // Drain whatever framed packets are queued this tick.
    while ((n = rndis_recv(buf, sizeof(buf))) > 0)
        net_rx(buf, (uint16_t)n);
}
static int usbnet_link(void) {
    return rndis_present();      // RNDIS has no separate link signal we poll
}

const nic_driver_t usbnet_driver = {
    .name = "usbnet",
    .init = usbnet_init,
    .mac  = usbnet_mac,
    .send = usbnet_send,
    .poll = usbnet_poll,
    .link = usbnet_link,
};
