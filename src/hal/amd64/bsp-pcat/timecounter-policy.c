/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Pure policy for the amd64 PC/AT boot-local timecounter.
 */

#include "timecounter-policy.h"

#define CPUID_TSC_RATIO_LEAF  0x00000015U
#define TSC_FREQUENCY_MIN_HZ  1000000ULL
#define TSC_FREQUENCY_MAX_HZ  10000000000ULL

static bool frequency_input_equal(const struct amd64_tsc_cpuid_frequency_input *left, const struct amd64_tsc_cpuid_frequency_input *right);

/*
 * Evaluates architectural CPUID data for an invariant-TSC frequency.
 */
enum amd64_tsc_frequency_policy_result
amd64_tsc_cpuid_frequency_evaluate(
	const struct amd64_tsc_cpuid_frequency_input *input,
	uint64_t *frequency_hz)
{
	uint64_t result;
	uint64_t scaled;

	/* Requires a destination before evaluating any metadata. */
	if (frequency_hz == NULL)
		return AMD64_TSC_FREQUENCY_UNAVAILABLE;
	*frequency_hz = 0U;

	/* Requires both the TSC and its invariant-rate guarantee. */
	if (input == NULL || !input->tsc_supported ||
	    !input->invariant_tsc_supported)
		return AMD64_TSC_FREQUENCY_UNAVAILABLE;

	/* Falls back to PIT calibration for incomplete or overflowing ratios. */
	if (input->max_basic_leaf < CPUID_TSC_RATIO_LEAF ||
	    input->denominator == 0U || input->numerator == 0U ||
	    input->crystal_hz == 0U ||
	    (uint64_t)input->crystal_hz >
	    UINT64_MAX / (uint64_t)input->numerator)
		return AMD64_TSC_FREQUENCY_NEEDS_PIT;

	/* Scales the crystal rate and divides by the architectural ratio. */
	scaled = (uint64_t)input->crystal_hz *
	    (uint64_t)input->numerator;
	result = scaled / input->denominator;

	/* Rounds a fractional result upward without overflowing. */
	if (scaled % input->denominator != 0U) {
		/* Falls back when rounding cannot increment the quotient. */
		if (result == UINT64_MAX)
			return AMD64_TSC_FREQUENCY_NEEDS_PIT;
		result++;
	}

	/* Rejects implausible architectural rates in favor of measurement. */
	if (result < TSC_FREQUENCY_MIN_HZ ||
	    result > TSC_FREQUENCY_MAX_HZ)
		return AMD64_TSC_FREQUENCY_NEEDS_PIT;

	/* Publishes the accepted CPUID-derived rate. */
	*frequency_hz = result;
	return AMD64_TSC_FREQUENCY_READY;
}

/*
 * Derives a bounded TSC frequency from a measured PIT reference window.
 */
bool
amd64_tsc_pit_window_frequency(
	uint64_t start,
	uint64_t end,
	uint64_t reference_hz,
	uint64_t reference_ticks,
	uint64_t *frequency_hz)
{
	uint64_t delta;
	uint64_t result;
	uint64_t scaled;

	/* Requires a destination before evaluating the measured window. */
	if (frequency_hz == NULL)
		return false;
	*frequency_hz = 0U;

	/* Requires a forward TSC window and a complete PIT reference. */
	if (end <= start || reference_hz == 0U || reference_ticks == 0U)
		return false;

	/* Scales the observed delta while rejecting multiplication overflow. */
	delta = end - start;
	if (delta > UINT64_MAX / reference_hz)
		return false;
	scaled = delta * reference_hz;
	result = scaled / reference_ticks;

	/* Rounds to the nearest rate without overflowing the quotient. */
	if (scaled % reference_ticks >= reference_ticks / 2U +
	    reference_ticks % 2U) {
		/* Rejects a rounded result that cannot be incremented. */
		if (result == UINT64_MAX)
			return false;
		result++;
	}

	/* Rejects implausible measured rates. */
	if (result < TSC_FREQUENCY_MIN_HZ ||
	    result > TSC_FREQUENCY_MAX_HZ)
		return false;

	/* Publishes the accepted PIT-derived rate. */
	*frequency_hz = result;
	return true;
}

/*
 * Checks whether one AP may share the BSP's timecounter source and rate.
 */
