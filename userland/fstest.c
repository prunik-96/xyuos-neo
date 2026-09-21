// fstest - boot demo that exercises the seek/stat/truncate syscalls from
// userland, i.e. the whole path a hosted stdio (and later tcc) will use.

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <setjmp.h>

static jmp_buf jb;
static int jump_depth = 0;
static volatile long canary = 0;

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

// Burns a few stack frames so longjmp has something real to unwind.
static void deep_call(int depth) {
    canary = 0xC0FFEE;              // lives in a callee-saved register
    if (depth > 0) { deep_call(depth - 1); return; }
    longjmp(jb, 7);
}

static int passed = 0;
static int failed = 0;

static void check(int cond, const char *label) {
    printf("  %s %s\n", cond ? "PASS" : "FAIL", label);
    if (cond) passed++; else failed++;
}

int main(int argc, char **argv) {
    printf("userland fs syscall test:\n");

    // argv arrives on the stack in the System V layout set up by the kernel
    printf("  seen: argc=%d argv[0]=%s argv[1]=%s\n",
           argc,
           argv && argv[0] ? argv[0] : "(null)",
           argv && argv[1] ? argv[1] : "(null)");
    if (argc != 1) {                 // launched with extra args: just report
        printf("  (multi-arg run, skipping the single-arg assertions)\n");
        return 0;
    }
    check(argc == 1, "argc is 1");
    check(argv != NULL && argv[0] != NULL, "argv[0] is present");
    check(argv[0] && strcmp(argv[0], "fstest") == 0, "argv[0] is the program name");
    check(argv[1] == NULL, "argv is NULL-terminated");

    struct xyuos_stat st;
    check(xyuos_stat("/hello.txt", &st) == 0, "stat existing file");
    check(st.size == 123, "stat reports size 123");
    check(st.is_dir == 0, "regular file is not a dir");
    check(xyuos_stat("/", &st) == 0 && st.is_dir == 1, "root is a dir");
    check(xyuos_stat("/definitely-missing", &st) != 0, "missing path fails");

    long fd = xyuos_open("/hello.txt");
    check(fd >= 0, "open");
    char buf[16];
    xyuos_read(fd, buf, 5);
    check(xyuos_seek(fd, 0, SEEK_CUR) == 5, "read advanced the cursor");
    check(xyuos_seek(fd, 0, SEEK_SET) == 0, "seek SET");
    check(xyuos_seek(fd, 0, SEEK_END) == 123, "seek END equals size");
    check(xyuos_seek(fd, -4, SEEK_END) == 119, "seek END-4");
    check(xyuos_read(fd, buf, 4) == 4, "read the last 4 bytes");
    xyuos_close(fd);

    // truncate round trip on a scratch file
    xyuos_unlink("/utest.bin");
    check(xyuos_create("/utest.bin") == 0, "create scratch file");
    fd = xyuos_open("/utest.bin");
    char data[300];
    for (int i = 0; i < 300; i++) data[i] = (char)('a' + (i % 26));
    check(xyuos_writefd(fd, data, 300) == 300, "wrote 300 bytes");
    check(xyuos_ftruncate(fd, 50) == 0, "ftruncate to 50");
    check(xyuos_seek(fd, 0, SEEK_CUR) == 50, "cursor clamped after truncate");
    xyuos_close(fd);
    check(xyuos_stat("/utest.bin", &st) == 0 && st.size == 50, "size is now 50");
    xyuos_unlink("/utest.bin");

    // ---- stdio: formatting ----
    printf("stdio test:\n");
    char b[64];

    int n = snprintf(b, sizeof b, "%d/%s/%x", 42, "abc", 255);
    check(n == 9 && strcmp(b, "42/abc/ff") == 0, "snprintf basic");

    snprintf(b, sizeof b, "[%05d][%-5d][%5s]", 42, 7, "hi");
    check(strcmp(b, "[00042][7    ][   hi]") == 0, "snprintf width and padding");

    n = snprintf(b, 5, "abcdefgh");
    check(n == 8 && strcmp(b, "abcd") == 0, "snprintf truncates but returns full length");

    snprintf(b, sizeof b, "%ld %lu %c %%", (long)-5, (unsigned long)7, 'Z');
    check(strcmp(b, "-5 7 Z %") == 0, "snprintf long/char/percent");

    sprintf(b, "%s=%d", "n", -12);
    check(strcmp(b, "n=-12") == 0, "sprintf");

    // ---- stdio: files ----
    FILE *f = fopen("/stdio.txt", "w");
    check(f != NULL, "fopen w");
    fprintf(f, "line %d\n", 1);
    fputs("line 2\n", f);
    fclose(f);

    f = fopen("/stdio.txt", "r");
    check(f != NULL, "fopen r");
    char line[64];
    check(fgets(line, sizeof line, f) != NULL && strcmp(line, "line 1\n") == 0, "fgets reads a line");
    check(ftell(f) == 7, "ftell after one line");
    int c = fgetc(f);
    check(c == 'l', "fgetc");
    ungetc(c, f);
    check(fgetc(f) == 'l', "ungetc pushes back");
    rewind(f);
    check(ftell(f) == 0, "rewind");
    fseek(f, 0, SEEK_END);
    check(ftell(f) == 14, "fseek END equals total length");
    fclose(f);

    f = fopen("/stdio.txt", "w");         // must truncate, not append
    fclose(f);
    check(xyuos_stat("/stdio.txt", &st) == 0 && st.size == 0, "fopen w truncates");

    f = fopen("/stdio.txt", "a");
    fputs("xyz", f);
    fclose(f);
    check(xyuos_stat("/stdio.txt", &st) == 0 && st.size == 3, "fopen a appends");
    xyuos_unlink("/stdio.txt");

    fprintf(stderr, "  (this line came through fprintf(stderr))\n");

    // ---- string / stdlib additions ----
    printf("string+stdlib test:\n");
    check(strcmp(strrchr("a/b/c", '/'), "/c") == 0, "strrchr");
    check(strcmp(strstr("hello world", "wor"), "world") == 0, "strstr");
    check(strstr("abc", "zz") == NULL, "strstr miss");
    check(memchr("abcdef", 'd', 6) != NULL, "memchr");
    char *dup = strdup("copy me");
    check(dup && strcmp(dup, "copy me") == 0, "strdup");
    free(dup);
    check(strtol("  -42abc", NULL, 10) == -42, "strtol decimal + whitespace + sign");
    check(strtol("0x1f", NULL, 0) == 31, "strtol auto-detects hex");
    char *endp;
    strtol("77rest", &endp, 10);
    check(strcmp(endp, "rest") == 0, "strtol end pointer");
    check(abs(-5) == 5 && labs(-7L) == 7L, "abs/labs");
    check(getenv("PATH") == NULL, "getenv returns unset");

    int arr[6] = { 5, 3, 9, 1, 4, 2 };
    qsort(arr, 6, sizeof(int), cmp_int);
    check(arr[0] == 1 && arr[2] == 3 && arr[5] == 9, "qsort");

    check(isdigit('7') && isalpha('x') && isspace(' ') && !isalpha('7'), "ctype predicates");
    check(toupper('a') == 'A' && tolower('Z') == 'z', "ctype case conversion");

    // ---- setjmp / longjmp ----
    printf("setjmp test:\n");
    int r = setjmp(jb);
    if (r == 0) {
        check(1, "setjmp returns 0 on the direct call");
        jump_depth++;
        deep_call(3);                       // unwinds several frames back here
        check(0, "unreachable: longjmp did not transfer control");
    } else {
        check(r == 7, "setjmp returns the value passed to longjmp");
        check(jump_depth == 1, "control came back through setjmp exactly once");
        check(canary == 0xC0FFEE, "work done before the longjmp is still visible");
    }

    // ---- heap growth via sbrk ----
    printf("heap test:\n");
    void *b0 = sbrk(0);
    check(b0 != (void *)-1, "sbrk(0) reports the break");
    void *b1 = sbrk(4096);
    check(b1 == b0, "sbrk returns the previous break");
    check(sbrk(0) == (char *)b0 + 4096, "break advanced by the increment");
    unsigned char *fresh = (unsigned char *)b1;
    int zeroed = 1;
    for (int i = 0; i < 4096; i++) if (fresh[i] != 0) { zeroed = 0; break; }
    check(zeroed, "sbrk memory comes back zeroed");

    // well past the old 2 MiB static arena
    char *big = (char *)malloc(8 * 1024 * 1024);
    check(big != NULL, "malloc 8 MiB (old arena was only 2 MiB)");
    if (big) {
        big[0] = 'a';
        big[8 * 1024 * 1024 - 1] = 'z';
        check(big[0] == 'a' && big[8 * 1024 * 1024 - 1] == 'z', "8 MiB block is fully usable");
        free(big);
    }

    // reuse after free, and realloc that has to move
    char *p1 = (char *)malloc(1000);
    strcpy(p1, "keep me");
    char *p2 = (char *)realloc(p1, 200000);
    check(p2 && strcmp(p2, "keep me") == 0, "realloc preserves contents when it moves");
    free(p2);

    void *many[64];
    int all = 1;
    for (int i = 0; i < 64; i++) { many[i] = malloc(4096); if (!many[i]) all = 0; }
    for (int i = 0; i < 64; i++) free(many[i]);
    check(all, "64 x 4 KiB allocations all succeeded");
    char *after = (char *)malloc(100000);
    check(after != NULL, "allocation after freeing everything reuses the heap");
    free(after);

    /* malloc MUST return 16-byte aligned memory. The compiler emits
     * movaps/movdqa for struct copies and those raise #GP on a misaligned
     * address -- they do not merely run slowly. This went unnoticed for a long
     * time because nothing in the test suite copied a struct through a
     * malloc'd pointer; tcc did, and panicked the machine. */
    {
        int aligned = 1;
        void *keep[24];
        for (int i = 0; i < 24; i++) {
            /* deliberately awkward sizes, so a block that merely happens to
             * be aligned cannot hide a broken header size */
            keep[i] = malloc((size_t)(i * 7 + 1));
            if (!keep[i] || ((unsigned long)keep[i] & 15u)) aligned = 0;
        }
        check(aligned, "every malloc result is 16-byte aligned");
        for (int i = 0; i < 24; i++) free(keep[i]);

        void *r = malloc(10);
        r = realloc(r, 5000);
        check(r != NULL && ((unsigned long)r & 15u) == 0, "realloc result is 16-byte aligned");
        free(r);

        void *c = calloc(7, 13);
        check(c != NULL && ((unsigned long)c & 15u) == 0, "calloc result is 16-byte aligned");
        free(c);
    }

    /* printf %f/%e/%g and the scanf family. Checked through sscanf/snprintf
     * rather than the terminal, so this stays a non-interactive test. */
    {
        char b[64];
        snprintf(b, sizeof(b), "%.2f", 3.14159);
        check(strcmp(b, "3.14") == 0, "printf %f rounds to the precision asked");
        snprintf(b, sizeof(b), "%.0f", 2.5);
        check(strcmp(b, "3") == 0, "printf %f rounds half up");
        snprintf(b, sizeof(b), "%.3f", -0.5);
        check(strcmp(b, "-0.500") == 0, "printf %f keeps the sign");
        snprintf(b, sizeof(b), "%.2f", 99.999);
        check(strcmp(b, "100.00") == 0, "printf %f carries into the integer part");
        snprintf(b, sizeof(b), "%.2e", 1234.5);
        check(strcmp(b, "1.23e+03") == 0, "printf %e uses scientific notation");
        snprintf(b, sizeof(b), "%8.2f|", 3.5);
        check(strcmp(b, "    3.50|") == 0, "printf %f honors the field width");

        int i = 0; double d = 0; char c = 0; char word[16];
        check(sscanf("42", "%d", &i) == 1 && i == 42, "sscanf %d");
        check(sscanf("  -17 ", "%d", &i) == 1 && i == -17, "sscanf %d skips space, keeps sign");
        check(sscanf("2.5", "%lf", &d) == 1 && d > 2.49 && d < 2.51, "sscanf %lf");
        check(sscanf("  x", " %c", &c) == 1 && c == 'x', "sscanf %c after a space directive");
        check(sscanf("hello world", "%s", word) == 1 && strcmp(word, "hello") == 0,
              "sscanf %s stops at whitespace");
        int a2 = 0, b2 = 0;
        check(sscanf("7 8", "%d %d", &a2, &b2) == 2 && a2 == 7 && b2 == 8,
              "sscanf reads two values");
        check(sscanf("ff", "%x", &i) == 1 && i == 255, "sscanf %x");
        check(sscanf("abc", "%d", &i) == 0, "sscanf reports no conversion");
    }

    printf("userland fs test: %d passed, %d failed\n", passed, failed);
    return 0;
}
