/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Virtual Memory, Paging & Task Address-Space Protection
 */

#ifndef OKSINIX_VM_H
#define OKSINIX_VM_H

#include "types.h"

#define VM_PAGE_SIZE      4096U
#define VM_PAGE_MASK      0xFFFFF000UL

/* x86 32-bit PDE / PTE flags */
#define VM_PTE_P          0x001U /* Present */
#define VM_PTE_W          0x002U /* Writable */
#define VM_PTE_U          0x004U /* User-accessible (Ring 3) */
#define VM_PTE_PS         0x080U /* 4 MiB Page Size (PDE only, CR4.PSE=1) */

/* Fixed high virtual addresses for isolated Ring-3 user tasks (non-Linux ELF) */
#define VM_UTASK_CODE_VA  0x40000000UL
#define VM_UTASK_DATA_VA  0x40001000UL
#define VM_UTASK_GUARD_VA 0x40002000UL /* Unmapped 4 KiB stack guard page */
#define VM_UTASK_STACK_VA 0x40003000UL /* 4 KiB Ring-3 stack page */
#define VM_UTASK_STACK_TOP (VM_UTASK_STACK_VA + VM_PAGE_SIZE)

void      vm_init(void);
bool_t    vm_is_enabled(void);
uint32_t *vm_get_kernel_pgdir(void);
uint32_t *vm_get_active_pgdir(void);
void      vm_switch(uint32_t *pgdir);

void      vm_unmap_kernel_page(uint32_t vaddr);

uint32_t *vm_create_user_pgdir(void);
void      vm_destroy_user_pgdir(uint32_t *pgdir);
int       vm_map_user_page(uint32_t *pgdir, uint32_t va, uint32_t pa, uint32_t flags);
int       vm_unmap_user_page(uint32_t *pgdir, uint32_t va);
int       vm_map_user_range(uint32_t *pgdir, uint32_t va_start, uint32_t pa_start,
                            uint32_t len, uint32_t flags);

uint32_t  vm_alloc_user_page(void);
void      vm_free_user_page(uint32_t pa);

bool_t    vm_user_range_ok(const uint32_t *pgdir, uint32_t va, uint32_t len, bool_t need_write);
bool_t    vm_user_str_ok(const uint32_t *pgdir, uint32_t va, uint32_t max_len);

#endif /* OKSINIX_VM_H */
