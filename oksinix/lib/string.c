/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Freestanding C90 string & memory utilities
 */

#include "string.h"

void *memset(void *dest, int c, size_t n)
{
    unsigned char *p = (unsigned char *)dest;
    size_t i;
    for (i = 0; i < n; i++) {
        p[i] = (unsigned char)c;
    }
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    size_t i;
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    size_t i;
    for (i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return (int)p1[i] - (int)p2[i];
        }
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 != '\0' && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (int)(unsigned char)(*s1) - (int)(unsigned char)(*s2);
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (s1[i] != s2[i] || s1[i] == '\0') {
            return (int)(unsigned char)s1[i] - (int)(unsigned char)s2[i];
        }
    }
    return 0;
}
