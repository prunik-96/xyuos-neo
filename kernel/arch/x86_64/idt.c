#include "idt.h"
#include "gdt.h"
#include "../../kernel/process.h"
#include "../../kernel/panic.h"
#include "../../drivers/serial.h"
#include "../../drivers/framebuffer.h"
#include "../../kernel/kio.h"
#include "pic.h"
#include "apic.h"
#include "../../wm/wm.h"
#include <stddef.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idtr idtr;
static irq_handler_t irq_handlers[16];

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

static void (*isr_stub_table[32])(void) = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
};

static void (*irq_stub_table[16])(void) = {
    irq0, irq1, irq2,  irq3,  irq4,  irq5,  irq6,  irq7,
    irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15,
};

static const char *exception_names[32] = {
    "Divide-by-zero", "Debug", "NMI", "Breakpoint", "Overflow",
    "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS",
    "Segment Not Present", "Stack-Segment Fault", "General Protection Fault",
    "Page Fault", "Reserved", "x87 FP Exception", "Alignment Check",
    "Machine Check", "SIMD FP Exception", "Virtualization Exception",
    "Control Protection Exception", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Security Exception", "Reserved",
};

void idt_set_gate(uint8_t vector, void (*handler)(void), uint8_t ist, uint8_t type_attr) {
    uint64_t addr = (uint64_t)handler;
    idt[vector].offset_low  = addr & 0xFFFF;
    idt[vector].selector    = 0x08; // kernel code segment from boot.S GDT64
    idt[vector].ist         = ist;
    idt[vector].type_attr   = type_attr;
    idt[vector].offset_mid  = (addr >> 16) & 0xFFFF;
    idt[vector].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[vector].zero        = 0;
}

void irq_register_handler(uint8_t irq, irq_handler_t handler) {
    if (irq < 16) {
        irq_handlers[irq] = handler;
    }
}

// Enable / disable delivery of a hardware IRQ, routed to whichever interrupt
// controller is in charge (IO-APIC once it is up, otherwise the 8259 PIC).
void irq_unmask(uint8_t irq) {
    if (apic_active()) apic_unmask_irq(irq);
    else pic_clear_mask(irq);
}
void irq_mask(uint8_t irq) {
    if (apic_active()) apic_mask_irq(irq);
    else pic_set_mask(irq);
}

void idt_init(void) {
    for (int i = 0; i < 32; i++) {
        idt_set_gate(i, isr_stub_table[i], 0, 0x8E);
    }
    // Double fault (8) and page fault (14) run on IST1, a private stack, so a
    // kernel-stack overflow still lands in the panic handler instead of tripping
    // a triple fault and silently resetting the machine.
    idt_set_gate(8,  isr_stub_table[8],  1, 0x8E);
    idt_set_gate(14, isr_stub_table[14], 1, 0x8E);

    pic_remap(32, 40);

    for (int i = 0; i < 16; i++) {
        idt_set_gate(32 + i, irq_stub_table[i], 0, 0x8E);
        irq_handlers[i] = NULL;
    }

    // SMP inter-processor vectors: the wake IPI and the LAPIC spurious vector.
    extern void smp_wake_isr(void);
    extern void spurious_isr(void);
    idt_set_gate(240, smp_wake_isr, 0, 0x8E);
    idt_set_gate(255, spurious_isr, 0, 0x8E);

    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)&idt;
    __asm__ volatile ("lidt %0" : : "m"(idtr));
}

// Application processors load the same IDT (it is read-only and shared).
void idt_load(void) {
    __asm__ volatile ("lidt %0" : : "m"(idtr));
}

// A sentence a person can act on, per fault. "General protection fault" is
// what the manual calls it; "the program tried to do something only the
// kernel may do" is what actually happened.
static const char *fault_reason(uint32_t n) {
    switch (n) {
        case 0:  return "The program divided by zero.";
        case 6:  return "The program ran into an instruction that does not exist.";
        case 13: return "The program tried to do something only the kernel may do.";
        case 14: return "The program touched memory that does not belong to it.";
        case 16:
        case 19: return "A floating-point operation failed.";
        default: return "The program hit a processor fault and was stopped.";
    }
}

static void put_hex(char *out, int *n, int max, uint64_t v, int digits) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; i--) {
        if (*n < max - 1) out[(*n)++] = hex[(v >> (i * 4)) & 0xF];
    }
}

static void put_str(char *out, int *n, int max, const char *s) {
    while (*s && *n < max - 1) out[(*n)++] = *s++;
}

static void put_dec(char *out, int *n, int max, int v) {
    char tmp[12];
    int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v > 0 && t < 12) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    while (t > 0 && *n < max - 1) out[(*n)++] = tmp[--t];
}

// A stable identifier per fault, so "it says 0x0E-0004" is a useful thing to
// be told by someone standing at the machine.
static void fmt_fault_code(char *out, int max, uint32_t n) {
    int k = 0;
    put_str(out, &k, max, "0x");
    put_hex(out, &k, max, n, 2);
    put_str(out, &k, max, "-XYU");
    out[k] = 0;
}

