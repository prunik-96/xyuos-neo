#include "kio.h"
#include "../drivers/serial.h"
#include "../drivers/framebuffer.h"
#include <stdarg.h>
#include <stdint.h>

// A ring buffer of everything the kernel has logged, so the panic screen can
// show the last messages leading up to a crash ("recent kernel log").
#define KLOG_SIZE 8192
static char     klog[KLOG_SIZE];
static uint32_t klog_head = 0;   // total chars ever written; index = head % SIZE

void klog_snapshot(const char **buf, uint32_t *size, uint32_t *head) {
    *buf = klog;
    *size = KLOG_SIZE;
    *head = klog_head;
}

static void out_char(char c) {
    serial_putc(c);
    fb_putc(c);
    klog[klog_head % KLOG_SIZE] = c;
    klog_head++;
}

static void out_str(const char *s) {
    while (*s) {
        out_char(*s++);
    }
}

static void out_uint(uint64_t value, uint32_t base, int uppercase) {
    char buf[32];
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;

    if (value == 0) {
        out_char('0');
        return;
    }
    while (value > 0) {
        buf[i++] = digits[value % base];
        value /= base;
    }
    while (i > 0) {
        out_char(buf[--i]);
    }
}

static void out_int(int64_t value) {
    if (value < 0) {
        out_char('-');
        out_uint((uint64_t)(-value), 10, 0);
    } else {
        out_uint((uint64_t)value, 10, 0);
    }
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            out_char(*fmt++);
            continue;
        }
        fmt++;
        switch (*fmt) {
            case 's': out_str(va_arg(args, const char *)); break;
            case 'd': out_int(va_arg(args, int)); break;
            case 'u': out_uint(va_arg(args, unsigned int), 10, 0); break;
            case 'x': out_uint(va_arg(args, unsigned int), 16, 0); break;
            case 'c': out_char((char)va_arg(args, int)); break;
            case '%': out_char('%'); break;
            default:  out_char('%'); out_char(*fmt); break;
        }
        fmt++;
    }

    va_end(args);
}
