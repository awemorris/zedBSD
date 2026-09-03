/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The private amd64 timecounter ownership and SMP-validation path.
 */

#include <hal/hal.h>
#include "timecounter.h"
#include "../asm.h"
#include "../defs.h"
#include "../percpu.h"

#define CPUID_TSC                   (1U << 4)
#define CPUID_TSC_ADJUST            (1U << 1)
#define CPUID_INVARIANT_TSC         (1U << 8)
#define CPUID_TSC_RATIO             0x00000015U
#define CPUID_EXTENDED_MAX          0x80000000U
#define CPUID_EXTENDED_POWER        0x80000007U
#define MSR_IA32_TSC_ADJUST         0x03bU
#define TIMECOUNTER_PROBE_TIMEOUT   10000000U
#define TIMECOUNTER_RUNTIME_TIMEOUT 10000000U
#define TIMECOUNTER_RUNTIME_READS   64U

enum timecounter_runtime_status {
	TIMECOUNTER_RUNTIME_PENDING = 0,
	TIMECOUNTER_RUNTIME_PASS,
	TIMECOUNTER_RUNTIME_UNAVAILABLE,
	TIMECOUNTER_RUNTIME_FAIL
};

static struct amd64_timecounter_cpu_metadata bsp_metadata;
static struct amd64_timecounter_read_state read_state;
static enum amd64_timecounter_source candidate_source;
static uint64_t candidate_frequency_hz;
static int candidate_valid;
static volatile unsigned runtime_start;
static volatile unsigned runtime_release;
static unsigned runtime_expected_available;
static uint64_t runtime_expected_frequency;

