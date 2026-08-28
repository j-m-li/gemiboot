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
