#include <hal/hal.h>
#include "asm.h"
#include "int.h"
#include "task.h"
#include "irq.h"
#include "bsp-rpi4/gic.h"
#include <errno.h>

extern char arm64_vectors[];
void rpi4_timer_init(void);

void arm64_int_init(void)
{
	__asm__ volatile("msr vbar_el1, %0\n\tisb"::"r"(arm64_vectors):"memory");
	rpi4_gic_init();
	rpi4_timer_init();
	hal_puts("ARM64 EXCEPTION PASS\nARM64 IRQ READY\n");
}

/* Maps an exception class onto the generic cause, mode and address. */
static void
arm64_classify(const struct arm64_exception_frame *f, uint32_t ec,
    int *cause, int *mode, uintptr_t *address)
{
	*mode = HAL_TRAP_MODE_NONE;
	*address = 0;
	if (ec == 0x20 || ec == 0x21 || ec == 0x24 || ec == 0x25) {
		*cause = HAL_TRAP_CAUSE_PAGE_FAULT;
		*address = (uintptr_t)f->far;
		if (ec == 0x20 || ec == 0x21)
			*mode = HAL_TRAP_MODE_EXEC;
		else if (f->esr & (1ULL << 6))
			*mode = HAL_TRAP_MODE_WRITE;
		else
			*mode = HAL_TRAP_MODE_READ;
	} else if (ec == 0x3c) {
		*cause = HAL_TRAP_CAUSE_BREAKPOINT;
	} else if (ec == 0x22 || ec == 0x26) {
		*cause = HAL_TRAP_CAUSE_ALIGNMENT;
		*address = (uintptr_t)f->far;
	} else if (ec == 0) {
		*cause = HAL_TRAP_CAUSE_ILLEGAL_INSN;
	} else {
		*cause = HAL_TRAP_CAUSE_OTHER;
	}
}

void arm64_sync_handler(struct arm64_exception_frame *f,uint64_t vector)
{
	uint32_t ec=(uint32_t)((f->esr>>26)&0x3f);
	int cause;
	int mode;
	uintptr_t address;

	if (vector == 8 && ec == 0x15) {
		uintptr_t args[HAL_SYSCALL_ARGS];
		unsigned i;

		for (i = 0; i < HAL_SYSCALL_ARGS; i++)
			args[i] = (uintptr_t)f->x[i];
		arm64_task_enter_user_frame(f);
		/* The generic kernel owns accounting and its interruptible window. */
		f->x[0] = (uint64_t)kernel_syscall_handler((uint32_t)f->x[8], args);
		kernel_user_return_handler();
		arm64_task_leave_user_frame();
		return;
	}
	arm64_classify(f, ec, &cause, &mode, &address);
	if (vector == 0) {
		if (kernel_sys_fault_handler(cause, mode, (uintptr_t)f->elr,
		    address, ec, (uintptr_t)f->esr) == HAL_TRAP_RET_SUCCESS)
			return;
	}
	if (vector == 8) {
		int handled;
		arm64_task_enter_user_frame(f);
		handled = kernel_user_fault_handler(cause, mode, (uintptr_t)f->elr,
		    address, ec, (uintptr_t)f->esr) == HAL_TRAP_RET_SUCCESS;
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
