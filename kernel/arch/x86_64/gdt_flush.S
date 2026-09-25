.code64
.global gdt_flush
.global tss_flush

# void gdt_flush(uint64_t gdt_pointer_addr)
gdt_flush:
    lgdt (%rdi)

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    # NOT fs and gs. In long mode, loading a selector into either of them
    # also loads its base from the descriptor -- zero -- and the GS base is
    # this core's per-CPU block, set before this runs. Nothing here uses FS
    # or GS as a segment, so there is nothing to refresh and a lot to lose.

    # reload CS via a far return, required since lgdt alone does not
    # refresh CS's cached descriptor attributes.
    push $0x08
    lea .reload_cs(%rip), %rax
    push %rax
    lretq
.reload_cs:
    ret

# void tss_flush(uint16_t selector)
tss_flush:
    mov %di, %ax
    ltr %ax
    ret
