/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix - Standalone Ring-3 Bourne-style Interactive Shell (/bin/sh, /mnt/SH.ELF)
 * Uses strictly Linux i386 int $0x80 syscalls (zero kernel symbol dependencies).
 */

typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef int            int32_t;
typedef uint32_t       size_t;

#define NULL ((void *)0)

extern int32_t __oksinix_int80(uint32_t nr, uint32_t a1, uint32_t a2,
                               uint32_t a3, uint32_t a4, uint32_t a5);

/* Linux i386 syscall numbers */
#define SYS_exit          1U
#define SYS_fork          2U
#define SYS_read          3U
#define SYS_write         4U
#define SYS_open          5U
#define SYS_close         6U
#define SYS_waitpid       7U
#define SYS_execve        11U
#define SYS_chdir         12U
#define SYS_mount         21U
#define SYS_kill          37U
#define SYS_mkdir         39U
#define SYS_umount2       52U
#define SYS_reboot        88U
#define SYS_nanosleep     162U
#define SYS_getcwd        183U
#define SYS_exit_group    252U
#define SYS_clock_gettime 265U
#define SYS_oksinix_ctl   500U

/* OKSINIX_SYS_ctl operations */
#define OKS_CTL_SYNC_USB    1U
#define OKS_CTL_STATUS_BAR  2U
#define OKS_CTL_LSUSB       3U
#define OKS_CTL_MEMMAP      4U
#define OKS_CTL_NET         5U
#define OKS_CTL_KEYMAP      6U
#define OKS_CTL_MOUNT_LIST  7U
#define OKS_CTL_MOUNT_ALL   8U
#define OKS_CTL_PS          9U
#define OKS_CTL_SPAWN       10U
#define OKS_CTL_SCHEDTEST   11U
#define OKS_CTL_PROTTEST    12U
#define OKS_CTL_READDIR     13U
#define OKS_CTL_STAT        14U

#define O_RDONLY   0x0000U
#define O_WRONLY   0x0001U
#define O_CREAT    0x0040U
#define O_TRUNC    0x0200U

#define VFS_TYPE_DIR      1U
#define VFS_TYPE_FILE     2U
#define VFS_TYPE_CHARDEV  3U
#define VFS_TYPE_BLOCKDEV 4U

#define SH_MAX_PATH 256
#define SH_LINE_MAX 128
#define SH_MAX_ARGS 16

struct sh_timespec {
    int32_t tv_sec;
    int32_t tv_nsec;
};

struct sh_dirent {
    char     name[128];
    uint32_t inode;
    uint32_t type;
    uint32_t size;
    uint16_t rdev_major;
    uint16_t rdev_minor;
};

struct sh_mouse_state {
    int32_t  x;
    int32_t  y;
    int32_t  dx;
    int32_t  dy;
    uint8_t  buttons;
    uint32_t packets;
};

int main(int argc, char **argv);

static char cwd[SH_MAX_PATH] = "/";
static int  sh_serial_fd = -1;

static int sys_open(const char *path, uint32_t flags, uint32_t mode)
{
    return (int)__oksinix_int80(SYS_open, (uint32_t)path, flags, mode, 0U, 0U);
}

static int32_t sys_read(int fd, void *buf, uint32_t len)
{
    return __oksinix_int80(SYS_read, (uint32_t)fd, (uint32_t)buf, len, 0U, 0U);
}

static int32_t sys_write(int fd, const void *buf, uint32_t len)
{
    return __oksinix_int80(SYS_write, (uint32_t)fd, (uint32_t)buf, len, 0U, 0U);
}

static int sys_close(int fd)
{
    return (int)__oksinix_int80(SYS_close, (uint32_t)fd, 0U, 0U, 0U, 0U);
}

static int sys_fork(void)
{
    return (int)__oksinix_int80(SYS_fork, 0U, 0U, 0U, 0U, 0U);
}

static int sys_execve(const char *path, char *const argv[], char *const envp[])
{
    return (int)__oksinix_int80(SYS_execve, (uint32_t)path, (uint32_t)argv,
                                (uint32_t)envp, 0U, 0U);
}

