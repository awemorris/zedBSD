#ifndef ZEDBSD_HAL_AMD64_INT_H
#define ZEDBSD_HAL_AMD64_INT_H

#include <hal/types.h>
#include "defs.h"

struct amd64_interrupt_frame {
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
	uint64_t vector, error_code;
	uint64_t rip, cs, rflags, rsp, ss;
};

void amd64_int_init(void);
void amd64_int_load(void);
void int_handler(struct amd64_interrupt_frame *frame);
extern void *amd64_fault_table[32];
extern void *amd64_irq_table[16];
void amd64_notify_entry(void);
void amd64_tlb_entry(void);
void amd64_error_entry(void);
void amd64_spurious_entry(void);
void amd64_syscall_entry(void);
void amd64_undefined_entry(void);

#endif
