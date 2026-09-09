ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* Read (4) + Execute (1) = 5 (RX) */
    rodata PT_LOAD FLAGS(4); /* Read-only (4) = 4 (R) */
    data PT_LOAD FLAGS(6);   /* Read (4) + Write (2) = 6 (RW) */
}

SECTIONS
{
    . = 0x40000000;

    .text.boot : {
        *(.text.boot)
    } :text

    . = ALIGN(2048); /* Mandatory 2KB alignment for VBAR_EL1 exception vectors */

    .vectors : {
        *(.text.vectors)
    } :text

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(4096); /* Align to page boundary before changing segment permissions */

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(4096); /* Align to page boundary before writable segment */

    .data : ALIGN(16) {
        *(.data .data.*)
    } :data

    .bss : ALIGN(16) {
        . = ALIGN(8);
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.gnu*)
        *(.note*)
    }
}