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
    uint32_t dir_lba;       /* Sector LBA containing the 32-byte dirent */
    uint32_t dir_offset;    /* Byte offset (0..480, multiple of 32) within dir_lba */
    uint32_t first_cluster; /* First data cluster of file/directory */
    uint32_t file_size;     /* Size in bytes */
    uint8_t  attr;          /* Attribute byte */
    char     name[VFS_MAX_NAME];
};

static struct fat32_fs_info fs_state;
static uint8_t  sector_io_buf[512];
static uint32_t fat_cache_lba = 0xFFFFFFFFU;
static int      fat_cache_disk = -1;
static uint8_t  fat_cache_buf[512];
static uint32_t data_cache_lba = 0xFFFFFFFFU;
static int      data_cache_disk = -1;
static uint8_t  data_cache_buf[512];

static int32_t fat32_read_sec(uint32_t lba, uint8_t *buf512)
{
    if (data_cache_disk == fs_state.disk_idx && data_cache_lba == lba) {
        memcpy(buf512, data_cache_buf, 512);
        return 512;
    }
    if (xhci_msc_read_sector_idx(fs_state.disk_idx, lba, buf512) != 512) {
        return -1;
    }
    memcpy(data_cache_buf, buf512, 512);
    data_cache_disk = fs_state.disk_idx;
    data_cache_lba  = lba;
    return 512;
}

static int32_t fat32_write_sec(uint32_t lba, const uint8_t *buf512)
{
    if (xhci_msc_write_sector_idx(fs_state.disk_idx, lba, buf512) != 512) {
        data_cache_lba = 0xFFFFFFFFU;
        return -1;
    }
    memcpy(data_cache_buf, buf512, 512);
    data_cache_disk = fs_state.disk_idx;
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
    return fs_state.data_start_lba + (cluster - 2U) * fs_state.sectors_per_cluster;
}

