/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The x86 processor driver's use of the model-specific registers: HWP,
 * the hardware feedback table, the energy counters and the idle
 * residency counters.
 *
 * A register that does not exist faults when it is touched, and the
 * kernel has no way back from that fault.  So the driver touches a
 * register only when CPUID says the processor has it (HWP, the feedback
 * interface, the delivered-performance counters) or when the model is on
 * the allow list of the registers its tables name (the platform
 * information, the energy and residency counters), never under a
 * hypervisor, and never when the boot parameter turned it off.  Nothing
 * is written at boot: HWP and the feedback interface start with a CPU's
 * first request.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/atomic.h>
#include <kern/cpuperf.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/pmem.h>
#include <kern/thermal.h>
#include <kern/types.h>
#include <uapi/errno.h>

#include "cpu-x86-insn.h"
#include "cpu-x86-private.h"
#include "cpux86.h"
#include "cpux86-cpuid.h"
#include "cpux86-hwp.h"
#include "cpux86-power.h"
#include "cpux86-pstate.h"

#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/* The leaf of the thermal and power features. */
#define CPU_X86_LEAF_POWER		0x06U

/* The most packages the driver keeps a feedback table for. */
#define CPU_X86_PACKAGE_MAX		8U

/*
 * The highest address a feedback table is placed at: below 4 GiB, which
 * every MAXPHYADDR reaches even when memory encryption takes some of its
 * bits.
 */
#define CPU_X86_FEEDBACK_ADDRESS_MAX	0xffffffffULL

/* How often a reader of a feedback copy tries before it gives up on a moving copy. */
#define CPU_X86_FEEDBACK_READ_TRIES	4U

/* The states of a package's feedback interface. */
#define CPU_X86_FEEDBACK_IDLE		0U	/* not enabled yet */
#define CPU_X86_FEEDBACK_CLAIMED	1U	/* a CPU of the package is enabling it */
#define CPU_X86_FEEDBACK_ON		2U	/* enabled; the hardware writes the table */
#define CPU_X86_FEEDBACK_REFUSED	3U	/* left alone for good */

/* The energy preference a request keeps when the old one cannot be read. */
#define CPU_X86_PREFERENCE_DEFAULT	0x80U

/* The largest energy preference. */
#define CPU_X86_PREFERENCE_MAX		255U

/* Units of the conversions. */
#define CPU_X86_HZ_PER_MHZ		1000000U
#define CPU_X86_NANOJOULES_PER_JOULE	1000000000U
#define CPU_X86_MODULE_RATE_HZ		1000000U

/*
 * One package's hardware feedback table and the copy the driver reads.
 *
 * The table is physical memory the hardware writes once the interface is
 * on and never moves; the copy is the driver's, rewritten by the feedback
 * notification on a CPU of the package and read by the threads.  sequence
 * is odd while the copy is being rewritten.  Slots are filled once after
 * the provider is registered and live for the kernel's life.
 */
struct cpu_x86_package {
	uint32_t package;		/* the topology's package number */
	struct kern_pmem memory;
	const uint8_t *table;
	uint8_t *copy;
	atomic_uint_t state;		/* CPU_X86_FEEDBACK_* */
	atomic_uint_t sequence;
	atomic_uint_t copied;		/* 1 once the copy holds a table */
};

/*
 * One energy counter the driver may offer as a meter: the register, the
 * allow-list bit that names it and what it measures.
 */
struct cpu_x86_meter {
	const char *name;
	uint32_t kind;			/* KERN_THERMAL_METER_* */
	uint32_t msr;
	uint32_t model_bit;		/* CPUX86_MODEL_* */
};

/*
 * The energy counters of the client RAPL hierarchy.
 *
 * Values referenced: Intel SDM Vol. 4 rev. 093, Table 2-20 (611H
 * MSR_PKG_ENERGY_STATUS), Table 2-39 (639H MSR_PP0_ENERGY_STATUS, the
 * cores; 64DH MSR_PLATFORM_ENERGY_COUNTER), Table 2-21 (641H
 * MSR_PP1_ENERGY_STATUS, a client uncore device, the graphics); Vol. 3B
 * rev. 093, §17.10.2 (the domains of a client processor).
 *
 * Never changed; it is not const only because each entry is the context
 * the thermal core hands back to the meter's read.
 */
static struct cpu_x86_meter cpu_x86_meters[] = {
	{ "package", KERN_THERMAL_METER_PACKAGE, CPUX86_MSR_PKG_ENERGY_STATUS, CPUX86_MODEL_PKG_ENERGY },
	{ "cores", KERN_THERMAL_METER_CORES, CPUX86_MSR_PP0_ENERGY_STATUS, CPUX86_MODEL_PP0_ENERGY },
	{ "graphics", KERN_THERMAL_METER_GRAPHICS, CPUX86_MSR_PP1_ENERGY_STATUS, CPUX86_MODEL_PP1_ENERGY },
	{ "platform", KERN_THERMAL_METER_PLATFORM, CPUX86_MSR_PLATFORM_ENERGY, CPUX86_MODEL_PLATFORM_ENERGY },
};

/* The residency counters a package reports, by the decoder's identifiers. */
static const uint32_t cpu_x86_package_residency[] = {
	CPUX86_RESIDENCY_PKG_C2,
	CPUX86_RESIDENCY_PKG_C3,
	CPUX86_RESIDENCY_PKG_C6,
	CPUX86_RESIDENCY_PKG_C7,
};

/* The residency counters a core reports, by the decoder's identifiers. */
static const uint32_t cpu_x86_core_residency[] = {
	CPUX86_RESIDENCY_CORE_C3,
	CPUX86_RESIDENCY_CORE_C6,
	CPUX86_RESIDENCY_CORE_C7,
};

/*
 * The packages the driver keeps a feedback table for, filled by
 * drv_cpu_x86_control_attach() and fixed after that; a CPU names its slot
 * by its package_index.
 */
static struct cpu_x86_package cpu_x86_packages[CPU_X86_PACKAGE_MAX];

/* The number of filled slots of cpu_x86_packages. */
static uint32_t cpu_x86_package_count;

/*
 * The layout of every package's feedback table, worked out once from the
 * leaf 06H of the CPU that ran the probe; valid when the machine's
 * feedback gate is open.
 */
