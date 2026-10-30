/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the x86 processor driver (src/drivers/platform/pcat/cpu-x86/cpu-x86*.c).
 *
 * The three instructions are replaced by a model of a processor: CPUID
 * answers from a table built for the scenario, per CPU; the
 * model-specific registers live in a model that knows which registers the
 * processor has and when they may be touched, and records a fault (as the
 * real processor's #GP would stop the kernel) for any other access, for a
 * write of a reserved bit, and for every register touched under a
 * hypervisor.  The kernel services are small stand-ins: the performance
 * framework runs the topology of each CPU as that CPU and keeps the
 * operations, so the test calls them as the framework would, on the
 * CPU they name.  Each scenario runs in its own process, since the driver
 * keeps its state for the kernel's life.
 */

#include <kern/boot.h>
#include <kern/cpuperf.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/platform.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/thermal.h>
#include <uapi/errno.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cpu-x86.h"
#include "cpu-x86-insn.h"
#include "cpux86.h"

#define TEST_CPU_MAX		16U
#define TEST_MSR_MAX		64U
#define TEST_METER_MAX		8U
#define TEST_TABLE_ADDRESS	0x10000000UL

/* The registers the model knows. */
#define MSR_PLATFORM_INFO	0x0CEU
#define MSR_PKG_CST_CONFIG	0x0E2U
#define MSR_MPERF		0x0E7U
#define MSR_APERF		0x0E8U
#define MSR_PKG_THERM_STATUS	0x1B1U
#define MSR_PKG_C3		0x3F8U
#define MSR_PKG_C6		0x3F9U
#define MSR_PKG_C7		0x3FAU
#define MSR_CORE_C3		0x3FCU
#define MSR_CORE_C6		0x3FDU
#define MSR_CORE_C7		0x3FEU
#define MSR_RAPL_UNIT		0x606U
#define MSR_PKG_C2		0x60DU
#define MSR_PKG_ENERGY		0x611U
#define MSR_PP0_ENERGY		0x639U
#define MSR_PP1_ENERGY		0x641U
#define MSR_PLATFORM_ENERGY	0x64DU
#define MSR_MC6			0x664U
#define MSR_PM_ENABLE		0x770U
#define MSR_HWP_CAPABILITIES	0x771U
#define MSR_HWP_REQUEST_PKG	0x772U
#define MSR_HWP_INTERRUPT	0x773U
#define MSR_HWP_REQUEST		0x774U
#define MSR_HWP_CTL		0x776U
#define MSR_HWP_STATUS		0x777U
#define MSR_HFI_PTR		0x17D0U
#define MSR_HFI_CONFIG		0x17D1U

/* Leaf 06H EAX bits of the model. */
#define P_ARAT			(1U << 2)
#define P_PKG_THERM		(1U << 6)
#define P_HWP			(1U << 7)
#define P_HWP_INT		(1U << 8)
#define P_HWP_WINDOW		(1U << 9)
#define P_HWP_EPP		(1U << 10)
#define P_HFI			(1U << 19)
#define P_HWP_CTL		(1U << 22)
#define P_TD			(1U << 23)

/* What one scenario's processor is. */
struct test_machine {
	const char *name;
	bool intel;
	bool hypervisor;
	uint32_t family_model_eax;	/* leaf 01H EAX */
	uint32_t power_eax;		/* leaf 06H EAX */
	bool aperf;			/* leaf 06H ECX[0] */
	bool hybrid;
	bool listed;			/* the model's registers exist (it is on the allow list) */
	bool hwp_on_at_boot;
	bool hfi_on_at_boot;
	uint32_t cpu_count;
	uint32_t packages;
	const char *command_line;
};

/* One CPU of the model. */
struct test_cpu {
	uint32_t x2apic;
	uint32_t core_type;		/* 0x40 P, 0x20 E, 0 one kind */
	uint32_t row;
	uint32_t caps[4];		/* highest, guaranteed, efficient, lowest */
	uint64_t request;
	uint64_t hwp_ctl;
	uint64_t hwp_interrupt;
	uint64_t hwp_status;
	uint64_t aperf;
	uint64_t mperf;
	uint64_t core_c[3];
};

/* One package of the model. */
struct test_package {
	uint64_t pm_enable;
	uint64_t therm_status;
	uint64_t hfi_ptr;
	uint64_t hfi_config;
	uint64_t energy[4];
	uint64_t pkg_c[4];
};

/* One register access the model refused, as the processor would fault. */
struct test_fault {
	uint32_t cpu;
	uint32_t msr;
	bool write;
	uint64_t value;
};

static struct test_machine machine;
static struct test_cpu cpus[TEST_CPU_MAX];
static struct test_package packages[2];
static uint32_t current_cpu;
static uint32_t faults;
static struct test_fault last_fault;
static uint32_t reads;
static uint32_t writes;
static uint32_t write_log_msr[256] __attribute__((unused));
static uint32_t write_log_cpu[256] __attribute__((unused));
static uint32_t write_log_count __attribute__((unused));

static const struct kern_cpuperf_provider_ops *provider;
static struct kern_cpuperf_topology topologies[TEST_CPU_MAX];
static struct kern_cpuperf_levels levels[TEST_CPU_MAX];
static int provider_result;

static const char *meter_names[TEST_METER_MAX];
static uint32_t meter_kinds[TEST_METER_MAX];
static const struct kern_thermal_meter_ops *meter_ops[TEST_METER_MAX];
static void *meter_contexts[TEST_METER_MAX];
static uint32_t meter_count;

static uint8_t *table_memory;
static size_t table_size;
static uint32_t table_allocations;

static char log_text[8192];
static uint32_t checks;
static uint32_t failures;

#define CHECK(condition) check_at((condition), #condition, __LINE__)

static void
check_at(
	bool condition,
	const char *text,
	int line)
{
	checks++;
	if (condition)
		return;
	failures++;
	fprintf(stderr, "  FAIL %s:%d %s\n", machine.name, line, text);
}

/* ---- the kernel's services ---- */

void
kern_logf(
	const char *format,
	...)
{
	va_list arguments;
	size_t used;

	used = strlen(log_text);
	va_start(arguments, format);
	vsnprintf(log_text + used, sizeof(log_text) - used, format, arguments);
	va_end(arguments);
}

void *
kern_malloc(
	size_t size)
{
	return malloc(size);
}

void *
kern_calloc(
	size_t count,
	size_t size)
{
	return calloc(count, size);
}

void
kern_free(
	void *pointer)
{
	free(pointer);
}

unsigned
kern_cpu_count(void)
{
	return machine.cpu_count;
}

unsigned
kern_cpu_current(void)
{
	return current_cpu;
}

void *
kern_boot_handoff(
	const char *name)
{
	if (strcmp(name, "boot.command-line") != 0)
		return NULL;
	return (void *)machine.command_line;
}

int
kern_boot_parameters_token_present(
	const char *text,
	const char *token)
{
	const char *cursor;
	size_t length;

	if (text == NULL)
		return 0;
	length = strlen(token);
	for (cursor = text; *cursor != '\0'; cursor++) {
		if (cursor != text && cursor[-1] != ' ')
			continue;
		if (strncmp(cursor, token, length) == 0 && (cursor[length] == ' ' || cursor[length] == '\0'))
			return 1;
	}
	return 0;
}

int
kern_pmem_alloc_limited(
	size_t size,
	size_t alignment,
	uint64_t max_address,
	size_t boundary,
	struct kern_pmem *run)
{
	(void)boundary;
	CHECK(alignment == 4096U);
	CHECK(max_address <= 0xffffffffULL);
	table_allocations++;
	if (table_memory != NULL)
		return ENOMEM;
	table_memory = aligned_alloc(4096U, size);
	memset(table_memory, 0xa5, size);
	table_size = size;
	run->paddr = TEST_TABLE_ADDRESS;
	run->size = size;
	return 0;
}

int
kern_pmem_free(
	struct kern_pmem *run)
{
	(void)run;
	return 0;
}

void *
kern_pmem_to_kernel(
	kern_physaddr_t address)
{
	if (address != TEST_TABLE_ADDRESS)
		return NULL;
	return table_memory;
}

int
kern_thermal_meter_register(
	const char *name,
	uint32_t kind,
	const struct kern_thermal_meter_ops *ops,
	void *context)
{
	if (meter_count >= TEST_METER_MAX)
		return ENOSPC;
	CHECK(ops->count_bits == 32U);
	CHECK(ops->unit_numerator != 0U && ops->unit_denominator != 0U);
	meter_names[meter_count] = name;
	meter_kinds[meter_count] = kind;
	meter_ops[meter_count] = ops;
	meter_contexts[meter_count] = context;
	meter_count++;
	return 0;
}

int
kern_cpuperf_provider_register(
	const struct kern_cpuperf_provider_ops *ops)
{
	uint32_t cpu;

	/* Reads every CPU's place on that CPU, then asks for the levels, as the framework does. */
	provider = ops;
	for (cpu = 0; cpu < machine.cpu_count; cpu++) {
		current_cpu = cpu;
		memset(&topologies[cpu], 0, sizeof(topologies[cpu]));
		CHECK(ops->topology(cpu, &topologies[cpu]) == 0);
	}
	current_cpu = 0;
	for (cpu = 0; cpu < machine.cpu_count; cpu++) {
		memset(&levels[cpu], 0, sizeof(levels[cpu]));
		levels[cpu].lowest = KERN_CPUPERF_UNKNOWN;
		levels[cpu].efficient = KERN_CPUPERF_UNKNOWN;
		levels[cpu].guaranteed = KERN_CPUPERF_UNKNOWN;
		levels[cpu].highest = KERN_CPUPERF_UNKNOWN;
		CHECK(ops->describe(cpu, &levels[cpu]) == 0);
	}
	return provider_result;
}

int
kern_cpuperf_cpu(
	kern_cpu_id_t cpu,
	uint32_t flags,
	struct kern_cpuperf_cpu *info)
{
	CHECK(flags == 0U);
	if (cpu >= machine.cpu_count)
		return EINVAL;
	memset(info, 0, sizeof(*info));
	info->topology = topologies[cpu];
	return 0;
}

/* ---- the processor ---- */

static uint32_t
package_of(
	uint32_t cpu)
{
	if (machine.packages == 2U && cpu >= machine.cpu_count / 2U)
		return 1U;
	return 0U;
}

static void __attribute__((unused))
fault(
	uint32_t msr,
	bool write,
	uint64_t value)
{
	faults++;
	last_fault.cpu = current_cpu;
	last_fault.msr = msr;
	last_fault.write = write;
	last_fault.value = value;
}

/* Reports whether a register exists on this CPU and may be touched now. */
static bool __attribute__((unused))
msr_allowed(
	uint32_t msr)
{
	struct test_package *package;
	uint32_t power;

	if (machine.hypervisor)
		return false;
	power = machine.power_eax;
	package = &packages[package_of(current_cpu)];
	switch (msr) {
	case MSR_PM_ENABLE:
		return (power & P_HWP) != 0U;
	case MSR_HWP_INTERRUPT:
		return (power & P_HWP_INT) != 0U;
	case MSR_HWP_CAPABILITIES:
	case MSR_HWP_REQUEST:
	case MSR_HWP_STATUS:
		return (power & P_HWP) != 0U && (package->pm_enable & 1U) != 0U;
	case MSR_HWP_CTL:
		return (power & P_HWP_CTL) != 0U && (package->pm_enable & 1U) != 0U;
	case MSR_HFI_PTR:
	case MSR_HFI_CONFIG:
		return (power & P_HFI) != 0U;
	case MSR_PKG_THERM_STATUS:
		return (power & P_PKG_THERM) != 0U;
	case MSR_MPERF:
	case MSR_APERF:
		return machine.aperf;
	case MSR_PLATFORM_INFO:
	case MSR_PKG_CST_CONFIG:
	case MSR_RAPL_UNIT:
	case MSR_PKG_ENERGY:
	case MSR_PP0_ENERGY:
	case MSR_PP1_ENERGY:
	case MSR_PLATFORM_ENERGY:
	case MSR_PKG_C2:
	case MSR_PKG_C3:
	case MSR_PKG_C6:
	case MSR_PKG_C7:
	case MSR_CORE_C3:
	case MSR_CORE_C6:
	case MSR_CORE_C7:
	case MSR_MC6:
		return machine.listed;
	default:
		return false;
	}
}

#if CONFIG_CPU_PERF

uint64_t
drv_cpu_x86_rdmsr(
	uint32_t msr)
{
	struct test_package *package;
	struct test_cpu *cpu;

	reads++;
	if (!msr_allowed(msr)) {
		fault(msr, false, 0U);
		return 0U;
	}
	cpu = &cpus[current_cpu];
	package = &packages[package_of(current_cpu)];
	switch (msr) {
	case MSR_PM_ENABLE:
		return package->pm_enable;
	case MSR_HWP_INTERRUPT:
		return cpu->hwp_interrupt;
	case MSR_HWP_CAPABILITIES:
		return (uint64_t)cpu->caps[0] | (uint64_t)cpu->caps[1] << 8 |
		       (uint64_t)cpu->caps[2] << 16 | (uint64_t)cpu->caps[3] << 24;
	case MSR_HWP_REQUEST:
		return cpu->request;
	case MSR_HWP_STATUS:
		return cpu->hwp_status;
	case MSR_HWP_CTL:
		return cpu->hwp_ctl;
	case MSR_HFI_PTR:
		return package->hfi_ptr;
	case MSR_HFI_CONFIG:
		return package->hfi_config;
	case MSR_PKG_THERM_STATUS:
		return package->therm_status;
	case MSR_MPERF:
		return cpu->mperf;
	case MSR_APERF:
		return cpu->aperf;
	case MSR_PLATFORM_INFO:
		return (uint64_t)16U << 8 | (uint64_t)4U << 40 | (uint64_t)4U << 48;	/* 1600 MHz */
	case MSR_RAPL_UNIT:
		return 0x000A0E03U;	/* energy 1/2^14 J */
	case MSR_PKG_ENERGY:
		return packages[0].energy[0];
	case MSR_PP0_ENERGY:
		return packages[0].energy[1];
	case MSR_PP1_ENERGY:
		return packages[0].energy[2];
	case MSR_PLATFORM_ENERGY:
		return packages[0].energy[3];
	case MSR_PKG_C2:
		return package->pkg_c[0];
	case MSR_PKG_C3:
		return package->pkg_c[1];
	case MSR_PKG_C6:
		return package->pkg_c[2];
	case MSR_PKG_C7:
		return package->pkg_c[3];
	case MSR_CORE_C3:
		return cpu->core_c[0];
	case MSR_CORE_C6:
		return cpu->core_c[1];
	case MSR_CORE_C7:
		return cpu->core_c[2];
	default:
		return 0U;
	}
}

void
drv_cpu_x86_wrmsr(
	uint32_t msr,
	uint64_t value)
{
	struct test_package *package;
	struct test_cpu *cpu;

	writes++;
	if (write_log_count < 256U) {
		write_log_msr[write_log_count] = msr;
		write_log_cpu[write_log_count] = current_cpu;
		write_log_count++;
	}
	if (!msr_allowed(msr)) {
		fault(msr, true, value);
		return;
	}
	cpu = &cpus[current_cpu];
	package = &packages[package_of(current_cpu)];
	switch (msr) {
	case MSR_PM_ENABLE:
		if (value > 1U)
			fault(msr, true, value);
		if ((package->pm_enable & 1U) == 0U)
			package->pm_enable = value & 1U;
		return;
	case MSR_HWP_INTERRUPT:
		if ((value >> 3) != 0U)
			fault(msr, true, value);
		cpu->hwp_interrupt = value;
		return;
	case MSR_HWP_REQUEST:
		/* Bits 58:43 are reserved, and so are the EPP bits without EPP. */
		if ((value & 0x07fff80000000000ULL) != 0U)
			fault(msr, true, value);
		if ((machine.power_eax & P_HWP_EPP) == 0U && (value & 0xff000000ULL) != 0U)
			fault(msr, true, value);
		cpu->request = value;
		return;
	case MSR_HWP_STATUS:
		if ((value & ~0x3dULL) != 0U)
			fault(msr, true, value);
		cpu->hwp_status &= value;
		return;
	case MSR_HWP_CTL:
		if ((value & ~1ULL) != 0U)
			fault(msr, true, value);
		cpu->hwp_ctl = value;
		return;
	case MSR_HFI_PTR:
		package->hfi_ptr = value;
		return;
	case MSR_HFI_CONFIG:
		if ((value & ~3ULL) != 0U)
			fault(msr, true, value);
		if ((value & 2U) != 0U && (machine.power_eax & P_TD) == 0U)
			fault(msr, true, value);
		if ((value & 1U) != 0U && (package->hfi_ptr & 1U) == 0U)
			fault(msr, true, value);
		package->hfi_config = value;
		return;
	case MSR_PKG_THERM_STATUS:
		/* Writing 0 to bit 26 clears it; the log bits are R/WC0. */
		if ((value & (1ULL << 26)) == 0U)
			package->therm_status &= ~(1ULL << 26);
		return;
	default:
		/* Every other register the driver may only read. */
		fault(msr, true, value);
		return;
	}
}

#endif

void
drv_cpu_x86_cpuid(
	uint32_t leaf,
	uint32_t subleaf,
	struct cpux86_cpuid_regs *regs)
{
	struct test_cpu *cpu;
	uint32_t ecx;
	bool p_core;

	cpu = &cpus[current_cpu];
	p_core = cpu->core_type != 0x20U;
	memset(regs, 0, sizeof(*regs));
	switch (leaf) {
	case 0x00U:
		regs->eax = 0x20U;
		if (machine.intel) {
			regs->ebx = 0x756e6547U;
			regs->edx = 0x49656e69U;
			regs->ecx = 0x6c65746eU;
		} else {
			regs->ebx = 0x68747541U;	/* "AuthenticAMD" */
			regs->edx = 0x69746e65U;
			regs->ecx = 0x444d4163U;
		}
		return;
	case 0x01U:
		regs->eax = machine.family_model_eax;
		regs->ebx = (cpu->x2apic & 0xffU) << 24 | 0x00800000U;
		ecx = (1U << 3) | (1U << 7) | (1U << 21) | (1U << 24);
		if (machine.hypervisor)
			ecx |= 1U << 31;
		regs->ecx = ecx;
		return;
	case 0x04U:
		if (subleaf == 0U) {
			regs->eax = 1U | 1U << 5 | (p_core ? 1U : 0U) << 14;
		} else if (subleaf == 1U) {
			regs->eax = 2U | 1U << 5 | (p_core ? 1U : 0U) << 14;
		} else if (subleaf == 2U) {
			regs->eax = 3U | 2U << 5 | (p_core ? 1U : 7U) << 14;
		} else if (subleaf == 3U) {
			regs->eax = 3U | 3U << 5 | 31U << 14;
		} else {
			return;
		}
		regs->ebx = 63U | 7U << 22;
		regs->ecx = 63U;
		return;
	case 0x05U:
		regs->eax = 64U;
		regs->ebx = 64U;
		regs->ecx = 3U;
		regs->edx = 0x10102120U;
		return;
	case 0x06U:
		regs->eax = machine.power_eax;
		regs->ecx = (machine.aperf ? 1U : 0U) | 4U << 8;
		regs->edx = 3U | cpu->row << 16;
		return;
	case 0x07U:
		if (machine.hybrid && subleaf == 0U)
			regs->edx = 1U << 15;
		return;
	case 0x1AU:
		if (machine.hybrid)
			regs->eax = cpu->core_type << 24 | 1U;
		return;
	case 0x0BU:
	case 0x1FU:
		regs->edx = cpu->x2apic;
		if (subleaf == 0U) {
			regs->eax = 1U;
			regs->ebx = 2U;
			regs->ecx = 0U | 1U << 8;
		} else if (subleaf == 1U) {
			regs->eax = 7U;
			regs->ebx = machine.cpu_count;
			regs->ecx = 1U | 2U << 8;
		} else {
			regs->ecx = subleaf;
		}
		return;
	case 0x80000000U:
		regs->eax = 0x80000008U;
		return;
	case 0x80000007U:
		regs->edx = 1U << 8;
		return;
	case 0x80000008U:
		regs->eax = 39U;
		return;
	default:
		return;
	}
}

/* ---- the scenarios ---- */

/* Builds the CPUs of a Latitude 5330 (i5-1245U): two P-cores with two threads, eight E-cores. */
static void
cpus_5330(void)
{
	static const uint32_t ids[12] = { 0, 1, 8, 9, 16, 18, 20, 22, 24, 26, 28, 30 };
	uint32_t index;

	for (index = 0; index < 12U; index++) {
		memset(&cpus[index], 0, sizeof(cpus[index]));
		cpus[index].x2apic = ids[index];
		cpus[index].row = index;
		cpus[index].core_type = index < 4U ? 0x40U : 0x20U;
		if (index < 4U) {
			cpus[index].caps[0] = 56U;
			cpus[index].caps[1] = 21U;
			cpus[index].caps[2] = 20U;
			cpus[index].caps[3] = 1U;
		} else {
			cpus[index].caps[0] = 33U;
			cpus[index].caps[1] = 12U;
			cpus[index].caps[2] = 15U;
			cpus[index].caps[3] = 1U;
		}
		cpus[index].hwp_ctl = 1U;
		cpus[index].request = 0x80002a01ULL;
	}
}

static void
machine_5330(
	const char *name,
	const char *command_line)
{
	memset(&machine, 0, sizeof(machine));
	memset(packages, 0, sizeof(packages));
	machine.name = name;
	machine.intel = true;
	machine.family_model_eax = 0x000906A4U;
	machine.power_eax = 1U | 2U | P_ARAT | P_PKG_THERM | P_HWP | P_HWP_INT | P_HWP_WINDOW |
			    P_HWP_EPP | P_HFI | P_HWP_CTL | P_TD;
	machine.aperf = true;
	machine.hybrid = true;
	machine.listed = true;
	machine.cpu_count = 12U;
	machine.packages = 1U;
	machine.command_line = command_line;
	cpus_5330();
	packages[0].energy[0] = 1000U;
	packages[0].energy[1] = 400U;
	packages[0].energy[2] = 100U;
	packages[0].energy[3] = 0U;
}

static int
run_init(void)
{
	int error;

	current_cpu = 0;
	error = drv_cpu_x86_init();
	return error;
}

/* cpuperf=off: no provider, not even CPUID beyond nothing. */
static void
scenario_off(void)
{
	machine_5330("cpuperf-off", "kmsg=quiet cpuperf=off");
	CHECK(run_init() == ENODEV);
	CHECK(provider == NULL);
	CHECK(reads == 0U && writes == 0U);
	CHECK(meter_count == 0U);
}

/* A processor that is not Intel's: no provider. */
static void
scenario_not_intel(void)
{
	machine_5330("not-intel", "");
	machine.intel = false;
	CHECK(run_init() == ENODEV);
	CHECK(provider == NULL && reads == 0U);
}

#if CONFIG_CPU_PERF

/* The processor's identity, gates and topology on a 5330. */
static void
scenario_5330_topology(void)
{
	uint32_t cpu;
	int error;

	machine_5330("5330-topology", "boot0=UUID=1234 rootpart=2");
	error = run_init();
	CHECK(error == 0);
	CHECK(faults == 0U);
	CHECK(writes == 0U);	/* nothing is written at boot */
	CHECK(provider != NULL);

	/* Places: two P-cores of two threads, two modules of four E-cores. */
	CHECK(topologies[0].core == topologies[1].core);
	CHECK(topologies[0].thread == 0U && topologies[1].thread == 1U);
	CHECK(topologies[0].core != topologies[2].core);
	CHECK(topologies[4].core != topologies[5].core);
	CHECK(topologies[0].module == topologies[1].module);
	CHECK(topologies[0].module != topologies[2].module);
	CHECK(topologies[4].module == topologies[7].module);
	CHECK(topologies[8].module == topologies[11].module);
	CHECK(topologies[4].module != topologies[8].module);
	for (cpu = 0; cpu < 12U; cpu++) {
		CHECK(topologies[cpu].package == 0U);
		CHECK(topologies[cpu].firmware_id == KERN_CPUPERF_UNKNOWN);
		if (cpu < 4U) {
			CHECK(topologies[cpu].core_class == KERN_CPUPERF_CORE_PERFORMANCE);
			CHECK(topologies[cpu].core_type == 0x40U);
		} else {
			CHECK(topologies[cpu].core_class == KERN_CPUPERF_CORE_EFFICIENCY);
			CHECK(topologies[cpu].core_type == 0x20U);
		}
		/* A RANGE CPU shared by the threads of a core, anchors unknown until the start. */
		CHECK(levels[cpu].kind == KERN_CPUPERF_LEVELS_RANGE);
		CHECK(levels[cpu].share == KERN_CPUPERF_SHARE_CORE);
		CHECK(levels[cpu].highest == KERN_CPUPERF_UNKNOWN);
		CHECK(levels[cpu].lowest == KERN_CPUPERF_UNKNOWN);
	}

	/* Every operation is offered. */
	CHECK(provider->start != NULL && provider->apply != NULL && provider->sample != NULL);
	CHECK(provider->measure != NULL);
	CHECK(provider->feedback != NULL && provider->feedback_read != NULL && provider->relative != NULL);
	CHECK(provider->relative_sources == KERN_CPUPERF_RELATIVE_FEEDBACK);
	CHECK(provider->idle_package != NULL && provider->idle_core != NULL);

	/* One feedback table, page aligned below 4 GiB, and three meters (the platform reads 0). */
	CHECK(table_allocations == 1U);
	CHECK(table_size == 4096U);
	CHECK(meter_count == 3U);
	CHECK(strcmp(meter_names[0], "package") == 0 && meter_kinds[0] == KERN_THERMAL_METER_PACKAGE);
	CHECK(strcmp(meter_names[1], "cores") == 0 && meter_kinds[1] == KERN_THERMAL_METER_CORES);
	CHECK(strcmp(meter_names[2], "graphics") == 0 && meter_kinds[2] == KERN_THERMAL_METER_GRAPHICS);
	CHECK(meter_ops[0]->unit_numerator == 1000000000U && meter_ops[0]->unit_denominator == 16384U);
	CHECK(strstr(log_text, "model 9aH") != NULL);
}

/* The start, the requests, the samples and the measurement on a 5330. */
static void
scenario_5330_control(void)
{
	struct kern_cpuperf_sample sample;
	struct kern_cpuperf_request request;
	uint64_t count;
	uint32_t mhz;
	uint32_t cpu;
	int error;

	machine_5330("5330-control", "");
	CHECK(run_init() == 0);
	CHECK(faults == 0U && writes == 0U);

	/* A request before the start is refused without a register touched. */
	current_cpu = 2;
	request.minimum = 1U;
	request.maximum = 56U;
	request.desired = 0U;
	request.preference = 0x80U;
	CHECK(provider->apply(2, &request) == EIO);
	CHECK(faults == 0U);

	/* The start enables HWP once, frees the package request, silences the interrupt, reads the anchors. */
	for (cpu = 0; cpu < 12U; cpu++) {
		current_cpu = cpu;
		memset(&sample, 0, sizeof(sample));
		error = provider->start(cpu, &sample);
		CHECK(error == 0);
		CHECK((sample.flags & KERN_CPUPERF_SAMPLE_CAPABILITIES) != 0U);
		CHECK(sample.highest == cpus[cpu].caps[0] && sample.guaranteed == cpus[cpu].caps[1]);
		CHECK(sample.efficient == cpus[cpu].caps[2] && sample.lowest == cpus[cpu].caps[3]);
		CHECK(cpus[cpu].hwp_ctl == 0U);
	}
	CHECK(faults == 0U);
	CHECK(packages[0].pm_enable == 1U);
	CHECK(write_log_msr[0] == MSR_PM_ENABLE);	/* the enable comes before any other HWP register */
	CHECK((packages[0].hfi_ptr & ~0xfffULL) == TEST_TABLE_ADDRESS && (packages[0].hfi_ptr & 1U) == 1U);
	CHECK(packages[0].hfi_config == 1U);		/* plain feedback, no Thread Director */

	/* A request in range is written field by field; window, bit 42 and the valid bits are 0. */
	current_cpu = 0;
	request.minimum = 21U;
	request.maximum = 56U;
	request.desired = 0U;
	request.preference = 0U;
	CHECK(provider->apply(0, &request) == 0);
	CHECK(cpus[0].request == (21ULL | 56ULL << 8));
	current_cpu = 5;
	request.minimum = 1U;
	request.maximum = 15U;
	request.desired = 0U;
	request.preference = 0xC0U;
	CHECK(provider->apply(5, &request) == 0);
	CHECK(cpus[5].request == (1ULL | 15ULL << 8 | 0xC0ULL << 24));

	/* An unknown preference keeps the one in force. */
	request.preference = KERN_CPUPERF_UNKNOWN;
	request.maximum = 33U;
	CHECK(provider->apply(5, &request) == 0);
	CHECK(cpus[5].request == (1ULL | 33ULL << 8 | 0xC0ULL << 24));

	/* A request outside the anchors is refused and nothing is written. */
	request.maximum = 34U;
	cpus[5].request = 7U;
	CHECK(provider->apply(5, &request) == EINVAL);
	CHECK(cpus[5].request == 7U);
	CHECK(faults == 0U);

	/* A sample reads the anchors again, notes and acknowledges an excursion. */
	current_cpu = 1;
	cpus[1].caps[1] = 18U;
	cpus[1].hwp_status = 0x04U | 0x01U;
	memset(&sample, 0, sizeof(sample));
	provider->sample(1, &sample);
	CHECK(sample.guaranteed == 18U);
	CHECK((sample.flags & KERN_CPUPERF_SAMPLE_PLATFORM_LIMIT) != 0U);
	CHECK(cpus[1].hwp_status == 0U);
	CHECK(faults == 0U);

	/* The delivered rate is IA32_APERF over IA32_MPERF times 1600 MHz. */
	cpus[1].aperf = 1000U;
	cpus[1].mperf = 1000U;
	memset(&sample, 0, sizeof(sample));
	provider->sample(1, &sample);
	cpus[1].aperf += 2750000U;
	cpus[1].mperf += 1000000U;
	memset(&sample, 0, sizeof(sample));
	provider->sample(1, &sample);
	CHECK(sample.delivered_mhz == 4400U);

	/* The measurement of the highest rate. */
	current_cpu = 4;
	mhz = 0U;
	CHECK(provider->measure(4, false, &mhz) == ENODATA);
	CHECK(provider->measure(4, true, &mhz) == 0);
	cpus[4].aperf += 2062500U;
	cpus[4].mperf += 1000000U;
	CHECK(provider->measure(4, false, &mhz) == 0);
	CHECK(mhz == 3300U);
	CHECK(provider->measure(4, false, &mhz) == ENODATA);

	/* Large advances do not overflow. */
	CHECK(provider->measure(4, true, &mhz) == 0);
	cpus[4].aperf += 0x4000000000000000ULL;
	cpus[4].mperf += 0x2000000000000000ULL;
	CHECK(provider->measure(4, false, &mhz) == 0);
	CHECK(mhz == 3200U);

	/* The meters read the raw counters. */
	CHECK(meter_ops[0]->read(meter_contexts[0], &count) == 0 && count == 1000U);
	CHECK(meter_ops[1]->read(meter_contexts[1], &count) == 0 && count == 400U);
	CHECK(meter_ops[2]->read(meter_contexts[2], &count) == 0 && count == 100U);
	CHECK(faults == 0U);
}

/* The hardware feedback, the residency, and the start on the second thread of a core. */
static void
scenario_5330_feedback(void)
{
	struct kern_cpuperf_sample sample;
	struct kern_cpuperf_idle idle;
	uint32_t available;
	uint32_t efficiency;
	uint32_t value;
	uint32_t cpu;
	bool changed;

	machine_5330("5330-feedback", "");
	CHECK(run_init() == 0);

	/* Before the start the feedback reads nothing and touches no register. */
	current_cpu = 0;
	CHECK(provider->feedback(0, &changed) == ENODATA && !changed);
	CHECK(provider->feedback_read(0, &available, &efficiency) == ENODATA);
	CHECK(reads == 0U || faults == 0U);
	CHECK(faults == 0U);

	/* Starts every CPU; the table is enabled once. */
	for (cpu = 0; cpu < 12U; cpu++) {
		current_cpu = cpu;
		memset(&sample, 0, sizeof(sample));
		CHECK(provider->start(cpu, &sample) == 0);
	}
	CHECK(packages[0].hfi_config == 1U);

	/* The hardware has not updated yet. */
	current_cpu = 3;
	CHECK(provider->feedback(3, &changed) == 0 && !changed);

	/* The hardware fills the table (header 16 bytes, rows of 8) and sets bit 26. */
	memset(table_memory, 0, table_size);
	for (cpu = 0; cpu < 12U; cpu++) {
		table_memory[16U + cpu * 8U] = cpu < 4U ? 255U : 180U;
		table_memory[16U + cpu * 8U + 1U] = cpu < 4U ? 100U : 200U;
	}
	packages[0].therm_status = 1ULL << 26 | 1ULL << 1;
	CHECK(provider->feedback(3, &changed) == 0 && changed);
	CHECK((packages[0].therm_status & (1ULL << 26)) == 0U);
	CHECK(provider->feedback_read(0, &available, &efficiency) == 0);
	CHECK(available == 255U && efficiency == 100U);
	CHECK(provider->feedback_read(7, &available, &efficiency) == 0);
	CHECK(available == 180U && efficiency == 200U);
	CHECK(provider->relative(9, KERN_CPUPERF_RELATIVE_FEEDBACK, &value) == 0 && value == 180U);
	CHECK(provider->relative(9, KERN_CPUPERF_RELATIVE_FREQUENCY, &value) == EOPNOTSUPP);

	/* The copy is the driver's: a later change of the table is not seen until the next update. */
	table_memory[16U] = 1U;
	CHECK(provider->feedback_read(0, &available, &efficiency) == 0 && available == 255U);

	/* The residency: four package states and three core states, in nanoseconds of the 1600 MHz TSC. */
	packages[0].pkg_c[2] = 1600000000ULL;
	cpus[2].core_c[1] = 3200000000ULL;
	current_cpu = 0;
	memset(&idle, 0, sizeof(idle));
	CHECK(provider->idle_package(0, &idle) == 0);
	CHECK(idle.count == 4U && idle.state[0] == 2U && idle.state[2] == 6U && idle.state[3] == 7U);
	CHECK(idle.residency_ns[2] == 1000000000ULL);
	current_cpu = 2;
	memset(&idle, 0, sizeof(idle));
	CHECK(provider->idle_core(2, &idle) == 0);
	CHECK(idle.count == 3U && idle.state[0] == 3U && idle.state[1] == 6U);
	CHECK(idle.residency_ns[1] == 2000000000ULL);
	CHECK(idle.available != 0U);
	CHECK(faults == 0U);
}

/* HWP the firmware left on: the anchors come with the topology, the start writes no enable. */
static void
scenario_hwp_on_at_boot(void)
{
	struct kern_cpuperf_sample sample;
	uint32_t index;

	machine_5330("hwp-on-at-boot", "cpuperf.hwp=off");
	packages[0].pm_enable = 1U;
	CHECK(run_init() == 0);
	CHECK(faults == 0U && writes == 0U);
	CHECK(levels[0].highest == 56U && levels[0].guaranteed == 21U);
	CHECK(levels[0].efficient == 20U && levels[0].lowest == 1U);
	CHECK(levels[6].highest == 33U);
	CHECK(provider->apply != NULL);
	CHECK(strstr(log_text, "cannot turn it off") != NULL);
	current_cpu = 0;
	memset(&sample, 0, sizeof(sample));
	CHECK(provider->start(0, &sample) == 0);
	for (index = 0; index < write_log_count; index++)
		CHECK(write_log_msr[index] != MSR_PM_ENABLE);
	CHECK(faults == 0U);
}

/* cpuperf.hwp=off on a processor whose HWP is still off: described, not controlled. */
static void
scenario_hwp_off(void)
{
	machine_5330("hwp-off", "cpuperf.hwp=off");
	CHECK(run_init() == 0);
	CHECK(faults == 0U && writes == 0U);
	CHECK(provider->apply == NULL && provider->start == NULL);
	CHECK(provider->feedback == NULL);
	CHECK(levels[0].kind == 0U);
	CHECK(provider->idle_package != NULL);
	CHECK(meter_count == 3U);
	CHECK(table_allocations == 0U);
}

/* Under a hypervisor: the topology only, not a single register. */
static void
scenario_hypervisor(void)
{
	uint32_t cpu;

	machine_5330("hypervisor", "");
	machine.hypervisor = true;
	CHECK(run_init() == 0);
	CHECK(reads == 0U && writes == 0U && faults == 0U);
	CHECK(provider != NULL);
	CHECK(provider->start == NULL && provider->apply == NULL && provider->sample == NULL);
	CHECK(provider->measure == NULL && provider->feedback == NULL && provider->idle_package == NULL);
	CHECK(provider->idle_core == NULL);
	for (cpu = 0; cpu < 12U; cpu++)
		CHECK(levels[cpu].kind == 0U);
	CHECK(topologies[4].core_class == KERN_CPUPERF_CORE_EFFICIENCY);
	CHECK(meter_count == 0U && table_allocations == 0U);
	CHECK(strstr(log_text, "hypervisor") != NULL);
}

/* HWP on a model off the allow list: controlled, but no model-specific register is read. */
static void
scenario_unlisted(void)
{
	struct kern_cpuperf_sample sample;
	struct kern_cpuperf_request request;

	machine_5330("unlisted", "");
	machine.family_model_eax = 0x000806C1U;	/* 06_8CH */
	machine.listed = false;
	CHECK(run_init() == 0);
	CHECK(faults == 0U && writes == 0U);
	CHECK(provider->apply != NULL);
	CHECK(provider->measure == NULL);	/* no reference frequency */
	CHECK(provider->idle_package == NULL && provider->idle_core == NULL);
	CHECK(meter_count == 0U);
	current_cpu = 0;
	memset(&sample, 0, sizeof(sample));
	CHECK(provider->start(0, &sample) == 0);
	request.minimum = 1U;
	request.maximum = 56U;
	request.desired = 0U;
	request.preference = 0x80U;
	CHECK(provider->apply(0, &request) == 0);
	CHECK(faults == 0U);
}

/* Intel without HWP, without the feedback, without EPP. */
static void
scenario_no_hwp(void)
{
	machine_5330("no-hwp", "");
	machine.power_eax = 1U | P_ARAT;
	machine.hybrid = false;
	CHECK(run_init() == 0);
	CHECK(faults == 0U && writes == 0U);
	CHECK(provider->apply == NULL && provider->start == NULL && provider->feedback == NULL);
	CHECK(provider->sample != NULL);	/* the delivered rate */
	CHECK(levels[0].kind == 0U);
	CHECK(topologies[5].core_class == KERN_CPUPERF_CORE_PERFORMANCE);
	CHECK(meter_count == 3U);
}

/* HWP without EPP: the preference field is written as 0. */
static void
scenario_no_epp(void)
{
	struct kern_cpuperf_sample sample;
	struct kern_cpuperf_request request;

	machine_5330("no-epp", "");
	machine.power_eax &= ~(P_HWP_EPP | P_HFI | P_TD);
	CHECK(run_init() == 0);
	current_cpu = 0;
	memset(&sample, 0, sizeof(sample));
	CHECK(provider->start(0, &sample) == 0);
	request.minimum = 1U;
	request.maximum = 56U;
	request.desired = 30U;
	request.preference = 0xC0U;
	CHECK(provider->apply(0, &request) == 0);
	CHECK(cpus[0].request == (1ULL | 56ULL << 8 | 30ULL << 16));
	CHECK(provider->feedback == NULL && table_allocations == 0U);
	CHECK(faults == 0U);
}

/* A feedback interface some earlier software left on is not taken over. */
static void
scenario_hfi_on_at_boot(void)
{
	struct kern_cpuperf_sample sample;
	bool changed;

	machine_5330("hfi-on-at-boot", "");
	packages[0].hfi_ptr = 0x20000001ULL;
	packages[0].hfi_config = 1U;
	CHECK(run_init() == 0);
	current_cpu = 0;
	memset(&sample, 0, sizeof(sample));
	CHECK(provider->start(0, &sample) == 0);
	CHECK(packages[0].hfi_ptr == 0x20000001ULL);
	packages[0].therm_status = 1ULL << 26;
	CHECK(provider->feedback(0, &changed) == ENODATA && !changed);
	CHECK(faults == 0U);
}

/* Two packages: one table each would be needed, the meters are not offered. */
static void
scenario_two_packages(void)
{
	uint32_t index;

	machine_5330("two-packages", "");
	for (index = 6; index < 12U; index++)
		cpus[index].x2apic += 256U;
	machine.packages = 2U;
	CHECK(run_init() == 0);
	CHECK(topologies[0].package != topologies[11].package);
	CHECK(meter_count == 0U);
	CHECK(table_allocations == 2U);	/* the model gives memory to the first only */
	CHECK(faults == 0U && writes == 0U);
}

#endif

/* Without the performance control: the description only, and not one register. */
static void __attribute__((unused))
scenario_without_control(void)
{
	uint32_t cpu;

	machine_5330("without-control", "");
	CHECK(run_init() == 0);
	CHECK(reads == 0U && writes == 0U && faults == 0U);
	CHECK(provider->start == NULL && provider->apply == NULL && provider->sample == NULL);
	CHECK(provider->feedback == NULL && provider->idle_package == NULL && provider->idle_core == NULL);
	for (cpu = 0; cpu < 12U; cpu++)
		CHECK(levels[cpu].kind == 0U);
	CHECK(topologies[0].core == topologies[1].core);
	CHECK(topologies[4].module == topologies[7].module);
	CHECK(topologies[4].core_class == KERN_CPUPERF_CORE_EFFICIENCY);
	CHECK(meter_count == 0U && table_allocations == 0U);
}

struct scenario {
	const char *name;
	void (*run)(void);
};

static const struct scenario scenarios[] = {
#if !CONFIG_CPU_PERF
	{ "without-control", scenario_without_control },
	{ "cpuperf-off", scenario_off },
	{ "not-intel", scenario_not_intel },
#else
	{ "5330-topology", scenario_5330_topology },
	{ "5330-control", scenario_5330_control },
	{ "5330-feedback", scenario_5330_feedback },
	{ "hwp-on-at-boot", scenario_hwp_on_at_boot },
	{ "hwp-off", scenario_hwp_off },
	{ "cpuperf-off", scenario_off },
	{ "hypervisor", scenario_hypervisor },
	{ "not-intel", scenario_not_intel },
	{ "unlisted", scenario_unlisted },
	{ "no-hwp", scenario_no_hwp },
	{ "no-epp", scenario_no_epp },
	{ "hfi-on-at-boot", scenario_hfi_on_at_boot },
	{ "two-packages", scenario_two_packages },
#endif
};

int
main(void)
{
	size_t index;
	int status;
	int total_failed;
	pid_t child;

	total_failed = 0;
	for (index = 0; index < sizeof(scenarios) / sizeof(scenarios[0]); index++) {
		fflush(stdout);
		child = fork();
		if (child == 0) {
			scenarios[index].run();
			if (faults != 0U)
				fprintf(stderr, "  fault: cpu %u msr 0x%x %s 0x%llx\n", last_fault.cpu, last_fault.msr,
					last_fault.write ? "write" : "read", (unsigned long long)last_fault.value);
			printf("%-16s checks %u failures %u\n", scenarios[index].name, checks, failures);
			fflush(stdout);
			_exit(failures == 0U ? 0 : 1);
		}
		waitpid(child, &status, 0);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			printf("%-16s FAIL\n", scenarios[index].name);
			total_failed++;
		}
	}
	if (total_failed != 0) {
		printf("scenarios: FAIL\n");
		return 1;
	}
	printf("scenarios: PASS\n");
	return 0;
}
