.code64

# PER-CPU block offsets (struct percpu in smp.h): kstack_top @8, user_rsp @16.
.set PERCPU_KSTACK,  8
.set PERCPU_USERRSP, 16

.section .text
.global syscall_entry
.extern syscall_dispatch

# Entered via the SYSCALL instruction (LSTAR MSR points here).
# On entry: RCX = user return RIP, R11 = user RFLAGS, RSP = still the user
# stack (SYSCALL does not switch stacks -- that's our job).
# Args (Linux-style): RAX=num, RDI=a1, RSI=a2, RDX=a3.
#
# swapgs brings the per-CPU block into GS so each core finds ITS OWN kernel
# stack -- the scratch user rsp lives there too, so two cores' syscalls never
# clobber each other.
syscall_entry:
    swapgs
    movq %rsp, %gs:PERCPU_USERRSP       # park user rsp in this core's scratch
    movq %gs:PERCPU_KSTACK, %rsp        # switch to this core's kernel stack

    pushq %gs:PERCPU_USERRSP            # save user rsp on the kernel stack

    push %rcx
    push %r11

    # Save the registers the caller is entitled to get back.
    #
    # SYSCALL itself destroys only RCX and R11, and every compiler on earth
    # knows that. It does NOT know that this entry sequence goes on to use RDI,
    # RSI and RDX to build the argument list for syscall_dispatch, nor that the
    # C function it calls may clobber the rest of the caller-saved set.
    #
    # Leaving them destroyed made the kernel's ABI quietly different from the
    # one the compiler compiles against, and the result was not a crash but
    # wrong VALUES: given two syscalls in a row whose arguments are mostly the
    # same, GCC loads the changed argument and reuses what it believes is still
    # in the others. The second call then runs with whatever the kernel left
    # behind. It took six consecutive settings reads to make it show, and it
    # would have gone on producing nonsense in one program in ten for ever.
    #
    # Eight pushes plus the user rsp above keeps the count odd, which is what
    # the alignment at `call` already depended on.
    push %rdi
    push %rsi
    push %rdx
    push %r8
    push %r9
    push %r10

    mov %rdi, %r9      # stash user a1
    mov %rsi, %r10      # stash user a2
    mov %rdx, %r11      # stash user a3 (r11's real value is already saved above)

    mov %rax, %rdi      # num
    mov %r9,  %rsi      # a1
    mov %r10, %rdx      # a2
    mov %r11, %rcx      # a3

    # Fifth argument: where the saved user state is. Everything above this
    # point is a `struct syscall_frame` (kernel/kernel/signal.h) read from
    # the bottom up, and signal delivery rewrites three of its fields to send
    # the program to a handler instead of back to where it called from. The
    # pushes and that struct must stay in step.
    mov %rsp, %r8

    call syscall_dispatch

    pop %r10
    pop %r9
    pop %r8
    pop %rdx
    pop %rsi
    pop %rdi
    pop %r11
    pop %rcx
    pop %rsp             # the user rsp we parked above
    swapgs              # restore the user's GS before returning to ring 3
    sysretq

.global enter_usermode
# void enter_usermode(uint64_t entry, uint64_t user_stack)
enter_usermode:
    mov $0x23, %ax       # user data selector (GDT_USER_DATA, RPL=3)
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    push $0x23            # SS
    push %rsi              # RSP (user_stack)
    push $0x202             # RFLAGS (IF=1)
    push $0x2B                # CS (GDT_USER_CODE, RPL=3)
    push %rdi                  # RIP (entry)
    iretq
