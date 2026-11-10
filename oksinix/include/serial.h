/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Serial console (COM1) and formatted logging
 */

#ifndef OKSINIX_SERIAL_H
#define OKSINIX_SERIAL_H

#include "types.h"

#define COM1_PORT 0x3F8

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
int  serial_received(void);
char serial_getc(void);

void kprintf(const char *fmt, ...);

#endif /* OKSINIX_SERIAL_H */
