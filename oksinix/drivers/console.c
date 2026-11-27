/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Graphical VGA 640x480 Console & Desktop UI
 */

#include "console.h"
#include "vga.h"
#include "ps2.h"
#include "xhci.h"
#include "serial.h"
#include "string.h"

static char     con_grid[CON_ROWS][CON_COLS + 1];
static uint8_t  con_hl[CON_ROWS][CON_COLS];
static char     con_saved_grid[CON_ROWS][CON_COLS + 1];
static int32_t  con_col = 0;
static int32_t  con_row = 0;
static int32_t  con_saved_col = 0;
static int32_t  con_saved_row = 0;
static char     con_proc_info[48] = "PID 1: /bin/init -> PID 2: /bin/sh";
static uint8_t  con_last_scancode = 0;
static bool_t   con_alt_screen = FALSE;
static uint8_t  con_cur_hl = 0;
static uint8_t  con_esc_state = 0;
static bool_t   con_esc_priv = FALSE;
static int32_t  con_esc_params[2];
static uint8_t  con_esc_nparams = 0;

static char *fmt_int(char *dst, int32_t val)
{
    char tmp[16];
    int i = 0;
    uint32_t uval;

    if (val < 0) {
        *dst++ = '-';
        uval = (uint32_t)(-(val + 1)) + 1U;
    } else {
        uval = (uint32_t)val;
    }

    if (uval == 0) {
        tmp[i++] = '0';
    } else {
        while (uval > 0) {
            tmp[i++] = (char)('0' + (uval % 10U));
            uval /= 10U;
        }
    }
    while (i > 0) {
        *dst++ = tmp[--i];
    }
    *dst = '\0';
    return dst;
}

static char *fmt_hex8(char *dst, uint8_t val)
{
    const char *hex = "0123456789ABCDEF";
    *dst++ = '0';
    *dst++ = 'x';
    *dst++ = hex[(val >> 4) & 0x0FU];
    *dst++ = hex[val & 0x0FU];
    *dst = '\0';
    return dst;
}

static char *fmt_str(char *dst, const char *src)
{
    while (*src != '\0') {
        *dst++ = *src++;
    }
    *dst = '\0';
    return dst;
}

static void console_scroll(void)
{
    int32_t r;
    for (r = 0; r < CON_ROWS - 1; r++) {
        memcpy(con_grid[r], con_grid[r + 1], CON_COLS + 1);
        memcpy(con_hl[r], con_hl[r + 1], CON_COLS);
    }
    memset(con_grid[CON_ROWS - 1], ' ', CON_COLS);
    con_grid[CON_ROWS - 1][CON_COLS] = '\0';
    memset(con_hl[CON_ROWS - 1], 0, CON_COLS);
    con_row = CON_ROWS - 1;
}

void console_clear(void)
{
    int32_t r;
    for (r = 0; r < CON_ROWS; r++) {
        memset(con_grid[r], ' ', CON_COLS);
        con_grid[r][CON_COLS] = '\0';
        memset(con_hl[r], 0, CON_COLS);
    }
    con_col = 0;
    con_row = 0;
}

void console_init(void)
{
    console_clear();
}

static void console_exec_csi(char cmd)
{
    int32_t p0 = (con_esc_nparams >= 1U) ? con_esc_params[0] : 0;
    int32_t p1 = (con_esc_nparams >= 2U) ? con_esc_params[1] : 0;

    if (con_esc_priv) {
        if (cmd == 'h' && p0 == 1049) {
            console_save_screen();
            console_clear();
            con_alt_screen = TRUE;
            con_cur_hl = 0;
        } else if (cmd == 'l' && p0 == 1049) {
            con_alt_screen = FALSE;
            con_cur_hl = 0;
            console_restore_screen();
            console_refresh();
        } else if (cmd == 'h' && p0 == 25) {
            console_refresh();
        }
        return;
    }

    if (cmd == 'H' || cmd == 'f') {
        int32_t row = (p0 > 0) ? (p0 - 1) : 0;
        int32_t col = (p1 > 0) ? (p1 - 1) : 0;
        console_set_cursor(col, row);
    } else if (cmd == 'K') {
        if (p0 == 2) {
            console_clear_row(con_row);
        } else if (p0 == 0 && con_row >= 0 && con_row < CON_ROWS) {
            int32_t c;
            for (c = con_col; c < CON_COLS; c++) {
                con_grid[con_row][c] = ' ';
                con_hl[con_row][c] = 0;
            }
        }
    } else if (cmd == 'J' && p0 == 2) {
        console_clear();
    } else if (cmd == 'm') {
        con_cur_hl = (p0 == 7) ? 1U : 0U;
    }
}

