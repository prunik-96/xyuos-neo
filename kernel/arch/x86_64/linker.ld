ENTRY(_start)

SECTIONS
{
    . = 1M;
    kernel_start = .;

    /* The multiboot2 header MUST live within the first 32768 bytes of the
       file. Keeping it as a separate .boot section let the linker push it
       past that limit once .text grew; merging it into the front of .text
       guarantees it stays at the very start of the image. */
    .text BLOCK(4K) : ALIGN(4K)
    {
        KEEP(*(.multiboot))
        *(.text)
    }

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata*)
    }

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }

    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }

    . = ALIGN(4K);
    kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
