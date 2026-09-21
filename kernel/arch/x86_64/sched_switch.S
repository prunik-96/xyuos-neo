.code64

# void sched_resume(uint64_t rsp)
#
# Adopt a saved interrupt frame and return to whatever it describes -- ring 3
# for a user process. The frame layout is exactly what irq_common_stub leaves
# on the stack, so resuming a process that was preempted by the timer and
# starting one that has never run (see build_initial_frame) are the same code.
#
# Never returns.
.global sched_resume
sched_resume:
    mov %rdi, %rsp

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

    add $16, %rsp        # skip int_no and err_code
    # This always resumes a ring-3 process (a freshly built frame or one the
    # timer preempted in user mode), so hand GS back to the user first.
    swapgs
    iretq
