/*
 * GEMIBOOT - x86 Boot Loader for GEMIOS
 * Dedicated to the Public Domain (CC0)
 */

#include "fat.h"
#include "types.h"

#define SECTOR_SIZE 512

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - ('a' - 'A'));
    }
    return c;
}

static void format_83_name(const char *input, char *output_83) {
    int i;
    int name_idx = 0;
    int ext_idx = 0;
    bool in_ext = false;

    memset(output_83, ' ', 11);

    for (i = 0; input[i] != '\0' && i < 64; i++) {
        char c = input[i];
        if (c == '.') {
            in_ext = true;
            continue;
        }
        if (!in_ext) {
            if (name_idx < 8) {
                output_83[name_idx++] = to_upper(c);
            }
        } else {
            if (ext_idx < 3) {
                output_83[8 + ext_idx++] = to_upper(c);
            }
        }
    }
}

int fat_init(fat_fs_t *fs, uint32_t part_lba, disk_read_fn read_fn) {
    uint8_t sector_buf[SECTOR_SIZE];
    struct fat_bpb *bpb;
    uint32_t root_dir_sectors;
    uint32_t data_sectors;

    if (!fs || !read_fn) {
        return -1;
    }

    memset(fs, 0, sizeof(fat_fs_t));
    fs->part_lba = part_lba;

    /* Read BPB / VBR sector */
    if (read_fn(part_lba, 1, sector_buf) != 0) {
        return -1;
    }

    bpb = (struct fat_bpb*)sector_buf;

    fs->bytes_per_sec = bpb->bytes_per_sec;
    if (fs->bytes_per_sec != SECTOR_SIZE) {
        return -1;
    }

    fs->sec_per_clus = bpb->sec_per_clus;
    fs->rsvd_sec_cnt = bpb->rsvd_sec_cnt;
    fs->num_fats = bpb->num_fats;
    fs->root_ent_cnt = bpb->root_ent_cnt;

    fs->total_sectors = bpb->tot_sec_16 ? bpb->tot_sec_16 : bpb->tot_sec_32;

    if (bpb->fat_sz_16 != 0) {
        fs->fat_size = bpb->fat_sz_16;
    } else {
        fs->fat_size = bpb->spec.fat32.fat_sz_32;
    }

    root_dir_sectors = ((fs->root_ent_cnt * 32) + (fs->bytes_per_sec - 1)) / fs->bytes_per_sec;
    fs->root_dir_sec = fs->part_lba + fs->rsvd_sec_cnt + (fs->num_fats * fs->fat_size);
    fs->data_sec = fs->root_dir_sec + root_dir_sectors;

    data_sectors = fs->total_sectors - (fs->rsvd_sec_cnt + (fs->num_fats * fs->fat_size) + root_dir_sectors);
    fs->total_clusters = data_sectors / fs->sec_per_clus;

    if (fs->total_clusters < 4085) {
        fs->fat_type = 12;
    } else if (fs->total_clusters < 65525) {
        fs->fat_type = 16;
    } else {
        fs->fat_type = 32;
        fs->root_clus = bpb->spec.fat32.root_clus;
    }

    return 0;
}

static uint32_t cluster_to_lba(const fat_fs_t *fs, uint32_t cluster) {
    return fs->data_sec + ((cluster - 2) * fs->sec_per_clus);
}

static uint32_t fat_get_next_cluster(const fat_fs_t *fs, disk_read_fn read_fn, uint32_t cluster) {
    uint8_t sector_buf[SECTOR_SIZE];
    uint32_t fat_offset;
    uint32_t fat_sector;
    uint32_t ent_offset;

    if (fs->fat_type == 32) {
        fat_offset = cluster * 4;
        fat_sector = fs->part_lba + fs->rsvd_sec_cnt + (fat_offset / fs->bytes_per_sec);
        ent_offset = fat_offset % fs->bytes_per_sec;

        if (read_fn(fat_sector, 1, sector_buf) != 0) {
            return 0x0FFFFFFF;
        }
        return (*(uint32_t*)&sector_buf[ent_offset]) & 0x0FFFFFFF;
    } else if (fs->fat_type == 16) {
        fat_offset = cluster * 2;
        fat_sector = fs->part_lba + fs->rsvd_sec_cnt + (fat_offset / fs->bytes_per_sec);
        ent_offset = fat_offset % fs->bytes_per_sec;

        if (read_fn(fat_sector, 1, sector_buf) != 0) {
            return 0xFFFF;
        }
        return *(uint16_t*)&sector_buf[ent_offset];
    }

    return 0x0FFFFFFF;
}

