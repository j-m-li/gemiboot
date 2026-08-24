
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

#define XHCI_DEFAULT_BASE0 0xB0000000
#define XHCI_DEFAULT_BASE1 0xB8000000
#define XHCI_SUPPORTED_VERSION 0x95
#define XHCI_HEAP_SIZE (1024 * 1024)

#define XHCI_MAX_CONTROLLER 2
#define XHCI_MAX_PORT 16

#define PCI_MAX_BUS 256
#define PCI_MAX_SLOT 32
#define PCI_MAX_FUNCTION 8
#define PCI_CONFIG_ADDRESS 0xcf8
#define PCI_CONFIG_DATA 0xcfc

#define APIC_BASE_MSR 0x1B
#define APIC_BASE_MSR_BSP 0x100
#define APIC_BASE_MSR_ENABLE 0x800
#define APIC_DISABLE 0x10000
#define APIC_SW_ENABLE 0x100
#define APIC_CPUFOCUS 0x200
#define APIC_NMI (4 << 8)
#define TMR_PERIODIC 0x20000
#define TMR_BASEDIV (1 << 20)

#define APIC_APICID (0x020 >> 2)
#define APIC_APICVER (0x030 >> 2)
#define APIC_TASKPRIOR (0x080 >> 2)
#define APIC_EOI (0x0B0 >> 2)
#define APIC_LDR (0x0D0 >> 2)
#define APIC_DFR (0x0E0 >> 2)
#define APIC_SPURIOUS (0x0F0 >> 2)
#define APIC_ESR (0x280 >> 2)
#define APIC_ICRL (0x300 >> 2)
#define APIC_ICRH (0x310 >> 2)
#define APIC_LVT_TMR (0x320 >> 2)
#define APIC_LVT_PERF (0x340 >> 2)
#define APIC_LVT_LINT0 (0x350 >> 2)
#define APIC_LVT_LINT1 (0x360 >> 2)
#define APIC_LVT_ERR (0x370 >> 2)
#define APIC_TMRINITCNT (0x380 >> 2)
#define APIC_LAST (0x38F >> 2)
#define APIC_TMRCURRCNT (0x390 >> 2)
#define APIC_TMRDIV (0x3E0 >> 2)

#define IRQ_MASK irq_mask
#define IRQ_STATUS irq_status
#define IRQ_COUNTER18 1
#define IRQ_COUNTER18_NOT 2

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

#define ISR_ADDR (KERNEL_ENTRY64 + 0x800)
#define ISR_STUB (ISR_ADDR)
#define ISR_STUB_ERR (ISR_ADDR + 0x10)
#define ISR_SYSCALL (ISR_ADDR + 0x20)
#define ISR_TIMER (ISR_ADDR + 0x40)
#define ISR_KBD (ISR_ADDR + 0x60)
#define ISR_MOUSE (ISR_ADDR + 0x70)
#define ISR_USB (ISR_ADDR + 0x80)
#define ISR_END (ISR_ADDR + 0x100)

#define BEGIN_DESCRIPTORS (ISR_END + 0x400)
#define SIZE_OF_UEFI                                                           \
	0x40000 /* FIXME approximation of size of the uefi boot code ()*/
#define IDT_ADDR ((SIZE_OF_UEFI + BEGIN_DESCRIPTORS + 0xFFF) & ~0xFFF)
#define PDT_ADDR (IDT_ADDR + 0x8000)
#define MEM_PROG_END (PDT_ADDR + ((4 * 512 + 4 + 1) * 0x1000))

#endif /* __DEFS_H__ */
