/* Signals, of which this system delivers none.
 *
 * Portable programs ask to ignore one on their way in -- most often the one
 * that would otherwise kill them when a connection goes away mid-write. That
 * request is granted here trivially and truthfully: nothing will interrupt
 * them, because nothing here interrupts anything.
 */
#ifndef SIGNAL_H
#define SIGNAL_H

typedef int sig_atomic_t;
typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGABRT  6
#define SIGFPE   8
#define SIGKILL  9
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15

#ifdef __cplusplus
extern "C" {
#endif

/* Remembers what was asked for and hands back what was asked for last time,
 * so a program that saves and restores a handler gets its own back. Nothing
 * is ever delivered. */
sighandler_t signal(int sig, sighandler_t handler);

/* There is nobody to raise it to. */
int raise(int sig);

#ifdef __cplusplus
}
#endif

#endif
