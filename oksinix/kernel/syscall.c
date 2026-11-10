/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Linux x86 32-bit (i386) ELF Loader & Syscall Layer
 */

#include "syscall.h"
#include "acpi.h"
#include "vfs.h"
#include "pit.h"
#include "console.h"
#include "serial.h"
#include "io.h"
#include "string.h"

/* Linux negative errno values */
#define LINUX_EPERM   1
#define LINUX_ENOENT  2
#define LINUX_ESRCH   3
#define LINUX_EBADF   9
#define LINUX_ECHILD  10
#define LINUX_ENOMEM  12
#define LINUX_EACCES  13
#define LINUX_EFAULT  14
#define LINUX_EINVAL  22
#define LINUX_ENOTTY  25
#define LINUX_ERANGE  34
#define LINUX_ENOSYS  38

/* Linux open/at flags */
#define LINUX_AT_FDCWD       (-100)
#define LINUX_AT_EMPTY_PATH  0x1000
#define LINUX_O_ACCMODE      0x0003U
#define LINUX_O_RDONLY       0x0000U
#define LINUX_O_WRONLY       0x0001U
#define LINUX_O_RDWR         0x0002U
#define LINUX_O_CREAT        0x0040U
#define LINUX_O_TRUNC        0x0200U
#define LINUX_O_APPEND       0x0400U

/* Linux fcntl commands */
#define LINUX_F_DUPFD         0
#define LINUX_F_GETFD         1
#define LINUX_F_SETFD         2
#define LINUX_F_GETFL         3
#define LINUX_F_SETFL         4
#define LINUX_F_DUPFD_CLOEXEC 1030

/* Linux ioctl requests */
#define LINUX_TCGETS     0x5401U
#define LINUX_TCSETS     0x5402U
#define LINUX_TCSETSW    0x5403U
#define LINUX_TCSETSF    0x5404U
#define LINUX_TIOCGPGRP  0x540FU
#define LINUX_TIOCSPGRP  0x5410U
#define LINUX_TIOCGWINSZ 0x5413U

/* Linux mmap flags */
#define LINUX_MAP_FIXED     0x10U
#define LINUX_MAP_ANONYMOUS 0x20U

/* Linux waitid idtype */
#define LINUX_P_ALL  0
#define LINUX_P_PID  1
#define LINUX_P_PGID 2
#define LINUX_WNOWAIT 0x01000000

/* ELF32 header constants */
#define ELF_MAGIC0      0x7FU
#define ELF_MAGIC1      (uint8_t)'E'
#define ELF_MAGIC2      (uint8_t)'L'
#define ELF_MAGIC3      (uint8_t)'F'
#define ELFCLASS32      1U
#define ELFDATA2LSB     1U
#define ET_EXEC         2U
#define ET_DYN          3U
#define EM_386          3U
#define PT_LOAD         1U
#define PT_DYNAMIC      2U
#define PT_INTERP       3U

/* ELF32 Dynamic section tags (d_tag) */
#define DT_NULL         0
#define DT_NEEDED       1
#define DT_PLTRELSZ     2
#define DT_PLTGOT       3
#define DT_HASH         4
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_STRSZ        10
#define DT_SYMENT       11
#define DT_SONAME       14
#define DT_REL          17
#define DT_RELSZ        18
#define DT_RELENT       19
#define DT_PLTREL       20
#define DT_JMPREL       23

/* i386 ELF32 relocation types */
#define R_386_NONE      0U
#define R_386_32        1U
#define R_386_PC32      2U
#define R_386_COPY      5U
#define R_386_GLOB_DAT  6U
#define R_386_JMP_SLOT  7U
#define R_386_RELATIVE  8U

/* ELF Auxiliary Vector types */
#define AT_NULL         0U
#define AT_PHDR         3U
#define AT_PHENT        4U
#define AT_PHNUM        5U
#define AT_PAGESZ       6U
#define AT_BASE         7U
#define AT_ENTRY        9U
#define AT_UID          11U
#define AT_EUID         12U
#define AT_GID          13U
#define AT_EGID         14U
#define AT_RANDOM       25U

/* Flat physical address layout for Linux x86 32-bit process space (16 MiB window at linux_phys_base) */
static uint32_t linux_phys_base = 0x01000000U;

#define LINUX_VA_BASE        (linux_phys_base)
#define LINUX_PIE_LOAD_BASE  (linux_phys_base + 0x00048000U)
#define LINUX_BRK_MAX        (linux_phys_base + 0x00400000U)
#define LINUX_MMAP_BASE      (linux_phys_base + 0x00400000U)
#define LINUX_MMAP_MAX       (linux_phys_base + 0x00500000U)
#define LINUX_SHLIB_BASE     (linux_phys_base + 0x00500000U)
#define LINUX_SHLIB_MAX      (linux_phys_base + 0x00600000U)
#define LINUX_INTERP_BASE    (linux_phys_base + 0x00600000U)
#define LINUX_INTERP_MAX     (linux_phys_base + 0x00680000U)
#define LINUX_STACK_BOTTOM   (linux_phys_base + 0x007D0000U)
#define LINUX_STACK_TOP      (linux_phys_base + 0x007F0000U)
#define LINUX_STACK_SIZE     0x00020000U

/* Snapshot buffers in upper 8 MiB of Linux physical window (+8 MiB .. +16 MiB) */
#define SNAP_IMAGE_BASE      (linux_phys_base + 0x00800000U)
#define SNAP_MMAP_BASE       (linux_phys_base + 0x00C00000U)
#define SNAP_STACK_BASE      (linux_phys_base + 0x00F00000U)

struct elf32_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf32_phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
};

struct elf32_dyn {
    int32_t  d_tag;
    uint32_t d_val;
};

struct elf32_sym {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
};

struct elf32_rel {
    uint32_t r_offset;
    uint32_t r_info;
};

#define MAX_DYN_MODS 6

struct elf_dyn_mod {
    char                    name[VFS_MAX_NAME];
    uint32_t                load_bias;
    uint32_t                dyn_vaddr;
    uint32_t                dyn_size;
    const char             *strtab;
    const struct elf32_sym *symtab;
    uint32_t                sym_count;
    const struct elf32_rel *rel;
    uint32_t                rel_sz;
    const struct elf32_rel *jmprel;
    uint32_t                jmprel_sz;
    uint32_t                needed_offs[6];
    int                     needed_cnt;
};

struct linux_iovec {
    uint32_t iov_base;
    uint32_t iov_len;
};

struct linux_timeval32 {
    int32_t tv_sec;
    int32_t tv_usec;
};

struct linux_timespec32 {
    int32_t tv_sec;
    int32_t tv_nsec;
};

struct linux_timespec64 {
    uint32_t tv_sec_lo;
    int32_t  tv_sec_hi;
    uint32_t tv_nsec_lo;
    int32_t  tv_nsec_hi;
};

struct linux_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

struct linux_stat {
    uint32_t st_dev;
    uint32_t st_ino;
    uint16_t st_mode;
    uint16_t st_nlink;
    uint16_t st_uid;
    uint16_t st_gid;
    uint32_t st_rdev;
    uint32_t st_size;
    uint32_t st_blksize;
    uint32_t st_blocks;
    uint32_t st_atime;
    uint32_t st_atime_nsec;
    uint32_t st_mtime;
    uint32_t st_mtime_nsec;
    uint32_t st_ctime;
    uint32_t st_ctime_nsec;
    uint32_t unused4;
    uint32_t unused5;
};

struct linux_stat64 {
    uint32_t st_dev_lo;
    uint32_t st_dev_hi;
    uint32_t pad0;
    uint32_t __st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_rdev_lo;
    uint32_t st_rdev_hi;
    uint32_t pad3;
    uint32_t st_size_lo;
    uint32_t st_size_hi;
    uint32_t st_blksize;
    uint32_t st_blocks_lo;
    uint32_t st_blocks_hi;
    uint32_t st_atime;
    uint32_t st_atime_nsec;
    uint32_t st_mtime;
    uint32_t st_mtime_nsec;
    uint32_t st_ctime;
    uint32_t st_ctime_nsec;
    uint32_t st_ino_lo;
    uint32_t st_ino_hi;
};

struct linux_statx_timestamp {
    int32_t  tv_sec_lo;
    int32_t  tv_sec_hi;
    uint32_t tv_nsec;
    int32_t  reserved;
};

struct linux_statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint32_t stx_attributes_lo;
    uint32_t stx_attributes_hi;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t pad0;
    uint32_t stx_ino_lo;
    uint32_t stx_ino_hi;
    uint32_t stx_size_lo;
    uint32_t stx_size_hi;
    uint32_t stx_blocks_lo;
    uint32_t stx_blocks_hi;
    uint32_t stx_attributes_mask_lo;
    uint32_t stx_attributes_mask_hi;
    struct linux_statx_timestamp stx_atime;
    struct linux_statx_timestamp stx_btime;
    struct linux_statx_timestamp stx_ctime;
    struct linux_statx_timestamp stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint32_t spare[28];
};

struct linux_siginfo {
    int32_t  si_signo;
    int32_t  si_errno;
    int32_t  si_code;
    int32_t  si_pid;
    uint32_t si_uid;
    int32_t  si_status;
    uint32_t pad[26];
};

#define PROC_UNUSED  0
#define PROC_RUNNING 1
#define PROC_ZOMBIE  2
#define MAX_PROCS    8

struct linux_proc {
    int      state;
    int      pid;
    int      ppid;
    int      exit_code;
    bool_t   is_cloned_child;
    uint32_t load_low;
    uint32_t brk_start;
    uint32_t brk_cur;
    uint32_t mmap_cur;
    uint32_t saved_regs_esp;
    uint32_t saved_image_len;
    uint32_t saved_mmap_len;
    char     cwd[VFS_MAX_PATH];
    char     exe_path[VFS_MAX_PATH];
};

static struct linux_proc proc_table[MAX_PROCS];
static int               cur_proc_idx = -1;
static int               next_pid = 100;
static uint32_t          saved_host_esp = 0;

/* Temporary kernel storage for sys_execve arguments across image replacement */
static uint8_t           exec_kstack[8192];
static int               exec_argc_saved = 0;
static int               exec_envc_saved = 0;
static char              exec_path_buf[VFS_MAX_PATH];
static char              exec_arg_Pool[16][64];
static char             *exec_argv_ptrs[17];
static char              exec_env_Pool[8][64];
static char             *exec_envp_ptrs[9];

static uint32_t page_align_up(uint32_t addr)
{
    return (addr + 0xFFFU) & ~0xFFFU;
}