static struct cpux86_feedback_layout cpu_x86_layout;

/*
 * Whether the processor can run Thread Director, which the configuration
 * value must be told even though the driver enables only the plain
 * feedback.
 */
static uint32_t cpu_x86_thread_director;

/*
 * The meter operations every energy counter shares: the counter width and
 * the energy unit, filled once by drv_cpu_x86_control_attach() before the
 * first meter is registered.  The thermal core keeps the pointer.
 */
static struct kern_thermal_meter_ops cpu_x86_meter_ops;

static int cpu_x86_start(kern_cpu_id_t cpu, struct kern_cpuperf_sample *sample);
static int cpu_x86_apply(kern_cpu_id_t cpu, const struct kern_cpuperf_request *request);
static void cpu_x86_sample(kern_cpu_id_t cpu, struct kern_cpuperf_sample *sample);
static int cpu_x86_measure(kern_cpu_id_t cpu, bool begin, uint32_t *mhz);
static int cpu_x86_relative(kern_cpu_id_t cpu, uint32_t source, uint32_t *value);
static int cpu_x86_feedback(kern_cpu_id_t cpu, bool *changed);
static int cpu_x86_feedback_read(kern_cpu_id_t cpu, uint32_t *available, uint32_t *efficiency);
static int cpu_x86_idle_package(kern_cpu_id_t cpu, struct kern_cpuperf_idle *idle);
static int cpu_x86_idle_core(kern_cpu_id_t cpu, struct kern_cpuperf_idle *idle);
static int cpu_x86_meter_read(void *context, uint64_t *count);
static void cpu_x86_probe_hwp(const struct cpux86_power *power, bool hwp_off);
static void cpu_x86_probe_feedback(const struct cpux86_power *power);
static void cpu_x86_probe_platform(void);
static int cpu_x86_read_capabilities(struct cpu_x86_cpu *state);
static void cpu_x86_feedback_enable(struct cpu_x86_cpu *state);
static int cpu_x86_feedback_cells(kern_cpu_id_t cpu, uint32_t *performance, uint32_t *efficiency);
static uint32_t cpu_x86_rate_mhz(uint64_t aperf, uint64_t mperf);
static bool cpu_x86_has_residency(const uint32_t *counters, uint32_t count);
static int cpu_x86_read_residency(const uint32_t *counters, uint32_t count, struct kern_cpuperf_idle *idle);
static void cpu_x86_attach_packages(void);
static void cpu_x86_attach_tables(void);
static void cpu_x86_attach_meters(void);

/*
 * Decides which model-specific registers the driver may touch.
 *
 * Called only on bare metal with the driver on: reads the CPU's leaf 06H
 * and the allow list, then, where they let it, the registers that are
 * only read at boot (HWP's enable bit and the platform information).
 */
void
drv_cpu_x86_control_probe(
	bool hwp_off)
{
	struct cpux86_cpuid_regs regs;
	struct cpux86_power power;
	struct cpu_x86_machine *machine;
	int present;
	int error;

	machine = &drv_cpu_x86_machine;

	/* Reads which registers the allow list names for this model; another model has none. */
	machine->model_registers = 0U;
	error = cpux86_model_registers(&machine->identity, &machine->model_registers);
	if (error != 0)
		machine->model_registers = 0U;

	/* Reads the frequency the TSC-rate counters run at, where the model has the register. */
	cpu_x86_probe_platform();

	/* Reads the thermal and power features; without the leaf nothing else is used. */
	present = cpux86_cpuid_has_leaf(&machine->identity, CPU_X86_LEAF_POWER);
	if (!present)
		return;
	drv_cpu_x86_cpuid(CPU_X86_LEAF_POWER, 0U, &regs);
	error = cpux86_cpuid_power(&regs, &power);
	if (error != 0)
		return;

	/* Notes the delivered-performance counters. */
	if ((power.counters & CPUX86_POWER_APERF_MPERF) != 0U)
		machine->counters = true;

	/* Decides on HWP and on the feedback interface. */
	cpu_x86_probe_hwp(&power, hwp_off);
	cpu_x86_probe_feedback(&power);
}

/*
 * Reads what a CPU's control has before its first request: when HWP is
 * already on, the processor reports its capabilities at once.
 */
void
drv_cpu_x86_control_cpu(
	struct cpu_x86_cpu *state)
{
	uint64_t raw;
	uint32_t enabled;
	int error;

	/* Reads nothing on a CPU without HWP control. */
	if (!drv_cpu_x86_machine.hwp)
		return;
	if ((state->power_features & CPUX86_POWER_HWP) == 0U)
		return;

	/*
	 * Reads the enable bit, which CPUID's HWP bit lets the driver read
	 * before HWP is on; the other HWP registers fault until then.
	 *
	 * Procedure referenced: Intel SDM Vol. 3B rev. 093, §17.4.2.
	 */
	raw = drv_cpu_x86_rdmsr(CPUX86_MSR_PM_ENABLE);
	error = cpux86_hwp_enabled(raw, &enabled);
	if (error != 0 || enabled == 0U)
		return;
	state->hwp_enabled = true;

	/* Keeps the capabilities the already running HWP reports. */
	(void)cpu_x86_read_capabilities(state);
}

/*
 * Fills the operations of the provider that touch the registers, by the
 * gates the probe opened.
 */
void
drv_cpu_x86_control_ops(
	struct kern_cpuperf_provider_ops *ops)
{
	const struct cpu_x86_machine *machine;
	bool residency;

	machine = &drv_cpu_x86_machine;

	/* Controls the CPUs through HWP. */
	if (machine->hwp) {
		ops->start = cpu_x86_start;
		ops->apply = cpu_x86_apply;
	}

	/* Samples the capabilities and the delivered rate where either can be read. */
	if (machine->hwp || (machine->counters && machine->base_mhz != 0U))
		ops->sample = cpu_x86_sample;

	/* Measures the highest rate from the delivered-performance counters. */
	if (machine->hwp && machine->counters && machine->base_mhz != 0U)
		ops->measure = cpu_x86_measure;

