#ifndef ALLOCA_H
#define ALLOCA_H

/* Space on the stack, released when the function returns.
 *
 * There is no library routine behind this and there cannot be: the memory has
 * to come from the caller's own frame, which only the compiler can arrange.
 * So the header exists purely to give the name somewhere to live -- the
 * compiler does the work.
 *
 * Nothing here should reach for it. It exists because ported code does, and
 * because a variable-length array is the same thing said in standard C. */

#define alloca(n) __builtin_alloca(n)

#endif
