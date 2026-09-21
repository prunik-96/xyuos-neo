/* wc -- count lines, words and bytes. */

#include "uio.h"

static void count(struct ureader *r, const char *label, int show_label,
                  long *tl, long *tw, long *tb) {
    long lines = 0, words = 0, bytes = 0;
    int c, in_word = 0;

    while ((c = ureader_getc(r)) >= 0) {
        bytes++;
        if (c == '\n') lines++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') in_word = 0;
        else if (!in_word) { in_word = 1; words++; }
    }
    printf("%8ld %8ld %8ld", lines, words, bytes);
    if (show_label) printf(" %s", label);
    printf("\n");

    *tl += lines; *tw += words; *tb += bytes;
}

int main(int argc, char **argv) {
    long tl = 0, tw = 0, tb = 0;
    struct ureader r;

    if (argc < 2) {
        ureader_open(&r, NULL);
        count(&r, "", 0, &tl, &tw, &tb);
        return 0;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (ureader_open(&r, argv[i]) != 0) {
            printf("wc: cannot open '%s'\n", argv[i]);
            status = 1;
            continue;
        }
        count(&r, argv[i], 1, &tl, &tw, &tb);
        ureader_close(&r);
    }
    if (argc > 2) printf("%8ld %8ld %8ld total\n", tl, tw, tb);
    return status;
}
