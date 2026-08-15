/* amd64 xAPIC CPU discovery and INIT-SIPI-SIPI startup. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "asm.h"
#include "descriptor.h"
#include "int.h"
#include "percpu.h"
#include "smp.h"
#include "space.h"
#include "bsp-pcat/lapic.h"

extern uint8 amd64_ap_trampoline_start[];
extern uint8 amd64_ap_trampoline_end[];
extern uint8 amd64_ap_trampoline_cr3[];
extern uint8 amd64_ap_trampoline_stack[];
extern uint8 amd64_ap_trampoline_cpu[];
extern uint8 amd64_ap_trampoline_entry[];

static unsigned present_count = 1;
static struct hal_cpu_mask ready_mask;
static volatile unsigned panic_available;

static void
short_delay(void)
{
	volatile unsigned count;
	for (count = 0; count < 100000U; count++)
		__asm__ volatile("pause");
}

void
amd64_smp_init(const struct amd64_acpi_info *acpi)
{
	struct amd64_percpu *bsp;
	uint32 bsp_apic = amd64_lapic_id();
	unsigned source, target = 1;

	if (acpi == NULL || acpi->cpu_count == 0)
		HAL_FATAL("invalid amd64 CPU topology");
	bsp = amd64_percpu_get(0);
	if (bsp != amd64_percpu_current())
		HAL_FATAL("amd64 BSP per-CPU state mismatch");
	bsp->logical_id = 0;
	bsp->apic_id = bsp_apic;
	bsp->ready = 1;
	amd64_percpu_select(bsp);
	hal_cpu_mask_zero(&ready_mask);
	hal_cpu_mask_set(&ready_mask, 0);
	for (source = 0; source < acpi->cpu_count &&
	    target < AMD64_SMP_MAX_CPUS; source++) {
		struct amd64_percpu *cpu;
		if (acpi->cpus[source].apic_id == bsp_apic)
			continue;
		cpu = amd64_percpu_get(target);
		hal_memset(cpu, 0, sizeof(*cpu));
		cpu->logical_id = target;
		cpu->apic_id = acpi->cpus[source].apic_id;
		target++;
	}
	present_count = target;
	__atomic_store_n(&panic_available, 1U, __ATOMIC_RELEASE);
}

int amd64_smp_panic_available(void)
{ return __atomic_load_n(&panic_available, __ATOMIC_ACQUIRE) != 0; }

hal_cpu_id_t hal_cpu_current(void)
{ return amd64_percpu_current()->logical_id; }
unsigned hal_cpu_count(void) { return present_count; }

void
hal_cpu_ready_mask(struct hal_cpu_mask *result)
{
	unsigned word;
	if (result == NULL)
		return;
	for (word = 0; word < HAL_CPU_MASK_WORDS; word++)
		result->bits[word] = __atomic_load_n(&ready_mask.bits[word],
		    __ATOMIC_ACQUIRE);
}

uint32
amd64_smp_apic_id(hal_cpu_id_t cpu)
{
	struct amd64_percpu *state = amd64_percpu_get(cpu);
	return state != NULL ? state->apic_id : UINT32_MAX;
}

static int
start_one(struct amd64_percpu *cpu)
{
	const struct hal_pmem_request stack_request = {
		HAL_PMEM_PADDR_ANY, AMD64_AP_STACK_SIZE, 4096,
		HAL_PMEM_TYPE_RAM, 0
	};
	uint8 *destination = amd64_phys_to_direct(AMD64_AP_TRAMPOLINE);
	size_t image_size = (size_t)(amd64_ap_trampoline_end -
	    amd64_ap_trampoline_start);
	unsigned timeout;

	if (image_size == 0 || image_size > 4096U ||
	    hal_pmem_alloc(&stack_request, &cpu->bootstrap_stack) != HAL_OK)
		return HAL_ERR_NOMEM;
	hal_memcpy(destination, amd64_ap_trampoline_start, image_size);
	*(uint32 *)(destination + (amd64_ap_trampoline_cr3 -
	    amd64_ap_trampoline_start)) = (uint32)amd64_system_cr3();
	*(uint64 *)(destination + (amd64_ap_trampoline_stack -
	    amd64_ap_trampoline_start)) = (uint64)(uintptr_t)
	    cpu->bootstrap_stack.vaddr + cpu->bootstrap_stack.size;
	*(uint64 *)(destination + (amd64_ap_trampoline_cpu -
	    amd64_ap_trampoline_start)) = cpu->logical_id;
	*(uint64 *)(destination + (amd64_ap_trampoline_entry -
	    amd64_ap_trampoline_start)) = (uint64)(uintptr_t)amd64_ap_entry;
	hal_wmb();
	if (amd64_lapic_send_init(cpu->apic_id) != HAL_OK)
		return HAL_ERR_IO;
	short_delay();
	if (amd64_lapic_send_startup(cpu->apic_id,
	    AMD64_AP_TRAMPOLINE >> 12) != HAL_OK)
		return HAL_ERR_IO;
	short_delay();
	(void)amd64_lapic_send_startup(cpu->apic_id,
	    AMD64_AP_TRAMPOLINE >> 12);
	for (timeout = 0; timeout < 10000000U; timeout++) {
		hal_rmb();
		if (__atomic_load_n(&cpu->ready, __ATOMIC_ACQUIRE) != 0)
			return HAL_OK;
		__asm__ volatile("pause");
	}
	return HAL_ERR_TIMEOUT;
}

int
hal_cpu_start_others(void)
{
	hal_cpu_id_t cpu;
	for (cpu = 1; cpu < present_count; cpu++) {
		int error = start_one(amd64_percpu_get(cpu));
		if (error != HAL_OK)
			return error;
	}
	return HAL_OK;
}

void
amd64_ap_entry(uint64 logical_cpu)
{
	struct amd64_percpu *cpu = amd64_percpu_get((hal_cpu_id_t)logical_cpu);
	if (cpu == NULL || cpu->logical_id != logical_cpu)
		HAL_FATAL("invalid amd64 AP logical ID");
	amd64_cpu_init();
	amd64_percpu_select(cpu);
	cpu->current_space = HAL_SPACE_SYS;
	amd64_descriptor_init();
	amd64_int_load();
	amd64_lapic_init_cpu();
	amd64_lapic_timer_start();
	__atomic_store_n(&cpu->ready, 1U, __ATOMIC_RELEASE);
	(void)__atomic_fetch_or(
	    &ready_mask.bits[cpu->logical_id / 64U],
	    (uint64)1U << (cpu->logical_id % 64U), __ATOMIC_RELEASE);
	kernel_secondary_entry(cpu->logical_id);
	HAL_FATAL("kernel_secondary_entry returned");
	for (;;)
		asm_hlt();
}

int
hal_cpu_notify(hal_cpu_id_t cpu)
{
	struct amd64_percpu *target;
	if (cpu >= present_count)
		return HAL_ERR_INVALID;
	target = amd64_percpu_get(cpu);
	if (__atomic_load_n(&target->ready, __ATOMIC_ACQUIRE) == 0)
		return HAL_ERR_STATE;
	return amd64_lapic_notify(target->apic_id);
}

int
hal_cpu_notify_mask(const struct hal_cpu_mask *targets)
{
	hal_cpu_id_t cpu;
	if (targets == NULL)
		return HAL_ERR_INVALID;
	for (cpu = 0; cpu < HAL_CPU_MAX; cpu++)
		if (hal_cpu_mask_test(targets, cpu) &&
		    (cpu >= present_count || __atomic_load_n(
		    &amd64_percpu_get(cpu)->ready, __ATOMIC_ACQUIRE) == 0))
			return HAL_ERR_INVALID;
	for (cpu = 0; cpu < present_count; cpu++)
		if (hal_cpu_mask_test(targets, cpu) &&
		    amd64_lapic_notify(amd64_percpu_get(cpu)->apic_id) != HAL_OK)
			HAL_FATAL("amd64 CPU notify delivery failed");
	return HAL_OK;
}

_Noreturn void
hal_cpu_park(void)
{
	amd64_lapic_timer_stop();
	(void)hal_irq_disable();
	for (;;)
		asm_hlt();
}

_Noreturn void hal_cpu_panic_all(void) { amd64_lapic_panic_all(); }
