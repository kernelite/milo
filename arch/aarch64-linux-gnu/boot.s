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
    mov     x0, #0x531              // SCR_EL3: RW=1 (64-bit EL2), NS=1
    msr     scr_el3, x0
    mov     x0, #0x3c9              // SPSR_EL3: EL2h (0x9) DAIF masked
    msr     spsr_el3, x0
    ldr     x0, =.Lsetup_el2
    msr     elr_el3, x0
    isb
    eret

.Lsetup_el2:
    msr     cptr_el2, xzr           // Disable EL2 coprocessor traps (FP/SIMD)
    mov     x0, #(1 << 31)          // HCR_EL2: RW=1 (64-bit EL1)
    msr     hcr_el2, x0
    mov     x0, #0x3c5              // SPSR_EL2: EL1h (0x5) DAIF masked
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

    // Register 2KB-aligned Exception Vector Table Base Address in VBAR_EL1
    ldr     x0, =el1_vector_table
    msr     vbar_el1, x0
    isb

    b       kmain

.ltorg

// ==========================================================================
// Context Save & Restore Macros
// Stack frame size: 272 bytes (16-byte aligned for AAPCS64 compliance)
// Layout matches HAL::CpuContext (x0-x30, sp_el0, elr_el1, spsr_el1)
// ==========================================================================

.macro SAVE_CONTEXT
    sub     sp, sp, #272
    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30,      [sp, #240]

    mrs     x9,  sp_el0
    mrs     x10, elr_el1
    mrs     x11, spsr_el1
    stp     x9,  x10, [sp, #248]
    str     x11,      [sp, #264]

    mov     x0, sp                  // Pass CpuContext* as first argument (x0)
.endm

.macro RESTORE_CONTEXT
    ldr     x11,      [sp, #264]
    ldp     x9,  x10, [sp, #248]
    msr     spsr_el1, x11
    msr     elr_el1,  x10
    msr     sp_el0,   x9

    str     xzr,      [sp, #264]    // Clean stack slot
    ldr     x30,      [sp, #240]
    ldp     x28, x29, [sp, #224]
    ldp     x26, x27, [sp, #208]
    ldp     x24, x25, [sp, #192]
    ldp     x22, x23, [sp, #176]
    ldp     x20, x21, [sp, #160]
    ldp     x18, x19, [sp, #144]
    ldp     x16, x17, [sp, #128]
    ldp     x14, x15, [sp, #112]
    ldp     x12, x13, [sp, #96]
    ldp     x10, x11, [sp, #80]
    ldp     x8,  x9,  [sp, #64]
    ldp     x6,  x7,  [sp, #48]
    ldp     x4,  x5,  [sp, #32]
    ldp     x2,  x3,  [sp, #16]
    ldp     x0,  x1,  [sp, #0]
    add     sp, sp, #272
.endm

// ==========================================================================
// Vector Table Common Entry Point Routines
// ==========================================================================

.section .text
.balign 4
sync_handler_entry:
    SAVE_CONTEXT
    mrs     x1, esr_el1             // Pass ESR_EL1 as second argument (x1)
    mrs     x2, far_el1             // Pass FAR_EL1 as third argument (x2)
    bl      aarch64_handle_sync_exception
    RESTORE_CONTEXT
    eret

.balign 4
invalid_handler_entry:
    SAVE_CONTEXT
    // x1 contains the exception type index set by vector table slot
    bl      aarch64_handle_invalid_exception
    RESTORE_CONTEXT
    eret

// ==========================================================================
// Arm64 16-Entry Vector Table (Aligned to 2048 bytes; 128 bytes per entry)
// ==========================================================================

.section .text.vectors
.balign 2048
.global el1_vector_table
el1_vector_table:
    // --- Current EL with SP0 ---
    .align 7; b sync_handler_entry
    .align 7; mov x1, #1; b invalid_handler_entry  // IRQ
    .align 7; mov x1, #2; b invalid_handler_entry  // FIQ
    .align 7; mov x1, #3; b invalid_handler_entry  // SError

    // --- Current EL with SPx ---
    .align 7; b sync_handler_entry
    .align 7; mov x1, #5; b invalid_handler_entry  // IRQ
    .align 7; mov x1, #6; b invalid_handler_entry  // FIQ
    .align 7; mov x1, #7; b invalid_handler_entry  // SError

    // --- Lower EL using AArch64 ---
    .align 7; b sync_handler_entry
    .align 7; mov x1, #9; b invalid_handler_entry  // IRQ
    .align 7; mov x1, #10; b invalid_handler_entry // FIQ
    .align 7; mov x1, #11; b invalid_handler_entry // SError

    // --- Lower EL using AArch32 ---
    .align 7; mov x1, #12; b invalid_handler_entry // Sync
    .align 7; mov x1, #13; b invalid_handler_entry // IRQ
    .align 7; mov x1, #14; b invalid_handler_entry // FIQ
    .align 7; mov x1, #15; b invalid_handler_entry // SError

// ==========================================================================
// User Mode & Arch Test Routines
// ==========================================================================

.section .text
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
    mov     x4, #0x3c0             // Mask interrupts for EL0 entry
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

.balign 4
.global trigger_svc_test
trigger_svc_test:
    mov     x8, #64                // SYS_WRITE
    mov     x0, #1                 // stdout
    adr     x1, .Lsvc_test_msg
    mov     x2, #34
    svc     #0
    ret

.balign 4
.Lsvc_test_msg:
    .ascii "[SVC TEST] Trap returned cleanly!\r\n"
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