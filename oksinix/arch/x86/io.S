/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit Port I/O & descriptor table helpers
 */

.section .text
.code32

.global inb
.type inb, @function
inb:
    movw 4(%esp), %dx
    xorl %eax, %eax
    inb %dx, %al
    ret

.global outb
.type outb, @function
outb:
    movw 4(%esp), %dx
    movb 8(%esp), %al
    outb %al, %dx
    ret

.global inw
.type inw, @function
inw:
    movw 4(%esp), %dx
    xorl %eax, %eax
    inw %dx, %ax
    ret

.global outw
.type outw, @function
outw:
    movw 4(%esp), %dx
    movw 8(%esp), %ax
    outw %ax, %dx
    ret

.global inl
.type inl, @function
inl:
    movw 4(%esp), %dx
    inl %dx, %eax
    ret

.global outl
.type outl, @function
outl:
    movw 4(%esp), %dx
    movl 8(%esp), %eax
    outl %eax, %dx
    ret

.global io_wait
.type io_wait, @function
io_wait:
    xorl %eax, %eax
    outb %al, $0x80
    ret

.global cpu_cli
.type cpu_cli, @function
cpu_cli:
    cli
    ret

.global cpu_sti
.type cpu_sti, @function
cpu_sti:
    sti
    ret

.global cpu_hlt
.type cpu_hlt, @function
cpu_hlt:
    hlt
    ret

.global cpu_pause
.type cpu_pause, @function
cpu_pause:
    pause
    ret

.global cpu_memory_barrier
.type cpu_memory_barrier, @function
cpu_memory_barrier:
    lock addl $0, 0(%esp)
    ret

.global cpu_eflags
.type cpu_eflags, @function
cpu_eflags:
    pushfl
    popl %eax
    ret

.global gdt_flush
.type gdt_flush, @function
gdt_flush:
    movl 4(%esp), %eax
    lgdt (%eax)
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    ljmp $0x08, $.Lgdt_flush_done
.Lgdt_flush_done:
    ret

.global idt_flush
.type idt_flush, @function
idt_flush:
    movl 4(%esp), %eax
    lidt (%eax)
    ret

.global linux_enter_elf
.type linux_enter_elf, @function
linux_enter_elf:
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl 20(%esp), %eax
    movl 24(%esp), %ecx
    movl 28(%esp), %edx
    testl %edx, %edx
    jz .Lno_save_esp
    movl %esp, (%edx)
.Lno_save_esp:
    movl %ecx, %esp
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %esi, %esi
    xorl %edi, %edi
    xorl %ebp, %ebp
    sti
    jmpl *%eax

.global linux_return_to_host
.type linux_return_to_host, @function
linux_return_to_host:
    movl 4(%esp), %ecx
    movl 8(%esp), %eax
    movl %ecx, %esp
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    sti
    ret

.global linux_resume_regs
.type linux_resume_regs, @function
linux_resume_regs:
    movl 4(%esp), %esp
    popl %eax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    popal
    addl $8, %esp
    iret

.global linux_restore_and_resume
.type linux_restore_and_resume, @function
linux_restore_and_resume:
    cli
    movl 4(%esp), %edi
    movl 8(%esp), %esi
    movl 12(%esp), %ecx
    movl 16(%esp), %esp
    cld
    rep movsl
    popl %eax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    popal
    addl $8, %esp
    iret

.global linux_call_on_kstack
.type linux_call_on_kstack, @function
linux_call_on_kstack:
    movl 4(%esp), %eax
    movl 8(%esp), %ecx
    andl $0xFFFFFFF0, %ecx
    movl %ecx, %esp
    call *%eax
1:  jmp 1b

.global linux_int80
.type linux_int80, @function
linux_int80:
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
