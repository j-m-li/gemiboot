/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - FAT32 Filesystem Driver (Read/Write on /dev/sda)
 */

#include "fat32.h"
#include "xhci.h"
#include "serial.h"
#include "string.h"

#define FAT32_EOC_MIN      0x0FFFFFF8UL
#define FAT32_EOC_MARK     0x0FFFFFFFUL
#define FAT32_CLUSTER_MASK 0x0FFFFFFFUL

/* FAT Directory Entry Attributes */
#define FAT_ATTR_READ_ONLY 0x01U
#define FAT_ATTR_HIDDEN    0x02U
#define FAT_ATTR_SYSTEM    0x04U
#define FAT_ATTR_VOLUME_ID 0x08U
#define FAT_ATTR_DIRECTORY 0x10U
#define FAT_ATTR_ARCHIVE   0x20U
#define FAT_ATTR_LFN       0x0FU

/* Internal representation of a located 32-byte FAT32 directory entry */
struct fat32_loc {
    uint32_t dir_cluster;    /* First cluster of containing directory */
    uint32_t dir_lba;        /* Sector LBA containing the 32-byte SFN dirent */
    uint32_t dir_offset;     /* Byte offset (0..480, multiple of 32) within dir_lba */
    uint32_t lfn_first_slot; /* Starting slot index of LFN sequence (or SFN slot if none) */
    uint32_t lfn_count;      /* Number of preceding LFN entries (0..20) */
    uint32_t first_cluster;  /* First data cluster of file/directory */
    uint32_t file_size;      /* Size in bytes */
    uint8_t  attr;           /* Attribute byte */
    char     name[VFS_MAX_NAME];
};

static struct fat32_fs_info fs_mounts[FAT32_MAX_MOUNTS];
static struct fat32_fs_info *active_fs = &fs_mounts[0];
static bool_t   disk_auto_handled[XHCI_MAX_MSC_DEVS];
static const char *const fat32_mount_points[FAT32_MAX_MOUNTS] = {
    "/mnt", "/media1", "/media2", "/media3"
};
static uint8_t  sector_io_buf[512];
static uint32_t fat_cache_lba = 0xFFFFFFFFU;
static int      fat_cache_disk = -1;
static uint8_t  fat_cache_buf[512];
static uint32_t data_cache_lba = 0xFFFFFFFFU;
static int      data_cache_disk = -1;
static uint8_t  data_cache_buf[512];

static int32_t fat32_read_sec(uint32_t lba, uint8_t *buf512)
{
    if (data_cache_disk == active_fs->disk_idx && data_cache_lba == lba) {
        memcpy(buf512, data_cache_buf, 512);
        return 512;
    }
    if (xhci_msc_read_sector_idx(active_fs->disk_idx, lba, buf512) != 512) {
        return -1;
    }
    memcpy(data_cache_buf, buf512, 512);
    data_cache_disk = active_fs->disk_idx;
    data_cache_lba  = lba;
    return 512;
}

static int32_t fat32_write_sec(uint32_t lba, const uint8_t *buf512)
{
    if (xhci_msc_write_sector_idx(active_fs->disk_idx, lba, buf512) != 512) {
        data_cache_lba = 0xFFFFFFFFU;
        return -1;
    }
    memcpy(data_cache_buf, buf512, 512);
    data_cache_disk = active_fs->disk_idx;
    data_cache_lba  = lba;
    return 512;
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static char to_upper(char c)
{
    if (c >= 'a' && c <= 'z') {
        return (char)(c - ('a' - 'A'));
    }
    return c;
}

static int str_casecmp(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = to_upper(*a);
        char cb = to_upper(*b);
        if (ca != cb) {
            return (int)(unsigned char)ca - (int)(unsigned char)cb;
        }
        a++;
        b++;
    }
    return (int)(unsigned char)to_upper(*a) - (int)(unsigned char)to_upper(*b);
}

static uint32_t cluster_to_lba(uint32_t cluster)
{
    return active_fs->data_start_lba + (cluster - 2U) * active_fs->sectors_per_cluster;
}

static uint32_t fat32_get_next_cluster(uint32_t cluster)
{
    uint32_t fat_offset = cluster * 4U;
    uint32_t fat_lba    = active_fs->fat_start_lba + (fat_offset / 512U);
    uint32_t ent_offset = fat_offset % 512U;

    if (fat_cache_disk != active_fs->disk_idx || fat_cache_lba != fat_lba) {
        if (xhci_msc_read_sector_idx(active_fs->disk_idx, fat_lba, fat_cache_buf) != 512) {
            return FAT32_EOC_MARK;
        }
        fat_cache_disk = active_fs->disk_idx;
        fat_cache_lba  = fat_lba;
    }
    return read_le32(&fat_cache_buf[ent_offset]) & FAT32_CLUSTER_MASK;
}

static int fat32_set_next_cluster(uint32_t cluster, uint32_t next_val)
{
    uint32_t fat_offset = cluster * 4U;
    uint32_t sec_index  = fat_offset / 512U;
    uint32_t ent_offset = fat_offset % 512U;
    uint32_t f;

    for (f = 0; f < active_fs->num_fats; f++) {
        uint32_t fat_lba = active_fs->fat_start_lba + f * active_fs->fat_size_sectors + sec_index;
        uint32_t old_val;

        if (xhci_msc_read_sector_idx(active_fs->disk_idx, fat_lba, sector_io_buf) != 512) {
            return -1;
        }
        old_val = read_le32(&sector_io_buf[ent_offset]) & 0xF0000000UL;
        write_le32(&sector_io_buf[ent_offset], old_val | (next_val & FAT32_CLUSTER_MASK));
        if (xhci_msc_write_sector_idx(active_fs->disk_idx, fat_lba, sector_io_buf) != 512) {
            fat_cache_lba = 0xFFFFFFFFU;
            return -1;
        }
        if (f == 0) {
            memcpy(fat_cache_buf, sector_io_buf, 512);
            fat_cache_disk = active_fs->disk_idx;
            fat_cache_lba  = fat_lba;
        }
    }
    return 0;
}

static uint32_t fat32_alloc_cluster(void)
{
    uint32_t clus;
    uint32_t max_clus = active_fs->total_clusters + 2U;

    for (clus = 2U; clus < max_clus; clus++) {
        if (fat32_get_next_cluster(clus) == 0U) {
            uint32_t s;
            uint32_t lba;
            if (fat32_set_next_cluster(clus, FAT32_EOC_MARK) < 0) {
                return 0;
            }
            /* Zero the newly allocated cluster */
            memset(sector_io_buf, 0, 512);
            lba = cluster_to_lba(clus);
            for (s = 0; s < active_fs->sectors_per_cluster; s++) {
                (void)fat32_write_sec(lba + s, sector_io_buf);
            }
            return clus;
        }
    }
    return 0;
}

static void fat32_free_chain(uint32_t start_clus)
{
    uint32_t cur = start_clus;
    while (cur >= 2U && cur < FAT32_EOC_MIN) {
        uint32_t next = fat32_get_next_cluster(cur);
        (void)fat32_set_next_cluster(cur, 0U);
        cur = next;
    }
}

/* Byte offsets of the 13 UTF-16LE characters in a 32-byte VFAT LFN entry */
static const uint8_t lfn_char_offs[13] = {
    1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U, 24U, 28U, 30U
};

static uint8_t fat_lfn_checksum(const uint8_t *short11)
{
    uint8_t sum = 0;
    int i;
    for (i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1U) ? 0x80U : 0U) + (sum >> 1) + short11[i]);
    }
    return sum;
}

static bool_t is_valid_83_char(char c, bool_t allow_lower)
{
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return TRUE;
    }
    if (allow_lower && c >= 'a' && c <= 'z') {
        return TRUE;
    }
    if (c == '_' || c == '-' || c == '$' || c == '%' || c == '\'' ||
        c == '@' || c == '~' || c == '!' || c == '#' || c == '&' ||
        c == '(' || c == ')' || c == '{' || c == '}' || c == '^' || c == '`') {
        return TRUE;
    }
    return FALSE;
}

/* Return TRUE if name fits cleanly in a plain uppercase 8.3 short name (no LFN required) */
static bool_t fat_fits_plain_83(const char *name)
{
    int base_len = 0;
    int ext_len  = 0;
    bool_t in_ext = FALSE;
    const char *p = name;

    if (p == NULL || *p == '\0' || *p == '.') {
        return FALSE;
    }
    while (*p != '\0') {
        char c = *p++;
        if (c == '.') {
            if (in_ext || *p == '\0') {
                return FALSE;
            }
            in_ext = TRUE;
        } else if (!is_valid_83_char(c, FALSE)) {
            return FALSE;
        } else if (!in_ext) {
            base_len++;
            if (base_len > 8) {
                return FALSE;
            }
        } else {
            ext_len++;
            if (ext_len > 3) {
                return FALSE;
            }
        }
    }
    return (base_len >= 1) ? TRUE : FALSE;
}

static void fat_lfn_extract(const uint8_t *de, char *dst13)
{
    int i;
    for (i = 0; i < 13; i++) {
        uint16_t ch = read_le16(&de[lfn_char_offs[i]]);
        if (ch == 0x0000U || ch == 0xFFFFU) {
            dst13[i] = '\0';
            break;
        }
        dst13[i] = (ch < 0x80U) ? (char)ch : '?';
    }
}

static void fat_lfn_pack(uint8_t *de, uint8_t ord, uint8_t chksum,
                         const char *name, uint32_t name_len, uint32_t chunk_start)
{
    int i;
    memset(de, 0, 32);
    de[0]  = ord;
    de[11] = FAT_ATTR_LFN;
    de[12] = 0x00U;
    de[13] = chksum;
    de[26] = 0x00U;
    de[27] = 0x00U;

    for (i = 0; i < 13; i++) {
        uint32_t idx = chunk_start + (uint32_t)i;
        uint16_t ch;
        if (idx < name_len) {
            ch = (uint16_t)(uint8_t)name[idx];
        } else if (idx == name_len) {
            ch = 0x0000U;
        } else {
            ch = 0xFFFFU;
        }
        write_le16(&de[lfn_char_offs[i]], ch);
    }
}

/* Convert 11-byte 8.3 FAT name ("README  TXT") into standard string ("README.TXT") */
static void fat_83_to_str(const uint8_t *raw11, char *out)
{
    int i;
    int pos = 0;

    for (i = 0; i < 8 && raw11[i] != ' '; i++) {
        out[pos++] = (char)raw11[i];
    }
    if (raw11[8] != ' ') {
        out[pos++] = '.';
        for (i = 8; i < 11 && raw11[i] != ' '; i++) {
            out[pos++] = (char)raw11[i];
        }
    }
    out[pos] = '\0';
}

/* Format standard filename ("readme.txt") into 11-byte 8.3 FAT name ("README  TXT") */
static void str_to_fat_83(const char *name, uint8_t *out11)
{
    int i = 0;
    const char *p = name;

    memset(out11, ' ', 11);
    while (*p != '\0' && *p != '.' && i < 8) {
        out11[i++] = (uint8_t)to_upper(*p++);
    }
    while (*p != '\0' && *p != '.') {
        p++;
    }
    if (*p == '.') {
        p++;
        i = 8;
        while (*p != '\0' && i < 11) {
            out11[i++] = (uint8_t)to_upper(*p++);
        }
    }
}

static bool_t fat32_short_name_exists(uint32_t dir_cluster, const uint8_t *raw11)
{
    uint32_t cur_clus = dir_cluster;
    uint8_t sec_buf[512];

    while (cur_clus >= 2U && cur_clus < FAT32_EOC_MIN) {
        uint32_t base_lba = cluster_to_lba(cur_clus);
        uint32_t s;
        for (s = 0; s < active_fs->sectors_per_cluster; s++) {
            uint32_t off;
            if (fat32_read_sec(base_lba + s, sec_buf) != 512) {
                return FALSE;
            }
            for (off = 0; off < 512U; off += 32U) {
                uint8_t first = sec_buf[off];
                uint8_t attr  = sec_buf[off + 11U];
                if (first == 0x00U) {
                    return FALSE;
                }
                if (first == 0xE5U || attr == FAT_ATTR_LFN || (attr & FAT_ATTR_VOLUME_ID) != 0) {
                    continue;
                }
                if (memcmp(&sec_buf[off], raw11, 11) == 0) {
                    return TRUE;
                }
            }
        }
        cur_clus = fat32_get_next_cluster(cur_clus);
    }
    return FALSE;
}

