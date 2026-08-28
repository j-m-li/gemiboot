/*
 * GEMIBOOT - x86 Boot Loader for GEMIOS
 * Dedicated to the Public Domain (CC0)
 */

#ifndef GEMIBOOT_DEFS_H
#define GEMIBOOT_DEFS_H 1

#define START_ADDRESS       0x010000 /* io16.S _start load address */
#define SIZE_16BIT_CODE     2048     /* size of the second stage code portion (io16.S) */
#define MEM_KERNEL_STACK    0x0007FFF0

#define KERNEL_ENTRY32      (START_ADDRESS + SIZE_16BIT_CODE)

#endif /* GEMIBOOT_DEFS_H */
