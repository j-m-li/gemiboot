/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - PCI Bus Configuration Space Driver
 */

#include "pci.h"
#include "io.h"
#include "serial.h"

#define PCI_FALLBACK_MMIO_BASE 0xFE000000UL

static uint32_t pci_make_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    return (uint32_t)((1UL << 31) |
                      ((uint32_t)bus << 16) |
                      ((uint32_t)(slot & 0x1FU) << 11) |
                      ((uint32_t)(func & 0x07U) << 8) |
                      ((uint32_t)(offset & 0xFCU)));
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDR, pci_make_addr(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val)
{
    outl(PCI_CONFIG_ADDR, pci_make_addr(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, val);
}

static uint32_t pci_resolve_bridge_mmio(uint8_t target_bus, uint32_t fallback_addr)
{
    uint32_t b, s, f;

    if (target_bus == 0) {
        return fallback_addr;
    }

    for (b = 0; b < 256; b++) {
        for (s = 0; s < 32; s++) {
            for (f = 0; f < 8; f++) {
                uint32_t id_reg = pci_read32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x00);
                uint16_t vendor = (uint16_t)(id_reg & 0xFFFFU);
                uint32_t hdr;
                uint8_t  hdr_type;

                if (vendor == 0xFFFFU || vendor == 0x0000U) {
                    if (f == 0) {
                        break;
                    }
                    continue;
                }

                hdr = pci_read32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x0C);
                hdr_type = (uint8_t)((hdr >> 16) & 0x7FU);
                if (hdr_type == 0x01U) {
                    uint32_t buses = pci_read32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x18);
                    uint8_t  sec_bus = (uint8_t)((buses >> 8) & 0xFFU);
                    uint8_t  sub_bus = (uint8_t)((buses >> 16) & 0xFFU);

                    if (sec_bus != 0 && target_bus >= sec_bus && target_bus <= sub_bus) {
                        uint32_t mem_win   = pci_read32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x20);
                        uint32_t mem_base  = (mem_win & 0xFFF0U) << 16;
                        uint32_t mem_limit = (mem_win & 0xFFF00000UL) | 0x000FFFFFUL;
                        uint32_t br_cmd    = pci_read32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x04);

                        /* Enable Memory Space + Bus Master on parent PCI bridge */
                        pci_write32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x04, br_cmd | 0x0006U);

                        if (mem_base != 0 && mem_base <= mem_limit && mem_base >= 0x80000000UL) {
                            return mem_base;
                        } else {
                            uint16_t win_hi = (uint16_t)((fallback_addr >> 16) & 0xFFF0U);
                            uint32_t new_win = (uint32_t)win_hi | ((uint32_t)win_hi << 16);
                            pci_write32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x20, new_win);
                            return fallback_addr;
                        }
                    }
                }

                if (f == 0 && ((hdr >> 16) & 0x80U) == 0) {
                    break;
                }
            }
        }
    }
    return fallback_addr;
}

int pci_find_class_nth(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                       int index, struct pci_device *out)
{
    uint32_t bus, slot, func;
    int match_idx = 0;

    for (bus = 0; bus < 256; bus++) {
        for (slot = 0; slot < 32; slot++) {
            for (func = 0; func < 8; func++) {
                uint32_t id_reg;
                uint16_t vendor;
                uint32_t class_reg;
                uint8_t  cc, sc, pi;

                id_reg = pci_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x00);
                vendor = (uint16_t)(id_reg & 0xFFFFU);
                if (vendor == 0xFFFFU || vendor == 0x0000U) {
                    if (func == 0) {
                        break;
                    }
                    continue;
                }

                class_reg = pci_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x08);
                cc = (uint8_t)((class_reg >> 24) & 0xFFU);
                sc = (uint8_t)((class_reg >> 16) & 0xFFU);
                pi = (uint8_t)((class_reg >> 8) & 0xFFU);

                if (cc == class_code && sc == subclass && pi == prog_if) {
                    if (match_idx == index) {
                        uint32_t bar0 = pci_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x10);
                        uint32_t cmd  = pci_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x04);
                        uint32_t intr = pci_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x3C);
                        uint32_t mmio_base = bar0 & ~0x0FUL;
                        bool_t   is_64bit = (((bar0 >> 1) & 0x03U) == 0x02U) ? TRUE : FALSE;
                        uint32_t fallback = PCI_FALLBACK_MMIO_BASE + ((uint32_t)index * 0x00100000UL);

                        if (mmio_base == 0) {
                            pci_write32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x04, cmd & ~0x0002U);
                            mmio_base = pci_resolve_bridge_mmio((uint8_t)bus, fallback);
                            pci_write32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x10,
                                        mmio_base | (bar0 & 0x0FUL));
                            if (is_64bit) {
                                pci_write32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x14, 0);
                            }
                        } else if (is_64bit) {
                            /* Ensure upper 32 bits of 64-bit BAR are 0 for 32-bit protected mode */
                            uint32_t bar1 = pci_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x14);
                            if (bar1 != 0) {
                                pci_write32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x04, cmd & ~0x0002U);
                                mmio_base = pci_resolve_bridge_mmio((uint8_t)bus, fallback);
                                pci_write32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x10,
                                            mmio_base | (bar0 & 0x0FUL));
                                pci_write32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x14, 0);
                            }
                        }

                        /* Enable PCI Memory Space (bit 1) and Bus Master (bit 2) */
                        cmd |= 0x0006U;
                        pci_write32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x04, cmd);

                        out->bus        = (uint8_t)bus;
                        out->slot       = (uint8_t)slot;
                        out->func       = (uint8_t)func;
                        out->vendor_id  = vendor;
                        out->device_id  = (uint16_t)((id_reg >> 16) & 0xFFFFU);
                        out->class_code = cc;
                        out->subclass   = sc;
                        out->prog_if    = pi;
                        out->irq_line   = (uint8_t)(intr & 0xFFU);
                        out->bar0_mmio  = mmio_base;

                        kprintf("[PCI] Found %02x:%02x.%u vendor=0x%04x device=0x%04x BAR0=0x%08x\n",
                                bus, slot, func,
                                (uint32_t)out->vendor_id, (uint32_t)out->device_id,
                                out->bar0_mmio);
                        return 0;
                    }
                    match_idx++;
                }

                /* If function 0 is not multi-function, skip functions 1..7 */
                if (func == 0) {
                    uint32_t hdr = pci_read32((uint8_t)bus, (uint8_t)slot, 0, 0x0C);
                    if (((hdr >> 16) & 0x80U) == 0) {
                        break;
                    }
                }
            }
        }
    }
    return -1;
}

int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, struct pci_device *out)
{
    return pci_find_class_nth(class_code, subclass, prog_if, 0, out);
}