/* Generate a unique 11-byte 8.3 short alias (e.g., "MYLONG~1TXT") for a long filename */
static void fat32_make_unique_short_name(uint32_t dir_cluster, const char *name, uint8_t *out11)
{
    char base[8];
    char ext[4];
    int base_len = 0;
    int ext_len  = 0;
    const char *last_dot = NULL;
    const char *p;
    int tail;

    for (p = name; *p != '\0'; p++) {
        if (*p == '.') {
            last_dot = p;
        }
    }
    if (last_dot == name) {
        last_dot = NULL;
    }

    for (p = name; *p != '\0' && p != last_dot && base_len < 6; p++) {
        char up = to_upper(*p);
        if (is_valid_83_char(up, FALSE)) {
            base[base_len++] = up;
        }
    }
    if (base_len == 0) {
        memcpy(base, "FILE", 4);
        base_len = 4;
    }

    if (last_dot != NULL) {
        for (p = last_dot + 1; *p != '\0' && ext_len < 3; p++) {
            char up = to_upper(*p);
            if (is_valid_83_char(up, FALSE)) {
                ext[ext_len++] = up;
            }
        }
    }

    for (tail = 1; tail <= 99; tail++) {
        int bcopy = base_len;
        int pos = 0;
        memset(out11, ' ', 11);
        if (tail >= 10 && bcopy > 5) {
            bcopy = 5;
        }
        while (pos < bcopy) {
            out11[pos] = (uint8_t)base[pos];
            pos++;
        }
        out11[pos++] = '~';
        if (tail >= 10) {
            out11[pos++] = (uint8_t)('0' + (tail / 10));
        }
        out11[pos++] = (uint8_t)('0' + (tail % 10));

        if (ext_len > 0) {
            memcpy(&out11[8], ext, (size_t)ext_len);
        }
        if (!fat32_short_name_exists(dir_cluster, out11)) {
            return;
        }
    }
}

/* Resolve directory slot index to (LBA, byte offset within sector) */
static int fat32_dir_slot_to_lba(uint32_t dir_cluster, uint32_t slot_idx,
                                 uint32_t *out_lba, uint32_t *out_off)
{
    uint32_t slots_per_clus = active_fs->sectors_per_cluster * 16U;
    uint32_t clus_skip = slot_idx / slots_per_clus;
    uint32_t rem       = slot_idx % slots_per_clus;
    uint32_t cur_clus  = dir_cluster;
    uint32_t i;

    for (i = 0; i < clus_skip; i++) {
        cur_clus = fat32_get_next_cluster(cur_clus);
        if (cur_clus < 2U || cur_clus >= FAT32_EOC_MIN) {
            return -1;
        }
    }
    *out_lba = cluster_to_lba(cur_clus) + (rem / 16U);
    *out_off = (rem % 16U) * 32U;
    return 0;
}

static int fat32_write_dir_slot(uint32_t dir_cluster, uint32_t slot_idx, const uint8_t *entry32)
{
    uint32_t lba, off;
    uint8_t sec_buf[512];

    if (fat32_dir_slot_to_lba(dir_cluster, slot_idx, &lba, &off) < 0) {
        return -1;
    }
    if (fat32_read_sec(lba, sec_buf) != 512) {
        return -1;
    }
    memcpy(&sec_buf[off], entry32, 32);
    return (fat32_write_sec(lba, sec_buf) == 512) ? 0 : -1;
}

static int fat32_mark_dir_slot_deleted(uint32_t dir_cluster, uint32_t slot_idx)
{
    uint32_t lba, off;
    uint8_t sec_buf[512];

    if (fat32_dir_slot_to_lba(dir_cluster, slot_idx, &lba, &off) < 0) {
        return -1;
    }
    if (fat32_read_sec(lba, sec_buf) != 512) {
        return -1;
    }
    sec_buf[off] = 0xE5U;
    return (fat32_write_sec(lba, sec_buf) == 512) ? 0 : -1;
}

/* Find entry by name (or by index if target_name == NULL) inside a directory cluster chain */
static int fat32_scan_dir(uint32_t dir_cluster, const char *target_name,
                          uint32_t target_idx, struct fat32_loc *out)
{
    uint32_t cur_clus = dir_cluster;
    uint32_t visible_idx = 0;
    uint32_t slot_idx = 0;
    uint8_t  sec_buf[512];

    char     lfn_buf[261];
    bool_t   lfn_active     = FALSE;
    uint8_t  lfn_checksum   = 0;
    uint8_t  lfn_expect_ord = 0;
    uint32_t lfn_first_slot = 0;
    uint32_t lfn_entry_cnt  = 0;

    while (cur_clus >= 2U && cur_clus < FAT32_EOC_MIN) {
        uint32_t base_lba = cluster_to_lba(cur_clus);
        uint32_t s;

        for (s = 0; s < active_fs->sectors_per_cluster; s++) {
            uint32_t lba = base_lba + s;
            uint32_t off;

            if (fat32_read_sec(lba, sec_buf) != 512) {
                return -1;
            }

            for (off = 0; off < 512U; off += 32U, slot_idx++) {
                const uint8_t *de = &sec_buf[off];
                uint8_t first_byte = de[0];
                uint8_t attr = de[11];
                char short_name[13];
                char entry_name[VFS_MAX_NAME];
                uint32_t fst_clus;
                bool_t has_lfn;

                if (first_byte == 0x00U) {
                    return -1; /* End of directory */
                }
                if (first_byte == 0xE5U) {
                    lfn_active = FALSE;
                    continue; /* Deleted entry */
                }
                if (attr == FAT_ATTR_LFN) {
                    if (de[12] == 0x00U && read_le16(&de[26]) == 0U) {
                        uint8_t ord = de[0];
                        if ((ord & 0x40U) != 0) {
                            uint8_t seq = (uint8_t)(ord & 0x1FU);
                            if (seq >= 1U && seq <= 20U) {
                                memset(lfn_buf, 0, sizeof(lfn_buf));
                                lfn_active     = TRUE;
                                lfn_checksum   = de[13];
                                lfn_expect_ord = (uint8_t)(seq - 1U);
                                lfn_first_slot = slot_idx;
                                lfn_entry_cnt  = (uint32_t)seq;
                                fat_lfn_extract(de, &lfn_buf[(uint32_t)(seq - 1U) * 13U]);
                            } else {
                                lfn_active = FALSE;
                            }
                        } else if (lfn_active && ord >= 1U && ord == lfn_expect_ord &&
                                   de[13] == lfn_checksum) {
                            fat_lfn_extract(de, &lfn_buf[(uint32_t)(ord - 1U) * 13U]);
                            lfn_expect_ord = (uint8_t)(ord - 1U);
                        } else {
                            lfn_active = FALSE;
                        }
                    } else {
                        lfn_active = FALSE;
                    }
                    continue;
                }
                if ((attr & FAT_ATTR_VOLUME_ID) != 0) {
                    lfn_active = FALSE;
                    continue; /* Skip Volume ID entries */
                }

                has_lfn = (lfn_active && lfn_expect_ord == 0U &&
                           fat_lfn_checksum(de) == lfn_checksum &&
                           lfn_buf[0] != '\0') ? TRUE : FALSE;
                lfn_active = FALSE;

                if (de[0] == '.') {
                    continue; /* Skip '.' and '..' in listings */
                }

                fat_83_to_str(de, short_name);
                if (has_lfn) {
                    size_t n = strlen(lfn_buf);
                    if (n >= VFS_MAX_NAME) {
                        n = VFS_MAX_NAME - 1U;
                    }
                    memcpy(entry_name, lfn_buf, n);
                    entry_name[n] = '\0';
                } else {
                    size_t n = strlen(short_name);
                    memcpy(entry_name, short_name, n + 1U);
                }

                fst_clus = ((uint32_t)read_le16(&de[20]) << 16) |
                           (uint32_t)read_le16(&de[26]);

                if (target_name != NULL) {
                    if (str_casecmp(entry_name, target_name) == 0 ||
                        str_casecmp(short_name, target_name) == 0) {
                        out->dir_cluster    = dir_cluster;
                        out->dir_lba        = lba;
                        out->dir_offset     = off;
                        out->lfn_first_slot = has_lfn ? lfn_first_slot : slot_idx;
                        out->lfn_count      = has_lfn ? lfn_entry_cnt : 0U;
                        out->first_cluster  = fst_clus;
                        out->file_size      = read_le32(&de[28]);
                        out->attr           = attr;
                        memcpy(out->name, entry_name, VFS_MAX_NAME);
                        return 0;
                    }
                } else {
                    if (visible_idx == target_idx) {
                        out->dir_cluster    = dir_cluster;
                        out->dir_lba        = lba;
                        out->dir_offset     = off;
                        out->lfn_first_slot = has_lfn ? lfn_first_slot : slot_idx;
                        out->lfn_count      = has_lfn ? lfn_entry_cnt : 0U;
                        out->first_cluster  = fst_clus;
                        out->file_size      = read_le32(&de[28]);
                        out->attr           = attr;
                        memcpy(out->name, entry_name, VFS_MAX_NAME);
                        return 0;
                    }
                    visible_idx++;
                }
            }
        }
        cur_clus = fat32_get_next_cluster(cur_clus);
    }
    return -1;
}