static bool is_end_of_cluster_chain(const fat_fs_t *fs, uint32_t cluster) {
    if (fs->fat_type == 32) {
        return cluster >= 0x0FFFFFF8;
    } else if (fs->fat_type == 16) {
        return cluster >= 0xFFF8;
    }
    return cluster >= 0xFF8;
}

int fat_find_file(fat_fs_t *fs, disk_read_fn read_fn, const char *filename, uint32_t *first_cluster, uint32_t *file_size) {
    char target_83[11];
    uint8_t sector_buf[SECTOR_SIZE];

    format_83_name(filename, target_83);

    if (fs->fat_type == 32) {
        uint32_t current_clus = fs->root_clus;
        while (!is_end_of_cluster_chain(fs, current_clus) && current_clus >= 2) {
            uint32_t clus_lba = cluster_to_lba(fs, current_clus);
            uint8_t s;
            for (s = 0; s < fs->sec_per_clus; s++) {
                struct fat_dir_entry *entry;
                size_t e;

                if (read_fn(clus_lba + s, 1, sector_buf) != 0) {
                    return -1;
                }

                entry = (struct fat_dir_entry*)sector_buf;
                for (e = 0; e < SECTOR_SIZE / 32; e++, entry++) {
                    if ((uint8_t)entry->name[0] == 0x00) {
                        return -1; /* End of directory */
                    }
                    if ((uint8_t)entry->name[0] == 0xE5 || (entry->attr & FAT_ATTR_VOLUME_ID)) {
                        continue;
                    }
                    if (memcmp(entry->name, target_83, 11) == 0) {
                        if (first_cluster) {
                            *first_cluster = ((uint32_t)entry->fst_clus_hi << 16) | entry->fst_clus_lo;
                        }
                        if (file_size) {
                            *file_size = entry->file_size;
                        }
                        return 0;
                    }
                }
            }
            current_clus = fat_get_next_cluster(fs, read_fn, current_clus);
        }
    } else {
        /* FAT12 / FAT16 root directory */
        uint32_t root_sectors = ((fs->root_ent_cnt * 32) + (fs->bytes_per_sec - 1)) / fs->bytes_per_sec;
        uint32_t s;
        for (s = 0; s < root_sectors; s++) {
            struct fat_dir_entry *entry;
            size_t e;

            if (read_fn(fs->root_dir_sec + s, 1, sector_buf) != 0) {
                return -1;
            }

            entry = (struct fat_dir_entry*)sector_buf;
            for (e = 0; e < SECTOR_SIZE / 32; e++, entry++) {
                if ((uint8_t)entry->name[0] == 0x00) {
                    return -1;
                }
                if ((uint8_t)entry->name[0] == 0xE5 || (entry->attr & FAT_ATTR_VOLUME_ID)) {
                    continue;
                }
                if (memcmp(entry->name, target_83, 11) == 0) {
                    if (first_cluster) {
                        *first_cluster = ((uint32_t)entry->fst_clus_hi << 16) | entry->fst_clus_lo;
                    }
                    if (file_size) {
                        *file_size = entry->file_size;
                    }
                    return 0;
                }
            }
        }
    }

    return -1;
}

int fat_read_file(fat_fs_t *fs, disk_read_fn read_fn, uint32_t first_cluster, uint32_t file_size, void *dest_buffer) {
    uint32_t current_clus = first_cluster;
    uint32_t bytes_remaining = file_size;
    uint8_t *dest = (uint8_t*)dest_buffer;
    uint8_t sector_buf[SECTOR_SIZE];

    while (bytes_remaining > 0 && !is_end_of_cluster_chain(fs, current_clus) && current_clus >= 2) {
        uint32_t clus_lba = cluster_to_lba(fs, current_clus);
        uint8_t s;

        for (s = 0; s < fs->sec_per_clus && bytes_remaining > 0; s++) {
            uint32_t copy_len = bytes_remaining > SECTOR_SIZE ? SECTOR_SIZE : bytes_remaining;

            if (copy_len == SECTOR_SIZE) {
                if (read_fn(clus_lba + s, 1, dest) != 0) {
                    return -1;
                }
            } else {
                if (read_fn(clus_lba + s, 1, sector_buf) != 0) {
                    return -1;
                }
                memcpy(dest, sector_buf, copy_len);
            }

            dest += copy_len;
            bytes_remaining -= copy_len;
        }

        current_clus = fat_get_next_cluster(fs, read_fn, current_clus);
    }

    return (bytes_remaining == 0) ? 0 : -1;
}
