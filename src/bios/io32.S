# GEMIBOOT - x86 Boot Loader for GEMIOS
# Dedicated to the Public Domain (CC0)

.code32
.text
.global protected32
.global multiboot_jump

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

# void multiboot_jump(uint32_t entry_point, uint32_t magic, uint32_t mbi_addr);
multiboot_jump:
    movl 4(%esp), %edx   # entry_point
    movl 8(%esp), %eax   # magic (0x2BADB002)
    movl 12(%esp), %ebx  # mbi_addr
    cli
    jmpl *%edx
