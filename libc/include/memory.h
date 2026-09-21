/* <memory.h> is <string.h> under an older name.
 *
 * System V put the mem* functions here and the str* ones in <string.h>. The
 * C standard merged them into <string.h> in 1989 and never mentioned this
 * file again -- but glibc still ships it, so code written against glibc
 * still includes it, and that code is otherwise perfectly portable.
 *
 * There is nothing to implement. Anything that would go here is already in
 * string.h, and having this file forward to it is the whole of what every
 * other system does.
 */
#ifndef MEMORY_H
#define MEMORY_H

#include <string.h>

#endif
