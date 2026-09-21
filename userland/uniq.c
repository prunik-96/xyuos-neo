/* uniq -- collapse adjacent identical lines (like the real one, it only
 * compares neighbours, so the input usually wants sorting first). */

#include "uio.h"

int main(int argc, char **argv) {
    int opt_count = 0, opt_dup = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
                case 'c': opt_count = 1; break;
                case 'd': opt_dup = 1; break;
                default:
                    printf("uniq: unknown option -%c\n", *o);
                    return 2;
            }
        }
    }

    struct ureader r;
    if (ureader_open(&r, (i < argc) ? argv[i] : NULL) != 0) {
        printf("uniq: cannot open '%s'\n", argv[i]);
        return 1;
    }

    char cur[ULINE_MAX], prev[ULINE_MAX];
    int have = 0, run = 0;

    while (ureader_line(&r, cur, sizeof(cur)) >= 0) {
        if (have && strcmp(cur, prev) == 0) { run++; continue; }
        if (have && (!opt_dup || run > 1)) {
            if (opt_count) printf("%4d %s\n", run, prev);
            else           printf("%s\n", prev);
        }
        strcpy(prev, cur);
        have = 1;
        run = 1;
    }
    if (have && (!opt_dup || run > 1)) {
        if (opt_count) printf("%4d %s\n", run, prev);
        else           printf("%s\n", prev);
    }
    ureader_close(&r);
    return 0;
}
