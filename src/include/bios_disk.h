/*
 * GEMIBOOT - x86 Boot Loader for GEMIOS
 * Dedicated to the Public Domain (CC0)
 */

#ifndef GEMIBOOT_BIOS_DISK_H
#define GEMIBOOT_BIOS_DISK_H 1

#include "types.h"

int bios_disk_init(void);
int bios_read_sectors(uint32_t lba, uint32_t count, void *buffer);

#endif /* GEMIBOOT_BIOS_DISK_H */
