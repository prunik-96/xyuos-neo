/* kill -- send a signal to a process by pid.
 *
 * The default is SIGTERM, which a program may catch and put its things away
 * before ending. `kill -9` is SIGKILL, which it may not. Ctrl+C in a pane
 * sends SIGINT to whatever is in the foreground there; this reaches any
 * process by number, from any pane.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static const struct { const char *name; int sig; } names[] = {
    { "HUP", SIGHUP },   { "INT", SIGINT },   { "QUIT", SIGQUIT },
    { "ILL", SIGILL },   { "ABRT", SIGABRT }, { "FPE", SIGFPE },
    { "KILL", SIGKILL }, { "USR1", SIGUSR1 }, { "SEGV", SIGSEGV },
    { "USR2", SIGUSR2 }, { "PIPE", SIGPIPE }, { "ALRM", SIGALRM },
    { "TERM", SIGTERM }, { "CHLD", SIGCHLD }, { "CONT", SIGCONT },
    { "STOP", SIGSTOP }, { "TSTP", SIGTSTP },
    { 0, 0 }
};

/* "-9", "-KILL", "-SIGKILL" -- all the spellings people actually type. */
static int parse_signal(const char *s) {
    if (s[0] >= '0' && s[0] <= '9') {
        int n = (int)strtol(s, NULL, 10);
        return (n > 0 && n < NSIG) ? n : -1;
    }
    if (!strncmp(s, "SIG", 3)) s += 3;
    for (int i = 0; names[i].name; i++)
        if (!strcasecmp(s, names[i].name)) return names[i].sig;
    return -1;
}

static void list_signals(void) {
    for (int i = 0; names[i].name; i++)
        printf("%2d %-5s%s", names[i].sig, names[i].name,
               (i % 5 == 4) ? "\n" : " ");
    printf("\n");
}

int main(int argc, char **argv) {
    int sig = SIGTERM;
    int first = 1;

    if (argc > 1 && !strcmp(argv[1], "-l")) { list_signals(); return 0; }

    if (argc > 1 && argv[1][0] == '-' && argv[1][1]) {
        sig = parse_signal(argv[1] + 1);
        if (sig < 0) {
            printf("kill: no such signal: %s\n", argv[1] + 1);
            return 1;
        }
        first = 2;
    }

    if (first >= argc) {
        printf("usage: kill [-SIGNAL] PID...\n");
        printf("       kill -l          list the signal names\n");
        return 1;
    }

    int status = 0;
    for (int i = first; i < argc; i++) {
        int pid = (int)strtol(argv[i], NULL, 10);
        if (pid <= 0) {
            printf("kill: not a pid: %s\n", argv[i]);
            status = 1;
            continue;
        }
        if (kill(pid, sig) != 0) {
            printf("kill: no process %d\n", pid);
            status = 1;
        }
    }
    return status;
}
