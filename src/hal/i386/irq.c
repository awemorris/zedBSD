/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i386 IRQ-controller selection and service implementation.
 */

#include <hal/hal.h>

#include "apic-topology.h"
#include "asm.h"
#include "clock.h"
#include "defs.h"
#include "ioapic.h"
#include "irq.h"
#include "lapic.h"
#include "pic.h"
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
 * Initializes the i386 IRQ service registry and legacy controller.
 */
void
irq_init(
	void)
{
	int irq;

	/* Clears every IRQ service record and requests the bootstrap CPU. */
	hal_memset(irq_service, 0, sizeof(irq_service));

	/* Assigns the default CPU affinity to every legacy IRQ. */
	for (irq = 0; irq <= IRQ_MAX; irq++)
		hal_cpu_mask_set(&irq_service[irq].requested, 0);

	/* Programs the legacy controller before interrupts are enabled. */
	pic_init();
}

/*
 * Disables maskable interrupts and reports their previous state.
 */
bool
hal_irq_disable(
	void)
{
	bool enabled;

	/* Captures IF before clearing it on the current CPU. */
	enabled = (asm_get_eflags() & EFLAGS_IF) != 0;
	asm_cli();

	/* Returns whether interrupts were enabled on entry. */
	return enabled;
}

/*
 * Enables maskable interrupts on the current CPU.
 */
void
hal_irq_enable(
	void)
{
	/* Sets IF on the current CPU. */
	asm_sti();
}

/*
 * Masks one valid legacy IRQ route.
 */
void
hal_irq_mask(
	int irq)
{
	/* Ignores invalid public IRQ numbers. */
	if (irq >= 0 && irq <= IRQ_MAX)
		i386_interrupt_mask(irq);
}

/*
 * Unmasks one valid legacy IRQ route.
 */
void
hal_irq_unmask(
	int irq)
{
	/* Ignores invalid public IRQ numbers. */
	if (irq >= 0 && irq <= IRQ_MAX)
		i386_interrupt_unmask(irq);
}

/*
 * Completes an acknowledged IRQ delivery.
 */
void
hal_irq_send_eoi(
	hal_irq_ack_t acknowledge)
{
	unsigned irq;
	int task_acknowledge;

	/* Separates the task-wait marker from the controller token. */
	task_acknowledge = (acknowledge & IRQ_ACK_TASK) != 0;
	acknowledge &= ~IRQ_ACK_TASK;

	/* Completes the reserved interprocessor acknowledgement in APIC mode. */
	if (acknowledge == IRQ_MAX + 2U) {
		/* Completes and returns only for the active local-APIC path. */
		if (i386_interrupt_uses_apic()) {
			i386_interrupt_eoi(0);
			return;
		}
	}

	/* Rejects malformed or empty controller acknowledgements. */
	if (acknowledge == HAL_IRQ_ACK_NONE || acknowledge > IRQ_MAX + 1U)
		HAL_FATAL("invalid i386 IRQ acknowledgement");

	/* Resolves and validates the per-CPU in-flight service. */
	irq = (unsigned)acknowledge - 1U;

	/* Rejects an acknowledgement without matching in-flight ownership. */
	if (!irq_service[irq].in_flight[hal_cpu_current()])
		HAL_FATAL("stale i386 IRQ acknowledgement");
	irq_service[irq].in_flight[hal_cpu_current()] = 0;

	/* Signals completion to the active interrupt controller. */
	i386_interrupt_eoi((int)irq);

	/* Reopens the interruptible window owned by a task service. */
	if (task_acknowledge)
		hal_irq_enable();
}

/*
 * Installs or removes one real-time IRQ handler.
 */
int
hal_irq_set_handler(
	int irq,
	hal_irq_handler_t handler,
	void *argument)
{
	struct irq_service_info *service;
	bool enabled;

	/* Rejects an invalid IRQ or an argument without a handler. */
	if (irq < 0 || irq > IRQ_MAX || (handler == NULL && argument != NULL))
		return HAL_ERR_INVALID;

	/* Serializes the selected service against interrupt delivery. */
	enabled = hal_irq_disable();
	service = &irq_service[irq];

	/* Rejects replacement while an acknowledgement remains active. */
	if (service_active(service)) {
		/* Restores interrupts before reporting the busy service. */
		if (enabled)
			hal_irq_enable();
		return HAL_ERR_BUSY;
	}

	/* Removes a handler or installs a handler into an unused service. */
	if (handler == NULL) {
		i386_interrupt_mask(irq);

		/* Keeps a task-owned service registered until its waiter exits. */
		if (service->mode == IRQ_MODE_TASK) {
			/* Restores interrupts before reporting task ownership. */
			if (enabled)
				hal_irq_enable();
			return HAL_ERR_BUSY;
		}
		service->mode = IRQ_MODE_NONE;
		service->handler = NULL;
		service->argument = NULL;
	} else {
		/* Rejects installation over any existing service mode. */
		if (service->mode != IRQ_MODE_NONE) {
			/* Restores interrupts before reporting existing ownership. */
			if (enabled)
				hal_irq_enable();
			return HAL_ERR_BUSY;
		}
		service->mode = IRQ_MODE_REALTIME;
		service->handler = handler;
		service->argument = argument;
	}

	/* Restores interrupts only when they were previously enabled. */
	if (enabled)
		hal_irq_enable();

	/* Reports a completed handler update. */
	return HAL_OK;
}

