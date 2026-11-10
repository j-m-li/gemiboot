/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - 8259 Programmable Interrupt Controller (PIC)
 */

#ifndef OKSINIX_PIC_H
#define OKSINIX_PIC_H

#include "types.h"

void   pic_init(void);
void   pic_send_eoi(uint8_t irq);
bool_t pic_is_spurious_irq(uint8_t irq);
void   pic_unmask_irq(uint8_t irq);
void   pic_mask_irq(uint8_t irq);

#endif /* OKSINIX_PIC_H */
