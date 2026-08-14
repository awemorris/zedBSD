#ifndef ZEDBSD_HAL_AMD64_TASK_H
#define ZEDBSD_HAL_AMD64_TASK_H

#include <hal/hal.h>

#define AMD64_SYS_STACK_SIZE 16384U

struct amd64_task {
	struct amd64_task *next;
	hal_space_t space;
	int run_cpu;
	void *sys_stack;
	void *sys_stack_allocation;
	uintptr_t resume_rsp;
	uint8 fpregs[512 + 15];
	void *private_data;
	uintptr_t tls;
};

void asm_task_dispatch(uintptr_t *save, const uintptr_t *load);
void amd64_kernel_task_entry(void);
void amd64_user_task_entry(void);
void amd64_task_returned(void) __attribute__((noreturn));

#endif
