/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - PS/2 Mouse driver (IRQ12, 3-byte standard packet)
 */

#include "ps2.h"
#include "idt.h"
#include "pic.h"
#include "io.h"
#include "serial.h"

static struct mouse_state current_mouse;
static int32_t            screen_w = 640;
static int32_t            screen_h = 480;
static uint8_t            pkt_buf[3];
static uint8_t            pkt_cycle = 0;

static int mouse_write(uint8_t cmd)
{
    if (ps2_wait_write() < 0) {
        return -1;
    }
    outb(PS2_CMD_PORT, 0xD4);
    if (ps2_wait_write() < 0) {
        return -1;
    }
    outb(PS2_DATA_PORT, cmd);
    if (ps2_wait_read() < 0) {
        return -1;
    }
    return (inb(PS2_DATA_PORT) == 0xFAU) ? 0 : -1;
}

void mouse_inject_byte(uint8_t data)
{
    switch (pkt_cycle) {
    case 0:
        /* Bit 3 of byte 0 must always be 1 in standard PS/2 mouse packets */
        if ((data & 0x08U) == 0) {
            return;
        }
        pkt_buf[0] = data;
        pkt_cycle = 1;
        break;

    case 1:
        pkt_buf[1] = data;
        pkt_cycle = 2;
        break;

    case 2: {
        int32_t rel_x;
        int32_t rel_y;
        uint8_t flags;

        pkt_buf[2] = data;
        pkt_cycle = 0;

        flags = pkt_buf[0];

        /* Ignore packet if X or Y overflow bits (bits 6, 7) are set */
        if ((flags & 0xC0U) != 0) {
            break;
        }

        rel_x = (int32_t)pkt_buf[1];
        rel_y = (int32_t)pkt_buf[2];

        if ((flags & 0x10U) != 0) {
            rel_x |= -256; /* Sign-extend 9-bit X delta */
        }
        if ((flags & 0x20U) != 0) {
            rel_y |= -256; /* Sign-extend 9-bit Y delta */
        }

        current_mouse.dx = rel_x;
        current_mouse.dy = -rel_y; /* PS/2 Y is positive upward; screen Y is downward */
        current_mouse.x += current_mouse.dx;
        current_mouse.y += current_mouse.dy;

        if (current_mouse.x < 0) {
            current_mouse.x = 0;
        }
        if (current_mouse.x >= screen_w) {
            current_mouse.x = screen_w - 1;
        }
        if (current_mouse.y < 0) {
            current_mouse.y = 0;
        }
        if (current_mouse.y >= screen_h) {
            current_mouse.y = screen_h - 1;
        }

        current_mouse.buttons = (uint8_t)(flags & 0x07U);
        current_mouse.packets++;

        kprintf("[MOUSE] pkt#%u pos=(%d,%d) delta=(%d,%d) buttons=0x%x\n",
                current_mouse.packets,
                current_mouse.x, current_mouse.y,
                current_mouse.dx, current_mouse.dy,
                (uint32_t)current_mouse.buttons);
        break;
    }

    default:
        pkt_cycle = 0;
        break;
    }
}

void mouse_inject_usb_report(int32_t dx, int32_t dy, uint8_t buttons)
{
    current_mouse.dx = dx;
    current_mouse.dy = dy;
    current_mouse.x += dx;
    current_mouse.y += dy;

    if (current_mouse.x < 0) {
        current_mouse.x = 0;
    }
    if (current_mouse.x >= screen_w) {
        current_mouse.x = screen_w - 1;
    }
    if (current_mouse.y < 0) {
        current_mouse.y = 0;
    }
    if (current_mouse.y >= screen_h) {
        current_mouse.y = screen_h - 1;
    }

    current_mouse.buttons = (uint8_t)(buttons & 0x07U);
    current_mouse.packets++;

    kprintf("[USB-MOUSE] pkt#%u pos=(%d,%d) delta=(%d,%d) buttons=0x%x\n",
            current_mouse.packets,
            current_mouse.x, current_mouse.y,
            current_mouse.dx, current_mouse.dy,
            (uint32_t)current_mouse.buttons);
}

static void mouse_irq_handler(struct isr_regs *regs)
{
    uint8_t status;
    uint8_t data;
    (void)regs;

    status = inb(PS2_STATUS_PORT);
    if ((status & 0x01U) == 0) {
        return;
    }
    data = inb(PS2_DATA_PORT);
    if ((status & 0x20U) != 0) {
        mouse_inject_byte(data);
    } else {
        kbd_inject_scancode(data);
    }
}

void mouse_get_state(struct mouse_state *out)
{
    uint32_t flags = cpu_eflags();
    cpu_cli();
    *out = current_mouse;
    if ((flags & 0x200U) != 0) {
        cpu_sti();
    }
}

void mouse_init(int32_t max_width, int32_t max_height)
{
    screen_w = max_width;
    screen_h = max_height;

    current_mouse.x = max_width / 2;
    current_mouse.y = max_height / 2;
    current_mouse.dx = 0;
    current_mouse.dy = 0;
    current_mouse.buttons = 0;
    current_mouse.packets = 0;
    pkt_cycle = 0;

    /* Reset to default settings and enable packet streaming (if PS/2 mouse present) */
    if (mouse_write(0xF6) == 0) {
        (void)mouse_write(0xF4);
    }

    /* Flush any lingering bytes */
    while ((inb(PS2_STATUS_PORT) & 0x01U) != 0) {
        (void)inb(PS2_DATA_PORT);
    }

    irq_register_handler(12, mouse_irq_handler);
    pic_unmask_irq(2);  /* Slave PIC cascade */
    pic_unmask_irq(12); /* Mouse IRQ 12 */

    kprintf("[MOUSE] PS/2 mouse initialized on IRQ12 (pos=%d,%d)\n",
            current_mouse.x, current_mouse.y);
}
