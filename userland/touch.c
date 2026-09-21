/* touch -- create empty files. There are no timestamps in this ext2 layer to
 * update, so an existing file is left completely alone. */

#include <stdio.h>
#include <unistd.h>
#include "upath.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: touch FILE...\n");
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        char path[UPATH_MAX];
        upath_resolve("/", argv[i], path);

        struct xyuos_stat st;
        if (xyuos_stat(path, &st) == 0) continue;   /* already exists */

        if (xyuos_create(path) != 0) {
            printf("touch: cannot create '%s'\n", argv[i]);
            status = 1;
        }
    }
    return status;
}
