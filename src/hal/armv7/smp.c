/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 processor lifecycle: which processors run the kernel, how
 * they reach each other, and how they wait and stop.
 *
 * The kernel runs on the boot processor alone for now; a second processor
 * the board may have is left where the loader left it.  The per-CPU
 * records, the ready mask and the notifications are those of a
 * multiprocessor kernel all the same: a processor interrupts another with
 * a software-generated interrupt, whose reason is a bit of the receiver's
 * per-CPU word set before the interrupt is raised.
 *
 * hal_cpu_start_others() is the HAL's last step of the boot, once the
 * kernel's heap exists: it checks the address spaces and the
 * floating-point unit's lazy switch and writes the bring-up's summary
 * line.
 */

#include <hal/hal.h>
#include "asm.h"
#include "clock.h"
#include "defs.h"
#include "gic.h"
#include "percpu.h"
#include "selftest.h"
#include "space.h"

/* The pieces of the bring-up's summary line. */
#define SMP_READY_PIECES	6U

/*
 * The processors that are up, one bit per logical processor.  A processor
 * sets its own bit with a release once it runs its initial task and its
 * tick; the boot processor's bit is set by the boot.
 */
static struct hal_cpu_mask ready_mask;

static int cpu_is_ready(hal_cpu_id_t cpu);
static void send_reason(hal_cpu_id_t cpu, unsigned long reason);

/*
 * Makes the boot processor known: its record, and its place in the ready
 * mask.
 */
void
armv7_smp_bootstrap(
	void)
{
	uint32_t mpidr;

	/* Makes the record the boot processor's own before any lock is taken. */
	mpidr = armv7_read_mpidr();
	armv7_percpu_bootstrap(0, mpidr);

	/* The boot processor is up from here on. */
	hal_cpu_mask_zero(&ready_mask);
	hal_cpu_mask_set(&ready_mask, 0);
	__atomic_store_n(&armv7_percpu_get(0)->ready, 1U, __ATOMIC_RELEASE);
}

/*
 * Reports the processor the caller runs on.
 */
hal_cpu_id_t
hal_cpu_current(
	void)
{
	struct armv7_percpu *record;

	/*
	 * The record TPIDRPRW names carries the processor's number.  A caller
	 * with interrupts on may be moved to another processor right after
	 * the read; it gets the one it was on, which is all such a caller can
	 * be told.
	 */
	record = armv7_percpu_current();

	/* Succeeded: the caller's processor. */
	return record->logical_id;
}

/*
 * Reports how many processors the kernel schedules on: the boot processor.
 */
unsigned
hal_cpu_count(
	void)
{
	/* Succeeded: one processor. */
	return 1;
}

/*
 * Reports the processors that are up.
 */
void
hal_cpu_ready_mask(
	struct hal_cpu_mask *result)
{
	unsigned word;

	/* A caller with nowhere to put the answer asked for nothing. */
	if (result == NULL)
		return;

	/* Copies each word with an acquire, pairing with the processors' own release. */
	for (word = 0; word < HAL_CPU_MASK_WORDS; word++)
		result->bits[word] = hal_atomic_load_acquire(&ready_mask.bits[word]);
}

/*
 * Ends the HAL's part of the boot: checks the address spaces, now that the
 * kernel's heap exists, and writes the summary line.  There is no other
 * processor to start.
 */
int
hal_cpu_start_others(
	void)
{
	const char *pieces[SMP_READY_PIECES];
	const char *timer;
	const char *vfp;
	unsigned long ticks;
	unsigned index;
	int vfp_checked;

	/* Stops at the first check that fails; returns only when all passed. */
	armv7_paging_selftest();
	vfp_checked = armv7_vfp_selftest();

	/* Words whether the floating-point unit was there to check. */
	vfp = "VFP NONE";
	if (vfp_checked)
		vfp = "VFP PASS";

	/* Words whether the tick has arrived. */
	timer = "TIMER NO TICK";
	ticks = armv7_clock_ticks();
	if (ticks != 0)
		timer = "TIMER TICK";

	/*
	 * Writes the line, which stays in view after the kernel's own lines.
	 * The checks before it stopped the boot if they failed, so each of them
	 * passed.  The floating-point piece comes last, so a line read up to
	 * the tick's piece reads as it did before the unit was checked.
	 */
	pieces[0] = "ARMV7 HAL READY";
	pieces[1] = "AEABI PASS";
	pieces[2] = "ATOMIC PASS";
	pieces[3] = "PAGING PASS";
	pieces[4] = timer;
	pieces[5] = vfp;
	for (index = 0; index < SMP_READY_PIECES; index++) {
		if (index != 0)
			hal_putc(' ');
		hal_puts(pieces[index]);
	}

	/* The count of processors, the line the tests look for. */
	hal_puts("\nARMV7 CPU 1 ONLINE\n");

	/* Succeeded: the one processor is up. */
	return HAL_OK;
}

/*
 * Interrupts a processor so that it looks for work.
 */
int
hal_cpu_notify(
	hal_cpu_id_t cpu)
{
	unsigned count;
	int ready;

	/* A processor the kernel was not told about does not exist. */
	count = hal_cpu_count();
	if (cpu >= count)
		return HAL_ERR_INVALID;

	/* A processor that is not up cannot take the interrupt. */
	ready = cpu_is_ready(cpu);
	if (!ready)
		return HAL_ERR_STATE;

	/* Raises the notification on the processor. */
	send_reason(cpu, ARMV7_IPI_NOTIFY);

	/* Succeeded: the processor runs kernel_cpu_notify_handler(). */
	return HAL_OK;
}

