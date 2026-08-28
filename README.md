# GEMIBOOT x86 boot loader for GEMIOS

agy

Public domain [CC0](LICENSE.txt) multiboot complient boot loader written in GNU x86 assembly and strict C90 complied by clang. 
Supports UEFI 32 bit and 64 bit startup and old fashion BIOS.
Provides multiboot 1 memory map (1) and framebuffer (12) information.
Tries best attempt to  provide a configured framebuffer of 640x480 16 colors or RGB.
Load the gemios.elf kernel and jump to its entry point in 32 bit protected mode  RING 0 flat memory schem, A20 enabled, GDT code and data segment configured.

Test is done using qemu for the three boot modes.

https://kojipkgs.fedoraproject.org//packages/edk2/20250812/21.fc42/noarch/edk2-ovmf-ia32-20250812-21.fc42.noarch.rpm

