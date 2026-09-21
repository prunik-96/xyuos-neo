/* head -- print the first lines of the input. */

#include "uio.h"

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

    struct ureader r;
    char line[ULINE_MAX];

    if (i >= argc) {
        ureader_open(&r, NULL);
        for (int k = 0; k < n && ureader_line(&r, line, sizeof(line)) >= 0; k++)
            printf("%s\n", line);
        return 0;
    }

    int many = (argc - i) > 1;
    for (int first = 1; i < argc; i++, first = 0) {
        if (ureader_open(&r, argv[i]) != 0) {
            printf("head: cannot open '%s'\n", argv[i]);
            continue;
        }
        if (many) printf("%s==> %s <==\n", first ? "" : "\n", argv[i]);
        for (int k = 0; k < n && ureader_line(&r, line, sizeof(line)) >= 0; k++)
            printf("%s\n", line);
        ureader_close(&r);
    }
    return 0;
}