static int sys_waitpid(int pid, int *status, uint32_t options)
{
    return (int)__oksinix_int80(SYS_waitpid, (uint32_t)pid, (uint32_t)status,
                                options, 0U, 0U);
}

static void sys_exit(int code)
{
    (void)__oksinix_int80(SYS_exit_group, (uint32_t)code, 0U, 0U, 0U, 0U);
    for (;;) {
    }
}

static int sys_chdir(const char *path)
{
    return (int)__oksinix_int80(SYS_chdir, (uint32_t)path, 0U, 0U, 0U, 0U);
}

static int sys_getcwd(char *buf, uint32_t len)
{
    return (int)__oksinix_int80(SYS_getcwd, (uint32_t)buf, len, 0U, 0U, 0U);
}

static int sys_mkdir(const char *path, uint32_t mode)
{
    return (int)__oksinix_int80(SYS_mkdir, (uint32_t)path, mode, 0U, 0U, 0U);
}

static int sys_mount(const char *src, const char *dst)
{
    return (int)__oksinix_int80(SYS_mount, (uint32_t)src, (uint32_t)dst, 0U, 0U, 0U);
}

static int sys_umount(const char *dst)
{
    return (int)__oksinix_int80(SYS_umount2, (uint32_t)dst, 0U, 0U, 0U, 0U);
}

static int sys_kill(int pid, int sig)
{
    return (int)__oksinix_int80(SYS_kill, (uint32_t)pid, (uint32_t)sig, 0U, 0U, 0U);
}

static void sys_reboot(void)
{
    (void)__oksinix_int80(SYS_reboot, 0U, 0U, 0U, 0U, 0U);
}

static void sys_clock_gettime(struct sh_timespec *ts)
{
    (void)__oksinix_int80(SYS_clock_gettime, 0U, (uint32_t)ts, 0U, 0U, 0U);
}

static void sys_sleep_ms(int32_t ms)
{
    struct sh_timespec ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = ms * 1000000;
    (void)__oksinix_int80(SYS_nanosleep, (uint32_t)&ts, 0U, 0U, 0U, 0U);
}

static int32_t sys_ctl(uint32_t op, uint32_t a1, uint32_t a2, uint32_t a3)
{
    return __oksinix_int80(SYS_oksinix_ctl, op, a1, a2, a3, 0U);
}

static size_t sh_strlen(const char *s)
{
    size_t n = 0U;
    if (s == NULL) {
        return 0U;
    }
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int sh_strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

static int sh_strncmp(const char *a, const char *b, size_t n)
{
    while (n > 0U && *a != '\0' && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0U) {
        return 0;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

static void sh_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- > 0U) {
        *d++ = *s++;
    }
}

static void sh_memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n-- > 0U) {
        *d++ = (uint8_t)c;
    }
}

static void sh_puts(const char *s)
{
    (void)sys_write(1, s, (uint32_t)sh_strlen(s));
}

static void sh_putc(char c)
{
    (void)sys_write(1, &c, 1U);
}

static void sh_flush(void)
{
    (void)sys_write(1, "\x1b[?25h", 6U);
}

static void sh_print_uint(uint32_t val)
{
    char buf[16];
    int i = 0;
    if (val == 0U) {
        sh_putc('0');
        return;
    }
    while (val > 0U) {
        buf[i++] = (char)('0' + (val % 10U));
        val /= 10U;
    }
    while (i > 0) {
        sh_putc(buf[--i]);
    }
}

static int sh_parse_int(const char *s)
{
    int val = 0;
    if (s == NULL || *s == '\0') {
        return -1;
    }
    while (*s != '\0') {
        if (*s < '0' || *s > '9') {
            return -1;
        }
        val = val * 10 + (int)(*s - '0');
        s++;
    }
    return val;
}

static void make_abs_path(const char *input, char *out, size_t out_sz)
{
    size_t i = 0U;
    size_t j = 0U;

    if (input == NULL || input[0] == '\0') {
        while (cwd[i] != '\0' && i + 1U < out_sz) {
            out[i] = cwd[i];
            i++;
        }
        out[i] = '\0';
        return;
    }

    if (input[0] == '/') {
        while (input[i] != '\0' && i + 1U < out_sz) {
            out[i] = input[i];
            i++;
        }
        out[i] = '\0';
        return;
    }

    while (cwd[i] != '\0' && i + 1U < out_sz) {
        out[i] = cwd[i];
        i++;
    }
    if (i > 0U && out[i - 1U] != '/' && i + 1U < out_sz) {
        out[i++] = '/';
    }
    while (input[j] != '\0' && i + 1U < out_sz) {
        out[i++] = input[j++];
    }
    out[i] = '\0';
}

static int tokenize_line(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *p = line;

    while (*p != '\0' && argc < max_args - 1) {
        while (*p == ' ' || *p == '\t') {
            *p++ = '\0';
        }
        if (*p == '\0') {
            break;
        }
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
    }
    argv[argc] = NULL;
    return argc;
}

static int sh_run_child(const char *path, char *const argv[], const char *status_text)
{
    int pid;
    int status = 0;

    if (status_text != NULL) {
        (void)sys_ctl(OKS_CTL_STATUS_BAR, (uint32_t)status_text, 0U, 0U);
    }
    pid = sys_fork();
    if (pid < 0) {
        if (status_text != NULL) {
            (void)sys_ctl(OKS_CTL_STATUS_BAR, (uint32_t)"PID 1: /bin/init -> PID 2: /bin/sh", 0U, 0U);
        }
        return -1;
    }
    if (pid == 0) {
        (void)sys_execve(path, argv, NULL);
        sys_exit(127);
    }
    (void)sys_waitpid(pid, &status, 0U);
    if (status_text != NULL) {
        (void)sys_ctl(OKS_CTL_STATUS_BAR, (uint32_t)"PID 1: /bin/init -> PID 2: /bin/sh", 0U, 0U);
    }
    sh_flush();
    return (status >> 8) & 0xFF;
}

static void cmd_ls(int argc, char **argv)
{
    char path[SH_MAX_PATH];
    struct sh_dirent de;
    uint32_t idx = 0U;

    make_abs_path((argc > 1) ? argv[1] : cwd, path, sizeof(path));

    if (sys_ctl(OKS_CTL_STAT, (uint32_t)path, (uint32_t)&de, 0U) < 0) {
        sh_puts("ls: cannot access ");
        sh_puts(path);
        sh_putc('\n');
        return;
    }
    if (de.type != VFS_TYPE_DIR) {
        sh_puts(path);
        sh_putc('\n');
        return;
    }

    while (sys_ctl(OKS_CTL_READDIR, (uint32_t)path, idx, (uint32_t)&de) == 0) {
        if (de.type == VFS_TYPE_DIR) {
            sh_puts("drwxr-xr-x  ");
            sh_puts(de.name);
            sh_puts("/\n");
        } else if (de.type == VFS_TYPE_CHARDEV) {
            sh_puts("crw-rw-rw-  ");
            sh_puts(de.name);
            sh_puts(" (");
            sh_print_uint(de.rdev_major);
            sh_putc(',');
            sh_print_uint(de.rdev_minor);
            sh_puts(")\n");
        } else if (de.type == VFS_TYPE_BLOCKDEV) {
            sh_puts("brw-rw----  ");
            sh_puts(de.name);
            sh_puts(" (");
            sh_print_uint(de.rdev_major);
            sh_putc(',');
            sh_print_uint(de.rdev_minor);
            sh_puts(") ");
            sh_print_uint(de.size);
            sh_puts(" B\n");
        } else {
            sh_puts("-rw-r--r--  ");
            sh_puts(de.name);
            sh_puts(" (");
            sh_print_uint(de.size);
            sh_puts(" B)\n");
        }
        idx++;
    }
}

