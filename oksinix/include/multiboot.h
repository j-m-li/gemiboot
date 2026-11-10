/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Multiboot v1 Specification structures & constants
 */

#ifndef OKSINIX_MULTIBOOT_H
#define OKSINIX_MULTIBOOT_H

#include "types.h"

#define MULTIBOOT_HEADER_MAGIC      0x1BADB002UL
#define MULTIBOOT_BOOTLOADER_MAGIC  0x2BADB002UL

/* Multiboot Info flags */
#define MULTIBOOT_INFO_MEMORY           (1UL << 0)
#define MULTIBOOT_INFO_BOOTDEV          (1UL << 1)
#define MULTIBOOT_INFO_CMDLINE          (1UL << 2)
#define MULTIBOOT_INFO_MODS             (1UL << 3)
#define MULTIBOOT_INFO_MEM_MAP          (1UL << 6)
#define MULTIBOOT_INFO_BOOT_LOADER_NAME (1UL << 9)
#define MULTIBOOT_INFO_VBE_INFO         (1UL << 11)
#define MULTIBOOT_INFO_FRAMEBUFFER_INFO (1UL << 12)

struct multiboot_mod_list {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t cmdline;
    uint32_t pad;
};

struct multiboot_mmap_entry {
    uint32_t size;
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t len_lo;
    uint32_t len_hi;
    uint32_t type;
};

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint32_t framebuffer_addr_lo;
    uint32_t framebuffer_addr_hi;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  color_info[6];
};

#endif /* OKSINIX_MULTIBOOT_H */
