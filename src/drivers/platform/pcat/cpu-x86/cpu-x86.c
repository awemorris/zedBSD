/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The x86 processor driver's description of the CPUs.
 *
 * Every amd64 kernel has this part: it reads, with CPUID on each CPU,
 * where the CPU sits in its package, which cache it shares with which
 * cores and what kind of core it is, and registers itself with the CPU
 * performance framework as the provider.  CPUID cannot fault, so this
 * part also runs under a hypervisor.  The model-specific registers
 * belong to the other part, which only a kernel built with the
 * performance control has, and which this part asks only after its
 * gates let it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/boot.h>
#include <kern/cpuperf.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/platform.h>
#include <kern/sched.h>
#include <kern/types.h>
#include <uapi/errno.h>

#include "cpu-x86.h"
#include "cpu-x86-insn.h"
#include "cpu-x86-private.h"
#include "cpux86.h"
#include "cpux86-cpuid.h"

/* The leaves the description reads. */
#define CPU_X86_LEAF_VENDOR		0x00U
#define CPU_X86_LEAF_FEATURES		0x01U
#define CPU_X86_LEAF_CACHE		0x04U
#define CPU_X86_LEAF_MWAIT		0x05U
#define CPU_X86_LEAF_POWER		0x06U
#define CPU_X86_LEAF_STRUCTURED		0x07U
#define CPU_X86_LEAF_HYBRID		0x1AU
#define CPU_X86_LEAF_INVARIANT_TSC	0x80000007U
#define CPU_X86_LEAF_ADDRESS_SIZE	0x80000008U

/* The most sub-leaves of leaf 04H the description reads before it gives up. */
#define CPU_X86_CACHE_SUBLEAF_MAX	16U

/* The cache level whose sharing makes a module: the second level. */
#define CPU_X86_MODULE_CACHE_LEVEL	2U

/* The boot parameters that turn the driver, or its use of HWP, off. */
#define CPU_X86_PARAMETER_OFF		"cpuperf=off"
#define CPU_X86_PARAMETER_HWP_OFF	"cpuperf.hwp=off"

/*
 * The machine, filled by drv_cpu_x86_init() on the CPU that runs it and
 * read-only once the provider is registered.
 */
struct cpu_x86_machine drv_cpu_x86_machine;

/*
 * One entry per kernel CPU, written by that CPU's notifications; NULL
 * until drv_cpu_x86_init() allocates it for the kernel's life.
 */
struct cpu_x86_cpu *drv_cpu_x86_cpus;

/* The number of entries of drv_cpu_x86_cpus, 0 until they exist. */
uint32_t drv_cpu_x86_cpu_count;

/*
 * The provider the driver registers.
 *
 * Filled once by drv_cpu_x86_init() before the registration: the
 * description always, and the register operations only when the gates
 * let them.  The framework keeps the pointer for the kernel's life.
 */
static struct kern_cpuperf_provider_ops cpu_x86_provider;

static int cpu_x86_topology(kern_cpu_id_t cpu, struct kern_cpuperf_topology *topology);
static int cpu_x86_describe(kern_cpu_id_t cpu, struct kern_cpuperf_levels *levels);
static int cpu_x86_identify(void);
static int cpu_x86_read_place(struct cpux86_topology *place);
static int cpu_x86_read_topology_leaf(uint32_t leaf, struct cpux86_topology *place);
static uint32_t cpu_x86_read_module(uint32_t x2apic_id, uint32_t core);
static void cpu_x86_read_kind(struct kern_cpuperf_topology *topology);
static void cpu_x86_read_power(struct cpu_x86_cpu *state);
static void cpu_x86_read_mwait(struct cpu_x86_cpu *state);
static uint32_t cpu_x86_domain_number(const struct cpux86_topology *place, uint32_t type);

/*
 * Describes the CPUs to the performance framework.
 *
 * Identifies the processor, reads the boot parameters, lets the register
 * part decide what it may touch, and registers the provider; the
 * framework then has every CPU read its place in its own notification.
 */
