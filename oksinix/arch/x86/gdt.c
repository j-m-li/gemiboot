/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Global Descriptor Table (GDT) & 32-bit TSS
 */

#include "gdt.h"
#include "io.h"
#include "string.h"

#define GDT_ENTRIES 6

/* x86 32-bit Task State Segment (104 bytes) */
struct tss32 {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
};

extern uint32_t kernel_stack_top[];

static volatile uint8_t  gdt_table[GDT_ENTRIES][8];
static volatile uint16_t gdt_ptr[3]; /* 6-byte GDTR: limit (16-bit) + base (32-bit) */
static struct tss32      kernel_tss;

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    gdt_table[num][0] = (uint8_t)(limit & 0xFFU);
    gdt_table[num][1] = (uint8_t)((limit >> 8) & 0xFFU);
    gdt_table[num][2] = (uint8_t)(base & 0xFFU);
    gdt_table[num][3] = (uint8_t)((base >> 8) & 0xFFU);
    gdt_table[num][4] = (uint8_t)((base >> 16) & 0xFFU);
    gdt_table[num][5] = access;
    gdt_table[num][6] = (uint8_t)(((limit >> 16) & 0x0FU) | (gran & 0xF0U));
    gdt_table[num][7] = (uint8_t)((base >> 24) & 0xFFU);
}

void tss_set_esp0(uint32_t esp0)
{
    kernel_tss.esp0 = esp0;
}

uint32_t tss_get_esp0(void)
{
    return kernel_tss.esp0;
}

void gdt_init(void)
{
    uint32_t base_addr = (uint32_t)(uintptr_t)&gdt_table[0][0];
    uint16_t limit = (uint16_t)(sizeof(gdt_table) - 1U);
    uint32_t tss_base = (uint32_t)(uintptr_t)&kernel_tss;
    uint32_t tss_limit = (uint32_t)(sizeof(kernel_tss) - 1U);

    memset(&kernel_tss, 0, sizeof(kernel_tss));
    kernel_tss.ss0        = KERNEL_DS;
    kernel_tss.esp0       = (uint32_t)(uintptr_t)kernel_stack_top;
    kernel_tss.iomap_base = (uint16_t)sizeof(kernel_tss); /* Deny all Ring-3 port I/O */

    gdt_set_gate(0, 0, 0, 0, 0);                          /* 0x00: Null descriptor */
    gdt_set_gate(1, 0, 0xFFFFFFFFUL, 0x9A, 0xCF);         /* 0x08: Kernel code (DPL 0) */
    gdt_set_gate(2, 0, 0xFFFFFFFFUL, 0x92, 0xCF);         /* 0x10: Kernel data (DPL 0) */
    gdt_set_gate(3, 0, 0xFFFFFFFFUL, 0xFA, 0xCF);         /* 0x18: User code (DPL 3) */
    gdt_set_gate(4, 0, 0xFFFFFFFFUL, 0xF2, 0xCF);         /* 0x20: User data (DPL 3) */
    gdt_set_gate(5, tss_base, tss_limit, 0x89, 0x00);     /* 0x28: 32-bit Available TSS */

    gdt_ptr[0] = limit;
    gdt_ptr[1] = (uint16_t)(base_addr & 0xFFFFU);
    gdt_ptr[2] = (uint16_t)((base_addr >> 16) & 0xFFFFU);

    gdt_flush(&gdt_ptr[0]);
    tss_flush((uint16_t)TSS_SEG);
}