void console_putc(char c)
{
    /* ANSI/VT100 CSI escape sequence parser (used by standalone Ring-3 full-screen apps) */
    if (con_esc_state == 1U) {
        if (c == '[') {
            con_esc_state     = 2U;
            con_esc_priv      = FALSE;
            con_esc_params[0] = 0;
            con_esc_params[1] = 0;
            con_esc_nparams   = 0U;
            return;
        }
        con_esc_state = 0U;
    } else if (con_esc_state == 2U) {
        if (c == '?' && con_esc_nparams == 0U && !con_esc_priv) {
            con_esc_priv = TRUE;
            return;
        }
        if (c >= '0' && c <= '9') {
            if (con_esc_nparams == 0U) {
                con_esc_nparams = 1U;
            }
            if (con_esc_nparams <= 2U) {
                con_esc_params[con_esc_nparams - 1U] =
                    con_esc_params[con_esc_nparams - 1U] * 10 + (int32_t)(c - '0');
            }
            return;
        }
        if (c == ';') {
            if (con_esc_nparams == 0U) {
                con_esc_nparams = 1U;
            }
            if (con_esc_nparams < 2U) {
                con_esc_nparams++;
                con_esc_params[con_esc_nparams - 1U] = 0;
            }
            return;
        }
        con_esc_state = 0U;
        console_exec_csi(c);
        return;
    }

    if (c == 0x1B) {
        con_esc_state = 1U;
        return;
    }

    /* Mirror normal console output to serial COM1 (suppressed during alt-screen TUI apps) */
    if (!con_alt_screen) {
        serial_putc(c);
    }

    if (c == '\r') {
        con_col = 0;
        return;
    }
    if (c == '\n') {
        con_col = 0;
        con_row++;
        if (con_row >= CON_ROWS) {
            console_scroll();
        }
        return;
    }
    if (c == '\b') {
        if (con_col > 0) {
            con_col--;
            con_grid[con_row][con_col] = ' ';
            con_hl[con_row][con_col] = 0;
        }
        return;
    }
    if (c >= 32 && c <= 126) {
        con_grid[con_row][con_col] = c;
        con_hl[con_row][con_col] = con_cur_hl;
        con_col++;
        if (con_col >= CON_COLS) {
            if (con_alt_screen) {
                con_col = CON_COLS - 1;
            } else {
                con_col = 0;
                con_row++;
                if (con_row >= CON_ROWS) {
                    console_scroll();
                }
            }
        }
    }
}

void console_write(const char *buf, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        console_putc(buf[i]);
    }
}

void console_puts(const char *s)
{
    while (*s != '\0') {
        console_putc(*s++);
    }
}

void console_set_cursor(int32_t col, int32_t row)
{
    if (col < 0) {
        col = 0;
    }
    if (col >= CON_COLS) {
        col = CON_COLS - 1;
    }
    if (row < 0) {
        row = 0;
    }
    if (row >= CON_ROWS) {
        row = CON_ROWS - 1;
    }
    con_col = col;
    con_row = row;
}

void console_put_at(int32_t col, int32_t row, char c)
{
    if (row >= 0 && row < CON_ROWS && col >= 0 && col < CON_COLS) {
        con_grid[row][col] = c;
    }
}

void console_puts_at(int32_t col, int32_t row, const char *s)
{
    if (row < 0 || row >= CON_ROWS || s == NULL) {
        return;
    }
    while (*s != '\0' && col < CON_COLS) {
        if (col >= 0) {
            con_grid[row][col] = *s;
        }
        col++;
        s++;
    }
}

void console_set_highlight(int32_t col, int32_t row, bool_t highlight)
{
    if (row >= 0 && row < CON_ROWS && col >= 0 && col < CON_COLS) {
        con_hl[row][col] = highlight ? 1U : 0U;
    }
}

void console_clear_row(int32_t row)
{
    if (row >= 0 && row < CON_ROWS) {
        memset(con_grid[row], ' ', CON_COLS);
        con_grid[row][CON_COLS] = '\0';
        memset(con_hl[row], 0, CON_COLS);
    }
}

void console_save_screen(void)
{
    memcpy(con_saved_grid, con_grid, sizeof(con_grid));
    con_saved_col = con_col;
    con_saved_row = con_row;
}

void console_restore_screen(void)
{
    memcpy(con_grid, con_saved_grid, sizeof(con_grid));
    memset(con_hl, 0, sizeof(con_hl));
    con_col = con_saved_col;
    con_row = con_saved_row;
}

void console_set_status(const char *proc_status, uint8_t last_scancode)
{
    if (proc_status != NULL) {
        size_t i = 0;
        while (proc_status[i] != '\0' && i + 1 < sizeof(con_proc_info)) {
            con_proc_info[i] = proc_status[i];
            i++;
        }
        con_proc_info[i] = '\0';
    }
    con_last_scancode = last_scancode;
}

