/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 xAPIC controller, periodic timer, and CPU notification path.
 */

#include <hal/hal.h>
#include "lapic.h"
#include "acpi.h"
#include "early-init-policy.h"
#include "timecounter.h"
#include "../asm.h"
#include "../defs.h"
#include "../space.h"

#define LAPIC_ID            0x020U
#define LAPIC_VERSION       0x030U
#define LAPIC_LVT_CMCI      0x2f0U
#define LAPIC_LVT_THERMAL   0x330U
#define LAPIC_LVT_PERF      0x340U
#define LAPIC_LVT_LINT0     0x350U
#define LAPIC_LVT_LINT1     0x360U
#define LAPIC_EOI           0x0b0U
#define LAPIC_SVR           0x0f0U
#define LAPIC_ESR           0x280U
#define LAPIC_ICR_LOW       0x300U
#define LAPIC_ICR_HIGH      0x310U
#define LAPIC_LVT_TIMER     0x320U
#define LAPIC_LVT_ERROR     0x370U
#define LAPIC_TIMER_INITIAL 0x380U
#define LAPIC_TIMER_CURRENT 0x390U
#define LAPIC_TIMER_DIVIDE  0x3e0U

#define LAPIC_ENABLE        0x100U
#define LAPIC_MASKED        0x10000U

/* An LVT entry's delivery mode (bits 8 to 10): fixed is the only one quieted. */
#define LAPIC_DELIVERY_MASK  0x700U
#define LAPIC_DELIVERY_FIXED 0x000U

/* The highest LVT entry the version register reports, and the counts that add the optional entries. */
#define LAPIC_MAX_LVT_SHIFT  16U
#define LAPIC_MAX_LVT_MASK   0xffU
#define LAPIC_LVT_WITH_PERF  4U
#define LAPIC_LVT_WITH_THERMAL 5U
#define LAPIC_LVT_WITH_CMCI  6U
#define LAPIC_PERIODIC      0x20000U
#define LAPIC_TSC_DEADLINE  0x40000U
#define ICR_PENDING         0x1000U
#define PIT_POLL_LIMIT      1000000U
#define PIT_INPUT_HZ        1193182ULL
#define PIT_CAL_TICKS       11932U

/*
 * How many PIT windows calibrate the local APIC timer.  The shortest one is
 * kept: a delay can only lengthen a window as the APIC timer sees it (a
 * virtual CPU descheduled by its host between a gate edge and the sample
 * that brackets it), never shorten it (one window was once measured at 17
 * times its length, and the tick ran 17 times slow).
 */
#define TIMER_CAL_WINDOWS   3U

/* How many calibration windows make one second: each window is 10 ms. */
#define TIMER_CAL_WINDOWS_PER_SECOND 100U

/*
 * The most TSC samples one TSC-timed window takes before the TSC is taken to
 * have stopped.  Each poll is a serialized TSC read and a pause, so this
 * bound lasts far longer than 10 ms at any rate the TSC policy accepts.
 */
#define TSC_POLL_LIMIT      100000000U

#define CPUID_ARCH_CAPABILITIES       (1U << 29)
#define ARCH_CAP_XAPIC_DISABLE_STATUS (1ULL << 21)
#define MSR_IA32_APIC_BASE            0x01bU
#define MSR_IA32_ARCH_CAPABILITIES    0x10aU
#define MSR_IA32_XAPIC_DISABLE_STATUS 0x0bdU
#define MSR_IA32_TSC_DEADLINE         0x6e0U

/* CPUID leaf 1's ECX: the local APIC timer has the TSC-deadline mode. */
#define CPUID_TSC_DEADLINE            (1U << 24)

/*
 * Whether the tick is timed by the TSC-deadline mode, with every CPU's
 * deadlines at the multiples of one tick period on the shared TSC.  Every
 * CPU then takes its tick at the same moment and sleeps in the same gap,
 * so the package can stay in a deep state for most of the period; a
 * periodic timer started on each CPU at its own moment spreads the ticks
 * over the period instead.  0 keeps the periodic timer on every CPU, as a
 * build to compare against.
 */
#ifndef LAPIC_TICK_TSC_DEADLINE
#define LAPIC_TICK_TSC_DEADLINE       1
#endif

static volatile uint32_t *lapic;
static uint32_t timer_initial;
static uint32_t lapic_physical_address;

/*
 * The TSC count of one tick when the TSC-deadline mode may time the tick,
 * else 0; chosen once on the BSP with the calibration.
 */
static uint64_t tick_period_tsc;

/*
 * Which CPUs' ticks the TSC-deadline mode times now: each such CPU arms its
 * next deadline at every tick.  Only the CPU itself writes its entry.
 */
static bool tick_deadline[AMD64_SMP_MAX_CPUS];

