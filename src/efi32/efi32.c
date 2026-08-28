/*
 * GEMIBOOT - x86 Boot Loader for GEMIOS
 * Dedicated to the Public Domain (CC0)
 */

#include "efi.h"
#include "multiboot.h"
#include "elf.h"
#include "types.h"
#include "font.h"

#define MULTIBOOT_INFO_ADDR 0x00095000
#define MMAP_BUFFER_ADDR    0x00008000
#define KERNEL_TEMP_ADDR    0x00200000
#define UEFI_MMAP_BUF_SIZE  0x4000

extern void load_gdt_and_jump(uint32_t entry_point, uint32_t magic, uint32_t mbi_addr);

static EFI_SYSTEM_TABLE *g_st = NULL;

static uintptr_t g_fb_base = 0;
static uint32_t  g_fb_width = 0;
static uint32_t  g_fb_height = 0;
static uint32_t  g_fb_pitch = 0;
static uint32_t  g_cursor_x = 0;
static uint32_t  g_cursor_y = 0;
static uint32_t  g_fg_color = 0x00FFFFFF; /* White */
static uint32_t  g_bg_color = 0x00000000; /* Black */

static void gop_clear_screen(void) {
    if (!g_fb_base || !g_fb_height || !g_fb_pitch) return;
    memset((void*)g_fb_base, 0, g_fb_height * g_fb_pitch);
    g_cursor_x = 0;
    g_cursor_y = 0;
}

static void gop_scroll(void) {
    uint32_t line_bytes;
    uint32_t copy_bytes;
    if (!g_fb_base || !g_fb_height || !g_fb_pitch) return;
    line_bytes = FONT_HEIGHT * g_fb_pitch;
    copy_bytes = (g_fb_height - FONT_HEIGHT) * g_fb_pitch;
    memmove((void*)g_fb_base, (void*)(g_fb_base + line_bytes), copy_bytes);
    memset((void*)(g_fb_base + copy_bytes), 0, line_bytes);
}

static void gop_draw_char(char c) {
    const uint8_t *glyph;
    uint32_t px_start, py_start;
    uint32_t y, x;

    if (!g_fb_base || !g_fb_width || !g_fb_height) return;

    if (c == '\r') {
        g_cursor_x = 0;
        return;
    }
    if (c == '\n') {
        g_cursor_x = 0;
        g_cursor_y++;
        if ((g_cursor_y + 1) * FONT_HEIGHT > g_fb_height) {
            gop_scroll();
            g_cursor_y = (g_fb_height / FONT_HEIGHT) - 1;
        }
        return;
    }

    if ((g_cursor_x + 1) * FONT_WIDTH > g_fb_width) {
        g_cursor_x = 0;
        g_cursor_y++;
        if ((g_cursor_y + 1) * FONT_HEIGHT > g_fb_height) {
            gop_scroll();
            g_cursor_y = (g_fb_height / FONT_HEIGHT) - 1;
        }
    }

    if ((uint8_t)c >= FONT_FIRST_CHAR && (uint8_t)c <= FONT_LAST_CHAR) {
        glyph = &g_font_data[((uint8_t)c - FONT_FIRST_CHAR) * FONT_HEIGHT];
    } else {
        static const uint8_t blank_glyph[16] = {0};
        glyph = blank_glyph;
    }

    px_start = g_cursor_x * FONT_WIDTH;
    py_start = g_cursor_y * FONT_HEIGHT;

    for (y = 0; y < FONT_HEIGHT; y++) {
        uint8_t row = glyph[y];
        uint32_t *line = (uint32_t*)(g_fb_base + (py_start + y) * g_fb_pitch + px_start * 4);
        for (x = 0; x < FONT_WIDTH; x++) {
            line[x] = (row & (0x80 >> x)) ? g_fg_color : g_bg_color;
        }
    }

    g_cursor_x++;
}

static void uefi_puts(const char *str) {
    utf16 buf[256];
    size_t i = 0, j = 0;

    /* 1. Output to UEFI Console (Serial / Firmware Console) */
    if (g_st && g_st->ConOut) {
        while (str[i] && j < 254) {
            if (str[i] == '\n') {
                buf[j++] = '\r';
            }
            buf[j++] = (utf16)(uint8_t)str[i];
            if (g_fb_base) {
                gop_draw_char(str[i]);
            }
            i++;
        }
        buf[j] = 0;
        g_st->ConOut->OutputString(g_st->ConOut, buf);
    }

    /* 2. Output to GOP Framebuffer if remaining */
    while (str[i]) {
        if (g_fb_base) {
            gop_draw_char(str[i]);
        }
        i++;
    }
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
    /* Reverse */
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

    /* Scan available GOP modes */
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

    /* Record GOP framebuffer in multiboot info */
    if (gop->Mode && gop->Mode->Info) {
        g_fb_base = (uintptr_t)gop->Mode->FrameBufferBase;
        g_fb_width = gop->Mode->Info->HorizontalResolution;
        g_fb_height = gop->Mode->Info->VerticalResolution;
        g_fb_pitch = gop->Mode->Info->PixelsPerScanLine * 4;
        gop_clear_screen();

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

static bool utf16_case_equals(const utf16 *u, const char *ascii) {
    size_t i = 0;
    while (ascii[i]) {
        char c1 = ascii[i];
        char c2 = (char)(u[i] & 0xFF);
        if (c1 >= 'a' && c1 <= 'z') c1 = (char)(c1 - 32);
        if (c2 >= 'a' && c2 <= 'z') c2 = (char)(c2 - 32);
        if (c1 != c2) return false;
        i++;
    }
    return u[i] == 0;
}

static efi_status try_open_file_on_fs(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs, void *dest_buf, uintptr_t max_size, uintptr_t *out_size) {
    static const utf16 p1[] = {'G','E','M','I','O','S','.','E','L','F', 0};
    static const utf16 p2[] = {'\\','G','E','M','I','O','S','.','E','L','F', 0};
    static const utf16 p3[] = {'g','e','m','i','o','s','.','e','l','f', 0};
    static const utf16 p4[] = {'\\','g','e','m','i','o','s','.','e','l','f', 0};
    static const utf16 p5[] = {'E','F','I','\\','B','O','O','T','\\','G','E','M','I','O','S','.','E','L','F', 0};
    static const utf16 p6[] = {'E','F','I','\\','B','O','O','T','\\','g','e','m','i','o','s','.','e','l','f', 0};
    static const utf16 p7[] = {'\\','E','F','I','\\','B','O','O','T','\\','G','E','M','I','O','S','.','E','L','F', 0};
    static const utf16 p8[] = {'\\','E','F','I','\\','B','O','O','T','\\','g','e','m','i','o','s','.','e','l','f', 0};
    const utf16 *paths[9];
    EFI_FILE_PROTOCOL *root = NULL;
    EFI_FILE_PROTOCOL *file = NULL;
    efi_status status;
    int p;

    paths[0] = p1;
    paths[1] = p2;
    paths[2] = p3;
    paths[3] = p4;
    paths[4] = p5;
    paths[5] = p6;
    paths[6] = p7;
    paths[7] = p8;
    paths[8] = NULL;

    if (!fs) return EFI_INVALID_PARAMETER;

    status = fs->OpenVolume(fs, &root);
    if (status != EFI_SUCCESS || !root) {
        return status;
    }

    /* 1. Try well-known direct paths */
    for (p = 0; paths[p] != NULL; p++) {
        status = root->Open(root, &file, (utf16*)paths[p], EFI_FILE_MODE_READ, 0);
        if (status == EFI_SUCCESS && file) {
            break;
        }
    }

    /* 2. If not found by path, list root directory entries */
    if (!file) {
        uint8_t buf[512];
        uintptr_t buf_size;
        root->SetPosition(root, 0);
        while (1) {
            buf_size = sizeof(buf);
            status = root->Read(root, &buf_size, buf);
            if (status != EFI_SUCCESS || buf_size == 0) {
                break;
            }
            {
                EFI_FILE_INFO *info = (EFI_FILE_INFO*)buf;
                if (!(info->Attribute & EFI_FILE_DIRECTORY)) {
                    if (utf16_case_equals(info->FileName, "gemios.elf") ||
                        utf16_case_equals(info->FileName, "GEMIOS.ELF")) {
                        status = root->Open(root, &file, info->FileName, EFI_FILE_MODE_READ, 0);
                        if (status == EFI_SUCCESS && file) {
                            break;
                        }
                    }
                }
            }
        }
    }

    if (!file) {
        root->Close(root);
        return EFI_NOT_FOUND;
    }

    /* Read kernel contents */
    {
        uintptr_t read_bytes = max_size;
        status = file->Read(file, &read_bytes, dest_buf);
        file->Close(file);
        root->Close(root);

        if (status == EFI_SUCCESS && out_size) {
            *out_size = read_bytes;
        }
    }
    return status;
}

static efi_status read_kernel_file(efi_handle image_handle, void *dest_buf, uintptr_t max_size, uintptr_t *out_size) {
    efi_guid loaded_img_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    efi_guid fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_img = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    efi_status status;
    uintptr_t num_handles = 0;
    efi_handle *handles = NULL;
    uintptr_t h;

    /* 1. Try file system on the device from which this EFI image was loaded */
    status = g_st->BootServices->HandleProtocol(image_handle, &loaded_img_guid, (void**)&loaded_img);
    if (status == EFI_SUCCESS && loaded_img && loaded_img->DeviceHandle) {
        status = g_st->BootServices->HandleProtocol(loaded_img->DeviceHandle, &fs_guid, (void**)&fs);
        if (status == EFI_SUCCESS && fs) {
            status = try_open_file_on_fs(fs, dest_buf, max_size, out_size);
            if (status == EFI_SUCCESS) {
                return EFI_SUCCESS;
            }
        }
    }

    /* 2. Try default located file system */
    status = g_st->BootServices->LocateProtocol(&fs_guid, NULL, (void**)&fs);
    if (status == EFI_SUCCESS && fs) {
        status = try_open_file_on_fs(fs, dest_buf, max_size, out_size);
        if (status == EFI_SUCCESS) {
            return EFI_SUCCESS;
        }
    }

    /* 3. Search across ALL handles supporting SimpleFileSystem */
    status = g_st->BootServices->LocateHandleBuffer(ByProtocol, &fs_guid, NULL, &num_handles, &handles);
    if (status == EFI_SUCCESS && handles && num_handles > 0) {
        for (h = 0; h < num_handles; h++) {
            fs = NULL;
            status = g_st->BootServices->HandleProtocol(handles[h], &fs_guid, (void**)&fs);
            if (status == EFI_SUCCESS && fs) {
                status = try_open_file_on_fs(fs, dest_buf, max_size, out_size);
                if (status == EFI_SUCCESS) {
                    g_st->BootServices->FreePool(handles);
                    return EFI_SUCCESS;
                }
            }
        }
        g_st->BootServices->FreePool(handles);
    }

    return EFI_NOT_FOUND;
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

static uint8_t uefi_mmap_buf[UEFI_MMAP_BUF_SIZE];

static void *find_acpi_rsdp(EFI_SYSTEM_TABLE *st) {
    static const efi_guid acpi20_guid = { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };
    static const efi_guid acpi10_guid = { 0xeb9d2d30, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } };
    uintptr_t i;

    if (!st || !st->ConfigurationTable) return NULL;

    for (i = 0; i < st->NumberOfTableEntries; i++) {
        if (memcmp(&st->ConfigurationTable[i].VendorGuid, &acpi20_guid, sizeof(efi_guid)) == 0) {
            return st->ConfigurationTable[i].VendorTable;
        }
    }

    for (i = 0; i < st->NumberOfTableEntries; i++) {
        if (memcmp(&st->ConfigurationTable[i].VendorGuid, &acpi10_guid, sizeof(efi_guid)) == 0) {
            return st->ConfigurationTable[i].VendorTable;
        }
    }

    return NULL;
}

efi_status EFIAPI efi_main(efi_handle image_handle, EFI_SYSTEM_TABLE *system_table) {
    struct multiboot_info *mbi;
    uint8_t *kernel_temp = (uint8_t*)KERNEL_TEMP_ADDR;
    uintptr_t kernel_size = 0;
    uint32_t entry_point = 0;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    uintptr_t mmap_size = sizeof(uefi_mmap_buf);
    uintptr_t map_key = 0;
    uintptr_t desc_size = 0;
    uint32_t desc_ver = 0;
    void *rsdp = NULL;
    efi_status status;

    g_st = system_table;

    mbi = (struct multiboot_info*)MULTIBOOT_INFO_ADDR;
    memset(mbi, 0, sizeof(struct multiboot_info));

    mbi->boot_device = 0x8000FFFF;
    mbi->flags |= MULTIBOOT_INFO_BOOTDEV;
    strcpy((char*)(MULTIBOOT_INFO_ADDR + 256), "gemiboot 1.0 (UEFI 32-bit)");
    mbi->boot_loader_name = (uint32_t)(MULTIBOOT_INFO_ADDR + 256);
    mbi->flags |= MULTIBOOT_INFO_BOOT_LOADER_NAME;

    rsdp = find_acpi_rsdp(system_table);
    if (rsdp) {
        mbi->config_table = (uint32_t)(uintptr_t)rsdp;
        mbi->flags |= MULTIBOOT_INFO_CONFIG_TABLE;
    }

    /* 1. Setup GOP first so all messages render to screen */
    init_gop(&gop, mbi);

    uefi_puts("\n=======================================================\n");
    uefi_puts(" GEMIBOOT - Multiboot x86 Boot Loader (UEFI 32-bit)\n");
    uefi_puts("=======================================================\n");
    uefi_puts("[GEMIBOOT] Graphics Output Protocol (GOP) initialized.\n");

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

    /* 5-second pause before booting kernel */
    {
        int s;
        uefi_puts("[GEMIBOOT] Pausing for 5 seconds before booting kernel...\n");
        for (s = 5; s > 0; s--) {
            uefi_puts("[GEMIBOOT] Booting in ");
            uefi_put_dec(s);
            uefi_puts(" second(s)...\n");
            g_st->BootServices->Stall(1000000);
        }
    }

    /* 3. Exit Boot Services */
    uefi_puts("[GEMIBOOT] Exiting UEFI Boot Services and jumping to GEMIOS...\n");

    while (1) {
        mmap_size = sizeof(uefi_mmap_buf);
        status = g_st->BootServices->GetMemoryMap(&mmap_size, (EFI_MEMORY_DESCRIPTOR*)uefi_mmap_buf, &map_key, &desc_size, &desc_ver);
        if (status == EFI_SUCCESS) {
            convert_memory_map((EFI_MEMORY_DESCRIPTOR*)uefi_mmap_buf, mmap_size, desc_size, mbi);
            status = g_st->BootServices->ExitBootServices(image_handle, map_key);
            if (status == EFI_SUCCESS) {
                break;
            }
        }
    }

    /* 4. Switch GDT and jump to 32-bit kernel */
    load_gdt_and_jump(entry_point, MULTIBOOT_BOOTLOADER_MAGIC, MULTIBOOT_INFO_ADDR);

    while (1) { ; }
    return 0;
}