int
drv_cpu_x86_init(void)
{
	const char *line;
	uint32_t index;
	unsigned count;
#if CONFIG_CPU_PERF
	int hwp_off;
#endif
	int off;
	int error;

	/* Leaves the CPUs to the framework's default when the boot parameter says so. */
	line = kern_boot_handoff("boot.command-line");
	off = kern_boot_parameters_token_present(line, CPU_X86_PARAMETER_OFF);
	if (off) {
		kern_logf("cpu-x86: turned off by %s; the CPUs keep the default description\n",
			  CPU_X86_PARAMETER_OFF);
		return ENODEV;
	}

	/* Reads who made the processor; only Intel's leaves are decoded here. */
	error = cpu_x86_identify();
	if (error != 0)
		return error;

	/* Counts the CPUs the description covers. */
	count = kern_cpu_count();
	if (count == 0U || count > KERN_CPU_MAX)
		return ENODEV;

	/* Allocates the record of each CPU. */
	drv_cpu_x86_cpus = kern_calloc(count, sizeof(*drv_cpu_x86_cpus));
	if (drv_cpu_x86_cpus == NULL) {
		kern_logf("cpu-x86: no memory for %u CPUs\n", count);
		return ENOMEM;
	}

	/* Starts every CPU outside any package slot, until the slots exist. */
	for (index = 0; index < count; index++)
		drv_cpu_x86_cpus[index].package_index = CPU_X86_PACKAGE_NONE;
	drv_cpu_x86_cpu_count = count;

	/*
	 * Decides whether the model-specific registers may be touched at
	 * all: never under a hypervisor, whose virtual processor need not
	 * have the registers its CPUID names.
	 */
	drv_cpu_x86_machine.msr = false;
#if CONFIG_CPU_PERF
	hwp_off = kern_boot_parameters_token_present(line, CPU_X86_PARAMETER_HWP_OFF);
	if ((drv_cpu_x86_machine.features.flags & CPUX86_FEATURE_HYPERVISOR) != 0U) {
		kern_logf("cpu-x86: running under a hypervisor; the model-specific registers are left alone\n");
	} else {
		drv_cpu_x86_machine.msr = true;
		drv_cpu_x86_control_probe(hwp_off != 0);
	}
#endif

	/* Fills the provider: the description, and the registers where allowed. */
	cpu_x86_provider.topology = cpu_x86_topology;
	cpu_x86_provider.describe = cpu_x86_describe;
#if CONFIG_CPU_PERF
	if (drv_cpu_x86_machine.msr)
		drv_cpu_x86_control_ops(&cpu_x86_provider);
#endif

	/* Registers the provider; every CPU reads its place before this returns. */
	error = kern_cpuperf_provider_register(&cpu_x86_provider);
	if (error != 0) {
		kern_logf("cpu-x86: the performance framework refused the provider (%d)\n", error);
		return error;
	}
	kern_logf("cpu-x86: family %02xH model %02xH, %u CPUs described%s\n",
		  drv_cpu_x86_machine.identity.display_family,
		  drv_cpu_x86_machine.identity.display_model,
		  count,
		  drv_cpu_x86_machine.hybrid != 0U ? ", hybrid" : "");

#if CONFIG_CPU_PERF
	/* Prepares the feedback tables and the energy meters, which need the CPUs' places. */
	if (drv_cpu_x86_machine.msr)
		drv_cpu_x86_control_attach();
#endif

	/* Succeeded: the framework describes the CPUs from the driver's reading. */
	return 0;
}

/*
 * Reads one CPU's place in the machine and its kind of core.
 *
 * Runs on the CPU, in its notification with interrupts masked.  Also
 * keeps the CPU's own thermal and power leaf, its MWAIT states and, in a
 * kernel with the register part, what its control has before its first
 * request.
 */
static int
cpu_x86_topology(
	kern_cpu_id_t cpu,
	struct kern_cpuperf_topology *topology)
{
	struct cpux86_topology place;
	struct cpu_x86_cpu *state;
	uint32_t core;
	int error;

	/* Refuses a CPU the driver has no record for. */
	if (cpu >= drv_cpu_x86_cpu_count)
		return EINVAL;
	state = &drv_cpu_x86_cpus[cpu];

	/* Keeps the leaves that differ from CPU to CPU. */
	cpu_x86_read_power(state);
	cpu_x86_read_mwait(state);

	/* Reads where the CPU sits in its package. */
	error = cpu_x86_read_place(&place);
	if (error != 0)
		return error;
	state->x2apic_id = place.x2apic_id;

	/*
	 * Numbers each level by the x2APIC ID bits above the level below
	 * it, so a number is unique in the machine and two CPUs agree on it
	 * exactly when they share that level.
	 */
	core = cpu_x86_domain_number(&place, CPUX86_DOMAIN_CORE);
	topology->package = place.package;
	topology->die = cpu_x86_domain_number(&place, CPUX86_DOMAIN_DIE);
	topology->core = core;
	topology->thread = place.id[CPUX86_DOMAIN_THREAD];
	topology->module = cpu_x86_read_module(place.x2apic_id, core);
	topology->firmware_id = KERN_CPUPERF_UNKNOWN;

