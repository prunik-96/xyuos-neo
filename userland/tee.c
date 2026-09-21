/* tee -- copy stdin to stdout and to a file.
 *
 * Note this only makes sense mid-pipeline, and the shell's pipes are
 * file-backed: `a | tee f | b` still runs the stages one after another. */

#include "uio.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: tee FILE\n");
        return 1;
    }

    char path[UPATH_MAX];
    upath_resolve("/", argv[1], path);
    xyuos_unlink(path);
    if (xyuos_create(path) != 0) {
        printf("tee: cannot create '%s'\n", argv[1]);
        return 1;
    }
    int fd = open(path, 0);
    if (fd < 0) {
        printf("tee: cannot open '%s'\n", argv[1]);
        return 1;
    }

    struct ureader r;
    ureader_open(&r, NULL);

    char buf[1024];
    int n = 0;
    int c;
    while ((c = ureader_getc(&r)) >= 0) {
        buf[n++] = (char)c;
        putchar(c);
        if (n == (int)sizeof(buf)) { write(fd, buf, n); n = 0; }
    }
    if (n > 0) write(fd, buf, n);

    close(fd);
    return 0;
}
