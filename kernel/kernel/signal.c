// Signals. See signal.h for the shape of the thing; this is how it works.
//
// The awkward part of a signal is not sending it. It is that the program has
// to be made to call a function it was not calling, at a moment it did not
// choose, and then be put back exactly as it was -- including the registers
// it was halfway through using.
//
// The way back is saved on the program's OWN stack, in a frame this file
// writes and reads and nothing else ever looks at. The handler is given a
// return address pointing at a scrap of libc that does nothing but ask the
// kernel to put everything back. So an interrupted program carries its own
// undo, which is what lets several signals nest without the kernel keeping a
// stack of its own for them.

#include "signal.h"
#include "process.h"
#include "kio.h"
#include "../mm/vmm.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/pit.h"

#define SIGBIT(n) ((uint32_t)1 << (n))

// Neither of these may be caught, blocked or ignored. That is the whole of
// what makes "kill it" mean something.
#define UNCATCHABLE (SIGBIT(SIGKILL) | SIGBIT(SIGSTOP))

// What a signal does when the program has not said otherwise.
enum { DFL_TERM, DFL_IGN, DFL_STOP, DFL_CONT };

static int default_action(int signo) {
    switch (signo) {
        case SIGCHLD:
        case SIGWINCH: return DFL_IGN;
        case SIGSTOP:
        case SIGTSTP:  return DFL_STOP;
        case SIGCONT:  return DFL_CONT;
        default:       return DFL_TERM;
    }
}

static uint64_t handler_of(process_t *p, int signo) {
    if (!p->as) return SIG_DFL_ADDR;
    if (signo <= 0 || signo >= NSIG) return SIG_DFL_ADDR;
    return p->as->sig_handler[signo];
}

// A handler is a user address. Anything below the user window is one of the
// two constants, and a program cannot name an address there in any case.
static int is_handler(uint64_t h) { return h > SIG_IGN_ADDR; }

// The lowest-numbered pending signal that would run user code, or -1.
static int next_handled(process_t *p) {
    uint32_t ready = p->sig_pending & ~p->sig_blocked;
    for (int i = 1; i < NSIG; i++) {
        if (!(ready & SIGBIT(i))) continue;
        if (SIGBIT(i) & UNCATCHABLE) continue;
        if (is_handler(handler_of(p, i))) return i;
    }
    return -1;
}

int signal_deliverable(process_t *p) {
    return p && next_handled(p) >= 0;
}

void signal_no_restart(void) {
    process_t *p = process_current();
    if (p) p->sig_restart = 0;
}

/* --- sending -------------------------------------------------------------- */

// Would this signal make a sleeping process do something? If so it has to be
// woken, because a process that is not running never reaches a safe point.
static int wakes(process_t *p, int signo) {
    if (SIGBIT(signo) & UNCATCHABLE) return 1;
    if (p->sig_blocked & SIGBIT(signo)) return 0;
    if (is_handler(handler_of(p, signo))) return 1;   /* a handler to run */
    if (handler_of(p, signo) == SIG_IGN_ADDR) return 0;
    return default_action(signo) != DFL_IGN;
}

void signal_post(process_t *p, int signo) {
    if (!p || signo <= 0 || signo >= NSIG) return;
    if (p->state == PROC_UNUSED || p->state == PROC_ZOMBIE) return;

    // SIGCONT is the one signal that has to act the moment it arrives: a
    // stopped process is never scheduled, so it could not act on it itself.
    if (signo == SIGCONT) {
        p->stopped = 0;
        p->sig_pending &= ~(SIGBIT(SIGSTOP) | SIGBIT(SIGTSTP));
    }
    if (signo == SIGSTOP || signo == SIGTSTP)
        p->sig_pending &= ~SIGBIT(SIGCONT);

    p->sig_pending |= SIGBIT(signo);

    if (p->state == PROC_BLOCKED && wakes(p, signo)) p->state = PROC_READY;
}

int signal_send(int pid, int signo) {
    if (signo < 0 || signo >= NSIG) return -1;
    process_t *p = process_by_pid(pid);
    if (!p || p->state == PROC_UNUSED || p->state == PROC_ZOMBIE) return -1;
    // Signal 0 sends nothing. It is how a program asks whether a process is
    // still there, and the answer is the return value.
    if (signo == 0) return 0;
    signal_post(p, signo);
    return 0;
}

/* --- dispositions --------------------------------------------------------- */

