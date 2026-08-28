/*
 * GEMIBOOT - x86 Boot Loader for GEMIOS
 * Dedicated to the Public Domain (CC0)
 */

#include "bios_disk.h"
#include "string.h"

extern int bios_disk_read_raw(uint32_t lba, uint16_t sectors, uint16_t segment, uint16_t offset);

#define BOUNCE_BUF_PADDR 0x00020000
#define BOUNCE_BUF_SEG   0x2000
#define BOUNCE_BUF_OFF   0x0000
#define MAX_SECTORS_CHUNK 64 /* 32 KB per BIOS INT 0x13 call */

int bios_disk_init(void) {
    return 0;
}

extern void serial_puts(const char *s);
extern void serial_put_dec(uint32_t val);
extern void serial_put_hex(uint32_t val);

int bios_read_sectors(uint32_t lba, uint32_t count, void *buffer) {
    uint8_t *dest = (uint8_t*)buffer;
    uint8_t *bounce = (uint8_t*)BOUNCE_BUF_PADDR;

    if (count == 0 || !buffer) {
        return 0;
    }

    while (count > 0) {
        uint32_t chunk = count;
        int res;

        if (chunk > MAX_SECTORS_CHUNK) {
            chunk = MAX_SECTORS_CHUNK;
        }

        res = bios_disk_read_raw(lba, (uint16_t)chunk, BOUNCE_BUF_SEG, BOUNCE_BUF_OFF);
        if (res != 0) {
            serial_puts("[BIOS DISK] ERROR: read failed for LBA ");
            serial_put_dec(lba);
            serial_puts(" count ");
            serial_put_dec(chunk);
            serial_puts(" res=");
            serial_put_dec((uint32_t)res);
            serial_puts("\n");
            return -1;
        }

        memcpy(dest, bounce, chunk * 512);

        lba += chunk;
        count -= chunk;
        dest += chunk * 512;
    }

    return 0;
}
