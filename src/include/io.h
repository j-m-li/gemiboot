/*
 * GEMIBOOT - x86 Boot Loader for GEMIOS
 * Dedicated to the Public Domain (CC0)
 */

#ifndef GEMIBOOT_IO_H
#define GEMIBOOT_IO_H 1

#include "types.h"

static __inline__ uint8_t inb(uint16_t port) {
    uint8_t data;
    __asm__ volatile ("inb %1, %0" : "=a"(data) : "Nd"(port));
    return data;
}

static __inline__ void outb(uint16_t port, uint8_t data) {
    __asm__ volatile ("outb %0, %1" : : "a"(data), "Nd"(port));
}

static __inline__ uint16_t inw(uint16_t port) {
    uint16_t data;
    __asm__ volatile ("inw %1, %0" : "=a"(data) : "Nd"(port));
    return data;
}

static __inline__ void outw(uint16_t port, uint16_t data) {
    __asm__ volatile ("outw %0, %1" : : "a"(data), "Nd"(port));
}

static __inline__ uint32_t inl(uint16_t port) {
    uint32_t data;
    __asm__ volatile ("inl %1, %0" : "=a"(data) : "Nd"(port));
    return data;
}

static __inline__ void outl(uint16_t port, uint32_t data) {
    __asm__ volatile ("outl %0, %1" : : "a"(data), "Nd"(port));
}

static __inline__ void insw(uint16_t port, void *addr, size_t count) {
    __asm__ volatile ("cld; rep insw" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

static __inline__ void outsw(uint16_t port, const void *addr, size_t count) {
    __asm__ volatile ("cld; rep outsw" : "+S"(addr), "+c"(count) : "d"(port));
}

static __inline__ void io_wait(void) {
    outb(0x80, 0);
}

#endif /* GEMIBOOT_IO_H */
