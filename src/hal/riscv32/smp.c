/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv32 hart lifecycle: which harts exist, how the secondary harts
 * are started, and how harts reach each other.
 *
 * The firmware holds every hart but the boot hart stopped.  The device
 * tree lists the harts; the ones that translate addresses and are not
 * disabled are used, the boot hart first, up to the cpus=N boot parameter.
 * The kernel is told the number before any hart starts.  Each is started
 * through the firmware, one at a time; one that does not come up stops the
 * boot, with the advice to start with cpus=1.
 *
 * A hart interrupts another with the supervisor software interrupt, which
 * carries no reason of its own: the reasons are bits of the receiver's
 * per-CPU word, set before the interrupt is raised.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bootinfo.h"
#include "clock.h"
#include "defs.h"
#include "int.h"
#include "irq.h"
#include "percpu.h"
#include "sbi.h"
#include "space.h"
#include "task.h"

/* The bytes of a secondary hart's boot stack, which becomes its initial task's. */
#define SMP_SECONDARY_STACK_SIZE	16384U

/* How long the boot hart waits for a started hart, in milliseconds. */
#define SMP_SECONDARY_WAIT_MS		1000U

/* The highest hart id a hart mask of the firmware calls can name from base 0. */
#define SMP_HART_ID_LIMIT		64U

/*
 * What a secondary hart reads, with translation off, to come up.
 *
 * locore.S reads it at the RISCV32_BOOT_BLOCK_ offsets through its
 * physical address, which the firmware passes in a1.
 */
struct smp_boot_block {
	uint32_t satp;
	uint32_t stack_top;
	uint32_t cpu;
	uint32_t hart_id;
	uint32_t reserved[12];
};

_Static_assert(sizeof(struct smp_boot_block) == RISCV32_BOOT_BLOCK_SIZE,
	       "locore.S reads the boot block at fixed offsets");
_Static_assert(__builtin_offsetof(struct smp_boot_block, stack_top) == RISCV32_BOOT_BLOCK_STACK,
	       "locore.S reads the stack top at this offset");
_Static_assert(__builtin_offsetof(struct smp_boot_block, cpu) == RISCV32_BOOT_BLOCK_CPU,
	       "locore.S reads the logical number at this offset");

void riscv32_secondary_entry(void);
void riscv32_secondary_main(unsigned long cpu, unsigned long hart_id);

/*
 * The boot blocks, one per logical hart.
 *
 * The boot hart fills a block before it starts the hart; the hart reads it
 * once.  The memory is coherent, so no cache maintenance is needed.
 */
static struct smp_boot_block smp_boot_blocks[RISCV32_CPU_MAX] __attribute__((aligned(64)));

/*
 * How many harts the kernel is told about.
 *
 * Fixed by riscv32_smp_configure() before the kernel asks for it, and
 * never changed: the kernel sizes its per-CPU tables by it before
 * hal_cpu_start_others() runs.
 */
static unsigned cpu_count = 1;

/* Each logical hart's hart id, logical 0 being the boot hart.  Filled by riscv32_smp_configure(). */
static unsigned long cpu_hart_id[RISCV32_CPU_MAX];

/*
 * The harts that are up, one bit per logical hart.
 *
 * A hart sets its own bit with a release once it runs its initial task and
 * its tick; the boot hart's bit is set by the boot.
 */
static struct hal_cpu_mask ready_mask;

static int start_one(hal_cpu_id_t cpu);
static int cpu_is_ready(hal_cpu_id_t cpu);
static int send_reason(hal_cpu_id_t cpu, unsigned long reason);
static void describe_bringup(void);

/*
 * Makes the boot hart known: its record, and its place in the ready mask.
 */
void
riscv32_smp_bootstrap(
	unsigned long hart_id)
{
	/* Makes the record the boot hart's own before any lock is taken. */
	riscv32_percpu_bootstrap(0, hart_id);
	cpu_hart_id[0] = hart_id;

	/* The boot hart is up from here on. */
	hal_cpu_mask_zero(&ready_mask);
	hal_cpu_mask_set(&ready_mask, 0);
	__atomic_store_n(&riscv32_percpu_get(0)->ready, 1U, __ATOMIC_RELEASE);
}

