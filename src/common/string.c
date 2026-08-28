/*
 * GEMIBOOT - x86 Boot Loader for GEMIOS
 * Dedicated to the Public Domain (CC0)
 */

#include "types.h"
#include "io.h"

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t*)dest;
    const uint8_t *s = (const uint8_t*)src;
    size_t i;
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t*)s;
    uint8_t val = (uint8_t)c;
    size_t i;
    for (i = 0; i < n; i++) {
        p[i] = val;
    }
    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t*)dest;
    const uint8_t *s = (const uint8_t*)src;
    size_t i;
    if (d < s) {
        for (i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t*)s1;
    const uint8_t *p2 = (const uint8_t*)s2;
    size_t i;
    for (i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return (int)p1[i] - (int)p2[i];
        }
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) {
        len++;
    }
    return len;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (s1[i] != s2[i]) {
            return (int)(unsigned char)s1[i] - (int)(unsigned char)s2[i];
        }
        if (s1[i] == '\0') {
            break;
        }
    }
    return 0;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++) != '\0') {
        ;
    }
    return dest;
}

/* COM1 serial port output for debugging in BIOS / bare-metal mode */
#define COM1_PORT 0x3F8

void serial_init(void) {
    outb(COM1_PORT + 1, 0x00); /* Disable interrupts */
    outb(COM1_PORT + 3, 0x80); /* Enable DLAB */
    outb(COM1_PORT + 0, 0x03); /* Set divisor to 3 (38400 baud) */
    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x03); /* 8 bits, no parity, 1 stop bit */
    outb(COM1_PORT + 2, 0xC7); /* Enable FIFO */
    outb(COM1_PORT + 4, 0x0B); /* IRQs enabled, RTS/DSR set */
}

void serial_putc(char c) {
    while ((inb(COM1_PORT + 5) & 0x20) == 0) {
        ;
    }
    outb(COM1_PORT, (uint8_t)c);
}

void serial_puts(const char *s) {
    while (s && *s) {
        if (*s == '\n') {
            serial_putc('\r');
        }
        serial_putc(*s++);
    }
}

void serial_put_hex(uint32_t val) {
    static const char hex_chars[] = "0123456789ABCDEF";
    int i;
    serial_puts("0x");
    for (i = 28; i >= 0; i -= 4) {
        serial_putc(hex_chars[(val >> i) & 0xF]);
    }
}

void serial_put_dec(uint32_t val) {
    char buf[12];
    int i = 0;
    if (val == 0) {
        serial_putc('0');
        return;
    }
    while (val > 0) {
        buf[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (i > 0) {
        serial_putc(buf[--i]);
    }
}
