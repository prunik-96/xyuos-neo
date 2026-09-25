#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include "../arch/x86_64/syscall.h"   // struct si_proc, for process_list()
#include "../mm/vmm.h"

#define PROC_NAME_MAX 32

// Why a PROC_BLOCKED process is asleep, and therefore what will wake it.
#define WAIT_NONE  0
#define WAIT_CHILD 1   // in process_wait(), woken when a child exits
#define WAIT_KEY   2   // in a read-key syscall, woken by the keyboard IRQ
#define WAIT_TIME  3   // sleeping; woken by the timer once wake_at passes
#define WAIT_PIPE  4   // in a pipe read/write, woken by the other end

// A standard stream is one of three things. This replaces the old "vfs fd or
// -1 for terminal" pair with something that can also name a pipe endpoint.
#define STREAM_TERM 0  // the pane (output) / keyboard line discipline (input)
#define STREAM_FILE 1  // a redirected file; `handle` is a VFS fd
#define STREAM_PIPE 2  // a pipe endpoint; `handle` is a pipe index

struct pstream {
    int kind;
    int handle;
};

typedef enum {
    PROC_UNUSED = 0,
    PROC_READY,     // runnable, not currently on the CPU
    PROC_RUNNING,   // on the CPU right now
    PROC_BLOCKED,   // waiting for something (today: for a child to exit)
    PROC_ZOMBIE     // exited; exit code kept until the parent reaps it
} proc_state_t;

// A process is its address space, its kernel stack, and enough saved state to
// be put down and picked back up.
typedef struct process {
    int          pid;
    int          parent_pid;   // 0 when spawned by the kernel
    proc_state_t state;
    // Suspended by the task manager. Deliberately NOT a proc_state_t value:
    // a stopped process keeps whatever state it had, so resuming it does not
    // have to reconstruct whether it was runnable or blocked on a read.
    int          stopped;
    // Timer ticks spent on the CPU. The task manager samples the difference
    // between two reads, which is the only honest way to show a percentage.
    unsigned long long ticks;
    char         name[PROC_NAME_MAX];

    uint64_t pml4;        // physical address of this process's PML4
    uint64_t entry;
    uint64_t brk;         // heap break, per process

    // What this process has asked to have mapped, beyond its image, heap and
    // stack. Placed from the top of the window downwards; the heap grows up
    // towards them and neither may pass the other. See kernel/mm/vmm.c.
    vm_region_t vm[VM_REGIONS_MAX];

    // Kernel stack. The CPU switches to this (via TSS.RSP0) on every trap out
    // of ring 3, and syscalls run on it too, so each process needs its own --
    // otherwise preempting one process would land the next one's state on top
    // of it, and no process could ever block inside a syscall.
    uint8_t *kstack;
    uint64_t kstack_top;

    // How to put this process back on the CPU.
    //   resume_kernel == 0: it was preempted in ring 3; saved_rsp points at
    //                       its interrupt frame, resume with sched_resume().
    //   resume_kernel == 1: it blocked inside a syscall; kctx holds its kernel
    //                       execution state, resume with kctx_restore().
    int      resume_kernel;
    uint64_t saved_rsp;
    uint64_t kctx[8];

    // Standard streams. A file redirect, a pipe endpoint, or the terminal.
    // Set from a struct spawn_req and released when the process exits.
    struct pstream in;
    struct pstream out;

    int wait_reason;      // WAIT_* -- what this process is blocked on
    int waiting_for;      // pid this process is blocked on, -1 for any child
    uint64_t wake_at;     // tick to wake on, for WAIT_TIME
    int exit_code;

    // Set by process_kill(); the process ends itself at the next SAFE point
    // (syscall entry, wakeup from a block, or a timer tick in ring 3) rather
    // than being torn down asynchronously mid-syscall, where it might hold
    // open fds or point into another process's memory.
    int pending_kill;

    // x87/SSE state. Userland does floating point (strtod, printf), so this
    // has to be swapped too. 16-byte aligned, as FXSAVE requires.
    uint8_t fxstate[512] __attribute__((aligned(16)));
} process_t;

// Create a process from an ELF on the filesystem and put it on the run queue,
// WITHOUT running it. Returns the pid, or -1 if it could not be loaded.
int process_spawn(const char *path, int argc, const char *const *argv);

// Same, from an image already in memory (tcc is still a GRUB module).
int process_spawn_image(const char *name, const void *elf_data, uint64_t size,
                        int argc, const char *const *argv);

