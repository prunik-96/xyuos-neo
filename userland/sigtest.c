/* Signals: that they arrive, that the program carries on afterwards exactly
 * where it was, and that the ones which are not supposed to be catchable are
 * not catchable.
 *
 * The test with teeth is the fourth. A handler that arrives in the middle of
 * arithmetic and does its own work must not disturb a single register of the
 * code it interrupted -- so the loop below keeps a running sum in whatever
 * registers the compiler chose, takes several hundred signals while it runs,
 * and checks the sum at the end. A saved-context bug shows up there as a
 * wrong number and nowhere else.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <setjmp.h>
#include <thread.h>

static volatile sig_atomic_t got_usr1;
static volatile sig_atomic_t got_usr2;
static volatile sig_atomic_t got_alrm;
static volatile sig_atomic_t got_int;
static volatile sig_atomic_t last_signo;

static void note(int sig) {
    last_signo = sig;
    switch (sig) {
        case SIGUSR1: got_usr1++; break;
        case SIGUSR2: got_usr2++; break;
        case SIGALRM: got_alrm++; break;
        case SIGINT:  got_int++;  break;
    }
}

/* --- the arithmetic the handler must not disturb ------------------------- */

static volatile sig_atomic_t ticks;

/* Deliberately does real work of its own, in both kinds of register: it has to
 * clobber the caller-saved integer registers AND the floating-point ones, or
 * it would not be testing anything. printf inside a handler does exactly this
 * without meaning to, which is why it matters. */
static volatile double fp_junk;
static void busy_handler(int sig) {
    (void)sig;
    volatile long junk = 0;
    for (int i = 0; i < 200; i++) junk += i * 3;
    double d = 1.0;
    for (int i = 1; i < 60; i++) d = d * 1.000001 + (double)i / 7.0;
    fp_junk = d;
    ticks++;
}

/* Both kinds of accumulator, so a lost integer register and a lost xmm each
 * show up. Not inlined away: the whole point is that the values live in
 * registers across the additions. */
static void one_pass(long *isum, double *fsum) {
    long a = *isum;
    double d = *fsum;
    for (long i = 1; i <= 100000; i++) { a += (i % 7) + 1; d += 1.0 / (double)i; }
    *isum = a;
    *fsum = d;
}

static jmp_buf escape;

static void segv_handler(int sig) {
    (void)sig;
    longjmp(escape, 1);
}

