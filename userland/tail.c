/* tail -- print the last lines of the input.
 *
 * Keeps a ring of the last N lines, so it works on a pipe (where seeking to
 * the end is not possible) and never holds the whole input. */

#include "uio.h"

#define MAX_KEEP 128

static char ring[MAX_KEEP][ULINE_MAX];

int main(int argc, char **argv) {
    int n = 10;
    int i = 1;

    if (i < argc && argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') {
        n = 0;
        for (const char *p = argv[i] + 1; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
        i++;
    } else if (i + 1 < argc && strcmp(argv[i], "-n") == 0) {
        n = 0;
        for (const char *p = argv[i + 1]; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
        i += 2;
    }
    if (n > MAX_KEEP) n = MAX_KEEP;
    if (n < 1) n = 1;

    struct ureader r;
    if (ureader_open(&r, (i < argc) ? argv[i] : NULL) != 0) {
        printf("tail: cannot open '%s'\n", argv[i]);
        return 1;
    }

    int total = 0;
    while (ureader_line(&r, ring[total % n], ULINE_MAX) >= 0) total++;
    ureader_close(&r);

    int start = (total > n) ? total - n : 0;
    for (int k = start; k < total; k++) printf("%s\n", ring[k % n]);
    return 0;
}
