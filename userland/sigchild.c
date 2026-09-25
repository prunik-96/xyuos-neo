/* The other half of sigtest: a program to send signals TO.
 *
 *   sigchild catch    catches SIGTERM and exits 42
 *   sigchild ignore   ignores SIGTERM, so only SIGKILL ends it
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static volatile sig_atomic_t asked_to_stop;

static void on_term(int sig) {
    (void)sig;
    asked_to_stop = 1;
}

int main(int argc, char **argv) {
    const char *mode = (argc > 1) ? argv[1] : "catch";

    if (!strcmp(mode, "ignore")) signal(SIGTERM, SIG_IGN);
    else                         signal(SIGTERM, on_term);

    /* Wait to be signalled, without spinning. A signal cuts the sleep short,
     * so this comes round quickly when one arrives and costs nothing when
     * none does. */
    for (int i = 0; i < 600 && !asked_to_stop; i++) sleep_ms(100);

    return asked_to_stop ? 42 : 7;
}
