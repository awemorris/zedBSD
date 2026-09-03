/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 xAPIC CPU discovery and INIT-SIPI-SIPI startup path.
 */

#include <hal/hal.h>
#include "asm.h"
#include "descriptor.h"
#include "int.h"
#include "percpu.h"
#include "smp.h"
#include "space.h"
#include "task.h"
#include "bsp-pcat/early-init-policy.h"
#include "bsp-pcat/lapic.h"
#include "bsp-pcat/timecounter.h"

extern uint8_t amd64_ap_trampoline_start[];
extern uint8_t amd64_ap_trampoline_end[];
extern uint8_t amd64_ap_trampoline_cr3[];
extern uint8_t amd64_ap_trampoline_stack[];
extern uint8_t amd64_ap_trampoline_cpu[];
extern uint8_t amd64_ap_trampoline_entry[];

static unsigned present_count = 1;
static struct hal_cpu_mask ready_mask;
static volatile unsigned panic_available;

static void short_delay(void);
static int start_one(struct amd64_percpu *cpu, int *timecounter_valid);

/*
 * Builds the logical amd64 CPU topology from ACPI.
 */
void
amd64_smp_init(
	const struct amd64_acpi_info *acpi)
{
	struct amd64_percpu *bsp;
	struct amd64_percpu *cpu;
	uint32_t bsp_apic;
	unsigned source;
	unsigned target;

	/* Reads the active BSP APIC identity before topology validation. */
	bsp_apic = amd64_lapic_id();
	target = 1;

	/* Requires a nonempty firmware CPU topology. */
	if (acpi == NULL || acpi->cpu_count == 0)
		HAL_FATAL("invalid amd64 CPU topology");

	/* Initializes the BSP's established per-CPU record. */
	bsp = amd64_percpu_get(0);
	if (bsp != amd64_percpu_current())
		HAL_FATAL("amd64 BSP per-CPU state mismatch");
	bsp->logical_id = 0;
	bsp->apic_id = bsp_apic;
	bsp->ready = 1;
	amd64_percpu_select(bsp);

	/* Publishes only the BSP in the initial ready mask. */
	hal_cpu_mask_zero(&ready_mask);
	hal_cpu_mask_set(&ready_mask, 0);

	/* Builds dense logical records for every non-BSP firmware CPU. */
	for (source = 0;
	     source < acpi->cpu_count && target < AMD64_SMP_MAX_CPUS;
	     source++) {
		/* Omits the BSP's firmware topology entry. */
		if (acpi->cpus[source].apic_id == bsp_apic)
			continue;

		/* Initializes the next dense logical record from firmware topology. */
		cpu = amd64_percpu_get(target);
		hal_memset(cpu, 0, sizeof(*cpu));
		cpu->logical_id = target;
		cpu->apic_id = acpi->cpus[source].apic_id;
		target++;
	}

	/* Publishes the discovered logical CPU count. */
	present_count = target;

	/*
	 * NMI panic broadcast is unsafe until every AP passes its local APIC
	 * preflight and publishes readiness.
	 */
	__atomic_store_n(&panic_available, 0U, __ATOMIC_RELEASE);
}

/*
 * Reports whether panic broadcast is safe for all CPUs.
 */
int
amd64_smp_panic_available(
	void)
{
	int available;

	/* Samples the readiness publication with acquire ordering. */
	available = __atomic_load_n(
		&panic_available,
		__ATOMIC_ACQUIRE) != 0;

	/* Returns the sampled broadcast state. */
	return available;
}

/*
 * Reports the current logical CPU identifier.
 */
hal_cpu_id_t
hal_cpu_current(
	void)
{
	struct amd64_percpu *cpu;

	/* Resolves the current GS-selected CPU record. */
	cpu = amd64_percpu_current();

	/* Returns its dense logical identifier. */
	return cpu->logical_id;
}

/*
 * Reports the number of discovered logical CPUs.
 */
unsigned
hal_cpu_count(
	void)
{
	/* Returns the topology count established during SMP initialization. */
	return present_count;
}

/*
 * Copies the current CPU-ready mask.
 */
void
hal_cpu_ready_mask(
	struct hal_cpu_mask *result)
{
	unsigned word;

	/* Ignores an absent destination mask. */
	if (result == NULL)
		return;

	/* Acquires each independently published ready-mask word. */
	for (word = 0; word < HAL_CPU_MASK_WORDS; word++) {
		result->bits[word] = __atomic_load_n(
			&ready_mask.bits[word],
			__ATOMIC_ACQUIRE);
	}
}

/*
 * Resolves a logical CPU to its APIC identifier.
 */
uint32_t
amd64_smp_apic_id(
	hal_cpu_id_t logical_cpu)
{
	struct amd64_percpu *cpu;

	/* Resolves the dense per-CPU record. */
	cpu = amd64_percpu_get(logical_cpu);

	/* Rejects a logical CPU outside the architecture limit. */
	if (cpu == NULL)
		return UINT32_MAX;

	/* Returns the firmware APIC identifier. */
	return cpu->apic_id;
}

/*
 * Starts every discovered secondary processor.
 */
int
hal_cpu_start_others(
	void)
{
	hal_cpu_id_t logical_cpu;
	int complete_counter_set;
	int timecounter_valid;
	int error;

	/* Starts with the BSP timecounter candidate state. */
	complete_counter_set = amd64_timecounter_bsp_candidate_valid();

	/* Starts and validates each secondary CPU in logical order. */
	for (logical_cpu = 1; logical_cpu < present_count; logical_cpu++) {
		error = start_one(
			amd64_percpu_get(logical_cpu),
			&timecounter_valid);

		/* Releases waiting APs when any startup operation fails. */
		if (error != HAL_OK) {
			amd64_timecounter_release_boot_validation();
			return error;
		}

		/* Records an incomplete cross-CPU timecounter set. */
		if (!timecounter_valid)
			complete_counter_set = 0;
	}

	/* Completes timecounter validation for the admitted CPU set. */
	amd64_timecounter_complete_boot_validation(
		complete_counter_set != 0,
		present_count);

	/* Enables NMI panic broadcast only after every AP is ready. */
	__atomic_store_n(&panic_available, 1U, __ATOMIC_RELEASE);

	/* Reports successful startup of every secondary CPU. */
	return HAL_OK;
}

/*
 * Initializes one secondary processor after trampoline entry.
 */
void
amd64_ap_entry(
	uint64_t logical_cpu)
{
	struct amd64_percpu *cpu;
	unsigned startup_error;
	int error;

	/* Resolves and validates the trampoline-provided CPU identity. */
	cpu = amd64_percpu_get((hal_cpu_id_t)logical_cpu);
	if (cpu == NULL || cpu->logical_id != logical_cpu)
		HAL_FATAL("invalid amd64 AP logical ID");

	/* Establishes CPU-local architectural and descriptor state. */
	amd64_cpu_init();
	amd64_percpu_select(cpu);
	cpu->current_space = HAL_SPACE_SYS;
	amd64_descriptor_init();
	amd64_int_load();

	/* Validates and enables this secondary CPU's local APIC. */
	error = amd64_lapic_init_secondary(cpu->apic_id, &startup_error);
	if (error != HAL_OK) {
		hal_printf(
			"A64 APIC AP HALT cpu=%u expected-id=%u\n",
			cpu->logical_id,
			cpu->apic_id);

		/* Ensures the BSP receives a concrete APIC policy failure. */
		if (startup_error == AMD64_APIC_POLICY_OK)
			startup_error = AMD64_APIC_POLICY_INVALID_MODE;
		__atomic_store_n(
			&cpu->startup_error,
			startup_error,
			__ATOMIC_RELEASE);
		(void)hal_irq_disable();

		/* Halts the rejected secondary CPU permanently. */
		for (;;)
			asm_hlt();
	}

	/* Completes the boot-time counter probe and task setup. */
	amd64_timecounter_ap_probe(cpu);
	amd64_task_init_cpu(0);

	/* Starts this CPU's local scheduler tick. */
	error = amd64_lapic_timer_start();
	if (error != HAL_OK) {
		hal_printf("A64 TIMER AP HALT cpu=%u\n", cpu->logical_id);
		__atomic_store_n(
			&cpu->startup_error,
			AMD64_STARTUP_ERROR_TIMER,
			__ATOMIC_RELEASE);
		(void)hal_irq_disable();

		/* Halts the timerless secondary CPU permanently. */
		for (;;)
			asm_hlt();
	}

	/* Publishes this CPU first through its record and then the ready mask. */
	__atomic_store_n(&cpu->ready, 1U, __ATOMIC_RELEASE);
	(void)__atomic_fetch_or(
		&ready_mask.bits[cpu->logical_id / 64U],
		(uint64_t)1U << (cpu->logical_id % 64U),
		__ATOMIC_RELEASE);

	/* Validates runtime counter ordering before entering the kernel. */
	amd64_timecounter_ap_runtime_validate(cpu);
	kernel_secondary_entry(cpu->logical_id);
	HAL_FATAL("kernel_secondary_entry returned");

	/* Retains a physical halt fallback for a returning fatal handler. */
	for (;;)
		asm_hlt();
}