	/* Copies the hardware feedback and offers it as a relative performance. */
	if (machine->feedback) {
		ops->feedback = cpu_x86_feedback;
		ops->feedback_read = cpu_x86_feedback_read;
		ops->relative_sources = KERN_CPUPERF_RELATIVE_FEEDBACK;
		ops->relative = cpu_x86_relative;
	}

	/* Reads the idle residency where the model has the counters and their rate is known. */
	if (machine->tsc_hz == 0U)
		return;
	residency = cpu_x86_has_residency(cpu_x86_package_residency,
					  sizeof(cpu_x86_package_residency) / sizeof(cpu_x86_package_residency[0]));
	if (residency)
		ops->idle_package = cpu_x86_idle_package;
	residency = cpu_x86_has_residency(cpu_x86_core_residency,
					  sizeof(cpu_x86_core_residency) / sizeof(cpu_x86_core_residency[0]));
	if (residency)
		ops->idle_core = cpu_x86_idle_core;
}

/*
 * Prepares what needs the CPUs' places: the feedback tables of the
 * packages and the energy meters, and reports what the driver offers.
 */
void
drv_cpu_x86_control_attach(void)
{
	const struct cpu_x86_machine *machine;

	machine = &drv_cpu_x86_machine;

	/* Gives every CPU its package's slot. */
	cpu_x86_attach_packages();

	/* Allocates a feedback table per package. */
	if (machine->feedback)
		cpu_x86_attach_tables();

	/* Offers the energy counters as meters. */
	cpu_x86_attach_meters();

	/* Reports what the CPUs offer. */
	kern_logf("cpu-x86: family %02xH model %02xH: HWP %s, feedback %s, base %u MHz, %u package%s\n",
		  machine->identity.display_family,
		  machine->identity.display_model,
		  machine->hwp ? "on first request" : "off",
		  machine->feedback ? "on first request" : "off",
		  machine->base_mhz,
		  cpu_x86_package_count,
		  cpu_x86_package_count == 1U ? "" : "s");
}

/*
 * Starts HWP on a CPU before its first request and reports its
 * capabilities.
 *
 * Runs on the CPU with interrupts masked.  Enables HWP for the package
 * unless it is on, makes this CPU's own request register the one in
 * force, silences HWP's interrupt, reads the capabilities and, on the
 * first CPU of its package, turns the feedback interface on.
 *
 * Procedure referenced: Intel SDM Vol. 3B rev. 093, §17.4.2 (enable
 * first, then the other HWP registers), §17.4.4.4 and Table 17-2 (the
 * request register in force), §17.4.6 (IA32_HWP_INTERRUPT).
 */
static int
cpu_x86_start(
	kern_cpu_id_t cpu,
	struct kern_cpuperf_sample *sample)
{
	struct cpu_x86_cpu *state;
	uint64_t raw;
	uint64_t control;
	uint64_t package_bit;
	uint32_t enabled;
	int error;

	/* Refuses a CPU without HWP. */
	if (cpu >= drv_cpu_x86_cpu_count)
		return EINVAL;
	state = &drv_cpu_x86_cpus[cpu];
	if ((state->power_features & CPUX86_POWER_HWP) == 0U)
		return EOPNOTSUPP;

	/*
	 * Reads whether HWP is on already: the firmware, or another CPU of
	 * the package, may have turned it on.
	 */
	raw = drv_cpu_x86_rdmsr(CPUX86_MSR_PM_ENABLE);
	error = cpux86_hwp_enabled(raw, &enabled);
	if (error != 0)
		return EIO;

	/* Turns HWP on for the package and confirms it; only a reset turns it off. */
	if (enabled == 0U) {
		drv_cpu_x86_wrmsr(CPUX86_MSR_PM_ENABLE, CPUX86_PM_ENABLE_HWP);
		raw = drv_cpu_x86_rdmsr(CPUX86_MSR_PM_ENABLE);
		error = cpux86_hwp_enabled(raw, &enabled);
		if (error != 0 || enabled == 0U)
			return EIO;
	}
	state->hwp_enabled = true;

	/*
	 * Makes this CPU's own request register the one in force: with
	 * IA32_HWP_CTL bit 0 clear and the request's bit 42 written as 0,
	 * the package request is not used.
	 */
	if ((state->power_features & CPUX86_POWER_HWP_CTL) != 0U) {
		control = drv_cpu_x86_rdmsr(CPUX86_MSR_HWP_CTL);
		package_bit = cpux86_field64(control, 0U, 1U);
		if (package_bit != 0U) {
			control = cpux86_insert64(control, 0U, 1U, 0U);
			drv_cpu_x86_wrmsr(CPUX86_MSR_HWP_CTL, control);
		}
	}

	/* Silences HWP's interrupt: the framework reads the changes when it samples. */
	if ((state->power_features & CPUX86_POWER_HWP_INTERRUPT) != 0U)
		drv_cpu_x86_wrmsr(CPUX86_MSR_HWP_INTERRUPT, 0U);

	/* Reads the capabilities, which exist now that HWP is on. */
	error = cpu_x86_read_capabilities(state);
	if (error != 0)
		return EIO;

	/* Hands the anchors to the framework. */
	sample->flags |= KERN_CPUPERF_SAMPLE_CAPABILITIES;
	sample->lowest = state->capabilities.lowest;
	sample->efficient = state->capabilities.efficient;
	sample->guaranteed = state->capabilities.guaranteed;
	sample->highest = state->capabilities.highest;

	/* Turns the feedback interface on for the package, once. */
	cpu_x86_feedback_enable(state);

	/* Succeeded: HWP takes this CPU's requests. */
	return 0;
}

/*
 * Writes a request to a CPU's HWP request register.
 *
 * Runs on the CPU with interrupts masked, after its start.  The range is
 * checked against the capabilities the CPU reported just before; window,
 * package control and the valid bits are written as 0, so the hardware
 * picks the window and this register alone is in force.
 *
 * Procedure referenced: Intel SDM Vol. 3B rev. 093, §17.4.4.1.
 */
