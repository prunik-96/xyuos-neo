#ifndef PANIC_H
#define PANIC_H

#include "../arch/x86_64/idt.h"

// Paint a full-screen kernel panic report: the exception, a full register
// dump, the faulting context, and the last kernel log lines. Draws straight to
// the framebuffer and presents it; the caller halts afterwards.
void panic_screen(const char *reason, struct interrupt_frame *frame);

// Stop the machine over a broken kernel invariant -- something that must never
// happen and that no user program can cause. Says what went wrong on the
// serial port and the kernel log, and halts this core for good. There is no
// frame to show, which is why it is not panic_screen.
void panic(const char *msg) __attribute__((noreturn));

#endif
