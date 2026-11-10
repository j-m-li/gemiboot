/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - /bin/sh (Interactive Bourne-style minimal shell)
 */

#include "user.h"
#include "acpi.h"
#include "vfs.h"
#include "fat32.h"
#include "xhci.h"
#include "console.h"
#include "pit.h"
#include "ps2.h"
#include "io.h"
#include "string.h"
#include "serial.h"

#define SH_LINE_MAX 128
#define SH_MAX_ARGS 16

static char cwd[VFS_MAX_PATH] = "/";

static void sh_puts(const char *s)
{
    (void)vfs_write(1, s, (uint32_t)strlen(s));
}

static void sh_putc(char c)
{
    (void)vfs_write(1, &c, 1);
}

static void sh_print_uint(uint32_t val)
{
    char buf[16];
    int i = 0;
    if (val == 0) {
        sh_putc('0');
        return;
    }
    while (val > 0) {
        buf[i++] = (char)('0' + (val % 10U));
        val /= 10U;
    }
    while (i > 0) {
        sh_putc(buf[--i]);
    }
}

static void sh_print_hex32(uint32_t val)
{
    const char *hex = "0123456789ABCDEF";
    int shift;
    sh_puts("0x");
    for (shift = 28; shift >= 0; shift -= 4) {
        sh_putc(hex[(val >> shift) & 0x0FU]);
    }
}

static void cmd_memmap(void)
{
    int i;
    int tcnt = acpi_get_table_count();
    int mcnt = acpi_get_mmap_count();

    if (acpi_has_rsdp()) {
        sh_puts("ACPI RSDP: ");
        sh_print_hex32(acpi_get_rsdp_addr());
        sh_puts(" RSDT: ");
        sh_print_hex32(acpi_get_rsdt_addr());
        sh_puts(" Tables:");
        for (i = 0; i < tcnt; i++) {
            struct acpi_table_desc td;
            if (acpi_get_table_desc(i, &td) == 0) {
                sh_putc(' ');
                sh_puts(td.signature);
            }
        }
        sh_putc('\n');
    }

    for (i = 0; i < mcnt; i++) {
        struct acpi_mmap_entry me;
        if (acpi_get_mmap_entry(i, &me) == 0) {
            uint32_t end = (me.length_lo > 0) ? (me.base_lo + me.length_lo - 1U) : me.base_lo;
            sh_puts("  ");
            sh_print_hex32(me.base_lo);
            sh_puts(" - ");
            sh_print_hex32(end);
            sh_puts(" (");
            sh_print_uint(me.length_lo / 1024U);
            sh_puts(" KiB) [");
            sh_puts(acpi_mmap_type_name(me.type));
            sh_puts("]\n");
        }
    }
    sh_puts("Total Usable RAM: ");
    sh_print_uint(acpi_get_total_usable_ram() / (1024U * 1024U));
    sh_puts(" MiB\n");
}

static void make_abs_path(const char *input, char *out, size_t out_sz)
{
    size_t i = 0;
    size_t j = 0;

    if (input == NULL || input[0] == '\0') {
        while (cwd[i] != '\0' && i + 1 < out_sz) {
            out[i] = cwd[i];
            i++;
        }
        out[i] = '\0';
        return;
    }

    if (input[0] == '/') {
        while (input[i] != '\0' && i + 1 < out_sz) {
            out[i] = input[i];
            i++;
        }
        out[i] = '\0';
        return;
    }

    while (cwd[i] != '\0' && i + 1 < out_sz) {
        out[i] = cwd[i];
        i++;
    }
    if (i > 0 && out[i - 1] != '/' && i + 1 < out_sz) {
        out[i++] = '/';
    }
    while (input[j] != '\0' && i + 1 < out_sz) {
        out[i++] = input[j++];
    }
    out[i] = '\0';
}

static int tokenize_line(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *p = line;

    while (*p != '\0' && argc < max_args - 1) {
        while (*p == ' ' || *p == '\t') {
            *p++ = '\0';
        }
        if (*p == '\0') {
            break;
        }
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
    }
    argv[argc] = NULL;
    return argc;
}

static void cmd_ls(int argc, char **argv)
{
    char path[VFS_MAX_PATH];
    struct vfs_dirent de;
    struct vfs_node *node;
    uint32_t idx = 0;

    make_abs_path((argc > 1) ? argv[1] : cwd, path, sizeof(path));
    node = vfs_lookup(path);
    if (node == NULL) {
        sh_puts("ls: cannot access ");
        sh_puts(path);
        sh_puts(": No such file or directory\n");
        return;
    }

    if (node->type != VFS_TYPE_DIR) {
        sh_puts(node->name);
        sh_putc('\n');
        return;
    }

    while (vfs_readdir(path, idx, &de) == 0) {
        if (de.type == VFS_TYPE_DIR) {
            sh_puts("drwxr-xr-x  ");
            sh_puts(de.name);
            sh_puts("/\n");
        } else if (de.type == VFS_TYPE_CHARDEV) {
            sh_puts("crw-rw-rw-  ");
            sh_puts(de.name);
            sh_puts(" (");
            sh_print_uint(de.rdev_major);
            sh_puts(", ");
            sh_print_uint(de.rdev_minor);
            sh_puts(")\n");
        } else if (de.type == VFS_TYPE_BLOCKDEV) {
            sh_puts("brw-rw----  ");
            sh_puts(de.name);
            sh_puts(" (");
            sh_print_uint(de.rdev_major);
            sh_puts(", ");
            sh_print_uint(de.rdev_minor);
            sh_puts(") ");
            sh_print_uint(de.size);
            sh_puts(" B\n");
        } else {
            sh_puts("-rw-r--r--  ");
            sh_puts(de.name);
            sh_puts(" (");
            sh_print_uint(de.size);
            sh_puts(" B)\n");
        }
        idx++;
    }
}

static void cmd_cat(int argc, char **argv)
{
    char path[VFS_MAX_PATH];
    uint8_t buf[64];
    int fd;
    int32_t n;

    if (argc < 2) {
        sh_puts("usage: cat <path>\n");
        return;
    }

    make_abs_path(argv[1], path, sizeof(path));
    fd = vfs_open(path, VFS_O_RDONLY);
    if (fd < 0) {
        sh_puts("cat: ");
        sh_puts(path);
        sh_puts(": No such file or device\n");
        return;
    }

    while ((n = vfs_read(fd, buf, sizeof(buf))) > 0) {
        int32_t i;
        for (i = 0; i < n; i++) {
            char c = (char)buf[i];
            if ((c >= 32 && c <= 126) || c == '\n' || c == '\t') {
                sh_putc(c);
            } else {
                sh_putc('.');
            }
        }
        /* Avoid infinite read on endless stream devices like /dev/zero */
        if (strcmp(path, "/dev/zero") == 0 ||
            strcmp(path, "/dev/mouse") == 0 ||
            strcmp(path, "/dev/fb0") == 0 ||
            strncmp(path, "/dev/sd", 7) == 0) {
            sh_putc('\n');
            break;
        }
    }
    (void)vfs_close(fd);
}

static void cmd_cp(int argc, char **argv)
{
    char src_path[VFS_MAX_PATH];
    char dst_path[VFS_MAX_PATH];
    struct vfs_node *src_node;
    struct vfs_node *dst_node;
    uint8_t buf[128];
    int src_fd;
    int dst_fd;
    int32_t n;

    if (argc < 3) {
        sh_puts("usage: cp <src> <dst>\n");
        return;
    }

    make_abs_path(argv[1], src_path, sizeof(src_path));
    make_abs_path(argv[2], dst_path, sizeof(dst_path));

    src_node = vfs_lookup(src_path);
    if (src_node == NULL) {
        sh_puts("cp: cannot stat ");
        sh_puts(src_path);
        sh_puts(": No such file or directory\n");
        return;
    }
    if (src_node->type == VFS_TYPE_DIR) {
        sh_puts("cp: omitting directory ");
        sh_puts(src_path);
        sh_putc('\n');
        return;
    }

    dst_node = vfs_lookup(dst_path);
    if (dst_node != NULL && dst_node->type == VFS_TYPE_DIR) {
        const char *base = src_path;
        const char *p = src_path;
        size_t dlen = strlen(dst_path);
        size_t blen;

        while (*p != '\0') {
            if (*p == '/' && *(p + 1) != '\0') {
                base = p + 1;
            }
            p++;
        }
        blen = strlen(base);
        if (dlen > 0 && dst_path[dlen - 1] != '/' && dlen + 1 < sizeof(dst_path)) {
            dst_path[dlen++] = '/';
            dst_path[dlen] = '\0';
        }
        if (dlen + blen < sizeof(dst_path)) {
            memcpy(&dst_path[dlen], base, blen + 1);
        }
    }

    if (strcmp(src_path, dst_path) == 0) {
        sh_puts("cp: ");
        sh_puts(src_path);
        sh_puts(" and ");
        sh_puts(dst_path);
        sh_puts(" are the same file\n");
        return;
    }

    src_fd = vfs_open(src_path, VFS_O_RDONLY);
    if (src_fd < 0) {
        sh_puts("cp: cannot open ");
        sh_puts(src_path);
        sh_putc('\n');
        return;
    }

    dst_fd = vfs_open(dst_path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (dst_fd < 0) {
        (void)vfs_close(src_fd);
        sh_puts("cp: cannot create ");
        sh_puts(dst_path);
        sh_putc('\n');
        return;
    }

    while ((n = vfs_read(src_fd, buf, sizeof(buf))) > 0) {
        if (vfs_write(dst_fd, buf, (uint32_t)n) != n) {
            sh_puts("cp: write error on ");
            sh_puts(dst_path);
            sh_putc('\n');
            break;
        }
        if (strcmp(src_path, "/dev/zero") == 0 ||
            strcmp(src_path, "/dev/mouse") == 0 ||
            strcmp(src_path, "/dev/fb0") == 0 ||
            strncmp(src_path, "/dev/sd", 7) == 0) {
            break;
        }
    }

    (void)vfs_close(dst_fd);
    (void)vfs_close(src_fd);
}

static void cmd_echo(int argc, char **argv)
{
    int i;
    int redirect_idx = -1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], ">") == 0 && i + 1 < argc) {
            redirect_idx = i;
            break;
        }
    }

    if (redirect_idx > 0) {
        char path[VFS_MAX_PATH];
        int fd;
        make_abs_path(argv[redirect_idx + 1], path, sizeof(path));
        fd = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
        if (fd < 0) {
            sh_puts("echo: cannot open ");
            sh_puts(path);
            sh_putc('\n');
            return;
        }
        for (i = 1; i < redirect_idx; i++) {
            if (i > 1) {
                (void)vfs_write(fd, " ", 1);
            }
            (void)vfs_write(fd, argv[i], (uint32_t)strlen(argv[i]));
        }
        (void)vfs_write(fd, "\n", 1);
        (void)vfs_close(fd);
    } else {
        for (i = 1; i < argc; i++) {
            if (i > 1) {
                sh_putc(' ');
            }
            sh_puts(argv[i]);
        }
        sh_putc('\n');
    }
}

static void cmd_stat(int argc, char **argv)
{
    char path[VFS_MAX_PATH];
    struct vfs_node *node;

    if (argc < 2) {
        sh_puts("usage: stat <path>\n");
        return;
    }
    make_abs_path(argv[1], path, sizeof(path));
    node = vfs_lookup(path);
    if (node == NULL) {
        sh_puts("stat: cannot stat ");
        sh_puts(path);
        sh_puts(": No such file or directory\n");
        return;
    }

    sh_puts("  File: ");
    sh_puts(path);
    sh_puts("\n  Inode: ");
    sh_print_uint(node->inode);
    sh_puts("  Type: ");
    if (node->type == VFS_TYPE_DIR) {
        sh_puts("directory\n");
    } else if (node->type == VFS_TYPE_CHARDEV) {
        sh_puts("character device (");
        sh_print_uint(node->rdev_major);
        sh_puts(",");
        sh_print_uint(node->rdev_minor);
        sh_puts(")\n");
    } else if (node->type == VFS_TYPE_BLOCKDEV) {
        sh_puts("block device (");
        sh_print_uint(node->rdev_major);
        sh_puts(",");
        sh_print_uint(node->rdev_minor);
        sh_puts(")  Size: ");
        sh_print_uint(node->size);
        sh_puts(" bytes\n");
    } else {
        sh_puts("regular file  Size: ");
        sh_print_uint(node->size);
        sh_puts(" bytes\n");
    }
}

static void cmd_cd(int argc, char **argv)
{
    char path[VFS_MAX_PATH];
    struct vfs_node *node;

    make_abs_path((argc > 1) ? argv[1] : "/", path, sizeof(path));
    node = vfs_lookup(path);
    if (node == NULL || node->type != VFS_TYPE_DIR) {
        sh_puts("cd: not a directory: ");
        sh_puts(path);
        sh_putc('\n');
        return;
    }
    /* Normalize root or directory path into cwd */
    if (node->parent_idx < 0) {
        cwd[0] = '/';
        cwd[1] = '\0';
    } else {
        size_t len = strlen(path);
        while (len > 1 && path[len - 1] == '/') {
            path[--len] = '\0';
        }
        memcpy(cwd, path, len + 1);
    }
}

static void sh_execute_line(char *line)
{
    char *argv[SH_MAX_ARGS];
    int argc = tokenize_line(line, argv, SH_MAX_ARGS);

    if (argc == 0) {
        return;
    }

    if (strcmp(argv[0], "help") == 0) {
        sh_puts("Built-in commands: help ls cd pwd cat cp echo stat mkdir mount lsusb memmap acpi keymap ps uname uptime clear vi\n");
    } else if (strcmp(argv[0], "memmap") == 0 || strcmp(argv[0], "acpi") == 0) {
        cmd_memmap();
    } else if (strcmp(argv[0], "cp") == 0) {
        cmd_cp(argc, argv);
    } else if (strcmp(argv[0], "keymap") == 0 || strcmp(argv[0], "loadkeys") == 0) {
        if (argc < 2) {
            sh_puts("Current keymap: ");
            sh_puts(kbd_get_keymap_name());
            if (kbd_get_keymap() == KBD_KEYMAP_FR_CH) {
                sh_puts(" (Swiss French QWERTZ)\n");
            } else {
                sh_puts(" (US QWERTY)\n");
            }
        } else if (strcmp(argv[1], "fr_CH") == 0 || strcmp(argv[1], "ch_fr") == 0 ||
                   strcmp(argv[1], "ch") == 0 || strcmp(argv[1], "swiss") == 0) {
            int fd;
            kbd_set_keymap(KBD_KEYMAP_FR_CH);
            fd = vfs_open("/etc/keymap", VFS_O_WRONLY | VFS_O_TRUNC);
            if (fd >= 0) {
                (void)vfs_write(fd, "fr_CH\n", 6);
                (void)vfs_close(fd);
            }
            sh_puts("Keymap switched to fr_CH (Swiss French QWERTZ)\n");
        } else if (strcmp(argv[1], "us") == 0) {
            int fd;
            kbd_set_keymap(KBD_KEYMAP_US);
            fd = vfs_open("/etc/keymap", VFS_O_WRONLY | VFS_O_TRUNC);
            if (fd >= 0) {
                (void)vfs_write(fd, "us\n", 3);
                (void)vfs_close(fd);
            }
            sh_puts("Keymap switched to us (US QWERTY)\n");
        } else {
            sh_puts("usage: keymap [fr_CH|us]\n");
        }
    } else if (strcmp(argv[0], "uptime") == 0) {
        uint32_t ticks = pit_get_ticks();
        sh_puts("up ");
        sh_print_uint(ticks / 1000U);
        sh_puts(".");
        sh_print_uint((ticks % 1000U) / 100U);
        sh_puts("s (");
        sh_print_uint(ticks);
        sh_puts(" PIT ticks @ 1000 Hz)\n");
    } else if (strcmp(argv[0], "mount") == 0) {
        sh_puts("rootfs on / type ramfs (rw)\n");
        sh_puts("devfs on /dev type devfs (rw)\n");
        if (fat32_is_mounted()) {
            struct fat32_fs_info fi;
            char dev_part[16];
            fat32_get_info(&fi);
            memcpy(dev_part, "/dev/sda1", 10);
            dev_part[7] = (char)('a' + fi.disk_idx);
            sh_puts(dev_part);
            sh_puts(" on /mnt type fat32 (rw, label=");
            sh_puts(fi.volume_label);
            sh_puts(", lba=");
            sh_print_uint(fi.part_lba);
            sh_puts(", clusters=");
            sh_print_uint(fi.total_clusters);
            sh_puts(")\n");
        }
    } else if (strcmp(argv[0], "lsusb") == 0) {
        if (!xhci_is_initialized()) {
            sh_puts("No xHCI controller found.\n");
        } else {
            struct xhci_controller_info ci;
            struct xhci_hub_info hub;
            int msc_cnt;
            int i;
            devfs_sync_usb_storage();
            msc_cnt = xhci_get_msc_count();
            xhci_get_controller_info(&ci);
            xhci_get_hub_info(&hub);
            sh_puts("Bus 001: xHCI USB 3.0 Host Controller (ports=");
            sh_print_uint(ci.max_ports);
            sh_puts(", devices=");
            sh_print_uint(ci.num_devices);
            sh_puts(")\n");
            if (hub.present) {
                sh_puts("  - USB Hub (slot ");
                sh_print_uint(hub.slot_id);
                sh_puts(", ");
                sh_print_uint(hub.num_ports);
                sh_puts(" ports, ");
                sh_print_uint(hub.downstream_devices);
                sh_puts(" downstream)\n");
            }
            if (xhci_has_usb_kbd()) {
                sh_puts("  - USB HID Boot Keyboard (/dev/kbd)\n");
            }
            if (xhci_has_usb_mouse()) {
                sh_puts("  - USB HID Boot Mouse (/dev/mouse)\n");
            }
            for (i = 0; i < msc_cnt; i++) {
                struct xhci_msc_info msc;
                if (xhci_get_msc_info_idx(i, &msc) == 0 && msc.present) {
                    char sd_name[12];
                    memcpy(sd_name, "/dev/sda", 9);
                    sd_name[7] = (char)('a' + i);
                    sh_puts("  - USB Mass Storage (BOT/SCSI): ");
                    sh_puts(msc.vendor);
                    sh_putc(' ');
                    sh_puts(msc.product);
                    sh_puts(" -> ");
                    sh_puts(sd_name);
                    sh_puts(" (");
                    sh_print_uint(msc.block_count * msc.block_size);
                    sh_puts(" B)\n");
                }
            }
        }
    } else if (strcmp(argv[0], "ls") == 0) {
        cmd_ls(argc, argv);
    } else if (strcmp(argv[0], "cd") == 0) {
        cmd_cd(argc, argv);
    } else if (strcmp(argv[0], "pwd") == 0) {
        sh_puts(cwd);
        sh_putc('\n');
    } else if (strcmp(argv[0], "cat") == 0) {
        cmd_cat(argc, argv);
    } else if (strcmp(argv[0], "echo") == 0) {
        cmd_echo(argc, argv);
    } else if (strcmp(argv[0], "stat") == 0) {
        cmd_stat(argc, argv);
    } else if (strcmp(argv[0], "mkdir") == 0) {
        if (argc < 2) {
            sh_puts("usage: mkdir <path>\n");
        } else {
            char path[VFS_MAX_PATH];
            make_abs_path(argv[1], path, sizeof(path));
            if (vfs_mkdir(path) < 0) {
                sh_puts("mkdir: failed to create directory\n");
            }
        }
    } else if (strcmp(argv[0], "ps") == 0) {
        sh_puts("  PID TTY          STAT CMD\n");
        sh_puts("    1 /dev/console S    /bin/init\n");
        sh_puts("    2 /dev/console R    /bin/sh\n");
    } else if (strcmp(argv[0], "uname") == 0) {
        sh_puts("Oksinix 0.2 i386 C90 (CC0 Public Domain)\n");
    } else if (strcmp(argv[0], "clear") == 0) {
        console_clear();
    } else if (strcmp(argv[0], "vi") == 0 || strcmp(argv[0], "/bin/vi") == 0) {
        char path[VFS_MAX_PATH];
        char *vi_argv[3];
        int vi_argc = 1;
        vi_argv[0] = "/bin/vi";
        if (argc > 1) {
            make_abs_path(argv[1], path, sizeof(path));
            vi_argv[1] = path;
            vi_argv[2] = NULL;
            vi_argc = 2;
        } else {
            vi_argv[1] = NULL;
        }
        (void)vfs_exec("/bin/vi", vi_argc, vi_argv);
    } else {
        char path[VFS_MAX_PATH];
        struct vfs_node *node;
        make_abs_path(argv[0], path, sizeof(path));
        node = vfs_lookup(path);
        if (node != NULL && (node->exec_op != NULL || node->type == VFS_TYPE_FILE)) {
            char *orig_arg0 = argv[0];
            argv[0] = path;
            if (vfs_exec(path, argc, argv) < 0 && node->exec_op == NULL) {
                sh_puts("sh: cannot execute binary: ");
                sh_puts(orig_arg0);
                sh_putc('\n');
            }
        } else {
            sh_puts("sh: unknown command: ");
            sh_puts(argv[0]);
            sh_putc('\n');
        }
    }
}

static void sh_print_prompt(void)
{
    sh_puts("oksinix:");
    sh_puts(cwd);
    sh_puts("# ");
    console_refresh();
}

int bin_sh_main(int argc, char **argv)
{
    char line[SH_LINE_MAX];
    uint32_t line_len = 0;
    struct mouse_state last_mouse;

    if (argc > 2 && strcmp(argv[1], "-c") == 0) {
        char cmd_copy[SH_LINE_MAX];
        size_t len = strlen(argv[2]);
        if (len >= SH_LINE_MAX) {
            len = SH_LINE_MAX - 1U;
        }
        memcpy(cmd_copy, argv[2], len);
        cmd_copy[len] = '\0';
        sh_execute_line(cmd_copy);
        console_refresh();
        return 0;
    }

    sh_puts("[/bin/sh] Interactive shell ready. Type 'help' for commands.\n");
    mouse_get_state(&last_mouse);
    sh_print_prompt();

    for (;;) {
        uint8_t ch;
        struct mouse_state cur_mouse;
        int need_redraw = 0;

        while (vfs_read(0, &ch, 1) == 1) {
            if (ch == '\n') {
                sh_putc('\n');
                line[line_len] = '\0';
                kprintf("[SH] exec=\"%s\"\n", line);
                sh_execute_line(line);
                line_len = 0;
                sh_print_prompt();
            } else if (ch == '\b' || ch == 127) {
                if (line_len > 0) {
                    line_len--;
                    sh_putc('\b');
                    need_redraw = 1;
                }
            } else if (ch >= 32 && ch <= 126 && line_len + 1U < SH_LINE_MAX) {
                line[line_len++] = (char)ch;
                sh_putc((char)ch);
                need_redraw = 1;
            }
        }

        mouse_get_state(&cur_mouse);
        if (cur_mouse.x != last_mouse.x ||
            cur_mouse.y != last_mouse.y ||
            cur_mouse.buttons != last_mouse.buttons ||
            cur_mouse.packets != last_mouse.packets) {
            last_mouse = cur_mouse;
            need_redraw = 1;
        }

        if (need_redraw) {
            console_refresh();
        }

        if (xhci_is_initialized()) {
            xhci_poll();
            devfs_sync_usb_storage();
            io_wait();
        } else {
            cpu_hlt();
        }
    }
}
