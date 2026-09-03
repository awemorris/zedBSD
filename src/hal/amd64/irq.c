/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 xAPIC and I/O APIC interrupt service implementation.
 */

#include <hal/hal.h>
#include "irq.h"
#include "clock.h"
#include "asm.h"
#include "percpu.h"
#include "smp.h"
#include "bsp-pcat/acpi.h"
#include "bsp-pcat/ioapic.h"
#include "bsp-pcat/lapic.h"
#include "pic.h"

enum irq_mode {
	IRQ_MODE_NONE,
	IRQ_MODE_REALTIME,
	IRQ_MODE_TASK
};

static struct irq_service_info irq_service[IRQ_LOGICAL_MAX + 1];

static int valid_irq(int irq);
static int is_msi_irq(int irq);
static void hardware_mask(int irq);
static void hardware_unmask(int irq);
static bool service_lock(struct irq_service_info *service);
static void service_unlock(struct irq_service_info *service, bool enabled);

/*
 * Initializes amd64 interrupt service and hardware routing state.
 */
void
irq_init(
	const struct amd64_acpi_info *acpi)
{
	uint32_t bsp_apic_id;
	unsigned irq;
	int error;

	/* Initializes every logical IRQ as masked and BSP-requested. */
	hal_memset(irq_service, 0, sizeof(irq_service));
	for (irq = 0; irq <= IRQ_LOGICAL_MAX; irq++) {
		hal_cpu_mask_set(&irq_service[irq].requested, 0);
		irq_service[irq].masked = 1;
		irq_service[irq].handler_cpu = HAL_CPU_MAX;
	}

	/* Leaves both legacy programmable interrupt controllers masked. */
	pic_init();

	/* Initializes I/O APIC routing toward the BSP. */
	bsp_apic_id = amd64_smp_apic_id(0);
	error = amd64_ioapic_init(acpi, bsp_apic_id);
	if (error != HAL_OK)
		HAL_FATAL("I/O APIC initialization failed");
}

/*
 * Disables local interrupts and reports their previous state.
 */
bool
hal_irq_disable(
	void)
{
	uint64_t flags;
	bool enabled;

	/* Samples interrupt state before disabling delivery. */
	flags = asm_get_rflags();
	enabled = (flags & 0x200U) != 0;
	asm_cli();

	/* Returns whether local interrupts were enabled. */
	return enabled;
}

/*
 * Enables local interrupt delivery.
 */
void
hal_irq_enable(
	void)
{
	/* Enables maskable interrupts on the current CPU. */
	asm_sti();
}

/*
 * Masks one external logical interrupt.
 */
void
hal_irq_mask(
	int irq)
{
	struct irq_service_info *service;
	bool enabled;

	/* Leaves the timer and invalid logical IRQs unchanged. */
	if (irq <= IRQ_TIMER || !valid_irq(irq))
		return;

	/* Serializes the software state and matching hardware mask. */
	service = &irq_service[irq];
	enabled = service_lock(service);
	service->masked = 1;
	hardware_mask(irq);
	service_unlock(service, enabled);
}

/*
 * Unmasks one external logical interrupt.
 */
void
hal_irq_unmask(
	int irq)
{
	struct irq_service_info *service;
	bool enabled;

	/* Leaves the timer and invalid logical IRQs unchanged. */
	if (irq <= IRQ_TIMER || !valid_irq(irq))
		return;

	/* Serializes the software state and matching hardware unmask. */
	service = &irq_service[irq];
	enabled = service_lock(service);
	service->masked = 0;
	hardware_unmask(irq);
	service_unlock(service, enabled);
}

/*
 * Completes the current CPU's topmost interrupt acknowledgement.
 */
void
hal_irq_send_eoi(
	hal_irq_ack_t acknowledge)
{
	struct amd64_percpu *cpu;
	struct amd64_irq_ack *record;

	/* Resolves the acknowledgement against this CPU's strict stack. */
	cpu = amd64_percpu_current();
	record = (void *)(uintptr_t)acknowledge;
	if (acknowledge == HAL_IRQ_ACK_NONE ||
	    cpu->acknowledgement_depth == 0 ||
	    record !=
	    &cpu->acknowledgements[cpu->acknowledgement_depth - 1U] ||
	    !record->active)
		HAL_FATAL("invalid amd64 APIC acknowledgement");

	/* Acknowledges hardware before retiring the software record. */
	amd64_lapic_eoi();
	record->active = 0;
	cpu->acknowledgement_depth--;
}

/*
 * Installs or removes a real-time interrupt handler.
 */
int
hal_irq_set_handler(
	int irq,
	hal_irq_handler_t handler,
	void *argument)
{
	struct irq_service_info *service;
	bool enabled;

	/* Validates the IRQ and handler-argument pairing. */
	if (irq <= IRQ_TIMER ||
	    !valid_irq(irq) ||
	    (handler == NULL && argument != NULL))
		return HAL_ERR_INVALID;

	/* Serializes this service record before changing ownership. */
	service = &irq_service[irq];
	enabled = service_lock(service);

	/* Selects removal or installation without overlapping ownership. */
	if (handler == NULL) {
		/* Prevents a handler from removing itself on its current CPU. */
		if (service->in_handler &&
		    service->handler_cpu == hal_cpu_current()) {
			service_unlock(service, enabled);
			return HAL_ERR_BUSY;
		}

		/* Blocks new delivery and masks hardware before draining. */
		service->removing = 1;
		service->masked = 1;
		hardware_mask(irq);

		/* Keeps task-wait ownership until its waiter releases it. */
		if (service->mode == IRQ_MODE_TASK) {
			service->removing = 0;
			service_unlock(service, enabled);
			return HAL_ERR_BUSY;
		}

		/* Waits without the lock for every active delivery to leave. */
		service_unlock(service, enabled);
		while (__atomic_load_n(
		    &service->in_handler,
		    __ATOMIC_ACQUIRE) != 0 ||
		    __atomic_load_n(
		    &service->in_flight,
		    __ATOMIC_ACQUIRE) != 0) {
			__asm__ volatile("pause");
		}

		/* Clears handler ownership after the drain completes. */
		enabled = service_lock(service);
		service->mode = IRQ_MODE_NONE;
		service->handler = NULL;
		service->argument = NULL;
		service->removing = 0;
	} else {
		/* Requires a completely unowned service record. */
		if (service->mode != IRQ_MODE_NONE || service->removing) {
			service_unlock(service, enabled);
			return HAL_ERR_BUSY;
		}

		/* Publishes the real-time callback and its argument. */
		service->mode = IRQ_MODE_REALTIME;
		service->handler = handler;
		service->argument = argument;
	}

	/* Releases the service record and restores interrupt state. */
	service_unlock(service, enabled);

	/* Reports a completed handler update. */
	return HAL_OK;
}

/*
 * Waits for one task-serviced interrupt delivery.
 */
int
hal_irq_service_wait(
	int irq,
	hal_irq_ack_t *acknowledge)
{
	struct irq_service_info *service;
	hal_task_t current;
	hal_cpu_id_t cpu;
	uint32_t apic_id;
	bool enabled;
	int error;

	/* Validates the task-service request. */
	if (irq <= IRQ_TIMER || !valid_irq(irq) || acknowledge == NULL)
		return HAL_ERR_INVALID;

	/* Rejects MSI vectors that cannot be task-routed through the I/O APIC. */
	if (is_msi_irq(irq))
		return HAL_ERR_UNSUPPORTED;

	/* Rechecks the service state after every scheduler wakeup. */
	for (;;) {
		service = &irq_service[irq];
		enabled = service_lock(service);

		/* Rejects incompatible callback ownership or removal. */
		if (service->mode == IRQ_MODE_REALTIME || service->removing) {
			service_unlock(service, enabled);
			return HAL_ERR_BUSY;
		}

		/* Allows only the established waiter to retain task ownership. */
		if (service->waiter != NULL) {
			current = hal_task_get_current();

			/* Rejects a different task attempting to share this IRQ waiter. */
			if (service->waiter != current) {
				service_unlock(service, enabled);
				return HAL_ERR_BUSY;
			}
		}

		/* Claims task-service mode for this waiter. */
		service->mode = IRQ_MODE_TASK;
		if (service->waiter == NULL) {
			cpu = hal_cpu_current();
			apic_id = amd64_smp_apic_id(cpu);
			error = amd64_ioapic_route(irq, apic_id);

			/* Rolls back ownership when hardware cannot route it. */
			if (error != HAL_OK) {
				service->mode = IRQ_MODE_NONE;
				service_unlock(service, enabled);
				return HAL_ERR_UNSUPPORTED;
			}

			/* Records the route and current task as its sole waiter. */
			hal_cpu_mask_zero(&service->requested);
			hal_cpu_mask_set(&service->requested, cpu);
			service->waiter = hal_task_get_current();
		}

		/* Rejects an absent current task after routing. */
		if (service->waiter == NULL) {
			service->mode = IRQ_MODE_NONE;
			service_unlock(service, enabled);
			return HAL_ERR_STATE;
		}

		/* Transfers a pending acknowledgement to the waiter. */
		if (service->pending) {
			service->pending = 0;
			*acknowledge = service->acknowledge;
			service_unlock(service, false);
			return HAL_OK;
		}

		/* Arms hardware before blocking the current task. */
		service->masked = 0;
		amd64_ioapic_unmask(irq);
		service_unlock(service, enabled);
		kernel_wait_task();
	}
}

/*
 * Selects the requested CPU affinity for one external IRQ.
 */
int
hal_irq_set_affinity(
	int irq,
	const struct hal_cpu_mask *requested)
{
	hal_cpu_id_t cpu;
	hal_cpu_id_t selected;
	struct hal_cpu_mask ready;
	struct irq_service_info *service;
	uint32_t apic_id;
	bool enabled;
	unsigned was_masked;
	int error;

	/* Validates the external interrupt affinity request. */
	selected = HAL_CPU_MAX;
	if (irq <= IRQ_TIMER || !valid_irq(irq) || requested == NULL)
		return HAL_ERR_INVALID;

	/* Rejects MSI vectors whose destination is fixed at allocation. */
	if (is_msi_irq(irq))
		return HAL_ERR_UNSUPPORTED;

	/* Selects the first requested CPU that is currently ready. */
	hal_cpu_ready_mask(&ready);
	for (cpu = 0; cpu < HAL_CPU_MAX; cpu++) {
		/* Skips CPUs absent from the requested affinity mask. */
		if (!hal_cpu_mask_test(requested, cpu))
			continue;

		/* Selects the first requested CPU which is currently ready. */
		if (hal_cpu_mask_test(&ready, cpu)) {
			selected = cpu;
			break;
		}
	}

	/* Rejects a mask without any ready CPU. */
	if (selected == HAL_CPU_MAX)
		return HAL_ERR_INVALID;

	/* Serializes the route update with handler activity. */
	service = &irq_service[irq];
	enabled = service_lock(service);
	if (service->in_handler || service->in_flight || service->removing) {
		service_unlock(service, enabled);
		return HAL_ERR_BUSY;
	}

	/* Masks the line while programming its new destination. */
	was_masked = service->masked;
	service->masked = 1;
	amd64_ioapic_mask(irq);
	apic_id = amd64_smp_apic_id(selected);
	error = amd64_ioapic_route(irq, apic_id);

	/* Restores the prior state after an unsupported route. */
	if (error != HAL_OK) {
		service->masked = was_masked;

		/* Restores delivery when the line was previously unmasked. */
		if (!was_masked)
			amd64_ioapic_unmask(irq);
		service_unlock(service, enabled);
		return HAL_ERR_UNSUPPORTED;
	}

	/* Publishes the requested mask and restores the prior mask state. */
	service->requested = *requested;
	service->masked = was_masked;
	if (!was_masked)
		amd64_ioapic_unmask(irq);
	service_unlock(service, enabled);

	/* Reports a completed route update. */
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
	hal_cpu_id_t cpu;
	struct hal_cpu_mask ready;
	struct irq_service_info *service;
	bool enabled;

	/* Validates the IRQ and result destination. */
	if (irq <= IRQ_TIMER || !valid_irq(irq) || result == NULL)
		return HAL_ERR_INVALID;

	/* Snapshots the requested mask under the service lock. */
	service = &irq_service[irq];
	enabled = service_lock(service);
	result->requested = service->requested;
	hal_cpu_mask_zero(&result->effective);
	hal_cpu_ready_mask(&ready);

	/* Selects the first ready CPU from the requested mask. */
	for (cpu = 0; cpu < HAL_CPU_MAX; cpu++) {
		/* Skips CPUs absent from the stored requested mask. */
		if (!hal_cpu_mask_test(&result->requested, cpu))
			continue;

		/* Publishes the first requested CPU which is currently ready. */
		if (hal_cpu_mask_test(&ready, cpu)) {
			hal_cpu_mask_set(&result->effective, cpu);
			break;
		}
	}
	service_unlock(service, enabled);

	/* Reports a completed affinity snapshot. */
	return HAL_OK;
}