bool
amd64_timecounter_metadata_compatible(
	enum amd64_timecounter_source source,
	uint64_t frequency_hz,
	const struct amd64_timecounter_cpu_metadata *bsp,
	const struct amd64_timecounter_cpu_metadata *ap)
{
	uint64_t ap_frequency;
	enum amd64_tsc_frequency_policy_result result;

	/* Requires matching architectural support and TSC-adjust state. */
	if (source == AMD64_TIMECOUNTER_SOURCE_NONE || frequency_hz == 0U ||
	    bsp == NULL || ap == NULL ||
	    !frequency_input_equal(&bsp->frequency, &ap->frequency) ||
	    !ap->frequency.tsc_supported ||
	    !ap->frequency.invariant_tsc_supported ||
	    bsp->tsc_adjust_supported != ap->tsc_adjust_supported ||
	    (bsp->tsc_adjust_supported && bsp->tsc_adjust != ap->tsc_adjust))
		return false;

	/*
	 * A PIT measurement establishes the candidate rate on the BSP.  Equal
	 * architectural metadata plus the separate real TSC bracket probe on
	 * every AP admits that candidate for the complete CPU set.
	 */
	if (source == AMD64_TIMECOUNTER_SOURCE_PIT)
		return true;

	/* Requires any non-PIT source to be the CPUID ratio source. */
	if (source != AMD64_TIMECOUNTER_SOURCE_CPUID15)
		return false;

	/* Recomputes and compares the AP's architectural frequency. */
	result = amd64_tsc_cpuid_frequency_evaluate(
		&ap->frequency,
		&ap_frequency);
	if (result != AMD64_TSC_FREQUENCY_READY)
		return false;
	if (ap_frequency != frequency_hz)
		return false;

	/* Accepts identical CPUID ratio metadata and rate. */
	return true;
}

/*
 * Initializes an empty BSP-to-AP TSC bracket probe result.
 */
void
amd64_timecounter_probe_init(
	struct amd64_timecounter_probe_result *probe)
{
	/* Leaves an absent optional result untouched. */
	if (probe == NULL)
		return;

	/* Clears all samples and validity state. */
	probe->bsp_before = 0U;
	probe->ap_sample = 0U;
	probe->bsp_after = 0U;
	probe->width = 0U;
	probe->valid_rounds = 0U;
	probe->has_best = 0;
}

/*
 * Considers one BSP-to-AP TSC bracket and retains the narrowest sample.
 */
bool
amd64_timecounter_probe_consider(
	struct amd64_timecounter_probe_result *probe,
	uint64_t bsp_before,
	uint64_t ap_sample,
	uint64_t bsp_after)
{
	uint64_t width;

	/* Requires a forward BSP bracket which contains the AP sample. */
	if (probe == NULL || bsp_after <= bsp_before ||
	    ap_sample < bsp_before || ap_sample > bsp_after)
		return false;

	/* Counts the valid round before comparing its uncertainty width. */
	width = bsp_after - bsp_before;
	probe->valid_rounds++;
	if (!probe->has_best || width < probe->width) {
		/* Retains the complete narrowest bracket. */
		probe->bsp_before = bsp_before;
		probe->ap_sample = ap_sample;
		probe->bsp_after = bsp_after;
		probe->width = width;
		probe->has_best = 1;
	}

	/* Reports a structurally valid probe round. */
	return true;
}

/*
 * Decides whether a complete AP bracket probe is sufficiently precise.
 */
bool
amd64_timecounter_probe_accept(
	const struct amd64_timecounter_probe_result *probe,
	uint64_t frequency_hz)
{
	uint64_t maximum_width;

	/* Requires every planned round and a nonzero candidate rate. */
	if (probe == NULL || !probe->has_best || probe->valid_rounds !=
	    AMD64_TIMECOUNTER_PROBE_ROUNDS || frequency_hz == 0U)
		return false;

	/* Converts the policy uncertainty fraction into a TSC width. */
	maximum_width = frequency_hz /
	    AMD64_TIMECOUNTER_MAX_UNCERTAINTY_DIVISOR;
	if (maximum_width == 0U)
		maximum_width = 1U;

	/* Rejects a best bracket wider than the permitted uncertainty. */
	if (probe->width > maximum_width)
		return false;

	/* Accepts the complete bounded probe. */
	return true;
}

