/* sort -- sort lines.
 *
 * Holds the whole input in memory, so it is bounded by MAX_LINES rather than
 * by the disk. That limit is reported rather than silently truncating. */

#include "uio.h"
#include <stdlib.h>

#define MAX_LINES 4096

static char *lines[MAX_LINES];
static int   count;
static int   opt_reverse, opt_unique, opt_numeric;

static long as_number(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return strtol(s, NULL, 10);
}

static int cmp(const void *a, const void *b) {
    const char *x = *(const char *const *)a;
    const char *y = *(const char *const *)b;
    int r;
    if (opt_numeric) {
        long nx = as_number(x), ny = as_number(y);
        r = (nx < ny) ? -1 : (nx > ny) ? 1 : 0;
    } else {
        r = strcmp(x, y);
    }
    return opt_reverse ? -r : r;
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
                case 'r': opt_reverse = 1; break;
                case 'u': opt_unique = 1; break;
                case 'n': opt_numeric = 1; break;
                default:
                    printf("sort: unknown option -%c\n", *o);
                    return 2;
            }
        }
    }

    struct ureader r;
    if (ureader_open(&r, (i < argc) ? argv[i] : NULL) != 0) {
        printf("sort: cannot open '%s'\n", argv[i]);
        return 1;
    }

    char line[ULINE_MAX];
    int overflow = 0;
    while (ureader_line(&r, line, sizeof(line)) >= 0) {
        if (count >= MAX_LINES) { overflow = 1; break; }
        lines[count] = strdup(line);
        if (!lines[count]) { overflow = 1; break; }
        count++;
    }
    ureader_close(&r);

    if (overflow) printf("sort: input larger than %d lines, sorting what fits\n", MAX_LINES);

    qsort(lines, count, sizeof(lines[0]), cmp);

    for (int k = 0; k < count; k++) {
        if (opt_unique && k > 0 && strcmp(lines[k], lines[k - 1]) == 0) continue;
        printf("%s\n", lines[k]);
    }
    return 0;
}
