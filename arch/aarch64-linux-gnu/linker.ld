ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5); /* Read (4) + Execute (1) = RX */
    rodata PT_LOAD FLAGS(4); /* Read-only (4) = R */
    data   PT_LOAD FLAGS(6); /* Read (4) + Write (2) = RW */
}

SECTIONS
{
    . = 0x40000000;

    /* Executable Code Segment (RX) */
    .text : {
        _text_start = .;
        
        *(.text.boot)
        *(.text .text.*)

        . = ALIGN(2048); /* 2KB alignment for VBAR_EL1 exception vectors */
        *(.text.vectors)

        _text_end = .;
    } :text

    . = ALIGN(4096); /* Page boundary before changing segment permissions */

    /* Read-Only Data Segment (R) */
    .rodata : ALIGN(16) {
        _rodata_start = .;
        *(.rodata .rodata.*)
        _rodata_end = .;
    } :rodata

    . = ALIGN(4096); /* Page boundary before writable segment */

    /* Read-Write Data Segment (RW) */
    .data : ALIGN(16) {
        _data_start = .;
        *(.data .data.*)
        _data_end = .;
    } :data

    /* Uninitialized Data (BSS) */
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