/*
 * Interrupts every processor of a mask, as hal_cpu_notify() does one.
 */
int
hal_cpu_notify_mask(
	const struct hal_cpu_mask *targets)
{
	hal_cpu_id_t cpu;
	unsigned count;
	int named;
	int ready;
	int any;

	/* A missing mask names no processor. */
	if (targets == NULL)
		return HAL_ERR_INVALID;

	/* Refuses a mask that names a processor the kernel was not told about. */
	count = hal_cpu_count();
	for (cpu = count; cpu < HAL_CPU_MAX; cpu++) {
		named = hal_cpu_mask_test(targets, cpu);
		if (named)
			return HAL_ERR_INVALID;
	}

	/* Every named processor must be up before any is notified. */
	any = 0;
	for (cpu = 0; cpu < count; cpu++) {
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

	/* Notifies each named processor. */
	for (cpu = 0; cpu < count; cpu++) {
		named = hal_cpu_mask_test(targets, cpu);
		if (named)
			send_reason(cpu, ARMV7_IPI_NOTIFY);
	}

	/* Succeeded: every named processor runs kernel_cpu_notify_handler(). */
	return HAL_OK;
}

/*
 * Serves a software-generated interrupt: takes every reason set, and has
 * the interrupt ended by whatever serves it.
 *
 * A reason set after the reasons are taken raises the interrupt again,
 * which the GIC holds pending until this one is ended, so it is not lost.
 */
void
armv7_smp_take_ipi(
	uint32_t sgi,
	hal_irq_ack_t acknowledge)
{
	struct armv7_percpu *cpu;
	unsigned long reasons;

	UNUSED_PARAMETER(sgi);

	/* Takes every reason set so far. */
	cpu = armv7_percpu_current();
	reasons = __atomic_exchange_n(&cpu->ipi_reasons, 0UL, __ATOMIC_ACQ_REL);

	/* Another processor panicked: this one stops where it is. */
	if ((reasons & ARMV7_IPI_STOP) != 0) {
		hal_irq_send_eoi(acknowledge);
		hal_cpu_park();
	}

	/* Another processor asks this one to look for work; the kernel ends the interrupt. */
	if ((reasons & ARMV7_IPI_NOTIFY) != 0) {
		kernel_cpu_notify_handler(cpu->logical_id, acknowledge);
		return;
	}

	/* An interrupt whose reason an earlier one already served ends here. */
	hal_irq_send_eoi(acknowledge);
}

/*
 * Idles the processor until one interrupt arrives.
 *
 * wfi returns once an interrupt is pending even while it is masked; the
 * mask is then lifted for a moment so the interrupt is taken, and set
 * again.  An interrupt that arrives before the wfi ends it at once.
 */
void
hal_cpu_idle(
	void)
{
	/* Waits, takes what woke the processor, and leaves with interrupts masked. */
	armv7_wfi();
	__asm__ volatile("cpsie i\n\tisb\n\tcpsid i" : : : "memory");
}

/*
 * Halts until the next interrupt.
 */
void
hal_halt(
	void)
{
	/* The same wait as the idle, without taking the interrupt. */
	armv7_wfi();
}

/*
 * Stops the calling processor for good.
 */
_Noreturn void
hal_cpu_park(
	void)
{
	/* Waits with interrupts masked, so nothing wakes the processor into the kernel again. */
	armv7_halt_forever();
}

/*
 * Stops every processor after a panic.
 *
 * The other processors get the stop reason; one that runs with interrupts
 * masked does not take it and runs on until it does.
 */
_Noreturn void
hal_cpu_panic_all(
	void)
{
	hal_cpu_id_t self;
	hal_cpu_id_t cpu;
	unsigned count;
	int ready;

	/* Masks interrupts for good first, so the processor named as this one is the one that parks. */
	(void)hal_irq_disable();

	/* Tells every other processor that is up to stop, then stops this one. */
	self = hal_cpu_current();
	count = hal_cpu_count();
	for (cpu = 0; cpu < count; cpu++) {
		/* The panicking processor parks itself below. */
		if (cpu == self)
			continue;

		/* A processor that never came up has nothing to stop. */
		ready = cpu_is_ready(cpu);
		if (ready)
			send_reason(cpu, ARMV7_IPI_STOP);
	}

	/* Stops this processor too. */
	hal_cpu_park();
}

/* Reports whether a processor has published itself as up. */
static int
cpu_is_ready(
	hal_cpu_id_t cpu)
{
	struct armv7_percpu *record;
	unsigned ready;

	/* A number past the records names no processor. */
	record = armv7_percpu_get(cpu);
	if (record == NULL)
		return 0;

	/* The processor's own release pairs with this acquire. */
	ready = __atomic_load_n(&record->ready, __ATOMIC_ACQUIRE);
	if (ready == 0)
		return 0;

	/* Succeeded: the processor is up. */
	return 1;
}

/*
 * Sets a reason on a processor and raises its software-generated
 * interrupt.  Logical processor N is CPU interface N.
 */
static void
send_reason(
	hal_cpu_id_t cpu,
	unsigned long reason)
{
	struct armv7_percpu *record;
	uint32_t sgi;

	/* The reason is visible before the interrupt that reports it. */
	record = armv7_percpu_get(cpu);
	(void)__atomic_fetch_or(&record->ipi_reasons, reason, __ATOMIC_RELEASE);

	/* Raises the interrupt of the reason's kind at the processor's interface. */
	sgi = ARMV7_SGI_NOTIFY;
	if ((reason & ARMV7_IPI_STOP) != 0)
		sgi = ARMV7_SGI_STOP;
	armv7_gic_send_sgi(sgi, 1U << cpu);
}
