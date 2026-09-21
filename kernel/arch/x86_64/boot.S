.section .multiboot
.align 8
multiboot_header_start:
    .long 0xE85250D6                       # multiboot2 magic
    .long 0                                # architecture: 0 = i386/protected mode
    .long multiboot_header_end - multiboot_header_start
    .long -(0xE85250D6 + 0 + (multiboot_header_end - multiboot_header_start))

    # framebuffer request tag: ask for a 1920x1080x32 linear framebuffer
    # (a preference; the kernel reads the actual dims GRUB returns)
    .align 8
    .word 5
    .word 0
    .long 20
    .long 1920
    .long 1080
    .long 32

    .align 8
    .word 0                                # end tag
    .word 0
    .long 8
multiboot_header_end:

.section .bss
.align 4096
.global p2_table
.global p3_table
.global p4_table
p4_table:
    .skip 4096
p3_table:
    .skip 4096
p2_table:
    .skip 4096 * 4                         # 4 contiguous PD tables -> identity-map 4GiB
.align 16
stack_bottom:
    # 128 KiB. Was 16 KiB, which is enough for the kernel/WM but leaves no
    # headroom for deeply recursive kernel work. This is .bss (zero-init), so
    # it costs nothing in the ISO and pmm already reserves
    # [kernel_start,kernel_end].
    .skip 131072
stack_top:

.section .text
.code32
.global _start
.type _start, @function
_start:
    mov $stack_top, %esp
    mov $0, %ebp

    mov %ebx, %edi                          # preserve multiboot info pointer BEFORE any
                                             # cpuid (check_long_mode clobbers ebx/ecx/edx)
    call check_multiboot
    call check_cpuid
    call check_long_mode

    call set_up_page_tables
    call enable_paging

    lgdt (gdt64_pointer)
    ljmp $0x08, $long_mode_start

check_multiboot:
    cmp $0x36d76289, %eax
    jne .no_multiboot
    ret
.no_multiboot:
    mov $'0', %al
    jmp error

check_cpuid:
    pushfl
    pop %eax
    mov %eax, %ecx
    xor $(1 << 21), %eax
    push %eax
    popfl
    pushfl
    pop %eax
    push %ecx
    popfl
    cmp %ecx, %eax
    je .no_cpuid
    ret
.no_cpuid:
    mov $'1', %al
    jmp error

check_long_mode:
    mov $0x80000000, %eax
    cpuid
    cmp $0x80000001, %eax
    jb .no_long_mode

    mov $0x80000001, %eax
    cpuid
    test $(1 << 29), %edx
    jz .no_long_mode
    ret
.no_long_mode:
    mov $'2', %al
    jmp error

set_up_page_tables:
    mov $p3_table, %eax
    or $0b11, %eax
    mov %eax, (p4_table)

    mov $0, %ecx
.map_p3_table:
    mov $p2_table, %eax
    mov %ecx, %edx
    shl $12, %edx
    add %edx, %eax
    or $0b11, %eax
    mov %eax, p3_table(,%ecx,8)
    inc %ecx
    cmp $4, %ecx
    jne .map_p3_table

    mov $0, %ecx
.map_p2_table:
    mov $0x200000, %eax
    mul %ecx
    or $0b10000011, %eax                    # present + writable + huge(2MiB)
    mov %eax, p2_table(,%ecx,8)
    inc %ecx
    cmp $2048, %ecx
    jne .map_p2_table

    ret

enable_paging:
    mov $p4_table, %eax
    mov %eax, %cr3

    mov %cr4, %eax
    or $(1 << 5), %eax                      # PAE
    mov %eax, %cr4

    mov $0xC0000080, %ecx                   # EFER MSR
    rdmsr
    or $(1 << 8), %eax                      # long mode enable
    wrmsr

    mov %cr0, %eax
    or $(1 << 31), %eax                     # enable paging
    mov %eax, %cr0

    ret

# prints "ERR: X" in white-on-red to the VGA text buffer and halts.
# only used for early boot-time sanity checks, before serial/framebuffer are up.
error:
    movl $0x4f524f45, (0xb8000)
    movl $0x4f3a4f52, (0xb8004)
    movl $0x4f204f20, (0xb8008)
    movb %al, (0xb800a)
    hlt

.section .rodata
.align 8
gdt64:
    .quad 0
.gdt64_code:
    .quad (1<<43) | (1<<44) | (1<<47) | (1<<53)   # executable, code/data, present, 64-bit
.gdt64_data:
    .quad (1<<44) | (1<<47) | (1<<41)              # present, code/data, writable
gdt64_pointer:
    .word . - gdt64 - 1
    .quad gdt64

.section .text
.code64
long_mode_start:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    # RDI already holds the multiboot info pointer (zero-extended from EDI set above)
    call kernel_main

    cli
.hang:
    hlt
    jmp .hang
