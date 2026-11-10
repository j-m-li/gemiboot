/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Virtual Filesystem (VFS) implementation
 */

#include "vfs.h"
#include "fat32.h"
#include "syscall.h"
#include "string.h"
#include "serial.h"

struct vfs_fd {
    bool_t           used;
    bool_t           is_fat32;
    char             fat32_rel[VFS_MAX_PATH];
    struct vfs_node *node;
    uint32_t         offset;
    uint32_t         flags;
};

static struct vfs_node nodes[VFS_MAX_NODES];
static struct vfs_fd   fd_table[VFS_MAX_FDS];
static struct vfs_node fat32_proxy_node;

static void copy_name(char *dst, const char *src, size_t max_len)
{
    size_t i = 0;
    while (src[i] != '\0' && i + 1 < max_len) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static bool_t is_fat32_path(const char *path, const char **out_rel)
{
    if (!fat32_is_mounted() || path == NULL) {
        return FALSE;
    }
    if (strcmp(path, "/mnt") == 0) {
        *out_rel = "";
        return TRUE;
    }
    if (strncmp(path, "/mnt/", 5) == 0) {
        *out_rel = &path[5];
        return TRUE;
    }
    return FALSE;
}

static int32_t ramfile_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buf)
{
    uint32_t avail;
    if (offset >= node->size) {
        return 0;
    }
    avail = node->size - offset;
    if (size > avail) {
        size = avail;
    }
    memcpy(buf, &node->data[offset], size);
    return (int32_t)size;
}

static int32_t ramfile_write(struct vfs_node *node, uint32_t offset, uint32_t size, const uint8_t *buf)
{
    uint32_t max_write;
    if (offset >= VFS_MAX_FILE_SZ) {
        return 0;
    }
    max_write = VFS_MAX_FILE_SZ - offset;
    if (size > max_write) {
        size = max_write;
    }
    memcpy(&node->data[offset], buf, size);
    if (offset + size > node->size) {
        node->size = offset + size;
    }
    return (int32_t)size;
}

static int32_t alloc_node(const char *name, uint32_t type, int32_t parent_idx)
{
    int32_t i;
    for (i = 0; i < VFS_MAX_NODES; i++) {
        if (nodes[i].type == VFS_TYPE_UNUSED) {
            memset(&nodes[i], 0, sizeof(struct vfs_node));
            copy_name(nodes[i].name, name, VFS_MAX_NAME);
            nodes[i].inode      = (uint32_t)(i + 1);
            nodes[i].type       = type;
            nodes[i].parent_idx = parent_idx;
            return i;
        }
    }
    return -1;
}

static int32_t find_child(int32_t parent_idx, const char *name, size_t name_len)
{
    int32_t i;
    for (i = 0; i < VFS_MAX_NODES; i++) {
        if (nodes[i].type != VFS_TYPE_UNUSED &&
            nodes[i].parent_idx == parent_idx &&
            strlen(nodes[i].name) == name_len &&
            strncmp(nodes[i].name, name, name_len) == 0) {
            return i;
        }
    }
    return -1;
}

static int32_t resolve_path_idx(const char *path)
{
    int32_t cur = 0;
    const char *p;

    if (path == NULL || path[0] != '/') {
        return -1;
    }

    p = path;
    while (*p == '/') {
        p++;
    }
    if (*p == '\0') {
        return 0;
    }

    while (*p != '\0') {
        const char *start = p;
        size_t len = 0;

        while (*p != '\0' && *p != '/') {
            p++;
            len++;
        }

        if (len == 1 && start[0] == '.') {
            /* stay */
        } else if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (nodes[cur].parent_idx >= 0) {
                cur = nodes[cur].parent_idx;
            }
        } else {
            cur = find_child(cur, start, len);
            if (cur < 0) {
                return -1;
            }
        }

        while (*p == '/') {
            p++;
        }
    }

    return cur;
}

static int split_parent_and_name(const char *path, int32_t *out_parent, char *out_name)
{
    char parent_path[VFS_MAX_PATH];
    size_t len;
    size_t last_slash = 0;
    size_t i;

    if (path == NULL || path[0] != '/') {
        return -1;
    }

    len = strlen(path);
    if (len == 0 || len >= VFS_MAX_PATH) {
        return -1;
    }

    for (i = 0; i < len; i++) {
        if (path[i] == '/') {
            last_slash = i;
        }
    }

    if (last_slash == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        memcpy(parent_path, path, last_slash);
        parent_path[last_slash] = '\0';
    }

    copy_name(out_name, &path[last_slash + 1], VFS_MAX_NAME);
    if (out_name[0] == '\0') {
        return -1;
    }

    *out_parent = resolve_path_idx(parent_path);
    if (*out_parent < 0 || nodes[*out_parent].type != VFS_TYPE_DIR) {
        return -1;
    }
    return 0;
}

