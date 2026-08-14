#ifndef ZEDBSD_HAL_AMD64_IRQ_H
#define ZEDBSD_HAL_AMD64_IRQ_H

#include <hal/hal.h>

#define IRQ_MAX      15
#define IRQ_TIMER    0
#define IRQ_KEYBOARD 1

struct irq_service_info { hal_task_t ist; };
void irq_init(void);
void irq_handler(int irq_num);

#endif
