#ifndef ZEDBSD_HAL_AMD64_TASK_H
#define ZEDBSD_HAL_AMD64_TASK_H

#include <hal/hal.h>
#include "int.h"

#define AMD64_SYS_STACK_SIZE 16384U

struct amd64_task {
	struct amd64_task *next;
	hal_space_t space;
	int run_cpu;
	hal_cpu_id_t target_cpu;
	void *sys_stack;
	void *sys_stack_allocation;
	uintptr_t resume_rsp;
	uint8 fpregs[512 + 15];
	void *private_data;
	uintptr_t tls;
	void *active_user_frame;
	struct amd64_interrupt_frame signal_frame[HAL_SIGNAL_NEST_MAX];
	uint8 signal_fpregs[HAL_SIGNAL_NEST_MAX][512]
	    __attribute__((aligned(16)));
	uint32 signal_token[HAL_SIGNAL_NEST_MAX];
	unsigned signal_depth;
};

void asm_task_dispatch(uintptr_t *save, const uintptr_t *load);
void amd64_kernel_task_entry(void);
void amd64_user_task_entry(void);
void amd64_user_frame_entry(void);
void amd64_task_returned(void) __attribute__((noreturn));
void amd64_task_enter_user_frame(void *);
void amd64_task_leave_user_frame(void);
void amd64_task_init_cpu(int run_selftest);
void amd64_xmm_load(const void *);
void amd64_xmm_store(void *);

#endif