struct vfs_node *vfs_lookup(const char *path)
{
    const char *rel;
    int32_t idx;

    if (is_fat32_path(path, &rel) && rel[0] != '\0') {
        struct vfs_dirent de;
        if (fat32_stat_path(rel, &de) < 0) {
            return NULL;
        }
        memset(&fat32_proxy_node, 0, sizeof(fat32_proxy_node));
        copy_name(fat32_proxy_node.name, de.name, VFS_MAX_NAME);
        fat32_proxy_node.inode      = de.inode;
        fat32_proxy_node.type       = de.type;
        fat32_proxy_node.mode       = (de.type == VFS_TYPE_DIR) ? 0755U : 0644U;
        fat32_proxy_node.size       = de.size;
        fat32_proxy_node.parent_idx = resolve_path_idx("/mnt");
        return &fat32_proxy_node;
    }

    idx = resolve_path_idx(path);
    if (idx < 0) {
        return NULL;
    }
    return &nodes[idx];
}

int vfs_mkdir(const char *path)
{
    const char *rel;
    int32_t parent_idx;
    char name[VFS_MAX_NAME];
    int32_t idx;

    if (is_fat32_path(path, &rel) && rel[0] != '\0') {
        return fat32_create_path(rel, TRUE);
    }

    if (vfs_lookup(path) != NULL) {
        return 0;
    }
    if (split_parent_and_name(path, &parent_idx, name) < 0) {
        return -1;
    }
    idx = alloc_node(name, VFS_TYPE_DIR, parent_idx);
    if (idx < 0) {
        return -1;
    }
    nodes[idx].mode = 0755U;
    return 0;
}

int vfs_create_file(const char *path, const char *content, uint32_t size)
{
    const char *rel;
    int32_t parent_idx;
    char name[VFS_MAX_NAME];
    int32_t idx;
    struct vfs_node *existing;

    if (is_fat32_path(path, &rel) && rel[0] != '\0') {
        if (fat32_create_path(rel, FALSE) < 0) {
            return -1;
        }
        if (content != NULL && size > 0) {
            if (fat32_write_path(rel, 0, size, (const uint8_t *)content, TRUE) < 0) {
                return -1;
            }
        }
        return 0;
    }

    existing = vfs_lookup(path);
    if (existing != NULL) {
        if (existing->type != VFS_TYPE_FILE) {
            return -1;
        }
        existing->size = 0;
        if (content != NULL && size > 0) {
            (void)ramfile_write(existing, 0, size, (const uint8_t *)content);
        }
        return 0;
    }

    if (split_parent_and_name(path, &parent_idx, name) < 0) {
        return -1;
    }
    idx = alloc_node(name, VFS_TYPE_FILE, parent_idx);
    if (idx < 0) {
        return -1;
    }
    nodes[idx].mode     = 0644U;
    nodes[idx].read_op  = ramfile_read;
    nodes[idx].write_op = ramfile_write;
    if (content != NULL && size > 0) {
        (void)ramfile_write(&nodes[idx], 0, size, (const uint8_t *)content);
    }
    return 0;
}

int vfs_register_chardev(const char *path, uint16_t major, uint16_t minor,
                         vfs_read_fn read_op, vfs_write_fn write_op)
{
    int32_t parent_idx;
    char name[VFS_MAX_NAME];
    int32_t idx;

    if (split_parent_and_name(path, &parent_idx, name) < 0) {
        return -1;
    }
    idx = alloc_node(name, VFS_TYPE_CHARDEV, parent_idx);
    if (idx < 0) {
        return -1;
    }
    nodes[idx].mode       = 0666U;
    nodes[idx].rdev_major = major;
    nodes[idx].rdev_minor = minor;
    nodes[idx].read_op    = read_op;
    nodes[idx].write_op   = write_op;
    return 0;
}

int vfs_register_blockdev(const char *path, uint16_t major, uint16_t minor,
                          uint32_t size_bytes,
                          vfs_read_fn read_op, vfs_write_fn write_op)
{
    int32_t parent_idx;
    char name[VFS_MAX_NAME];
    int32_t idx;

    if (split_parent_and_name(path, &parent_idx, name) < 0) {
        return -1;
    }
    idx = alloc_node(name, VFS_TYPE_BLOCKDEV, parent_idx);
    if (idx < 0) {
        return -1;
    }
    nodes[idx].mode       = 0660U;
    nodes[idx].size       = size_bytes;
    nodes[idx].rdev_major = major;
    nodes[idx].rdev_minor = minor;
    nodes[idx].read_op    = read_op;
    nodes[idx].write_op   = write_op;
    return 0;
}

