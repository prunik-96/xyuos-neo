/* Threads: that they run, that they share what they should, that they have
 * their own of what they should, and that a lock actually keeps them apart.
 *
 * The counting test is the one with teeth. Each thread adds to the same
 * variable a hundred thousand times; if the additions were not protected the
 * total would come out short, because an increment is a read, an add and a
 * write, and a thread can lose the processor between any two of them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread.h>
#include <unistd.h>

static int THREADS = 4;               /* -- overridable, to push the limit */
static int ROUNDS = 100000;           /* -- fewer when there are many */

static mutex_t lock = MUTEX_INIT;
static volatile long guarded;       /* only ever touched under `lock` */
static volatile long unguarded;     /* deliberately not */

/* What each thread saw, written only by that thread. */
#define THREADS_MAX 40
static int  saw_tid[THREADS_MAX];
static void *saw_stack[THREADS_MAX];
static volatile int finished[THREADS_MAX];

static int shared_value = 1234;     /* in the program's memory, not a copy */
static int shared_seen[THREADS_MAX];

/* Nobody finishes until everybody has started.
 *
 * Without this the test asks the wrong question. Early threads finish while
 * later ones are still being created, their stacks go back, and the next
 * thread is quite correctly given the same address. Two threads sharing an
 * address is only wrong if they were ever alive at the same time -- so this
 * makes sure they all are. */
static volatile int all_started;

static void worker(void *p) {
    int me = (int)(long)p;
    int on_my_stack;                /* its address IS this thread's stack */

    saw_tid[me] = thread_self();
    saw_stack[me] = (void *)&on_my_stack;
    shared_seen[me] = shared_value;

    while (!all_started) { __asm__ volatile ("pause" ::: "memory"); }

    for (int i = 0; i < ROUNDS; i++) {
        mutex_lock(&lock);
        guarded++;
        mutex_unlock(&lock);
        unguarded++;
    }
    finished[me] = 1;
}

int main(int argc, char **argv) {
    if (argc > 1) THREADS = atoi(argv[1]);
    if (THREADS < 1) THREADS = 1;
    if (THREADS > THREADS_MAX) THREADS = THREADS_MAX;
    if (argc > 2) ROUNDS = atoi(argv[2]);
    if (ROUNDS < 1) ROUNDS = 1;
    int tid[THREADS_MAX];
    int bad = 0;

    printf("main thread is %d\n", thread_self());
    printf("starting %d threads, %d rounds each\n", THREADS, ROUNDS);

    for (int i = 0; i < THREADS; i++) {
        tid[i] = thread_create(worker, (void *)(long)i);
        if (tid[i] < 0) { printf("FAILED: could not start thread %d\n", i); return 1; }
    }

    /* Everyone is started; now they may all finish. Until this line every one
     * of them is alive at once, which is the only condition under which "they
     * must not share a stack" means anything. */
    all_started = 1;

    for (int i = 0; i < THREADS; i++)
        if (thread_join(tid[i]) != 0) { printf("FAILED: join %d\n", i); return 1; }

    printf("all joined\n");

    /* --- they all actually ran ------------------------------------------ */
    for (int i = 0; i < THREADS; i++)
        if (!finished[i]) { printf("FAILED: thread %d never finished\n", i); bad++; }

    /* --- each is its own thread, with its own stack ---------------------- */
    for (int i = 0; i < THREADS; i++) {
        if (saw_tid[i] != tid[i]) {
            printf("FAILED: thread %d called itself %d\n", tid[i], saw_tid[i]);
            bad++;
        }
        for (int j = i + 1; j < THREADS; j++) {
            long d = (long)saw_stack[i] - (long)saw_stack[j];
            if (d < 0) d = -d;
            if (d < 4096) {
                printf("FAILED: threads %d and %d share a stack\n", i, j);
                bad++;
            }
        }
    }
    printf("stacks: %p %p %p %p\n",
           saw_stack[0], saw_stack[1], saw_stack[2], saw_stack[3]);

    /* --- and they share the program's memory ----------------------------- */
    for (int i = 0; i < THREADS; i++)
        if (shared_seen[i] != shared_value) {
            printf("FAILED: thread %d saw %d, not %d\n",
                   i, shared_seen[i], shared_value);
            bad++;
        }
    printf("all four read the same shared variable\n");

    /* --- the count ------------------------------------------------------- */
    long want = (long)THREADS * (long)ROUNDS;
    printf("guarded:   %ld  (should be %ld)\n", guarded, want);
    printf("unguarded: %ld\n", unguarded);
    if (guarded != want) { printf("FAILED: the lock did not hold\n"); bad++; }

    if (unguarded == want)
        printf("(the unguarded count came out right this time -- on one core\n"
               " that can happen; it is not a promise)\n");
    else
        printf("(the unguarded count lost %ld, which is what a race looks like)\n",
               want - unguarded);

    printf(bad ? "FAILED\n" : "all good\n");
    return bad ? 1 : 0;
}
