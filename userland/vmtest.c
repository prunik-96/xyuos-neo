/* Mappings: that they cost nothing until touched, and that the rights on
 * them are real.
 *
 * The last part is the one that matters for what comes later. A just-in-time
 * compiler writes machine code into memory and then runs it, and it must not
 * be able to do both at once -- memory that is writable must not be
 * executable, or a mistake anywhere in a browser becomes a way to run
 * anything. So: ask for memory that can be written, write the code, ask for
 * the same memory to be executable instead, and call it.
 *
 *   vmtest        map, write, unmap, and run code out of a mapping
 *   vmtest write  write to a mapping that is read-only   (must be stopped)
 *   vmtest exec   run code from a mapping that is not executable (ditto)
 */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define BIG (256UL * 1024 * 1024)
#define PAGE 4096

/* mov eax, 42 ; ret -- the smallest honest answer. */
static const unsigned char code[] = { 0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3 };

typedef int (*fn)(void);

static int fail(const char *what) { printf("FAILED: %s\n", what); return 1; }

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";

    if (strcmp(mode, "write") == 0) {
        unsigned char *p = mmap(0, PAGE, PROT_READ | PROT_WRITE,
                                MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED) return fail("mmap");
        p[0] = 1;                                  /* fine: still writable */
        if (mprotect(p, PAGE, PROT_READ) != 0) return fail("mprotect");
        printf("writing to a read-only mapping at %p...\n", (void *)p);
        p[0] = 2;                                  /* must not survive this */
        return fail("the write went through");
    }

    if (strcmp(mode, "exec") == 0) {
        unsigned char *p = mmap(0, PAGE, PROT_READ | PROT_WRITE,
                                MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED) return fail("mmap");
        memcpy(p, code, sizeof code);
        printf("running code from a writable mapping at %p...\n", (void *)p);
        fn f = (fn)(void *)p;
        printf("it returned %d\n", f());           /* must not get here */
        return fail("the code ran");
    }

    /* --- a big mapping costs nothing until it is touched ------------------ */
    printf("mapping %lu MiB...\n", BIG / 1048576);
    unsigned char *big = mmap(0, BIG, PROT_READ | PROT_WRITE,
                              MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (big == MAP_FAILED) return fail("mmap of 256 MiB");
    printf("  got it at %p\n", (void *)big);

    /* Touch one page near the start, one near the end. */
    big[0] = 0xAB;
    big[BIG - 1] = 0xCD;
    if (big[0] != 0xAB || big[BIG - 1] != 0xCD)
        return fail("what was written did not read back");
    printf("  wrote the first and last byte, and read them back\n");

    /* A fresh mapping reads as zero. */
    for (unsigned long i = PAGE; i < PAGE * 64; i += PAGE)
        if (big[i] != 0) return fail("a fresh mapping was not zero");
    printf("  and the rest of it is zero\n");

    if (munmap(big, BIG) != 0) return fail("munmap");
    printf("  unmapped\n");

    /* --- write code, then ask for it to be executable -------------------- */
    unsigned char *p = mmap(0, PAGE, PROT_READ | PROT_WRITE,
                            MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) return fail("mmap for code");
    memcpy(p, code, sizeof code);
    printf("wrote %d bytes of code at %p\n", (int)sizeof code, (void *)p);

    if (mprotect(p, PAGE, PROT_READ | PROT_EXEC) != 0)
        return fail("mprotect to executable");

    fn f = (fn)(void *)p;
    int got = f();
    printf("the code ran and returned %d\n", got);
    if (got != 42) return fail("it returned the wrong thing");

    /* --- and now the half that matters ----------------------------------- */
    /* Everything above shows that a mapping CAN be made executable. That is
     * only half of W^X and the easy half: the other half is that memory which
     * was not asked to be executable must refuse, or the separation is a
     * label rather than a rule.
     *
     * So the same page, with the same code already in it, is asked to be
     * writable instead -- and called. This is the last thing this program
     * does, because if the rule holds it does not come back. "stopped" on
     * the screen afterwards is the test passing. */
    if (mprotect(p, PAGE, PROT_READ | PROT_WRITE) != 0)
        return fail("mprotect back to writable");

    printf("all good so far\n");
    printf("now calling the same code with the page writable instead --\n");
    printf("this must NOT return, and the program must be stopped\n");
    got = f();
    printf("FAILED: it ran anyway and returned %d\n", got);
    return 1;
}
