/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - PCI Bus Configuration Space Driver
 */

#ifndef OKSINIX_PCI_H
#define OKSINIX_PCI_H

#include "types.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

struct pci_device {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  irq_line;
    uint32_t bar0_mmio;
};

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);
int      pci_find_class_nth(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                            int index, struct pci_device *out);
int      pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, struct pci_device *out);

#endif /* OKSINIX_PCI_H */
