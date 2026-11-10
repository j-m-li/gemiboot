/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Global Descriptor Table (GDT)
 */

#include "gdt.h"
#include "io.h"

#define GDT_ENTRIES 3

static volatile uint8_t  gdt_table[GDT_ENTRIES][8];
static volatile uint16_t gdt_ptr[3]; /* 6-byte GDTR: limit (16-bit) + base (32-bit) */

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

void gdt_init(void)
{
    uint32_t base_addr = (uint32_t)(uintptr_t)&gdt_table[0][0];
    uint16_t limit = (uint16_t)(sizeof(gdt_table) - 1U);

    gdt_set_gate(0, 0, 0, 0, 0);                /* Null descriptor */
    gdt_set_gate(1, 0, 0xFFFFFFFFUL, 0x9A, 0xCF); /* Kernel code segment (0x08) */
    gdt_set_gate(2, 0, 0xFFFFFFFFUL, 0x92, 0xCF); /* Kernel data segment (0x10) */

    gdt_ptr[0] = limit;
    gdt_ptr[1] = (uint16_t)(base_addr & 0xFFFFU);
    gdt_ptr[2] = (uint16_t)((base_addr >> 16) & 0xFFFFU);

    gdt_flush(&gdt_ptr[0]);
}
