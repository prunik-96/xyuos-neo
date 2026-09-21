/* count N -- print "line 1".."line N", fast. For filling a pane past its
 * height to test scrollback. */

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 20;
    for (int i = 1; i <= n; i++) printf("line %d\n", i);
    return 0;
}
