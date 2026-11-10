/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Global Descriptor Table (GDT)
 */

#ifndef OKSINIX_GDT_H
#define OKSINIX_GDT_H

#include "types.h"

#define KERNEL_CS 0x08
#define KERNEL_DS 0x10

void gdt_init(void);

#endif /* OKSINIX_GDT_H */
