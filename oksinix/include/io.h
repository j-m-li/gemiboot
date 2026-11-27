/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix x86 32-bit kernel - Port I/O & CPU control
 */

#ifndef OKSINIX_IO_H
#define OKSINIX_IO_H

#include "types.h"

uint8_t  inb(uint16_t port);
void     outb(uint16_t port, uint8_t val);
uint16_t inw(uint16_t port);
void     outw(uint16_t port, uint16_t val);
uint32_t inl(uint16_t port);
void     outl(uint16_t port, uint32_t val);
void     io_wait(void);

void     cpu_cli(void);
void     cpu_sti(void);
void     cpu_hlt(void);
void     cpu_pause(void);
void     cpu_memory_barrier(void);
uint32_t cpu_eflags(void);
void     cpu_yield(void);

void     cpu_enable_paging(uint32_t pgdir_phys);
void     cpu_load_cr3(uint32_t pgdir_phys);
uint32_t cpu_read_cr3(void);
uint32_t cpu_read_cr2(void);
void     cpu_invlpg(uint32_t vaddr);

void     gdt_flush(const volatile void *gdt_ptr_addr);
void     tss_flush(uint16_t tss_selector);
void     idt_flush(const volatile void *idt_ptr_addr);
int      linux_enter_ring3(uint32_t entry, uint32_t user_esp, uint32_t *saved_esp_out);
void     linux_return_to_host(uint32_t saved_esp, int exit_code);
void     linux_resume_regs(uint32_t regs_esp);
void     linux_call_on_kstack(void (*fn)(void), uint32_t kstack_top);

void     stack_guard_init(void);
void     stack_guard_check(const char *ctx);

#endif /* OKSINIX_IO_H */
