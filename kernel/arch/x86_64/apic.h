#ifndef APIC_H
#define APIC_H

#include <stdint.h>

// Bring up the Local APIC and IO-APIC from the ACPI MADT and mask the legacy
// 8259 PIC. After this, hardware IRQs are delivered through the IO-APIC to the
// bootstrap core and acknowledged with the Local APIC. Returns 1 on success, 0
// if there is no usable APIC (in which case the caller keeps using the PIC).
int  apic_init(uint32_t multiboot_addr);

// 1 once apic_init() has succeeded.
int  apic_active(void);

// End-of-interrupt to the Local APIC (replaces the PIC EOI).
void lapic_eoi(void);

// Software-enable the Local APIC of the CURRENT core. Every core must call this
// (the BSP via apic_init, each AP in its startup) to receive interrupts/IPIs.
void apic_enable_local(void);

// Route / mask an ISA IRQ (0..15) through the IO-APIC. Vector = 32 + irq,
// delivered to the bootstrap core.
void apic_unmask_irq(uint8_t irq);
void apic_mask_irq(uint8_t irq);

// Topology the MADT reported (used by SMP bring-up later).
int      apic_cpu_count(void);
const uint8_t *apic_cpu_ids(void);   // apic_cpu_count() entries
uint32_t apic_bsp_id(void);

// Inter-processor interrupts for waking application processors.
void lapic_send_init(uint8_t apic_id);
void lapic_send_sipi(uint8_t apic_id, uint8_t vector);
void lapic_broadcast_ipi(uint8_t vector);

#endif
