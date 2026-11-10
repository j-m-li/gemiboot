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

void     gdt_flush(const volatile void *gdt_ptr_addr);
void     idt_flush(const volatile void *idt_ptr_addr);
int      linux_enter_elf(uint32_t entry, uint32_t user_esp, uint32_t *saved_esp_out);
void     linux_return_to_host(uint32_t saved_esp, int exit_code);
void     linux_resume_regs(uint32_t regs_esp);
void     linux_restore_and_resume(uint32_t dst_stack, const void *src_stack,
                                  uint32_t stack_dwords, uint32_t parent_regs_esp);
void     linux_call_on_kstack(void (*fn)(void), uint32_t kstack_top);
int32_t  linux_int80(uint32_t nr, uint32_t a1, uint32_t a2,
                     uint32_t a3, uint32_t a4, uint32_t a5);

#endif /* OKSINIX_IO_H */
