.section .text.boot
.global _start

_start:
    // Park secondary CPU cores
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbnz    x0, halt

    // Check CurrentEL
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    and     x0, x0, #3

    cmp     x0, #3
    b.eq    el3_to_el1

    cmp     x0, #2
    b.eq    el2_to_el1

    b       el1_entry

el3_to_el1:
    mov     x0, #(1 << 10) | (1 << 0)
    msr     scr_el3, x0
    mov     x0, #0x3c5
    msr     spsr_el3, x0
    adr     x0, el1_entry
    msr     elr_el3, x0
    eret

el2_to_el1:
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0
    mov     x0, #0x3c5
    msr     spsr_el2, x0
    adr     x0, el1_entry
    msr     elr_el2, x0
    eret

el1_entry:
    // Enable SIMD/FP at EL1
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb

    // Load 2KB-aligned vector table into VBAR_EL1
    ldr     x0, =vector_table
    msr     vbar_el1, x0
    isb

    // Setup boot stack
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

/* --- 2KB Aligned Vector Table --- */
.section .text.vectors, "ax"
.balign 2048
.global vector_table
vector_table:
    // Current EL with SP0
    b sync_handler; .balign 128
    b unhandled_trap; .balign 128
    b unhandled_trap; .balign 128
    b unhandled_trap; .balign 128

    // Current EL with SPx
    b sync_handler; .balign 128
    b unhandled_trap; .balign 128
    b unhandled_trap; .balign 128
    b unhandled_trap; .balign 128

    // Lower EL using AArch64
    b sync_handler; .balign 128
    b unhandled_trap; .balign 128
    b unhandled_trap; .balign 128
    b unhandled_trap; .balign 128

    // Lower EL using AArch32
    b unhandled_trap; .balign 128
    b unhandled_trap; .balign 128
    b unhandled_trap; .balign 128
    b unhandled_trap; .balign 128

.text
.global sync_handler
sync_handler:
    // Allocate 800 bytes (16-byte aligned) on stack
    sub sp, sp, #800

    // 1. Save General Purpose Registers (x0 - x29)
    stp x0, x1,   [sp, #0]
    stp x2, x3,   [sp, #16]
    stp x4, x5,   [sp, #32]
    stp x6, x7,   [sp, #48]
    stp x8, x9,   [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]

    // 2. Read System Fault Registers
    mrs x0, elr_el1
    mrs x1, spsr_el1
    mrs x2, esr_el1
    mrs x3, far_el1

    stp x30, x0,  [sp, #240]
    stp x1, x2,   [sp, #256]
    str x3,       [sp, #272]

    // 3. Save SIMD/NEON Vector Registers (q0 - q31) at sp + 288
    stp q0,  q1,  [sp, #288]
    stp q2,  q3,  [sp, #320]
    stp q4,  q5,  [sp, #352]
    stp q6,  q7,  [sp, #384]
    stp q8,  q9,  [sp, #416]
    stp q10, q11, [sp, #448]
    stp q12, q13, [sp, #480]
    stp q14, q15, [sp, #512]
    stp q16, q17, [sp, #544]
    stp q18, q19, [sp, #576]
    stp q20, q21, [sp, #608]
    stp q22, q23, [sp, #640]
    stp q24, q25, [sp, #672]
    stp q26, q27, [sp, #704]
    stp q28, q29, [sp, #736]
    stp q30, q31, [sp, #768]

    // Pass TrapFrame pointer to Rust
    mov x0, sp
    bl rust_exception_handler

    // 4. Restore SIMD/NEON Vector Registers
    ldp q0,  q1,  [sp, #288]
    ldp q2,  q3,  [sp, #320]
    ldp q4,  q5,  [sp, #352]
    ldp q6,  q7,  [sp, #384]
    ldp q8,  q9,  [sp, #416]
    ldp q10, q11, [sp, #448]
    ldp q12, q13, [sp, #480]
    ldp q14, q15, [sp, #512]
    ldp q16, q17, [sp, #544]
    ldp q18, q19, [sp, #576]
    ldp q20, q21, [sp, #608]
    ldp q22, q23, [sp, #640]
    ldp q24, q25, [sp, #672]
    ldp q26, q27, [sp, #704]
    ldp q28, q29, [sp, #736]
    ldp q30, q31, [sp, #768]

    // 5. Restore System & General Purpose Registers
    ldp x30, x0,  [sp, #240]
    ldp x1, x2,   [sp, #256]
    msr elr_el1, x0
    msr spsr_el1, x1

    ldp x0, x1,   [sp, #0]
    ldp x2, x3,   [sp, #16]
    ldp x4, x5,   [sp, #32]
    ldp x6, x7,   [sp, #48]
    ldp x8, x9,   [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]

    add sp, sp, #800
    eret

unhandled_trap:
    wfe
    b unhandled_trap

.section .bss
.balign 16
boot_stack:
    .space 16384
boot_stack_top:
