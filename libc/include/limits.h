#ifndef XYUOS_LIMITS_H
#define XYUOS_LIMITS_H

/* The compiler already knows how wide its own types are, and ships a
 * freestanding <limits.h> that says so -- CHAR_BIT, INT_MAX and the rest.
 * What it cannot know is the limits the C library imposes, because those are
 * ours to choose. So this pulls in the compiler's and adds what is missing.
 *
 * #include_next is the mechanism for exactly this: keep looking down the
 * include path for another header of the same name. Our directory is searched
 * before the compiler's, so this file is found first and GCC's is found
 * second. */

#include_next <limits.h>

/* The largest value a signed size can hold. POSIX puts it here; the C
 * standard does not, which is why the compiler's copy has no opinion. */
#ifndef SSIZE_MAX
#define SSIZE_MAX  __LONG_MAX__
#endif

/* How long a path may be. The filesystem's own limit, not the compiler's. */
#ifndef PATH_MAX
#define PATH_MAX   256
#endif
#ifndef NAME_MAX
#define NAME_MAX   255
#endif

/* Nothing here opens this many files at once, but ported code sizes arrays
 * with it. */
#ifndef OPEN_MAX
#define OPEN_MAX   32
#endif

#endif
