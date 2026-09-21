// Processes and the round-robin scheduler.
//
// Preemption rule: a process is only ever switched away from involuntarily
// when the timer interrupts it in RING 3. A process inside a syscall runs with
// IF clear (MSR_FMASK) and cannot be preempted -- but it CAN give the CPU up
// voluntarily by blocking, which is how SYS_WAIT works. Those are the two
// kinds of switch, and `resume_kernel` records which one a process is
// suspended by.

#include "process.h"
#include "elf.h"
#include "kio.h"
#include "../arch/x86_64/syscall.h"
#include "../arch/x86_64/gdt.h"
#include "../mm/vmm.h"
#include "../mm/paging.h"
#include "../mm/heap.h"
#include "../fs/vfs.h"
#include "../wm/wm.h"
#include "../arch/x86_64/pit.h"
#include "../arch/x86_64/smp.h"
#include <stddef.h>

extern uint64_t kctx_save(uint64_t ctx[8]);
extern void kctx_restore(uint64_t ctx[8], uint64_t return_value);

// Load a saved interrupt frame and return to whatever it describes. Never
// returns. Defined in sched_switch.S.
extern void sched_resume(uint64_t rsp) __attribute__((noreturn));

// A single `run` in one pane is already a chain of four (sh -> run -> cc ->
// tcc), and each extra pane adds a shell, so 8 was uncomfortably tight.
#define MAX_PROCESSES 16
#define MAX_ARGS 32
#define MAX_ARG_LEN 256
#define KSTACK_SIZE 16384

#define USER_CS 0x2B     /* GDT user code, RPL 3 */
#define USER_SS 0x23     /* GDT user data, RPL 3 */
#define RFLAGS_IF 0x202  /* IF set, bit 1 reserved-one */

static process_t proc_table[MAX_PROCESSES];
static process_t *current = NULL;
static int next_pid = 1;

// --- pipes -----------------------------------------------------------------
#define MAX_PIPES 16
#define PIPE_BUF_SIZE 4096

struct pipe {
    int in_use;
    unsigned char buf[PIPE_BUF_SIZE];
    uint32_t head, tail, count;
    int write_open;    // a writer end still exists
    int read_open;     // a reader end still exists
    int writer_pid;    // process writing, or 0
    int reader_pid;    // process reading, or 0
};
static struct pipe pipes[MAX_PIPES];

static void process_block_on_pipe(void);
static void process_wake_pipe(void);

// Where to go when nothing is left to run, and the address space to go back to.
static uint64_t idle_ctx[8];

// Set while the CPU is halted in the idle loop; the PIT samples it to estimate
// CPU utilisation. Global so the timer handler can read it cheaply.
volatile int g_cpu_idle = 0;
static uint64_t kernel_space = 0;
static int scheduler_active = 0;
static int last_exit_code = 0;

process_t *process_current(void) {
    return current;
}

process_t *process_by_pid(int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state != PROC_UNUSED && proc_table[i].pid == pid) {
            return &proc_table[i];
        }
    }
    return 0;
}

static process_t *find_by_pid(int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state != PROC_UNUSED && proc_table[i].pid == pid) {
            return &proc_table[i];
        }
    }
    return NULL;
}

// --- process lifecycle -----------------------------------------------------

static process_t *proc_alloc(const char *name) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state != PROC_UNUSED) continue;
        process_t *p = &proc_table[i];
        p->pid = next_pid++;
        p->parent_pid = current ? current->pid : 0;
        p->state = PROC_READY;
        p->pml4 = 0;
        p->entry = 0;
        p->brk = USER_HEAP_BASE;
        p->exit_code = 0;
        p->kstack = NULL;
        p->resume_kernel = 0;
        p->wait_reason = WAIT_NONE;
        p->waiting_for = 0;
        p->wake_at = 0;
        p->pending_kill = 0;
        p->in.kind = STREAM_TERM;   p->in.handle = -1;
        p->out.kind = STREAM_TERM;  p->out.handle = -1;
        int k = 0;
        while (name[k] && k < PROC_NAME_MAX - 1) { p->name[k] = name[k]; k++; }
        p->name[k] = 0;
        return p;
    }
    return NULL;
}

// Give back the kernel stack and the slot. Must NOT be called while running on
// that kernel stack -- reaping always happens from another process's context.
static void proc_reap(process_t *p) {
    if (p->kstack) kfree(p->kstack);
    p->kstack = NULL;
    p->state = PROC_UNUSED;
}

