/* amd64 UP interrupt locks and ISR-task protocol. */
#include <hal/hal.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include "irq.h"
#include "pic.h"
#include "clock.h"
#include "int.h"
#include "asm.h"

static struct irq_service_info irq_service[IRQ_MAX + 1];

void
irq_init(void)
{
	int index;
	for (index = 0; index <= IRQ_MAX; index++) irq_service[index].ist = NULL;
	pic_init();
}

irqlock_t irq_acquire_lock(void)
{
	int enabled = (asm_get_rflags() & 0x200U) != 0;
	asm_cli();
	return enabled;
}

void irq_unacquire_lock(irqlock_t lock) { if (lock) asm_sti(); }
bool hal_irq_disable(void) { return irq_acquire_lock() != 0; }
void hal_irq_enable(void) { asm_sti(); }
void hal_irq_mask(int irq) { pic_set_irq_mask(irq, 1); }
void hal_irq_unmask(int irq) { pic_set_irq_mask(irq, 0); }
void hal_irq_send_eoi(int irq) { pic_send_eoi(irq); }
void hal_irq_set_handler(int irq, void (*func)(void *), void *arg)
{ (void)irq; (void)func; (void)arg; }

void
irq_enter_isr(int irq)
{
	hal_task_t task;
	struct thread *thread;
	irqlock_t lock;
	ENTER_IRQLOCK(lock) {
		HAL_ASSERT(irq >= 0 && irq <= IRQ_MAX);
		HAL_ASSERT(irq_service[irq].ist == NULL);
		task = hal_task_get_current();
		irq_service[irq].ist = task;
		thread = hal_task_get_private(task);
		HAL_ASSERT(thread != NULL);
		thread->state = THREAD_SLEEPING;
		sched_unlink(thread);
		pic_set_irq_mask(irq, 0);
	} LEAVE_IRQLOCK(lock);
	sched_yield();
}

void irq_leave_isr(int irq) { (void)irq; }

void
irq_handler(int irq)
{
	hal_task_t task;
	struct thread *thread;
	if (irq == IRQ_TIMER) {
		clock_handler();
		kernel_timer_handler();
		sched_clock();
		pic_send_eoi(irq);
		return;
	}
	pic_set_irq_mask(irq, 1);
	pic_send_eoi(irq);
	task = irq_service[irq].ist;
	HAL_ASSERT(task != NULL);
	irq_service[irq].ist = NULL;
	thread = hal_task_get_private(task);
	HAL_ASSERT(thread != NULL);
	sched_wakeup(thread);
	int_set_resched_flag();
}
