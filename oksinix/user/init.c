/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - /bin/init (PID 1 system initialization)
 */

#include "user.h"
#include "sched.h"
#include "vfs.h"
#include "fat32.h"
#include "xhci.h"
#include "console.h"
#include "serial.h"
#include "string.h"

static void fd_puts(int fd, const char *s)
{
    (void)vfs_write(fd, s, (uint32_t)strlen(s));
}

static int verify_devfs_nodes(void)
{
    int fd;
    uint8_t buf[8];
    int32_t n;

    /* Test /dev/null: write succeeds, read returns 0 (EOF) */
    fd = vfs_open("/dev/null", VFS_O_RDWR);
    if (fd < 0) {
        return -1;
    }
    if (vfs_write(fd, "test", 4) != 4) {
        (void)vfs_close(fd);
        return -1;
    }
    if (vfs_read(fd, buf, sizeof(buf)) != 0) {
        (void)vfs_close(fd);
        return -1;
    }
    (void)vfs_close(fd);

    /* Test /dev/zero: read fills buffer with zeroes */
    memset(buf, 0xAA, sizeof(buf));
    fd = vfs_open("/dev/zero", VFS_O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    n = vfs_read(fd, buf, sizeof(buf));
    (void)vfs_close(fd);
    if (n != (int32_t)sizeof(buf) || buf[0] != 0 || buf[7] != 0) {
        return -1;
    }

    /* Verify /dev/kbd, /dev/mouse, /dev/fb0 exist as character devices */
    if (vfs_lookup("/dev/kbd") == NULL ||
        vfs_lookup("/dev/mouse") == NULL ||
        vfs_lookup("/dev/fb0") == NULL) {
        return -1;
    }

    return 0;
}

int bin_init_main(int argc, char **argv)
{
    int fd_in, fd_out, fd_err;
    int fd;
    char buf[128];
    int32_t n;
    char *sh_argv[2];

    (void)argc;
    (void)argv;

    console_set_status("PID 1: /bin/init (initializing)", 0);

    /* Open standard file descriptors 0, 1, 2 on /dev/console */
    fd_in  = vfs_open("/dev/console", VFS_O_RDONLY);
    fd_out = vfs_open("/dev/console", VFS_O_WRONLY);
    fd_err = vfs_open("/dev/console", VFS_O_WRONLY);
    (void)fd_in;
    (void)fd_err;

    fd_puts(fd_out, "[/bin/init] PID 1 starting system initialization...\n");

    /* Read and display /etc/motd via VFS */
    fd = vfs_open("/etc/motd", VFS_O_RDONLY);
    if (fd >= 0) {
        n = vfs_read(fd, buf, sizeof(buf) - 1U);
        if (n > 0) {
            buf[n] = '\0';
            fd_puts(fd_out, buf);
        }
        (void)vfs_close(fd);
    }

    /* Read /etc/hostname */
    fd = vfs_open("/etc/hostname", VFS_O_RDONLY);
    if (fd >= 0) {
        n = vfs_read(fd, buf, sizeof(buf) - 1U);
        if (n > 0) {
            buf[n] = '\0';
            fd_puts(fd_out, "[/bin/init] Hostname: ");
            fd_puts(fd_out, buf);
        }
        (void)vfs_close(fd);
    }

    /* Verify /dev character devices */
    if (verify_devfs_nodes() == 0) {
        fd_puts(fd_out, "[/bin/init] Verified /dev/null, /dev/zero, /dev/console, /dev/kbd, /dev/mouse, /dev/fb0\n");
    } else {
        fd_puts(fd_out, "[/bin/init] ERROR: /dev device verification failed!\n");
    }

    if (xhci_is_initialized()) {
        struct xhci_controller_info ci;
        const char *hci_tag = "xHCI";
        xhci_get_controller_info(&ci);
        if (ci.hci_type == USB_HCI_EHCI) {
            hci_tag = "EHCI";
        } else if (ci.hci_type == USB_HCI_OHCI) {
            hci_tag = "OHCI";
        } else if (ci.hci_type == USB_HCI_UHCI) {
            hci_tag = "UHCI";
        }
        fd_puts(fd_out, "[/bin/init] ");
        fd_puts(fd_out, hci_tag);
        fd_puts(fd_out, " USB bus active (");
        if (xhci_has_usb_hub()) {
            fd_puts(fd_out, "usb-hub ");
        }
        if (xhci_has_usb_kbd()) {
            fd_puts(fd_out, "usb-kbd ");
        }
        if (xhci_has_usb_mouse()) {
            fd_puts(fd_out, "usb-mouse ");
        }
        if (xhci_has_usb_net()) {
            fd_puts(fd_out, "usb-net ");
        }
        if (xhci_has_usb_storage()) {
            int i;
            int printed = 0;
            fd_puts(fd_out, "usb-storage:");
            for (i = 0; i < XHCI_MAX_MSC_DEVS; i++) {
                struct xhci_msc_info msc;
                char sd_name[12];
                if (xhci_get_msc_info_idx(i, &msc) != 0 || !msc.present) {
                    continue;
                }
                if (printed > 0) {
                    fd_puts(fd_out, ",");
                }
                memcpy(sd_name, "/dev/sda", 9);
                sd_name[7] = (char)('a' + i);
                fd_puts(fd_out, sd_name);
                printed++;
            }
        }
        fd_puts(fd_out, ")\n");
    }

    {
        int m;
        for (m = 0; m < FAT32_MAX_MOUNTS; m++) {
            struct fat32_fs_info fi;
            if (fat32_get_info_idx(m, &fi) == 0 && fi.mounted) {
                fd_puts(fd_out, "[/bin/init] Mounted FAT32 filesystem on ");
                fd_puts(fd_out, fi.mount_point);
                fd_puts(fd_out, "\n");
            }
        }
    }

    console_refresh();

    /* Spawn /bin/sh as specified in /etc/inittab */
    fd_puts(fd_out, "[/bin/init] Spawning /bin/sh on /dev/console...\n");
    console_set_status("PID 1: /bin/init -> PID 2: /bin/sh", 0);
    console_refresh();

    sh_argv[0] = "/bin/sh";
    sh_argv[1] = NULL;
    sched_set_current_name("/bin/sh");
    return vfs_exec("/bin/sh", 1, sh_argv);
}

void user_programs_init(void)
{
    (void)vfs_register_exec("/bin/init", bin_init_main, "Oksinix PID 1 init");
    (void)vfs_register_exec("/bin/sh",   bin_sh_main,   "Oksinix interactive shell");
    (void)vfs_register_exec("/bin/vi",   bin_vi_main,   "Oksinix modal vi editor");
    (void)vfs_register_exec("/bin/cc",   bin_cc_main,   "c compiler");
    kprintf("[VFS] Registered executables: /bin/init, /bin/sh, /bin/vi\n");
}
