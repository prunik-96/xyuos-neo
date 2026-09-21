/* Minimal stand-in for tcc's lib/runmain.c, built for xyuOS with our own
 * cross-compiler and shipped on the disk as /tcc/runmain.o.
 *
 * tcc's -run mode links this in and jumps to _runmain, which calls the
 * compiled program's main(). The upstream file also runs .init_array /
 * .fini_array constructors and supports atexit(); programs compiled here are
 * -nostdlib and have no init arrays, so that machinery is left out rather
 * than faked.
 *
 * exit() must go through __rt_exit, which tcc registers with tcc_add_symbol
 * before running -- that is what unwinds back into the compiler instead of
 * halting.
 */

int main(int argc, char **argv, char **envp);

typedef struct rt_frame {
    void *ip, *fp, *sp;
} rt_frame;

__attribute__((noreturn)) void __rt_exit(rt_frame *f, int code);

int _runmain(int argc, char **argv, char **envp)
{
    return main(argc, argv, envp);
}

void exit(int code)
{
    rt_frame f;
    f.ip = (void *)0;
    f.fp = (void *)0;
    f.sp = (void *)0;
    __rt_exit(&f, code);
}
