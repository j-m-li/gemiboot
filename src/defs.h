
/*
 *                         OS-3o3 Operating System
 *
 *                      13 may MMXXIV PUBLIC DOMAIN
 *           The authors disclaim copyright to this source code.
 *
 *
 */

/*
 * memory range that is safe to use : 0x000500 to  0x07FFFF and 0x100000 to
 * 0xEFFFFF
 */

#ifndef __DEFS_H__
#define __DEFS_H__ 1

#define FB_DEFAULT_ADDR 0xA0000000
#define MEM_HEAP_START 0x01000000
/* #define MEM_HEAP_START 0x00100000 FIXME */
#define MEM_HEAP_SIZE ((128 * 1024 * 1024) - MEM_HEAP_START)
#define START_ADDRESS 0x010000 /* io16.S _start load address*/
#define SIZE_16BIT_CODE                                                        \
	1024 /* size of the second stage code portion                          \
		(io16.S)*/
#define MEM_KERNEL_STACK START_ADDRESS

#define KERNEL_ENTRY32 (START_ADDRESS + SIZE_16BIT_CODE)
#define MEM_KERNEL_INFO                                                        \
	(KERNEL_ENTRY32 + 16) /* kernel info structure full address */
#define MEM_HPET_CONFIG (MEM_KERNEL_INFO + 0x8)
#define MEM_GFX_MODE (MEM_KERNEL_INFO + 0x10)
#define MEM_GFX_WIDTH (MEM_KERNEL_INFO + 0x14)
#define MEM_GFX_HEIGHT (MEM_KERNEL_INFO + 0x18)
#define MEM_GFX_PITCH (MEM_KERNEL_INFO + 0x1C)
#define MEM_GFX_FB (MEM_KERNEL_INFO + 0x20)
#define SPIN_LOCK (MEM_KERNEL_INFO + 0x0F8)
#define KERNEL_INFO_END (KERNEL_INFO + 0x100)

#define GDTP_ADDR (KERNEL_ENTRY32 + 0x120)
#define GDT_ADDR (GDTP_ADDR + 0x40)
#define KERNEL_ENTRY64 (GDT_ADDR + 0x20 + 0x600)

#endif /* __DEFS_H__ */
