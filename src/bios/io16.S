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
    ljmp $0x0000, $(norm_io16_cs - _start + 0x7C00)
norm_io16_cs:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7C00, %sp
    sti

    # Save BIOS drive ID and LBA of IO.SYS
    movb %dl, (bios_drive_id_stage1 - _start + 0x7C00)
    movl %ebx, (iosys_sector - _start + 0x7C00)

    addl $1, (iosys_sector - _start + 0x7C00)
    movl $12, (iosys_count - _start + 0x7C00) # Load up to 384 KB (12 chunks of 64 sectors)

self_load:
    movb $0x10, (lba_packet_size - _start + 0x7C00)
    movb $0x0, (lba_packet_reserved - _start + 0x7C00)
    movw $64, (lba_packet_sectors - _start + 0x7C00)
    movw $0, (lba_packet_offset - _start + 0x7C00)
    movw $0x1000, (lba_packet_segment - _start + 0x7C00)
    movl (iosys_sector - _start + 0x7C00), %eax
    movl %eax, (lba_packet_lba - _start + 0x7C00)
    movl $0x0, (lba_packet_padding - _start + 0x7C00)

    movw $3, %cx
try_read:
    pushw %cx
    movw $0, %ax
    movw %ax, %ds
    movb (bios_drive_id_stage1 - _start + 0x7C00), %dl   # Reload drive ID
    movw $(lba_packet - _start + 0x7C00), %si           # Reload packet address
    movb $0x42, %ah     # Extended read sectors
    int $0x13
    popw %cx
    jnc read_done

    movb (bios_drive_id_stage1 - _start + 0x7C00), %dl
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
    movw (lba_packet_segment - _start + 0x7C00), %bx
    addw $(512 / 16 * 64), %bx
    movw %bx, (lba_packet_segment - _start + 0x7C00)

    addl $64, (iosys_sector - _start + 0x7C00)
    subl $1, (iosys_count - _start + 0x7C00)
    cmpl $0, (iosys_count - _start + 0x7C00)
    je load_finished

    movw $3, %cx
    jmp try_read

load_finished:
    movb (bios_drive_id_stage1 - _start + 0x7C00), %dl
    # Jump to second stage at 0x1000:0000 with DL = drive ID
    ljmpw $0x1000, $0

putc:
    pusha
    movb $0x0E, %ah
    xorw %bx, %bx
    int $0x10
    popa
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
bios_drive_id_stage1:
    .byte 0x80

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

    # Store BIOS drive ID passed in DL
    movb %dl, (bios_drive_id + STROFF - _start)
    cmpb $0x80, %dl
    jae 1f
    movb $0x80, (bios_drive_id + STROFF - _start)
1:

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

    # Try VBE mode 0x4112 (640x480 24/32-bit RGB with linear framebuffer)
    movw $0x4F01, %ax
    movw $0x4112, %cx
    movw $vbe_mode_info + STROFF - _start, %di
    int $0x10
    cmpw $0x004F, %ax
    jne try_16bpp

    movw $0x4F02, %ax
    movw $0x4112, %bx
    int $0x10
    cmpw $0x004F, %ax
    je vbe_mode_set_ok

try_16bpp:
    # Try VBE mode 0x4111 (640x480 16-bit 5:6:5 with linear framebuffer)
    movw $0x4F01, %ax
    movw $0x4111, %cx
    movw $vbe_mode_info + STROFF - _start, %di
    int $0x10
    cmpw $0x004F, %ax
    jne try_256color

    movw $0x4F02, %ax
    movw $0x4111, %bx
    int $0x10
    cmpw $0x004F, %ax
    je vbe_mode_set_ok

try_256color:
    # Try VBE mode 0x4101 (640x480 8-bit 256 colors with linear framebuffer)
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

