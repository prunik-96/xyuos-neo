#include "nic.h"
#include "e1000.h"
#include "rtl8125.h"
#include "../kernel/kio.h"
#include <stddef.h>

// Drivers, tried in this order: e1000 (QEMU wired dev target), rtl8125 (the
// board's wired NIC), usbnet (RNDIS phone tethering). A wired link is preferred
// over tethering when both are up, hence rtl8125 before usbnet.
extern const nic_driver_t e1000_driver;
extern const nic_driver_t rtl8125_driver;
extern const nic_driver_t usbnet_driver;

static const nic_driver_t *drivers[] = {
    &e1000_driver,
    &rtl8125_driver,
    &usbnet_driver,
};

static const nic_driver_t *bound = NULL;

int nic_init(void) {
    if (bound) return 1;
    // A driver can be PRESENT without a link (e.g. the Realtek NIC with no cable
    // plugged in). Prefer the first present adapter that also has a live link;
    // fall back to the first present one if nothing has link yet (its own link
    // wait then applies). This lets a tethered phone win over an idle wired NIC.
    const nic_driver_t *first_ok = NULL;
    unsigned n = sizeof(drivers) / sizeof(drivers[0]);
    for (unsigned i = 0; i < n; i++) {
        if (!drivers[i]->init()) continue;
        if (!first_ok) first_ok = drivers[i];
        int link = drivers[i]->link ? drivers[i]->link() : 1;
        if (link) {
            bound = drivers[i];
            kprintf("nic: bound %s (link up)\n", bound->name);
            return 1;
        }
    }
    if (first_ok) {
        bound = first_ok;
        kprintf("nic: bound %s\n", bound->name);
        return 1;
    }
    return 0;
}

const uint8_t *nic_mac(void)                 { return bound ? bound->mac() : NULL; }
const char *nic_name(void)                   { return bound ? bound->name : NULL; }
int  nic_send(const void *f, uint16_t len)   { return bound ? bound->send(f, len) : -1; }
void nic_poll(void)                          { if (bound) bound->poll(); }
int  nic_link(void) {
    if (!bound) return 0;
    if (!bound->link) return 1;              // driver reports no link state -> assume up
    return bound->link();
}