/*
 * Dispatches one logical interrupt from an assembly entry point.
 */
void
irq_handler(
	int irq)
{
	struct irq_service_info *service;
	struct amd64_irq_ack *record;
	hal_irq_handler_t handler;
	hal_task_t waiter;
	hal_cpu_id_t cpu;
	void *argument;
	hal_irq_ack_t acknowledge;
	uint32_t vector;

	/* Requires a valid logical interrupt number. */
	if (!valid_irq(irq))
		HAL_FATAL("invalid amd64 APIC IRQ");

	/* Selects the architectural vector for the acknowledgement record. */
	if (is_msi_irq(irq)) {
		vector = AMD64_VECTOR_MSI_BASE +
		    (uint32_t)(irq - IRQ_MSI_BASE);
	} else {
		vector = INT_IRQ_BASE + (uint32_t)irq;
	}
	acknowledge = amd64_irq_ack_begin(vector, irq);

	/* Gives the timer its dedicated non-service dispatch path. */
	if (irq == IRQ_TIMER) {
		clock_handler();
		cpu = hal_cpu_current();
		kernel_timer_handler(cpu, acknowledge);
		return;
	}

	/* Marks this interrupt in flight under its service lock. */
	service = &irq_service[irq];
	(void)service_lock(service);
	__atomic_store_n(&service->in_flight, 1U, __ATOMIC_RELEASE);

	/* Dispatches an installed and unmasked real-time callback. */
	if (!service->removing &&
	    !service->masked &&
	    service->mode == IRQ_MODE_REALTIME &&
	    service->handler != NULL) {
		handler = service->handler;
		argument = service->argument;
		service->handler_cpu = hal_cpu_current();
		__atomic_store_n(&service->in_handler, 1U, __ATOMIC_RELEASE);
		service_unlock(service, false);
		handler(irq, acknowledge, argument);

		/* Requires the callback to acknowledge before returning. */
		record = (struct amd64_irq_ack *)(uintptr_t)acknowledge;
		if (record->active)
			HAL_FATAL("amd64 realtime IRQ handler omitted EOI");

		/* Retires handler and in-flight state under the service lock. */
		(void)service_lock(service);
		service->handler_cpu = HAL_CPU_MAX;
		__atomic_store_n(&service->in_handler, 0U, __ATOMIC_RELEASE);
		__atomic_store_n(&service->in_flight, 0U, __ATOMIC_RELEASE);
		service_unlock(service, false);
		return;
	}

	/* Transfers an installed task-mode delivery to its waiter. */
	if (!service->removing &&
	    !service->masked &&
	    service->mode == IRQ_MODE_TASK &&
	    service->waiter != NULL) {
		waiter = service->waiter;
		amd64_ioapic_mask(irq);
		service->masked = 1;
		service->acknowledge = acknowledge;
		service->pending = 1;
		__atomic_store_n(&service->in_flight, 0U, __ATOMIC_RELEASE);
		service_unlock(service, false);
		kernel_notify_task(waiter);
		return;
	}

	/* Masks and acknowledges an interrupt without an active consumer. */
	hardware_mask(irq);
	service->masked = 1;
	__atomic_store_n(&service->in_flight, 0U, __ATOMIC_RELEASE);
	service_unlock(service, false);
	hal_irq_send_eoi(acknowledge);
}