vbe_mode_set_ok:
    # Store GFX info at physical address 0x00009000
    xorw %ax, %ax
    movw %ax, %es
    movzbl vbe_mode_info + STROFF - _start + 25, %eax # BitsPerPixel (offset 25)
    movl %eax, %es:0x9000
    movzwl vbe_mode_info + STROFF - _start + 18, %eax # Width (offset 18)
    movl %eax, %es:0x9004
    movzwl vbe_mode_info + STROFF - _start + 20, %eax # Height (offset 20)
    movl %eax, %es:0x9008
    movzwl vbe_mode_info + STROFF - _start + 16, %eax # Pitch (offset 16)
    movl %eax, %es:0x900C
    movl vbe_mode_info + STROFF - _start + 40, %eax   # PhysBasePtr (offset 40)
    movl %eax, %es:0x9010

    # Color field positions and mask sizes
    movb vbe_mode_info + STROFF - _start + 32, %al    # RedFieldPosition
    movb %al, %es:0x9014
    movb vbe_mode_info + STROFF - _start + 31, %al    # RedMaskSize
    movb %al, %es:0x9015
    movb vbe_mode_info + STROFF - _start + 34, %al    # GreenFieldPosition
    movb %al, %es:0x9016
    movb vbe_mode_info + STROFF - _start + 33, %al    # GreenMaskSize
    movb %al, %es:0x9017
    movb vbe_mode_info + STROFF - _start + 36, %al    # BlueFieldPosition
    movb %al, %es:0x9018
    movb vbe_mode_info + STROFF - _start + 35, %al    # BlueMaskSize
    movb %al, %es:0x9019

    pop %es
    ret

vbe_fallback:
    # Fallback to VGA 80x25 text mode (0x03)
    movw $0x0003, %ax
    int $0x10
    xorw %ax, %ax
    movw %ax, %es
    movl $0, %es:0x9000                         # Mode: 0 (Text)
    movl $80, %es:0x9004                        # Width
    movl $25, %es:0x9008                        # Height
    movl $160, %es:0x900C                       # Pitch
    movl $0xB8000, %es:0x9010                   # Base: 0xB8000
    pop %es
    ret

enable_a20:
    # Check if A20 is already enabled
    call check_a20
    testw %ax, %ax
    jnz a20_ok

    # Method 1: BIOS INT 0x15, AX=0x2401
    movw $0x2401, %ax
    int $0x15
    call check_a20
    testw %ax, %ax
    jnz a20_ok

    # Method 2: Fast A20 Gate (Port 0x92)
    inb $0x92, %al
    testb $0x02, %al
    jnz 1f
    orb $0x02, %al
    andb $0xFE, %al
    outb %al, $0x92
1:
    call check_a20
    testw %ax, %ax
    jnz a20_ok

    # Method 3: Keyboard Controller A20 (Port 0x64/0x60)
    cli
    call a20_wait_kbc_in
    movb $0xAD, %al
    outb %al, $0x64
    call a20_wait_kbc_in
    movb $0xD0, %al
    outb %al, $0x64
    call a20_wait_kbc_out
    inb $0x60, %al
    pushl %eax
    call a20_wait_kbc_in
    movb $0xD1, %al
    outb %al, $0x64
    call a20_wait_kbc_in
    popl %eax
    orb $2, %al
    outb %al, $0x60
    call a20_wait_kbc_in
    movb $0xAE, %al
    outb %al, $0x64
    sti

a20_ok:
    ret

a20_wait_kbc_in:
    inb $0x64, %al
    testb $2, %al
    jnz a20_wait_kbc_in
    ret

a20_wait_kbc_out:
    inb $0x64, %al
    testb $1, %al
    jz a20_wait_kbc_out
    ret

