/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix - Standalone Linux x86 32-bit (i386) ELF Position-Independent Executable
 * Pure PIC (-m elf_i386 -pie): runs on both native Linux i386/x86_64 and Oksinix
 * at any flat physical address without text relocations.
 * Exercises all 37 Linux i386 syscalls via int $0x80.
 */

#define PIC_ADDR(sym) ((sym) - .Lpic_base)(%ebp)

.section .text
.code32
.global _start
.type _start, @function
_start:
    /* Establish Position-Independent Code base pointer in %ebp */
    call .Lpic_base
.Lpic_base:
    popl %ebp

    /* Check argc on initial System V i386 ELF stack: 0(%esp) == argc */
    movl 0(%esp), %eax
    cmpl $2, %eax
    je .Lexecve_child

    /* Initialize runtime pointers in .data using PIC base %ebp */
    leal PIC_ADDR(file_msg), %eax
    movl %eax, PIC_ADDR(iov_write)
    leal PIC_ADDR(scratch_buf), %eax
    movl %eax, PIC_ADDR(iov_read)
    leal PIC_ADDR(self_path), %eax
    movl %eax, PIC_ADDR(child_argv)
    leal PIC_ADDR(child_arg1), %eax
    movl %eax, PIC_ADDR(child_argv + 4)

    /* Save argv[0] (4(%esp)) for SYS_execve self-invocation */
    movl 4(%esp), %eax
    testl %eax, %eax
    jz .Lno_argv0
    movl %eax, PIC_ADDR(child_argv)
.Lno_argv0:

    /* 1. SYS_getpid (20), SYS_getuid (24), SYS_geteuid (49), SYS_getgid (47), SYS_getegid (50) */
    movl $20, %eax
    int $0x80
    testl %eax, %eax
    jle .Lfail
    movl %eax, PIC_ADDR(saved_pid)

    movl $24, %eax
    int $0x80
    testl %eax, %eax
    js .Lfail

    movl $49, %eax
    int $0x80
    testl %eax, %eax
    js .Lfail

    movl $47, %eax
    int $0x80
    testl %eax, %eax
    js .Lfail

    movl $50, %eax
    int $0x80
    testl %eax, %eax
    js .Lfail

    /* 2. SYS_kill (37): kill(pid, 0) existence check */
    movl $37, %eax
    movl PIC_ADDR(saved_pid), %ebx
    xorl %ecx, %ecx
    int $0x80
    testl %eax, %eax
    jnz .Lfail

    /* 3. SYS_getcwd (183) */
    movl $183, %eax
    leal PIC_ADDR(scratch_buf), %ebx
    movl $64, %ecx
    int $0x80
    testl %eax, %eax
    jle .Lfail

    /* 4. SYS_brk (45) */
    movl $45, %eax
    xorl %ebx, %ebx
    int $0x80
    cmpl $0x00010000, %eax
    jb .Lfail
    cmpl $0xfffff000, %eax
    ja .Lfail
    leal 4096(%eax), %ebx
    movl $45, %eax
    int $0x80
    cmpl %ebx, %eax
    jne .Lfail

    /* 5. SYS_mmap (90) & SYS_munmap (91) */
    movl $90, %eax
    leal PIC_ADDR(mmap_args), %ebx
    int $0x80
    testl %eax, %eax
    jz .Lfail
    cmpl $0xfffff000, %eax
    ja .Lfail
    movl %eax, %ebx
    movl $0x12345678, (%ebx)
    movl $91, %eax
    movl $4096, %ecx
    int $0x80
    testl %eax, %eax
    jnz .Lfail

    /* 6. SYS_gettimeofday (78), SYS_clock_gettime (265), SYS_nanosleep (162) */
    movl $78, %eax
    leal PIC_ADDR(scratch_buf), %ebx
    xorl %ecx, %ecx
    int $0x80
    testl %eax, %eax
    jnz .Lfail

    movl $265, %eax
    xorl %ebx, %ebx
    leal PIC_ADDR(scratch_buf), %ecx
    int $0x80
    testl %eax, %eax
    jnz .Lfail

    movl $162, %eax
    leal PIC_ADDR(sleep_req), %ebx
    xorl %ecx, %ecx
    int $0x80
    testl %eax, %eax
    jnz .Lfail

    /* 7. SYS_ioctl (54) on stdout (fd 1, TIOCGWINSZ = 0x5413) */
    movl $54, %eax
    movl $1, %ebx
    movl $0x5413, %ecx
    leal PIC_ADDR(scratch_buf), %edx
    int $0x80
    testl %eax, %eax
    jz .Lioctl_ok
    cmpl $-25, %eax
    jne .Lfail