/*
 * Waits for one task-serviced IRQ delivery.
 */
int
hal_irq_service_wait(
	int irq,
	hal_irq_ack_t *acknowledge)
{
	struct irq_service_info *service;
	hal_task_t current;
	bool busy;
	bool enabled;

	/* Rejects invalid, timer, and output-less wait requests. */
	if (irq < 0 || irq > IRQ_MAX || irq == IRQ_TIMER || acknowledge == NULL)
		return HAL_ERR_INVALID;

	/* Repeats registration and sleep until an IRQ is pending. */
	for (;;) {
		/* Serializes the task service against interrupt delivery. */
		enabled = hal_irq_disable();
		service = &irq_service[irq];
		busy = service->mode == IRQ_MODE_REALTIME;

		/* Tests an existing waiter only when no real-time handler owns it. */
		if (!busy && service->waiter != NULL) {
			current = hal_task_get_current();
			busy = service->waiter != current;
		}

		/* Rejects an IRQ owned by another service or task. */
		if (busy) {
			/* Restores interrupts before reporting conflicting ownership. */
			if (enabled)
				hal_irq_enable();
			return HAL_ERR_BUSY;
		}

		/* Registers the current task as the IRQ waiter. */
		service->mode = IRQ_MODE_TASK;
		service->waiter = hal_task_get_current();

		/* Transfers a pending acknowledgement with IRQs still disabled. */
		if (service->pending) {
			service->pending = 0;
			*acknowledge = service->acknowledge | IRQ_ACK_TASK;
			return HAL_OK;
		}

		/* Arms delivery before reopening the caller's interrupt window. */
		i386_interrupt_unmask(irq);

		/* Restores interrupts only when they were previously enabled. */
		if (enabled)
			hal_irq_enable();

		/* Sleeps until the interrupt handler notifies this task. */
		kernel_wait_task();
	}
}

/*
 * Routes one IRQ to the first requested online CPU.
 */
int
hal_irq_set_affinity(
	int irq,
	const struct hal_cpu_mask *requested)
{
	hal_cpu_id_t cpu;
	int route_result;

	/* Rejects an invalid IRQ or missing affinity mask. */
	if (irq < 0 || irq > IRQ_MAX || requested == NULL)
		return HAL_ERR_INVALID;

	/* Selects the first CPU requested by the caller. */
	for (cpu = 0; cpu < hal_cpu_count(); cpu++) {
		/* Stops at the first configured CPU selected by the mask. */
		if (hal_cpu_mask_test(requested, cpu))
			break;
	}

	/* Rejects a mask which contains no configured CPU. */
	if (cpu == hal_cpu_count())
		return HAL_ERR_UNSUPPORTED;

	/* Programs the route before publishing the requested affinity. */
	route_result = i386_interrupt_route(irq, cpu);

	/* Reports an unsupported route when controller programming fails. */
	if (route_result != HAL_OK)
		return HAL_ERR_UNSUPPORTED;
	irq_service[irq].requested = *requested;

	/* Reports a programmed interrupt route. */
	return HAL_OK;
}

/*
 * Reports the requested and effective affinity of one IRQ.
 */
int
hal_irq_get_affinity(
	int irq,
	struct hal_irq_affinity *result)
{
	/* Rejects an invalid IRQ or missing result object. */
	if (irq < 0 || irq > IRQ_MAX || result == NULL)
		return HAL_ERR_INVALID;

	/* Copies the requested mask and reconstructs the effective mask. */
	result->requested = irq_service[irq].requested;
	hal_memset(&result->effective, 0, sizeof(result->effective));
	result->effective = irq_service[irq].requested;

	/* Reports the available affinity information. */
	return HAL_OK;
}

/*
 * Dispatches one validated hardware IRQ.
 */