uint64_t signal_set_handler(int signo, uint64_t handler) {
    process_t *p = process_current();
    if (!p || !p->as) return (uint64_t)-1;
    if (signo <= 0 || signo >= NSIG) return (uint64_t)-1;
    if (SIGBIT(signo) & UNCATCHABLE) return (uint64_t)-1;
    if (is_handler(handler)) {
        if (!vmm_user_range_ok(handler, 1)) return (uint64_t)-1;
        // Refused rather than accepted-and-fatal: without a trampoline there
        // is nowhere for the handler to return to, and finding that out at
        // delivery time would kill the program for asking a question.
        if (!p->as->sig_tramp) return (uint64_t)-1;
    }

    uint64_t old = p->as->sig_handler[signo];
    p->as->sig_handler[signo] = handler;
    // A signal that arrived while it was still being ignored has nothing
    // waiting to happen; leaving it pending would fire the new handler for
    // something the program had already said it did not care about.
    if (old == SIG_IGN_ADDR) p->sig_pending &= ~SIGBIT(signo);
    return old;
}

int signal_set_trampoline(uint64_t addr) {
    process_t *p = process_current();
    if (!p || !p->as) return -1;
    if (!vmm_user_range_ok(addr, 1)) return -1;
    p->as->sig_tramp = addr;
    return 0;
}

uint32_t signal_mask(int how, uint32_t mask) {
    process_t *p = process_current();
    if (!p) return 0;
    uint32_t old = p->sig_blocked;
    mask &= ~UNCATCHABLE;              /* neither of those can be held off */
    if (how == SIGMASK_SET)          p->sig_blocked = mask;
    else if (how == SIGMASK_BLOCK)   p->sig_blocked |= mask;
    else if (how == SIGMASK_UNBLOCK) p->sig_blocked &= ~mask;
    return old;
}

int signal_alarm(uint64_t ms) {
    process_t *p = process_current();
    if (!p) return -1;
    if (ms == 0) { p->alarm_at = 0; return 0; }
    uint64_t ticks = (ms + 9) / 10;    /* the PIT runs at 100 Hz */
    if (ticks == 0) ticks = 1;
    p->alarm_at = pit_get_ticks() + ticks;
    return 0;
}

// From the timer IRQ, with interrupts off, on whatever stack was interrupted.
// It only sets bits and flips states, which is all that is safe here.
void signal_tick(uint64_t now) {
    for (int i = 0; i < process_slots(); i++) {
        process_t *p = process_at(i);
        if (!p || !p->alarm_at || now < p->alarm_at) continue;
        p->alarm_at = 0;
        signal_post(p, SIGALRM);
    }
}

/* --- the default actions -------------------------------------------------- */

void signal_check(void) {
    process_t *p = process_current();
    if (!p) return;

    for (;;) {
        uint32_t ready = p->sig_pending & ~p->sig_blocked;
        int signo = -1;
        for (int i = 1; i < NSIG; i++) {
            if (!(ready & SIGBIT(i))) continue;
            uint64_t h = handler_of(p, i);
            // A signal with a handler is not this function's business: it
            // waits for the way out to ring 3, where there is a frame to
            // rewrite. Keep looking -- something further down the list may
            // still be about to end the process.
            if (is_handler(h) && !(SIGBIT(i) & UNCATCHABLE)) continue;
            signo = i;
            break;
        }
        if (signo < 0) return;

        p->sig_pending &= ~SIGBIT(signo);

        if (handler_of(p, signo) == SIG_IGN_ADDR &&
            !(SIGBIT(signo) & UNCATCHABLE))
            continue;

        switch (default_action(signo)) {
            case DFL_IGN:
                break;
            case DFL_CONT:
                p->stopped = 0;
                break;
            case DFL_STOP:
                // Stopped processes are simply never picked, so the next
                // timer tick takes this one off the CPU and nothing brings
                // it back until SIGCONT. Up to one tick of extra running,
                // which is what the task manager's suspend has always done.
                p->stopped = 1;
                break;
            case DFL_TERM:
                process_notify_exit(128 + signo);   /* never returns */
                break;
        }
    }
}

/* --- delivery ------------------------------------------------------------- */

#define SIGFRAME_MAGIC 0x5849554F53534947ULL   /* "XIUOSSIG" */

#define SIGFROM_SYSCALL   1
#define SIGFROM_INTERRUPT 2

// What goes on the user's stack under its handler. The register order is the
// order of struct interrupt_frame on purpose: a signal that arrived from an
// interrupt is put back by handing exactly this to sched_resume().
struct sigframe {
    // The floating-point registers, first so that FXSAVE gets the sixteen-byte
    // alignment it insists on (the frame itself is placed aligned).
    //
    // These are as much a part of "where the program was" as RAX is. A handler
    // is an ordinary C function and the ABI lets it use xmm0-15 freely --
    // printf inside one does, for a start -- so without this, arithmetic that
    // a signal lands in the middle of comes out quietly wrong. The kernel
    // swaps this state between PROCESSES already; a signal is the same
    // problem inside one.
    uint8_t  fx[512];