static int
cpu_x86_apply(
	kern_cpu_id_t cpu,
	const struct kern_cpuperf_request *request)
{
	struct cpux86_hwp_request current;
	struct cpux86_hwp_request next;
	struct cpu_x86_cpu *state;
	uint64_t raw;
	uint32_t preference;
	int error;

	/* Refuses a CPU whose HWP has not started. */
	if (cpu >= drv_cpu_x86_cpu_count)
		return EINVAL;
	state = &drv_cpu_x86_cpus[cpu];
	if (!state->hwp_enabled || !state->capabilities_known)
		return EIO;

	/* Reads the request in force, for the preference a request may leave as it is. */
	raw = drv_cpu_x86_rdmsr(CPUX86_MSR_HWP_REQUEST);
	error = cpux86_hwp_request_decode(raw, &current);
	if (error != 0)
		current.preference = CPU_X86_PREFERENCE_DEFAULT;

	/*
	 * Picks the energy preference: none on a processor without the
	 * field (its bits must stay 0), the old one when the request leaves
	 * it, otherwise the request's.
	 */
	if ((state->power_features & CPUX86_POWER_HWP_EPP) == 0U) {
		preference = 0U;
	} else if (request->preference == KERN_CPUPERF_UNKNOWN) {
		preference = current.preference;
	} else if (request->preference > CPU_X86_PREFERENCE_MAX) {
		return EINVAL;
	} else {
		preference = request->preference;
	}

	/* Builds the new request. */
	next.minimum = request->minimum;
	next.maximum = request->maximum;
	next.desired = request->desired;
	next.preference = preference;
	next.window = 0U;
	next.package_control = 0U;
	next.valid = 0U;

	/* Refuses a request outside the capabilities. */
	error = cpux86_hwp_request_encode(&next, &state->capabilities, &raw);
	if (error != 0)
		return EINVAL;

	/* Writes it. */
	drv_cpu_x86_wrmsr(CPUX86_MSR_HWP_REQUEST, raw);

	/* Succeeded: the CPU runs with the request. */
	return 0;
}

/*
 * Samples a CPU: its capabilities now, whether the platform held it below
 * its request, and the rate it delivered since its previous sample.
 *
 * Runs on the CPU with interrupts masked.  The status events are
 * acknowledged here, since the driver uses no HWP interrupt.
 *
 * Procedure referenced: Intel SDM Vol. 3B rev. 093, §17.4.5 (the status
 * events and their acknowledgement), §17.2 and §22.7.2 (the delivered
 * rate as IA32_APERF over IA32_MPERF times the maximum non-turbo
 * frequency).
 */
static void
cpu_x86_sample(
	kern_cpu_id_t cpu,
	struct kern_cpuperf_sample *sample)
{
	struct cpu_x86_cpu *state;
	uint64_t raw;
	uint64_t ack;
	uint64_t aperf;
	uint64_t mperf;
	uint32_t events;
	int error;

	/* Refuses a CPU the driver has no record for. */
	sample->delivered_mhz = 0U;
	if (cpu >= drv_cpu_x86_cpu_count)
		return;
	state = &drv_cpu_x86_cpus[cpu];

	/* Reads the capabilities and the events of a CPU under HWP. */
	if (state->hwp_enabled) {
		error = cpu_x86_read_capabilities(state);
		if (error == 0) {
			sample->flags |= KERN_CPUPERF_SAMPLE_CAPABILITIES;
			sample->lowest = state->capabilities.lowest;
			sample->efficient = state->capabilities.efficient;
			sample->guaranteed = state->capabilities.guaranteed;
			sample->highest = state->capabilities.highest;
		}

		/* Notes an excursion below the minimum and acknowledges every event seen. */
		raw = drv_cpu_x86_rdmsr(CPUX86_MSR_HWP_STATUS);
		error = cpux86_hwp_status_decode(raw, &events);
		if (error == 0 && events != 0U) {
			if ((events & CPUX86_HWP_STATUS_EXCURSION) != 0U)
				sample->flags |= KERN_CPUPERF_SAMPLE_PLATFORM_LIMIT;
			error = cpux86_hwp_status_ack(events, &ack);
			if (error == 0)
				drv_cpu_x86_wrmsr(CPUX86_MSR_HWP_STATUS, ack);
		}
	}

	/* Reads the delivered rate where the counters and their reference are known. */
	if (!drv_cpu_x86_machine.counters || drv_cpu_x86_machine.base_mhz == 0U)
		return;
	mperf = drv_cpu_x86_rdmsr(CPUX86_MSR_MPERF);
	aperf = drv_cpu_x86_rdmsr(CPUX86_MSR_APERF);
	if (state->sample_marked && mperf > state->sample_mperf && aperf >= state->sample_aperf)
		sample->delivered_mhz = cpu_x86_rate_mhz(aperf - state->sample_aperf, mperf - state->sample_mperf);

	/* Keeps the counters for the next sample. */
	state->sample_aperf = aperf;
	state->sample_mperf = mperf;
	state->sample_marked = true;
}

/*
 * Measures a CPU's delivered rate between two calls, from the
 * delivered-performance counters.
 *
 * Runs on the CPU with interrupts masked; the framework keeps the CPU
 * busy at its highest level between the mark and the end.
 */
static int
cpu_x86_measure(
	kern_cpu_id_t cpu,
	bool begin,
	uint32_t *mhz)
{
	struct cpu_x86_cpu *state;
	uint64_t aperf;
	uint64_t mperf;
	uint32_t rate;

	/* Refuses a CPU the driver has no record for. */
	if (cpu >= drv_cpu_x86_cpu_count)
		return EINVAL;
	state = &drv_cpu_x86_cpus[cpu];

	/* Reads the counters, the reference one first. */
	mperf = drv_cpu_x86_rdmsr(CPUX86_MSR_MPERF);
	aperf = drv_cpu_x86_rdmsr(CPUX86_MSR_APERF);

	/* Marks the start. */
	if (begin) {
		state->measure_aperf = aperf;
		state->measure_mperf = mperf;
		state->measure_marked = true;
		return 0;
	}

	/* Refuses an end without a start, or counters that were reset between. */
	if (!state->measure_marked)
		return ENODATA;
	state->measure_marked = false;
	if (mperf <= state->measure_mperf || aperf < state->measure_aperf)
		return ENODATA;

	/* Works the rate out from the two advances. */
	rate = cpu_x86_rate_mhz(aperf - state->measure_aperf, mperf - state->measure_mperf);
	if (rate == 0U)
		return ENODATA;

	/* Succeeded: the rate since the mark. */
	*mhz = rate;
	return 0;
}

