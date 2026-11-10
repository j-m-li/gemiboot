/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - /dev Device Filesystem (devfs)
 */

#include "vfs.h"
#include "fat32.h"
#include "console.h"
#include "serial.h"
#include "ps2.h"
#include "vga.h"
#include "xhci.h"
#include "string.h"

/* /dev/null (1, 3) */
static int32_t dev_null_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buf)
{
    (void)node;
    (void)offset;
    (void)size;
    (void)buf;
    return 0; /* EOF */
}

static int32_t dev_null_write(struct vfs_node *node, uint32_t offset, uint32_t size, const uint8_t *buf)
{
    (void)node;
    (void)offset;
    (void)buf;
    return (int32_t)size;
}

/* /dev/zero (1, 5) */
static int32_t dev_zero_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buf)
{
    (void)node;
    (void)offset;
    memset(buf, 0, size);
    return (int32_t)size;
}

static int32_t dev_zero_write(struct vfs_node *node, uint32_t offset, uint32_t size, const uint8_t *buf)
{
    (void)node;
    (void)offset;
    (void)buf;
    return (int32_t)size;
}

/* /dev/console (5, 1) and /dev/tty0 (4, 0) */
static int32_t dev_console_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buf)
{
    char ch;
    (void)node;
    (void)offset;
    if (size == 0) {
        return 0;
    }
    if (console_poll_char(&ch)) {
        buf[0] = (uint8_t)ch;
        return 1;
    }
    return 0;
}

static int32_t dev_console_write(struct vfs_node *node, uint32_t offset, uint32_t size, const uint8_t *buf)
{
    (void)node;
    (void)offset;
    console_write((const char *)buf, size);
    return (int32_t)size;
}

/* /dev/ttyS0 (4, 64) - COM1 serial port */
static int32_t dev_ttys0_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buf)
{
    (void)node;
    (void)offset;
    if (size == 0 || !serial_received()) {
        return 0;
    }
    buf[0] = (uint8_t)serial_getc();
    return 1;
}

static int32_t dev_ttys0_write(struct vfs_node *node, uint32_t offset, uint32_t size, const uint8_t *buf)
{
    uint32_t i;
    (void)node;
    (void)offset;
    for (i = 0; i < size; i++) {
        serial_putc((char)buf[i]);
    }
    return (int32_t)size;
}

/* /dev/kbd (13, 64) - PS/2 Keyboard raw event / scancode reader */
static int32_t dev_kbd_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buf)
{
    struct key_event ev;
    (void)node;
    (void)offset;
    if (size < sizeof(struct key_event)) {
        if (size >= 1 && kbd_pop_event(&ev)) {
            buf[0] = ev.scancode;
            return 1;
        }
        return 0;
    }
    if (kbd_pop_event(&ev)) {
        memcpy(buf, &ev, sizeof(struct key_event));
        return (int32_t)sizeof(struct key_event);
    }
    return 0;
}

/* /dev/mouse (13, 63) - PS/2 Mouse state reader */
static int32_t dev_mouse_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buf)
{
    struct mouse_state ms;
    (void)node;
    (void)offset;
    mouse_get_state(&ms);
    if (size >= sizeof(struct mouse_state)) {
        memcpy(buf, &ms, sizeof(struct mouse_state));
        return (int32_t)sizeof(struct mouse_state);
    }
    if (size >= 3) {
        buf[0] = ms.buttons;
        buf[1] = (uint8_t)(ms.x & 0xFF);
        buf[2] = (uint8_t)(ms.y & 0xFF);
        return 3;
    }
    return 0;
}

/* /dev/fb0 (29, 0) - VGA 640x480 framebuffer device */
static int32_t dev_fb0_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buf)
{
    const uint8_t *fb = vga_get_backbuffer();
    uint32_t avail;
    (void)node;

    if (offset >= VGA_FB_SIZE) {
        return 0;
    }
    avail = VGA_FB_SIZE - offset;
    if (size > avail) {
        size = avail;
    }
    memcpy(buf, &fb[offset], size);
    return (int32_t)size;
}

