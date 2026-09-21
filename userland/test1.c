#include "syscalls.h"

void _start(void) {
    const char msg[] = "Hello from userland (ring3)! xyuOS Neo syscalls work.\n";
    sys_write(msg, sizeof(msg) - 1);
    sys_exit(42);
}
