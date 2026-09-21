#ifndef SETJMP_H
#define SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

// System V x86-64: only the callee-saved registers, the stack pointer and the
// return address have to survive. Layout (8 bytes each), matching setjmp.S:
//   0 rbx  8 rbp  16 r12  24 r13  32 r14  40 r15  48 rsp  56 rip
typedef unsigned long jmp_buf[8];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif
