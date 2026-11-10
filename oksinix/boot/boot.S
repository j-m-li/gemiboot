/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit Multiboot & BIOS MBR entry point
 */

.set MULTIBOOT_MAGIC,    0x1BADB002
.set MULTIBOOT_ALIGN,    (1 << 0)
.set MULTIBOOT_MEMINFO,  (1 << 1)
.set MULTIBOOT_FLAGS,    (MULTIBOOT_ALIGN | MULTIBOOT_MEMINFO)
.set MULTIBOOT_CHECKSUM, -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

.section .multiboot, "a"
.align 4
.long MULTIBOOT_MAGIC
.long MULTIBOOT_FLAGS
.long MULTIBOOT_CHECKSUM
/* Pad Multiboot header to 48 bytes (0x30) so _start stays at 0x00100030 */
.long 0
.long 0
.long 0
.long 0
.long 0
.long 0
.long 0
.long 0
.long 0

.section .bss
.align 16
stack_bottom:
.skip 32768 /* 32 KiB kernel stack */
stack_top:

.section .text.entry, "ax"
.code32
.global _start
.type _start, @function
_start:
    cli
    cld

    /* Preserve %eax (Multiboot magic) and %ebx (Multiboot info) across .bss clear */
    movl %eax, %esi
    movl %ebx, %ebp

    /* Zero .bss section (required when booting from raw binary / MBR disk image) */
    movl $_bss_start, %edi
    movl $_bss_end, %ecx
    subl %edi, %ecx
    xorl %eax, %eax
    rep stosb

    movl $stack_top, %esp

    /* Push Multiboot info pointer (%ebp) and magic (%esi) */
    pushl %ebp
    pushl %esi
    xorl %ebp, %ebp

    call kernel_main

.Lhalt:
    cli
    hlt
    jmp .Lhalt

.size _start, . - _start
.section .note.GNU-stack,"",@progbits
