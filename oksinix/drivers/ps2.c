/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - 8042 PS/2 Controller driver
 */

#include "ps2.h"
#include "io.h"
#include "serial.h"

int ps2_wait_write(void)
{
    int timeout = 50000;
    while (timeout > 0) {
        if ((inb(PS2_STATUS_PORT) & 0x02U) == 0) {
            return 0;
        }
        io_wait();
        timeout--;
    }
    return -1;
}

int ps2_wait_read(void)
{
    int timeout = 50000;
    while (timeout > 0) {
        if ((inb(PS2_STATUS_PORT) & 0x01U) != 0) {
            return 0;
        }
        io_wait();
        timeout--;
    }
    return -1;
}

void ps2_write_cmd(uint8_t cmd)
{
    (void)ps2_wait_write();
    outb(PS2_CMD_PORT, cmd);
}

void ps2_write_data(uint8_t data)
{
    (void)ps2_wait_write();
    outb(PS2_DATA_PORT, data);
}

uint8_t ps2_read_data(void)
{
    (void)ps2_wait_read();
    return inb(PS2_DATA_PORT);
}

static void ps2_flush_output(void)
{
    int max_reads = 64;
    while ((inb(PS2_STATUS_PORT) & 0x01U) != 0 && max_reads > 0) {
        (void)inb(PS2_DATA_PORT);
        max_reads--;
    }
}

void ps2_init(void)
{
    uint8_t config;
    uint8_t selftest = 0;

    /* Disable both PS/2 ports during setup */
    ps2_write_cmd(0xAD);
    ps2_write_cmd(0xA7);
    ps2_flush_output();

    /* Read controller configuration byte */
    ps2_write_cmd(0x20);
    config = ps2_read_data();

    /* Disable IRQs temporarily during self-test */
    config &= (uint8_t)~((1U << 0) | (1U << 1));
    ps2_write_cmd(0x60);
    ps2_write_data(config);

    /* Perform 8042 Controller Self-Test (0xAA -> 0x55) */
    ps2_write_cmd(0xAA);
    if (ps2_wait_read() == 0) {
        selftest = inb(PS2_DATA_PORT);
    }

    /*
     * Enable IRQ1 (bit 0), IRQ12 (bit 1), and translation (bit 6).
     * Clear port 1 clock disable (bit 4) and port 2 clock disable (bit 5).
     * Re-written after 0xAA because real 8042 hardware resets config on self-test.
     */
    config |= (uint8_t)((1U << 0) | (1U << 1) | (1U << 6));
    config &= (uint8_t)~((1U << 4) | (1U << 5));

    ps2_write_cmd(0x60);
    ps2_write_data(config);

    /* Enable first and second PS/2 ports */
    ps2_write_cmd(0xAE);
    ps2_write_cmd(0xA8);
    ps2_flush_output();

    kprintf("[PS2] 8042 controller initialized (selftest=0x%02x, config=0x%02x)\n",
            (uint32_t)selftest, (uint32_t)config);
}
