/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Preemptive Round-Robin Multitasking Scheduler
 * with xv6-style per-task page directories, Ring-3 isolation & hardware stack guard pages
 */

#include "sched.h"
#include "gdt.h"
#include "vm.h"
#include "pit.h"
#include "io.h"
#include "serial.h"
#include "string.h"

#define TASK_KSTACK_PAGES   4U    /* 16 KiB Ring-0 kernel stack per spawned task */
#define TASK_KSTACK_BYTES   (TASK_KSTACK_PAGES * VM_PAGE_SIZE)
#define TASK_SLOT_PAGES     (1U + TASK_KSTACK_PAGES) /* 1 guard page + 4 stack pages */
#define TASK_MAX_OWNED_PGS  4

#define TASK_GUARD_W0       0xDEADBEEFUL
#define TASK_GUARD_W1       0xCAFEBABEUL
#define TASK_GUARD_W2       0x57AC4600UL
#define TASK_GUARD_W3       0xBADC0FFEUL

struct sched_task {
    int            pid;
    uint32_t       state;
    bool_t         is_idle;
    bool_t         is_user;
    bool_t         detached;
    bool_t         need_resched;
    int            exit_code;
    uint32_t       fault_vec;
    uint32_t       fault_cr2;
    uint32_t      *pgdir;
    uint32_t       owned_pages[TASK_MAX_OWNED_PGS];
    int            owned_page_count;
    uint32_t       saved_esp;
    uint32_t       stack_base;
    uint32_t       stack_top;
    uint32_t       wake_tick;
    uint32_t       ticks;
    uint32_t       switches;
    sched_entry_t  entry;
    void          *arg;
    char           name[SCHED_NAME_MAX];
};

extern uint32_t kernel_stack_bottom[];
extern uint32_t kernel_stack_top[];

static struct sched_task task_table[SCHED_MAX_TASKS];
static uint8_t           task_stacks_raw[(SCHED_MAX_TASKS * TASK_SLOT_PAGES + 1U) * VM_PAGE_SIZE];
static uint32_t         *task_stack_bases[SCHED_MAX_TASKS];
static int               cur_task_idx = -1;
static int               next_task_pid = 2;
static volatile bool_t   sched_enabled = FALSE;
static volatile int      preempt_lock_depth = 0;
static uint32_t          cur_slice_ticks = 0U;
static uint32_t          total_ctx_switches = 0U;