int vfs_register_exec(const char *path, vfs_exec_fn exec_op, const char *desc)
{
    int32_t parent_idx;
    char name[VFS_MAX_NAME];
    int32_t idx;

    if (split_parent_and_name(path, &parent_idx, name) < 0) {
        return -1;
    }
    idx = alloc_node(name, VFS_TYPE_FILE, parent_idx);
    if (idx < 0) {
        return -1;
    }
    nodes[idx].mode     = 0755U;
    nodes[idx].read_op  = ramfile_read;
    nodes[idx].write_op = NULL;
    nodes[idx].exec_op  = exec_op;

    nodes[idx].data[0] = 0x7FU;
    nodes[idx].data[1] = (uint8_t)'E';
    nodes[idx].data[2] = (uint8_t)'L';
    nodes[idx].data[3] = (uint8_t)'F';
    nodes[idx].size    = 4U;
    if (desc != NULL) {
        uint32_t dlen = (uint32_t)strlen(desc);
        if (4U + dlen < VFS_MAX_FILE_SZ) {
            memcpy(&nodes[idx].data[4], desc, dlen);
            nodes[idx].size = 4U + dlen;
        }
    }
    return 0;
}

int vfs_open(const char *path, uint32_t flags)
{
    const char *rel;
    struct vfs_node *node;
    int fd;

    if (is_fat32_path(path, &rel) && rel[0] != '\0') {
        struct vfs_dirent de;
        if (fat32_stat_path(rel, &de) < 0) {
            if ((flags & VFS_O_CREAT) == 0) {
                return -1;
            }
            if (fat32_create_path(rel, FALSE) < 0 ||
                fat32_stat_path(rel, &de) < 0) {
                return -1;
            }
        }
        if (de.type == VFS_TYPE_FILE && (flags & VFS_O_TRUNC) != 0) {
            (void)fat32_write_path(rel, 0, 0, NULL, TRUE);
            de.size = 0;
        }
        for (fd = 0; fd < VFS_MAX_FDS; fd++) {
            if (!fd_table[fd].used) {
                fd_table[fd].used     = TRUE;
                fd_table[fd].is_fat32 = TRUE;
                copy_name(fd_table[fd].fat32_rel, rel, VFS_MAX_PATH);
                fd_table[fd].node     = NULL;
                fd_table[fd].flags    = flags;
                fd_table[fd].offset   = ((flags & VFS_O_APPEND) != 0) ? de.size : 0U;
                return fd;
            }
        }
        return -1;
    }

    node = vfs_lookup(path);
    if (node == NULL && (flags & VFS_O_CREAT) != 0) {
        if (vfs_create_file(path, NULL, 0) < 0) {
            return -1;
        }
        node = vfs_lookup(path);
    }
    if (node == NULL) {
        return -1;
    }

    if (node->type == VFS_TYPE_FILE && (flags & VFS_O_TRUNC) != 0) {
        node->size = 0;
    }

    for (fd = 0; fd < VFS_MAX_FDS; fd++) {
        if (!fd_table[fd].used) {
            fd_table[fd].used     = TRUE;
            fd_table[fd].is_fat32 = FALSE;
            fd_table[fd].node     = node;
            fd_table[fd].flags    = flags;
            fd_table[fd].offset   = ((flags & VFS_O_APPEND) != 0) ? node->size : 0U;
            return fd;
        }
    }
    return -1;
}

int32_t vfs_read(int fd, void *buf, uint32_t count)
{
    struct vfs_node *node;
    int32_t n;

    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used || buf == NULL) {
        return -1;
    }
    if (fd_table[fd].is_fat32) {
        n = fat32_read_path(fd_table[fd].fat32_rel, fd_table[fd].offset, count, (uint8_t *)buf);
        if (n > 0) {
            fd_table[fd].offset += (uint32_t)n;
        }
        return n;
    }
    node = fd_table[fd].node;
    if (node == NULL || node->read_op == NULL) {
        return -1;
    }
    n = node->read_op(node, fd_table[fd].offset, count, (uint8_t *)buf);
    if (n > 0) {
        fd_table[fd].offset += (uint32_t)n;
    }
    return n;
}

