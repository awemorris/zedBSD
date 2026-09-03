/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i386 xAPIC CPU discovery and INIT-SIPI-SIPI implementation.
 */

#include <hal/hal.h>

#include "apic-topology.h"
#include "asm.h"
#include "int.h"
#include "irq.h"
#include "lapic.h"
#include "percpu.h"
#include "smp.h"
#include "space.h"
#include "task.h"

#define TRAMPOLINE_PHYS 0x6000U
#define AP_STACK_SIZE 16384U
#define VECTOR_NOTIFY 0xd0U
#define VECTOR_PANIC 0xd1U
#define VECTOR_TLB 0xd2U

struct cpu_state {
	uint8_t apic_id;
	volatile unsigned ready;
	void *stack;
};

extern uint8_t i386_ap_trampoline_start[];
extern uint8_t i386_ap_trampoline_end[];
extern uint8_t i386_ap_trampoline_cr3[];
extern uint8_t i386_ap_trampoline_stack[];
extern uint8_t i386_ap_trampoline_cpu[];
extern uint8_t i386_ap_trampoline_target[];

static struct cpu_state cpus[I386_APIC_MAX_CPUS];
static unsigned cpu_count = 1;
static volatile unsigned configured;
static struct hal_cpu_mask ready_mask;

extern void i386_ap_high_entry(void);

static size_t trampoline_offset(uint8_t *symbol);
static void startup_delay(void);
static int start_one(hal_cpu_id_t cpu);

/*
 * Configures the logical i386 CPU inventory from firmware topology.
 */
void
i386_smp_configure(
	const struct i386_apic_topology *topology)
{
	uint8_t bootstrap_apic_id;
	unsigned i;
	unsigned target;

	/* Captures the bootstrap APIC ID and clears all prior CPU state. */
	bootstrap_apic_id = i386_lapic_id();
	hal_memset(cpus, 0, sizeof(cpus));
	cpus[0].apic_id = bootstrap_apic_id;

	/* Appends each non-bootstrap firmware CPU within the fixed limit. */
	target = 1;
	for (i = 0;
	     i < topology->cpu_count && target < I386_APIC_MAX_CPUS;
	     i++) {
		/* Skips the bootstrap APIC entry already assigned to logical zero. */
		if (topology->cpus[i].apic_id != bootstrap_apic_id) {
			cpus[target].apic_id = topology->cpus[i].apic_id;
			target++;
		}
	}

	/* Publishes the CPU count and bootstrap-ready mask before AP startup. */
	cpu_count = target;
	hal_cpu_mask_zero(&ready_mask);
	hal_cpu_mask_set(&ready_mask, 0);
	configured = 1;
}

/*
 * Resolves the current logical i386 CPU identifier.
 */
hal_cpu_id_t
hal_cpu_current(
	void)
{
	uint8_t apic_id;
	unsigned i;

	/* Uses the bootstrap logical ID before APIC topology is published. */
	if (!configured)
		return 0;

	/* Matches the current hardware APIC ID against the logical inventory. */
	apic_id = i386_lapic_id();
	for (i = 0; i < cpu_count; i++) {
		/* Returns the logical ID for the matching hardware APIC. */
		if (cpus[i].apic_id == apic_id)
			return i;
	}

	/* Rejects a hardware CPU absent from the configured inventory. */
	HAL_FATAL("unknown i386 APIC ID");

	/* Satisfies the compiler after the non-returning fatal path. */
	return 0;
}

/*
 * Reports the configured i386 CPU count.
 */
unsigned
hal_cpu_count(
	void)
{
	/* Returns the published logical CPU count. */
	return cpu_count;
}

/*
 * Copies the mask of i386 CPUs ready for kernel work.
 */
void
hal_cpu_ready_mask(
	struct hal_cpu_mask *result)
{
	unsigned word;

	/* Ignores requests without result storage. */
	if (result == NULL)
		return;

	/* Orders the ready publication before copying every mask word. */
	hal_rmb();
	for (word = 0; word < HAL_CPU_MASK_WORDS; word++)
		result->bits[word] = ready_mask.bits[word];
}

/*
 * Returns one secondary CPU's bootstrap stack.
 */
void *
i386_smp_bootstrap_stack(
	hal_cpu_id_t cpu)
{
	/* Rejects logical IDs outside the configured inventory. */
	if (cpu >= cpu_count)
		return NULL;

	/* Returns the retained bootstrap stack allocation. */
	return cpus[cpu].stack;
}

/*
 * Resolves one logical CPU to its local-APIC identifier.
 */
int
i386_smp_apic_id(
	hal_cpu_id_t cpu,
	uint8_t *id)
{
	/* Requires a configured logical ID and writable result. */
	if (cpu >= cpu_count || id == NULL)
		return HAL_ERR_INVALID;

	/* Copies the firmware local-APIC identifier. */
	*id = cpus[cpu].apic_id;

	/* Reports a resolved identifier. */
	return HAL_OK;
}

/*
 * Starts every configured secondary i386 CPU.
 */
int
hal_cpu_start_others(
	void)
{
	hal_cpu_id_t cpu;
	int result;

	/* Starts secondary CPUs in logical-ID order. */
	for (cpu = 1; cpu < cpu_count; cpu++) {
		result = start_one(cpu);

		/* Propagates the first secondary-startup failure. */
		if (result != HAL_OK)
			return result;
	}

	/* Reports that every secondary CPU reached its trampoline. */
	return HAL_OK;
}

/*
 * Initializes a secondary CPU after its real-mode trampoline.
 */
void
i386_smp_ap_entry(
	uint32_t cpu)
{
	uintptr_t stack_top;

	/* Rejects the bootstrap or an unconfigured logical CPU. */
	if (cpu == 0 || cpu >= cpu_count)
		HAL_FATAL("invalid i386 AP entry");

	/* Builds this CPU's descriptor, interrupt, memory, and task state. */
	stack_top = (uintptr_t)cpus[cpu].stack + AP_STACK_SIZE;
	i386_percpu_init(cpu, stack_top);
	i386_int_load();
	i386_space_init_secondary();
	i386_task_init_secondary(cpu, stack_top);

	/* Starts this CPU's local interrupt controller and periodic timer. */
	i386_lapic_init_cpu();
	i386_lapic_timer_start(i386_interrupt_timer_ticks());

	/* Publishes readiness before transferring control to the kernel. */
	cpus[cpu].ready = 1;
	ready_mask.bits[cpu / 64U] |= (uint64_t)1U << (cpu % 64U);
	hal_wmb();
	kernel_secondary_entry(cpu);

	/* Rejects an unexpected return from the secondary kernel entry. */
	HAL_FATAL("secondary kernel entry returned");
}

/*
 * Sends a scheduler notification to one i386 CPU.
 */
int
hal_cpu_notify(
	hal_cpu_id_t cpu)
{
	int result;

	/* Applies the legacy-controller result convention before CPU checks. */
	if (!i386_interrupt_uses_apic()) {
		/* Distinguishes unsupported bootstrap delivery from an invalid AP. */
		if (cpu == 0)
			return HAL_ERR_UNSUPPORTED;
		return HAL_ERR_INVALID;
	}

	/* Requires a configured and ready destination CPU. */
	if (cpu >= cpu_count)
		return HAL_ERR_INVALID;

	/* Rejects a secondary CPU which has not published readiness. */
	if (cpu != 0 && !cpus[cpu].ready)
		return HAL_ERR_STATE;

	/* Sends the fixed scheduler-notification vector. */
	result = i386_lapic_send_fixed(cpus[cpu].apic_id, VECTOR_NOTIFY);

	/* Returns the local-APIC delivery result. */
	return result;
}

/*
 * Sends a scheduler notification to an i386 CPU mask.
 */
int
hal_cpu_notify_mask(
	const struct hal_cpu_mask *mask)
{
	hal_cpu_id_t cpu;
	int result;

	/* Requires a source mask. */
	if (mask == NULL)
		return HAL_ERR_INVALID;

	/* Rejects requested bits outside the configured CPU inventory. */
	for (cpu = cpu_count; cpu < HAL_CPU_MAX; cpu++) {
		/* Rejects the first requested unconfigured CPU bit. */
		if (hal_cpu_mask_test(mask, cpu))
			return HAL_ERR_INVALID;
	}

	/* Requires APIC delivery for mask notifications. */
	if (!i386_interrupt_uses_apic())
		return HAL_ERR_UNSUPPORTED;

	/* Sends the notification to each requested configured CPU. */
	for (cpu = 0; cpu < cpu_count; cpu++) {
		/* Delivers only to CPUs selected by the source mask. */
		if (hal_cpu_mask_test(mask, cpu)) {
			result = i386_lapic_send_fixed(
				cpus[cpu].apic_id,
				VECTOR_NOTIFY);

			/* Propagates the first local-APIC delivery failure. */
			if (result != HAL_OK)
				return result;
		}
	}

	/* Reports delivery to the complete requested mask. */
	return HAL_OK;
}

/*
 * Sends a TLB-shootdown request to one i386 CPU.
 */
int
i386_smp_send_tlb(
	hal_cpu_id_t cpu)
{
	int result;

	/* Requires a configured and ready destination CPU. */
	if (cpu >= cpu_count)
		return HAL_ERR_INVALID;

	/* Rejects a secondary CPU which has not published readiness. */
	if (cpu != 0 && !cpus[cpu].ready)
		return HAL_ERR_STATE;

	/* Requires APIC delivery for remote TLB invalidation. */
	if (!i386_interrupt_uses_apic())
		return HAL_ERR_UNSUPPORTED;

	/* Sends the fixed TLB-invalidation vector. */
	result = i386_lapic_send_fixed(cpus[cpu].apic_id, VECTOR_TLB);

	/* Returns the local-APIC delivery result. */
	return result;
}

/*
 * Stops the current i386 CPU permanently.
 */
void __attribute__((noreturn))
hal_cpu_park(
	void)
{
	/* Stops the local timer when APIC mode is active. */
	if (i386_interrupt_uses_apic())
		i386_lapic_timer_stop();

	/* Disables interrupts before entering the permanent halt loop. */
	(void)hal_irq_disable();

	/* Halts this CPU permanently. */
	for (;;)
		asm_hlt();
}

/*
 * Sends a panic request and stops the current i386 CPU permanently.
 */
void __attribute__((noreturn))
hal_cpu_panic_all(
	void)
{
	hal_cpu_id_t self;
	hal_cpu_id_t cpu;

	/* Captures the initiating CPU before selecting the delivery path. */
	self = hal_cpu_current();

	/* Selects APIC delivery only when the interrupt topology supports it. */
	if (i386_interrupt_uses_apic()) {
		/* Notifies every other bootstrap or ready CPU. */
		for (cpu = 0; cpu < cpu_count; cpu++) {
			/* Skips the sender and secondaries which are not ready. */
			if (cpu != self && (cpu == 0 || cpus[cpu].ready)) {
				(void)i386_lapic_send_fixed(
					cpus[cpu].apic_id,
					VECTOR_PANIC);
			}
		}
	}

	/* Disables interrupts before entering the permanent halt loop. */
	(void)hal_irq_disable();

	/* Halts this CPU permanently. */
	for (;;)
		asm_hlt();
}

/* Computes one patch offset within the AP trampoline image. */
static size_t
trampoline_offset(
	uint8_t *symbol)
{
	/* Returns the byte offset from the trampoline image start. */
	return (size_t)(symbol - i386_ap_trampoline_start);
}

/* Provides the fixed delay required between startup IPIs. */
static void
startup_delay(
	void)
{
	volatile unsigned count;

	/* Executes a bounded processor-pause delay. */
	for (count = 0; count < 200000U; count++)
		__asm__ volatile("pause");
}

/* Copies the trampoline and starts one secondary CPU. */
static int
start_one(
	hal_cpu_id_t cpu)
{
	uint8_t *destination;
	size_t size;
	unsigned timeout;
	int result;

	/* Locates and measures the low-memory trampoline image. */
	destination = (uint8_t *)(uintptr_t)(TRAMPOLINE_PHYS | 0x80000000U);
	size = (size_t)(i386_ap_trampoline_end - i386_ap_trampoline_start);

	/* Requires a nonempty trampoline which fits in its reserved page. */
	if (size == 0 || size > 4096U)
		return HAL_ERR_STATE;

	/* Allocates the secondary CPU's retained bootstrap stack. */
	cpus[cpu].stack = hal_malloc(AP_STACK_SIZE);
	if (cpus[cpu].stack == NULL)
		return HAL_ERR_NOMEM;

	/* Copies and patches the trampoline before making it observable. */
	hal_memcpy(destination, i386_ap_trampoline_start, size);
	*(uint32_t *)(destination +
	    trampoline_offset(i386_ap_trampoline_cr3)) = asm_get_cr3();
	*(uint32_t *)(destination +
	    trampoline_offset(i386_ap_trampoline_stack)) =
	    (uint32_t)(uintptr_t)cpus[cpu].stack + AP_STACK_SIZE;
	*(uint32_t *)(destination +
	    trampoline_offset(i386_ap_trampoline_cpu)) = cpu;
	*(uint32_t *)(destination +
	    trampoline_offset(i386_ap_trampoline_target)) =
	    (uint32_t)(uintptr_t)i386_ap_high_entry;
	hal_wmb();

	/* Sends INIT, the first SIPI, and the optional second SIPI in order. */
	result = i386_lapic_send_init(cpus[cpu].apic_id);

	/* Reports an INIT delivery failure through the public I/O result. */
	if (result != HAL_OK)
		return HAL_ERR_IO;
	startup_delay();
	result = i386_lapic_send_startup(
		cpus[cpu].apic_id,
		TRAMPOLINE_PHYS >> 12);

	/* Reports a first-SIPI failure before issuing the optional retry. */
	if (result != HAL_OK)
		return HAL_ERR_IO;
	startup_delay();
	(void)i386_lapic_send_startup(
		cpus[cpu].apic_id,
		TRAMPOLINE_PHYS >> 12);

	/* Waits for the secondary CPU to publish readiness. */
	for (timeout = 0; timeout < 10000000U; timeout++) {
		/* Reports startup when the AP's acquire load observes readiness. */
		if (__atomic_load_n(&cpus[cpu].ready, __ATOMIC_ACQUIRE))
			return HAL_OK;
		__asm__ volatile("pause");
	}

	/* Reports a secondary startup timeout. */
	return HAL_ERR_TIMEOUT;
}
