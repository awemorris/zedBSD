/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arm64 CPU lifecycle: which CPUs exist, how the secondary cores are
 * released from the firmware, and how CPUs reach each other.
 *
 * The device tree lists the cores and how the firmware holds each one: a
 * spin table on the Pi 4, PSCI on the Pi 5.  The board releases a core to
 * the physical entry of arm64_secondary_entry (locore.S), and the core
 * comes up through its own boot block, which it finds by its core number
 * (the MPIDR affinity field the board numbers its cores by).  The kernel is
 * told the number of CPUs before any is released; one that does not come
 * up stops the boot, with the advice to start with cpus=1 (ws208, D6).
 */

#include <hal/hal.h>
#include "asm.h"
#include "bsp.h"
#include "defs.h"
#include "int.h"
#include "percpu.h"
#include "space.h"
#include "task.h"
#include "watch.h"
#include "clock.h"
#include "gic.h"

/* The bytes of a secondary CPU's boot stack, which becomes its initial task's. */
#define ARM64_SECONDARY_STACK_SIZE 16384U

/* How long the boot CPU waits for a released core, in milliseconds. */
#define ARM64_SECONDARY_WAIT_MS 1000U

/*
 * What a secondary core reads, with its MMU off, to come up.
 *
 * locore.S finds the block by the core's number and reads it at
 * fixed offsets: the kernel's TTBR1 (0 means "not released"), the stack
 * top in the kernel half, and the CPU's logical number.
 */
struct arm64_secondary_boot {
	uint64_t ttbr1;
	uint64_t stack_top;
	uint64_t cpu;
	uint64_t reserved;
};

_Static_assert(sizeof(struct arm64_secondary_boot) == 32U,
	       "locore.S indexes the boot blocks by 32 bytes");

void arm64_secondary_entry(void);
void arm64_secondary_main(uint64_t cpu);

/*
 * The boot blocks, one per core number (0-3).
 *
 * The boot CPU fills a block and cleans it to memory before it releases
 * the core; the core reads it with its caches off.  Not changed afterwards.
 */
struct arm64_secondary_boot arm64_secondary_boot[ARM64_CPU_MAX] __attribute__((aligned(64)));

/*
 * How many CPUs the kernel is told about.
 *
 * Fixed by arm64_smp_configure() before the kernel asks for it, and never
 * changed: the kernel sizes its per-CPU tables by it before
 * hal_cpu_start_others() runs.
 */
static unsigned cpu_count = 1;

/*
 * Each logical CPU's core number, which also numbers its GIC CPU interface
 * and its boot block, and the device tree's entry for it, which says how
 * the board releases it; logical 0 is the boot CPU, whose entry is not
 * used.  The boot CPU's number is set by arm64_smp_bootstrap() and the
 * others' by arm64_smp_configure(); not changed afterwards.
 */
static uint64_t cpu_core[ARM64_CPU_MAX];
static struct arm64_fdt_cpu cpu_entry[ARM64_CPU_MAX];

/*
 * The CPUs that are up, one bit per CPU.
 *
 * A CPU sets its own bit with a release once it runs its initial task and
 * its tick; the boot CPU's bit is set by the boot.
 */
static struct hal_cpu_mask ready_mask;

static unsigned boot_parameter_cpus(void);
static uint32_t cpu_interface_bit(hal_cpu_id_t cpu);
static int start_one(hal_cpu_id_t cpu);
static int cpu_is_ready(hal_cpu_id_t cpu);

/*
 * Makes the boot CPU known: its record, and its place in the ready mask.
 */
void
arm64_smp_bootstrap(
	void)
{
	uint64_t mpidr;

	/* Reads the boot CPU's affinity, which names it in the interrupt controller. */
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));

	/* Makes the record the boot CPU's own before any lock is taken. */
	arm64_percpu_bootstrap(0, mpidr);
	cpu_core[0] = arm64_bsp_core_of(mpidr);

	/* The boot CPU is up from here on. */
	hal_cpu_mask_zero(&ready_mask);
	hal_cpu_mask_set(&ready_mask, 0);
	__atomic_store_n(&arm64_percpu_get(0)->ready, 1U, __ATOMIC_RELEASE);
}

/*
 * Decides how many CPUs the kernel gets, from the device tree and cpus=N.
 *
 * The boot CPU is logical 0; the other cores the board can release follow
 * in the tree's order, up to four and up to the boot parameter.
 */
void
arm64_smp_configure(
	void)
{
	const struct arm64_fdt_info *info;
	const struct arm64_fdt_cpu *entry;
	unsigned limit;
	unsigned index;
	uint64_t core;
	int startable;

	/* The boot parameter caps the count; absent, every core is used. */
	limit = boot_parameter_cpus();
	if (limit == 0 || limit > ARM64_CPU_MAX)
		limit = ARM64_CPU_MAX;

	/* Takes each other core the board can release. */
	info = arm64_boot_info();
	for (index = 0; index < info->cpu_count; index++) {
		entry = &info->cpus[index];
		core = arm64_bsp_core_of(entry->mpidr);

		/* The boot CPU is already logical 0. */
		if (core == cpu_core[0])
			continue;

		/* A core this HAL cannot index is left alone. */
		if (core >= ARM64_CPU_MAX)
			continue;

		/* A core the firmware holds in a way the board cannot release is left alone. */
		startable = arm64_bsp_cpu_startable(entry);
		if (!startable)
			continue;

		/* Stops at the limit. */
		if (cpu_count >= limit)
			break;

		/* Gives the core the next logical number. */
		cpu_core[cpu_count] = core;
		cpu_entry[cpu_count] = *entry;
		cpu_count++;
	}

	hal_printf("ARM64 SMP %u CPUs (device tree %u, limit %u)\n",
		   cpu_count, info->cpu_count, limit);
}

/*
 * Reports the CPU the caller runs on.
 */
hal_cpu_id_t
hal_cpu_current(
	void)
{
	struct arm64_percpu *record;
	int masked;

	/*
	 * The record TPIDR_EL1 names carries the CPU's number.  A caller with
	 * interrupts on may be moved to another CPU right after the read; it
	 * gets the CPU it was on, which is all such a caller can be told.
	 */
	masked = arm64_irq_masked();
	record = arm64_percpu_current();

	/*
	 * Every kernel spin lock asks for its CPU on each try, so a CPU that
	 * spins here with its interrupts off looks at its own tick now and
	 * then, and reports itself when the tick stopped.  Only a caller with
	 * interrupts off counts: it stays on the CPU whose record it read, so
	 * the countdown is written by its own CPU alone (a caller moved
	 * between the read and the write would count down another CPU's),
	 * and a CPU with interrupts on takes its tick and needs no look.
	 */
	if (masked) {
		record->watch_countdown--;
		if (record->watch_countdown == 0) {
			record->watch_countdown = ARM64_WATCH_SELF_INTERVAL;
			arm64_watch_self_check();
		}
	}

	/* Succeeded: the caller's CPU. */
	return record->logical_id;
}

/*
 * Reports how many CPUs the kernel schedules on.
 */
unsigned
hal_cpu_count(
	void)
{
	/* Succeeded: the count fixed at boot. */
	return cpu_count;
}

/*
 * Reports the CPUs that are up.
 */
void
hal_cpu_ready_mask(
	struct hal_cpu_mask *result)
{
	unsigned word;

	/* A caller with nowhere to put the answer asked for nothing. */
	if (result == NULL)
		return;

	/* Copies each word with an acquire, pairing with the CPUs' own release. */
	for (word = 0; word < HAL_CPU_MASK_WORDS; word++)
		result->bits[word] = __atomic_load_n(&ready_mask.bits[word], __ATOMIC_ACQUIRE);
}

/*
 * Starts the CPUs other than the boot CPU, one at a time.
 */
int
hal_cpu_start_others(
	void)
{
	hal_cpu_id_t cpu;
	int error;

	/* Releases each core and waits until it is up. */
	for (cpu = 1; cpu < cpu_count; cpu++) {
		error = start_one(cpu);
		if (error != HAL_OK)
			return error;
	}

	/* Succeeded: every CPU the kernel was told about is up. */
	return HAL_OK;
}

/*
 * Enters the HAL on a secondary CPU, from locore.S, on its boot stack.
 *
 * The CPU arrives at EL1 with its MMU on through the boot identity map;
 * it takes its record, its tables, vectors, interrupt controller, debug
 * state, initial task and tick, then joins the kernel.
 */
void
arm64_secondary_main(
	uint64_t cpu)
{
	struct arm64_percpu *record;
	hal_task_t task;
	uint64_t mpidr;

	/* Takes the CPU's record before anything that may lock. */
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	arm64_percpu_bootstrap((hal_cpu_id_t)cpu, mpidr);
	record = arm64_percpu_current();

	/* Leaves the boot identity map for the kernel's own user table. */
	arm64_space_select_system();

	/* Sets up the CPU's exceptions, its part of the GIC and its debug state. */
	arm64_int_init_secondary();
	arm64_debug_init();

	/* Wraps the boot stack's context as the CPU's initial task. */
	task = hal_task_create_for_init_context();
	if (task == NULL)
		HAL_FATAL("arm64 secondary initial task allocation failed");

	/* Starts the CPU's scheduling tick. */
	arm64_timer_init();

	/* Publishes the CPU first through its record, then the ready mask. */
	__atomic_store_n(&record->ready, 1U, __ATOMIC_RELEASE);
	(void)__atomic_fetch_or(&ready_mask.bits[cpu / 64U],
				(uint64_t)1U << (cpu % 64U),
				__ATOMIC_RELEASE);

	/* Joins the kernel's scheduler; this never returns. */
	kernel_secondary_entry((hal_cpu_id_t)cpu);
	HAL_FATAL("kernel_secondary_entry returned");
}

