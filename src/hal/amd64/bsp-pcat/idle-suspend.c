/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 suspend-to-idle (S0ix) of one CPU (ws052-p006, HAL v2 H1v2),
 * and the ordinary idle of hal_cpu_idle() in an MWAIT C-state.
 *
 * The state is the one the firmware names for low-power S0 idle: the LPIT's
 * first enabled native C-state with a functional-fixed-hardware trigger,
 * whose address is an MWAIT hint (C10 on the Latitude 5330).  The machine
 * can suspend to idle when that hint exists, the CPU has MONITOR/MWAIT with
 * the extensions that break on an interrupt while interrupts are masked
 * and has sub-states for the hint's C-state, and the TSC behind
 * hal_rtc_read_counter() is invariant (it keeps its rate through every
 * C-state).  QEMU/KVM has neither the LPIT nor an invariant TSC, so it is
 * refused there before any device is touched.
 *
 * Around the wait the CPU's periodic tick is stopped and its local
 * interrupt sources quieted, and both are put back before returning.
 * MWAIT is entered with interrupts masked, so an interrupt that arrives
 * between the caller's last check and the MWAIT still ends it; the
 * interrupt is then taken in an sti/nop window before interrupts are
 * masked again, so its handler has run when the call returns.
 *
 * The ordinary idle waits the same way, with the CPU's tick running: in
 * the deepest C-state up to IDLE_WAIT_DEEPEST_CSTATE that CPUID reports
 * sub-states for, when the CPU has MONITOR/MWAIT with the break on a masked
 * interrupt, its local APIC timer keeps running in every C-state (ARAT) and
 * the TSC is invariant; otherwise it halts (C1) as before.  A C-state
 * deeper than C1 lets the core power down, and a package whose cores all do
 * enter its own low-power states.
 */

#include <hal/hal.h>
#include "idle-suspend.h"
#include "acpi.h"
#include "lapic.h"
#include "timecounter.h"
#include "../defs.h"

/* CPUID: leaf 1's MONITOR/MWAIT bit, and leaf 5's extensions and break-on-interrupt bits. */
#define CPUID_LEAF_FEATURES		1U
#define CPUID_LEAF_MWAIT		5U
#define CPUID_FEATURE_MONITOR		(1U << 3)
#define CPUID_MWAIT_EXTENSIONS		(1U << 0)
#define CPUID_MWAIT_BREAK_ON_IRQ	(1U << 1)

/* An MWAIT hint's C-state (bits 4 to 7, 0 for C1) and sub-state (bits 0 to 3); leaf 5's EDX has 4 bits a C-state from C0. */
#define MWAIT_HINT_CSTATE_SHIFT		4U
#define MWAIT_HINT_FIELD_MASK		0xfU
#define MWAIT_CSTATES_IN_EDX		8U

/* MWAIT's ECX: break on an interrupt even while interrupts are masked. */
#define MWAIT_BREAK_ON_IRQ		1U

/* CPUID leaf 6's EAX: the local APIC timer runs in every C-state (ARAT). */
#define CPUID_LEAF_POWER		6U
#define CPUID_POWER_ARAT		(1U << 2)

/*
 * The deepest C-state the ordinary idle enters, as CPUID leaf 5 counts
 * them (C0 is 0, an MWAIT hint's C-state field plus one).  The tick wakes
 * every CPU each millisecond, so a state whose exit costs a large part of
 * that is not taken.
 */
#ifndef IDLE_WAIT_DEEPEST_CSTATE
#define IDLE_WAIT_DEEPEST_CSTATE	3U
#endif

/* A cache line, which MONITOR watches. */
#define IDLE_LINE_BYTES			64U

/*
 * The firmware's low-power S0 idle MWAIT hint, and whether the boot CPU
 * can enter it (MONITOR/MWAIT, the extensions and the hint's sub-state).
 * Both are written once on the boot CPU before any AP starts and read
 * afterwards by every CPU.
 */
static uint32_t idle_hint;
static unsigned idle_cpu_ready;

/*
 * The line each CPU's MONITOR arms.  Nothing writes it, so only an
 * interrupt ends an MWAIT; a line per CPU keeps them apart.
 */
static volatile uint8_t idle_lines[AMD64_SMP_MAX_CPUS][IDLE_LINE_BYTES] __attribute__((aligned(IDLE_LINE_BYTES)));

/*
 * The ordinary idle's MWAIT hint, and whether the CPU can wait in it
 * (idle_wait_ready).  Written once on the boot CPU before any AP starts.
 */
static uint32_t idle_wait_hint;
static unsigned idle_wait_ready;

static void idle_cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
static void idle_wait_init(void);
static unsigned idle_hint_supported(uint32_t hint);