/*
 * Decides how many harts the kernel gets, from the device tree and cpus=N.
 */
void
riscv32_smp_configure(
	void)
{
	const struct riscv32_fdt_info *info;
	const struct riscv32_fdt_cpu *entry;
	unsigned limit;
	unsigned index;
	unsigned usable;

	/* The boot parameter caps the count; absent, every usable hart is used. */
	limit = riscv32_boot_parameter_number("cpus=");
	if (limit == 0 || limit > RISCV32_CPU_MAX)
		limit = RISCV32_CPU_MAX;

	/* The boot hart's id must be one a hart mask can name. */
	if (cpu_hart_id[0] >= SMP_HART_ID_LIMIT)
		HAL_FATAL("riscv32 boot hart id beyond the hart masks");

	/* Takes each other usable hart in the tree's order. */
	info = riscv32_boot_fdt();
	usable = 0;
	for (index = 0; index < info->cpu_count; index++) {
		entry = &info->cpus[index];
		if (!entry->usable)
			continue;
		usable++;

		/* The boot hart is already logical 0. */
		if (entry->hart_id == cpu_hart_id[0])
			continue;

		/* A hart a mask cannot name is left alone. */
		if (entry->hart_id >= SMP_HART_ID_LIMIT)
			continue;

		/* Stops at the limit. */
		if (cpu_count >= limit)
			break;
		cpu_hart_id[cpu_count] = entry->hart_id;
		cpu_count++;
	}

	hal_printf("RISCV32 SMP %u CPUs (device tree %u usable of %u, limit %u)\n",
		   cpu_count,
		   usable,
		   info->cpu_count,
		   limit);
}

/*
 * Reports the hart the caller runs on.
 */
hal_cpu_id_t
hal_cpu_current(
	void)
{
	struct riscv32_percpu *record;

	/*
	 * The record the thread pointer names carries the hart's number.  A
	 * caller with interrupts on may be moved to another hart right after
	 * the read; it gets the hart it was on, which is all such a caller can
	 * be told.
	 */
	record = riscv32_percpu_current();

	/* Succeeded: the caller's hart. */
	return record->logical_id;
}

/*
 * Reports how many harts the kernel schedules on.
 */
unsigned
hal_cpu_count(
	void)
{
	/* Succeeded: the count fixed at boot. */
	return cpu_count;
}

/*
 * Reports the harts that are up.
 */
void
hal_cpu_ready_mask(
	struct hal_cpu_mask *result)
{
	unsigned word;

	/* A caller with nowhere to put the answer asked for nothing. */
	if (result == NULL)
		return;

	/* Copies each word with an acquire, pairing with the harts' own release. */
	for (word = 0; word < HAL_CPU_MASK_WORDS; word++)
		result->bits[word] = hal_atomic_load_acquire(&ready_mask.bits[word]);
}

/*
 * Starts the harts other than the boot hart, one at a time.
 */
int
hal_cpu_start_others(
	void)
{
	hal_cpu_id_t cpu;
	int error;

	/* Starts each hart and waits until it is up. */
	for (cpu = 1; cpu < cpu_count; cpu++) {
		error = start_one(cpu);
		if (error != HAL_OK)
			return error;
	}

	/* Closes the HAL's part of the boot log with what the bring-up showed. */
	describe_bringup();

	/* Succeeded: every hart the kernel was told about is up. */
	hal_printf("RISCV32 SMP %u ONLINE\n", cpu_count);
	return HAL_OK;
}

/*
 * Enters the HAL on a secondary hart, from locore.S, on its boot stack.
 *
 * The hart arrives with translation on through the boot table; it takes
 * its record, the kernel's root, its trap entry and interrupt context, its
 * initial task and its tick, then joins the kernel.
 */
