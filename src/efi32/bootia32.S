# GEMIBOOT - x86 Boot Loader for GEMIOS
# Dedicated to the Public Domain (CC0)
#
# UEFI IA32 low-level entry and GDT switch

.code32
.text

.global _load_gdt_and_jump
.global load_gdt_and_jump
# __chkstk / __chkstk_ms for 32-bit x86 Windows ABI:
# Input: %eax = size of stack frame to allocate
# Output: %esp adjusted, stack probed
.global _chkstk
.global __chkstk
.global __chkstk_ms

_chkstk:
__chkstk:
__chkstk_ms:
    pushl   %ecx
    leal    8(%esp), %ecx       # Address above return address + pushed ecx
1:
    cmpl    $4096, %eax
    jb      2f
    subl    $4096, %ecx
    testl   %eax, (%ecx)        # Probe page
    subl    $4096, %eax
    jmp     1b
2:
    subl    %eax, %ecx
    testl   %eax, (%ecx)        # Probe final page
    movl    %esp, %eax          # Pointer to old stack frame
    movl    %ecx, %esp          # Set new adjusted ESP
    movl    0(%eax), %ecx       # Restore ECX
    pushl   4(%eax)             # Put return address onto new stack
    ret

# void load_gdt_and_jump(uint32_t entry_point, uint32_t magic, uint32_t mbi_addr);
_load_gdt_and_jump:
load_gdt_and_jump:
    cli

    # Read arguments from current stack
    movl 4(%esp), %edx   # entry_point
    movl 8(%esp), %eax   # magic (0x2BADB002)
    movl 12(%esp), %ebx  # mbi_addr

    # Load 32-bit GDT and null IDT
    lgdtl gdtp
    lidtl idt

    # Reload data segments
    movw $0x10, %cx
    movw %cx, %ds
    movw %cx, %es
    movw %cx, %fs
    movw %cx, %gs
    movw %cx, %ss

    # Set new kernel stack
    movl $0x0007FFF0, %esp
    movl %esp, %ebp

    # Far jump to 32-bit code segment 0x08
    ljmpl $0x08, $1f

1:
    # Enable Fast A20 Gate
    inb $0x92, %al
    testb $0x02, %al
    jnz 2f
    orb $0x02, %al
    andb $0xFE, %al
    outb %al, $0x92
2:

    # Restore magic into EAX
    movl $0x2BADB002, %eax

    # Jump to kernel entry point with EAX=magic, EBX=mbi_addr
    jmpl *%edx

.align 16
gdtp:
    .word gdt_end - gdt - 1
    .long gdt

.align 16
gdt:
gdt_null:
    .quad 0
gdt_code_segment:
    .word 0xffff
    .word 0x0000
    .byte 0x00
    .byte 0b10011010
    .byte 0b11001111
    .byte 0x00
gdt_data_segment:
    .word 0xffff
    .word 0x0000
    .byte 0x00
    .byte 0b10010010
    .byte 0b11001111
    .byte 0x00
gdt_end:

idt:
    .word 0
    .long 0

.global @feat.00
.set @feat.00, 1