/*
 * Dispatches a local CPU notification interrupt.
 */
void
amd64_notify_interrupt(
	void)
{
	hal_cpu_id_t cpu;
	hal_irq_ack_t acknowledge;

	/* Opens an acknowledgement record for the local notification vector. */
	acknowledge = amd64_irq_ack_begin(AMD64_VECTOR_NOTIFY, -1);

	/* Gives the notification and acknowledgement to the kernel. */
	cpu = hal_cpu_current();
	kernel_cpu_notify_handler(cpu, acknowledge);
}

/*
 * Acknowledges and reports a local APIC error interrupt.
 */
void
amd64_error_interrupt(
	void)
{
	hal_irq_ack_t acknowledge;

	/* Opens and immediately completes the error acknowledgement. */
	acknowledge = amd64_irq_ack_begin(AMD64_VECTOR_ERROR, -1);
	hal_irq_send_eoi(acknowledge);

	/* Stops after the architectural error report. */
	HAL_FATAL("Local APIC error interrupt");
}

/*
 * Reports whether a task can move without IRQ waiter ownership.
 */
int
amd64_irq_task_transferable(
	hal_task_t task)
{
	struct irq_service_info *service;
	bool enabled;
	int irq;

	/* Rejects an absent task. */
	if (task == NULL)
		return 0;

	/* Searches every external service record for this waiter. */
	for (irq = 1; irq <= IRQ_LOGICAL_MAX; irq++) {
		service = &irq_service[irq];
		enabled = service_lock(service);

		/* Rejects transfer while the task owns an IRQ wait. */
		if (service->waiter == task) {
			service_unlock(service, enabled);
			return 0;
		}
		service_unlock(service, enabled);
	}

	/* Reports that no IRQ service pins this task. */
	return 1;
}

/*
 * Allocates one message-signaled interrupt and callback.
 */
int
hal_irq_register_msi(
	const char *source,
	hal_irq_handler_t handler,
	void *handler_argument,
	int *mapped_irq,
	paddr_t *mapped_address,
	uint32_t *mapped_event)
{
	struct irq_service_info *service;
	bool enabled;
	int source_valid;
	int irq;

	/* Parses the canonical source before validating output arguments. */
	source_valid = amd64_msi_source_valid(source);
	if (!source_valid ||
	    handler == NULL ||
	    mapped_irq == NULL ||
	    mapped_address == NULL ||
	    mapped_event == NULL)
		return HAL_ERR_INVALID;

	/* Claims the first unused MSI service record. */
	for (irq = IRQ_MSI_BASE; irq <= IRQ_LOGICAL_MAX; irq++) {
		service = &irq_service[irq];
		enabled = service_lock(service);

		/* Publishes callback ownership for an available vector. */
		if (!service->allocated &&
		    service->mode == IRQ_MODE_NONE &&
		    !service->removing) {
			service->allocated = 1;
			service->msi = 1;
			service->masked = 0;
			service->mode = IRQ_MODE_REALTIME;
			service->handler = handler;
			service->argument = handler_argument;
			service_unlock(service, enabled);

			/* Returns the BSP-targeted MSI message and logical IRQ. */
			*mapped_irq = irq;
			*mapped_address = (paddr_t)(0xfee00000U |
			    (amd64_smp_apic_id(0) << 12));
			*mapped_event = AMD64_VECTOR_MSI_BASE +
			    (uint32_t)(irq - IRQ_MSI_BASE);
			return HAL_OK;
		}
		service_unlock(service, enabled);
	}

	/* Reports exhaustion of the architecture MSI vector range. */
	return HAL_ERR_NOMEM;
}

