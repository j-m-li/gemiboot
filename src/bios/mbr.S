# GEMIBOOT - x86 Boot Loader for GEMIOS
# Dedicated to the Public Domain (CC0)
#
# Master Boot Record (MBR) loaded by BIOS at 0x0000:7C00
# Relocates to 0x0000:0600, finds active partition, loads VBR into 0x7C00, and jumps to 7C00

.code16
.text
.org 0
.global _start

_start:
    jmp bypass
    nop
.org 0x03
oem_id:
    .ascii "GEMIBOOT"
.org 0x0B
    .word 512       # Bytes per sector
.org 0x0D
    .byte 4         # Sectors per cluster
.org 0x0E
    .word 2080      # Reserved sectors (PARTITION_START_LBA + 32)
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
    .long 0         # Hidden sectors
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
    .word 2049      # FSInfo sector
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
    .long 0x12345678 # Volume serial number
.org 0x47
    .ascii "GEMIBOOT   " # Volume label
.org 0x52
    .ascii "FAT32   "    # System identifier

.org 0x5A
bypass:
    # 1. Normalize CS:IP to 0x0000:0x7C00
    ljmp $0x0000, $norm_cs

norm_cs:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7C00, %sp
    sti

    # Save BIOS boot drive ID (DL)
    movb %dl, (0x7C00 + bios_drive_id - _start)

    # 2. Verify MBR signature at 0x7C00 + 510
    cmpw $0xAA55, (0x7C00 + 510)
    jne bad_sign

    # 3. Copy MBR from 0x0000:7C00 to 0x0000:0600
    movw $0x7C00, %si
    movw $0x0600, %di
    movw $512, %cx
    rep movsb

    # 4. Far jump to relocated code at 0x0000:0600
    ljmp $0x0000, $(0x0600 + after_reloc - _start)

after_reloc:
    # 5. Check LBA Support (INT 0x13, AH=0x41)
    movb (0x0600 + bios_drive_id - _start), %dl
    movb $0x41, %ah
    movw $0x55AA, %bx
    int $0x13
    jc no_lba
    cmpw $0xAA55, %bx
    jne no_lba
    testb $1, %cl
    jz no_lba

find_active_part:
    movw $(0x0600 + partition_table - _start), %si
    movw $4, %cx

find_loop:
    cmpb $0x80, (%si)
    je found_active
    addw $16, %si
    loop find_loop

    # Fallback to first partition if no active partition
    movw $(0x0600 + partition_table - _start), %si

found_active:
    movl 8(%si), %eax       # Partition start LBA
    movl %eax, (0x0600 + lba_packet_lba - _start)
    movl %eax, (0x0600 + saved_part_lba - _start)

    movw $3, %cx
try_read_vbr:
    pushw %cx
    movw $0, %ax
    movw %ax, %ds
    movb (0x0600 + bios_drive_id - _start), %dl   # Reload drive ID
    movw $(0x0600 + lba_packet - _start), %si    # Reload packet address
    movb $0x42, %ah
    int $0x13
    popw %cx
    jnc read_vbr_ok

    # Reset disk controller and retry
    movb (0x0600 + bios_drive_id - _start), %dl
    xorw %ax, %ax
    int $0x13
    loop try_read_vbr

cant_read_vbr:
    movb $'#', %al
    call putc
hang_mbr:
    hlt
    jmp hang_mbr

no_lba:
    movb $'@', %al
    call putc
    jmp hang_mbr

bad_sign:
    movb $'(', %al
    call putc
    jmp hang_mbr

read_vbr_ok:
    # Verify VBR signature 0xAA55 at 0x7DFE
    cmpw $0xAA55, 0x7DFE
    jne bad_sign

    # Pass partition start LBA in EBX and drive ID in DL/EDX to VBR
    movl (0x0600 + saved_part_lba - _start), %ebx
    movzbl (0x0600 + bios_drive_id - _start), %edx

    # Jump to VBR at 0x0000:7C00
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
saved_part_lba:
    .long 2048

.org 440
nt_disk_number:
    .long 0x544F4F42
    .word 0

.org 446
partition_table:
p_status:
    .byte 0x80                  # Active
p_start_chs:
    .byte 32, 33, 0             # Head 32, Sector 33, Cyl 0 (LBA 2048)
p_type:
    .byte 0x0C                  # FAT32 LBA
p_end_chs:
    .byte 254, 0xFF, 0xFF       # Head 254, Sector 63, Cyl 1023
p_start_lba:
    .long 2048                  # Default 1MB offset
p_length_lba:
    .long 0

three_other_volumes:
    .rept 48
    .byte 0
    .endr

.org 510
    .byte 0x55, 0xAA
