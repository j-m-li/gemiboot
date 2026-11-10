/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix - Dynamically linked 32-bit Linux i386 ELF executable (DYN.ELF)
 * Uses PT_INTERP (/lib/ld-linux.so.2), DT_NEEDED (LIBTEST.SO, libc.so.6),
 * and PLT/GOT (R_386_JMP_SLOT) relocations.
 */

extern int         shlib_add(int a, int b);
extern const char *shlib_get_msg(void);

extern int   puts(const char *s);
extern void *malloc(unsigned int sz);
extern void  free(void *p);
extern char *strcpy(char *dst, const char *src);
extern int   strcmp(const char *a, const char *b);
extern int   getpid(void);
extern void  exit(int status);

void _start(void);

void _start(void)
{
    char *buf;

    if (getpid() <= 0) {
        puts("[LINUX-DYN] getpid() FAILED");
        exit(1);
    }

    if (shlib_add(19, 23) != 42) {
        puts("[LINUX-DYN] shlib_add() FAILED");
        exit(1);
    }

    buf = (char *)malloc(32U);
    if (buf == (char *)0) {
        puts("[LINUX-DYN] malloc() FAILED");
        exit(1);
    }

    strcpy(buf, "dyn-heap-ok");
    if (strcmp(buf, "dyn-heap-ok") != 0) {
        puts("[LINUX-DYN] strcpy/strcmp FAILED");
        exit(1);
    }
    free(buf);

    puts(shlib_get_msg());
    exit(0);
}
