/* Parse a page, run its scripts, and print what the page became.
 *
 * The same three files the browser uses -- dom.h, js.h, jsdom.h -- driven from
 * the host so a failure can be found in a second rather than a boot.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static void sink(const char *line);
static double clock_ms(void);

#include "../userland/js.h"
#include "../userland/jsdom.h"

static void sink(const char *line) { printf("[log] %s\n", line); }

static double clock_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (double)tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}
/* Milliseconds since this program started. Casting the wall clock straight to
 * an unsigned is undefined once it passes four billion, which it did in 1970. */
static double t_start;
static unsigned now_ms(void) { return (unsigned)(clock_ms() - t_start); }

/* What one element says, with the runs of whitespace squeezed. Scripts and
 * stylesheets are skipped, the same way the browser skips them when drawing --
 * printing a page's own source back is not what anyone wants to read. */
static int text_of(int node, char *out, int max, int n) {
    if (node < 0) return n;
    if (dom[node].kind == DOM_TEXT) {
        for (int i = 0; i < dom[node].textlen && n < max - 1; i++)
            out[n++] = dom[node].text[i];
        return n;
    }
    if (dom_ci_eq(dom[node].tag, "script") || dom_ci_eq(dom[node].tag, "style"))
        return n;
    for (int c = dom[node].first; c >= 0; c = dom[c].next)
        n = text_of(c, out, max, n);
    return n;
}

static void show(const char *label, const char *id) {
    static char buf[65536];
    int node = id ? dom_find_id(id) : 0;
    if (node < 0) { printf("%-10s <no element %s>\n", label, id); return; }
    int n = text_of(node, buf, (int)sizeof buf, 0);
    buf[n] = 0;
    printf("%-10s ", label);
    int sp = 1;
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            if (!sp) { putchar(' '); sp = 1; }
        } else { putchar(c); sp = 0; }
    }
    printf("\n");
}

static void print_text(void) {
    show("[heading]", "heading");
    show("[count]",   "count");
    show("[built]",   "built");
    show("[later]",   "later");
}

static void run_scripts(void) {
    for (int i = 0; i < dom_n; i++) {
        if (dom[i].kind != DOM_ELEM || !dom_ci_eq(dom[i].tag, "script")) continue;
        int c = dom[i].first;
        if (c < 0 || dom[c].kind != DOM_TEXT || dom[c].textlen <= 0) continue;
        if (!js_run(dom[c].text, dom[c].textlen))
            printf("[error] %s\n", js_error() ? js_error() : "unknown");
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: domtest page.html [click-selector]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc((size_t)n + 1);
    if (fread(src, 1, (size_t)n, f) != (size_t)n) return 2;
    src[n] = 0;
    fclose(f);

    t_start = clock_ms();
    if (!dom_init() || !js_init()) { fprintf(stderr, "no memory\n"); return 2; }
    js_console_sink = sink;
    js_clock_ms = clock_ms;
    jsd_now_hook = now_ms;

    dom_parse(src, (int)n);
    printf("[nodes] %d\n", dom_n);
    jsd_setup("http://test.invalid/page");
    run_scripts();
    print_text();

    /* A click, so the event path is exercised too. */
    for (int i = 2; i < argc; i++) {
        int node = -1;
        for (int k = 0; k < dom_n; k++)
            if (dom[k].kind == DOM_ELEM && jsd_matches(k, argv[i])) { node = k; break; }
        if (node < 0) { printf("[click] %s: no such element\n", argv[i]); continue; }
        printf("[click] %s\n", argv[i]);
        jsd_dispatch(node, "click");
        print_text();
    }

    /* And whatever the timers were waiting for. */
    /* Let the clock run forward so anything waiting on it comes due. */
    for (int round = 1; round <= 40; round++) jsd_run_timers(now_ms() + (unsigned)round * 50);
    print_text();
    fprintf(stderr, "arena %ld KB, dom %d nodes, edits %d bytes\n",
            js_memory_used() / 1024, dom_n, dom_editlen);
    return 0;
}
