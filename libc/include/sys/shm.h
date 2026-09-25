/* Shared memory: one lump of physical memory that two programs can both
 * write to and both see.
 *
 * Pipes copy; this does not. That is the whole difference, and it is why the
 * rules are stricter: nothing arranges the two programs' turns for them.
 * Something must -- a lock they both agree on inside the segment, or a signal
 * between them -- or both will write at once and neither will know.
 *
 * A pointer INTO a segment means nothing to the other program. The two see
 * the same memory at addresses that need not be the same, so what travels
 * between them is an offset from the start, never an address.
 *
 * A segment outlives the program that made it. It goes away when somebody
 * asks for it to (shmctl IPC_RMID) and everybody who had it has let go --
 * which is what lets one program set something up and exit, leaving it for
 * the next.
 *
 * The interface is System V's, so code written for it works unchanged.
 */
#ifndef SYS_SHM_H
#define SYS_SHM_H

typedef int key_t;

#define IPC_PRIVATE 0       /* a segment with no name, for children only */
#define IPC_CREAT   01000
#define IPC_EXCL    02000
#define IPC_RMID    0
#define SHM_RDONLY  010000

#ifdef __cplusplus
extern "C" {
#endif

/* Find the segment under `key`, or make one of `size` bytes if IPC_CREAT is
 * given. Returns its id, or -1. A key of IPC_PRIVATE always makes a new one.
 * Sizes are rounded up to whole pages; the biggest is 16 MiB. */
int shmget(key_t key, unsigned long size, int flags);

/* Map it into this program. `addr` must be NULL -- the kernel chooses where,
 * and where it chose is what comes back. (void *)-1 on failure, as System V
 * says. SHM_RDONLY maps it read-only. */
void *shmat(int shmid, const void *addr, int flags);

/* Let go of it. The memory stays; only this program's view of it goes. */
int shmdt(const void *addr);

/* The only command is IPC_RMID: the name goes at once, the memory when the
 * last program lets go. `buf` is ignored and should be NULL. */
int shmctl(int shmid, int cmd, void *buf);

/* How big it actually is, in bytes -- rounded up to pages from what was
 * asked for. Not System V; there is no shmid_ds here to put it in. */
unsigned long shmsize(int shmid);

#ifdef __cplusplus
}
#endif

#endif
