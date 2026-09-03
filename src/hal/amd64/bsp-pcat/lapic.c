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

#define LAPIC_ID            0x020U
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
#define LAPIC_PERIODIC      0x20000U
#define ICR_PENDING         0x1000U
#define PIT_POLL_LIMIT      1000000U
#define PIT_INPUT_HZ        1193182ULL
#define PIT_CAL_TICKS       11932U

#define CPUID_ARCH_CAPABILITIES       (1U << 29)
#define ARCH_CAP_XAPIC_DISABLE_STATUS (1ULL << 21)
#define MSR_IA32_APIC_BASE            0x01bU
#define MSR_IA32_ARCH_CAPABILITIES    0x10aU
#define MSR_IA32_XAPIC_DISABLE_STATUS 0x0bdU

static volatile uint32_t *lapic;
static uint32_t timer_initial;
static uint32_t lapic_physical_address;

static uint32_t read_reg(unsigned offset);
static void write_reg(unsigned offset, uint32_t value);
static void cpuid_count(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
static enum amd64_apic_policy_result preflight_cpu(uint32_t expected_apic_id, uint32_t physical_address, uint32_t *actual_apic_id);
static int acpi_has_apic_id(const struct amd64_acpi_info *acpi, uint32_t apic_id);
static int wait_icr(void);
static void init_registers(void);
static int pit_wait_level(struct amd64_pit_poll *poll, int expected_high, uint8_t *last_port61);
static int pit_wait_10ms(uint8_t *last_port61, unsigned *polls, const char **stage, int measure_tsc, uint64_t *tsc_start, uint64_t *tsc_end);
static int send_icr(uint32_t apic_id, uint32_t low);

/*
 * Initializes the BSP local APIC from ACPI topology.
 */
int
amd64_lapic_init(
	const struct amd64_acpi_info *acpi)
{
	struct hal_pmem_request request;
	struct hal_pmem memory;
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

	/* Claims the uncached local APIC MMIO page. */
	request.paddr = acpi->lapic_address;
	request.size = 4096;
	request.alignment = 4096;
	request.type = HAL_PMEM_TYPE_MMIO;
	request.attr = HAL_PMEM_ATTR_NOCACHE;
	error = hal_pmem_alloc(&request, &memory);
	if (error != HAL_OK) {
		hal_puts("A64 LAPIC MAP FAIL\n");
		return HAL_ERR_UNSUPPORTED;
	}

	/* Publishes the persistent MMIO mapping and physical base. */
	lapic = memory.vaddr;
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
 */
int
amd64_lapic_timer_start(
	void)
{
	const char *stage;
	uint64_t tsc_start;
	uint64_t tsc_end;
	uint32_t elapsed;
	uint32_t current;
	uint8_t port61;
	unsigned polls;
	enum amd64_tsc_frequency_policy_result tsc_policy;
	int measure_tsc;
	int error;

	/* Calibrates the shared timer period once on the BSP. */
	if (timer_initial == 0) {
		tsc_start = 0U;
		tsc_end = 0U;
		stage = "complete";
		hal_puts("A64 TIMER CAL BEGIN\n");

		/* Determines whether the PIT window must also calibrate TSC. */
		tsc_policy = amd64_timecounter_bsp_prepare();
		measure_tsc = tsc_policy == AMD64_TSC_FREQUENCY_NEEDS_PIT;

		/* Starts a masked free-running APIC timer before the PIT window. */
		write_reg(LAPIC_TIMER_DIVIDE, 0x3U);
		write_reg(LAPIC_LVT_TIMER, LAPIC_MASKED | INT_IRQ_BASE);
		write_reg(LAPIC_TIMER_INITIAL, 0xffffffffU);
		error = pit_wait_10ms(
			&port61,
			&polls,
			&stage,
			measure_tsc,
			&tsc_start,
			&tsc_end);

		/* Stops both calibrations after a bounded PIT timeout. */
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

		/* Converts the APIC down-counter sample to elapsed ticks. */
		current = read_reg(LAPIC_TIMER_CURRENT);
		elapsed = 0xffffffffU - current;
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

		/* Publishes APIC calibration before optional PIT TSC completion. */
		timer_initial = elapsed;
		if (measure_tsc) {
			amd64_timecounter_pit_complete(
				tsc_start,
				tsc_end,
				PIT_INPUT_HZ,
				PIT_CAL_TICKS);
		}
		hal_printf("A64 TIMER CAL READY ticks=%u\n", timer_initial);
	}

	/* Programs the calibrated periodic timer on the current CPU. */
	write_reg(LAPIC_TIMER_DIVIDE, 0x3U);
	write_reg(LAPIC_LVT_TIMER, LAPIC_PERIODIC | INT_IRQ_BASE);
	write_reg(LAPIC_TIMER_INITIAL, timer_initial);

	/* Reports an active periodic local timer. */
	return HAL_OK;
}

/*
 * Stops the current CPU's local APIC timer.
 */
void
amd64_lapic_timer_stop(
	void)
{
	/* Masks the timer vector before clearing its initial count. */
	write_reg(LAPIC_LVT_TIMER, LAPIC_MASKED | INT_IRQ_BASE);
	write_reg(LAPIC_TIMER_INITIAL, 0);
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
	uint64_t *tsc_end)
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

	/* Brackets the raised-gate interval when TSC calibration is needed. */
	if (measure_tsc)
		*tsc_start = amd64_timecounter_sample_serialized();
	asm_outb(0x61U, (uint8_t)((value & ~2U) | 1U));
	amd64_pit_poll_init(&poll, PIT_POLL_LIMIT);
	error = pit_wait_level(&poll, 1, last_port61);
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