static uint32_t page_align_down(uint32_t addr)
{
    return addr & ~0xFFFU;
}

static void resolve_user_path(const char *cwd, const char *in_path, char *out_path, size_t max_len)
{
    char combined[VFS_MAX_PATH * 2];
    char segments[16][VFS_MAX_NAME];
    int seg_count = 0;
    const char *p;
    size_t pos;
    int i;

    if (in_path == NULL || in_path[0] == '\0') {
        size_t clen = strlen(cwd);
        if (clen >= max_len) {
            clen = max_len - 1U;
        }
        memcpy(out_path, cwd, clen);
        out_path[clen] = '\0';
        return;
    }

    if (in_path[0] == '/') {
        size_t ilen = strlen(in_path);
        if (ilen >= sizeof(combined)) {
            ilen = sizeof(combined) - 1U;
        }
        memcpy(combined, in_path, ilen);
        combined[ilen] = '\0';
    } else {
        size_t clen = strlen(cwd);
        size_t ilen = strlen(in_path);
        pos = 0;
        if (clen > 1) {
            memcpy(combined, cwd, clen);
            pos = clen;
        }
        combined[pos++] = '/';
        if (pos + ilen >= sizeof(combined)) {
            ilen = sizeof(combined) - pos - 1U;
        }
        memcpy(&combined[pos], in_path, ilen);
        combined[pos + ilen] = '\0';
    }

    p = combined;
    while (*p != '\0') {
        const char *start;
        size_t len = 0;
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        start = p;
        while (*p != '\0' && *p != '/') {
            p++;
            len++;
        }
        if (len == 1 && start[0] == '.') {
            continue;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (seg_count > 0) {
                seg_count--;
            }
            continue;
        }
        if (seg_count < 16) {
            if (len >= VFS_MAX_NAME) {
                len = VFS_MAX_NAME - 1U;
            }
            memcpy(segments[seg_count], start, len);
            segments[seg_count][len] = '\0';
            seg_count++;
        }
    }

    if (seg_count == 0) {
        out_path[0] = '/';
        out_path[1] = '\0';
        return;
    }

    pos = 0;
    for (i = 0; i < seg_count; i++) {
        size_t slen = strlen(segments[i]);
        if (pos + 1U + slen >= max_len) {
            break;
        }
        out_path[pos++] = '/';
        memcpy(&out_path[pos], segments[i], slen);
        pos += slen;
    }
    out_path[pos] = '\0';
}

static uint16_t dirent_to_linux_mode(const struct vfs_dirent *de)
{
    if (de->type == VFS_TYPE_DIR) {
        return (uint16_t)(0040000U | 0755U);
    }
    if (de->type == VFS_TYPE_CHARDEV) {
        return (uint16_t)(0020000U | 0666U);
    }
    if (de->type == VFS_TYPE_BLOCKDEV) {
        return (uint16_t)(0060000U | 0660U);
    }
    return (uint16_t)(0100000U | 0755U);
}

static void fill_linux_stat(struct linux_stat *st, const struct vfs_dirent *de)
{
    uint32_t sec = pit_get_ticks() / 1000U;
    memset(st, 0, sizeof(*st));
    st->st_dev     = 1U;
    st->st_ino     = (de->inode != 0) ? de->inode : 1U;
    st->st_mode    = dirent_to_linux_mode(de);
    st->st_nlink   = 1U;
    st->st_uid     = 0U;
    st->st_gid     = 0U;
    st->st_rdev    = ((uint32_t)de->rdev_major << 8) | (uint32_t)de->rdev_minor;
    st->st_size    = de->size;
    st->st_blksize = 512U;
    st->st_blocks  = (de->size + 511U) / 512U;
    st->st_atime   = sec;
    st->st_mtime   = sec;
    st->st_ctime   = sec;
}

static void fill_linux_stat64(struct linux_stat64 *st, const struct vfs_dirent *de)
{
    uint32_t sec = pit_get_ticks() / 1000U;
    uint32_t ino = (de->inode != 0) ? de->inode : 1U;
    memset(st, 0, sizeof(*st));
    st->st_dev_lo    = 1U;
    st->__st_ino     = ino;
    st->st_mode      = (uint32_t)dirent_to_linux_mode(de);
    st->st_nlink     = 1U;
    st->st_uid       = 0U;
    st->st_gid       = 0U;
    st->st_rdev_lo   = ((uint32_t)de->rdev_major << 8) | (uint32_t)de->rdev_minor;
    st->st_size_lo   = de->size;
    st->st_blksize   = 512U;
    st->st_blocks_lo = (de->size + 511U) / 512U;
    st->st_atime     = sec;
    st->st_mtime     = sec;
    st->st_ctime     = sec;
    st->st_ino_lo    = ino;
}

static void fill_linux_statx(struct linux_statx *sx, const struct vfs_dirent *de, uint32_t mask)
{
    int32_t sec = (int32_t)(pit_get_ticks() / 1000U);
    uint32_t ino = (de->inode != 0) ? de->inode : 1U;
    memset(sx, 0, sizeof(*sx));
    sx->stx_mask        = (mask != 0) ? mask : 0x07FFU;
    sx->stx_blksize     = 512U;
    sx->stx_nlink       = 1U;
    sx->stx_uid         = 0U;
    sx->stx_gid         = 0U;
    sx->stx_mode        = dirent_to_linux_mode(de);
    sx->stx_ino_lo      = ino;
    sx->stx_size_lo     = de->size;
    sx->stx_blocks_lo   = (de->size + 511U) / 512U;
    sx->stx_atime.tv_sec_lo = sec;
    sx->stx_btime.tv_sec_lo = sec;
    sx->stx_ctime.tv_sec_lo = sec;
    sx->stx_mtime.tv_sec_lo = sec;
    sx->stx_rdev_major  = de->rdev_major;
    sx->stx_rdev_minor  = de->rdev_minor;
    sx->stx_dev_major   = 0U;
    sx->stx_dev_minor   = 1U;
}

/*
 * Built-in user-space libc.so.6 functions invoked via PLT/GOT by dynamically
 * linked Linux i386 binaries (all system operations go through int $0x80).
 */
static int user_errno_val = 0;

static int *user_libc_errno_location(void)
{
    return &user_errno_val;
}

static size_t user_libc_strlen(const char *s)
{
    return (s != NULL) ? strlen(s) : 0U;
}

static int user_libc_strcmp(const char *a, const char *b)
{
    return strcmp(a, b);
}

static int user_libc_strncmp(const char *a, const char *b, size_t n)
{
    return strncmp(a, b, n);
}

static char *user_libc_strcpy(char *dst, const char *src)
{
    size_t len = strlen(src) + 1U;
    memcpy(dst, src, len);
    return dst;
}

static void *user_libc_memcpy(void *dst, const void *src, size_t n)
{
    return memcpy(dst, src, n);
}

static void *user_libc_memset(void *dst, int c, size_t n)
{
    return memset(dst, c, n);
}

static int32_t user_libc_write(int fd, const void *buf, uint32_t count)
{
    return linux_int80(LINUX_SYS_write, (uint32_t)fd, (uint32_t)(uintptr_t)buf, count, 0, 0);
}

static int32_t user_libc_read(int fd, void *buf, uint32_t count)
{
    return linux_int80(LINUX_SYS_read, (uint32_t)fd, (uint32_t)(uintptr_t)buf, count, 0, 0);
}

static int user_libc_open(const char *path, int flags, int mode)
{
    return (int)linux_int80(LINUX_SYS_openat, (uint32_t)LINUX_AT_FDCWD,
                            (uint32_t)(uintptr_t)path, (uint32_t)flags, (uint32_t)mode, 0);
}

static int user_libc_close(int fd)
{
    return (int)linux_int80(LINUX_SYS_close, (uint32_t)fd, 0, 0, 0, 0);
}

static int32_t user_libc_lseek(int fd, int32_t offset, int whence)
{
    return linux_int80(LINUX_SYS_lseek, (uint32_t)fd, (uint32_t)offset, (uint32_t)whence, 0, 0);
}

static int user_libc_getpid(void)
{
    return (int)linux_int80(LINUX_SYS_getpid, 0, 0, 0, 0, 0);
}

static int user_libc_puts(const char *s)
{
    char nl = '\n';
    size_t len;
    if (s == NULL) {
        s = "(null)";
    }
    len = strlen(s);
    if (len > 0) {
        (void)user_libc_write(1, s, (uint32_t)len);
    }
    (void)user_libc_write(1, &nl, 1U);
    return (int)(len + 1U);
}

static int user_libc_putchar(int c)
{
    char ch = (char)c;
    (void)user_libc_write(1, &ch, 1U);
    return c;
}

static void *user_libc_malloc(size_t size)
{
    uint32_t cur_brk;
    uint32_t new_brk;
    uint32_t aligned = ((uint32_t)size + 15U) & ~15U;
    if (aligned == 0) {
        aligned = 16U;
    }
    cur_brk = (uint32_t)linux_int80(LINUX_SYS_brk, 0, 0, 0, 0, 0);
    new_brk = (uint32_t)linux_int80(LINUX_SYS_brk, cur_brk + aligned, 0, 0, 0, 0);
    if (new_brk < cur_brk + aligned) {
        return NULL;
    }
    return (void *)(uintptr_t)cur_brk;
}

static void *user_libc_calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void *p = user_libc_malloc(total);
    if (p != NULL) {
        memset(p, 0, total);
    }
    return p;
}

static void user_libc_free(void *ptr)
{
    (void)ptr;
}

static void user_libc_exit(int status)
{
    (void)linux_int80(LINUX_SYS_exit_group, (uint32_t)status, 0, 0, 0, 0);
}

static void user_libc_cxa_finalize(void *d)
{
    (void)d;
}

typedef int (*libc_main_fn)(int, char **, char **);
typedef void (*libc_init_fn)(void);

static int user_libc_start_main(libc_main_fn main_fn, int argc, char **ubp_av,
                                libc_init_fn init_fn, libc_init_fn fini_fn,
                                libc_init_fn rtld_fini_fn, void *stack_end)
{
    char **envp;
    int rc;

    (void)fini_fn;
    (void)rtld_fini_fn;
    (void)stack_end;

    if (init_fn != NULL) {
        init_fn();
    }
    envp = (ubp_av != NULL) ? &ubp_av[argc + 1] : NULL;
    rc = main_fn(argc, ubp_av, envp);
    user_libc_exit(rc);
    return rc;
}

struct libc_export {
    const char *name;
    uint32_t    addr;
};

