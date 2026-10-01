// The four memory functions every C compiler assumes exist.
//
// The kernel went without them for a long time -- everything copied with its
// own loop -- but code written for an ordinary C library (librast, linked in
// for the desktop) calls them by name, and GCC itself may turn a struct copy
// into a call to memcpy whatever the source says.
//
// `rep movsb` and `rep stosb` rather than C loops: a loop here is exactly the
// pattern GCC recognises and replaces with a call to memcpy -- this very
// function -- and the string instructions are what modern processors make
// fast anyway.

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n) {
    void *d = dst;
    __asm__ volatile ("rep movsb" : "+D"(d), "+S"(src), "+c"(n) :: "memory");
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    void *d = dst;
    __asm__ volatile ("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return dst;
}

// Overlap is only a problem when the destination starts inside the source:
// then the copy has to run from the end, with the direction flag set for the
// length of one instruction.
void *memmove(void *dst, const void *src, size_t n) {
    if ((uintptr_t)dst <= (uintptr_t)src || (uintptr_t)dst >= (uintptr_t)src + n)
        return memcpy(dst, src, n);
    void *d = (uint8_t *)dst + n - 1;
    const void *s = (const uint8_t *)src + n - 1;
    __asm__ volatile ("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const volatile uint8_t *p = a, *q = b;
    for (size_t i = 0; i < n; i++)
        if (p[i] != q[i]) return p[i] < q[i] ? -1 : 1;
    return 0;
}

size_t strlen(const char *s) {
    const volatile char *p = s;
    size_t n = 0;
    while (p[n]) n++;
    return n;
}