/*
 * Takes the firmware's low-power S0 idle state from discovery and checks
 * that the boot CPU can enter it.
 */
void
prekern_amd64_idle_suspend_init(
	const struct amd64_acpi_info *acpi)
{
	unsigned supported;

	/* The ordinary idle's state, whatever the firmware names for S0ix. */
	idle_wait_init();

	/* Without the LPIT's state there is nothing to enter. */
	idle_cpu_ready = 0U;
	if (acpi == NULL || !acpi->lpit_found) {
		hal_puts("A64 IDLE SUSPEND NONE reason=lpit\n");
		return;
	}

	/* The CPU must reach the hint's state with MWAIT and break out of it on an interrupt. */
	supported = idle_hint_supported(acpi->lpit_mwait_hint);
	if (!supported) {
		hal_printf("A64 IDLE SUSPEND NONE reason=cpu hint=%02X\n", acpi->lpit_mwait_hint);
		return;
	}

	/* Succeeded: the state is kept for the probe and the idle. */
	idle_hint = acpi->lpit_mwait_hint;
	idle_cpu_ready = 1U;
	hal_printf("A64 IDLE SUSPEND READY hint=%02X\n", idle_hint);
}

/*
 * Reports whether hal_cpu_idle_suspend() works on this machine.
 */
int
hal_cpu_idle_suspend_supported(
	void)
{
	uint64_t counter;
	uint64_t frequency;
	bool counting;
	bool invariant;

	/* The firmware's state and the CPU's way into it. */
	if (!idle_cpu_ready)
		return HAL_ERR_UNSUPPORTED;

	/* The counter must be the TSC and keep its rate through the state. */
	invariant = amd64_timecounter_invariant();
	if (!invariant)
		return HAL_ERR_UNSUPPORTED;
	counting = amd64_timecounter_read(&counter, &frequency);
	if (!counting)
		return HAL_ERR_UNSUPPORTED;

	/* Succeeded: every CPU can suspend to idle. */
	return HAL_OK;
}

/*
 * Idles the current CPU in the firmware's low-power S0 idle state until
 * one interrupt arrives, its tick stopped and its local sources quieted
 * meanwhile.
 */
int
hal_cpu_idle_suspend(
	void)
{
	struct amd64_lapic_quiet saved;
	volatile uint8_t *line;
	hal_cpu_id_t cpu;
	uint32_t hint;
	int supported;

	/* Without the state the call does nothing. */
	supported = hal_cpu_idle_suspend_supported();
	if (supported != HAL_OK)
		return supported;

	/* The CPU's local sources quieted and its tick stopped. */
	amd64_lapic_quiet_sources(&saved);
	amd64_lapic_timer_stop();

	/* MONITOR on this CPU's line, then MWAIT into the state with interrupts still masked. */
	cpu = hal_cpu_current();
	line = idle_lines[cpu];
	hint = idle_hint;
	__asm__ volatile("monitor" :: "a"(line), "c"(0U), "d"(0U) : "memory");
	__asm__ volatile("mwait" :: "a"(hint), "c"(MWAIT_BREAK_ON_IRQ) : "memory");

	/* The interrupt that ended the wait is taken here, its handler run, before interrupts are masked again. */
	__asm__ volatile("sti; nop; cli" ::: "memory");

	/* The tick and the local sources as they were. */
	(void)amd64_lapic_timer_start();
	amd64_lapic_restore_sources(&saved);

	/* Succeeded: the wait ended. */
	return HAL_OK;
}

/*
 * Waits on the current CPU until one interrupt arrives: in the ordinary
 * idle's MWAIT C-state when the CPU has one, else halted.  Entered and left
 * with interrupts masked; the interrupt that ends the wait has been taken
 * when the call returns.
 */
void
bsp_cpu_idle(
	void)
{
	volatile uint8_t *line;

	/* Without the state, or without a TSC that counts through it, the CPU halts. */
	if (!idle_wait_ready || !amd64_timecounter_invariant()) {
		__asm__ volatile("sti; hlt; cli" ::: "memory");
		return;
	}

	/* MONITOR on this CPU's line, then MWAIT with interrupts still masked; an interrupt ends it. */
	line = idle_lines[hal_cpu_current()];
	__asm__ volatile("monitor" :: "a"(line), "c"(0U), "d"(0U) : "memory");
	__asm__ volatile("mwait" :: "a"(idle_wait_hint), "c"(MWAIT_BREAK_ON_IRQ) : "memory");

	/* The interrupt that ended the wait is taken here before interrupts are masked again. */
	__asm__ volatile("sti; nop; cli" ::: "memory");
}

