/*
 * GEMIBOOT - x86 Boot Loader for GEMIOS
 * Dedicated to the Public Domain (CC0)
 */

#ifndef GEMIBOOT_TYPES_H
#define GEMIBOOT_TYPES_H 1

#if defined(_MSC_VER)
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;
#else
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;
#endif

#if defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)
typedef unsigned long long size_t;
typedef unsigned long long uintptr_t;
#else
typedef unsigned int       size_t;
typedef unsigned int       uintptr_t;
#endif

typedef int bool;
#define true 1
#define false 0

#ifndef NULL
#define NULL ((void*)0)
#endif

#define UNUSED(x) ((void)(x))

void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
size_t strlen(const char *s);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strcpy(char *dest, const char *src);

#endif /* GEMIBOOT_TYPES_H */
