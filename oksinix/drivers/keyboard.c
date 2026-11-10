/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - PS/2 & USB Keyboard driver (US QWERTY & Swiss French fr_CH QWERTZ)
 */

#include "ps2.h"
#include "idt.h"
#include "pic.h"
#include "io.h"
#include "serial.h"

#define KBD_RING_SIZE 64
#define USB_HID_MAX   104

static struct key_event kbd_ring[KBD_RING_SIZE];
static volatile uint32_t kbd_head = 0;
static volatile uint32_t kbd_tail = 0;
static volatile uint32_t kbd_irq_total = 0;

static int    current_keymap = KBD_KEYMAP_FR_CH;
static bool_t shift_pressed  = FALSE;
static bool_t ctrl_pressed   = FALSE;
static bool_t alt_pressed    = FALSE;
static bool_t altgr_pressed  = FALSE;
static bool_t caps_lock      = FALSE;
static bool_t e0_prefix      = FALSE;

/* =========================================================================
 * 1. US QWERTY Scancode Set 1 tables (PS/2)
 * ========================================================================= */
static const char ps2_us_normal[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t','q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'','`',
    0,   '\\','z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1', '2',
    '3', '0', '.', 0,   0,   '\\',0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0
};

static const char ps2_us_shift[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t','Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1', '2',
    '3', '0', '.', 0,   0,   '|', 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0
};

/* =========================================================================
 * 2. Swiss French (fr_CH) QWERTZ Scancode Set 1 tables (PS/2)
 * ========================================================================= */
static const char ps2_fr_ch_normal[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\'','^', '\b',
    '\t','q', 'w', 'e', 'r', 't', 'z', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'','`',
    0,   '$', 'y', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0,
    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1', '2',
    '3', '0', '.', 0,   0,   '<', 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0
};

static const char ps2_fr_ch_shift[128] = {
    0,   27,  '+', '"', '*', 'c', '%', '&', '/', '(', ')', '=', '?', '`', '\b',
    '\t','Q', 'W', 'E', 'R', 'T', 'Z', 'U', 'I', 'O', 'P', '{', '!', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '{', '~',
    0,   '}', 'Y', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0,
    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1', '2',
    '3', '0', '.', 0,   0,   '>', 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0
};

static const char ps2_fr_ch_altgr[128] = {
    0,   0,   '|', '@', '#', '^', '[', ']', '|', '{', '}', 0,   '`', '~', 0,
    0,   0,   0,   'E', 0,   0,   0,   0,   0,   0,   0,   '[', ']', 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   '[', '{', '~',
    0,   '}', 0,   0,   0,   0,   0,   0,   0,   '<', '>', 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   '\\',0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0
};

/* =========================================================================
 * 3. USB HID Usage ID (0x00..0x67) tables for US QWERTY & Swiss French fr_CH
 * ========================================================================= */
static const char usb_hid_us_normal[USB_HID_MAX] = {
    0,   0,   0,   0,   'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '1', '2',
    '3', '4', '5', '6', '7', '8', '9', '0', '\n', 27, '\b','\t',' ', '-', '=', '[',
    ']', '\\','\\',';', '\'','`', ',', '.', '/', 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   127, 0,   0,   0,
    0,   0,   0,   0,   '/', '*', '-', '+', '\n','1', '2', '3', '4', '5', '6', '7',
    '8', '9', '0', '.', '\\',0,   0,   0
};

static const char usb_hid_us_shift[USB_HID_MAX] = {
    0,   0,   0,   0,   'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
    'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '!', '@',
    '#', '$', '%', '^', '&', '*', '(', ')', '\n', 27, '\b','\t',' ', '_', '+', '{',
    '}', '|', '|', ':', '"', '~', '<', '>', '?', 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   127, 0,   0,   0,
    0,   0,   0,   0,   '/', '*', '-', '+', '\n','1', '2', '3', '4', '5', '6', '7',
    '8', '9', '0', '.', '|', 0,   0,   0
};

static const char usb_hid_fr_ch_normal[USB_HID_MAX] = {
    0,   0,   0,   0,   'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'z', 'y', '1', '2',
    '3', '4', '5', '6', '7', '8', '9', '0', '\n', 27, '\b','\t',' ', '\'','^', '[',
    ']', '$', '$', ';', '\'','`', ',', '.', '-', 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   127, 0,   0,   0,
    0,   0,   0,   0,   '/', '*', '-', '+', '\n','1', '2', '3', '4', '5', '6', '7',
    '8', '9', '0', '.', '<', 0,   0,   0
};

static const char usb_hid_fr_ch_shift[USB_HID_MAX] = {
    0,   0,   0,   0,   'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
    'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Z', 'Y', '+', '"',
    '*', 'c', '%', '&', '/', '(', ')', '=', '\n', 27, '\b','\t',' ', '?', '`', '{',
    '!', '}', '}', ':', '{', '~', ';', ':', '_', 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   127, 0,   0,   0,
    0,   0,   0,   0,   '/', '*', '-', '+', '\n','1', '2', '3', '4', '5', '6', '7',
    '8', '9', '0', '.', '>', 0,   0,   0
};

static const char usb_hid_fr_ch_altgr[USB_HID_MAX] = {
    0,   0,   0,   0,   0,   0,   0,   0,   'E', 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   '|', '@',
    '#', '^', '[', ']', '|', '{', '}', 0,   0,   0,   0,   0,   0,   '`', '~', '[',
    ']', '}', '}', '[', '{', '~', '<', '>', 0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   '\\',0,   0,   0
};

static void kbd_push_event(const struct key_event *ev)
{
    uint32_t next = (kbd_head + 1U) % KBD_RING_SIZE;
    if (next != kbd_tail) {
        kbd_ring[kbd_head] = *ev;
        kbd_head = next;
    }
}

void kbd_set_keymap(int keymap)
{
    if (keymap == KBD_KEYMAP_US || keymap == KBD_KEYMAP_FR_CH) {
        current_keymap = keymap;
        kprintf("[KBD] Keymap set to %s\n", kbd_get_keymap_name());
    }
}

int kbd_get_keymap(void)
{
    return current_keymap;
}

const char *kbd_get_keymap_name(void)
{
    return (current_keymap == KBD_KEYMAP_FR_CH) ? "fr_CH" : "us";
}

void kbd_inject_scancode(uint8_t scancode)
{
    struct key_event ev;
    uint8_t key;
    bool_t released;
    char ch = 0;

    kbd_irq_total++;

    /* Ignore ACK / resend bytes */
    if (scancode == 0xFAU || scancode == 0xFEU) {
        return;
    }

    if (scancode == 0xE0U) {
        e0_prefix = TRUE;
        return;
    }

    released = ((scancode & 0x80U) != 0) ? TRUE : FALSE;
    key = (uint8_t)(scancode & 0x7FU);

    /* Update modifier states */
    if (key == 0x2AU || key == 0x36U) {
        /* Left or Right Shift */
        shift_pressed = !released;
    } else if (key == 0x1DU) {
        /* Left or Right Ctrl */
        ctrl_pressed = !released;
    } else if (key == 0x38U) {
        /* Left Alt or Right Alt (AltGr when e0_prefix is set) */
        alt_pressed = !released;
        if (e0_prefix) {
            altgr_pressed = !released;
        }
    } else if (key == 0x3AU && !released) {
        /* Caps Lock toggle */
        caps_lock = !caps_lock;
    }

    if (key < 128U) {
        const char *norm_tbl  = (current_keymap == KBD_KEYMAP_FR_CH) ? ps2_fr_ch_normal : ps2_us_normal;
        const char *shift_tbl = (current_keymap == KBD_KEYMAP_FR_CH) ? ps2_fr_ch_shift  : ps2_us_shift;
        bool_t use_shift = shift_pressed;
        char base_ch = norm_tbl[key];

        if (base_ch >= 'a' && base_ch <= 'z' && caps_lock) {
            use_shift = !use_shift;
        }
        if (current_keymap == KBD_KEYMAP_FR_CH && (altgr_pressed || alt_pressed) &&
            ps2_fr_ch_altgr[key] != 0) {
            ch = ps2_fr_ch_altgr[key];
        } else {
            ch = use_shift ? shift_tbl[key] : norm_tbl[key];
        }
    }

    ev.scancode = key;
    ev.ascii    = ch;
    ev.released = released;
    ev.shift    = shift_pressed;
    ev.ctrl     = ctrl_pressed;
    ev.alt      = alt_pressed;
    ev.extended = e0_prefix;

    e0_prefix = FALSE;
    kbd_push_event(&ev);

    if (!released && ch >= 32 && ch <= 126) {
        kprintf("[KBD] key press scancode=0x%02x ascii='%c'\n", (uint32_t)key, ch);
    } else if (!released && ch == '\n') {
        kprintf("[KBD] key press scancode=0x%02x ascii=ENTER\n", (uint32_t)key);
    }
}

