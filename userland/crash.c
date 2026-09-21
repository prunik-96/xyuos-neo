// Deliberately faults, to prove the kernel isolates a userland crash: it must
// terminate this process and return to the shell, never panic the whole OS.
#include <stdio.h>

int main(void) {
    printf("crash: about to dereference NULL...\n");
    volatile int *p = (volatile int *)0;
    *p = 42;                 // page fault in ring 3
    printf("crash: still alive?! (should never print)\n");
    return 0;
}
