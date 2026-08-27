ENTRY(_start)

SECTIONS
{
    . = 0x7C00;
    .text : {
        *(.text)
        *(.data)
        *(.rodata*)
        *(.bss)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
