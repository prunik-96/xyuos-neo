#ifndef SHM_H
#define SHM_H

// Memory that belongs to nobody in particular.
//
// Every other kind of memory here has exactly one owner: a page in a process
// is that process's, and when it ends the page goes back. A shared segment is
// the opposite -- it is a lump of physical frames that exists on its own, and
// address spaces come and go around it. That inversion is the whole of what
// makes this different from mmap, and it shows up in three places:
//
//   Tearing down an address space must NOT free those frames. They are not
//   its to free -- see vmm_space_unref, which takes shared mappings out
//   before the page tables go.
//
//   A segment outlives the program that made it. It is freed when it has
//   been marked for removal AND nobody has it mapped, which are two separate
//   conditions on purpose: that is what lets one program set something up,
//   exit, and another find it still there.
//
//   Two address spaces map the same frames at addresses that need not match.
//   A pointer INTO shared memory is therefore meaningful only to the program
//   holding it -- offsets travel between programs, addresses do not.
//
// The interface is System V's, because it is the one that fits: integer keys,
// integer ids, attach and detach. Nothing here is an fd.

#include <stdint.h>

#define SHM_SEGMENTS   16
#define SHM_MAX_PAGES  4096      /* 16 MiB in one segment */

/* flags for shm_get */
#define IPC_PRIVATE 0
#define IPC_CREAT   01000
#define IPC_EXCL    02000

/* cmd for shm_ctl */
#define IPC_RMID    0

/* flags for shm_attach */
#define SHM_RDONLY  010000

// Find or create a segment. `key` of IPC_PRIVATE always makes a new one that
// nobody can name. Returns the id, or -1.
int shm_get(int key, uint64_t size, int flags);

// Map a segment into the calling process's address space. Returns the address
// it landed at, or 0. The address is this process's alone -- another process
// mapping the same segment may well see it somewhere else.
uint64_t shm_attach(int id, int flags);

// Take a mapping out of the calling process. 0, or -1 if that address is not
// the start of an attached segment.
int shm_detach(uint64_t addr);

// IPC_RMID: no new attach may find it, and it goes as soon as the last
// mapping does. 0, or -1 if there is no such segment.
int shm_ctl(int id, int cmd);

// How big a segment is, in bytes, or 0 if there is no such segment.
uint64_t shm_size(int id);

// Called by the memory layer when a mapping of `id` has gone: the last one
// out of a removed segment frees it.
void shm_dropped(int id);

#endif
