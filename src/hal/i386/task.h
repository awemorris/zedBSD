/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The private i386 task-context and signal-frame contract.
 */

#ifndef ZEDBSD_HAL_I386_TASK_H
#define ZEDBSD_HAL_I386_TASK_H

#include <hal/hal.h>
#include "asm.h"
#include "int.h"

#define SYS_STACK_SIZE 8192U

struct task_resume_frame {
	uint32_t gs, fs, es, ds;
	uint32_t edi, esi, ebp, discarded_esp, ebx, edx, ecx, eax;
	uint32_t eflags;
	uint32_t ret_eip;
	union {
		struct {
			uint32_t eip, cs, eflags, return_eip, param;
		} sys;
		struct {
			uint32_t eip, cs, eflags, esp, ss;
		} user;
	} initial;
};

struct task_info {
	struct task_info *next;
	hal_space_t space;
	int run_cpu;
	hal_cpu_id_t target_cpu;
	void *sys_stack;
	unsigned owns_stack;
	struct task_resume_frame *resume_esp;
	uint8_t fpregs[512] __attribute__((aligned(16)));
	void *private_data;
	uintptr_t tls;
	void *active_user_frame;
	struct interrupt_frame signal_frame[HAL_SIGNAL_NEST_MAX];
	uint8_t signal_fpregs[HAL_SIGNAL_NEST_MAX][512]
	    __attribute__((aligned(16)));
	uint32_t signal_token[HAL_SIGNAL_NEST_MAX];
	unsigned signal_depth;
};

void i386_task_init(void);
void i386_task_init_secondary(hal_cpu_id_t, uintptr_t);
void asm_task_entrypoint(void);
void asm_task_dispatch(struct task_resume_frame **save, struct task_resume_frame **load);
void i386_task_enter_user_frame(void *);
void i386_task_leave_user_frame(void);

#endif
