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
.align 4096
.global kernel_stack_guard
.global kernel_stack_bottom
.global kernel_stack_top
kernel_stack_guard:
.skip 4096  /* 4 KiB unmapped hardware guard page below boot kernel stack */
stack_bottom:
kernel_stack_bottom:
.skip 32768 /* 32 KiB kernel stack */
stack_top:
kernel_stack_top:

.align 4096
.global exec_kstack_guard
.global exec_kstack_bottom
.global exec_kstack_top
exec_kstack_guard:
.skip 4096  /* 4 KiB unmapped hardware guard page below ELF syscall/execve kernel stack */
exec_kstack_bottom:
.skip 32768 /* 32 KiB dedicated Ring-0 kernel stack for Ring-3 Linux ELF syscalls/execve */
exec_kstack_top:

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
