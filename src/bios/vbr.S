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
    .byte 1         # Sectors per cluster
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
    # Print 'V'
    movb $0x0E, %ah
    movb $'V', %al
    int $0x10

    movw $0x7C00, %sp
    xorw %ax, %ax
    movw %ax, %ss
    pushl %ebx          # Partition start LBA passed by MBR
    pushl %edx          # Drive ID passed by MBR
    movw %ax, %es
    movw %ax, %ds

move_vbr:
    cli
    movw $0x7C00, %si
    movw $0x0600, %di
    movw $512, %cx
    cld
    rep movsb

    sti
    movb $0x0E, %ah
    movb $'B', %al
    int $0x10

    movw $0, (0x7C00 + 510)
    movw $0, (0x0600 + 510)

    ljmp $0, $(0x0600 + after_move - _start)

after_move:
    movb $0x0E, %ah
    movb $'R', %al
    int $0x10
    movb $' ', %al
    call putc

    popl %edx           # Drive ID
    popl %ebx           # Partition start LBA

    # IO.SYS is placed in reserved sectors at PARTITION_START_LBA + 2
    addl $2, %ebx

read_io_sys:
    movw $0, (0x7C00 + 510)

    movb $0x10, (0x0600 + lba_packet_size - _start)
    movb $0x0, (0x0600 + lba_packet_reserved - _start)
    movw $1, (0x0600 + lba_packet_sectors - _start)
    movw $0x7C00, (0x0600 + lba_packet_offset - _start)
    movw $0x0, (0x0600 + lba_packet_segment - _start)
    movl %ebx, (0x0600 + lba_packet_lba - _start)
    movl $0x0, (0x0600 + lba_packet_padding - _start)

    movw $3, %cx
try_read_vbr:
    movw $0, %ax
    movw %ax, %ds
    movb $0x42, %ah
    movw $(0x0600 + lba_packet - _start), %si
    int $0x13
    jnc vbr_read_done

    movb $0, %ah
    int $0x13
    loop try_read_vbr

cant_read_io:
    movb $'#', %al
    call putc
io_err:
    hlt
    jmp io_err

vbr_read_done:
    pushl %ebx
    pushl %edx
    movw (0x7C00 + 510), %ax
    cmpw $0xAA55, %ax
    jne try_next_sec

    # Verify "gemi" identifier at offset 3
    cmpl $0x696D6567, 0x7C03
    je io_found

try_next_sec:
    popl %edx
    popl %ebx
    addl $1, %ebx
    jmp read_io_sys

io_found:
    popl %edx
    popl %ebx

    movb $'L', %al
    call putc
    movb $'o', %al
    call putc
    movb $'a', %al
    call putc
    movb $'d', %al
    call putc
    movb $0x0D, %al
    call putc
    movb $0x0A, %al
    call putc

    # Jump to IO.SYS loaded at 0x7C00 with EBX=IO.SYS start LBA, EDX=Drive ID
    ljmp $0x0000, $0x7C00

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

.org 510
    .byte 0x55, 0xAA
