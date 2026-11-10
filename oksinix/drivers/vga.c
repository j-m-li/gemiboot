/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - VGA Mode 12h (640x480x16) Framebuffer Driver
 */

#include "vga.h"
#include "multiboot.h"
#include "pci.h"
#include "io.h"
#include "string.h"
#include "serial.h"

/* VGA I/O ports */
#define VGA_AC_INDEX        0x3C0
#define VGA_AC_WRITE        0x3C0
#define VGA_MISC_WRITE      0x3C2
#define VGA_SEQ_INDEX       0x3C4
#define VGA_SEQ_DATA        0x3C5
#define VGA_DAC_WRITE_INDEX 0x3C8
#define VGA_DAC_DATA        0x3C9
#define VGA_GC_INDEX        0x3CE
#define VGA_GC_DATA         0x3CF
#define VGA_CRTC_INDEX      0x3D4
#define VGA_CRTC_DATA       0x3D5
#define VGA_INSTAT_READ     0x3DA

/* Bochs / QEMU VBE DISPI I/O ports & registers */
#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF
#define VBE_DISPI_INDEX_ID     0x00
#define VBE_DISPI_INDEX_XRES   0x01
#define VBE_DISPI_INDEX_YRES   0x02
#define VBE_DISPI_INDEX_BPP    0x03
#define VBE_DISPI_INDEX_ENABLE 0x04
#define VBE_DISPI_ENABLED      0x01
#define VBE_DISPI_LFB_ENABLED  0x40

static uint8_t vga_backbuffer[VGA_FB_SIZE];
static uint8_t vga_plane_buf[VGA_PLANE_SIZE];
static volatile uint8_t * const vga_vram = (volatile uint8_t *)VGA_FB_PHYS;

/* Linear Framebuffer (VESA VBE 2.0 / Multiboot / Bochs VBE) state */
static bool_t            lfb_active  = FALSE;
static volatile uint8_t *lfb_base    = NULL;
static uint32_t          lfb_phys    = 0;
static uint32_t          lfb_pitch   = 0;
static uint32_t          lfb_width   = 0;
static uint32_t          lfb_height  = 0;
static uint8_t           lfb_bpp     = 0;
static uint8_t           lfb_red_pos = 16;

static int32_t dirty_y_min = 0;
static int32_t dirty_y_max = VGA_HEIGHT - 1;

/* Standard VGA Mode 12h (640x480x16 4-plane planar) register values */
static const uint8_t mode12h_seq[5] = {
    0x03, 0x01, 0x0F, 0x00, 0x06
};

static const uint8_t mode12h_crtc[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0x0B, 0x3E,
    0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xEA, 0x0C, 0xDF, 0x28, 0x00, 0xE7, 0x04, 0xE3,
    0xFF
};

static const uint8_t mode12h_gc[9] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0F,
    0xFF
};

static const uint8_t mode12h_ac[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x01, 0x00, 0x0F, 0x00, 0x00
};

/* Base 16-color palette (6-bit RGB values: 0..63) */
static const uint8_t base_palette[16][3] = {
    {  0,  0,  0 }, /* 0: Black */
    {  0,  0, 42 }, /* 1: Blue */
    {  0, 42,  0 }, /* 2: Green */
    {  0, 42, 42 }, /* 3: Cyan */
    { 42,  0,  0 }, /* 4: Red */
    { 42,  0, 42 }, /* 5: Magenta */
    { 42, 21,  0 }, /* 6: Brown */
    { 42, 42, 42 }, /* 7: Light Gray */
    { 21, 21, 21 }, /* 8: Dark Gray */
    { 21, 21, 63 }, /* 9: Light Blue */
    { 21, 63, 21 }, /* 10: Light Green */
    { 21, 63, 63 }, /* 11: Light Cyan */
    { 63, 21, 21 }, /* 12: Light Red */
    { 63, 21, 63 }, /* 13: Light Magenta */
    { 63, 63, 21 }, /* 14: Yellow */
    { 63, 63, 63 }  /* 15: White */
};

/* Public domain 8x8 bitmap font for printable ASCII 32..127 */
static const uint8_t font8x8_basic[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 32 ' ' */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* 33 '!' */
    {0x6C,0x6C,0x24,0x00,0x00,0x00,0x00,0x00}, /* 34 '"' */
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* 35 '#' */
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, /* 36 '$' */
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, /* 37 '%' */
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, /* 38 '&' */
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, /* 39 ''' */
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, /* 40 '(' */
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, /* 41 ')' */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* 42 '*' */
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, /* 43 '+' */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* 44 ',' */
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /* 45 '-' */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* 46 '.' */
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, /* 47 '/' */
    {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00}, /* 48 '0' */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, /* 49 '1' */
    {0x7C,0xC6,0x06,0x1C,0x30,0x66,0xFE,0x00}, /* 50 '2' */
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, /* 51 '3' */
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, /* 52 '4' */
    {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00}, /* 53 '5' */
    {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, /* 54 '6' */
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, /* 55 '7' */
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, /* 56 '8' */
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, /* 57 '9' */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, /* 58 ':' */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, /* 59 ';' */
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, /* 60 '<' */
    {0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00}, /* 61 '=' */
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, /* 62 '>' */
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, /* 63 '?' */
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00}, /* 64 '@' */
    {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, /* 65 'A' */
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, /* 66 'B' */
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, /* 67 'C' */
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, /* 68 'D' */
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, /* 69 'E' */
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, /* 70 'F' */
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00}, /* 71 'G' */
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, /* 72 'H' */
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* 73 'I' */
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, /* 74 'J' */
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, /* 75 'K' */
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, /* 76 'L' */
    {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, /* 77 'M' */
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, /* 78 'N' */
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, /* 79 'O' */
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, /* 80 'P' */
    {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06}, /* 81 'Q' */
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, /* 82 'R' */
    {0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00}, /* 83 'S' */
    {0x7E,0x5A,0x18,0x18,0x18,0x18,0x3C,0x00}, /* 84 'T' */
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, /* 85 'U' */
    {0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, /* 86 'V' */
    {0xC6,0xC6,0xD6,0xFE,0xFE,0xEE,0xC6,0x00}, /* 87 'W' */
    {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00}, /* 88 'X' */
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00}, /* 89 'Y' */
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00}, /* 90 'Z' */
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, /* 91 '[' */
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, /* 92 '\' */
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, /* 93 ']' */
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, /* 94 '^' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* 95 '_' */
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, /* 96 '`' */
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, /* 97 'a' */
    {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00}, /* 98 'b' */
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00}, /* 99 'c' */
    {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00}, /* 100 'd' */
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00}, /* 101 'e' */
    {0x3C,0x66,0x60,0xF8,0x60,0x60,0xF0,0x00}, /* 102 'f' */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8}, /* 103 'g' */
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, /* 104 'h' */
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, /* 105 'i' */
    {0x06,0x00,0x06,0x06,0x06,0x66,0x66,0x3C}, /* 106 'j' */
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, /* 107 'k' */
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* 108 'l' */
    {0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0x00}, /* 109 'm' */
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00}, /* 110 'n' */
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, /* 111 'o' */
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0}, /* 112 'p' */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E}, /* 113 'q' */
    {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00}, /* 114 'r' */
    {0x00,0x00,0x7E,0xC0,0x7C,0x06,0xFC,0x00}, /* 115 's' */
    {0x30,0x30,0xFC,0x30,0x30,0x36,0x1C,0x00}, /* 116 't' */
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, /* 117 'u' */
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, /* 118 'v' */
    {0x00,0x00,0xC6,0xD6,0xFE,0xFE,0x6C,0x00}, /* 119 'w' */
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, /* 120 'x' */
    {0x00,0x00,0xC6,0xC6,0xC6,0x7E,0x06,0xFC}, /* 121 'y' */
    {0x00,0x00,0xFE,0x8C,0x18,0x32,0xFE,0x00}, /* 122 'z' */
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, /* 123 '{' */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* 124 '|' */
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, /* 125 '}' */
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, /* 126 '~' */
    {0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0xFE,0x00}  /* 127 DEL */
};