/* Resolve a relative path inside the FAT32 mount ("" or "/" -> root directory) */
static int fat32_resolve_rel(const char *rel_path, struct fat32_loc *out)
{
    const char *p = rel_path;
    uint32_t cur_dir_clus;

    if (!active_fs->mounted || rel_path == NULL) {
        return -1;
    }

    while (*p == '/') {
        p++;
    }
    if (*p == '\0') {
        /* Root directory of FAT32 volume */
        memset(out, 0, sizeof(*out));
        out->first_cluster = active_fs->root_cluster;
        out->attr          = FAT_ATTR_DIRECTORY;
        out->name[0]       = '/';
        out->name[1]       = '\0';
        return 0;
    }

    cur_dir_clus = active_fs->root_cluster;
    while (*p != '\0') {
        char comp[VFS_MAX_NAME];
        size_t len = 0;

        while (*p != '\0' && *p != '/') {
            if (len + 1U < VFS_MAX_NAME) {
                comp[len++] = *p;
            }
            p++;
        }
        comp[len] = '\0';

        while (*p == '/') {
            p++;
        }

        if (fat32_scan_dir(cur_dir_clus, comp, 0, out) < 0) {
            return -1;
        }
        if (*p != '\0') {
            if ((out->attr & FAT_ATTR_DIRECTORY) == 0) {
                return -1;
            }
            cur_dir_clus = out->first_cluster;
        }
    }
    return 0;
}

/* Split relative path into parent directory cluster and leaf filename */
static int fat32_resolve_parent(const char *rel_path, uint32_t *out_dir_clus, char *out_leaf)
{
    char parent_rel[VFS_MAX_PATH];
    size_t len, i;
    size_t last_slash = 0;
    bool_t has_slash = FALSE;
    struct fat32_loc parent_loc;

    while (*rel_path == '/') {
        rel_path++;
    }
    len = strlen(rel_path);
    if (len == 0 || len >= VFS_MAX_PATH) {
        return -1;
    }

    for (i = 0; i < len; i++) {
        if (rel_path[i] == '/') {
            last_slash = i;
            has_slash = TRUE;
        }
    }

    if (!has_slash) {
        if (len >= VFS_MAX_NAME) {
            return -1;
        }
        *out_dir_clus = active_fs->root_cluster;
        memcpy(out_leaf, rel_path, len + 1U);
        return 0;
    }

    memcpy(parent_rel, rel_path, last_slash);
    parent_rel[last_slash] = '\0';

    i = 0;
    while (rel_path[last_slash + 1U + i] != '\0' && i + 1U < VFS_MAX_NAME) {
        out_leaf[i] = rel_path[last_slash + 1U + i];
        i++;
    }
    out_leaf[i] = '\0';

    if (out_leaf[0] == '\0') {
        return -1;
    }
    if (fat32_resolve_rel(parent_rel, &parent_loc) < 0 ||
        (parent_loc.attr & FAT_ATTR_DIRECTORY) == 0) {
        return -1;
    }
    *out_dir_clus = parent_loc.first_cluster;
    return 0;
}

/* Find `needed` contiguous free directory slots, expanding the directory cluster chain if needed */
static int fat32_find_free_slots(uint32_t dir_cluster, uint32_t needed, uint32_t *out_start_slot)
{
    uint32_t cur_clus  = dir_cluster;
    uint32_t last_clus = dir_cluster;
    uint32_t slot_idx  = 0;
    uint32_t run_start = 0;
    uint32_t run_len   = 0;
    uint8_t  sec_buf[512];

    for (;;) {
        while (cur_clus >= 2U && cur_clus < FAT32_EOC_MIN) {
            uint32_t base_lba = cluster_to_lba(cur_clus);
            uint32_t s;
            last_clus = cur_clus;

            for (s = 0; s < active_fs->sectors_per_cluster; s++) {
                uint32_t off;
                if (fat32_read_sec(base_lba + s, sec_buf) != 512) {
                    return -1;
                }
                for (off = 0; off < 512U; off += 32U, slot_idx++) {
                    uint8_t first = sec_buf[off];
                    if (first == 0x00U || first == 0xE5U) {
                        if (run_len == 0) {
                            run_start = slot_idx;
                        }
                        run_len++;
                        if (run_len >= needed) {
                            *out_start_slot = run_start;
                            return 0;
                        }
                    } else {
                        run_len = 0;
                    }
                }
            }
            cur_clus = fat32_get_next_cluster(cur_clus);
        }

        /* Grow directory by allocating and linking a fresh zeroed cluster */
        cur_clus = fat32_alloc_cluster();
        if (cur_clus == 0) {
            return -1;
        }
        if (fat32_set_next_cluster(last_clus, cur_clus) < 0) {
            return -1;
        }
    }
}

static int fat32_select_mount(int mount_idx)
{
    if (mount_idx < 0 || mount_idx >= FAT32_MAX_MOUNTS || !fs_mounts[mount_idx].mounted) {
        return -1;
    }
    active_fs = &fs_mounts[mount_idx];
    return 0;
}