	/* Reads the kind of core. */
	cpu_x86_read_kind(topology);

#if CONFIG_CPU_PERF
	/* Reads what the CPU's control has before its first request. */
	if (drv_cpu_x86_machine.msr)
		drv_cpu_x86_control_cpu(state);
#endif

	/* Marks the CPU as read, for the parts that need its place. */
	state->read = true;

	/* Succeeded: the CPU's place is in the report. */
	return 0;
}

/*
 * Describes the performance levels of a CPU from what its topology read
 * kept.
 *
 * A CPU under HWP offers a range of abstract units it starts reporting
 * once its control has started (or at once when HWP was already on);
 * the two threads of a core share one clock.  Any other CPU is described
 * but not controlled.
 */
static int
cpu_x86_describe(
	kern_cpu_id_t cpu,
	struct kern_cpuperf_levels *levels)
{
	struct cpu_x86_cpu *state;

	/* Refuses a CPU the driver has no record for. */
	if (cpu >= drv_cpu_x86_cpu_count)
		return EINVAL;
	state = &drv_cpu_x86_cpus[cpu];

	/* Leaves a CPU without HWP control as the framework's uncontrolled one. */
	if (!drv_cpu_x86_machine.hwp)
		return 0;
	if ((state->power_features & CPUX86_POWER_HWP) == 0U)
		return 0;

	/* Offers the HWP range, shared by the threads of a core. */
	levels->kind = KERN_CPUPERF_LEVELS_RANGE;
	levels->share = KERN_CPUPERF_SHARE_CORE;

	/* Gives the anchors HWP already reported; otherwise they come with the start. */
	if (state->capabilities_known) {
		levels->lowest = state->capabilities.lowest;
		levels->efficient = state->capabilities.efficient;
		levels->guaranteed = state->capabilities.guaranteed;
		levels->highest = state->capabilities.highest;
	}

	/* Succeeded: the CPU is a RANGE CPU. */
	return 0;
}

/*
 * Reads the processor's identity and the features every CPU shares, on
 * the CPU that runs the initialization.
 *
 * Reports ENODEV for a processor this driver does not decode.
 */
static int
cpu_x86_identify(void)
{
	struct cpux86_cpuid_regs leaf0;
	struct cpux86_cpuid_regs leaf1;
	struct cpux86_cpuid_regs regs;
	struct cpux86_cpuid_regs extended7;
	struct cpux86_cpuid_regs extended8;
	const struct cpux86_cpuid_regs *invariant;
	const struct cpux86_cpuid_regs *address;
	struct cpu_x86_machine *machine;
	int present;
	int error;

	machine = &drv_cpu_x86_machine;

	/* Reads the vendor and the model. */
	drv_cpu_x86_cpuid(CPU_X86_LEAF_VENDOR, 0U, &leaf0);
	drv_cpu_x86_cpuid(CPU_X86_LEAF_FEATURES, 0U, &leaf1);
	error = cpux86_cpuid_identity(&leaf0, &leaf1, &machine->identity);
	if (error != 0) {
		kern_logf("cpu-x86: the processor reports no identity (%d)\n", error);
		return ENODEV;
	}

	/* Serves Intel's processors only: the leaves are decoded by Intel's manual. */
	if (machine->identity.vendor != CPUX86_VENDOR_INTEL) {
		kern_logf("cpu-x86: not an Intel processor; the CPUs keep the default description\n");
		return ENODEV;
	}

	/* Decodes the features of leaf 01H, the hypervisor bit among them. */
	error = cpux86_cpuid_features(&leaf1, &machine->features);
	if (error != 0)
		return ENODEV;

	/* Finds how far the extended leaves go. */
	drv_cpu_x86_cpuid(CPUX86_EXTENDED_BASE, 0U, &regs);
	error = cpux86_cpuid_extended_range(&regs, &machine->identity);
	if (error != 0)
		return ENODEV;

	/* Reads the invariant TSC and the physical address width where the leaves exist. */
	invariant = NULL;
	present = cpux86_cpuid_has_leaf(&machine->identity, CPU_X86_LEAF_INVARIANT_TSC);
	if (present) {
		drv_cpu_x86_cpuid(CPU_X86_LEAF_INVARIANT_TSC, 0U, &extended7);
		invariant = &extended7;
	}
	address = NULL;
	present = cpux86_cpuid_has_leaf(&machine->identity, CPU_X86_LEAF_ADDRESS_SIZE);
	if (present) {
		drv_cpu_x86_cpuid(CPU_X86_LEAF_ADDRESS_SIZE, 0U, &extended8);
		address = &extended8;
	}
	error = cpux86_cpuid_extended(invariant, address, &machine->extended);
	if (error != 0)
		return ENODEV;

	/* Reads whether the processor mixes kinds of core. */
	machine->hybrid = 0U;
	present = cpux86_cpuid_has_leaf(&machine->identity, CPU_X86_LEAF_STRUCTURED);
	if (present) {
		drv_cpu_x86_cpuid(CPU_X86_LEAF_STRUCTURED, 0U, &regs);
		error = cpux86_cpuid_hybrid(&regs, &machine->hybrid);
		if (error != 0)
			machine->hybrid = 0U;
	}

	/* Succeeded: an Intel processor whose leaves the driver reads. */
	return 0;
}

/*
 * Reads where this CPU sits in its package, from leaf 1FH when the
 * processor has it and otherwise from leaf 0BH.
 */
static int
cpu_x86_read_place(
	struct cpux86_topology *place)
{
	int present;
	int error;

	/* Prefers the newer leaf, which also names modules, tiles and dies. */
	present = cpux86_cpuid_has_leaf(&drv_cpu_x86_machine.identity, CPUX86_LEAF_TOPOLOGY_V2);
	if (present) {
		error = cpu_x86_read_topology_leaf(CPUX86_LEAF_TOPOLOGY_V2, place);
		if (error == 0)
			return 0;
	}

	/* Falls back to the older leaf, which names threads and cores. */
	present = cpux86_cpuid_has_leaf(&drv_cpu_x86_machine.identity, CPUX86_LEAF_TOPOLOGY);
	if (!present)
		return ENOTSUP;
	error = cpu_x86_read_topology_leaf(CPUX86_LEAF_TOPOLOGY, place);
	if (error != 0)
		return error;

	/* Succeeded: the older leaf placed the CPU. */
	return 0;
}

/*
 * Reads one extended topology leaf's sub-leaves, up to and including the
 * one that ends the list, and places this CPU from them.
 */
static int
cpu_x86_read_topology_leaf(
	uint32_t leaf,
	struct cpux86_topology *place)
{
	struct cpux86_cpuid_regs subleaves[CPUX86_TOPOLOGY_SUBLEAF_MAX];
	struct cpux86_topology_level level;
	uint32_t count;
	int error;

	/* Reads the sub-leaves until the one of type INVALID, which is kept as the terminator. */
	count = 0U;
	while (count < CPUX86_TOPOLOGY_SUBLEAF_MAX) {
		drv_cpu_x86_cpuid(leaf, count, &subleaves[count]);
		error = cpux86_cpuid_topology_level(&subleaves[count], &level);
		count++;
		if (error != 0)
			break;
		if (level.type == CPUX86_DOMAIN_INVALID)
			break;
	}

	/* Places the CPU; a list the decoder refuses leaves the CPU to the default. */
	error = cpux86_cpuid_topology(leaf, subleaves, count, place);
	if (error != 0)
		return error;

	/* Succeeded: the CPU is placed. */
	return 0;
}

/*
 * Reports the module of this CPU: the group of cores that share its
 * second-level cache, numbered by the x2APIC ID above the sharing bits.
 * A processor that does not describe that cache makes each core its own
 * module.
 */
static uint32_t
cpu_x86_read_module(
	uint32_t x2apic_id,
	uint32_t core)
{
	struct cpux86_cpuid_regs regs;
	struct cpux86_cache cache;
	uint32_t subleaf;
	uint32_t group;
	int present;
	int error;

	/* Keeps each core alone without the cache leaf. */
	present = cpux86_cpuid_has_leaf(&drv_cpu_x86_machine.identity, CPU_X86_LEAF_CACHE);
	if (!present)
		return core;

	/* Looks for the second-level data or unified cache among the sub-leaves. */
	for (subleaf = 0; subleaf < CPU_X86_CACHE_SUBLEAF_MAX; subleaf++) {
		drv_cpu_x86_cpuid(CPU_X86_LEAF_CACHE, subleaf, &regs);
		error = cpux86_cpuid_cache(&regs, &cache);
		if (error == ENOTSUP)
			break;
		if (error != 0)
			continue;
		if (cache.level != CPU_X86_MODULE_CACHE_LEVEL)
			continue;
		if (cache.type == CPUX86_CACHE_INSTRUCTION)
			continue;

		/* Numbers the module by the cache instance the CPU uses. */
		error = cpux86_cache_group(x2apic_id, &cache, &group);
		if (error != 0)
			return core;
		return group;
	}

	/* No second-level cache was described: the core is its own module. */
	return core;
}

