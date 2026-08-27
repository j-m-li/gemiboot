/*
 * GEMIBOOT - x86 Boot Loader for GEMIOS
 * Dedicated to the Public Domain (CC0)
 */

#ifndef GEMIBOOT_ATA_H
#define GEMIBOOT_ATA_H 1

#include "types.h"

#define ATA_PRIMARY_IO_BASE        0x1F0
#define ATA_PRIMARY_CONTROL_BASE   0x3F6

#define ATA_REG_DATA               0
#define ATA_REG_ERROR              1
#define ATA_REG_FEATURES           1
#define ATA_REG_SECCOUNT           2
#define ATA_REG_LBA_LO             3
#define ATA_REG_LBA_MID            4
#define ATA_REG_LBA_HI             5
#define ATA_REG_DRIVE_SELECT       6
#define ATA_REG_STATUS             7
#define ATA_REG_COMMAND            7

#define ATA_CMD_READ_SECTORS       0x20
#define ATA_CMD_WRITE_SECTORS      0x30
#define ATA_CMD_IDENTIFY           0xEC

#define ATA_SR_BSY                 0x80
#define ATA_SR_DRDY                0x40
#define ATA_SR_DF                  0x20
#define ATA_SR_DSC                 0x10
#define ATA_SR_DRQ                 0x08
#define ATA_SR_CORR                0x04
#define ATA_SR_IDX                 0x02
#define ATA_SR_ERR                 0x01

int ata_init(void);
int ata_read_sectors(uint32_t lba, uint32_t count, void *buffer);

#endif /* GEMIBOOT_ATA_H */