void console_refresh(void)
{
    struct mouse_state ms;
    char status_line[96];
    char *p;
    int32_t r, c;

    mouse_get_state(&ms);

    /* Desktop background */
    vga_clear(COLOR_BLUE);

    /* Top title bar */
    vga_fill_rect(0, 0, VGA_WIDTH, 18, COLOR_DARK_GRAY);
    vga_draw_line(0, 18, VGA_WIDTH - 1, 18, COLOR_WHITE);
    vga_draw_string(10, 5, "OKSINIX x86-32 C90 Kernel (CC0) - VGA 640x480 Mode 12h",
                    COLOR_YELLOW, COLOR_DARK_GRAY, TRUE);

    /* Borderless 80x25 console window (/dev/console - full 640px width) */
    vga_fill_rect(0, 19, VGA_WIDTH, 377, COLOR_BLACK);

    /* Render terminal character grid (80 cols x 25 rows) */
    for (r = 0; r < CON_ROWS; r++) {
        int32_t py = 24 + r * CON_CELL_H;
        for (c = 0; c < CON_COLS; c++) {
            int32_t px = c * CON_CELL_W;
            char ch = con_grid[r][c];
            if (con_hl[r][c] != 0) {
                vga_fill_rect(px, py, CON_CELL_W, 9, COLOR_LIGHT_GREEN);
                if (ch != ' ' && ch != '\0') {
                    vga_draw_char(px, py, ch, COLOR_BLACK, COLOR_LIGHT_GREEN, FALSE);
                }
            } else if (ch != ' ' && ch != '\0') {
                vga_draw_char(px, py, ch, COLOR_LIGHT_GREEN, COLOR_BLACK, TRUE);
            }
        }
    }

    /* Terminal text cursor */
    if (con_row >= 0 && con_row < CON_ROWS && con_col >= 0 && con_col < CON_COLS) {
        vga_fill_rect(con_col * CON_CELL_W, 24 + con_row * CON_CELL_H + 8,
                      CON_CELL_W - 1, 2, COLOR_LIGHT_GREEN);
    }

    /* Hardware & VFS Status Panel */
    vga_fill_rect(10, 396, 620, 58, COLOR_LIGHT_GRAY);
    vga_draw_rect(10, 396, 620, 58, COLOR_WHITE);
    vga_fill_rect(11, 397, 618, 13, COLOR_DARK_GRAY);
    vga_draw_string(16, 400, "System & Device Status (/dev/kbd IRQ1, /dev/mouse IRQ12, /dev/fb0)",
                    COLOR_WHITE, COLOR_DARK_GRAY, TRUE);

    /* Line 1 of status panel */
    p = status_line;
    p = fmt_str(p, "Task: ");
    p = fmt_str(p, con_proc_info);
    p = fmt_str(p, "   Last Scancode: ");
    p = fmt_hex8(p, con_last_scancode);
    vga_draw_string(16, 416, status_line, COLOR_BLACK, COLOR_LIGHT_GRAY, TRUE);

    /* Line 2 of status panel: Mouse coordinates + buttons */
    p = status_line;
    p = fmt_str(p, "Mouse (/dev/mouse): X=");
    p = fmt_int(p, ms.x);
    p = fmt_str(p, " Y=");
    p = fmt_int(p, ms.y);
    p = fmt_str(p, " Pkts=");
    p = fmt_int(p, (int32_t)ms.packets);
    vga_draw_string(16, 434, status_line, COLOR_BLACK, COLOR_LIGHT_GRAY, TRUE);

    vga_draw_string(370, 434, "Buttons:", COLOR_BLACK, COLOR_LIGHT_GRAY, TRUE);
    vga_fill_rect(440, 432, 32, 12, (ms.buttons & MOUSE_BTN_LEFT)   ? COLOR_LIGHT_RED   : COLOR_DARK_GRAY);
    vga_draw_string(452, 434, "L", COLOR_WHITE, 0, TRUE);
    vga_fill_rect(478, 432, 32, 12, (ms.buttons & MOUSE_BTN_MIDDLE) ? COLOR_YELLOW      : COLOR_DARK_GRAY);
    vga_draw_string(490, 434, "M", COLOR_BLACK, 0, TRUE);
    vga_fill_rect(516, 432, 32, 12, (ms.buttons & MOUSE_BTN_RIGHT)  ? COLOR_LIGHT_GREEN : COLOR_DARK_GRAY);
    vga_draw_string(528, 434, "R", COLOR_WHITE, 0, TRUE);

    /* Bottom status bar */
    vga_fill_rect(0, 462, VGA_WIDTH, 18, COLOR_DARK_GRAY);
    vga_draw_string(10, 467,
                    "VGA 640x480x16 @ 0xA0000 | VFS: / /dev /bin /etc /tmp | Ready",
                    COLOR_LIGHT_CYAN, COLOR_DARK_GRAY, TRUE);

    /* Draw mouse cursor on top */
    vga_draw_cursor(ms.x, ms.y, ms.buttons);

    /* Flush to VGA VRAM */
    vga_present();
}

int console_poll_char(char *out_ch)
{
    struct key_event kev;

    xhci_poll();

    while (kbd_pop_event(&kev)) {
        con_last_scancode = kev.scancode;
        if (!kev.released && kev.ascii != 0) {
            *out_ch = kev.ascii;
            return 1;
        }
    }

    if (serial_received()) {
        char c = serial_getc();
        if (c == '\r') {
            c = '\n';
        }
        *out_ch = c;
        return 1;
    }

    return 0;
}
