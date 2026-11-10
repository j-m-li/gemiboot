/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - ACPI Table Discovery & ACPI Memory Map
 */

#include "acpi.h"
#include "multiboot.h"
#include "serial.h"
#include "string.h"

/* Physical address where boot/mbr.S stores BIOS INT 15h EAX=0xE820 map */
#define BIOS_E820_COUNT_ADDR 0x00008000U
#define BIOS_E820_ARRAY_ADDR 0x00008004U

static bool_t                 rsdp_found = FALSE;
static uint32_t               rsdp_phys  = 0;
static uint32_t               rsdt_phys  = 0;
static char                   acpi_oem[7];
static uint8_t                acpi_rev   = 0;
static struct acpi_table_desc acpi_tables[ACPI_MAX_TABLES];
static int                    num_acpi_tables = 0;

static struct acpi_mmap_entry mmap_table[ACPI_MAX_MMAP];
static int                    num_mmap_entries = 0;
static uint32_t               total_usable_bytes = 0;
static uint32_t               total_acpi_bytes   = 0;

static uint32_t read_le32_unaligned(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool_t acpi_checksum_ok(const void *ptr, uint32_t len)
{
    const uint8_t *bytes = (const uint8_t *)ptr;
    uint8_t sum = 0;
    uint32_t i;

    if (ptr == NULL || len == 0) {
        return FALSE;
    }
    for (i = 0; i < len; i++) {
        sum = (uint8_t)(sum + bytes[i]);
    }
    return (sum == 0) ? TRUE : FALSE;
}

static const struct acpi_rsdp *scan_rsdp_range(uint32_t start, uint32_t end)
{
    uint32_t addr;
    for (addr = start; addr + 20U <= end; addr += 16U) {
        const struct acpi_rsdp *cand = (const struct acpi_rsdp *)(uintptr_t)addr;
        if (memcmp(cand->signature, "RSD PTR ", 8) == 0 &&
            acpi_checksum_ok(cand, 20U)) {
            if (cand->revision >= 2U && cand->length >= 36U) {
                if (!acpi_checksum_ok(cand, 36U)) {
                    continue;
                }
            }
            return cand;
        }
    }
    return NULL;
}

static void record_acpi_table(const char *sig4, uint32_t phys_addr, uint32_t length, uint8_t rev)
{
    int i;
    if (phys_addr == 0 || length == 0 || num_acpi_tables >= ACPI_MAX_TABLES) {
        return;
    }
    for (i = 0; i < num_acpi_tables; i++) {
        if (acpi_tables[i].phys_addr == phys_addr) {
            return;
        }
    }
    memcpy(acpi_tables[num_acpi_tables].signature, sig4, 4);
    acpi_tables[num_acpi_tables].signature[4] = '\0';
    acpi_tables[num_acpi_tables].phys_addr    = phys_addr;
    acpi_tables[num_acpi_tables].length       = length;
    acpi_tables[num_acpi_tables].revision     = rev;
    num_acpi_tables++;
}

static void discover_acpi_tables(void)
{
    uint16_t ebda_seg;
    uint32_t ebda_base;
    const struct acpi_rsdp *rsdp = NULL;
    const struct acpi_sdt_header *rsdt;
    uint32_t entry_count;
    uint32_t i;

    ebda_seg  = *(const volatile uint16_t *)(uintptr_t)0x0000040EU;
    ebda_base = ((uint32_t)ebda_seg) << 4;
    if (ebda_base >= 0x00080000U && ebda_base < 0x000A0000U) {
        rsdp = scan_rsdp_range(ebda_base, ebda_base + 1024U);
    }
    if (rsdp == NULL) {
        rsdp = scan_rsdp_range(0x000E0000U, 0x00100000U);
    }
    if (rsdp == NULL) {
        return;
    }

    rsdp_found = TRUE;
    rsdp_phys  = (uint32_t)(uintptr_t)rsdp;
    rsdt_phys  = rsdp->rsdt_address;
    acpi_rev   = rsdp->revision;
    memcpy(acpi_oem, rsdp->oem_id, 6);
    acpi_oem[6] = '\0';

    if (rsdt_phys == 0) {
        return;
    }

    rsdt = (const struct acpi_sdt_header *)(uintptr_t)rsdt_phys;
    if (memcmp(rsdt->signature, "RSDT", 4) != 0 ||
        rsdt->length < sizeof(struct acpi_sdt_header) ||
        rsdt->length > 65536U ||
        !acpi_checksum_ok(rsdt, rsdt->length)) {
        return;
    }

    record_acpi_table("RSDT", rsdt_phys, rsdt->length, rsdt->revision);

    entry_count = (rsdt->length - (uint32_t)sizeof(struct acpi_sdt_header)) / 4U;
    for (i = 0; i < entry_count; i++) {
        const uint8_t *ent_ptr = (const uint8_t *)rsdt + sizeof(struct acpi_sdt_header) + i * 4U;
        uint32_t tbl_phys = read_le32_unaligned(ent_ptr);
        const struct acpi_sdt_header *hdr;

        if (tbl_phys == 0) {
            continue;
        }
        hdr = (const struct acpi_sdt_header *)(uintptr_t)tbl_phys;
        if (hdr->length < sizeof(struct acpi_sdt_header) || hdr->length > 0x00100000U) {
            continue;
        }
        record_acpi_table(hdr->signature, tbl_phys, hdr->length, hdr->revision);

        /* If this is the FADT ("FACP"), also locate FACS and DSDT physical tables */
        if (memcmp(hdr->signature, "FACP", 4) == 0 && hdr->length >= 44U) {
            const uint8_t *fadt_bytes = (const uint8_t *)hdr;
            uint32_t facs_phys = read_le32_unaligned(&fadt_bytes[36]);
            uint32_t dsdt_phys = read_le32_unaligned(&fadt_bytes[40]);
            if (facs_phys != 0) {
                const uint8_t *facs = (const uint8_t *)(uintptr_t)facs_phys;
                uint32_t facs_len = read_le32_unaligned(&facs[4]);
                if (facs_len >= 64U && facs_len <= 65536U) {
                    record_acpi_table("FACS", facs_phys, facs_len, 1U);
                }
            }
            if (dsdt_phys != 0) {
                const struct acpi_sdt_header *dsdt =
                    (const struct acpi_sdt_header *)(uintptr_t)dsdt_phys;
                if (memcmp(dsdt->signature, "DSDT", 4) == 0 &&
                    dsdt->length >= sizeof(struct acpi_sdt_header)) {
                    record_acpi_table("DSDT", dsdt_phys, dsdt->length, dsdt->revision);
                }
            }
        }
    }
}

static void add_mmap_entry(uint32_t base_lo, uint32_t base_hi,
                           uint32_t len_lo, uint32_t len_hi, uint32_t type)
{
    if ((len_lo == 0 && len_hi == 0) || num_mmap_entries >= ACPI_MAX_MMAP) {
        return;
    }
    mmap_table[num_mmap_entries].base_lo   = base_lo;
    mmap_table[num_mmap_entries].base_hi   = base_hi;
    mmap_table[num_mmap_entries].length_lo = len_lo;
    mmap_table[num_mmap_entries].length_hi = len_hi;
    mmap_table[num_mmap_entries].type      = type;
    num_mmap_entries++;
}

static void integrate_acpi_table_regions(void)
{
    uint32_t min_acpi = 0xFFFFFFFFU;
    uint32_t max_acpi = 0;
    int i;
    bool_t already_marked = FALSE;

    if (num_acpi_tables == 0) {
        return;
    }

    for (i = 0; i < num_acpi_tables; i++) {
        uint32_t start = acpi_tables[i].phys_addr;
        uint32_t end   = start + acpi_tables[i].length;
        if (start < 0x00100000U) {
            continue;
        }
        if (start < min_acpi) {
            min_acpi = start;
        }
        if (end > max_acpi) {
            max_acpi = end;
        }
    }

    if (min_acpi >= max_acpi) {
        return;
    }

    min_acpi &= ~0xFFFU;
    max_acpi = (max_acpi + 0xFFFU) & ~0xFFFU;

    for (i = 0; i < num_mmap_entries; i++) {
        if (mmap_table[i].base_hi == 0 &&
            (mmap_table[i].type == ACPI_MEM_ACPI_RECLAIM ||
             mmap_table[i].type == ACPI_MEM_ACPI_NVS)) {
            already_marked = TRUE;
            break;
        }
    }

    if (already_marked) {
        return;
    }

    /* Carve out ACPI Reclaimable region from any overlapping usable RAM entry */
    for (i = 0; i < num_mmap_entries; i++) {
        uint32_t b = mmap_table[i].base_lo;
        uint32_t e = b + mmap_table[i].length_lo;
        if (mmap_table[i].base_hi == 0 && mmap_table[i].type == ACPI_MEM_USABLE &&
            b <= min_acpi && e >= max_acpi) {
            mmap_table[i].length_lo = min_acpi - b;
            add_mmap_entry(min_acpi, 0, max_acpi - min_acpi, 0, ACPI_MEM_ACPI_RECLAIM);
            if (e > max_acpi) {
                add_mmap_entry(max_acpi, 0, e - max_acpi, 0, ACPI_MEM_USABLE);
            }
            return;
        }
    }

    add_mmap_entry(min_acpi, 0, max_acpi - min_acpi, 0, ACPI_MEM_ACPI_RECLAIM);
}

const char *acpi_mmap_type_name(uint32_t type)
{
    switch (type) {
    case ACPI_MEM_USABLE:       return "Usable RAM";
    case ACPI_MEM_RESERVED:     return "Reserved";
    case ACPI_MEM_ACPI_RECLAIM: return "ACPI Reclaimable";
    case ACPI_MEM_ACPI_NVS:     return "ACPI NVS";
    case ACPI_MEM_BAD:          return "Bad Memory";
    default:                    return "Reserved";
    }
}

void acpi_init(uint32_t mb_magic, uint32_t mbi_addr)
{
    uint16_t e820_count = *(const volatile uint16_t *)(uintptr_t)BIOS_E820_COUNT_ADDR;
    const char *src_name = "ACPI/BIOS";
    int i;

    memset(acpi_tables, 0, sizeof(acpi_tables));
    memset(mmap_table, 0, sizeof(mmap_table));
    num_acpi_tables    = 0;
    num_mmap_entries   = 0;
    total_usable_bytes = 0;
    total_acpi_bytes   = 0;

    /* 1. Discover ACPI RSDP, RSDT, FADT, DSDT, FACS, MADT tables in physical memory */
    discover_acpi_tables();

    /* 2. Populate ACPI Memory Map from BIOS E820 (0x8000) or Multiboot E820 mmap */
    if (mbi_addr == 0 && e820_count > 0 && e820_count <= 128U) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)BIOS_E820_ARRAY_ADDR;
        if (e820_count > ACPI_MAX_MMAP) {
            e820_count = ACPI_MAX_MMAP;
        }
        src_name = "ACPI E820 (INT 15h)";
        for (i = 0; i < (int)e820_count; i++) {
            uint32_t base_lo = read_le32_unaligned(&p[0]);
            uint32_t base_hi = read_le32_unaligned(&p[4]);
            uint32_t len_lo  = read_le32_unaligned(&p[8]);
            uint32_t len_hi  = read_le32_unaligned(&p[12]);
            uint32_t type    = read_le32_unaligned(&p[16]);
            add_mmap_entry(base_lo, base_hi, len_lo, len_hi, type);
            p += 20U;
        }
    } else if (mb_magic == MULTIBOOT_BOOTLOADER_MAGIC && mbi_addr != 0) {
        const struct multiboot_info *mbi = (const struct multiboot_info *)(uintptr_t)mbi_addr;
        if ((mbi->flags & MULTIBOOT_INFO_MEM_MAP) != 0 && mbi->mmap_length > 0) {
            uint32_t off = 0;
            src_name = "ACPI/Multiboot E820";
            while (off < mbi->mmap_length && num_mmap_entries < ACPI_MAX_MMAP) {
                const struct multiboot_mmap_entry *me =
                    (const struct multiboot_mmap_entry *)(uintptr_t)(mbi->mmap_addr + off);
                add_mmap_entry(me->addr_lo, me->addr_hi, me->len_lo, me->len_hi, me->type);
                if (me->size == 0) {
                    break;
                }
                off += me->size + 4U;
            }
        } else if ((mbi->flags & MULTIBOOT_INFO_MEMORY) != 0) {
            uint32_t low_bytes = mbi->mem_lower * 1024U;
            uint32_t hi_bytes  = mbi->mem_upper * 1024U;
            src_name = "ACPI + Multiboot Memory";
            add_mmap_entry(0x00000000U, 0, low_bytes, 0, ACPI_MEM_USABLE);
            add_mmap_entry(low_bytes, 0, 0x00100000U - low_bytes, 0, ACPI_MEM_RESERVED);
            add_mmap_entry(0x00100000U, 0, hi_bytes, 0, ACPI_MEM_USABLE);
        }
    }

    if (num_mmap_entries == 0) {
        /* Fallback when neither E820 nor Multiboot mmap is present */
        src_name = "ACPI Table Synthesis";
        add_mmap_entry(0x00000000U, 0, 0x0009FC00U, 0, ACPI_MEM_USABLE);
        add_mmap_entry(0x0009FC00U, 0, 0x00060400U, 0, ACPI_MEM_RESERVED);
        add_mmap_entry(0x00100000U, 0, 0x07EE0000U, 0, ACPI_MEM_USABLE);
    }

    /* 3. Ensure all discovered ACPI table regions are marked in the memory map */
    integrate_acpi_table_regions();

    /* 4. Compute totals and log ACPI tables + ACPI memory map */
    for (i = 0; i < num_mmap_entries; i++) {
        if (mmap_table[i].base_hi == 0) {
            if (mmap_table[i].type == ACPI_MEM_USABLE) {
                total_usable_bytes += mmap_table[i].length_lo;
            } else if (mmap_table[i].type == ACPI_MEM_ACPI_RECLAIM ||
                       mmap_table[i].type == ACPI_MEM_ACPI_NVS) {
                total_acpi_bytes += mmap_table[i].length_lo;
            }
        }
    }

    if (rsdp_found) {
        kprintf("[ACPI] RSDP v%u at 0x%08x (OEM \"%s\", RSDT=0x%08x, %d tables:",
                (uint32_t)acpi_rev, rsdp_phys, acpi_oem, rsdt_phys, num_acpi_tables);
        for (i = 0; i < num_acpi_tables; i++) {
            kprintf(" %s@0x%x", acpi_tables[i].signature, acpi_tables[i].phys_addr);
        }
        kprintf(")\n");
    } else {
        kprintf("[ACPI] RSDP not found; using %s memory map\n", src_name);
    }

    for (i = 0; i < num_mmap_entries; i++) {
        uint32_t start = mmap_table[i].base_lo;
        uint32_t len   = mmap_table[i].length_lo;
        uint32_t end   = (len > 0) ? (start + len - 1U) : start;
        kprintf("[ACPI-MMAP] #%d: 0x%08x - 0x%08x (%u KiB) [%s]\n",
                i, start, end, len / 1024U, acpi_mmap_type_name(mmap_table[i].type));
    }
    kprintf("[ACPI-MMAP] Source: %s | Usable RAM: %u MiB (%u KiB) | ACPI Tables: %u KiB\n",
            src_name, total_usable_bytes / (1024U * 1024U),
            total_usable_bytes / 1024U, total_acpi_bytes / 1024U);
}

bool_t acpi_has_rsdp(void)
{
    return rsdp_found;
}

uint32_t acpi_get_rsdp_addr(void)
{
    return rsdp_phys;
}

uint32_t acpi_get_rsdt_addr(void)
{
    return rsdt_phys;
}

int acpi_get_table_count(void)
{
    return num_acpi_tables;
}

int acpi_get_table_desc(int index, struct acpi_table_desc *out)
{
    if (index < 0 || index >= num_acpi_tables || out == NULL) {
        return -1;
    }
    *out = acpi_tables[index];
    return 0;
}

int acpi_get_mmap_count(void)
{
    return num_mmap_entries;
}

int acpi_get_mmap_entry(int index, struct acpi_mmap_entry *out)
{
    if (index < 0 || index >= num_mmap_entries || out == NULL) {
        return -1;
    }
    *out = mmap_table[index];
    return 0;
}

uint32_t acpi_get_total_usable_ram(void)
{
    return total_usable_bytes;
}

uint32_t acpi_find_usable_ram(uint32_t min_phys, uint32_t size, uint32_t align)
{
    int i;
    if (align == 0) {
        align = 4096U;
    }
    for (i = 0; i < num_mmap_entries; i++) {
        uint32_t r_start;
        uint32_t r_end;
        uint32_t cand;

        if (mmap_table[i].base_hi != 0 || mmap_table[i].type != ACPI_MEM_USABLE) {
            continue;
        }
        r_start = mmap_table[i].base_lo;
        if (mmap_table[i].length_hi != 0 ||
            r_start + mmap_table[i].length_lo < r_start) {
            r_end = 0xFFFFFFFFU;
        } else {
            r_end = r_start + mmap_table[i].length_lo;
        }
        cand = (r_start < min_phys) ? min_phys : r_start;
        if ((cand & (align - 1U)) != 0) {
            cand = (cand + align - 1U) & ~(align - 1U);
        }
        if (cand >= r_start && cand + size > cand && cand + size <= r_end) {
            return cand;
        }
    }
    return min_phys;
}

bool_t acpi_is_usable_ram(uint32_t phys_addr, uint32_t size)
{
    int i;
    for (i = 0; i < num_mmap_entries; i++) {
        uint32_t r_start;
        uint32_t r_end;
        if (mmap_table[i].base_hi != 0 || mmap_table[i].type != ACPI_MEM_USABLE) {
            continue;
        }
        r_start = mmap_table[i].base_lo;
        if (mmap_table[i].length_hi != 0 ||
            r_start + mmap_table[i].length_lo < r_start) {
            r_end = 0xFFFFFFFFU;
        } else {
            r_end = r_start + mmap_table[i].length_lo;
        }
        if (phys_addr >= r_start && phys_addr + size > phys_addr && phys_addr + size <= r_end) {
            return TRUE;
        }
    }
    return FALSE;
}
