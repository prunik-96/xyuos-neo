/* Run a script through our interpreter on the host and print what it says.
 *
 * The test suite is written in JavaScript itself (tools/jstests/*.js): it
 * checks its own answers and prints the ones that came out wrong. That keeps
 * the expectations in the language they are about, and means running the suite
 * exercises the whole engine on the way to reading them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static void sink(const char *line);
static double clock_ms(void);

#include "../userland/js.h"

static void sink(const char *line) { printf("%s\n", line); }

static double clock_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (double)tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: jstest file.js\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc((size_t)n + 1);
    if (fread(src, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read failed\n"); return 2; }
    src[n] = 0;
    fclose(f);

    if (!js_init()) { fprintf(stderr, "no memory\n"); return 2; }
    js_console_sink = sink;
    js_clock_ms = clock_ms;

    int ok = js_run(src, (int)n);
    if (!ok) {
        printf("ERROR: %s\n", js_error() ? js_error() : "unknown");
        return 1;
    }
    fprintf(stderr, "arena: %ld KB\n", js_memory_used() / 1024);
    return 0;
}
