.code64
.global context_switch

# void context_switch(uint64_t *old_rsp, uint64_t new_rsp)
# Saves callee-saved registers on the current stack, stashes the resulting
# RSP into *old_rsp, switches to new_rsp, and returns into whatever context
# that stack was prepared for (either a previous switch-out point, or the
# initial trampoline frame built by task_create).
context_switch:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15

    mov %rsp, (%rdi)
    mov %rsi, %rsp

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret
