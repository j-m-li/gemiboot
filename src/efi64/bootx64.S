# GEMIBOOT - x86 Boot Loader for GEMIOS
# Dedicated to the Public Domain (CC0)
#
# UEFI x86_64 low-level entry and 64-to-32 bit mode switch

.code64
.text

.global jump_to_32bit_kernel
.global _jump_to_32bit_kernel
.global __chkstk
.global __chkstk_ms

__chkstk:
__chkstk_ms:
    ret

# void jump_to_32bit_kernel(uint64_t entry_point, uint64_t magic, uint64_t mbi_addr);
# Windows x64 calling convention: RCX=entry_point, RDX=magic, R8=mbi_addr
_jump_to_32bit_kernel:
jump_to_32bit_kernel:
    cli

    # Save arguments into 32-bit registers
    movl %ecx, %esi     # ESI = entry_point
    movl %r8d, %ebx     # EBX = mbi_addr

    # Load GDT with 32-bit and 64-bit descriptors
    lgdtq gdtp(%rip)

    # Perform 64-bit to 32-bit far return into Code Segment 0x08
    pushq $0x08
    leaq compat32(%rip), %rax
    pushq %rax
    lretq

.code32
compat32:
    # Now running in 32-bit Compatibility Mode

    # 1. Disable Paging: clear CR0.PG (bit 31)
    movl %cr0, %eax
    btrl $31, %eax
    movl %eax, %cr0

    # 2. Disable Long Mode: clear EFER.LME (bit 8)
    movl $0xC0000080, %ecx
    rdmsr
    btrl $8, %eax
    wrmsr

    # 3. Disable PAE: clear CR4.PAE (bit 5)
    movl %cr4, %eax
    btrl $5, %eax
    movl %eax, %cr4

    # 4. Reload flat 32-bit data segments
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    # 5. Set 32-bit kernel stack
    movl $0x0007FFF0, %esp
    movl %esp, %ebp

    # 6. Enable Fast A20 Gate
    inb $0x92, %al
    testb $0x02, %al
    jnz 1f
    orb $0x02, %al
    andb $0xFE, %al
    outb %al, $0x92
1:

    # 7. Set Multiboot Magic in EAX (0x2BADB002)
    movl $0x2BADB002, %eax

    # 8. Jump to kernel entry point in pure 32-bit protected mode
    jmpl *%esi

.code64
.align 16
gdtp:
    .word gdt_end - gdt - 1
    .quad gdt

.align 16
gdt:
gdt_null:
    .quad 0
gdt_code32:
    .word 0xffff
    .word 0x0000
    .byte 0x00
    .byte 0b10011010        # Present, Ring 0, Code, Exec/Read
    .byte 0b11001111        # 4KB gran, 32-bit op size, L=0
    .byte 0x00
gdt_data32:
    .word 0xffff
    .word 0x0000
    .byte 0x00
    .byte 0b10010010        # Present, Ring 0, Data, Read/Write
    .byte 0b11001111        # 4KB gran, 32-bit op size
    .byte 0x00
gdt_code64:
    .word 0xffff
    .word 0x0000
    .byte 0x00
    .byte 0b10011010
    .byte 0b10101111        # 64-bit Code (L=1, D=0)
    .byte 0x00
gdt_data64:
    .word 0xffff
    .word 0x0000
    .byte 0x00
    .byte 0b10010010
    .byte 0b10101111        # 64-bit Data
    .byte 0x00
gdt_end:
