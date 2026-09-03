/* Pure policy for the amd64 PC/AT boot-local timecounter. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "timecounter-policy.h"

#define CPUID_TSC_RATIO_LEAF 0x00000015U
#define TSC_FREQUENCY_MIN_HZ 1000000ULL
#define TSC_FREQUENCY_MAX_HZ 10000000000ULL

enum amd64_tsc_frequency_policy_result
amd64_tsc_cpuid_frequency_evaluate(
	const struct amd64_tsc_cpuid_frequency_input *input,
	uint64_t *frequency_hz)
{
	uint64_t result;
	uint64_t scaled;

	if (frequency_hz == NULL)
		return AMD64_TSC_FREQUENCY_UNAVAILABLE;
	*frequency_hz = 0U;
	if (input == NULL || !input->tsc_supported ||
	    !input->invariant_tsc_supported)
		return AMD64_TSC_FREQUENCY_UNAVAILABLE;
	if (input->max_basic_leaf < CPUID_TSC_RATIO_LEAF ||
	    input->denominator == 0U || input->numerator == 0U ||
	    input->crystal_hz == 0U ||
	    (uint64_t)input->crystal_hz >
	    UINT64_MAX / (uint64_t)input->numerator)
		return AMD64_TSC_FREQUENCY_NEEDS_PIT;
	scaled = (uint64_t)input->crystal_hz * (uint64_t)input->numerator;
	result = scaled / input->denominator;
	if (scaled % input->denominator != 0U) {
		if (result == UINT64_MAX)
			return AMD64_TSC_FREQUENCY_NEEDS_PIT;
		result++;
	}
	if (result < TSC_FREQUENCY_MIN_HZ ||
	    result > TSC_FREQUENCY_MAX_HZ)
		return AMD64_TSC_FREQUENCY_NEEDS_PIT;
	*frequency_hz = result;
	return AMD64_TSC_FREQUENCY_READY;
}

bool
amd64_tsc_pit_window_frequency(uint64_t start, uint64_t end,
	uint64_t reference_hz, uint64_t reference_ticks,
	uint64_t *frequency_hz)
{
	uint64_t delta;
	uint64_t result;
	uint64_t scaled;

	if (frequency_hz == NULL)
		return false;
	*frequency_hz = 0U;
	if (end <= start || reference_hz == 0U || reference_ticks == 0U)
		return false;
	delta = end - start;
	if (delta > UINT64_MAX / reference_hz)
		return false;
	scaled = delta * reference_hz;
	result = scaled / reference_ticks;
	if (scaled % reference_ticks >= reference_ticks / 2U +
	    reference_ticks % 2U) {
		if (result == UINT64_MAX)
			return false;
		result++;
	}
	if (result < TSC_FREQUENCY_MIN_HZ ||
	    result > TSC_FREQUENCY_MAX_HZ)
		return false;
	*frequency_hz = result;
	return true;
}

static bool
frequency_input_equal(const struct amd64_tsc_cpuid_frequency_input *left,
	const struct amd64_tsc_cpuid_frequency_input *right)
{
	return left->max_basic_leaf == right->max_basic_leaf &&
	    left->denominator == right->denominator &&
	    left->numerator == right->numerator &&
	    left->crystal_hz == right->crystal_hz &&
	    left->tsc_supported == right->tsc_supported &&
	    left->invariant_tsc_supported == right->invariant_tsc_supported;
}

bool
amd64_timecounter_metadata_compatible(enum amd64_timecounter_source source,
	uint64_t frequency_hz,
	const struct amd64_timecounter_cpu_metadata *bsp,
	const struct amd64_timecounter_cpu_metadata *ap)
{
	uint64_t ap_frequency;

	if (source == AMD64_TIMECOUNTER_SOURCE_NONE || frequency_hz == 0U ||
	    bsp == NULL || ap == NULL ||
	    !frequency_input_equal(&bsp->frequency, &ap->frequency) ||
	    !ap->frequency.tsc_supported ||
	    !ap->frequency.invariant_tsc_supported ||
	    bsp->tsc_adjust_supported != ap->tsc_adjust_supported ||
	    (bsp->tsc_adjust_supported && bsp->tsc_adjust != ap->tsc_adjust))
		return false;
	/* A PIT measurement establishes the candidate rate on the BSP.  Equal
	 * architectural metadata plus the separate, real TSC bracket probe on
	 * every AP admits that candidate for the complete CPU set. */
	if (source == AMD64_TIMECOUNTER_SOURCE_PIT)
		return true;
	if (source != AMD64_TIMECOUNTER_SOURCE_CPUID15)
		return false;
	return amd64_tsc_cpuid_frequency_evaluate(&ap->frequency,
	    &ap_frequency) == AMD64_TSC_FREQUENCY_READY &&
	    ap_frequency == frequency_hz;
}