// Failure path for a process that was never started.
static void proc_abandon(process_t *p) {
    if (p->pml4) paging_free_address_space(p->pml4);
    p->pml4 = 0;
    proc_reap(p);
}

static uint64_t pstrlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

// Lay out argc/argv on the user stack the way the System V ABI expects, so
// crt0 can just read them off:
//
//   [rsp]      argc
//   [rsp+8]    argv[0]
//   ...
//   [rsp+8+8n] NULL
//
// The strings themselves live higher up the stack. rsp stays 16-byte aligned.
//
// Called with the process's address space in CR3, so these are plain writes.
// The argv strings are read from kernel memory, which is mapped in every
// address space, so they survive the switch.
static uint64_t setup_args(uint64_t stack_top, int argc, const char *const *argv) {
    uint64_t sp = stack_top;
    uint64_t ptrs[MAX_ARGS];

    if (argc > MAX_ARGS) argc = MAX_ARGS;

    for (int i = argc - 1; i >= 0; i--) {
        uint64_t len = pstrlen(argv[i]) + 1;
        sp -= len;
        char *dst = (char *)(uintptr_t)sp;
        for (uint64_t k = 0; k < len; k++) dst[k] = argv[i][k];
        ptrs[i] = sp;
    }

    sp &= ~0xFULL;

    uint64_t words = 1 + (uint64_t)argc + 1;      // argc + argv[] + NULL
    if (words & 1) sp -= 8;                       // keep rsp 16-byte aligned
    sp -= words * 8;

    uint64_t *slot = (uint64_t *)(uintptr_t)sp;
    slot[0] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) slot[1 + i] = ptrs[i];
    slot[1 + argc] = 0;

    return sp;
}

// Fabricate the interrupt frame a never-yet-run process would have had if it
// HAD been preempted at its entry point. That way "start a process" and
// "resume a process" are the same operation, and the scheduler needs no
// special case for the first run.
//
// Layout, low address to high, matching irq_common_stub's epilogue
// (POP_REGS; add $16, %rsp; iretq):
//
//   r15 r14 r13 r12 r11 r10 r9 r8 rbp rdi rsi rdx rcx rbx rax
//   int_no err_code
//   rip cs rflags rsp ss
static void build_initial_frame(process_t *p, uint64_t user_sp) {
    uint64_t *sp = (uint64_t *)(uintptr_t)p->kstack_top;

    *--sp = USER_SS;      // ss
    *--sp = user_sp;      // rsp
    *--sp = RFLAGS_IF;    // rflags
    *--sp = USER_CS;      // cs
    *--sp = p->entry;     // rip

    *--sp = 0;            // err_code
    *--sp = 0;            // int_no

    for (int i = 0; i < 15; i++) *--sp = 0;   // rax..r15, all zeroed

    p->saved_rsp = (uint64_t)(uintptr_t)sp;
    p->resume_kernel = 0;
}

