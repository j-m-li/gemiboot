/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - USB HID Boot Keyboard & Mouse Driver
 */

#include "xhci_internal.h"
#include "ps2.h"
#include "serial.h"
#include "string.h"

static struct xhci_usb_dev *kbd_dev   = NULL;
static struct xhci_usb_dev *mouse_dev = NULL;

void usbhid_reset(void)
{
    kbd_dev   = NULL;
    mouse_dev = NULL;
}

static void usbhid_queue_int_trb(struct xhci_usb_dev *dev)
{
    uint32_t len = (dev->dev_type == USB_DEV_KBD) ? 8U : 4U;

    if (len > dev->int_in_mps && dev->int_in_mps > 0) {
        len = dev->int_in_mps;
    }
    xhci_interrupt_in_submit(dev, dev->hid_buf, len);
}

static void usbhid_handle_kbd_report(struct xhci_usb_dev *dev)
{
    const uint8_t *rep = dev->hid_buf;
    uint8_t mods = rep[0];
    bool_t shift = ((mods & 0x22U) != 0) ? TRUE : FALSE;
    bool_t ctrl  = ((mods & 0x11U) != 0) ? TRUE : FALSE;
    bool_t alt   = ((mods & 0x44U) != 0) ? TRUE : FALSE;
    int i, j;

    for (i = 2; i < 8; i++) {
        uint8_t usage = rep[i];
        bool_t was_pressed = FALSE;

        if (usage < 4U) {
            continue;
        }
        for (j = 2; j < 8; j++) {
            if (dev->prev_kbd_report[j] == usage) {
                was_pressed = TRUE;
                break;
            }
        }
        if (!was_pressed) {
            kbd_inject_usb_key(usage, 0, FALSE, shift, ctrl, alt);
        }
    }

    memcpy(dev->prev_kbd_report, rep, 8);
}

static void usbhid_handle_mouse_report(struct xhci_usb_dev *dev)
{
    const uint8_t *rep = dev->hid_buf;
    uint8_t buttons = (uint8_t)(rep[0] & 0x07U);
    int32_t dx = (int32_t)(int8_t)rep[1];
    int32_t dy = (int32_t)(int8_t)rep[2];

    mouse_inject_usb_report(dx, dy, buttons);
}

bool_t usbhid_handle_transfer_event(uint8_t slot_id)
{
    if (kbd_dev != NULL && kbd_dev->slot_id == slot_id) {
        usbhid_handle_kbd_report(kbd_dev);
        usbhid_queue_int_trb(kbd_dev);
        return TRUE;
    }
    if (mouse_dev != NULL && mouse_dev->slot_id == slot_id) {
        usbhid_handle_mouse_report(mouse_dev);
        usbhid_queue_int_trb(mouse_dev);
        return TRUE;
    }
    return FALSE;
}

void usbhid_init_kbd(struct xhci_usb_dev *dev)
{
    if (dev->hid_buf == NULL) {
        dev->hid_buf = (uint8_t *)xhci_dma_alloc(64, 64);
    }
    if (dev->hid_buf == NULL) {
        return;
    }
    memset(dev->prev_kbd_report, 0, sizeof(dev->prev_kbd_report));

    /* SET_PROTOCOL (Boot Protocol = 0) and SET_IDLE (0) */
    (void)xhci_control_transfer(dev, 0x21U, 0x0BU, 0, dev->iface_num, 0, NULL);
    (void)xhci_control_transfer(dev, 0x21U, 0x0AU, 0, dev->iface_num, 0, NULL);
    kbd_dev = dev;
    kprintf("[XHCI] USB Keyboard initialized (slot %u, root_port %u, route 0x%x, EP DCI %u)\n",
            (uint32_t)dev->slot_id, (uint32_t)dev->root_port, dev->route_string,
            (uint32_t)dev->int_in_dci);
    usbhid_queue_int_trb(dev);
}

void usbhid_init_mouse(struct xhci_usb_dev *dev)
{
    if (dev->hid_buf == NULL) {
        dev->hid_buf = (uint8_t *)xhci_dma_alloc(64, 64);
    }
    if (dev->hid_buf == NULL) {
        return;
    }

    /* SET_PROTOCOL (Boot Protocol = 0) */
    (void)xhci_control_transfer(dev, 0x21U, 0x0BU, 0, dev->iface_num, 0, NULL);
    mouse_dev = dev;
    kprintf("[XHCI] USB Mouse initialized (slot %u, root_port %u, route 0x%x, EP DCI %u)\n",
            (uint32_t)dev->slot_id, (uint32_t)dev->root_port, dev->route_string,
            (uint32_t)dev->int_in_dci);
    usbhid_queue_int_trb(dev);
}

bool_t xhci_has_usb_kbd(void)
{
    return (kbd_dev != NULL) ? TRUE : FALSE;
}

bool_t xhci_has_usb_mouse(void)
{
    return (mouse_dev != NULL) ? TRUE : FALSE;
}