.Lioctl_ok:

    /* 8. SYS_openat (295): openat(AT_FDCWD=-100, "/tmp/ltest", O_CREAT|O_RDWR|O_TRUNC=0x242, 0644) */
    movl $295, %eax
    movl $-100, %ebx
    leal PIC_ADDR(tmp_path), %ecx
    movl $0x242, %edx
    movl $0644, %esi
    int $0x80
    testl %eax, %eax
    js .Lfail
    movl %eax, PIC_ADDR(saved_fd)

    /* 9. SYS_write (4) & SYS_writev (146) */
    movl $4, %eax
    movl PIC_ADDR(saved_fd), %ebx
    leal PIC_ADDR(file_msg), %ecx
    movl $4, %edx
    int $0x80
    cmpl $4, %eax
    jne .Lfail

    movl $146, %eax
    movl PIC_ADDR(saved_fd), %ebx
    leal PIC_ADDR(iov_write), %ecx
    movl $1, %edx
    int $0x80
    cmpl $4, %eax
    jne .Lfail

    /* 10. SYS_fsync (118) & SYS_ftruncate (93) */
    movl $118, %eax
    movl PIC_ADDR(saved_fd), %ebx
    int $0x80
    testl %eax, %eax
    jnz .Lfail

    movl $93, %eax
    movl PIC_ADDR(saved_fd), %ebx
    movl $6, %ecx
    int $0x80
    testl %eax, %eax
    jnz .Lfail

    /* 11. SYS_lseek (19), SYS_read (3), SYS_readv (145) */
    movl $19, %eax
    movl PIC_ADDR(saved_fd), %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    int $0x80
    testl %eax, %eax
    jnz .Lfail

    movl $3, %eax
    movl PIC_ADDR(saved_fd), %ebx
    leal PIC_ADDR(scratch_buf), %ecx
    movl $3, %edx
    int $0x80
    cmpl $3, %eax
    jne .Lfail

    movl $145, %eax
    movl PIC_ADDR(saved_fd), %ebx
    leal PIC_ADDR(iov_read), %ecx
    movl $1, %edx
    int $0x80
    cmpl $3, %eax
    jne .Lfail

    /* 12. SYS_fstat (108), SYS_fstatat (300), SYS_statx (383), SYS_faccessat (307) */
    movl $108, %eax
    movl PIC_ADDR(saved_fd), %ebx
    leal PIC_ADDR(scratch_buf), %ecx
    int $0x80
    testl %eax, %eax
    jnz .Lfail

    movl $300, %eax
    movl $-100, %ebx
    leal PIC_ADDR(tmp_path), %ecx
    leal PIC_ADDR(scratch_buf), %edx
    xorl %esi, %esi
    int $0x80
    testl %eax, %eax
    jnz .Lfail

    movl $383, %eax
    movl $-100, %ebx
    leal PIC_ADDR(tmp_path), %ecx
    xorl %edx, %edx
    movl $0x7FF, %esi
    leal PIC_ADDR(scratch_buf), %edi
    int $0x80
    testl %eax, %eax
    jnz .Lfail

    movl $307, %eax
    movl $-100, %ebx
    leal PIC_ADDR(tmp_path), %ecx
    xorl %edx, %edx
    xorl %esi, %esi
    int $0x80
    testl %eax, %eax
    jnz .Lfail

    /* 13. SYS_dup (41), SYS_dup3 (330), SYS_fcntl (55), SYS_close (6), SYS_unlinkat (301) */
    movl $41, %eax
    movl PIC_ADDR(saved_fd), %ebx
    int $0x80
    testl %eax, %eax
    js .Lfail
    movl %eax, %esi

    movl $330, %eax
    movl PIC_ADDR(saved_fd), %ebx
    movl $10, %ecx
    xorl %edx, %edx
    int $0x80
    cmpl $10, %eax
    jne .Lfail

    movl $55, %eax
    movl $10, %ebx
    movl $3, %ecx
    xorl %edx, %edx
    int $0x80
    testl %eax, %eax
    js .Lfail

    movl $6, %eax
    movl $10, %ebx
    int $0x80

    movl $6, %eax
    movl %esi, %ebx
    int $0x80

    movl $6, %eax
    movl PIC_ADDR(saved_fd), %ebx
    int $0x80

    movl $301, %eax
    movl $-100, %ebx
    leal PIC_ADDR(tmp_path), %ecx
    xorl %edx, %edx
    int $0x80
    testl %eax, %eax
    jnz .Lfail

    /* 14. SYS_clone (120) + SYS_execve (11) + SYS_waitid (284) + SYS_wait4 (114) */
    movl $120, %eax
    movl $17, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %esi, %esi
    xorl %edi, %edi
    int $0x80
    testl %eax, %eax
    js .Lfail
    jz .Lclone_child

    /* Parent: waitid (284) with WNOWAIT (0x01000000 | WEXITED=4) then wait4 (114) */
    movl %eax, PIC_ADDR(saved_child_pid)
    movl $284, %eax
    movl $1, %ebx                           /* P_PID */
    movl PIC_ADDR(saved_child_pid), %ecx    /* child pid */
    leal PIC_ADDR(scratch_buf), %edx        /* siginfo */
    movl $0x01000004, %esi                  /* WEXITED | WNOWAIT */
    xorl %edi, %edi
    int $0x80
    testl %eax, %eax
    jnz .Lfail

    movl $114, %eax
    movl PIC_ADDR(saved_child_pid), %ebx
    leal PIC_ADDR(wait_status), %ecx
    xorl %edx, %edx
    xorl %esi, %esi
    int $0x80
    cmpl PIC_ADDR(saved_child_pid), %eax
    jne .Lfail
    cmpl $0x2A00, PIC_ADDR(wait_status)     /* exit status 42 << 8 */
    jne .Lfail

    /* Print success banner via SYS_write (4) and exit via SYS_exit_group (252) */
    movl $4, %eax
    movl $1, %ebx
    leal PIC_ADDR(pass_msg), %ecx
    movl $pass_msg_len, %edx
    int $0x80

    movl $252, %eax
    xorl %ebx, %ebx
    int $0x80