static void cmd_cat(int argc, char **argv)
{
    char path[SH_MAX_PATH];
    uint8_t buf[64];
    int fd;
    int32_t n;

    if (argc < 2) {
        sh_puts("usage: cat <path>\n");
        return;
    }

    make_abs_path(argv[1], path, sizeof(path));
    fd = sys_open(path, O_RDONLY, 0U);
    if (fd < 0) {
        sh_puts("cat: ");
        sh_puts(path);
        sh_puts(": No such file or device\n");
        return;
    }

    while ((n = sys_read(fd, buf, (uint32_t)sizeof(buf))) > 0) {
        int32_t i;
        for (i = 0; i < n; i++) {
            char c = (char)buf[i];
            if ((c >= 32 && c <= 126) || c == '\n' || c == '\t') {
                sh_putc(c);
            } else {
                sh_putc('.');
            }
        }
        if (sh_strcmp(path, "/dev/zero") == 0 ||
            sh_strcmp(path, "/dev/mouse") == 0 ||
            sh_strcmp(path, "/dev/fb0") == 0 ||
            sh_strncmp(path, "/dev/sd", 7U) == 0) {
            sh_putc('\n');
            break;
        }
    }
    (void)sys_close(fd);
}

static void cmd_cp(int argc, char **argv)
{
    char src_path[SH_MAX_PATH];
    char dst_path[SH_MAX_PATH];
    struct sh_dirent src_de;
    struct sh_dirent dst_de;
    uint8_t buf[128];
    int src_fd;
    int dst_fd;
    int32_t n;

    if (argc < 3) {
        sh_puts("usage: cp <src> <dst>\n");
        return;
    }

    make_abs_path(argv[1], src_path, sizeof(src_path));
    make_abs_path(argv[2], dst_path, sizeof(dst_path));

    if (sys_ctl(OKS_CTL_STAT, (uint32_t)src_path, (uint32_t)&src_de, 0U) < 0) {
        sh_puts("cp: cannot stat ");
        sh_puts(src_path);
        sh_puts(": No such file or directory\n");
        return;
    }
    if (src_de.type == VFS_TYPE_DIR) {
        sh_puts("cp: omitting directory ");
        sh_puts(src_path);
        sh_putc('\n');
        return;
    }

    if (sys_ctl(OKS_CTL_STAT, (uint32_t)dst_path, (uint32_t)&dst_de, 0U) == 0 &&
        dst_de.type == VFS_TYPE_DIR) {
        const char *base = src_path;
        const char *p = src_path;
        size_t dlen = sh_strlen(dst_path);
        size_t blen;

        while (*p != '\0') {
            if (*p == '/' && *(p + 1) != '\0') {
                base = p + 1;
            }
            p++;
        }
        blen = sh_strlen(base);
        if (dlen > 0U && dst_path[dlen - 1U] != '/' && dlen + 1U < sizeof(dst_path)) {
            dst_path[dlen++] = '/';
            dst_path[dlen] = '\0';
        }
        if (dlen + blen < sizeof(dst_path)) {
            sh_memcpy(&dst_path[dlen], base, blen + 1U);
        }
    }

    if (sh_strcmp(src_path, dst_path) == 0) {
        sh_puts("cp: ");
        sh_puts(src_path);
        sh_puts(" and ");
        sh_puts(dst_path);
        sh_puts(" are the same file\n");
        return;
    }

    src_fd = sys_open(src_path, O_RDONLY, 0U);
    if (src_fd < 0) {
        sh_puts("cp: cannot open ");
        sh_puts(src_path);
        sh_putc('\n');
        return;
    }

    dst_fd = sys_open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644U);
    if (dst_fd < 0) {
        (void)sys_close(src_fd);
        sh_puts("cp: cannot create ");
        sh_puts(dst_path);
        sh_putc('\n');
        return;
    }

    while ((n = sys_read(src_fd, buf, (uint32_t)sizeof(buf))) > 0) {
        if (sys_write(dst_fd, buf, (uint32_t)n) != n) {
            sh_puts("cp: write error on ");
            sh_puts(dst_path);
            sh_putc('\n');
            break;
        }
        if (sh_strcmp(src_path, "/dev/zero") == 0 ||
            sh_strcmp(src_path, "/dev/mouse") == 0 ||
            sh_strcmp(src_path, "/dev/fb0") == 0 ||
            sh_strncmp(src_path, "/dev/sd", 7U) == 0) {
            break;
        }
    }

    (void)sys_close(dst_fd);
    (void)sys_close(src_fd);
}

