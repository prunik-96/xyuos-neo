// setjmp/longjmp for System V x86-64.
//
// Only callee-saved state has to be preserved: rbx, rbp, r12-r15, plus the
// stack pointer and the return address. Caller-saved registers are already
// the caller's problem, which is why this is eight words and no more.
//
// Slot layout (must match jmp_buf in setjmp.h):
//   0 rbx   8 rbp   16 r12   24 r13   32 r14   40 r15   48 rsp   56 rip

.section .text

.global setjmp
.type setjmp, @function
setjmp:
    movq %rbx,  0(%rdi)
    movq %rbp,  8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)

    leaq 8(%rsp), %rax        // rsp as it will be *after* our ret pops
    movq %rax, 48(%rdi)
    movq (%rsp), %rax         // return address sitting at the top of the stack
    movq %rax, 56(%rdi)

    xorl %eax, %eax           // direct call returns 0
    ret
.size setjmp, . - setjmp

.global longjmp
.type longjmp, @function
longjmp:
    movq  0(%rdi), %rbx
    movq  8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15

    movq 48(%rdi), %rsp       // unwind the stack to the setjmp frame
    movq 56(%rdi), %rdx       // where setjmp would have returned to

    movl %esi, %eax           // longjmp(env, val) makes setjmp return val,
    testl %eax, %eax          // except that 0 becomes 1 (C requires this)
    jnz 1f
    movl $1, %eax
1:  jmp *%rdx
.size longjmp, . - longjmp

// No executable stack.
.section .note.GNU-stack,"",@progbits
