.section .text
.balign 4

// ==========================================================================
// EL0 TTY ioctl & non-blocking sys_read verification suite
// Entry: x0 = pointer to HAL::Test::TtyIoctlSuiteArgs in user memory
//   [x0, #0]  = rw_buf_vaddr
//   [x0, #8]  = ro_buf_vaddr
//   [x0, #16] = crossing_buf_vaddr
//   [x0, #24] = crossing_buf_len
//   [x0, #32] = non_tty_fd
// Exits via SYS_EXIT (#93) with failure count in x0 (0 == all passed)
// ==========================================================================
.global el0_tty_ioctl_suite_start
.global el0_tty_ioctl_suite_end
el0_tty_ioctl_suite_start:
    mov     x20, x0                 // Save args pointer
    mov     x19, #0                 // Failure accumulator

    // 1) ioctl(fd=1, TCGETS=0x5401, rw_buf_vaddr) -> 0, ICANON|ECHO set in c_lflag
    mov     x8, #29                 // SYS_IOCTL
    mov     x0, #1                  // STDOUT
    mov     x1, #0x5401             // TCGETS
    ldr     x2, [x20, #0]           // rw_buf_vaddr
    svc     #0
    cbz     x0, 1f
    add     x19, x19, #1
1:
    ldr     x1, [x20, #0]
    ldr     w4, [x1, #12]           // ktermios.c_lflag
    mov     w6, #0xA                // ICANON (0x2) | ECHO (0x8)
    and     w5, w4, w6
    cmp     w5, w6
    b.eq    2f
    add     x19, x19, #1
2:

    // 2) ioctl(fd=0, TCGETS=0x5401, rw_buf_vaddr) -> 0
    mov     x8, #29                 // SYS_IOCTL
    mov     x0, #0                  // STDIN
    mov     x1, #0x5401             // TCGETS
    ldr     x2, [x20, #0]
    svc     #0
    cbz     x0, 3f
    add     x19, x19, #1
3:

    // 3) ioctl(fd=2, TCGETS=0x5401, rw_buf_vaddr) -> 0
    mov     x8, #29                 // SYS_IOCTL
    mov     x0, #2                  // STDERR
    mov     x1, #0x5401             // TCGETS
    ldr     x2, [x20, #0]
    svc     #0
    cbz     x0, 4f
    add     x19, x19, #1
4:

    // 4) Modify c_lflag via TCSETS (0x5402), verify via TCGETS, restore via TCSETSW (0x5403)
    ldr     x1, [x20, #0]
    ldr     w21, [x1, #12]          // Save original c_lflag
    eor     w4, w21, #1             // Toggle bit 0
    str     w4, [x1, #12]

    mov     x8, #29                 // SYS_IOCTL
    mov     x0, #1
    mov     x1, #0x5402             // TCSETS
    ldr     x2, [x20, #0]
    svc     #0
    cbz     x0, 5f
    add     x19, x19, #1
5:

    ldr     x1, [x20, #0]
    str     wzr, [x1, #12]          // Clear local copy before reading back
    mov     x8, #29                 // SYS_IOCTL
    mov     x0, #1
    mov     x1, #0x5401             // TCGETS
    ldr     x2, [x20, #0]
    svc     #0
    cbz     x0, 6f
    add     x19, x19, #1
6:
    ldr     x1, [x20, #0]
    ldr     w4, [x1, #12]
    eor     w5, w21, #1
    cmp     w4, w5
    b.eq    7f
    add     x19, x19, #1
7:

    str     w21, [x1, #12]          // Restore original c_lflag
    mov     x8, #29                 // SYS_IOCTL
    mov     x0, #1
    mov     x1, #0x5403             // TCSETSW
    ldr     x2, [x20, #0]
    svc     #0
    cbz     x0, 8f
    add     x19, x19, #1
8:

    // 5) ioctl(fd=1, TIOCGWINSZ=0x5413, rw_buf_vaddr) -> 0, ws_row==24, ws_col==80
    mov     x8, #29                 // SYS_IOCTL
    mov     x0, #1
    mov     x1, #0x5413             // TIOCGWINSZ
    ldr     x2, [x20, #0]
    svc     #0
    cbz     x0, 9f
    add     x19, x19, #1
9:
    ldr     x1, [x20, #0]
    ldrh    w4, [x1, #0]            // winsize.ws_row
    cmp     w4, #24
    b.eq    10f
    add     x19, x19, #1
10:
    ldrh    w5, [x1, #2]            // winsize.ws_col
    cmp     w5, #80
    b.eq    11f
    add     x19, x19, #1
11:

    // 6) ioctl(fd=99, TCGETS, rw_buf_vaddr) -> -9 (-EBADF)
    mov     x8, #29                 // SYS_IOCTL
    mov     x0, #99
    mov     x1, #0x5401
    ldr     x2, [x20, #0]
    svc     #0
    cmn     x0, #9
    b.eq    12f
    add     x19, x19, #1
12:

    // 7) ioctl(fd=5 [closed], TCGETS, rw_buf_vaddr) -> -9 (-EBADF)
    mov     x8, #29                 // SYS_IOCTL
    mov     x0, #5
    mov     x1, #0x5401
    ldr     x2, [x20, #0]
    svc     #0
    cmn     x0, #9
    b.eq    13f
    add     x19, x19, #1
13:

    // 8) ioctl(non_tty_fd, TCGETS, rw_buf_vaddr) -> -25 (-ENOTTY)
    mov     x8, #29                 // SYS_IOCTL
    ldr     x0, [x20, #32]          // non_tty_fd
    mov     x1, #0x5401
    ldr     x2, [x20, #0]
    svc     #0
    cmn     x0, #25
    b.eq    14f
    add     x19, x19, #1
14:

    // 9) ioctl(fd=1, 0x9999, rw_buf_vaddr) -> -22 (-EINVAL)
    mov     x8, #29                 // SYS_IOCTL
    mov     x0, #1
    mov     x1, #0x9999
    ldr     x2, [x20, #0]
    svc     #0
    cmn     x0, #22
    b.eq    15f
    add     x19, x19, #1
15:

    // 10) ioctl(fd=1, TCGETS, ro_buf_vaddr) -> -14 (-EFAULT)
    mov     x8, #29                 // SYS_IOCTL
    mov     x0, #1
    mov     x1, #0x5401
    ldr     x2, [x20, #8]           // ro_buf_vaddr
    svc     #0
    cmn     x0, #14
    b.eq    16f
    add     x19, x19, #1
16:

    // 11) sys_read(fd=0, ro_buf_vaddr, 4) -> -14 (-EFAULT) without blocking
    mov     x8, #63                 // SYS_READ
    mov     x0, #0
    ldr     x1, [x20, #8]           // ro_buf_vaddr
    mov     x2, #4
    svc     #0
    cmn     x0, #14
    b.eq    17f
    add     x19, x19, #1
17:

    // 12) sys_read(fd=0, crossing_buf_vaddr, crossing_buf_len) -> -14 (-EFAULT)
    mov     x8, #63                 // SYS_READ
    mov     x0, #0
    ldr     x1, [x20, #16]          // crossing_buf_vaddr
    ldr     x2, [x20, #24]          // crossing_buf_len
    svc     #0
    cmn     x0, #14
    b.eq    18f
    add     x19, x19, #1
18:

    // 13) sys_read(fd=1 [write-only], rw_buf_vaddr, 4) -> -9 (-EBADF)
    mov     x8, #63                 // SYS_READ
    mov     x0, #1
    ldr     x1, [x20, #0]
    mov     x2, #4
    svc     #0
    cmn     x0, #9
    b.eq    19f
    add     x19, x19, #1
19:

    // 14) sys_read(fd=99, rw_buf_vaddr, 4) -> -9 (-EBADF)
    mov     x8, #63                 // SYS_READ
    mov     x0, #99
    ldr     x1, [x20, #0]
    mov     x2, #4
    svc     #0
    cmn     x0, #9
    b.eq    20f
    add     x19, x19, #1
20:

    // 15) sys_read(fd=0, rw_buf_vaddr, 0) -> 0 without blocking
    mov     x8, #63                 // SYS_READ
    mov     x0, #0
    ldr     x1, [x20, #0]
    mov     x2, #0
    svc     #0
    cbz     x0, 21f
    add     x19, x19, #1
21:

    // 16) Exit with accumulated failure count
    mov     x8, #93                 // SYS_EXIT
    mov     x0, x19
    svc     #0
el0_tty_ioctl_suite_end:

// ==========================================================================
// EL0 Interactive TTY Echo Program
// Entry: x0 = pointer to HAL::Test::TtyEchoArgs in user memory
//   [x0, #0]  = prompt_vaddr
//   [x0, #8]  = prompt_len
//   [x0, #16] = prefix_vaddr
//   [x0, #24] = prefix_len
//   [x0, #32] = line_buf_vaddr
//   [x0, #40] = line_buf_cap
// ==========================================================================
.global el0_tty_echo_start
.global el0_tty_echo_end
el0_tty_echo_start:
    mov     x20, x0                 // Save args pointer
    mov     x19, #0                 // Exit status

    // 1) write(fd=1, prompt_vaddr, prompt_len)
    mov     x8, #64                 // SYS_WRITE
    mov     x0, #1
    ldr     x1, [x20, #0]
    ldr     x2, [x20, #8]
    svc     #0

    // 2) n = read(fd=0, line_buf_vaddr, line_buf_cap)
    mov     x8, #63                 // SYS_READ
    mov     x0, #0
    ldr     x1, [x20, #32]
    ldr     x2, [x20, #40]
    svc     #0
    mov     x21, x0                 // Save bytes read

    cmp     x21, #0
    b.gt    1f
    add     x19, x19, #1
    b       2f
1:
    // 3) write(fd=1, prefix_vaddr, prefix_len)
    mov     x8, #64                 // SYS_WRITE
    mov     x0, #1
    ldr     x1, [x20, #16]
    ldr     x2, [x20, #24]
    svc     #0

    // 4) write(fd=1, line_buf_vaddr, x21)
    mov     x8, #64                 // SYS_WRITE
    mov     x0, #1
    ldr     x1, [x20, #32]
    mov     x2, x21
    svc     #0
2:
    // 5) exit(x19)
    mov     x8, #93                 // SYS_EXIT
    mov     x0, x19
    svc     #0
el0_tty_echo_end:

// ==========================================================================
// EL0 System V ABI argc/argv Echo Test
// Entry: x0 = argc, x1 = argv (NULL-terminated array of char* pointers)
// Prints all argv[0..argc-1] arguments to STDOUT (fd=1), verifies
// argv[argc] == NULL, and exits via SYS_EXIT (#93) with 0 on success.
// ==========================================================================
.global el0_user_echo_start
.global el0_user_echo_end
el0_user_echo_start:
    mov     x19, x0                 // x19 = argc
    mov     x20, x1                 // x20 = argv
    mov     x21, #0                 // x21 = i = 0
    mov     x22, #0                 // x22 = failure count = 0

    // Verify argc > 0 and argv != NULL
    cmp     x19, #0
    b.le    .Lecho_fail_exit
    cbz     x20, .Lecho_fail_exit

    // Print header "  [EL0 argv] "
    mov     x8, #64                 // SYS_WRITE
    mov     x0, #1                  // STDOUT
    adr     x1, .Lecho_hdr
    mov     x2, #13
    svc     #0
    cmp     x0, #13
    b.eq    .Lecho_loop
    add     x22, x22, #1

.Lecho_loop:
    cmp     x21, x19
    b.ge    .Lecho_loop_done

    // Load argv[i]
    ldr     x23, [x20, x21, lsl #3]
    cbz     x23, .Lecho_null_arg

    // Compute strlen(argv[i]) in x24
    mov     x24, #0
.Lecho_strlen:
    ldrb    w25, [x23, x24]
    cbz     w25, .Lecho_strlen_done
    add     x24, x24, #1
    b       .Lecho_strlen

.Lecho_strlen_done:
    // write(1, argv[i], len)
    mov     x8, #64                 // SYS_WRITE
    mov     x0, #1
    mov     x1, x23
    mov     x2, x24
    svc     #0
    cmp     x0, x24
    b.eq    1f
    add     x22, x22, #1
1:
    // Print space separator if (i + 1) < argc
    add     x25, x21, #1
    cmp     x25, x19
    b.ge    2f
    mov     x8, #64                 // SYS_WRITE
    mov     x0, #1
    adr     x1, .Lecho_space
    mov     x2, #1
    svc     #0
2:
    add     x21, x21, #1
    b       .Lecho_loop

.Lecho_null_arg:
    add     x22, x22, #1

.Lecho_loop_done:
    // Verify System V ABI invariant: argv[argc] == NULL
    ldr     x23, [x20, x19, lsl #3]
    cbz     x23, 3f
    add     x22, x22, #1
3:
    // Print "\r\n"
    mov     x8, #64                 // SYS_WRITE
    mov     x0, #1
    adr     x1, .Lecho_crlf
    mov     x2, #2
    svc     #0

    // exit(x22)
    mov     x8, #93                 // SYS_EXIT
    mov     x0, x22
    svc     #0

.Lecho_fail_exit:
    mov     x8, #93                 // SYS_EXIT
    mov     x0, #1
    svc     #0

.Lecho_hdr:
    .ascii  "  [EL0 argv] "
.Lecho_space:
    .ascii  " "
.Lecho_crlf:
    .ascii  "\r\n"
.balign 4
el0_user_echo_end:

// ==========================================================================
// EL0 User Heap (sys_brk) & Anonymous Virtual Memory (sys_mmap/sys_munmap) Test
// Entry: x0 = pointer to HAL::Test::UserMmSuiteArgs in user memory
//   [x0, #0] = expected_brk_start (page-aligned end of ELF .bss)
// Exits via SYS_EXIT (#93) with failure count in x0 (0 == all passed)
// ==========================================================================
.global el0_user_mm_suite_start
.global el0_user_mm_suite_end
el0_user_mm_suite_start:
    mov     x20, x0                 // Save args pointer
    mov     x19, #0                 // Failure accumulator
    ldr     x21, [x20, #0]          // x21 = expected_brk_start

    // 1) brk(0) -> must return expected_brk_start
    mov     x8, #214                // SYS_BRK
    mov     x0, #0
    svc     #0
    cmp     x0, x21
    b.eq    1f
    add     x19, x19, #1
1:

    // 2) brk(brk_start - 0x1000) -> below brk_start must return unchanged brk_start
    mov     x8, #214                // SYS_BRK
    sub     x0, x21, #0x1000
    svc     #0
    cmp     x0, x21
    b.eq    2f
    add     x19, x19, #1
2:

    // 3) Expand brk by 3 pages (brk_start + 0x3000) -> returns new break
    add     x22, x21, #0x3000       // x22 = target expanded break
    mov     x8, #214                // SYS_BRK
    mov     x0, x22
    svc     #0
    cmp     x0, x22
    b.eq    3f
    add     x19, x19, #1
3:

    // Verify new heap pages are zero-filled and read/write accessible across all 3 pages
    ldr     x4, [x21, #0]
    cbz     x4, 4f
    add     x19, x19, #1
4:
    movz    x5, #0x7788
    movk    x5, #0x5566, lsl #16
    movk    x5, #0x3344, lsl #32
    movk    x5, #0x1122, lsl #48
    str     x5, [x21, #0]           // Page 0
    str     x5, [x21, #0x1000]      // Page 1
    mov     x6, #0x2FF8
    add     x6, x21, x6             // End of Page 2
    str     x5, [x6]

    ldr     x4, [x21, #0x1000]
    cmp     x4, x5
    b.eq    5f
    add     x19, x19, #1
5:
    ldr     x4, [x6]
    cmp     x4, x5
    b.eq    6f
    add     x19, x19, #1
6:

    // 4) brk(0x3FFE0000) above heap ceiling -> must fail and return unchanged x22
    mov     x8, #214                // SYS_BRK
    mov     x0, #0x3FFE0000
    svc     #0
    cmp     x0, x22
    b.eq    7f
    add     x19, x19, #1
7:

    // 5) Shrink brk back to expected_brk_start -> returns x21
    mov     x8, #214                // SYS_BRK
    mov     x0, x21
    svc     #0
    cmp     x0, x21
    b.eq    8f
    add     x19, x19, #1
8:

    // 6) mmap(0, 2 * 4096, PROT_READ|PROT_WRITE (0x3), MAP_PRIVATE|MAP_ANONYMOUS (0x22), -1, 0)
    mov     x8, #222                // SYS_MMAP
    mov     x0, #0
    mov     x1, #0x2000             // 2 pages (8192 bytes)
    mov     x2, #0x3                // PROT_READ | PROT_WRITE
    mov     x3, #0x22               // MAP_PRIVATE | MAP_ANONYMOUS
    mov     x4, #-1                 // fd = -1
    mov     x5, #0                  // offset = 0
    svc     #0
    mov     x23, x0                 // Save mapped address

    // Verify mapped address is in [0x30000000, 0x3FFE0000) and 4 KiB aligned
    mov     x3, #0x30000000
    cmp     x23, x3
    b.hs    9f
    add     x19, x19, #1
9:
    mov     x3, #0x3FFE0000
    cmp     x23, x3
    b.lo    10f
    add     x19, x19, #1
10:
    tst     x23, #0xFFF
    b.eq    11f
    add     x19, x19, #1
11:

    // Verify anonymous mmap pages are zero-filled and read/write accessible
    ldr     x4, [x23, #0]
    cbz     x4, 12f
    add     x19, x19, #1
12:
    movz    x5, #0x5678
    movk    x5, #0x1234, lsl #16
    movk    x5, #0xBABE, lsl #32
    movk    x5, #0xCAFE, lsl #48
    str     x5, [x23, #0]
    str     x5, [x23, #0x1FF8]
    ldr     x4, [x23, #0x1FF8]
    cmp     x4, x5
    b.eq    13f
    add     x19, x19, #1
13:

    // 7) mmap with unaligned length (1234) -> -22 (-EINVAL)
    mov     x8, #222                // SYS_MMAP
    mov     x0, #0
    mov     x1, #1234
    mov     x2, #0x3
    mov     x3, #0x22
    mov     x4, #-1
    mov     x5, #0
    svc     #0
    cmn     x0, #22
    b.eq    14f
    add     x19, x19, #1
14:

    // 8) mmap without MAP_ANONYMOUS (flags = MAP_PRIVATE = 0x2) -> -22 (-EINVAL)
    mov     x8, #222                // SYS_MMAP
    mov     x0, #0
    mov     x1, #0x1000
    mov     x2, #0x3
    mov     x3, #0x2
    mov     x4, #-1
    mov     x5, #0
    svc     #0
    cmn     x0, #22
    b.eq    15f
    add     x19, x19, #1
15:

    // 9) munmap with unaligned address -> -22 (-EINVAL)
    mov     x8, #215                // SYS_MUNMAP
    add     x0, x23, #16
    mov     x1, #0x1000
    svc     #0
    cmn     x0, #22
    b.eq    16f
    add     x19, x19, #1
16:

    // 10) munmap valid mapped region -> 0
    mov     x8, #215                // SYS_MUNMAP
    mov     x0, x23
    mov     x1, #0x2000
    svc     #0
    cbz     x0, 17f
    add     x19, x19, #1
17:

    // Exit with failure count
    mov     x8, #93                 // SYS_EXIT
    mov     x0, x19
    svc     #0
el0_user_mm_suite_end: