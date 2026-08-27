/*
 * GEMIBOOT - x86 Boot Loader for GEMIOS
 * Dedicated to the Public Domain (CC0)
 */

#include "types.h"
#include "multiboot.h"
#include "elf.h"
#include "fat.h"
#include "ata.h"
#include "io.h"

extern void serial_init(void);
extern void serial_puts(const char *s);
extern void serial_put_hex(uint32_t val);
extern void serial_put_dec(uint32_t val);
extern void multiboot_jump(uint32_t entry_point, uint32_t magic, uint32_t mbi_addr);

/* MBR partition entry structure */
#pragma pack(push, 1)
struct mbr_part_entry {
    uint8_t  status;
    uint8_t  start_chs[3];
    uint8_t  type;
    uint8_t  end_chs[3];
    uint32_t start_lba;
    uint32_t length_lba;
};
#pragma pack(pop)

#define KERNEL_LOAD_TEMP_ADDR 0x00200000
#define MULTIBOOT_INFO_ADDR   0x00095000
#define MMAP_BUFFER_ADDR      0x00008000
#define GFX_INFO_BUFFER_ADDR  0x00009000

static uint32_t find_active_partition_lba(void) {
    uint8_t mbr_buf[512];
    struct mbr_part_entry *parts;
    int i;

    if (ata_read_sectors(0, 1, mbr_buf) != 0) {
        return 2048; /* Default fallback */
    }

    parts = (struct mbr_part_entry*)(mbr_buf + 446);
    for (i = 0; i < 4; i++) {
        if (parts[i].status == 0x80 && parts[i].start_lba != 0) {
            return parts[i].start_lba;
        }
    }

    /* If no active partition is found, check if first partition exists */
    if (parts[0].start_lba != 0) {
        return parts[0].start_lba;
    }

    return 2048;
}

