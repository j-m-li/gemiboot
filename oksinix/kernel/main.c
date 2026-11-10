/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Main entry point
 */

#include "types.h"
#include "multiboot.h"
#include "io.h"
#include "string.h"
#include "serial.h"
#include "gdt.h"
#include "idt.h"
#include "acpi.h"
#include "pic.h"
#include "pit.h"
#include "ps2.h"
#include "xhci.h"
#include "vga.h"
#include "console.h"
#include "vfs.h"
#include "fat32.h"
#include "syscall.h"
#include "user.h"

static void parse_multiboot_info(uint32_t mbi_addr)
{
    const struct multiboot_info *mbi;

    if (mbi_addr == 0) {
        return;
    }
    mbi = (const struct multiboot_info *)(uintptr_t)mbi_addr;

    if ((mbi->flags & MULTIBOOT_INFO_BOOT_LOADER_NAME) != 0 && mbi->boot_loader_name != 0) {
        kprintf("[MULTIBOOT] Bootloader: \"%s\"\n",
                (const char *)(uintptr_t)mbi->boot_loader_name);
    }
    if ((mbi->flags & MULTIBOOT_INFO_MEMORY) != 0) {
        kprintf("[MULTIBOOT] Memory: lower=%u KiB, upper=%u KiB (%u MiB total)\n",
                mbi->mem_lower, mbi->mem_upper,
                (mbi->mem_upper + 1024U) / 1024U);
    }
    if ((mbi->flags & MULTIBOOT_INFO_CMDLINE) != 0 && mbi->cmdline != 0) {
        const char *cmd = (const char *)(uintptr_t)mbi->cmdline;
        kprintf("[MULTIBOOT] Command line: \"%s\"\n", cmd);
    }
}

static int run_vga_selftest(void)
{
    uint8_t p_bg, p_rect, p_vram_bg, p_vram_rect;

    vga_clear(COLOR_BLUE);
    vga_fill_rect(10, 10, 20, 20, COLOR_LIGHT_RED);
    vga_present();

    p_bg        = vga_getpixel(0, 0);
    p_rect      = vga_getpixel(15, 15);
    p_vram_bg   = vga_getpixel_vram(0, 0);
    p_vram_rect = vga_getpixel_vram(15, 15);

    if (p_bg != COLOR_BLUE || p_rect != COLOR_LIGHT_RED ||
        p_vram_bg != COLOR_BLUE || p_vram_rect != COLOR_LIGHT_RED) {
        kprintf("[TEST] VGA 640x480 framebuffer self-test FAILED (bg=%u/%u rect=%u/%u)\n",
                (uint32_t)p_bg, (uint32_t)p_vram_bg,
                (uint32_t)p_rect, (uint32_t)p_vram_rect);
        return -1;
    }

    kprintf("[TEST] VGA 640x480 framebuffer self-test PASSED (VRAM 0xA0000 4-plane verified)\n");
    return 0;
}

void kernel_main(uint32_t magic, uint32_t mbi_addr)
{
    char *init_argv[2];

    (void)mbi_addr;

    serial_init();
    kprintf("\n================================================\n");
    kprintf("Oksinix x86 32-bit Kernel (ISO C90, CC0 Public Domain)\n");
    kprintf("================================================\n");

    if (magic == MULTIBOOT_BOOTLOADER_MAGIC) {
        kprintf("[BOOT] Multiboot v1 magic verified (0x%08x, mbi=0x%08x)\n", magic, mbi_addr);
        parse_multiboot_info(mbi_addr);
    } else {
        kprintf("[BOOT] Warning: unexpected multiboot magic 0x%08x\n", magic);
    }

    gdt_init();
    kprintf("[CPU] GDT initialized (flat 32-bit segments)\n");

    idt_init();
    kprintf("[CPU] IDT & 8259 PIC initialized (IRQs mapped to 32..47)\n");

    acpi_init(magic, mbi_addr);
    syscall_init();

    pit_init();

    vga_init(mbi_addr);
    (void)run_vga_selftest();

    console_init();
    console_set_status("KERNEL: Probing PS/2 8042 controller...", 0);
    console_refresh();

    ps2_init();
    kbd_init();
    mouse_init(VGA_WIDTH, VGA_HEIGHT);

    console_set_status("KERNEL: Probing PCI xHCI USB 3.0 controller...", 0);
    console_refresh();

    (void)xhci_init();

    console_set_status("KERNEL: Mounting VFS & FAT32 filesystems...", 0);
    console_refresh();

    vfs_init();
    devfs_init();
    (void)fat32_init();
    user_programs_init();

    cpu_sti();
    kprintf("[KERNEL] Interrupts enabled. Launching /bin/init (PID 1)...\n");

    init_argv[0] = "/bin/init";
    init_argv[1] = NULL;
    (void)vfs_exec("/bin/init", 1, init_argv);

    kprintf("[KERNEL] /bin/init exited. System halted.\n");
    for (;;) {
        cpu_hlt();
    }
}
