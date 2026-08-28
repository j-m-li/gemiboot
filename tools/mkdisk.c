/*
 * GEMIBOOT - x86 Boot Loader for GEMIOS
 * Dedicated to the Public Domain (CC0)
 *
 * mkdisk: Creates a bootable disk image for BIOS, UEFI 32-bit, and UEFI 64-bit.
 * Standard ANSI C90 compliant.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_MSC_VER)
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
#else
#include <stdint.h>
#endif

#define SECTOR_SIZE         512
#define DISK_SIZE_MB        256
#define TOTAL_SECTORS       ((uint32_t)DISK_SIZE_MB * 1024 * 1024 / SECTOR_SIZE)
#define PARTITION_START_LBA 2048
#define PARTITION_SECTORS   (TOTAL_SECTORS - PARTITION_START_LBA)

#define SEC_PER_CLUS        4
#define RSVD_SEC_CNT        32
#define NUM_FATS            2
#define ROOT_CLUS           2

#define ATTR_READ_ONLY      0x01
#define ATTR_HIDDEN         0x02
#define ATTR_SYSTEM         0x04
#define ATTR_VOLUME_ID      0x08
#define ATTR_DIRECTORY      0x10
#define ATTR_ARCHIVE        0x20

#pragma pack(push, 1)

struct mbr_entry {
    uint8_t  status;
    uint8_t  start_chs[3];
    uint8_t  type;
    uint8_t  end_chs[3];
    uint32_t start_lba;
    uint32_t length_lba;
};

struct fat32_bpb {
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
};

struct fat32_fsinfo {
    uint32_t lead_sig;       /* 0x41615252 */
    uint8_t  reserved1[480];
    uint32_t struc_sig;      /* 0x61417272 */
    uint32_t free_count;     /* 0xFFFFFFFF */
    uint32_t nxt_free;       /* 0xFFFFFFFF */
    uint8_t  reserved2[12];
    uint32_t trail_sig;      /* 0xAA550000 */
};

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
};

#pragma pack(pop)

static uint8_t *g_disk = NULL;
static uint32_t g_fat_start = 0;
static uint32_t g_fat_size = 0;
static uint32_t g_data_start = 0;
static uint32_t g_next_free_clus = 3;

static uint8_t *get_sector(uint32_t lba) {
    return g_disk + (lba * SECTOR_SIZE);
}

static void fat32_set_fat_entry(uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat1_sec = g_fat_start + (fat_offset / SECTOR_SIZE);
    uint32_t fat2_sec = g_fat_start + g_fat_size + (fat_offset / SECTOR_SIZE);
    uint32_t ent_offset = fat_offset % SECTOR_SIZE;

    *(uint32_t*)(get_sector(fat1_sec) + ent_offset) = (value & 0x0FFFFFFF);
    *(uint32_t*)(get_sector(fat2_sec) + ent_offset) = (value & 0x0FFFFFFF);
}

static uint32_t cluster_to_lba(uint32_t cluster) {
    return g_data_start + ((cluster - 2) * SEC_PER_CLUS);
}

static void format_83(const char *name, char *out_name, char *out_ext) {
    int i = 0, ni = 0, ei = 0;
    int in_ext = 0;

    memset(out_name, ' ', 8);
    memset(out_ext, ' ', 3);

    if (strcmp(name, ".") == 0) {
        out_name[0] = '.';
        return;
    }
    if (strcmp(name, "..") == 0) {
        out_name[0] = '.';
        out_name[1] = '.';
        return;
    }

    for (i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        if (c == '.') {
            in_ext = 1;
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 32);
        }
        if (!in_ext) {
            if (ni < 8) {
                out_name[ni++] = c;
            }
        } else {
            if (ei < 3) {
                out_ext[ei++] = c;
            }
        }
    }
}

static uint32_t allocate_cluster(void) {
    uint32_t clus = g_next_free_clus++;
    fat32_set_fat_entry(clus, 0x0FFFFFFF);
    memset(get_sector(cluster_to_lba(clus)), 0, SEC_PER_CLUS * SECTOR_SIZE);
    return clus;
}

static int add_dir_entry(uint32_t dir_clus, const char *name, uint8_t attr, uint32_t first_clus, uint32_t size) {
    uint32_t cur_clus = dir_clus;
    while (1) {
        uint32_t clus_lba = cluster_to_lba(cur_clus);
        uint32_t s, e;

        for (s = 0; s < SEC_PER_CLUS; s++) {
            struct fat_dir_entry *entries = (struct fat_dir_entry*)get_sector(clus_lba + s);
            for (e = 0; e < SECTOR_SIZE / sizeof(struct fat_dir_entry); e++) {
                if ((uint8_t)entries[e].name[0] == 0x00 || (uint8_t)entries[e].name[0] == 0xE5) {
                    char o_name[8], o_ext[3];
                    format_83(name, o_name, o_ext);
                    memcpy(entries[e].name, o_name, 8);
                    memcpy(entries[e].ext, o_ext, 3);
                    entries[e].attr = attr;
                    entries[e].fst_clus_hi = (uint16_t)(first_clus >> 16);
                    entries[e].fst_clus_lo = (uint16_t)(first_clus & 0xFFFF);
                    entries[e].file_size = size;
                    entries[e].wdate = 0x5821; /* 2024 */
                    entries[e].wtime = 0x6000;
                    return 0;
                }
            }
        }
        break;
    }
    return -1;
}

static uint32_t create_directory(uint32_t parent_clus, const char *name) {
    uint32_t clus = allocate_cluster();
    add_dir_entry(parent_clus, name, ATTR_DIRECTORY, clus, 0);

    /* Add . and .. */
    add_dir_entry(clus, ".", ATTR_DIRECTORY, clus, 0);
    add_dir_entry(clus, "..", ATTR_DIRECTORY, parent_clus == ROOT_CLUS ? 0 : parent_clus, 0);
    return clus;
}

static uint32_t write_file_data(uint32_t parent_clus, const char *name, const void *data, uint32_t size) {
    uint32_t num_clusters = (size + (SEC_PER_CLUS * SECTOR_SIZE) - 1) / (SEC_PER_CLUS * SECTOR_SIZE);
    uint32_t first_clus = 0;
    uint32_t prev_clus = 0;
    uint32_t i;
    const uint8_t *src = (const uint8_t*)data;
    uint32_t remaining = size;

    if (num_clusters == 0) num_clusters = 1;

    for (i = 0; i < num_clusters; i++) {
        uint32_t clus = allocate_cluster();
        uint32_t copy_bytes = remaining > (SEC_PER_CLUS * SECTOR_SIZE) ? (SEC_PER_CLUS * SECTOR_SIZE) : remaining;

        if (i == 0) {
            first_clus = clus;
        } else {
            fat32_set_fat_entry(prev_clus, clus);
        }

        if (copy_bytes > 0 && src) {
            memcpy(get_sector(cluster_to_lba(clus)), src, copy_bytes);
            src += copy_bytes;
            remaining -= copy_bytes;
        }

        prev_clus = clus;
    }

    fat32_set_fat_entry(prev_clus, 0x0FFFFFFF);
    add_dir_entry(parent_clus, name, ATTR_ARCHIVE, first_clus, size);
    return first_clus;
}