/*
 * Reports a CPU's raw relative performance by the hardware feedback: its
 * performance cell of class 0.
 */
static int
cpu_x86_relative(
	kern_cpu_id_t cpu,
	uint32_t source,
	uint32_t *value)
{
	uint32_t performance;
	uint32_t efficiency;
	int error;

	/* Offers only the hardware feedback. */
	if (source != KERN_CPUPERF_RELATIVE_FEEDBACK)
		return EOPNOTSUPP;

	/* Reads the CPU's cells from its package's copy. */
	error = cpu_x86_feedback_cells(cpu, &performance, &efficiency);
	if (error != 0)
		return error;

	/* Succeeded: the performance cell. */
	*value = performance;
	return 0;
}

/*
 * Copies a package's feedback table when the hardware has updated it,
 * then hands the table back to the hardware.
 *
 * Runs on a CPU of the package with interrupts masked.  Until the update
 * bit is cleared the hardware does not write the table again, so the
 * copy is taken first.
 *
 * Procedure referenced: Intel SDM Vol. 3B rev. 093, §17.6.6 and §17.6.7.
 */
static int
cpu_x86_feedback(
	kern_cpu_id_t cpu,
	bool *changed)
{
	struct cpu_x86_package *package;
	struct cpu_x86_cpu *state;
	uint64_t raw;
	uint64_t ack;
	uint32_t updated;
	uint32_t index;
	unsigned feedback_state;
	int error;

	/* Finds the CPU's package. */
	*changed = false;
	if (cpu >= drv_cpu_x86_cpu_count)
		return EINVAL;
	state = &drv_cpu_x86_cpus[cpu];
	index = state->package_index;
	if (index >= cpu_x86_package_count)
		return ENODATA;
	package = &cpu_x86_packages[index];

	/* Reads nothing until the package's interface is on. */
	feedback_state = atomic_load_acquire(&package->state);
	if (feedback_state != CPU_X86_FEEDBACK_ON)
		return ENODATA;

	/* Leaves the table alone while the hardware has not updated it. */
	raw = drv_cpu_x86_rdmsr(CPUX86_MSR_PACKAGE_THERM_STATUS);
	error = cpux86_feedback_updated(raw, &updated);
	if (error != 0)
		return EIO;
	if (updated == 0U)
		return 0;

	/*
	 * Copies the table between two moves of the sequence, which leave it
	 * odd while the copy is rewritten.  The locked increment orders the
	 * first move before the copy on this processor.
	 */
	(void)atomic_fetch_add_relaxed(&package->sequence, 1U);
	kern_memcpy(package->copy, package->table, cpu_x86_layout.table_size);
	(void)atomic_raw_fetch_add_release(&package->sequence.value, 1U);
	atomic_store_release(&package->copied, 1U);

	/* Hands the table back to the hardware. */
	error = cpux86_feedback_ack(raw, &ack);
	if (error != 0)
		return EIO;
	drv_cpu_x86_wrmsr(CPUX86_MSR_PACKAGE_THERM_STATUS, ack);

	/* Succeeded: a new copy is in place. */
	*changed = true;
	return 0;
}

/* Reads a CPU's values from its package's last feedback copy. */
static int
cpu_x86_feedback_read(
	kern_cpu_id_t cpu,
	uint32_t *available,
	uint32_t *efficiency)
{
	uint32_t performance;
	uint32_t energy;
	int error;

	/* Reads the CPU's cells. */
	error = cpu_x86_feedback_cells(cpu, &performance, &energy);
	if (error != 0)
		return error;

	/* Succeeded: the performance and efficiency cells of class 0. */
	*available = performance;
	*efficiency = energy;
	return 0;
}

/* Reads the residency of the CPU's package, on a CPU of that package. */
static int
cpu_x86_idle_package(
	kern_cpu_id_t cpu,
	struct kern_cpuperf_idle *idle)
{
	uint32_t count;
	int error;

	UNUSED_PARAMETER(cpu);

	/* Reads every package counter the model has. */
	count = sizeof(cpu_x86_package_residency) / sizeof(cpu_x86_package_residency[0]);
	error = cpu_x86_read_residency(cpu_x86_package_residency, count, idle);
	if (error != 0)
		return error;

	/* Succeeded: the package's states and their times. */
	return 0;
}

/* Reads the residency of the CPU's core, on that CPU. */
static int
cpu_x86_idle_core(
	kern_cpu_id_t cpu,
	struct kern_cpuperf_idle *idle)
{
	const struct cpu_x86_cpu *state;
	uint32_t count;
	uint32_t index;
	int error;

	/* Refuses a CPU the driver has no record for. */
	if (cpu >= drv_cpu_x86_cpu_count)
		return EINVAL;
	state = &drv_cpu_x86_cpus[cpu];

	/* Reads every core counter the model has. */
	count = sizeof(cpu_x86_core_residency) / sizeof(cpu_x86_core_residency[0]);
	error = cpu_x86_read_residency(cpu_x86_core_residency, count, idle);
	if (error != 0)
		return error;

	/* Offers the C-states MWAIT can request on this CPU, when it says. */
	if (state->mwait_known) {
		idle->available = 0U;
		for (index = 1; index < CPUX86_MWAIT_CSTATE_COUNT; index++) {
			if (state->mwait.substates[index] != 0U)
				idle->available |= 1U << index;
		}
	}

	/* Succeeded: the core's states and their times. */
	return 0;
}

/* Reads one energy counter for the thermal core's poll thread. */
static int
cpu_x86_meter_read(
	void *context,
	uint64_t *count)
{
	const struct cpu_x86_meter *meter;
	uint64_t raw;
	uint32_t energy;
	int error;

	/* Reads the counter; any CPU of the one package reads the same. */
	meter = context;
	raw = drv_cpu_x86_rdmsr(meter->msr);
	error = cpux86_energy_status_decode(raw, &energy);
	if (error != 0)
		return EIO;

	/* Succeeded: the raw count in the processor's energy units. */
	*count = energy;
	return 0;
}