static uint32_t lookup_builtin_libc_symbol(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    if (strcmp(name, "__libc_start_main") == 0) {
        return (uint32_t)(uintptr_t)user_libc_start_main;
    }
    if (strcmp(name, "puts") == 0) {
        return (uint32_t)(uintptr_t)user_libc_puts;
    }
    if (strcmp(name, "putchar") == 0) {
        return (uint32_t)(uintptr_t)user_libc_putchar;
    }
    if (strcmp(name, "write") == 0) {
        return (uint32_t)(uintptr_t)user_libc_write;
    }
    if (strcmp(name, "read") == 0) {
        return (uint32_t)(uintptr_t)user_libc_read;
    }
    if (strcmp(name, "open") == 0) {
        return (uint32_t)(uintptr_t)user_libc_open;
    }
    if (strcmp(name, "close") == 0) {
        return (uint32_t)(uintptr_t)user_libc_close;
    }
    if (strcmp(name, "lseek") == 0) {
        return (uint32_t)(uintptr_t)user_libc_lseek;
    }
    if (strcmp(name, "getpid") == 0) {
        return (uint32_t)(uintptr_t)user_libc_getpid;
    }
    if (strcmp(name, "malloc") == 0) {
        return (uint32_t)(uintptr_t)user_libc_malloc;
    }
    if (strcmp(name, "calloc") == 0) {
        return (uint32_t)(uintptr_t)user_libc_calloc;
    }
    if (strcmp(name, "free") == 0) {
        return (uint32_t)(uintptr_t)user_libc_free;
    }
    if (strcmp(name, "strlen") == 0) {
        return (uint32_t)(uintptr_t)user_libc_strlen;
    }
    if (strcmp(name, "strcmp") == 0) {
        return (uint32_t)(uintptr_t)user_libc_strcmp;
    }
    if (strcmp(name, "strncmp") == 0) {
        return (uint32_t)(uintptr_t)user_libc_strncmp;
    }
    if (strcmp(name, "strcpy") == 0) {
        return (uint32_t)(uintptr_t)user_libc_strcpy;
    }
    if (strcmp(name, "memcpy") == 0) {
        return (uint32_t)(uintptr_t)user_libc_memcpy;
    }
    if (strcmp(name, "memset") == 0) {
        return (uint32_t)(uintptr_t)user_libc_memset;
    }
    if (strcmp(name, "exit") == 0 || strcmp(name, "_exit") == 0) {
        return (uint32_t)(uintptr_t)user_libc_exit;
    }
    if (strcmp(name, "__cxa_finalize") == 0 || strcmp(name, "__gmon_start__") == 0) {
        return (uint32_t)(uintptr_t)user_libc_cxa_finalize;
    }
    if (strcmp(name, "__errno_location") == 0) {
        return (uint32_t)(uintptr_t)user_libc_errno_location;
    }
    return 0;
}

/* Populate /lib/ld-linux.so.2 in VFS as a valid 111-byte ET_DYN ELF32 interpreter */
static void ensure_ld_linux_vfs(void)
{
    uint8_t img[112];
    struct elf32_ehdr *eh;
    struct elf32_phdr *ph;
    static const uint8_t ld_code[27] = {
        0x8BU, 0x04U, 0x24U,                   /* movl (%esp), %eax */
        0x8DU, 0x74U, 0x84U, 0x08U,            /* leal 8(%esp,%eax,4), %esi */
        0xADU,                                 /* 1: lodsl */
        0x85U, 0xC0U,                          /* testl %eax, %eax */
        0x75U, 0xFBU,                          /* jnz 1b */
        0xADU,                                 /* 2: lodsl (a_type) */
        0x8BU, 0x0EU,                          /* movl (%esi), %ecx (a_val) */
        0x83U, 0xC6U, 0x04U,                   /* addl $4, %esi */
        0x83U, 0xF8U, 0x09U,                   /* cmpl $9, %eax (AT_ENTRY) */
        0x75U, 0xF5U,                          /* jne 2b */
        0x31U, 0xD2U,                          /* xorl %edx, %edx */
        0xFFU, 0xE1U                           /* jmpl *%ecx */
    };

    if (vfs_lookup("/lib/ld-linux.so.2") != NULL) {
        return;
    }

    memset(img, 0, sizeof(img));
    eh = (struct elf32_ehdr *)img;
    eh->e_ident[0]  = ELF_MAGIC0;
    eh->e_ident[1]  = ELF_MAGIC1;
    eh->e_ident[2]  = ELF_MAGIC2;
    eh->e_ident[3]  = ELF_MAGIC3;
    eh->e_ident[4]  = ELFCLASS32;
    eh->e_ident[5]  = ELFDATA2LSB;
    eh->e_ident[6]  = 1U;
    eh->e_type      = ET_DYN;
    eh->e_machine   = EM_386;
    eh->e_version   = 1U;
    eh->e_entry     = 84U; /* 0x54: right after ehdr (52) + phdr (32) */
    eh->e_phoff     = 52U;
    eh->e_ehsize    = 52U;
    eh->e_phentsize = 32U;
    eh->e_phnum     = 1U;

    ph = (struct elf32_phdr *)&img[52];
    ph->p_type   = PT_LOAD;
    ph->p_offset = 0U;
    ph->p_vaddr  = 0U;
    ph->p_paddr  = 0U;
    ph->p_filesz = 111U;
    ph->p_memsz  = 111U;
    ph->p_flags  = 5U; /* PF_R | PF_X */
    ph->p_align  = 4096U;

    memcpy(&img[84], ld_code, sizeof(ld_code));
    (void)vfs_create_file("/lib/ld-linux.so.2", (const char *)img, 111U);
}

static uint32_t adjust_dyn_ptr(uint32_t val, uint32_t load_bias)
{
    if (val == 0) {
        return 0;
    }
    return (val < load_bias) ? (val + load_bias) : val;
}

static void parse_elf32_dynamic(struct elf_dyn_mod *mod)
{
    const struct elf32_dyn *dyn;
    uint32_t max_entries;
    uint32_t i;
    uint32_t strtab_va = 0;
    uint32_t symtab_va = 0;
    uint32_t hash_va   = 0;
    uint32_t rel_va    = 0;
    uint32_t jmprel_va = 0;

    if (mod->dyn_vaddr == 0 || mod->dyn_size < sizeof(struct elf32_dyn)) {
        return;
    }

    dyn = (const struct elf32_dyn *)(uintptr_t)mod->dyn_vaddr;
    max_entries = mod->dyn_size / (uint32_t)sizeof(struct elf32_dyn);

    for (i = 0; i < max_entries && dyn[i].d_tag != DT_NULL; i++) {
        switch (dyn[i].d_tag) {
        case DT_NEEDED:
            if (mod->needed_cnt < 6) {
                mod->needed_offs[mod->needed_cnt++] = dyn[i].d_val;
            }
            break;
        case DT_STRTAB:
            strtab_va = adjust_dyn_ptr(dyn[i].d_val, mod->load_bias);
            break;
        case DT_SYMTAB:
            symtab_va = adjust_dyn_ptr(dyn[i].d_val, mod->load_bias);
            break;
        case DT_HASH:
            hash_va = adjust_dyn_ptr(dyn[i].d_val, mod->load_bias);
            break;
        case DT_REL:
            rel_va = adjust_dyn_ptr(dyn[i].d_val, mod->load_bias);
            break;
        case DT_RELSZ:
            mod->rel_sz = dyn[i].d_val;
            break;
        case DT_JMPREL:
            jmprel_va = adjust_dyn_ptr(dyn[i].d_val, mod->load_bias);
            break;
        case DT_PLTRELSZ:
            mod->jmprel_sz = dyn[i].d_val;
            break;
        default:
            break;
        }
    }

    mod->strtab = (const char *)(uintptr_t)strtab_va;
    mod->symtab = (const struct elf32_sym *)(uintptr_t)symtab_va;
    mod->rel    = (const struct elf32_rel *)(uintptr_t)rel_va;
    mod->jmprel = (const struct elf32_rel *)(uintptr_t)jmprel_va;

    if (hash_va != 0) {
        const uint32_t *ht = (const uint32_t *)(uintptr_t)hash_va;
        mod->sym_count = ht[1]; /* nchain == number of symbols in .dynsym */
    } else if (strtab_va > symtab_va && symtab_va != 0) {
        mod->sym_count = (strtab_va - symtab_va) / (uint32_t)sizeof(struct elf32_sym);
    }
}

static int load_elf32_shlib(const char *soname, uint32_t *shlib_cur_io,
                            struct elf_dyn_mod *out_mod, uint32_t *out_entry)
{
    char try_path[VFS_MAX_PATH];
    int fd = -1;
    struct elf32_ehdr ehdr;
    uint32_t load_bias = *shlib_cur_io;
    uint32_t max_end = load_bias;
    uint16_t i;

    if (soname == NULL || soname[0] == '\0') {
        return -1;
    }

    if (soname[0] == '/') {
        fd = vfs_open(soname, VFS_O_RDONLY);
    }
    if (fd < 0) {
        memcpy(try_path, "/lib/", 5);
        resolve_user_path("/lib", soname, try_path, sizeof(try_path));
        fd = vfs_open(try_path, VFS_O_RDONLY);
    }
    if (fd < 0) {
        resolve_user_path("/mnt", soname, try_path, sizeof(try_path));
        fd = vfs_open(try_path, VFS_O_RDONLY);
    }
    if (fd < 0) {
        return -1;
    }

    if (vfs_read(fd, &ehdr, sizeof(ehdr)) != (int32_t)sizeof(ehdr) ||
        ehdr.e_ident[0] != ELF_MAGIC0 ||
        ehdr.e_ident[1] != ELF_MAGIC1 ||
        ehdr.e_ident[2] != ELF_MAGIC2 ||
        ehdr.e_ident[3] != ELF_MAGIC3 ||
        ehdr.e_machine != EM_386) {
        (void)vfs_close(fd);
        return -1;
    }

    memset(out_mod, 0, sizeof(*out_mod));
    {
        size_t slen = strlen(soname);
        if (slen >= VFS_MAX_NAME) {
            slen = VFS_MAX_NAME - 1U;
        }
        memcpy(out_mod->name, soname, slen);
        out_mod->name[slen] = '\0';
    }
    out_mod->load_bias = load_bias;

    for (i = 0; i < ehdr.e_phnum; i++) {
        struct elf32_phdr phdr;
        uint32_t ph_off = ehdr.e_phoff + (uint32_t)i * (uint32_t)ehdr.e_phentsize;

        if (vfs_lseek(fd, (int32_t)ph_off, VFS_SEEK_SET) < 0 ||
            vfs_read(fd, &phdr, sizeof(phdr)) != (int32_t)sizeof(phdr)) {
            (void)vfs_close(fd);
            return -1;
        }

        if (phdr.p_type == PT_DYNAMIC) {
            out_mod->dyn_vaddr = phdr.p_vaddr + load_bias;
            out_mod->dyn_size  = phdr.p_memsz;
            continue;
        }
        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0) {
            continue;
        }

