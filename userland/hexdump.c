/* hexdump -- bytes in hex and ASCII. */

#include "uio.h"

int main(int argc, char **argv) {
    struct ureader r;
    if (ureader_open(&r, (argc > 1) ? argv[1] : NULL) != 0) {
        printf("hexdump: cannot open '%s'\n", argv[1]);
        return 1;
    }

    unsigned char row[16];
    long offset = 0;
    int n;

    for (;;) {
        for (n = 0; n < 16; n++) {
            int c = ureader_getc(&r);
            if (c < 0) break;
            row[n] = (unsigned char)c;
        }
        if (n == 0) break;

        printf("%08lx  ", offset);
        for (int i = 0; i < 16; i++) {
            if (i < n) printf("%02x ", row[i]);
            else       printf("   ");
            if (i == 7) printf(" ");
        }
        printf(" |");
        for (int i = 0; i < n; i++)
            putchar((row[i] >= 32 && row[i] < 127) ? row[i] : '.');
        printf("|\n");

        offset += n;
        if (n < 16) break;
    }
    ureader_close(&r);
    return 0;
}