    uint64_t magic;
    uint64_t from;
    uint64_t blocked;
    uint64_t signo;

    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;     /* unused; keeps the frame layout exact */
    uint64_t rip, cs, rflags, rsp, ss;
};

extern void sched_resume(uint64_t rsp) __attribute__((noreturn));

#define USER_CS 0x2B
#define USER_SS 0x23

// Put the frame on the stack and point the program at `h`. `sf` is filled in
// by the caller with the context to save; on success its rip/rflags/rsp/rdi
// come back describing the handler instead. Returns 0 if the program's stack
// cannot hold it, which is not recoverable -- see the callers.
static int deliver(process_t *p, int signo, uint64_t h, struct sigframe *sf) {
    uint64_t tramp = p->as ? p->as->sig_tramp : 0;
    if (!tramp) return 0;         /* libc never said where to come back to */

    // Below the red zone: the interrupted code is entitled to the 128 bytes
    // under its stack pointer and may have live data there.
    uint64_t sp = sf->rsp - 128;
    sp -= sizeof(struct sigframe);
    sp &= ~0xFULL;
    uint64_t at = sp;
    sp -= 8;                      /* where the handler's return address goes */

    if (!vmm_touch_write(sp, sizeof(struct sigframe) + 8)) return 0;

    sf->magic = SIGFRAME_MAGIC;
    sf->blocked = p->sig_blocked;
    sf->signo = (uint64_t)signo;

    struct sigframe *dst = (struct sigframe *)(uintptr_t)at;
    *dst = *sf;
    // Straight into the user's frame rather than through the copy above: the
    // kernel's own sigframe is a local, and FXSAVE wants sixteen-byte
    // alignment that only the placed frame is guaranteed to have.
    __asm__ volatile ("fxsave (%0)" : : "r"(dst->fx) : "memory");
    *(uint64_t *)(uintptr_t)sp = tramp;

    // The handler runs with its own signal held off, so a second one of the
    // same kind cannot re-enter it. sigreturn puts the mask back.
    p->sig_blocked |= SIGBIT(signo);
    p->sig_pending &= ~SIGBIT(signo);

    sf->rsp = sp;
    sf->rip = h;
    sf->rdi = (uint64_t)signo;    /* the handler's one argument */
    sf->rflags &= ~0x400ULL;      /* DF clear on entry, as the ABI requires */
    return 1;
}

// A signal that could not be delivered ends the program. This happens when
// the stack is too far gone to hold a frame, which is exactly the case where
// pretending otherwise would send it somewhere worse.
static void deliver_failed(int signo) __attribute__((noreturn));
static void deliver_failed(int signo) {
    process_notify_exit(128 + signo);
    for (;;) { }
}

void signal_on_syscall_return(struct syscall_frame *f, uint64_t num,
                              uint64_t ret) {
    process_t *p = process_current();
    if (!p) return;

    int restart = p->sig_restart;
    p->sig_restart = 0;

    int signo = next_handled(p);
    if (signo < 0) return;

    struct sigframe sf;
    for (unsigned i = 0; i < sizeof sf / 8; i++) ((uint64_t *)&sf)[i] = 0;

    // A syscall that gave up because of this signal is put back the way it
    // was, not finished: RAX goes back to the call number and RIP to the
    // two-byte SYSCALL instruction itself, so returning from the handler
    // runs it again with the same arguments. That is the only way a program
    // sitting in a read can run a handler and still be sitting in a read
    // afterwards.
    sf.rax = restart ? num : ret;
    sf.rip = restart ? f->rip - 2 : f->rip;

    sf.rdi = f->rdi; sf.rsi = f->rsi; sf.rdx = f->rdx;
    sf.r8 = f->r8; sf.r9 = f->r9; sf.r10 = f->r10;
    // RCX and R11 are destroyed by the SYSCALL instruction itself, so the
    // program cannot have wanted them back; they carry RIP and RFLAGS here.
    sf.rflags = f->rflags;
    sf.rsp = f->user_rsp;
    sf.cs = USER_CS;
    sf.ss = USER_SS;
    sf.from = SIGFROM_SYSCALL;

    if (!deliver(p, signo, handler_of(p, signo), &sf)) deliver_failed(signo);

    f->rdi = sf.rdi; f->rsi = sf.rsi; f->rdx = sf.rdx;
    f->r8 = sf.r8; f->r9 = sf.r9; f->r10 = sf.r10;
    f->rip = sf.rip;
    f->rflags = sf.rflags;
    f->user_rsp = sf.rsp;
}

