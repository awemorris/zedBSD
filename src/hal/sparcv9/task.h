#ifndef ZEDBSD_HAL_SPARCV9_TASK_H
#define ZEDBSD_HAL_SPARCV9_TASK_H

#include <hal/hal.h>
#define SPARCV9_SYS_STACK_SIZE 32768U
#define SPARCV9_NWINDOWS 8U
struct sparcv9_window_context{uint64 window[SPARCV9_NWINDOWS][16];uint64 cwp,cansave,canrestore,otherwin,cleanwin,wstate;uint64 global[8];};
struct sparcv9_task{struct sparcv9_task*next;hal_space_t space;void*stack_allocation;void*stack;uintptr_t trap_sp;void*private_data;uintptr_t tls;uint64 trap_level,trap_pc,trap_next_pc,trap_state,trap_type;struct sparcv9_window_context windows,trap_windows;};
extern uintptr_t sparcv9_current_trap_sp;
extern struct sparcv9_window_context *sparcv9_current_user_windows;
void sparcv9_task_dispatch(struct sparcv9_window_context *save,const struct sparcv9_window_context *load);
void sparcv9_kernel_task_entry(void);
void sparcv9_user_task_entry(void);
void sparcv9_user_task_prepare(uintptr_t entry, uintptr_t stack_pointer);
void sparcv9_task_returned(void) __attribute__((noreturn));
void sparcv9_context_selftest(void);
#endif