/* 12x12 Mouse cursor bitmap: 0=transparent, 1=border(black), 2=fill */
static const uint8_t cursor_sprite[12][12] = {
    { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0 },
    { 1, 2, 2, 2, 2, 1, 1, 1, 0, 0, 0, 0 },
    { 1, 2, 1, 1, 2, 2, 1, 0, 0, 0, 0, 0 },
    { 1, 1, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 1, 2, 2, 1, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0 }
};

static void mark_dirty(int32_t y0, int32_t y1)
{
    if (y0 < 0) {
        y0 = 0;
    }
    if (y1 >= VGA_HEIGHT) {
        y1 = VGA_HEIGHT - 1;
    }
    if (y0 < dirty_y_min) {
        dirty_y_min = y0;
    }
    if (y1 > dirty_y_max) {
        dirty_y_max = y1;
    }
}

static void vga_program_mode12h(void)
{
    uint32_t i;

    /* Assert Sequencer synchronous reset before changing clocks/timings */
    outb(VGA_SEQ_INDEX, 0x00);
    outb(VGA_SEQ_DATA, 0x01);

    /* Write Miscellaneous Output register (480 lines, 25 MHz clock) */
    outb(VGA_MISC_WRITE, 0xE3);

    /* Write Sequencer registers 1..4, then release synchronous reset (SR00 = 0x03) */
    for (i = 1; i < 5; i++) {
        outb(VGA_SEQ_INDEX, (uint8_t)i);
        outb(VGA_SEQ_DATA, mode12h_seq[i]);
    }
    outb(VGA_SEQ_INDEX, 0x00);
    outb(VGA_SEQ_DATA, mode12h_seq[0]);

    /* Unlock CRTC registers 0..7 (clear bit 7 of index 0x11) */
    outb(VGA_CRTC_INDEX, 0x03);
    outb(VGA_CRTC_DATA, (uint8_t)(inb(VGA_CRTC_DATA) | 0x80U));
    outb(VGA_CRTC_INDEX, 0x11);
    outb(VGA_CRTC_DATA, (uint8_t)(inb(VGA_CRTC_DATA) & ~0x80U));

    /* Write CRTC registers */
    for (i = 0; i < 25; i++) {
        outb(VGA_CRTC_INDEX, (uint8_t)i);
        outb(VGA_CRTC_DATA, mode12h_crtc[i]);
    }

    /* Write Graphics Controller registers */
    for (i = 0; i < 9; i++) {
        outb(VGA_GC_INDEX, (uint8_t)i);
        outb(VGA_GC_DATA, mode12h_gc[i]);
    }

    /* Write Attribute Controller registers (identity map 0..15) */
    for (i = 0; i < 21; i++) {
        (void)inb(VGA_INSTAT_READ);
        outb(VGA_AC_INDEX, (uint8_t)i);
        outb(VGA_AC_WRITE, mode12h_ac[i]);
    }

    /* Lock palette and unblank display */
    (void)inb(VGA_INSTAT_READ);
    outb(VGA_AC_INDEX, 0x20);

    /* Program first 16 DAC palette entries */
    outb(VGA_DAC_WRITE_INDEX, 0);
    for (i = 0; i < 16; i++) {
        outb(VGA_DAC_DATA, base_palette[i][0]);
        outb(VGA_DAC_DATA, base_palette[i][1]);
        outb(VGA_DAC_DATA, base_palette[i][2]);
    }
}

/* 24-bit RGB888 lookup table for Linear Framebuffer modes */
static const uint32_t lfb_palette32[16] = {
    0x00000000UL, /* 0: Black */
    0x000000AAUL, /* 1: Blue */
    0x0000AA00UL, /* 2: Green */
    0x0000AAAAUL, /* 3: Cyan */
    0x00AA0000UL, /* 4: Red */
    0x00AA00AAUL, /* 5: Magenta */
    0x00AA5500UL, /* 6: Brown */
    0x00AAAAAAUL, /* 7: Light Gray */
    0x00555555UL, /* 8: Dark Gray */
    0x005555FFUL, /* 9: Light Blue */
    0x0055FF55UL, /* 10: Light Green */
    0x0055FFFFUL, /* 11: Light Cyan */
    0x00FF5555UL, /* 12: Light Red */
    0x00FF55FFUL, /* 13: Light Magenta */
    0x00FFFF55UL, /* 14: Yellow */
    0x00FFFFFFUL  /* 15: White */
};