int process_spawn_image(const char *name, const void *elf_data, uint64_t size,
                        int argc, const char *const *argv) {
    process_t *p = proc_alloc(name);
    if (!p) {
        kprintf("process: no free process slots\n");
        return -1;
    }

    p->pml4 = paging_new_address_space();
    if (!p->pml4) {
        kprintf("process: out of memory creating an address space\n");
        p->state = PROC_UNUSED;
        return -1;
    }

    if (elf_load_into(p->pml4, elf_data, size, &p->entry) != 0) {
        proc_abandon(p);
        return -1;
    }

    if (paging_map_alloc(p->pml4, USER_STACK_BASE, USER_STACK_SIZE,
                         PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) {
        kprintf("process: out of memory mapping the stack\n");
        proc_abandon(p);
        return -1;
    }

    p->kstack = (uint8_t *)kmalloc(KSTACK_SIZE);
    if (!p->kstack) {
        kprintf("process: out of memory allocating a kernel stack\n");
        proc_abandon(p);
        return -1;
    }
    // 16-byte aligned, as the ABI wants for the frame we are about to build.
    p->kstack_top = ((uint64_t)(uintptr_t)p->kstack + KSTACK_SIZE) & ~0xFULL;

    // argv has to be written through the process's own page tables, so borrow
    // its address space for a moment. Safe: the kernel is mapped identically
    // in every space, so the code doing the writing does not move.
    uint64_t prev = paging_current();
    paging_switch(p->pml4);
    uint64_t user_sp = setup_args((USER_STACK_TOP - 16) & ~0xFULL, argc, argv);
    paging_switch(prev);

    build_initial_frame(p, user_sp);

    // Give the process a sane initial FPU/SSE state to be restored from.
    __asm__ volatile ("fxsave (%0)" : : "r"(p->fxstate) : "memory");

    return p->pid;
}

int process_spawn(const char *path, int argc, const char *const *argv) {
    struct vfs_stat st;
    if (vfs_stat(path, &st) != 0) {
        kprintf("process: '%s' not found\n", path);
        return -1;
    }
    if (st.is_dir) {
        kprintf("process: '%s' is a directory\n", path);
        return -1;
    }

    int fd = vfs_open(path);
    if (fd < 0) {
        kprintf("process: cannot open '%s'\n", path);
        return -1;
    }

    uint8_t *image = (uint8_t *)kmalloc(st.size);
    if (!image) {
        kprintf("process: out of memory reading '%s' (%u bytes)\n",
                path, (unsigned int)st.size);
        vfs_close(fd);
        return -1;
    }

    // vfs_read is not guaranteed to return everything in one call, so loop
    // until the whole image is in.
    uint32_t got = 0;
    while (got < st.size) {
        int32_t n = vfs_read(fd, image + got, st.size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    vfs_close(fd);

    if (got != st.size) {
        kprintf("process: short read on '%s' (%u of %u bytes)\n",
                path, (unsigned int)got, (unsigned int)st.size);
        kfree(image);
        return -1;
    }

    const char *name = path;
    for (const char *c = path; *c; c++) if (*c == '/') name = c + 1;

    int pid = process_spawn_image(name, image, st.size, argc, argv);
    kfree(image);
    return pid;
}

// --- spawning on behalf of userland ----------------------------------------

// Copy a NUL-terminated string out of user space, validating every byte. The
// caller's pointers become meaningless the moment we switch to the child's
// address space, so everything has to be in kernel memory first.
static int copy_user_str(uint64_t uaddr, char *dst, int max) {
    for (int i = 0; i < max; i++) {
        if (!vmm_user_range_ok(uaddr + (uint64_t)i, 1)) return -1;
        char c = *(const char *)(uintptr_t)(uaddr + (uint64_t)i);
        dst[i] = c;
        if (!c) return i;
    }
    return -1;   // unterminated, or longer than we are willing to take
}

// Full spawn on behalf of userland: path + argv from user space, plus optional
// file redirection (paths) and/or pipe endpoints (indices). A pipe wins over a
// path for the same stream.
static int spawn_from_user(uint64_t upath, uint64_t uargv, int argc,
                           uint64_t uin, uint64_t uout, int append,
                           int in_pipe, int out_pipe) {
    // Static rather than automatic: 8 KiB is far too much for a 16 KiB kernel
    // stack. Safe because syscalls are never preempted and spawning does not
    // block, so there is only ever one user of this at a time.
    static char argbuf[MAX_ARGS][MAX_ARG_LEN];
    char path[MAX_ARG_LEN];
    char in_path[MAX_ARG_LEN], out_path[MAX_ARG_LEN];
    const char *argv[MAX_ARGS];

    if (copy_user_str(upath, path, MAX_ARG_LEN) < 0) return -1;
    if (argc < 0 || argc > MAX_ARGS) return -1;
    if (argc > 0 && !vmm_user_range_ok(uargv, (uint64_t)argc * 8)) return -1;

    for (int i = 0; i < argc; i++) {
        uint64_t str = ((const uint64_t *)(uintptr_t)uargv)[i];
        if (copy_user_str(str, argbuf[i], MAX_ARG_LEN) < 0) return -1;
        argv[i] = argbuf[i];
    }
    if (uin  && copy_user_str(uin,  in_path,  MAX_ARG_LEN) < 0) return -1;
    if (uout && copy_user_str(uout, out_path, MAX_ARG_LEN) < 0) return -1;

    int pid = process_spawn(path, argc, argv);
    if (pid < 0) return -1;

    process_t *child = process_by_pid(pid);
    if (!child) return pid;

    // stdin: pipe first, else file.
    if (in_pipe >= 0 && in_pipe < MAX_PIPES && pipes[in_pipe].in_use) {
        child->in.kind = STREAM_PIPE;
        child->in.handle = in_pipe;
        pipe_attach_reader(in_pipe, pid);
    } else if (uin) {
        int fd = vfs_open(in_path);
        if (fd < 0) {
            kprintf("spawn: cannot read '%s'\n", in_path);
            child->state = PROC_UNUSED;   // never ran; nothing to unwind
            paging_free_address_space(child->pml4);
            if (child->kstack) kfree(child->kstack);
            return -1;
        }
        child->in.kind = STREAM_FILE;
        child->in.handle = fd;
    }

    // stdout: pipe first, else file.
    if (out_pipe >= 0 && out_pipe < MAX_PIPES && pipes[out_pipe].in_use) {
        child->out.kind = STREAM_PIPE;
        child->out.handle = out_pipe;
        pipe_attach_writer(out_pipe, pid);
    } else if (uout) {
        vfs_create(out_path);            // no-op if it already exists
        int fd = vfs_open(out_path);
        if (fd >= 0) {
            if (append) vfs_seek(fd, 0, VFS_SEEK_END);
            else        vfs_ftruncate(fd, 0);
            child->out.kind = STREAM_FILE;
            child->out.handle = fd;
        }
    }
    return pid;
}

int process_spawn_user(uint64_t upath, uint64_t uargv, int argc) {
    return spawn_from_user(upath, uargv, argc, 0, 0, 0, -1, -1);
}

int process_spawn_user2(uint64_t ureq) {
    if (!vmm_user_range_ok(ureq, sizeof(struct spawn_req))) return -1;
    const struct spawn_req *r = (const struct spawn_req *)(uintptr_t)ureq;

    // Read the whole request out of user memory first: the fields are user
    // pointers and must not be re-read after any of the copying below.
    uint64_t upath = (uint64_t)(uintptr_t)r->path;
    uint64_t uargv = (uint64_t)(uintptr_t)r->argv;
    int argc = r->argc;
    uint64_t uin  = (uint64_t)(uintptr_t)r->in_path;
    uint64_t uout = (uint64_t)(uintptr_t)r->out_path;
    int append = r->append;
    int in_pipe = r->in_pipe;
    int out_pipe = r->out_pipe;

    return spawn_from_user(upath, uargv, argc, uin, uout, append, in_pipe, out_pipe);
}

// --- pipe object -----------------------------------------------------------

int pipe_create(void) {
    for (int i = 0; i < MAX_PIPES; i++) {
        if (pipes[i].in_use) continue;
        struct pipe *p = &pipes[i];
        p->in_use = 1;
        p->head = p->tail = p->count = 0;
        p->write_open = 1;
        p->read_open = 1;
        p->writer_pid = 0;
        p->reader_pid = 0;
        return i;
    }
    return -1;
}

void pipe_attach_writer(int idx, int pid) { pipes[idx].writer_pid = pid; }
void pipe_attach_reader(int idx, int pid) { pipes[idx].reader_pid = pid; }

void pipe_close(int idx) {
    if (idx < 0 || idx >= MAX_PIPES || !pipes[idx].in_use) return;
    pipes[idx].write_open = 0;
    pipes[idx].read_open = 0;
    pipes[idx].in_use = 0;
    process_wake_pipe();   // release any end blocked on it
}

// Free a pipe once neither end is open. Called as each end's process exits.
static void pipe_maybe_free(int idx) {
    if (idx < 0 || idx >= MAX_PIPES) return;
    struct pipe *p = &pipes[idx];
    if (!p->write_open && !p->read_open) p->in_use = 0;
}

// Called from process exit to detach a stream, so the other end sees the close.
static void stream_release(struct pstream *s) {
    if (s->kind == STREAM_FILE) {
        vfs_close(s->handle);
    } else if (s->kind == STREAM_PIPE) {
        int idx = s->handle;
        if (idx >= 0 && idx < MAX_PIPES && pipes[idx].in_use) {
            struct pipe *p = &pipes[idx];
            if (p->writer_pid == current->pid) {
                p->write_open = 0;   // the reader gets EOF once it drains
            }
            if (p->reader_pid == current->pid) {
                p->read_open = 0;
                // SIGPIPE: a writer with no reader left has nothing to do.
                // Terminate it (the P7 way) so `yes | head` actually stops
                // instead of spinning on failed writes.
                if (p->write_open && p->writer_pid) {
                    process_t *w = process_by_pid(p->writer_pid);
                    if (w) {
                        w->pending_kill = 1;
                        if (w->state == PROC_BLOCKED) w->state = PROC_READY;
                    }
                }
            }
            process_wake_pipe();   // EOF for a waiting reader / space for a writer
            pipe_maybe_free(idx);
        }
    }
    s->kind = STREAM_TERM;
    s->handle = -1;
}

static int pipe_write(int idx, const uint8_t *buf, uint32_t len) {
    struct pipe *p = &pipes[idx];
    uint32_t written = 0;
    while (written < len) {
        if (!p->read_open) return (written > 0) ? (int)written : -1;  // EPIPE
        if (p->count == PIPE_BUF_SIZE) {
            process_block_on_pipe();
            continue;
        }
        while (written < len && p->count < PIPE_BUF_SIZE) {
            p->buf[p->head] = buf[written++];
            p->head = (p->head + 1) % PIPE_BUF_SIZE;
            p->count++;
        }
        process_wake_pipe();   // data available for the reader
    }
    return (int)written;
}

static int pipe_read(int idx, uint8_t *buf, uint32_t len) {
    struct pipe *p = &pipes[idx];
    while (p->count == 0) {
        if (!p->write_open) return 0;   // EOF
        process_block_on_pipe();
    }
    uint32_t n = 0;
    while (n < len && p->count > 0) {
        buf[n++] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
        p->count--;
    }
    process_wake_pipe();   // space available for the writer
    return (int)n;
}

// Return STREAM_TERMINAL (see process.h) when the stream is the terminal, so
// the syscall layer knows to fall back to the pane/keyboard -- distinct from a
// real -1 error (e.g. a broken pipe), which must NOT fall back.
long process_read_stdin(void *buf, uint64_t len) {
    if (!current) return STREAM_TERMINAL;
    if (current->in.kind == STREAM_FILE)
        return vfs_read(current->in.handle, buf, (uint32_t)len);
    if (current->in.kind == STREAM_PIPE)
        return pipe_read(current->in.handle, (uint8_t *)buf, (uint32_t)len);
    return STREAM_TERMINAL;
}

long process_write_stdout(const void *buf, uint64_t len) {
    if (!current) return STREAM_TERMINAL;
    if (current->out.kind == STREAM_FILE)
        return vfs_write(current->out.handle, buf, (uint32_t)len);
    if (current->out.kind == STREAM_PIPE)
        return pipe_write(current->out.handle, (const uint8_t *)buf, (uint32_t)len);
    return STREAM_TERMINAL;
}

// --- scheduling ------------------------------------------------------------

static process_t *pick_next(process_t *after) {
    // Round robin: start looking just past `after` and wrap.
    int start = 0;
    if (after) {
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (&proc_table[i] == after) { start = i + 1; break; }
        }
    }
    for (int n = 0; n < MAX_PROCESSES; n++) {
        process_t *p = &proc_table[(start + n) % MAX_PROCESSES];
        if (p->state == PROC_READY && !p->stopped) return p;
    }
    return NULL;
}

// Make `p` the running process: its address space, its kernel stack for both
// traps and syscalls, and its FPU state.
static void activate(process_t *p) {
    current = p;
    p->state = PROC_RUNNING;
    paging_switch(p->pml4);
    tss_set_kernel_stack(p->kstack_top);
    this_cpu()->kstack_top = p->kstack_top;   // where THIS core runs p's syscalls
    __asm__ volatile ("fxrstor (%0)" : : "r"(p->fxstate) : "memory");
}

// Put `p` on the CPU and jump into it. Never returns.
static void resume_process(process_t *p) __attribute__((noreturn));
static void resume_process(process_t *p) {
    activate(p);
    if (p->resume_kernel) {
        kctx_restore(p->kctx, 1);   // returns inside whatever syscall blocked
    }
    sched_resume(p->saved_rsp);
}

uint64_t sched_on_tick(uint64_t cur_rsp, uint64_t cs) {
    if (!scheduler_active || !current) return cur_rsp;
    // Only a process actually ON the CPU is charged. A blocked one idles on
    // its own kernel stack with `current` still pointing at it, so charging
    // unconditionally would bill it for the machine doing nothing.
    if (current->state == PROC_RUNNING) current->ticks++;

    // Only preempt ring 3. A tick that caught the kernel (in a syscall, or in
    // the idle loop) is left alone.
    if ((cs & 3) != 3) return cur_rsp;

    // A process spinning in ring 3 (`while (1) {}`) makes no syscalls, so the
    // timer is the only place we can interrupt it. We are on its kernel stack
    // (the IRQ from ring 3 landed here), which is exactly where the exit path
    // expects to run. Never returns when it fires.
    if (current->pending_kill) {
        current->saved_rsp = cur_rsp;   // unused after exit, but keep it sane
        process_check_kill();
    }

    process_t *next = pick_next(current);
    if (!next || next == current) return cur_rsp;

    // Put the current process down: its entire register state is the frame at
    // cur_rsp, on its own kernel stack, so remembering the pointer is enough.
    current->saved_rsp = cur_rsp;
    current->resume_kernel = 0;
    current->state = PROC_READY;
    __asm__ volatile ("fxsave (%0)" : : "r"(current->fxstate) : "memory");

    activate(next);
    if (next->resume_kernel) {
        // The next process blocked inside a syscall rather than being
        // preempted, so it cannot be resumed by returning a frame -- jump to
        // it directly and never come back to this interrupt.
        kctx_restore(next->kctx, 1);
    }
    return next->saved_rsp;
}

// Give up the CPU from inside a syscall. Returns once someone has made this
// process READY again.
static void block_current(void) {
    process_t *me = current;

    while (me->state == PROC_BLOCKED) {
        process_t *next = pick_next(me);
        if (next) {
            me->resume_kernel = 1;
            __asm__ volatile ("fxsave (%0)" : : "r"(me->fxstate) : "memory");
            if (kctx_save(me->kctx) == 0) {
                resume_process(next);   // never returns
            }
            // Resumed: kctx_restore brought us back, and activate() has
            // already restored our address space, stacks and FPU state.
            break;
        }

        // Nothing else can run. Idle here with interrupts ENABLED until one of
        // them wakes us -- we are in ring 0 on our own kernel stack, so this is
        // a safe place to sit. (Syscalls normally run with IF clear; enabling
        // it is exactly what makes the wakeup possible.)
        //
        // The loop matters: wm_poll() can act on a WM key binding that SPAWNS a
        // process, so what was unrunnable a moment ago may now be runnable, and
        // we have to go back and look again rather than idle through it.
        __asm__ volatile ("sti");
        wm_poll();
        g_cpu_idle = 1;
        __asm__ volatile ("hlt; cli");
        g_cpu_idle = 0;
    }

    // Woken. If a Ctrl+C arrived while we slept, die now instead of resuming
    // the syscall we blocked in -- this is the safe point for it.
    process_check_kill();
}

void process_block_on_key(void) {
    if (!current) return;
    current->state = PROC_BLOCKED;
    current->wait_reason = WAIT_KEY;
    block_current();
    current->wait_reason = WAIT_NONE;
}

static void process_block_on_pipe(void) {
    if (!current) return;
    current->state = PROC_BLOCKED;
    current->wait_reason = WAIT_PIPE;
    block_current();
    current->wait_reason = WAIT_NONE;
}

// Wake every process blocked on a pipe. They re-check their own pipe's state,
// so a broad wake is simplest and correct with this few processes.
static void process_wake_pipe(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = &proc_table[i];
        if (p->state == PROC_BLOCKED && p->wait_reason == WAIT_PIPE) {
            p->state = PROC_READY;
        }
    }
}

// Timer resolution is the PIT rate set in kmain (100 Hz => 10 ms).
#define TICK_MS 10

void process_sleep_ms(uint64_t ms) {
    if (!current) return;
    uint64_t ticks = (ms + TICK_MS - 1) / TICK_MS;
    if (ticks == 0) ticks = 1;

    current->wake_at = pit_get_ticks() + ticks;
    current->state = PROC_BLOCKED;
    current->wait_reason = WAIT_TIME;
    block_current();
    current->wait_reason = WAIT_NONE;
}

// Called from the timer IRQ. Runs with interrupts off on whatever stack was
// interrupted, so it only flips states -- no allocation, no switching.
void process_tick(uint64_t now) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = &proc_table[i];
        if (p->state == PROC_BLOCKED && p->wait_reason == WAIT_TIME &&
            now >= p->wake_at) {
            p->state = PROC_READY;
        }
    }
}

