/* MC68030 exception, syscall, and autovector dispatch. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <errno.h>
#include <hal/hal.h>
#include "exception.h"
#include "frame-offsets.h"
#include "mmu030.h"

#define M68K_SR_SUPERVISOR 0x2000U
#define M68K_TRAP_CAUSE_COUNT 5U

extern char m68k_vector_table[];
void m68k_set_vbr(void *table);
void m68k_task_enter_user_frame(struct m68k_saved_frame *frame);
void m68k_task_leave_user_frame(void);

static hal_trap_handler_t trap_handlers[M68K_TRAP_CAUSE_COUNT];
static hal_syscall_handler_t syscall_handler;

int __attribute__((weak))
x68k_irq_dispatch(unsigned vector)
{
	(void)vector;
	return 0;
}

static uint16_t
frame_word(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t
frame_long(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	    (uint32_t)p[2] << 8 | p[3];
}

static uint32_t
kernel_fault_vector(unsigned vector, uint32_t cause)
{
	if (cause == HAL_TRAP_CAUSE_PAGE_FAULT)
		return 14U;
	if (cause == HAL_TRAP_CAUSE_BREAKPOINT)
		return 3U;
	if (cause == HAL_TRAP_CAUSE_ILLEGAL_INSN)
		return 6U;
	if (vector == 5U || vector == 6U || vector == 7U ||
	    (vector >= 48U && vector <= 55U))
		return 0U;
	return 17U;
}

void
m68k_int_init(void)
{
	m68k_set_vbr(m68k_vector_table);
}

void
m68k_exception_dispatch(struct m68k_saved_frame *frame)
{
	uint8_t *hardware = frame->hardware;
	uint16_t sr = frame_word(hardware);
	uint16_t format_vector = frame_word(hardware + 6U);
	unsigned vector = (format_vector & 0x0fffU) >> 2;
	int user = (sr & M68K_SR_SUPERVISOR) == 0;

	/*
	 * Unlike the other trap mechanisms, a 68k user exception can preserve
	 * IPL0 in the live supervisor SR.  Mask locally while the HAL owns the
	 * saved-frame bookkeeping; generic callbacks open their own IRQ windows.
	 */
	if (user)
		(void)hal_irq_disable();

	if ((vector >= 25U && vector <= 31U) ||
	    (vector >= 0x40U && vector <= 0x4fU)) {
		unsigned irq = vector >= 0x40U ? vector : vector - 24U;
		if (user)
			m68k_task_enter_user_frame(frame);
		if (!x68k_irq_dispatch(irq))
			hal_printf("X68k: unhandled IRQ vector %u\n", irq);
		if (user) {
			kernel_user_return_handler();
			m68k_task_leave_user_frame();
		}
		return;
	}

	if (vector == 32U && user) {
		uintptr_t arguments[HAL_SYSCALL_ARGS];
		kernel_user_int_handler(0xc2U, 3U,
		    frame_long(hardware + 2U), frame->d[0]);
		arguments[0] = frame->d[1];
		arguments[1] = frame->d[2];
		arguments[2] = frame->d[3];
		arguments[3] = frame->d[4];
		arguments[4] = frame->d[5];
		arguments[5] = frame->a[0];
		m68k_task_enter_user_frame(frame);
		frame->d[0] = (uint32_t)(syscall_handler != NULL ?
		    syscall_handler(frame->d[0], arguments) : -ENOSYS);
		kernel_user_return_handler();
		m68k_task_leave_user_frame();
		return;
	}

	{
		uintptr_t fault_address = 0;
		uint16_t ssw = 0;
		uint16_t mmusr = 0;
		size_t frame_size = m68k_exception_frame_size(format_vector);
		uint32_t cause;
		uint32_t access = HAL_TRAP_MODE_READ;
		uintptr_t error_code = 0;

		if (m68k_exception_fault_address(hardware, frame_size,
		    &fault_address, &ssw) == 0 && vector == 2U) {
			access = m68k_exception_access(ssw);
			switch (access) {
			case HAL_TRAP_MODE_WRITE:
				mmusr = (uint16_t)m68k030_test_user_write(
				    fault_address);
				break;
			case HAL_TRAP_MODE_EXEC:
				mmusr = (uint16_t)m68k030_test_user_exec(
				    fault_address);
				break;
			default:
				mmusr = (uint16_t)m68k030_test_user_read(
				    fault_address);
				break;
			}
		}
		cause = m68k_exception_cause(vector, mmusr);
		if (user) {
			int handled;
			if (access == HAL_TRAP_MODE_EXEC)
				error_code = 0x10U;
			else if (access == HAL_TRAP_MODE_WRITE)
				error_code = 2U;
			m68k_task_enter_user_frame(frame);
			handled = kernel_user_fault_handler(
			    kernel_fault_vector(vector, cause), 3U,
			    frame_long(hardware + 2U), error_code,
			    fault_address) == HAL_TRAP_RET_SUCCESS;
			if (handled) {
				kernel_user_return_handler();
				m68k_task_leave_user_frame();
				return;
			}
			m68k_task_leave_user_frame();
			HAL_FATAL("m68k user fault handler returned");
		}
		if (cause < M68K_TRAP_CAUSE_COUNT &&
		    trap_handlers[cause] != NULL &&
		    trap_handlers[cause]((void *)(uintptr_t)
		    frame_long(hardware + 2U), (void *)fault_address,
		    (int)access) == HAL_TRAP_RET_SUCCESS)
			return;
	}

	hal_printf("m68k supervisor exception vector=%u format=%x sr=%x pc=%x\n",
	    vector, format_vector >> 12, sr, frame_long(hardware + 2U));
	HAL_FATAL("unhandled m68k supervisor exception");
}

void
hal_set_trap_handler(int trap, hal_trap_handler_t handler)
{
	if (trap < 0 || trap >= (int)M68K_TRAP_CAUSE_COUNT)
		HAL_FATAL("invalid m68k trap handler");
	trap_handlers[trap] = handler;
}

void
hal_syscall_set_handler(hal_syscall_handler_t handler)
{
	syscall_handler = handler;
}
