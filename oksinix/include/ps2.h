/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - 8042 PS/2 Controller, Keyboard & Mouse
 */

#ifndef OKSINIX_PS2_H
#define OKSINIX_PS2_H

#include "types.h"

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_CMD_PORT     0x64

#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

struct key_event {
    uint8_t scancode;
    char    ascii;
    bool_t  released;
    bool_t  shift;
    bool_t  ctrl;
    bool_t  alt;
    bool_t  extended;
};

struct mouse_state {
    int32_t  x;
    int32_t  y;
    int32_t  dx;
    int32_t  dy;
    uint8_t  buttons;
    uint32_t packets;
};

/* PS/2 8042 Controller */
void    ps2_init(void);
int     ps2_wait_write(void);
int     ps2_wait_read(void);
void    ps2_write_cmd(uint8_t cmd);
void    ps2_write_data(uint8_t data);
uint8_t ps2_read_data(void);

/* PS/2 & USB Keyboard */
#define KBD_KEYMAP_US    0
#define KBD_KEYMAP_FR_CH 1

void        kbd_init(void);
void        kbd_set_keymap(int keymap);
int         kbd_get_keymap(void);
const char *kbd_get_keymap_name(void);
int         kbd_pop_event(struct key_event *ev);
uint32_t    kbd_get_irq_count(void);
void        kbd_inject_scancode(uint8_t scancode);
void        kbd_inject_usb_key(uint8_t hid_usage, char ascii, bool_t released,
                               bool_t shift, bool_t ctrl, bool_t alt);

/* PS/2 & USB Mouse */
void    mouse_init(int32_t max_width, int32_t max_height);
void    mouse_get_state(struct mouse_state *out);
void    mouse_inject_byte(uint8_t data);
void    mouse_inject_usb_report(int32_t dx, int32_t dy, uint8_t buttons);

#endif /* OKSINIX_PS2_H */