int process_suspend(int pid) {
    process_t *p = process_by_pid(pid);
    if (!p || p->state == PROC_UNUSED || p->state == PROC_ZOMBIE) return -1;
    if (p == current) return -1;          // suspending yourself never resumes
    p->stopped = 1;
    return 0;
}

int process_resume(int pid) {
    process_t *p = process_by_pid(pid);
    if (!p || p->state == PROC_UNUSED) return -1;
    p->stopped = 0;
    return 0;
}

int process_list(struct si_proc *out, int max) {
    int n = 0;
    for (int i = 0; i < MAX_PROCESSES && n < max; i++) {
        process_t *p = &proc_table[i];
        if (p->state == PROC_UNUSED) continue;
        out[n].pid = p->pid;
        out[n].parent_pid = p->parent_pid;
        out[n].state = (int)p->state;
        out[n].pane = wm_pane_for_pid(p->pid) ? 1 : 0;
        out[n].stopped = p->stopped;
        out[n].ticks = p->ticks;
        int k = 0;
        while (p->name[k] && k < SI_NAME_MAX - 1) { out[n].name[k] = p->name[k]; k++; }
        out[n].name[k] = '\0';
        n++;
    }
    return n;
}

void process_wake_key(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = &proc_table[i];
        if (p->state == PROC_BLOCKED && p->wait_reason == WAIT_KEY) {
            p->state = PROC_READY;
        }
    }
}