static int vga_activate_lfb(uint32_t phys, uint32_t width, uint32_t height,
                            uint32_t pitch, uint8_t bpp, uint8_t red_pos,
                            const char *source)
{
    uint32_t y;

    if (phys == 0 || phys == VGA_FB_PHYS ||
        width < (uint32_t)VGA_WIDTH || height < (uint32_t)VGA_HEIGHT ||
        (bpp != 32U && bpp != 24U && bpp != 16U && bpp != 15U)) {
        return -1;
    }
    if (pitch == 0) {
        pitch = width * ((uint32_t)(bpp + 7U) / 8U);
    }

    lfb_active  = TRUE;
    lfb_phys    = phys;
    lfb_base    = (volatile uint8_t *)(uintptr_t)phys;
    lfb_width   = width;
    lfb_height  = height;
    lfb_pitch   = pitch;
    lfb_bpp     = bpp;
    lfb_red_pos = (red_pos == 0U || red_pos == 16U) ? red_pos : 16U;

    for (y = 0; y < lfb_height; y++) {
        memset((void *)(uintptr_t)(phys + y * lfb_pitch), 0, lfb_pitch);
    }

    kprintf("[FB] Linear Framebuffer active via %s (%ux%ux%u at 0x%08x, pitch=%u)\n",
            source, lfb_width, lfb_height, (uint32_t)lfb_bpp, lfb_phys, lfb_pitch);
    return 0;
}

static int vga_try_vbe_mode_info(uint32_t mode_info_addr, const char *source)
{
    const uint8_t *mi = (const uint8_t *)(uintptr_t)mode_info_addr;
    uint16_t attrs = (uint16_t)((uint16_t)mi[0] | ((uint16_t)mi[1] << 8));
    uint16_t pitch = (uint16_t)((uint16_t)mi[16] | ((uint16_t)mi[17] << 8));
    uint16_t width = (uint16_t)((uint16_t)mi[18] | ((uint16_t)mi[19] << 8));
    uint16_t height = (uint16_t)((uint16_t)mi[20] | ((uint16_t)mi[21] << 8));
    uint8_t  bpp = mi[25];
    uint8_t  red_pos = mi[32];
    uint32_t phys = (uint32_t)mi[40] |
                    ((uint32_t)mi[41] << 8) |
                    ((uint32_t)mi[42] << 16) |
                    ((uint32_t)mi[43] << 24);

    if ((attrs & 0x0091U) != 0x0091U) {
        return -1;
    }
    return vga_activate_lfb(phys, width, height, pitch, bpp, red_pos, source);
}

static void vbe_dispi_write(uint16_t index, uint16_t val)
{
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, val);
}

