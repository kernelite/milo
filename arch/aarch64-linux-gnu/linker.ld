ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text.boot : {
        *(.text.boot)
    }

    . = ALIGN(2048);

    .vectors : {
        *(.text.vectors)
    }

    .text : {
        *(.text .text.*)
    }

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(16) {
        *(.data .data.*)
    }

    .bss : ALIGN(16) {
        . = ALIGN(8);
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.gnu*)
        *(.note*)
    }
}