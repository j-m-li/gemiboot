/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Built-in userland programs (/bin/init, /bin/sh)
 */

#ifndef OKSINIX_USER_H
#define OKSINIX_USER_H

#include "types.h"

void user_programs_init(void);
int  bin_init_main(int argc, char **argv);
int  bin_sh_main(int argc, char **argv);
int  bin_vi_main(int argc, char **argv);

#endif /* OKSINIX_USER_H */