static int32_t dev_fb0_write(struct vfs_node *node, uint32_t offset, uint32_t size, const uint8_t *buf)
{
    uint8_t *fb = vga_get_backbuffer();
    uint32_t avail;
    (void)node;

    if (offset >= VGA_FB_SIZE) {
        return 0;
    }
    avail = VGA_FB_SIZE - offset;
    if (size > avail) {
        size = avail;
    }
    memcpy(&fb[offset], buf, size);
    vga_present();
    return (int32_t)size;
}

/* /dev/sd[a-d] (8, 0/16/32/48) - xHCI USB Mass Storage block devices */
static int32_t dev_sd_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buf)
{
    int disk_idx = (node != NULL) ? (int)(node->rdev_minor / 16U) : 0;
    return xhci_msc_read_bytes_idx(disk_idx, offset, size, buf);
}

static int32_t dev_sd_write(struct vfs_node *node, uint32_t offset, uint32_t size, const uint8_t *buf)
{
    int disk_idx = (node != NULL) ? (int)(node->rdev_minor / 16U) : 0;
    return xhci_msc_write_bytes_idx(disk_idx, offset, size, buf);
}

static int devfs_register_usb_disks(void)
{
    int msc_count = xhci_get_msc_count();
    int i;
    int added = 0;

    for (i = 0; i < msc_count; i++) {
        struct xhci_msc_info msc;
        char dev_path[16];
        uint16_t minor = (uint16_t)(i * 16);

        memcpy(dev_path, "/dev/sda", 9);
        dev_path[7] = (char)('a' + i);
        if (vfs_lookup(dev_path) != NULL) {
            continue;
        }
        if (xhci_get_msc_info_idx(i, &msc) == 0 && msc.present) {
            (void)vfs_register_blockdev(dev_path, 8, minor,
                                        msc.block_count * msc.block_size,
                                        dev_sd_read, dev_sd_write);
            kprintf("[DEVFS] Registered block device %s (8,%u) size=%u bytes\n",
                    dev_path, (uint32_t)minor, msc.block_count * msc.block_size);
            added = 1;
        }
    }
    return added;
}

void devfs_sync_usb_storage(void)
{
    if (devfs_register_usb_disks() > 0 && !fat32_is_mounted()) {
        (void)fat32_init();
    }
}

void devfs_init(void)
{
    struct vfs_node *fb_node;
    int msc_count;

    (void)vfs_register_chardev("/dev/null",    1,  3,  dev_null_read,    dev_null_write);
    (void)vfs_register_chardev("/dev/zero",    1,  5,  dev_zero_read,    dev_zero_write);
    (void)vfs_register_chardev("/dev/console", 5,  1,  dev_console_read, dev_console_write);
    (void)vfs_register_chardev("/dev/tty0",    4,  0,  dev_console_read, dev_console_write);
    (void)vfs_register_chardev("/dev/ttyS0",   4,  64, dev_ttys0_read,   dev_ttys0_write);
    (void)vfs_register_chardev("/dev/kbd",     13, 64, dev_kbd_read,     NULL);
    (void)vfs_register_chardev("/dev/mouse",   13, 63, dev_mouse_read,   NULL);
    (void)vfs_register_chardev("/dev/fb0",     29, 0,  dev_fb0_read,     dev_fb0_write);

    fb_node = vfs_lookup("/dev/fb0");
    if (fb_node != NULL) {
        fb_node->size = VGA_FB_SIZE;
    }

    (void)devfs_register_usb_disks();
    msc_count = xhci_get_msc_count();

    kprintf("[DEVFS] Mounted /dev (null, zero, console, tty0, ttyS0, kbd, mouse, fb0%s)\n",
            (msc_count > 0) ? ", sd*" : "");
}
