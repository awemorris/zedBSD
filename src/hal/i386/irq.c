/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * IRQ management: the local interrupt lock, the ISR-task protocol, and
 * the dispatch called from the general interrupt handler.
 */

#include <hal/hal.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include "irq.h"
#include "pic.h"
#include "asm.h"
#include "clock.h"
#include "int.h"

void kernel_timer_handler(void);

/* IRQ service registry. */
static struct irq_service_info irq_service[IRQ_MAX + 1];

/*
 * Initialize IRQ management.
 */
void
irq_init(void)
{
	int i;

	for (i = 0; i <= IRQ_MAX; i++)
		irq_service[i].ist = NULL;

	/* Initialize the interrupt controller; every IRQ starts masked. */
	pic_init();
}

/*
 * Disable local IRQ delivery, returning the previous state.
 */
irqlock_t
irq_acquire_lock(void)
{
	int status = asm_get_eflags() & EFLAGS_IF;

	asm_cli();
	return status;
}

/*
 * Restore the interrupt-enable state saved by irq_acquire_lock().
 */
void
irq_unacquire_lock(irqlock_t lock)
{
	if (lock != 0)
		asm_sti();
}

bool
hal_irq_disable(void)
{
	return irq_acquire_lock() != 0;
}

void
hal_irq_enable(void)
{
	asm_sti();
}

/*
 * Begin waiting for an IRQ: register the running task as the service
 * task, unlink it from the scheduler (so it sleeps from the next yield
 * or preemption), unmask the IRQ, and yield.  The task resumes when
 * irq_handler() relinks it.
 */
void
irq_enter_isr(int irq_num)
{
	hal_task_t t;
	struct thread *thread;
	irqlock_t irqlock;

	ENTER_IRQLOCK(irqlock)
	{
		/* Only one service task per IRQ. */
		HAL_ASSERT(irq_service[irq_num].ist == NULL);

		t = hal_task_get_current();
		irq_service[irq_num].ist = t;
		thread = hal_task_get_private(t);
		HAL_ASSERT(thread != NULL);
		thread->state = THREAD_SLEEPING;
		sched_unlink(thread);

		pic_set_irq_mask(irq_num, 0);
	}
	LEAVE_IRQLOCK(irqlock);

	sched_yield();

	/* The IRQ has fired and irq_handler() relinked this task. */
}

/*
 * Called when the interrupt service routine finishes.
 */
void
irq_leave_isr(int irq_num)
{
	(void)irq_num;
}

/*
 * IRQ dispatch; runs with interrupts disabled, from int_handler().
 */
void
irq_handler(int irq_num)
{
	hal_task_t t;
	struct thread *thread;

	/*
	 * The interval timer is handled inline: count the tick, run the
	 * scheduler's clock hook, and finish the IRQ.
	 */
	if (irq_num == IRQ_TIMER) {
		clock_handler();
		kernel_timer_handler();
		sched_clock();
		pic_send_eoi(irq_num);
		return;
	}

	/*
	 * Every other IRQ wakes its registered service task.
	 */
	pic_set_irq_mask(irq_num, 1);
	pic_send_eoi(irq_num);

	t = irq_service[irq_num].ist;
	HAL_ASSERT(t != NULL);
	irq_service[irq_num].ist = NULL;
	thread = hal_task_get_private(t);
	HAL_ASSERT(thread != NULL);
	sched_wakeup(thread);

	int_set_resched_flag();
}
