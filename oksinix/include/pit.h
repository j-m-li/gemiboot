/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - 8254 Programmable Interval Timer (PIT)
 */

#ifndef OKSINIX_PIT_H
#define OKSINIX_PIT_H

#include "types.h"

#define PIT_BASE_HZ 1193182UL
#define PIT_TICK_HZ 1000UL

void     pit_init(void);
uint32_t pit_get_ticks(void);
void     pit_udelay(uint32_t us);
void     pit_mdelay(uint32_t ms);

#endif /* OKSINIX_PIT_H */
