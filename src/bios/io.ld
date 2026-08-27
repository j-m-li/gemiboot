ENTRY(_start)

SECTIONS
{
    . = 0x00010000 - 512;
    .text : {
        *(.text)
        *(.rodata*)
        *(.data)
        *(.bss)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
