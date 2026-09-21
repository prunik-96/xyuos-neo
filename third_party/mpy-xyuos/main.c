/* Python on xyuOS Neo.
 *
 *   python              an interactive prompt
 *   python file.py      run a script
 *   python -c "code"    run one line
 *
 * The heap the collector manages is taken from the process heap once, at
 * startup, and never grows: a garbage collector that can ask the system for
 * more memory whenever it is short never has to collect, and on a machine
 * where one program can already take all of RAM that is the wrong direction.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "py/builtin.h"
#include "py/compile.h"
#include "py/runtime.h"
#include "py/repl.h"
#include "py/gc.h"
#include "py/stackctrl.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "shared/runtime/pyexec.h"
#include "shared/runtime/gchelper.h"
#include "extmod/vfs.h"
#include "extmod/vfs_posix.h"

static char *heap;

static void run_str(const char *src, mp_parse_input_kind_t kind, const char *name) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_str_len(qstr_from_str(name), src,
                                                    strlen(src), 0);
        qstr source_name = lex->source_name;
        mp_parse_tree_t tree = mp_parse(lex, kind);
        mp_obj_t fun = mp_compile(&tree, source_name, false);
        mp_call_function_0(fun);
        nlr_pop();
    } else {
        mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
    }
}

static int run_file(const char *path) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_file(qstr_from_str(path));
        qstr source_name = lex->source_name;
        mp_parse_tree_t tree = mp_parse(lex, MP_PARSE_FILE_INPUT);
        mp_obj_t fun = mp_compile(&tree, source_name, false);
        mp_call_function_0(fun);
        nlr_pop();
        return 0;
    }
    mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
    return 1;
}

int main(int argc, char **argv) {
    int stack_dummy;
    mp_stack_set_top(&stack_dummy);
    /* The process stack is a megabyte; leave the collector a margin so a deep
     * recursion raises a Python exception instead of walking off the end. */
    mp_stack_set_limit(768 * 1024);

    heap = (char *)malloc(MICROPY_HEAP_SIZE);
    if (!heap) {
        printf("python: not enough memory for a %d MB heap\n",
               (int)(MICROPY_HEAP_SIZE / (1024 * 1024)));
        return 1;
    }
    gc_init(heap, heap + MICROPY_HEAP_SIZE);
    mp_init();

    /* The filesystem layer starts out with nothing mounted, so every path
     * would come back "no such device". The system has exactly one
     * filesystem and it is already at the root, so say so. */
    {
        mp_obj_t args[2] = {
            MP_OBJ_TYPE_GET_SLOT(&mp_type_vfs_posix, make_new)(&mp_type_vfs_posix, 0, 0, NULL),
            MP_OBJ_NEW_QSTR(MP_QSTR__slash_),
        };
        mp_vfs_mount(2, args, (mp_map_t *)&mp_const_empty_map);
        MP_STATE_VM(vfs_cur) = MP_STATE_VM(vfs_mount_table);
    }

    int rc = 0;
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        run_str(argv[2], MP_PARSE_FILE_INPUT, "<string>");
    } else if (argc >= 2) {
        rc = run_file(argv[1]);
    } else {
        printf("MicroPython on xyuOS Neo.  Ctrl-D to leave.\n");
        pyexec_friendly_repl();
    }

    mp_deinit();
    return rc;
}

/* --- what the runtime asks the port for ----------------------------------- */

/* Anything reachable from a live object is found by the collector itself; what
 * it cannot see on its own is what the C code is holding -- values sitting in
 * registers or on the stack with no other reference. This spills the registers
 * and hands over the stack so those count as roots too. */
void gc_collect(void) {
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}

/* mp_import_stat and the open() builtin come from the filesystem layer now;
 * the port only has to supply what nothing else can know. */

void nlr_jump_fail(void *val) {
    printf("python: unhandled exception, cannot continue\n");
    (void)val;
    exit(1);
}

void NORETURN __fatal_error(const char *msg) {
    printf("python: %s\n", msg);
    exit(1);
}

#ifndef NDEBUG
void MP_WEAK __assert_func(const char *file, int line, const char *func, const char *expr) {
    printf("assertion '%s' failed, at %s:%d in %s\n", expr, file, line, func);
    exit(1);
}
#endif
