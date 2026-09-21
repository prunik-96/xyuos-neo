/* Reading a line of input, with the backspace key working.
 *
 * In a file of its own rather than beside printf, because it is the one name
 * in this library that a ported program is likely to have its own version of.
 * Kept apart, the archive hands it over only to a program that actually calls
 * it, and a program that brings its own is left alone.
 */

#include <stdio.h>

size_t readline(char *buf, size_t max) {
    size_t len = 0;
    for (;;) {
        int c = getchar();
        if (c == '\n' || c == '\r') { putchar('\n'); break; }
        if (c == '\b' || c == 127) {
            if (len > 0) { len--; putchar('\b'); putchar(' '); putchar('\b'); }
            continue;
        }
        if (len + 1 < max) { buf[len++] = (char)c; putchar(c); }
    }
    buf[len] = '\0';
    return len;
}
