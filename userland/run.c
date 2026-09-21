/* run -- compile a C file and execute it in one step.
 *
 *   run /demo.c one two
 *
 * There is no magic here: it invokes cc to build a real executable in /tmp,
 * runs that, and removes it. tcc's own `-run` (compile straight to memory)
 * would avoid the temporary file, but that path is compiled out of our tcc --
 * it sits behind TCC_IS_NATIVE, which we do not define -- so this uses the
 * ordinary compile-to-disk route that the loader already handles.
 *
 * The exit status is the program's, so `run x.c; echo $?` behaves. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define OUT "/tmp/run.elf"
#define MAX_ARGS 32

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: run FILE.c [args...]\n");
        return 1;
    }

    char *cc_args[5];
    cc_args[0] = "cc";
    cc_args[1] = argv[1];
    cc_args[2] = "-o";
    cc_args[3] = OUT;

    int pid = spawn("/bin/cc", cc_args, 4);
    if (pid < 0) {
        printf("run: cannot start the compiler\n");
        return 1;
    }
    int rc = waitpid(pid);
    if (rc != 0) {
        /* cc has already printed the diagnostics. */
        return rc;
    }

    /* argv[0] is the source name, as a compiled program would see it, and the
     * user's own arguments follow. */
    char *args[MAX_ARGS];
    int n = 0;
    args[n++] = argv[1];
    for (int i = 2; i < argc && n < MAX_ARGS; i++) args[n++] = argv[i];

    pid = spawn(OUT, args, n);
    if (pid < 0) {
        printf("run: compiled, but could not start %s\n", OUT);
        xyuos_unlink(OUT);
        return 1;
    }
    rc = waitpid(pid);

    xyuos_unlink(OUT);
    return rc;
}
