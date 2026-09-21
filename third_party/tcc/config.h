/* Hand-written stand-in for what ./configure would generate, targeting
 * xyuOS Neo: x86_64, ELF, static, single-threaded. */
#define TCC_VERSION "0.9.28rc"
#define CONFIG_TCCDIR "/tcc"
#define CONFIG_TCC_STATIC 1

/* Single-process cooperative OS: there are no threads, so tcc's semaphore
 * locking is pure overhead (and would need semaphore.h, which we do not have). */
#define CONFIG_TCC_SEMLOCK 0

/* No dynamic loader, no shared libraries. */
#define TCC_TARGET_X86_64 1
#define CONFIG_TCC_BACKTRACE 0

/* Where the compiler looks when it runs ON xyuOS. The disk image lays these
 * out in the disk.img rule of the Makefile. */
/* Both directories, in this order. Setting this REPLACES tcc's built-in
 * default, which is where CONFIG_TCCDIR "/include" would otherwise come from --
 * leave /tcc/include out and the compiler cannot find its own stddef.h. */
#define CONFIG_TCC_SYSINCLUDEPATHS "/tcc/include:/include"
#define CONFIG_TCC_LIBPATHS "/lib"
#define CONFIG_TCC_CRTPREFIX "/lib"
