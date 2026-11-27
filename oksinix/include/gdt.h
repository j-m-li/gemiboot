/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Global Descriptor Table (GDT) & 32-bit TSS
 */

#ifndef OKSINIX_GDT_H
#define OKSINIX_GDT_H

#include "types.h"

#define KERNEL_CS 0x08U
#define KERNEL_DS 0x10U
#define USER_CS   0x1BU /* GDT entry 3 (0x18) | RPL 3 */
#define USER_DS   0x23U /* GDT entry 4 (0x20) | RPL 3 */
#define TSS_SEG   0x28U /* GDT entry 5 (0x28) */

void     gdt_init(void);
void     tss_set_esp0(uint32_t esp0);
uint32_t tss_get_esp0(void);

#endif /* OKSINIX_GDT_H */