int fat32_create_path_idx(int mount_idx, const char *rel_path, bool_t is_dir)
{
    struct fat32_loc existing;
    uint32_t dir_clus;
    char leaf[VFS_MAX_NAME];
    uint32_t leaf_len;
    bool_t need_lfn;
    uint32_t lfn_cnt = 0;
    uint32_t run_start = 0;
    uint8_t short11[11];
    uint8_t sfn_entry[32];
    uint8_t sec_buf[512];
    uint32_t new_clus = 0;

    if (fat32_select_mount(mount_idx) < 0) {
        return -1;
    }
    if (fat32_resolve_rel(rel_path, &existing) == 0) {
        return 0; /* Already exists */
    }
    if (fat32_resolve_parent(rel_path, &dir_clus, leaf) < 0) {
        return -1;
    }

    leaf_len = (uint32_t)strlen(leaf);
    if (leaf_len == 0 || leaf_len > 255U) {
        return -1;
    }

    need_lfn = fat_fits_plain_83(leaf) ? FALSE : TRUE;
    if (need_lfn) {
        lfn_cnt = (leaf_len + 12U) / 13U;
        fat32_make_unique_short_name(dir_clus, leaf, short11);
    } else {
        str_to_fat_83(leaf, short11);
    }

    if (fat32_find_free_slots(dir_clus, lfn_cnt + 1U, &run_start) < 0) {
        return -1;
    }

    if (is_dir) {
        new_clus = fat32_alloc_cluster();
        if (new_clus == 0) {
            return -1;
        }
        /* Initialize '.' and '..' entries in the new directory cluster */
        memset(sec_buf, 0, 512);
        memset(&sec_buf[0], ' ', 11);
        sec_buf[0]  = '.';
        sec_buf[11] = FAT_ATTR_DIRECTORY;
        write_le16(&sec_buf[20], (uint16_t)((new_clus >> 16) & 0xFFFFU));
        write_le16(&sec_buf[26], (uint16_t)(new_clus & 0xFFFFU));

        memset(&sec_buf[32], ' ', 11);
        sec_buf[32] = '.';
        sec_buf[33] = '.';
        sec_buf[43] = FAT_ATTR_DIRECTORY;
        write_le16(&sec_buf[52], (uint16_t)((dir_clus >> 16) & 0xFFFFU));
        write_le16(&sec_buf[58], (uint16_t)(dir_clus & 0xFFFFU));

        (void)fat32_write_sec(cluster_to_lba(new_clus), sec_buf);
    }

    /* Write LFN entries in reverse sequence order before the 8.3 entry */
    if (need_lfn) {
        uint8_t chksum = fat_lfn_checksum(short11);
        uint32_t i;
        for (i = 0; i < lfn_cnt; i++) {
            uint8_t lfn_entry[32];
            uint32_t seq = lfn_cnt - i;
            uint8_t ord  = (uint8_t)(seq | ((i == 0U) ? 0x40U : 0x00U));
            fat_lfn_pack(lfn_entry, ord, chksum, leaf, leaf_len, (seq - 1U) * 13U);
            if (fat32_write_dir_slot(dir_clus, run_start + i, lfn_entry) < 0) {
                return -1;
            }
        }
    }

    /* Write the 8.3 Short File Name (SFN) directory entry */
    memset(sfn_entry, 0, 32);
    memcpy(sfn_entry, short11, 11);
    sfn_entry[11] = is_dir ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
    write_le16(&sfn_entry[20], (uint16_t)((new_clus >> 16) & 0xFFFFU));
    write_le16(&sfn_entry[26], (uint16_t)(new_clus & 0xFFFFU));
    write_le32(&sfn_entry[28], 0U);
    return fat32_write_dir_slot(dir_clus, run_start + lfn_cnt, sfn_entry);
}

int fat32_stat_path_idx(int mount_idx, const char *rel_path, struct vfs_dirent *out)
{
    struct fat32_loc loc;
    if (fat32_select_mount(mount_idx) < 0 || fat32_resolve_rel(rel_path, &loc) < 0) {
        return -1;
    }
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        memcpy(out->name, loc.name, VFS_MAX_NAME);
        out->inode = (loc.first_cluster != 0) ? loc.first_cluster : 2U;
        out->type  = ((loc.attr & FAT_ATTR_DIRECTORY) != 0) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
        out->size  = loc.file_size;
    }
    return 0;
}

int fat32_readdir_path_idx(int mount_idx, const char *rel_dir_path, uint32_t index, struct vfs_dirent *out)
{
    struct fat32_loc dir_loc;
    struct fat32_loc item_loc;

    if (fat32_select_mount(mount_idx) < 0 ||
        fat32_resolve_rel(rel_dir_path, &dir_loc) < 0 ||
        (dir_loc.attr & FAT_ATTR_DIRECTORY) == 0) {
        return -1;
    }
    if (fat32_scan_dir(dir_loc.first_cluster, NULL, index, &item_loc) < 0) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    memcpy(out->name, item_loc.name, VFS_MAX_NAME);
    out->inode = (item_loc.first_cluster != 0) ? item_loc.first_cluster : (index + 100U);
    out->type  = ((item_loc.attr & FAT_ATTR_DIRECTORY) != 0) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    out->size  = item_loc.file_size;
    return 0;
}

int32_t fat32_read_path_idx(int mount_idx, const char *rel_path, uint32_t offset, uint32_t size, uint8_t *buf)
{
    struct fat32_loc loc;
    uint32_t cur_clus;
    uint32_t clus_idx;
    uint32_t skip_clusters;
    uint32_t bytes_read = 0;
    uint8_t  sec_buf[512];

    if (fat32_select_mount(mount_idx) < 0 ||
        fat32_resolve_rel(rel_path, &loc) < 0 ||
        (loc.attr & FAT_ATTR_DIRECTORY) != 0) {
        return -1;
    }
    if (offset >= loc.file_size || loc.first_cluster < 2U) {
        return 0;
    }
    if (offset + size > loc.file_size) {
        size = loc.file_size - offset;
    }

    cur_clus = loc.first_cluster;
    skip_clusters = offset / active_fs->bytes_per_cluster;
    for (clus_idx = 0; clus_idx < skip_clusters; clus_idx++) {
        cur_clus = fat32_get_next_cluster(cur_clus);
        if (cur_clus < 2U || cur_clus >= FAT32_EOC_MIN) {
            return 0;
        }
    }

    while (bytes_read < size && cur_clus >= 2U && cur_clus < FAT32_EOC_MIN) {
        uint32_t cur_pos     = offset + bytes_read;
        uint32_t clus_offset = cur_pos % active_fs->bytes_per_cluster;
        uint32_t sec_in_clus = clus_offset / 512U;
        uint32_t off_in_sec  = clus_offset % 512U;
        uint32_t lba         = cluster_to_lba(cur_clus) + sec_in_clus;
        uint32_t chunk       = 512U - off_in_sec;

        if (chunk > size - bytes_read) {
            chunk = size - bytes_read;
        }
        if (xhci_msc_read_sector_idx(active_fs->disk_idx, lba, sec_buf) != 512) {
            break;
        }
        memcpy(&buf[bytes_read], &sec_buf[off_in_sec], chunk);
        bytes_read += chunk;

        if (((offset + bytes_read) % active_fs->bytes_per_cluster) == 0) {
            cur_clus = fat32_get_next_cluster(cur_clus);
        }
    }

    return (int32_t)bytes_read;
}

