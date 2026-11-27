/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Preemptive Round-Robin Multitasking Scheduler
 */

#include "sched.h"
#include "gdt.h"
#include "pit.h"
#include "io.h"
#include "serial.h"
#include "string.h"

#define TASK_STACK_DWORDS 4096U /* 16 KiB per spawned kernel task */
#define TASK_GUARD_W0     0xDEADBEEFUL
#define TASK_GUARD_W1     0xCAFEBABEUL
#define TASK_GUARD_W2     0x57AC4600UL
#define TASK_GUARD_W3     0xBADC0FFEUL

struct sched_task {
    int            pid;
    uint32_t       state;
    bool_t         is_idle;
    bool_t         detached;
    int            exit_code;
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
static uint32_t          task_stacks[SCHED_MAX_TASKS][TASK_STACK_DWORDS];
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
    uint32_t top_addr;

    memset(task_table, 0, sizeof(task_table));
    cur_task_idx       = 0;
    next_task_pid      = 2;
    preempt_lock_depth = 0;
    cur_slice_ticks    = 0U;
    total_ctx_switches = 0U;

    /* Slot 0: primary boot / init / shell task running on boot kernel_stack */
    boot_t = &task_table[0];
    boot_t->pid        = 1;
    boot_t->state      = TASK_STATE_RUNNING;
    boot_t->is_idle    = FALSE;
    boot_t->detached   = FALSE;
    boot_t->stack_base = (uint32_t)(uintptr_t)kernel_stack_bottom;
    boot_t->stack_top  = (uint32_t)(uintptr_t)kernel_stack_top;
    sched_copy_name(boot_t->name, "/bin/init");

    /* Slot 1: lowest-priority idle task (PID 0), scheduled only when no other task is READY */
    idle_t = &task_table[1];
    sched_arm_guard(task_stacks[1]);
    top_addr = (uint32_t)(uintptr_t)&task_stacks[1][TASK_STACK_DWORDS];
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
    idle_t->detached   = TRUE;
    idle_t->saved_esp  = (uint32_t)(uintptr_t)regs;
    idle_t->stack_base = (uint32_t)(uintptr_t)task_stacks[1];
    idle_t->stack_top  = top_addr;
    idle_t->entry      = sched_idle_entry;
    idle_t->arg        = NULL;
    sched_copy_name(idle_t->name, "kidle");

    for (i = 2; i < SCHED_MAX_TASKS; i++) {
        task_table[i].state = TASK_STATE_UNUSED;
    }

    sched_enabled = TRUE;
    kprintf("[SCHED] Preemptive multitasking initialized (max_tasks=%u, quantum=%ums, stack=%uKiB)\n",
            (uint32_t)SCHED_MAX_TASKS, (uint32_t)SCHED_QUANTUM_TICKS,
            (uint32_t)((TASK_STACK_DWORDS * 4U) / 1024U));
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
    memset(task_stacks[slot], 0, sizeof(task_stacks[slot]));
    sched_arm_guard(task_stacks[slot]);

    top_addr = (uint32_t)(uintptr_t)&task_stacks[slot][TASK_STACK_DWORDS];
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
    t->detached   = detached;
    t->exit_code  = 0;
    t->saved_esp  = (uint32_t)(uintptr_t)regs;
    t->stack_base = (uint32_t)(uintptr_t)task_stacks[slot];
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
    task_table[cur_task_idx].state     = task_table[cur_task_idx].detached
                                             ? TASK_STATE_UNUSED
                                             : TASK_STATE_ZOMBIE;
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
            task_table[i].state     = task_table[i].detached
                                          ? TASK_STATE_UNUSED
                                          : TASK_STATE_ZOMBIE;
            rc = 0;
            break;
        }
    }
    if ((eflags & 0x0200UL) != 0UL) {
        cpu_sti();
    }
    return rc;
}

int sched_wait(int pid, int *out_exit_code)
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
                    task_table[i].state = TASK_STATE_UNUSED;
                    task_table[i].pid   = 0;
                    if (out_exit_code != NULL) {
                        *out_exit_code = code;
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

    return task_table[next_idx].saved_esp;
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
        if (task_table[i].state != TASK_STATE_UNUSED && !sched_guard_ok(task_stacks[i])) {
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
    t = &task_table[slot];
    if (t->state == TASK_STATE_UNUSED) {
        if ((eflags & 0x0200UL) != 0UL) {
            cpu_sti();
        }
        return -1;
    }
    out->pid        = t->pid;
    out->state      = t->state;
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
