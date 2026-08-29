# GEMIBOOT x86 boot loader for GEMIOS

agy

Public domain [CC0](LICENSE.txt) multiboot complient boot loader written in GNU x86 assembly and strict C90 complied by clang. 
Supports UEFI 32 bit and 64 bit startup and old fashion BIOS.
Provides multiboot 1 memory map (1) and framebuffer (12) information.
Tries best attempt to  provide a configured framebuffer of 640x480 16 colors or RGB.
Load the gemios.elf kernel and jump to its entry point in 32 bit protected mode  RING 0 flat memory schem, A20 enabled, GDT code and data segment configured.

Test is done using QEMU for the three boot modes and Bochs for BIOS mode booting from a SuperSpeed USB drive with USB keyboard and mouse on an xHCI controller.

### Running & Testing

- `make test` or `make test-all`: Run both QEMU and Bochs automated test suites
- `make test-qemu`: Run QEMU automated test suite (BIOS, UEFI32, UEFI64)
- `make test-bochs`: Run Bochs automated test suite (xHCI USB boot with USB Mass Storage, Keyboard & Mouse)
- `make run-bios`: Launch QEMU with BIOS boot
- `make run-uefi64`: Launch QEMU with UEFI 64-bit boot
- `make run-uefi32`: Launch QEMU with UEFI 32-bit boot
- `make run-bochs`: Launch Bochs virtual machine (booting from xHCI USB drive)

https://kojipkgs.fedoraproject.org//packages/edk2/20250812/21.fc42/noarch/edk2-ovmf-ia32-20250812-21.fc42.noarch.rpm


