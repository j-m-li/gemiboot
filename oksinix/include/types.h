/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Basic C90 types
 */

#ifndef OKSINIX_TYPES_H
#define OKSINIX_TYPES_H

typedef unsigned char      uint8_t;
typedef signed char        int8_t;
typedef unsigned short     uint16_t;
typedef signed short       int16_t;
typedef unsigned int       uint32_t;
typedef signed int         int32_t;
typedef unsigned int       size_t;
typedef unsigned int       uintptr_t;

typedef int                bool_t;
#define TRUE               1
#define FALSE              0

#ifndef NULL
#define NULL ((void *)0)
#endif

/* Compile-time size checks for 32-bit x86 target */
typedef char assert_u8_size[(sizeof(uint8_t) == 1) ? 1 : -1];
typedef char assert_u16_size[(sizeof(uint16_t) == 2) ? 1 : -1];
typedef char assert_u32_size[(sizeof(uint32_t) == 4) ? 1 : -1];
typedef char assert_ptr_size[(sizeof(void *) == 4) ? 1 : -1];

#endif /* OKSINIX_TYPES_H */
