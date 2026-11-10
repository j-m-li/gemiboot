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
static char     con_saved_grid[CON_ROWS][CON_COLS + 1];
static int32_t  con_col = 0;
static int32_t  con_row = 0;
static int32_t  con_saved_col = 0;
static int32_t  con_saved_row = 0;
static char     con_proc_info[48] = "PID 1: /bin/init -> PID 2: /bin/sh";
static uint8_t  con_last_scancode = 0;

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
    }
    memset(con_grid[CON_ROWS - 1], ' ', CON_COLS);
    con_grid[CON_ROWS - 1][CON_COLS] = '\0';
    con_row = CON_ROWS - 1;
}

void console_clear(void)
{
    int32_t r;
    for (r = 0; r < CON_ROWS; r++) {
        memset(con_grid[r], ' ', CON_COLS);
        con_grid[r][CON_COLS] = '\0';
    }
    con_col = 0;
    con_row = 0;
}

void console_init(void)
{
    console_clear();
}

void console_putc(char c)
{
    /* Mirror to serial COM1 */
    serial_putc(c);

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
        }
        return;
    }
    if (c >= 32 && c <= 126) {
        con_grid[con_row][con_col] = c;
        con_col++;
        if (con_col >= CON_COLS) {
            con_col = 0;
            con_row++;
            if (con_row >= CON_ROWS) {
                console_scroll();
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

void console_clear_row(int32_t row)
{
    if (row >= 0 && row < CON_ROWS) {
        memset(con_grid[row], ' ', CON_COLS);
        con_grid[row][CON_COLS] = '\0';
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

    /* Terminal window (/dev/console - /bin/init & /bin/sh) */
    vga_fill_rect(10, 24, 620, 366, COLOR_BLACK);
    vga_draw_rect(10, 24, 620, 366, COLOR_LIGHT_CYAN);
    vga_fill_rect(11, 25, 618, 14, COLOR_CYAN);
    vga_draw_string(16, 28, "/dev/console - Virtual Filesystem (/dev, /bin/init, /bin/sh)",
                    COLOR_BLACK, COLOR_CYAN, TRUE);

    /* Render terminal character grid (75 cols x 33 rows) */
    for (r = 0; r < CON_ROWS; r++) {
        int32_t py = 44 + r * 10;
        for (c = 0; c < CON_COLS; c++) {
            char ch = con_grid[r][c];
            if (ch != ' ' && ch != '\0') {
                vga_draw_char(16 + c * 8, py, ch, COLOR_LIGHT_GREEN, COLOR_BLACK, TRUE);
            }
        }
    }

    /* Terminal text cursor */
    if (con_row >= 0 && con_row < CON_ROWS && con_col >= 0 && con_col < CON_COLS) {
        vga_fill_rect(16 + con_col * 8, 44 + con_row * 10 + 7, 7, 2, COLOR_LIGHT_GREEN);
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
