/*
 * GEMIBOOT - x86 Boot Loader for GEMIOS
 * Dedicated to the Public Domain (CC0)
 */

#include "efi.h"
#include "multiboot.h"
#include "elf.h"
#include "types.h"

#define MULTIBOOT_INFO_ADDR 0x00095000
#define MMAP_BUFFER_ADDR    0x00008000
#define KERNEL_TEMP_ADDR    0x00200000
#define UEFI_MMAP_BUF_SIZE  0x4000

extern void jump_to_32bit_kernel(uint64_t entry_point, uint64_t magic, uint64_t mbi_addr);

static EFI_SYSTEM_TABLE *g_st = NULL;

static void uefi_puts(const char *str) {
    utf16 buf[256];
    size_t i = 0, j = 0;
    if (!g_st || !g_st->ConOut) return;

    while (str[i] && j < 254) {
        if (str[i] == '\n') {
            buf[j++] = '\r';
        }
        buf[j++] = (utf16)(uint8_t)str[i++];
    }
    buf[j] = 0;
    g_st->ConOut->OutputString(g_st->ConOut, buf);
}

static void uefi_put_dec(uint32_t val) {
    char buf[16];
    int i = 0;
    if (val == 0) {
        uefi_puts("0");
        return;
    }
    while (val > 0) {
        buf[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    buf[i] = 0;
    {
        int start = 0, end = i - 1;
        while (start < end) {
            char t = buf[start];
            buf[start++] = buf[end];
            buf[end--] = t;
        }
    }
    uefi_puts(buf);
}

static void uefi_put_hex(uint32_t val) {
    static const char hex_chars[] = "0123456789ABCDEF";
    char buf[12];
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 8; i++) {
        buf[2 + i] = hex_chars[(val >> (28 - i * 4)) & 0xF];
    }
    buf[10] = 0;
    uefi_puts(buf);
}

static efi_status init_gop(EFI_GRAPHICS_OUTPUT_PROTOCOL **out_gop, struct multiboot_info *mbi) {
    efi_guid gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
    uintptr_t info_size = 0;
    uint32_t best_mode = 0xFFFFFFFF;
    uint32_t fallback_mode = 0xFFFFFFFF;
    uint32_t mode;
    efi_status status;

    status = g_st->BootServices->LocateProtocol(&gop_guid, NULL, (void**)&gop);
    if (status != EFI_SUCCESS || !gop) {
        return status;
    }

    if (out_gop) *out_gop = gop;

    for (mode = 0; mode < gop->Mode->MaxMode; mode++) {
        status = gop->QueryMode(gop, mode, &info_size, &info);
        if (status == EFI_SUCCESS && info) {
            if (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ||
                info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
                if (fallback_mode == 0xFFFFFFFF) {
                    fallback_mode = mode;
                }
                if (info->HorizontalResolution == 640 && info->VerticalResolution == 480) {
                    best_mode = mode;
                    break;
                }
            }
        }
    }

    if (best_mode == 0xFFFFFFFF) {
        best_mode = (fallback_mode != 0xFFFFFFFF) ? fallback_mode : gop->Mode->Mode;
    }

    if (best_mode != gop->Mode->Mode) {
        gop->SetMode(gop, best_mode);
    }

    if (gop->Mode && gop->Mode->Info) {
        mbi->framebuffer_addr_low = (uint32_t)(gop->Mode->FrameBufferBase & 0xFFFFFFFF);
        mbi->framebuffer_addr_hi = (uint32_t)(gop->Mode->FrameBufferBase >> 32);
        mbi->framebuffer_width = gop->Mode->Info->HorizontalResolution;
        mbi->framebuffer_height = gop->Mode->Info->VerticalResolution;
        mbi->framebuffer_pitch = gop->Mode->Info->PixelsPerScanLine * 4;
        mbi->framebuffer_bpp = 32;
        mbi->framebuffer_type = MULTIBOOT_FRAMEBUFFER_TYPE_RGB;

        if (gop->Mode->Info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
            mbi->framebuffer_red_field_position = 0;
            mbi->framebuffer_red_mask_size = 8;
            mbi->framebuffer_green_field_position = 8;
            mbi->framebuffer_green_mask_size = 8;
            mbi->framebuffer_blue_field_position = 16;
            mbi->framebuffer_blue_mask_size = 8;
        } else {
            mbi->framebuffer_blue_field_position = 0;
            mbi->framebuffer_blue_mask_size = 8;
            mbi->framebuffer_green_field_position = 8;
            mbi->framebuffer_green_mask_size = 8;
            mbi->framebuffer_red_field_position = 16;
            mbi->framebuffer_red_mask_size = 8;
        }
        mbi->flags |= MULTIBOOT_INFO_FRAMEBUFFER_INFO;
    }

    return EFI_SUCCESS;
}

static efi_status read_kernel_file(efi_handle image_handle, void *dest_buf, uintptr_t max_size, uintptr_t *out_size) {
    efi_guid loaded_img_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    efi_guid fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_img = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    EFI_FILE_PROTOCOL *file = NULL;
    efi_status status;
    utf16 name1[] = {'g','e','m','i','o','s','.','e','l','f', 0};
    utf16 name2[] = {'\\','g','e','m','i','o','s','.','e','l','f', 0};
    utf16 name3[] = {'E','F','I','\\','B','O','O','T','\\','g','e','m','i','o','s','.','e','l','f', 0};
    uintptr_t read_bytes;

    status = g_st->BootServices->HandleProtocol(image_handle, &loaded_img_guid, (void**)&loaded_img);
    if (status == EFI_SUCCESS && loaded_img && loaded_img->DeviceHandle) {
        status = g_st->BootServices->HandleProtocol(loaded_img->DeviceHandle, &fs_guid, (void**)&fs);
    }
    if (status != EFI_SUCCESS || !fs) {
        status = g_st->BootServices->LocateProtocol(&fs_guid, NULL, (void**)&fs);
    }
    if (status != EFI_SUCCESS || !fs) {
        return status;
    }

    status = fs->OpenVolume(fs, &root);
    if (status != EFI_SUCCESS || !root) {
        return status;
    }

    status = root->Open(root, &file, name1, EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS) {
        status = root->Open(root, &file, name2, EFI_FILE_MODE_READ, 0);
    }
    if (status != EFI_SUCCESS) {
        status = root->Open(root, &file, name3, EFI_FILE_MODE_READ, 0);
    }
    if (status != EFI_SUCCESS || !file) {
        root->Close(root);
        return EFI_NOT_FOUND;
    }

    read_bytes = max_size;
    status = file->Read(file, &read_bytes, dest_buf);
    file->Close(file);
    root->Close(root);

    if (status == EFI_SUCCESS && out_size) {
        *out_size = read_bytes;
    }
    return status;
}

static void convert_memory_map(EFI_MEMORY_DESCRIPTOR *mmap, uintptr_t mmap_size, uintptr_t desc_size, struct multiboot_info *mbi) {
    struct multiboot_mmap_entry *entries = (struct multiboot_mmap_entry*)(MMAP_BUFFER_ADDR + 4);
    uintptr_t num_desc = mmap_size / desc_size;
    uintptr_t i;
    uint32_t entry_count = 0;
    uint64_t total_ram_bytes = 0;

    for (i = 0; i < num_desc && entry_count < 120; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR*)((uint8_t*)mmap + i * desc_size);
        uint64_t len = desc->NumberOfPages * 4096;
        uint32_t type;

        if (desc->Type == EfiLoaderCode || desc->Type == EfiLoaderData ||
            desc->Type == EfiBootServicesCode || desc->Type == EfiBootServicesData ||
            desc->Type == EfiConventionalMemory) {
            type = MULTIBOOT_MEMORY_AVAILABLE;
            total_ram_bytes += len;
        } else if (desc->Type == EfiACPIReclaimMemory) {
            type = MULTIBOOT_MEMORY_ACPI_RECLAIMABLE;
        } else if (desc->Type == EfiACPIMemoryNVS) {
            type = MULTIBOOT_MEMORY_NVS;
        } else {
            type = MULTIBOOT_MEMORY_RESERVED;
        }

        entries[entry_count].size = 20;
        entries[entry_count].addr_low = (uint32_t)(desc->PhysicalStart & 0xFFFFFFFF);
        entries[entry_count].addr_high = (uint32_t)(desc->PhysicalStart >> 32);
        entries[entry_count].len_low = (uint32_t)(len & 0xFFFFFFFF);
        entries[entry_count].len_high = (uint32_t)(len >> 32);
        entries[entry_count].type = type;
        entry_count++;
    }

    *(uint32_t*)MMAP_BUFFER_ADDR = entry_count;
    mbi->mmap_addr = MMAP_BUFFER_ADDR + 4;
    mbi->mmap_length = entry_count * 24;
    mbi->flags |= MULTIBOOT_INFO_MEM_MAP;

    mbi->mem_lower = 640;
    mbi->mem_upper = (uint32_t)(total_ram_bytes > (1024 * 1024) ? (total_ram_bytes - (1024 * 1024)) / 1024 : 127 * 1024);
    mbi->flags |= MULTIBOOT_INFO_MEMORY;
}

