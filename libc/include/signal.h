/* Signals.
 *
 * This used to be a polite lie -- signal() remembered what it was given and
 * nothing was ever delivered, because there was nothing to deliver. There is
 * now: a handler really runs, on this program's own stack, at a point the
 * program did not choose, and the program is put back afterwards exactly as
 * it was.
 *
 * What a handler may do is the usual short list, and for the usual reason:
 * it runs BETWEEN two instructions of the program, so anything the
 * interrupted code was halfway through is halfway through underneath it.
 * Setting a volatile flag and returning is always safe. Calling longjmp out
 * of it is safe and is how a program survives a fault it caught. printf is
 * not, strictly, and works here in practice.
 */
#ifndef SIGNAL_H
#define SIGNAL_H

typedef int sig_atomic_t;
typedef unsigned long sigset_t;
typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

#define SIGHUP   1
#define SIGINT   2    /* Ctrl+C */
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9    /* cannot be caught, cannot be ignored */
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19    /* cannot be caught, cannot be ignored */
#define SIGTSTP 20
#define SIGWINCH 28
#define NSIG    32

/* sigprocmask */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#ifdef __cplusplus
extern "C" {
#endif

/* Install a handler, SIG_DFL or SIG_IGN. Returns what was there before, or
 * SIG_ERR for a signal that does not exist or cannot be caught. */
sighandler_t signal(int sig, sighandler_t handler);

/* Send one. `sig` of 0 sends nothing and only answers whether the process is
 * there. Returns 0, or -1 if it is not. */
int kill(int pid, int sig);

/* Send one to yourself. It is delivered before this returns. */
int raise(int sig);

/* SIGALRM after this many seconds (alarm) or milliseconds (alarm_ms). 0
 * cancels one that was set. Returns 0. Unlike POSIX, this does not report
 * how much of a previous alarm was left. */
unsigned int alarm(unsigned int seconds);
unsigned int alarm_ms(unsigned int ms);

/* Wait until a signal has been delivered. Always returns -1. */
int pause(void);

/* Hold signals off and let them through again. A signal sent while it is
 * held stays pending and arrives the moment it is let through. SIGKILL and
 * SIGSTOP cannot be held and are dropped from anything asked for. */
int sigprocmask(int how, const sigset_t *set, sigset_t *old);

int sigemptyset(sigset_t *s);
int sigfillset(sigset_t *s);
int sigaddset(sigset_t *s, int sig);
int sigdelset(sigset_t *s, int sig);
int sigismember(const sigset_t *s, int sig);

#ifdef __cplusplus
}
#endif

#endif