// --- interruption (Ctrl+C) -------------------------------------------------

int process_kill(int pid) {
    process_t *p = process_by_pid(pid);
    if (!p || p->state == PROC_UNUSED || p->state == PROC_ZOMBIE) return -1;
    p->pending_kill = 1;
    // If it is asleep it will never reach a safe point on its own -- wake it,
    // and it dies the instant it returns from the block.
    if (p->state == PROC_BLOCKED) p->state = PROC_READY;
    return 0;
}

int process_foreground(int pid) {
    int cur = pid;
    // Bounded by the table size: the parent chain cannot be longer.
    for (int depth = 0; depth < MAX_PROCESSES; depth++) {
        int child = -1;
        for (int i = 0; i < MAX_PROCESSES; i++) {
            process_t *p = &proc_table[i];
            if (p->state == PROC_UNUSED || p->state == PROC_ZOMBIE) continue;
            if (p->parent_pid == cur) { child = p->pid; break; }
        }
        if (child < 0) break;
        cur = child;
    }
    return cur;
}

void process_check_kill(void) {
    if (current && current->pending_kill) {
        current->pending_kill = 0;
        process_notify_exit(130);   // 128 + SIGINT; never returns
    }
}

void process_make_session_root(int pid) {
    // A pane's shell is a top-level session leader, not a child of whatever
    // process happened to be `current` when the idle loop performed the split.
    // Getting this wrong corrupts the foreground chain: Ctrl+C would walk into
    // another pane's shell. Session roots are reaped by themselves on exit
    // (parent_pid 0), which is correct -- nobody wait()s on a pane shell.
    process_t *p = process_by_pid(pid);
    if (p) p->parent_pid = 0;
}

