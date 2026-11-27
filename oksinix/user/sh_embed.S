/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix - Embed standalone Ring-3 user/sh.elf into ramfs at /bin/sh
 * Ensures /bin/sh is a real Ring-3 ELF32 executable available on BIOS, Multiboot,
 * UEFI 32-bit, and UEFI 64-bit even before/without an external FAT32 disk.
 */

.section .rodata
.align 4
.global bin_sh_elf_start
.global bin_sh_elf_end
bin_sh_elf_start:
    .incbin "user/sh.elf"
bin_sh_elf_end:

.section .note.GNU-stack,"",@progbits
