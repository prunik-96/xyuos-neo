.code64

.global kctx_save
# uint64_t kctx_save(uint64_t ctx[8])  -- returns 0 the first time.
kctx_save:
    mov %rbx, 0(%rdi)
    mov %rbp, 8(%rdi)
    mov %r12, 16(%rdi)
    mov %r13, 24(%rdi)
    mov %r14, 32(%rdi)
    mov %r15, 40(%rdi)
    lea 8(%rsp), %rax
    mov %rax, 48(%rdi)
    mov (%rsp), %rax
    mov %rax, 56(%rdi)
    xor %eax, %eax
    ret

.global kctx_restore
# void kctx_restore(uint64_t ctx[8], uint64_t return_value) -- makes the
# matching kctx_save() call appear to return return_value instead.
kctx_restore:
    mov 0(%rdi), %rbx
    mov 8(%rdi), %rbp
    mov 16(%rdi), %r12
    mov 24(%rdi), %r13
    mov 32(%rdi), %r14
    mov 40(%rdi), %r15
    mov 48(%rdi), %rsp
    mov 56(%rdi), %rcx
    mov %rsi, %rax
    jmp *%rcx
