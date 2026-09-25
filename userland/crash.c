// Deliberately faults, to prove the kernel isolates a userland crash: it must
// terminate this process and return to the shell, never panic the whole OS.
//
//   crash          dereference NULL -- an address belonging to nothing
//   crash stack    recurse until the stack runs out
//
// The second one matters more than it looks. The stack is eight megabytes of
// address space that is built a page at a time as it is used, so running off
// the bottom of it is not a fault by accident -- it is a fault because one
// page below the stack is deliberately left belonging to nobody. Without that
// page the stack would simply carry on into the top of the heap and quietly
// overwrite it, which is the kind of bug that surfaces somewhere else
// entirely, hours later.
#include <stdio.h>
#include <string.h>

static unsigned long deep(unsigned long n) {
    /* Big enough that this does not take all day, and touched so the compiler
     * cannot optimise the frame away. */
    volatile char pad[4096];
    memset((void *)pad, (int)n, sizeof pad);
    if (n % 256 == 0) printf("  %lu KiB down\n", n * sizeof pad / 1024);
    return pad[0] + deep(n + 1);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "stack") == 0) {
        printf("crash: walking down the stack until it ends...\n");
        printf("  (8 MiB of it, then a page that belongs to nobody)\n");
        deep(1);
        printf("crash: still alive?! (should never print)\n");
        return 0;
    }

    printf("crash: about to dereference NULL...\n");
    volatile int *p = (volatile int *)0;
    *p = 42;                 // page fault in ring 3
    printf("crash: still alive?! (should never print)\n");
    return 0;
}