        {
            uint32_t seg_vaddr = phdr.p_vaddr + load_bias;
            uint32_t seg_end   = seg_vaddr + phdr.p_memsz;
            memset((void *)(uintptr_t)seg_vaddr, 0, phdr.p_memsz);
            if (phdr.p_filesz > 0) {
                if (vfs_lseek(fd, (int32_t)phdr.p_offset, VFS_SEEK_SET) < 0 ||
                    vfs_read(fd, (void *)(uintptr_t)seg_vaddr, phdr.p_filesz) != (int32_t)phdr.p_filesz) {
                    (void)vfs_close(fd);
                    return -1;
                }
            }
            if (seg_end > max_end) {
                max_end = seg_end;
            }
        }
    }

    (void)vfs_close(fd);
    *shlib_cur_io = page_align_up(max_end);
    if (out_entry != NULL) {
        *out_entry = ehdr.e_entry + load_bias;
    }

    parse_elf32_dynamic(out_mod);
    return 0;
}

static uint32_t resolve_dyn_symbol(const char *sym_name, const struct elf_dyn_mod *mods,
                                   int num_mods, int start_mod, uint32_t *out_size)
{
    int m;
    uint32_t builtin;

    if (sym_name == NULL || sym_name[0] == '\0') {
        return 0;
    }

    for (m = start_mod; m < num_mods; m++) {
        const struct elf_dyn_mod *mod = &mods[m];
        uint32_t s;
        if (mod->symtab == NULL || mod->strtab == NULL) {
            continue;
        }
        for (s = 1U; s < mod->sym_count; s++) {
            const struct elf32_sym *sym = &mod->symtab[s];
            const char *cand_name;
            if (sym->st_shndx == 0 || sym->st_value == 0) {
                continue;
            }
            cand_name = &mod->strtab[sym->st_name];
            if (strcmp(cand_name, sym_name) == 0) {
                if (out_size != NULL) {
                    *out_size = sym->st_size;
                }
                return adjust_dyn_ptr(sym->st_value, mod->load_bias);
            }
        }
    }

    builtin = lookup_builtin_libc_symbol(sym_name);
    if (builtin != 0 && out_size != NULL) {
        *out_size = 4U;
    }
    return builtin;
}

static void apply_elf32_reloc_table(const struct elf_dyn_mod *mod,
                                    const struct elf32_rel *rel_tab, uint32_t rel_bytes,
                                    const struct elf_dyn_mod *all_mods, int num_mods)
{
    uint32_t count;
    uint32_t i;

    if (rel_tab == NULL || rel_bytes < sizeof(struct elf32_rel)) {
        return;
    }

    count = rel_bytes / (uint32_t)sizeof(struct elf32_rel);
    for (i = 0; i < count; i++) {
        uint32_t r_off  = rel_tab[i].r_offset;
        uint32_t r_info = rel_tab[i].r_info;
        uint32_t r_sym  = r_info >> 8;
        uint32_t r_type = r_info & 0xFFU;
        uint32_t target = adjust_dyn_ptr(r_off, mod->load_bias);
        uint32_t *where = (uint32_t *)(uintptr_t)target;
        uint32_t sym_val = 0;
        uint32_t sym_sz  = 0;

        if (r_type == R_386_NONE) {
            continue;
        }
        if (r_type == R_386_RELATIVE) {
            *where += mod->load_bias;
            continue;
        }

        if (r_sym != 0 && mod->symtab != NULL && mod->strtab != NULL) {
            const struct elf32_sym *sym = &mod->symtab[r_sym];
            const char *sname = &mod->strtab[sym->st_name];
            int start_idx = (r_type == R_386_COPY) ? 1 : 0;
            sym_val = resolve_dyn_symbol(sname, all_mods, num_mods, start_idx, &sym_sz);
            if (sym_val == 0 && sym->st_shndx != 0 && sym->st_value != 0) {
                sym_val = adjust_dyn_ptr(sym->st_value, mod->load_bias);
                sym_sz  = sym->st_size;
            }
        }

        switch (r_type) {
        case R_386_32:
            *where += sym_val;
            break;
        case R_386_PC32:
            *where += sym_val - target;
            break;
        case R_386_GLOB_DAT:
        case R_386_JMP_SLOT:
            *where = sym_val;
            break;
        case R_386_COPY:
            if (sym_val != 0 && sym_sz > 0) {
                memcpy((void *)(uintptr_t)target, (const void *)(uintptr_t)sym_val, sym_sz);
            }
            break;
        default:
            break;
        }
    }
}

void syscall_init(void)
{
    linux_phys_base = acpi_find_usable_ram(0x01000000U, 0x01000000U, 0x00100000U);

    memset(proc_table, 0, sizeof(proc_table));
    cur_proc_idx = -1;

    kprintf("[SYSCALL] Linux i386 ELF32 & int 0x80 ABI initialized (flat physical RAM: 0x%08x - 0x%08x via ACPI mmap)\n",
            linux_phys_base, linux_phys_base + 0x01000000U - 1U);
}

static struct elf_dyn_mod dyn_mods[MAX_DYN_MODS];