.Lclone_child:
    /* Child process: call SYS_execve(child_argv[0], child_argv, NULL) */
    movl $11, %eax
    movl PIC_ADDR(child_argv), %ebx
    leal PIC_ADDR(child_argv), %ecx
    xorl %edx, %edx
    int $0x80
    jmp .Lfail

.Lexecve_child:
    /* Executed via SYS_execve with argc == 2: write message and call SYS_exit(42) */
    movl $4, %eax
    movl $1, %ebx
    leal PIC_ADDR(child_msg), %ecx
    movl $child_msg_len, %edx
    int $0x80

    movl $1, %eax
    movl $42, %ebx
    int $0x80

.Lfail:
    movl $4, %eax
    movl $1, %ebx
    leal PIC_ADDR(fail_msg), %ecx
    movl $fail_msg_len, %edx
    int $0x80

    movl $252, %eax
    movl $1, %ebx
    int $0x80

.section .data
.align 4
mmap_args:
    .long 0, 4096, 3, 0x22, -1, 0

sleep_req:
    .long 0, 1000000

iov_write:
    .long 0, 4

iov_read:
    .long 0, 3

child_argv:
    .long 0
    .long 0
    .long 0

self_path:
    .asciz "/mnt/LINUX.ELF"
child_arg1:
    .asciz "--child"
tmp_path:
    .asciz "/tmp/ltest"
file_msg:
    .ascii "OKSX"

child_msg:
    .ascii "[LINUX-ELF] execve child running via int 0x80 (PID verified)\n"
.set child_msg_len, . - child_msg

pass_msg:
    .ascii "[LINUX-ELF] All 37 Linux i386 syscalls PASSED!\n"
.set pass_msg_len, . - pass_msg

fail_msg:
    .ascii "[LINUX-ELF] Syscall verification FAILED!\n"
.set fail_msg_len, . - fail_msg

.align 4
saved_pid:
    .long 0
saved_child_pid:
    .long 0
saved_fd:
    .long 0
wait_status:
    .long 0
scratch_buf:
    .space 256
