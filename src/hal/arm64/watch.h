/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arm64 stall watch: what each CPU leaves behind so that a CPU that
 * stops can be described, and the calls that look for one (watch.c).
 */

#ifndef KERN_HAL_ARM64_WATCH_H
#define KERN_HAL_ARM64_WATCH_H

#include <hal/hal.h>
#include "asm.h"
#include "percpu.h"

/* How many calls of hal_cpu_current() with interrupts off pass between two looks at the CPU's own tick. */
#define ARM64_WATCH_SELF_INTERVAL 4096U

/* How many tries of a HAL spin loop pass between two looks at the CPU's own tick. */
#define ARM64_WATCH_SPIN_INTERVAL 65536U

void arm64_watch_init(void);
void arm64_watch_beat(struct arm64_percpu *cpu, uint64_t now);
void arm64_watch_self_check(void) __attribute__((noinline, cold));
void arm64_watch_fatal(const char *file, int line, const char *message) __attribute__((noinline, cold));
void arm64_watch_fault(uint64_t esr, uint64_t elr, uint64_t far) __attribute__((noinline, cold));
void arm64_watch_note_char(int character);

/*
 * Reads the calling CPU's record, or NULL before the CPU has one.
 *
 * TPIDR_EL1 is zero until arm64_percpu_bootstrap(), and the interrupt
 * calls below run from the first instruction of the HAL on.
 */
static __inline struct arm64_percpu *
arm64_watch_record(
	void)
{
	uint64_t value;

	/* TPIDR_EL1 names the record, or is still zero. */
	__asm__ volatile("mrs %0, tpidr_el1" : "=r"(value));

	/* Succeeded: the record, or NULL. */
	return (struct arm64_percpu *)(uintptr_t)value;
}

/*
 * Reads the counter without waiting for earlier instructions.
 *
 * The watch wants a cheap, roughly placed timestamp, not an exact one.
 */
static __inline uint64_t
arm64_watch_counter(
	void)
{
	uint64_t value;

	/* The physical counter, the same on every CPU. */
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(value));

	/* Succeeded: the counter's value about now. */
	return value;
}

/*
 * Notes that the calling CPU just turned its interrupts off.
 *
 * Called only on the change from on to off, with the place of the code
 * that did it and of its caller, so a CPU that stops with interrupts off
 * is described by where they went off.
 */
static __inline void
arm64_watch_note_irq_off(
	uint64_t pc,
	uint64_t caller)
{
	struct arm64_percpu *cpu;
	uint64_t now;

	/* A CPU without a record yet has nothing to note into. */
	cpu = arm64_watch_record();
	if (cpu == NULL)
		return;

	/* Records the time and the place, which a later report reads. */
	now = arm64_watch_counter();
	__atomic_store_n(&cpu->watch_irq_off_pc, pc, __ATOMIC_RELAXED);
	__atomic_store_n(&cpu->watch_irq_off_caller, caller, __ATOMIC_RELAXED);
	__atomic_store_n(&cpu->watch_irq_off_time, now, __ATOMIC_RELAXED);
}

/*
 * Notes that the calling CPU's interrupts are about to come on again.
 *
 * A caller whose interrupts are already on has nothing to clear, and
 * could be moved to another CPU between reading the record and the
 * store, clearing that CPU's note instead.
 */
static __inline void
arm64_watch_note_irq_on(
	void)
{
	struct arm64_percpu *cpu;
	int masked;

	/* Interrupts that are on already were noted as on. */
	masked = arm64_irq_masked();
	if (!masked)
		return;

	/* A CPU without a record yet has nothing to clear. */
	cpu = arm64_watch_record();
	if (cpu == NULL)
		return;

	/* A zero time says that interrupts are on. */
	__atomic_store_n(&cpu->watch_irq_off_time, 0U, __ATOMIC_RELAXED);
}

/*
 * Notes that the calling CPU stops for good on purpose.
 *
 * Called with interrupts off, just before the CPU halts for ever, so the
 * other CPUs do not report its silence as a stall.
 */
static __inline void
arm64_watch_park(
	void)
{
	struct arm64_percpu *cpu;

	/* A CPU without a record yet is not looked at by the others. */
	cpu = arm64_watch_record();
	if (cpu == NULL)
		return;

	/* The others leave a parked CPU out of their look at the heartbeats. */
	__atomic_store_n(&cpu->watch_parked, 1U, __ATOMIC_RELEASE);
}

#endif
