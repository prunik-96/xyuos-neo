#ifndef KSHIM_STRING_H
#define KSHIM_STRING_H

/* The memory functions, defined in kernel/kernel/kmem.c. */

#include <stddef.h>

void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

#endif