static uint32_t read_reg(unsigned offset);
static void write_reg(unsigned offset, uint32_t value);
static void cpuid_count(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
static enum amd64_apic_policy_result preflight_cpu(uint32_t expected_apic_id, uint32_t physical_address, uint32_t *actual_apic_id);
static int acpi_has_apic_id(const struct amd64_acpi_info *acpi, uint32_t apic_id);
static int wait_icr(void);
static void init_registers(void);
static int calibrate_timer(void);
static void choose_tick_source(void);
static bool cpu_has_tsc_deadline(void);
static void arm_next_deadline(void);
static int tsc_wait_window(uint64_t window_ticks, unsigned *polls, const char **stage, uint32_t *lapic_start, uint32_t *lapic_end);
static int pit_wait_level(struct amd64_pit_poll *poll, int expected_high, uint8_t *last_port61);
static int pit_wait_10ms(uint8_t *last_port61, unsigned *polls, const char **stage, int measure_tsc, uint64_t *tsc_start, uint64_t *tsc_end, uint32_t *lapic_start, uint32_t *lapic_end);
static int send_icr(uint32_t apic_id, uint32_t low);
static uint32_t quiet_entry(unsigned offset);
static uint32_t quiet_line(unsigned offset);

/*
 * Initializes the BSP local APIC from ACPI topology.
 */
int
prekern_amd64_lapic_init(
	const struct amd64_acpi_info *acpi)
{
	void *mmio;
	uint32_t apic_id;
	uint32_t mmio_id;
	enum amd64_apic_policy_result policy;
	int present;
	int error;

	/* Requires an ACPI topology before CPU preflight. */
	if (acpi == NULL)
		return HAL_ERR_INVALID;

	/* Validates the CPU's xAPIC mode and MADT base. */
	policy = preflight_cpu(
		AMD64_APIC_EXPECT_ANY,
		acpi->lapic_address,
		&apic_id);
	if (policy != AMD64_APIC_POLICY_OK)
		return HAL_ERR_UNSUPPORTED;

	/* Requires the running BSP APIC identifier in the MADT CPU set. */
	present = acpi_has_apic_id(acpi, apic_id);
	if (!present) {
		hal_printf(
			"A64 APIC PREFLIGHT FAIL id=%u result=madt-id-missing\n",
			apic_id);
		return HAL_ERR_UNSUPPORTED;
	}

	/* Maps the uncached local APIC MMIO page. */
	error = amd64_device_map(acpi->lapic_address, 4096, &mmio);
	if (error != HAL_OK) {
		hal_puts("A64 LAPIC MAP FAIL\n");
		return HAL_ERR_UNSUPPORTED;
	}

	/* Publishes the persistent MMIO mapping and physical base. */
	lapic = mmio;
	lapic_physical_address = acpi->lapic_address;

	/* Verifies the volatile MMIO identity against CPUID. */
	mmio_id = amd64_lapic_id();
	if (mmio_id != apic_id) {
		mmio_id = amd64_lapic_id();
		hal_printf(
			"A64 LAPIC ID FAIL cpuid=%u mmio=%u\n",
			apic_id,
			mmio_id);
		return HAL_ERR_UNSUPPORTED;
	}

	/* Enables local APIC service and error vectors. */
	init_registers();
	hal_printf(
		"A64 LAPIC READY id=%u base=%08X\n",
		apic_id,
		lapic_physical_address);

	/* Reports successful BSP local APIC initialization. */
	return HAL_OK;
}

/*
 * Initializes local APIC state on one secondary CPU.
 */
int
amd64_lapic_init_secondary(
	uint32_t expected_apic_id,
	unsigned *failure_reason)
{
	uint32_t actual_apic_id;
	uint32_t mmio_id;
	enum amd64_apic_policy_result policy;

	/* Requires a destination for a concrete preflight reason. */
	if (failure_reason == NULL)
		return HAL_ERR_INVALID;
	*failure_reason = AMD64_APIC_POLICY_OK;

	/* Requires the BSP to have published the shared mapping. */
	if (lapic == NULL) {
		*failure_reason = AMD64_APIC_POLICY_INVALID_MODE;
		return HAL_ERR_STATE;
	}

	/* Validates this CPU's xAPIC mode, base, and expected identity. */
	policy = preflight_cpu(
		expected_apic_id,
		lapic_physical_address,
		&actual_apic_id);
	if (policy != AMD64_APIC_POLICY_OK) {
		*failure_reason = (unsigned)policy;
		return HAL_ERR_UNSUPPORTED;
	}

	/* Verifies the volatile MMIO identity against CPU preflight. */
	mmio_id = amd64_lapic_id();
	if (mmio_id != actual_apic_id) {
		mmio_id = amd64_lapic_id();
		hal_printf(
			"A64 LAPIC AP ID FAIL expected=%u mmio=%u\n",
			actual_apic_id,
			mmio_id);
		*failure_reason = AMD64_APIC_POLICY_ID_MISMATCH;
		return HAL_ERR_UNSUPPORTED;
	}

	/* Enables local APIC service and error vectors on this CPU. */
	init_registers();

	/* Reports successful secondary local APIC initialization. */
	return HAL_OK;
}

/*
 * Reads the current CPU's xAPIC identifier.
 */
uint32_t
amd64_lapic_id(
	void)
{
	uint32_t value;

	/* Samples the volatile local APIC identifier register. */
	value = read_reg(LAPIC_ID);

	/* Returns its eight-bit xAPIC destination field. */
	return value >> 24;
}

/*
 * Acknowledges the current local APIC interrupt.
 */
void
amd64_lapic_eoi(
	void)
{
	/* Writes the local APIC end-of-interrupt register. */
	write_reg(LAPIC_EOI, 0);
}

/*
 * Starts the calibrated periodic local APIC timer.
 *
 * The BSP calibrates the period once; every later call, on a secondary CPU
 * or after a system sleep, programs the same period.
 */
int
amd64_lapic_timer_start(
	void)
{
	hal_cpu_id_t cpu;
	bool deadline;
	int error;

	/* Calibrates the shared timer period once on the BSP and chooses how ticks are timed. */
	if (timer_initial == 0) {
		error = calibrate_timer();
		if (error != HAL_OK)
			return error;
		choose_tick_source();
	}

	/* Times this CPU's tick by TSC deadlines when the BSP chose them and this CPU has the mode. */
	cpu = hal_cpu_current();
	deadline = false;
	if (tick_period_tsc != 0U)
		deadline = cpu_has_tsc_deadline();
	tick_deadline[cpu] = deadline;

	/*
	 * Arms the first deadline at the next multiple of the period.  The
	 * LVT write is fenced (write_reg) before the deadline MSR write, so
	 * the deadline is not written while the timer is still in another mode,
	 * where the write would be ignored.
	 */
	if (deadline) {
		write_reg(LAPIC_LVT_TIMER, LAPIC_TSC_DEADLINE | INT_IRQ_BASE);
		arm_next_deadline();
	}

	/* Otherwise programs the calibrated periodic timer on the current CPU. */
	if (!deadline) {
		write_reg(LAPIC_TIMER_DIVIDE, 0x3U);
		write_reg(LAPIC_LVT_TIMER, LAPIC_PERIODIC | INT_IRQ_BASE);
		write_reg(LAPIC_TIMER_INITIAL, timer_initial);
	}

	/* Succeeded: the local timer is running. */
	return HAL_OK;
}

/*
 * Arms the current CPU's next tick when the TSC-deadline mode times it.
 *
 * Called at every tick of the CPU, before the tick's work.  A periodic timer
 * rearms itself, and then nothing is done.
 */
void
amd64_lapic_timer_tick(
	hal_cpu_id_t cpu)
{
	/* A periodic tick needs nothing. */
	if (!tick_deadline[cpu])
		return;

	/* The next deadline, at the next multiple of the period. */
	arm_next_deadline();
}

/*
 * Stops the current CPU's local APIC timer.
 */
void
amd64_lapic_timer_stop(
	void)
{
	/*
	 * Masks the timer vector before clearing its initial count.  Leaving
	 * the TSC-deadline mode also disarms a deadline, and a deadline write
	 * from a tick still pending is ignored outside that mode.
	 */
	tick_deadline[hal_cpu_current()] = false;
	write_reg(LAPIC_LVT_TIMER, LAPIC_MASKED | INT_IRQ_BASE);
	write_reg(LAPIC_TIMER_INITIAL, 0);
}

/*
 * Quiets the current CPU's local interrupt sources for a system sleep:
 * the corrected machine-check, thermal, performance-counter
 * and error entries, and the two local lines when they deliver a fixed
 * vector (an NMI or ExtINT line is left alone), each masked with its value
 * kept in saved for amd64_lapic_restore_sources().
 */
void
amd64_lapic_quiet_sources(
	struct amd64_lapic_quiet *saved)
{
	uint32_t version;
	uint32_t highest;

	/* The optional entries this local APIC has, from the highest LVT entry it reports. */
	version = read_reg(LAPIC_VERSION);
	highest = (version >> LAPIC_MAX_LVT_SHIFT) & LAPIC_MAX_LVT_MASK;
	saved->has_performance = 0U;
	if (highest >= LAPIC_LVT_WITH_PERF)
		saved->has_performance = 1U;
	saved->has_thermal = 0U;
	if (highest >= LAPIC_LVT_WITH_THERMAL)
		saved->has_thermal = 1U;
	saved->has_cmci = 0U;
	if (highest >= LAPIC_LVT_WITH_CMCI)
		saved->has_cmci = 1U;

	/* The optional entries, masked. */
	saved->performance = 0U;
	if (saved->has_performance)
		saved->performance = quiet_entry(LAPIC_LVT_PERF);
	saved->thermal = 0U;
	if (saved->has_thermal)
		saved->thermal = quiet_entry(LAPIC_LVT_THERMAL);
	saved->cmci = 0U;
	if (saved->has_cmci)
		saved->cmci = quiet_entry(LAPIC_LVT_CMCI);

	/* The local lines, only when they deliver a fixed vector, and the error entry. */
	saved->lint0 = quiet_line(LAPIC_LVT_LINT0);
	saved->lint1 = quiet_line(LAPIC_LVT_LINT1);
	saved->error = quiet_entry(LAPIC_LVT_ERROR);
}

/*
 * Puts back the local interrupt sources amd64_lapic_quiet_sources() masked,
 * each to the value it had.
 */
void
amd64_lapic_restore_sources(
	const struct amd64_lapic_quiet *saved)
{
	/* The error entry and the local lines. */
	write_reg(LAPIC_LVT_ERROR, saved->error);
	write_reg(LAPIC_LVT_LINT1, saved->lint1);
	write_reg(LAPIC_LVT_LINT0, saved->lint0);

	/* The optional entries this local APIC has. */
	if (saved->has_cmci)
		write_reg(LAPIC_LVT_CMCI, saved->cmci);
	if (saved->has_thermal)
		write_reg(LAPIC_LVT_THERMAL, saved->thermal);
	if (saved->has_performance)
		write_reg(LAPIC_LVT_PERF, saved->performance);
}

/*
 * Sends an INIT assertion and deassertion to one APIC destination.
 */
int
amd64_lapic_send_init(
	uint32_t apic_id)
{
	volatile unsigned delay;
	int error;

	/* Sends the INIT level assertion. */
	error = send_icr(apic_id, 0x0000c500U);
	if (error != HAL_OK)
		return error;

	/* Holds the asserted level for a bounded processor delay. */
	for (delay = 0; delay < 100000U; delay++)
		__asm__ volatile("pause");

	/* Sends the INIT level deassertion. */
	error = send_icr(apic_id, 0x00008500U);

	/* Returns the deassertion result unchanged. */
	return error;
}

/*
 * Sends a startup interrupt to one APIC destination.
 */
int
amd64_lapic_send_startup(
	uint32_t apic_id,
	uint8_t vector)
{
	int error;

	/* Sends the startup delivery-mode ICR. */
	error = send_icr(apic_id, 0x00004600U | vector);

	/* Returns the delivery result unchanged. */
	return error;
}

/*
 * Sends a scheduler notification to one APIC destination.
 */
int
amd64_lapic_notify(
	uint32_t apic_id)
{
	int error;

	/* Sends the fixed notification vector. */
	error = send_icr(apic_id, AMD64_VECTOR_NOTIFY);

	/* Returns the delivery result unchanged. */
	return error;
}

/*
 * Sends one valid fixed vector to an APIC destination.
 */
int
amd64_lapic_send_vector(
	uint32_t apic_id,
	uint8_t vector)
{
	int error;

	/* Rejects vectors reserved for architectural exceptions. */
	if (vector < 0x20U)
		return HAL_ERR_INVALID;

	/* Sends the requested fixed delivery vector. */
	error = send_icr(apic_id, vector);

	/* Returns the delivery result unchanged. */
	return error;
}

/*
 * Broadcasts a panic NMI and halts the current CPU.
 */
void __attribute__((noreturn))
amd64_lapic_panic_all(
	void)
{
	/* Waits best-effort for an earlier ICR before broadcasting the NMI. */
	(void)wait_icr();
	write_reg(LAPIC_ICR_HIGH, 0);
	write_reg(LAPIC_ICR_LOW, 0x000c0400U);
	(void)hal_irq_disable();

	/* Halts the panic-broadcasting CPU permanently. */
	for (;;)
		asm_hlt();
}

/* Reads one volatile local APIC register. */
static uint32_t
read_reg(
	unsigned offset)
{
	uint32_t value;

	/* Samples the selected volatile MMIO register. */
	value = lapic[offset / 4U];

	/* Returns the register sample. */
	return value;
}

/* Writes and orders one volatile local APIC register. */
static void
write_reg(
	unsigned offset,
	uint32_t value)
{
	/* Publishes the volatile write before later I/O operations. */
	lapic[offset / 4U] = value;
	hal_io_mb();
}

/* Executes one counted CPUID query and returns every register. */
static void
cpuid_count(
	uint32_t leaf,
	uint32_t subleaf,
	uint32_t *eax,
	uint32_t *ebx,
	uint32_t *ecx,
	uint32_t *edx)
{
	uint32_t a;
	uint32_t b;
	uint32_t c;
	uint32_t d;

	/* Executes CPUID with the requested leaf and subleaf. */
	a = leaf;
	c = subleaf;
	__asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));

	/* Publishes the four architectural result registers. */
	*eax = a;
	*ebx = b;
	*ecx = c;
	*edx = d;
}

