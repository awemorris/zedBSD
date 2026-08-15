/* MC68030 task context. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_HAL_M68K_TASK_H
#define ZEDBSD_HAL_M68K_TASK_H

#include <hal/hal.h>
#include "frame-offsets.h"

#define M68K_KERNEL_STACK_SIZE 8192U
#define M68K_DISPATCH_REG_BYTES 44U
#define M68K_MAX_HARDWARE_FRAME 92U

struct m68k_task {
	struct m68k_task *next;
	void *allocation;
	hal_space_t space;
	void *kernel_stack;
	void *resume_sp;
	void *private_data;
	uintptr_t tls;
	struct m68k_saved_frame *active_user_frame;
	uint8_t signal_frame[M68K_FRAME_HARDWARE_OFFSET +
		M68K_MAX_HARDWARE_FRAME];
	size_t signal_frame_size;
	uint32_t signal_token;
	unsigned signal_depth;
};

void m68k_task_dispatch(void **save_sp, void *load_sp);
void m68k_exception_restore(void);
void m68k_kernel_task_start(void);
void m68k_user_task_start(void);
void m68k_cpu_idle(void);
void m68k_task_enter_user_frame(struct m68k_saved_frame *frame);
void m68k_task_leave_user_frame(void);

#endif