void kbd_inject_usb_key(uint8_t hid_usage, char ascii, bool_t released,
                        bool_t shift, bool_t ctrl, bool_t alt)
{
    struct key_event ev;
    char ch = ascii;

    kbd_irq_total++;

    if (hid_usage == 0x39U && !released) {
        caps_lock = !caps_lock;
    }

    if (ch == 0 && hid_usage < USB_HID_MAX) {
        const char *norm_tbl  = (current_keymap == KBD_KEYMAP_FR_CH) ? usb_hid_fr_ch_normal : usb_hid_us_normal;
        const char *shift_tbl = (current_keymap == KBD_KEYMAP_FR_CH) ? usb_hid_fr_ch_shift  : usb_hid_us_shift;
        bool_t use_shift = shift;
        char base_ch = norm_tbl[hid_usage];

        if (base_ch >= 'a' && base_ch <= 'z' && caps_lock) {
            use_shift = !use_shift;
        }
        if (current_keymap == KBD_KEYMAP_FR_CH && alt && usb_hid_fr_ch_altgr[hid_usage] != 0) {
            ch = usb_hid_fr_ch_altgr[hid_usage];
        } else {
            ch = use_shift ? shift_tbl[hid_usage] : norm_tbl[hid_usage];
        }
    }

    ev.scancode = hid_usage;
    ev.ascii    = ch;
    ev.released = released;
    ev.shift    = shift;
    ev.ctrl     = ctrl;
    ev.alt      = alt;
    ev.extended = FALSE;

    kbd_push_event(&ev);

    if (!released && ch >= 32 && ch <= 126) {
        kprintf("[USB-KBD] key press usage=0x%02x ascii='%c'\n", (uint32_t)hid_usage, ch);
    } else if (!released && ch == '\n') {
        kprintf("[USB-KBD] key press usage=0x%02x ascii=ENTER\n", (uint32_t)hid_usage);
    }
}

static void kbd_irq_handler(struct isr_regs *regs)
{
    uint8_t status;
    uint8_t data;
    (void)regs;

    status = inb(PS2_STATUS_PORT);
    if ((status & 0x01U) == 0) {
        return;
    }
    data = inb(PS2_DATA_PORT);
    if ((status & 0x20U) != 0) {
        /* Auxiliary (mouse) byte arrived while polling */
        mouse_inject_byte(data);
    } else {
        kbd_inject_scancode(data);
    }
}

int kbd_pop_event(struct key_event *ev)
{
    uint32_t flags;
    int has_event = 0;

    flags = cpu_eflags();
    cpu_cli();

    if (kbd_tail != kbd_head) {
        *ev = kbd_ring[kbd_tail];
        kbd_tail = (kbd_tail + 1U) % KBD_RING_SIZE;
        has_event = 1;
    }

    if ((flags & 0x200U) != 0) {
        cpu_sti();
    }
    return has_event;
}

uint32_t kbd_get_irq_count(void)
{
    return kbd_irq_total;
}

void kbd_init(void)
{
    kbd_head = 0;
    kbd_tail = 0;
    kbd_irq_total = 0;

    irq_register_handler(1, kbd_irq_handler);
    pic_unmask_irq(1);

    kprintf("[KBD] PS/2 keyboard initialized on IRQ1 (keymap=%s)\n", kbd_get_keymap_name());
}
