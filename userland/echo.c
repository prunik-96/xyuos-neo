/* echo -- print the arguments, space separated.
 *
 * The shell has already expanded $VAR and stripped quotes by the time these
 * arrive, so there is nothing clever to do here. */

#include <stdio.h>

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        printf("%s%s", argv[i], (i + 1 < argc) ? " " : "");
    }
    printf("\n");
    return 0;
}
