#ifndef IDT_H
#define IDT_H

#include <stdint.h>

struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*irq_handler_t)(struct interrupt_frame *frame);

void idt_init(void);
void idt_load(void);   // load the shared IDT on an application processor
void idt_set_gate(uint8_t vector, void (*handler)(void), uint8_t ist, uint8_t type_attr);
void irq_register_handler(uint8_t irq, irq_handler_t handler);

// Enable / disable a hardware IRQ line via the active interrupt controller
// (IO-APIC or PIC). Drivers should use these rather than the PIC directly.
void irq_unmask(uint8_t irq);
void irq_mask(uint8_t irq);

#endif
