/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - ACPI Table Parser & ACPI E820 Memory Map
 */

#ifndef OKSINIX_ACPI_H
#define OKSINIX_ACPI_H

#include "types.h"

/* ACPI System Address Map (E820) memory range types (ACPI Spec Chapter 15) */
#define ACPI_MEM_USABLE       1U
#define ACPI_MEM_RESERVED     2U
#define ACPI_MEM_ACPI_RECLAIM 3U
#define ACPI_MEM_ACPI_NVS     4U
#define ACPI_MEM_BAD          5U

#define ACPI_MAX_MMAP   64
#define ACPI_MAX_TABLES 16

struct acpi_rsdp {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint32_t xsdt_lo;
    uint32_t xsdt_hi;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
};

struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

struct acpi_mmap_entry {
    uint32_t base_lo;
    uint32_t base_hi;
    uint32_t length_lo;
    uint32_t length_hi;
    uint32_t type;
};

struct acpi_table_desc {
    char     signature[5];
    uint32_t phys_addr;
    uint32_t length;
    uint8_t  revision;
};

void        acpi_init(uint32_t mb_magic, uint32_t mbi_addr);
bool_t      acpi_has_rsdp(void);
uint32_t    acpi_get_rsdp_addr(void);
uint32_t    acpi_get_rsdt_addr(void);
int         acpi_get_table_count(void);
int         acpi_get_table_desc(int index, struct acpi_table_desc *out);
int         acpi_get_mmap_count(void);
int         acpi_get_mmap_entry(int index, struct acpi_mmap_entry *out);
const char *acpi_mmap_type_name(uint32_t type);
uint32_t    acpi_get_total_usable_ram(void);
uint32_t    acpi_find_usable_ram(uint32_t min_phys, uint32_t size, uint32_t align);
bool_t      acpi_is_usable_ram(uint32_t phys_addr, uint32_t size);

#endif /* OKSINIX_ACPI_H */