/*
 * Interrupts another CPU so that it looks for work.
 */
int
hal_cpu_notify(
	hal_cpu_id_t cpu)
{
	int ready;

	/* A CPU the kernel was not told about does not exist. */
	if (cpu >= cpu_count)
		return HAL_ERR_INVALID;

	/* A CPU that is not up cannot take the interrupt. */
	ready = cpu_is_ready(cpu);
	if (!ready)
		return HAL_ERR_STATE;

	/* Sends the notification SGI to the CPU's interface. */
	arm64_gic_send_sgi(ARM64_SGI_NOTIFY, cpu_interface_bit(cpu));

	/* Succeeded: the CPU runs kernel_cpu_notify_handler(). */
	return HAL_OK;
}

/*
 * Interrupts every CPU of a mask, as hal_cpu_notify() does one.
 */
int
hal_cpu_notify_mask(
	const struct hal_cpu_mask *targets)
{
	hal_cpu_id_t cpu;
	uint32_t interfaces;
	int named;
	int ready;

	/* A missing mask names no CPU. */
	if (targets == NULL)
		return HAL_ERR_INVALID;

	/* Refuses a mask that names a CPU the kernel was not told about. */
	for (cpu = cpu_count; cpu < HAL_CPU_MAX; cpu++) {
		named = hal_cpu_mask_test(targets, cpu);
		if (named)
			return HAL_ERR_INVALID;
	}

	/* Collects the interfaces of the named CPUs, which must all be up. */
	interfaces = 0;
	for (cpu = 0; cpu < cpu_count; cpu++) {
		named = hal_cpu_mask_test(targets, cpu);
		if (!named)
			continue;
		ready = cpu_is_ready(cpu);
		if (!ready)
			return HAL_ERR_STATE;
		interfaces |= cpu_interface_bit(cpu);
	}

	/* An empty mask is not a request. */
	if (interfaces == 0)
		return HAL_ERR_INVALID;

	/* One SGI request reaches every named interface. */
	arm64_gic_send_sgi(ARM64_SGI_NOTIFY, interfaces);

	/* Succeeded: every named CPU runs kernel_cpu_notify_handler(). */
	return HAL_OK;
}

/*
 * Stops the calling CPU for good.
 */
_Noreturn void
hal_cpu_park(
	void)
{
	/* Waits with interrupts off, so nothing wakes the CPU into the kernel again. */
	(void)hal_irq_disable();

	/* The stall watch is told that this CPU's silence is meant. */
	arm64_watch_park();

	/* Halts for ever. */
	for (;;)
		hal_halt();
}

/*
 * Stops every CPU after a panic.
 *
 * The other CPUs get the stop SGI; one that runs with interrupts off does
 * not take it and runs on until it does.  GIC-400 offers this HAL no
 * non-maskable way to stop a CPU.
 */
_Noreturn void
hal_cpu_panic_all(
	void)
{
	hal_cpu_id_t self;
	hal_cpu_id_t cpu;
	uint32_t interfaces;
	int ready;

	/*
	 * Turns interrupts off for good first, so the CPU named as this one
	 * is the one that parks below: a caller moved to another CPU after
	 * naming itself would leave its old CPU running.
	 */
	(void)hal_irq_disable();

	/* Collects every other CPU that is up. */
	self = hal_cpu_current();
	interfaces = 0;
	for (cpu = 0; cpu < cpu_count; cpu++) {
		/* The panicking CPU parks itself below. */
		if (cpu == self)
			continue;

		/* A CPU that never came up has nothing to stop. */
		ready = cpu_is_ready(cpu);
		if (ready)
			interfaces |= cpu_interface_bit(cpu);
	}

	/* Tells them to stop, then stops this one. */
	if (interfaces != 0)
		arm64_gic_send_sgi(ARM64_SGI_STOP, interfaces);
	hal_cpu_park();
}

/*
 * Reads cpus=N from the firmware's command line; 0 when it is absent.
 */
