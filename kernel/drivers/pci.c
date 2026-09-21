#include "pci.h"
#include "../include/port_io.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t pci_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return 0x80000000u
        | ((uint32_t)bus << 16)
        | ((uint32_t)slot << 11)
        | ((uint32_t)func << 8)
        | (offset & 0xFC);
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

static void fill_device(pci_device_t *out, uint32_t bus, uint32_t slot, uint32_t func,
                        uint16_t vid, uint16_t did) {
    out->bus = (uint8_t)bus;
    out->slot = (uint8_t)slot;
    out->func = (uint8_t)func;
    out->vendor_id = vid;
    out->device_id = did;
    {
        uint32_t cls = pci_config_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x08);
        out->class_code = (uint8_t)((cls >> 24) & 0xFF);
        out->subclass   = (uint8_t)((cls >> 16) & 0xFF);
        out->prog_if    = (uint8_t)((cls >> 8)  & 0xFF);
    }
    for (int b = 0; b < 6; b++) {
        out->bar[b] = pci_config_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x10 + b * 4);
    }
}

int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out) {
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            uint32_t id = pci_config_read32((uint8_t)bus, (uint8_t)slot, 0, 0x00);
            uint16_t vid = id & 0xFFFF;
            if (vid == 0xFFFF) continue;
            uint16_t did = (id >> 16) & 0xFFFF;
            if (vid == vendor_id && did == device_id) {
                fill_device(out, bus, slot, 0, vid, did);
                return 1;
            }
        }
    }
    return 0;
}

int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                   pci_device_t *out) {
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            uint32_t id0 = pci_config_read32((uint8_t)bus, (uint8_t)slot, 0, 0x00);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            // Bit 23:16 of the header-type dword marks a multi-function device.
            uint32_t hdr = pci_config_read32((uint8_t)bus, (uint8_t)slot, 0, 0x0C);
            int nfuncs = (hdr & 0x00800000) ? 8 : 1;
            for (int func = 0; func < nfuncs; func++) {
                uint32_t id = pci_config_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x00);
                uint16_t vid = id & 0xFFFF;
                if (vid == 0xFFFF) continue;
                uint32_t cls = pci_config_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x08);
                uint8_t c  = (cls >> 24) & 0xFF;
                uint8_t sc = (cls >> 16) & 0xFF;
                uint8_t pi = (cls >> 8)  & 0xFF;
                if (c == class_code && sc == subclass && pi == prog_if) {
                    fill_device(out, bus, slot, (uint32_t)func, vid, (id >> 16) & 0xFFFF);
                    return 1;
                }
            }
        }
    }
    return 0;
}

int pci_device_n(int n, pci_device_t *out) {
    int seen = 0;
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            uint32_t id0 = pci_config_read32((uint8_t)bus, (uint8_t)slot, 0, 0x00);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            uint32_t hdr = pci_config_read32((uint8_t)bus, (uint8_t)slot, 0, 0x0C);
            int nfuncs = (hdr & 0x00800000) ? 8 : 1;
            for (int func = 0; func < nfuncs; func++) {
                uint32_t id = pci_config_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x00);
                uint16_t vid = id & 0xFFFF;
                if (vid == 0xFFFF) continue;
                if (seen == n) {
                    fill_device(out, bus, slot, (uint32_t)func, vid, (id >> 16) & 0xFFFF);
                    return 1;
                }
                seen++;
            }
        }
    }
    return 0;
}

int pci_find_class_n(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                     int n, pci_device_t *out) {
    int seen = 0;
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            uint32_t id0 = pci_config_read32((uint8_t)bus, (uint8_t)slot, 0, 0x00);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            uint32_t hdr = pci_config_read32((uint8_t)bus, (uint8_t)slot, 0, 0x0C);
            int nfuncs = (hdr & 0x00800000) ? 8 : 1;
            for (int func = 0; func < nfuncs; func++) {
                uint32_t id = pci_config_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x00);
                uint16_t vid = id & 0xFFFF;
                if (vid == 0xFFFF) continue;
                uint32_t cls = pci_config_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x08);
                uint8_t c  = (cls >> 24) & 0xFF;
                uint8_t sc = (cls >> 16) & 0xFF;
                uint8_t pi = (cls >> 8)  & 0xFF;
                if (c == class_code && sc == subclass && pi == prog_if) {
                    if (seen == n) {
                        fill_device(out, bus, slot, (uint32_t)func, vid, (id >> 16) & 0xFFFF);
                        return 1;
                    }
                    seen++;
                }
            }
        }
    }
    return 0;
}

void pci_enable_bus_master(const pci_device_t *dev) {
    uint32_t cmd = pci_config_read32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1 << 1) | (1 << 2);   // memory space + bus master
    pci_config_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);
}
