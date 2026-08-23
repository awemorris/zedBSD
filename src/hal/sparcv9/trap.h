#ifndef ZEDBSD_HAL_SPARCV9_TRAP_H
#define ZEDBSD_HAL_SPARCV9_TRAP_H

#include <hal/types.h>

void sparcv9_trap_init(void);
int sparcv9_trap_dispatch(uint64_t trap_type, uintptr_t pc, uintptr_t next_pc,
	uint64_t tstate);
struct sparcv9_user_trap_frame {
	uintptr_t old_sp;
	uint64_t syscall_number;
	uint64_t out[6];
};
int sparcv9_user_trap_dispatch(uint64_t trap_type, uintptr_t pc,
	uintptr_t next_pc, uint64_t tstate, struct sparcv9_user_trap_frame *frame);
void sparcv9_window_selftest(void);

#endif