static unsigned
boot_parameter_cpus(
	void)
{
	static const char key[] = "cpus=";
	const struct arm64_fdt_info *info;
	const char *line;
	unsigned length;
	unsigned position;
	unsigned matched;
	unsigned value;
	int at_word;

	/* No command line, no parameter. */
	info = arm64_boot_info();
	if (info->bootargs_offset == 0 || info->bootargs_length == 0)
		return 0;
	line = (const char *)arm64_phys_to_direct(arm64_boot_fdt_phys() + info->bootargs_offset);
	length = info->bootargs_length;

	/* Looks for the key at the start of a word. */
	at_word = 1;
	for (position = 0; position < length && line[position] != '\0'; position++) {
		/* A space starts the next word. */
		if (line[position] == ' ') {
			at_word = 1;
			continue;
		}

		/* Inside a word the key cannot start. */
		if (!at_word)
			continue;
		at_word = 0;

		/* Compares the key. */
		for (matched = 0; key[matched] != '\0'; matched++) {
			if (position + matched >= length)
				break;
			if (line[position + matched] != key[matched])
				break;
		}
		if (key[matched] != '\0')
			continue;

		/* Reads the decimal value after the key. */
		value = 0;
		position += matched;
		while (position < length && line[position] >= '0' && line[position] <= '9') {
			value = value * 10U + (unsigned)(line[position] - '0');
			if (value > 1000U)
				value = 1000U;
			position++;
		}

		/* Succeeded: the value given. */
		return value;
	}

	/* The parameter is absent. */
	return 0;
}

/* Reports a CPU's bit in the GIC's target list: its interface is its core number. */
static uint32_t
cpu_interface_bit(
	hal_cpu_id_t cpu)
{
	uint32_t bit;

	/* The GIC-400 of the Pi 4 and 5 numbers its CPU interfaces by core. */
	bit = 1U << (uint32_t)cpu_core[cpu];

	/* Succeeded: the target-list bit. */
	return bit;
}

/* Reports whether a CPU has published itself as up. */
static int
cpu_is_ready(
	hal_cpu_id_t cpu)
{
	struct arm64_percpu *record;
	unsigned ready;

	/* A number past the records names no CPU. */
	record = arm64_percpu_get(cpu);
	if (record == NULL)
		return 0;

	/* The CPU's own release pairs with this acquire. */
	ready = __atomic_load_n(&record->ready, __ATOMIC_ACQUIRE);

	/* Succeeded: whether the CPU is up. */
	return ready != 0;
}

/*
 * Releases one core through the board and waits until it is up.
 *
 * A core that is not released, or does not come up, stops the boot: the
 * kernel was already told it exists (D6).
 */
static int
start_one(
	hal_cpu_id_t cpu)
{
	struct arm64_secondary_boot *block;
	hal_physaddr_t stack;
	uint64_t entry_phys;
	uint64_t core;
	uint64_t frequency;
	uint64_t deadline;
	uint64_t now;
	int error;
	int ready;

	/* Allocates the core's boot stack, which becomes its initial task's. */
	error = hal_pmem_alloc(ARM64_SECONDARY_STACK_SIZE, ARM64_PAGE_SIZE, &stack);
	if (error != HAL_OK)
		return error;

	/* Fills the core's boot block and cleans it to memory for the uncached reader. */
	core = cpu_core[cpu];
	block = &arm64_secondary_boot[core];
	block->stack_top = (uint64_t)(uintptr_t)hal_pmem_to_kernel(stack) + ARM64_SECONDARY_STACK_SIZE;
	block->cpu = cpu;
	block->ttbr1 = arm64_space_kernel_ttbr1();
	hal_dcache_clean_range((uintptr_t)block, sizeof(*block));

	/* Asks the board to start the core at the entry's physical address. */
	entry_phys = arm64_direct_to_phys((const void *)arm64_secondary_entry);
	error = arm64_bsp_cpu_release(&cpu_entry[cpu], entry_phys);
	if (error != HAL_OK) {
		hal_printf("ARM64 SMP cpu %u (core %u) was not released (%d); "
			   "add cpus=1 to cmdline.txt to boot on one CPU\n",
			   (uint32_t)cpu,
			   (uint32_t)core,
			   error);
		HAL_FATAL("arm64 secondary CPU was not released");
	}

	/* The limit of the wait for the core to publish itself. */
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
	__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(now) : : "memory");
	deadline = now + frequency * ARM64_SECONDARY_WAIT_MS / 1000U;

	/* Waits for the core to publish itself, up to the limit. */
	for (;;) {
		ready = cpu_is_ready(cpu);
		if (ready)
			break;

		/* A core that does not come up stops the boot. */
		__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(now) : : "memory");
		if (now >= deadline) {
			hal_printf("ARM64 SMP cpu %u (core %u) did not come up; "
				   "add cpus=1 to cmdline.txt to boot on one CPU\n",
				   (uint32_t)cpu,
				   (uint32_t)core);
			HAL_FATAL("arm64 secondary CPU did not come up");
		}

		/* Lets the other hardware thread run while the core starts. */
		hal_atomic_relax();
	}

	/* Reports the core in the boot log. */
	hal_printf("ARM64 SMP cpu %u online (core %u)\n", (uint32_t)cpu, (uint32_t)core);

	/* Succeeded: the core runs its initial task. */
	return HAL_OK;
}