int32_t fat32_write_path_idx(int mount_idx, const char *rel_path, uint32_t offset, uint32_t size,
                             const uint8_t *buf, bool_t truncate)
{
    struct fat32_loc loc;
    uint32_t cur_clus;
    uint32_t bytes_written = 0;
    uint8_t  sec_buf[512];

    if (fat32_select_mount(mount_idx) < 0) {
        return -1;
    }
    if (fat32_resolve_rel(rel_path, &loc) < 0) {
        if (fat32_create_path_idx(mount_idx, rel_path, FALSE) < 0 ||
            fat32_resolve_rel(rel_path, &loc) < 0) {
            return -1;
        }
    }
    if ((loc.attr & FAT_ATTR_DIRECTORY) != 0) {
        return -1;
    }

    if (truncate && loc.first_cluster >= 2U) {
        fat32_free_chain(loc.first_cluster);
        loc.first_cluster = 0;
        loc.file_size = 0;
    }

    if (size == 0) {
        goto update_dirent;
    }

    if (loc.first_cluster < 2U) {
        loc.first_cluster = fat32_alloc_cluster();
        if (loc.first_cluster == 0) {
            return -1;
        }
    }

    /* Advance to the cluster corresponding to offset, allocating if needed */
    {
        uint32_t skip_clusters = offset / active_fs->bytes_per_cluster;
        uint32_t i;
        cur_clus = loc.first_cluster;
        for (i = 0; i < skip_clusters; i++) {
            uint32_t next = fat32_get_next_cluster(cur_clus);
            if (next < 2U || next >= FAT32_EOC_MIN) {
                next = fat32_alloc_cluster();
                if (next == 0) {
                    return -1;
                }
                (void)fat32_set_next_cluster(cur_clus, next);
            }
            cur_clus = next;
        }
    }

    while (bytes_written < size) {
        uint32_t cur_pos     = offset + bytes_written;
        uint32_t clus_offset = cur_pos % active_fs->bytes_per_cluster;
        uint32_t sec_in_clus = clus_offset / 512U;
        uint32_t off_in_sec  = clus_offset % 512U;
        uint32_t lba         = cluster_to_lba(cur_clus) + sec_in_clus;
        uint32_t chunk       = 512U - off_in_sec;

        if (chunk > size - bytes_written) {
            chunk = size - bytes_written;
        }
        if (off_in_sec != 0 || chunk < 512U) {
            if (fat32_read_sec(lba, sec_buf) != 512) {
                break;
            }
        }
        memcpy(&sec_buf[off_in_sec], &buf[bytes_written], chunk);
        if (fat32_write_sec(lba, sec_buf) != 512) {
            break;
        }
        bytes_written += chunk;

        if (bytes_written < size &&
            ((offset + bytes_written) % active_fs->bytes_per_cluster) == 0) {
            uint32_t next = fat32_get_next_cluster(cur_clus);
            if (next < 2U || next >= FAT32_EOC_MIN) {
                next = fat32_alloc_cluster();
                if (next == 0) {
                    break;
                }
                (void)fat32_set_next_cluster(cur_clus, next);
            }
            cur_clus = next;
        }
    }

    if (offset + bytes_written > loc.file_size) {
        loc.file_size = offset + bytes_written;
    }

update_dirent:
    /* Write updated first_cluster and file_size back to the 32-byte directory entry */
    if (fat32_read_sec(loc.dir_lba, sec_buf) == 512) {
        uint8_t *de = &sec_buf[loc.dir_offset];
        write_le16(&de[20], (uint16_t)((loc.first_cluster >> 16) & 0xFFFFU));
        write_le16(&de[26], (uint16_t)(loc.first_cluster & 0xFFFFU));
        write_le32(&de[28], loc.file_size);
        (void)fat32_write_sec(loc.dir_lba, sec_buf);
    }

    return (int32_t)bytes_written;
}

int fat32_unlink_path_idx(int mount_idx, const char *rel_path)
{
    struct fat32_loc loc;
    uint32_t k;

    if (fat32_select_mount(mount_idx) < 0 || rel_path == NULL || rel_path[0] == '\0') {
        return -1;
    }
    if (fat32_resolve_rel(rel_path, &loc) < 0 ||
        (loc.attr & FAT_ATTR_DIRECTORY) != 0) {
        return -1;
    }
    if (loc.first_cluster >= 2U) {
        fat32_free_chain(loc.first_cluster);
    }
    /* Mark all preceding LFN entries and the 8.3 SFN entry as deleted (0xE5) */
    for (k = 0; k <= loc.lfn_count; k++) {
        if (fat32_mark_dir_slot_deleted(loc.dir_cluster, loc.lfn_first_slot + k) < 0) {
            return -1;
        }
    }
    return 0;
}

int fat32_truncate_path_idx(int mount_idx, const char *rel_path, uint32_t new_size)
{
    struct fat32_loc loc;
    uint8_t sec_buf[512];

    if (fat32_select_mount(mount_idx) < 0 || rel_path == NULL || rel_path[0] == '\0') {
        return -1;
    }
    if (fat32_resolve_rel(rel_path, &loc) < 0 ||
        (loc.attr & FAT_ATTR_DIRECTORY) != 0) {
        return -1;
    }
    if (new_size == 0) {
        if (loc.first_cluster >= 2U) {
            fat32_free_chain(loc.first_cluster);
            loc.first_cluster = 0;
        }
        loc.file_size = 0;
    } else if (new_size > loc.file_size) {
        uint8_t zero = 0;
        if (fat32_write_path_idx(mount_idx, rel_path, new_size - 1U, 1U, &zero, FALSE) < 0) {
            return -1;
        }
        return 0;
    } else {
        loc.file_size = new_size;
    }

    if (fat32_read_sec(loc.dir_lba, sec_buf) != 512) {
        return -1;
    }
    write_le16(&sec_buf[loc.dir_offset + 20U], (uint16_t)((loc.first_cluster >> 16) & 0xFFFFU));
    write_le16(&sec_buf[loc.dir_offset + 26U], (uint16_t)(loc.first_cluster & 0xFFFFU));
    write_le32(&sec_buf[loc.dir_offset + 28U], loc.file_size);
    return (fat32_write_sec(loc.dir_lba, sec_buf) == 512) ? 0 : -1;
}

