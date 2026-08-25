/* amd64 xAPIC/I/O APIC IRQ contract and task-based IRQ service. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

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

enum irq_mode { IRQ_MODE_NONE, IRQ_MODE_REALTIME, IRQ_MODE_TASK };
static struct irq_service_info irq_service[IRQ_LOGICAL_MAX + 1];

static int
valid_irq(int irq)
{
	return irq >= 0 && irq <= IRQ_LOGICAL_MAX;
}

static int
is_msi_irq(int irq)
{
	return irq >= IRQ_MSI_BASE && irq <= IRQ_LOGICAL_MAX;
}

static void
hardware_mask(int irq)
{
	if (!is_msi_irq(irq))
		amd64_ioapic_mask(irq);
}

static void
hardware_unmask(int irq)
{
	if (!is_msi_irq(irq))
		amd64_ioapic_unmask(irq);
}

static bool
service_lock(struct irq_service_info *service)
{
	bool enabled = hal_irq_disable();
	while (__atomic_exchange_n(&service->lock, 1U,
	    __ATOMIC_ACQUIRE) != 0)
		__asm__ volatile("pause");
	return enabled;
}

static void
service_unlock(struct irq_service_info *service, bool enabled)
{
	__atomic_store_n(&service->lock, 0U, __ATOMIC_RELEASE);
	if (enabled)
		hal_irq_enable();
}

void
irq_init(const struct amd64_acpi_info *acpi)
{
	unsigned irq;
	hal_memset(irq_service, 0, sizeof(irq_service));
	for (irq = 0; irq <= IRQ_LOGICAL_MAX; irq++) {
		hal_cpu_mask_set(&irq_service[irq].requested, 0);
		irq_service[irq].masked = 1;
		irq_service[irq].handler_cpu = HAL_CPU_MAX;
	}
	pic_init(); /* Leaves both legacy PICs fully masked. */
	if (amd64_ioapic_init(acpi, amd64_smp_apic_id(0)) != HAL_OK)
		HAL_FATAL("I/O APIC initialization failed");
}

bool
hal_irq_disable(void)
{
	bool enabled = (asm_get_rflags() & 0x200U) != 0;
	asm_cli();
	return enabled;
}

void hal_irq_enable(void) { asm_sti(); }
void
hal_irq_mask(int irq)
{
	struct irq_service_info *service;
	bool enabled;
	if (irq <= IRQ_TIMER || !valid_irq(irq)) return;
	service = &irq_service[irq];
	enabled = service_lock(service);
	service->masked = 1;
	hardware_mask(irq);
	service_unlock(service, enabled);
}
void
hal_irq_unmask(int irq)
{
	struct irq_service_info *service;
	bool enabled;
	if (irq <= IRQ_TIMER || !valid_irq(irq)) return;
	service = &irq_service[irq];
	enabled = service_lock(service);
	service->masked = 0;
	hardware_unmask(irq);
	service_unlock(service, enabled);
}

void
hal_irq_send_eoi(hal_irq_ack_t acknowledge)
{
	struct amd64_percpu *cpu = amd64_percpu_current();
	struct amd64_irq_ack *record = (void *)(uintptr_t)acknowledge;
	if (acknowledge == HAL_IRQ_ACK_NONE ||
	    cpu->acknowledgement_depth == 0 ||
	    record != &cpu->acknowledgements[cpu->acknowledgement_depth - 1U] ||
	    !record->active)
		HAL_FATAL("invalid amd64 APIC acknowledgement");
	amd64_lapic_eoi();
	record->active = 0;
	cpu->acknowledgement_depth--;
}

int
hal_irq_set_handler(int irq, hal_irq_handler_t handler, void *argument)
{
	struct irq_service_info *service;
	bool enabled;
	if (irq <= IRQ_TIMER || !valid_irq(irq) ||
	    (handler == NULL && argument != NULL))
		return HAL_ERR_INVALID;
	service = &irq_service[irq];
	enabled = service_lock(service);
	if (handler == NULL) {
		if (service->in_handler && service->handler_cpu ==
		    hal_cpu_current()) {
			service_unlock(service, enabled);
			return HAL_ERR_BUSY;
		}
		service->removing = 1;
		service->masked = 1;
		hardware_mask(irq);
		if (service->mode == IRQ_MODE_TASK) {
			service->removing = 0;
			service_unlock(service, enabled);
			return HAL_ERR_BUSY;
		}
		service_unlock(service, enabled);
		while (__atomic_load_n(&service->in_handler,
		    __ATOMIC_ACQUIRE) != 0 || __atomic_load_n(
		    &service->in_flight, __ATOMIC_ACQUIRE) != 0)
			__asm__ volatile("pause");
		enabled = service_lock(service);
		service->mode = IRQ_MODE_NONE;
		service->handler = NULL;
		service->argument = NULL;
		service->removing = 0;
	} else {
		if (service->mode != IRQ_MODE_NONE || service->removing) {
			service_unlock(service, enabled);
			return HAL_ERR_BUSY;
		}
		service->mode = IRQ_MODE_REALTIME;
		service->handler = handler;
		service->argument = argument;
	}
	service_unlock(service, enabled);
	return HAL_OK;
}

