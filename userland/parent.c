/* P4 demo: a userland program that starts other programs and collects their
 * exit codes -- the thing a shell has to be able to do.
 *
 * The interesting part is the middle: after spawning, this process blocks in
 * waitpid() INSIDE a syscall while the children keep running. That only works
 * because each process has its own kernel stack. */

#include <stdio.h>
#include <unistd.h>

int main(void) {
    printf("[parent] spawning two children\n");

    char *a[] = { "spin", "kid-A" };
    char *b[] = { "spin", "kid-B" };

    int pid_a = spawn("/bin/spin", a, 2);
    int pid_b = spawn("/bin/spin", b, 2);

    if (pid_a < 0 || pid_b < 0) {
        printf("[parent] spawn failed (%d, %d)\n", pid_a, pid_b);
        return 1;
    }
    printf("[parent] children are pid %d and pid %d\n", pid_a, pid_b);

    /* Deliberately waited for in the opposite order to the one they finish in,
     * to prove waitpid() targets a specific child rather than just the first
     * corpse it finds. */
    int code_b = waitpid(pid_b);
    printf("[parent] pid %d exited with %d\n", pid_b, code_b);

    int code_a = waitpid(pid_a);
    printf("[parent] pid %d exited with %d\n", pid_a, code_a);

    int none = waitpid(-1);
    printf("[parent] waiting with no children left returns %d\n", none);

    printf("[parent] done\n");
    return 7;
}
