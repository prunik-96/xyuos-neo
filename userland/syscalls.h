#ifndef USER_SYSCALLS_H
#define USER_SYSCALLS_H

#define SYS_WRITE   1
#define SYS_EXIT    2
#define SYS_OPEN    3
#define SYS_READ    4
#define SYS_CLOSE   5
#define SYS_LISTDIR 6

// The SYSCALL instruction itself only clobbers RCX/R11, but our kernel-side
// syscall_entry.S additionally uses R8-R10 as scratch while marshaling
// arguments to syscall_dispatch() and never restores them. Without listing
// them here, GCC assumes they survive the asm block and happily caches
// constants in them across consecutive syscalls -- which then get
// silently corrupted, sending garbage syscall numbers on the next call.
static inline long syscall3(long num, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "r8", "r9", "r10", "memory"
    );
    return ret;
}

static inline long sys_write(const char *buf, unsigned long len) {
    return syscall3(SYS_WRITE, (long)buf, (long)len, 0);
}

static inline void sys_exit(int code) {
    syscall3(SYS_EXIT, code, 0, 0);
    for (;;) { }
}

static inline long sys_open(const char *path) {
    return syscall3(SYS_OPEN, (long)path, 0, 0);
}

static inline long sys_read(long fd, void *buf, unsigned long len) {
    return syscall3(SYS_READ, fd, (long)buf, (long)len);
}

static inline void sys_close(long fd) {
    syscall3(SYS_CLOSE, fd, 0, 0);
}

static inline long sys_listdir(const char *path, char *buf, unsigned long len) {
    return syscall3(SYS_LISTDIR, (long)path, (long)buf, (long)len);
}

static inline unsigned long strlen_(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

#endif
