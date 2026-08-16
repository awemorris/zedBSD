#ifndef ZEDBSD_HAL_SPARCV9_TASK_H
#define ZEDBSD_HAL_SPARCV9_TASK_H

#include <hal/hal.h>
#include "trap.h"
#define SPARCV9_SYS_STACK_SIZE 32768U
#define SPARCV9_NWINDOWS 8U
struct sparcv9_window_context{uint64 window[SPARCV9_NWINDOWS][16];uint64 cwp,cansave,canrestore,otherwin,cleanwin,wstate;uint64 global[8];};
struct sparcv9_task{struct sparcv9_task*next;hal_space_t space;int run_cpu;void*stack_allocation;void*stack;uintptr_t trap_sp;void*private_data;uintptr_t tls;uint64 trap_level,trap_pc,trap_next_pc,trap_state,trap_type;struct sparcv9_window_context windows,trap_windows;struct sparcv9_user_trap_frame*active_user_frame;uint64 active_pc,active_next_pc,active_tstate,active_trap_type;struct sparcv9_window_context signal_windows[HAL_SIGNAL_NEST_MAX];struct sparcv9_user_trap_frame signal_frame[HAL_SIGNAL_NEST_MAX];uint64 signal_pc[HAL_SIGNAL_NEST_MAX],signal_next_pc[HAL_SIGNAL_NEST_MAX],signal_tstate[HAL_SIGNAL_NEST_MAX];uint32 signal_token[HAL_SIGNAL_NEST_MAX],signal_depth;};
extern uintptr_t sparcv9_current_trap_sp;
extern struct sparcv9_window_context *sparcv9_current_user_windows;
void sparcv9_task_dispatch(struct sparcv9_window_context *save,const struct sparcv9_window_context *load);
uintptr_t sparcv9_tls_read(void);
void sparcv9_tls_write(uintptr_t);
void sparcv9_kernel_task_entry(void);
void sparcv9_user_task_entry(void);
void sparcv9_user_fork_entry(void);
void sparcv9_user_task_prepare(uintptr_t entry, uintptr_t stack_pointer);
void sparcv9_task_enter_user_frame(struct sparcv9_user_trap_frame *,
	uintptr_t, uintptr_t, uint64, uint64);
void sparcv9_task_leave_user_frame(void);
void sparcv9_task_returned(void) __attribute__((noreturn));
void sparcv9_context_selftest(void);
#endif
