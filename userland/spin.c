/* Scheduler demo: burns CPU between prints so the timer has to preempt it.
 *
 * Run several of these at once and the output interleaves -- which is the
 * whole proof that preemption works, since nothing in here ever yields. */

#include <stdio.h>

int main(int argc, char **argv) {
    const char *name = (argc > 1) ? argv[1] : "spin";

    for (int i = 0; i < 5; i++) {
        printf("[%s] step %d\n", name, i);
        /* Long enough to span several 100 Hz ticks. */
        for (volatile long d = 0; d < 20000000L; d++) { }
    }

    printf("[%s] done\n", name);
    return 0;
}
