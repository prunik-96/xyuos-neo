#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>

void heap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);
// Grow or shrink a block, keeping its contents up to the smaller of the two
// sizes. NULL behaves as kmalloc; a size of 0 frees and returns NULL.
void *krealloc(void *ptr, size_t size);

#endif
