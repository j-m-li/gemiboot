/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Virtual Filesystem (VFS) & Device Filesystem (/dev)
 */

#ifndef OKSINIX_VFS_H
#define OKSINIX_VFS_H

#include "types.h"

#define VFS_MAX_NAME    32
#define VFS_MAX_PATH    128
#define VFS_MAX_NODES   64
#define VFS_MAX_FDS     32
#define VFS_MAX_FILE_SZ 512

/* Node types */
#define VFS_TYPE_UNUSED   0
#define VFS_TYPE_DIR      1
#define VFS_TYPE_FILE     2
#define VFS_TYPE_CHARDEV  3
#define VFS_TYPE_BLOCKDEV 4

/* Open flags */
#define VFS_O_RDONLY 0x0000U
#define VFS_O_WRONLY 0x0001U
#define VFS_O_RDWR   0x0002U
#define VFS_O_CREAT  0x0040U
#define VFS_O_TRUNC  0x0200U
#define VFS_O_APPEND 0x0400U

/* Seek whence */
#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

struct vfs_node;

typedef int32_t (*vfs_read_fn)(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buf);
typedef int32_t (*vfs_write_fn)(struct vfs_node *node, uint32_t offset, uint32_t size, const uint8_t *buf);
typedef int     (*vfs_exec_fn)(int argc, char **argv);

struct vfs_node {
    char         name[VFS_MAX_NAME];
    uint32_t     inode;
    uint32_t     type;
    uint32_t     mode;
    uint32_t     size;
    uint16_t     rdev_major;
    uint16_t     rdev_minor;
    int32_t      parent_idx;
    vfs_read_fn  read_op;
    vfs_write_fn write_op;
    vfs_exec_fn  exec_op;
    uint8_t      data[VFS_MAX_FILE_SZ];
};

struct vfs_dirent {
    char     name[VFS_MAX_NAME];
    uint32_t inode;
    uint32_t type;
    uint32_t size;
    uint16_t rdev_major;
    uint16_t rdev_minor;
};

void             vfs_init(void);
void             devfs_init(void);
void             devfs_sync_usb_storage(void);

struct vfs_node *vfs_lookup(const char *path);
int              vfs_mkdir(const char *path);
int              vfs_create_file(const char *path, const char *content, uint32_t size);
int              vfs_register_chardev(const char *path, uint16_t major, uint16_t minor,
                                      vfs_read_fn read_op, vfs_write_fn write_op);
int              vfs_register_blockdev(const char *path, uint16_t major, uint16_t minor,
                                       uint32_t size_bytes,
                                       vfs_read_fn read_op, vfs_write_fn write_op);
int              vfs_register_exec(const char *path, vfs_exec_fn exec_op, const char *desc);

int              vfs_open(const char *path, uint32_t flags);
int32_t          vfs_read(int fd, void *buf, uint32_t count);
int32_t          vfs_write(int fd, const void *buf, uint32_t count);
int32_t          vfs_lseek(int fd, int32_t offset, int whence);
int              vfs_close(int fd);
int              vfs_dup(int oldfd);
int              vfs_dup3(int oldfd, int newfd, uint32_t flags);
int              vfs_dup_min(int oldfd, int min_fd);
int              vfs_get_flags(int fd, uint32_t *out_flags);
int              vfs_set_flags(int fd, uint32_t flags);
int              vfs_ftruncate(int fd, uint32_t length);
int              vfs_unlink(const char *path);
int              vfs_stat(const char *path, struct vfs_dirent *out);
int              vfs_fstat(int fd, struct vfs_dirent *out);
bool_t           vfs_is_chardev(int fd);
int              vfs_readdir(const char *dir_path, uint32_t index, struct vfs_dirent *out);
int              vfs_exec(const char *path, int argc, char **argv);

#endif /* OKSINIX_VFS_H */
