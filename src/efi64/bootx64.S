# GEMIBOOT - x86 Boot Loader for GEMIOS
# Dedicated to the Public Domain (CC0)
#
# UEFI x86_64 low-level entry and 64-to-32 bit trampoline mode switch

.code64
.text

.global jump_to_32bit_kernel
.global _jump_to_32bit_kernel
.global trampoline_32_start
.global trampoline_32_end
.global gdt_table
.global gdt_table_end
.global __chkstk
.global __chkstk_ms

__chkstk:
__chkstk_ms:
    ret

# void jump_to_32bit_kernel(uint64_t entry_point, uint64_t magic, uint64_t mbi_addr, uint64_t fb_base, uint64_t fb_pitch, uint64_t tramp_phys);
# Windows x64 ABI: RCX=entry_point, RDX=magic, R8=mbi_addr, R9=fb_base, 40(%rsp)=fb_pitch, 48(%rsp)=tramp_phys
_jump_to_32bit_kernel:
jump_to_32bit_kernel:
    cli

    # Save kernel arguments into non-volatile 32-bit registers
    movl %ecx, %esi         # ESI = kernel entry point
    movl %r8d, %ebx         # EBX = mbi_addr
    movq 48(%rsp), %r15     # R15 = tramp_phys (guaranteed < 4GB)

    # 1. Load GDT from descriptor at (tramp_phys + 0x800)
    subq $16, %rsp
    movw $(gdt_table_end - gdt_table - 1), (%rsp)
    leaq 0x800(%r15), %rax
    movq %rax, 2(%rsp)
    lgdt (%rsp)
    addq $16, %rsp

    # 2. Far return to 32-bit Compatibility Mode at tramp_phys (< 4GB)
    pushq $0x08                 # 32-bit Code Segment selector
    pushq %r15                  # Target RIP = tramp_phys (< 4GB)
    lretq

# -----------------------------------------------------------------------------
# 32-bit Relocatable Trampoline Code (Copied to tramp_phys < 4GB)
# -----------------------------------------------------------------------------
.code32
.align 16
trampoline_32_start:
    # 1. Disable Paging: clear CR0.PG (bit 31)
    movl %cr0, %eax
    btrl $31, %eax
    movl %eax, %cr0

    # 2. Pipeline flush
    jmp 1f
1:

    # 3. Disable Long Mode: clear IA32_EFER.LME (bit 8)
    movl $0xC0000080, %ecx
    rdmsr
    btrl $8, %eax
    wrmsr

    # 4. Reload 32-bit data segment selectors
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    # 5. Set 32-bit kernel stack
    movl $0x0007FFF0, %esp
    movl %esp, %ebp

    # 6. Load null IDT (limit=0, base=0) from stack
    pushl $0
    pushl $0
    lidtl (%esp)
    addl $8, %esp

    # 7. Enable Fast A20 Gate
    inb $0x92, %al
    testb $0x02, %al
    jnz 3f
    orb $0x02, %al
    andb $0xFE, %al
    outb %al, $0x92
3:

    # 8. Reset EFLAGS
    pushl $0
    popfl

    # 9. Set Multiboot Magic in EAX (0x2BADB002) and info ptr in EBX (0x00095000)
    movl $0x2BADB002, %eax
    movl $0x00095000, %ebx

    # 10. Jump to kernel entry point (in ESI)
    jmpl *%esi
trampoline_32_end:

# -----------------------------------------------------------------------------
# Global Descriptor Table (Read-Only)
# -----------------------------------------------------------------------------
.align 16
gdt_table:
    # 0x00: Null descriptor
    .quad 0
    # 0x08: 32-bit Code Segment (Base=0, Limit=4GB, Present, Ring 0, Code Exec/Read, 32-bit size, 4KB gran)
    .word 0xffff, 0x0000
    .byte 0x00, 0x9a, 0xcf, 0x00
    # 0x10: 32-bit Data Segment (Base=0, Limit=4GB, Present, Ring 0, Data Read/Write, 32-bit size, 4KB gran)
    .word 0xffff, 0x0000
    .byte 0x00, 0x92, 0xcf, 0x00
    # 0x18: 64-bit Code Segment
    .word 0xffff, 0x0000
    .byte 0x00, 0x9a, 0xaf, 0x00
    # 0x20: 64-bit Data Segment
    .word 0xffff, 0x0000
    .byte 0x00, 0x92, 0xaf, 0x00
gdt_table_end:

.code64

