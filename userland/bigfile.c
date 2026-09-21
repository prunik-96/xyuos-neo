/* bigfile -- write a large file, read it back, verify every byte.
 *
 * A 1 MiB file is 1024 blocks at a 1 KiB block size, well past the 268-block
 * single-indirect limit, so it exercises double-indirect blocks. The pattern
 * is position-dependent, so a misplaced or zero-filled block is detected --
 * which is exactly how the old silent 268 KiB cap would have shown up. */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#define SIZE   (1024 * 1024)
#define CHUNK  4096

static unsigned char pat(long i) {
    return (unsigned char)(i ^ (i >> 8) ^ (i >> 16) ^ 0x5A);
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "/big.dat";
    static unsigned char buf[CHUNK];

    /* write */
    xyuos_unlink(path);
    if (xyuos_create(path) != 0) { printf("bigfile: create failed\n"); return 1; }
    int fd = open(path, 0);
    if (fd < 0) { printf("bigfile: open-for-write failed\n"); return 1; }

    long written = 0;
    while (written < SIZE) {
        for (int i = 0; i < CHUNK; i++) buf[i] = pat(written + i);
        long n = write(fd, buf, CHUNK);
        if (n != CHUNK) { printf("bigfile: short write at %ld (n=%ld)\n", written, n); close(fd); return 1; }
        written += CHUNK;
    }
    close(fd);
    printf("wrote %ld bytes to %s\n", written, path);

    /* stat: size must match */
    struct xyuos_stat st;
    if (xyuos_stat(path, &st) != 0) { printf("bigfile: stat failed\n"); return 1; }
    if ((long)st.size != SIZE) { printf("FAIL size %ld != %d\n", (long)st.size, SIZE); return 1; }

    /* read back and verify every byte */
    fd = open(path, 0);
    if (fd < 0) { printf("bigfile: open-for-read failed\n"); return 1; }
    long pos = 0, bad = 0, firstbad = -1;
    for (;;) {
        long n = read(fd, buf, CHUNK);
        if (n <= 0) break;
        for (long i = 0; i < n; i++) {
            if (buf[i] != pat(pos + i)) {
                if (firstbad < 0) firstbad = pos + i;
                bad++;
            }
        }
        pos += n;
    }
    close(fd);

    if (pos != SIZE) { printf("FAIL read back %ld of %d bytes\n", pos, SIZE); return 1; }
    if (bad) {
        printf("FAIL %ld mismatched bytes, first at offset %ld\n", bad, firstbad);
        return 1;
    }
    printf("PASS verified %ld bytes, all correct\n", pos);
    return 0;
}