static int load_elf32_into_proc(struct linux_proc *proc, const char *path,
                                int argc, char **argv, char **envp,
                                uint32_t *out_entry, uint32_t *out_user_esp)
{
    int fd;
    struct elf32_ehdr ehdr;
    uint32_t load_bias = 0;
    uint32_t min_vaddr = 0xFFFFFFFFU;
    uint32_t max_vaddr = 0;
    uint32_t phdr_vaddr = 0;
    uint32_t dyn_vaddr = 0;
    uint32_t dyn_size = 0;
    uint32_t interp_base = 0;
    uint32_t interp_entry = 0;
    char interp_path[VFS_MAX_PATH];
    bool_t has_interp = FALSE;
    uint16_t i;
    uint32_t sp;
    uint32_t argv_user_addrs[16];
    uint32_t envp_user_addrs[8];
    uint32_t rand_addr;
    int envc = 0;
    int arg_i;
    uint32_t *ustack;

    ensure_ld_linux_vfs();
    interp_path[0] = '\0';
    if (path != NULL) {
        size_t plen = strlen(path);
        if (plen >= sizeof(proc->exe_path)) {
            plen = sizeof(proc->exe_path) - 1U;
        }
        memcpy(proc->exe_path, path, plen);
        proc->exe_path[plen] = '\0';
    }

    fd = vfs_open(path, VFS_O_RDONLY);
    if (fd < 0) {
        kprintf("[ELF] cannot open %s\n", path);
        return -LINUX_ENOENT;
    }

    if (vfs_read(fd, &ehdr, sizeof(ehdr)) != (int32_t)sizeof(ehdr)) {
        kprintf("[ELF] short header read on %s\n", path);
        (void)vfs_close(fd);
        return -LINUX_ENOENT;
    }

    if (ehdr.e_ident[0] != ELF_MAGIC0 ||
        ehdr.e_ident[1] != ELF_MAGIC1 ||
        ehdr.e_ident[2] != ELF_MAGIC2 ||
        ehdr.e_ident[3] != ELF_MAGIC3 ||
        ehdr.e_ident[4] != ELFCLASS32 ||
        ehdr.e_ident[5] != ELFDATA2LSB ||
        ehdr.e_machine != EM_386 ||
        (ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN) ||
        ehdr.e_phnum == 0 || ehdr.e_phentsize < sizeof(struct elf32_phdr)) {
        kprintf("[ELF] invalid ELF32 header in %s (type=%u machine=%u)\n",
                path, (uint32_t)ehdr.e_type, (uint32_t)ehdr.e_machine);
        (void)vfs_close(fd);
        return -LINUX_EINVAL;
    }

    if (ehdr.e_type == ET_DYN) {
        if (cur_proc_idx < 0) {
            linux_phys_base = acpi_find_usable_ram(0x01000000U, 0x01000000U, 0x00100000U);
        }
        load_bias = LINUX_PIE_LOAD_BASE;
    } else if (cur_proc_idx < 0) {
        if (ehdr.e_entry >= 0x08000000U && ehdr.e_entry < 0x08400000U &&
            acpi_is_usable_ram(0x08000000U, 0x01000000U)) {
            linux_phys_base = 0x08000000U;
        } else if (ehdr.e_entry >= 0x01000000U && ehdr.e_entry < 0x01400000U) {
            linux_phys_base = 0x01000000U;
        }
    }

    for (i = 0; i < ehdr.e_phnum; i++) {
        struct elf32_phdr phdr;
        uint32_t ph_off = ehdr.e_phoff + (uint32_t)i * (uint32_t)ehdr.e_phentsize;
        uint32_t seg_vaddr;
        uint32_t seg_end;

        if (vfs_lseek(fd, (int32_t)ph_off, VFS_SEEK_SET) < 0 ||
            vfs_read(fd, &phdr, sizeof(phdr)) != (int32_t)sizeof(phdr)) {
            kprintf("[ELF] phdr %u read failed in %s\n", (uint32_t)i, path);
            (void)vfs_close(fd);
            return -LINUX_EINVAL;
        }

        if (phdr.p_type == PT_INTERP && phdr.p_filesz > 0 && phdr.p_filesz < sizeof(interp_path)) {
            if (vfs_lseek(fd, (int32_t)phdr.p_offset, VFS_SEEK_SET) >= 0 &&
                vfs_read(fd, interp_path, phdr.p_filesz) == (int32_t)phdr.p_filesz) {
                interp_path[phdr.p_filesz] = '\0';
                has_interp = TRUE;
            }
            continue;
        }

        if (phdr.p_type == PT_DYNAMIC) {
            dyn_vaddr = phdr.p_vaddr + load_bias;
            dyn_size  = phdr.p_memsz;
            continue;
        }

        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0) {
            continue;
        }

        seg_vaddr = phdr.p_vaddr + load_bias;
        seg_end   = seg_vaddr + phdr.p_memsz;
        if (seg_vaddr < LINUX_VA_BASE || seg_end > LINUX_BRK_MAX) {
            kprintf("[ELF] segment [0x%08x..0x%08x] out of range [0x%08x..0x%08x]\n",
                    seg_vaddr, seg_end, LINUX_VA_BASE, LINUX_BRK_MAX);
            (void)vfs_close(fd);
            return -LINUX_ENOMEM;
        }

        if (seg_vaddr < min_vaddr) {
            min_vaddr = seg_vaddr;
        }
        if (seg_end > max_vaddr) {
            max_vaddr = seg_end;
        }

        memset((void *)(uintptr_t)seg_vaddr, 0, phdr.p_memsz);
        if (phdr.p_filesz > 0) {
            if (vfs_lseek(fd, (int32_t)phdr.p_offset, VFS_SEEK_SET) < 0 ||
                vfs_read(fd, (void *)(uintptr_t)seg_vaddr, phdr.p_filesz) != (int32_t)phdr.p_filesz) {
                kprintf("[ELF] segment %u read failed in %s\n", (uint32_t)i, path);
                (void)vfs_close(fd);
                return -LINUX_EINVAL;
            }
        }

        if (phdr.p_offset <= ehdr.e_phoff &&
            ehdr.e_phoff < phdr.p_offset + phdr.p_filesz) {
            phdr_vaddr = seg_vaddr + (ehdr.e_phoff - phdr.p_offset);
        }
    }

    (void)vfs_close(fd);

    if (max_vaddr == 0) {
        return -LINUX_EINVAL;
    }

    proc->load_low  = page_align_down(min_vaddr);
    proc->brk_start = page_align_up(max_vaddr);
    proc->brk_cur   = proc->brk_start;
    proc->mmap_cur  = LINUX_MMAP_BASE;

    /* Perform dynamic linking if PT_DYNAMIC is present */
    if (dyn_vaddr != 0 && dyn_size > 0) {
        int num_mods = 1;
        int n_idx;
        int m_idx;
        uint32_t shlib_cur = LINUX_SHLIB_BASE;

        memset(dyn_mods, 0, sizeof(dyn_mods));
        dyn_mods[0].load_bias = load_bias;
        dyn_mods[0].dyn_vaddr = dyn_vaddr;
        dyn_mods[0].dyn_size  = dyn_size;
        parse_elf32_dynamic(&dyn_mods[0]);

        if (dyn_mods[0].strtab != NULL) {
            for (n_idx = 0; n_idx < dyn_mods[0].needed_cnt && num_mods < MAX_DYN_MODS; n_idx++) {
                const char *soname = &dyn_mods[0].strtab[dyn_mods[0].needed_offs[n_idx]];
                if (strcmp(soname, "libc.so.6") == 0 ||
                    strcmp(soname, "ld-linux.so.2") == 0 ||
                    strcmp(soname, "/lib/ld-linux.so.2") == 0) {
                    continue;
                }
                if (load_elf32_shlib(soname, &shlib_cur, &dyn_mods[num_mods], NULL) == 0) {
                    kprintf("[DYN] Loaded shared library \"%s\" at 0x%08x\n",
                            soname, dyn_mods[num_mods].load_bias);
                    num_mods++;
                }
            }
        }

        /* Apply relocations to shared libraries first, then to main executable */
        for (m_idx = num_mods - 1; m_idx >= 0; m_idx--) {
            apply_elf32_reloc_table(&dyn_mods[m_idx], dyn_mods[m_idx].rel,
                                    dyn_mods[m_idx].rel_sz, dyn_mods, num_mods);
            apply_elf32_reloc_table(&dyn_mods[m_idx], dyn_mods[m_idx].jmprel,
                                    dyn_mods[m_idx].jmprel_sz, dyn_mods, num_mods);
        }
    }

    /* Load PT_INTERP ELF dynamic linker if requested */
    if (has_interp) {
        struct elf_dyn_mod interp_mod;
        uint32_t ibase = LINUX_INTERP_BASE;
        const char *ipath = interp_path;
        if (vfs_lookup(ipath) == NULL) {
            ipath = "/lib/ld-linux.so.2";
        }
        if (load_elf32_shlib(ipath, &ibase, &interp_mod, &interp_entry) == 0) {
            interp_base = LINUX_INTERP_BASE;
            kprintf("[DYN] PT_INTERP \"%s\" loaded at 0x%08x (entry=0x%08x)\n",
                    interp_path, interp_base, interp_entry);
        }
    }

    /* Clear user stack region */
    memset((void *)(uintptr_t)LINUX_STACK_BOTTOM, 0, LINUX_STACK_SIZE);
    sp = LINUX_STACK_TOP - 64U;

    /* Push 16 random bytes for AT_RANDOM */
    sp -= 16U;
    rand_addr = sp;
    for (i = 0; i < 16U; i++) {
        ((uint8_t *)(uintptr_t)rand_addr)[i] = (uint8_t)(0xA5U ^ (i * 17U));
    }

    /* Copy environment strings onto top of stack */
    if (envp != NULL) {
        while (envp[envc] != NULL && envc < 8) {
            size_t len = strlen(envp[envc]) + 1U;
            sp -= (uint32_t)len;
            memcpy((void *)(uintptr_t)sp, envp[envc], len);
            envp_user_addrs[envc] = sp;
            envc++;
        }
    } else {
        const char *def_env = "PATH=/bin:/mnt";
        size_t len = strlen(def_env) + 1U;
        sp -= (uint32_t)len;
        memcpy((void *)(uintptr_t)sp, def_env, len);
        envp_user_addrs[0] = sp;
        envc = 1;
    }

    /* Copy argument strings onto stack */
    if (argc < 0) {
        argc = 0;
    }
    if (argc > 16) {
        argc = 16;
    }
    for (arg_i = 0; arg_i < argc; arg_i++) {
        const char *s = (argv != NULL && argv[arg_i] != NULL) ? argv[arg_i] : "";
        size_t len = strlen(s) + 1U;
        sp -= (uint32_t)len;
        memcpy((void *)(uintptr_t)sp, s, len);
        argv_user_addrs[arg_i] = sp;
    }

    /* Align stack pointer to 16-byte boundary before pushing table */
    sp &= ~0xFU;
    /* Reserve space for: argc (1) + argv (argc+1) + envp (envc+1) + auxv (24) */
    sp -= (uint32_t)((1 + (argc + 1) + (envc + 1) + 24) * 4);
    sp &= ~0xFU;

    ustack = (uint32_t *)(uintptr_t)sp;
    *ustack++ = (uint32_t)argc;
    for (arg_i = 0; arg_i < argc; arg_i++) {
        *ustack++ = argv_user_addrs[arg_i];
    }
    *ustack++ = 0U;

    for (arg_i = 0; arg_i < envc; arg_i++) {
        *ustack++ = envp_user_addrs[arg_i];
    }
    *ustack++ = 0U;

    /* ELF Auxiliary Vector (auxv) */
    *ustack++ = AT_PAGESZ; *ustack++ = 4096U;
    *ustack++ = AT_PHDR;   *ustack++ = phdr_vaddr;
    *ustack++ = AT_PHENT;  *ustack++ = (uint32_t)ehdr.e_phentsize;
    *ustack++ = AT_PHNUM;  *ustack++ = (uint32_t)ehdr.e_phnum;
    *ustack++ = AT_BASE;   *ustack++ = (interp_base != 0) ? interp_base : load_bias;
    *ustack++ = AT_ENTRY;  *ustack++ = ehdr.e_entry + load_bias;
    *ustack++ = AT_UID;    *ustack++ = 0U;
    *ustack++ = AT_EUID;   *ustack++ = 0U;
    *ustack++ = AT_GID;    *ustack++ = 0U;
    *ustack++ = AT_EGID;   *ustack++ = 0U;
    *ustack++ = AT_RANDOM; *ustack++ = rand_addr;
    *ustack++ = AT_NULL;   *ustack++ = 0U;

    *out_entry    = (interp_entry != 0) ? interp_entry : (ehdr.e_entry + load_bias);
    *out_user_esp = sp;
    return 0;
}

int linux_elf_execve(const char *path, int argc, char **argv, char **envp)
{
    int idx = -1;
    int i;
    uint32_t entry = 0;
    uint32_t user_esp = 0;
    int rc;
    struct linux_proc *proc;

    for (i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_UNUSED) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        return -1;
    }

    proc = &proc_table[idx];
    memset(proc, 0, sizeof(*proc));
    proc->state = PROC_RUNNING;
    proc->pid   = next_pid++;
    proc->ppid  = 2; /* /bin/sh */
    proc->is_cloned_child = FALSE;
    proc->cwd[0] = '/';
    proc->cwd[1] = '\0';
    if (path != NULL && path[0] == '/') {
        const char *last_slash = NULL;
        const char *p = path;
        while (*p != '\0') {
            if (*p == '/') {
                last_slash = p;
            }
            p++;
        }
        if (last_slash != NULL && last_slash > path) {
            size_t dlen = (size_t)(last_slash - path);
            if (dlen < sizeof(proc->cwd)) {
                memcpy(proc->cwd, path, dlen);
                proc->cwd[dlen] = '\0';
            }
        }
    }

    rc = load_elf32_into_proc(proc, path, argc, argv, envp, &entry, &user_esp);
    if (rc < 0) {
        proc->state = PROC_UNUSED;
        return -1;
    }

    cur_proc_idx = idx;
    kprintf("[ELF32] Launching Linux i386 binary \"%s\" (PID %d, entry=0x%08x, esp=0x%08x)\n",
            path, proc->pid, entry, user_esp);

    rc = linux_enter_elf(entry, user_esp, &saved_host_esp);
    console_refresh();
    return rc;
}

static void handle_process_exit(int exit_code)
{
    struct linux_proc *proc;

    if (cur_proc_idx < 0 || cur_proc_idx >= MAX_PROCS) {
        linux_return_to_host(saved_host_esp, exit_code);
        return;
    }

    proc = &proc_table[cur_proc_idx];
    kprintf("[SYSCALL] Process PID %d exited with code %d\n", proc->pid, exit_code);

    if (proc->is_cloned_child) {
        int parent_idx = -1;
        int i;
        for (i = 0; i < MAX_PROCS; i++) {
            if (proc_table[i].state == PROC_RUNNING && proc_table[i].pid == proc->ppid) {
                parent_idx = i;
                break;
            }
        }
        proc->state     = PROC_ZOMBIE;
        proc->exit_code = exit_code;

        if (parent_idx >= 0) {
            struct linux_proc *parent = &proc_table[parent_idx];
            struct isr_regs *saved_regs_in_snap;
            uint32_t regs_off;

            cur_proc_idx = parent_idx;

            /* Restore parent's code/data/brk and mmap regions */
            if (parent->saved_image_len > 0) {
                memcpy((void *)(uintptr_t)parent->load_low,
                       (const void *)(uintptr_t)SNAP_IMAGE_BASE,
                       parent->saved_image_len);
            }
            if (parent->saved_mmap_len > 0) {
                memcpy((void *)(uintptr_t)LINUX_MMAP_BASE,
                       (const void *)(uintptr_t)SNAP_MMAP_BASE,
                       parent->saved_mmap_len);
            }

            /* Set parent's return value (eax = child_pid) in saved stack snapshot */
            regs_off = parent->saved_regs_esp - LINUX_STACK_BOTTOM;
            saved_regs_in_snap = (struct isr_regs *)(uintptr_t)(SNAP_STACK_BASE + regs_off);
            saved_regs_in_snap->eax = (uint32_t)proc->pid;

            /* Restore parent stack and iret directly into parent */
            linux_restore_and_resume(LINUX_STACK_BOTTOM,
                                     (const void *)(uintptr_t)SNAP_STACK_BASE,
                                     LINUX_STACK_SIZE / 4U,
                                     parent->saved_regs_esp);
        }
    }

    /* Clean up any remaining processes and return to host kernel (/bin/sh) */
    memset(proc_table, 0, sizeof(proc_table));
    cur_proc_idx = -1;
    linux_return_to_host(saved_host_esp, exit_code);
}

