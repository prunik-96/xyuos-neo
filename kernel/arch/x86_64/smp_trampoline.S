# Application-processor (AP) startup trampoline.
#
# A SIPI starts an AP in 16-bit real mode at (vector << 12). We pick the page at
# 0x8000, so this whole blob is COPIED there at runtime and the SIPI vector is
# 0x08. Every internal absolute reference is therefore computed as
# TRAMP + (label - ap_trampoline_start): the label difference is an
# assemble-time constant, and TRAMP is where the bytes actually execute.
#
# The path is the same as the BSP's boot.S, only starting one stage earlier
# (real mode): 16-bit -> protected mode -> enable PAE+LME+paging on the SHARED
# kernel CR3 -> 64-bit -> load our own stack -> call ap_entry().
#
# The BSP fills ap_tramp_cr3 / ap_tramp_stack / ap_tramp_entry (patched in the
# copy at 0x8000) before sending the SIPI.

.set TRAMP, 0x8000

.section .text
.code16
.global ap_trampoline_start
.global ap_trampoline_end
.global ap_tramp_cr3
.global ap_tramp_stack
.global ap_tramp_entry
ap_trampoline_start:
    cli
    cld
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    lgdtl (TRAMP + (ap_gdt32_ptr - ap_trampoline_start))

    mov %cr0, %eax
    or  $1, %eax                      # CR0.PE -> protected mode
    mov %eax, %cr0
    ljmpl $0x08, $(TRAMP + (ap_pm32 - ap_trampoline_start))

.code32
ap_pm32:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    mov (TRAMP + (ap_tramp_cr3 - ap_trampoline_start)), %eax
    mov %eax, %cr3

    mov %cr4, %eax
    or  $(1 << 5), %eax               # CR4.PAE
    mov %eax, %cr4

    mov $0xC0000080, %ecx             # EFER
    rdmsr
    or  $(1 << 8), %eax               # EFER.LME
    wrmsr

    mov %cr0, %eax
    or  $(1 << 31), %eax              # CR0.PG
    mov %eax, %cr0

    lgdtl (TRAMP + (ap_gdt64_ptr - ap_trampoline_start))
    ljmpl $0x08, $(TRAMP + (ap_lm64 - ap_trampoline_start))

.code64
ap_lm64:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs

    mov $(TRAMP + (ap_tramp_stack - ap_trampoline_start)), %eax
    mov (%rax), %rsp
    xor %rbp, %rbp

    mov $(TRAMP + (ap_tramp_entry - ap_trampoline_start)), %eax
    mov (%rax), %rax
    call *%rax                        # ap_entry() -- never returns
.ap_hang:
    hlt
    jmp .ap_hang

# --- parameters the BSP patches into the 0x8000 copy ----------------------
.align 8
ap_tramp_cr3:   .quad 0
ap_tramp_stack: .quad 0
ap_tramp_entry: .quad 0

# --- 32-bit protected-mode GDT (null, code32, data32) ---------------------
.align 8
ap_gdt32:
    .quad 0
    .quad 0x00CF9A000000FFFF
    .quad 0x00CF92000000FFFF
ap_gdt32_ptr:
    .word ap_gdt32_ptr - ap_gdt32 - 1
    .long TRAMP + (ap_gdt32 - ap_trampoline_start)

# --- 64-bit GDT (null, code64, data64) ------------------------------------
.align 8
ap_gdt64:
    .quad 0
    .quad (1<<43)|(1<<44)|(1<<47)|(1<<53)
    .quad (1<<44)|(1<<47)|(1<<41)
ap_gdt64_ptr:
    .word ap_gdt64_ptr - ap_gdt64 - 1
    .long TRAMP + (ap_gdt64 - ap_trampoline_start)

ap_trampoline_end:
