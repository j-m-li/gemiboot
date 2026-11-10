/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Graphical VGA 640x480 Console & Desktop UI
 */

#ifndef OKSINIX_CONSOLE_H
#define OKSINIX_CONSOLE_H

#include "types.h"

#define CON_COLS 75
#define CON_ROWS 33

void console_init(void);
void console_clear(void);
void console_putc(char c);
void console_write(const char *buf, uint32_t len);
void console_puts(const char *s);
void console_set_cursor(int32_t col, int32_t row);
void console_put_at(int32_t col, int32_t row, char c);
void console_puts_at(int32_t col, int32_t row, const char *s);
void console_clear_row(int32_t row);
void console_save_screen(void);
void console_restore_screen(void);
void console_set_status(const char *proc_status, uint8_t last_scancode);
void console_refresh(void);

/* Poll keyboard and serial for 1 character (non-blocking returns 0 if none) */
int  console_poll_char(char *out_ch);

#endif /* OKSINIX_CONSOLE_H */
