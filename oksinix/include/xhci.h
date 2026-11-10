/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - xHCI USB 3.0 Host Controller Driver
 * Supports USB Hub, USB HID Boot Keyboard, USB HID Boot Mouse, and USB Mass Storage (BOT/SCSI)
 */

#ifndef OKSINIX_XHCI_H
#define OKSINIX_XHCI_H

#include "types.h"

#define XHCI_MAX_MSC_DEVS 4

struct xhci_msc_info {
    bool_t   present;
    uint8_t  slot_id;
    uint32_t block_count;
    uint32_t block_size;
    char     vendor[9];
    char     product[17];
};

struct xhci_hub_info {
    bool_t   present;
    uint8_t  slot_id;
    uint8_t  root_port;
    uint8_t  num_ports;
    uint8_t  downstream_devices;
};

struct xhci_controller_info {
    bool_t   present;
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t bar0_mmio;
    uint32_t max_ports;
    uint32_t max_slots;
    uint32_t num_devices;
};

int  xhci_init(void);
void xhci_poll(void);

bool_t xhci_is_initialized(void);
bool_t xhci_has_usb_hub(void);
bool_t xhci_has_usb_kbd(void);
bool_t xhci_has_usb_mouse(void);
bool_t xhci_has_usb_storage(void);

void    xhci_get_controller_info(struct xhci_controller_info *out);
void    xhci_get_hub_info(struct xhci_hub_info *out);
int     xhci_get_msc_count(void);
int     xhci_get_msc_info_idx(int disk_idx, struct xhci_msc_info *out);
void    xhci_get_msc_info(struct xhci_msc_info *out);
int32_t xhci_msc_read_sector_idx(int disk_idx, uint32_t lba, uint8_t *buf512);
int32_t xhci_msc_write_sector_idx(int disk_idx, uint32_t lba, const uint8_t *buf512);
int32_t xhci_msc_read_bytes_idx(int disk_idx, uint32_t offset, uint32_t size, uint8_t *buf);
int32_t xhci_msc_write_bytes_idx(int disk_idx, uint32_t offset, uint32_t size, const uint8_t *buf);
int32_t xhci_msc_read_sector(uint32_t lba, uint8_t *buf512);
int32_t xhci_msc_write_sector(uint32_t lba, const uint8_t *buf512);
int32_t xhci_msc_read_bytes(uint32_t offset, uint32_t size, uint8_t *buf);
int32_t xhci_msc_write_bytes(uint32_t offset, uint32_t size, const uint8_t *buf);

#endif /* OKSINIX_XHCI_H */
