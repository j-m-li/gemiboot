/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Multi-Volume FAT32 Filesystem Driver
 */

#ifndef OKSINIX_FAT32_H
#define OKSINIX_FAT32_H

#include "types.h"
#include "vfs.h"

/* Mount slots: 0=/mnt (boot mass storage), 1=/media1, 2=/media2, 3=/media3 */
#define FAT32_MAX_MOUNTS 4

struct fat32_fs_info {
    bool_t   mounted;
    int      disk_idx;
    char     mount_point[16];
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

int         fat32_init(void);
void        fat32_sync_mounts(void);
void        fat32_remount_all(void);
int         fat32_mount_disk(int disk_idx, int mount_idx);
int         fat32_umount_idx(int mount_idx);
int         fat32_umount_path(const char *mount_point);
bool_t      fat32_is_mounted(void);
bool_t      fat32_is_mounted_idx(int mount_idx);
void        fat32_get_info(struct fat32_fs_info *out);
int         fat32_get_info_idx(int mount_idx, struct fat32_fs_info *out);
const char *fat32_get_mount_point(int mount_idx);
int         fat32_match_mount(const char *abs_path, const char **out_rel);

/* VFS bridge operations indexed by mount slot (0..FAT32_MAX_MOUNTS-1) */
int     fat32_stat_path_idx(int mount_idx, const char *rel_path, struct vfs_dirent *out);
int     fat32_readdir_path_idx(int mount_idx, const char *rel_dir_path, uint32_t index,
                               struct vfs_dirent *out);
int32_t fat32_read_path_idx(int mount_idx, const char *rel_path, uint32_t offset,
                            uint32_t size, uint8_t *buf);
int32_t fat32_write_path_idx(int mount_idx, const char *rel_path, uint32_t offset,
                             uint32_t size, const uint8_t *buf, bool_t truncate);
int     fat32_create_path_idx(int mount_idx, const char *rel_path, bool_t is_dir);
int     fat32_unlink_path_idx(int mount_idx, const char *rel_path);
int     fat32_truncate_path_idx(int mount_idx, const char *rel_path, uint32_t new_size);

#endif /* OKSINIX_FAT32_H */
