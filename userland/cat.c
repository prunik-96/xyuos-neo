/* cat -- copy files, or standard input, to standard output.
 *
 * Reading stdin when given no file is what makes it useful on the right-hand
 * side of a pipe. */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include "upath.h"

static char buf[1024];

static int copy_fd(int fd) {
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++) putchar(buf[i]);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return copy_fd(0);      /* stdin */

    int status = 0;
    for (int i = 1; i < argc; i++) {
        char path[UPATH_MAX];
        upath_resolve("/", argv[i], path);

        int fd = open(path, 0);
        if (fd < 0) {
            printf("cat: cannot open '%s'\n", path);
            status = 1;
            continue;
        }
        copy_fd(fd);
        close(fd);
    }
    return status;
}