static int32_t do_openat(struct linux_proc *proc, int dirfd, const char *upath, uint32_t lflags)
{
    char abs_path[VFS_MAX_PATH];
    uint32_t vflags = 0;
    int fd;

    (void)dirfd;
    if (upath == NULL) {
        return -LINUX_EFAULT;
    }
    resolve_user_path(proc->cwd, upath, abs_path, sizeof(abs_path));

    if ((lflags & LINUX_O_ACCMODE) == LINUX_O_WRONLY) {
        vflags |= VFS_O_WRONLY;
    } else if ((lflags & LINUX_O_ACCMODE) == LINUX_O_RDWR) {
        vflags |= VFS_O_RDWR;
    } else {
        vflags |= VFS_O_RDONLY;
    }
    if ((lflags & LINUX_O_CREAT) != 0) {
        vflags |= VFS_O_CREAT;
    }
    if ((lflags & LINUX_O_TRUNC) != 0) {
        vflags |= VFS_O_TRUNC;
    }
    if ((lflags & LINUX_O_APPEND) != 0) {
        vflags |= VFS_O_APPEND;
    }

    fd = vfs_open(abs_path, vflags);
    if (fd < 0) {
        return -LINUX_ENOENT;
    }
    return fd;
}

static int32_t do_mmap(struct linux_proc *proc, uint32_t addr, uint32_t len,
                       uint32_t prot, uint32_t flags, int fd, uint32_t file_offset)
{
    uint32_t aligned_len;
    uint32_t map_addr;

    (void)prot;
    if (len == 0) {
        return -LINUX_EINVAL;
    }
    aligned_len = page_align_up(len);

    if ((flags & LINUX_MAP_FIXED) != 0 && addr != 0) {
        if (addr < LINUX_VA_BASE || addr + aligned_len > LINUX_MMAP_MAX) {
            return -LINUX_ENOMEM;
        }
        map_addr = addr;
    } else {
        if (proc->mmap_cur + aligned_len > LINUX_MMAP_MAX) {
            return -LINUX_ENOMEM;
        }
        map_addr = proc->mmap_cur;
        proc->mmap_cur += aligned_len;
    }

    memset((void *)(uintptr_t)map_addr, 0, aligned_len);

    if ((flags & LINUX_MAP_ANONYMOUS) == 0 && fd >= 0) {
        int32_t saved_off = vfs_lseek(fd, 0, VFS_SEEK_CUR);
        if (saved_off < 0) {
            return -LINUX_EBADF;
        }
        if (vfs_lseek(fd, (int32_t)file_offset, VFS_SEEK_SET) >= 0) {
            (void)vfs_read(fd, (void *)(uintptr_t)map_addr, len);
        }
        (void)vfs_lseek(fd, saved_off, VFS_SEEK_SET);
    }

    return (int32_t)map_addr;
}

static int32_t do_clone(struct linux_proc *parent, struct isr_regs *regs, uint32_t child_stack)
{
    int child_idx = -1;
    int i;
    struct linux_proc *child;

    for (i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_UNUSED) {
            child_idx = i;
            break;
        }
    }
    if (child_idx < 0) {
        return -LINUX_ENOMEM;
    }

    /* Snapshot parent's load+brk region, mmap region, and stack */
    parent->saved_image_len = parent->brk_cur - parent->load_low;
    parent->saved_mmap_len  = parent->mmap_cur - LINUX_MMAP_BASE;
    parent->saved_regs_esp  = (uint32_t)(uintptr_t)regs;

    if (parent->saved_image_len > 0) {
        memcpy((void *)(uintptr_t)SNAP_IMAGE_BASE,
               (const void *)(uintptr_t)parent->load_low,
               parent->saved_image_len);
    }
    if (parent->saved_mmap_len > 0) {
        memcpy((void *)(uintptr_t)SNAP_MMAP_BASE,
               (const void *)(uintptr_t)LINUX_MMAP_BASE,
               parent->saved_mmap_len);
    }
    memcpy((void *)(uintptr_t)SNAP_STACK_BASE,
           (const void *)(uintptr_t)LINUX_STACK_BOTTOM,
           LINUX_STACK_SIZE);

    child = &proc_table[child_idx];
    *child = *parent;
    child->state           = PROC_RUNNING;
    child->pid             = next_pid++;
    child->ppid            = parent->pid;
    child->exit_code       = 0;
    child->is_cloned_child = TRUE;

    cur_proc_idx = child_idx;

    if (child_stack != 0 &&
        child_stack >= LINUX_VA_BASE + sizeof(struct isr_regs) &&
        child_stack <= LINUX_STACK_TOP) {
        uint32_t new_regs_esp = child_stack - (uint32_t)sizeof(struct isr_regs);
        struct isr_regs *child_regs = (struct isr_regs *)(uintptr_t)new_regs_esp;
        *child_regs = *regs;
        child_regs->eax = 0U;
        linux_resume_regs(new_regs_esp);
    }

    return 0; /* Child receives 0 in %eax */
}

static void handle_process_exit(int exit_code);

static void do_execve_on_kstack(void)
{
    struct linux_proc *proc = &proc_table[cur_proc_idx];
    uint32_t entry = 0;
    uint32_t user_esp = 0;
    int rc;

    rc = load_elf32_into_proc(proc, exec_path_buf, exec_argc_saved, exec_argv_ptrs,
                              (exec_envc_saved > 0) ? exec_envp_ptrs : NULL,
                              &entry, &user_esp);
    if (rc < 0) {
        handle_process_exit(127);
        return;
    }

    kprintf("[SYSCALL] SYS_execve(\"%s\") in PID %d -> entry=0x%08x\n",
            exec_path_buf, proc->pid, entry);
    (void)linux_enter_elf(entry, user_esp, NULL);
}

static int32_t do_execve(struct linux_proc *proc, const char *ufilename,
                         char *const uargv[], char *const uenvp[])
{
    int argc = 0;
    int envc = 0;

    if (ufilename == NULL) {
        return -LINUX_EFAULT;
    }

    resolve_user_path(proc->cwd, ufilename, exec_path_buf, sizeof(exec_path_buf));
    if ((strcmp(exec_path_buf, "/proc/self/exe") == 0 ||
         strcmp(exec_path_buf, "/mnt/LINUX.ELF") == 0 ||
         vfs_lookup(exec_path_buf) == NULL) &&
        proc->exe_path[0] != '\0') {
        size_t elen = strlen(proc->exe_path);
        if (elen >= sizeof(exec_path_buf)) {
            elen = sizeof(exec_path_buf) - 1U;
        }
        memcpy(exec_path_buf, proc->exe_path, elen);
        exec_path_buf[elen] = '\0';
    }
    if (vfs_lookup(exec_path_buf) == NULL) {
        return -LINUX_ENOENT;
    }

    if (uargv != NULL) {
        while (uargv[argc] != NULL && argc < 16) {
            size_t len = strlen(uargv[argc]);
            if (len >= 63U) {
                len = 63U;
            }
            memcpy(exec_arg_Pool[argc], uargv[argc], len);
            exec_arg_Pool[argc][len] = '\0';
            exec_argv_ptrs[argc] = exec_arg_Pool[argc];
            argc++;
        }
    }
    exec_argv_ptrs[argc] = NULL;

    if (uenvp != NULL) {
        while (uenvp[envc] != NULL && envc < 8) {
            size_t len = strlen(uenvp[envc]);
            if (len >= 63U) {
                len = 63U;
            }
            memcpy(exec_env_Pool[envc], uenvp[envc], len);
            exec_env_Pool[envc][len] = '\0';
            exec_envp_ptrs[envc] = exec_env_Pool[envc];
            envc++;
        }
    }
    exec_envp_ptrs[envc] = NULL;

    exec_argc_saved = argc;
    exec_envc_saved = envc;

    linux_call_on_kstack(do_execve_on_kstack,
                         (uint32_t)(uintptr_t)&exec_kstack[sizeof(exec_kstack)]);
    return 0;
}