void bios_loader_main(void) {
    fat_fs_t fs;
    uint32_t part_lba;
    uint32_t first_clus = 0;
    uint32_t file_size = 0;
    uint32_t entry_point = 0;
    struct multiboot_info *mbi;
    uint8_t *kernel_temp = (uint8_t*)KERNEL_LOAD_TEMP_ADDR;
    uint32_t e820_count;
    uint32_t mmap_len;

    serial_init();
    serial_puts("\n=======================================================\n");
    serial_puts(" GEMIBOOT - Multiboot x86 Boot Loader for GEMIOS (BIOS)\n");
    serial_puts("=======================================================\n");

    ata_init();

    part_lba = find_active_partition_lba();
    serial_puts("[GEMIBOOT] Found active partition at LBA: ");
    serial_put_dec(part_lba);
    serial_puts("\n");

    if (fat_init(&fs, part_lba, ata_read_sectors) != 0) {
        serial_puts("[GEMIBOOT] ERROR: Failed to initialize FAT filesystem!\n");
        while (1) {
            __asm__ volatile ("hlt");
        }
    }

    serial_puts("[GEMIBOOT] Searching for gemios.elf...\n");
    if (fat_find_file(&fs, ata_read_sectors, "gemios.elf", &first_clus, &file_size) != 0 &&
        fat_find_file(&fs, ata_read_sectors, "GEMIOS.ELF", &first_clus, &file_size) != 0) {
        serial_puts("[GEMIBOOT] ERROR: gemios.elf not found on disk!\n");
        while (1) {
            __asm__ volatile ("hlt");
        }
    }

    serial_puts("[GEMIBOOT] Found gemios.elf, size: ");
    serial_put_dec(file_size);
    serial_puts(" bytes. Loading into memory...\n");

    if (fat_read_file(&fs, ata_read_sectors, first_clus, file_size, kernel_temp) != 0) {
        serial_puts("[GEMIBOOT] ERROR: Failed to read gemios.elf!\n");
        while (1) {
            __asm__ volatile ("hlt");
        }
    }

    serial_puts("[GEMIBOOT] Parsing ELF32 and loading sections...\n");
    entry_point = elf32_load(kernel_temp, file_size);
    if (entry_point == 0) {
        serial_puts("[GEMIBOOT] ERROR: Invalid ELF32 binary or load failed!\n");
        while (1) {
            __asm__ volatile ("hlt");
        }
    }

    serial_puts("[GEMIBOOT] Kernel entry point: ");
    serial_put_hex(entry_point);
    serial_puts("\n");

    /* Assemble Multiboot 1 info structure */
    mbi = (struct multiboot_info*)MULTIBOOT_INFO_ADDR;
    memset(mbi, 0, sizeof(struct multiboot_info));

    e820_count = *(uint32_t*)MMAP_BUFFER_ADDR;
    if (e820_count > 0 && e820_count < 128) {
        mmap_len = e820_count * 24;
        mbi->mmap_addr = MMAP_BUFFER_ADDR + 4;
        mbi->mmap_length = mmap_len;
        mbi->flags |= MULTIBOOT_INFO_MEM_MAP;
    } else {
        /* Fallback: construct a simple 128MB RAM memory map */
        struct multiboot_mmap_entry *entries = (struct multiboot_mmap_entry*)(MMAP_BUFFER_ADDR + 4);
        
        /* 0x00000000 - 0x0009FC00 (639 KB Available) */
        entries[0].size = 20;
        entries[0].addr_low = 0x00000000;
        entries[0].addr_high = 0;
        entries[0].len_low = 0x0009FC00;
        entries[0].len_high = 0;
        entries[0].type = MULTIBOOT_MEMORY_AVAILABLE;

        /* 0x0009FC00 - 0x00100000 (385 KB Reserved) */
        entries[1].size = 20;
        entries[1].addr_low = 0x0009FC00;
        entries[1].addr_high = 0;
        entries[1].len_low = 0x00060400;
        entries[1].len_high = 0;
        entries[1].type = MULTIBOOT_MEMORY_RESERVED;

        /* 0x00100000 - 0x08000000 (127 MB Available) */
        entries[2].size = 20;
        entries[2].addr_low = 0x00100000;
        entries[2].addr_high = 0;
        entries[2].len_low = (127 * 1024 * 1024);
        entries[2].len_high = 0;
        entries[2].type = MULTIBOOT_MEMORY_AVAILABLE;

        mbi->mmap_addr = (uint32_t)(uintptr_t)entries;
        mbi->mmap_length = 3 * 24;
        mbi->flags |= MULTIBOOT_INFO_MEM_MAP;
    }

    mbi->mem_lower = 640;
    mbi->mem_upper = 127 * 1024; /* 127 MB upper RAM */
    mbi->flags |= MULTIBOOT_INFO_MEMORY;

    mbi->boot_device = 0x8000FFFF;
    mbi->flags |= MULTIBOOT_INFO_BOOTDEV;

    mbi->boot_loader_name = (uint32_t)(uintptr_t)"gemiboot 1.0 (BIOS)";
    mbi->flags |= MULTIBOOT_INFO_BOOT_LOADER_NAME;

    /* Fill Framebuffer info from video setup in real mode */
    {
        uint32_t *gfx_data = (uint32_t*)GFX_INFO_BUFFER_ADDR;
        uint32_t gfx_mode = gfx_data[0];
        uint32_t gfx_width = gfx_data[1];
        uint32_t gfx_height = gfx_data[2];
        uint32_t gfx_pitch = gfx_data[3];
        uint32_t gfx_fb = gfx_data[4];

        if (gfx_fb != 0 && gfx_width > 0 && gfx_height > 0) {
            mbi->framebuffer_addr_low = gfx_fb;
            mbi->framebuffer_addr_hi = 0;
            mbi->framebuffer_width = gfx_width;
            mbi->framebuffer_height = gfx_height;
            mbi->framebuffer_pitch = gfx_pitch;
            mbi->framebuffer_bpp = (uint8_t)(gfx_mode == 1 ? 32 : (gfx_mode == 4 ? 8 : 4));
            mbi->framebuffer_type = (uint8_t)(gfx_mode == 1 ? MULTIBOOT_FRAMEBUFFER_TYPE_RGB : MULTIBOOT_FRAMEBUFFER_TYPE_INDEXED);
            mbi->flags |= MULTIBOOT_INFO_FRAMEBUFFER_INFO;
        } else {
            /* Standard VGA 80x25 text mode fallback */
            mbi->framebuffer_addr_low = 0xB8000;
            mbi->framebuffer_addr_hi = 0;
            mbi->framebuffer_width = 80;
            mbi->framebuffer_height = 25;
            mbi->framebuffer_pitch = 160;
            mbi->framebuffer_bpp = 16;
            mbi->framebuffer_type = MULTIBOOT_FRAMEBUFFER_TYPE_EGA_TEXT;
            mbi->flags |= MULTIBOOT_INFO_FRAMEBUFFER_INFO;
        }
    }

    serial_puts("[GEMIBOOT] Multiboot information prepared. Jumping to GEMIOS kernel...\n\n");

    /* Jump to kernel entry point */
    multiboot_jump(entry_point, MULTIBOOT_BOOTLOADER_MAGIC, MULTIBOOT_INFO_ADDR);

    while (1) {
        __asm__ volatile ("hlt");
    }
}
