ENTRY(_start)

SECTIONS
{
    /* USER_LOAD_BASE from kernel/mm/vmm.h: 1 GiB + 4 MiB. Inside the private
       per-process window, and still below 2 GiB so the default small code
       model (the only one tcc emits) stays valid. */
    . = 0x40400000;

    .text : { *(.text) }
    .rodata : ALIGN(16) { *(.rodata*) }

    /* The exception tables.
     *
       .eh_frame says how to undo a stack frame, and .gcc_except_table says
       which catch clauses a frame has. Both used to be thrown away here,
       which made C++ exceptions impossible however the compiler was built --
       the machinery was all present and there was simply nothing for it to
       read.

       KEEP, because nothing REFERS to these sections; they are found by
       address, so --gc-sections would otherwise remove them as unused.

       The zero word at the end is a terminator, not padding. The unwinder
       walks .eh_frame entry by entry and stops at a length of zero; without
       it, it walks off the end into whatever comes next. */
    .eh_frame : ALIGN(8)
    {
        __eh_frame_start = .;
        KEEP(*(.eh_frame))
        LONG(0)
    }
    .gcc_except_table : ALIGN(8) { KEEP(*(.gcc_except_table*)) }

    /* Constructors and destructors of objects at file scope, as lists of
       function pointers. The compiler emits them; libc/src/start.c walks the
       first, and exit() walks what the first registered. */
    .init_array : ALIGN(8)
    {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        KEEP(*(.ctors))
        __init_array_end = .;
    }
    .fini_array : ALIGN(8)
    {
        __fini_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        KEEP(*(.dtors))
        __fini_array_end = .;
    }

    .data : ALIGN(16) { *(.data) *(.data.*) }
    .bss : ALIGN(16) { *(COMMON) *(.bss) *(.bss.*) }

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
    }
}