check_a20:
    pushw %ds
    pushw %es
    xorw %ax, %ax
    movw %ax, %ds
    decw %ax
    movw %ax, %es
    movw $0x7DFE, %si
    movw $0x7E0E, %di
    movw %ds:(%si), %cx
    movw %es:(%di), %dx
    movw $0x0000, %ds:(%si)
    movw $0xFFFF, %es:(%di)
    cmpw $0xFFFF, %ds:(%si)
    movw %cx, %ds:(%si)
    movw %dx, %es:(%di)
    movw $1, %ax
    jne 2f
    xorw %ax, %ax
2:
    popw %es
    popw %ds
    ret

stage2_str:
    .asciz "[GEMIBOOT] Stage 2 starting...\r\n"

# 32-bit and 16-bit GDT definition
.align 16
.globl gdtp
gdtp:
    .word gdt_end - gdt - 1
    .long gdt - _start + START_ADDRESS - 512

.align 16
.globl gdt
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
gdt_code16_segment:
    .word 0xffff
    .word 0x0000
    .byte 0x01
    .byte 0b10011010
    .byte 0b00000000
    .byte 0x00
gdt_data16_segment:
    .word 0xffff
    .word 0x0000
    .byte 0x01
    .byte 0b10010010
    .byte 0b00000000
    .byte 0x00
gdt_end:

.globl idt
idt:
    .word 0
    .long 0

.globl rm_idtr
rm_idtr:
    .word 0x03FF
    .long 0x00000000

.globl dap_packet
.globl dap_size
.globl dap_reserved
.globl dap_sectors
.globl dap_offset
.globl dap_segment
.globl dap_lba
.globl bios_drive_id
.globl saved_esp32

.p2align 4, 0
dap_packet:
dap_size:     .byte 0x10
dap_reserved: .byte 0x00
dap_sectors:  .word 0x0001
dap_offset:   .word 0x0000
dap_segment:  .word 0x2000
dap_lba:      .quad 0

bios_drive_id: .byte 0x80
saved_esp32:   .long 0

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

# BIOS INT 0x13 Real Mode Thunk
.code16
.globl bios_int13_thunk16
.globl bios_int13_thunk16_offset
bios_int13_thunk16:
    # Disable Protected Mode (CR0.PE = 0)
    movl %cr0, %eax
    andl $~1, %eax
    movl %eax, %cr0

    # Far jump to real mode (CS=0x1000)
    ljmpw $0x1000, $(thunk16_real + STROFF - _start)

bios_int13_thunk16_offset = bios_int13_thunk16 + STROFF - _start

thunk16_real:
    # Reload real mode segment registers
    movw %cs, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    # Setup low memory stack
    xorw %ax, %ax
    movw %ax, %ss
    movw $0x7C00, %sp

    # Load Real Mode IVT
    lidtl (rm_idtr + STROFF - _start)

    # Perform Extended Read (AH=0x42)
    movb (bios_drive_id + STROFF - _start), %dl
    cmpb $0x80, %dl
    jae 1f
    movb $0x80, %dl
1:
    movw $(dap_packet + STROFF - _start), %si
    movw $3, %cx

    sti
try_thunk_int13:
    pushw %cx
    movb (bios_drive_id + STROFF - _start), %dl
    movw $(dap_packet + STROFF - _start), %si
    movb $0x42, %ah
    int $0x13
    popw %cx
    jnc thunk_int13_ok

    # Reset drive controller and retry
    movb (bios_drive_id + STROFF - _start), %dl
    xorw %ax, %ax
    int $0x13
    loop try_thunk_int13

    # Error
    movl $1, %ebx
    jmp thunk_return_pm32

thunk_int13_ok:
    xorl %ebx, %ebx

thunk_return_pm32:
    cli
    # Reload 32-bit GDT & IDT
    lgdtl (gdtp + STROFF - _start)
    lidtl (idt + STROFF - _start)

    # Enable Protected Mode (CR0.PE = 1)
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0

    # Far jump back to 32-bit protected mode
    ljmpl $0x08, $thunk_pm32_return

.org _start + SIZE_16BIT_CODE - 1 + 512
    nop
