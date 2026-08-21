/* i386 UP IRQ controller contract and task-based IRQ service. */
#include <hal/hal.h>
#include "irq.h"
#include "pic.h"
#include "asm.h"
#include "clock.h"
#include "interrupt-controller.h"

enum irq_mode { IRQ_MODE_NONE, IRQ_MODE_REALTIME, IRQ_MODE_TASK };

#define IRQ_ACK_TASK 0x100U

static struct irq_service_info irq_service[IRQ_MAX + 1];

static int
service_active(const struct irq_service_info *service)
{
	hal_cpu_id_t cpu;
	for (cpu = 0; cpu < hal_cpu_count(); cpu++)
		if (service->in_handler[cpu] || service->in_flight[cpu])
			return 1;
	return 0;
}

void
irq_init(void)
{
	hal_memset(irq_service, 0, sizeof(irq_service));
	for (int irq = 0; irq <= IRQ_MAX; irq++)
		hal_cpu_mask_set(&irq_service[irq].requested, 0);
	pic_init();
}

bool hal_irq_disable(void)
{
	bool enabled = (asm_get_eflags() & EFLAGS_IF) != 0;
	asm_cli();
	return enabled;
}

void hal_irq_enable(void) { asm_sti(); }
void hal_irq_mask(int irq) { if (irq >= 0 && irq <= IRQ_MAX) i386_interrupt_mask(irq); }
void hal_irq_unmask(int irq) { if (irq >= 0 && irq <= IRQ_MAX) i386_interrupt_unmask(irq); }

static hal_irq_ack_t
begin_ack(int irq)
{
	struct irq_service_info *service = &irq_service[irq];
	hal_cpu_id_t cpu = hal_cpu_current();
	if (service->in_flight[cpu])
		HAL_FATAL("nested i386 IRQ acknowledgement");
	service->in_flight[cpu] = 1;
	return (hal_irq_ack_t)(unsigned)(irq + 1);
}

void
hal_irq_send_eoi(hal_irq_ack_t acknowledge)
{
	unsigned irq;
	int task_acknowledge = (acknowledge & IRQ_ACK_TASK) != 0;
	acknowledge &= ~IRQ_ACK_TASK;
	if (acknowledge == IRQ_MAX + 2U && i386_interrupt_uses_apic()) {
		i386_interrupt_eoi(0);
		return;
	}
	if (acknowledge == HAL_IRQ_ACK_NONE || acknowledge > IRQ_MAX + 1U)
		HAL_FATAL("invalid i386 IRQ acknowledgement");
	irq = (unsigned)acknowledge - 1U;
	if (!irq_service[irq].in_flight[hal_cpu_current()])
		HAL_FATAL("stale i386 IRQ acknowledgement");
	irq_service[irq].in_flight[hal_cpu_current()] = 0;
	i386_interrupt_eoi((int)irq);
	if (task_acknowledge)
		hal_irq_enable();
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
	if (service_active(service)) {
		if (enabled) hal_irq_enable();
		return HAL_ERR_BUSY;
	}
	if (handler == NULL) {
		i386_interrupt_mask(irq);
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
			*acknowledge = service->acknowledge | IRQ_ACK_TASK;
			return HAL_OK;
		}
		i386_interrupt_unmask(irq);
		if (enabled) hal_irq_enable();
		kernel_wait_task();
	}
}

int
hal_irq_set_affinity(int irq, const struct hal_cpu_mask *requested)
{
	if (irq < 0 || irq > IRQ_MAX || requested == NULL)
		return HAL_ERR_INVALID;
	{
		hal_cpu_id_t cpu;
		for (cpu = 0; cpu < hal_cpu_count(); cpu++)
			if (hal_cpu_mask_test(requested, cpu))
				break;
		if (cpu == hal_cpu_count() || i386_interrupt_route(irq, cpu) != HAL_OK)
			return HAL_ERR_UNSUPPORTED;
	}
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
	result->effective = irq_service[irq].requested;
	return HAL_OK;
}

void
irq_handler(int irq)
{
	struct irq_service_info *service;
	hal_irq_ack_t acknowledge;
	if (irq < 0 || irq > IRQ_MAX)
		HAL_FATAL("invalid i386 IRQ");
	if (irq == IRQ_TIMER && i386_interrupt_calibration_tick())
		return;
	service = &irq_service[irq];
	acknowledge = begin_ack(irq);
	if (irq == IRQ_TIMER) {
		clock_handler();
		kernel_timer_handler(hal_cpu_current(), acknowledge);
		return;
	}
	if (service->mode == IRQ_MODE_REALTIME && service->handler != NULL) {
		service->in_handler[hal_cpu_current()] = 1;
		service->handler(irq, acknowledge, service->argument);
		service->in_handler[hal_cpu_current()] = 0;
		return;
	}
	if (service->mode == IRQ_MODE_TASK && service->waiter != NULL) {
		hal_task_t waiter = service->waiter;
		i386_interrupt_mask(irq);
		service->acknowledge = acknowledge;
		service->pending = 1;
		kernel_notify_task(waiter);
		return;
	}
	i386_interrupt_mask(irq);
	hal_irq_send_eoi(acknowledge);
}