int process_wait(int pid) {
    if (!current) return -1;

    for (;;) {
        process_t *child = NULL;
        int have_live_child = 0;

        for (int i = 0; i < MAX_PROCESSES; i++) {
            process_t *p = &proc_table[i];
            if (p->state == PROC_UNUSED) continue;
            if (p->parent_pid != current->pid) continue;
            if (pid >= 0 && p->pid != pid) continue;

            if (p->state == PROC_ZOMBIE) { child = p; break; }
            have_live_child = 1;
        }

        if (child) {
            int code = child->exit_code;
            proc_reap(child);
            return code;
        }
        if (!have_live_child) return -1;   // no such child

        current->state = PROC_BLOCKED;
        current->wait_reason = WAIT_CHILD;
        current->waiting_for = pid;
        block_current();
        current->wait_reason = WAIT_NONE;
    }
}

// Wake a parent blocked in process_wait() for this child.
static void wake_parent_of(process_t *dead) {
    if (!dead->parent_pid) return;
    process_t *parent = find_by_pid(dead->parent_pid);
    if (!parent || parent->state != PROC_BLOCKED) return;
    if (parent->waiting_for >= 0 && parent->waiting_for != dead->pid) return;
    parent->state = PROC_READY;
}

void process_notify_exit(int code) {
    if (!current) {
        kprintf("process: exit(%d) with no process running\n", code);
        for (;;) { __asm__ volatile ("hlt"); }
    }

    process_t *dead = current;
    dead->exit_code = code;
    dead->state = PROC_ZOMBIE;
    last_exit_code = code;

    // Release streams here rather than in proc_reap: a file's data must be on
    // disk, and a pipe's close (EOF / SIGPIPE) must reach the other end, before
    // the parent is woken and reads what the child produced. stream_release
    // reads current->pid, and current is still `dead` at this point.
    stream_release(&dead->in);
    stream_release(&dead->out);

    wake_parent_of(dead);
    wm_notify_exit(dead->pid);   // hand the pane a fresh shell, if it had one

    // The address space can go now, but NOT the kernel stack: this code is
    // running on it. The stack is freed when the process is reaped, which
    // always happens from somebody else's context.
    process_t *next = pick_next(dead);
    if (next) {
        activate(next);
    } else {
        current = NULL;
        paging_switch(kernel_space);
    }
    paging_free_address_space(dead->pml4);
    dead->pml4 = 0;

    // Nobody will ever reap a process the kernel started, so retire it here.
    if (!dead->parent_pid) proc_reap(dead);

    if (next) {
        if (next->resume_kernel) kctx_restore(next->kctx, 1);
        sched_resume(next->saved_rsp);
    }

    // Nothing READY right now. Hand control back to the scheduler loop, which
    // decides whether to idle (someone is merely asleep) or finish. It runs on
    // kmain's stack, so it is safe to get here from a dying process.
    kctx_restore(idle_ctx, 1);
}