void syscall_dispatch(struct isr_regs *regs)
{
    uint32_t nr   = regs->eax;
    uint32_t arg1 = regs->ebx;
    uint32_t arg2 = regs->ecx;
    uint32_t arg3 = regs->edx;
    uint32_t arg4 = regs->esi;
    uint32_t arg5 = regs->edi;
    uint32_t arg6 = regs->ebp;
    int32_t  ret  = -LINUX_ENOSYS;
    struct linux_proc *proc;

    if (cur_proc_idx < 0 || cur_proc_idx >= MAX_PROCS) {
        proc_table[0].state = PROC_RUNNING;
        proc_table[0].pid   = 100;
        proc_table[0].ppid  = 2;
        proc_table[0].cwd[0] = '/';
        proc_table[0].cwd[1] = '\0';
        cur_proc_idx = 0;
    }
    proc = &proc_table[cur_proc_idx];

    switch (nr) {
    case LINUX_SYS_exit:
    case LINUX_SYS_exit_group:
        handle_process_exit((int)arg1);
        ret = 0;
        break;

    case LINUX_SYS_getcwd: {
        char *buf = (char *)(uintptr_t)arg1;
        uint32_t size = arg2;
        size_t len = strlen(proc->cwd) + 1U;
        if (buf == NULL) {
            ret = -LINUX_EFAULT;
        } else if (size < len) {
            ret = -LINUX_ERANGE;
        } else {
            memcpy(buf, proc->cwd, len);
            ret = (int32_t)len;
        }
        break;
    }

    case LINUX_SYS_dup:
        ret = vfs_dup((int)arg1);
        if (ret < 0) {
            ret = -LINUX_EBADF;
        }
        break;

    case LINUX_SYS_dup2:
        if ((int)arg1 == (int)arg2) {
            uint32_t fl;
            ret = (vfs_get_flags((int)arg1, &fl) == 0) ? (int32_t)arg1 : -LINUX_EBADF;
        } else {
            ret = vfs_dup3((int)arg1, (int)arg2, 0);
            if (ret < 0) {
                ret = -LINUX_EBADF;
            }
        }
        break;

    case LINUX_SYS_dup3:
        if ((int)arg1 == (int)arg2) {
            ret = -LINUX_EINVAL;
        } else {
            ret = vfs_dup3((int)arg1, (int)arg2, arg3);
            if (ret < 0) {
                ret = -LINUX_EBADF;
            }
        }
        break;

    case LINUX_SYS_fcntl:
    case LINUX_SYS_fcntl64: {
        int fd = (int)arg1;
        int cmd = (int)arg2;
        uint32_t fl = 0;
        if (vfs_get_flags(fd, &fl) < 0) {
            ret = -LINUX_EBADF;
            break;
        }
        if (cmd == LINUX_F_DUPFD || cmd == LINUX_F_DUPFD_CLOEXEC) {
            ret = vfs_dup_min(fd, (int)arg3);
            if (ret < 0) {
                ret = -LINUX_EINVAL;
            }
        } else if (cmd == LINUX_F_GETFD || cmd == LINUX_F_SETFD) {
            ret = 0;
        } else if (cmd == LINUX_F_GETFL) {
            ret = (int32_t)fl;
        } else if (cmd == LINUX_F_SETFL) {
            (void)vfs_set_flags(fd, arg3);
            ret = 0;
        } else {
            ret = 0;
        }
        break;
    }

    case LINUX_SYS_ioctl: {
        int fd = (int)arg1;
        uint32_t req = arg2;
        uint32_t fl;
        if (vfs_get_flags(fd, &fl) < 0) {
            ret = -LINUX_EBADF;
            break;
        }
        if (!vfs_is_chardev(fd)) {
            ret = -LINUX_ENOTTY;
            break;
        }
        if (req == LINUX_TIOCGWINSZ && arg3 != 0) {
            struct linux_winsize *ws = (struct linux_winsize *)(uintptr_t)arg3;
            ws->ws_row    = CON_ROWS;
            ws->ws_col    = CON_COLS;
            ws->ws_xpixel = (uint16_t)(CON_COLS * 8);
            ws->ws_ypixel = (uint16_t)(CON_ROWS * 10);
            ret = 0;
        } else if (req == LINUX_TCGETS && arg3 != 0) {
            memset((void *)(uintptr_t)arg3, 0, 36);
            ret = 0;
        } else if (req == LINUX_TCSETS || req == LINUX_TCSETSW ||
                   req == LINUX_TCSETSF || req == LINUX_TIOCGPGRP ||
                   req == LINUX_TIOCSPGRP) {
            ret = 0;
        } else {
            ret = 0;
        }
        break;
    }

    case LINUX_SYS_unlink: {
        char abs_path[VFS_MAX_PATH];
        if (arg1 == 0) {
            ret = -LINUX_EFAULT;
            break;
        }
        resolve_user_path(proc->cwd, (const char *)(uintptr_t)arg1, abs_path, sizeof(abs_path));
        ret = (vfs_unlink(abs_path) == 0) ? 0 : -LINUX_ENOENT;
        break;
    }

    case LINUX_SYS_unlinkat: {
        char abs_path[VFS_MAX_PATH];
        if (arg2 == 0) {
            ret = -LINUX_EFAULT;
            break;
        }
        resolve_user_path(proc->cwd, (const char *)(uintptr_t)arg2, abs_path, sizeof(abs_path));
        ret = (vfs_unlink(abs_path) == 0) ? 0 : -LINUX_ENOENT;
        break;
    }

    case LINUX_SYS_ftruncate:
    case LINUX_SYS_ftruncate64:
        ret = (vfs_ftruncate((int)arg1, arg2) == 0) ? 0 : -LINUX_EINVAL;
        break;

    case LINUX_SYS_access: {
        char abs_path[VFS_MAX_PATH];
        if (arg1 == 0) {
            ret = -LINUX_EFAULT;
            break;
        }
        resolve_user_path(proc->cwd, (const char *)(uintptr_t)arg1, abs_path, sizeof(abs_path));
        ret = (vfs_lookup(abs_path) != NULL) ? 0 : -LINUX_ENOENT;
        break;
    }

    case LINUX_SYS_faccessat: {
        char abs_path[VFS_MAX_PATH];
        if (arg2 == 0) {
            ret = -LINUX_EFAULT;
            break;
        }
        resolve_user_path(proc->cwd, (const char *)(uintptr_t)arg2, abs_path, sizeof(abs_path));
        ret = (vfs_lookup(abs_path) != NULL) ? 0 : -LINUX_ENOENT;
        break;
    }

    case LINUX_SYS_open:
        ret = do_openat(proc, LINUX_AT_FDCWD, (const char *)(uintptr_t)arg1, arg2);
        break;

    case LINUX_SYS_openat:
        ret = do_openat(proc, (int)arg1, (const char *)(uintptr_t)arg2, arg3);
        break;

    case LINUX_SYS_close:
        ret = (vfs_close((int)arg1) == 0) ? 0 : -LINUX_EBADF;
        break;

    case LINUX_SYS_lseek:
        ret = vfs_lseek((int)arg1, (int32_t)arg2, (int)arg3);
        if (ret < 0) {
            ret = -LINUX_EINVAL;
        }
        break;

    case LINUX_SYS__llseek: {
        int32_t pos = vfs_lseek((int)arg1, (int32_t)arg3, (int)arg5);
        if (pos < 0) {
            ret = -LINUX_EINVAL;
        } else {
            if (arg4 != 0) {
                uint32_t *res64 = (uint32_t *)(uintptr_t)arg4;
                res64[0] = (uint32_t)pos;
                res64[1] = 0U;
            }
            ret = 0;
        }
        break;
    }

    case LINUX_SYS_read:
        if (arg3 == 0) {
            ret = 0;
        } else {
            ret = vfs_read((int)arg1, (void *)(uintptr_t)arg2, arg3);
            if (ret < 0) {
                ret = -LINUX_EBADF;
            }
        }
        break;

    case LINUX_SYS_write:
        if (arg3 == 0) {
            ret = 0;
        } else {
            ret = vfs_write((int)arg1, (const void *)(uintptr_t)arg2, arg3);
            if (ret < 0) {
                ret = -LINUX_EBADF;
            }
        }
        break;

    case LINUX_SYS_readv: {
        const struct linux_iovec *iov = (const struct linux_iovec *)(uintptr_t)arg2;
        int iovcnt = (int)arg3;
        int i;
        int32_t total = 0;
        if (iov == NULL || iovcnt < 0) {
            ret = -LINUX_EINVAL;
            break;
        }
        for (i = 0; i < iovcnt; i++) {
            int32_t n;
            if (iov[i].iov_len == 0) {
                continue;
            }
            n = vfs_read((int)arg1, (void *)(uintptr_t)iov[i].iov_base, iov[i].iov_len);
            if (n < 0) {
                if (total == 0) {
                    total = -LINUX_EBADF;
                }
                break;
            }
            total += n;
            if ((uint32_t)n < iov[i].iov_len) {
                break;
            }
        }
        ret = total;
        break;
    }

    case LINUX_SYS_writev: {
        const struct linux_iovec *iov = (const struct linux_iovec *)(uintptr_t)arg2;
        int iovcnt = (int)arg3;
        int i;
        int32_t total = 0;
        if (iov == NULL || iovcnt < 0) {
            ret = -LINUX_EINVAL;
            break;
        }
        for (i = 0; i < iovcnt; i++) {
            int32_t n;
            if (iov[i].iov_len == 0) {
                continue;
            }
            n = vfs_write((int)arg1, (const void *)(uintptr_t)iov[i].iov_base, iov[i].iov_len);
            if (n < 0) {
                if (total == 0) {
                    total = -LINUX_EBADF;
                }
                break;
            }
            total += n;
        }
        ret = total;
        break;
    }

    case LINUX_SYS_stat:
    case LINUX_SYS_lstat: {
        char abs_path[VFS_MAX_PATH];
        struct vfs_dirent de;
        if (arg1 == 0 || arg2 == 0) {
            ret = -LINUX_EFAULT;
            break;
        }
        resolve_user_path(proc->cwd, (const char *)(uintptr_t)arg1, abs_path, sizeof(abs_path));
        if (vfs_stat(abs_path, &de) < 0) {
            ret = -LINUX_ENOENT;
            break;
        }
        fill_linux_stat((struct linux_stat *)(uintptr_t)arg2, &de);
        ret = 0;
        break;
    }

    case LINUX_SYS_stat64:
    case LINUX_SYS_lstat64: {
        char abs_path[VFS_MAX_PATH];
        struct vfs_dirent de;
        if (arg1 == 0 || arg2 == 0) {
            ret = -LINUX_EFAULT;
            break;
        }
        resolve_user_path(proc->cwd, (const char *)(uintptr_t)arg1, abs_path, sizeof(abs_path));
        if (vfs_stat(abs_path, &de) < 0) {
            ret = -LINUX_ENOENT;
            break;
        }
        fill_linux_stat64((struct linux_stat64 *)(uintptr_t)arg2, &de);
        ret = 0;
        break;
    }

    case LINUX_SYS_fstatat: {
        char abs_path[VFS_MAX_PATH];
        struct vfs_dirent de;
        const char *upath = (const char *)(uintptr_t)arg2;
        if (upath == NULL || arg3 == 0) {
            ret = -LINUX_EFAULT;
            break;
        }
        if (upath[0] == '\0' && (arg4 & (uint32_t)LINUX_AT_EMPTY_PATH) != 0) {
            if (vfs_fstat((int)arg1, &de) < 0) {
                ret = -LINUX_EBADF;
                break;
            }
        } else {
            resolve_user_path(proc->cwd, upath, abs_path, sizeof(abs_path));
            if (vfs_stat(abs_path, &de) < 0) {
                ret = -LINUX_ENOENT;
                break;
            }
        }
        fill_linux_stat64((struct linux_stat64 *)(uintptr_t)arg3, &de);
        ret = 0;
        break;
    }

    case LINUX_SYS_fstat: {
        struct vfs_dirent de;
        if (arg2 == 0) {
            ret = -LINUX_EFAULT;
            break;
        }
        if (vfs_fstat((int)arg1, &de) < 0) {
            ret = -LINUX_EBADF;
            break;
        }
        fill_linux_stat((struct linux_stat *)(uintptr_t)arg2, &de);
        ret = 0;
        break;
    }

    case LINUX_SYS_fstat64: {
        struct vfs_dirent de;
        if (arg2 == 0) {
            ret = -LINUX_EFAULT;
            break;
        }
        if (vfs_fstat((int)arg1, &de) < 0) {
            ret = -LINUX_EBADF;
            break;
        }
        fill_linux_stat64((struct linux_stat64 *)(uintptr_t)arg2, &de);
        ret = 0;
        break;
    }

    case LINUX_SYS_statx: {
        char abs_path[VFS_MAX_PATH];
        struct vfs_dirent de;
        const char *upath = (const char *)(uintptr_t)arg2;
        if (arg5 == 0) {
            ret = -LINUX_EFAULT;
            break;
        }
        if ((upath == NULL || upath[0] == '\0') &&
            (arg3 & (uint32_t)LINUX_AT_EMPTY_PATH) != 0) {
            if (vfs_fstat((int)arg1, &de) < 0) {
                ret = -LINUX_EBADF;
                break;
            }
        } else {
            if (upath == NULL) {
                ret = -LINUX_EFAULT;
                break;
            }
            resolve_user_path(proc->cwd, upath, abs_path, sizeof(abs_path));
            if (vfs_stat(abs_path, &de) < 0) {
                ret = -LINUX_ENOENT;
                break;
            }
        }
        fill_linux_statx((struct linux_statx *)(uintptr_t)arg5, &de, arg4);
        ret = 0;
        break;
    }

    case LINUX_SYS_fsync: {
        uint32_t fl;
        ret = (vfs_get_flags((int)arg1, &fl) == 0) ? 0 : -LINUX_EBADF;
        break;
    }

    case LINUX_SYS_nanosleep: {
        const struct linux_timespec32 *req = (const struct linux_timespec32 *)(uintptr_t)arg1;
        struct linux_timespec32 *rem = (struct linux_timespec32 *)(uintptr_t)arg2;
        uint32_t ms;
        if (req == NULL || req->tv_sec < 0 || req->tv_nsec < 0) {
            ret = -LINUX_EINVAL;
            break;
        }
        ms = (uint32_t)req->tv_sec * 1000U + (uint32_t)req->tv_nsec / 1000000U;
        if (ms == 0 && req->tv_nsec > 0) {
            pit_udelay((uint32_t)req->tv_nsec / 1000U + 1U);
        } else if (ms > 0) {
            if (ms > 100U) {
                ms = 100U;
            }
            pit_mdelay(ms);
        }
        if (rem != NULL) {
            rem->tv_sec  = 0;
            rem->tv_nsec = 0;
        }
        ret = 0;
        break;
    }

    case LINUX_SYS_clock_gettime: {
        struct linux_timespec32 *tp = (struct linux_timespec32 *)(uintptr_t)arg2;
        uint32_t ticks = pit_get_ticks();
        if (tp == NULL) {
            ret = -LINUX_EFAULT;
            break;
        }
        tp->tv_sec  = (int32_t)(ticks / 1000U);
        tp->tv_nsec = (int32_t)((ticks % 1000U) * 1000000U);
        ret = 0;
        break;
    }

    case LINUX_SYS_clock_gettime64: {
        struct linux_timespec64 *tp = (struct linux_timespec64 *)(uintptr_t)arg2;
        uint32_t ticks = pit_get_ticks();
        if (tp == NULL) {
            ret = -LINUX_EFAULT;
            break;
        }
        tp->tv_sec_lo  = ticks / 1000U;
        tp->tv_sec_hi  = 0;
        tp->tv_nsec_lo = (ticks % 1000U) * 1000000U;
        tp->tv_nsec_hi = 0;
        ret = 0;
        break;
    }

    case LINUX_SYS_gettimeofday: {
        struct linux_timeval32 *tv = (struct linux_timeval32 *)(uintptr_t)arg1;
        uint32_t *tz = (uint32_t *)(uintptr_t)arg2;
        uint32_t ticks = pit_get_ticks();
        if (tv != NULL) {
            tv->tv_sec  = (int32_t)(ticks / 1000U);
            tv->tv_usec = (int32_t)((ticks % 1000U) * 1000U);
        }
        if (tz != NULL) {
            tz[0] = 0U;
            tz[1] = 0U;
        }
        ret = 0;
        break;
    }

    case LINUX_SYS_kill: {
        int target_pid = (int)arg1;
        int sig = (int)arg2;
        int i;
        int found = 0;
        if (sig < 0 || sig > 64) {
            ret = -LINUX_EINVAL;
            break;
        }
        if (target_pid == proc->pid && sig != 0) {
            handle_process_exit(128 + sig);
            ret = 0;
            break;
        }
        for (i = 0; i < MAX_PROCS; i++) {
            if (proc_table[i].state != PROC_UNUSED && proc_table[i].pid == target_pid) {
                found = 1;
                if (sig != 0 && proc_table[i].state == PROC_RUNNING) {
                    proc_table[i].state     = PROC_ZOMBIE;
                    proc_table[i].exit_code = 128 + sig;
                }
                break;
            }
        }
        ret = found ? 0 : -LINUX_ESRCH;
        break;
    }

    case LINUX_SYS_getpid:
        ret = proc->pid;
        break;

    case LINUX_SYS_getuid:
    case LINUX_SYS_getuid32:
    case LINUX_SYS_geteuid:
    case LINUX_SYS_geteuid32:
    case LINUX_SYS_getgid:
    case LINUX_SYS_getgid32:
    case LINUX_SYS_getegid:
    case LINUX_SYS_getegid32:
        ret = 0;
        break;

    case LINUX_SYS_brk: {
        uint32_t new_brk = arg1;
        if (new_brk >= proc->brk_start && new_brk <= LINUX_BRK_MAX) {
            if (new_brk > proc->brk_cur) {
                memset((void *)(uintptr_t)proc->brk_cur, 0, new_brk - proc->brk_cur);
            }
            proc->brk_cur = new_brk;
        }
        ret = (int32_t)proc->brk_cur;
        break;
    }

    case LINUX_SYS_mmap: {
        const uint32_t *margs = (const uint32_t *)(uintptr_t)arg1;
        if (margs == NULL) {
            ret = -LINUX_EFAULT;
            break;
        }
        ret = do_mmap(proc, margs[0], margs[1], margs[2], margs[3], (int)margs[4], margs[5]);
        break;
    }

    case LINUX_SYS_mmap2:
        ret = do_mmap(proc, arg1, arg2, arg3, arg4, (int)arg5, arg6 * 4096U);
        break;

    case LINUX_SYS_munmap: {
        uint32_t addr = arg1;
        uint32_t len  = arg2;
        uint32_t aligned_len;
        if ((addr & 0xFFFU) != 0 || len == 0) {
            ret = -LINUX_EINVAL;
            break;
        }
        aligned_len = page_align_up(len);
        if (addr + aligned_len == proc->mmap_cur && addr >= LINUX_MMAP_BASE) {
            proc->mmap_cur = addr;
        }
        ret = 0;
        break;
    }

    case LINUX_SYS_fork:
    case LINUX_SYS_vfork:
        ret = do_clone(proc, regs, 0U);
        break;

    case LINUX_SYS_clone:
        ret = do_clone(proc, regs, arg2);
        break;

    case LINUX_SYS_execve:
        ret = do_execve(proc, (const char *)(uintptr_t)arg1,
                        (char *const *)(uintptr_t)arg2,
                        (char *const *)(uintptr_t)arg3);
        break;

    case LINUX_SYS_waitpid:
    case LINUX_SYS_wait4: {
        int target_pid = (int)arg1;
        int *wstatus = (int *)(uintptr_t)arg2;
        void *rusage = (void *)(uintptr_t)arg4;
        int i;
        int found_idx = -1;

        for (i = 0; i < MAX_PROCS; i++) {
            if (proc_table[i].state == PROC_ZOMBIE && proc_table[i].ppid == proc->pid) {
                if (target_pid <= 0 || proc_table[i].pid == target_pid) {
                    found_idx = i;
                    break;
                }
            }
        }
        if (found_idx < 0) {
            ret = -LINUX_ECHILD;
            break;
        }
        ret = proc_table[found_idx].pid;
        if (wstatus != NULL) {
            *wstatus = (proc_table[found_idx].exit_code & 0xFF) << 8;
        }
        if (nr == LINUX_SYS_wait4 && rusage != NULL) {
            memset(rusage, 0, 72);
        }
        proc_table[found_idx].state = PROC_UNUSED;
        break;
    }

    case LINUX_SYS_waitid: {
        int idtype = (int)arg1;
        int id = (int)arg2;
        struct linux_siginfo *infop = (struct linux_siginfo *)(uintptr_t)arg3;
        int options = (int)arg4;
        void *rusage = (void *)(uintptr_t)arg5;
        int i;
        int found_idx = -1;

        for (i = 0; i < MAX_PROCS; i++) {
            if (proc_table[i].state == PROC_ZOMBIE && proc_table[i].ppid == proc->pid) {
                if (idtype == LINUX_P_ALL ||
                    (idtype == LINUX_P_PID && proc_table[i].pid == id) ||
                    idtype == LINUX_P_PGID) {
                    found_idx = i;
                    break;
                }
            }
        }
        if (found_idx < 0) {
            ret = -LINUX_ECHILD;
            break;
        }
        if (infop != NULL) {
            memset(infop, 0, sizeof(*infop));
            infop->si_signo  = 17; /* SIGCHLD */
            infop->si_code   = 1;  /* CLD_EXITED */
            infop->si_pid    = proc_table[found_idx].pid;
            infop->si_uid    = 0U;
            infop->si_status = proc_table[found_idx].exit_code;
        }
        if (rusage != NULL) {
            memset(rusage, 0, 72);
        }
        if ((options & LINUX_WNOWAIT) == 0) {
            proc_table[found_idx].state = PROC_UNUSED;
        }
        ret = 0;
        break;
    }

    case LINUX_SYS_set_thread_area:
        ret = 0;
        break;

    case LINUX_SYS_set_tid_address:
        ret = proc->pid;
        break;

    default:
        kprintf("[SYSCALL] Unimplemented Linux syscall #%u (ebx=0x%x, ecx=0x%x, edx=0x%x)\n",
                nr, arg1, arg2, arg3);
        ret = -LINUX_ENOSYS;
        break;
    }

    regs->eax = (uint32_t)ret;
}
