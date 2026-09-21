/* mv -- rename a file. Same-filesystem only: there is one filesystem. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "upath.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: mv SOURCE DEST\n");
        return 1;
    }

    char src[UPATH_MAX], dst[UPATH_MAX];
    upath_resolve("/", argv[1], src);
    upath_resolve("/", argv[2], dst);

    if (strcmp(src, dst) == 0) return 0;

    if (xyuos_rename(src, dst) != 0) {
        printf("mv: cannot move '%s' to '%s'\n", argv[1], argv[2]);
        return 1;
    }
    return 0;
}