/*
 * Sends a notification interrupt to one logical CPU.
 */
int
hal_cpu_notify(
	hal_cpu_id_t logical_cpu)
{
	struct amd64_percpu *target;
	int error;

	/* Rejects logical CPUs absent from the discovered topology. */
	if (logical_cpu >= present_count)
		return HAL_ERR_INVALID;

	/* Requires the target CPU to have published readiness. */
	target = amd64_percpu_get(logical_cpu);
	if (__atomic_load_n(&target->ready, __ATOMIC_ACQUIRE) == 0)
		return HAL_ERR_STATE;

	/* Sends the notification through the target local APIC. */
	error = amd64_lapic_notify(target->apic_id);

	/* Returns the APIC delivery result unchanged. */
	return error;
}

/*
 * Sends notification interrupts to a logical CPU mask.
 */
int
hal_cpu_notify_mask(
	const struct hal_cpu_mask *targets)
{
	struct amd64_percpu *target;
	hal_cpu_id_t logical_cpu;
	int error;

	/* Rejects an absent target mask. */
	if (targets == NULL)
		return HAL_ERR_INVALID;

	/* Validates every requested CPU before sending any interrupt. */
	for (logical_cpu = 0; logical_cpu < HAL_CPU_MAX; logical_cpu++) {
		/* Skips CPUs absent from the requested mask. */
		if (!hal_cpu_mask_test(targets, logical_cpu))
			continue;

		/* Rejects targets absent from the discovered topology. */
		if (logical_cpu >= present_count)
			return HAL_ERR_INVALID;

		/* Resolves the requested logical CPU's readiness record. */
		target = amd64_percpu_get(logical_cpu);

		/* Rejects targets that have not published readiness. */
		if (__atomic_load_n(&target->ready, __ATOMIC_ACQUIRE) == 0)
			return HAL_ERR_INVALID;
	}

	/* Sends notifications to each validated requested CPU. */
	for (logical_cpu = 0; logical_cpu < present_count; logical_cpu++) {
		/* Skips discovered CPUs absent from the requested mask. */
		if (!hal_cpu_mask_test(targets, logical_cpu))
			continue;

		/* Resolves and notifies this requested logical CPU. */
		target = amd64_percpu_get(logical_cpu);
		error = amd64_lapic_notify(target->apic_id);

		/* Treats a post-validation delivery failure as fatal. */
		if (error != HAL_OK)
			HAL_FATAL("amd64 CPU notify delivery failed");
	}

	/* Reports delivery to the complete requested mask. */
	return HAL_OK;
}

/*
 * Parks the current CPU permanently.
 */
void __attribute__((noreturn))
hal_cpu_park(
	void)
{
	/* Stops the local tick and disables local interrupts. */
	amd64_lapic_timer_stop();
	(void)hal_irq_disable();

	/* Halts the parked CPU permanently. */
	for (;;)
		asm_hlt();
}

/*
 * Broadcasts the panic NMI to every other CPU.
 */
void __attribute__((noreturn))
hal_cpu_panic_all(
	void)
{
	/* Delivers the architecture-wide panic broadcast. */
	amd64_lapic_panic_all();
}

/* Delays between architectural AP startup messages. */
static void
short_delay(
	void)
{
	volatile unsigned count;

	/* Executes a bounded processor-friendly delay loop. */
	for (count = 0; count < 100000U; count++)
		__asm__ volatile("pause");
}