static uint8_t *read_host_file(const char *path, uint32_t *out_size) {
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long sz;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    buf = (uint8_t*)malloc(sz > 0 ? sz : 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (sz > 0) {
        if (fread(buf, 1, sz, f) != (size_t)sz) {
            free(buf);
            fclose(f);
            return NULL;
        }
    }
    fclose(f);
    if (out_size) *out_size = (uint32_t)sz;
    return buf;
}

int main(int argc, char *argv[]) {
    const char *out_path;
    const char *mbr_path;
    const char *vbr_path;
    const char *io_sys_path;
    const char *ia32_path;
    const char *x64_path;
    const char *kernel_path;

    uint8_t *mbr_data = NULL, *vbr_data = NULL, *io_sys_data = NULL;
    uint8_t *ia32_data = NULL, *x64_data = NULL, *kernel_data = NULL;
    uint32_t mbr_sz = 0, vbr_sz = 0, io_sys_sz = 0;
    uint32_t ia32_sz = 0, x64_sz = 0, kernel_sz = 0;

    struct mbr_entry *mbr_part;
    struct fat32_bpb *bpb;
    struct fat32_fsinfo *fsinfo;
    uint32_t data_sectors;
    uint32_t efi_clus, boot_clus;
    FILE *out_f;

    if (argc < 8) {
        printf("Usage: %s <output.img> <mbr.bin> <vbr.bin> <io.sys> <bootia32.efi> <bootx64.efi> <gemios.elf>\n", argv[0]);
        return 1;
    }

    out_path    = argv[1];
    mbr_path    = argv[2];
    vbr_path    = argv[3];
    io_sys_path = argv[4];
    ia32_path   = argv[5];
    x64_path    = argv[6];
    kernel_path = argv[7];

    printf("[MKDISK] Creating %u MB disk image: %s\n", DISK_SIZE_MB, out_path);

    /* Allocate disk buffer */
    g_disk = (uint8_t*)calloc(TOTAL_SECTORS, SECTOR_SIZE);
    if (!g_disk) {
        fprintf(stderr, "[MKDISK] ERROR: Out of memory\n");
        return 1;
    }

    /* Read input files */
    mbr_data = read_host_file(mbr_path, &mbr_sz);
    vbr_data = read_host_file(vbr_path, &vbr_sz);
    io_sys_data = read_host_file(io_sys_path, &io_sys_sz);
    ia32_data = read_host_file(ia32_path, &ia32_sz);
    x64_data = read_host_file(x64_path, &x64_sz);
    kernel_data = read_host_file(kernel_path, &kernel_sz);

    if (!mbr_data || !vbr_data || !io_sys_data || !ia32_data || !x64_data || !kernel_data) {
        fprintf(stderr, "[MKDISK] ERROR: Failed to read input files\n");
        return 1;
    }

    /* 1. Write MBR code to sector 0 */
    memcpy(g_disk, mbr_data, mbr_sz < 440 ? mbr_sz : 440);
    *(uint32_t*)(g_disk + 440) = 0x544F4F42; /* Windows NT Disk Signature: "BOOT" */
    *(uint16_t*)(g_disk + 444) = 0x0000;
    *(uint16_t*)(g_disk + 510) = 0xAA55;

    /* Write MBR partition table */
    mbr_part = (struct mbr_entry*)(g_disk + 446);
    mbr_part[0].status = 0x80; /* Active bootable */

    /* CHS Start: Head 32, Sector 33, Cylinder 0 (LBA 2048) */
    mbr_part[0].start_chs[0] = 32;
    mbr_part[0].start_chs[1] = 33;
    mbr_part[0].start_chs[2] = 0;

    mbr_part[0].type = 0x0C;   /* FAT32 with LBA */

    /* CHS End: Head 254, Sector 63, Cylinder 1023 */
    mbr_part[0].end_chs[0] = 254;
    mbr_part[0].end_chs[1] = 0xFF;
    mbr_part[0].end_chs[2] = 0xFF;

    mbr_part[0].start_lba = PARTITION_START_LBA;
    mbr_part[0].length_lba = PARTITION_SECTORS;

    /* 2. Format FAT32 partition at PARTITION_START_LBA */
    g_fat_start = PARTITION_START_LBA + RSVD_SEC_CNT;

    data_sectors = PARTITION_SECTORS - RSVD_SEC_CNT;
    /* Calculate FAT size in sectors */
    g_fat_size = ((data_sectors / SEC_PER_CLUS * 4) + SECTOR_SIZE - 1) / SECTOR_SIZE;
    g_data_start = g_fat_start + (NUM_FATS * g_fat_size);

    /* Write VBR to PARTITION_START_LBA */
    memcpy(get_sector(PARTITION_START_LBA), vbr_data, vbr_sz < SECTOR_SIZE ? vbr_sz : SECTOR_SIZE);

    bpb = (struct fat32_bpb*)get_sector(PARTITION_START_LBA);
    memcpy(bpb->oem_name, "GEMIBOOT", 8);
    bpb->bytes_per_sec = SECTOR_SIZE;
    bpb->sec_per_clus = SEC_PER_CLUS;
    bpb->rsvd_sec_cnt = RSVD_SEC_CNT;
    bpb->num_fats = NUM_FATS;
    bpb->root_ent_cnt = 0;
    bpb->tot_sec_16 = 0;
    bpb->media = 0xF8;
    bpb->fat_sz_16 = 0;
    bpb->sec_per_trk = 63;
    bpb->num_heads = 255;
    bpb->hidd_sec = PARTITION_START_LBA;
    bpb->tot_sec_32 = PARTITION_SECTORS;
    bpb->fat_sz_32 = g_fat_size;
    bpb->root_clus = ROOT_CLUS;
    bpb->fs_info = 1;
    bpb->bk_boot_sec = 6;
    bpb->drv_num = 0x80;
    bpb->boot_sig = 0x29;
    bpb->vol_id = 0x12345678;
    memcpy(bpb->vol_lab, "GEMIBOOT   ", 11);
    memcpy(bpb->fil_sys_type, "FAT32   ", 8);
    bpb->signature = 0xAA55;

    /* Write FSInfo at sector 1 */
    fsinfo = (struct fat32_fsinfo*)get_sector(PARTITION_START_LBA + 1);
    fsinfo->lead_sig = 0x41615252;
    fsinfo->struc_sig = 0x61417272;
    fsinfo->free_count = 0xFFFFFFFF;
    fsinfo->nxt_free = 0xFFFFFFFF;
    fsinfo->trail_sig = 0xAA550000;

    /* Backup boot sector at sector 6 and FSInfo at sector 7 */
    memcpy(get_sector(PARTITION_START_LBA + 6), get_sector(PARTITION_START_LBA), SECTOR_SIZE);
    memcpy(get_sector(PARTITION_START_LBA + 7), get_sector(PARTITION_START_LBA + 1), SECTOR_SIZE);

    /* Also copy IO.SYS right after VBR in the reserved sectors (LBA 2049 onwards) */
    {
        uint32_t io_sec_count = (io_sys_sz + SECTOR_SIZE - 1) / SECTOR_SIZE;
        if (2 + io_sec_count < RSVD_SEC_CNT) {
            memcpy(get_sector(PARTITION_START_LBA + 2), io_sys_data, io_sys_sz);
        }
    }

    /* Initialize FAT entries */
    fat32_set_fat_entry(0, 0x0FFFFFF8); /* Media descriptor */
    fat32_set_fat_entry(1, 0x0FFFFFFF); /* End of cluster mark */
    fat32_set_fat_entry(ROOT_CLUS, 0x0FFFFFFF); /* Root dir */

    /* 3. Populate FAT32 filesystem */
    /* Add IO.SYS to root directory */
    write_file_data(ROOT_CLUS, "IO.SYS", io_sys_data, io_sys_sz);

    /* Add GEMIOS.ELF to root directory */
    write_file_data(ROOT_CLUS, "GEMIOS.ELF", kernel_data, kernel_sz);

    /* Create /EFI and /EFI/BOOT directories */
    efi_clus = create_directory(ROOT_CLUS, "EFI");
    boot_clus = create_directory(efi_clus, "BOOT");

    /* Copy EFI boot loaders */
    write_file_data(boot_clus, "BOOTIA32.EFI", ia32_data, ia32_sz);
    write_file_data(boot_clus, "BOOTX64.EFI", x64_data, x64_sz);

    printf("[MKDISK] Written files:\n");
    printf("  - MBR (Sector 0)\n");
    printf("  - VBR (LBA %u)\n", PARTITION_START_LBA);
    printf("  - /IO.SYS (%u bytes)\n", io_sys_sz);
    printf("  - /GEMIOS.ELF (%u bytes)\n", kernel_sz);
    printf("  - /EFI/BOOT/BOOTIA32.EFI (%u bytes)\n", ia32_sz);
    printf("  - /EFI/BOOT/BOOTX64.EFI (%u bytes)\n", x64_sz);

    /* Write disk image */
    out_f = fopen(out_path, "wb");
    if (!out_f) {
        fprintf(stderr, "[MKDISK] ERROR: Cannot open %s for writing\n", out_path);
        return 1;
    }

    if (fwrite(g_disk, SECTOR_SIZE, TOTAL_SECTORS, out_f) != TOTAL_SECTORS) {
        fprintf(stderr, "[MKDISK] ERROR: Failed to write full disk image\n");
        fclose(out_f);
        return 1;
    }

    fclose(out_f);
    printf("[MKDISK] Successfully created %s\n", out_path);

    free(g_disk);
    if (mbr_data) free(mbr_data);
    if (vbr_data) free(vbr_data);
    if (io_sys_data) free(io_sys_data);
    if (ia32_data) free(ia32_data);
    if (x64_data) free(x64_data);
    if (kernel_data) free(kernel_data);

    return 0;
}
