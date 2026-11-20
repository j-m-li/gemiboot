/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Internal xHCI / USB class driver interface
 */

#ifndef OKSINIX_XHCI_INTERNAL_H
#define OKSINIX_XHCI_INTERNAL_H

#include "xhci.h"

/* xHCI Ring and Slot limits */
#define XHCI_RING_TRBS     256
#define XHCI_MAX_SLOTS     16

/* Per-device class types */
#define USB_DEV_NONE       0
#define USB_DEV_KBD        1
#define USB_DEV_MOUSE      2
#define USB_DEV_STORAGE    3
#define USB_DEV_HUB        4

/* 16-byte xHCI Transfer Request Block (TRB) */
struct xhci_trb {
    uint32_t param_lo;
    uint32_t param_hi;
    uint32_t status;
    uint32_t control;
};

/* Transfer Ring wrapper with enqueue index and cycle state */
struct xhci_ring {
    volatile struct xhci_trb *trbs;
    uint32_t                  enq_idx;
    uint32_t                  ccs;
};

/* Per-device xHCI state shared with USB class drivers */
struct xhci_usb_dev {
    uint32_t         dev_type;
    uint8_t          slot_id;
    uint8_t          root_port;
    uint32_t         route_string;
    uint8_t          speed;
    uint8_t          parent_hub_slot;
    uint8_t          parent_port;
    uint8_t          hub_ports;
    uint8_t          hub_tier;
    uint8_t          hub_ttt;
    uint32_t         hub_port_conn_mask;
    uint8_t          iface_num;
    uint8_t          config_val;
    uint16_t         ep0_mps;
    uint8_t          int_in_dci;
    uint16_t         int_in_mps;
    uint8_t          bulk_in_dci;
    uint16_t         bulk_in_mps;
    uint8_t          bulk_in_burst;
    uint8_t          bulk_out_dci;
    uint16_t         bulk_out_mps;
    uint8_t          bulk_out_burst;
    uint8_t         *out_ctx;
    uint8_t         *in_ctx;
    struct xhci_ring ep0_ring;
    struct xhci_ring int_in_ring;
    struct xhci_ring bulk_in_ring;
    struct xhci_ring bulk_out_ring;
    uint8_t         *hid_buf;
    uint8_t          prev_kbd_report[8];
};

/* Core xHCI transport & enumeration primitives (drivers/xhci.c) */
void  *xhci_dma_alloc(uint32_t size, uint32_t align);
int    xhci_control_transfer(struct xhci_usb_dev *dev,
                             uint8_t bmRequestType, uint8_t bRequest,
                             uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                             void *data);
int    xhci_bulk_transfer(struct xhci_usb_dev *dev, bool_t is_in,
                          void *buf, uint32_t len);
void   xhci_clear_bulk_stall(struct xhci_usb_dev *dev, bool_t is_in);
void   xhci_interrupt_in_submit(struct xhci_usb_dev *dev, void *buf, uint32_t len);
bool_t xhci_has_free_slot(void);
int    xhci_enumerate_device(uint8_t root_port, uint32_t route_string,
                             uint32_t tier, uint8_t speed,
                             uint8_t parent_hub_slot, uint8_t parent_port);

/* USB HID Boot Keyboard & Mouse class driver (drivers/usbhid.c) */
void   usbhid_reset(void);
void   usbhid_init_kbd(struct xhci_usb_dev *dev);
void   usbhid_init_mouse(struct xhci_usb_dev *dev);
bool_t usbhid_handle_transfer_event(uint8_t slot_id);

/* USB Mass Storage BOT/SCSI class driver (drivers/usbmsc.c) */
void   usbmsc_reset(void);
void   usbmsc_init_device(struct xhci_usb_dev *dev);

/* USB Hub class driver (drivers/usbhub.c) */
void     usbhub_reset(void);
uint32_t usbhub_read_descriptor(struct xhci_usb_dev *hub_dev, uint32_t tier);
void     usbhub_init_device(struct xhci_usb_dev *hub_dev, uint32_t tier, uint32_t pwr_delay_ms);
void     usbhub_poll_ports(void);

#endif /* OKSINIX_XHCI_INTERNAL_H */
