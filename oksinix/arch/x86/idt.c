/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Interrupt Descriptor Table (IDT)
 */

#include "idt.h"
#include "gdt.h"
#include "pic.h"
#include "io.h"
#include "serial.h"
#include "string.h"
#include "syscall.h"

#define IDT_ENTRIES 256
#define IRQ_COUNT   16

static volatile uint8_t  idt_table[IDT_ENTRIES][8];
static volatile uint16_t idt_ptr[3]; /* 6-byte IDTR: limit (16-bit) + base (32-bit) */
static irq_handler_t irq_handlers[IRQ_COUNT];

/* External assembly ISR & IRQ entry stubs */
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);
extern void isr128(void);

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags)
{
    idt_table[num][0] = (uint8_t)(base & 0xFFU);
    idt_table[num][1] = (uint8_t)((base >> 8) & 0xFFU);
    idt_table[num][2] = (uint8_t)(sel & 0xFFU);
    idt_table[num][3] = (uint8_t)((sel >> 8) & 0xFFU);
    idt_table[num][4] = 0;
    idt_table[num][5] = flags;
    idt_table[num][6] = (uint8_t)((base >> 16) & 0xFFU);
    idt_table[num][7] = (uint8_t)((base >> 24) & 0xFFU);
}

void irq_register_handler(uint8_t irq, irq_handler_t handler)
{
    if (irq < IRQ_COUNT) {
        irq_handlers[irq] = handler;
    }
}

void isr_dispatch(struct isr_regs *regs)
{
    if (regs->int_no == 0x80U) {
        syscall_dispatch(regs);
        return;
    }
    if (regs->int_no >= IRQ_BASE && regs->int_no < IRQ_BASE + IRQ_COUNT) {
        uint8_t irq = (uint8_t)(regs->int_no - IRQ_BASE);
        if (pic_is_spurious_irq(irq)) {
            return;
        }
        if (irq_handlers[irq] != NULL) {
            irq_handlers[irq](regs);
        }
        pic_send_eoi(irq);
    } else if (regs->int_no < 32) {
        kprintf("[EXCEPTION] CPU fault #%u (err=0x%x) at EIP=0x%08x\n",
                regs->int_no, regs->err_code, regs->eip);
        cpu_cli();
        for (;;) {
            cpu_hlt();
        }
    }
}