/*
 * Decides whether HWP controls the CPUs.
 *
 * HWP is used when CPUID reports it, unless cpuperf.hwp=off asks
 * otherwise; an HWP the firmware or an earlier system turned on cannot be
 * turned off before a reset, so the driver then uses it anyway.
 */
static void
cpu_x86_probe_hwp(
	const struct cpux86_power *power,
	bool hwp_off)
{
	struct cpu_x86_machine *machine;
	uint64_t raw;
	uint32_t enabled;
	int error;

	machine = &drv_cpu_x86_machine;

	/* Leaves a processor without HWP uncontrolled. */
	machine->hwp = false;
	if ((power->features & CPUX86_POWER_HWP) == 0U) {
		kern_logf("cpu-x86: no HWP; the CPUs are described, not controlled\n");
		return;
	}

	/* Reads whether HWP is on already. */
	raw = drv_cpu_x86_rdmsr(CPUX86_MSR_PM_ENABLE);
	error = cpux86_hwp_enabled(raw, &enabled);
	if (error != 0) {
		kern_logf("cpu-x86: IA32_PM_ENABLE reads 0x%llx; HWP is left alone\n", (unsigned long long)raw);
		return;
	}
	machine->hwp_enabled_at_boot = enabled != 0U;

	/* Honours cpuperf.hwp=off where HWP is still off. */
	if (hwp_off && !machine->hwp_enabled_at_boot) {
		kern_logf("cpu-x86: HWP turned off by cpuperf.hwp=off; the CPUs are described, not controlled\n");
		return;
	}
	if (hwp_off)
		kern_logf("cpu-x86: HWP was on before the kernel started; cpuperf.hwp=off cannot turn it off\n");

	/* Succeeded: HWP controls the CPUs. */
	machine->hwp = true;
}

/*
 * Decides whether the hardware feedback interface is used: only with HWP
 * control, where the processor has the interface and the package thermal
 * status that announces its updates, and its table has a layout the
 * decoder accepts.
 */
static void
cpu_x86_probe_feedback(
	const struct cpux86_power *power)
{
	struct cpu_x86_machine *machine;
	int error;

	machine = &drv_cpu_x86_machine;

	/* Leaves the interface off without HWP control or one of its registers. */
	machine->feedback = false;
	if (!machine->hwp)
		return;
	if ((power->features & CPUX86_POWER_HW_FEEDBACK) == 0U)
		return;
	if ((power->features & CPUX86_POWER_PKG_THERM) == 0U)
		return;
	if (machine->extended.physical_address_bits == 0U)
		return;

	/* Works out the table's layout. */
	error = cpux86_feedback_layout(power, &cpu_x86_layout);
	if (error != 0) {
		kern_logf("cpu-x86: the hardware feedback table has no usable layout (%d)\n", error);
		return;
	}

	/* Notes whether the processor could run Thread Director, which the configuration names. */
	cpu_x86_thread_director = 0U;
	if ((power->features & CPUX86_POWER_THREAD_DIRECTOR) != 0U)
		cpu_x86_thread_director = 1U;

	/* Succeeded: the interface starts with the first request. */
	machine->feedback = true;
}

/*
 * Reads the maximum non-turbo frequency, which is also the rate of the
 * time-stamp counter and of IA32_MPERF, on a model whose allow list names
 * the platform information register.
 *
 * Procedure referenced: Intel SDM Vol. 3B rev. 093, §22.7.2 (the TSC and
 * IA32_MPERF run close to the maximum non-turbo frequency).
 */
static void
cpu_x86_probe_platform(void)
{
	struct cpux86_platform_info info;
	struct cpu_x86_machine *machine;
	uint64_t raw;
	uint32_t mhz;
	int error;

	machine = &drv_cpu_x86_machine;

	/* Knows no frequency off the allow list. */
	machine->base_mhz = 0U;
	machine->tsc_hz = 0U;
	if ((machine->model_registers & CPUX86_MODEL_PLATFORM_INFO) == 0U)
		return;

	/* Reads and decodes the register. */
	raw = drv_cpu_x86_rdmsr(CPUX86_MSR_PLATFORM_INFO);
	error = cpux86_platform_info_decode(raw, &info);
	if (error != 0)
		return;
	error = cpux86_ratio_mhz(info.max_non_turbo_ratio, &mhz);
	if (error != 0)
		return;

	/* Succeeded: the frequency, and the rate in hertz of the counters that follow it. */
	machine->base_mhz = mhz;
	machine->tsc_hz = (uint64_t)mhz * CPU_X86_HZ_PER_MHZ;
}

/* Reads and keeps a CPU's HWP capabilities.  Only once HWP is on. */
static int
cpu_x86_read_capabilities(
	struct cpu_x86_cpu *state)
{
	struct cpux86_hwp_capabilities capabilities;
	uint64_t raw;
	int error;

	/* Reads and decodes the register. */
	raw = drv_cpu_x86_rdmsr(CPUX86_MSR_HWP_CAPABILITIES);
	error = cpux86_hwp_capabilities_decode(raw, &capabilities);
	if (error != 0)
		return error;

	/* Succeeded: the capabilities are kept for the requests. */
	state->capabilities = capabilities;
	state->capabilities_known = true;
	return 0;
}

/*
 * Turns the feedback interface on for a CPU's package, on the first CPU
 * of the package that starts.  An interface that some earlier software
 * left on is left alone, since its table is not the driver's.
 *
 * Procedure referenced: Intel SDM Vol. 3B rev. 093, §17.6.4 and §17.6.5
 * (the pointer first, then the enable bit).
 */
static void
cpu_x86_feedback_enable(
	struct cpu_x86_cpu *state)
{
	struct cpu_x86_package *package;
	uint64_t pointer;
	uint64_t config;
	uint64_t enable_bit;
	unsigned expected;
	uint32_t index;
	int claimed;
	int error;

	/* Finds the package's slot. */
	if (!drv_cpu_x86_machine.feedback)
		return;
	index = state->package_index;
	if (index >= cpu_x86_package_count)
		return;
	package = &cpu_x86_packages[index];

	/* Claims the package, so one CPU enables it. */
	expected = CPU_X86_FEEDBACK_IDLE;
	claimed = atomic_compare_exchange(&package->state, &expected, CPU_X86_FEEDBACK_CLAIMED);
	if (!claimed)
		return;