void
irq_handler(
	int irq)
{
	struct irq_service_info *service;
	hal_irq_ack_t acknowledge;
	hal_task_t waiter;

	/* Rejects an IRQ outside the configured controller range. */
	if (irq < 0 || irq > IRQ_MAX)
		HAL_FATAL("invalid i386 IRQ");

	/* Lets the temporary APIC calibration consume timer interrupts first. */
	if (irq == IRQ_TIMER) {
		/* Returns when calibration consumed this timer delivery. */
		if (i386_interrupt_calibration_tick())
			return;
	}

	/* Starts controller acknowledgement for the selected service. */
	service = &irq_service[irq];
	acknowledge = begin_ack(irq);

	/* Delivers the normal timer tick through the kernel timer path. */
	if (irq == IRQ_TIMER) {
		clock_handler();
		kernel_timer_handler(hal_cpu_current(), acknowledge);
		return;
	}

	/* Invokes an installed real-time handler in interrupt context. */
	if (service->mode == IRQ_MODE_REALTIME && service->handler != NULL) {
		service->in_handler[hal_cpu_current()] = 1;
		service->handler(irq, acknowledge, service->argument);
		service->in_handler[hal_cpu_current()] = 0;
		return;
	}

	/* Transfers a task-owned IRQ acknowledgement to its waiter. */
	if (service->mode == IRQ_MODE_TASK && service->waiter != NULL) {
		waiter = service->waiter;
		i386_interrupt_mask(irq);
		service->acknowledge = acknowledge;
		service->pending = 1;
		kernel_notify_task(waiter);
		return;
	}

	/* Masks and completes an IRQ which has no registered service. */
	i386_interrupt_mask(irq);
	hal_irq_send_eoi(acknowledge);
}

/*
 * Reports that message-signaled interrupts are unavailable on i386.
 */