/* Evaluates xAPIC policy against one CPU's architectural state. */
static enum amd64_apic_policy_result
preflight_cpu(
	uint32_t expected_apic_id,
	uint32_t physical_address,
	uint32_t *actual_apic_id)
{
	struct amd64_apic_policy_input input;
	const char *format;
	const char *result_name;
	uint64_t capabilities;
	uint32_t max_leaf;
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	unsigned lock_status;
	enum amd64_apic_policy_result result;

	/* Initializes policy input and queries the maximum basic leaf. */
	hal_memset(&input, 0, sizeof(input));
	cpuid_count(0, 0, &max_leaf, &ebx, &ecx, &edx);

	/* Collects the APIC feature bits and base MSR when supported. */
	if (max_leaf >= 1U) {
		cpuid_count(
			1,
			0,
			&eax,
			&input.cpuid1_ebx,
			&input.cpuid1_ecx,
			&input.cpuid1_edx);
		UNUSED_PARAMETER(eax);

		/* Reads the APIC base only when CPUID enumerates the feature. */
		if ((input.cpuid1_edx & (1U << 9)) != 0)
			input.apic_base = asm_read_msr(MSR_IA32_APIC_BASE);
	}

	/* Collects the architectural legacy-xAPIC lock status when present. */
	if ((input.cpuid1_edx & (1U << 9)) != 0 && max_leaf >= 7U) {
		cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);

		/* Reads architectural capabilities only when CPUID advertises them. */
		if ((edx & CPUID_ARCH_CAPABILITIES) != 0) {
			capabilities = asm_read_msr(MSR_IA32_ARCH_CAPABILITIES);

			/* Reads lock status only when its capability is present. */
			if ((capabilities & ARCH_CAP_XAPIC_DISABLE_STATUS) != 0) {
				input.xapic_disable_status =
				    asm_read_msr(MSR_IA32_XAPIC_DISABLE_STATUS);
				input.lock_status_valid = 1;
			}
		}
	}

	/* Evaluates the complete policy input. */
	input.madt_base = physical_address;
	input.expected_apic_id = expected_apic_id;
	result = amd64_apic_policy_evaluate(&input, actual_apic_id);

	/* Selects the stable diagnostic format and policy name. */
	if (result == AMD64_APIC_POLICY_OK) {
		format = "A64 APIC PREFLIGHT PASS id=%u base=%08X:%08X "
		    "madt=%08X lock=%u result=%s\n";
	} else {
		format = "A64 APIC PREFLIGHT FAIL id=%u base=%08X:%08X "
		    "madt=%08X lock=%u result=%s\n";
	}

	/* Selects the printable lock bit only when the MSR was valid. */
	if (input.lock_status_valid)
		lock_status = (unsigned)(input.xapic_disable_status & 1U);
	else
		lock_status = 0U;
	result_name = amd64_apic_policy_result_name(result);
	hal_printf(
		format,
		*actual_apic_id,
		(uint32_t)(input.apic_base >> 32),
		(uint32_t)input.apic_base,
		physical_address,
		lock_status,
		result_name);

	/* Returns the evaluated APIC policy result. */
	return result;
}

