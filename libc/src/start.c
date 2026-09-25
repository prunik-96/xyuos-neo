/* What happens between the kernel jumping to _start and main() running.
 *
 * It used to be nothing: crt0 called main and that was the whole of it. Two
 * things that a C++ program cannot do without were therefore quietly missing,
 * and neither of them announces itself -- a program with them absent does not
 * fail to link, it just behaves as if the code had never been written.
 *
 *   Objects at file scope were never constructed. The compiler emits their
 *   constructors as a list of function pointers in .init_array and expects
 *   somebody to walk it. Nobody did.
 *
 *   The unwinder could not find anything. On a system with a dynamic loader
 *   it asks the loader where the exception tables are; there is no loader
 *   here, so the program has to tell it, once, before anything can throw.
 */
#include <stdlib.h>

extern int main(int argc, char **argv);

/* Laid out by userland/link.ld -- and WEAK, because that is not the only
 * linker that builds programs for this system. A program compiled on the
 * machine goes through tcc, which lays sections out its own way: it makes
 * __init_array_start and __init_array_end, and knows nothing about
 * __eh_frame_start. A weak symbol nobody defines is zero rather than a link
 * error, so the same crt0 and the same libc serve both. */
extern void (*__init_array_start[])(void) __attribute__((weak));
extern void (*__init_array_end[])(void) __attribute__((weak));
extern const char __eh_frame_start[] __attribute__((weak));

/* libgcc's, and WEAK on purpose.
 *
 * A weak reference that nothing satisfies resolves to zero instead of pulling
 * the archive member in, so a C program -- which has no use for an unwinder
 * -- does not carry one. A program that can throw drags the unwinder in by
 * other means, and then this address is real and the registration happens. */
extern void __register_frame_info(const void *begin, void *object)
    __attribute__((weak));

/* libgcc's `struct object`, whose definition lives in a header libgcc keeps
 * to itself. It is seven pointers wide in every configuration GCC has
 * shipped; this is twice that, and nothing here ever looks inside it. */
static unsigned long long eh_object[16];

void __libc_start(int argc, char **argv) __attribute__((noreturn));
void __libc_start(int argc, char **argv) {
    /* Before the constructors, because a constructor may throw. */
    if (__register_frame_info && __eh_frame_start)
        __register_frame_info(__eh_frame_start, eh_object);

    if (__init_array_start && __init_array_end)
        for (void (**fn)(void) = __init_array_start; fn != __init_array_end; fn++)
            (*fn)();

    /* exit(), not _exit(): what the constructors registered on the way up has
     * to run on the way down. */
    exit(main(argc, argv));
}