int
hal_irq_register_msi(
	const char *source,
	hal_irq_handler_t handler,
	void *handler_arg,
	int *mapped_irq,
	paddr_t *mapped_addr,
	uint32_t *mapped_event)
{
	UNUSED_PARAMETER(source);
	UNUSED_PARAMETER(handler);
	UNUSED_PARAMETER(handler_arg);
	UNUSED_PARAMETER(mapped_irq);
	UNUSED_PARAMETER(mapped_addr);
	UNUSED_PARAMETER(mapped_event);

	/* Reports the fixed absence of an i386 MSI implementation. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Reports that message-signaled interrupts are unavailable on i386.
 */
int
hal_irq_unregister_msi(
	int mapped_irq)
{
	UNUSED_PARAMETER(mapped_irq);

	/* Reports the fixed absence of an i386 MSI implementation. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Selects APIC routing when multiprocessor topology is available.
 */
int
i386_interrupt_select(
	void)
{
	int error;

	/* Discovers the board's available processor and interrupt topology. */
#if defined(HAL_BOARD_PC98)
	error = i386_mps_discover(&topology);
#else
	error = i386_acpi_discover(&topology);

	/* Falls back to the legacy multiprocessor table when ACPI is absent. */
	if (error != HAL_OK)
		error = i386_mps_discover(&topology);
#endif

	/* Requires topology for at least two processors. */
	if (error != HAL_OK || topology.cpu_count < 2U)
		return HAL_ERR_UNSUPPORTED;

	/* Maps and enables the bootstrap processor's local APIC. */
	error = i386_lapic_init(topology.lapic_address);

	/* Falls back to the legacy controller when local-APIC setup fails. */
	if (error != HAL_OK)
		return HAL_ERR_UNSUPPORTED;

	/* Measures the local timer against two already configured PIT ticks. */
	calibration_stage = 0;
	calibration_active = 1;
	asm_sti();

	/* Waits for both PIT calibration interrupts. */
	while (calibration_stage < 2U)
		asm_hlt();
	asm_cli();
	calibration_active = 0;

	/* Rejects an implausibly small local timer interval. */
	timer_ticks = i386_lapic_timer_elapsed();

	/* Falls back when calibration produced too few local timer ticks. */
	if (timer_ticks < 100U)
		return HAL_ERR_UNSUPPORTED;

	/* Programs the I/O APIC with the bootstrap local-APIC destination. */
	error = i386_ioapic_init(&topology, i386_lapic_id());

	/* Falls back when I/O-APIC route programming fails. */
	if (error != HAL_OK)
		return HAL_ERR_UNSUPPORTED;

	/* Masks every legacy interrupt while the routing mode changes. */
	pic_init();

	/* Routes external interrupts through APIC mode when IMCR is present. */
	if (topology.imcr_present)
		imcr_to_apic();

	/* Publishes APIC mode and starts local timer and secondary CPUs. */
	apic_mode = 1;
	i386_lapic_timer_start(timer_ticks);
	i386_smp_configure(&topology);

	/* Reports a completed APIC selection. */
	return HAL_OK;
}

/*
 * Consumes one PIT tick during local-APIC timer calibration.
 */
int
i386_interrupt_calibration_tick(
	void)
{
	/* Leaves ordinary timer ticks for normal IRQ dispatch. */
	if (!calibration_active || apic_mode || calibration_stage >= 2U)
		return 0;

	/* Completes the calibration tick at the still-active legacy PIC. */
	pic_send_eoi(0);

	/* Starts the local timer on the first tick and stops on the second. */
	if (calibration_stage == 0U) {
		i386_lapic_timer_prepare();
		calibration_stage = 1U;
	} else {
		calibration_stage = 2U;
	}

	/* Reports that calibration consumed this timer interrupt. */
	return 1;
}

/*
 * Reports whether the APIC interrupt path is active.
 */
int
i386_interrupt_uses_apic(
	void)
{
	/* Returns the selected controller mode. */
	return apic_mode;
}

/*
 * Validates one delivered IRQ against the active controller.
 */
int
i386_interrupt_validate(
	int irq)
{
	int in_service;

	/* APIC vectors already identify the delivered IRQ directly. */
	if (apic_mode)
		return irq;

	/* Reads the legacy controller's in-service IRQ. */
	in_service = pic_get_irq_in_service();

	/* Returns the legacy in-service result. */
	return in_service;
}

/*
 * Masks one IRQ at the active interrupt controller.
 */
void
i386_interrupt_mask(
	int irq)
{
	/* Programs the selected APIC or legacy PIC path. */
	if (apic_mode) {
		i386_ioapic_mask(irq);
	} else {
		pic_set_irq_mask(irq, 1);
	}
}

/*
 * Unmasks one IRQ at the active interrupt controller.
 */
void
i386_interrupt_unmask(
	int irq)
{
	/* Programs the selected APIC or legacy PIC path. */
	if (apic_mode) {
		i386_ioapic_unmask(irq);
	} else {
		pic_set_irq_mask(irq, 0);
	}
}

/*
 * Completes one IRQ at the active interrupt controller.
 */
void
i386_interrupt_eoi(
	int irq)
{
	/* Signals the selected local APIC or legacy PIC path. */
	if (apic_mode) {
		i386_lapic_eoi();
	} else {
		pic_send_eoi(irq);
	}
}

/*
 * Routes one IRQ to a configured CPU.
 */
int
i386_interrupt_route(
	int irq,
	hal_cpu_id_t cpu)
{
	uint8_t apic_id;
	int result;

	/* Supports only the bootstrap CPU while the legacy PIC is active. */
	if (!apic_mode) {
		/* Reports the bootstrap route as already effective. */
		if (cpu == 0)
			return HAL_OK;
		return HAL_ERR_UNSUPPORTED;
	}

	/* Resolves the logical CPU to its firmware local-APIC identifier. */
	result = i386_smp_apic_id(cpu, &apic_id);

	/* Rejects a logical CPU without a configured APIC identifier. */
	if (result != HAL_OK)
		return HAL_ERR_INVALID;

	/* Programs and reports the I/O-APIC route. */
	result = i386_ioapic_route(irq, apic_id);

	/* Returns the route-programming result. */
	return result;
}

/*
 * Reports the calibrated local-APIC timer interval.
 */
uint32_t
i386_interrupt_timer_ticks(
	void)
{
	/* Returns the measured interval. */
	return timer_ticks;
}

/* Tests whether one service has active handler or acknowledgement state. */
static int
service_active(
	const struct irq_service_info *service)
{
	hal_cpu_id_t cpu;

	/* Checks every configured CPU's service state. */
	for (cpu = 0; cpu < hal_cpu_count(); cpu++) {
		/* Reports the first active handler or acknowledgement slot. */
		if (service->in_handler[cpu] || service->in_flight[cpu])
			return 1;
	}

	/* Reports an idle service. */
	return 0;
}

/* Starts acknowledgement tracking for one IRQ. */
static hal_irq_ack_t
begin_ack(
	int irq)
{
	struct irq_service_info *service;
	hal_cpu_id_t cpu;

	/* Selects the current CPU's acknowledgement slot. */
	service = &irq_service[irq];
	cpu = hal_cpu_current();

	/* Rejects a nested acknowledgement on the same CPU and IRQ. */
	if (service->in_flight[cpu])
		HAL_FATAL("nested i386 IRQ acknowledgement");

	/* Marks the delivery active before returning its one-based token. */
	service->in_flight[cpu] = 1;

	/* Returns the controller acknowledgement token. */
	return (hal_irq_ack_t)(unsigned)(irq + 1);
}

/* Routes legacy external interrupts through the APIC fabric. */
static void
imcr_to_apic(
	void)
{
	/* Selects and programs the IMCR routing register in hardware order. */
	asm_outb(0x22U, 0x70U);
	asm_outb(0x23U, 0x01U);
}
