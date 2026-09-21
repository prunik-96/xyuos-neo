#ifndef POWER_H
#define POWER_H

#include <stdint.h>

// Parse ACPI (RSDP handed to us by the UEFI firmware via the multiboot2 tag)
// to learn how to power the machine off and reset it. Safe to call once at boot;
// on failure the reboot/off paths fall back to legacy/emulator methods.
void power_init(uint32_t multiboot_addr);

// Reboot the machine. Tries the ACPI reset register, then the 8042 keyboard
// controller, then a triple fault. Does not return.
void power_reboot(void);

// Power the machine off (ACPI S5), with QEMU/VBox port fallbacks. If nothing
// works it returns (e.g. real hardware without a matching ACPI table).
void power_off(void);

#endif
