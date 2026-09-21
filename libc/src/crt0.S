.code64

.global _start
.extern main
.extern _exit

// The kernel lays out the System V argument block at the stack pointer:
//   [rsp]   argc
//   [rsp+8] argv[0] ... argv[argc-1], then NULL
_start:
    xor %ebp, %ebp
    mov (%rsp), %rdi          // argc
    lea 8(%rsp), %rsi         // argv
    call main
    mov %eax, %edi
    call _exit
    hlt
