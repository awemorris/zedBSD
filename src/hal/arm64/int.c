#include <hal/hal.h>
#include "asm.h"
#include "int.h"
#include "task.h"
#include "irq.h"
#include "bsp-rpi4/gic.h"
#include <errno.h>

extern char arm64_vectors[];
static hal_trap_handler_t trap_handlers[5];
static hal_syscall_handler_t syscall_handler;
void rpi4_timer_init(void);

void arm64_int_init(void)
{
	__asm__ volatile("msr vbar_el1, %0\n\tisb"::"r"(arm64_vectors):"memory");
	rpi4_gic_init();
	rpi4_timer_init();
	hal_puts("ARM64 EXCEPTION PASS\nARM64 IRQ READY\n");
}

void arm64_sync_handler(struct arm64_exception_frame *f,uint64_t vector)
{
	uint32_t ec=(uint32_t)((f->esr>>26)&0x3f);
	if (vector == 0) {
		int cause = (ec == 0x20 || ec == 0x21 || ec == 0x24 ||
		    ec == 0x25) ? HAL_TRAP_CAUSE_PAGE_FAULT :
		    ec == 0x3c ? HAL_TRAP_CAUSE_BREAKPOINT :
		    (ec == 0x22 || ec == 0x26) ? HAL_TRAP_CAUSE_ALIGNMENT :
		    ec == 0 ? HAL_TRAP_CAUSE_ILLEGAL_INSN :
		    HAL_TRAP_CAUSE_MACHINE_CHECK;
		int mode = (ec == 0x20 || ec == 0x21) ? HAL_TRAP_MODE_EXEC :
		    (f->esr & (1ULL << 6)) ? HAL_TRAP_MODE_WRITE :
		    HAL_TRAP_MODE_READ;
		uintptr_t address = cause == HAL_TRAP_CAUSE_PAGE_FAULT ?
		    (uintptr_t)f->far : 0;

		if (trap_handlers[cause] != NULL &&
		    trap_handlers[cause]((void *)(uintptr_t)f->elr,
		    (void *)address, mode) == HAL_TRAP_RET_SUCCESS)
			return;
	}
	if (vector == 8 && ec == 0x15) {
		uintptr_t args[HAL_SYSCALL_ARGS];
		unsigned i;

		kernel_user_int_handler(0xc2U, 3U, f->elr, f->x[8]);
		for (i = 0; i < HAL_SYSCALL_ARGS; i++)
			args[i] = (uintptr_t)f->x[i];
		arm64_task_enter_user_frame(f);
		/* The generic callback owns accounting and its interruptible window. */
		f->x[0] = (uint64_t)(syscall_handler != NULL ?
			syscall_handler((uint32_t)f->x[8], args) : -ENOSYS);
		kernel_user_return_handler();
		arm64_task_leave_user_frame();
		return;
	}
	if (vector == 8 && (ec == 0x20 || ec == 0x21 ||
	    ec == 0x24 || ec == 0x25)) {
		int handled;
		arm64_task_enter_user_frame(f);
		handled = kernel_user_fault_handler(14U, 3U, f->elr,
		    (ec == 0x20 || ec == 0x21) ? 0x10U :
		    ((f->esr & (1ULL << 6)) ? 2U : 0U), f->far) ==
		    HAL_TRAP_RET_SUCCESS;
		if (handled) {
			kernel_user_return_handler();
			arm64_task_leave_user_frame();
			return;
		}
		arm64_task_leave_user_frame();
		HAL_FATAL("AArch64 user page fault handler returned");
	}
	if (vector == 8) {
		int handled;
		arm64_task_enter_user_frame(f);
		handled = kernel_user_fault_handler((ec == 0x3c) ? 3U :
		    (ec == 0x22 || ec == 0x26) ? 17U : 6U, 3U, f->elr,
		    0, f->far) == HAL_TRAP_RET_SUCCESS;
		if (handled) {
			kernel_user_return_handler();
			arm64_task_leave_user_frame();
			return;
		}
		arm64_task_leave_user_frame();
		HAL_FATAL("AArch64 user fault handler returned");
	}
	hal_printf("ARM64 sync vector=%u ec=%x esr=%llx elr=%llx far=%llx\n",
	    (uint32_t)vector,ec,f->esr,f->elr,f->far);
	HAL_FATAL("unhandled AArch64 synchronous exception");
}

void arm64_irq_handler(struct arm64_exception_frame *f, int from_user)
{
	uint32_t iar,id;
	if (from_user)
		arm64_task_enter_user_frame(f);
	iar=rpi4_gic_ack();id=iar&0x3ff;
	if(id<1020)
		arm64_irq_dispatch(id, (hal_irq_ack_t)iar + 1U);
	if (from_user) {
		kernel_user_return_handler();
		arm64_task_leave_user_frame();
	}
}

void hal_set_trap_handler(int trap,hal_trap_handler_t h){if(trap<0||trap>=5)HAL_FATAL("bad trap");trap_handlers[trap]=h;}
void hal_syscall_set_handler(hal_syscall_handler_t h){syscall_handler=h;}