static void cmd_echo(int argc, char **argv)
{
    int i;
    int redir_idx = -1;
    int fd = 1;

    for (i = 1; i < argc; i++) {
        if (sh_strcmp(argv[i], ">") == 0 && i + 1 < argc) {
            redir_idx = i;
            break;
        }
    }

    if (redir_idx > 0) {
        char path[SH_MAX_PATH];
        make_abs_path(argv[redir_idx + 1], path, sizeof(path));
        fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644U);
        if (fd < 0) {
            sh_puts("echo: cannot open ");
            sh_puts(path);
            sh_putc('\n');
            return;
        }
    }

    {
        int end = (redir_idx > 0) ? redir_idx : argc;
        for (i = 1; i < end; i++) {
            if (i > 1) {
                (void)sys_write(fd, " ", 1U);
            }
            (void)sys_write(fd, argv[i], (uint32_t)sh_strlen(argv[i]));
        }
        (void)sys_write(fd, "\n", 1U);
    }

    if (fd != 1) {
        (void)sys_close(fd);
    }
}

static void cmd_stat(int argc, char **argv)
{
    char path[SH_MAX_PATH];
    struct sh_dirent de;

    if (argc < 2) {
        sh_puts("usage: stat <path>\n");
        return;
    }

    make_abs_path(argv[1], path, sizeof(path));
    if (sys_ctl(OKS_CTL_STAT, (uint32_t)path, (uint32_t)&de, 0U) < 0) {
        sh_puts("stat: cannot stat ");
        sh_puts(path);
        sh_putc('\n');
        return;
    }

    sh_puts("  File: ");
    sh_puts(path);
    sh_puts("\n  Inode: ");
    sh_print_uint(de.inode);
    sh_puts("  Type: ");
    if (de.type == VFS_TYPE_DIR) {
        sh_puts("directory\n");
    } else if (de.type == VFS_TYPE_CHARDEV) {
        sh_puts("character device (");
        sh_print_uint(de.rdev_major);
        sh_putc(',');
        sh_print_uint(de.rdev_minor);
        sh_puts(")\n");
    } else if (de.type == VFS_TYPE_BLOCKDEV) {
        sh_puts("block device (");
        sh_print_uint(de.rdev_major);
        sh_putc(',');
        sh_print_uint(de.rdev_minor);
        sh_puts(")  Size: ");
        sh_print_uint(de.size);
        sh_puts(" bytes\n");
    } else {
        sh_puts("regular file  Size: ");
        sh_print_uint(de.size);
        sh_puts(" bytes\n");
    }
}

static void cmd_cd(int argc, char **argv)
{
    const char *target = (argc > 1) ? argv[1] : "/";
    if (sys_chdir(target) < 0) {
        sh_puts("cd: not a directory: ");
        sh_puts(target);
        sh_putc('\n');
        return;
    }
    if (sys_getcwd(cwd, (uint32_t)sizeof(cwd)) <= 0) {
        cwd[0] = '/';
        cwd[1] = '\0';
    }
}

static void sh_serial_log_exec(const char *line)
{
    if (sh_serial_fd < 0) {
        return;
    }
    (void)sys_write(sh_serial_fd, "[SH] exec=\"", 11U);
    (void)sys_write(sh_serial_fd, line, (uint32_t)sh_strlen(line));
    (void)sys_write(sh_serial_fd, "\"\n", 2U);
}

static void sh_execute_line(char *line)
{
    char *argv[SH_MAX_ARGS];
    int argc = tokenize_line(line, argv, SH_MAX_ARGS);

    if (argc == 0) {
        return;
    }

    if (sh_strcmp(argv[0], "help") == 0) {
        sh_puts("Built-in commands: help ls cd pwd cat cp echo stat mkdir mount umount lsusb net telnet memmap acpi keymap ps kill spawn schedtest prottest uname uptime clear vi cc ccia as ld shutdown\n");
    } else if (sh_strcmp(argv[0], "net") == 0) {
        (void)sys_ctl(OKS_CTL_NET, (uint32_t)argc, (uint32_t)argv, 0U);
    } else if (sh_strcmp(argv[0], "telnet") == 0 || sh_strcmp(argv[0], "/bin/telnet") == 0) {
        argv[0] = "/mnt/TELNET.ELF";
        if (sh_run_child("/mnt/TELNET.ELF", argv, "PID 2: /mnt/TELNET.ELF") == 127) {
            sh_puts("telnet: /mnt/TELNET.ELF not found\n");
        }
    } else if (sh_strcmp(argv[0], "shutdown") == 0 || sh_strcmp(argv[0], "poweroff") == 0) {
        sh_puts("Shutting down system via ACPI...\n");
        sh_flush();
        sys_reboot();
    } else if (sh_strcmp(argv[0], "memmap") == 0 || sh_strcmp(argv[0], "acpi") == 0) {
        (void)sys_ctl(OKS_CTL_MEMMAP, 0U, 0U, 0U);
    } else if (sh_strcmp(argv[0], "cp") == 0) {
        cmd_cp(argc, argv);
    } else if (sh_strcmp(argv[0], "keymap") == 0 || sh_strcmp(argv[0], "loadkeys") == 0) {
        (void)sys_ctl(OKS_CTL_KEYMAP, (argc >= 2) ? (uint32_t)argv[1] : 0U, 0U, 0U);
    } else if (sh_strcmp(argv[0], "uptime") == 0) {
        struct sh_timespec ts;
        uint32_t ticks;
        sys_clock_gettime(&ts);
        ticks = (uint32_t)ts.tv_sec * 1000U + (uint32_t)ts.tv_nsec / 1000000U;
        sh_puts("up ");
        sh_print_uint(ticks / 1000U);
        sh_putc('.');
        sh_print_uint((ticks % 1000U) / 100U);
        sh_puts("s (");
        sh_print_uint(ticks);
        sh_puts(" PIT ticks @ 1000 Hz)\n");
    } else if (sh_strcmp(argv[0], "mount") == 0) {
        if (argc == 2 && sh_strcmp(argv[1], "-a") == 0) {
            (void)sys_ctl(OKS_CTL_MOUNT_ALL, 0U, 0U, 0U);
        } else if (argc >= 3) {
            if (sys_mount(argv[1], argv[2]) < 0) {
                sh_puts("mount: failed to mount ");
                sh_puts(argv[1]);
                sh_puts(" on ");
                sh_puts(argv[2]);
                sh_putc('\n');
            }
        } else {
            (void)sys_ctl(OKS_CTL_MOUNT_LIST, 0U, 0U, 0U);
        }
    } else if (sh_strcmp(argv[0], "umount") == 0) {
        if (argc < 2) {
            sh_puts("usage: umount <mountpoint>\n");
        } else if (sys_umount(argv[1]) < 0) {
            sh_puts("umount: ");
            sh_puts(argv[1]);
            sh_puts(": not mounted\n");
        }
    } else if (sh_strcmp(argv[0], "lsusb") == 0) {
        (void)sys_ctl(OKS_CTL_LSUSB, 0U, 0U, 0U);
    } else if (sh_strcmp(argv[0], "ls") == 0) {
        cmd_ls(argc, argv);
    } else if (sh_strcmp(argv[0], "cd") == 0) {
        cmd_cd(argc, argv);
    } else if (sh_strcmp(argv[0], "pwd") == 0) {
        sh_puts(cwd);
        sh_putc('\n');
    } else if (sh_strcmp(argv[0], "cat") == 0) {
        cmd_cat(argc, argv);
    } else if (sh_strcmp(argv[0], "echo") == 0) {
        cmd_echo(argc, argv);
    } else if (sh_strcmp(argv[0], "stat") == 0) {
        cmd_stat(argc, argv);
    } else if (sh_strcmp(argv[0], "mkdir") == 0) {
        if (argc < 2) {
            sh_puts("usage: mkdir <path>\n");
        } else {
            char path[SH_MAX_PATH];
            make_abs_path(argv[1], path, sizeof(path));
            if (sys_mkdir(path, 0755U) < 0) {
                sh_puts("mkdir: failed to create directory\n");
            }
        }
    } else if (sh_strcmp(argv[0], "ps") == 0) {
        (void)sys_ctl(OKS_CTL_PS, 0U, 0U, 0U);
    } else if (sh_strcmp(argv[0], "kill") == 0) {
        int pid = (argc >= 2) ? sh_parse_int(argv[1]) : -1;
        if (pid < 0) {
            sh_puts("usage: kill <pid>\n");
        } else if (sys_kill(pid, 9) < 0) {
            sh_puts("kill: cannot terminate PID ");
            sh_print_uint((uint32_t)pid);
            sh_putc('\n');
        } else {
            sh_puts("Terminated PID ");
            sh_print_uint((uint32_t)pid);
            sh_putc('\n');
        }
    } else if (sh_strcmp(argv[0], "spawn") == 0) {
        uint32_t kind = (argc >= 2 && sh_strcmp(argv[1], "sleeper") == 0) ? 1U : 0U;
        int32_t pid = sys_ctl(OKS_CTL_SPAWN, kind, 0U, 0U);
        if (pid < 0) {
            sh_puts("spawn: task table full\n");
        } else {
            sh_puts("Spawned background task PID ");
            sh_print_uint((uint32_t)pid);
            sh_putc('\n');
        }
    } else if (sh_strcmp(argv[0], "schedtest") == 0) {
        if (sys_ctl(OKS_CTL_SCHEDTEST, 0U, 0U, 0U) == 0) {
            sh_puts("Preemptive scheduler test PASSED.\n");
        } else {
            sh_puts("Preemptive scheduler test FAILED.\n");
        }
    } else if (sh_strcmp(argv[0], "prottest") == 0) {
        if (sys_ctl(OKS_CTL_PROTTEST, 0U, 0U, 0U) == 0) {
            sh_puts("Hardware task protection test (Ring 3 / paging / TSS) PASSED.\n");
        } else {
            sh_puts("Hardware task protection test FAILED.\n");
        }
    } else if (sh_strcmp(argv[0], "uname") == 0) {
        sh_puts("Oksinix 0.2 i386 C90 (CC0 Public Domain)\n");
    } else if (sh_strcmp(argv[0], "clear") == 0) {
        (void)sys_write(1, "\x1b[2J\x1b[H\x1b[?25h", 13U);
    } else if (sh_strcmp(argv[0], "vi") == 0 || sh_strcmp(argv[0], "/bin/vi") == 0) {
        char path[SH_MAX_PATH];
        char *vi_argv[3];
        vi_argv[0] = "/mnt/VI.ELF";
        if (argc > 1) {
            make_abs_path(argv[1], path, sizeof(path));
            vi_argv[1] = path;
            vi_argv[2] = NULL;
        } else {
            vi_argv[1] = NULL;
        }
        if (sh_run_child("/mnt/VI.ELF", vi_argv, "PID 2: /mnt/VI.ELF (editing)") == 127) {
            sh_puts("vi: /mnt/VI.ELF not found\n");
        }
    } else if (sh_strcmp(argv[0], "cc") == 0 || sh_strcmp(argv[0], "/bin/cc") == 0) {
        char path[SH_MAX_PATH];
        char *cc_argv[3];
        if (argc > 1) {
            make_abs_path(argv[1], path, sizeof(path));
            cc_argv[0] = "/mnt/CC.ELF";
            cc_argv[1] = path;
            cc_argv[2] = NULL;
            if (sh_run_child("/mnt/CC.ELF", cc_argv, NULL) == 127) {
                sh_puts("cc: /mnt/CC.ELF not found\n");
            }
        } else {
            sh_puts("usage: cc <file.c>\n");
        }
    } else if (sh_strcmp(argv[0], "as") == 0 || sh_strcmp(argv[0], "/bin/as") == 0) {
        argv[0] = "/mnt/AS.ELF";
        if (sh_run_child("/mnt/AS.ELF", argv, NULL) == 127) {
            sh_puts("as: /mnt/AS.ELF not found\n");
        }
    } else if (sh_strcmp(argv[0], "ld") == 0 || sh_strcmp(argv[0], "/bin/ld") == 0) {
        argv[0] = "/mnt/LD.ELF";
        if (sh_run_child("/mnt/LD.ELF", argv, NULL) == 127) {
            sh_puts("ld: /mnt/LD.ELF not found\n");
        }
    } else if (sh_strcmp(argv[0], "ccia") == 0 || sh_strcmp(argv[0], "/bin/ccia") == 0) {
        argv[0] = "/mnt/CCIA";
        if (sh_run_child("/mnt/CCIA", argv, NULL) == 127) {
            sh_puts("ccia: /mnt/CCIA not found\n");
        }
    } else if (sh_strcmp(argv[0], "exit") == 0) {
        sys_exit(0);
    } else {
        char path[SH_MAX_PATH];
        struct sh_dirent de;
        make_abs_path(argv[0], path, sizeof(path));
        if (sys_ctl(OKS_CTL_STAT, (uint32_t)path, (uint32_t)&de, 0U) == 0 &&
            de.type == VFS_TYPE_FILE) {
            char *orig_arg0 = argv[0];
            argv[0] = path;
            if (sh_run_child(path, argv, NULL) == 127) {
                sh_puts("sh: cannot execute binary: ");
                sh_puts(orig_arg0);
                sh_putc('\n');
            }
        } else {
            sh_puts("sh: unknown command: ");
            sh_puts(argv[0]);
            sh_putc('\n');
        }
    }
}

