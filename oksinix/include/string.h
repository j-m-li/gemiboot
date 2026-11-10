/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Freestanding C90 string & memory utilities
 */

#ifndef OKSINIX_STRING_H
#define OKSINIX_STRING_H

#include "types.h"

void  *memset(void *dest, int c, size_t n);
void  *memcpy(void *dest, const void *src, size_t n);
int    memcmp(const void *s1, const void *s2, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *s1, const char *s2);
int    strncmp(const char *s1, const char *s2, size_t n);

#endif /* OKSINIX_STRING_H */