void
riscv32_secondary_main(
	unsigned long cpu,
	unsigned long hart_id)
{
	struct riscv32_percpu *record;
	hal_task_t task;

	/* Takes the hart's record before anything that may lock. */
	riscv32_percpu_bootstrap((hal_cpu_id_t)cpu, hart_id);
	record = riscv32_percpu_current();

	/* Leaves the boot table for the kernel's root. */
	riscv32_space_select_system();

	/* Sets up the hart's trap entry and its interrupt context. */
	riscv32_int_init_secondary();

	/* Wraps the boot stack's context as the hart's initial task. */
	task = hal_task_create_for_init_context();
	if (task == NULL)
		HAL_FATAL("riscv32 secondary initial task allocation failed");

	/* Starts the hart's scheduling tick. */
	riscv32_clock_init();

	/* Publishes the hart first through its record, then the ready mask. */
	__atomic_store_n(&record->ready, 1U, __ATOMIC_RELEASE);
	(void)hal_atomic_fetch_or_release(&ready_mask.bits[cpu / 64U],
					  (uint64_t)1U << (cpu % 64U));

	/* Joins the kernel's scheduler; this never returns. */
	kernel_secondary_entry((hal_cpu_id_t)cpu);
	HAL_FATAL("kernel_secondary_entry returned");
}

/*
 * Interrupts another hart so that it looks for work.
 */
int
hal_cpu_notify(
	hal_cpu_id_t cpu)
{
	int ready;
	int error;

	/* A hart the kernel was not told about does not exist. */
	if (cpu >= cpu_count)
		return HAL_ERR_INVALID;

	/* A hart that is not up cannot take the interrupt. */
	ready = cpu_is_ready(cpu);
	if (!ready)
		return HAL_ERR_STATE;

	/* Raises the notification on the hart. */
	error = send_reason(cpu, RISCV32_IPI_NOTIFY);
	if (error != HAL_OK)
		return error;

	/* Succeeded: the hart runs kernel_cpu_notify_handler(). */
	return HAL_OK;
}

/*
 * Interrupts every hart of a mask, as hal_cpu_notify() does one.
 */
int
hal_cpu_notify_mask(
	const struct hal_cpu_mask *targets)
{
	hal_cpu_id_t cpu;
	int named;
	int ready;
	int any;
	int error;

	/* A missing mask names no hart. */
	if (targets == NULL)
		return HAL_ERR_INVALID;

	/* Refuses a mask that names a hart the kernel was not told about. */
	for (cpu = cpu_count; cpu < HAL_CPU_MAX; cpu++) {
		named = hal_cpu_mask_test(targets, cpu);
		if (named)
			return HAL_ERR_INVALID;
	}

	/* Every named hart must be up before any is notified. */
	any = 0;
	for (cpu = 0; cpu < cpu_count; cpu++) {
		named = hal_cpu_mask_test(targets, cpu);
		if (!named)
			continue;
		ready = cpu_is_ready(cpu);
		if (!ready)
			return HAL_ERR_STATE;
		any = 1;
	}

	/* An empty mask is not a request. */
	if (!any)
		return HAL_ERR_INVALID;

	/* Notifies each named hart. */
	for (cpu = 0; cpu < cpu_count; cpu++) {
		named = hal_cpu_mask_test(targets, cpu);
		if (!named)
			continue;
		error = send_reason(cpu, RISCV32_IPI_NOTIFY);
		if (error != HAL_OK)
			return error;
	}

	/* Succeeded: every named hart runs kernel_cpu_notify_handler(). */
	return HAL_OK;
}

/*
 * Takes the software interrupt: clears it, then serves every reason set,
 * until no reason is left.
 *
 * The pending bit is cleared before the reasons are read, so a reason set
 * after the read raises the interrupt again rather than being lost.
 */