static void fat32_invalidate_cache_for_disk(int disk_idx)
{
    if (fat_cache_disk == disk_idx) {
        fat_cache_disk = -1;
        fat_cache_lba  = 0xFFFFFFFFU;
    }
    if (data_cache_disk == disk_idx) {
        data_cache_disk = -1;
        data_cache_lba  = 0xFFFFFFFFU;
    }
}

static int fat32_try_mount_lba(int mount_idx, int disk_idx, uint32_t lba)
{
    struct fat32_fs_info *fs = &fs_mounts[mount_idx];
    uint8_t bpb[512];
    uint16_t bytes_per_sec;
    uint8_t  sec_per_clus;
    uint16_t rsvd_sec_cnt;
    uint8_t  num_fats;
    uint32_t tot_sec_32;
    uint32_t fat_sz_32;
    uint32_t root_clus;
    uint32_t data_sec;
    size_t mp_len;

    if (xhci_msc_read_sector_idx(disk_idx, lba, bpb) != 512) {
        return -1;
    }
    if (bpb[510] != 0x55U || bpb[511] != 0xAAU) {
        return -1;
    }
    /* Verify "FAT32   " signature at offset 0x52 (82) */
    if (memcmp(&bpb[0x52], "FAT32   ", 8) != 0) {
        return -1;
    }

    bytes_per_sec = read_le16(&bpb[0x0B]);
    sec_per_clus  = bpb[0x0D];
    rsvd_sec_cnt  = read_le16(&bpb[0x0E]);
    num_fats      = bpb[0x10];
    tot_sec_32    = read_le32(&bpb[0x20]);
    fat_sz_32     = read_le32(&bpb[0x24]);
    root_clus     = read_le32(&bpb[0x2C]);

    if (bytes_per_sec != 512U || sec_per_clus == 0 || fat_sz_32 == 0) {
        return -1;
    }

    memset(fs, 0, sizeof(*fs));
    fs->mounted             = TRUE;
    fs->disk_idx            = disk_idx;
    mp_len = strlen(fat32_mount_points[mount_idx]);
    if (mp_len >= sizeof(fs->mount_point)) {
        mp_len = sizeof(fs->mount_point) - 1U;
    }
    memcpy(fs->mount_point, fat32_mount_points[mount_idx], mp_len + 1U);
    fs->part_lba            = lba;
    fs->fat_start_lba       = lba + rsvd_sec_cnt;
    fs->data_start_lba      = fs->fat_start_lba + ((uint32_t)num_fats * fat_sz_32);
    fs->sectors_per_cluster = sec_per_clus;
    fs->bytes_per_cluster   = (uint32_t)sec_per_clus * 512U;
    fs->fat_size_sectors    = fat_sz_32;
    fs->num_fats            = num_fats;
    fs->root_cluster        = root_clus;

    data_sec = tot_sec_32 - (rsvd_sec_cnt + ((uint32_t)num_fats * fat_sz_32));
    fs->total_clusters = data_sec / sec_per_clus;

    memcpy(fs->volume_label, &bpb[0x47], 11);
    fs->volume_label[11] = '\0';

    fat32_invalidate_cache_for_disk(disk_idx);
    kprintf("[FAT32] Mounted volume \"%s\" at %s (/dev/sd%c, part_lba=%u, clusters=%u, %u B/clus)\n",
            fs->volume_label, fs->mount_point, (char)('a' + disk_idx),
            fs->part_lba, fs->total_clusters, fs->bytes_per_cluster);
    return 0;
}

