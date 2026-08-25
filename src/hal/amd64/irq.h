#ifndef ZEDBSD_HAL_AMD64_IRQ_H
#define ZEDBSD_HAL_AMD64_IRQ_H

#include <hal/hal.h>
#include "bsp-pcat/acpi.h"

#define IRQ_MAX      15
#define IRQ_MSI_BASE (IRQ_MAX + 1)
#define IRQ_MSI_COUNT AMD64_VECTOR_MSI_COUNT
#define IRQ_LOGICAL_MAX (IRQ_MSI_BASE + IRQ_MSI_COUNT - 1)
#define IRQ_TIMER    0
#define IRQ_KEYBOARD 1

struct irq_service_info {
	int mode;
	hal_irq_handler_t handler;
	void *argument;
	hal_task_t waiter;
	hal_irq_ack_t acknowledge;
	unsigned pending, in_flight, in_handler, removing, masked;
	unsigned msi, allocated;
	hal_cpu_id_t handler_cpu;
	volatile unsigned lock;
	struct hal_cpu_mask requested;
};
void irq_init(const struct amd64_acpi_info *acpi);
void irq_handler(int irq_num);
void amd64_notify_interrupt(void);
void amd64_error_interrupt(void);
int amd64_irq_task_transferable(hal_task_t task);
int amd64_msi_source_valid(const char *source);

#endif
