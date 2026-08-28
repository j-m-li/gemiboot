# GEMIBOOT - x86 Boot Loader for GEMIOS
# Dedicated to the Public Domain (CC0)
#
# Volume Boot Record (VBR) loaded by MBR at 0x0000:7C00
# Relocates to 0x0000:0600, loads IO.SYS into 0x7C00, and jumps to 7C00

.code16
.text
.org 0
.global _start

_start:
    jmp bypass
    nop
.org 0x03
public_domain:
    .ascii "gemiboot"
.org 0x0B
    .word 512       # Bytes per sector
.org 0x0D
    .byte 4         # Sectors per cluster
.org 0x0E
    .word 32        # Reserved sectors
.org 0x10
    .byte 2         # Number of FATs
.org 0x11
    .word 0         # Root directory entries
.org 0x13
    .word 0         # Total sectors 16
.org 0x15
    .byte 0xF8      # Media descriptor
.org 0x16
    .word 0         # FAT size 16
.org 0x18
    .word 63        # Sectors per track
.org 0x1A
    .word 255       # Number of heads
.org 0x1C
    .long 2048      # Hidden sectors (partition start LBA)
.org 0x20
    .long 0         # Total sectors 32

.org 0x24
    .long 0         # FAT size 32
.org 0x28
    .word 0         # Flags
.org 0x2A
    .word 0x0000    # FAT version
.org 0x2C
    .long 2         # Root directory cluster
.org 0x30
    .word 1         # FSInfo sector
.org 0x32
    .word 6         # Backup boot sector
.org 0x34
    .long 0, 0, 0   # Reserved
.org 0x40
    .byte 0x80      # Drive number
.org 0x41
    .byte 0         # Reserved
.org 0x42
    .byte 0x29      # Extended boot signature
.org 0x43
    .long 0xDEADBEEF # Volume serial number
.org 0x47
    .ascii "GEMIBOOT   " # Volume label
.org 0x52
    .ascii "FAT32   "    # System identifier

.org 0x5A
bypass:
    # 1. Normalize CS:IP
    ljmp $0x0000, $norm_vbr_cs

norm_vbr_cs:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7C00, %sp
    sti

    # Save drive ID (DL/EDX) and partition start LBA (EBX)
    movb %dl, (0x7C00 + bios_drive_id - _start)
    movl %ebx, (0x7C00 + partition_lba - _start)

    # 2. Relocate VBR from 0x0000:7C00 to 0x0000:0600
    movw $0x7C00, %si
    movw $0x0600, %di
    movw $512, %cx
    rep movsb

    # 3. Jump to relocated VBR
    ljmp $0x0000, $(0x0600 + after_vbr_reloc - _start)

after_vbr_reloc:
    # IO.SYS is placed in reserved sectors at PARTITION_START_LBA + 2
    movl (0x0600 + partition_lba - _start), %ebx
    addl $2, %ebx
    movl %ebx, (0x0600 + iosys_lba - _start)

read_io_sys_loop:
    movb $0x10, (0x0600 + lba_packet_size - _start)
    movb $0x0, (0x0600 + lba_packet_reserved - _start)
    movw $1, (0x0600 + lba_packet_sectors - _start)
    movw $0x7C00, (0x0600 + lba_packet_offset - _start)
    movw $0x0000, (0x0600 + lba_packet_segment - _start)
    movl (0x0600 + iosys_lba - _start), %eax
    movl %eax, (0x0600 + lba_packet_lba - _start)
    movl $0, (0x0600 + lba_packet_padding - _start)

    movw $3, %cx
try_read_iosys:
    pushw %cx
    movw $0, %ax
    movw %ax, %ds
    movb (0x0600 + bios_drive_id - _start), %dl   # Reload drive ID
    movw $(0x0600 + lba_packet - _start), %si    # Reload packet ptr
    movb $0x42, %ah
    int $0x13
    popw %cx
    jnc iosys_sec_read_ok

    # Reset controller and retry
    movb (0x0600 + bios_drive_id - _start), %dl
    xorw %ax, %ax
    int $0x13
    loop try_read_iosys

cant_read_iosys:
    movb $'#', %al
    call putc
hang_vbr:
    hlt
    jmp hang_vbr

iosys_sec_read_ok:
    # Verify boot signature 0xAA55 at 0x7DFE
    cmpw $0xAA55, 0x7DFE
    jne check_next_sec

    # Verify "gemi" identifier at 0x7C03
    cmpl $0x696D6567, 0x7C03
    je iosys_found

check_next_sec:
    incl (0x0600 + iosys_lba - _start)
    jmp read_io_sys_loop

iosys_found:
    # Pass IO.SYS start LBA in EBX and drive ID in DL/EDX to IO.SYS
    movl (0x0600 + iosys_lba - _start), %ebx
    movzbl (0x0600 + bios_drive_id - _start), %edx

    # Jump to IO.SYS at 0x0000:7C00
    ljmp $0x0000, $0x7C00

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
    .word 0x0000
lba_packet_lba:
    .long 0
lba_packet_padding:
    .long 0

bios_drive_id:
    .byte 0x80
partition_lba:
    .long 2048
iosys_lba:
    .long 2050

.org 510
    .byte 0x55, 0xAA
