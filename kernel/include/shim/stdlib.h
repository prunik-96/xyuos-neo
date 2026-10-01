#ifndef KSHIM_STDLIB_H
#define KSHIM_STDLIB_H

/* Just enough of <stdlib.h> for code written against a C library to build
 * inside the kernel: the allocator, routed to the kernel heap. Only the
 * librast build puts this directory on the include path. */

#include <stddef.h>

void *kmalloc(size_t size);
void  kfree(void *ptr);
void *krealloc(void *ptr, size_t size);

static inline void *malloc(size_t n)           { return kmalloc(n ? n : 1); }
static inline void  free(void *p)              { kfree(p); }
static inline void *realloc(void *p, size_t n) { return krealloc(p, n); }

static inline void *calloc(size_t n, size_t m) {
    size_t t = n * m;
    if (m && t / m != n) return 0;
    unsigned char *p = (unsigned char *)kmalloc(t ? t : 1);
    if (p) for (size_t i = 0; i < t; i++) ((volatile unsigned char *)p)[i] = 0;
    return p;
}

static inline int abs(int v) { return v < 0 ? -v : v; }

#endif
