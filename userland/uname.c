/* uname -- print the system name. */

#include <stdio.h>
#include <unistd.h>
#include "xyuos_syscall.h"

int main(void) {
    char buf[64];
    if (xyuos_sysinfo(SI_UNAME, buf, sizeof(buf)) < 0) {
        printf("uname: cannot read system information\n");
        return 1;
    }
    printf("%s\n", buf);
    return 0;
}