/* Reports whether the MADT contains one APIC identifier. */
static int
acpi_has_apic_id(
	const struct amd64_acpi_info *acpi,
	uint32_t apic_id)
{
	unsigned index;

	/* Searches every enabled processor record in MADT order. */
	for (index = 0; index < acpi->cpu_count; index++) {
		/* Reports the first matching APIC identifier. */
		if (acpi->cpus[index].apic_id == apic_id)
			return 1;
	}

	/* Reports an APIC identifier absent from the topology. */
	return 0;
}

/* Waits for the local APIC interrupt-command register to become idle. */
static int
wait_icr(
	void)
{
	uint32_t value;
	unsigned count;

	/* Polls the volatile delivery-status bit for a bounded interval. */
	for (count = 0; count < 1000000U; count++) {
		value = read_reg(LAPIC_ICR_LOW);

		/* Reports readiness as soon as delivery is no longer pending. */
		if ((value & ICR_PENDING) == 0)
			return HAL_OK;
	}

	/* Reports expiration of the ICR delivery wait. */
	return HAL_ERR_TIMEOUT;
}

/* Enables the local APIC and its error vector. */
static void
init_registers(
	void)
{
	/* Programs service before clearing any prior error state twice. */
	write_reg(LAPIC_SVR, LAPIC_ENABLE | AMD64_VECTOR_SPURIOUS);
	write_reg(LAPIC_LVT_ERROR, AMD64_VECTOR_ERROR);
	write_reg(LAPIC_ESR, 0);
	write_reg(LAPIC_ESR, 0);
}