void idt_init(void)
{
    uint32_t base_addr = (uint32_t)(uintptr_t)&idt_table[0][0];
    uint16_t limit = (uint16_t)(sizeof(idt_table) - 1U);
    int i;

    for (i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate((uint8_t)i, 0, 0, 0);
    }
    memset(irq_handlers, 0, sizeof(irq_handlers));

    pic_init();

    /* CPU Exceptions 0..31 */
    idt_set_gate(0,  (uint32_t)(uintptr_t)isr0,  KERNEL_CS, 0x8E);
    idt_set_gate(1,  (uint32_t)(uintptr_t)isr1,  KERNEL_CS, 0x8E);
    idt_set_gate(2,  (uint32_t)(uintptr_t)isr2,  KERNEL_CS, 0x8E);
    idt_set_gate(3,  (uint32_t)(uintptr_t)isr3,  KERNEL_CS, 0x8E);
    idt_set_gate(4,  (uint32_t)(uintptr_t)isr4,  KERNEL_CS, 0x8E);
    idt_set_gate(5,  (uint32_t)(uintptr_t)isr5,  KERNEL_CS, 0x8E);
    idt_set_gate(6,  (uint32_t)(uintptr_t)isr6,  KERNEL_CS, 0x8E);
    idt_set_gate(7,  (uint32_t)(uintptr_t)isr7,  KERNEL_CS, 0x8E);
    idt_set_gate(8,  (uint32_t)(uintptr_t)isr8,  KERNEL_CS, 0x8E);
    idt_set_gate(9,  (uint32_t)(uintptr_t)isr9,  KERNEL_CS, 0x8E);
    idt_set_gate(10, (uint32_t)(uintptr_t)isr10, KERNEL_CS, 0x8E);
    idt_set_gate(11, (uint32_t)(uintptr_t)isr11, KERNEL_CS, 0x8E);
    idt_set_gate(12, (uint32_t)(uintptr_t)isr12, KERNEL_CS, 0x8E);
    idt_set_gate(13, (uint32_t)(uintptr_t)isr13, KERNEL_CS, 0x8E);
    idt_set_gate(14, (uint32_t)(uintptr_t)isr14, KERNEL_CS, 0x8E);
    idt_set_gate(15, (uint32_t)(uintptr_t)isr15, KERNEL_CS, 0x8E);
    idt_set_gate(16, (uint32_t)(uintptr_t)isr16, KERNEL_CS, 0x8E);
    idt_set_gate(17, (uint32_t)(uintptr_t)isr17, KERNEL_CS, 0x8E);
    idt_set_gate(18, (uint32_t)(uintptr_t)isr18, KERNEL_CS, 0x8E);
    idt_set_gate(19, (uint32_t)(uintptr_t)isr19, KERNEL_CS, 0x8E);
    idt_set_gate(20, (uint32_t)(uintptr_t)isr20, KERNEL_CS, 0x8E);
    idt_set_gate(21, (uint32_t)(uintptr_t)isr21, KERNEL_CS, 0x8E);
    idt_set_gate(22, (uint32_t)(uintptr_t)isr22, KERNEL_CS, 0x8E);
    idt_set_gate(23, (uint32_t)(uintptr_t)isr23, KERNEL_CS, 0x8E);
    idt_set_gate(24, (uint32_t)(uintptr_t)isr24, KERNEL_CS, 0x8E);
    idt_set_gate(25, (uint32_t)(uintptr_t)isr25, KERNEL_CS, 0x8E);
    idt_set_gate(26, (uint32_t)(uintptr_t)isr26, KERNEL_CS, 0x8E);
    idt_set_gate(27, (uint32_t)(uintptr_t)isr27, KERNEL_CS, 0x8E);
    idt_set_gate(28, (uint32_t)(uintptr_t)isr28, KERNEL_CS, 0x8E);
    idt_set_gate(29, (uint32_t)(uintptr_t)isr29, KERNEL_CS, 0x8E);
    idt_set_gate(30, (uint32_t)(uintptr_t)isr30, KERNEL_CS, 0x8E);
    idt_set_gate(31, (uint32_t)(uintptr_t)isr31, KERNEL_CS, 0x8E);

    /* PIC IRQs 0..15 mapped to 32..47 */
    idt_set_gate(32, (uint32_t)(uintptr_t)irq0,  KERNEL_CS, 0x8E);
    idt_set_gate(33, (uint32_t)(uintptr_t)irq1,  KERNEL_CS, 0x8E);
    idt_set_gate(34, (uint32_t)(uintptr_t)irq2,  KERNEL_CS, 0x8E);
    idt_set_gate(35, (uint32_t)(uintptr_t)irq3,  KERNEL_CS, 0x8E);
    idt_set_gate(36, (uint32_t)(uintptr_t)irq4,  KERNEL_CS, 0x8E);
    idt_set_gate(37, (uint32_t)(uintptr_t)irq5,  KERNEL_CS, 0x8E);
    idt_set_gate(38, (uint32_t)(uintptr_t)irq6,  KERNEL_CS, 0x8E);
    idt_set_gate(39, (uint32_t)(uintptr_t)irq7,  KERNEL_CS, 0x8E);
    idt_set_gate(40, (uint32_t)(uintptr_t)irq8,  KERNEL_CS, 0x8E);
    idt_set_gate(41, (uint32_t)(uintptr_t)irq9,  KERNEL_CS, 0x8E);
    idt_set_gate(42, (uint32_t)(uintptr_t)irq10, KERNEL_CS, 0x8E);
    idt_set_gate(43, (uint32_t)(uintptr_t)irq11, KERNEL_CS, 0x8E);
    idt_set_gate(44, (uint32_t)(uintptr_t)irq12, KERNEL_CS, 0x8E);
    idt_set_gate(45, (uint32_t)(uintptr_t)irq13, KERNEL_CS, 0x8E);
    idt_set_gate(46, (uint32_t)(uintptr_t)irq14, KERNEL_CS, 0x8E);
    idt_set_gate(47, (uint32_t)(uintptr_t)irq15, KERNEL_CS, 0x8E);

    /* Linux x86 32-bit syscall gate (int $0x80) */
    idt_set_gate(0x80, (uint32_t)(uintptr_t)isr128, KERNEL_CS, 0xEE);

    idt_ptr[0] = limit;
    idt_ptr[1] = (uint16_t)(base_addr & 0xFFFFU);
    idt_ptr[2] = (uint16_t)((base_addr >> 16) & 0xFFFFU);

    idt_flush(&idt_ptr[0]);
}