void
riscv32_smp_take_ipi(
	void)
{
	struct riscv32_percpu *cpu;
	unsigned long reasons;

	/* Serves the reasons until a pass finds none. */
	cpu = riscv32_percpu_current();
	for (;;) {
		riscv32_clear_sip(RISCV32_SIE_SSIE);
		reasons = __atomic_exchange_n(&cpu->ipi_reasons, 0UL, __ATOMIC_ACQ_REL);
		if (reasons == 0)
			return;

		/* Another hart panicked: this one stops where it is. */
		if ((reasons & RISCV32_IPI_STOP) != 0)
			hal_cpu_park();

		/*
		 * Another hart asks this one to look for work.  The interrupt
		 * needs no end at a controller, so its acknowledgement is the
		 * local one.
		 */
		if ((reasons & RISCV32_IPI_NOTIFY) != 0) {
			cpu->active_ack = RISCV32_ACK_LOCAL;
			kernel_cpu_notify_handler(cpu->logical_id, RISCV32_ACK_LOCAL);
			cpu = riscv32_percpu_current();
		}
	}
}

/*
 * Idles the hart until one interrupt arrives.
 *
 * wfi returns once an interrupt the enable register lets in is pending,
 * even with the global enable off; the global enable is then turned on
 * for a moment so the interrupt is taken, and off again.
 */
void
hal_cpu_idle(
	void)
{
	/* Waits, takes what woke the hart, and leaves with interrupts off. */
	riscv32_wfi();
	riscv32_set_sstatus(RISCV32_SSTATUS_SIE);
	riscv32_clear_sstatus(RISCV32_SSTATUS_SIE);
}

/*
 * Halts until the next interrupt.
 */
void
hal_halt(
	void)
{
	/* The same wait as the idle, without taking the interrupt. */
	riscv32_wfi();
}

/*
 * Stops the calling hart for good.
 */
_Noreturn void
hal_cpu_park(
	void)
{
	/* Waits with interrupts off, so nothing wakes the hart into the kernel again. */
	riscv32_halt_forever();
}

/*
 * Stops every hart after a panic.
 *
 * The other harts get the stop reason; one that runs with interrupts off
 * does not take it and runs on until it does.
 */
_Noreturn void
hal_cpu_panic_all(
	void)
{
	hal_cpu_id_t self;
	hal_cpu_id_t cpu;
	int ready;

	/*
	 * Turns interrupts off for good first, so the hart named as this one
	 * is the one that parks below.
	 */
	(void)hal_irq_disable();

	/* Tells every other hart that is up to stop, then stops this one. */
	self = hal_cpu_current();
	for (cpu = 0; cpu < cpu_count; cpu++) {
		/* The panicking hart parks itself below. */
		if (cpu == self)
			continue;

		/* A hart that never came up has nothing to stop. */
		ready = cpu_is_ready(cpu);
		if (ready)
			(void)send_reason(cpu, RISCV32_IPI_STOP);
	}

	/* Stops this hart too. */
	hal_cpu_park();
}

/* Reports whether a hart has published itself as up. */
static int
cpu_is_ready(
	hal_cpu_id_t cpu)
{
	struct riscv32_percpu *record;
	unsigned ready;

	/* A number past the records names no hart. */
	record = riscv32_percpu_get(cpu);
	if (record == NULL)
		return 0;

	/* The hart's own release pairs with this acquire. */
	ready = __atomic_load_n(&record->ready, __ATOMIC_ACQUIRE);
	if (ready == 0)
		return 0;

	/* Succeeded: the hart is up. */
	return 1;
}

/* Sets a reason on a hart and raises its software interrupt. */
static int
send_reason(
	hal_cpu_id_t cpu,
	unsigned long reason)
{
	struct riscv32_percpu *record;
	int error;

	/* The reason is visible before the interrupt that reports it. */
	record = riscv32_percpu_get(cpu);
	(void)__atomic_fetch_or(&record->ipi_reasons, reason, __ATOMIC_RELEASE);

	/* Raises the interrupt on the hart through the firmware. */
	error = riscv32_sbi_send_ipi(1UL << cpu_hart_id[cpu], 0);
	if (error != HAL_OK)
		return error;

	/* Succeeded: the hart has the interrupt pending. */
	return HAL_OK;
}