/*
 * Measures the local APIC timer's count over 10 ms and publishes the period.
 *
 * The reference that times the 10 ms is the TSC when CPUID has already given
 * its rate, and the PIT otherwise.  The PIT is not used when it is not needed:
 * a Core i5-13400F desktop's PIT output never rose, and the boot stopped.
 * A PIT window also measures the TSC's rate when CPUID did not.
 */
static int
calibrate_timer(
	void)
{
	const char *stage;
	uint64_t tsc_frequency;
	uint64_t tsc_window;
	uint64_t tsc_start;
	uint64_t tsc_end;
	uint64_t window_tsc_start;
	uint64_t window_tsc_end;
	uint32_t lapic_start;
	uint32_t lapic_end;
	uint32_t window_elapsed[TIMER_CAL_WINDOWS];
	uint32_t elapsed;
	uint32_t current;
	uint8_t port61;
	unsigned polls;
	unsigned window;
	enum amd64_tsc_frequency_policy_result tsc_policy;
	bool tsc_reference;
	int measure_tsc;
	int error;

	/* Starts with no window measured and no reference chosen. */
	tsc_frequency = 0U;
	tsc_window = 0U;
	tsc_start = 0U;
	tsc_end = 0U;
	elapsed = 0xffffffffU;
	stage = "complete";
	port61 = 0U;
	polls = 0U;
	hal_puts("A64 TIMER CAL BEGIN\n");

	/* Learns whether CPUID gave the TSC's rate or a PIT window must measure it. */
	tsc_policy = amd64_timecounter_bsp_prepare();
	measure_tsc = 0;
	if (tsc_policy == AMD64_TSC_FREQUENCY_NEEDS_PIT)
		measure_tsc = 1;

	/* Takes the TSC's rate as the reference when CPUID already gave it. */
	tsc_reference = false;
	if (tsc_policy == AMD64_TSC_FREQUENCY_READY)
		tsc_reference = amd64_timecounter_bsp_candidate_frequency(&tsc_frequency);

	/* Converts that rate into the TSC count one window lasts. */
	if (tsc_reference) {
		tsc_reference = amd64_tsc_window_ticks(
			tsc_frequency,
			TIMER_CAL_WINDOWS_PER_SECOND,
			&tsc_window);
	}

	/* Reports which reference times the windows. */
	if (tsc_reference) {
		hal_printf(
			"A64 TIMER CAL source=tsc hz=%llu window=%llu\n",
			(unsigned long long)tsc_frequency,
			(unsigned long long)tsc_window);
	} else {
		hal_puts("A64 TIMER CAL source=pit\n");
	}

	/*
	 * Measures several windows with a masked free-running APIC timer, each
	 * bracketed at the reference's edges, and keeps the shortest together
	 * with its TSC pair.
	 */
	for (window = 0U; window < TIMER_CAL_WINDOWS; window++) {
		/* Restarts the free-running APIC count for this window. */
		write_reg(LAPIC_TIMER_DIVIDE, 0x3U);
		write_reg(LAPIC_LVT_TIMER, LAPIC_MASKED | INT_IRQ_BASE);
		write_reg(LAPIC_TIMER_INITIAL, 0xffffffffU);

		/* Waits out one window of the chosen reference. */
		window_tsc_start = 0U;
		window_tsc_end = 0U;
		lapic_start = 0U;
		lapic_end = 0U;
		if (tsc_reference) {
			error = tsc_wait_window(
				tsc_window,
				&polls,
				&stage,
				&lapic_start,
				&lapic_end);
		} else {
			error = pit_wait_10ms(
				&port61,
				&polls,
				&stage,
				measure_tsc,
				&window_tsc_start,
				&window_tsc_end,
				&lapic_start,
				&lapic_end);
		}

		/* Stops both calibrations after a bounded reference timeout. */
		if (error != HAL_OK) {
			current = read_reg(LAPIC_TIMER_CURRENT);
			write_reg(LAPIC_LVT_TIMER, LAPIC_MASKED | INT_IRQ_BASE);
			write_reg(LAPIC_TIMER_INITIAL, 0);
			amd64_timecounter_bsp_abort();
			hal_printf(
				"A64 TIMER CAL TIMEOUT stage=%s port61=%02X "
				"lapic-current=%08X polls=%u\n",
				stage,
				port61,
				current,
				polls);
			return HAL_ERR_TIMEOUT;
		}

		/* The APIC counts down: the window is the difference of its two samples. */
		window_elapsed[window] = lapic_start - lapic_end;
		hal_printf("A64 TIMER CAL WINDOW %u elapsed=%u\n", window, window_elapsed[window]);

		/* Keeps the shortest window, which no delay has lengthened most. */
		if (window_elapsed[window] < elapsed) {
			elapsed = window_elapsed[window];
			tsc_start = window_tsc_start;
			tsc_end = window_tsc_end;
		}
	}

	/* Stops the free-running count before the periodic timer is programmed. */
	write_reg(LAPIC_TIMER_INITIAL, 0);

	/* Rejects an implausibly short calibration interval. */
	if (elapsed < 100U) {
		write_reg(LAPIC_LVT_TIMER, LAPIC_MASKED | INT_IRQ_BASE);
		amd64_timecounter_bsp_abort();
		hal_printf(
			"A64 TIMER CAL INVALID elapsed=%u\n",
			elapsed);
		return HAL_ERR_STATE;
	}

	/*
	 * Publishes APIC calibration before optional PIT TSC completion.
	 * A window is a TIMER_CAL_WINDOWS_PER_SECOND-th of a second; the
	 * period is scaled to the HAL tick rate.
	 */
	timer_initial = (uint32_t)(((uint64_t)elapsed * TIMER_CAL_WINDOWS_PER_SECOND) /
	    HAL_TIMER_FREQUENCY);
	if (measure_tsc) {
		amd64_timecounter_pit_complete(
			tsc_start,
			tsc_end,
			PIT_INPUT_HZ,
			PIT_CAL_TICKS);
	}
	hal_printf("A64 TIMER CAL READY ticks=%u\n", timer_initial);

	/* Succeeded: timer_initial holds the period every CPU programs. */
	return HAL_OK;
}

