#ifndef SIGNAL_H
#define SIGNAL_H

// Signals: a way to interrupt a program at a point it did not choose, and
// hand it back afterwards as if nothing had happened.
//
// There are two halves to this, and they live in different places on purpose.
//
//   The DISPOSITION -- what a signal does -- belongs to the program, so it
//   sits in the address space (see addr_space_t). A handler is a function in
//   the program's own code; every thread sharing that code shares the
//   handlers, which is both what POSIX says and the only thing that could
//   sensibly be meant by a pointer into shared memory.
//
//   The PENDING SET -- which signals have arrived and not yet been acted on
//   -- belongs to the process, so it sits in process_t. A signal is sent to
//   one process (a thread is a process here), and it is that one which runs
//   the handler.
//
// Delivery happens on the way OUT to ring 3 and nowhere else. The kernel has
// no business running a program's code while it is halfway through a syscall,
// so a pending signal waits for either the syscall to finish or the timer to
// catch the program in user mode. Default actions -- ending the process,
// stopping it -- need no user code and so are done at any safe point inside
// the kernel.

#include <stdint.h>

struct process;
struct interrupt_frame;

#define NSIG 32

#define SIGHUP   1
#define SIGINT   2    /* Ctrl+C */
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9    /* cannot be caught, cannot be ignored */
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19   /* cannot be caught, cannot be ignored */
#define SIGTSTP  20
#define SIGWINCH 28

// A disposition is either one of these two or a user address.
#define SIG_DFL_ADDR 0ULL
#define SIG_IGN_ADDR 1ULL

// The registers the kernel saved on its way into the syscall, as
// syscall_entry.S left them on the kernel stack. Signal delivery rewrites
// three of these fields (rip, rflags, user_rsp) to send the program to its
// handler instead of back to where it called from.
//
// The order is the reverse of the push order in syscall_entry.S. Changing one
// without the other silently sends a program to a wrong address.
struct syscall_frame {
    uint64_t r10, r9, r8, rdx, rsi, rdi;
    uint64_t rflags;    /* pushed as r11; SYSRET takes RFLAGS from r11 */
    uint64_t rip;       /* pushed as rcx; SYSRET takes RIP from rcx */
    uint64_t user_rsp;
};

// --- what userland asks for (SYS_SIGNAL ops) -------------------------------
#define SIGOP_HANDLER 0   /* a2 = signo, a3 = handler -> previous, or -1 */
#define SIGOP_SEND    1   /* a2 = pid,   a3 = signo   -> 0 / -1          */
#define SIGOP_RETURN  2   /* a2 = frame; never returns normally          */
#define SIGOP_TRAMP   3   /* a2 = the trampoline libc will return through */
#define SIGOP_ALARM   4   /* a2 = ms, 0 cancels -> 0                      */
#define SIGOP_MASK    5   /* a2 = how (0 set, 1 block, 2 unblock), a3 = mask */

#define SIGMASK_SET     0
#define SIGMASK_BLOCK   1
#define SIGMASK_UNBLOCK 2

// --- sending ---------------------------------------------------------------

// Put `signo` in a process's pending set and wake it if it is asleep and the
// signal is one it would act on. Returns 0, or -1 if there is no such live
// process or the number is not a signal.
int signal_send(int pid, int signo);

// The same, to a process the kernel already has in its hand. Used by the
// places that raise a signal because of something that just happened: a
// fault, a broken pipe, a child that exited.
void signal_post(struct process *p, int signo);

// --- dispositions ----------------------------------------------------------

// Returns the previous handler, or (uint64_t)-1 if the signal cannot be
// caught or the number is not a signal.
uint64_t signal_set_handler(int signo, uint64_t handler);

// Where libc's return trampoline lives. A handler cannot be delivered until
// this is known, because there would be nowhere for it to return to.
int signal_set_trampoline(uint64_t addr);

// Block / unblock / replace the current process's mask. Returns the previous
// mask. SIGKILL and SIGSTOP cannot be blocked and are silently dropped from
// anything asked for.
uint32_t signal_mask(int how, uint32_t mask);

// Raise SIGALRM on the current process in `ms` milliseconds. 0 cancels a
// pending alarm. Returns 0.
int signal_alarm(uint64_t ms);

// Called from the timer IRQ: raise SIGALRM on anything whose alarm has come.
void signal_tick(uint64_t now);

// --- acting on them --------------------------------------------------------

// Do everything a pending signal asks for that needs no user code: end the
// process, stop it, let it continue, or drop it. Signals with a handler are
// left where they are for the delivery path below. Never returns when one of
// them ends the process.
//
// This is the safe-point check: syscall entry, and every wakeup from a block.
void signal_check(void);

// Is there a signal waiting that will run user code? Blocking syscalls ask
// this after they wake, to decide whether to give up and let the handler run.
int signal_deliverable(struct process *p);

// Say that the syscall now running must NOT be put back if a signal cut it
// short: it has already done something that cannot be done twice. The handler
// still runs; the call simply returns what it managed instead of happening
// again.
void signal_no_restart(void);

// The two ways back to ring 3. Each rewrites the frame it is given so that
// the program resumes inside its handler, with everything needed to put it
// back afterwards saved on its own stack.
void signal_on_syscall_return(struct syscall_frame *f, uint64_t num,
                              uint64_t ret);
void signal_on_interrupt_return(struct interrupt_frame *f);

// SYS_SIGNAL / SIGOP_RETURN. Restores the context saved by the delivery above
// and goes straight back to it; never returns normally. `uframe` is the user
// address of the saved frame, which is the trampoline's stack pointer.
uint64_t signal_return(struct syscall_frame *f, uint64_t uframe);

// A fault in ring 3 that vmm_fault would not fix. Turns the processor's
// exception number into a signal and delivers it if the program is waiting
// for one. Returns 1 if the program is now inside its handler and the
// interrupt may simply return, 0 if it should be terminated as before.
int signal_from_fault(struct interrupt_frame *f, int int_no);

#endif
