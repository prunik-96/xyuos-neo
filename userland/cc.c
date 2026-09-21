/* cc -- the C compiler driver.
 *
 * TinyCC does the actual work; this fills in everything specific to xyuOS so
 * that `cc hello.c` is all a user has to type:
 *
 *   -nostdlib          our libc is not glibc and has no crt1/crti/crtn
 *   /lib/crt0.o        our startup file, which calls main(argc, argv)
 *   /lib/libc.a        our libc
 *   -Wl,-Ttext=...     programs must land in the per-process user window
 *                      (USER_LOAD_BASE, see kernel/mm/vmm.h); tcc's default
 *                      base of 0x400000 is outside it and the loader would
 *                      refuse the image
 *
 * Anything the user passes is inserted before the libraries, so ordinary flags
 * (-I, -D, -O, -o) work as expected. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_ARGS 64

/* Must match USER_LOAD_BASE in kernel/mm/vmm.h and userland/link.ld. */
#define TEXT_BASE "-Wl,-Ttext=0x40400000"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: cc [options] file.c [-o output]\n");
        printf("Compiles for xyuOS: links /lib/crt0.o and /lib/libc.a,\n");
        printf("headers come from /include.\n");
        return 1;
    }

    char *args[MAX_ARGS];
    int n = 0;

    args[n++] = "tcc";
    args[n++] = "-nostdlib";
    args[n++] = "-static";
    args[n++] = TEXT_BASE;
    args[n++] = "/lib/crt0.o";

    /* The user's own arguments, in order. */
    for (int i = 1; i < argc && n < MAX_ARGS - 2; i++) {
        args[n++] = argv[i];
    }

    /* libc goes last: a static archive only resolves symbols already
     * referenced by the objects to its left. */
    args[n++] = "/lib/libc.a";

    if (n >= MAX_ARGS) {
        printf("cc: too many arguments\n");
        return 1;
    }

    int pid = spawn("/bin/tcc", args, n);
    if (pid < 0) {
        printf("cc: cannot start /bin/tcc\n");
        return 1;
    }
    return waitpid(pid);
}
