# GEMIBOOT - x86 Boot Loader for GEMIOS
# Dedicated to the Public Domain (CC0)

#include "defs.h"

.set STROFF, -512

.code32
.text
.global protected32
.global multiboot_jump
.global bios_disk_read_raw
.global thunk_pm32_return
.extern _start
.extern bios_int13_thunk16
.extern dap_lba
.extern dap_sectors
.extern dap_segment
.extern dap_offset
.extern saved_esp32

# Entry point called by io16.S after switching to 32-bit protected mode
protected32:
    # Setup flat 32-bit data segments
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    # Setup stack below 0x80000 (safe area)
    movl $0x0007FFF0, %esp
    movl %esp, %ebp

    # Reset EFLAGS
    pushl $0
    popf

    # Call 32-bit C loader
    call bios_loader_main

halt_loop:
    cli
    hlt
    jmp halt_loop

# int bios_disk_read_raw(uint32_t lba, uint16_t sectors, uint16_t segment, uint16_t offset);
bios_disk_read_raw:
    pushl %ebx
    pushl %esi
    pushl %edi
    pushl %ebp

    movb $0x10, dap_size
    movb $0x00, dap_reserved

    movl 20(%esp), %eax    # lba
    movl %eax, dap_lba
    movl $0, dap_lba + 4

    movl 24(%esp), %eax    # sectors
    movw %ax, dap_sectors

    movl 28(%esp), %eax    # segment
    movw %ax, dap_segment

    movl 32(%esp), %eax    # offset
    movw %ax, dap_offset

    # Save 32-bit stack pointer
    movl %esp, saved_esp32

    # Switch to 16-bit protected mode segments (Base = 0x10000)
    cli
    movw $0x20, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    # Far jump to 16-bit code segment (CS=0x18, Base=0x10000)
    ljmp $0x18, $bios_int13_thunk16_offset

thunk_pm32_return:
    # Restore flat 32-bit data segments
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    # Restore 32-bit stack pointer
    movl saved_esp32, %esp

    # Return value from %ebx into %eax (0 = success, 1 = error)
    movl %ebx, %eax

    popl %ebp
    popl %edi
    popl %esi
    popl %ebx
    ret

# void multiboot_jump(uint32_t entry_point, uint32_t magic, uint32_t mbi_addr);
multiboot_jump:
    movl 4(%esp), %edx   # entry_point
    movl 8(%esp), %eax   # magic (0x2BADB002)
    movl 12(%esp), %ebx  # mbi_addr
    cli
    jmpl *%edx
