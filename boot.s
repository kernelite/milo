.section .text.boot
.global _start

_start:
    // Park secondary CPU cores
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbnz    x0, halt

    // Enable SIMD/Floating Point at EL1 (FPEN = 0b11 in CPACR_EL1)
    // Prevents CPU traps when Rust optimizations use 128-bit vector registers
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb

    // Set stack pointer
    ldr     x0, =boot_stack_top
    mov     sp, x0

    // Zero out BSS
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
1:  cmp     x1, x2
    b.ge    2f
    str     xzr, [x1], #8
    b       1b

2:  bl      kmain

halt:
    wfe
    b       halt

.section .bss
.balign 16
boot_stack:
    .space 16384
boot_stack_top:
