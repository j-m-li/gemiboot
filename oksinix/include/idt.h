/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Interrupt Descriptor Table (IDT) & IRQ handling
 */

#ifndef OKSINIX_IDT_H
#define OKSINIX_IDT_H

#include "types.h"

#define IRQ_BASE 32
#define IRQ0     32
#define IRQ1     33
#define IRQ2     34
#define IRQ12    44

struct isr_regs {
    uint32_t ds;
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp_dummy;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
    uint32_t int_no;
    uint32_t err_code;
    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
};

typedef void (*irq_handler_t)(struct isr_regs *regs);

void idt_init(void);
void irq_register_handler(uint8_t irq, irq_handler_t handler);
void isr_dispatch(struct isr_regs *regs);

#endif /* OKSINIX_IDT_H */
