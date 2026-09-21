/* What the kernel thinks each network request cost.
 *
 * The browser measures a page from the outside: type an address, wait, look.
 * That number includes everything -- parsing, layout, drawing, and the time
 * the program spent not asking. This prints the other half: for each request
 * the kernel actually made, when it started and how long it took inside the
 * kernel. The difference between the two totals is the time that went
 * somewhere other than the network, and it is the only way to tell those
 * apart without guessing.
 */

#include <stdio.h>
#include <unistd.h>

#define ROWS 64

int main(void) {
    static struct net_log_ent ents[ROWS];
    int n = net_log(ents, ROWS);

    if (n <= 0) {
        printf("no requests have been made yet\n");
        return 0;
    }

    unsigned first = ents[0].start_ms, last = 0;
    unsigned long total = 0;

    printf("%-6s %-8s %-7s %s\n", "at", "took", "bytes", "what");
    for (int i = 0; i < n; i++) {
        struct net_log_ent *e = &ents[i];
        unsigned end = e->start_ms + e->dur_ms;
        if (e->start_ms < first) first = e->start_ms;
        if (end > last) last = end;
        total += e->dur_ms;

        printf("%5ums %5ums %7d  %s%s\n",
               e->start_ms - ents[0].start_ms, e->dur_ms, e->bytes,
               e->host, e->path);
    }

    unsigned span = last - first;
    printf("\n%d request(s) over %ums; %lums of that was inside the kernel"
           " (%lu%%)\n", n, span, total,
           span ? (total * 100 / span) : 0);
    printf("the rest is time the program did not spend asking.\n");
    return 0;
}
