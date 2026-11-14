.section .text
.balign 4

.global _start
_start:
    // write(1, msg, 44)
    mov     x8, #64                 // SYS_WRITE
    mov     x0, #1                  // STDOUT
    adr     x1, msg
    mov     x2, #44
    svc     #0

    // exit(77)
    mov     x8, #93                 // SYS_EXIT
    mov     x0, #77
    svc     #0

msg:
    .ascii  "  [EL0 /bin/hello] Hello from initrd ELF!\r\n"
