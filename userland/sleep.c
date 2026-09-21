/* sleep -- wait, without burning the CPU.
 *
 * The process is genuinely descheduled (see WAIT_TIME in the kernel), so the
 * others keep running while it waits. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: sleep SECONDS\n");
        return 1;
    }

    /* Accept fractions: sleep 0.5 is more useful than a whole second. */
    double secs = strtod(argv[1], NULL);
    if (secs < 0) secs = 0;
    unsigned int ms = (unsigned int)(secs * 1000.0);

    sleep_ms(ms);
    return 0;
}