// Is any process still alive? A BLOCKED process counts -- it is waiting for an
// interrupt or a child, not finished. Getting this wrong means the scheduler
// walks away from a sleeping process and never comes back to it.
static int any_alive(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        proc_state_t s = proc_table[i].state;
        if (s == PROC_READY || s == PROC_RUNNING || s == PROC_BLOCKED) return 1;
    }
    return 0;
}

// Resume the next runnable process (never returning), or -- when everything
// alive is asleep -- idle with interrupts enabled until an IRQ wakes someone.
// Returns only once no process is left at all.
static void schedule_or_idle(void) {
    for (;;) {
        // Repaint and handle WM bindings BEFORE picking a process: this is the
        // compositor's only chance to run, and a binding may spawn the very
        // process we are about to look for.
        wm_poll();
        process_t *next = pick_next(NULL);
        if (next) resume_process(next);     // never returns
        if (!any_alive()) return;
        __asm__ volatile ("sti; hlt; cli");
    }
}

int process_run_all(void) {
    if (!any_alive()) return -1;

    kernel_space = paging_current();
    scheduler_active = 1;

    // Processes come back here when they exit, via kctx_restore(idle_ctx).
    // Both the first entry and every re-entry fall through to the scheduler.
    kctx_save(idle_ctx);

    current = NULL;
    paging_switch(kernel_space);
    this_cpu()->kstack_top = 0;
    schedule_or_idle();

    // Every process has exited. Anything still sitting in the table is a
    // zombie whose parent died before reaping it.
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state == PROC_ZOMBIE) proc_reap(&proc_table[i]);
    }

    scheduler_active = 0;
    current = NULL;
    this_cpu()->kstack_top = 0;
    tss_set_kernel_stack(0);
    return last_exit_code;
}