/*
 * Starts one hart through the firmware and waits until it is up.
 *
 * A hart that does not come up stops the boot: the kernel was already
 * told it exists.
 */
static int
start_one(
	hal_cpu_id_t cpu)
{
	struct smp_boot_block *block;
	hal_physaddr_t stack;
	uint64_t frequency;
	uint64_t deadline;
	uint64_t now;
	uint64_t entry_phys;
	uint64_t block_phys;
	int error;
	int ready;

	/* Allocates the hart's boot stack, which becomes its initial task's. */
	error = hal_pmem_alloc(SMP_SECONDARY_STACK_SIZE, RISCV32_PAGE_SIZE, &stack);
	if (error != HAL_OK)
		return error;

	/* Fills the hart's boot block. */
	block = &smp_boot_blocks[cpu];
	block->satp = riscv32_boot_satp();
	block->stack_top = (uint64_t)(uintptr_t)hal_pmem_to_kernel(stack) + SMP_SECONDARY_STACK_SIZE;
	block->cpu = cpu;
	block->hart_id = cpu_hart_id[cpu];
	hal_mb();

	/* Asks the firmware to start the hart at the entry's physical address. */
	entry_phys = riscv32_virt_to_phys((const void *)riscv32_secondary_entry);
	block_phys = riscv32_virt_to_phys(block);
	error = riscv32_sbi_hart_start((unsigned long)cpu_hart_id[cpu], (uintptr_t)entry_phys, (uintptr_t)block_phys);
	if (error != HAL_OK) {
		hal_printf("RISCV32 SMP hart %lu could not be started (%u)\n",
			   cpu_hart_id[cpu],
			   (uint32_t)error);
		HAL_FATAL("riscv32 secondary hart did not start");
	}

	/* Waits for the hart to publish itself, up to the limit. */
	frequency = riscv32_boot_fdt()->timebase_frequency;
	now = riscv32_read_time();
	deadline = now + frequency * SMP_SECONDARY_WAIT_MS / 1000U;
	for (;;) {
		ready = cpu_is_ready(cpu);
		if (ready)
			break;

		/* A hart that does not come up stops the boot. */
		now = riscv32_read_time();
		if (now >= deadline) {
			hal_printf("RISCV32 SMP cpu %u (hart %lu) did not come up; "
				   "boot with cpus=1 to use one hart\n",
				   (uint32_t)cpu,
				   cpu_hart_id[cpu]);
			HAL_FATAL("riscv32 secondary hart did not come up");
		}

		/* Waits a moment before the next attempt. */
		hal_atomic_relax();
	}

	/* Reports the hart that came up. */
	hal_printf("RISCV32 SMP cpu %u online (hart %lu)\n", (uint32_t)cpu, cpu_hart_id[cpu]);

	/* Succeeded: the hart runs its initial task. */
	return HAL_OK;
}

/*
 * Writes one line that sums up the bring-up: the firmware's SBI version,
 * the kernel's tables, and the boot hart's tick.
 *
 * Starting the harts is the HAL's last step of the boot, and the kernel
 * prints a screenful of its own after it, which pushes the HAL's earlier
 * lines off a small display.  This line, with the ONLINE line after it,
 * stays in view.
 */
static void
describe_bringup(
	void)
{
	unsigned long major;
	unsigned long minor;
	const char *paging;
	const char *timer;
	bool checked;
	bool ticked;

	/* Reads the firmware's version. */
	riscv32_sbi_version(&major, &minor);

	/* Words the result of the table check. */
	checked = riscv32_space_checked();
	if (checked) {
		paging = "PASS";
	} else {
		paging = "FAIL";
	}

	/* Words whether the boot hart's tick has arrived, giving it a moment. */
	ticked = riscv32_clock_ticked();
	if (ticked) {
		timer = "TICK";
	} else {
		timer = "NO TICK";
	}

	/* Writes the line. */
	hal_printf("RISCV32 HAL READY SBI v%lu.%lu PAGING %s TIMER %s\n",
		   (unsigned long)major,
		   (unsigned long)minor,
		   paging,
		   timer);
}
