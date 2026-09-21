#ifndef MICROPY_INCLUDED_XYUOS_MPHALPORT_H
#define MICROPY_INCLUDED_XYUOS_MPHALPORT_H

#include <stdint.h>
#include <errno.h>

/* The bridge to the operating system: characters in, characters out, and a
 * clock. Everything else MicroPython needs it already has. */

static inline mp_uint_t mp_hal_ticks_cpu(void) { return 0; }

mp_uint_t mp_hal_ticks_ms(void);
mp_uint_t mp_hal_ticks_us(void);
void      mp_hal_delay_ms(mp_uint_t ms);
void      mp_hal_delay_us(mp_uint_t us);

/* Ported code expects to retry a system call that was interrupted. Nothing
 * here can be interrupted -- there are no signals -- so this is one attempt
 * and the error branch. `err` is in scope for the raise, which is what the
 * callers expect to find there. */
#define MP_HAL_RETRY_SYSCALL(ret, syscall, raise) \
    do {                                          \
        (ret) = (syscall);                        \
        if ((ret) == -1) {                        \
            int err = errno;                      \
            (void)err;                            \
            raise;                                \
        }                                         \
    } while (0)

void mp_hal_set_interrupt_char(int c);

int  mp_hal_stdin_rx_chr(void);
mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len);

#endif
