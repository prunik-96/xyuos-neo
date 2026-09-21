/* kill -- interrupt a process by pid.
 *
 * There is one signal in this OS: terminate. Ctrl+C in a pane does the same
 * thing to the foreground process; this reaches one by pid, from any pane. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: kill PID...\n");
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        int pid = (int)strtol(argv[i], NULL, 10);
        if (pid <= 0) {
            printf("kill: not a pid: %s\n", argv[i]);
            status = 1;
            continue;
        }
        if (kill(pid) != 0) {
            printf("kill: no process %d\n", pid);
            status = 1;
        }
    }
    return status;
}
