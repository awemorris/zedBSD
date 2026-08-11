/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * i386 CPU-context implementation. Private to the HAL.
 */
#ifndef BOOTS_HAL_I386_TASK_H
#define BOOTS_HAL_I386_TASK_H

#include <hal/hal.h>
#include "asm.h"

#define SYS_STACK_SIZE 8192U

struct task_resume_frame {
	uint32 gs, fs, es, ds;
	uint32 edi, esi, ebp, discarded_esp, ebx, edx, ecx, eax;
	uint32 eflags;
	uint32 ret_eip;
	union {
		struct {
			uint32 eip, cs, eflags, return_eip, param;
		} sys;
		struct {
			uint32 eip, cs, eflags, esp, ss;
		} user;
	} initial;
};

struct task_info {
	struct task_info *next;
	hal_space_t space;
	int run_cpu;
	void *sys_stack;
	struct task_resume_frame *resume_esp;
	uint8 fpregs[512];
	void *private_data;
	uintptr_t tls;
};

void i386_task_init(void);
void asm_task_entrypoint(void);
void asm_task_dispatch(struct task_resume_frame **save,
			struct task_resume_frame **load);

#endif
