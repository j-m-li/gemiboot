/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit Port I/O, paging, TSS & Ring-3 transition helpers
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

.global cpu_yield
.type cpu_yield, @function
cpu_yield:
    int $0x81
    ret

.global cpu_enable_paging
.type cpu_enable_paging, @function
cpu_enable_paging:
    movl 4(%esp), %eax
    /* First disable CR0.PG in case UEFI (IA32/X64) left paging enabled */
    movl %cr0, %ecx
    andl $0x7FFFFFFF, %ecx
    movl %ecx, %cr0
    jmp .Lpaging_off
.Lpaging_off:
    /* Reset CR4 to CR4.PSE (bit 4) only: clears PAE, PGE, LA57, PCIDE, SMEP, SMAP from UEFI/OVMF */
    movl $0x00000010, %ecx
    movl %ecx, %cr4
    /* Load Page Directory physical address into CR3 */
    movl %eax, %cr3
    /* Enable CR0.PG (bit 31) and CR0.WP (bit 16) */
    movl %cr0, %ecx
    orl $0x80010000, %ecx
    movl %ecx, %cr0
    jmp .Lpaging_flush
.Lpaging_flush:
    ret

.global cpu_load_cr3
.type cpu_load_cr3, @function
cpu_load_cr3:
    movl 4(%esp), %eax
    movl %eax, %cr3
    ret

.global cpu_read_cr3
.type cpu_read_cr3, @function
cpu_read_cr3:
    movl %cr3, %eax
    ret

.global cpu_read_cr2
.type cpu_read_cr2, @function
cpu_read_cr2:
    movl %cr2, %eax
    ret

.global cpu_invlpg
.type cpu_invlpg, @function
cpu_invlpg:
    movl 4(%esp), %eax
    invlpg (%eax)
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

.global tss_flush
.type tss_flush, @function
tss_flush:
    movw 4(%esp), %ax
    ltr %ax
    ret

.global idt_flush
.type idt_flush, @function
idt_flush:
    movl 4(%esp), %eax
    lidt (%eax)
    ret

/*
 * int linux_enter_ring3(uint32_t entry, uint32_t user_esp, uint32_t *saved_esp_out)
 * Saves callee-saved registers on host kernel stack, switches data segments to USER_DS (0x23),
 * and executes iret into Ring 3 (USER_CS=0x1B, USER_DS=0x23, EFLAGS=0x202 with IF=1, IOPL=0).
 */
.global linux_enter_ring3
.type linux_enter_ring3, @function
linux_enter_ring3:
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl 20(%esp), %eax     /* entry */
    movl 24(%esp), %ecx     /* user_esp */
    movl 28(%esp), %edx     /* saved_esp_out */
    testl %edx, %edx
    jz .Lno_save_esp
    movl %esp, (%edx)
.Lno_save_esp:
    cli
    movw $0x23, %bx
    movw %bx, %ds
    movw %bx, %es
    movw %bx, %fs
    movw %bx, %gs
    pushl $0x23             /* user SS (USER_DS) */
    pushl %ecx              /* user ESP */
    pushl $0x00000202       /* EFLAGS: IF=1, reserved bit 1=1, IOPL=0 */
    pushl $0x1B             /* user CS (USER_CS) */
    pushl %eax              /* user EIP */
    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %esi, %esi
    xorl %edi, %edi
    xorl %ebp, %ebp
    iret

.global linux_return_to_host
.type linux_return_to_host, @function
linux_return_to_host:
    cli
    movl 4(%esp), %ecx
    movl 8(%esp), %eax
    movw $0x10, %dx
    movw %dx, %ds
    movw %dx, %es
    movw %dx, %fs
    movw %dx, %gs
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
    cli
    movl 4(%esp), %esp
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

.section .note.GNU-stack,"",@progbits
