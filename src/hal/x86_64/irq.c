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

/*
 * Whether hal_irq_suspend() holds the system-wide sources (ws052-p006).
 * Set and cleared only by hal_irq_suspend() and hal_irq_resume(), with an
 * atomic exchange so that only one suspension is held; each IRQ's own
 * suspended flag (under its service lock) says whether it is held.
 */
static volatile unsigned irq_suspension;

static int valid_irq(int irq);
static int is_msi_irq(int irq);
static void hardware_mask(int irq);
static void hardware_unmask(int irq);
static int irq_set_handler(int irq, hal_irq_handler_t handler, void *argument);
static bool service_lock(struct irq_service_info *service);
static void service_unlock(struct irq_service_info *service, bool enabled);

/*
 * Initializes amd64 interrupt service and hardware routing state.
 */
void
prekern_irq_init(
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

	/* Serializes the software state and matching hardware unmask; a suspended IRQ is unmasked only when resumed. */
	service = &irq_service[irq];
	enabled = service_lock(service);
	service->masked = 0;
	if (!service->suspended)
		hardware_unmask(irq);
	service_unlock(service, enabled);
}

/*
 * Arms or disarms a numbered or message-signalled IRQ as a wake source of
 * a system sleep.
 */
int
hal_irq_set_wake(
	int irq,
	bool enable)
{
	struct irq_service_info *service;
	bool enabled;
	unsigned held;
	int valid;

	/* An external IRQ; the timer is the CPU's own, never a wake source. */
	valid = valid_irq(irq);
	if (irq <= IRQ_TIMER || !valid)
		return HAL_ERR_INVALID;

	/*
	 * The set of wake sources is fixed while a suspension holds the others.
	 * Checked under the IRQ's lock: hal_irq_suspend() raises the flag
	 * before it takes each IRQ's lock, so a 0 seen here means it has not
	 * handled this IRQ yet and will see the new flag.
	 */
	service = &irq_service[irq];
	enabled = service_lock(service);
	held = __atomic_load_n(&irq_suspension, __ATOMIC_ACQUIRE);
	if (held != 0U) {
		service_unlock(service, enabled);
		return HAL_ERR_STATE;
	}

	/* The flag, under the IRQ's lock. */
	service->wake = 0U;
	if (enable)
		service->wake = 1U;
	service_unlock(service, enabled);

	/* Succeeded: kept until it is changed. */
	return HAL_OK;
}

/*
 * Masks every system-wide source but the wake IRQs for a system sleep:
 * each held IRQ's I/O APIC line is masked (an MSI has no line to mask; one
 * that arrives is dropped by irq_handler()), and what the kernel asked of
 * it is kept in its own masked flag for hal_irq_resume().
 */
int
hal_irq_suspend(
	void)
{
	struct irq_service_info *service;
	bool enabled;
	unsigned held;
	int irq;

	/* Only one suspension at a time. */
	held = __atomic_exchange_n(&irq_suspension, 1U, __ATOMIC_ACQ_REL);
	if (held != 0U)
		return HAL_ERR_STATE;

	/* Every external IRQ that is not a wake source, held (the timer and the IPI vectors are not IRQs here). */
	for (irq = IRQ_TIMER + 1; irq <= IRQ_LOGICAL_MAX; irq++) {
		service = &irq_service[irq];
		enabled = service_lock(service);
		if (!service->wake) {
			service->suspended = 1U;
			hardware_mask(irq);
		}

		/* The next IRQ. */
		service_unlock(service, enabled);
	}

	/* Succeeded: only the wake IRQs, the CPU-local sources and the notifications deliver. */
	return HAL_OK;
}

/*
 * Puts back every source hal_irq_suspend() held, each delivered or masked
 * as the kernel last asked.
 */
