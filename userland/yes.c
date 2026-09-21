/* yes -- print a line forever. The classic pipe test: `yes | head` must stop
 * once head has read enough, with the kernel terminating yes on the broken
 * pipe rather than letting it spin. */

#include <stdio.h>

int main(int argc, char **argv) {
    const char *s = (argc > 1) ? argv[1] : "y";
    for (;;) {
        printf("%s\n", s);
    }
    return 0;
}