	/* Leaves a package without a table, or with an interface already on. */
	if (package->copy == NULL) {
		atomic_store_release(&package->state, CPU_X86_FEEDBACK_REFUSED);
		return;
	}
	config = drv_cpu_x86_rdmsr(CPUX86_MSR_HW_FEEDBACK_CONFIG);
	enable_bit = cpux86_field64(config, 0U, 1U);
	if (enable_bit != 0U) {
		atomic_store_release(&package->state, CPU_X86_FEEDBACK_REFUSED);
		return;
	}

	/* Builds the two values. */
	error = cpux86_feedback_pointer_encode(package->memory.paddr,
					       drv_cpu_x86_machine.extended.physical_address_bits,
					       &pointer);
	if (error == 0)
		error = cpux86_feedback_config_encode(1U, 0U, cpu_x86_thread_director, &config);
	if (error != 0) {
		atomic_store_release(&package->state, CPU_X86_FEEDBACK_REFUSED);
		return;
	}

	/* Points the package at its table, then enables the plain feedback. */
	drv_cpu_x86_wrmsr(CPUX86_MSR_HW_FEEDBACK_PTR, pointer);
	drv_cpu_x86_wrmsr(CPUX86_MSR_HW_FEEDBACK_CONFIG, config);

	/* The hardware now owns the table; the feedback notifications may copy it. */
	atomic_store_release(&package->state, CPU_X86_FEEDBACK_ON);
}

/*
 * Reads a CPU's performance and efficiency cells of class 0 from its
 * package's copy, retrying while the copy moves.
 */
static int
cpu_x86_feedback_cells(
	kern_cpu_id_t cpu,
	uint32_t *performance,
	uint32_t *efficiency)
{
	const struct cpu_x86_package *package;
	const struct cpu_x86_cpu *state;
	unsigned before;
	unsigned after;
	unsigned copied;
	uint32_t index;
	uint32_t tries;
	int error;

	/* Finds the CPU's package and refuses one without a copy yet. */
	if (cpu >= drv_cpu_x86_cpu_count)
		return EINVAL;
	state = &drv_cpu_x86_cpus[cpu];
	index = state->package_index;
	if (index >= cpu_x86_package_count)
		return ENODATA;
	package = &cpu_x86_packages[index];
	copied = atomic_load_acquire(&package->copied);
	if (copied == 0U)
		return ENODATA;

	/* Reads the two cells between two equal, even values of the sequence. */
	for (tries = 0; tries < CPU_X86_FEEDBACK_READ_TRIES; tries++) {
		before = atomic_load_acquire(&package->sequence);
		if ((before & 1U) != 0U)
			continue;
		error = cpux86_feedback_cell(package->copy, cpu_x86_layout.table_size, &cpu_x86_layout,
					     state->feedback_row, 0U, CPUX86_FEEDBACK_CAP_PERFORMANCE, performance);
		if (error == 0) {
			error = cpux86_feedback_cell(package->copy, cpu_x86_layout.table_size, &cpu_x86_layout,
						     state->feedback_row, 0U, CPUX86_FEEDBACK_CAP_EFFICIENCY, efficiency);
		}
		if (error != 0)
			return error;
		atomic_acquire_fence();
		after = atomic_load_acquire(&package->sequence);
		if (after == before)
			return 0;
	}

	/* The copy kept moving: the caller tries again on the next poll. */
	return EAGAIN;
}

/*
 * Works a delivered rate out from the advances of IA32_APERF and
 * IA32_MPERF: the maximum non-turbo frequency times their ratio.  Both
 * advances are halved together until the product fits 64 bits.
 */
static uint32_t
cpu_x86_rate_mhz(
	uint64_t aperf,
	uint64_t mperf)
{
	uint64_t base;
	uint64_t rate;

	/* Scales the two advances down together until the product cannot overflow. */
	base = drv_cpu_x86_machine.base_mhz;
	while (base != 0U && aperf > UINT64_MAX / base) {
		aperf >>= 1;
		mperf >>= 1;
	}

	/* Refuses a reference that did not advance. */
	if (base == 0U || mperf == 0U)
		return 0U;

	/* Refuses a rate the report cannot hold. */
	rate = aperf * base / mperf;
	if (rate > UINT32_MAX)
		return 0U;

	/* Succeeded: the rate in MHz. */
	return (uint32_t)rate;
}

/* Reports whether the model has any of the listed residency counters. */
static bool
cpu_x86_has_residency(
	const uint32_t *counters,
	uint32_t count)
{
	struct cpux86_residency_info info;
	uint32_t index;
	int error;

	/* Looks for one counter the allow list names. */
	for (index = 0; index < count; index++) {
		error = cpux86_residency_info(counters[index], &info);
		if (error != 0)
			continue;
		if ((drv_cpu_x86_machine.model_registers & info.model_bit) != 0U)
			return true;
	}

	/* The model has none. */
	return false;
}

/*
 * Reads the listed residency counters the model has, on this CPU, and
 * turns their counts into nanoseconds.
 */
static int
cpu_x86_read_residency(
	const uint32_t *counters,
	uint32_t count,
	struct kern_cpuperf_idle *idle)
{
	struct cpux86_residency_info info;
	uint64_t nanoseconds;
	uint64_t rate;
	uint64_t raw;
	uint32_t index;
	uint32_t slot;
	int error;

	/* Starts an empty report. */
	idle->count = 0U;
	idle->available = 0U;

	/* Reads each counter the allow list names. */
	for (index = 0; index < count; index++) {
		error = cpux86_residency_info(counters[index], &info);
		if (error != 0)
			continue;
		if ((drv_cpu_x86_machine.model_registers & info.model_bit) == 0U)
			continue;
		if (idle->count >= KERN_CPUPERF_IDLE_STATE_MAX)
			break;

		/* Converts the count by the counter's own rate. */
		rate = drv_cpu_x86_machine.tsc_hz;
		if (info.rate == CPUX86_RATE_1MHZ)
			rate = CPU_X86_MODULE_RATE_HZ;
		raw = drv_cpu_x86_rdmsr(info.msr);
		error = cpux86_residency_nanoseconds(raw, rate, &nanoseconds);
		if (error != 0)
			continue;

		/* Adds the state to the report. */
		slot = idle->count;
		idle->state[slot] = info.state;
		idle->residency_ns[slot] = nanoseconds;
		idle->available |= 1U << info.state;
		idle->count = slot + 1U;
	}