void
amd64_timecounter_probe_init(struct amd64_timecounter_probe_result *probe)
{
	if (probe == NULL)
		return;
	probe->bsp_before = 0U;
	probe->ap_sample = 0U;
	probe->bsp_after = 0U;
	probe->width = 0U;
	probe->valid_rounds = 0U;
	probe->has_best = 0;
}

bool
amd64_timecounter_probe_consider(struct amd64_timecounter_probe_result *probe,
	uint64_t bsp_before, uint64_t ap_sample, uint64_t bsp_after)
{
	uint64_t width;

	if (probe == NULL || bsp_after <= bsp_before ||
	    ap_sample < bsp_before || ap_sample > bsp_after)
		return false;
	width = bsp_after - bsp_before;
	probe->valid_rounds++;
	if (!probe->has_best || width < probe->width) {
		probe->bsp_before = bsp_before;
		probe->ap_sample = ap_sample;
		probe->bsp_after = bsp_after;
		probe->width = width;
		probe->has_best = 1;
	}
	return true;
}

bool
amd64_timecounter_probe_accept(
	const struct amd64_timecounter_probe_result *probe,
	uint64_t frequency_hz)
{
	uint64_t maximum_width;

	if (probe == NULL || !probe->has_best || probe->valid_rounds !=
	    AMD64_TIMECOUNTER_PROBE_ROUNDS ||
	    frequency_hz == 0U)
		return false;
	maximum_width = frequency_hz /
	    AMD64_TIMECOUNTER_MAX_UNCERTAINTY_DIVISOR;
	if (maximum_width == 0U)
		maximum_width = 1U;
	return probe->width <= maximum_width;
}

void
amd64_timecounter_read_state_init(struct amd64_timecounter_read_state *state)
{
	if (state == NULL)
		return;
	__atomic_store_n(&state->available, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&state->lock, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&state->last_sample, 0U, __ATOMIC_RELAXED);
	state->frequency_hz = 0U;
}

bool
amd64_timecounter_read_state_publish(
	struct amd64_timecounter_read_state *state, uint64_t frequency_hz)
{
	if (state == NULL || frequency_hz == 0U ||
	    __atomic_load_n(&state->available, __ATOMIC_ACQUIRE) != 0U)
		return false;
	state->frequency_hz = frequency_hz;
	__atomic_store_n(&state->last_sample, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&state->available, 1U, __ATOMIC_RELEASE);
	return true;
}

void
amd64_timecounter_read_state_disable(struct amd64_timecounter_read_state *state)
{
	if (state != NULL)
		__atomic_store_n(&state->available, 0U, __ATOMIC_RELEASE);
}

bool
amd64_timecounter_read_guarded(struct amd64_timecounter_read_state *state,
	amd64_timecounter_sample_fn sample, void *context, uint64_t *counter,
	uint64_t *frequency_hz)
{
	uint64_t raw = 0U;
	uint64_t frequency = 0U;
	int result = 0;

	if (state == NULL || sample == NULL || counter == NULL ||
	    frequency_hz == NULL)
		return false;
	while (__atomic_exchange_n(&state->lock, 1U,
	    __ATOMIC_ACQUIRE) != 0U) {
		/* The HAL wrapper disables local interrupts before entering this
		 * private global critical section. */
	}
	if (__atomic_load_n(&state->available, __ATOMIC_ACQUIRE) != 0U) {
		raw = sample(context);
		if (raw < __atomic_load_n(&state->last_sample,
		    __ATOMIC_RELAXED)) {
			__atomic_store_n(&state->available, 0U,
			    __ATOMIC_RELEASE);
		} else {
			frequency = state->frequency_hz;
			__atomic_store_n(&state->last_sample, raw,
			    __ATOMIC_RELAXED);
			result = 1;
		}
	}
	__atomic_store_n(&state->lock, 0U, __ATOMIC_RELEASE);
	if (!result)
		return false;
	*counter = raw;
	*frequency_hz = frequency;
	return true;
}
