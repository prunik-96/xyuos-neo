/* Mapping a file into memory.
 *
 * There is no demand paging behind this. A read-only mapping is read in full
 * when it is asked for, and munmap gives the memory back. For a program that
 * maps a file to read it -- which is every caller here -- that is the same
 * thing observed from the outside: the same bytes at the same addresses for
 * the same lifetime. What it is not is cheap for a large file, and it is not
 * shared between programs.
 *
 * A writable shared mapping is a different promise -- that writes reach the
 * file and other readers -- and this cannot keep it, so it is refused rather
 * than quietly given as a private copy.
 */
#ifndef SYS_MMAN_H
#define SYS_MMAN_H

#include <stddef.h>

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS

#define MAP_FAILED ((void *)-1)

#ifdef __cplusplus
extern "C" {
#endif

/* `offset` must be 0 and `addr` NULL: this cannot place a mapping at a
 * chosen address, and says so by failing rather than putting it elsewhere. */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);
int   munmap(void *addr, size_t length);

#ifdef __cplusplus
}
#endif

#endif