int
hal_irq_resume(
	void)
{
	struct irq_service_info *service;
	bool enabled;
	unsigned held;
	int irq;

	/* Only a held suspension can be put back. */
	held = __atomic_load_n(&irq_suspension, __ATOMIC_ACQUIRE);
	if (held == 0U)
		return HAL_ERR_STATE;

	/* Every held IRQ: delivered again unless the kernel masked it meanwhile or before. */
	for (irq = IRQ_TIMER + 1; irq <= IRQ_LOGICAL_MAX; irq++) {
		service = &irq_service[irq];
		enabled = service_lock(service);
		if (service->suspended) {
			service->suspended = 0U;
			if (!service->masked)
				hardware_unmask(irq);
		}

		/* The next IRQ. */
		service_unlock(service, enabled);
	}

	/* The suspension ends; wake sources may change again. */
	__atomic_store_n(&irq_suspension, 0U, __ATOMIC_RELEASE);

	/* Succeeded: every source is as the kernel asked. */
	return HAL_OK;
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
static int
irq_set_handler(
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

		/* Keeps task-wait ownership until its waiter releases it; nothing is changed. */
		if (service->mode == IRQ_MODE_TASK) {
			service_unlock(service, enabled);
			return HAL_ERR_BUSY;
		}

		/* Blocks new delivery and masks hardware before draining. */
		service->removing = 1;
		service->masked = 1;
		hardware_mask(irq);

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
 * Registers a real-time handler for one numbered IRQ.
 */
int
hal_irq_register(
	int irq_num,
	hal_irq_handler_t func,
	void *arg)
{
	/* Requires a handler; removal goes through hal_irq_unregister(). */
	if (func == NULL)
		return HAL_ERR_INVALID;

	/* Installs the callback for this line. */
	return irq_set_handler(irq_num, func, arg);
}

/*
 * Removes the real-time handler registered for one numbered IRQ.
 */
int
hal_irq_unregister(
	int irq_num,
	hal_irq_handler_t func,
	void *arg)
{
	struct irq_service_info *service;
	bool enabled;
	int matches;

	/* Requires the registration this caller owns. */
	if (func == NULL || !valid_irq(irq_num))
		return HAL_ERR_INVALID;

	/* Confirms the stored registration before releasing it. */
	service = &irq_service[irq_num];
	enabled = service_lock(service);
	matches = service->handler == func && service->argument == arg;
	service_unlock(service, enabled);
	if (!matches)
		return HAL_ERR_INVALID;

	/* Removes the confirmed callback. */
	return irq_set_handler(irq_num, NULL, NULL);
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
 * Sets the trigger mode and polarity of one I/O APIC IRQ.  The line is
 * masked while its route is rewritten and its mask state is restored
 * after; an MSI vector has no such configuration.
 */
int
hal_irq_set_mode(
	int irq,
	int trigger,
	int polarity)
{
	struct irq_service_info *service;
	bool enabled;
	unsigned was_masked;
	int valid;
	int msi;
	int error;

	/* An external line's IRQ; an MSI vector's delivery is fixed. */
	valid = valid_irq(irq);
	if (irq <= IRQ_TIMER || !valid)
		return HAL_ERR_INVALID;
	msi = is_msi_irq(irq);
	if (msi)
		return HAL_ERR_UNSUPPORTED;

	/* Serialized with the line's deliveries; none may be under way. */
	service = &irq_service[irq];
	enabled = service_lock(service);
	if (service->in_handler || service->in_flight || service->removing) {
		service_unlock(service, enabled);
		return HAL_ERR_BUSY;
	}

	/* Masked while the route changes. */
	was_masked = service->masked;
	service->masked = 1;
	amd64_ioapic_mask(irq);
	error = amd64_ioapic_set_mode(irq, trigger, polarity);

	/* The mask state as it was. */
	service->masked = was_masked;
	if (!was_masked)
		amd64_ioapic_unmask(irq);
	service_unlock(service, enabled);

	/* The route's result. */
	if (error != HAL_OK)
		return error;

	/* Succeeded: the line has the new configuration. */
	return HAL_OK;
}

/*
 * Reports the requested and effective affinity of one IRQ.
 */
int
hal_irq_get_affinity(
	int irq,
	struct hal_cpu_mask *requested,
	struct hal_cpu_mask *effective)
{
	hal_cpu_id_t cpu;
	struct hal_cpu_mask ready;
	struct irq_service_info *service;
	bool enabled;

	/* Validates the IRQ and result destination. */
	if (irq <= IRQ_TIMER || !valid_irq(irq) ||
	    requested == NULL || effective == NULL)
		return HAL_ERR_INVALID;

	/* Snapshots the requested mask under the service lock. */
	service = &irq_service[irq];
	enabled = service_lock(service);
	*requested = service->requested;
	hal_cpu_mask_zero(effective);
	hal_cpu_ready_mask(&ready);

	/* Selects the first ready CPU from the requested mask. */
	for (cpu = 0; cpu < HAL_CPU_MAX; cpu++) {
		/* Skips CPUs absent from the stored requested mask. */
		if (!hal_cpu_mask_test(requested, cpu))
			continue;

		/* Publishes the first requested CPU which is currently ready. */
		if (hal_cpu_mask_test(&ready, cpu)) {
			hal_cpu_mask_set(effective, cpu);
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

	/*
	 * Another delivery of this IRQ still owns the in-flight and handler
	 * flags (one value each): this one is acknowledged and dropped without
	 * touching them, so the owner's drain stays correct.
	 */
	service = &irq_service[irq];
	(void)service_lock(service);
	if (__atomic_load_n(&service->in_flight, __ATOMIC_ACQUIRE) != 0U ||
	    __atomic_load_n(&service->in_handler, __ATOMIC_ACQUIRE) != 0U) {
		service_unlock(service, false);
		hal_irq_send_eoi(acknowledge);
		return;
	}

	/* Marks this interrupt in flight under its service lock. */
	__atomic_store_n(&service->in_flight, 1U, __ATOMIC_RELEASE);

	/*
	 * An IRQ that hal_irq_suspend() holds is acknowledged and dropped, not
	 * kept: its line is masked again (another path may have opened it),
	 * and the kernel's own masked flag is left as it asked.
	 */
	if (service->suspended) {
		hardware_mask(irq);
		__atomic_store_n(&service->in_flight, 0U, __ATOMIC_RELEASE);
		service_unlock(service, false);
		hal_irq_send_eoi(acknowledge);
		return;
	}

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
	error = irq_set_handler(mapped_irq, NULL, NULL);
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

/*
 * Allocates one message-signaled interrupt vector, routing, and message WITHOUT
 * attaching a driver handler.  Leaves the record unattached (mode NONE): an
 * arrival is masked and acknowledged by irq_handler()'s no-consumer path, never
 * dispatched through a NULL handler.
 */
int
hal_irq_alloc_msi(
	const char *source,
	int *mapped_irq,
	paddr_t *mapped_address,
	uint32_t *mapped_event)
{
	struct irq_service_info *service;
	bool enabled;
	int irq;

	/* Parses the canonical source before validating output arguments. */
	if (!amd64_msi_source_valid(source) ||
	    mapped_irq == NULL ||
	    mapped_address == NULL ||
	    mapped_event == NULL)
		return HAL_ERR_INVALID;

	/* Claims the first unused MSI service record, unattached. */
	for (irq = IRQ_MSI_BASE; irq <= IRQ_LOGICAL_MAX; irq++) {
		service = &irq_service[irq];
		enabled = service_lock(service);

		/* Reserves the vector without publishing any callback ownership. */
		if (!service->allocated &&
		    service->mode == IRQ_MODE_NONE &&
		    !service->removing) {
			service->allocated = 1;
			service->msi = 1;
			service->masked = 0;
			service->mode = IRQ_MODE_NONE;
			service->handler = NULL;
			service->argument = NULL;
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
 * Attaches a driver handler to a vector previously allocated by
 * hal_irq_alloc_msi.  The record must still be unattached.
 */
int
hal_irq_attach_msi(
	int mapped_irq,
	hal_irq_handler_t handler,
	void *handler_arg)
{
	struct irq_service_info *service;
	bool enabled;

	/* Requires an MSI vector and an initialised handler. */
	if (!is_msi_irq(mapped_irq) || handler == NULL)
		return HAL_ERR_INVALID;

	/* Requires an allocated, still-unattached MSI record. */
	service = &irq_service[mapped_irq];
	enabled = service_lock(service);
	if (!service->allocated || !service->msi ||
	    service->removing || service->handler != NULL) {
		service_unlock(service, enabled);
		return HAL_ERR_STATE;
	}
	service_unlock(service, enabled);

	/* Publishes the real-time callback (mode NONE -> REALTIME). */
	return irq_set_handler(mapped_irq, handler, handler_arg);
}

/*
 * Detaches the driver handler from an hal_irq_alloc_msi vector, draining any
 * in-flight invocation on every CPU, while KEEPING the vector allocated.
 */
int
hal_irq_detach_msi_sync(
	int mapped_irq,
	hal_irq_handler_t handler,
	void *handler_arg)
{
	struct irq_service_info *service;
	bool enabled;
	int error;

	/* Requires an MSI vector from the allocation range. */
	if (!is_msi_irq(mapped_irq))
		return HAL_ERR_INVALID;

	/* Requires the exact live registration to detach. */
	service = &irq_service[mapped_irq];
	enabled = service_lock(service);
	if (!service->allocated || !service->msi || service->removing ||
	    service->handler != handler || service->argument != handler_arg) {
		service_unlock(service, enabled);
		return HAL_ERR_INVALID;
	}
	service->removing = 1;
	service_unlock(service, enabled);

	/* Drains every active delivery and clears the callback ownership. */
	error = irq_set_handler(mapped_irq, NULL, NULL);

	/* Restores the record to an allocated-but-unattached state. */
	enabled = service_lock(service);
	service->removing = 0;
	service_unlock(service, enabled);
	return error;
}

/*
 * Frees a vector allocated by hal_irq_alloc_msi.  The handler must already be
 * detached and the caller must already have stopped the source.
 */
int
hal_irq_free_msi(
	int mapped_irq)
{
	struct irq_service_info *service;
	bool enabled;

	/* Requires an MSI vector from the allocation range. */
	if (!is_msi_irq(mapped_irq))
		return HAL_ERR_INVALID;

	/* Requires a live MSI allocation. */
	service = &irq_service[mapped_irq];
	enabled = service_lock(service);
	if (!service->allocated || !service->msi) {
		service_unlock(service, enabled);
		return HAL_ERR_INVALID;
	}

	/* Requires the handler detached and drained before reuse. */
	if (service->handler != NULL ||
	    __atomic_load_n(&service->in_handler, __ATOMIC_ACQUIRE) != 0U ||
	    __atomic_load_n(&service->in_flight, __ATOMIC_ACQUIRE) != 0U) {
		service_unlock(service, enabled);
		return HAL_ERR_STATE;
	}

	/* Returns the drained record to the MSI allocation pool. */
	service->allocated = 0;
	service->msi = 0;
	service->masked = 1;
	service->mode = IRQ_MODE_NONE;
	service_unlock(service, enabled);
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
