// Map entry code directly to .text.boot in linker.ld
.section .text.boot
.global _start

_start:
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #3
    b.eq    .Lsetup_el3
    cmp     x0, #2
    b.eq    .Lsetup_el2
    b       .Lsetup_el1

.Lsetup_el3:
    mov     x0, #0x531             // SCR_EL3: RW=1 (64-bit EL2), NS=1
    msr     scr_el3, x0
    mov     x0, #0x3c9             // SPSR_EL3: EL2h (0x9) DAIF masked
    msr     spsr_el3, x0
    ldr     x0, =.Lsetup_el2
    msr     elr_el3, x0
    isb
    eret

.Lsetup_el2:
    msr     cptr_el2, xzr          // Disable EL2 coprocessor traps (FP/SIMD)
    mov     x0, #(1 << 31)         // HCR_EL2: RW=1 (64-bit EL1)
    msr     hcr_el2, x0
    mov     x0, #0x3c5             // SPSR_EL2: EL1h (0x5) DAIF masked
    msr     spsr_el2, x0
    ldr     x0, =.Lsetup_el1
    msr     elr_el2, x0
    isb
    eret

.Lsetup_el1:
    ldr     x0, =_boot_stack_top
    mov     sp, x0

    // Enable SIMD/FP Coprocessor in EL1
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0

    // Set Vector Base Address (Must match .text.vectors section)
    ldr     x0, =el1_vector_table
    msr     vbar_el1, x0
    isb

    b       kmain

.ltorg  // Explicitly emit literal pool here so ldr x0, =... resolves correctly

// Map vector table directly to .text.vectors in linker.ld
.section .text.vectors
.balign 2048
.global el1_vector_table
el1_vector_table:
    // Current EL with SP0
    .align 7; b el1_trap_handler
    .align 7; b el1_trap_handler
    .align 7; b el1_trap_handler
    .align 7; b el1_trap_handler

    // Current EL with SPx
    .align 7; b el1_trap_handler
    .align 7; b el1_trap_handler
    .align 7; b el1_trap_handler
    .align 7; b el1_trap_handler

    // Lower EL using AArch64 (EL0 Traps)
    .align 7; b el0_sync_handler
    .align 7; b el0_async_handler  // IRQ
    .align 7; b el0_async_handler  // FIQ
    .align 7; b el0_async_handler  // SError

    // Lower EL using AArch32
    .align 7; b el0_async_handler
    .align 7; b el0_async_handler
    .align 7; b el0_async_handler
    .align 7; b el0_async_handler

.section .text
el0_sync_handler:
    mrs     x9, esr_el1
    lsr     x10, x9, #26
    cmp     x10, #0x15             // SVC in AArch64
    b.ne    .Luser_abort

    cmp     x8, #93                // SYS_EXIT
    b.eq    return_to_kernel

    mrs     x9, elr_el1
    add     x9, x9, #4
    msr     elr_el1, x9
    eret

.Luser_abort:
    mov     x0, #-1
    b       return_to_kernel

el0_async_handler:
    mov     x0, #-3
    b       return_to_kernel

el1_trap_handler:
    mov     x0, #-2
    b       return_to_kernel

.balign 4
.global enter_user_mode
enter_user_mode:
    adrp    x2, kernel_ctx
    add     x2, x2, :lo12:kernel_ctx

    stp     x19, x20, [x2, #0]
    stp     x21, x22, [x2, #16]
    stp     x23, x24, [x2, #32]
    stp     x25, x26, [x2, #48]
    stp     x27, x28, [x2, #64]
    stp     x29, x30, [x2, #80]
    mov     x3, sp
    str     x3,       [x2, #96]

    msr     sp_el0, x1
    msr     elr_el1, x0
    mov     x4, #0x3c0             // Mask interrupts for initial EL0 entry
    msr     spsr_el1, x4

    mov     x0, #0
    mov     x1, #0
    mov     x2, #0
    mov     x3, #0

    isb
    eret

.balign 4
.global return_to_kernel
return_to_kernel:
    ldr     x0, [sp]
    adrp    x2, kernel_ctx
    add     x2, x2, :lo12:kernel_ctx

    ldp     x19, x20, [x2, #0]
    ldp     x21, x22, [x2, #16]
    ldp     x23, x24, [x2, #32]
    ldp     x25, x26, [x2, #48]
    ldp     x27, x28, [x2, #64]
    ldp     x29, x30, [x2, #80]
    ldr     x1,       [x2, #96]
    mov     sp, x1
    ret

.section .text
.balign 4
.global user_space_code
user_space_code:
    mov     x8, #64
    mov     x0, #1
    adr     x1, inline_msg
    mov     x2, #66
    svc     #0

    mov     x8, #93
    mov     x0, #0
    svc     #0

.balign 4
inline_msg:
    .ascii "[EL0 USER SPACE] Successfully executed code inside EL0 User Mode!\r\n"
    .balign 4

.section .bss
.balign 16
.global kernel_ctx
kernel_ctx:
    .space 104

.balign 16
.global _boot_stack_bottom
.global _boot_stack_top
_boot_stack_bottom:
    .space 16384
_boot_stack_top:
