/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix - Link-time stub for libc.so.6 dynamic symbol resolution
 */

int   puts(const char *s);
void *malloc(unsigned int sz);
void  free(void *p);
char *strcpy(char *dst, const char *src);
int   strcmp(const char *a, const char *b);
int   getpid(void);
void  exit(int status);

int   puts(const char *s) { (void)s; return 0; }
void *malloc(unsigned int sz) { (void)sz; return (void *)0; }
void  free(void *p) { (void)p; }
char *strcpy(char *dst, const char *src) { (void)src; return dst; }
int   strcmp(const char *a, const char *b) { (void)a; (void)b; return 0; }
int   getpid(void) { return 0; }
void  exit(int status) { (void)status; }
