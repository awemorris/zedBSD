/* BR-T52 host regression for amd64 early APIC/PIT policy. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "src/hal/amd64/bsp-pcat/early-init-policy.h"
#include "src/hal/amd64/bsp-pcat/timecounter-policy.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define CPUID_APIC   (1U << 9)
#define CPUID_X2APIC (1U << 21)
#define APIC_ENABLE  (1ULL << 11)
#define APIC_X2      (1ULL << 10)

static struct amd64_apic_policy_input
valid_input(void)
{
	struct amd64_apic_policy_input input = {
		.cpuid1_ebx = 6U << 24,
		.cpuid1_ecx = CPUID_X2APIC,
		.cpuid1_edx = CPUID_APIC,
		.apic_base = 0xfee00000ULL | APIC_ENABLE | (1U << 8),
		.madt_base = 0xfee00000ULL,
		.expected_apic_id = AMD64_APIC_EXPECT_ANY
	};

	return input;
}

static void
expect_policy(struct amd64_apic_policy_input *input,
	enum amd64_apic_policy_result expected)
{
	uint32_t id = UINT32_MAX;
	enum amd64_apic_policy_result actual =
	    amd64_apic_policy_evaluate(input, &id);

	assert(actual == expected);
	assert(id == 6U);
	assert(strlen(amd64_apic_policy_result_name(actual)) != 0);
}

static void
test_apic_policy(void)
{
	struct amd64_apic_policy_input input = valid_input();
	uint32_t id;

	expect_policy(&input, AMD64_APIC_POLICY_OK);
	assert(amd64_apic_policy_evaluate(&input, &id) ==
	    AMD64_APIC_POLICY_OK && id == 6U);
	input.expected_apic_id = 6U;
	expect_policy(&input, AMD64_APIC_POLICY_OK);

	input = valid_input();
	input.cpuid1_edx = 0;
	expect_policy(&input, AMD64_APIC_POLICY_NO_CPUID_APIC);

	input = valid_input();
	input.apic_base &= ~APIC_ENABLE;
	expect_policy(&input, AMD64_APIC_POLICY_DISABLED);

	input = valid_input();
	input.apic_base = 0xfee00000ULL | APIC_X2;
	expect_policy(&input, AMD64_APIC_POLICY_INVALID_MODE);

	input = valid_input();
	input.apic_base |= APIC_X2;
	expect_policy(&input, AMD64_APIC_POLICY_X2APIC);

	input = valid_input();
	input.cpuid1_ecx = 0;
	input.apic_base |= APIC_X2;
	expect_policy(&input, AMD64_APIC_POLICY_X2APIC_UNSUPPORTED);

	input = valid_input();
	input.lock_status_valid = 1;
	input.xapic_disable_status = 1;
	expect_policy(&input, AMD64_APIC_POLICY_LEGACY_LOCKED);

	input = valid_input();
	input.xapic_disable_status = 1;
	expect_policy(&input, AMD64_APIC_POLICY_OK);

	input = valid_input();
	input.madt_base = 0;
	expect_policy(&input, AMD64_APIC_POLICY_INVALID_MADT_BASE);
	input.madt_base = 0xfee00001ULL;
	expect_policy(&input, AMD64_APIC_POLICY_INVALID_MADT_BASE);

	input = valid_input();
	input.madt_base = 0xfec00000ULL;
	expect_policy(&input, AMD64_APIC_POLICY_BASE_MISMATCH);

	input = valid_input();
	input.expected_apic_id = 256U;
	expect_policy(&input, AMD64_APIC_POLICY_ID_RANGE);
	input.expected_apic_id = 7U;
	expect_policy(&input, AMD64_APIC_POLICY_ID_MISMATCH);

	assert(amd64_apic_policy_evaluate(NULL, &id) ==
	    AMD64_APIC_POLICY_INVALID_MODE);
	assert(amd64_apic_policy_evaluate(&input, NULL) ==
	    AMD64_APIC_POLICY_INVALID_MODE);
}

static void
test_pit_poll(void)
{
	struct amd64_pit_poll poll;

	amd64_pit_poll_init(&poll, 3);
	assert(amd64_pit_poll_step(&poll, 0x20U, 1) ==
	    AMD64_PIT_POLL_READY);
	assert(poll.remaining == 3);

	amd64_pit_poll_init(&poll, 3);
	assert(amd64_pit_poll_step(&poll, 0x20U, 0) ==
	    AMD64_PIT_POLL_WAIT);
	assert(poll.remaining == 2);
	assert(amd64_pit_poll_step(&poll, 0, 0) ==
	    AMD64_PIT_POLL_READY);

	amd64_pit_poll_init(&poll, 2);
	assert(amd64_pit_poll_step(&poll, 0, 1) ==
	    AMD64_PIT_POLL_WAIT);
	assert(amd64_pit_poll_step(&poll, 0, 1) ==
	    AMD64_PIT_POLL_TIMEOUT);
	assert(poll.remaining == 0);
	assert(amd64_pit_poll_step(&poll, 0x20U, 1) ==
	    AMD64_PIT_POLL_READY);

	amd64_pit_poll_init(&poll, 0);
	assert(amd64_pit_poll_step(&poll, 0, 1) ==
	    AMD64_PIT_POLL_TIMEOUT);
	assert(amd64_pit_poll_step(NULL, 0, 1) ==
	    AMD64_PIT_POLL_TIMEOUT);
}

static struct amd64_tsc_cpuid_frequency_input
valid_tsc_input(void)
{
	struct amd64_tsc_cpuid_frequency_input input = {
		.max_basic_leaf = 0x15U,
		.denominator = 1U,
		.numerator = 96U,
		.crystal_hz = 25000000U,
		.tsc_supported = 1,
		.invariant_tsc_supported = 1
	};

	return input;
}

static void
test_tsc_frequency_policy(void)
{
	struct amd64_tsc_cpuid_frequency_input input;
	uint64_t frequency;

	input = valid_tsc_input();
	assert(amd64_tsc_cpuid_frequency_evaluate(&input, &frequency) ==
	    AMD64_TSC_FREQUENCY_READY);
	assert(frequency == 2400000000ULL);

	input.tsc_supported = 0;
	frequency = UINT64_MAX;
	assert(amd64_tsc_cpuid_frequency_evaluate(&input, &frequency) ==
	    AMD64_TSC_FREQUENCY_UNAVAILABLE);
	assert(frequency == 0U);
	input = valid_tsc_input();
	input.invariant_tsc_supported = 0;
	assert(amd64_tsc_cpuid_frequency_evaluate(&input, &frequency) ==
	    AMD64_TSC_FREQUENCY_UNAVAILABLE);

	input = valid_tsc_input();
	input.max_basic_leaf = 0x14U;
	assert(amd64_tsc_cpuid_frequency_evaluate(&input, &frequency) ==
	    AMD64_TSC_FREQUENCY_NEEDS_PIT);
	input = valid_tsc_input();
	input.denominator = 0U;
	assert(amd64_tsc_cpuid_frequency_evaluate(&input, &frequency) ==
	    AMD64_TSC_FREQUENCY_NEEDS_PIT);
	input = valid_tsc_input();
	input.numerator = 0U;
	assert(amd64_tsc_cpuid_frequency_evaluate(&input, &frequency) ==
	    AMD64_TSC_FREQUENCY_NEEDS_PIT);
	input = valid_tsc_input();
	input.crystal_hz = 0U;
	assert(amd64_tsc_cpuid_frequency_evaluate(&input, &frequency) ==
	    AMD64_TSC_FREQUENCY_NEEDS_PIT);

	input = valid_tsc_input();
	input.denominator = 3U;
	input.numerator = 1U;
	input.crystal_hz = 10000000U;
	assert(amd64_tsc_cpuid_frequency_evaluate(&input, &frequency) ==
	    AMD64_TSC_FREQUENCY_READY);
	assert(frequency == 3333334ULL);
	input = valid_tsc_input();
	input.denominator = 2U;
	input.numerator = 1U;
	input.crystal_hz = 1000000U;
	assert(amd64_tsc_cpuid_frequency_evaluate(&input, &frequency) ==
	    AMD64_TSC_FREQUENCY_NEEDS_PIT);
	input = valid_tsc_input();
	input.numerator = 500U;
	assert(amd64_tsc_cpuid_frequency_evaluate(&input, &frequency) ==
	    AMD64_TSC_FREQUENCY_NEEDS_PIT);

	assert(amd64_tsc_cpuid_frequency_evaluate(NULL, &frequency) ==
	    AMD64_TSC_FREQUENCY_UNAVAILABLE);
	assert(amd64_tsc_cpuid_frequency_evaluate(&input, NULL) ==
	    AMD64_TSC_FREQUENCY_UNAVAILABLE);
}

static void
test_tsc_pit_window_frequency(void)
{
	uint64_t frequency;

	assert(amd64_tsc_pit_window_frequency(1000U, 24001362U,
	    1193182U, 11932U, &frequency));
	assert(frequency == 2399999994ULL);
	frequency = UINT64_MAX;
	assert(!amd64_tsc_pit_window_frequency(1000U, 1000U,
	    1193182U, 11932U, &frequency));
	assert(frequency == 0U);
	assert(!amd64_tsc_pit_window_frequency(1001U, 1000U,
	    1193182U, 11932U, &frequency));
	assert(!amd64_tsc_pit_window_frequency(0U, 9999U,
	    1193182U, 11932U, &frequency));
	assert(!amd64_tsc_pit_window_frequency(0U, 100100000U,
	    1193182U, 11932U, &frequency));
	assert(!amd64_tsc_pit_window_frequency(0U, UINT64_MAX,
	    1193182U, 11932U, &frequency));
	assert(!amd64_tsc_pit_window_frequency(0U, 24000000U,
	    0U, 11932U, &frequency));
	assert(!amd64_tsc_pit_window_frequency(0U, 24000000U,
	    1193182U, 0U, &frequency));
	assert(!amd64_tsc_pit_window_frequency(0U, 24000000U,
	    1193182U, 11932U, NULL));
}

static struct amd64_timecounter_cpu_metadata
valid_timecounter_metadata(void)
{
	struct amd64_timecounter_cpu_metadata metadata = {
		.frequency = {
			.max_basic_leaf = 0x15U,
			.denominator = 1U,
			.numerator = 96U,
			.crystal_hz = 25000000U,
			.tsc_supported = 1,
			.invariant_tsc_supported = 1
		},
		.tsc_adjust = 0U,
		.tsc_adjust_supported = 1
	};

	return metadata;
}

static void
test_timecounter_metadata_policy(void)
{
	struct amd64_timecounter_cpu_metadata bsp =
	    valid_timecounter_metadata();
	struct amd64_timecounter_cpu_metadata ap = bsp;

	assert(amd64_timecounter_metadata_compatible(
	    AMD64_TIMECOUNTER_SOURCE_CPUID15, 2400000000ULL, &bsp, &ap));
	assert(!amd64_timecounter_metadata_compatible(
	    AMD64_TIMECOUNTER_SOURCE_NONE, 2400000000ULL, &bsp, &ap));
	assert(amd64_timecounter_metadata_compatible(
	    AMD64_TIMECOUNTER_SOURCE_PIT, 2400000000ULL, &bsp, &ap));
	assert(!amd64_timecounter_metadata_compatible(
	    AMD64_TIMECOUNTER_SOURCE_CPUID15, 0U, &bsp, &ap));
	assert(!amd64_timecounter_metadata_compatible(
	    AMD64_TIMECOUNTER_SOURCE_CPUID15, 2400000001ULL, &bsp, &ap));
	ap.frequency.numerator = 95U;
	assert(!amd64_timecounter_metadata_compatible(
	    AMD64_TIMECOUNTER_SOURCE_CPUID15, 2400000000ULL, &bsp, &ap));
	assert(!amd64_timecounter_metadata_compatible(
	    AMD64_TIMECOUNTER_SOURCE_PIT, 2400000000ULL, &bsp, &ap));
	ap = bsp;
	ap.frequency.max_basic_leaf++;
	assert(!amd64_timecounter_metadata_compatible(
	    AMD64_TIMECOUNTER_SOURCE_PIT, 2400000000ULL, &bsp, &ap));
	ap = bsp;
	ap.frequency.invariant_tsc_supported = 0;
	assert(!amd64_timecounter_metadata_compatible(
	    AMD64_TIMECOUNTER_SOURCE_CPUID15, 2400000000ULL, &bsp, &ap));
	assert(!amd64_timecounter_metadata_compatible(
	    AMD64_TIMECOUNTER_SOURCE_PIT, 2400000000ULL, &bsp, &ap));
	ap = bsp;
	ap.tsc_adjust_supported = 0;
	assert(!amd64_timecounter_metadata_compatible(
	    AMD64_TIMECOUNTER_SOURCE_CPUID15, 2400000000ULL, &bsp, &ap));
	assert(!amd64_timecounter_metadata_compatible(
	    AMD64_TIMECOUNTER_SOURCE_PIT, 2400000000ULL, &bsp, &ap));
	ap = bsp;
	ap.tsc_adjust = 1U;
	assert(!amd64_timecounter_metadata_compatible(
	    AMD64_TIMECOUNTER_SOURCE_CPUID15, 2400000000ULL, &bsp, &ap));
	assert(!amd64_timecounter_metadata_compatible(
	    AMD64_TIMECOUNTER_SOURCE_PIT, 2400000000ULL, &bsp, &ap));
	assert(!amd64_timecounter_metadata_compatible(
	    AMD64_TIMECOUNTER_SOURCE_CPUID15, 2400000000ULL, NULL, &ap));
}

static void
test_timecounter_probe_policy(void)
{
	struct amd64_timecounter_probe_result probe;
	unsigned round;

	amd64_timecounter_probe_init(&probe);
	for (round = 0U; round < AMD64_TIMECOUNTER_PROBE_ROUNDS; round++) {
		uint64_t width = 200000U - round * 10000U;
		assert(amd64_timecounter_probe_consider(&probe, 1000000U,
		    1000000U + width / 2U, 1000000U + width));
	}
	assert(probe.valid_rounds == AMD64_TIMECOUNTER_PROBE_ROUNDS);
	assert(probe.width == 130000U);
	assert(amd64_timecounter_probe_accept(&probe, 2400000000ULL));

	amd64_timecounter_probe_init(&probe);
	assert(!amd64_timecounter_probe_consider(&probe, 1000U, 999U,
	    1100U));
	assert(!amd64_timecounter_probe_consider(&probe, 1000U, 1101U,
	    1100U));
	assert(!amd64_timecounter_probe_consider(&probe, 1000U, 1000U,
	    1000U));
	assert(!amd64_timecounter_probe_accept(&probe, 2400000000ULL));

	amd64_timecounter_probe_init(&probe);
	for (round = 0U; round < AMD64_TIMECOUNTER_PROBE_ROUNDS; round++)
		assert(amd64_timecounter_probe_consider(&probe, 0U, 250000U,
		    250001U));
	assert(!amd64_timecounter_probe_accept(&probe, 2400000000ULL));

	amd64_timecounter_probe_init(&probe);
	for (round = 1U; round < AMD64_TIMECOUNTER_PROBE_ROUNDS; round++)
		assert(amd64_timecounter_probe_consider(&probe, 10U, 11U,
		    12U));
	assert(!amd64_timecounter_probe_accept(&probe, 2400000000ULL));
}

struct sample_sequence {
	const uint64_t *values;
	unsigned count;
	unsigned index;
};

static uint64_t
sample_sequence_read(void *context)
{
	struct sample_sequence *sequence = context;
	unsigned index = sequence->index++;

	assert(index < sequence->count);
	return sequence->values[index];
}

struct concurrent_counter {
	struct amd64_timecounter_read_state *state;
	volatile uint64_t next;
	volatile unsigned failed;
};

static uint64_t
sample_increment(void *context)
{
	struct concurrent_counter *counter = context;

	return __atomic_add_fetch(&counter->next, 1U, __ATOMIC_RELAXED);
}

static void *
concurrent_reader(void *argument)
{
	struct concurrent_counter *counter = argument;
	unsigned iteration;

	for (iteration = 0U; iteration < 10000U; iteration++) {
		uint64_t sample;
		uint64_t frequency;

		if (!amd64_timecounter_read_guarded(counter->state,
		    sample_increment, counter, &sample, &frequency) ||
		    sample == 0U || frequency != 2400000000ULL)
			__atomic_store_n(&counter->failed, 1U,
			    __ATOMIC_RELEASE);
	}
	return NULL;
}

static void
test_timecounter_read_guard(void)
{
	static const uint64_t backward_values[] = { 100U, 90U, 110U };
	struct amd64_timecounter_read_state state;
	struct sample_sequence sequence = {
		.values = backward_values,
		.count = sizeof(backward_values) / sizeof(backward_values[0])
	};
	struct concurrent_counter concurrent = {
		.state = &state,
		.next = 0U,
		.failed = 0U
	};
	pthread_t readers[4];
	uint64_t sample = UINT64_MAX;
	uint64_t frequency = UINT64_MAX;
	unsigned index;

	amd64_timecounter_read_state_init(&state);
	assert(!amd64_timecounter_read_guarded(&state,
	    sample_sequence_read, &sequence, &sample, &frequency));
	assert(sample == UINT64_MAX && frequency == UINT64_MAX);
	assert(!amd64_timecounter_read_state_publish(&state, 0U));
	assert(amd64_timecounter_read_state_publish(&state, 2400000000ULL));
	assert(!amd64_timecounter_read_state_publish(&state, 2400000000ULL));
	assert(amd64_timecounter_read_guarded(&state, sample_sequence_read,
	    &sequence, &sample, &frequency));
	assert(sample == 100U && frequency == 2400000000ULL);
	sample = 0xaaaaaaaaaaaaaaaaULL;
	frequency = 0xbbbbbbbbbbbbbbbbULL;
	assert(!amd64_timecounter_read_guarded(&state, sample_sequence_read,
	    &sequence, &sample, &frequency));
	assert(sample == 0xaaaaaaaaaaaaaaaaULL);
	assert(frequency == 0xbbbbbbbbbbbbbbbbULL);
	assert(!amd64_timecounter_read_guarded(&state, sample_sequence_read,
	    &sequence, &sample, &frequency));
	assert(sequence.index == 2U);

	amd64_timecounter_read_state_init(&state);
	assert(amd64_timecounter_read_state_publish(&state, 2400000000ULL));
	for (index = 0U; index < 4U; index++)
		assert(pthread_create(&readers[index], NULL, concurrent_reader,
		    &concurrent) == 0);
	for (index = 0U; index < 4U; index++)
		assert(pthread_join(readers[index], NULL) == 0);
	assert(__atomic_load_n(&concurrent.failed, __ATOMIC_ACQUIRE) == 0U);
	assert(__atomic_load_n(&concurrent.next, __ATOMIC_ACQUIRE) == 40000U);
	assert(__atomic_load_n(&state.last_sample, __ATOMIC_ACQUIRE) == 40000U);
}

static void
expect_ioapic(uint32_t version, uint32_t gsi_base,
	const struct amd64_ioapic_range *previous, unsigned previous_count,
	enum amd64_ioapic_policy_result expected, uint32_t expected_pins)
{
	uint32_t pins = 0;
	enum amd64_ioapic_policy_result actual =
	    amd64_ioapic_policy_evaluate(version, gsi_base, previous,
	    previous_count, &pins);

	assert(actual == expected);
	assert(pins == expected_pins);
	assert(strlen(amd64_ioapic_policy_result_name(actual)) != 0);
}

static void
test_ioapic_policy(void)
{
	const struct amd64_ioapic_range previous[] = {
		{ .gsi_base = 0, .redirections = 24 },
		{ .gsi_base = 48, .redirections = 24 }
	};

	expect_ioapic(0x00170011U, 0, NULL, 0,
	    AMD64_IOAPIC_POLICY_OK, 24);
	expect_ioapic(UINT32_MAX, 0, NULL, 0,
	    AMD64_IOAPIC_POLICY_INVALID_VERSION, 0);
	expect_ioapic(0x00170000U, 0, NULL, 0,
	    AMD64_IOAPIC_POLICY_INVALID_VERSION, 0);
	expect_ioapic(0x00780011U, 0, NULL, 0,
	    AMD64_IOAPIC_POLICY_INVALID_PIN_COUNT, 121);
	expect_ioapic(0x00170011U, UINT32_MAX - 22U, NULL, 0,
	    AMD64_IOAPIC_POLICY_GSI_OVERFLOW, 24);
	expect_ioapic(0x00170011U, 24, previous, 2,
	    AMD64_IOAPIC_POLICY_OK, 24);
	expect_ioapic(0x00170011U, 16, previous, 2,
	    AMD64_IOAPIC_POLICY_GSI_OVERLAP, 24);
	expect_ioapic(0x00170011U, 40, previous, 2,
	    AMD64_IOAPIC_POLICY_GSI_OVERLAP, 24);
	expect_ioapic(0x00170011U, 72, previous, 2,
	    AMD64_IOAPIC_POLICY_OK, 24);
}

int
main(void)
{
	test_apic_policy();
	test_pit_poll();
	test_tsc_frequency_policy();
	test_tsc_pit_window_frequency();
	test_timecounter_metadata_policy();
	test_timecounter_probe_policy();
	test_timecounter_read_guard();
	test_ioapic_policy();
	puts("BR-T52 amd64 early-init policy test: PASS");
	return 0;
}
