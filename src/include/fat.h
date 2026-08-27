/*
 * GEMIBOOT - x86 Boot Loader for GEMIOS
 * Dedicated to the Public Domain (CC0)
 */

#ifndef GEMIBOOT_FAT_H
#define GEMIBOOT_FAT_H 1

#include "types.h"

#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       0x0F

#if defined(__GNUC__) || defined(__clang__)
#define PACKED __attribute__((packed))
#else
#define PACKED
#endif

#pragma pack(push, 1)

struct fat_bpb {
    uint8_t  jmp_boot[3];
    char     oem_name[8];
    uint16_t bytes_per_sec;
    uint8_t  sec_per_clus;
    uint16_t rsvd_sec_cnt;
    uint8_t  num_fats;
    uint16_t root_ent_cnt;
    uint16_t tot_sec_16;
    uint8_t  media;
    uint16_t fat_sz_16;
    uint16_t sec_per_trk;
    uint16_t num_heads;
    uint32_t hidd_sec;
    uint32_t tot_sec_32;

    union {
        struct {
            uint8_t  drv_num;
            uint8_t  reserved1;
            uint8_t  boot_sig;
            uint32_t vol_id;
            char     vol_lab[11];
            char     fil_sys_type[8];
            uint8_t  boot_code[448];
            uint16_t signature;
        } fat16;

        struct {
            uint32_t fat_sz_32;
            uint16_t ext_flags;
            uint16_t fs_ver;
            uint32_t root_clus;
            uint16_t fs_info;
            uint16_t bk_boot_sec;
            uint8_t  reserved[12];
            uint8_t  drv_num;
            uint8_t  reserved1;
            uint8_t  boot_sig;
            uint32_t vol_id;
            char     vol_lab[11];
            char     fil_sys_type[8];
            uint8_t  boot_code[420];
            uint16_t signature;
        } fat32;
    } spec;
} PACKED;

struct fat_dir_entry {
    char     name[8];
    char     ext[3];
    uint8_t  attr;
    uint8_t  lcase;
    uint8_t  ctime_tenth;
    uint16_t ctime;
    uint16_t cdate;
    uint16_t adate;
    uint16_t fst_clus_hi;
    uint16_t wtime;
    uint16_t wdate;
    uint16_t fst_clus_lo;
    uint32_t file_size;
} PACKED;

#pragma pack(pop)

typedef struct {
    uint32_t part_lba;
    uint16_t bytes_per_sec;
    uint8_t  sec_per_clus;
    uint32_t rsvd_sec_cnt;
    uint8_t  num_fats;
    uint32_t fat_size;
    uint32_t root_dir_sec;
    uint16_t root_ent_cnt;
    uint32_t data_sec;
    uint32_t total_sectors;
    uint32_t total_clusters;
    uint32_t root_clus;
    int      fat_type; /* 12, 16, or 32 */
} fat_fs_t;

typedef int (*disk_read_fn)(uint32_t lba, uint32_t count, void *buf);

int fat_init(fat_fs_t *fs, uint32_t part_lba, disk_read_fn read_fn);
int fat_find_file(fat_fs_t *fs, disk_read_fn read_fn, const char *filename, uint32_t *first_cluster, uint32_t *file_size);
int fat_read_file(fat_fs_t *fs, disk_read_fn read_fn, uint32_t first_cluster, uint32_t file_size, void *dest_buffer);

#endif /* GEMIBOOT_FAT_H */