static bool_t fat32_disk_is_boot(int disk_idx)
{
    uint8_t sec0[512];
    int i;

    if (xhci_msc_read_sector_idx(disk_idx, 0, sec0) != 512) {
        return FALSE;
    }
    /* Oksinix MBR signature embedded at offset 3 of boot/mbr.S */
    if (memcmp(&sec0[3], "OKSINIX_USB_STORAGE_OK", 22) == 0) {
        return TRUE;
    }
    /* Direct FAT32 superfloppy with Oksinix volume label */
    if (sec0[510] == 0x55U && sec0[511] == 0xAAU &&
        memcmp(&sec0[0x52], "FAT32   ", 8) == 0 &&
        memcmp(&sec0[0x47], "OKSINIXFAT ", 11) == 0) {
        return TRUE;
    }
    /* Partitioned disk containing Oksinix FAT32 volume label */
    if (sec0[510] == 0x55U && sec0[511] == 0xAAU) {
        for (i = 0; i < 4; i++) {
            const uint8_t *pte = &sec0[446 + i * 16];
            uint8_t part_type  = pte[4];
            uint32_t start_lba = read_le32(&pte[8]);
            if (start_lba > 0 && (part_type == 0x0BU || part_type == 0x0CU || part_type == 0x83U)) {
                uint8_t bpb[512];
                if (xhci_msc_read_sector_idx(disk_idx, start_lba, bpb) == 512 &&
                    bpb[510] == 0x55U && bpb[511] == 0xAAU &&
                    memcmp(&bpb[0x52], "FAT32   ", 8) == 0 &&
                    memcmp(&bpb[0x47], "OKSINIXFAT ", 11) == 0) {
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}

static bool_t fat32_disk_is_mounted(int disk_idx)
{
    int m;
    for (m = 0; m < FAT32_MAX_MOUNTS; m++) {
        if (fs_mounts[m].mounted && fs_mounts[m].disk_idx == disk_idx) {
            return TRUE;
        }
    }
    return FALSE;
}

int fat32_mount_disk(int disk_idx, int mount_idx)
{
    uint8_t mbr[512];
    struct xhci_msc_info msc;
    int i;

    if (disk_idx < 0 || disk_idx >= XHCI_MAX_MSC_DEVS ||
        mount_idx < 0 || mount_idx >= FAT32_MAX_MOUNTS) {
        return -1;
    }
    if (fs_mounts[mount_idx].mounted || fat32_disk_is_mounted(disk_idx)) {
        return -1;
    }
    if (xhci_get_msc_info_idx(disk_idx, &msc) < 0 || !msc.present) {
        return -1;
    }

    /* Check if LBA 0 is directly a FAT32 VBR */
    if (fat32_try_mount_lba(mount_idx, disk_idx, 0) == 0) {
        disk_auto_handled[disk_idx] = TRUE;
        return 0;
    }

    /* Otherwise scan MBR Partition Table (offsets 446..509) for a FAT32 partition */
    if (xhci_msc_read_sector_idx(disk_idx, 0, mbr) != 512) {
        return -1;
    }
    if (mbr[510] == 0x55U && mbr[511] == 0xAAU) {
        for (i = 0; i < 4; i++) {
            const uint8_t *pte = &mbr[446 + i * 16];
            uint8_t part_type  = pte[4];
            uint32_t start_lba = read_le32(&pte[8]);

            if (start_lba > 0 && (part_type == 0x0BU || part_type == 0x0CU || part_type == 0x83U)) {
                if (fat32_try_mount_lba(mount_idx, disk_idx, start_lba) == 0) {
                    disk_auto_handled[disk_idx] = TRUE;
                    return 0;
                }
            }
        }
    }
    return -1;
}

int fat32_umount_idx(int mount_idx)
{
    int disk_idx;

    if (mount_idx < 0 || mount_idx >= FAT32_MAX_MOUNTS || !fs_mounts[mount_idx].mounted) {
        return -1;
    }

    disk_idx = fs_mounts[mount_idx].disk_idx;
    fat32_invalidate_cache_for_disk(disk_idx);
    vfs_invalidate_fat32_mount(mount_idx);

    kprintf("[FAT32] Unmounted %s (/dev/sd%c)\n",
            fs_mounts[mount_idx].mount_point, (char)('a' + disk_idx));

    memset(&fs_mounts[mount_idx], 0, sizeof(fs_mounts[mount_idx]));
    fs_mounts[mount_idx].disk_idx = -1;
    return 0;
}

int fat32_umount_path(const char *mount_point)
{
    int m;

    if (mount_point == NULL) {
        return -1;
    }
    for (m = 0; m < FAT32_MAX_MOUNTS; m++) {
        if (strcmp(mount_point, fat32_mount_points[m]) == 0) {
            return fat32_umount_idx(m);
        }
    }
    return -1;
}

void fat32_sync_mounts(void)
{
    struct xhci_msc_info msc;
    int m, d;

    /* 1. Auto-unmount any volume whose backing USB mass storage was disconnected */
    for (m = 0; m < FAT32_MAX_MOUNTS; m++) {
        if (fs_mounts[m].mounted) {
            if (xhci_get_msc_info_idx(fs_mounts[m].disk_idx, &msc) < 0 || !msc.present) {
                (void)fat32_umount_idx(m);
            }
        }
    }

    /* 2. Reset auto-mount tracking for disconnected disk slots so re-plugging auto-mounts */
    for (d = 0; d < XHCI_MAX_MSC_DEVS; d++) {
        if (xhci_get_msc_info_idx(d, &msc) < 0 || !msc.present) {
            disk_auto_handled[d] = FALSE;
        }
    }

    /* 3. Pass 1: mount the boot mass storage to /mnt (slot 0) */
    if (!fs_mounts[0].mounted) {
        for (d = 0; d < XHCI_MAX_MSC_DEVS; d++) {
            if (disk_auto_handled[d] || fat32_disk_is_mounted(d)) {
                continue;
            }
            if (xhci_get_msc_info_idx(d, &msc) == 0 && msc.present && fat32_disk_is_boot(d)) {
                if (fat32_mount_disk(d, 0) == 0) {
                    break;
                }
            }
        }
    }

    /* 4. Pass 2: mount other FAT32 mass storage devices to /media1, /media2, /media3 (or /mnt if free) */
    for (d = 0; d < XHCI_MAX_MSC_DEVS; d++) {
        int target_slot = -1;
        if (disk_auto_handled[d] || fat32_disk_is_mounted(d)) {
            continue;
        }
        if (xhci_get_msc_info_idx(d, &msc) < 0 || !msc.present) {
            continue;
        }
        if (!fs_mounts[0].mounted) {
            target_slot = 0;
        } else {
            for (m = 1; m < FAT32_MAX_MOUNTS; m++) {
                if (!fs_mounts[m].mounted) {
                    target_slot = m;
                    break;
                }
            }
        }
        if (target_slot >= 0) {
            (void)fat32_mount_disk(d, target_slot);
        }
        disk_auto_handled[d] = TRUE;
    }
}

void fat32_remount_all(void)
{
    int d;
    for (d = 0; d < XHCI_MAX_MSC_DEVS; d++) {
        disk_auto_handled[d] = FALSE;
    }
    fat32_sync_mounts();
}

int fat32_init(void)
{
    int m;

    memset(fs_mounts, 0, sizeof(fs_mounts));
    for (m = 0; m < FAT32_MAX_MOUNTS; m++) {
        fs_mounts[m].disk_idx = -1;
    }
    memset(disk_auto_handled, 0, sizeof(disk_auto_handled));
    active_fs       = &fs_mounts[0];
    fat_cache_lba   = 0xFFFFFFFFU;
    fat_cache_disk  = -1;
    data_cache_lba  = 0xFFFFFFFFU;
    data_cache_disk = -1;

    fat32_sync_mounts();

    for (m = 0; m < FAT32_MAX_MOUNTS; m++) {
        if (fs_mounts[m].mounted) {
            return 0;
        }
    }
    return -1;
}

bool_t fat32_is_mounted_idx(int mount_idx)
{
    if (mount_idx < 0 || mount_idx >= FAT32_MAX_MOUNTS) {
        return FALSE;
    }
    return fs_mounts[mount_idx].mounted;
}

bool_t fat32_is_mounted(void)
{
    return fs_mounts[0].mounted;
}

int fat32_get_info_idx(int mount_idx, struct fat32_fs_info *out)
{
    if (mount_idx < 0 || mount_idx >= FAT32_MAX_MOUNTS) {
        return -1;
    }
    if (out != NULL) {
        *out = fs_mounts[mount_idx];
    }
    return 0;
}

void fat32_get_info(struct fat32_fs_info *out)
{
    (void)fat32_get_info_idx(0, out);
}

const char *fat32_get_mount_point(int mount_idx)
{
    if (mount_idx < 0 || mount_idx >= FAT32_MAX_MOUNTS) {
        return "";
    }
    return fat32_mount_points[mount_idx];
}

int fat32_match_mount(const char *abs_path, const char **out_rel)
{
    int m;

    if (abs_path == NULL) {
        return -1;
    }
    for (m = 0; m < FAT32_MAX_MOUNTS; m++) {
        const char *mp;
        size_t mplen;
        if (!fs_mounts[m].mounted) {
            continue;
        }
        mp    = fat32_mount_points[m];
        mplen = strlen(mp);
        if (strcmp(abs_path, mp) == 0) {
            *out_rel = "";
            return m;
        }
        if (strncmp(abs_path, mp, mplen) == 0 && abs_path[mplen] == '/') {
            *out_rel = &abs_path[mplen + 1U];
            return m;
        }
    }
    return -1;
}