/*
 * Reads the kind of this CPU's core.  A processor of one kind reports
 * every core as a performance core; a hybrid one names each core's kind
 * in leaf 1AH.
 */
static void
cpu_x86_read_kind(
	struct kern_cpuperf_topology *topology)
{
	struct cpux86_cpuid_regs regs;
	struct cpux86_core_kind kind;
	int present;
	int error;

	/* Calls every core of a processor of one kind a performance core. */
	topology->core_class = KERN_CPUPERF_CORE_PERFORMANCE;
	topology->core_type = 0U;
	if (drv_cpu_x86_machine.hybrid == 0U)
		return;

	/* Reads the kind a hybrid processor names for this core. */
	topology->core_class = KERN_CPUPERF_CORE_UNKNOWN;
	present = cpux86_cpuid_has_leaf(&drv_cpu_x86_machine.identity, CPU_X86_LEAF_HYBRID);
	if (!present)
		return;
	drv_cpu_x86_cpuid(CPU_X86_LEAF_HYBRID, 0U, &regs);
	error = cpux86_cpuid_core_kind(&regs, &kind);
	if (error != 0)
		return;

	/* Maps the processor's code onto the framework's kinds; another code stays unknown. */
	topology->core_type = kind.core_type;
	if (kind.core_type == CPUX86_CORE_TYPE_CORE) {
		topology->core_class = KERN_CPUPERF_CORE_PERFORMANCE;
	} else if (kind.core_type == CPUX86_CORE_TYPE_ATOM) {
		topology->core_class = KERN_CPUPERF_CORE_EFFICIENCY;
	}
}

/*
 * Keeps this CPU's thermal and power features and its row of the
 * hardware feedback table.  A processor without leaf 06H has none.
 */
static void
cpu_x86_read_power(
	struct cpu_x86_cpu *state)
{
	struct cpux86_cpuid_regs regs;
	struct cpux86_power power;
	int present;
	int error;

	/* Starts with no feature. */
	state->power_features = 0U;
	state->feedback_row = 0U;

	/* Reads the leaf where it exists. */
	present = cpux86_cpuid_has_leaf(&drv_cpu_x86_machine.identity, CPU_X86_LEAF_POWER);
	if (!present)
		return;
	drv_cpu_x86_cpuid(CPU_X86_LEAF_POWER, 0U, &regs);
	error = cpux86_cpuid_power(&regs, &power);
	if (error != 0)
		return;

	/* Keeps the features and the row. */
	state->power_features = power.features;
	state->feedback_row = power.feedback_row;
}

/*
 * Keeps the MWAIT states this CPU offers.  A processor without leaf 05H
 * offers none the driver knows of.
 */
static void
cpu_x86_read_mwait(
	struct cpu_x86_cpu *state)
{
	struct cpux86_cpuid_regs regs;
	int present;
	int error;

	/* Reads the leaf where the processor has MONITOR and MWAIT. */
	state->mwait_known = false;
	if ((drv_cpu_x86_machine.features.flags & CPUX86_FEATURE_MONITOR) == 0U)
		return;
	present = cpux86_cpuid_has_leaf(&drv_cpu_x86_machine.identity, CPU_X86_LEAF_MWAIT);
	if (!present)
		return;
	drv_cpu_x86_cpuid(CPU_X86_LEAF_MWAIT, 0U, &regs);
	error = cpux86_cpuid_mwait(&regs, &state->mwait);
	if (error != 0)
		return;

	/* The states are known. */
	state->mwait_known = true;
}

/*
 * Numbers the instance of one domain a CPU sits in by its x2APIC ID with
 * the bits below that domain dropped: the domain's own field and every
 * field above it.  An absent domain is 0.
 */
static uint32_t
cpu_x86_domain_number(
	const struct cpux86_topology *place,
	uint32_t type)
{
	/* An absent domain has one instance. */
	if ((place->present & (1U << type)) == 0U)
		return 0U;

	/* A shift of the whole ID leaves only the higher levels, which are 0. */
	if (place->low[type] >= 32U)
		return 0U;

	/* Succeeded: the ID above the domain's lower boundary. */
	return place->x2apic_id >> place->low[type];
}
