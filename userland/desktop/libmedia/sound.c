/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Original continuous Kaiser-windowed sinc conversion; no codec library is consulted. */
#include "sound.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PCM_PHASES 1024U
#define PCM_PI 3.14159265358979323846264338327950288

static void pcm_compact(struct media_pcm *sound);
static int pcm_kernel(struct media_pcm *sound, uint32_t rate);
static double pcm_bessel(double value);
static float pcm_sample(const struct media_pcm *sound, int64_t position, unsigned side);
static int16_t pcm_integer(double value);
static int64_t pcm_time(const struct media_pcm *sound);

/*
 * Establish a bounded source queue with no output-rate assumption before the caller asks for sound.
 */
int
media_pcm_init(
	struct media_pcm *sound,
	uint32_t rate,
	int64_t end_us)
{
	/* Native AAC supports the normative rate table, including 96 kHz down sampling. */
	if (sound == NULL)
		return EINVAL;
	if (rate < 7350U)
		return ENOTSUP;
	if (rate > 96000U)
		return ENOTSUP;
	memset(sound, 0, sizeof(*sound));
	sound->input_rate = rate;
	sound->end_us = end_us;

	/* Succeeded: first input establishes the presentation origin. */
	return 0;
}

/*
 * Append one complete stereo source block while retaining old samples used by the convolution.
 */
int
media_pcm_push(
	struct media_pcm *sound,
	const float *stereo,
	size_t frames,
	int64_t time_us)
{
	uint64_t used;

	/* A drained stream cannot accept new source samples without flush. */
	if (sound == NULL)
		return EINVAL;
	if (stereo == NULL)
		return EINVAL;
	if (sound->drained != 0)
		return EINVAL;
	pcm_compact(sound);
	used = sound->total - sound->base;

	/* The queue cannot conceal an unbounded backlog behind the sound API. */
	if (frames > MEDIA_PCM_ROOM - used)
		return EAGAIN;

	/* Packet timestamps establish one exact source clock instead of rounding every block separately. */
	if (sound->started == 0) {
		sound->origin_us = time_us;
		sound->started = 1;
	}

	/* Append to the same source timeline, retaining lookbehind until convolution retires it. */
	memcpy(sound->samples + used * 2U, stereo, frames * 2U * sizeof(float));
	sound->total += frames;

	/* Succeeded: lookahead and previous convolution samples remain in the same source timeline. */
	return 0;
}

/*
 * Expose at most one source block, with real future samples except at the declared end.
 */
int
media_pcm_receive(
	struct media_pcm *sound,
	int64_t *time_us)
{
	uint64_t start;
	uint64_t end;
	uint64_t trimmed;
	int64_t difference;

	/* Next receive discards any unconverted part of the preceding block by contract. */
	if (sound->active != 0) {
		start = sound->cursor;
		if (sound->output_rate != 0U)
			start = sound->phase / sound->output_rate;
		if (start < sound->active_end) {
			sound->cursor = sound->active_end;
			sound->phase = sound->active_end * sound->output_rate;
			sound->history_start = sound->active_end;
		}

		/* The preceding receive no longer grants access to its source block. */
		sound->active = 0;
	}

	/* No source has established its time or future convolution samples yet. */
	if (sound->started == 0)
		return 0;
	start = sound->cursor;
	if (sound->output_rate != 0U)
		start = sound->phase / sound->output_rate;

	/* Trim on the source grid before the first output rate is selected. */
	if (sound->output_rate == 0U && sound->before_us > sound->origin_us) {
		difference = sound->before_us - sound->origin_us;
		trimmed = ((uint64_t)difference * sound->input_rate + 999999U) / 1000000U;
		if (trimmed > start)
			start = trimmed;
		sound->cursor = start;
	}

	/* One output presentation is bounded by the next 1024-source-sample boundary. */
	if (start >= sound->total)
		return 0;
	end = (start / 1024U + 1U) * 1024U;
	if (end > sound->total)
		end = sound->total;

	/* A full following block supplies even the widest permitted down-sampling kernel. */
	if (sound->drained == 0) {
		if (end + 1024U > sound->total)
			return 0;
	}

	/* Publish one bounded source region with enough real lookahead for its filter. */
	sound->active_end = end;
	sound->active = 1;
	*time_us = pcm_time(sound);

	/* A container's final edit boundary cannot produce another audible block. */
	if (sound->end_us > 0 && *time_us >= sound->end_us) {
		sound->cursor = sound->total;
		sound->phase = sound->total * sound->output_rate;
		sound->active = 0;
		return 0;
	}

	/* Succeeded: the caller can convert precisely this source block. */
	return 1;
}

/*
 * Convert the exposed block into bounded 16-bit stereo at the caller's requested rate.
 */
