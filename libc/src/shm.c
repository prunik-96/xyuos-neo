/* The userland half of shared memory -- five calls that are each one
 * syscall. See sys/shm.h for what a segment is. */
#include <sys/shm.h>
#include <xyuos_syscall.h>

int shmget(key_t key, unsigned long size, int flags) {
    struct shm_req rq;
    rq.key = key;
    rq.flags = flags;
    rq.size = size;
    return (int)xyuos_syscall3(SYS_SHM, SHMOP_GET, (long)(void *)&rq, 0);
}

void *shmat(int shmid, const void *addr, int flags) {
    /* The kernel places it. Asking for a particular address would mean
     * telling the program it got one when it had not, and there is nothing
     * here that needs it. */
    if (addr) return (void *)-1;
    long r = xyuos_syscall3(SYS_SHM, SHMOP_ATTACH, shmid, flags);
    return r ? (void *)r : (void *)-1;
}

int shmdt(const void *addr) {
    return (int)xyuos_syscall3(SYS_SHM, SHMOP_DETACH, (long)addr, 0);
}

int shmctl(int shmid, int cmd, void *buf) {
    (void)buf;
    return (int)xyuos_syscall3(SYS_SHM, SHMOP_CTL, shmid, cmd);
}

unsigned long shmsize(int shmid) {
    return (unsigned long)xyuos_syscall3(SYS_SHM, SHMOP_SIZE, shmid, 0);
}
