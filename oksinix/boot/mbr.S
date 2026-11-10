/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 16-bit Real Mode to 32-bit Protected Mode MBR Bootloader
 * Loads kernel from LBA 1..192 into 0x100000, sets VBIOS Mode 12h, and jumps to _start (0x100030)
 */

.section .text
.code16
.global mbr_start
mbr_start:
    jmp boot_real
    nop

/* Disk label at offset 3 (preserves /dev/sda signature check) */
.ascii "OKSINIX_USB_STORAGE_OK\n\0"

/*
 * Reserve bytes 0x03..0x59 (90-byte BIOS Parameter Block area).
 * Many real USB BIOSes patch BPB geometry fields at 0x7C0B..0x7C59 in RAM
 * before jumping to 0x7C00; placing boot_real at 0x5A prevents code corruption.
 */
.org 0x5A

boot_real:
    cli
    cld
    /* Normalize CS:IP to 0x0000:boot_cs_sync in case BIOS jumped to 0x07C0:0x0000 */
    ljmp $0x0000, $boot_cs_sync

boot_cs_sync:
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7C00, %sp
    movw %ax, (0x7E12)

    movb %dl, boot_drive

    /* Print boot banner via BIOS INT 10h AH=0Eh */
    movw $msg_boot, %si
    call bios_puts

    /* Enable A20 via BIOS INT 15h */
    movw $0x2401, %ax
    int $0x15

    /* Enable Fast A20 via port 0x92 (only write if bit 1 is not already set) */
    inb $0x92, %al
    testb $0x02, %al
    jnz a20_done
    orb $0x02, %al
    andb $0xFE, %al
    outb %al, $0x92
a20_done:

    /* Query ACPI E820 memory map via BIOS INT 15h EAX=0xE820 into 0x8000 */
    xorl %ebx, %ebx
    movw %bx, (0x8000)
    movw $0x8004, %di
e820_loop:
    movl $0x0000E820, %eax
    movl $0x534D4150, %edx
    movl $20, %ecx
    int $0x15
    jc e820_done
    incw (0x8000)
    addw $20, %di
    cmpw $64, (0x8000)
    jae e820_done
    testl %ebx, %ebx
    jnz e820_loop
e820_done:

    /*
     * Load 240 sectors (120 KiB) from LBA 1..240 to physical 0x10000..0x2DFFF
     * using 15 safe 16-sector (8 KiB) transfers so we never exceed USB BIOS
     * max transfer size or cross a 64 KiB DMA boundary.
     */
    movw $15, %di
load_loop:
    call read_dap_retry
    addw $0x0200, dap_segment
    addl $16, dap_lba_lo
    decw %di
    jnz load_loop

    /*
     * Switch display to VESA VBE 2.0 640x480 Linear Framebuffer (mode 0x0112)
     * with ModeInfoBlock at 0x7E00, or fall back to VGA Mode 12h (640x480x16).
     */
    movw $0x7E00, %di
    movw $0x0112, %cx
    movw $0x4F01, %ax
    int $0x10
    cmpw $0x004F, %ax
    jne vbe_fallback_12h
    movw $0x4112, %bx
    movw $0x4F02, %ax
    int $0x10
    cmpw $0x004F, %ax
    je vbe_done
vbe_fallback_12h:
    movw $0x0012, %ax
    int $0x10
vbe_done:

    /* Enter 32-bit Protected Mode */
    cli
    xorw %ax, %ax
    movw %ax, %ds
    lgdt gdtr
    movl %cr0, %eax
    orl $0x01, %eax
    movl %eax, %cr0
    ljmpl $0x08, $pm_entry

/* Read 16 sectors via BIOS INT 13h AH=42h with up to 3 retries + disk reset */
read_dap_retry:
    movw $3, %cx
1:
    pushw %cx
    xorw %ax, %ax
    movw %ax, %ds
    movw $dap, %si
    movb $0x42, %ah
    movb boot_drive, %dl
    int $0x13
    popw %cx
    jnc 2f
    xorw %ax, %ax
    movb boot_drive, %dl
    int $0x13
    decw %cx
    jnz 1b
    movw $msg_err, %si
    call bios_puts
    jmp disk_error
2:
    ret

bios_puts:
    lodsb
    testb %al, %al
    jz 1f
    movb $0x0E, %ah
    movw $0x0007, %bx
    int $0x10
    jmp bios_puts
1:
    ret

disk_error:
    cli
    hlt
    jmp disk_error

.code32
pm_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    movl $0x7C00, %esp

    /* Copy 120 KiB (30720 dwords) from 0x00010000 to 0x00100000 (1 MiB) */
    movl $0x00010000, %esi
    movl $0x00100000, %edi
    movl $30720, %ecx
    rep movsl

    /* Pass Multiboot-compatible magic in %eax and jump to _start at 0x00100030 */
    movl $0x2BADB002, %eax
    xorl %ebx, %ebx
    movl $0x00100030, %edx
    jmp *%edx

msg_boot:
    .asciz "Oksinix\r\n"
msg_err:
    .asciz "ERR\r\n"

boot_drive:
    .byte 0x80

/* BIOS INT 13h AH=42h Disk Address Packet (16 bytes) */
.align 2
dap:
    .byte 0x10          /* Size of packet (16 bytes) */
    .byte 0x00          /* Reserved */
dap_count:
    .word 16            /* 16 sectors per transfer (8 KiB, safe for all USB BIOSes) */
dap_offset:
    .word 0x0000        /* Transfer buffer offset */
dap_segment:
    .word 0x1000        /* Initial segment (0x1000:0x0000 = 0x10000) */
dap_lba_lo:
    .long 1             /* Starting LBA (low 32 bits) */
dap_lba_hi:
    .long 0             /* Starting LBA (high 32 bits) */

/* Temporary GDT for 32-bit Protected Mode transition (gdtr embedded in slot 0) */
.align 4
gdt_start:
gdtr:
    .word (gdt_end - gdt_start - 1)
    .long gdt_start
    .word 0x0000
    .quad 0x00CF9A000000FFFF /* 0x08: 32-bit Code segment (base=0, limit=4G) */
    .quad 0x00CF92000000FFFF /* 0x10: 32-bit Data segment (base=0, limit=4G) */
gdt_end:

/* Pad to MBR Partition Table at offset 446 (0x1BE) */
.org 446
    /* Partition 1: Bootable (0x80), FAT32 LBA (0x0C), LBA 256, 1792 sectors */
    .byte 0x80, 0x01, 0x01, 0x00, 0x0C, 0x0F, 0x3F, 0x04
    .long 256
    .long 1792
    /* Partitions 2..4: Empty */
    .space 48, 0

/* Bootable MBR signature at offset 510 (0x1FE) */
.word 0xAA55
.section .note.GNU-stack,"",@progbits
