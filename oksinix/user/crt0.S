/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix - Standalone Ring-3 Linux i386 ELF32 CRT0 entry point
 */

.section .text
.code32
.global _start
.type _start, @function
_start:
    /* System V i386 ELF stack: 0(%esp) = argc, 4(%esp) = argv[0], ... */
    movl 0(%esp), %eax
    leal 4(%esp), %ecx
    xorl %ebp, %ebp
    pushl %ecx
    pushl %eax
    call main

    /* Exit via Linux i386 SYS_exit_group (252) with main() return code */
    movl %eax, %ebx
    movl $252, %eax
    int $0x80
1:  jmp 1b

.size _start, . - _start
.section .note.GNU-stack,"",@progbits