// The interrupted program's whole register set is already on the kernel
// stack in the shape a sigframe wants, so this is mostly a copy.
static void frame_to_sigframe(const struct interrupt_frame *f,
                              struct sigframe *sf) {
    sf->r15 = f->r15; sf->r14 = f->r14; sf->r13 = f->r13; sf->r12 = f->r12;
    sf->r11 = f->r11; sf->r10 = f->r10; sf->r9  = f->r9;  sf->r8  = f->r8;
    sf->rbp = f->rbp; sf->rdi = f->rdi; sf->rsi = f->rsi; sf->rdx = f->rdx;
    sf->rcx = f->rcx; sf->rbx = f->rbx; sf->rax = f->rax;
    sf->int_no = 0; sf->err_code = 0;
    sf->rip = f->rip; sf->cs = f->cs; sf->rflags = f->rflags;
    sf->rsp = f->rsp; sf->ss = f->ss;
}

static void sigframe_to_frame(const struct sigframe *sf,
                              struct interrupt_frame *f) {
    f->r15 = sf->r15; f->r14 = sf->r14; f->r13 = sf->r13; f->r12 = sf->r12;
    f->r11 = sf->r11; f->r10 = sf->r10; f->r9  = sf->r9;  f->r8  = sf->r8;
    f->rbp = sf->rbp; f->rdi = sf->rdi; f->rsi = sf->rsi; f->rdx = sf->rdx;
    f->rcx = sf->rcx; f->rbx = sf->rbx; f->rax = sf->rax;
    f->int_no = 0; f->err_code = 0;
    f->rip = sf->rip; f->cs = USER_CS; f->rflags = sf->rflags;
    f->rsp = sf->rsp; f->ss = USER_SS;
}

void signal_on_interrupt_return(struct interrupt_frame *f) {
    process_t *p = process_current();
    if (!p) return;
    if ((f->cs & 3) != 3) return;     /* not on its way to ring 3 */

    int signo = next_handled(p);
    if (signo < 0) return;

    struct sigframe sf;
    frame_to_sigframe(f, &sf);
    sf.from = SIGFROM_INTERRUPT;

    if (!deliver(p, signo, handler_of(p, signo), &sf)) deliver_failed(signo);

    f->rip = sf.rip;
    f->rflags = sf.rflags;
    f->rsp = sf.rsp;
    f->rdi = sf.rdi;
}

int signal_from_fault(struct interrupt_frame *f, int int_no) {
    process_t *p = process_current();
    if (!p) return 0;

    int signo;
    switch (int_no) {
        case 0:  case 16: case 19: signo = SIGFPE;  break;
        case 6:                    signo = SIGILL;  break;
        case 3:  case 1:           signo = SIGTRAP; break;
        case 13: case 14: case 12: signo = SIGSEGV; break;
        case 17:                   signo = SIGBUS;  break;
        default: return 0;
    }

    // Only a real handler, and only one that is not held off right now. A
    // program that has ignored or blocked SIGSEGV and then faults must still
    // be stopped: returning to the same instruction would fault for ever.
    uint64_t h = handler_of(p, signo);
    if (!is_handler(h)) return 0;
    if (p->sig_blocked & SIGBIT(signo)) return 0;

    p->sig_pending |= SIGBIT(signo);
    signal_on_interrupt_return(f);
    return 1;
}

uint64_t signal_return(struct syscall_frame *f, uint64_t uframe) {
    process_t *p = process_current();
    if (!p) return (uint64_t)-1;
    if (!vmm_user_range_ok(uframe, sizeof(struct sigframe))) return (uint64_t)-1;
    if (uframe & 15) return (uint64_t)-1;   /* FXRSTOR would fault on it */

    const struct sigframe *u = (const struct sigframe *)(uintptr_t)uframe;
    if (u->magic != SIGFRAME_MAGIC) return (uint64_t)-1;

    // The floating-point registers go back from where they are, before the
    // copy below -- reading them through a kernel local would lose the
    // alignment FXRSTOR needs.
    __asm__ volatile ("fxrstor (%0)" : : "r"(u->fx) : "memory");

    struct sigframe sf = *u;
    p->sig_blocked = (uint32_t)sf.blocked;

    if (sf.from == SIGFROM_SYSCALL) {
        // Back out through SYSRET, which is how this context left ring 3 in
        // the first place. The registers SYSCALL destroys are not restored
        // because they were already gone before the signal arrived.
        f->rdi = sf.rdi; f->rsi = sf.rsi; f->rdx = sf.rdx;
        f->r8 = sf.r8; f->r9 = sf.r9; f->r10 = sf.r10;
        f->rip = sf.rip;
        f->rflags = sf.rflags;
        f->user_rsp = sf.rsp;
        return sf.rax;
    }

    // It arrived from an interrupt, so every register mattered and SYSRET
    // cannot give two of them back: it needs RCX and R11 for RIP and RFLAGS.
    // Leave by the same door the interrupt would have used instead.
    struct interrupt_frame fr;
    sigframe_to_frame(&sf, &fr);
    sched_resume((uint64_t)(uintptr_t)&fr);   /* never returns */
}
