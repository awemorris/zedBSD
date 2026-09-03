/* Private amd64 PC/AT timecounter ownership and SMP validation. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "timecounter.h"
#include "../asm.h"
#include "../percpu.h"

#define CPUID_TSC (1U << 4)
#define CPUID_TSC_ADJUST (1U << 1)
#define CPUID_INVARIANT_TSC (1U << 8)
#define CPUID_TSC_RATIO 0x00000015U
#define CPUID_EXTENDED_MAX 0x80000000U
#define CPUID_EXTENDED_POWER 0x80000007U
#define MSR_IA32_TSC_ADJUST 0x03bU
#define TIMECOUNTER_PROBE_TIMEOUT 10000000U
#define TIMECOUNTER_RUNTIME_TIMEOUT 10000000U
#define TIMECOUNTER_RUNTIME_READS 64U

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

static void
cpuid_count(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx,
	uint32_t *ecx, uint32_t *edx)
{
	uint32_t a = leaf;
	uint32_t b;
	uint32_t c = subleaf;
	uint32_t d;

	__asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
	*eax = a;
	*ebx = b;
	*ecx = c;
	*edx = d;
}

uint64_t
amd64_timecounter_sample_serialized(void)
{
	uint32_t high;
	uint32_t low;

	__asm__ volatile("lfence\n\trdtsc\n\tlfence" : "=a"(low),
	    "=d"(high) : : "memory");
	return ((uint64_t)high << 32) | low;
}

static void
metadata_collect(struct amd64_timecounter_cpu_metadata *metadata)
{
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	uint32_t maximum_extended;

	hal_memset(metadata, 0, sizeof(*metadata));
	cpuid_count(0U, 0U, &eax, &ebx, &ecx, &edx);
	metadata->frequency.max_basic_leaf = eax;
	if (eax >= 1U) {
		cpuid_count(1U, 0U, &eax, &ebx, &ecx, &edx);
		metadata->frequency.tsc_supported = (edx & CPUID_TSC) != 0U;
	}
	cpuid_count(CPUID_EXTENDED_MAX, 0U, &maximum_extended, &ebx, &ecx,
	    &edx);
	if (maximum_extended >= CPUID_EXTENDED_POWER) {
		cpuid_count(CPUID_EXTENDED_POWER, 0U, &eax, &ebx, &ecx, &edx);
		metadata->frequency.invariant_tsc_supported =
		    (edx & CPUID_INVARIANT_TSC) != 0U;
	}
	if (metadata->frequency.max_basic_leaf >= CPUID_TSC_RATIO) {
		cpuid_count(CPUID_TSC_RATIO, 0U, &eax, &ebx, &ecx, &edx);
		metadata->frequency.denominator = eax;
		metadata->frequency.numerator = ebx;
		metadata->frequency.crystal_hz = ecx;
	}
	if (metadata->frequency.max_basic_leaf >= 7U) {
		cpuid_count(7U, 0U, &eax, &ebx, &ecx, &edx);
		metadata->tsc_adjust_supported =
		    (ebx & CPUID_TSC_ADJUST) != 0U;
		if (metadata->tsc_adjust_supported)
			metadata->tsc_adjust =
			    asm_read_msr(MSR_IA32_TSC_ADJUST);
	}
}

enum amd64_tsc_frequency_policy_result
amd64_timecounter_bsp_prepare(void)
{
	enum amd64_tsc_frequency_policy_result result;

	amd64_timecounter_read_state_init(&read_state);
	candidate_frequency_hz = 0U;
	candidate_source = AMD64_TIMECOUNTER_SOURCE_NONE;
	candidate_valid = 0;
	__atomic_store_n(&runtime_start, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&runtime_release, 0U, __ATOMIC_RELAXED);
	runtime_expected_available = 0U;
	runtime_expected_frequency = 0U;
	metadata_collect(&bsp_metadata);
	result = amd64_tsc_cpuid_frequency_evaluate(&bsp_metadata.frequency,
	    &candidate_frequency_hz);
	if (result == AMD64_TSC_FREQUENCY_READY) {
		candidate_source = AMD64_TIMECOUNTER_SOURCE_CPUID15;
		candidate_valid = 1;
	} else if (result == AMD64_TSC_FREQUENCY_NEEDS_PIT) {
		candidate_source = AMD64_TIMECOUNTER_SOURCE_PIT;
	}
	hal_printf("A64 TIMECOUNTER CANDIDATE policy=%u max=%u ratio=%u/%u "
	    "crystal=%u invariant=%u adjust=%u\n", (unsigned)result,
	    bsp_metadata.frequency.max_basic_leaf,
	    bsp_metadata.frequency.numerator,
	    bsp_metadata.frequency.denominator,
	    bsp_metadata.frequency.crystal_hz,
	    bsp_metadata.frequency.invariant_tsc_supported != 0,
	    bsp_metadata.tsc_adjust_supported != 0);
	return result;
}

void
amd64_timecounter_pit_complete(uint64_t start, uint64_t end,
	uint64_t reference_hz, uint64_t reference_ticks)
{
	uint64_t frequency = 0U;
	int measured = amd64_tsc_pit_window_frequency(start, end,
	    reference_hz, reference_ticks, &frequency);

	if (candidate_source != AMD64_TIMECOUNTER_SOURCE_PIT ||
	    !measured) {
		hal_printf("A64 TIMECOUNTER PIT FAIL delta=%u:%u\n",
		    (uint32_t)((end - start) >> 32), (uint32_t)(end - start));
		candidate_source = AMD64_TIMECOUNTER_SOURCE_NONE;
		candidate_frequency_hz = 0U;
		candidate_valid = 0;
		return;
	}
	candidate_frequency_hz = frequency;
	candidate_valid = 1;
	hal_printf("A64 TIMECOUNTER PIT READY hz=%u:%u\n",
	    (uint32_t)(frequency >> 32), (uint32_t)frequency);
}

void
amd64_timecounter_bsp_abort(void)
{
	candidate_source = AMD64_TIMECOUNTER_SOURCE_NONE;
	candidate_frequency_hz = 0U;
	candidate_valid = 0;
	amd64_timecounter_read_state_disable(&read_state);
}

bool
amd64_timecounter_bsp_candidate_valid(void)
{
	return candidate_valid != 0 && candidate_frequency_hz != 0U;
}

void
amd64_timecounter_ap_probe(struct amd64_percpu *cpu)
{
	unsigned observed = 0U;

	if (cpu == NULL)
		return;
	metadata_collect(&cpu->timecounter_metadata);
#ifdef ZEDBSD_TEST_TIMECOUNTER_INCONSISTENT_CPU
	if (cpu->logical_id == 1U)
		cpu->timecounter_metadata.frequency.numerator ^= 1U;
#endif
	__atomic_store_n(&cpu->timecounter_probe_ready, 1U, __ATOMIC_RELEASE);
	while (__atomic_load_n(&cpu->timecounter_probe_release,
	    __ATOMIC_ACQUIRE) == 0U) {
		unsigned request = __atomic_load_n(
		    &cpu->timecounter_probe_request, __ATOMIC_ACQUIRE);

		if (request != 0U && request != observed) {
			uint64_t sample = amd64_timecounter_sample_serialized();
#ifdef ZEDBSD_TEST_TIMECOUNTER_INCONSISTENT_SAMPLE
			if (cpu->logical_id == 1U)
				sample = 0U;
#endif
			cpu->timecounter_probe_sample = sample;
			__atomic_store_n(&cpu->timecounter_probe_ack, request,
			    __ATOMIC_RELEASE);
			observed = request;
		}
		__asm__ volatile("pause");
	}
}

static int
wait_probe_value(volatile unsigned *value, unsigned expected,
	const struct amd64_percpu *cpu)
{
	unsigned timeout;

	for (timeout = 0U; timeout < TIMECOUNTER_PROBE_TIMEOUT; timeout++) {
		if (__atomic_load_n(value, __ATOMIC_ACQUIRE) == expected)
			return 1;
		if (__atomic_load_n(&cpu->startup_error,
		    __ATOMIC_ACQUIRE) != 0U)
			return 0;
		__asm__ volatile("pause");
	}
	return 0;
}

bool
amd64_timecounter_bsp_validate_ap(struct amd64_percpu *cpu)
{
	struct amd64_timecounter_probe_result probe;
	const char *reason = "ok";
	unsigned generation;
	unsigned failed_generation = 0U;
	int valid = 1;

	if (cpu == NULL)
		return false;
	amd64_timecounter_probe_init(&probe);
	if (!wait_probe_value(&cpu->timecounter_probe_ready, 1U, cpu)) {
		reason = __atomic_load_n(&cpu->startup_error,
		    __ATOMIC_ACQUIRE) != 0U ? "ready-error" : "ready-timeout";
		valid = 0;
		goto release;
	}
	if (!candidate_valid) {
		reason = "candidate-unavailable";
		valid = 0;
		goto release;
	}
	if (!amd64_timecounter_metadata_compatible(candidate_source,
	    candidate_frequency_hz, &bsp_metadata,
	    &cpu->timecounter_metadata)) {
		reason = "metadata-mismatch";
		valid = 0;
		goto release;
	}
	for (generation = 1U;
	    generation <= AMD64_TIMECOUNTER_PROBE_ROUNDS; generation++) {
		uint64_t before = amd64_timecounter_sample_serialized();
		uint64_t after;

		__atomic_store_n(&cpu->timecounter_probe_request, generation,
		    __ATOMIC_RELEASE);
		if (!wait_probe_value(&cpu->timecounter_probe_ack, generation,
		    cpu)) {
			reason = "ack-timeout";
			failed_generation = generation;
			valid = 0;
			break;
		}
		after = amd64_timecounter_sample_serialized();
		if (!amd64_timecounter_probe_consider(&probe, before,
		    cpu->timecounter_probe_sample, after)) {
			reason = "bracket";
			failed_generation = generation;
			valid = 0;
			break;
		}
	}
	if (valid && !amd64_timecounter_probe_accept(&probe,
	    candidate_frequency_hz)) {
		reason = "uncertainty";
		valid = 0;
	}
release:
	cpu->timecounter_probe_valid = valid != 0;
	__atomic_store_n(&cpu->timecounter_probe_release, 1U,
	    __ATOMIC_RELEASE);
	hal_printf(valid ?
	    "A64 TIMECOUNTER AP PASS cpu=%u reason=%s generation=%u "
	    "rounds=%u width=%u:%u\n" :
	    "A64 TIMECOUNTER AP FAIL cpu=%u reason=%s generation=%u "
	    "rounds=%u width=%u:%u\n",
	    cpu->logical_id, reason, failed_generation, probe.valid_rounds,
	    (uint32_t)(probe.width >> 32), (uint32_t)probe.width);
	return valid != 0;
}

static const char *
runtime_status_name(unsigned status)
{
	if (status == TIMECOUNTER_RUNTIME_PASS)
		return "pass";
	if (status == TIMECOUNTER_RUNTIME_UNAVAILABLE)
		return "unavailable";
	if (status == TIMECOUNTER_RUNTIME_FAIL)
		return "fail";
	return "pending";
}

static unsigned
runtime_read_check(unsigned expected_available, uint64_t expected_frequency,
	unsigned *completed_reads)
{
	uint64_t previous = 0U;
	unsigned have_previous = 0U;
	unsigned iteration;

	if (completed_reads != NULL)
		*completed_reads = 0U;
	for (iteration = 0U; iteration < TIMECOUNTER_RUNTIME_READS;
	    iteration++) {
		uint64_t counter = 0xa5a5a5a5a5a5a5a5ULL;
		uint64_t frequency = 0x5a5a5a5a5a5a5a5aULL;
		bool available = hal_rtc_read_counter(&counter, &frequency);

		if (expected_available != 0U) {
			if (!available || frequency == 0U ||
			    frequency != expected_frequency ||
			    (have_previous != 0U && counter < previous))
				return TIMECOUNTER_RUNTIME_FAIL;
			previous = counter;
			have_previous = 1U;
		} else if (available || counter != 0xa5a5a5a5a5a5a5a5ULL ||
		    frequency != 0x5a5a5a5a5a5a5a5aULL) {
			return TIMECOUNTER_RUNTIME_FAIL;
		}
		if (completed_reads != NULL)
			*completed_reads = iteration + 1U;
	}
	return expected_available != 0U ? TIMECOUNTER_RUNTIME_PASS :
	    TIMECOUNTER_RUNTIME_UNAVAILABLE;
}

void
amd64_timecounter_ap_runtime_validate(struct amd64_percpu *cpu)
{
	unsigned expected_available;
	uint64_t expected_frequency;

	if (cpu == NULL)
		return;
	__atomic_store_n(&cpu->timecounter_runtime_ready, 1U,
	    __ATOMIC_RELEASE);
	for (;;) {
		if (__atomic_load_n(&runtime_release, __ATOMIC_ACQUIRE) != 0U)
			return;
		if (__atomic_load_n(&runtime_start, __ATOMIC_ACQUIRE) != 0U)
			break;
		__asm__ volatile("pause");
	}
	if (__atomic_load_n(&runtime_release, __ATOMIC_ACQUIRE) != 0U)
		return;
	expected_available = runtime_expected_available;
	expected_frequency = runtime_expected_frequency;
	cpu->timecounter_runtime_status = runtime_read_check(
	    expected_available, expected_frequency,
	    &cpu->timecounter_runtime_reads);
	__atomic_store_n(&cpu->timecounter_runtime_done, 1U,
	    __ATOMIC_RELEASE);
	while (__atomic_load_n(&runtime_release, __ATOMIC_ACQUIRE) == 0U)
		__asm__ volatile("pause");
}

static int
wait_runtime_value(volatile unsigned *value, unsigned expected)
{
	unsigned timeout;

	for (timeout = 0U; timeout < TIMECOUNTER_RUNTIME_TIMEOUT; timeout++) {
		if (__atomic_load_n(value, __ATOMIC_ACQUIRE) == expected)
			return 1;
		__asm__ volatile("pause");
	}
	return 0;
}

void
amd64_timecounter_complete_boot_validation(bool complete_set_valid,
	unsigned cpu_count)
{
	const char *source;
	const char *failure_reason;
	unsigned bsp_reads = 0U;
	unsigned bsp_status;
	unsigned cpu_index;
	unsigned expected_available;
	int runtime_valid = 1;

	if (candidate_source == AMD64_TIMECOUNTER_SOURCE_CPUID15)
		source = "cpuid15";
	else if (candidate_source == AMD64_TIMECOUNTER_SOURCE_PIT)
		source = "pit";
	else
		source = "none";
	if (!candidate_valid || candidate_frequency_hz == 0U)
		failure_reason = "candidate-unavailable";
	else if (!complete_set_valid)
		failure_reason = "cpu-set-invalid";
	else
		failure_reason = "none";
	for (cpu_index = 1U; cpu_index < cpu_count; cpu_index++) {
		struct amd64_percpu *cpu = amd64_percpu_get(cpu_index);

		if (cpu == NULL || !wait_runtime_value(
		    &cpu->timecounter_runtime_ready, 1U)) {
			complete_set_valid = false;
			failure_reason = "runtime-ready-timeout";
		}
	}
	expected_available = complete_set_valid && candidate_valid &&
	    candidate_frequency_hz != 0U;
	if (expected_available != 0U &&
	    !amd64_timecounter_read_state_publish(&read_state,
	    candidate_frequency_hz)) {
		expected_available = 0U;
		failure_reason = "publish-failed";
	}
	runtime_expected_available = expected_available;
	runtime_expected_frequency = expected_available != 0U ?
	    candidate_frequency_hz : 0U;
	__atomic_store_n(&runtime_start, 1U, __ATOMIC_RELEASE);
	bsp_status = runtime_read_check(expected_available,
	    runtime_expected_frequency, &bsp_reads);
	if ((expected_available != 0U &&
	    bsp_status != TIMECOUNTER_RUNTIME_PASS) ||
	    (expected_available == 0U &&
	    bsp_status != TIMECOUNTER_RUNTIME_UNAVAILABLE))
		runtime_valid = 0;
	for (cpu_index = 1U; cpu_index < cpu_count; cpu_index++) {
		struct amd64_percpu *cpu = amd64_percpu_get(cpu_index);

		if (cpu == NULL || !wait_runtime_value(
		    &cpu->timecounter_runtime_done, 1U)) {
			runtime_valid = 0;
			continue;
		}
		if ((expected_available != 0U &&
		    cpu->timecounter_runtime_status !=
		    TIMECOUNTER_RUNTIME_PASS) ||
		    (expected_available == 0U &&
		    cpu->timecounter_runtime_status !=
		    TIMECOUNTER_RUNTIME_UNAVAILABLE))
			runtime_valid = 0;
	}
	if (expected_available != 0U && !runtime_valid) {
		amd64_timecounter_read_state_disable(&read_state);
		failure_reason = "runtime-read-failed";
	} else if (expected_available == 0U && !runtime_valid) {
		failure_reason = "unavailable-contract-failed";
	}
	__atomic_store_n(&runtime_release, 1U, __ATOMIC_RELEASE);
	hal_printf("A64 TIMECOUNTER READ cpu=0 status=%s reads=%u\n",
	    runtime_status_name(bsp_status), bsp_reads);
	for (cpu_index = 1U; cpu_index < cpu_count; cpu_index++) {
		struct amd64_percpu *cpu = amd64_percpu_get(cpu_index);

		if (cpu != NULL)
			hal_printf("A64 TIMECOUNTER READ cpu=%u status=%s "
			    "reads=%u\n", cpu_index, runtime_status_name(
			    cpu->timecounter_runtime_status),
			    cpu->timecounter_runtime_reads);
	}
	if (expected_available != 0U && runtime_valid) {
		hal_printf("A64 TIMECOUNTER READY cpus=%u source=%s hz=%u:%u\n",
		    cpu_count, source,
		    (uint32_t)(candidate_frequency_hz >> 32),
		    (uint32_t)candidate_frequency_hz);
	} else {
		amd64_timecounter_read_state_disable(&read_state);
		hal_printf("A64 TIMECOUNTER UNAVAILABLE cpus=%u source=%s "
		    "reason=%s\n", cpu_count, source, failure_reason);
	}
}

void
amd64_timecounter_release_boot_validation(void)
{
	amd64_timecounter_read_state_disable(&read_state);
	/* Store release first so an AP never begins a test during an abort. */
	__atomic_store_n(&runtime_release, 1U, __ATOMIC_RELEASE);
	__atomic_store_n(&runtime_start, 1U, __ATOMIC_RELEASE);
}

static uint64_t
sample_for_read(void *context)
{
	(void)context;
	return amd64_timecounter_sample_serialized();
}

bool
amd64_timecounter_read(uint64_t *counter, uint64_t *frequency_hz)
{
	bool irq_enabled;
	bool result;

	if (counter == NULL || frequency_hz == NULL)
		return false;
	irq_enabled = hal_irq_disable();
	result = amd64_timecounter_read_guarded(&read_state, sample_for_read,
	    NULL, counter, frequency_hz);
	if (irq_enabled)
		hal_irq_enable();
	return result;
}