int32_t vfs_write(int fd, const void *buf, uint32_t count)
{
    struct vfs_node *node;
    int32_t n;

    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used || buf == NULL) {
        return -1;
    }
    if (fd_table[fd].is_fat32) {
        n = fat32_write_path(fd_table[fd].fat32_rel, fd_table[fd].offset, count,
                             (const uint8_t *)buf, FALSE);
        if (n > 0) {
            fd_table[fd].offset += (uint32_t)n;
        }
        return n;
    }
    node = fd_table[fd].node;
    if (node == NULL || node->write_op == NULL) {
        return -1;
    }
    n = node->write_op(node, fd_table[fd].offset, count, (const uint8_t *)buf);
    if (n > 0) {
        fd_table[fd].offset += (uint32_t)n;
    }
    return n;
}

int vfs_close(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) {
        return -1;
    }
    fd_table[fd].used     = FALSE;
    fd_table[fd].is_fat32 = FALSE;
    fd_table[fd].node     = NULL;
    fd_table[fd].offset   = 0;
    fd_table[fd].flags    = 0;
    return 0;
}

int32_t vfs_lseek(int fd, int32_t offset, int whence)
{
    int32_t base = 0;
    int32_t new_off;

    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) {
        return -1;
    }
    if (whence == VFS_SEEK_SET) {
        base = 0;
    } else if (whence == VFS_SEEK_CUR) {
        base = (int32_t)fd_table[fd].offset;
    } else if (whence == VFS_SEEK_END) {
        if (fd_table[fd].is_fat32) {
            struct vfs_dirent de;
            if (fat32_stat_path(fd_table[fd].fat32_rel, &de) < 0) {
                return -1;
            }
            base = (int32_t)de.size;
        } else if (fd_table[fd].node != NULL) {
            base = (int32_t)fd_table[fd].node->size;
        } else {
            return -1;
        }
    } else {
        return -1;
    }

    new_off = base + offset;
    if (new_off < 0) {
        return -1;
    }
    fd_table[fd].offset = (uint32_t)new_off;
    return new_off;
}

int vfs_dup_min(int oldfd, int min_fd)
{
    int fd;

    if (oldfd < 0 || oldfd >= VFS_MAX_FDS || !fd_table[oldfd].used) {
        return -1;
    }
    if (min_fd < 0) {
        min_fd = 0;
    }
    for (fd = min_fd; fd < VFS_MAX_FDS; fd++) {
        if (!fd_table[fd].used) {
            fd_table[fd] = fd_table[oldfd];
            return fd;
        }
    }
    return -1;
}

int vfs_dup(int oldfd)
{
    return vfs_dup_min(oldfd, 0);
}

int vfs_dup3(int oldfd, int newfd, uint32_t flags)
{
    (void)flags;
    if (oldfd < 0 || oldfd >= VFS_MAX_FDS || !fd_table[oldfd].used) {
        return -1;
    }
    if (newfd < 0 || newfd >= VFS_MAX_FDS || oldfd == newfd) {
        return -1;
    }
    if (fd_table[newfd].used) {
        (void)vfs_close(newfd);
    }
    fd_table[newfd] = fd_table[oldfd];
    return newfd;
}

int vfs_get_flags(int fd, uint32_t *out_flags)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used || out_flags == NULL) {
        return -1;
    }
    *out_flags = fd_table[fd].flags;
    return 0;
}

int vfs_set_flags(int fd, uint32_t flags)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) {
        return -1;
    }
    fd_table[fd].flags = flags;
    return 0;
}

int vfs_ftruncate(int fd, uint32_t length)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) {
        return -1;
    }
    if (fd_table[fd].is_fat32) {
        return fat32_truncate_path(fd_table[fd].fat32_rel, length);
    }
    if (fd_table[fd].node == NULL || fd_table[fd].node->type != VFS_TYPE_FILE) {
        return -1;
    }
    if (length > VFS_MAX_FILE_SZ) {
        length = VFS_MAX_FILE_SZ;
    }
    if (length > fd_table[fd].node->size) {
        memset(&fd_table[fd].node->data[fd_table[fd].node->size], 0,
               length - fd_table[fd].node->size);
    }
    fd_table[fd].node->size = length;
    return 0;
}

int vfs_unlink(const char *path)
{
    const char *rel;
    int32_t idx;
    int32_t i;

    if (is_fat32_path(path, &rel) && rel[0] != '\0') {
        return fat32_unlink_path(rel);
    }
    idx = resolve_path_idx(path);
    if (idx <= 0) {
        return -1;
    }
    if (nodes[idx].type == VFS_TYPE_DIR) {
        for (i = 1; i < VFS_MAX_NODES; i++) {
            if (nodes[i].type != VFS_TYPE_UNUSED && nodes[i].parent_idx == idx) {
                return -1;
            }
        }
    }
    memset(&nodes[idx], 0, sizeof(struct vfs_node));
    return 0;
}

