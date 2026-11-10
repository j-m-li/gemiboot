/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix - 32-bit Linux i386 position-independent shared library (LIBTEST.SO)
 */

int shlib_add(int a, int b);
const char *shlib_get_msg(void);

int shlib_add(int a, int b)
{
    return a + b;
}

const char *shlib_get_msg(void)
{
    return "[LINUX-DYN] Shared library LIBTEST.SO + libc.so.6 + PT_INTERP PASSED!";
}
