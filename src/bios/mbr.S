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
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw (0x7C00 + 510), %ax     # Verify signature
    cmpw $0xAA55, %ax
    jz sign_ok

error_sign:
    sti
    movb $0x0E, %ah
    movb $'(', %al
    int $0x10
bad_sign:
    hlt
    jmp bad_sign

sign_ok:
    movw $0, (0x7C00 + 510)     # Clear signature
    sti
    movb $0x0E, %ah
    movb $'M', %al
    int $0x10

    movw $0x7C00, %sp
    xorw %ax, %ax
    movw %ax, %ss
    pushw %dx                   # Save BIOS drive ID
    movw %ax, %es
    movw %ax, %ds

move_mbr:
    cli
    movw $0x7C00, %si           # Source
    movw $0x0600, %di           # Destination
    movw $512, %cx
    cld
    rep movsb

    sti
    movb $'B', %al
    call putc

    ljmp $0, $(0x0600 + after_move - _start)

after_move:
    movb $'R', %al
    call putc

    popw %dx                    # Restore BIOS drive ID
check_lba:
    movb $0x41, %ah             # Check LBA support
    movw $0x55AA, %bx
    int $0x13
    cmpw $0xAA55, %bx
    jz find_active_part

no_lba:
    movb $'@', %al
    call putc
lba_error:
    hlt
    jmp lba_error

find_active_part:
    movw $(0x0600 + partition_table - _start), %si
    movw $4, %cx

find_loop:
    cmpb $0x80, (%si)           # Active partition flag
    je found_active
    addw $16, %si
    loop find_loop

    # If no active partition, use first partition
    movw $(0x0600 + partition_table - _start), %si

found_active:
    movl 8(%si), %eax           # Read p_start_lba
    movl %eax, (0x0600 + lba_packet_lba - _start)
    pushl %eax                  # Save partition start LBA for VBR
    movw $3, %cx

try_read:
    movb $0x42, %ah             # Extended read
    movw $(0x0600 + lba_packet - _start), %si
    int $0x13
    jnc read_done

reset_disk:
    movb $0, %ah
    int $0x13
    loop try_read

cant_read_vbr:
    movb $'#', %al
    call putc
vbr_error:
    hlt
    jmp vbr_error

read_done:
    popl %ebx                   # Pass partition start LBA in EBX to VBR
    movb $' ', %al
    call putc

    ljmp $0x0000, $0x7C00       # Jump to VBR at 0x7C00

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

.org 440
nt_disk_number:
    .long 0x0
    .word 0

.org 446
partition_table:
p_status:
    .byte 0x80                  # Active
p_start_chs:
    .byte 0, 0, 0
p_type:
    .byte 0x0C                  # FAT32 LBA
p_end_chs:
    .byte 0, 0, 0
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