static void fmt_fault_detail(char *out, int max, const char *name, int pid,
                             uint64_t rip, uint64_t cr2) {
    int k = 0;
    put_str(out, &k, max, name && name[0] ? name : "program");
    put_str(out, &k, max, " (pid ");
    put_dec(out, &k, max, pid);
    put_str(out, &k, max, ") at ");
    put_hex(out, &k, max, rip, 12);
    if (cr2) {
        put_str(out, &k, max, ", address ");
        put_hex(out, &k, max, cr2, 12);
    }
    out[k] = 0;
}

void isr_handler(struct interrupt_frame *frame) {
    const char *name = "Unknown";
    if (frame->int_no < 32) {
        name = exception_names[frame->int_no];
    }

    // A fault that happened in ring 3 is the running program's bug, not the
    // kernel's. Kill just that process and return to its parent (the shell)
    // instead of halting the whole machine -- a userland crash must never take
    // the OS down with it. cs low two bits = CPL; 3 means user mode.
    if ((frame->cs & 3) == 3) {
        process_t *p = process_current();
        if (p) {
            uint64_t cr2 = 0;
            if (frame->int_no == 14)
                __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            kprintf("process %d faulted: %s rip=%x:%x cr2=%x:%x -- terminated\n",
                    p->pid, name,
                    (unsigned)(frame->rip >> 32), (unsigned)(frame->rip & 0xFFFFFFFF),
                    (unsigned)(cr2 >> 32), (unsigned)(cr2 & 0xFFFFFFFF));
            // Tell the person at the keyboard, not just the serial log.
            char code[24], det[96];
            fmt_fault_code(code, sizeof code, frame->int_no);
            fmt_fault_detail(det, sizeof det, p->name, p->pid, frame->rip, cr2);
            wm_message_box(MB_ERROR, code, "Program stopped working",
                           fault_reason(frame->int_no), det);

            process_notify_exit(139);   // noreturn: wakes the parent, switches away
        }
        // No current process yet: fall through to the panic path.
    }

    // A kernel-mode fault is unrecoverable. Paint the full panic report to the
    // screen (reads the kernel log for its "recent messages" panel, so do it
    // before the serial dump below adds to that log).
    panic_screen(name, frame);

    // Also dump to the serial port for host-side debugging. This writes to the
    // off-screen buffer only (never presented again), so the panic screen that
    // panic_screen() already put up stays on screen untouched.
    serial_write("\n*** KERNEL PANIC: ");
    serial_write(name);
    serial_write(" ***\n");
    kprintf("int=%u err=%x rip=%x:%x rsp=%x:%x\n",
            (unsigned)frame->int_no, (unsigned)frame->err_code,
            (unsigned)(frame->rip >> 32), (unsigned)(frame->rip & 0xFFFFFFFF),
            (unsigned)(frame->rsp >> 32), (unsigned)(frame->rsp & 0xFFFFFFFF));
    if (frame->int_no == 13) {
        // A #GP naming a selector, raised at an iretq, is almost always the
        // frame being restored rather than the code doing the restoring. The
        // faulting rsp points straight at that frame, so print it -- and the
        // descriptor the selector refers to, since a clobbered GDT looks
        // exactly the same from the error code alone.
        const uint64_t *f = (const uint64_t *)(uintptr_t)frame->rsp;
        kprintf("gp: frame at rsp: rip=%x:%x cs=%x rflags=%x rsp=%x:%x ss=%x\n",
                (unsigned)(f[0] >> 32), (unsigned)f[0], (unsigned)f[1],
                (unsigned)f[2],
                (unsigned)(f[3] >> 32), (unsigned)f[3], (unsigned)f[4]);
        unsigned idx = (unsigned)(frame->err_code >> 3);
        uint64_t d = gdt_entry_raw((int)idx);
        kprintf("gp: selector index %u -> gdt[%u] = %x:%x\n",
                idx, idx, (unsigned)(d >> 32), (unsigned)d);
        kprintf("gp: gdt4=%x:%x gdt5=%x:%x\n",
                (unsigned)(gdt_entry_raw(4) >> 32), (unsigned)gdt_entry_raw(4),
                (unsigned)(gdt_entry_raw(5) >> 32), (unsigned)gdt_entry_raw(5));
    }
    if (frame->int_no == 14) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        kprintf("cr2=%x:%x (%s, %s, %s)\n",
                (unsigned)(cr2 >> 32), (unsigned)(cr2 & 0xFFFFFFFF),
                (frame->err_code & 1) ? "protection" : "not-present",
                (frame->err_code & 2) ? "write" : "read",
                (frame->err_code & 4) ? "user" : "kernel");
    }
    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

// Returns the stack pointer to resume on -- normally the frame we were handed,
// but the timer may hand back another process's saved frame instead, which is
// how preemption happens. See irq_common_stub in isr.S.
uint64_t irq_handler(struct interrupt_frame *frame) {
    uint64_t irq = frame->int_no - 32;
    if (irq < 16 && irq_handlers[irq]) {
        irq_handlers[irq](frame);
    }
    // EOI before any switch: the controller must be released even if we never
    // come back to this frame. The APIC owns delivery once it is up.
    if (apic_active()) lapic_eoi();
    else pic_send_eoi((uint8_t)irq);

    if (irq == 0) {
        return sched_on_tick((uint64_t)(uintptr_t)frame, frame->cs);
    }
    return (uint64_t)(uintptr_t)frame;
}
