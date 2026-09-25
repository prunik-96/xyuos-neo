/* The userland half of threads. See thread.h for what one is.
 *
 * The kernel jumps a new thread straight to an address with one argument in
 * a register and no return address behind it -- there is nowhere for the
 * thread function to return TO. So it is never started directly: the kernel
 * starts the trampoline below, which calls it and then ends the thread
 * properly when it comes back.
 */
#include <thread.h>
#include <xyuos_syscall.h>
#include <stdlib.h>

struct start {
    void (*fn)(void *);
    void  *arg;
};

/* Runs on the new thread, on its own stack, with nothing above it. */
static void thread_trampoline(void *p) {
    struct start *s = (struct start *)p;
    void (*fn)(void *) = s->fn;
    void *arg = s->arg;
    free(s);                     /* the shared heap: this is the same program */

    fn(arg);
    thread_exit(0);              /* falling off the end is an ordinary end */
}

int thread_create(void (*fn)(void *), void *arg) {
    if (!fn) return -1;

    struct start *s = (struct start *)malloc(sizeof *s);
    if (!s) return -1;
    s->fn = fn;
    s->arg = arg;

    long tid = xyuos_syscall3(SYS_THREAD, THREAD_OP_CREATE,
                              (long)(void *)thread_trampoline, (long)s);
    if (tid < 0) { free(s); return -1; }
    return (int)tid;
}

void thread_exit(int code) {
    xyuos_syscall3(SYS_THREAD, THREAD_OP_EXIT, (long)code, 0);
    for (;;) { }                 /* not reached; the kernel does not return */
}

int thread_join(int tid) {
    return (int)xyuos_syscall3(SYS_THREAD, THREAD_OP_JOIN, (long)tid, 0);
}

int thread_self(void) {
    return (int)xyuos_syscall3(SYS_THREAD, THREAD_OP_SELF, 0, 0);
}
