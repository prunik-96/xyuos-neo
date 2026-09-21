/* free -- physical memory in use.
 *
 * "total" is the RAM the firmware reported as usable, not the 4 GiB span the
 * frame allocator's bitmap covers. "used" therefore includes the kernel image
 * itself, its heap, and every page handed to a running process. */

#include <stdio.h>
#include <unistd.h>
#include "xyuos_syscall.h"

int main(void) {
    struct si_mem m;

    if (xyuos_sysinfo(SI_MEM, &m, sizeof(m)) < 0) {
        printf("free: cannot read memory information\n");
        return 1;
    }

    unsigned long used = m.total_frames - m.free_frames;
    unsigned long kb = m.page_size / 1024;

    printf("%-10s %10s %10s %10s\n", "", "total", "used", "free");
    printf("%-10s %9luK %9luK %9luK\n", "memory:",
           m.total_frames * kb, used * kb, m.free_frames * kb);
    printf("%-10s %10lu %10lu %10lu\n", "frames:",
           m.total_frames, used, m.free_frames);
    printf("page size: %lu bytes\n", m.page_size);
    return 0;
}
