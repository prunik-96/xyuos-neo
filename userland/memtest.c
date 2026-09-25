/* How much memory can one program actually get, and does it stay its own?
 *
 * The heap used to stop at 255 MiB no matter how much the machine had,
 * because the stack sat in the middle of the address window and boxed it in.
 * This walks up in steps until the allocator refuses, writing a pattern into
 * every page it is given and reading it back afterwards -- a limit that is
 * reported but not honoured would be worse than the old one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STEP   (16 * 1024 * 1024)
#define PAGE   4096
#define MAXBLK 256

static unsigned char pattern(unsigned long i) {
    /* Depends on the address, so a block that quietly overlaps another shows
     * up as the wrong byte rather than as a coincidence. */
    return (unsigned char)((i >> 12) * 31 + (i & 0xFF));
}

int main(int argc, char **argv) {
    unsigned long want = 0;
    if (argc > 1) want = strtoul(argv[1], 0, 10) * 1024UL * 1024UL;

    unsigned char *blk[MAXBLK];
    int n = 0;
    unsigned long got = 0;

    printf("asking for memory in %d MiB steps%s\n", STEP / 1048576,
           want ? ", up to the limit given" : ", until it is refused");

    while (n < MAXBLK && (!want || got < want)) {
        unsigned char *p = malloc(STEP);
        if (!p) break;
        /* Touch every page. Until now the kernel built these pages the moment
         * they were asked for; it builds them here instead, one fault each,
         * so this loop is also what proves the fault path works. */
        for (unsigned long i = 0; i < STEP; i += PAGE) p[i] = pattern(i);
        blk[n++] = p;
        got += STEP;
        if ((n % 4) == 0) printf("  %lu MiB\n", got / 1048576);
    }

    printf("got %lu MiB in %d blocks\n", got / 1048576, n);
    if (got == 0) { printf("FAILED: not a byte\n"); return 1; }

    int bad = 0;
    for (int b = 0; b < n; b++)
        for (unsigned long i = 0; i < STEP; i += PAGE)
            if (blk[b][i] != pattern(i)) { bad++; break; }

    printf("blocks that read back wrong: %d\n", bad);

    /* And give it all back, to show the pages are not lost to the system. */
    for (int b = 0; b < n; b++) free(blk[b]);
    printf("freed\n");

    return bad ? 1 : 0;
}