int process_run_image(const char *name, const void *elf_data, uint64_t size,
                      int argc, const char *const *argv) {
    if (process_spawn_image(name, elf_data, size, argc, argv) < 0) return -1;
    return process_run_all();
}

int process_run(const char *path, int argc, const char *const *argv) {
    if (process_spawn(path, argc, argv) < 0) return -1;
    return process_run_all();
}

// --- per-process heap ------------------------------------------------------

uint64_t process_sbrk(int64_t increment) {
    if (!current) return (uint64_t)-1;

    uint64_t old = current->brk;
    if (increment == 0) return old;

    if (increment > 0) {
        if ((uint64_t)increment > USER_HEAP_LIMIT - old) return (uint64_t)-1;
        // Pages left mapped by an earlier grow-then-shrink are reused as they
        // are; malloc does not assume sbrk memory is zeroed, and fresh frames
        // arrive zeroed anyway.
        if (paging_map_alloc(current->pml4, old, (uint64_t)increment,
                             PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) {
            return (uint64_t)-1;
        }
        current->brk = old + (uint64_t)increment;
    } else {
        uint64_t shrink = (uint64_t)(-increment);
        if (shrink > old - USER_HEAP_BASE) return (uint64_t)-1;
        current->brk = old - shrink;
    }
    return old;
}