/*
 * Releases one message-signaled interrupt allocation.
 */
int
hal_irq_unregister_msi(
	int mapped_irq)
{
	struct irq_service_info *service;
	bool enabled;
	int error;

	/* Requires a logical IRQ from the MSI allocation range. */
	if (!is_msi_irq(mapped_irq))
		return HAL_ERR_INVALID;

	/* Marks a live MSI allocation for removal. */
	service = &irq_service[mapped_irq];
	enabled = service_lock(service);
	if (!service->allocated || !service->msi || service->removing) {
		service_unlock(service, enabled);
		return HAL_ERR_INVALID;
	}
	service->removing = 1;
	service_unlock(service, enabled);

	/* Drains and removes the installed real-time callback. */
	error = hal_irq_set_handler(mapped_irq, NULL, NULL);
	if (error != HAL_OK) {
		enabled = service_lock(service);
		service->removing = 0;
		service_unlock(service, enabled);
		return error;
	}

	/* Returns the drained record to the MSI allocation pool. */
	enabled = service_lock(service);
	service->allocated = 0;
	service->msi = 0;
	service->masked = 1;
	service_unlock(service, enabled);

	/* Reports a completed MSI release. */
	return HAL_OK;
}

/* Reports whether a logical IRQ lies in the supported range. */
static int
valid_irq(
	int irq)
{
	/* Rejects values below the first logical IRQ. */
	if (irq < 0)
		return 0;

	/* Rejects values beyond the allocated MSI vectors. */
	if (irq > IRQ_LOGICAL_MAX)
		return 0;

	/* Reports a supported logical interrupt. */
	return 1;
}

/* Reports whether a logical IRQ uses an MSI vector. */
static int
is_msi_irq(
	int irq)
{
	/* Rejects values below the MSI vector range. */
	if (irq < IRQ_MSI_BASE)
		return 0;

	/* Rejects values beyond the allocated MSI vectors. */
	if (irq > IRQ_LOGICAL_MAX)
		return 0;

	/* Reports a message-signaled interrupt. */
	return 1;
}

/* Masks an interrupt when it has an I/O APIC line. */
static void
hardware_mask(
	int irq)
{
	/* Leaves message-signaled interrupts without software masking. */
	if (is_msi_irq(irq))
		return;

	/* Masks the corresponding I/O APIC line. */
	amd64_ioapic_mask(irq);
}

/* Unmasks an interrupt when it has an I/O APIC line. */
static void
hardware_unmask(
	int irq)
{
	/* Leaves message-signaled interrupts without software masking. */
	if (is_msi_irq(irq))
		return;

	/* Unmasks the corresponding I/O APIC line. */
	amd64_ioapic_unmask(irq);
}

/* Acquires one service lock with local interrupts disabled. */
static bool
service_lock(
	struct irq_service_info *service)
{
	bool enabled;

	/* Preserves interrupt state and acquires the service spin lock. */
	enabled = hal_irq_disable();
	while (__atomic_exchange_n(
	    &service->lock,
	    1U,
	    __ATOMIC_ACQUIRE) != 0) {
		__asm__ volatile("pause");
	}

	/* Returns whether interrupts must be restored on release. */
	return enabled;
}

/* Releases one service lock and restores local interrupt state. */
static void
service_unlock(
	struct irq_service_info *service,
	bool enabled)
{
	/* Publishes protected updates before releasing the lock. */
	__atomic_store_n(&service->lock, 0U, __ATOMIC_RELEASE);

	/* Restores interrupts only when they were originally enabled. */
	if (enabled)
		hal_irq_enable();
}
