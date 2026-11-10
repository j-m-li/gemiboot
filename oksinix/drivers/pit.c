/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - 8254 Programmable Interval Timer (PIT) Driver
 * Provides 1000 Hz IRQ0 system tick and hardware-calibrated microsecond/millisecond delays
 */

#include "pit.h"
#include "idt.h"
#include "pic.h"
#include "io.h"
#include "serial.h"

#define PIT_CH0_DATA 0x40
#define PIT_CMD_PORT 0x43

/* 1193182 Hz / 1000 Hz = 1193 ticks per millisecond */
#define PIT_DIVISOR  1193U

static volatile uint32_t pit_ticks = 0;
static bool_t            pit_initialized = FALSE;

static void pit_irq0_handler(struct isr_regs *regs)
{
    (void)regs;
    pit_ticks++;
}

static uint16_t pit_read_counter0(void)
{
    uint32_t flags = cpu_eflags();
    uint8_t lo, hi;

    cpu_cli();
    outb(PIT_CMD_PORT, 0x00); /* Latch counter 0 */
    lo = inb(PIT_CH0_DATA);
    hi = inb(PIT_CH0_DATA);
    if ((flags & 0x200U) != 0) {
        cpu_sti();
    }
    return (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
}

void pit_init(void)
{
    pit_ticks = 0;

    /* Channel 0, lobyte/hibyte (0x30), Mode 2 rate generator (0x04), binary (0x00) */
    outb(PIT_CMD_PORT, 0x34);
    outb(PIT_CH0_DATA, (uint8_t)(PIT_DIVISOR & 0xFFU));
    outb(PIT_CH0_DATA, (uint8_t)((PIT_DIVISOR >> 8) & 0xFFU));

    irq_register_handler(0, pit_irq0_handler);
    pic_unmask_irq(0);
    pit_initialized = TRUE;

    kprintf("[PIT] 8254 timer initialized at %u Hz (divisor=%u, IRQ0)\n",
            (uint32_t)PIT_TICK_HZ, (uint32_t)PIT_DIVISOR);
}

uint32_t pit_get_ticks(void)
{
    return pit_ticks;
}

void pit_udelay(uint32_t us)
{
    uint32_t total_pit_ticks;
    uint32_t elapsed = 0;
    uint16_t prev;
    uint32_t guard_iters;

    if (us == 0) {
        return;
    }

    if (!pit_initialized) {
        uint32_t i;
        for (i = 0; i < us; i++) {
            io_wait();
        }
        return;
    }

    /* 1.193182 PIT ticks per microsecond */
    total_pit_ticks = (us * 1193U) / 1000U;
    if (total_pit_ticks == 0) {
        total_pit_ticks = 1U;
    }

    prev = pit_read_counter0();
    guard_iters = us + 32U;

    while (elapsed < total_pit_ticks && guard_iters > 0) {
        uint16_t cur = pit_read_counter0();
        uint16_t delta;

        if (cur <= prev) {
            delta = (uint16_t)(prev - cur);
        } else {
            /* Counter wrapped around from PIT_DIVISOR */
            if (prev <= PIT_DIVISOR && cur <= PIT_DIVISOR) {
                delta = (uint16_t)((PIT_DIVISOR - cur) + prev);
            } else {
                delta = 2U;
            }
        }

        if (delta == 0) {
            /* If 8254 PIT is clock-gated on modern chipsets, port 0x80 I/O wait paces ~1-2 us */
            io_wait();
            elapsed += 2U;
        } else {
            elapsed += delta;
            prev = cur;
        }
        guard_iters--;
    }
}

void pit_mdelay(uint32_t ms)
{
    while (ms > 0) {
        pit_udelay(1000U);
        ms--;
    }
}
