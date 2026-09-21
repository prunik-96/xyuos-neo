#include "vmm.h"

int vmm_user_range_ok(uint64_t addr, uint64_t len) {
    if (addr < USER_VIRT_BASE) return 0;
    if (addr >= USER_VIRT_END) return 0;
    // Checked as a subtraction rather than `addr + len > END` so that a huge
    // length cannot wrap the sum back into the valid window.
    if (len > USER_VIRT_END - addr) return 0;
    return 1;
}
