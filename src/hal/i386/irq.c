/* i386 UP IRQ controller contract and task-based IRQ service. */
#include <hal/hal.h>
#include "irq.h"
#include "pic.h"
#include "asm.h"
#include "clock.h"

enum irq_mode { IRQ_MODE_NONE, IRQ_MODE_REALTIME, IRQ_MODE_TASK };

static struct irq_service_info irq_service[IRQ_MAX + 1];

void
irq_init(void)
{
	hal_memset(irq_service, 0, sizeof(irq_service));
	pic_init();
}

bool hal_irq_disable(void)
{
	bool enabled = (asm_get_eflags() & EFLAGS_IF) != 0;
	asm_cli();
	return enabled;
}

void hal_irq_enable(void) { asm_sti(); }
void hal_irq_mask(int irq) { if (irq >= 0 && irq <= IRQ_MAX) pic_set_irq_mask(irq, 1); }
void hal_irq_unmask(int irq) { if (irq >= 0 && irq <= IRQ_MAX) pic_set_irq_mask(irq, 0); }

static hal_irq_ack_t
begin_ack(int irq)
{
	struct irq_service_info *service = &irq_service[irq];
	if (service->in_flight)
		HAL_FATAL("nested i386 IRQ acknowledgement");
	service->in_flight = 1;
	return (hal_irq_ack_t)(unsigned)(irq + 1);
}

void
hal_irq_send_eoi(hal_irq_ack_t acknowledge)
{
	unsigned irq;
	if (acknowledge == HAL_IRQ_ACK_NONE || acknowledge > IRQ_MAX + 1U)
		HAL_FATAL("invalid i386 IRQ acknowledgement");
	irq = (unsigned)acknowledge - 1U;
	if (!irq_service[irq].in_flight)
		HAL_FATAL("stale i386 IRQ acknowledgement");
	irq_service[irq].in_flight = 0;
	pic_send_eoi((int)irq);
}

int
hal_irq_set_handler(int irq, hal_irq_handler_t handler, void *argument)
{
	struct irq_service_info *service;
	bool enabled;
	if (irq < 0 || irq > IRQ_MAX || (handler == NULL && argument != NULL))
		return HAL_ERR_INVALID;
	enabled = hal_irq_disable();
	service = &irq_service[irq];
	if (service->in_handler || service->in_flight) {
		if (enabled) hal_irq_enable();
		return HAL_ERR_BUSY;
	}
	if (handler == NULL) {
		pic_set_irq_mask(irq, 1);
		if (service->mode == IRQ_MODE_TASK) {
			if (enabled) hal_irq_enable();
			return HAL_ERR_BUSY;
		}
		service->mode = IRQ_MODE_NONE;
		service->handler = NULL;
		service->argument = NULL;
	} else {
		if (service->mode != IRQ_MODE_NONE) {
			if (enabled) hal_irq_enable();
			return HAL_ERR_BUSY;
		}
		service->mode = IRQ_MODE_REALTIME;
		service->handler = handler;
		service->argument = argument;
	}
	if (enabled) hal_irq_enable();
	return HAL_OK;
}

int
hal_irq_service_wait(int irq, hal_irq_ack_t *acknowledge)
{
	struct irq_service_info *service;
	bool enabled;
	if (irq < 0 || irq > IRQ_MAX || irq == IRQ_TIMER || acknowledge == NULL)
		return HAL_ERR_INVALID;
	for (;;) {
		enabled = hal_irq_disable();
		service = &irq_service[irq];
		if (service->mode == IRQ_MODE_REALTIME ||
		    (service->waiter != NULL &&
		    service->waiter != hal_task_get_current())) {
			if (enabled) hal_irq_enable();
			return HAL_ERR_BUSY;
		}
		service->mode = IRQ_MODE_TASK;
		service->waiter = hal_task_get_current();
		if (service->pending) {
			service->pending = 0;
			*acknowledge = service->acknowledge;
			return HAL_OK;
		}
		pic_set_irq_mask(irq, 0);
		if (enabled) hal_irq_enable();
		hal_cpu_idle();
		kernel_yield();
	}
}

int
hal_irq_set_affinity(int irq, const struct hal_cpu_mask *requested)
{
	if (irq < 0 || irq > IRQ_MAX || requested == NULL ||
	    (requested->bits[0] & 1U) == 0)
		return HAL_ERR_INVALID;
	irq_service[irq].requested = *requested;
	return HAL_OK;
}

int
hal_irq_get_affinity(int irq, struct hal_irq_affinity *result)
{
	if (irq < 0 || irq > IRQ_MAX || result == NULL)
		return HAL_ERR_INVALID;
	result->requested = irq_service[irq].requested;
	hal_memset(&result->effective, 0, sizeof(result->effective));
	result->effective.bits[0] = 1U;
	return HAL_OK;
}

void
irq_handler(int irq)
{
	struct irq_service_info *service;
	hal_irq_ack_t acknowledge;
	if (irq < 0 || irq > IRQ_MAX)
		HAL_FATAL("invalid i386 IRQ");
	service = &irq_service[irq];
	acknowledge = begin_ack(irq);
	if (irq == IRQ_TIMER) {
		clock_handler();
		kernel_timer_handler(0, acknowledge);
		return;
	}
	if (service->mode == IRQ_MODE_REALTIME && service->handler != NULL) {
		service->in_handler = 1;
		service->handler(irq, acknowledge, service->argument);
		service->in_handler = 0;
		return;
	}
	if (service->mode == IRQ_MODE_TASK && service->waiter != NULL) {
		pic_set_irq_mask(irq, 1);
		service->acknowledge = acknowledge;
		service->pending = 1;
		return;
	}
	pic_set_irq_mask(irq, 1);
	hal_irq_send_eoi(acknowledge);
}