	/* Refuses a report without a single counter. */
	if (idle->count == 0U)
		return EOPNOTSUPP;

	/* Succeeded: the states and their times. */
	return 0;
}

/*
 * Gives every CPU the slot of its package, from the places the framework
 * read; a machine with more packages than slots leaves the rest without
 * one.
 */
static void
cpu_x86_attach_packages(void)
{
	struct kern_cpuperf_cpu info;
	kern_cpu_id_t cpu;
	uint32_t index;
	int error;

	/* Finds or makes the slot of each CPU's package. */
	cpu_x86_package_count = 0U;
	for (cpu = 0; cpu < drv_cpu_x86_cpu_count; cpu++) {
		error = kern_cpuperf_cpu(cpu, 0U, &info);
		if (error != 0)
			continue;

		/* Looks for the package among the slots. */
		for (index = 0; index < cpu_x86_package_count; index++) {
			if (cpu_x86_packages[index].package == info.topology.package)
				break;
		}

		/* Makes a slot for a new package while there is room. */
		if (index == cpu_x86_package_count) {
			if (index >= CPU_X86_PACKAGE_MAX)
				continue;
			kern_memset(&cpu_x86_packages[index], 0, sizeof(cpu_x86_packages[index]));
			cpu_x86_packages[index].package = info.topology.package;
			cpu_x86_package_count = index + 1U;
		}

		/*
		 * Gives the CPU its slot.  The CPU reads it in its start, which
		 * a later notification runs; the notification's lock orders the
		 * write before that read.
		 */
		drv_cpu_x86_cpus[cpu].package_index = index;
	}
}

/*
 * Allocates each package's feedback table below 4 GiB and the copy the
 * driver reads.  A package whose memory cannot be had keeps the interface
 * off.
 */
static void
cpu_x86_attach_tables(void)
{
	struct cpu_x86_package *package;
	uint64_t limit;
	uint32_t index;
	void *mapped;
	int error;

	/* Keeps the table where the pointer register can name it. */
	limit = CPU_X86_FEEDBACK_ADDRESS_MAX;
	if (drv_cpu_x86_machine.extended.physical_address_bits < 32U)
		limit = ((uint64_t)1 << drv_cpu_x86_machine.extended.physical_address_bits) - 1U;

	/* Allocates each package's table and copy. */
	for (index = 0; index < cpu_x86_package_count; index++) {
		package = &cpu_x86_packages[index];

		/* Allocates the table, page aligned, and clears it. */
		error = kern_pmem_alloc_limited(cpu_x86_layout.table_size, CPUX86_FEEDBACK_PAGE_SIZE, limit, 0U,
					       &package->memory);
		if (error != 0) {
			kern_logf("cpu-x86: no memory for package %u's feedback table (%d)\n", package->package, error);
			continue;
		}
		mapped = kern_pmem_to_kernel(package->memory.paddr);
		if (mapped == NULL) {
			(void)kern_pmem_free(&package->memory);
			continue;
		}
		kern_memset(mapped, 0, cpu_x86_layout.table_size);
		package->table = mapped;

		/* Allocates the driver's copy. */
		package->copy = kern_calloc(1U, cpu_x86_layout.table_size);
		if (package->copy == NULL) {
			kern_logf("cpu-x86: no memory for package %u's feedback copy\n", package->package);
			(void)kern_pmem_free(&package->memory);
			package->table = NULL;
		}
	}
}

/*
 * Offers the energy counters of a machine of one package as meters.  The
 * thermal core reads them from its poll thread on whichever CPU it runs,
 * which is right only when every CPU belongs to the one package.
 */
static void
cpu_x86_attach_meters(void)
{
	struct cpux86_rapl_units units;
	struct cpu_x86_meter *meter;
	uint64_t raw;
	uint32_t registers;
	uint32_t energy;
	uint32_t index;
	uint32_t count;
	int error;

	/* Offers no meter off the allow list or on a machine of several packages. */
	registers = drv_cpu_x86_machine.model_registers;
	if ((registers & CPUX86_MODEL_RAPL_UNIT) == 0U)
		return;
	if (cpu_x86_package_count != 1U)
		return;

	/*
	 * Reads the energy unit: one count is 1/2^shift joule, which is
	 * 10^9 / 2^shift nanojoules.
	 */
	raw = drv_cpu_x86_rdmsr(CPUX86_MSR_RAPL_POWER_UNIT);
	error = cpux86_rapl_units_decode(raw, &units);
	if (error != 0) {
		kern_logf("cpu-x86: the RAPL unit register reads 0x%llx; no energy meter\n", (unsigned long long)raw);
		return;
	}
	cpu_x86_meter_ops.count_bits = 32U;
	cpu_x86_meter_ops.unit_numerator = CPU_X86_NANOJOULES_PER_JOULE;
	cpu_x86_meter_ops.unit_denominator = (uint32_t)1U << units.energy_shift;
	cpu_x86_meter_ops.read = cpu_x86_meter_read;

	/* Registers each counter the model has; a platform counter that reads 0 is not supported. */
	count = 0U;
	for (index = 0; index < sizeof(cpu_x86_meters) / sizeof(cpu_x86_meters[0]); index++) {
		meter = &cpu_x86_meters[index];
		if ((registers & meter->model_bit) == 0U)
			continue;
		if (meter->kind == KERN_THERMAL_METER_PLATFORM) {
			raw = drv_cpu_x86_rdmsr(meter->msr);
			error = cpux86_energy_status_decode(raw, &energy);
			if (error != 0 || energy == 0U)
				continue;
		}
		error = kern_thermal_meter_register(meter->name, meter->kind, &cpu_x86_meter_ops, meter);
		if (error != 0) {
			kern_logf("cpu-x86: the thermal core refused the %s meter (%d)\n", meter->name, error);
			continue;
		}
		count++;
	}

	/* Reports the meters. */
	kern_logf("cpu-x86: %u energy meter%s, 1/2^%u J per count\n", count, count == 1U ? "" : "s",
		  units.energy_shift);
}