static uint16_t vbe_dispi_read(uint16_t index)
{
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static int vga_try_bochs_vbe_lfb(void)
{
    struct pci_device vga_pci;
    uint16_t dispi_id;

    if (pci_find_class(0x03U, 0x00U, 0x00U, &vga_pci) < 0 || vga_pci.bar0_mmio == 0) {
        return -1;
    }
    if ((vga_pci.vendor_id != 0x1234U || vga_pci.device_id != 0x1111U) &&
        (vga_pci.vendor_id != 0x80EEU || vga_pci.device_id != 0xBEEFU)) {
        return -1;
    }

    dispi_id = vbe_dispi_read(VBE_DISPI_INDEX_ID);
    if ((dispi_id & 0xFFF0U) != 0xB0C0U) {
        return -1;
    }

    vbe_dispi_write(VBE_DISPI_INDEX_ENABLE, 0x00);
    vbe_dispi_write(VBE_DISPI_INDEX_XRES, (uint16_t)VGA_WIDTH);
    vbe_dispi_write(VBE_DISPI_INDEX_YRES, (uint16_t)VGA_HEIGHT);
    vbe_dispi_write(VBE_DISPI_INDEX_BPP, 32);
    vbe_dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    return vga_activate_lfb(vga_pci.bar0_mmio, VGA_WIDTH, VGA_HEIGHT,
                            (uint32_t)VGA_WIDTH * 4U, 32U, 16U, "PCI Bochs/QEMU VBE");
}

void vga_init(uint32_t mbi_addr)
{
    lfb_active = FALSE;

    /* 1. Check Multiboot v1 Framebuffer or VBE Mode Info */
    if (mbi_addr != 0) {
        const struct multiboot_info *mbi = (const struct multiboot_info *)(uintptr_t)mbi_addr;
        if ((mbi->flags & MULTIBOOT_INFO_FRAMEBUFFER_INFO) != 0 &&
            mbi->framebuffer_type == 1U && mbi->framebuffer_addr_hi == 0) {
            (void)vga_activate_lfb(mbi->framebuffer_addr_lo,
                                   mbi->framebuffer_width,
                                   mbi->framebuffer_height,
                                   mbi->framebuffer_pitch,
                                   mbi->framebuffer_bpp,
                                   mbi->color_info[0],
                                   "Multiboot Framebuffer");
        }
        if (!lfb_active && (mbi->flags & MULTIBOOT_INFO_VBE_INFO) != 0 &&
            mbi->vbe_mode_info != 0) {
            (void)vga_try_vbe_mode_info(mbi->vbe_mode_info, "Multiboot VBE");
        }
    }

    /* 2. Check VESA VBE 2.0 ModeInfoBlock populated by boot/mbr.S at 0x00007E00 */
    if (!lfb_active &&
        *(const uint16_t *)0x00007E12UL == (uint16_t)VGA_WIDTH &&
        *(const uint16_t *)0x00007E14UL == (uint16_t)VGA_HEIGHT) {
        (void)vga_try_vbe_mode_info(0x00007E00UL, "VESA VBE 2.0 BIOS");
    }

    /* 3. Check PCI Bochs/QEMU VBE Linear Framebuffer (e.g. when booted via -kernel) */
    if (!lfb_active) {
        (void)vga_try_bochs_vbe_lfb();
    }

    /* 4. Fallback to standard VGA Mode 12h 4-plane planar (0xA0000) */
    if (!lfb_active) {
        vga_program_mode12h();
    }

    vga_clear(COLOR_BLACK);
    vga_present();

    kprintf("[VGA] Mode 12h framebuffer initialized (%dx%dx16 at 0x%08x)\n",
            VGA_WIDTH, VGA_HEIGHT,
            (uint32_t)(lfb_active ? lfb_phys : VGA_FB_PHYS));
}

void vga_get_fb_info(struct fb_info *out)
{
    if (out == NULL) {
        return;
    }
    if (lfb_active) {
        out->is_lfb    = TRUE;
        out->phys_addr = lfb_phys;
        out->width     = lfb_width;
        out->height    = lfb_height;
        out->pitch     = lfb_pitch;
        out->bpp       = lfb_bpp;
    } else {
        out->is_lfb    = FALSE;
        out->phys_addr = VGA_FB_PHYS;
        out->width     = VGA_WIDTH;
        out->height    = VGA_HEIGHT;
        out->pitch     = VGA_WIDTH / 8U;
        out->bpp       = 4U;
    }
}

void vga_clear(uint8_t color)
{
    memset(vga_backbuffer, color & 0x0FU, VGA_FB_SIZE);
    dirty_y_min = 0;
    dirty_y_max = VGA_HEIGHT - 1;
}

void vga_putpixel(int32_t x, int32_t y, uint8_t color)
{
    if (x >= 0 && x < VGA_WIDTH && y >= 0 && y < VGA_HEIGHT) {
        vga_backbuffer[y * VGA_WIDTH + x] = (uint8_t)(color & 0x0FU);
        mark_dirty(y, y);
    }
}

uint8_t vga_getpixel(int32_t x, int32_t y)
{
    if (x >= 0 && x < VGA_WIDTH && y >= 0 && y < VGA_HEIGHT) {
        return vga_backbuffer[y * VGA_WIDTH + x];
    }
    return 0;
}

uint8_t vga_getpixel_vram(int32_t x, int32_t y)
{
    uint8_t color = 0;
    uint32_t plane;
    uint32_t byte_off;
    uint8_t bit_mask;

    if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT) {
        return 0;
    }

    if (lfb_active) {
        uint32_t x_off = (lfb_width > (uint32_t)VGA_WIDTH) ? ((lfb_width - (uint32_t)VGA_WIDTH) / 2U) : 0U;
        uint32_t y_off = (lfb_height > (uint32_t)VGA_HEIGHT) ? ((lfb_height - (uint32_t)VGA_HEIGHT) / 2U) : 0U;
        volatile const uint8_t *row_ptr = lfb_base + ((uint32_t)y + y_off) * lfb_pitch;
        uint32_t px_x = (uint32_t)x + x_off;
        uint32_t i;

        if (lfb_bpp == 32U) {
            uint32_t raw = *((volatile const uint32_t *)(const void *)row_ptr + px_x) & 0x00FFFFFFUL;
            if (lfb_red_pos == 0U) {
                raw = ((raw & 0xFFU) << 16) | (raw & 0xFF00U) | ((raw >> 16) & 0xFFU);
            }
            for (i = 0; i < 16U; i++) {
                if (lfb_palette32[i] == raw) {
                    return (uint8_t)i;
                }
            }
        }
        return vga_backbuffer[y * VGA_WIDTH + x];
    }

    byte_off = (uint32_t)y * (VGA_WIDTH / 8U) + ((uint32_t)x >> 3);
    bit_mask = (uint8_t)(0x80U >> ((uint32_t)x & 7U));

    for (plane = 0; plane < 4U; plane++) {
        outb(VGA_GC_INDEX, 0x04);
        outb(VGA_GC_DATA, (uint8_t)plane);
        if ((vga_vram[byte_off] & bit_mask) != 0) {
            color |= (uint8_t)(1U << plane);
        }
    }
    outb(VGA_GC_INDEX, 0x04);
    outb(VGA_GC_DATA, 0x00);
    return color;
}

