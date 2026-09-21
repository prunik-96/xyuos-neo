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

# SMP wake IPI (vector 240): an application processor takes this to leave its
# idle hlt and check the work queue. It flows through the normal IRQ path,
# which EOIs the local APIC and (for a non-zero irq) just returns the frame.
.global smp_wake_isr
smp_wake_isr:
    push $0
    push $240
    jmp irq_common_stub

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
isr_common_stub:
    testb $3, 24(%rsp)
    jz .Lisr_kernel_in
    swapgs
.Lisr_kernel_in:
    PUSH_REGS
    mov %rsp, %rdi
    call isr_handler
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
    POP_REGS
    add $16, %rsp
    testb $3, 8(%rsp)          # CS we are returning to (may be a different process)
    jz .Lirq_kernel_out
    swapgs
.Lirq_kernel_out:
    iretq
