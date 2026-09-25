.code64

.macro ISR_NOERRCODE num
.global isr\num
isr\num:
    push $0
    push $\num
    jmp isr_common_stub
.endm

.macro ISR_ERRCODE num
.global isr\num
isr\num:
    push $\num
    jmp isr_common_stub
.endm

.macro IRQ num, mapped_num
.global irq\num
irq\num:
    push $0
    push $\mapped_num
    jmp irq_common_stub
.endm

ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30
ISR_NOERRCODE 31

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

# The timer, passed on to the application processors by the bootstrap core
# (SMP_TICK_VECTOR, 0xE0). The same path as a device interrupt: it takes the
# kernel lock, and may switch to another process on the way out.
.global smp_tick_isr
smp_tick_isr:
    push $0
    push $0xE0
    jmp irq_common_stub

# The two interrupts that must NOT go near the kernel lock, and so do not go
# near C either. Neither touches GS, so neither needs swapgs, whether it
# caught ring 3 or ring 0; each saves the one register it uses.
#
# SMP_WAKE_VECTOR (0xF0): "look at the work queues". The waking is the whole
# of it -- the core comes out of hlt and its loop looks for itself. Going
# through the lock here would be worse than pointless: the core that sends a
# wake-up is often the one holding the lock, waiting for the woken core to
# pick up work that needs no lock at all.
.global smp_wake_isr
smp_wake_isr:
    push %rax
    mov lapic_eoi_reg(%rip), %rax
    movl $0, (%rax)
    pop %rax
    iretq

# SMP_TLB_VECTOR (0xF3): "your cached translations may be stale". Reloading
# CR3 with itself drops every one of them (nothing here is mapped global), and
# only then is the flush counted as done. The core that asked is holding the
# kernel lock and waiting for this count, so it must be possible to get here
# from anywhere -- including from a core that is itself waiting for the lock,
# which is why that wait keeps interrupts open.
.global smp_tlb_isr
smp_tlb_isr:
    push %rax
    mov %cr3, %rax
    mov %rax, %cr3
    lock incl tlb_acks(%rip)
    mov lapic_eoi_reg(%rip), %rax
    movl $0, (%rax)
    pop %rax
    iretq

# Spurious-interrupt vector (255): the Local APIC's SVR points here. It must NOT
# send an EOI, so it simply returns.
.global spurious_isr
spurious_isr:
    iretq

.macro PUSH_REGS
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
.endm

.macro POP_REGS
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
.endm

# On entry the CPU has pushed SS,RSP,RFLAGS,CS,RIP and the stub pushed
# err_code,int_no -> CS is at 24(%rsp). If it came from ring 3 (CS.RPL != 0) we
# must swapgs to bring the kernel's per-CPU block into GS, and swap back before
# returning. A trap that caught the kernel already has the kernel GS.
.extern isr_handler
.extern bkl_exit
isr_common_stub:
    testb $3, 24(%rsp)
    jz .Lisr_kernel_in
    swapgs
.Lisr_kernel_in:
    PUSH_REGS
    mov %rsp, %rdi
    call isr_handler
    # isr_handler took the kernel lock; this is where it is given back, still
    # on the stack the fault was taken on.
    call bkl_exit
    POP_REGS
    add $16, %rsp
    testb $3, 8(%rsp)          # CS of the frame we are returning to
    jz .Lisr_kernel_out
    swapgs
.Lisr_kernel_out:
    iretq

.extern irq_handler
irq_common_stub:
    testb $3, 24(%rsp)
    jz .Lirq_kernel_in
    swapgs
.Lirq_kernel_in:
    PUSH_REGS
    mov %rsp, %rdi
    call irq_handler
    # irq_handler returns the stack pointer to resume on: the same frame in the
    # ordinary case, or another process's saved frame when the scheduler
    # decided to rotate. Adopting it here IS the context switch.
    mov %rax, %rsp
    # And only NOW let go of the kernel lock -- after the switch, not before
    # it. The process being switched away from is runnable again the moment
    # the lock is free, and another core may pick it up at once; until this
    # instruction's predecessor ran, this core was still standing on its
    # kernel stack. (The frame at %rsp is 16-byte aligned, so this is a
    # well-formed call.)
    call bkl_exit
    POP_REGS
    add $16, %rsp
    testb $3, 8(%rsp)          # CS we are returning to (may be a different process)
    jz .Lirq_kernel_out
    swapgs
.Lirq_kernel_out:
    iretq
