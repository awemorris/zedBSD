/* Pure policy for the amd64 PC/AT boot-local timecounter. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_HAL_AMD64_TIMECOUNTER_POLICY_H
#define ZEDBSD_HAL_AMD64_TIMECOUNTER_POLICY_H

#include <hal/types.h>

#define AMD64_TIMECOUNTER_PROBE_ROUNDS 8U
#define AMD64_TIMECOUNTER_MAX_UNCERTAINTY_DIVISOR 10000U

enum amd64_tsc_frequency_policy_result {
	AMD64_TSC_FREQUENCY_UNAVAILABLE = 0,
	AMD64_TSC_FREQUENCY_NEEDS_PIT,
	AMD64_TSC_FREQUENCY_READY
};

enum amd64_timecounter_source {
	AMD64_TIMECOUNTER_SOURCE_NONE = 0,
	AMD64_TIMECOUNTER_SOURCE_CPUID15,
	AMD64_TIMECOUNTER_SOURCE_PIT
};

struct amd64_tsc_cpuid_frequency_input {
	uint32_t max_basic_leaf;
	uint32_t denominator;
	uint32_t numerator;
	uint32_t crystal_hz;
	int tsc_supported;
	int invariant_tsc_supported;
};

struct amd64_timecounter_cpu_metadata {
	struct amd64_tsc_cpuid_frequency_input frequency;
	uint64_t tsc_adjust;
	int tsc_adjust_supported;
};

struct amd64_timecounter_probe_result {
	uint64_t bsp_before;
	uint64_t ap_sample;
	uint64_t bsp_after;
	uint64_t width;
	unsigned valid_rounds;
	int has_best;
};

struct amd64_timecounter_read_state {
	volatile uint64_t last_sample;
	uint64_t frequency_hz;
	volatile unsigned lock;
	volatile unsigned available;
};

typedef uint64_t (*amd64_timecounter_sample_fn)(void *context);

enum amd64_tsc_frequency_policy_result
amd64_tsc_cpuid_frequency_evaluate(
	const struct amd64_tsc_cpuid_frequency_input *input,
	uint64_t *frequency_hz);
bool amd64_tsc_pit_window_frequency(uint64_t start, uint64_t end,
	uint64_t reference_hz, uint64_t reference_ticks,
	uint64_t *frequency_hz);
bool amd64_timecounter_metadata_compatible(
	enum amd64_timecounter_source source, uint64_t frequency_hz,
	const struct amd64_timecounter_cpu_metadata *bsp,
	const struct amd64_timecounter_cpu_metadata *ap);
void amd64_timecounter_probe_init(
	struct amd64_timecounter_probe_result *probe);
bool amd64_timecounter_probe_consider(
	struct amd64_timecounter_probe_result *probe, uint64_t bsp_before,
	uint64_t ap_sample, uint64_t bsp_after);
bool amd64_timecounter_probe_accept(
	const struct amd64_timecounter_probe_result *probe,
	uint64_t frequency_hz);
void amd64_timecounter_read_state_init(
	struct amd64_timecounter_read_state *state);
bool amd64_timecounter_read_state_publish(
	struct amd64_timecounter_read_state *state, uint64_t frequency_hz);
void amd64_timecounter_read_state_disable(
	struct amd64_timecounter_read_state *state);
bool amd64_timecounter_read_guarded(
	struct amd64_timecounter_read_state *state,
	amd64_timecounter_sample_fn sample, void *context,
	uint64_t *counter, uint64_t *frequency_hz);
#endif
