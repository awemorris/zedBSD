/*
 * Local IRQ interrupt lock and ISR service interface.
 */

#ifndef _SYS_ARCH_IRQ_H_
#define _SYS_ARCH_IRQ_H_

typedef int irqlock_t;

#define ENTER_IRQLOCK(v)			\
	do {					\
		v = irq_acquire_lock();		\
	} while (0);

#define LEAVE_IRQLOCK(v)			\
	do {					\
		irq_unacquire_lock(v);		\
	} while (0)

/* irq.c */
irqlock_t irq_acquire_lock(void);	/* disable local IRQs */
void irq_unacquire_lock(irqlock_t lock);
void irq_enter_isr(int irq_num);	/* block awaiting the IRQ */
void irq_leave_isr(int irq_num);

#endif
