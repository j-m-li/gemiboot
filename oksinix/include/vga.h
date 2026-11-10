/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - VGA Mode 12h (640x480x16) Framebuffer Driver
 */

#ifndef OKSINIX_VGA_H
#define OKSINIX_VGA_H

#include "types.h"
#include "ps2.h"

#define VGA_WIDTH       640
#define VGA_HEIGHT      480
#define VGA_FB_SIZE     (VGA_WIDTH * VGA_HEIGHT)
#define VGA_PLANE_SIZE  ((VGA_WIDTH / 8) * VGA_HEIGHT)
#define VGA_FB_PHYS     0x000A0000UL

/* Standard 16 VGA palette indices */
#define COLOR_BLACK         0
#define COLOR_BLUE          1
#define COLOR_GREEN         2
#define COLOR_CYAN          3
#define COLOR_RED           4
#define COLOR_MAGENTA       5
#define COLOR_BROWN         6
#define COLOR_LIGHT_GRAY    7
#define COLOR_DARK_GRAY     8
#define COLOR_LIGHT_BLUE    9
#define COLOR_LIGHT_GREEN   10
#define COLOR_LIGHT_CYAN    11
#define COLOR_LIGHT_RED     12
#define COLOR_LIGHT_MAGENTA 13
#define COLOR_YELLOW        14
#define COLOR_WHITE         15

struct fb_info {
    bool_t   is_lfb;
    uint32_t phys_addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  bpp;
};

void    vga_init(uint32_t mbi_addr);
void    vga_get_fb_info(struct fb_info *out);
void    vga_clear(uint8_t color);
void    vga_putpixel(int32_t x, int32_t y, uint8_t color);
uint8_t vga_getpixel(int32_t x, int32_t y);
uint8_t vga_getpixel_vram(int32_t x, int32_t y);
void    vga_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t color);
void    vga_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t color);
void    vga_draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color);
void    vga_draw_char(int32_t x, int32_t y, char c, uint8_t fg, uint8_t bg, bool_t transparent);
void    vga_draw_string(int32_t x, int32_t y, const char *str, uint8_t fg, uint8_t bg, bool_t transparent);
void    vga_draw_cursor(int32_t x, int32_t y, uint8_t buttons);
void    vga_present(void);
uint8_t *vga_get_backbuffer(void);

#endif /* OKSINIX_VGA_H */
