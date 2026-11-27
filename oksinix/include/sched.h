/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Preemptive Round-Robin Multitasking Scheduler
 */

#ifndef OKSINIX_SCHED_H
#define OKSINIX_SCHED_H

#include "types.h"
#include "idt.h"

#define SCHED_MAX_TASKS     8
#define SCHED_NAME_MAX      24
#define SCHED_QUANTUM_TICKS 10U   /* 10 ms time slice at 1000 Hz PIT */

#define TASK_STATE_UNUSED   0U
#define TASK_STATE_READY    1U
#define TASK_STATE_RUNNING  2U
#define TASK_STATE_SLEEPING 3U
#define TASK_STATE_ZOMBIE   4U

typedef void (*sched_entry_t)(void *arg);

struct sched_task_info {
    int      pid;
    uint32_t state;
    bool_t   is_user;
    uint32_t pgdir_phys;
    uint32_t ticks;
    uint32_t switches;
    uint32_t stack_size;
    uint32_t stack_used;
    char     name[SCHED_NAME_MAX];
};

void      sched_init(void);
bool_t    sched_is_enabled(void);
int       sched_spawn(const char *name, sched_entry_t entry, void *arg);
int       sched_spawn_ex(const char *name, sched_entry_t entry, void *arg, bool_t detached);
int       sched_spawn_user_code(const char *name, const void *code, uint32_t code_len,
                                uint32_t arg_val, bool_t detached);
void      sched_exit(int exit_code);
void      sched_yield(void);
void      sched_sleep_ms(uint32_t ms);
int       sched_kill(int pid);
int       sched_wait(int pid, int *out_exit_code);
int       sched_wait_ex(int pid, int *out_exit_code,
                        uint32_t *out_fault_vec, uint32_t *out_fault_cr2);
int       sched_getpid(void);
void      sched_set_current_name(const char *name);

bool_t    sched_current_is_user(void);
uint32_t *sched_current_pgdir(void);
bool_t    sched_current_needs_resched(void);
void      sched_mark_current_exit_isr(int exit_code);
void      sched_mark_current_yield_isr(void);
uint32_t  sched_handle_user_fault(struct isr_regs *regs, uint32_t cr2);

void      sched_preempt_disable(void);
void      sched_preempt_enable(void);

uint32_t  sched_on_tick(struct isr_regs *regs);
uint32_t  sched_reschedule(struct isr_regs *regs);
void      sched_check_stack_guards(const char *ctx);

int       sched_get_task_count(void);
int       sched_get_task_info(int slot, struct sched_task_info *out);
uint32_t  sched_get_total_switches(void);
int       sched_run_selftest(void);
int       sched_run_prot_selftest(void);

#endif /* OKSINIX_SCHED_H */