/*
 * Chooses once, on the BSP after the calibration, whether the TSC-deadline
 * mode may time the tick: the TSC must run at one rate through every P- and
 * C-state (invariant), and its rate must be known.  Each CPU still checks
 * that it has the mode when its timer starts.
 */
static void
choose_tick_source(
	void)
{
	uint64_t frequency;
	uint64_t period;
	bool invariant;
	bool known;

	/* Keeps the periodic timer when the build asks for it. */
	tick_period_tsc = 0U;
	if (LAPIC_TICK_TSC_DEADLINE == 0) {
		hal_puts("A64 TIMER TICK source=periodic\n");
		return;
	}

	/* Keeps the periodic timer unless the TSC's rate is fixed and known. */
	invariant = amd64_timecounter_invariant();
	known = amd64_timecounter_bsp_candidate_frequency(&frequency);
	period = 0U;
	if (invariant && known)
		period = frequency / HAL_TIMER_FREQUENCY;
	if (period == 0U || !cpu_has_tsc_deadline()) {
		hal_puts("A64 TIMER TICK source=periodic\n");
		return;
	}

	/* Publishes the period every CPU's deadlines are multiples of. */
	tick_period_tsc = period;
	hal_printf(
		"A64 TIMER TICK source=tsc-deadline period=%llu\n",
		(unsigned long long)period);
}