static void sched_copy_name(char dst[SCHED_NAME_MAX], const char *src)
{
    size_t len;
    if (src == NULL) {
        src = "task";
    }
    len = strlen(src);
    if (len >= SCHED_NAME_MAX) {
        len = SCHED_NAME_MAX - 1U;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void sched_arm_guard(uint32_t *base)
{
    base[0] = TASK_GUARD_W0;
    base[1] = TASK_GUARD_W1;
    base[2] = TASK_GUARD_W2;
    base[3] = TASK_GUARD_W3;
}

static bool_t sched_guard_ok(const uint32_t *base)
{
    return (base[0] == TASK_GUARD_W0 &&
            base[1] == TASK_GUARD_W1 &&
            base[2] == TASK_GUARD_W2 &&
            base[3] == TASK_GUARD_W3) ? TRUE : FALSE;
}

/* Free VM resources of a non-current slot (must be called with interrupts disabled) */
static void sched_release_slot_vm(int slot)
{
    struct sched_task *t = &task_table[slot];
    int i;

    if (slot == cur_task_idx) {
        return;
    }
    if (t->pgdir != NULL && t->pgdir != vm_get_kernel_pgdir()) {
        vm_destroy_user_pgdir(t->pgdir);
    }
    t->pgdir = NULL;
    for (i = 0; i < t->owned_page_count; i++) {
        if (t->owned_pages[i] != 0U) {
            vm_free_user_page(t->owned_pages[i]);
            t->owned_pages[i] = 0U;
        }
    }
    t->owned_page_count = 0;
}

/* Clean up any detached zombie tasks so their VM resources and slots can be reused */
static void sched_reap_detached(void)
{
    int i;
    for (i = 2; i < SCHED_MAX_TASKS; i++) {
        if (i != cur_task_idx &&
            task_table[i].state == TASK_STATE_ZOMBIE &&
            task_table[i].detached) {
            sched_release_slot_vm(i);
            task_table[i].state = TASK_STATE_UNUSED;
            task_table[i].pid   = 0;
        }
    }
}

static void sched_task_trampoline(void)
{
    sched_entry_t entry = NULL;
    void *arg = NULL;

    if (cur_task_idx >= 0 && cur_task_idx < SCHED_MAX_TASKS) {
        entry = task_table[cur_task_idx].entry;
        arg   = task_table[cur_task_idx].arg;
    }
    if (entry != NULL) {
        entry(arg);
    }
    sched_exit(0);
}

static void sched_idle_entry(void *arg)
{
    (void)arg;
    for (;;) {
        cpu_sti();
        cpu_hlt();
    }
}

void sched_init(void)
{
    int i;
    struct sched_task *boot_t;
    struct sched_task *idle_t;
    struct isr_regs *regs;
    uint32_t aligned_base;
    uint32_t top_addr;

    memset(task_table, 0, sizeof(task_table));
    cur_task_idx       = 0;
    next_task_pid      = 2;
    preempt_lock_depth = 0;
    cur_slice_ticks    = 0U;
    total_ctx_switches = 0U;

    /* Align task kernel stacks on 4 KiB boundaries and unmap a 4 KiB guard page below each */
    aligned_base = ((uint32_t)(uintptr_t)task_stacks_raw + (VM_PAGE_SIZE - 1U)) & VM_PAGE_MASK;
    for (i = 0; i < SCHED_MAX_TASKS; i++) {
        uint32_t slot_base = aligned_base + (uint32_t)i * TASK_SLOT_PAGES * VM_PAGE_SIZE;
        uint32_t stack_start = slot_base + VM_PAGE_SIZE;
        task_stack_bases[i] = (uint32_t *)(uintptr_t)stack_start;
        if (i >= 1) {
            vm_unmap_kernel_page(slot_base);
        }
    }

    /* Slot 0: primary boot / init / shell task running on boot kernel_stack */
    boot_t = &task_table[0];
    boot_t->pid        = 1;
    boot_t->state      = TASK_STATE_RUNNING;
    boot_t->is_idle    = FALSE;
    boot_t->is_user    = FALSE;
    boot_t->detached   = FALSE;
    boot_t->pgdir      = vm_get_kernel_pgdir();
    boot_t->stack_base = (uint32_t)(uintptr_t)kernel_stack_bottom;
    boot_t->stack_top  = (uint32_t)(uintptr_t)kernel_stack_top;
    sched_copy_name(boot_t->name, "/bin/init");

    /* Slot 1: lowest-priority idle task (PID 0), scheduled only when no other task is READY */
    idle_t = &task_table[1];
    sched_arm_guard(task_stack_bases[1]);
    top_addr = (uint32_t)(uintptr_t)task_stack_bases[1] + TASK_KSTACK_BYTES;
    top_addr &= ~0x0FUL;
    regs = (struct isr_regs *)(uintptr_t)(top_addr - (uint32_t)sizeof(struct isr_regs));
    memset(regs, 0, sizeof(*regs));
    regs->ds     = KERNEL_DS;
    regs->cs     = KERNEL_CS;
    regs->eip    = (uint32_t)(uintptr_t)sched_task_trampoline;
    regs->eflags = 0x00000202UL; /* IF=1, reserved bit 1=1 */

    idle_t->pid        = 0;
    idle_t->state      = TASK_STATE_READY;
    idle_t->is_idle    = TRUE;
    idle_t->is_user    = FALSE;
    idle_t->detached   = TRUE;
    idle_t->pgdir      = vm_get_kernel_pgdir();
    idle_t->saved_esp  = (uint32_t)(uintptr_t)regs;
    idle_t->stack_base = (uint32_t)(uintptr_t)task_stack_bases[1];
    idle_t->stack_top  = top_addr;
    idle_t->entry      = sched_idle_entry;
    idle_t->arg        = NULL;
    sched_copy_name(idle_t->name, "kidle");

    for (i = 2; i < SCHED_MAX_TASKS; i++) {
        task_table[i].state = TASK_STATE_UNUSED;
    }

    tss_set_esp0(boot_t->stack_top);
    sched_enabled = TRUE;
    kprintf("[SCHED] Preemptive multitasking initialized (max_tasks=%u, quantum=%ums, kstack=%uKiB + 4KiB hw guard)\n",
            (uint32_t)SCHED_MAX_TASKS, (uint32_t)SCHED_QUANTUM_TICKS,
            (uint32_t)(TASK_KSTACK_BYTES / 1024U));
}

bool_t sched_is_enabled(void)
{
    return sched_enabled;
}

void sched_preempt_disable(void)
{
    preempt_lock_depth++;
}

void sched_preempt_enable(void)
{
    if (preempt_lock_depth > 0) {
        preempt_lock_depth--;
    }
}

void sched_set_current_name(const char *name)
{
    uint32_t eflags = cpu_eflags();
    cpu_cli();
    if (cur_task_idx >= 0 && cur_task_idx < SCHED_MAX_TASKS) {
        sched_copy_name(task_table[cur_task_idx].name, name);
    }
    if ((eflags & 0x0200UL) != 0UL) {
        cpu_sti();
    }
}

int sched_getpid(void)
{
    if (cur_task_idx >= 0 && cur_task_idx < SCHED_MAX_TASKS) {
        return task_table[cur_task_idx].pid;
    }
    return 0;
}

bool_t sched_current_is_user(void)
{
    if (cur_task_idx >= 0 && cur_task_idx < SCHED_MAX_TASKS) {
        return task_table[cur_task_idx].is_user;
    }
    return FALSE;
}

uint32_t *sched_current_pgdir(void)
{
    if (cur_task_idx >= 0 && cur_task_idx < SCHED_MAX_TASKS &&
        task_table[cur_task_idx].pgdir != NULL) {
        return task_table[cur_task_idx].pgdir;
    }
    return vm_get_kernel_pgdir();
}

bool_t sched_current_needs_resched(void)
{
    if (cur_task_idx >= 0 && cur_task_idx < SCHED_MAX_TASKS) {
        if (task_table[cur_task_idx].need_resched ||
            task_table[cur_task_idx].state != TASK_STATE_RUNNING) {
            task_table[cur_task_idx].need_resched = FALSE;
            return TRUE;
        }
    }
    return FALSE;
}

void sched_mark_current_exit_isr(int exit_code)
{
    if (cur_task_idx <= 1 || cur_task_idx >= SCHED_MAX_TASKS) {
        return;
    }
    task_table[cur_task_idx].exit_code    = exit_code;
    task_table[cur_task_idx].state        = TASK_STATE_ZOMBIE;
    task_table[cur_task_idx].need_resched = TRUE;
    preempt_lock_depth = 0;
}

void sched_mark_current_yield_isr(void)
{
    if (cur_task_idx >= 0 && cur_task_idx < SCHED_MAX_TASKS) {
        task_table[cur_task_idx].need_resched = TRUE;
    }
}

int sched_spawn_ex(const char *name, sched_entry_t entry, void *arg, bool_t detached)
{
    uint32_t eflags = cpu_eflags();
    int slot = -1;
    int i;
    int pid;
    struct sched_task *t;
    struct isr_regs *regs;
    uint32_t top_addr;

    if (!sched_enabled || entry == NULL) {
        return -1;
    }

    cpu_cli();
    sched_reap_detached();
    for (i = 2; i < SCHED_MAX_TASKS; i++) {
        if (task_table[i].state == TASK_STATE_UNUSED) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if ((eflags & 0x0200UL) != 0UL) {
            cpu_sti();
        }
        return -1;
    }

    t = &task_table[slot];
    memset(t, 0, sizeof(*t));
    memset(task_stack_bases[slot], 0, TASK_KSTACK_BYTES);
    sched_arm_guard(task_stack_bases[slot]);

    top_addr = (uint32_t)(uintptr_t)task_stack_bases[slot] + TASK_KSTACK_BYTES;
    top_addr &= ~0x0FUL;
    regs = (struct isr_regs *)(uintptr_t)(top_addr - (uint32_t)sizeof(struct isr_regs));
    memset(regs, 0, sizeof(*regs));
    regs->ds     = KERNEL_DS;
    regs->cs     = KERNEL_CS;
    regs->eip    = (uint32_t)(uintptr_t)sched_task_trampoline;
    regs->eflags = 0x00000202UL; /* Interrupts enabled on task entry */

    pid = next_task_pid++;
    t->pid        = pid;
    t->state      = TASK_STATE_READY;
    t->is_idle    = FALSE;
    t->is_user    = FALSE;
    t->detached   = detached;
    t->exit_code  = 0;
    t->pgdir      = vm_get_kernel_pgdir();
    t->saved_esp  = (uint32_t)(uintptr_t)regs;
    t->stack_base = (uint32_t)(uintptr_t)task_stack_bases[slot];
    t->stack_top  = top_addr;
    t->entry      = entry;
    t->arg        = arg;
    sched_copy_name(t->name, name);

    if ((eflags & 0x0200UL) != 0UL) {
        cpu_sti();
    }
    return pid;
}

int sched_spawn(const char *name, sched_entry_t entry, void *arg)
{
    return sched_spawn_ex(name, entry, arg, FALSE);
}

int sched_spawn_user_code(const char *name, const void *code, uint32_t code_len,
                          uint32_t arg_val, bool_t detached)
{
    uint32_t eflags = cpu_eflags();
    int slot = -1;
    int i;
    int pid;
    uint32_t *pgdir = NULL;
    uint32_t code_pa = 0U;
    uint32_t data_pa = 0U;
    uint32_t stack_pa = 0U;
    struct sched_task *t;
    struct isr_regs *regs;
    uint32_t top_addr;

    if (!sched_enabled || !vm_is_enabled() || code == NULL ||
        code_len == 0U || code_len > VM_PAGE_SIZE) {
        return -1;
    }

    cpu_cli();
    sched_reap_detached();
    for (i = 2; i < SCHED_MAX_TASKS; i++) {
        if (task_table[i].state == TASK_STATE_UNUSED) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        goto fail;
    }

    pgdir    = vm_create_user_pgdir();
    code_pa  = vm_alloc_user_page();
    data_pa  = vm_alloc_user_page();
    stack_pa = vm_alloc_user_page();
    if (pgdir == NULL || code_pa == 0U || data_pa == 0U || stack_pa == 0U) {
        goto fail;
    }

    memcpy((void *)(uintptr_t)code_pa, code, code_len);
    *(uint32_t *)(uintptr_t)data_pa = arg_val;

    /* Map user code (R-X, no W), user data (RW), unmapped stack guard, and user stack (RW) */
    if (vm_map_user_page(pgdir, VM_UTASK_CODE_VA, code_pa, VM_PTE_U) < 0 ||
        vm_map_user_page(pgdir, VM_UTASK_DATA_VA, data_pa, VM_PTE_U | VM_PTE_W) < 0 ||
        vm_unmap_user_page(pgdir, VM_UTASK_GUARD_VA) < 0 ||
        vm_map_user_page(pgdir, VM_UTASK_STACK_VA, stack_pa, VM_PTE_U | VM_PTE_W) < 0) {
        goto fail;
    }

    t = &task_table[slot];
    memset(t, 0, sizeof(*t));
    memset(task_stack_bases[slot], 0, TASK_KSTACK_BYTES);
    sched_arm_guard(task_stack_bases[slot]);

    top_addr = (uint32_t)(uintptr_t)task_stack_bases[slot] + TASK_KSTACK_BYTES;
    top_addr &= ~0x0FUL;
    regs = (struct isr_regs *)(uintptr_t)(top_addr - (uint32_t)sizeof(struct isr_regs));
    memset(regs, 0, sizeof(*regs));
    regs->ds       = USER_DS;
    regs->ebx      = arg_val;
    regs->eip      = VM_UTASK_CODE_VA;
    regs->cs       = USER_CS;
    regs->eflags   = 0x00000202UL; /* IF=1, IOPL=0 (Ring 3 cannot cli/sti/in/out) */
    regs->user_esp = VM_UTASK_STACK_TOP - 16U;
    regs->user_ss  = USER_DS;

    pid = next_task_pid++;
    t->pid              = pid;
    t->state            = TASK_STATE_READY;
    t->is_idle          = FALSE;
    t->is_user          = TRUE;
    t->detached         = detached;
    t->exit_code        = 0;
    t->fault_vec        = 0U;
    t->fault_cr2        = 0U;
    t->pgdir            = pgdir;
    t->owned_pages[0]   = code_pa;
    t->owned_pages[1]   = data_pa;
    t->owned_pages[2]   = stack_pa;
    t->owned_page_count = 3;
    t->saved_esp        = (uint32_t)(uintptr_t)regs;
    t->stack_base       = (uint32_t)(uintptr_t)task_stack_bases[slot];
    t->stack_top        = top_addr;
    sched_copy_name(t->name, name);

    if ((eflags & 0x0200UL) != 0UL) {
        cpu_sti();
    }
    return pid;

fail:
    if (pgdir != NULL) {
        vm_destroy_user_pgdir(pgdir);
    }
    if (code_pa != 0U) {
        vm_free_user_page(code_pa);
    }
    if (data_pa != 0U) {
        vm_free_user_page(data_pa);
    }
    if (stack_pa != 0U) {
        vm_free_user_page(stack_pa);
    }
    if ((eflags & 0x0200UL) != 0UL) {
        cpu_sti();
    }
    return -1;
}

void sched_yield(void)
{
    if (!sched_enabled || preempt_lock_depth > 0) {
        return;
    }
    cpu_yield();
}

void sched_sleep_ms(uint32_t ms)
{
    uint32_t target;

    if (!sched_enabled || cur_task_idx < 0 || preempt_lock_depth > 0) {
        pit_mdelay(ms);
        return;
    }
    if (ms == 0U) {
        sched_yield();
        return;
    }

    target = pit_get_ticks() + ms;
    for (;;) {
        uint32_t now = pit_get_ticks();
        if ((int32_t)(now - target) >= 0) {
            break;
        }
        cpu_cli();
        task_table[cur_task_idx].wake_tick = target;
        task_table[cur_task_idx].state     = TASK_STATE_SLEEPING;
        cpu_yield();
        cpu_sti();
    }
}

void sched_exit(int exit_code)
{
    if (!sched_enabled || cur_task_idx <= 1) {
        for (;;) {
            cpu_hlt();
        }
    }

    cpu_cli();
    task_table[cur_task_idx].exit_code = exit_code;
    task_table[cur_task_idx].state     = TASK_STATE_ZOMBIE;
    preempt_lock_depth = 0;
    cpu_yield();

    for (;;) {
        cpu_cli();
        cpu_hlt();
    }
}

int sched_kill(int pid)
{
    uint32_t eflags = cpu_eflags();
    int i;
    int rc = -1;

    if (!sched_enabled || pid <= 1) {
        return -1; /* Protect kidle (0) and /bin/init (1) */
    }

    cpu_cli();
    for (i = 2; i < SCHED_MAX_TASKS; i++) {
        if (task_table[i].state != TASK_STATE_UNUSED && task_table[i].pid == pid) {
            if (i == cur_task_idx) {
                if ((eflags & 0x0200UL) != 0UL) {
                    cpu_sti();
                }
                sched_exit(128 + 9);
            }
            task_table[i].exit_code = 128 + 9;
            if (task_table[i].detached) {
                sched_release_slot_vm(i);
                task_table[i].state = TASK_STATE_UNUSED;
                task_table[i].pid   = 0;
            } else {
                task_table[i].state = TASK_STATE_ZOMBIE;
            }
            rc = 0;
            break;
        }
    }
    if ((eflags & 0x0200UL) != 0UL) {
        cpu_sti();
    }
    return rc;
}

int sched_wait_ex(int pid, int *out_exit_code,
                  uint32_t *out_fault_vec, uint32_t *out_fault_cr2)
{
    if (!sched_enabled || pid <= 1) {
        return -1;
    }

    for (;;) {
        uint32_t eflags = cpu_eflags();
        int i;
        int found = 0;

        cpu_cli();
        for (i = 2; i < SCHED_MAX_TASKS; i++) {
            if (task_table[i].state != TASK_STATE_UNUSED && task_table[i].pid == pid) {
                found = 1;
                if (task_table[i].state == TASK_STATE_ZOMBIE) {
                    int code = task_table[i].exit_code;
                    uint32_t fvec = task_table[i].fault_vec;
                    uint32_t fcr2 = task_table[i].fault_cr2;
                    sched_release_slot_vm(i);
                    task_table[i].state = TASK_STATE_UNUSED;
                    task_table[i].pid   = 0;
                    if (out_exit_code != NULL) {
                        *out_exit_code = code;
                    }
                    if (out_fault_vec != NULL) {
                        *out_fault_vec = fvec;
                    }
                    if (out_fault_cr2 != NULL) {
                        *out_fault_cr2 = fcr2;
                    }
                    if ((eflags & 0x0200UL) != 0UL) {
                        cpu_sti();
                    }
                    return pid;
                }
                break;
            }
        }
        if ((eflags & 0x0200UL) != 0UL) {
            cpu_sti();
        }
        if (!found) {
            return -1;
        }
        sched_yield();
    }
}

int sched_wait(int pid, int *out_exit_code)
{
    return sched_wait_ex(pid, out_exit_code, NULL, NULL);
}

static int sched_pick_next(void)
{
    int step;
    int idle_slot = -1;

    /* Round-robin across non-idle READY tasks starting after cur_task_idx */
    for (step = 1; step <= SCHED_MAX_TASKS; step++) {
        int idx = (cur_task_idx + step) % SCHED_MAX_TASKS;
        if (task_table[idx].state == TASK_STATE_READY) {
            if (task_table[idx].is_idle) {
                idle_slot = idx;
            } else {
                return idx;
            }
        }
    }

    /* Keep current task if it is still runnable (and non-idle) */
    if (cur_task_idx >= 0 &&
        task_table[cur_task_idx].state == TASK_STATE_RUNNING &&
        !task_table[cur_task_idx].is_idle) {
        return cur_task_idx;
    }

    if (idle_slot >= 0) {
        return idle_slot;
    }
    if (cur_task_idx >= 0 && task_table[cur_task_idx].state == TASK_STATE_RUNNING) {
        return cur_task_idx;
    }
    return 1; /* kidle fallback */
}

static uint32_t sched_switch_to(struct isr_regs *regs, int next_idx)
{
    int prev_idx = cur_task_idx;

    if (prev_idx < 0 || next_idx < 0 || next_idx == prev_idx) {
        return (uint32_t)(uintptr_t)regs;
    }

    task_table[prev_idx].saved_esp = (uint32_t)(uintptr_t)regs;
    if (task_table[prev_idx].state == TASK_STATE_RUNNING) {
        task_table[prev_idx].state = TASK_STATE_READY;
    }

    cur_task_idx = next_idx;
    task_table[next_idx].state = TASK_STATE_RUNNING;
    task_table[next_idx].switches++;
    cur_slice_ticks = 0U;
    total_ctx_switches++;

    /* xv6 switchuvm/switchkvm: update hardware TSS ESP0 and CR3 page directory */
    tss_set_esp0(task_table[next_idx].stack_top);
    vm_switch(task_table[next_idx].pgdir);

    return task_table[next_idx].saved_esp;
}

uint32_t sched_handle_user_fault(struct isr_regs *regs, uint32_t cr2)
{
    int sig = 11; /* SIGSEGV */
    int next_idx;

    if (regs->int_no == 0U) {
        sig = 8; /* SIGFPE */
    } else if (regs->int_no == 6U) {
        sig = 4; /* SIGILL */
    }

    if (cur_task_idx > 1 && cur_task_idx < SCHED_MAX_TASKS) {
        task_table[cur_task_idx].fault_vec = regs->int_no;
        task_table[cur_task_idx].fault_cr2 = cr2;
        task_table[cur_task_idx].exit_code = 128 + sig;
        task_table[cur_task_idx].state     = TASK_STATE_ZOMBIE;
        preempt_lock_depth = 0;
    }

    next_idx = sched_pick_next();
    return sched_switch_to(regs, next_idx);
}

uint32_t sched_reschedule(struct isr_regs *regs)
{
    int next_idx;

    if (!sched_enabled || cur_task_idx < 0) {
        return (uint32_t)(uintptr_t)regs;
    }
    if (preempt_lock_depth > 0 && task_table[cur_task_idx].state == TASK_STATE_RUNNING) {
        return (uint32_t)(uintptr_t)regs;
    }

    next_idx = sched_pick_next();
    return sched_switch_to(regs, next_idx);
}

uint32_t sched_on_tick(struct isr_regs *regs)
{
    uint32_t now;
    int i;
    int next_idx;

    if (!sched_enabled || cur_task_idx < 0) {
        return (uint32_t)(uintptr_t)regs;
    }

    now = pit_get_ticks();
    task_table[cur_task_idx].ticks++;
    cur_slice_ticks++;

    /* Wake any sleeping tasks whose deadline has elapsed */
    for (i = 0; i < SCHED_MAX_TASKS; i++) {
        if (task_table[i].state == TASK_STATE_SLEEPING &&
            (int32_t)(now - task_table[i].wake_tick) >= 0) {
            task_table[i].state = TASK_STATE_READY;
        }
    }

    if (preempt_lock_depth > 0) {
        return (uint32_t)(uintptr_t)regs;
    }

    /* Preempt immediately if running kidle and a real task became ready, or if quantum expired */
    if (!task_table[cur_task_idx].is_idle && cur_slice_ticks < SCHED_QUANTUM_TICKS) {
        return (uint32_t)(uintptr_t)regs;
    }

    next_idx = sched_pick_next();
    if (next_idx == cur_task_idx) {
        cur_slice_ticks = 0U;
        return (uint32_t)(uintptr_t)regs;
    }
    return sched_switch_to(regs, next_idx);
}

void sched_check_stack_guards(const char *ctx)
{
    int i;

    if (!sched_enabled) {
        return;
    }
    for (i = 1; i < SCHED_MAX_TASKS; i++) {
        if (task_table[i].state != TASK_STATE_UNUSED && !sched_guard_ok(task_stack_bases[i])) {
            cpu_cli();
            kprintf("\n[PANIC] Task stack overflow detected in %s: PID %d (%s) guard corrupted!\n",
                    ctx, task_table[i].pid, task_table[i].name);
            for (;;) {
                cpu_hlt();
            }
        }
    }
}

int sched_get_task_count(void)
{
    int i;
    int cnt = 0;
    for (i = 0; i < SCHED_MAX_TASKS; i++) {
        if (task_table[i].state != TASK_STATE_UNUSED) {
            cnt++;
        }
    }
    return cnt;
}

int sched_get_task_info(int slot, struct sched_task_info *out)
{
    uint32_t eflags;
    const struct sched_task *t;

    if (slot < 0 || slot >= SCHED_MAX_TASKS || out == NULL) {
        return -1;
    }

    eflags = cpu_eflags();
    cpu_cli();
    sched_reap_detached();
    t = &task_table[slot];
    if (t->state == TASK_STATE_UNUSED) {
        if ((eflags & 0x0200UL) != 0UL) {
            cpu_sti();
        }
        return -1;
    }
    out->pid        = t->pid;
    out->state      = t->state;
    out->is_user    = t->is_user;
    out->pgdir_phys = (uint32_t)(uintptr_t)t->pgdir;
    out->ticks      = t->ticks;
    out->switches   = t->switches;
    out->stack_size = t->stack_top - t->stack_base;
    if (t->saved_esp >= t->stack_base && t->saved_esp <= t->stack_top) {
        out->stack_used = t->stack_top - t->saved_esp;
    } else {
        out->stack_used = 0U;
    }
    memcpy(out->name, t->name, SCHED_NAME_MAX);
    if ((eflags & 0x0200UL) != 0UL) {
        cpu_sti();
    }
    return 0;
}

uint32_t sched_get_total_switches(void)
{
    return total_ctx_switches;
}

/*
 * Preemptive multitasking self-test:
 * Spawns two pure CPU-bound workers that NEVER call sched_yield() or sleep.
 * Both must advance concurrently and interleave solely via 1000 Hz PIT IRQ0 preemption.
 */
struct sched_test_state {
    volatile uint32_t stop_tick;
    volatile uint32_t work_a;
    volatile uint32_t work_b;
    volatile uint32_t last_owner;
    volatile uint32_t interleaves;
};

static struct sched_test_state st_state;

static void sched_selftest_worker(void *arg)
{
    uint32_t id = (uint32_t)(uintptr_t)arg;

    /* Tight CPU-bound loop with ZERO voluntary yields to prove hardware timer preemption */
    while ((int32_t)(pit_get_ticks() - st_state.stop_tick) < 0) {
        if (st_state.last_owner != id) {
            st_state.last_owner = id;
            st_state.interleaves++;
        }
        if (id == 1U) {
            st_state.work_a++;
        } else {
            st_state.work_b++;
        }
    }
}

int sched_run_selftest(void)
{
    uint32_t sw_before;
    uint32_t sw_delta;
    int pid_a, pid_b;
    int rc_a = -1, rc_b = -1;

    if (!sched_enabled) {
        return -1;
    }

    st_state.work_a      = 0U;
    st_state.work_b      = 0U;
    st_state.last_owner  = 0U;
    st_state.interleaves = 0U;
    st_state.stop_tick   = pit_get_ticks() + 60U; /* 60 ms = ~6 quanta across workers */
    sw_before            = total_ctx_switches;

    pid_a = sched_spawn("preemptA", sched_selftest_worker, (void *)(uintptr_t)1U);
    pid_b = sched_spawn("preemptB", sched_selftest_worker, (void *)(uintptr_t)2U);
    if (pid_a < 0 || pid_b < 0) {
        if (pid_a >= 0) {
            (void)sched_kill(pid_a);
            (void)sched_wait(pid_a, NULL);
        }
        if (pid_b >= 0) {
            (void)sched_kill(pid_b);
            (void)sched_wait(pid_b, NULL);
        }
        return -1;
    }

    (void)sched_wait(pid_a, &rc_a);
    (void)sched_wait(pid_b, &rc_b);

    sw_delta = total_ctx_switches - sw_before;
    if (st_state.work_a == 0U || st_state.work_b == 0U ||
        st_state.interleaves < 2U || sw_delta < 3U) {
        kprintf("[TEST] Preemptive multitasking self-test FAILED (A=%u B=%u inter=%u sw=%u)\n",
                st_state.work_a, st_state.work_b, st_state.interleaves, sw_delta);
        return -1;
    }

    kprintf("[TEST] Preemptive multitasking self-test PASSED (A=%u B=%u interleaves=%u ctx_sw=%u)\n",
            st_state.work_a, st_state.work_b, st_state.interleaves, sw_delta);
    return 0;
}

/*
 * xv6-style hardware task protection self-test:
 * Verifies Ring-3 syscalls, kernel-memory #PF isolation, read-only code #PF,
 * privileged instruction #GP, unmapped stack guard page #PF, per-task CR3 memory
 * isolation, and syscall kernel-pointer rejection (-EFAULT).
 */
int sched_run_prot_selftest(void)
{
    /* 1. Valid Ring-3 task: SYS_getpid (20), verify > 0, SYS_exit (1, 42) */
    static const uint8_t code_valid[] = {
        0xB8U, 0x14U, 0x00U, 0x00U, 0x00U, /* mov $20, %eax (SYS_getpid) */
        0xCDU, 0x80U,                      /* int $0x80 */
        0x85U, 0xC0U,                      /* test %eax, %eax */
        0x7EU, 0x0CU,                      /* jle fail */
        0xBBU, 0x2AU, 0x00U, 0x00U, 0x00U, /* mov $42, %ebx */
        0xB8U, 0x01U, 0x00U, 0x00U, 0x00U, /* mov $1, %eax (SYS_exit) */
        0xCDU, 0x80U,                      /* int $0x80 */
        /* fail: */
        0xBBU, 0x01U, 0x00U, 0x00U, 0x00U, /* mov $1, %ebx */
        0xB8U, 0x01U, 0x00U, 0x00U, 0x00U, /* mov $1, %eax */
        0xCDU, 0x80U                       /* int $0x80 */
    };

    /* 2. Ring-3 illegal read of kernel .text at 0x00100000 -> hardware #PF (14) */
    static const uint8_t code_kread[] = {
        0xA1U, 0x00U, 0x00U, 0x10U, 0x00U, /* mov 0x00100000, %eax */
        0xBBU, 0x00U, 0x00U, 0x00U, 0x00U, /* mov $0, %ebx */
        0xB8U, 0x01U, 0x00U, 0x00U, 0x00U, /* mov $1, %eax */
        0xCDU, 0x80U                       /* int $0x80 */
    };

    /* 3. Ring-3 illegal write to read-only user code page 0x40000000 -> hardware #PF (14) */
    static const uint8_t code_rowrite[] = {
        0xC7U, 0x05U, 0x00U, 0x00U, 0x00U, 0x40U, 0x78U, 0x56U, 0x34U, 0x12U,
                                           /* movl $0x12345678, 0x40000000 */
        0xBBU, 0x00U, 0x00U, 0x00U, 0x00U,
        0xB8U, 0x01U, 0x00U, 0x00U, 0x00U,
        0xCDU, 0x80U
    };

    /* 4. Ring-3 privileged instruction (cli) -> hardware #GP (13) */
    static const uint8_t code_priv[] = {
        0xFAU,                             /* cli (DPL0/IOPL0 required -> #GP) */
        0xBBU, 0x00U, 0x00U, 0x00U, 0x00U,
        0xB8U, 0x01U, 0x00U, 0x00U, 0x00U,
        0xCDU, 0x80U
    };

    /* 5. Ring-3 user stack overflow into unmapped guard page 0x40002FFC -> hardware #PF (14) */
    static const uint8_t code_guard[] = {
        0xC7U, 0x05U, 0xFCU, 0x2FU, 0x00U, 0x40U, 0xEFU, 0xBEU, 0xADU, 0xDEU,
                                           /* movl $0xDEADBEEF, 0x40002FFC */
        0xBBU, 0x00U, 0x00U, 0x00U, 0x00U,
        0xB8U, 0x01U, 0x00U, 0x00U, 0x00U,
        0xCDU, 0x80U
    };

    /*
     * 6. Concurrent Ring-3 isolation + syscall pointer validation:
     *    - Reads unique arg from %ebx (also stored at 0x40001000)
     *    - Loops 8 times calling SYS_sched_yield (158) and checking 0x40001000 == %esi
     *    - Calls SYS_write(1, 0x00100000, 16) and verifies return == -14 (-EFAULT)
     *    - Exits with 0 on success, 1 on failure
     */
    static const uint8_t code_iso[] = {
        0x89U, 0xDEU,                               /* mov %ebx, %esi (save magic) */
        0x89U, 0x35U, 0x00U, 0x10U, 0x00U, 0x40U,   /* mov %esi, 0x40001000 */
        0xBFU, 0x08U, 0x00U, 0x00U, 0x00U,          /* mov $8, %edi (8 yields) */
        /* loop (offset 13): */
        0xB8U, 0x9EU, 0x00U, 0x00U, 0x00U,          /* mov $158, %eax (SYS_sched_yield) */
        0xCDU, 0x80U,                               /* int $0x80 */
        0x3BU, 0x35U, 0x00U, 0x10U, 0x00U, 0x40U,   /* cmp 0x40001000, %esi */
        0x75U, 0x27U,                               /* jne fail (+39 -> offset 67) */
        0x4FU,                                      /* dec %edi */
        0x75U, 0xEEU,                               /* jnz loop (-18 -> offset 13) */
        /* Test syscall rejection of kernel pointer: write(1, 0x00100000, 16) -> -14 (-EFAULT) */
        0xB8U, 0x04U, 0x00U, 0x00U, 0x00U,          /* mov $4, %eax (SYS_write) */
        0xBBU, 0x01U, 0x00U, 0x00U, 0x00U,          /* mov $1, %ebx (stdout) */
        0xB9U, 0x00U, 0x00U, 0x10U, 0x00U,          /* mov $0x00100000, %ecx (kernel addr!) */
        0xBAU, 0x10U, 0x00U, 0x00U, 0x00U,          /* mov $16, %edx */
        0xCDU, 0x80U,                               /* int $0x80 */
        0x83U, 0xF8U, 0xF2U,                        /* cmp $-14, %eax (-LINUX_EFAULT) */
        0x75U, 0x09U,                               /* jne fail (+9 -> offset 67) */
        0x31U, 0xDBU,                               /* xor %ebx, %ebx (exit 0) */
        0xB8U, 0x01U, 0x00U, 0x00U, 0x00U,          /* mov $1, %eax (SYS_exit) */
        0xCDU, 0x80U,                               /* int $0x80 */
        /* fail (offset 67): */
        0xBBU, 0x01U, 0x00U, 0x00U, 0x00U,          /* mov $1, %ebx */
        0xB8U, 0x01U, 0x00U, 0x00U, 0x00U,          /* mov $1, %eax */
        0xCDU, 0x80U                                /* int $0x80 */
    };

    int pid, pid2;
    int rc = -1, rc2 = -1;
    uint32_t fvec = 0U, fcr2 = 0U;
    uint32_t fvec2 = 0U, fcr22 = 0U;

    if (!sched_enabled || !vm_is_enabled()) {
        return -1;
    }

    /* 1. Valid Ring-3 execution */
    pid = sched_spawn_user_code("u_valid", code_valid, (uint32_t)sizeof(code_valid), 0U, FALSE);
    if (pid < 0 || sched_wait_ex(pid, &rc, &fvec, &fcr2) < 0 || rc != 42 || fvec != 0U) {
        kprintf("[TEST] Task protection #1 (Ring-3 syscall) FAILED (rc=%d vec=%u)\n", rc, fvec);
        return -1;
    }

    /* 2. Kernel memory read from Ring 3 -> #PF at 0x00100000 */
    pid = sched_spawn_user_code("u_kread", code_kread, (uint32_t)sizeof(code_kread), 0U, FALSE);
    if (pid < 0 || sched_wait_ex(pid, &rc, &fvec, &fcr2) < 0 || fvec != 14U || fcr2 != 0x00100000UL) {
        kprintf("[TEST] Task protection #2 (kernel read #PF) FAILED (rc=%d vec=%u cr2=0x%08x)\n",
                rc, fvec, fcr2);
        return -1;
    }

    /* 3. Write to read-only code page from Ring 3 -> #PF at 0x40000000 */
    pid = sched_spawn_user_code("u_rowr", code_rowrite, (uint32_t)sizeof(code_rowrite), 0U, FALSE);
    if (pid < 0 || sched_wait_ex(pid, &rc, &fvec, &fcr2) < 0 || fvec != 14U || fcr2 != VM_UTASK_CODE_VA) {
        kprintf("[TEST] Task protection #3 (RO code #PF) FAILED (rc=%d vec=%u cr2=0x%08x)\n",
                rc, fvec, fcr2);
        return -1;
    }

    /* 4. Privileged instruction (cli) from Ring 3 -> #GP (13) */
    pid = sched_spawn_user_code("u_priv", code_priv, (uint32_t)sizeof(code_priv), 0U, FALSE);
    if (pid < 0 || sched_wait_ex(pid, &rc, &fvec, &fcr2) < 0 || fvec != 13U) {
        kprintf("[TEST] Task protection #4 (priv cli #GP) FAILED (rc=%d vec=%u)\n", rc, fvec);
        return -1;
    }

    /* 5. Stack overflow into unmapped guard page -> #PF at 0x40002FFC */
    pid = sched_spawn_user_code("u_guard", code_guard, (uint32_t)sizeof(code_guard), 0U, FALSE);
    if (pid < 0 || sched_wait_ex(pid, &rc, &fvec, &fcr2) < 0 || fvec != 14U || fcr2 != 0x40002FFCUL) {
        kprintf("[TEST] Task protection #5 (stack guard #PF) FAILED (rc=%d vec=%u cr2=0x%08x)\n",
                rc, fvec, fcr2);
        return -1;
    }

    /* 6. Concurrent per-task CR3 address-space isolation + syscall -EFAULT check */
    pid  = sched_spawn_user_code("u_isoA", code_iso, (uint32_t)sizeof(code_iso), 0x1111AAAAUL, FALSE);
    pid2 = sched_spawn_user_code("u_isoB", code_iso, (uint32_t)sizeof(code_iso), 0x2222BBBBUL, FALSE);
    if (pid < 0 || pid2 < 0) {
        if (pid >= 0) {
            (void)sched_kill(pid);
            (void)sched_wait(pid, NULL);
        }
        if (pid2 >= 0) {
            (void)sched_kill(pid2);
            (void)sched_wait(pid2, NULL);
        }
        return -1;
    }
    (void)sched_wait_ex(pid, &rc, &fvec, &fcr2);
    (void)sched_wait_ex(pid2, &rc2, &fvec2, &fcr22);
    if (rc != 0 || fvec != 0U || rc2 != 0 || fvec2 != 0U) {
        kprintf("[TEST] Task protection #6 (CR3 isolation + EFAULT) FAILED (A=%d/%u B=%d/%u)\n",
                rc, fvec, rc2, fvec2);
        return -1;
    }

    kprintf("[TEST] xv6-style hardware task protection self-test PASSED (6/6: Ring-3 syscall, kread #PF, RO-code #PF, priv #GP, stack-guard #PF, CR3 isolation + EFAULT)\n");
    return 0;
}