int main(void) {
    /* volatile because of the longjmp in test 6: a local that is live across
     * setjmp may otherwise be kept in a register the jump puts back. */
    static volatile int bad = 0;
    int me = thread_self();

    printf("sigtest: pid %d\n", me);

    /* --- 1. a signal sent to yourself arrives, and arrives before kill()
     *        comes back ------------------------------------------------- */
    if (signal(SIGUSR1, note) == SIG_ERR) { printf("FAILED: signal(USR1)\n"); return 1; }
    raise(SIGUSR1);
    if (got_usr1 != 1) { printf("FAILED: SIGUSR1 did not arrive (%d)\n", (int)got_usr1); bad++; }
    else printf("ok: raise(SIGUSR1) -> handler ran once\n");

    /* --- 2. ignoring one, and putting the handler back ------------------- */
    sighandler_t was = signal(SIGUSR1, SIG_IGN);
    if (was != note) { printf("FAILED: signal did not hand back the old handler\n"); bad++; }
    raise(SIGUSR1);
    if (got_usr1 != 1) { printf("FAILED: an ignored signal still arrived\n"); bad++; }
    else printf("ok: SIG_IGN drops it\n");
    signal(SIGUSR1, note);

    /* --- 3. held off, then let through ----------------------------------- */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR2);
    signal(SIGUSR2, note);
    sigprocmask(SIG_BLOCK, &block, &old);
    raise(SIGUSR2);
    if (got_usr2 != 0) { printf("FAILED: a blocked signal was delivered\n"); bad++; }
    else printf("ok: blocked, and waiting\n");
    sigprocmask(SIG_UNBLOCK, &block, NULL);
    if (got_usr2 != 1) { printf("FAILED: it did not arrive when unblocked (%d)\n", (int)got_usr2); bad++; }
    else printf("ok: unblocked -> delivered at once\n");

    /* --- 4. the registers of the interrupted code ------------------------ */
    signal(SIGALRM, busy_handler);

    long sum = 0, passes = 0;
    double fsum = 0.0;
    ticks = 0;
    alarm_ms(10);
    unsigned int began = uptime_ms();
    while (uptime_ms() - began < 500) {     /* half a second of arithmetic */
        one_pass(&sum, &fsum);
        passes++;
        alarm_ms(10);                       /* keep them coming */
    }
    alarm_ms(0);
    signal(SIGALRM, SIG_IGN);

    /* The same work again, exactly as many times, with nothing interrupting
     * it. Same additions in the same order, so the answer must match to the
     * last bit -- there is no rounding argument to hide a lost register
     * behind. */
    long rsum = 0;
    double rfsum = 0.0;
    for (long i = 0; i < passes; i++) one_pass(&rsum, &rfsum);

    if (sum != rsum) {
        printf("FAILED: the sum came out %ld, not %ld -- a handler ate a register\n",
               sum, rsum);
        bad++;
    } else {
        printf("ok: %d signals through %ld passes, sum still exactly %ld\n",
               (int)ticks, passes, sum);
    }
    if (fsum != rfsum) {
        printf("FAILED: the floating-point sum is %.12f, not %.12f -- a handler ate an xmm\n",
               fsum, rfsum);
        bad++;
    } else {
        printf("ok: and the floating-point sum, bit for bit (%.9f)\n", fsum);
    }
    if (ticks == 0) { printf("FAILED: no alarm arrived at all\n"); bad++; }

    /* --- 5. a sleep is cut short by a signal ----------------------------- */
    signal(SIGALRM, note);
    got_alrm = 0;
    alarm_ms(100);
    unsigned int t0 = uptime_ms();
    sleep_ms(3000);
    unsigned int spent = uptime_ms() - t0;
    if (got_alrm != 1) { printf("FAILED: the alarm did not arrive\n"); bad++; }
    else if (spent > 1500) { printf("FAILED: the sleep ran on for %u ms\n", spent); bad++; }
    else printf("ok: a 3000 ms sleep ended after %u ms, interrupted\n", spent);

    /* --- 6. catching a fault --------------------------------------------- */
    signal(SIGSEGV, segv_handler);
    if (setjmp(escape) == 0) {
        /* Inside the window, so the address is allowed to exist -- but
         * nothing was ever put there. */
        volatile int *nowhere = (volatile int *)0x40000000;
        *nowhere = 1;
        printf("FAILED: writing to nothing did not fault\n");
        bad++;
    } else {
        printf("ok: caught SIGSEGV and got out by longjmp\n");
    }
    /* Jumping out of a handler skips the return the kernel arranged, so the
     * signal is still held off -- that is why POSIX has siglongjmp. Undo it
     * by hand. */
    sigemptyset(&block);
    sigaddset(&block, SIGSEGV);
    sigprocmask(SIG_UNBLOCK, &block, NULL);
    signal(SIGSEGV, SIG_DFL);

    /* --- 7. what cannot be caught ---------------------------------------- */
    if (signal(SIGKILL, note) != SIG_ERR) { printf("FAILED: SIGKILL was catchable\n"); bad++; }
    else printf("ok: SIGKILL refuses a handler\n");
    if (signal(SIGSTOP, note) != SIG_ERR) { printf("FAILED: SIGSTOP was catchable\n"); bad++; }
    else printf("ok: SIGSTOP refuses a handler\n");

    /* --- 8. a signal to somebody who is not there ------------------------ */
    if (kill(31337, SIGTERM) == 0) { printf("FAILED: signalled a process that does not exist\n"); bad++; }
    else printf("ok: no such process\n");
    if (kill(me, 0) != 0) { printf("FAILED: signal 0 says I do not exist\n"); bad++; }
    else printf("ok: signal 0 finds a live process\n");

    /* --- 9. a child that catches SIGTERM, and one that does not ---------- */
    {
        char *const argv1[] = { (char *)"/bin/sigchild", (char *)"catch" };
        int pid = spawn("/bin/sigchild", argv1, 2);
        if (pid < 0) { printf("FAILED: cannot start sigchild\n"); bad++; }
        else {
            sleep_ms(200);                 /* let it install its handler */
            kill(pid, SIGTERM);
            int code = waitpid(pid);
            if (code != 42) { printf("FAILED: the catching child exited %d, not 42\n", code); bad++; }
            else printf("ok: a child caught SIGTERM and chose its own exit code\n");
        }

        char *const argv2[] = { (char *)"/bin/sigchild", (char *)"ignore" };
        pid = spawn("/bin/sigchild", argv2, 2);
        if (pid < 0) { printf("FAILED: cannot start sigchild\n"); bad++; }
        else {
            sleep_ms(200);
            kill(pid, SIGTERM);            /* ignored */
            sleep_ms(200);
            kill(pid, SIGKILL);            /* not ignorable */
            int code = waitpid(pid);
            if (code != 128 + SIGKILL) { printf("FAILED: SIGKILL gave exit %d, not %d\n", code, 128 + SIGKILL); bad++; }
            else printf("ok: SIGTERM ignored, SIGKILL still ended it (%d)\n", code);
        }
    }

    (void)last_signo;
    (void)got_int;
    printf(bad ? "FAILED\n" : "all good\n");
    return bad ? 1 : 0;
}
