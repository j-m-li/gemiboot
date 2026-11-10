/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - FAT32 Filesystem Driver
 */

#ifndef OKSINIX_FAT32_H
#define OKSINIX_FAT32_H

#include "types.h"
#include "vfs.h"

struct fat32_fs_info {
    bool_t   mounted;
    int      disk_idx;
    uint32_t part_lba;
    uint32_t fat_start_lba;
    uint32_t data_start_lba;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_cluster;
    uint32_t fat_size_sectors;
    uint32_t num_fats;
    uint32_t root_cluster;
    uint32_t total_clusters;
    char     volume_label[12];
};

int    fat32_init(void);
bool_t fat32_is_mounted(void);
void   fat32_get_info(struct fat32_fs_info *out);

/* VFS bridge operations for paths under /mnt */
int     fat32_stat_path(const char *rel_path, struct vfs_dirent *out);
int     fat32_readdir_path(const char *rel_dir_path, uint32_t index, struct vfs_dirent *out);
int32_t fat32_read_path(const char *rel_path, uint32_t offset, uint32_t size, uint8_t *buf);
int32_t fat32_write_path(const char *rel_path, uint32_t offset, uint32_t size,
                         const uint8_t *buf, bool_t truncate);
int     fat32_create_path(const char *rel_path, bool_t is_dir);
int     fat32_unlink_path(const char *rel_path);
int     fat32_truncate_path(const char *rel_path, uint32_t new_size);

#endif /* OKSINIX_FAT32_H */