int efi_main(void *image_handle, EFI_SYSTEM_TABLE *system_table) {
    struct multiboot_info *mbi;
    uint8_t *kernel_temp = (uint8_t*)KERNEL_TEMP_ADDR;
    uintptr_t kernel_size = 0;
    uint32_t entry_point = 0;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    uint8_t uefi_mmap_buf[UEFI_MMAP_BUF_SIZE];
    uintptr_t mmap_size = sizeof(uefi_mmap_buf);
    uintptr_t map_key = 0;
    uintptr_t desc_size = 0;
    uint32_t desc_ver = 0;
    efi_status status;

    g_st = system_table;

    uefi_puts("\n=======================================================\n");
    uefi_puts(" GEMIBOOT - Multiboot x86 Boot Loader (UEFI 64-bit)\n");
    uefi_puts("=======================================================\n");

    mbi = (struct multiboot_info*)MULTIBOOT_INFO_ADDR;
    memset(mbi, 0, sizeof(struct multiboot_info));

    mbi->boot_device = 0x8000FFFF;
    mbi->flags |= MULTIBOOT_INFO_BOOTDEV;
    strcpy((char*)(MULTIBOOT_INFO_ADDR + 256), "gemiboot 1.0 (UEFI 64-bit)");
    mbi->boot_loader_name = (uint32_t)(MULTIBOOT_INFO_ADDR + 256);
    mbi->flags |= MULTIBOOT_INFO_BOOT_LOADER_NAME;

    /* 1. Setup GOP */
    uefi_puts("[GEMIBOOT] Initializing Graphics Output Protocol (GOP)...\n");
    init_gop(&gop, mbi);

    /* 2. Load Kernel File */
    uefi_puts("[GEMIBOOT] Reading gemios.elf from EFI volume...\n");
    status = read_kernel_file(image_handle, kernel_temp, 4 * 1024 * 1024, &kernel_size);
    if (status != EFI_SUCCESS || kernel_size == 0) {
        uefi_puts("[GEMIBOOT] ERROR: Failed to find or read gemios.elf!\n");
        while (1) { ; }
    }

    uefi_puts("[GEMIBOOT] Read gemios.elf: ");
    uefi_put_dec((uint32_t)kernel_size);
    uefi_puts(" bytes. Loading ELF segments...\n");

    entry_point = elf32_load(kernel_temp, kernel_size);
    if (entry_point == 0) {
        uefi_puts("[GEMIBOOT] ERROR: Failed to parse and load ELF32 kernel!\n");
        while (1) { ; }
    }

    uefi_puts("[GEMIBOOT] Kernel entry point: ");
    uefi_put_hex(entry_point);
    uefi_puts("\n");

    /* 3. Get Memory Map */
    mmap_size = sizeof(uefi_mmap_buf);
    status = g_st->BootServices->GetMemoryMap(&mmap_size, (EFI_MEMORY_DESCRIPTOR*)uefi_mmap_buf, &map_key, &desc_size, &desc_ver);
    if (status == EFI_SUCCESS) {
        convert_memory_map((EFI_MEMORY_DESCRIPTOR*)uefi_mmap_buf, mmap_size, desc_size, mbi);
    }

    /* 4. Exit Boot Services */
    uefi_puts("[GEMIBOOT] Exiting UEFI Boot Services and jumping to GEMIOS...\n");

    status = g_st->BootServices->ExitBootServices(image_handle, map_key);
    if (status != EFI_SUCCESS) {
        /* Retry after re-fetching memory map */
        mmap_size = sizeof(uefi_mmap_buf);
        g_st->BootServices->GetMemoryMap(&mmap_size, (EFI_MEMORY_DESCRIPTOR*)uefi_mmap_buf, &map_key, &desc_size, &desc_ver);
        convert_memory_map((EFI_MEMORY_DESCRIPTOR*)uefi_mmap_buf, mmap_size, desc_size, mbi);
        g_st->BootServices->ExitBootServices(image_handle, map_key);
    }

    /* 5. Switch to 32-bit protected mode and jump to kernel */
    jump_to_32bit_kernel((uint64_t)entry_point, MULTIBOOT_BOOTLOADER_MAGIC, (uint64_t)MULTIBOOT_INFO_ADDR);

    while (1) { ; }
    return 0;
}
