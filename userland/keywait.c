/* Proves that blocking on input no longer stalls everyone else.
 *
 * Spawns a spinner, then sits in a read-key syscall. The spinner's output has
 * to keep appearing while this process is asleep -- with the old in-kernel
 * `sti; hlt` spin it would have stopped dead, because the timer does not
 * preempt ring 0. */

#include <stdio.h>
#include <unistd.h>

int main(void) {
    char *args[] = { "spin", "background" };

    int pid = spawn("/bin/spin", args, 2);
    if (pid < 0) {
        printf("[keywait] spawn failed\n");
        return 1;
    }

    printf("[keywait] sleeping on the keyboard, spinner is pid %d\n", pid);
    char c = xyuos_readkey();
    printf("[keywait] woke up on '%c'\n", c);

    int code = waitpid(pid);
    printf("[keywait] spinner exited with %d\n", code);
    return 0;
}
