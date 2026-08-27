/*
 * GEMIBOOT - x86 Boot Loader for GEMIOS
 * Dedicated to the Public Domain (CC0)
 */

#include "ata.h"
#include "io.h"

static int ata_wait_ready(void) {
    int timeout = 100000;
    while (timeout-- > 0) {
        uint8_t status = inb(ATA_PRIMARY_IO_BASE + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY)) {
            return 0;
        }
        io_wait();
    }
    return -1;
}

static int ata_wait_drq(void) {
    int timeout = 100000;
    while (timeout-- > 0) {
        uint8_t status = inb(ATA_PRIMARY_IO_BASE + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) {
            return -1;
        }
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) {
            return 0;
        }
        io_wait();
    }
    return -1;
}

int ata_init(void) {
    outb(ATA_PRIMARY_CONTROL_BASE, 0x02); /* Disable IRQs */
    io_wait();
    return 0;
}

int ata_read_sectors(uint32_t lba, uint32_t count, void *buffer) {
    uint8_t *buf = (uint8_t*)buffer;
    uint32_t i;

    if (count == 0 || !buffer) {
        return 0;
    }

    for (i = 0; i < count; i++) {
        uint32_t cur_lba = lba + i;

        if (ata_wait_ready() != 0) {
            return -1;
        }

        /* Select Master drive with LBA bits 24..27 */
        outb(ATA_PRIMARY_IO_BASE + ATA_REG_DRIVE_SELECT, (uint8_t)(0xE0 | ((cur_lba >> 24) & 0x0F)));
        io_wait();

        outb(ATA_PRIMARY_IO_BASE + ATA_REG_SECCOUNT, 1);
        outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_LO, (uint8_t)(cur_lba & 0xFF));
        outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_MID, (uint8_t)((cur_lba >> 8) & 0xFF));
        outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_HI, (uint8_t)((cur_lba >> 16) & 0xFF));
        outb(ATA_PRIMARY_IO_BASE + ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);

        if (ata_wait_drq() != 0) {
            return -1;
        }

        /* Read 256 words (512 bytes) from data port */
        insw(ATA_PRIMARY_IO_BASE + ATA_REG_DATA, buf, 256);
        buf += 512;
    }

    return 0;
}
