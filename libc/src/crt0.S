.code64

.global _start
.extern __libc_start

// The kernel lays out the System V argument block at the stack pointer:
//   [rsp]   argc
//   [rsp+8] argv[0] ... argv[argc-1], then NULL
//
// main() is not called from here any more. There is work to do first -- the
// constructors of objects at file scope, and telling the unwinder where the
// exception tables are -- and it is C, so it lives in start.c.
_start:
    xor %ebp, %ebp
    mov (%rsp), %rdi          // argc
    lea 8(%rsp), %rsi         // argv
    call __libc_start         // does not return
    hlt
