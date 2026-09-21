/* cp -- copy a file. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "upath.h"

static char buf[4096];

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: cp SOURCE DEST\n");
        return 1;
    }

    char src[UPATH_MAX], dst[UPATH_MAX];
    upath_resolve("/", argv[1], src);
    upath_resolve("/", argv[2], dst);

    if (strcmp(src, dst) == 0) {
        printf("cp: '%s' and '%s' are the same file\n", argv[1], argv[2]);
        return 1;
    }

    int in = open(src, 0);
    if (in < 0) { printf("cp: cannot open '%s'\n", argv[1]); return 1; }

    /* No O_TRUNC in this VFS: remove and recreate so the destination does not
     * keep a tail of its old contents. */
    xyuos_unlink(dst);
    if (xyuos_create(dst) != 0) {
        printf("cp: cannot create '%s'\n", argv[2]);
        close(in);
        return 1;
    }
    int out = open(dst, 0);
    if (out < 0) {
        printf("cp: cannot open '%s' for writing\n", argv[2]);
        close(in);
        return 1;
    }

    long n;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, n) != n) {
            printf("cp: write failed (disk full?)\n");
            close(in); close(out);
            return 1;
        }
    }
    close(in);
    close(out);
    return 0;
}
