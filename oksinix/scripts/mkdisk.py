#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# Public Domain (CC0 1.0 Universal)
# Oksinix - Build bootable raw x86 disk image with MBR, kernel, and FAT32 partition

import struct
import sys


def make_dirent_83(name83, attr, cluster, size):
    assert len(name83) == 11
    entry = bytearray(32)
    entry[0:11] = name83
    entry[11] = attr
    struct.pack_into("<H", entry, 20, (cluster >> 16) & 0xFFFF)
    struct.pack_into("<H", entry, 26, cluster & 0xFFFF)
    struct.pack_into("<I", entry, 28, size)
    return entry


def build_fat32_partition(part_sectors, linux_elf_bytes=b"", dyn_elf_bytes=b"", shlib_so_bytes=b""):
    part = bytearray(part_sectors * 512)

    bytes_per_sec = 512
    sec_per_clus = 1
    rsvd_sec_cnt = 32
    num_fats = 2
    fat_sz_32 = 16
    root_clus = 2
    fs_info_sec = 1

    # 1. FAT32 Volume Boot Record (VBR) at partition sector 0
    vbr = bytearray(512)
    vbr[0:3] = b"\xeb\x58\x90"
    vbr[3:11] = b"OKSINIX "
    struct.pack_into("<H", vbr, 0x0B, bytes_per_sec)
    vbr[0x0D] = sec_per_clus
    struct.pack_into("<H", vbr, 0x0E, rsvd_sec_cnt)
    vbr[0x10] = num_fats
    struct.pack_into("<H", vbr, 0x11, 0)       # root_ent_cnt = 0 for FAT32
    struct.pack_into("<H", vbr, 0x13, 0)       # tot_sec_16 = 0
    vbr[0x15] = 0xF8                           # media = fixed disk
    struct.pack_into("<H", vbr, 0x16, 0)       # fat_sz_16 = 0
    struct.pack_into("<H", vbr, 0x18, 63)      # sec_per_trk
    struct.pack_into("<H", vbr, 0x1A, 16)      # num_heads
    struct.pack_into("<I", vbr, 0x1C, 256)     # hidd_sec (partition LBA 256)
    struct.pack_into("<I", vbr, 0x20, part_sectors)
    struct.pack_into("<I", vbr, 0x24, fat_sz_32)
    struct.pack_into("<H", vbr, 0x28, 0)       # ext_flags
    struct.pack_into("<H", vbr, 0x2A, 0)       # fs_ver
    struct.pack_into("<I", vbr, 0x2C, root_clus)
    struct.pack_into("<H", vbr, 0x30, fs_info_sec)
    struct.pack_into("<H", vbr, 0x32, 6)       # bk_boot_sec
    vbr[0x40] = 0x80                           # drv_num
    vbr[0x42] = 0x29                           # boot_sig
    struct.pack_into("<I", vbr, 0x43, 0x20261031)
    vbr[0x47:0x52] = b"OKSINIXFAT "            # 11-byte volume label
    vbr[0x52:0x5A] = b"FAT32   "               # 8-byte fs_type signature
    vbr[510] = 0x55
    vbr[511] = 0xAA
    part[0:512] = vbr

    # 2. FSInfo sector at partition sector 1
    fsinfo = bytearray(512)
    struct.pack_into("<I", fsinfo, 0, 0x41615252)
    struct.pack_into("<I", fsinfo, 484, 0x61417272)
    struct.pack_into("<I", fsinfo, 488, 0xFFFFFFFF)
    struct.pack_into("<I", fsinfo, 492, 7)
    struct.pack_into("<I", fsinfo, 508, 0xAA550000)
    part[512:1024] = fsinfo

    # 3. FAT1 and FAT2 tables
    fat = bytearray(fat_sz_32 * 512)
    fat_entries = [
        0x0FFFFFF8,  # Cluster 0: media descriptor
        0x0FFFFFFF,  # Cluster 1: EOC
        0x0FFFFFFF,  # Cluster 2: Root directory
        0x0FFFFFFF,  # Cluster 3: README.TXT
        0x0FFFFFFF,  # Cluster 4: HELLO.TXT
        0x0FFFFFFF,  # Cluster 5: DOCS directory
        0x0FFFFFFF,  # Cluster 6: DOCS/NOTES.TXT
    ]
    def alloc_chain(blob):
        if not blob:
            return 0
        start_c = len(fat_entries)
        n_clus = (len(blob) + 511) // 512
        for i in range(n_clus):
            c = start_c + i
            nxt = (c + 1) if (i + 1 < n_clus) else 0x0FFFFFFF
            fat_entries.append(nxt)
        return start_c

    elf_start_clus = alloc_chain(linux_elf_bytes)
    dyn_start_clus = alloc_chain(dyn_elf_bytes)
    shlib_start_clus = alloc_chain(shlib_so_bytes)

    for idx, val in enumerate(fat_entries):
        struct.pack_into("<I", fat, idx * 4, val)

    fat1_off = rsvd_sec_cnt * 512
    fat2_off = (rsvd_sec_cnt + fat_sz_32) * 512
    part[fat1_off:fat1_off + len(fat)] = fat
    part[fat2_off:fat2_off + len(fat)] = fat

    # 4. Data Region (starts at sector rsvd_sec_cnt + 2 * fat_sz_32 = 64)
    data_sec_start = rsvd_sec_cnt + num_fats * fat_sz_32

    def clus_off(c):
        return (data_sec_start + (c - 2) * sec_per_clus) * 512

    readme_bytes = b"Oksinix FAT32 filesystem on USB Mass Storage (/dev/sda1 -> /mnt)\n"
    hello_bytes = b"Hello from FAT32 on Oksinix!\n"
    notes_bytes = b"Subdirectory file inside FAT32 /mnt/DOCS/NOTES.TXT\n"

    # Populate Cluster 2 (Root Directory)
    root_dir = bytearray(512)
    root_dir[0:32] = make_dirent_83(b"OKSINIXFAT ", 0x08, 0, 0)
    root_dir[32:64] = make_dirent_83(b"README  TXT", 0x20, 3, len(readme_bytes))
    root_dir[64:96] = make_dirent_83(b"HELLO   TXT", 0x20, 4, len(hello_bytes))
    root_dir[96:128] = make_dirent_83(b"DOCS       ", 0x10, 5, 0)
    dir_pos = 128
    if linux_elf_bytes:
        root_dir[dir_pos:dir_pos + 32] = make_dirent_83(b"LINUX   ELF", 0x20, elf_start_clus, len(linux_elf_bytes))
        dir_pos += 32
    if dyn_elf_bytes:
        root_dir[dir_pos:dir_pos + 32] = make_dirent_83(b"DYN     ELF", 0x20, dyn_start_clus, len(dyn_elf_bytes))
        dir_pos += 32
    if shlib_so_bytes:
        root_dir[dir_pos:dir_pos + 32] = make_dirent_83(b"LIBTEST SO ", 0x20, shlib_start_clus, len(shlib_so_bytes))
        dir_pos += 32
    part[clus_off(2):clus_off(2) + 512] = root_dir

    # Populate Cluster 3 (README.TXT)
    part[clus_off(3):clus_off(3) + len(readme_bytes)] = readme_bytes

    # Populate Cluster 4 (HELLO.TXT)
    part[clus_off(4):clus_off(4) + len(hello_bytes)] = hello_bytes

    # Populate Cluster 5 (DOCS Directory)
    docs_dir = bytearray(512)
    docs_dir[0:32] = make_dirent_83(b".          ", 0x10, 5, 0)
    docs_dir[32:64] = make_dirent_83(b"..         ", 0x10, 0, 0)
    docs_dir[64:96] = make_dirent_83(b"NOTES   TXT", 0x20, 6, len(notes_bytes))
    part[clus_off(5):clus_off(5) + 512] = docs_dir

    # Populate Cluster 6 (DOCS/NOTES.TXT)
    part[clus_off(6):clus_off(6) + len(notes_bytes)] = notes_bytes

    # Populate Clusters for LINUX.ELF, DYN.ELF, and LIBTEST.SO
    if linux_elf_bytes:
        part[clus_off(elf_start_clus):clus_off(elf_start_clus) + len(linux_elf_bytes)] = linux_elf_bytes
    if dyn_elf_bytes:
        part[clus_off(dyn_start_clus):clus_off(dyn_start_clus) + len(dyn_elf_bytes)] = dyn_elf_bytes
    if shlib_so_bytes:
        part[clus_off(shlib_start_clus):clus_off(shlib_start_clus) + len(shlib_so_bytes)] = shlib_so_bytes

    return part


def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <mbr.bin> <kernel.bin> <out_disk.img>", file=sys.stderr)
        sys.exit(1)

    mbr_path = sys.argv[1]
    kernel_bin_path = sys.argv[2]
    out_path = sys.argv[3]

    total_sectors = 2048  # 1 MiB (2048 * 512B sectors)
    part_start_lba = 256
    part_sectors = total_sectors - part_start_lba  # 1792 sectors
    total_size = total_sectors * 512

    with open(mbr_path, "rb") as f:
        mbr_data = f.read()

    if len(mbr_data) != 512:
        raise RuntimeError(f"Expected 512-byte MBR image, got {len(mbr_data)} bytes")
    if mbr_data[510:512] != b"\x55\xaa":
        raise RuntimeError("Missing 0x55AA MBR boot signature in mbr.bin")

    with open(kernel_bin_path, "rb") as f:
        kernel_data = f.read()

    max_kernel_size = 240 * 512  # 120 KiB loaded by MBR (LBA 1..240)
    if len(kernel_data) > max_kernel_size:
        raise RuntimeError(
            f"Kernel binary ({len(kernel_data)} B) exceeds MBR loader limit ({max_kernel_size} B)"
        )

    linux_elf_bytes = b""
    dyn_elf_bytes = b""
    shlib_so_bytes = b""
    if len(sys.argv) >= 5:
        with open(sys.argv[4], "rb") as f:
            linux_elf_bytes = f.read()
    if len(sys.argv) >= 6:
        with open(sys.argv[5], "rb") as f:
            dyn_elf_bytes = f.read()
    if len(sys.argv) >= 7:
        with open(sys.argv[6], "rb") as f:
            shlib_so_bytes = f.read()

    fat32_part = build_fat32_partition(part_sectors, linux_elf_bytes, dyn_elf_bytes, shlib_so_bytes)

    image = bytearray(total_size)
    image[0:512] = mbr_data
    image[512:512 + len(kernel_data)] = kernel_data
    image[part_start_lba * 512:] = fat32_part

    with open(out_path, "wb") as f:
        f.write(image)

    print(
        f"Created bootable + FAT32 disk image: {out_path} "
        f"({total_size} bytes, MBR=512B, kernel={len(kernel_data)}B, FAT32@LBA256={len(fat32_part)}B)"
    )


if __name__ == "__main__":
    main()
