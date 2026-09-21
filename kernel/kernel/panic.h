#ifndef PANIC_H
#define PANIC_H

#include "../arch/x86_64/idt.h"

// Paint a full-screen kernel panic report: the exception, a full register
// dump, the faulting context, and the last kernel log lines. Draws straight to
// the framebuffer and presents it; the caller halts afterwards.
void panic_screen(const char *reason, struct interrupt_frame *frame);

#endif
