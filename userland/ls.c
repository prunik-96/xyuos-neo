/* ls -- list a directory.
 *
 * A real program rather than a shell builtin, so that `ls > file` and
 * `ls | ...` work through the ordinary redirection machinery instead of
 * needing a special case inside the shell. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <tty.h>
#include "upath.h"

static char buf[8192];

/* Colors are raw attributes here, not escape sequences, so they would be lost
 * (not corrupt anything) in a redirected file -- but asking for them without a
 * pane is meaningless, so skip it. */
static int color;

static void show(const char *name, int is_dir, void *ctx) {
    (void)ctx;
    if (is_dir && color) tty_set_color(TC_BLUE, TC_BLACK);
    printf("%s%s\n", name, is_dir ? "/" : "");
    if (is_dir && color) tty_reset_color();
}

int main(int argc, char **argv) {
    const char *arg = (argc > 1) ? argv[1] : "/";
    char path[UPATH_MAX];
    upath_resolve("/", arg, path);

    int cols = 0, rows = 0;
    tty_size(&cols, &rows);
    color = (cols > 0 && rows > 0);

    long n = xyuos_listdir(path, buf, sizeof(buf));
    if (n <= 0) {
        printf("ls: cannot read '%s'\n", path);
        return 1;
    }
    upath_each_entry(buf, n, show, 0);
    return 0;
}