/* Starts and admits one secondary processor. */
static int
start_one(
	struct amd64_percpu *cpu,
	int *timecounter_valid)
{
	const struct hal_pmem_request stack_request = {
		HAL_PMEM_PADDR_ANY,
		AMD64_AP_STACK_SIZE,
		4096,
		HAL_PMEM_TYPE_RAM,
		0
	};
	uint8_t *destination;
	const char *reason;
	size_t image_size;
	unsigned timeout;
	unsigned startup_error;
	int error;

	/* Resolves and validates the low-memory trampoline image. */
	destination = amd64_phys_to_direct(AMD64_AP_TRAMPOLINE);
	image_size = (size_t)(amd64_ap_trampoline_end -
	    amd64_ap_trampoline_start);
	if (timecounter_valid == NULL || image_size == 0 || image_size > 4096U)
		return HAL_ERR_NOMEM;

	/* Allocates the secondary CPU's bootstrap stack. */
	error = hal_pmem_alloc(&stack_request, &cpu->bootstrap_stack);
	if (error != HAL_OK)
		return HAL_ERR_NOMEM;

	/* Resets the startup and cross-CPU timecounter handshake state. */
	*timecounter_valid = 0;
	__atomic_store_n(&cpu->startup_error, 0U, __ATOMIC_RELEASE);
	__atomic_store_n(
		&cpu->timecounter_probe_ready,
		0U,
		__ATOMIC_RELAXED);
	__atomic_store_n(
		&cpu->timecounter_probe_request,
		0U,
		__ATOMIC_RELAXED);
	__atomic_store_n(
		&cpu->timecounter_probe_ack,
		0U,
		__ATOMIC_RELAXED);
	__atomic_store_n(
		&cpu->timecounter_probe_release,
		0U,
		__ATOMIC_RELAXED);
	__atomic_store_n(
		&cpu->timecounter_runtime_ready,
		0U,
		__ATOMIC_RELAXED);
	__atomic_store_n(
		&cpu->timecounter_runtime_done,
		0U,
		__ATOMIC_RELAXED);
	cpu->timecounter_probe_sample = 0U;
	cpu->timecounter_probe_valid = 0;
	cpu->timecounter_runtime_status = 0U;
	cpu->timecounter_runtime_reads = 0U;

	/* Copies and patches the shared trampoline for this CPU. */
	hal_memcpy(destination, amd64_ap_trampoline_start, image_size);
	*(uint32_t *)(destination + (amd64_ap_trampoline_cr3 -
	    amd64_ap_trampoline_start)) = (uint32_t)amd64_system_cr3();
	*(uint64_t *)(destination + (amd64_ap_trampoline_stack -
	    amd64_ap_trampoline_start)) =
	    (uint64_t)(uintptr_t)cpu->bootstrap_stack.vaddr +
	    cpu->bootstrap_stack.size;
	*(uint64_t *)(destination + (amd64_ap_trampoline_cpu -
	    amd64_ap_trampoline_start)) = cpu->logical_id;
	*(uint64_t *)(destination + (amd64_ap_trampoline_entry -
	    amd64_ap_trampoline_start)) =
	    (uint64_t)(uintptr_t)amd64_ap_entry;
	hal_wmb();

	/* Sends the INIT message before either startup message. */
	error = amd64_lapic_send_init(cpu->apic_id);
	if (error != HAL_OK) {
		__atomic_store_n(
			&cpu->timecounter_probe_release,
			1U,
			__ATOMIC_RELEASE);
		return HAL_ERR_IO;
	}
	short_delay();

	/* Sends the first SIPI after the architectural delay. */
	error = amd64_lapic_send_startup(
		cpu->apic_id,
		AMD64_AP_TRAMPOLINE >> 12);
	if (error != HAL_OK) {
		__atomic_store_n(
			&cpu->timecounter_probe_release,
			1U,
			__ATOMIC_RELEASE);
		return HAL_ERR_IO;
	}
	short_delay();

	/* Sends the architecturally conventional second SIPI. */
	(void)amd64_lapic_send_startup(
		cpu->apic_id,
		AMD64_AP_TRAMPOLINE >> 12);

	/* Performs the boot-time TSC bracket handshake with this AP. */
	*timecounter_valid = amd64_timecounter_bsp_validate_ap(cpu);

	/* Waits for readiness or a published startup failure. */
	for (timeout = 0; timeout < 10000000U; timeout++) {
		hal_rmb();

		/* Admits a secondary CPU once it publishes readiness. */
		if (__atomic_load_n(&cpu->ready, __ATOMIC_ACQUIRE) != 0)
			return HAL_OK;

		/* Observes any startup failure published by the secondary CPU. */
		startup_error = __atomic_load_n(
			&cpu->startup_error,
			__ATOMIC_ACQUIRE);

		/* Reports a concrete startup policy failure. */
		if (startup_error != 0) {
			/* Selects a stable diagnostic for the published failure. */
			if (startup_error == AMD64_STARTUP_ERROR_TIMER) {
				reason = "timer-start-failed";
			} else {
				reason = amd64_apic_policy_result_name(
					(enum amd64_apic_policy_result)startup_error);
			}
			hal_printf(
				"A64 AP START FAIL cpu=%u apic-id=%u result=%s\n",
				cpu->logical_id,
				cpu->apic_id,
				reason);
			return HAL_ERR_UNSUPPORTED;
		}

		/* Reduces contention while waiting for the next publication. */
		__asm__ volatile("pause");
	}

	/* Reports expiration of the bounded startup wait. */
	return HAL_ERR_TIMEOUT;
}