// SYS_SPAWN: like process_spawn, but path and argv are USER pointers, copied
// and validated before the child's address space exists. Returns pid or -1.
int process_spawn_user(uint64_t upath, uint64_t uargv, int argc);

// SYS_SPAWN2: same, plus stdin/stdout redirection. `ureq` is a user pointer to
// a struct spawn_req. Returns pid or -1.
int process_spawn_user2(uint64_t ureq);

// Run every spawned process until all of them have exited. Returns the exit
// code of the process that exited last.
int process_run_all(void);

// Convenience: spawn one program and run it to completion. Returns its exit
// code, or -1 if it could not be loaded.
int process_run(const char *path, int argc, const char *const *argv);
int process_run_image(const char *name, const void *elf_data, uint64_t size,
                      int argc, const char *const *argv);

// The process on the CPU right now, or NULL if the kernel is running on its
// own behalf.
process_t *process_current(void);

// Look a process up by pid, or NULL. Used to walk the parent chain when
// deciding which pane a process may write to.
process_t *process_by_pid(int pid);

// Move the current process's break. Returns the PREVIOUS break, or
// (uint64_t)-1 if the request would leave the heap region.
uint64_t process_sbrk(int64_t increment);

// Fill `out` with one entry per live process, up to `max`. Returns the count.
int process_list(struct si_proc *out, int max);

// Block the current process for `ms` milliseconds.
void process_sleep_ms(uint64_t ms);

// Called from the timer IRQ: wake anything whose sleep has expired.
void process_tick(uint64_t now);

// Sentinel returned by the stdin/stdout helpers when the stream is the
// TERMINAL: the syscall layer must fall back to the pane / keyboard. Distinct
// from -1, which is a real error (e.g. a broken pipe) and must NOT fall back.
#define STREAM_TERMINAL (-2L)

// Standard streams for the running process. Return STREAM_TERMINAL for the
// terminal; otherwise do the real read/write (a pipe blocks as needed, and may
// return 0 for EOF or -1 for a broken pipe).
long process_read_stdin(void *buf, uint64_t len);
long process_write_stdout(const void *buf, uint64_t len);

// --- pipes -----------------------------------------------------------------
// A pipe is an in-kernel ring buffer with one writer and one reader. Reads
// block until data or the writer closes (then EOF); writes block until space
// or the reader closes (then the writer is terminated, SIGPIPE-style).

// Create a pipe, returns its index or -1 if none free.
int pipe_create(void);
// Force both ends shut and free the pipe (used by the shell to clean up a
// pipeline whose stage failed to spawn). Wakes anything blocked on it.
void pipe_close(int idx);
// Attach the current-being-spawned child to a pipe end. Called from spawn.
void pipe_attach_writer(int idx, int pid);
void pipe_attach_reader(int idx, int pid);

// SYS_WAIT: block until the given child (or any child, for pid < 0) has
// exited, reap it, and return its exit code. -1 if there is no such child.
int process_wait(int pid);

// Sleep the current process until a key arrives. Used by the read-key
// syscalls so that waiting for input does not stall every other process.
// Returns immediately (having done nothing) if no process is running.
void process_block_on_key(void);

// Called from the keyboard IRQ: make every process sleeping on input runnable.
void process_wake_key(void);

// Suspend / resume by pid. 0 on success, -1 if there is no such process.
int  process_suspend(int pid);
int  process_resume(int pid);

// Called from the SYS_EXIT syscall handler. Marks the caller dead and switches
// to another process; never returns.
void process_notify_exit(int code);

// Mark `pid` for death. It is not killed here -- it dies at its next safe
// point (see pending_kill). A blocked process is woken so it can reach one.
// Returns 0, or -1 if there is no such live process.
int process_kill(int pid);

// The deepest live descendant of `pid` -- the process actually in the
// foreground of a pane whose owner is `pid`. Returns `pid` itself when it has
// no living descendant (e.g. a shell sitting at its prompt). This is what
// Ctrl+C targets.
int process_foreground(int pid);

// If the current process has been marked for death, end it now (exit code
// 130 = 128 + SIGINT). Never returns when it fires. Call only at safe points.
void process_check_kill(void);

// Detach `pid` from its accidental parent and make it a session root
// (parent_pid 0). The WM calls this for every pane shell, so the foreground
// chain of one pane can never reach into another's.
void process_make_session_root(int pid);

// Called from the timer interrupt with the interrupted register frame. Returns
// the stack pointer to resume on -- the same one when no switch happens, or
// another process's when it is time to rotate.
uint64_t sched_on_tick(uint64_t cur_rsp, uint64_t cs);

#endif
