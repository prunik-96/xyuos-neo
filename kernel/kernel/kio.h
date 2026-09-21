#ifndef KIO_H
#define KIO_H

// Minimal printf-style logger: writes to both the serial port and the
// graphical framebuffer console. Supports %s %d %u %x %c %%.
void kprintf(const char *fmt, ...);

// Hand out the kernel log ring buffer (a raw circular char buffer). `head` is
// the running total of characters written; the valid window is the last
// min(head, size) chars ending at index (head-1) % size. Used by the panic
// screen to show the messages that preceded a crash.
#include <stdint.h>
void klog_snapshot(const char **buf, uint32_t *size, uint32_t *head);

#endif