size_t
media_pcm_read(
	struct media_pcm *sound,
	int16_t *samples,
	size_t capacity,
	uint32_t rate)
{
	uint64_t source;
	uint64_t remainder;
	unsigned phase;
	unsigned tap;
	unsigned taps;
	unsigned side;
	size_t written;
	double fraction;
	double weight;
	double value;
	int64_t position;
	int64_t time_us;
	int error;

	/* A caller without a received block obtains no historical or future samples. */
	if (sound->active == 0)
		return 0U;
	if (samples == NULL)
		return 0U;
	if (rate < 8000U)
		return 0U;
	if (rate > 192000U)
		return 0U;

	/* Select one output clock per continuous segment, preserving exact fractional positions. */
	if (sound->output_rate != rate) {
		/* Rate changes use a new segment without reinterpreting an old numerator. */
		if (sound->output_rate != 0U) {
			sound->cursor = sound->phase / sound->output_rate;
			sound->history_start = sound->cursor;
		}

		/* Build the kernel before publishing a new output-rate clock. */
		error = pcm_kernel(sound, rate);
		if (error != 0) {
			sound->error = error;
			return 0U;
		}

		/* Publish the new rational output clock only after kernel allocation succeeded. */
		sound->phase = sound->cursor * rate;
	}

	/* Each output advances by input_rate units on the output_rate-denominated source clock. */
	written = 0U;
	taps = sound->radius * 2U;
	while (written < capacity) {
		/* The exposed source boundary is exclusive, including fractional positions. */
		if (sound->phase >= sound->active_end * rate)
			break;
		time_us = pcm_time(sound);

		/* Initial edits and seek trim are sample-level boundaries, not whole AAC frame drops. */
		if (time_us < sound->before_us) {
			sound->phase += sound->input_rate;
			continue;
		}

		/* Respect the container's final edit without synthesizing trailing packet padding. */
		if (sound->end_us > 0 && time_us >= sound->end_us) {
			sound->phase = sound->active_end * rate;
			break;
		}

		/* Separate the integer source position from its exact fractional delay. */
		source = sound->phase / rate;
		remainder = sound->phase % rate;
		phase = (unsigned)((remainder * PCM_PHASES) / rate);
		fraction = (double)((remainder * PCM_PHASES) % rate) / (double)rate;

		/* Matching rates preserve exact samples and need no convolution latency. */
		for (side = 0U; side < 2U; side++) {
			value = 0.0;
			if (rate == sound->input_rate) {
				value = pcm_sample(sound, (int64_t)source, side);
			} else {
				/* Interpolate independently generated phase kernels for accurate fractional delays. */
				for (tap = 0U; tap < taps; tap++) {
					position = (int64_t)source + (int64_t)tap + 1 - (int64_t)sound->radius;
					weight = sound->kernel[phase * taps + tap] * (1.0 - fraction) + sound->kernel[(phase + 1U) * taps + tap] * fraction;
					value += pcm_sample(sound, position, side) * weight;
				}
			}

			/* Quantize only the fully accumulated stereo signal. */
			samples[written * 2U + side] = pcm_integer(value);
		}

		/* Advance the output and rational input clocks together. */
		written++;
		sound->phase += sound->input_rate;
	}

	/* Retain the next source position for both receive and buffer compaction. */
	sound->cursor = sound->phase / rate;

	/* Succeeded: unconverted remainder belongs only to this received block. */
	return written;
}

/*
 * Set the sample-level start boundary while keeping already decoded overlap and convolution history.
 */
void
media_pcm_trim(
	struct media_pcm *sound,
	int64_t before_us)
{
	/* Negative presentation samples are always priming rather than audible output. */
	sound->before_us = 0;
	if (before_us > 0)
		sound->before_us = before_us;
	return;
}

/*
 * Flush a continuous segment while retaining reusable mathematical kernel storage.
 */
void
media_pcm_reset(
	struct media_pcm *sound)
{
	/* The next packet establishes a fresh presentation origin and an empty filter history. */
	sound->base = 0U;
	sound->total = 0U;
	sound->cursor = 0U;
	sound->phase = 0U;
	sound->active_end = 0U;
	sound->history_start = 0U;
	sound->origin_us = 0;
	sound->before_us = 0;
	sound->started = 0;
	sound->active = 0;
	sound->drained = 0;
	sound->error = 0;
	return;
}

/*
 * Release decoder-local conversion resources after every retained source sample is discarded.
 */
void
media_pcm_close(
	struct media_pcm *sound)
{
	free(sound->kernel);
	sound->kernel = NULL;
	return;
}

/* Keep only the source samples still needed by the current clock's convolution. */
static void
pcm_compact(
	struct media_pcm *sound)
{
	uint64_t retained;
	uint64_t count;
	unsigned radius;

	/* Before rate selection, one maximum lookbehind prevents premature loss of any source samples. */
	radius = sound->radius;
	if (radius < 384U)
		radius = 384U;
	retained = 0U;
	if (sound->cursor > radius)
		retained = sound->cursor - radius;
	if (retained <= sound->base)
		return;
	if (retained > sound->total)
		retained = sound->total;
	count = sound->total - retained;
	memmove(sound->samples, sound->samples + (retained - sound->base) * 2U, (size_t)count * 2U * sizeof(float));
	sound->base = retained;
	return;
}

