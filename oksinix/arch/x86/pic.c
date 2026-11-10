/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - 8259 Programmable Interrupt Controller (PIC)
 */

#include "pic.h"
#include "io.h"

#define PIC1_CMD    0x20
#define PIC1_DATA   0x21
#define PIC2_CMD    0xA0
#define PIC2_DATA   0xA1

#define PIC_EOI     0x20

#define ICW1_INIT   0x10
#define ICW1_ICW4   0x01
#define ICW4_8086   0x01

void pic_init(void)
{
    /* Start initialization sequence in cascade mode */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();

    /* ICW2: Master PIC vector offset 32 (0x20), Slave PIC offset 40 (0x28) */
    outb(PIC1_DATA, 0x20);
    io_wait();
    outb(PIC2_DATA, 0x28);
    io_wait();

    /* ICW3: Tell Master PIC that there is a slave PIC at IRQ2 (0000 0100) */
    outb(PIC1_DATA, 0x04);
    io_wait();
    /* Tell Slave PIC its cascade identity (0000 0010) */
    outb(PIC2_DATA, 0x02);
    io_wait();

    /* ICW4: 8086/88 mode */
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* Mask all IRQs initially except IRQ2 (cascade) */
    outb(PIC1_DATA, 0xFB);
    outb(PIC2_DATA, 0xFF);
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
}

bool_t pic_is_spurious_irq(uint8_t irq)
{
    if (irq == 7U) {
        /* Read Master PIC In-Service Register (OCW3 = 0x0B) */
        outb(PIC1_CMD, 0x0B);
        if ((inb(PIC1_CMD) & 0x80U) == 0) {
            return TRUE;
        }
    } else if (irq == 15U) {
        /* Read Slave PIC In-Service Register (OCW3 = 0x0B) */
        outb(PIC2_CMD, 0x0B);
        if ((inb(PIC2_CMD) & 0x80U) == 0) {
            /* Spurious Slave IRQ15: still acknowledge Master PIC cascade (IRQ2) */
            outb(PIC1_CMD, PIC_EOI);
            return TRUE;
        }
    }
    return FALSE;
}

void pic_unmask_irq(uint8_t irq)
{
    uint16_t port;
    uint8_t val;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq = (uint8_t)(irq - 8);
    }
    val = (uint8_t)(inb(port) & ~(1U << irq));
    outb(port, val);
}

void pic_mask_irq(uint8_t irq)
{
    uint16_t port;
    uint8_t val;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq = (uint8_t)(irq - 8);
    }
    val = (uint8_t)(inb(port) | (1U << irq));
    outb(port, val);
}
