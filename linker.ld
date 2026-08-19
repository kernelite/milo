ENTRY(_start)

SECTIONS
{
    . = 0x40000000;
    
    .text : {
        *(.text.boot)
        *(.text*)
    }
    
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    
    .bss : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss*)
        . = ALIGN(16);
        __bss_end = .;
    }
}