static uint32_t fat32_get_next_cluster(uint32_t cluster)
{
    uint32_t fat_offset = cluster * 4U;
    uint32_t fat_lba    = fs_state.fat_start_lba + (fat_offset / 512U);
    uint32_t ent_offset = fat_offset % 512U;

    if (fat_cache_disk != fs_state.disk_idx || fat_cache_lba != fat_lba) {
        if (xhci_msc_read_sector_idx(fs_state.disk_idx, fat_lba, fat_cache_buf) != 512) {
            return FAT32_EOC_MARK;
        }
        fat_cache_disk = fs_state.disk_idx;
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

    for (f = 0; f < fs_state.num_fats; f++) {
        uint32_t fat_lba = fs_state.fat_start_lba + f * fs_state.fat_size_sectors + sec_index;
        uint32_t old_val;

        if (xhci_msc_read_sector_idx(fs_state.disk_idx, fat_lba, sector_io_buf) != 512) {
            return -1;
        }
        old_val = read_le32(&sector_io_buf[ent_offset]) & 0xF0000000UL;
        write_le32(&sector_io_buf[ent_offset], old_val | (next_val & FAT32_CLUSTER_MASK));
        if (xhci_msc_write_sector_idx(fs_state.disk_idx, fat_lba, sector_io_buf) != 512) {
            fat_cache_lba = 0xFFFFFFFFU;
            return -1;
        }
        if (f == 0) {
            memcpy(fat_cache_buf, sector_io_buf, 512);
            fat_cache_disk = fs_state.disk_idx;
            fat_cache_lba  = fat_lba;
        }
    }
    return 0;
}

static uint32_t fat32_alloc_cluster(void)
{
    uint32_t clus;
    uint32_t max_clus = fs_state.total_clusters + 2U;

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
            for (s = 0; s < fs_state.sectors_per_cluster; s++) {
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

/* Find entry by name (or by index if target_name == NULL) inside a directory cluster chain */
static int fat32_scan_dir(uint32_t dir_cluster, const char *target_name,
                          uint32_t target_idx, struct fat32_loc *out)
{
    uint32_t cur_clus = dir_cluster;
    uint32_t visible_idx = 0;
    uint8_t  sec_buf[512];

    while (cur_clus >= 2U && cur_clus < FAT32_EOC_MIN) {
        uint32_t base_lba = cluster_to_lba(cur_clus);
        uint32_t s;

        for (s = 0; s < fs_state.sectors_per_cluster; s++) {
            uint32_t lba = base_lba + s;
            uint32_t off;

            if (fat32_read_sec(lba, sec_buf) != 512) {
                return -1;
            }

            for (off = 0; off < 512U; off += 32U) {
                const uint8_t *de = &sec_buf[off];
                uint8_t first_byte = de[0];
                uint8_t attr = de[11];
                char entry_name[VFS_MAX_NAME];
                uint32_t fst_clus;

                if (first_byte == 0x00U) {
                    return -1; /* End of directory */
                }
                if (first_byte == 0xE5U) {
                    continue; /* Deleted entry */
                }
                if (attr == FAT_ATTR_LFN || (attr & FAT_ATTR_VOLUME_ID) != 0) {
                    continue; /* Skip LFN and Volume ID entries */
                }
                if (de[0] == '.') {
                    continue; /* Skip '.' and '..' in listings */
                }

                fat_83_to_str(de, entry_name);
                fst_clus = ((uint32_t)read_le16(&de[20]) << 16) |
                           (uint32_t)read_le16(&de[26]);

                if (target_name != NULL) {
                    if (str_casecmp(entry_name, target_name) == 0) {
                        out->dir_lba       = lba;
                        out->dir_offset    = off;
                        out->first_cluster = fst_clus;
                        out->file_size     = read_le32(&de[28]);
                        out->attr          = attr;
                        memcpy(out->name, entry_name, VFS_MAX_NAME);
                        return 0;
                    }
                } else {
                    if (visible_idx == target_idx) {
                        out->dir_lba       = lba;
                        out->dir_offset    = off;
                        out->first_cluster = fst_clus;
                        out->file_size     = read_le32(&de[28]);
                        out->attr          = attr;
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

    if (!fs_state.mounted || rel_path == NULL) {
        return -1;
    }

    while (*p == '/') {
        p++;
    }
    if (*p == '\0') {
        /* Root directory of FAT32 volume */
        memset(out, 0, sizeof(*out));
        out->first_cluster = fs_state.root_cluster;
        out->attr          = FAT_ATTR_DIRECTORY;
        out->name[0]       = '/';
        out->name[1]       = '\0';
        return 0;
    }

    cur_dir_clus = fs_state.root_cluster;
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
        *out_dir_clus = fs_state.root_cluster;
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

int fat32_create_path(const char *rel_path, bool_t is_dir)
{
    struct fat32_loc existing;
    uint32_t dir_clus;
    char leaf[VFS_MAX_NAME];
    uint32_t cur_clus;
    uint8_t sec_buf[512];
    uint32_t new_clus = 0;

    if (!fs_state.mounted) {
        return -1;
    }
    if (fat32_resolve_rel(rel_path, &existing) == 0) {
        return 0; /* Already exists */
    }
    if (fat32_resolve_parent(rel_path, &dir_clus, leaf) < 0) {
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

    /* Find a free 32-byte slot (0x00 or 0xE5) in parent directory */
    cur_clus = dir_clus;
    while (cur_clus >= 2U && cur_clus < FAT32_EOC_MIN) {
        uint32_t base_lba = cluster_to_lba(cur_clus);
        uint32_t s;

        for (s = 0; s < fs_state.sectors_per_cluster; s++) {
            uint32_t lba = base_lba + s;
            uint32_t off;

            if (fat32_read_sec(lba, sec_buf) != 512) {
                return -1;
            }
            for (off = 0; off < 512U; off += 32U) {
                if (sec_buf[off] == 0x00U || sec_buf[off] == 0xE5U) {
                    uint8_t *de = &sec_buf[off];
                    memset(de, 0, 32);
                    str_to_fat_83(leaf, de);
                    de[11] = is_dir ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
                    write_le16(&de[20], (uint16_t)((new_clus >> 16) & 0xFFFFU));
                    write_le16(&de[26], (uint16_t)(new_clus & 0xFFFFU));
                    write_le32(&de[28], 0U);
                    return (fat32_write_sec(lba, sec_buf) == 512) ? 0 : -1;
                }
            }
        }
        cur_clus = fat32_get_next_cluster(cur_clus);
    }

    return -1;
}

int fat32_stat_path(const char *rel_path, struct vfs_dirent *out)
{
    struct fat32_loc loc;
    if (fat32_resolve_rel(rel_path, &loc) < 0) {
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

int fat32_readdir_path(const char *rel_dir_path, uint32_t index, struct vfs_dirent *out)
{
    struct fat32_loc dir_loc;
    struct fat32_loc item_loc;

    if (fat32_resolve_rel(rel_dir_path, &dir_loc) < 0 ||
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

int32_t fat32_read_path(const char *rel_path, uint32_t offset, uint32_t size, uint8_t *buf)
{
    struct fat32_loc loc;
    uint32_t cur_clus;
    uint32_t clus_idx;
    uint32_t skip_clusters;
    uint32_t bytes_read = 0;
    uint8_t  sec_buf[512];

    if (fat32_resolve_rel(rel_path, &loc) < 0 ||
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
    skip_clusters = offset / fs_state.bytes_per_cluster;
    for (clus_idx = 0; clus_idx < skip_clusters; clus_idx++) {
        cur_clus = fat32_get_next_cluster(cur_clus);
        if (cur_clus < 2U || cur_clus >= FAT32_EOC_MIN) {
            return 0;
        }
    }

    while (bytes_read < size && cur_clus >= 2U && cur_clus < FAT32_EOC_MIN) {
        uint32_t cur_pos     = offset + bytes_read;
        uint32_t clus_offset = cur_pos % fs_state.bytes_per_cluster;
        uint32_t sec_in_clus = clus_offset / 512U;
        uint32_t off_in_sec  = clus_offset % 512U;
        uint32_t lba         = cluster_to_lba(cur_clus) + sec_in_clus;
        uint32_t chunk       = 512U - off_in_sec;

        if (chunk > size - bytes_read) {
            chunk = size - bytes_read;
        }
        if (xhci_msc_read_sector_idx(fs_state.disk_idx, lba, sec_buf) != 512) {
            break;
        }
        memcpy(&buf[bytes_read], &sec_buf[off_in_sec], chunk);
        bytes_read += chunk;

        if (((offset + bytes_read) % fs_state.bytes_per_cluster) == 0) {
            cur_clus = fat32_get_next_cluster(cur_clus);
        }
    }

    return (int32_t)bytes_read;
}

int32_t fat32_write_path(const char *rel_path, uint32_t offset, uint32_t size,
                         const uint8_t *buf, bool_t truncate)
{
    struct fat32_loc loc;
    uint32_t cur_clus;
    uint32_t bytes_written = 0;
    uint8_t  sec_buf[512];

    if (fat32_resolve_rel(rel_path, &loc) < 0) {
        if (fat32_create_path(rel_path, FALSE) < 0 ||
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
        uint32_t skip_clusters = offset / fs_state.bytes_per_cluster;
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
        uint32_t clus_offset = cur_pos % fs_state.bytes_per_cluster;
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
            ((offset + bytes_written) % fs_state.bytes_per_cluster) == 0) {
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

int fat32_unlink_path(const char *rel_path)
{
    struct fat32_loc loc;
    uint8_t sec_buf[512];

    if (!fs_state.mounted || rel_path == NULL || rel_path[0] == '\0') {
        return -1;
    }
    if (fat32_resolve_rel(rel_path, &loc) < 0 ||
        (loc.attr & FAT_ATTR_DIRECTORY) != 0) {
        return -1;
    }
    if (loc.first_cluster >= 2U) {
        fat32_free_chain(loc.first_cluster);
    }
    if (fat32_read_sec(loc.dir_lba, sec_buf) != 512) {
        return -1;
    }
    sec_buf[loc.dir_offset] = 0xE5U;
    return (fat32_write_sec(loc.dir_lba, sec_buf) == 512) ? 0 : -1;
}

int fat32_truncate_path(const char *rel_path, uint32_t new_size)
{
    struct fat32_loc loc;
    uint8_t sec_buf[512];

    if (!fs_state.mounted || rel_path == NULL || rel_path[0] == '\0') {
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
        if (fat32_write_path(rel_path, new_size - 1U, 1U, &zero, FALSE) < 0) {
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

static int fat32_try_mount_lba(uint32_t lba)
{
    uint8_t bpb[512];
    uint16_t bytes_per_sec;
    uint8_t  sec_per_clus;
    uint16_t rsvd_sec_cnt;
    uint8_t  num_fats;
    uint32_t tot_sec_32;
    uint32_t fat_sz_32;
    uint32_t root_clus;
    uint32_t data_sec;

    if (xhci_msc_read_sector_idx(fs_state.disk_idx, lba, bpb) != 512) {
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

    fs_state.mounted             = TRUE;
    fs_state.part_lba            = lba;
    fs_state.fat_start_lba       = lba + rsvd_sec_cnt;
    fs_state.data_start_lba      = fs_state.fat_start_lba + ((uint32_t)num_fats * fat_sz_32);
    fs_state.sectors_per_cluster = sec_per_clus;
    fs_state.bytes_per_cluster   = (uint32_t)sec_per_clus * 512U;
    fs_state.fat_size_sectors    = fat_sz_32;
    fs_state.num_fats            = num_fats;
    fs_state.root_cluster        = root_clus;

    data_sec = tot_sec_32 - (rsvd_sec_cnt + ((uint32_t)num_fats * fat_sz_32));
    fs_state.total_clusters = data_sec / sec_per_clus;

    memcpy(fs_state.volume_label, &bpb[0x47], 11);
    fs_state.volume_label[11] = '\0';

    kprintf("[FAT32] Mounted volume \"%s\" at /mnt (part_lba=%u, clusters=%u, %u B/clus)\n",
            fs_state.volume_label, fs_state.part_lba,
            fs_state.total_clusters, fs_state.bytes_per_cluster);
    return 0;
}

int fat32_init(void)
{
    uint8_t mbr[512];
    int msc_count;
    int d;
    int i;

    memset(&fs_state, 0, sizeof(fs_state));
    fat_cache_lba   = 0xFFFFFFFFU;
    fat_cache_disk  = -1;
    data_cache_lba  = 0xFFFFFFFFU;
    data_cache_disk = -1;
    msc_count = xhci_get_msc_count();
    if (msc_count <= 0) {
        return -1;
    }

    for (d = 0; d < msc_count; d++) {
        fs_state.disk_idx = d;

        /* Check if LBA 0 is directly a FAT32 VBR */
        if (fat32_try_mount_lba(0) == 0) {
            return 0;
        }

        /* Otherwise scan MBR Partition Table (offsets 446..509) for a FAT32 partition */
        if (xhci_msc_read_sector_idx(d, 0, mbr) != 512) {
            continue;
        }
        if (mbr[510] == 0x55U && mbr[511] == 0xAAU) {
            for (i = 0; i < 4; i++) {
                const uint8_t *pte = &mbr[446 + i * 16];
                uint8_t part_type  = pte[4];
                uint32_t start_lba = read_le32(&pte[8]);

                if (start_lba > 0 && (part_type == 0x0BU || part_type == 0x0CU || part_type == 0x83U)) {
                    if (fat32_try_mount_lba(start_lba) == 0) {
                        return 0;
                    }
                }
            }
        }
    }

    return -1;
}

bool_t fat32_is_mounted(void)
{
    return fs_state.mounted;
}

void fat32_get_info(struct fat32_fs_info *out)
{
    if (out != NULL) {
        *out = fs_state;
    }
}
