/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * i386 UP IRQ controller contract and task-based IRQ service.
 */

#include <hal/hal.h>
#include "apic-topology.h"
#include "irq.h"
#include "ioapic.h"
#include "lapic.h"
#include "pic.h"
#include "asm.h"
#include "clock.h"
#include "smp.h"

#define IRQ_ACK_TASK 0x100U

enum irq_mode {
	IRQ_MODE_NONE,
	IRQ_MODE_REALTIME,
	IRQ_MODE_TASK
};

static struct irq_service_info irq_service[IRQ_MAX + 1];

static struct i386_apic_topology topology;
static int apic_mode;
static volatile unsigned calibration_active;
static volatile unsigned calibration_stage;
static uint32_t timer_ticks;

static int service_active(const struct irq_service_info *service);
static hal_irq_ack_t begin_ack(int irq);
static void imcr_to_apic(void);

/*
 * Initialize the IRQ component.
 */
void
irq_init(void)
{
	hal_memset(irq_service, 0, sizeof(irq_service));

	for (int irq = 0; irq <= IRQ_MAX; irq++)
		hal_cpu_mask_set(&irq_service[irq].requested, 0);

	pic_init();
}

bool
hal_irq_disable(void)
{
	bool enabled;

	enabled = (asm_get_eflags() & EFLAGS_IF) != 0;
	asm_cli();

	return enabled;
}

void
hal_irq_enable(void) {
	asm_sti();
}

void
hal_irq_mask(
	int irq)
{
	if (irq >= 0 && irq <= IRQ_MAX)
		i386_interrupt_mask(irq);
}

void
hal_irq_unmask(
	int irq)
{
	if (irq >= 0 && irq <= IRQ_MAX)
		i386_interrupt_unmask(irq);
}

void
hal_irq_send_eoi(
	hal_irq_ack_t acknowledge)
{
	unsigned irq;
	int task_acknowledge;

	task_acknowledge = (acknowledge & IRQ_ACK_TASK) != 0;

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
hal_irq_set_handler(
	int irq,
	hal_irq_handler_t handler,
	void *argument)
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

	if (enabled)
		hal_irq_enable();

	return HAL_OK;
}

int
hal_irq_service_wait(
	int irq,
	hal_irq_ack_t *acknowledge)
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

		if (enabled)
			hal_irq_enable();

		kernel_wait_task();
	}
}

int
hal_irq_set_affinity(
	int irq,
	const struct hal_cpu_mask *requested)
{
	hal_cpu_id_t cpu;

	if (irq < 0 || irq > IRQ_MAX || requested == NULL)
		return HAL_ERR_INVALID;

	for (cpu = 0; cpu < hal_cpu_count(); cpu++)
		if (hal_cpu_mask_test(requested, cpu))
			break;

	if (cpu == hal_cpu_count() || i386_interrupt_route(irq, cpu) != HAL_OK)
		return HAL_ERR_UNSUPPORTED;

	irq_service[irq].requested = *requested;

	return HAL_OK;
}

int
hal_irq_get_affinity(
	int irq,
	struct hal_irq_affinity *result)
{
	if (irq < 0 || irq > IRQ_MAX || result == NULL)
		return HAL_ERR_INVALID;

	result->requested = irq_service[irq].requested;

	hal_memset(&result->effective, 0, sizeof(result->effective));

	result->effective = irq_service[irq].requested;

	return HAL_OK;
}

void
irq_handler(
	int irq)
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

int
hal_irq_register_msi(
	const char *source,
	hal_irq_handler_t handler,
	void *handler_arg,
	int *mapped_irq,
	paddr_t *mapped_addr,
	uint32_t *mapped_event)
{
	(void)source;
	(void)handler;
	(void)handler_arg;
	(void)mapped_irq;
	(void)mapped_addr;
	(void)mapped_event;

	return HAL_ERR_UNSUPPORTED;
}

