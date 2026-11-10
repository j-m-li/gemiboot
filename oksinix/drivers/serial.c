/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Serial console (COM1) and kprintf
 */

#include <stdarg.h>
#include "serial.h"
#include "io.h"

void serial_init(void)
{
    outb(COM1_PORT + 1, 0x00); /* Disable all interrupts */
    outb(COM1_PORT + 3, 0x80); /* Enable DLAB (set baud rate divisor) */
    outb(COM1_PORT + 0, 0x01); /* Divisor 1 (115200 baud) lo byte */
    outb(COM1_PORT + 1, 0x00); /* Divisor hi byte */
    outb(COM1_PORT + 3, 0x03); /* 8 bits, no parity, one stop bit */
    outb(COM1_PORT + 2, 0xC7); /* Enable FIFO, clear them, 14-byte threshold */
    outb(COM1_PORT + 4, 0x0B); /* IRQs enabled, RTS/DSR set */
}

void serial_putc(char c)
{
    int timeout = 100000;
    if (c == '\n') {
        serial_putc('\r');
    }
    while ((inb(COM1_PORT + 5) & 0x20) == 0 && timeout > 0) {
        timeout--;
    }
    outb(COM1_PORT, (uint8_t)c);
}

void serial_puts(const char *s)
{
    while (*s != '\0') {
        serial_putc(*s++);
    }
}

int serial_received(void)
{
    return (inb(COM1_PORT + 5) & 0x01) != 0;
}

char serial_getc(void)
{
    while (!serial_received()) {
        /* wait */
    }
    return (char)inb(COM1_PORT);
}

static void print_uint(uint32_t val, uint32_t base, int width, char pad_char, int upper)
{
    char buf[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;

    if (val == 0) {
        buf[i++] = '0';
    } else {
        while (val > 0) {
            buf[i++] = digits[val % base];
            val /= base;
        }
    }
    while (i < width) {
        buf[i++] = pad_char;
    }
    while (i > 0) {
        serial_putc(buf[--i]);
    }
}

static void print_int(int32_t val)
{
    uint32_t uval;
    if (val < 0) {
        serial_putc('-');
        uval = (uint32_t)(-(val + 1)) + 1U;
    } else {
        uval = (uint32_t)val;
    }
    print_uint(uval, 10, 0, ' ', 0);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    while (*fmt != '\0') {
        if (*fmt != '%') {
            serial_putc(*fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        {
            char pad_char = ' ';
            int width = 0;

            if (*fmt == '0') {
                pad_char = '0';
                fmt++;
            }
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }

            switch (*fmt) {
            case 'c':
                serial_putc((char)va_arg(ap, int));
                break;
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (s == NULL) {
                    s = "(null)";
                }
                serial_puts(s);
                break;
            }
            case 'd':
            case 'i':
                print_int(va_arg(ap, int32_t));
                break;
            case 'u':
                print_uint(va_arg(ap, uint32_t), 10, width, pad_char, 0);
                break;
            case 'x':
                print_uint(va_arg(ap, uint32_t), 16, width, pad_char, 0);
                break;
            case 'X':
                print_uint(va_arg(ap, uint32_t), 16, width, pad_char, 1);
                break;
            case '%':
                serial_putc('%');
                break;
            default:
                serial_putc('%');
                if (*fmt != '\0') {
                    serial_putc(*fmt);
                }
                break;
            }
        }
        if (*fmt != '\0') {
            fmt++;
        }
    }

    va_end(ap);
}