void vga_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t color)
{
    int32_t row, col;
    uint8_t c = (uint8_t)(color & 0x0FU);

    for (row = y; row < y + h; row++) {
        if (row < 0 || row >= VGA_HEIGHT) {
            continue;
        }
        for (col = x; col < x + w; col++) {
            if (col >= 0 && col < VGA_WIDTH) {
                vga_backbuffer[row * VGA_WIDTH + col] = c;
            }
        }
    }
    mark_dirty(y, y + h - 1);
}

void vga_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t color)
{
    int32_t i;
    for (i = 0; i < w; i++) {
        vga_putpixel(x + i, y, color);
        vga_putpixel(x + i, y + h - 1, color);
    }
    for (i = 0; i < h; i++) {
        vga_putpixel(x, y + i, color);
        vga_putpixel(x + w - 1, y + i, color);
    }
}

void vga_draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color)
{
    int32_t dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int32_t sx = (x0 < x1) ? 1 : -1;
    int32_t dy = (y1 > y0) ? -(y1 - y0) : -(y0 - y1);
    int32_t sy = (y0 < y1) ? 1 : -1;
    int32_t err = dx + dy;

    for (;;) {
        int32_t e2;
        vga_putpixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void vga_draw_char(int32_t x, int32_t y, char c, uint8_t fg, uint8_t bg, bool_t transparent)
{
    uint8_t idx;
    int32_t row, col;

    if (c < 32 || c > 127) {
        idx = (uint8_t)('?' - 32);
    } else {
        idx = (uint8_t)(c - 32);
    }

    for (row = 0; row < 8; row++) {
        uint8_t bits = font8x8_basic[idx][row];
        int32_t py = y + row;
        if (py < 0 || py >= VGA_HEIGHT) {
            continue;
        }
        for (col = 0; col < 8; col++) {
            int32_t px = x + col;
            if (px < 0 || px >= VGA_WIDTH) {
                continue;
            }
            if ((bits & (0x80U >> col)) != 0) {
                vga_backbuffer[py * VGA_WIDTH + px] = (uint8_t)(fg & 0x0FU);
            } else if (!transparent) {
                vga_backbuffer[py * VGA_WIDTH + px] = (uint8_t)(bg & 0x0FU);
            }
        }
    }
    mark_dirty(y, y + 7);
}

void vga_draw_string(int32_t x, int32_t y, const char *str, uint8_t fg, uint8_t bg, bool_t transparent)
{
    int32_t cur_x = x;
    int32_t cur_y = y;

    while (*str != '\0') {
        if (*str == '\n') {
            cur_x = x;
            cur_y += 10;
        } else {
            vga_draw_char(cur_x, cur_y, *str, fg, bg, transparent);
            cur_x += 8;
        }
        str++;
    }
}

void vga_draw_cursor(int32_t x, int32_t y, uint8_t buttons)
{
    int32_t r, c;
    uint8_t fill_color = COLOR_WHITE;

    if ((buttons & MOUSE_BTN_LEFT) != 0) {
        fill_color = COLOR_LIGHT_RED;
    } else if ((buttons & MOUSE_BTN_RIGHT) != 0) {
        fill_color = COLOR_LIGHT_GREEN;
    } else if ((buttons & MOUSE_BTN_MIDDLE) != 0) {
        fill_color = COLOR_YELLOW;
    }

    for (r = 0; r < 12; r++) {
        for (c = 0; c < 12; c++) {
            uint8_t px = cursor_sprite[r][c];
            if (px == 1) {
                vga_putpixel(x + c, y + r, COLOR_BLACK);
            } else if (px == 2) {
                vga_putpixel(x + c, y + r, fill_color);
            }
        }
    }
}

void vga_present(void)
{
    uint32_t plane;
    int32_t y, byte_x;
    int32_t y_start = dirty_y_min;
    int32_t y_end   = dirty_y_max;
    uint32_t dword_offset;
    uint32_t dword_count;

    if (y_start > y_end) {
        return;
    }
    if (y_start < 0) {
        y_start = 0;
    }
    if (y_end >= VGA_HEIGHT) {
        y_end = VGA_HEIGHT - 1;
    }

    if (lfb_active) {
        uint32_t x_off = (lfb_width > (uint32_t)VGA_WIDTH) ? ((lfb_width - (uint32_t)VGA_WIDTH) / 2U) : 0U;
        uint32_t y_off = (lfb_height > (uint32_t)VGA_HEIGHT) ? ((lfb_height - (uint32_t)VGA_HEIGHT) / 2U) : 0U;

        for (y = y_start; y <= y_end; y++) {
            const uint8_t *row_src = &vga_backbuffer[y * VGA_WIDTH];
            volatile uint8_t *row_dst = lfb_base + ((uint32_t)y + y_off) * lfb_pitch;
            int32_t x;

            if (lfb_bpp == 32U) {
                volatile uint32_t *dst32 = (volatile uint32_t *)(void *)row_dst + x_off;
                if (lfb_red_pos == 0U) {
                    for (x = 0; x < VGA_WIDTH; x++) {
                        uint32_t rgb = lfb_palette32[row_src[x] & 0x0FU];
                        dst32[x] = ((rgb & 0xFFU) << 16) | (rgb & 0xFF00U) | ((rgb >> 16) & 0xFFU);
                    }
                } else {
                    for (x = 0; x < VGA_WIDTH; x++) {
                        dst32[x] = lfb_palette32[row_src[x] & 0x0FU];
                    }
                }
            } else if (lfb_bpp == 24U) {
                volatile uint8_t *dst24 = row_dst + x_off * 3U;
                for (x = 0; x < VGA_WIDTH; x++) {
                    uint32_t rgb = lfb_palette32[row_src[x] & 0x0FU];
                    if (lfb_red_pos == 0U) {
                        dst24[x * 3 + 0] = (uint8_t)((rgb >> 16) & 0xFFU);
                        dst24[x * 3 + 1] = (uint8_t)((rgb >> 8) & 0xFFU);
                        dst24[x * 3 + 2] = (uint8_t)(rgb & 0xFFU);
                    } else {
                        dst24[x * 3 + 0] = (uint8_t)(rgb & 0xFFU);
                        dst24[x * 3 + 1] = (uint8_t)((rgb >> 8) & 0xFFU);
                        dst24[x * 3 + 2] = (uint8_t)((rgb >> 16) & 0xFFU);
                    }
                }
            } else if (lfb_bpp == 16U || lfb_bpp == 15U) {
                volatile uint16_t *dst16 = (volatile uint16_t *)(void *)row_dst + x_off;
                for (x = 0; x < VGA_WIDTH; x++) {
                    uint32_t rgb = lfb_palette32[row_src[x] & 0x0FU];
                    uint16_t r5 = (uint16_t)((rgb >> 19) & 0x1FU);
                    uint16_t g6 = (uint16_t)((rgb >> 10) & 0x3FU);
                    uint16_t b5 = (uint16_t)((rgb >> 3) & 0x1FU);
                    dst16[x] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
                }
            }
        }
        dirty_y_min = VGA_HEIGHT;
        dirty_y_max = -1;
        return;
    }

    dword_offset = ((uint32_t)y_start * (VGA_WIDTH / 8U)) / 4U;
    dword_count  = ((uint32_t)(y_end - y_start + 1) * (VGA_WIDTH / 8U)) / 4U;

    /* Write mode 0, all 4 bitplanes updated sequentially */
    outb(VGA_GC_INDEX, 0x05);
    outb(VGA_GC_DATA, 0x00);

    for (plane = 0; plane < 4U; plane++) {
        uint8_t plane_mask = (uint8_t)(1U << plane);
        const uint32_t *src32;
        volatile uint32_t *dst32;
        uint32_t i;

        /* Select bitplane in Sequencer Map Mask register (index 2) */
        outb(VGA_SEQ_INDEX, 0x02);
        outb(VGA_SEQ_DATA, plane_mask);

        for (y = y_start; y <= y_end; y++) {
            const uint8_t *row_src = &vga_backbuffer[y * VGA_WIDTH];
            uint8_t *row_dst = &vga_plane_buf[y * (VGA_WIDTH / 8)];

            for (byte_x = 0; byte_x < (VGA_WIDTH / 8); byte_x++) {
                const uint8_t *p = &row_src[byte_x * 8];
                uint8_t packed = 0;
                if ((p[0] & plane_mask) != 0) packed |= 0x80U;
                if ((p[1] & plane_mask) != 0) packed |= 0x40U;
                if ((p[2] & plane_mask) != 0) packed |= 0x20U;
                if ((p[3] & plane_mask) != 0) packed |= 0x10U;
                if ((p[4] & plane_mask) != 0) packed |= 0x08U;
                if ((p[5] & plane_mask) != 0) packed |= 0x04U;
                if ((p[6] & plane_mask) != 0) packed |= 0x02U;
                if ((p[7] & plane_mask) != 0) packed |= 0x01U;
                row_dst[byte_x] = packed;
            }
        }

        src32 = (const uint32_t *)(const void *)vga_plane_buf + dword_offset;
        dst32 = (volatile uint32_t *)VGA_FB_PHYS + dword_offset;
        for (i = 0; i < dword_count; i++) {
            dst32[i] = src32[i];
        }
    }

    /* Restore default Map Mask (all planes enabled) */
    outb(VGA_SEQ_INDEX, 0x02);
    outb(VGA_SEQ_DATA, 0x0F);

    dirty_y_min = VGA_HEIGHT;
    dirty_y_max = -1;
}

uint8_t *vga_get_backbuffer(void)
{
    dirty_y_min = 0;
    dirty_y_max = VGA_HEIGHT - 1;
    return vga_backbuffer;
}