/*
 * Initializes an unavailable serialized timecounter read state.
 */
void
amd64_timecounter_read_state_init(
	struct amd64_timecounter_read_state *state)
{
	/* Leaves an absent optional state untouched. */
	if (state == NULL)
		return;

	/* Clears publication and serialization state with relaxed stores. */
	__atomic_store_n(&state->available, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&state->lock, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&state->last_sample, 0U, __ATOMIC_RELAXED);
	state->frequency_hz = 0U;
}

/*
 * Publishes a frequency for a previously unavailable read state.
 */
bool
amd64_timecounter_read_state_publish(
	struct amd64_timecounter_read_state *state,
	uint64_t frequency_hz)
{
	/* Requires a state and a usable frequency. */
	if (state == NULL || frequency_hz == 0U)
		return false;

	/* Refuses to replace an already published timecounter. */
	if (__atomic_load_n(&state->available, __ATOMIC_ACQUIRE) != 0U)
		return false;

	/* Initializes data before the release publication of availability. */
	state->frequency_hz = frequency_hz;
	__atomic_store_n(&state->last_sample, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&state->available, 1U, __ATOMIC_RELEASE);

	/* Reports successful publication. */
	return true;
}

/*
 * Makes a serialized timecounter read state unavailable.
 */
void
amd64_timecounter_read_state_disable(
	struct amd64_timecounter_read_state *state)
{
	/* Release-publishes unavailability when state exists. */
	if (state != NULL)
		__atomic_store_n(&state->available, 0U, __ATOMIC_RELEASE);
}

/*
 * Samples a published timecounter while enforcing global monotonicity.
 */
bool
amd64_timecounter_read_guarded(
	struct amd64_timecounter_read_state *state,
	amd64_timecounter_sample_fn sample,
	void *context,
	uint64_t *counter,
	uint64_t *frequency_hz)
{
	uint64_t raw;
	uint64_t frequency;
	int result;

	/* Starts with no value eligible for publication to the caller. */
	raw = 0U;
	frequency = 0U;
	result = 0;

	/* Requires the complete state, callback, and output contract. */
	if (state == NULL || sample == NULL || counter == NULL ||
	    frequency_hz == NULL)
		return false;

	/* Acquires the private global timecounter critical section. */
	while (__atomic_exchange_n(
	    &state->lock,
	    1U,
	    __ATOMIC_ACQUIRE) != 0U) {
		/*
		 * The HAL wrapper disables local interrupts before entering this
		 * private global critical section.
		 */
	}

	/* Samples only while acquire-observing a published counter. */
	if (__atomic_load_n(&state->available, __ATOMIC_ACQUIRE) != 0U) {
		raw = sample(context);

		/* Permanently disables the source if monotonicity regresses. */
		if (raw < __atomic_load_n(
		    &state->last_sample,
		    __ATOMIC_RELAXED)) {
			__atomic_store_n(
				&state->available,
				0U,
				__ATOMIC_RELEASE);
		} else {
			/* Publishes the accepted sample before releasing the lock. */
			frequency = state->frequency_hz;
			__atomic_store_n(
				&state->last_sample,
				raw,
				__ATOMIC_RELAXED);
			result = 1;
		}
	}

	/* Releases the global read lock on every acquired path. */
	__atomic_store_n(&state->lock, 0U, __ATOMIC_RELEASE);

	/* Hides outputs when sampling was unavailable or regressed. */
	if (!result)
		return false;

	/* Returns the accepted sample and its stable published frequency. */
	*counter = raw;
	*frequency_hz = frequency;
	return true;
}

/* Compares the complete architectural TSC frequency metadata. */
static bool
frequency_input_equal(
	const struct amd64_tsc_cpuid_frequency_input *left,
	const struct amd64_tsc_cpuid_frequency_input *right)
{
	/* Requires every field used by frequency policy to match. */
	if (left->max_basic_leaf != right->max_basic_leaf ||
	    left->denominator != right->denominator ||
	    left->numerator != right->numerator ||
	    left->crystal_hz != right->crystal_hz ||
	    left->tsc_supported != right->tsc_supported ||
	    left->invariant_tsc_supported != right->invariant_tsc_supported)
		return false;

	/* Reports identical architectural metadata. */
	return true;
}
