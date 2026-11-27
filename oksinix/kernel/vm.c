/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Virtual Memory, Paging & Task Address-Space Protection
 *
 * Implements xv6-style x86 32-bit hardware isolation:
 *  - Page 0 (0x00000000) unmapped (null dereference -> #PF)
 *  - Low 32 MiB mapped via 4 KiB page tables (supervisor-only by default)
 *  - High 32 MiB .. 4 GiB (ACPI, VGA LFB, PCI MMIO) mapped via 4 MiB PSE supervisor PDEs
 *  - Hardware unmapped 4 KiB guard pages below kernel_stack, exec_kstack, and task stacks
 *  - Per-task user page directories cloning kernel PDEs (PTE_U=0) and granting PTE_U=1
 *    strictly to each task's own user pages
 *  - Syscall pointer validation walking the active user page directory (xv6 argptr/argstr)
 */

#include "vm.h"
#include "io.h"
#include "serial.h"
#include "string.h"

#define VM_KPT_COUNT      8U   /* 8 * 4 MiB = low 32 MiB with 4 KiB granularity */
#define VM_MAX_USER_SPACES 8U
#define VM_MAX_USER_PTS   32U
#define VM_MAX_USER_PAGES 32U

/* Total 4 KiB aligned pool in .bss: 1 kpgdir + 8 kpts + 8 upgdirs + 32 upts + 32 user phys pages */
#define VM_TOTAL_POOL_PAGES (1U + VM_KPT_COUNT + VM_MAX_USER_SPACES + VM_MAX_USER_PTS + VM_MAX_USER_PAGES)

extern uint32_t kernel_stack_guard[];
extern uint32_t exec_kstack_guard[];

static uint8_t   vm_pool_raw[(VM_TOTAL_POOL_PAGES + 1U) * VM_PAGE_SIZE];

static uint32_t *kpgdir = NULL;
static uint32_t *kpts[VM_KPT_COUNT];

static uint32_t *upgdir_slots[VM_MAX_USER_SPACES];
static bool_t    upgdir_used[VM_MAX_USER_SPACES];

static uint32_t *upt_slots[VM_MAX_USER_PTS];
static int       upt_owner_space[VM_MAX_USER_PTS];
static uint32_t  upt_owner_pde[VM_MAX_USER_PTS];

static uint32_t  upage_phys_base = 0U;
static bool_t    upage_used[VM_MAX_USER_PAGES];

static bool_t    vm_paging_enabled = FALSE;
static uint32_t *vm_active_pgdir = NULL;

static int vm_find_space_idx(const uint32_t *pgdir)
{
    uint32_t i;
    for (i = 0; i < VM_MAX_USER_SPACES; i++) {
        if (upgdir_used[i] && upgdir_slots[i] == pgdir) {
            return (int)i;
        }
    }
    return -1;
}

static uint32_t *vm_get_or_alloc_user_pt(int space_idx, uint32_t *pgdir, uint32_t pde_idx)
{
    uint32_t i;
    uint32_t *pt;

    for (i = 0; i < VM_MAX_USER_PTS; i++) {
        if (upt_owner_space[i] == space_idx && upt_owner_pde[i] == pde_idx) {
            return upt_slots[i];
        }
    }

    for (i = 0; i < VM_MAX_USER_PTS; i++) {
        if (upt_owner_space[i] < 0) {
            uint32_t pte_i;
            pt = upt_slots[i];
            upt_owner_space[i] = space_idx;
            upt_owner_pde[i]   = pde_idx;

            if (pde_idx < VM_KPT_COUNT) {
                /* Clone kernel 4 KiB PTEs (all supervisor-only, preserving unmapped guard pages) */
                memcpy(pt, kpts[pde_idx], VM_PAGE_SIZE);
            } else {
                /* Split 4 MiB PSE supervisor PDE into 1024 supervisor-only 4 KiB PTEs */
                uint32_t base_pa = pde_idx << 22;
                for (pte_i = 0; pte_i < 1024U; pte_i++) {
                    pt[pte_i] = (base_pa | (pte_i << 12)) | VM_PTE_P | VM_PTE_W;
                }
            }

            /* PDE carries PTE_U=1; individual PTEs still govern Ring-3 access (PDE.U && PTE.U) */
            pgdir[pde_idx] = ((uint32_t)(uintptr_t)pt & VM_PAGE_MASK) |
                             VM_PTE_P | VM_PTE_W | VM_PTE_U;
            return pt;
        }
    }
    return NULL;
}

void vm_init(void)
{
    uint32_t aligned_base;
    uint32_t cursor;
    uint32_t i;
    uint32_t p;

    aligned_base = ((uint32_t)(uintptr_t)vm_pool_raw + (VM_PAGE_SIZE - 1U)) & VM_PAGE_MASK;
    cursor = aligned_base;

    kpgdir = (uint32_t *)(uintptr_t)cursor;
    cursor += VM_PAGE_SIZE;

    for (i = 0; i < VM_KPT_COUNT; i++) {
        kpts[i] = (uint32_t *)(uintptr_t)cursor;
        cursor += VM_PAGE_SIZE;
    }
    for (i = 0; i < VM_MAX_USER_SPACES; i++) {
        upgdir_slots[i] = (uint32_t *)(uintptr_t)cursor;
        upgdir_used[i]  = FALSE;
        cursor += VM_PAGE_SIZE;
    }
    for (i = 0; i < VM_MAX_USER_PTS; i++) {
        upt_slots[i]       = (uint32_t *)(uintptr_t)cursor;
        upt_owner_space[i] = -1;
        upt_owner_pde[i]   = 0U;
        cursor += VM_PAGE_SIZE;
    }
    upage_phys_base = cursor;
    for (i = 0; i < VM_MAX_USER_PAGES; i++) {
        upage_used[i] = FALSE;
    }

    /* 1. Populate low 32 MiB with 4 KiB supervisor-only identity PTEs */
    for (i = 0; i < VM_KPT_COUNT; i++) {
        uint32_t base_pa = i << 22;
        for (p = 0; p < 1024U; p++) {
            kpts[i][p] = (base_pa | (p << 12)) | VM_PTE_P | VM_PTE_W;
        }
        kpgdir[i] = ((uint32_t)(uintptr_t)kpts[i] & VM_PAGE_MASK) | VM_PTE_P | VM_PTE_W;
    }

    /* 2. Populate 32 MiB .. 4 GiB with 4 MiB PSE supervisor-only identity PDEs (ACPI/LFB/MMIO) */
    for (i = VM_KPT_COUNT; i < 1024U; i++) {
        kpgdir[i] = (i << 22) | VM_PTE_P | VM_PTE_W | VM_PTE_PS;
    }

    /* 3. Unmap page 0 (null pointer guard) and kernel stack hardware guard pages */
    kpts[0][0] = 0U;
    vm_unmap_kernel_page((uint32_t)(uintptr_t)kernel_stack_guard);
    vm_unmap_kernel_page((uint32_t)(uintptr_t)exec_kstack_guard);

    vm_active_pgdir = kpgdir;
    cpu_enable_paging((uint32_t)(uintptr_t)kpgdir);
    vm_paging_enabled = TRUE;

    kprintf("[VM] x86 32-bit paging enabled (CR3=0x%08x, PSE+WP, null & kstack guard pages unmapped)\n",
            (uint32_t)(uintptr_t)kpgdir);
}

bool_t vm_is_enabled(void)
{
    return vm_paging_enabled;
}

uint32_t *vm_get_kernel_pgdir(void)
{
    return kpgdir;
}

uint32_t *vm_get_active_pgdir(void)
{
    return vm_active_pgdir;
}

void vm_switch(uint32_t *pgdir)
{
    if (!vm_paging_enabled) {
        return;
    }
    if (pgdir == NULL) {
        pgdir = kpgdir;
    }
    if (pgdir != vm_active_pgdir) {
        vm_active_pgdir = pgdir;
        cpu_load_cr3((uint32_t)(uintptr_t)pgdir);
    }
}

void vm_unmap_kernel_page(uint32_t vaddr)
{
    uint32_t pde_idx = vaddr >> 22;
    uint32_t pte_idx = (vaddr >> 12) & 0x3FFU;
    uint32_t i;

    if (kpgdir == NULL || pde_idx >= VM_KPT_COUNT) {
        return;
    }
    kpts[pde_idx][pte_idx] = 0U;
    for (i = 0; i < VM_MAX_USER_PTS; i++) {
        if (upt_owner_space[i] >= 0 && upt_owner_pde[i] == pde_idx) {
            upt_slots[i][pte_idx] = 0U;
        }
    }
    if (vm_paging_enabled) {
        cpu_invlpg(vaddr & VM_PAGE_MASK);
    }
}

uint32_t *vm_create_user_pgdir(void)
{
    uint32_t eflags;
    uint32_t i;
    uint32_t *pgdir = NULL;

    if (!vm_paging_enabled) {
        return NULL;
    }

    eflags = cpu_eflags();
    cpu_cli();
    for (i = 0; i < VM_MAX_USER_SPACES; i++) {
        if (!upgdir_used[i]) {
            upgdir_used[i] = TRUE;
            pgdir = upgdir_slots[i];
            memcpy(pgdir, kpgdir, VM_PAGE_SIZE);
            break;
        }
    }
    if ((eflags & 0x0200UL) != 0UL) {
        cpu_sti();
    }
    return pgdir;
}

void vm_destroy_user_pgdir(uint32_t *pgdir)
{
    uint32_t eflags;
    int space_idx;
    uint32_t i;

    if (pgdir == NULL || pgdir == kpgdir) {
        return;
    }

    eflags = cpu_eflags();
    cpu_cli();
    if (vm_active_pgdir == pgdir) {
        vm_active_pgdir = kpgdir;
        cpu_load_cr3((uint32_t)(uintptr_t)kpgdir);
    }
    space_idx = vm_find_space_idx(pgdir);
    if (space_idx >= 0) {
        for (i = 0; i < VM_MAX_USER_PTS; i++) {
            if (upt_owner_space[i] == space_idx) {
                upt_owner_space[i] = -1;
                upt_owner_pde[i]   = 0U;
            }
        }
        upgdir_used[space_idx] = FALSE;
    }
    if ((eflags & 0x0200UL) != 0UL) {
        cpu_sti();
    }
}

int vm_map_user_page(uint32_t *pgdir, uint32_t va, uint32_t pa, uint32_t flags)
{
    int space_idx;
    uint32_t pde_idx = va >> 22;
    uint32_t pte_idx = (va >> 12) & 0x3FFU;
    uint32_t *pt;

    if (pgdir == NULL || pgdir == kpgdir) {
        return -1;
    }
    space_idx = vm_find_space_idx(pgdir);
    if (space_idx < 0) {
        return -1;
    }
    pt = vm_get_or_alloc_user_pt(space_idx, pgdir, pde_idx);
    if (pt == NULL) {
        return -1;
    }
    pt[pte_idx] = (pa & VM_PAGE_MASK) | (flags & 0x0FFFU) | VM_PTE_P | VM_PTE_U;
    if (vm_paging_enabled && vm_active_pgdir == pgdir) {
        cpu_invlpg(va & VM_PAGE_MASK);
    }
    return 0;
}

int vm_unmap_user_page(uint32_t *pgdir, uint32_t va)
{
    int space_idx;
    uint32_t pde_idx = va >> 22;
    uint32_t pte_idx = (va >> 12) & 0x3FFU;
    uint32_t *pt;

    if (pgdir == NULL || pgdir == kpgdir) {
        return -1;
    }
    space_idx = vm_find_space_idx(pgdir);
    if (space_idx < 0) {
        return -1;
    }
    pt = vm_get_or_alloc_user_pt(space_idx, pgdir, pde_idx);
    if (pt == NULL) {
        return -1;
    }
    pt[pte_idx] = 0U;
    if (vm_paging_enabled && vm_active_pgdir == pgdir) {
        cpu_invlpg(va & VM_PAGE_MASK);
    }
    return 0;
}

int vm_map_user_range(uint32_t *pgdir, uint32_t va_start, uint32_t pa_start,
                      uint32_t len, uint32_t flags)
{
    uint32_t off;
    uint32_t va_aligned = va_start & VM_PAGE_MASK;
    uint32_t pa_aligned = pa_start & VM_PAGE_MASK;
    uint32_t aligned_len = (len + (va_start - va_aligned) + (VM_PAGE_SIZE - 1U)) & VM_PAGE_MASK;

    for (off = 0U; off < aligned_len; off += VM_PAGE_SIZE) {
        if (vm_map_user_page(pgdir, va_aligned + off, pa_aligned + off, flags) < 0) {
            return -1;
        }
    }
    return 0;
}

uint32_t vm_alloc_user_page(void)
{
    uint32_t eflags;
    uint32_t i;
    uint32_t pa = 0U;

    if (!vm_paging_enabled) {
        return 0U;
    }

    eflags = cpu_eflags();
    cpu_cli();
    for (i = 0; i < VM_MAX_USER_PAGES; i++) {
        if (!upage_used[i]) {
            upage_used[i] = TRUE;
            pa = upage_phys_base + i * VM_PAGE_SIZE;
            memset((void *)(uintptr_t)pa, 0, VM_PAGE_SIZE);
            break;
        }
    }
    if ((eflags & 0x0200UL) != 0UL) {
        cpu_sti();
    }
    return pa;
}

void vm_free_user_page(uint32_t pa)
{
    uint32_t eflags;
    uint32_t idx;

    if (pa < upage_phys_base || (pa & (VM_PAGE_SIZE - 1U)) != 0U) {
        return;
    }
    idx = (pa - upage_phys_base) / VM_PAGE_SIZE;
    if (idx >= VM_MAX_USER_PAGES) {
        return;
    }

    eflags = cpu_eflags();
    cpu_cli();
    upage_used[idx] = FALSE;
    if ((eflags & 0x0200UL) != 0UL) {
        cpu_sti();
    }
}

bool_t vm_user_range_ok(const uint32_t *pgdir, uint32_t va, uint32_t len, bool_t need_write)
{
    uint32_t end;
    uint32_t page;
    uint32_t end_page;

    if (!vm_paging_enabled || pgdir == NULL || pgdir == kpgdir) {
        return FALSE;
    }
    if (len == 0U) {
        return TRUE;
    }
    if (va == 0U) {
        return FALSE;
    }
    end = va + len - 1U;
    if (end < va) {
        return FALSE; /* Wrap-around */
    }

    page = va & VM_PAGE_MASK;
    end_page = end & VM_PAGE_MASK;
    for (;;) {
        uint32_t pde = pgdir[page >> 22];
        const uint32_t *pt;
        uint32_t pte;

        if ((pde & (VM_PTE_P | VM_PTE_U)) != (VM_PTE_P | VM_PTE_U) ||
            (pde & VM_PTE_PS) != 0U) {
            return FALSE;
        }
        if (need_write && (pde & VM_PTE_W) == 0U) {
            return FALSE;
        }
        pt = (const uint32_t *)(uintptr_t)(pde & VM_PAGE_MASK);
        pte = pt[(page >> 12) & 0x3FFU];
        if ((pte & (VM_PTE_P | VM_PTE_U)) != (VM_PTE_P | VM_PTE_U)) {
            return FALSE;
        }
        if (need_write && (pte & VM_PTE_W) == 0U) {
            return FALSE;
        }
        if (page == end_page) {
            break;
        }
        page += VM_PAGE_SIZE;
    }
    return TRUE;
}

bool_t vm_user_str_ok(const uint32_t *pgdir, uint32_t va, uint32_t max_len)
{
    uint32_t i;
    uint32_t last_page = 0xFFFFFFFFUL;

    if (!vm_paging_enabled || pgdir == NULL || pgdir == kpgdir || va == 0U || max_len == 0U) {
        return FALSE;
    }

    for (i = 0U; i < max_len; i++) {
        uint32_t cur = va + i;
        uint32_t cur_page;
        if (cur < va) {
            return FALSE;
        }
        cur_page = cur & VM_PAGE_MASK;
        if (cur_page != last_page) {
            if (!vm_user_range_ok(pgdir, cur_page, 1U, FALSE)) {
                return FALSE;
            }
            last_page = cur_page;
        }
        if (*(const char *)(uintptr_t)cur == '\0') {
            return TRUE;
        }
    }
    return FALSE;
}
