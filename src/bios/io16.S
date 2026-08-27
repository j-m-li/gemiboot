# GEMIBOOT - x86 Boot Loader for GEMIOS
# Dedicated to the Public Domain (CC0)
#
# First 512 bytes are loaded by vbr.S at 0x0000:7C00
# - Loads the rest of IO.SYS at 0x0001:0000 (0x00010000)
# - Gathers E820 memory map from BIOS
# - Configures VESA VBE / video mode
# - Enables A20 gate
# - Switches to 32-bit protected mode
# - Jumps to protected32 in io32.S / bios_loader.c

#include "defs.h"

.set STROFF, -512

.code16
.text
.global _start
_start:
    jmp bypass
    nop
public_domain:
    .ascii "gemiboot"

bypass:
    movw $0x7C00, %sp
    xorw %ax, %ax
    movw %ax, %ss
    pushl %ebx          # LBA of IO.SYS passed by VBR
    pushl %edx          # BIOS disk drive ID (0x80)
    movw %ax, %es
    movw %ax, %ds

    # Print 'I'
    movb $0x0E, %ah
    movb $'I', %al
    int $0x10

    ljmp $0, $updatecs - _start + 0x7C00

updatecs:
    # Print 'O'
    movb $0x0E, %ah
    movb $'O', %al
    int $0x10

boot_load:
    movb $'.', %al
    call putc
    movb $'S', %al
    call putc
    movb $'Y', %al
    call putc
    movb $'S', %al
    call putc
    movb $' ', %al
    call putc

    popl %edx           # BIOS drive ID
    popl %ebx           # LBA of IO.SYS

    addl $1, %ebx       # Next sector of IO.SYS
    movl %ebx, (iosys_sector)
    movl $12, (iosys_count) # Load up to 384 KB (12 chunks of 64 sectors)

self_load:
    movb $0x10, (lba_packet_size)
    movb $0x0, (lba_packet_reserved)
    movw $64, (lba_packet_sectors)
    movw $0, (lba_packet_offset)
    movw $0x1000, (lba_packet_segment)
    movl %ebx, (lba_packet_lba)
    movl $0x0, (lba_packet_padding)

    movw $3, %cx
try_read:
    movw $0, %ax
    movw %ax, %ds
    movb $0x42, %ah     # Extended read sectors
    movw $lba_packet, %si
    int $0x13
    jnc read_done

    movb $0, %ah        # Reset disk controller
    int $0x13
    loop try_read

cant_read:
    movb $'#', %al
    call putc
halt_real:
    hlt
    jmp halt_real

read_done:
    movb $'*', %al
    call putc

    movw (lba_packet_segment), %bx
    addw $(512 / 16 * 64), %bx
    movw %bx, (lba_packet_segment)

    movl (iosys_sector), %ebx
    addl $64, %ebx
    movl %ebx, (iosys_sector)
    movl %ebx, (lba_packet_lba)

    movl (iosys_count), %ebx
    subl $1, %ebx
    movl %ebx, (iosys_count)
    cmpl $0, %ebx
    je load_finished

    movb $0x10, (lba_packet_size)
    movb $0x0, (lba_packet_reserved)
    movw $0, (lba_packet_offset)
    movw $64, (lba_packet_sectors)
    movl $0x0, (lba_packet_padding)
    movw $3, %cx
    jmp try_read

load_finished:
    movb $0x0D, %al
    call putc
    movb $0x0A, %al
    call putc

    # Jump to second stage at 0x1000:0000
    ljmpw $0x1000, $0

putc:
    movb $0x0E, %ah
    int $0x10
    ret

.p2align 4, 0
lba_packet:
lba_packet_size:
    .byte 0x10
lba_packet_reserved:
    .byte 0x0
lba_packet_sectors:
    .word 0x0001
lba_packet_offset:
    .word 0x7C00
lba_packet_segment:
    .word 0x0
lba_packet_lba:
    .long 0
lba_packet_padding:
    .long 0
iosys_sector:
    .long 0
iosys_count:
    .long 0

signature:
.org _start + 510
    .byte 0x55, 0xAA

# Second stage begins at 0x1000:0000 (0x00010000)
.code16
.text
.globl second_stage
second_stage:
    movw %cs, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    # Print banner
    movw $stage2_str + STROFF - _start, %si
    call print_str

    # 1. Detect E820 Memory Map
    call get_e820_memory_map

    # 2. Setup VESA VBE 640x480 video mode
    call init_video_mode

    # 3. Enable A20 Line
    call enable_a20

    # 4. Load GDT and IDT
    cli
    lgdtl gdtp + STROFF - _start
    lidtl idt + STROFF - _start

    # 5. Enable Protected Mode (CR0.PE = 1)
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0

    # 6. Jump to 32-bit protected mode
    ljmpl $0x08, $protected32

