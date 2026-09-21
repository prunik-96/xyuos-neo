/* ps -- list processes. */

#include <stdio.h>
#include <unistd.h>
#include "xyuos_syscall.h"

#define MAX_PROCS 32

static const char *state_name(int s) {
    switch (s) {
        case 1: return "ready";
        case 2: return "run";
        case 3: return "block";
        case 4: return "zombie";
        default: return "?";
    }
}

int main(void) {
    struct si_proc procs[MAX_PROCS];

    long n = xyuos_sysinfo(SI_PROCS, procs, sizeof(procs));
    if (n < 0) {
        printf("ps: cannot read the process table\n");
        return 1;
    }
    int count = (int)(n / (long)sizeof(struct si_proc));

    printf("%5s %5s %-8s %-4s %s\n", "PID", "PPID", "STATE", "TTY", "NAME");
    for (int i = 0; i < count; i++) {
        printf("%5d %5d %-8s %-4s %s\n",
               procs[i].pid, procs[i].parent_pid,
               state_name(procs[i].state),
               procs[i].pane ? "yes" : "-",
               procs[i].name);
    }
    return 0;
}