/* Construct a rate-dependent anti-alias kernel from its analytical sinc and Kaiser formulas. */
static int
pcm_kernel(
	struct media_pcm *sound,
	uint32_t rate)
{
	float *kernel;
	double ratio;
	double cutoff;
	double distance;
	double absolute;
	double argument;
	double window;
	double normalization;
	double weight;
	unsigned radius;
	unsigned taps;
	unsigned phase;
	unsigned tap;

	/* Down sampling widens both the filter support and the transition region on the source grid. */
	ratio = 1.0;
	if (sound->input_rate > rate)
		ratio = (double)sound->input_rate / (double)rate;
	radius = (unsigned)ceil(32.0 * ratio);
	taps = radius * 2U;
	kernel = calloc((PCM_PHASES + 1U) * taps, sizeof(float));
	if (kernel == NULL)
		return ENOMEM;
	cutoff = 0.455 / ratio;
	normalization = pcm_bessel(8.6);

	/* Every phase has independently evaluated taps, followed by DC normalization. */
	for (phase = 0U; phase <= PCM_PHASES; phase++) {
		weight = 0.0;

		/* Offsets span the symmetric support around this phase's fractional center. */
		for (tap = 0U; tap < taps; tap++) {
			distance = (double)tap + 1.0 - (double)radius - (double)phase / PCM_PHASES;
			window = 0.0;
			absolute = fabs(distance);

			/* The Kaiser support is closed and evaluates safely at either endpoint. */
			if (absolute <= radius) {
				argument = distance / radius;
				window = pcm_bessel(8.6 * sqrt(1.0 - argument * argument)) / normalization;
			}

			/* Evaluate the sinc's removable singularity exactly at zero. */
			argument = 1.0;
			if (absolute > 1.0e-12)
				argument = sin(2.0 * PCM_PI * cutoff * distance) / (2.0 * PCM_PI * cutoff * distance);
			kernel[phase * taps + tap] = (float)(2.0 * cutoff * argument * window);
			weight += kernel[phase * taps + tap];
		}

		/* A constant source remains constant for every fractional output phase. */
		for (tap = 0U; tap < taps; tap++)
			kernel[phase * taps + tap] = (float)(kernel[phase * taps + tap] / weight);
	}

	/* Replace the old kernel only after every new phase is normalized. */
	free(sound->kernel);
	sound->kernel = kernel;
	sound->radius = radius;
	sound->output_rate = rate;

	/* Succeeded: the kernel's bandwidth and rational clock describe the same conversion. */
	return 0;
}

/* Evaluate I0 by its original convergent positive power series. */
static double
pcm_bessel(
	double value)
{
	double term;
	double sum;
	unsigned index;

	/* The fixed beta keeps the series short without any imported approximation coefficients. */
	term = 1.0;
	sum = 1.0;
	for (index = 1U; index < 64U; index++) {
		term *= value * value / (4.0 * index * index);
		sum += term;
		if (term < sum * 1.0e-16)
			break;
	}

	/* The positive series has converged at the fixed window parameter. */
	return sum;
}

/* Obtain a source sample, extending only declared segment boundaries with silence. */
static float
pcm_sample(
	const struct media_pcm *sound,
	int64_t position,
	unsigned side)
{
	float value;

	/* No filter history exists before this segment or after an intentional skipped block. */
	if (position < 0)
		return 0.0f;
	if ((uint64_t)position < sound->history_start)
		return 0.0f;
	if ((uint64_t)position < sound->base)
		return 0.0f;
	if ((uint64_t)position >= sound->total)
		return 0.0f;
	value = sound->samples[((uint64_t)position - sound->base) * 2U + side];
	return value;
}

/* Saturate the final stereo signal exactly once, after filtering and downmix normalization. */
static int16_t
pcm_integer(
	double value)
{
	int result;

	/* A corrupt non-finite reconstruction never enters an undefined floating-to-integer conversion. */
	if (value != value)
		return 0;
	if (value >= 1.0)
		return 32767;
	if (value <= -1.0)
		return -32768;
	result = (int)floor(value * 32768.0 + 0.5);
	if (result > 32767)
		result = 32767;
	return (int16_t)result;
}

/* Resolve the next source position into the segment's presentation clock without repeated rounding. */
static int64_t
pcm_time(
	const struct media_pcm *sound)
{
	uint64_t source;
	uint64_t remainder;
	uint64_t microseconds;

	/* Before output rate selection, the source cursor is already an exact sample boundary. */
	source = sound->cursor;
	remainder = 0U;
	if (sound->output_rate != 0U) {
		source = sound->phase / sound->output_rate;
		remainder = sound->phase % sound->output_rate;
	}

	/* Convert whole and fractional source positions into one presentation timestamp. */
	microseconds = source * 1000000U / sound->input_rate;
	if (sound->output_rate != 0U)
		microseconds = (source * 1000000U + remainder * 1000000U / sound->output_rate) / sound->input_rate;
	return sound->origin_us + (int64_t)microseconds;
}