int
hal_irq_unregister_msi(
	int mapped_irq)
{
	(void)mapped_irq;
	return HAL_ERR_UNSUPPORTED;
}

static hal_irq_ack_t
begin_ack(
	int irq)
{
	struct irq_service_info *service;
	hal_cpu_id_t cpu;

	service = &irq_service[irq];
	cpu = hal_cpu_current();

	if (service->in_flight[cpu])
		HAL_FATAL("nested i386 IRQ acknowledgement");

	service->in_flight[cpu] = 1;

	return (hal_irq_ack_t)(unsigned)(irq + 1);
}

static int
service_active(
	const struct irq_service_info *service)
{
	hal_cpu_id_t cpu;

	for (cpu = 0; cpu < hal_cpu_count(); cpu++) {
		if (service->in_handler[cpu] || service->in_flight[cpu])
			return 1;
	}

	return 0;
}

int
i386_interrupt_select(void)
{
	int error;

#if defined(HAL_BOARD_PC98)
	error = i386_mps_discover(&topology);
#else
	error = i386_acpi_discover(&topology);
	if (error != HAL_OK)
		error = i386_mps_discover(&topology);
#endif

	if (error != HAL_OK || topology.cpu_count < 2U)
		return HAL_ERR_UNSUPPORTED;

	if (i386_lapic_init(topology.lapic_address) != HAL_OK)
		return HAL_ERR_UNSUPPORTED;

	/* Measure the local timer against two already configured PIT ticks. */
	calibration_stage = 0;
	calibration_active = 1;

	asm_sti();

	while(calibration_stage < 2U)
		asm_hlt();

	asm_cli();

	calibration_active = 0;

	timer_ticks = i386_lapic_timer_elapsed();
	if (timer_ticks < 100U)
		return HAL_ERR_UNSUPPORTED;

	if (i386_ioapic_init(&topology,i386_lapic_id()) != HAL_OK)
		return HAL_ERR_UNSUPPORTED;

	/* No legacy interrupt may escape while the routing mode changes. */
	pic_init();

	if(topology.imcr_present)
		imcr_to_apic();

	apic_mode=1;
	i386_lapic_timer_start(timer_ticks);
	i386_smp_configure(&topology);

	return HAL_OK;
}

int
i386_interrupt_calibration_tick(void)
{
	if (!calibration_active || apic_mode || calibration_stage >= 2U)
		return 0;

	pic_send_eoi(0);

	if (calibration_stage==0U) {
		i386_lapic_timer_prepare();
		calibration_stage = 1U;
	} else {
		calibration_stage = 2U;
	}

	return 1;
}

int
i386_interrupt_uses_apic(void)
{
	return apic_mode;
}

int
i386_interrupt_validate(int irq)
{
	return apic_mode ? irq : pic_get_irq_in_service();
}

void
i386_interrupt_mask(int irq)
{
	if (apic_mode)
		i386_ioapic_mask(irq);
	else
		pic_set_irq_mask(irq,1);
}

void
i386_interrupt_unmask(int irq)
{
	if (apic_mode)
		i386_ioapic_unmask(irq);
	else
		pic_set_irq_mask(irq,0);
}

void
i386_interrupt_eoi(int irq)
{
	if (apic_mode) {
		(void)irq;
		i386_lapic_eoi();
	} else {
		pic_send_eoi(irq);
	}
}

int
i386_interrupt_route(int irq, hal_cpu_id_t cpu)
{
	uint8_t apic_id;

	if (!apic_mode)
		return cpu == 0 ? HAL_OK : HAL_ERR_UNSUPPORTED;

	if (i386_smp_apic_id(cpu,&apic_id) != HAL_OK)
		return HAL_ERR_INVALID;

	return i386_ioapic_route(irq,apic_id);
}

uint32_t i386_interrupt_timer_ticks(void)
{
	return timer_ticks;
}

static void
imcr_to_apic(void)
{
	asm_outb(0x22U,0x70U);
	asm_outb(0x23U,0x01U);
}