print_str:
    movb $0x0E, %ah
print_loop:
    lodsb
    cmpb $0, %al
    je print_ret
    int $0x10
    jmp print_loop
print_ret:
    ret

# Query BIOS E820 Memory Map and store at physical 0x00008000
get_e820_memory_map:
    push %es
    xorw %ax, %ax
    movw %ax, %es
    movw $0x8004, %di       # Destination buffer for entries
    xorl %ebx, %ebx         # Continuation value = 0
    movl $0x534D4150, %edx  # 'SMAP'
    xorl %ebp, %ebp         # Entry count

e820_query_loop:
    movl $20, %es:(%di)     # Size field = 20
    addw $4, %di
    movl $1, %es:20(%di)    # Default type = 1
    movl $0xE820, %eax
    movl $20, %ecx
    int $0x15
    jc e820_query_done
    cmpl $0x534D4150, %eax
    jne e820_query_done

    incl %ebp
    addw $20, %di

    testl %ebx, %ebx
    jz e820_query_done
    jmp e820_query_loop

e820_query_done:
    movl %ebp, %es:0x8000   # Store entry count at 0x8000
    pop %es
    ret

# VESA VBE 640x480 initialization
init_video_mode:
    push %es
    movw $0x1000, %ax
    movw %ax, %es

    # Try VBE mode 0x4101 (640x480 256 colors with linear framebuffer)
    movw $0x4F01, %ax
    movw $0x4101, %cx
    movw $vbe_mode_info + STROFF - _start, %di
    int $0x10
    cmpw $0x004F, %ax
    jne vbe_fallback

    # Set mode 0x4101
    movw $0x4F02, %ax
    movw $0x4101, %bx
    int $0x10
    cmpw $0x004F, %ax
    jne vbe_fallback

    # Set 256-color palette
    movw $0x1012, %ax
    xorw %bx, %bx
    movw $32, %cx
    movw $palette + STROFF - _start, %dx
    int $0x10

    # Store GFX info at physical address 0x00009000
    xorw %ax, %ax
    movw %ax, %es
    movl $4, %es:0x9000                         # Mode: 4 (256 colors)
    movzwl vbe_mode_info + STROFF - _start + 18, %eax
    movl %eax, %es:0x9004                       # Width
    movzwl vbe_mode_info + STROFF - _start + 20, %eax
    movl %eax, %es:0x9008                       # Height
    movzwl vbe_mode_info + STROFF - _start + 16, %eax
    movl %eax, %es:0x900C                       # Pitch
    movl vbe_mode_info + STROFF - _start + 40, %eax
    movl %eax, %es:0x9010                       # Linear Framebuffer Base

    pop %es
    ret

vbe_fallback:
    # Fallback to VGA 80x25 text mode (0x03)
    movw $0x0003, %ax
    int $0x10
    xorw %ax, %ax
    movw %ax, %es
    movl $2, %es:0x9000                         # Mode: 2 (Text)
    movl $80, %es:0x9004                        # Width
    movl $25, %es:0x9008                        # Height
    movl $160, %es:0x900C                       # Pitch
    movl $0xB8000, %es:0x9010                   # Base: 0xB8000
    pop %es
    ret

enable_a20:
    # 1. Try Fast A20 (Port 0x92)
    inb $0x92, %al
    testb $0x02, %al
    jnz a20_ok
    orb $0x02, %al
    andb $0xFE, %al
    outb %al, $0x92

a20_ok:
    ret

stage2_str:
    .asciz "[GEMIBOOT] Stage 2 starting...\r\n"

# 32-bit GDT definition
.align 16
gdtp:
    .word gdt_end - gdt - 1
    .long gdt - _start + START_ADDRESS - 512

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

palette:
    .byte 0x00, 0x00, 0x00 # black
    .byte 0x3F, 0x00, 0x00 # red
    .byte 0x00, 0x3F, 0x00 # green
    .byte 0x3F, 0x3F, 0x00
    .byte 0x00, 0x00, 0x3F # blue
    .byte 0x3F, 0x00, 0x3F
    .byte 0x00, 0x3F, 0x3F
    .byte 0x3F, 0x3F, 0x3F # white
    .byte 7, 7, 7
    .byte 14, 14, 14
    .byte 21, 21, 21
    .byte 28, 28, 28
    .byte 35, 35, 35
    .byte 42, 42, 42
    .byte 49, 49, 49
    .byte 56, 56, 56

vbe_mode_info:
    .rept 256
    .byte 0
    .endr

.org _start + SIZE_16BIT_CODE - 1 + 512
    nop