/* Reports whether the current CPU's local APIC timer has the TSC-deadline mode. */
static bool
cpu_has_tsc_deadline(
	void)
{
	uint32_t max_leaf;
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;

	/* A CPU without leaf 1 has no such mode. */
	cpuid_count(0, 0, &max_leaf, &ebx, &ecx, &edx);
	if (max_leaf < 1U)
		return false;

	/* The mode as leaf 1 reports it. */
	cpuid_count(1, 0, &eax, &ebx, &ecx, &edx);
	if ((ecx & CPUID_TSC_DEADLINE) == 0U)
		return false;

	/* Succeeded: the mode is present. */
	return true;
}

/*
 * Writes the current CPU's next deadline: the first multiple of the period
 * after the TSC now.  The deadlines are multiples of the period on every CPU
 * rather than the last deadline plus the period, so a tick that was late
 * does not move the phase, and every CPU keeps the same phase however long
 * after another it started.  At a tick the TSC has reached the deadline, so
 * the next multiple is always a later one.
 */
static void
arm_next_deadline(
	void)
{
	uint64_t now;
	uint64_t next;

	/* The next multiple of the period after now. */
	now = amd64_timecounter_sample_serialized();
	next = (now / tick_period_tsc + 1U) * tick_period_tsc;

	/* Arms the local APIC timer at that TSC value. */
	asm_write_msr(MSR_IA32_TSC_DEADLINE, next);
}

/* Measures the APIC timer's count over one window timed by the TSC. */
static int
tsc_wait_window(
	uint64_t window_ticks,
	unsigned *polls,
	const char **stage,
	uint32_t *lapic_start,
	uint32_t *lapic_end)
{
	uint64_t start;
	uint64_t now;
	uint64_t passed;
	unsigned count;

	/* Brackets the window's start with the TSC and the APIC timer's count. */
	start = amd64_timecounter_sample_serialized();
	*lapic_start = read_reg(LAPIC_TIMER_CURRENT);

	/*
	 * Spins until the TSC has advanced one window.  The poll bound only
	 * catches a TSC that does not move: a window is far fewer polls.
	 */
	for (count = 0U; count < TSC_POLL_LIMIT; count++) {
		now = amd64_timecounter_sample_serialized();

		/* A sample behind the start, never seen on one CPU, has not passed any time. */
		passed = 0U;
		if (now > start)
			passed = now - start;

		/* Ends the window once it has lasted its TSC count. */
		if (passed >= window_ticks)
			break;
		__asm__ volatile("pause");
	}

	/* Brackets the window's end with the APIC timer's count. */
	*lapic_end = read_reg(LAPIC_TIMER_CURRENT);
	*polls = count;

	/* Reports a TSC that never reached the window's end. */
	if (count == TSC_POLL_LIMIT) {
		*stage = "tsc";
		return HAL_ERR_TIMEOUT;
	}

	/* Succeeded: the two APIC samples bracket one window. */
	return HAL_OK;
}