static void cpuid_count(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
static void metadata_collect(struct amd64_timecounter_cpu_metadata *metadata);
static int wait_probe_value(volatile unsigned *value, unsigned expected, const struct amd64_percpu *cpu);
static bool finish_ap_validation(struct amd64_percpu *cpu, const struct amd64_timecounter_probe_result *probe, const char *reason, unsigned failed_generation, int valid);
static const char *runtime_status_name(unsigned status);
static unsigned runtime_read_check(unsigned expected_available, uint64_t expected_frequency, unsigned *completed_reads);
static int wait_runtime_value(volatile unsigned *value, unsigned expected);
static uint64_t sample_for_read(void *context);

/*
 * Samples the TSC with load fences on both sides.
 */
uint64_t
amd64_timecounter_sample_serialized(
	void)
{
	uint32_t high;
	uint32_t low;
	uint64_t value;

	/* Orders surrounding memory operations around the TSC sample. */
	__asm__ volatile("lfence\n\trdtsc\n\tlfence"
	    : "=a"(low), "=d"(high)
	    :
	    : "memory");
	value = ((uint64_t)high << 32) | low;

	/* Returns the serialized architectural counter. */
	return value;
}

/*
 * Prepares the BSP timecounter candidate.
 */
enum amd64_tsc_frequency_policy_result
amd64_timecounter_bsp_prepare(
	void)
{
	enum amd64_tsc_frequency_policy_result result;

	/* Resets publication, candidate, and runtime handshake state. */
	amd64_timecounter_read_state_init(&read_state);
	candidate_frequency_hz = 0U;
	candidate_source = AMD64_TIMECOUNTER_SOURCE_NONE;
	candidate_valid = 0;
	__atomic_store_n(&runtime_start, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&runtime_release, 0U, __ATOMIC_RELAXED);
	runtime_expected_available = 0U;
	runtime_expected_frequency = 0U;

	/* Collects BSP metadata and evaluates a CPUID frequency. */
	metadata_collect(&bsp_metadata);
	result = amd64_tsc_cpuid_frequency_evaluate(
		&bsp_metadata.frequency,
		&candidate_frequency_hz);

	/* Selects a ready CPUID candidate or a pending PIT candidate. */
	if (result == AMD64_TSC_FREQUENCY_READY) {
		candidate_source = AMD64_TIMECOUNTER_SOURCE_CPUID15;
		candidate_valid = 1;
	} else if (result == AMD64_TSC_FREQUENCY_NEEDS_PIT) {
		candidate_source = AMD64_TIMECOUNTER_SOURCE_PIT;
	}

	/* Reports the complete architectural candidate metadata. */
	hal_printf(
		"A64 TIMECOUNTER CANDIDATE policy=%u max=%u ratio=%u/%u "
		"crystal=%u invariant=%u adjust=%u\n",
		(unsigned)result,
		bsp_metadata.frequency.max_basic_leaf,
		bsp_metadata.frequency.numerator,
		bsp_metadata.frequency.denominator,
		bsp_metadata.frequency.crystal_hz,
		bsp_metadata.frequency.invariant_tsc_supported != 0,
		bsp_metadata.tsc_adjust_supported != 0);

	/* Returns the frequency-policy result. */
	return result;
}

/*
 * Completes a BSP PIT-based TSC frequency measurement.
 */
void
amd64_timecounter_pit_complete(
	uint64_t start,
	uint64_t end,
	uint64_t reference_hz,
	uint64_t reference_ticks)
{
	uint64_t frequency;
	uint64_t delta;
	int measured;

	/* Evaluates the captured PIT bracket. */
	frequency = 0U;
	measured = amd64_tsc_pit_window_frequency(
		start,
		end,
		reference_hz,
		reference_ticks,
		&frequency);

	/* Withdraws the candidate after a wrong source or invalid bracket. */
	if (candidate_source != AMD64_TIMECOUNTER_SOURCE_PIT || !measured) {
		delta = end - start;
		hal_printf(
			"A64 TIMECOUNTER PIT FAIL delta=%u:%u\n",
			(uint32_t)(delta >> 32),
			(uint32_t)delta);
		candidate_source = AMD64_TIMECOUNTER_SOURCE_NONE;
		candidate_frequency_hz = 0U;
		candidate_valid = 0;
		return;
	}

	/* Publishes the measured PIT-backed candidate. */
	candidate_frequency_hz = frequency;
	candidate_valid = 1;
	hal_printf(
		"A64 TIMECOUNTER PIT READY hz=%u:%u\n",
		(uint32_t)(frequency >> 32),
		(uint32_t)frequency);
}

/*
 * Aborts BSP timecounter preparation.
 */
void
amd64_timecounter_bsp_abort(
	void)
{
	/* Withdraws all candidate and published read state. */
	candidate_source = AMD64_TIMECOUNTER_SOURCE_NONE;
	candidate_frequency_hz = 0U;
	candidate_valid = 0;
	amd64_timecounter_read_state_disable(&read_state);
}

/*
 * Reports whether the BSP has a complete timecounter candidate.
 */
bool
amd64_timecounter_bsp_candidate_valid(
	void)
{
	/* Rejects an unpublished candidate. */
	if (!candidate_valid)
		return false;

	/* Rejects a candidate without a frequency. */
	if (candidate_frequency_hz == 0U)
		return false;

	/* Reports a complete BSP candidate. */
	return true;
}

/*
 * Serves the BSP's boot-time TSC bracket probes on one AP.
 */
void
amd64_timecounter_ap_probe(
	struct amd64_percpu *cpu)
{
	uint64_t sample;
	unsigned observed;
	unsigned request;

	/* Initializes the observed generation before validating the CPU. */
	observed = 0U;
	if (cpu == NULL)
		return;

	/* Collects this AP's architectural counter metadata. */
	metadata_collect(&cpu->timecounter_metadata);
#ifdef ZEDBSD_TEST_TIMECOUNTER_INCONSISTENT_CPU
	/* Injects inconsistent CPUID metadata for the retained negative fixture. */
	if (cpu->logical_id == 1U)
		cpu->timecounter_metadata.frequency.numerator ^= 1U;
#endif

	/* Publishes readiness before serving bracket generations. */
	__atomic_store_n(
		&cpu->timecounter_probe_ready,
		1U,
		__ATOMIC_RELEASE);
	while (__atomic_load_n(
	    &cpu->timecounter_probe_release,
	    __ATOMIC_ACQUIRE) == 0U) {
		request = __atomic_load_n(
			&cpu->timecounter_probe_request,
			__ATOMIC_ACQUIRE);

		/* Samples once for each new nonzero BSP request generation. */
		if (request != 0U && request != observed) {
			sample = amd64_timecounter_sample_serialized();
#ifdef ZEDBSD_TEST_TIMECOUNTER_INCONSISTENT_SAMPLE
			/* Injects an invalid sample for the retained negative fixture. */
			if (cpu->logical_id == 1U)
				sample = 0U;
#endif
			cpu->timecounter_probe_sample = sample;
			__atomic_store_n(
				&cpu->timecounter_probe_ack,
				request,
				__ATOMIC_RELEASE);
			observed = request;
		}
		__asm__ volatile("pause");
	}
}

/*
 * Validates one AP against the BSP timecounter candidate.
 */
bool
amd64_timecounter_bsp_validate_ap(
	struct amd64_percpu *cpu)
{
	struct amd64_timecounter_probe_result probe;
	const char *reason;
	uint64_t before;
	uint64_t after;
	unsigned generation;
	unsigned failed_generation;
	int compatible;
	int considered;
	int accepted;
	int valid;
	int ready;
	bool result;

	/* Initializes diagnostic state before validating the CPU. */
	reason = "ok";
	failed_generation = 0U;
	valid = 1;
	if (cpu == NULL)
		return false;
	amd64_timecounter_probe_init(&probe);

	/* Waits for the AP to publish metadata and probe readiness. */
	ready = wait_probe_value(&cpu->timecounter_probe_ready, 1U, cpu);
	if (!ready) {
		/* Distinguishes a reported startup error from an expired wait. */
		if (__atomic_load_n(&cpu->startup_error, __ATOMIC_ACQUIRE) != 0U)
			reason = "ready-error";
		else
			reason = "ready-timeout";
		valid = 0;

		/* Releases and reports the rejected AP. */
		result = finish_ap_validation(
			cpu,
			&probe,
			reason,
			failed_generation,
			valid);

		/* Reports the rejected AP result. */
		return result;
	}

	/* Requires a BSP candidate before probing the AP. */
	if (!candidate_valid) {
		reason = "candidate-unavailable";
		valid = 0;

		/* Releases and reports the rejected AP. */
		result = finish_ap_validation(
			cpu,
			&probe,
			reason,
			failed_generation,
			valid);

		/* Reports the rejected AP result. */
		return result;
	}

	/* Requires identical usable architectural counter metadata. */
	compatible = amd64_timecounter_metadata_compatible(
		candidate_source,
		candidate_frequency_hz,
		&bsp_metadata,
		&cpu->timecounter_metadata);
	if (!compatible) {
		reason = "metadata-mismatch";
		valid = 0;

		/* Releases and reports the rejected AP. */
		result = finish_ap_validation(
			cpu,
			&probe,
			reason,
			failed_generation,
			valid);

		/* Reports the rejected AP result. */
		return result;
	}

	/* Collects the fixed number of BSP-bracketed AP samples. */
	for (generation = 1U;
	     generation <= AMD64_TIMECOUNTER_PROBE_ROUNDS;
	     generation++) {
		before = amd64_timecounter_sample_serialized();
		__atomic_store_n(
			&cpu->timecounter_probe_request,
			generation,
			__ATOMIC_RELEASE);

		/* Requires the matching AP acknowledgement generation. */
		ready = wait_probe_value(
			&cpu->timecounter_probe_ack,
			generation,
			cpu);
		if (!ready) {
			reason = "ack-timeout";
			failed_generation = generation;
			valid = 0;
			break;
		}

		/* Validates and considers the completed TSC bracket. */
		after = amd64_timecounter_sample_serialized();
		considered = amd64_timecounter_probe_consider(
			&probe,
			before,
			cpu->timecounter_probe_sample,
			after);
		if (!considered) {
			reason = "bracket";
			failed_generation = generation;
			valid = 0;
			break;
		}
	}

	/* Requires a complete probe with bounded uncertainty. */
	if (valid) {
		/* Applies the candidate frequency to the completed probe result. */
		accepted = amd64_timecounter_probe_accept(
			&probe,
			candidate_frequency_hz);
		if (!accepted) {
			reason = "uncertainty";
			valid = 0;
		}
	}

	/* Releases and reports the final AP validation result. */
	result = finish_ap_validation(
		cpu,
		&probe,
		reason,
		failed_generation,
		valid);

	/* Reports the completed AP validation result. */
	return result;
}

/*
 * Runs the published timecounter read contract on one AP.
 */
void
amd64_timecounter_ap_runtime_validate(
	struct amd64_percpu *cpu)
{
	unsigned expected_available;
	uint64_t expected_frequency;

	/* Ignores an absent CPU record. */
	if (cpu == NULL)
		return;

	/* Publishes readiness before waiting for BSP start or release. */
	__atomic_store_n(
		&cpu->timecounter_runtime_ready,
		1U,
		__ATOMIC_RELEASE);
	for (;;) {
		/* Leaves immediately when the BSP aborts validation. */
		if (__atomic_load_n(&runtime_release, __ATOMIC_ACQUIRE) != 0U)
			return;

		/* Begins only after BSP expected state is published. */
		if (__atomic_load_n(&runtime_start, __ATOMIC_ACQUIRE) != 0U)
			break;
		__asm__ volatile("pause");
	}

	/* Honors a release published concurrently with the start. */
	if (__atomic_load_n(&runtime_release, __ATOMIC_ACQUIRE) != 0U)
		return;

	/* Runs the common read contract against BSP expectations. */
	expected_available = runtime_expected_available;
	expected_frequency = runtime_expected_frequency;
	cpu->timecounter_runtime_status = runtime_read_check(
		expected_available,
		expected_frequency,
		&cpu->timecounter_runtime_reads);
	__atomic_store_n(
		&cpu->timecounter_runtime_done,
		1U,
		__ATOMIC_RELEASE);

	/* Waits until the BSP completes diagnostics for every CPU. */
	while (__atomic_load_n(&runtime_release, __ATOMIC_ACQUIRE) == 0U)
		__asm__ volatile("pause");
}

/*
 * Completes and publishes system-wide timecounter validation.
 */
void
amd64_timecounter_complete_boot_validation(
	bool complete_set_valid,
	unsigned cpu_count)
{
	struct amd64_percpu *cpu;
	const char *source;
	const char *failure_reason;
	const char *status_name;
	unsigned bsp_reads;
	unsigned bsp_status;
	unsigned cpu_index;
	unsigned expected_available;
	int ready;
	int published;
	int runtime_valid;

	/* Initializes diagnostic and runtime-validation state. */
	bsp_reads = 0U;
	runtime_valid = 1;
	if (candidate_source == AMD64_TIMECOUNTER_SOURCE_CPUID15)
		source = "cpuid15";
	else if (candidate_source == AMD64_TIMECOUNTER_SOURCE_PIT)
		source = "pit";
	else
		source = "none";

	/* Selects the initial candidate-set failure reason. */
	if (!candidate_valid || candidate_frequency_hz == 0U)
		failure_reason = "candidate-unavailable";
	else if (!complete_set_valid)
		failure_reason = "cpu-set-invalid";
	else
		failure_reason = "none";

	/* Waits for every admitted AP to reach runtime validation. */
	for (cpu_index = 1U; cpu_index < cpu_count; cpu_index++) {
		/* Resolves and requires this admitted CPU's private state. */
		cpu = amd64_percpu_get(cpu_index);
		if (cpu == NULL) {
			complete_set_valid = false;
			failure_reason = "runtime-ready-timeout";
			continue;
		}

		/* Waits for and requires this AP's runtime-ready publication. */
		ready = wait_runtime_value(
			&cpu->timecounter_runtime_ready,
			1U);
		if (!ready) {
			complete_set_valid = false;
			failure_reason = "runtime-ready-timeout";
		}
	}

	/* Computes and conditionally publishes expected availability. */
	expected_available = complete_set_valid &&
	    candidate_valid && candidate_frequency_hz != 0U;
	if (expected_available != 0U) {
		/* Publishes the selected frequency and checks source availability. */
		published = amd64_timecounter_read_state_publish(
			&read_state,
			candidate_frequency_hz);
		if (!published) {
			expected_available = 0U;
			failure_reason = "publish-failed";
		}
	}
	runtime_expected_available = expected_available;

	/* Selects the expected runtime frequency for the published state. */
	if (expected_available != 0U)
		runtime_expected_frequency = candidate_frequency_hz;
	else
		runtime_expected_frequency = 0U;
	__atomic_store_n(&runtime_start, 1U, __ATOMIC_RELEASE);

	/* Exercises the BSP read contract. */
	bsp_status = runtime_read_check(
		expected_available,
		runtime_expected_frequency,
		&bsp_reads);
	if ((expected_available != 0U &&
	    bsp_status != TIMECOUNTER_RUNTIME_PASS) ||
	    (expected_available == 0U &&
	    bsp_status != TIMECOUNTER_RUNTIME_UNAVAILABLE))
		runtime_valid = 0;

	/* Waits for and verifies every AP read contract. */
	for (cpu_index = 1U; cpu_index < cpu_count; cpu_index++) {
		/* Resolves and requires this admitted CPU's private state. */
		cpu = amd64_percpu_get(cpu_index);
		if (cpu == NULL) {
			runtime_valid = 0;
			continue;
		}

		/* Waits for and requires this AP's runtime completion. */
		ready = wait_runtime_value(
			&cpu->timecounter_runtime_done,
			1U);
		if (!ready) {
			runtime_valid = 0;
			continue;
		}

		/* Requires the status matching expected availability. */
		if ((expected_available != 0U &&
		    cpu->timecounter_runtime_status !=
		    TIMECOUNTER_RUNTIME_PASS) ||
		    (expected_available == 0U &&
		    cpu->timecounter_runtime_status !=
		    TIMECOUNTER_RUNTIME_UNAVAILABLE))
			runtime_valid = 0;
	}

	/* Withdraws a published counter after any runtime-read failure. */
	if (expected_available != 0U && !runtime_valid) {
		amd64_timecounter_read_state_disable(&read_state);
		failure_reason = "runtime-read-failed";
	} else if (expected_available == 0U && !runtime_valid) {
		failure_reason = "unavailable-contract-failed";
	}

	/* Releases APs before printing their final stable results. */
	__atomic_store_n(&runtime_release, 1U, __ATOMIC_RELEASE);
	status_name = runtime_status_name(bsp_status);
	hal_printf(
		"A64 TIMECOUNTER READ cpu=0 status=%s reads=%u\n",
		status_name,
		bsp_reads);

	/* Reports the runtime result published by every admitted AP. */
	for (cpu_index = 1U; cpu_index < cpu_count; cpu_index++) {
		cpu = amd64_percpu_get(cpu_index);

		/* Reports state only for a resolvable admitted CPU. */
		if (cpu != NULL) {
			status_name = runtime_status_name(
				cpu->timecounter_runtime_status);
			hal_printf(
				"A64 TIMECOUNTER READ cpu=%u status=%s reads=%u\n",
				cpu_index,
				status_name,
				cpu->timecounter_runtime_reads);
		}
	}

	/* Publishes final ready or unavailable diagnostics. */
	if (expected_available != 0U && runtime_valid) {
		hal_printf(
			"A64 TIMECOUNTER READY cpus=%u source=%s hz=%u:%u\n",
			cpu_count,
			source,
			(uint32_t)(candidate_frequency_hz >> 32),
			(uint32_t)candidate_frequency_hz);
	} else {
		amd64_timecounter_read_state_disable(&read_state);
		hal_printf(
			"A64 TIMECOUNTER UNAVAILABLE cpus=%u source=%s reason=%s\n",
			cpu_count,
			source,
			failure_reason);
	}
}

/*
 * Aborts and releases the boot-time validation handshake.
 */
void
amd64_timecounter_release_boot_validation(
	void)
{
	/* Withdraws any published read state. */
	amd64_timecounter_read_state_disable(&read_state);

	/* Stores release first so an AP never begins a test during an abort. */
	__atomic_store_n(&runtime_release, 1U, __ATOMIC_RELEASE);
	__atomic_store_n(&runtime_start, 1U, __ATOMIC_RELEASE);
}

/*
 * Reads the published timecounter with local interrupts disabled.
 */
bool
amd64_timecounter_read(
	uint64_t *counter,
	uint64_t *frequency_hz)
{
	bool irq_enabled;
	bool result;

	/* Requires both result destinations. */
	if (counter == NULL || frequency_hz == NULL)
		return false;

	/* Serializes the global guarded reader against local interrupt entry. */
	irq_enabled = hal_irq_disable();
	result = amd64_timecounter_read_guarded(
		&read_state,
		sample_for_read,
		NULL,
		counter,
		frequency_hz);
	if (irq_enabled)
		hal_irq_enable();

	/* Returns the guarded availability result. */
	return result;
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

/* Collects architectural timecounter metadata on the current CPU. */
static void
metadata_collect(
	struct amd64_timecounter_cpu_metadata *metadata)
{
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	uint32_t maximum_extended;

	/* Initializes metadata and queries the maximum basic CPUID leaf. */
	hal_memset(metadata, 0, sizeof(*metadata));
	cpuid_count(0U, 0U, &eax, &ebx, &ecx, &edx);
	metadata->frequency.max_basic_leaf = eax;

	/* Collects base TSC support when the feature leaf exists. */
	if (eax >= 1U) {
		cpuid_count(1U, 0U, &eax, &ebx, &ecx, &edx);
		metadata->frequency.tsc_supported = (edx & CPUID_TSC) != 0U;
	}

	/* Collects invariant-TSC support from the extended power leaf. */
	cpuid_count(
		CPUID_EXTENDED_MAX,
		0U,
		&maximum_extended,
		&ebx,
		&ecx,
		&edx);
	if (maximum_extended >= CPUID_EXTENDED_POWER) {
		cpuid_count(
			CPUID_EXTENDED_POWER,
			0U,
			&eax,
			&ebx,
			&ecx,
			&edx);
		metadata->frequency.invariant_tsc_supported =
		    (edx & CPUID_INVARIANT_TSC) != 0U;
	}

	/* Collects the CPUID frequency ratio when available. */
	if (metadata->frequency.max_basic_leaf >= CPUID_TSC_RATIO) {
		cpuid_count(
			CPUID_TSC_RATIO,
			0U,
			&eax,
			&ebx,
			&ecx,
			&edx);
		metadata->frequency.denominator = eax;
		metadata->frequency.numerator = ebx;
		metadata->frequency.crystal_hz = ecx;
	}

	/* Collects TSC-adjust support and the current adjustment value. */
	if (metadata->frequency.max_basic_leaf >= 7U) {
		cpuid_count(7U, 0U, &eax, &ebx, &ecx, &edx);
		metadata->tsc_adjust_supported =
		    (ebx & CPUID_TSC_ADJUST) != 0U;

		/* Reads the adjustment MSR only when CPUID advertises it. */
		if (metadata->tsc_adjust_supported) {
			metadata->tsc_adjust =
			    asm_read_msr(MSR_IA32_TSC_ADJUST);
		}
	}
}

/* Waits for one AP probe value or startup failure. */
static int
wait_probe_value(
	volatile unsigned *value,
	unsigned expected,
	const struct amd64_percpu *cpu)
{
	unsigned timeout;

	/* Polls the value and startup error for a bounded interval. */
	for (timeout = 0U; timeout < TIMECOUNTER_PROBE_TIMEOUT; timeout++) {
		/* Reports success when the expected probe value is published. */
		if (__atomic_load_n(value, __ATOMIC_ACQUIRE) == expected)
			return 1;

		/* Stops when the AP publishes a startup error. */
		if (__atomic_load_n(&cpu->startup_error, __ATOMIC_ACQUIRE) != 0U)
			return 0;

		/* Backs off before sampling the shared state again. */
		__asm__ volatile("pause");
	}

	/* Reports expiration of the probe wait. */
	return 0;
}

/* Releases an AP probe and reports its final result. */
static bool
finish_ap_validation(
	struct amd64_percpu *cpu,
	const struct amd64_timecounter_probe_result *probe,
	const char *reason,
	unsigned failed_generation,
	int valid)
{
	const char *format;

	/* Publishes the validation result before releasing the AP. */
	cpu->timecounter_probe_valid = valid != 0;
	__atomic_store_n(
		&cpu->timecounter_probe_release,
		1U,
		__ATOMIC_RELEASE);

	/* Reports pass or failure with the best bracket evidence. */
	if (valid) {
		format = "A64 TIMECOUNTER AP PASS cpu=%u reason=%s generation=%u "
		    "rounds=%u width=%u:%u\n";
	} else {
		format = "A64 TIMECOUNTER AP FAIL cpu=%u reason=%s generation=%u "
		    "rounds=%u width=%u:%u\n";
	}
	hal_printf(
		format,
		cpu->logical_id,
		reason,
		failed_generation,
		probe->valid_rounds,
		(uint32_t)(probe->width >> 32),
		(uint32_t)probe->width);

	/* Accepts the AP when every validation step succeeded. */
	if (valid != 0)
		return true;

	/* Rejects an AP that failed any validation step. */
	return false;
}

/* Names one runtime-read validation state. */
static const char *
runtime_status_name(
	unsigned status)
{
	/* Names a successful runtime validation. */
	if (status == TIMECOUNTER_RUNTIME_PASS)
		return "pass";

	/* Names an unavailable public counter. */
	if (status == TIMECOUNTER_RUNTIME_UNAVAILABLE)
		return "unavailable";

	/* Names a failed runtime validation. */
	if (status == TIMECOUNTER_RUNTIME_FAIL)
		return "fail";

	/* Names the initial incomplete state. */
	return "pending";
}

/* Exercises the public counter-read contract repeatedly. */
static unsigned
runtime_read_check(
	unsigned expected_available,
	uint64_t expected_frequency,
	unsigned *completed_reads)
{
	uint64_t previous;
	uint64_t counter;
	uint64_t frequency;
	unsigned have_previous;
	unsigned iteration;
	bool available;

	/* Initializes progress and monotonicity state. */
	previous = 0U;
	have_previous = 0U;
	if (completed_reads != NULL)
		*completed_reads = 0U;

	/* Repeats the published or unavailable read contract. */
	for (iteration = 0U;
	     iteration < TIMECOUNTER_RUNTIME_READS;
	     iteration++) {
		counter = 0xa5a5a5a5a5a5a5a5ULL;
		frequency = 0x5a5a5a5a5a5a5a5aULL;
		available = hal_rtc_read_counter(&counter, &frequency);

		/* Validates available samples or unchanged unavailable outputs. */
		if (expected_available != 0U) {
			/* Rejects an unavailable, invalid, or regressing sample. */
			if (!available ||
			    frequency == 0U ||
			    frequency != expected_frequency ||
			    (have_previous != 0U && counter < previous))
				return TIMECOUNTER_RUNTIME_FAIL;
			previous = counter;
			have_previous = 1U;
		} else if (available ||
		    counter != 0xa5a5a5a5a5a5a5a5ULL ||
		    frequency != 0x5a5a5a5a5a5a5a5aULL) {
			return TIMECOUNTER_RUNTIME_FAIL;
		}

		/* Publishes completed progress when requested. */
		if (completed_reads != NULL)
			*completed_reads = iteration + 1U;
	}

	/* Reports the successfully exercised availability contract. */
	if (expected_available != 0U)
		return TIMECOUNTER_RUNTIME_PASS;

	/* Reports the successfully exercised unavailable contract. */
	return TIMECOUNTER_RUNTIME_UNAVAILABLE;
}

/* Waits for one AP runtime-handshake value. */
static int
wait_runtime_value(
	volatile unsigned *value,
	unsigned expected)
{
	unsigned timeout;

	/* Polls the acquire-loaded value for a bounded interval. */
	for (timeout = 0U; timeout < TIMECOUNTER_RUNTIME_TIMEOUT; timeout++) {
		/* Reports success when the expected runtime value is published. */
		if (__atomic_load_n(value, __ATOMIC_ACQUIRE) == expected)
			return 1;

		/* Backs off before sampling the shared value again. */
		__asm__ volatile("pause");
	}

	/* Reports expiration of the runtime wait. */
	return 0;
}

/* Supplies a serialized TSC sample to the guarded reader. */
static uint64_t
sample_for_read(
	void *context)
{
	uint64_t sample;

	UNUSED_PARAMETER(context);

	/* Samples the serialized architectural counter. */
	sample = amd64_timecounter_sample_serialized();

	/* Returns the sample to the guarded reader. */
	return sample;
}
