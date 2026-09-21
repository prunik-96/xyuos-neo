ENTRY(_start)

SECTIONS
{
    /* USER_LOAD_BASE from kernel/mm/vmm.h: 1 GiB + 4 MiB. Inside the private
       per-process window, and still below 2 GiB so the default small code
       model (the only one tcc emits) stays valid. */
    . = 0x40400000;

    .text : { *(.text) }
    .rodata : ALIGN(16) { *(.rodata*) }
    .data : ALIGN(16) { *(.data) }
    .bss : ALIGN(16) { *(COMMON) *(.bss) }

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