/*
 * Chooses the ordinary idle's MWAIT hint: the deepest C-state up to
 * IDLE_WAIT_DEEPEST_CSTATE with sub-states, its deepest sub-state, when the
 * CPU has MONITOR/MWAIT, breaks out of it on a masked interrupt and keeps
 * its local APIC timer running (ARAT).  Without any, the idle halts.
 */
static void
idle_wait_init(
	void)
{
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	uint32_t cstate;
	uint32_t substates;

	/* MONITOR/MWAIT, the MWAIT leaf and the power leaf. */
	idle_wait_ready = 0U;
	idle_cpuid(CPUID_LEAF_FEATURES, &eax, &ebx, &ecx, &edx);
	if ((ecx & CPUID_FEATURE_MONITOR) == 0U) {
		hal_puts("A64 IDLE WAIT HLT reason=monitor\n");
		return;
	}
	idle_cpuid(0U, &eax, &ebx, &ecx, &edx);
	if (eax < CPUID_LEAF_POWER) {
		hal_puts("A64 IDLE WAIT HLT reason=leaf\n");
		return;
	}

	/* The break on a masked interrupt, and the timer through the C-states. */
	idle_cpuid(CPUID_LEAF_MWAIT, &eax, &ebx, &ecx, &edx);
	if ((ecx & CPUID_MWAIT_EXTENSIONS) == 0U || (ecx & CPUID_MWAIT_BREAK_ON_IRQ) == 0U) {
		hal_puts("A64 IDLE WAIT HLT reason=break\n");
		return;
	}
	idle_cpuid(CPUID_LEAF_POWER, &eax, &ebx, &ecx, &edx);
	if ((eax & CPUID_POWER_ARAT) == 0U) {
		hal_puts("A64 IDLE WAIT HLT reason=arat\n");
		return;
	}

	/* The deepest C-state the CPU has sub-states for, past C1. */
	idle_cpuid(CPUID_LEAF_MWAIT, &eax, &ebx, &ecx, &edx);
	for (cstate = IDLE_WAIT_DEEPEST_CSTATE; cstate >= 2U; cstate--) {
		substates = (edx >> (cstate * 4U)) & MWAIT_HINT_FIELD_MASK;
		if (substates != 0U)
			break;
	}
	if (cstate < 2U) {
		hal_puts("A64 IDLE WAIT HLT reason=cstates\n");
		return;
	}

	/* Succeeded: that C-state's deepest sub-state. */
	idle_wait_hint = ((cstate - 1U) << MWAIT_HINT_CSTATE_SHIFT) | (substates - 1U);
	idle_wait_ready = 1U;
	hal_printf("A64 IDLE WAIT MWAIT hint=%02X cstates=%08X\n", idle_wait_hint, edx);
}

/* Executes CPUID for one leaf (sub-leaf 0). */
static void
idle_cpuid(
	uint32_t leaf,
	uint32_t *eax,
	uint32_t *ebx,
	uint32_t *ecx,
	uint32_t *edx)
{
	/* The four registers of the leaf. */
	__asm__ volatile("cpuid" : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx) : "a"(leaf), "c"(0U));
}

/* Tells whether the CPU can enter an MWAIT hint's state and leave it on a masked interrupt. */
static unsigned
idle_hint_supported(
	uint32_t hint)
{
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	uint32_t cstate;
	uint32_t substate;
	uint32_t substates;

	/* MONITOR/MWAIT itself. */
	idle_cpuid(CPUID_LEAF_FEATURES, &eax, &ebx, &ecx, &edx);
	if ((ecx & CPUID_FEATURE_MONITOR) == 0U)
		return 0U;

	/* The MWAIT leaf must exist. */
	idle_cpuid(0U, &eax, &ebx, &ecx, &edx);
	if (eax < CPUID_LEAF_MWAIT)
		return 0U;

	/* The extensions, and breaking out on an interrupt while interrupts are masked. */
	idle_cpuid(CPUID_LEAF_MWAIT, &eax, &ebx, &ecx, &edx);
	if ((ecx & CPUID_MWAIT_EXTENSIONS) == 0U)
		return 0U;
	if ((ecx & CPUID_MWAIT_BREAK_ON_IRQ) == 0U)
		return 0U;

	/* The hint's C-state (EDX counts the sub-states from C0, the hint's field from C1). */
	cstate = ((hint >> MWAIT_HINT_CSTATE_SHIFT) & MWAIT_HINT_FIELD_MASK) + 1U;
	if (cstate >= MWAIT_CSTATES_IN_EDX)
		return 0U;

	/* The hint's sub-state must be one the CPU reports for that C-state. */
	substate = hint & MWAIT_HINT_FIELD_MASK;
	substates = (edx >> (cstate * 4U)) & MWAIT_HINT_FIELD_MASK;
	if (substate >= substates)
		return 0U;

	/* Succeeded: the state can be entered. */
	return 1U;
}
