/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix - Ring-3 PIC i386 int $0x80 syscall helper for user/libc.so.6
 */

.section .text
.code32

.global __oksinix_int80
.type __oksinix_int80, @function
__oksinix_int80:
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl 20(%esp), %eax
    movl 24(%esp), %ebx
    movl 28(%esp), %ecx
    movl 32(%esp), %edx
    movl 36(%esp), %esi
    movl 40(%esp), %edi
    int $0x80
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret

.section .note.GNU-stack,"",@progbits
