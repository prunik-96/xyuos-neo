/* The small part of the C++ runtime that belongs to the system rather than to
 * the language.
 *
 * Everything else moved out. operator new and delete, __cxa_pure_virtual and
 * the whole of exceptions and typeinfo now come from libsupc++, which is the
 * compiler's own and knows things a stub cannot fake -- an operator new that
 * returns null where the standard says it must throw std::bad_alloc is the
 * kind of shortcut that only shows up on the day memory runs out.
 *
 * What is left here is the two things libsupc++ expects the system to supply.
 */
#include <stdlib.h>
#include <stddef.h>

/* An object at file scope registers its destructor here when it is built.
 * This used to return 0 and do nothing, which is a lie a program cannot see:
 * the destructors simply never ran. They run now, from exit(), in the reverse
 * of the order the objects were constructed.
 *
 * The third argument identifies the shared object being torn down. There are
 * none here -- every program is one static image -- so it is ignored. */
extern "C" int __libc_atexit_arg(void (*fn)(void *), void *arg);

extern "C" int __cxa_atexit(void (*fn)(void *), void *arg, void *dso) {
    (void)dso;
    return __libc_atexit_arg(fn, arg);
}

/* The address that identifies this image to __cxa_atexit. On a system with
 * shared libraries the loader provides it; here there is exactly one image,
 * so any stable address will do and its value is never looked at. */
void *__dso_handle = nullptr;