int vfs_stat(const char *path, struct vfs_dirent *out)
{
    const char *rel;
    struct vfs_node *node;

    if (out == NULL || path == NULL) {
        return -1;
    }
    if (is_fat32_path(path, &rel) && rel[0] != '\0') {
        return fat32_stat_path(rel, out);
    }
    node = vfs_lookup(path);
    if (node == NULL) {
        return -1;
    }
    copy_name(out->name, node->name, VFS_MAX_NAME);
    out->inode      = node->inode;
    out->type       = node->type;
    out->size       = node->size;
    out->rdev_major = node->rdev_major;
    out->rdev_minor = node->rdev_minor;
    return 0;
}

int vfs_fstat(int fd, struct vfs_dirent *out)
{
    struct vfs_node *node;

    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used || out == NULL) {
        return -1;
    }
    if (fd_table[fd].is_fat32) {
        return fat32_stat_path(fd_table[fd].fat32_rel, out);
    }
    node = fd_table[fd].node;
    if (node == NULL) {
        return -1;
    }
    copy_name(out->name, node->name, VFS_MAX_NAME);
    out->inode      = node->inode;
    out->type       = node->type;
    out->size       = node->size;
    out->rdev_major = node->rdev_major;
    out->rdev_minor = node->rdev_minor;
    return 0;
}

bool_t vfs_is_chardev(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used || fd_table[fd].is_fat32) {
        return FALSE;
    }
    if (fd_table[fd].node != NULL && fd_table[fd].node->type == VFS_TYPE_CHARDEV) {
        return TRUE;
    }
    return FALSE;
}

int vfs_readdir(const char *dir_path, uint32_t index, struct vfs_dirent *out)
{
    const char *rel;
    int32_t dir_idx;
    uint32_t cur = 0;
    int32_t i;

    if (out == NULL) {
        return -1;
    }
    if (is_fat32_path(dir_path, &rel)) {
        return fat32_readdir_path(rel, index, out);
    }

    dir_idx = resolve_path_idx(dir_path);
    if (dir_idx < 0 || nodes[dir_idx].type != VFS_TYPE_DIR) {
        return -1;
    }

    for (i = 1; i < VFS_MAX_NODES; i++) {
        if (nodes[i].type != VFS_TYPE_UNUSED && nodes[i].parent_idx == dir_idx) {
            if (cur == index) {
                copy_name(out->name, nodes[i].name, VFS_MAX_NAME);
                out->inode      = nodes[i].inode;
                out->type       = nodes[i].type;
                out->size       = nodes[i].size;
                out->rdev_major = nodes[i].rdev_major;
                out->rdev_minor = nodes[i].rdev_minor;
                return 0;
            }
            cur++;
        }
    }
    return -1;
}

int vfs_exec(const char *path, int argc, char **argv)
{
    struct vfs_node *node = vfs_lookup(path);
    if (node == NULL) {
        return -1;
    }
    if (node->exec_op != NULL) {
        return node->exec_op(argc, argv);
    }
    return linux_elf_execve(path, argc, argv, NULL);
}

void vfs_init(void)
{
    const char *hostname = "oksinix\n";
    const char *motd = "Welcome to Oksinix 0.2 (x86 32-bit ISO C90, Public Domain CC0)\n";
    const char *inittab = "::sysinit:/bin/init\n::respawn:/bin/sh\n";
    const char *keymap = "fr_CH\n";

    memset(nodes, 0, sizeof(nodes));
    memset(fd_table, 0, sizeof(fd_table));

    /* Allocate root directory '/' at index 0 */
    (void)alloc_node("/", VFS_TYPE_DIR, -1);
    nodes[0].mode = 0755U;

    /* Create standard root directories */
    (void)vfs_mkdir("/dev");
    (void)vfs_mkdir("/bin");
    (void)vfs_mkdir("/lib");
    (void)vfs_mkdir("/etc");
    (void)vfs_mkdir("/mnt");
    (void)vfs_mkdir("/tmp");

    /* Populate /etc configuration files */
    (void)vfs_create_file("/etc/hostname", hostname, (uint32_t)strlen(hostname));
    (void)vfs_create_file("/etc/motd", motd, (uint32_t)strlen(motd));
    (void)vfs_create_file("/etc/inittab", inittab, (uint32_t)strlen(inittab));
    (void)vfs_create_file("/etc/keymap", keymap, (uint32_t)strlen(keymap));

    kprintf("[VFS] Root filesystem initialized (/, /dev, /bin, /lib, /etc, /mnt, /tmp)\n");
}
