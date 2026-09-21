#ifndef PCI_H
#define PCI_H

#include <stdint.h>

typedef struct {
    uint8_t bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t class_code, subclass, prog_if;   // what the device says it is
    uint32_t bar[6];
} pci_device_t;

int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out);

// The n-th device on the bus, in scan order, whatever it is. For enumerating
// the machine rather than looking for one particular chip. Returns 0 once n
// is past the last device.
int pci_device_n(int n, pci_device_t *out);
// Find the first device matching a class/subclass/prog-if triple (e.g. XHCI is
// 0x0C / 0x03 / 0x30). Scans all functions of all slots.
int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                   pci_device_t *out);
// Find the n-th (0-based) device matching a class/subclass/prog-if triple.
// Lets a driver walk every controller of a kind (a board can have several
// xHCI controllers, and the keyboard may be on any of them). Returns 1 if the
// n-th match exists, 0 once n is past the last one.
int pci_find_class_n(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                     int n, pci_device_t *out);
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

// Enable memory-space decoding + bus mastering in the command register, which
// a DMA device (XHCI) needs before it can touch its MMIO or main memory.
void pci_enable_bus_master(const pci_device_t *dev);

#endif
