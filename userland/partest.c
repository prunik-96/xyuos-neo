/* partest -- does a program's work actually run on more than one core?
 *
 * The same work is done twice: first by one thread, then split between N.
 * If the threads really run at the same moment, the second pass takes about
 * a N-th of the time. If they only take turns on one core it takes as long
 * as the first, or a little longer -- and that is exactly what this showed
 * before the scheduler ran on every core.
 *
 * The answer has to be the same both times, or the speed means nothing.
 *
 *   partest [threads] [limit]
 */
#include <stdio.h>
#include <stdlib.h>
#include <thread.h>
#include <unistd.h>

#define MAX_THREADS 32

static unsigned long limit = 3000000;
static int nthreads = 8;
static volatile unsigned long counts[MAX_THREADS];

static int is_prime(unsigned long x) {
    if (x < 2) return 0;
    for (unsigned long d = 2; d * d <= x; d++) if (x % d == 0) return 0;
    return 1;
}

/* Thread `me` of `of` takes every of-th number: the big numbers, which cost
 * the most, are spread evenly instead of landing on whoever got the top
 * of the range. */
static unsigned long count_band(int me, int of) {
    unsigned long c = 0;
    for (unsigned long x = (unsigned long)me; x < limit; x += (unsigned long)of)
        c += is_prime(x);
    return c;
}

static void worker(void *arg) {
    int me = (int)(long)arg;
    counts[me] = count_band(me, nthreads);
}

int main(int argc, char **argv) {
    if (argc > 1) nthreads = atoi(argv[1]);
    if (nthreads < 1) nthreads = 1;
    if (nthreads > MAX_THREADS) nthreads = MAX_THREADS;
    if (argc > 2) limit = strtoul(argv[2], NULL, 10);

    printf("counting primes below %lu: one thread, then %d\n", limit, nthreads);

    unsigned int t0 = uptime_ms();
    unsigned long single = count_band(0, 1);
    unsigned int t1 = uptime_ms();

    int tid[MAX_THREADS];
    for (int i = 0; i < nthreads; i++) {
        tid[i] = thread_create(worker, (void *)(long)i);
        if (tid[i] < 0) { printf("FAILED: thread %d did not start\n", i); return 1; }
    }
    for (int i = 0; i < nthreads; i++) thread_join(tid[i]);
    unsigned int t2 = uptime_ms();

    unsigned long multi = 0;
    for (int i = 0; i < nthreads; i++) multi += counts[i];

    unsigned int ms1 = t1 - t0, msn = t2 - t1;
    printf("1 thread     %6u ms   %lu primes\n", ms1, single);
    printf("%2d threads   %6u ms   %lu primes\n", nthreads, msn, multi);
    if (msn) {
        unsigned int x10 = ms1 * 10 / msn;
        printf("speedup      %u.%ux\n", x10 / 10, x10 % 10);
    }

    if (single != multi) {
        printf("FAILED: the two answers differ\n");
        return 1;
    }
    printf("same answer both ways\n");
    return 0;
}
