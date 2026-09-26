/* What the kernel thinks each network request cost.
 *
 * The browser measures a page from the outside: type an address, wait, look.
 * That number includes everything -- parsing, layout, drawing, and the time
 * the program spent not asking. This prints the other half: for each request
 * the kernel actually made, when it started and how long it took inside the
 * kernel. The difference between the two totals is the time that went
 * somewhere other than the network, and it is the only way to tell those
 * apart without guessing.
 *
 * `netlog -c` empties the log, so that what it shows next is one page.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define ROWS 64

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-c") == 0) {
        net_log_reset();
        printf("the request log is empty\n");
        return 0;
    }

    static struct net_log_ent ents[ROWS];
    int n = net_log(ents, ROWS);

    if (n <= 0) {
        printf("no requests have been made yet\n");
        return 0;
    }

    /* The log is written as requests finish, and with several running at once
     * one can finish after another that started later. So times count from
     * the earliest start, not from the first line. */
    unsigned first = ents[0].start_ms, last = 0;
    for (int i = 0; i < n; i++) {
        unsigned end = ents[i].start_ms + ents[i].dur_ms;
        if (ents[i].start_ms < first) first = ents[i].start_ms;
        if (end > last) last = end;
    }

    printf("%-6s %-8s %-7s %s\n", "at", "took", "bytes", "what");
    for (int i = 0; i < n; i++) {
        struct net_log_ent *e = &ents[i];
        printf("%5ums %5ums %7d  %s%s\n",
               e->start_ms - first, e->dur_ms, e->bytes, e->host, e->path);
    }

    /* How long at least one request was running. Adding the durations up
     * stopped meaning anything once four could run at once -- a Wikipedia
     * page came out at 259% of the time it took. So the requests are taken
     * in order of starting, and each counts only for the time it runs past
     * the ones before it. */
    static int order[ROWS];
    for (int i = 0; i < n; i++) {
        int j = i;
        while (j > 0 && ents[order[j - 1]].start_ms > ents[i].start_ms) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }
    unsigned long busy = 0;
    unsigned reach = first;
    for (int k = 0; k < n; k++) {
        struct net_log_ent *e = &ents[order[k]];
        unsigned from = e->start_ms > reach ? e->start_ms : reach;
        unsigned end = e->start_ms + e->dur_ms;
        if (end > from) { busy += end - from; reach = end; }
    }

    unsigned span = last - first;
    printf("\n%d request(s) over %ums; the network was busy for %lums of it"
           " (%lu%%)\n", n, span, busy, span ? (busy * 100 / span) : 0);
    printf("the rest is time the program did not spend asking.\n");
    return 0;
}
