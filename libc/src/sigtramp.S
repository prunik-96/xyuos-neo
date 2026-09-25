.code64

# Where a signal handler returns to.
#
# The kernel does not call the handler -- it cannot; it is on the wrong side
# of the ring boundary. What it does instead is write the interrupted context
# onto this program's own stack, put THIS address underneath the handler as
# its return address, and send the program to the handler as if it had been
# called from here.
#
# So the handler returns by an ordinary `ret`, which lands here with the stack
# pointer on the saved context. Nothing else is on the stack, and no register
# may be assumed: the whole point is that the kernel is about to put them all
# back.
#
# The two numbers are SYS_SIGNAL and SIGOP_RETURN from xyuos_syscall.h, which
# this file cannot include (it has structs in it). libc/src/signal.c checks
# them against the header at compile time.

.section .text
.global __sig_trampoline
__sig_trampoline:
    mov $48, %eax        # SYS_SIGNAL
    mov $2,  %edi        # SIGOP_RETURN
    mov %rsp, %rsi       # the saved context is exactly here
    syscall
    # The kernel does not come back to THIS instruction: it resumes the
    # interrupted context, wherever that was. Getting here means it refused
    # the frame, which is not something a program can carry on from.
    ud2

# No executable stack.
.section .note.GNU-stack,"",@progbits