int
hal_irq_service_wait(int irq, hal_irq_ack_t *acknowledge)
{
	struct irq_service_info *service;
	bool enabled;
	if (irq <= IRQ_TIMER || !valid_irq(irq) || acknowledge == NULL)
		return HAL_ERR_INVALID;
	if (is_msi_irq(irq))
		return HAL_ERR_UNSUPPORTED;
	for (;;) {
		service = &irq_service[irq];
		enabled = service_lock(service);
		if (service->mode == IRQ_MODE_REALTIME ||
		    service->removing ||
		    (service->waiter != NULL &&
		    service->waiter != hal_task_get_current())) {
			service_unlock(service, enabled);
			return HAL_ERR_BUSY;
		}
		service->mode = IRQ_MODE_TASK;
		if (service->waiter == NULL) {
			hal_cpu_id_t cpu = hal_cpu_current();
			if (amd64_ioapic_route(irq,
			    amd64_smp_apic_id(cpu)) != HAL_OK) {
				service->mode = IRQ_MODE_NONE;
				service_unlock(service, enabled);
				return HAL_ERR_UNSUPPORTED;
			}
			hal_cpu_mask_zero(&service->requested);
			hal_cpu_mask_set(&service->requested, cpu);
			service->waiter = hal_task_get_current();
		}
		if (service->waiter == NULL) {
			service->mode = IRQ_MODE_NONE;
			service_unlock(service, enabled);
			return HAL_ERR_STATE;
		}
		if (service->pending) {
			service->pending = 0;
			*acknowledge = service->acknowledge;
			service_unlock(service, false);
			return HAL_OK;
		}
		service->masked = 0;
		amd64_ioapic_unmask(irq);
		service_unlock(service, enabled);
		kernel_wait_task();
	}
}

int
hal_irq_set_affinity(int irq, const struct hal_cpu_mask *requested)
{
	hal_cpu_id_t cpu, selected = HAL_CPU_MAX;
	struct hal_cpu_mask ready;
	struct irq_service_info *service;
	bool enabled;
	unsigned was_masked;
	if (irq <= IRQ_TIMER || !valid_irq(irq) || requested == NULL)
		return HAL_ERR_INVALID;
	if (is_msi_irq(irq))
		return HAL_ERR_UNSUPPORTED;
	hal_cpu_ready_mask(&ready);
	for (cpu = 0; cpu < HAL_CPU_MAX; cpu++)
		if (hal_cpu_mask_test(requested, cpu) &&
		    hal_cpu_mask_test(&ready, cpu)) {
			selected = cpu;
			break;
		}
	if (selected == HAL_CPU_MAX)
		return HAL_ERR_INVALID;
	service = &irq_service[irq];
	enabled = service_lock(service);
	if (service->in_handler || service->in_flight || service->removing) {
		service_unlock(service, enabled);
		return HAL_ERR_BUSY;
	}
	was_masked = service->masked;
	service->masked = 1;
	amd64_ioapic_mask(irq);
	if (amd64_ioapic_route(irq, amd64_smp_apic_id(selected)) != HAL_OK) {
		service->masked = was_masked;
		if (!was_masked) amd64_ioapic_unmask(irq);
		service_unlock(service, enabled);
		return HAL_ERR_UNSUPPORTED;
	}
	service->requested = *requested;
	service->masked = was_masked;
	if (!was_masked) amd64_ioapic_unmask(irq);
	service_unlock(service, enabled);
	return HAL_OK;
}

int
hal_irq_get_affinity(int irq, struct hal_irq_affinity *result)
{
	hal_cpu_id_t cpu;
	struct hal_cpu_mask ready;
	struct irq_service_info *service;
	bool enabled;
	if (irq <= IRQ_TIMER || !valid_irq(irq) || result == NULL)
		return HAL_ERR_INVALID;
	service = &irq_service[irq];
	enabled = service_lock(service);
	result->requested = service->requested;
	hal_cpu_mask_zero(&result->effective);
	hal_cpu_ready_mask(&ready);
	for (cpu = 0; cpu < HAL_CPU_MAX; cpu++)
		if (hal_cpu_mask_test(&result->requested, cpu) &&
		    hal_cpu_mask_test(&ready, cpu)) {
			hal_cpu_mask_set(&result->effective, cpu);
			break;
		}
	service_unlock(service, enabled);
	return HAL_OK;
}