/* Waits for the PIT output pin to reach one level. */
static int
pit_wait_level(
	struct amd64_pit_poll *poll,
	int expected_high,
	uint8_t *last_port61)
{
	enum amd64_pit_poll_result result;
	uint8_t current;

	/* Polls port 61 through the bounded policy state machine. */
	for (;;) {
		current = asm_inb(0x61U);

		/* Advances the poll policy and stops at a terminal result. */
		result = amd64_pit_poll_step(poll, current, expected_high);
		if (result != AMD64_PIT_POLL_WAIT)
			break;
		__asm__ volatile("pause");
	}

	/* Returns the final hardware sample when requested. */
	if (last_port61 != NULL)
		*last_port61 = current;

	/* Reports the ready or timeout state using HAL conventions. */
	if (result == AMD64_PIT_POLL_READY)
		return HAL_OK;

	/* Reports expiration of the bounded hardware poll. */
	return HAL_ERR_TIMEOUT;
}

/* Measures one ten-millisecond PIT interval and optional TSC bracket. */
static int
pit_wait_10ms(
	uint8_t *last_port61,
	unsigned *polls,
	const char **stage,
	int measure_tsc,
	uint64_t *tsc_start,
	uint64_t *tsc_end,
	uint32_t *lapic_start,
	uint32_t *lapic_end)
{
	struct amd64_pit_poll poll;
	uint8_t value;
	int error;

	/* Samples the original PIT gate and speaker control state. */
	value = asm_inb(0x61U);

	/*
	 * Mode zero drives OUT low when the count is loaded. Observe that edge
	 * before raising GATE so a stuck-high port cannot appear successful.
	 */
	asm_outb(0x61U, (uint8_t)(value & ~3U));
	asm_outb(0x43U, 0xb0U);
	asm_outb(0x42U, (uint8_t)PIT_CAL_TICKS);
	asm_outb(0x42U, (uint8_t)(PIT_CAL_TICKS >> 8));
	amd64_pit_poll_init(&poll, PIT_POLL_LIMIT);
	error = pit_wait_level(&poll, 0, last_port61);

	/* Restores hardware immediately after a missing low transition. */
	if (error != HAL_OK) {
		/* Records the failed transition stage when requested. */
		if (stage != NULL)
			*stage = "out-low";
		asm_outb(0x61U, value);

		/* Returns the consumed poll budget when requested. */
		if (polls != NULL)
			*polls = PIT_POLL_LIMIT - poll.remaining;

		/* Propagates the low-transition failure after restoration. */
		return error;
	}

	/*
	 * Brackets the raised-gate interval with the APIC timer's count and,
	 * when TSC calibration is needed, the TSC: only the gated 10 ms are
	 * measured, not the setup before them.
	 */
	if (measure_tsc)
		*tsc_start = amd64_timecounter_sample_serialized();
	*lapic_start = read_reg(LAPIC_TIMER_CURRENT);
	asm_outb(0x61U, (uint8_t)((value & ~2U) | 1U));
	amd64_pit_poll_init(&poll, PIT_POLL_LIMIT);
	error = pit_wait_level(&poll, 1, last_port61);
	*lapic_end = read_reg(LAPIC_TIMER_CURRENT);
	if (error == HAL_OK && measure_tsc)
		*tsc_end = amd64_timecounter_sample_serialized();

	/* Records a missing high transition for diagnostics. */
	if (error != HAL_OK && stage != NULL)
		*stage = "out-high";

	/* Restores PIT control and returns the consumed poll budget. */
	asm_outb(0x61U, value);
	if (polls != NULL)
		*polls = PIT_POLL_LIMIT - poll.remaining;

	/* Returns the high-transition result unchanged. */
	return error;
}

/* Sends one local APIC interrupt-command register message. */
static int
send_icr(
	uint32_t apic_id,
	uint32_t low)
{
	int error;

	/* Requires the earlier command to complete before overwriting the ICR. */
	error = wait_icr();
	if (error != HAL_OK)
		return error;

	/* Writes destination before the delivery command. */
	write_reg(LAPIC_ICR_HIGH, apic_id << 24);
	write_reg(LAPIC_ICR_LOW, low);

	/* Waits for completion of the newly issued command. */
	error = wait_icr();

	/* Returns the delivery-completion result. */
	return error;
}

/* Masks one LVT entry and returns the value it had. */
static uint32_t
quiet_entry(
	unsigned offset)
{
	uint32_t value;

	/* The entry as it was, then masked. */
	value = read_reg(offset);
	write_reg(offset, value | LAPIC_MASKED);

	/* Succeeded: the value to put back. */
	return value;
}

/* Masks a local line's LVT entry when it delivers a fixed vector, and returns the value it had. */
static uint32_t
quiet_line(
	unsigned offset)
{
	uint32_t value;

	/* The entry as it was; an NMI or ExtINT line keeps delivering. */
	value = read_reg(offset);
	if ((value & LAPIC_DELIVERY_MASK) != LAPIC_DELIVERY_FIXED)
		return value;

	/* A fixed vector's line, masked. */
	write_reg(offset, value | LAPIC_MASKED);

	/* Succeeded: the value to put back. */
	return value;
}
