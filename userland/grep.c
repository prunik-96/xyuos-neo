/* grep -- print lines containing a pattern.
 *
 * Plain substring matching, not regular expressions: a half-working regex
 * engine would be worse than an honest literal search, because the failures
 * would be silent and surprising. */

#include "uio.h"
#include <tty.h>

static int opt_invert, opt_count, opt_number, opt_icase;

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

static int contains(const char *hay, const char *needle) {
    if (!*needle) return 1;
    for (int i = 0; hay[i]; i++) {
        int k = 0;
        while (needle[k]) {
            char a = hay[i + k], b = needle[k];
            if (opt_icase) { a = lower(a); b = lower(b); }
            if (a != b) break;
            k++;
        }
        if (!needle[k]) return 1;
    }
    return 0;
}

static int scan(struct ureader *r, const char *pat, const char *label,
                int show_label) {
    char line[ULINE_MAX];
    int lineno = 0, matches = 0;

    while (ureader_line(r, line, sizeof(line)) >= 0) {
        lineno++;
        int hit = contains(line, pat);
        if (opt_invert) hit = !hit;
        if (!hit) continue;

        matches++;
        if (opt_count) continue;
        if (show_label) printf("%s:", label);
        if (opt_number) printf("%d:", lineno);
        printf("%s\n", line);
    }
    if (opt_count) {
        if (show_label) printf("%s:", label);
        printf("%d\n", matches);
    }
    return matches;
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
                case 'v': opt_invert = 1; break;
                case 'c': opt_count = 1; break;
                case 'n': opt_number = 1; break;
                case 'i': opt_icase = 1; break;
                default:
                    printf("grep: unknown option -%c\n", *o);
                    return 2;
            }
        }
    }

    if (i >= argc) {
        printf("usage: grep [-vcni] PATTERN [file...]\n");
        return 2;
    }
    const char *pat = argv[i++];

    struct ureader r;
    int found = 0;

    if (i >= argc) {
        ureader_open(&r, NULL);
        found = scan(&r, pat, "", 0);
        return found ? 0 : 1;
    }

    int many = (argc - i) > 1;
    for (; i < argc; i++) {
        if (ureader_open(&r, argv[i]) != 0) {
            printf("grep: cannot open '%s'\n", argv[i]);
            continue;
        }
        found += scan(&r, pat, argv[i], many);
        ureader_close(&r);
    }
    return found ? 0 : 1;
}
