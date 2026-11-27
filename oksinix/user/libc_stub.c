/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix - Minimal Ring-3 user-space PIC libc.so.6 for dynamically linked Linux i386 ELFs
 * All OS services go strictly through Linux i386 int $0x80 syscalls (zero kernel .text calls).
 */

typedef unsigned int u32_t;
typedef int          s32_t;

extern s32_t __oksinix_int80(u32_t nr, u32_t a1, u32_t a2, u32_t a3, u32_t a4, u32_t a5);

#define SYS_read       3U
#define SYS_write      4U
#define SYS_close      6U
#define SYS_lseek      19U
#define SYS_getpid     20U
#define SYS_brk        45U
#define SYS_exit_group 252U
#define SYS_openat     295U
#define AT_FDCWD       ((u32_t)(-100))

typedef int (*libc_main_fn)(int, char **, char **);
typedef void (*libc_init_fn)(void);

int   *__errno_location(void);
u32_t  strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, u32_t n);
char  *strcpy(char *dst, const char *src);
void  *memcpy(void *dst, const void *src, u32_t n);
void  *memset(void *dst, int c, u32_t n);
s32_t  write(int fd, const void *buf, u32_t count);
s32_t  read(int fd, void *buf, u32_t count);
int    open(const char *path, int flags, int mode);
int    close(int fd);
s32_t  lseek(int fd, s32_t offset, int whence);
int    getpid(void);
int    puts(const char *s);
int    putchar(int c);
void  *malloc(u32_t size);
void  *calloc(u32_t nmemb, u32_t size);
void   free(void *p);
void   exit(int status);
void   _exit(int status);
void   __cxa_finalize(void *d);
void   __gmon_start__(void);
int    __libc_start_main(libc_main_fn main_fn, int argc, char **ubp_av,
                         libc_init_fn init_fn, libc_init_fn fini_fn,
                         libc_init_fn rtld_fini_fn, void *stack_end);

static int user_errno_val = 0;

int *__errno_location(void)
{
    return &user_errno_val;
}

u32_t strlen(const char *s)
{
    u32_t n = 0U;
    if (s == (const char *)0) {
        return 0U;
    }
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, u32_t n)
{
    while (n > 0U && *a != '\0' && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0U) {
        return 0;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *out = dst;
    while ((*dst++ = *src++) != '\0') {
    }
    return out;
}

void *memcpy(void *dst, const void *src, u32_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n-- > 0U) {
        *d++ = *s++;
    }
    return dst;
}

void *memset(void *dst, int c, u32_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n-- > 0U) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

s32_t write(int fd, const void *buf, u32_t count)
{
    return __oksinix_int80(SYS_write, (u32_t)fd, (u32_t)buf, count, 0U, 0U);
}

s32_t read(int fd, void *buf, u32_t count)
{
    return __oksinix_int80(SYS_read, (u32_t)fd, (u32_t)buf, count, 0U, 0U);
}

int open(const char *path, int flags, int mode)
{
    return (int)__oksinix_int80(SYS_openat, AT_FDCWD, (u32_t)path,
                                (u32_t)flags, (u32_t)mode, 0U);
}

int close(int fd)
{
    return (int)__oksinix_int80(SYS_close, (u32_t)fd, 0U, 0U, 0U, 0U);
}

s32_t lseek(int fd, s32_t offset, int whence)
{
    return __oksinix_int80(SYS_lseek, (u32_t)fd, (u32_t)offset, (u32_t)whence, 0U, 0U);
}

int getpid(void)
{
    return (int)__oksinix_int80(SYS_getpid, 0U, 0U, 0U, 0U, 0U);
}

int puts(const char *s)
{
    char nl = '\n';
    u32_t len;
    if (s == (const char *)0) {
        s = "(null)";
    }
    len = strlen(s);
    if (len > 0U) {
        (void)write(1, s, len);
    }
    (void)write(1, &nl, 1U);
    return (int)(len + 1U);
}

int putchar(int c)
{
    char ch = (char)c;
    (void)write(1, &ch, 1U);
    return c;
}

void *malloc(u32_t size)
{
    u32_t cur_brk;
    u32_t new_brk;
    u32_t aligned = (size + 15U) & ~15U;
    if (aligned == 0U) {
        aligned = 16U;
    }
    cur_brk = (u32_t)__oksinix_int80(SYS_brk, 0U, 0U, 0U, 0U, 0U);
    new_brk = (u32_t)__oksinix_int80(SYS_brk, cur_brk + aligned, 0U, 0U, 0U, 0U);
    if (new_brk < cur_brk + aligned) {
        return (void *)0;
    }
    return (void *)cur_brk;
}

void *calloc(u32_t nmemb, u32_t size)
{
    u32_t total = nmemb * size;
    void *p = malloc(total);
    if (p != (void *)0) {
        (void)memset(p, 0, total);
    }
    return p;
}

void free(void *p)
{
    (void)p;
}

void exit(int status)
{
    (void)__oksinix_int80(SYS_exit_group, (u32_t)status, 0U, 0U, 0U, 0U);
    for (;;) {
    }
}

void _exit(int status)
{
    exit(status);
}

void __cxa_finalize(void *d)
{
    (void)d;
}

void __gmon_start__(void)
{
}

int __libc_start_main(libc_main_fn main_fn, int argc, char **ubp_av,
                      libc_init_fn init_fn, libc_init_fn fini_fn,
                      libc_init_fn rtld_fini_fn, void *stack_end)
{
    char **envp;
    int rc;

    (void)fini_fn;
    (void)rtld_fini_fn;
    (void)stack_end;

    if (init_fn != (libc_init_fn)0) {
        init_fn();
    }
    envp = (ubp_av != (char **)0) ? &ubp_av[argc + 1] : (char **)0;
    rc = main_fn(argc, ubp_av, envp);
    exit(rc);
    return rc;
}