void
irq_handler(int irq)
{
	struct irq_service_info *service;
	hal_irq_handler_t handler;
	void *argument;
	hal_irq_ack_t acknowledge;
	if (!valid_irq(irq))
		HAL_FATAL("invalid amd64 APIC IRQ");
	acknowledge = amd64_irq_ack_begin(is_msi_irq(irq) ?
	    AMD64_VECTOR_MSI_BASE + (uint32_t)(irq - IRQ_MSI_BASE) :
	    INT_IRQ_BASE + (uint32_t)irq, irq);
	if (irq == IRQ_TIMER) {
		clock_handler();
		kernel_timer_handler(hal_cpu_current(), acknowledge);
		return;
	}
	service = &irq_service[irq];
	(void)service_lock(service);
	__atomic_store_n(&service->in_flight, 1U, __ATOMIC_RELEASE);
	if (!service->removing && !service->masked &&
	    service->mode == IRQ_MODE_REALTIME &&
	    service->handler != NULL) {
		handler = service->handler;
		argument = service->argument;
		service->handler_cpu = hal_cpu_current();
		__atomic_store_n(&service->in_handler, 1U, __ATOMIC_RELEASE);
		service_unlock(service, false);
		handler(irq, acknowledge, argument);
		if (((struct amd64_irq_ack *)(uintptr_t)acknowledge)->active)
			HAL_FATAL("amd64 realtime IRQ handler omitted EOI");
		(void)service_lock(service);
		service->handler_cpu = HAL_CPU_MAX;
		__atomic_store_n(&service->in_handler, 0U, __ATOMIC_RELEASE);
		__atomic_store_n(&service->in_flight, 0U, __ATOMIC_RELEASE);
		service_unlock(service, false);
		return;
	}
	if (!service->removing && !service->masked &&
	    service->mode == IRQ_MODE_TASK &&
	    service->waiter != NULL) {
		hal_task_t waiter = service->waiter;
		amd64_ioapic_mask(irq);
		service->masked = 1;
		service->acknowledge = acknowledge;
		service->pending = 1;
		__atomic_store_n(&service->in_flight, 0U, __ATOMIC_RELEASE);
		service_unlock(service, false);
		kernel_notify_task(waiter);
		return;
	}
	hardware_mask(irq);
	service->masked = 1;
	__atomic_store_n(&service->in_flight, 0U, __ATOMIC_RELEASE);
	service_unlock(service, false);
	hal_irq_send_eoi(acknowledge);
}

void
amd64_notify_interrupt(void)
{
	hal_irq_ack_t acknowledge = amd64_irq_ack_begin(
	    AMD64_VECTOR_NOTIFY, -1);
	kernel_cpu_notify_handler(hal_cpu_current(), acknowledge);
}

void
amd64_error_interrupt(void)
{
	hal_irq_ack_t acknowledge = amd64_irq_ack_begin(
	    AMD64_VECTOR_ERROR, -1);
	hal_irq_send_eoi(acknowledge);
	HAL_FATAL("Local APIC error interrupt");
}

int
amd64_irq_task_transferable(hal_task_t task)
{
	int irq;
	if (task == NULL)
		return 0;
	for (irq = 1; irq <= IRQ_LOGICAL_MAX; irq++) {
		struct irq_service_info *service = &irq_service[irq];
		bool enabled = service_lock(service);
		if (service->waiter == task) {
			service_unlock(service, enabled);
			return 0;
		}
		service_unlock(service, enabled);
	}
	return 1;
}

int
hal_irq_register_msi(const char *source, hal_irq_handler_t handler,
	void *handler_arg, int *mapped_irq, paddr_t *mapped_addr,
	uint32_t *mapped_event)
{
	int irq;
	bool enabled;

	if (!amd64_msi_source_valid(source) || handler == NULL || mapped_irq == NULL ||
	    mapped_addr == NULL || mapped_event == NULL)
		return HAL_ERR_INVALID;
	for (irq = IRQ_MSI_BASE; irq <= IRQ_LOGICAL_MAX; irq++) {
		struct irq_service_info *service = &irq_service[irq];
		enabled = service_lock(service);
		if (!service->allocated && service->mode == IRQ_MODE_NONE &&
		    !service->removing) {
			service->allocated = 1;
			service->msi = 1;
			service->masked = 0;
			service->mode = IRQ_MODE_REALTIME;
			service->handler = handler;
			service->argument = handler_arg;
			service_unlock(service, enabled);
			*mapped_irq = irq;
			*mapped_addr = (paddr_t)(0xfee00000U |
			    (amd64_smp_apic_id(0) << 12));
			*mapped_event = AMD64_VECTOR_MSI_BASE +
			    (uint32_t)(irq - IRQ_MSI_BASE);
			return HAL_OK;
		}
		service_unlock(service, enabled);
	}
	return HAL_ERR_NOMEM;
}

int
hal_irq_unregister_msi(int mapped_irq)
{
	struct irq_service_info *service;
	bool enabled;
	int error;

	if (!is_msi_irq(mapped_irq))
		return HAL_ERR_INVALID;
	service = &irq_service[mapped_irq];
	enabled = service_lock(service);
	if (!service->allocated || !service->msi || service->removing) {
		service_unlock(service, enabled);
		return HAL_ERR_INVALID;
	}
	service->removing = 1;
	service_unlock(service, enabled);
	error = hal_irq_set_handler(mapped_irq, NULL, NULL);
	if (error != HAL_OK) {
		enabled = service_lock(service);
		service->removing = 0;
		service_unlock(service, enabled);
		return error;
	}
	enabled = service_lock(service);
	service->allocated = 0;
	service->msi = 0;
	service->masked = 1;
	service_unlock(service, enabled);
	return HAL_OK;
}
