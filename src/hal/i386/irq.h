/*
 * IRQ numbers and the ISR-task registry.
 */

#ifndef _SYS_ARCH_X86_IRQ_H_
#define _SYS_ARCH_X86_IRQ_H_

#include <hal/irq.h>	/* interface definition */
#include <hal/task.h>	/* task_t */

#define IRQ_MAX		(15)

/* Both boards put the interval timer on IRQ 0 and the keyboard on 1. */
#define IRQ_TIMER	(0)
#define IRQ_KEYBOARD	(1)

/* IRQ service registration. */
struct irq_service_info {
	task_t ist;	/* interrupt service task */
};

/* irq.c */
void irq_init(void);
void irq_handler(int irq_num);	/* called from int.c */

#endif
