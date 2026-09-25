/* The userland half of signals. The kernel does the hard part; this is the
 * paperwork.
 *
 * The one thing worth knowing is the trampoline. A handler has to return
 * somewhere, and "somewhere" has to be code the kernel can name before the
 * program runs -- so libc tells the kernel where its trampoline is the first
 * time a handler is installed, and the kernel puts that address under every
 * handler it delivers. See sigtramp.S.
 */
#include <signal.h>
#include <xyuos_syscall.h>

extern void __sig_trampoline(void);

/* sigtramp.S spells these out because it cannot include the header. */
_Static_assert(SYS_SIGNAL == 48, "sigtramp.S has SYS_SIGNAL written out");
_Static_assert(SIGOP_RETURN == 2, "sigtramp.S has SIGOP_RETURN written out");

static int tramp_told;

static void tell_kernel_where_to_return(void) {
    if (tramp_told) return;
    xyuos_syscall3(SYS_SIGNAL, SIGOP_TRAMP,
                   (long)(void *)__sig_trampoline, 0);
    tramp_told = 1;
}

sighandler_t signal(int sig, sighandler_t handler) {
    tell_kernel_where_to_return();
    long was = xyuos_syscall3(SYS_SIGNAL, SIGOP_HANDLER, sig,
                              (long)(void *)handler);
    /* A handler lives in the program's own window, which -1 is not, so this
     * cannot be mistaken for one. */
    if (was == -1) return SIG_ERR;
    return (sighandler_t)was;
}

int kill(int pid, int sig) {
    return (int)xyuos_syscall3(SYS_SIGNAL, SIGOP_SEND, pid, sig);
}

int raise(int sig) {
    int me = (int)xyuos_syscall3(SYS_THREAD, THREAD_OP_SELF, 0, 0);
    return kill(me, sig);
}

unsigned int alarm_ms(unsigned int ms) {
    xyuos_syscall3(SYS_SIGNAL, SIGOP_ALARM, (long)ms, 0);
    return 0;
}

unsigned int alarm(unsigned int seconds) {
    return alarm_ms(seconds * 1000u);
}

int pause(void) {
    /* There is no "sleep until something happens" in the kernel, and one
     * would buy nothing: a signal cuts a sleep short and does not restart
     * it, so a long enough sleep IS waiting for a signal. */
    xyuos_syscall3(SYS_SLEEP, 1000000000L, 0, 0);   /* eleven days */
    return -1;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old) {
    int op;
    switch (how) {
        case SIG_BLOCK:   op = SIGMASK_BLOCK;   break;
        case SIG_UNBLOCK: op = SIGMASK_UNBLOCK; break;
        case SIG_SETMASK: op = SIGMASK_SET;     break;
        default: return -1;
    }
    if (!set) {
        /* Only asking. Blocking nothing extra changes nothing and hands the
         * current mask back. */
        long cur = xyuos_syscall3(SYS_SIGNAL, SIGOP_MASK, SIGMASK_BLOCK, 0);
        if (old) *old = (sigset_t)(unsigned long)cur;
        return 0;
    }
    long was = xyuos_syscall3(SYS_SIGNAL, SIGOP_MASK, op,
                              (long)(unsigned long)*set);
    if (old) *old = (sigset_t)(unsigned long)was;
    return 0;
}

int sigemptyset(sigset_t *s) { if (!s) return -1; *s = 0; return 0; }
int sigfillset(sigset_t *s)  { if (!s) return -1; *s = ~0UL; return 0; }

int sigaddset(sigset_t *s, int sig) {
    if (!s || sig <= 0 || sig >= NSIG) return -1;
    *s |= 1UL << sig;
    return 0;
}

int sigdelset(sigset_t *s, int sig) {
    if (!s || sig <= 0 || sig >= NSIG) return -1;
    *s &= ~(1UL << sig);
    return 0;
}

int sigismember(const sigset_t *s, int sig) {
    if (!s || sig <= 0 || sig >= NSIG) return -1;
    return (*s >> sig) & 1UL;
}
