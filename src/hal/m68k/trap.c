/* MC68030 exception, syscall, and autovector dispatch. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <errno.h>
#include <hal/hal.h>
#include <kern/sched.h>
#include "exception.h"
#include "frame-offsets.h"
#include "mmu030.h"

#define M68K_SR_SUPERVISOR 0x2000U

extern char m68k_vector_table[];
void m68k_set_vbr(void *table);
void m68k_task_enter_user_frame(struct m68k_saved_frame *frame);
void m68k_task_leave_user_frame(void);

static hal_trap_handler_t trap_handlers[HAL_TRAP_CAUSE_COUNT];
static hal_syscall_handler_t syscall_handler;
static hal_user_return_handler_t user_return_handler;
static hal_user_int_handler_t user_int_handler;
static hal_user_fault_handler_t user_fault_handler;
static int reschedule_pending;

int __attribute__((weak))
x68k_irq_dispatch(unsigned level)
{
	(void)level;
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

	reschedule_pending = 0;
	if ((vector >= 25U && vector <= 31U) ||
	    (vector >= 0x40U && vector <= 0x4fU)) {
		unsigned level = vector >= 0x40U ? vector : vector - 24U;
		if (user)
			m68k_task_enter_user_frame(frame);
		irq_enter_isr((int)level);
		if (!x68k_irq_dispatch(level))
			hal_printf("X68k: unhandled IRQ level %u\n", level);
		irq_leave_isr((int)level);
		if (reschedule_pending) {
			reschedule_pending = 0;
			sched_yield();
		}
		if (user) {
			hal_user_return_invoke();
			m68k_task_leave_user_frame();
		}
		return;
	}

	if (vector == 32U && user) {
		struct hal_user_trap trap;
		uintptr_t arguments[HAL_SYSCALL_ARGS];
		trap.cause = HAL_TRAP_CAUSE_SYSCALL;
		trap.access = HAL_TRAP_MODE_UNKNOWN;
		trap.raw_vector = vector;
		trap.status = format_vector;
		trap.pc = frame_long(hardware + 2U);
		trap.result = frame->d[0];
		trap.fault_address = 0;
		if (user_int_handler != NULL)
			user_int_handler(&trap);
		arguments[0] = frame->d[1];
		arguments[1] = frame->d[2];
		arguments[2] = frame->d[3];
		arguments[3] = frame->d[4];
		arguments[4] = frame->d[5];
		arguments[5] = frame->a[0];
		m68k_task_enter_user_frame(frame);
		frame->d[0] = (uint32_t)(syscall_handler != NULL ?
			syscall_handler(frame->d[0], arguments) : -ENOSYS);
		hal_user_return_invoke();
		m68k_task_leave_user_frame();
		if (reschedule_pending) {
			reschedule_pending = 0;
			sched_yield();
		}
		return;
	}

	if (user) {
		struct hal_user_trap trap;
		uintptr_t fault_address = 0;
		uint16_t ssw = 0;
		uint16_t mmusr = 0;
		size_t frame_size = m68k_exception_frame_size(format_vector);
		int handled;
		if (m68k_exception_fault_address(hardware, frame_size,
		    &fault_address, &ssw) == 0 && vector == 2U) {
			switch (m68k_exception_access(ssw)) {
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
		trap.cause = m68k_exception_cause(vector, mmusr);
		trap.access = vector == 2U ? m68k_exception_access(ssw) :
			HAL_TRAP_MODE_UNKNOWN;
		trap.raw_vector = vector;
		trap.status = (uint32_t)mmusr << 16 | ssw;
		trap.pc = frame_long(hardware + 2U);
		trap.result = frame->d[0];
		trap.fault_address = fault_address;
		m68k_task_enter_user_frame(frame);
		handled = user_fault_handler != NULL &&
			user_fault_handler(&trap) == HAL_TRAP_RET_SUCCESS;
		if (handled) {
			hal_user_return_invoke();
			m68k_task_leave_user_frame();
			return; /* unchanged PC: retry demand-faulting instruction */
		}
		m68k_task_leave_user_frame();
		HAL_FATAL("m68k user fault handler returned");
	}

	hal_printf("m68k supervisor exception vector=%u format=%x sr=%x pc=%x\n",
		vector, format_vector >> 12, sr, frame_long(hardware + 2U));
	HAL_FATAL("unhandled m68k supervisor exception");
}

void
hal_set_trap_handler(int trap, hal_trap_handler_t handler)
{
	if (trap < 0 || trap >= HAL_TRAP_CAUSE_COUNT)
		HAL_FATAL("invalid m68k trap handler");
	trap_handlers[trap] = handler;
}

void hal_syscall_set_handler(hal_syscall_handler_t h) { syscall_handler = h; }
void hal_user_return_set_handler(hal_user_return_handler_t h) { user_return_handler = h; }
void hal_user_return_invoke(void) { if (user_return_handler != NULL) user_return_handler(); }
void hal_user_int_set_handler(hal_user_int_handler_t h) { user_int_handler = h; }
void hal_user_fault_set_handler(hal_user_fault_handler_t h) { user_fault_handler = h; }
void hal_reschedule_on_interrupt_return(void) { reschedule_pending = 1; }
