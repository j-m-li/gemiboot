/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Linux x86 32-bit (i386) ELF Loader & Syscall ABI
 */

#ifndef OKSINIX_SYSCALL_H
#define OKSINIX_SYSCALL_H

#include "types.h"
#include "idt.h"

/* Linux x86 32-bit (i386) system call numbers (int $0x80) */
#define LINUX_SYS_exit           1
#define LINUX_SYS_fork           2
#define LINUX_SYS_read           3
#define LINUX_SYS_write          4
#define LINUX_SYS_open           5
#define LINUX_SYS_close          6
#define LINUX_SYS_waitpid        7
#define LINUX_SYS_unlink         10
#define LINUX_SYS_execve         11
#define LINUX_SYS_lseek          19
#define LINUX_SYS_getpid         20
#define LINUX_SYS_getuid         24
#define LINUX_SYS_access         33
#define LINUX_SYS_kill           37
#define LINUX_SYS_dup            41
#define LINUX_SYS_brk            45
#define LINUX_SYS_getgid         47
#define LINUX_SYS_geteuid        49
#define LINUX_SYS_getegid        50
#define LINUX_SYS_ioctl          54
#define LINUX_SYS_fcntl          55
#define LINUX_SYS_dup2           63
#define LINUX_SYS_gettimeofday   78
#define LINUX_SYS_mmap           90
#define LINUX_SYS_munmap         91
#define LINUX_SYS_ftruncate      93
#define LINUX_SYS_stat           106
#define LINUX_SYS_lstat          107
#define LINUX_SYS_fstat          108
#define LINUX_SYS_wait4          114
#define LINUX_SYS_fsync          118
#define LINUX_SYS_clone          120
#define LINUX_SYS__llseek        140
#define LINUX_SYS_readv          145
#define LINUX_SYS_writev         146
#define LINUX_SYS_nanosleep      162
#define LINUX_SYS_getcwd         183
#define LINUX_SYS_vfork          190
#define LINUX_SYS_mmap2          192
#define LINUX_SYS_ftruncate64    194
#define LINUX_SYS_stat64         195
#define LINUX_SYS_lstat64        196
#define LINUX_SYS_fstat64        197
#define LINUX_SYS_getuid32       199
#define LINUX_SYS_getgid32       200
#define LINUX_SYS_geteuid32      201
#define LINUX_SYS_getegid32      202
#define LINUX_SYS_fcntl64        221
#define LINUX_SYS_set_thread_area 243
#define LINUX_SYS_exit_group     252
#define LINUX_SYS_set_tid_address 258
#define LINUX_SYS_clock_gettime  265
#define LINUX_SYS_waitid         284
#define LINUX_SYS_openat         295
#define LINUX_SYS_fstatat        300
#define LINUX_SYS_unlinkat       301
#define LINUX_SYS_faccessat      307
#define LINUX_SYS_dup3           330
#define LINUX_SYS_statx          383
#define LINUX_SYS_clock_gettime64 403

void syscall_init(void);
void syscall_dispatch(struct isr_regs *regs);
int  linux_elf_execve(const char *path, int argc, char **argv, char **envp);

#endif /* OKSINIX_SYSCALL_H */
