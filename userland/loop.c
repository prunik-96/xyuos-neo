/* loop -- a program that never finishes on its own. For testing Ctrl+C.
 *
 * Two modes: a pure CPU spin (never enters the kernel, so only the timer can
 * interrupt it) or a loop that prints and sleeps (blocks in a syscall). Both
 * must be killable. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int quiet = (argc > 1 && strcmp(argv[1], "-q") == 0);

    if (quiet) {
        /* Pure ring-3 spin: makes no syscalls at all. */
        for (volatile unsigned long i = 0; ; i++) { }
    }

    for (unsigned long i = 0; ; i++) {
        printf("loop %lu\n", i);
        sleep_ms(300);
    }
    return 0;
}