static void sh_print_prompt(void)
{
    sh_puts("oksinix:");
    sh_puts(cwd);
    sh_puts("# ");
    sh_flush();
}

int main(int argc, char **argv)
{
    char line[SH_LINE_MAX];
    uint32_t line_len = 0U;
    struct sh_mouse_state last_mouse;
    struct sh_mouse_state cur_mouse;
    int mouse_fd;

    if (sys_getcwd(cwd, (uint32_t)sizeof(cwd)) <= 0) {
        cwd[0] = '/';
        cwd[1] = '\0';
    }

    if (argc > 2 && sh_strcmp(argv[1], "-c") == 0) {
        char cmd_copy[SH_LINE_MAX];
        size_t len = sh_strlen(argv[2]);
        if (len >= SH_LINE_MAX) {
            len = SH_LINE_MAX - 1U;
        }
        sh_memcpy(cmd_copy, argv[2], len);
        cmd_copy[len] = '\0';
        sh_execute_line(cmd_copy);
        sh_flush();
        return 0;
    }

    sh_serial_fd = sys_open("/dev/ttyS0", O_WRONLY, 0U);
    mouse_fd     = sys_open("/dev/mouse", O_RDONLY, 0U);

    sh_puts("[/bin/sh] Interactive shell ready. Type 'help' for commands.\n");
    sh_memset(&last_mouse, 0, sizeof(last_mouse));
    if (mouse_fd >= 0) {
        (void)sys_read(mouse_fd, &last_mouse, (uint32_t)sizeof(last_mouse));
    }
    sh_print_prompt();

    for (;;) {
        uint8_t ch;
        int need_redraw = 0;

        while (sys_read(0, &ch, 1U) == 1) {
            if (ch == '\n') {
                sh_putc('\n');
                line[line_len] = '\0';
                sh_serial_log_exec(line);
                sh_execute_line(line);
                line_len = 0U;
                sh_print_prompt();
            } else if (ch == '\b' || ch == 127) {
                if (line_len > 0U) {
                    line_len--;
                    sh_putc('\b');
                    need_redraw = 1;
                }
            } else if (ch >= 32 && ch <= 126 && line_len + 1U < SH_LINE_MAX) {
                line[line_len++] = (char)ch;
                sh_putc((char)ch);
                need_redraw = 1;
            }
        }

        if (mouse_fd >= 0 &&
            sys_read(mouse_fd, &cur_mouse, (uint32_t)sizeof(cur_mouse)) == (int32_t)sizeof(cur_mouse)) {
            if (cur_mouse.x != last_mouse.x ||
                cur_mouse.y != last_mouse.y ||
                cur_mouse.buttons != last_mouse.buttons ||
                cur_mouse.packets != last_mouse.packets) {
                last_mouse = cur_mouse;
                need_redraw = 1;
            }
        }

        if (need_redraw) {
            sh_flush();
        } else {
            (void)sys_ctl(OKS_CTL_SYNC_USB, 0U, 0U, 0U);
            sys_sleep_ms(2);
        }
    }
}
