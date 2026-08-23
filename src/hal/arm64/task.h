#ifndef ZEDBSD_HAL_ARM64_TASK_H
#define ZEDBSD_HAL_ARM64_TASK_H

#include <hal/hal.h>
#include "int.h"

#define ARM64_SYS_STACK_SIZE 16384U
struct arm64_task {
	struct arm64_task *next;
	hal_space_t space;
	int run_cpu;
	void *sys_stack;
	void *sys_stack_allocation;
	uintptr_t resume_sp;
	uint8_t fpregs[528] __attribute__((aligned(16)));
	void *private_data;
	uintptr_t tls;
	void *active_user_frame;
	struct arm64_exception_frame signal_frame[HAL_SIGNAL_NEST_MAX];
	uint8_t signal_fpregs[HAL_SIGNAL_NEST_MAX][528]
	    __attribute__((aligned(16)));
	uint32_t signal_token[HAL_SIGNAL_NEST_MAX];
	unsigned signal_depth;
};
void asm_task_dispatch(uintptr_t *save,const uintptr_t *load);
void arm64_fp_save(void *state);
void arm64_fp_restore(const void *state);
void arm64_kernel_task_entry(void);
void arm64_user_task_entry(void);
void arm64_user_frame_entry(void);
void arm64_task_returned(void) __attribute__((noreturn));
void arm64_context_selftest(void);
void arm64_task_enter_user_frame(void *);
void arm64_task_leave_user_frame(void);

#endif
