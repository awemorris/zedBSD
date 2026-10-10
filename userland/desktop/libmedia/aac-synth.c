/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Original AAC inverse quantization, stereo/PNS/TNS tools and FFT-based IMDCT filterbank. */
#include "aac-synth.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <string.h>

#define AAC_PI 3.14159265358979323846264338327950288

/* Immutable mathematical tables, published once before any decoder reconstruction. */
static float synth_powers[8192];

/* Sine/KBD half windows indexed by length (long/short), shape and sample. */
static float synth_windows[2][2][1024];

/* FFT phases for the largest transform; smaller transforms use an exact stride. */
static float synth_fft_cos[1024];

/* Matching sine phases, written only by the same once initializer. */
static float synth_fft_sin[1024];

/* DCT-IV pre-rotation phases for each coefficient count. */
static float synth_pre_cos[2][1024];

/* Negative-imaginary pre-rotation magnitudes paired with synth_pre_cos. */
static float synth_pre_sin[2][1024];

/* DCT-IV post-rotation phases, immutable for all decoder lifetimes. */
static float synth_post_cos[2][1024];

/* Matching post-rotation sine phases for the same DCT-IV output indices. */
static float synth_post_sin[2][1024];

/* Publish all mathematical tables exactly once across simultaneous decoder threads. */
static pthread_once_t synth_once = PTHREAD_ONCE_INIT;

static void synth_initialize(void);
static double synth_bessel(double argument);
static void synth_inverse_quant(struct media_aac_channel *channel, uint32_t *random);
static void synth_stereo(struct media_aac_channel *left, struct media_aac_channel *right);
static void synth_tns(struct media_aac_channel *channel);
static void synth_imdct(const float *spectrum, unsigned size, struct media_aac_transform *transform, float *output);
static void synth_fft(struct media_aac_transform *transform, unsigned length);

/*
 * Reconstruct every frequency-domain channel before the filterbank updates overlap history.
 */
int
media_aac_reconstruct(
	struct media_aac_frame *frame,
	uint32_t *random)
{
	unsigned channel;
	int error;

	/* Reconstruction requires caller-owned scratch and a decoder-local random sequence. */
	if (frame == NULL || random == NULL)
		return EINVAL;

	/* Generate mathematical facts independently of any external decoder implementation. */
	error = pthread_once(&synth_once, synth_initialize);
	if (error != 0)
		return error;

	/* Each channel first obtains its independent inverse-quantized or noise spectrum. */
	for (channel = 0U; channel < frame->channels; channel++)
		synth_inverse_quant(&frame->channel[channel], random);

	/* A transmitted pair has adjacent left/right members, matched by the raw parser. */
	for (channel = 0U; channel < frame->channels; channel++) {
		/* Only a pair's left member owns the masks and initiates stereo reconstruction. */
		if (frame->channel[channel].type == 1U && frame->channel[channel].side == 0U) {
			/* The partner must actually exist before either channel is addressed. */
			if (channel + 1U >= frame->channels)
				return EINVAL;

			/* Independent-window pairs have no common band positions or stereo masks. */
			if (frame->channel[channel].common != 0U)
				synth_stereo(&frame->channel[channel], &frame->channel[channel + 1U]);
		}
	}

	/* Temporal noise shaping follows all stereo and noise tools. */
	for (channel = 0U; channel < frame->channels; channel++)
		synth_tns(&frame->channel[channel]);

	/* Succeeded: each channel contains its complete reconstructed frequency spectrum. */
	return 0;
}

/*
 * Synthesize one complete 1024-sample channel using its own previous overlap and window shape.
 */
int
media_aac_synthesize(
	const struct media_aac_channel *channel,
	struct media_aac_history *history,
	struct media_aac_transform *transform,
	float pcm[1024])
{
	float short_output[256];
	float left_window;
	float right_window;
	unsigned index;
	unsigned window;
	unsigned offset;
	unsigned previous;
	unsigned shape;
	int error;

	/* Transform scratch and history belong to one decoder, with no concurrent mutation. */
	if (channel == NULL ||
	    history == NULL ||
	    transform == NULL ||
	    pcm == NULL)
		return EINVAL;

	/* Even direct synthesis callers must observe initialized mathematical windows. */
	error = pthread_once(&synth_once, synth_initialize);
	if (error != 0)
		return error;
	previous = history->shape;
	shape = channel->info.shape;

	/* A malformed shape cannot select outside the two standard windows. */
	if (previous > 1U || shape > 1U)
		return EINVAL;

	/* Eight short windows overlap at 128-sample strides within the long output span. */
	if (channel->info.sequence == 2U) {
		memset(transform->windowed, 0, sizeof(transform->windowed));

		/* The first short window uses the previous shape on its left half only. */
		for (window = 0U; window < 8U; window++) {
			synth_imdct(channel->spectrum + window * 128U, 128U, transform, short_output);
			offset = 448U + window * 128U;

			/* Later left halves use the current shape, matching their preceding right half. */
			if (window != 0U)
				previous = shape;

			/* Add each short window without overwriting its neighbor's overlap. */
			for (index = 0U; index < 128U; index++) {
				transform->windowed[offset + index] += short_output[index] * synth_windows[1][previous][index];
				transform->windowed[offset + 128U + index] += short_output[128U + index] * synth_windows[1][shape][127U - index];
			}
		}
	} else {
		synth_imdct(channel->spectrum, 1024U, transform, transform->windowed);

		/* Long-start and long-stop transition halves preserve their exact flat and zero regions. */
		for (index = 0U; index < 1024U; index++) {
			left_window = synth_windows[0][previous][index];
			right_window = synth_windows[0][shape][1023U - index];

			/* Long-stop places the short left window between an initial zero and final flat region. */
			if (channel->info.sequence == 3U) {
				left_window = 1.0f;

				/* Samples before the short window's left edge are silent. */
				if (index < 448U) {
					left_window = 0.0f;
				} else if (index < 576U) {
					left_window = synth_windows[1][previous][index - 448U];
				}
			}

			/* Long-start uses a flat right prefix, then the reversed short window, then zeros. */
			if (channel->info.sequence == 1U) {
				right_window = 0.0f;

				/* The first 448 samples are flat before the short transition starts. */
				if (index < 448U) {
					right_window = 1.0f;
				} else if (index < 576U) {
					right_window = synth_windows[1][shape][575U - index];
				}
			}

			/* Window both transform halves before any overlap is committed. */
			transform->windowed[index] *= left_window;
			transform->windowed[index + 1024U] *= right_window;
		}
	}

	/* Commit output and next overlap only after all frequency-domain tools have succeeded. */
	for (index = 0U; index < 1024U; index++) {
		pcm[index] = history->overlap[index] + transform->windowed[index];
		history->overlap[index] = transform->windowed[index + 1024U];
	}

	/* The next block uses this block's signalled shape for its left edge. */
	history->shape = shape;

	/* Succeeded: one PCM block and the next overlap have matching window boundaries. */
	return 0;
}

/* Construct only mathematical windows, phases and inverse-quantization powers. */
static void
synth_initialize(
	void)
{
	double kaiser[1025];
	double sum;
	double cumulative;
	double coordinate;
	double alpha;
	double angle;
	unsigned length;
	unsigned size;
	unsigned index;

	/* Quantized magnitudes are immutable numeric powers rather than imported decoder lookup data. */
	for (index = 0U; index < 8192U; index++)
		synth_powers[index] = (float)pow((double)index, 4.0 / 3.0);

	/* Long and short transforms share the same original radix-two FFT implementation. */
	for (index = 0U; index < 1024U; index++) {
		angle = 2.0 * AAC_PI * (double)index / 2048.0;
		synth_fft_cos[index] = (float)cos(angle);
		synth_fft_sin[index] = (float)sin(angle);
	}

	/* Independently construct the sine and Kaiser-Bessel-derived windows for both coefficient counts. */
	for (length = 0U; length < 2U; length++) {
		size = 1024U;
		alpha = 4.0;

		/* Short windows use the standard alpha six and 128 coefficients. */
		if (length == 1U) {
			size = 128U;
			alpha = 6.0;
		}

		/* Symmetric Kaiser samples include both endpoints for complementary squared half windows. */
		sum = 0.0;
		for (index = 0U; index <= size; index++) {
			coordinate = 2.0 * (double)index / (double)size - 1.0;
			kaiser[index] = synth_bessel(AAC_PI * alpha * sqrt(1.0 - coordinate * coordinate));
			sum += kaiser[index];
		}

		/* Cumulative Kaiser energy supplies KBD without any external approximation table. */
		cumulative = 0.0;
		for (index = 0U; index < size; index++) {
			cumulative += kaiser[index];
			synth_windows[length][0][index] = (float)sin(AAC_PI * ((double)index + 0.5) / (2.0 * (double)size));
			synth_windows[length][1][index] = (float)sqrt(cumulative / sum);
			angle = AAC_PI * ((double)index + 0.5) / (2.0 * (double)size);
			synth_pre_cos[length][index] = (float)cos(angle);
			synth_pre_sin[length][index] = (float)sin(angle);
			angle = AAC_PI * (double)index / (2.0 * (double)size);
			synth_post_cos[length][index] = (float)cos(angle);
			synth_post_sin[length][index] = (float)sin(angle);
		}
	}

	/* Succeeded: pthread_once publishes all immutable mathematical facts. */
	return;
}

/* Evaluate the zeroth modified Bessel function from its convergent positive series. */
static double
synth_bessel(
	double argument)
{
	double term;
	double sum;
	double square;
	unsigned order;

	/* The small fixed window arguments converge well within this bounded series. */
	term = 1.0;
	sum = 1.0;
	square = argument * argument / 4.0;
	for (order = 1U; order < 64U; order++) {
		term *= square / ((double)order * (double)order);
		sum += term;

		/* Remaining positive terms are insignificant once their relative contribution vanishes. */
		if (term < sum * 1e-16)
			break;
	}

	/* Succeeded: the window initializer receives a finite positive Bessel value. */
	return sum;
}

/* Inverse-quantize ordinary bands and normalize decoder-local PNS vectors by their energy. */
static void
synth_inverse_quant(
	struct media_aac_channel *channel,
	uint32_t *random)
{
	unsigned group;
	unsigned band;
	unsigned first_window;
	unsigned window;
	unsigned index;
	unsigned start;
	unsigned end;
	unsigned size;
	unsigned book;
	int quantized;
	double scale;
	double energy;
	float noise;

	/* Short spectra use independent windows; long spectra contain one complete band layout. */
	size = 1024U;
	if (channel->info.sequence == 2U)
		size = 128U;

	/* Reconstruct each transmitted group's bands at their deinterleaved frequency positions. */
	first_window = 0U;
	for (group = 0U; group < channel->info.groups; group++) {
		/* Zero and intensity bands are reconstructed separately and start with zero scratch. */
		for (band = 0U; band < channel->info.max_sfb; band++) {
			book = channel->books[group][band];

			/* A zero or intensity book has no independent quantized spectrum. */
			if (book == 0U || book >= 14U)
				continue;
			scale = pow(2.0, 0.25 * (double)(channel->scales[group][band] - 100));

			/* Noise energy has no spectral scalefactor bias of one hundred. */
			if (book == 13U)
				scale = pow(2.0, 0.25 * (double)channel->scales[group][band]);

			/* Each window gets its own vector even when the scalefactor is grouped. */
			for (window = 0U; window < channel->info.group_length[group]; window++) {
				start = (first_window + window) * size + channel->info.offsets[band];
				end = (first_window + window) * size + channel->info.offsets[band + 1U];

				/* PNS uses a decoder-local LCG and never a shared mutable random state. */
				if (book == 13U) {
					energy = 0.0;

					/* Measure actual vector energy so the chosen random generator does not alter loudness. */
					for (index = start; index < end; index++) {
						*random = *random * 1664525U + 1013904223U;
						noise = (float)((double)(*random >> 1U) / 1073741824.0 - 1.0);
						channel->spectrum[index] = noise;
						energy += (double)noise * noise;
					}

					/* A nonzero vector normalizes to the signalled band energy. */
					if (energy > 0.0)
						energy = scale / sqrt(energy);

					/* Publish the normalized noise spectrum at the requested energy. */
					for (index = start; index < end; index++)
						channel->spectrum[index] = (float)(channel->spectrum[index] * energy);
				} else {
					/* Ordinary bands use their original coefficient sign and normative four-thirds power. */
					for (index = start; index < end; index++) {
						quantized = channel->quantized[index];

						/* A negative quantized value keeps its sign outside the positive power table. */
						if (quantized < 0) {
							channel->spectrum[index] = (float)(-synth_powers[-quantized] * scale);
						} else {
							channel->spectrum[index] = (float)(synth_powers[quantized] * scale);
						}
					}
				}
			}
		}

		/* Advance to the next group's independent spectral windows. */
		first_window += channel->info.group_length[group];
	}

	/* Succeeded: the independent channel spectrum is ready for stereo tools. */
	return;
}

/* Apply intensity, correlated noise and M/S while keeping their book-specific rules distinct. */
static void
synth_stereo(
	struct media_aac_channel *left,
	struct media_aac_channel *right)
{
	unsigned group;
	unsigned band;
	unsigned first_window;
	unsigned window;
	unsigned start;
	unsigned end;
	unsigned index;
	unsigned size;
	unsigned left_book;
	unsigned right_book;
	double scale;
	float mid;
	float side;

	/* Stereo bands address matching windows in a common-window pair. */
	size = 1024U;
	if (left->info.sequence == 2U)
		size = 128U;

	/* Apply the pair's tools in transmitted group/band order. */
	first_window = 0U;
	for (group = 0U; group < left->info.groups; group++) {
		/* The right channel may terminate spectral bands earlier than the left. */
		for (band = 0U; band < left->info.max_sfb; band++) {
			left_book = left->books[group][band];
			right_book = right->books[group][band];

			/* Intensity position scales the reconstructed left spectrum rather than M/S components. */
			if (right_book >= 14U) {
				scale = pow(2.0, -0.25 * (double)right->scales[group][band]);

				/* Intensity codebook fourteen uses the opposite polarity from fifteen. */
				if (right_book == 14U)
					scale = -scale;

				/* An M/S bit in an intensity band reverses only the intensity polarity. */
				if (left->ms_mode == 1U && left->ms[group][band] != 0U)
					scale = -scale;
			} else if (left_book == 13U && right_book == 13U) {
				scale = pow(2.0, 0.25 * (double)(right->scales[group][band] - left->scales[group][band]));
			} else {
				scale = 1.0;
			}

			/* Apply one rule consistently over every window represented by this band. */
			for (window = 0U; window < left->info.group_length[group]; window++) {
				start = (first_window + window) * size + left->info.offsets[band];
				end = (first_window + window) * size + left->info.offsets[band + 1U];

				/* Each coefficient is either intensity, correlated PNS, M/S, or already independent. */
				for (index = start; index < end; index++) {
					/* Intensity always obtains the right coefficient from the reconstructed left. */
					if (right_book >= 14U) {
						right->spectrum[index] = (float)(left->spectrum[index] * scale);
					} else if (left_book == 13U &&
					    right_book == 13U &&
					    left->ms[group][band] != 0U) {
						right->spectrum[index] = (float)(left->spectrum[index] * scale);
					} else if (left_book < 13U &&
					    right_book < 13U &&
					    left->ms[group][band] != 0U) {
						mid = left->spectrum[index];
						side = right->spectrum[index];
						left->spectrum[index] = mid + side;
						right->spectrum[index] = mid - side;
					}
				}
			}
		}

		/* Advance both common-window channels by the same group length. */
		first_window += left->info.group_length[group];
	}

	/* Succeeded: pair spectra obey their distinct intensity, PNS and M/S interpretations. */
	return;
}

/* Convert reflection indices to prediction coefficients and perform bounded all-pole TNS filtering. */
static void
synth_tns(
	struct media_aac_channel *channel)
{
	const struct media_aac_tns *filter;
	double prediction[MEDIA_AAC_TNS_ORDER];
	double previous[MEDIA_AAC_TNS_ORDER];
	double history[MEDIA_AAC_TNS_ORDER];
	double reflection;
	double denominator;
	double sample;
	unsigned window;
	unsigned region;
	unsigned order;
	unsigned index;
	unsigned top;
	unsigned bottom;
	unsigned first;
	unsigned last;
	unsigned count;
	unsigned position;
	unsigned size;
	unsigned limit;

	/* TNS limits depend on the frequency table and this channel's signalled bands. */
	size = 1024U;
	if (channel->info.sequence == 2U)
		size = 128U;
	limit = channel->info.max_sfb;

	/* The frequency's normative TNS band limit may reduce the active range further. */
	if (channel->info.tns_limit < limit)
		limit = channel->info.tns_limit;

	/* Each window's successive regions are counted downward from the complete band layout. */
	for (window = 0U; window < channel->info.windows; window++) {
		top = channel->info.band_count;

		/* Every region retains its original top boundary even when its filter order is zero. */
		for (region = 0U; region < channel->tns_count[window]; region++) {
			filter = &channel->tns[window][region];
			bottom = 0U;

			/* Subtract the region length only within the available full band layout. */
			if (filter->length < top)
				bottom = top - filter->length;
			first = bottom;
			last = top;
			top = bottom;

			/* Clip both region ends to the channel's actual TNS spectral limit. */
			if (first > limit)
				first = limit;

			/* The upper region edge cannot extend beyond this channel's active bands. */
			if (last > limit)
				last = limit;
			first = channel->info.offsets[first];
			last = channel->info.offsets[last];

			/* Empty and zero-order regions make no spectral changes. */
			if (filter->order == 0U || first == last)
				continue;
			memset(prediction, 0, sizeof(prediction));

			/* Expand each reflection coefficient using its original uncompressed resolution. */
			for (order = 0U; order < filter->order; order++) {
				denominator = (double)((1U << filter->resolution) - 1U);

				/* Negative signed indices use the complementary asymmetric sine denominator. */
				if (filter->coefficients[order] < 0)
					denominator += 2.0;
				reflection = sin(AAC_PI * (double)filter->coefficients[order] / denominator);
				memcpy(previous, prediction, sizeof(previous));

				/* Lattice-to-prediction recursion updates each earlier coefficient symmetrically. */
				for (index = 0U; index < order; index++)
					prediction[index] = previous[index] + reflection * previous[order - 1U - index];
				prediction[order] = reflection;
			}

			/* A new region has no spectral history from the preceding filter. */
			memset(history, 0, sizeof(history));
			count = last - first;
			position = first;

			/* Reverse filters start at the highest included frequency. */
			if (filter->direction != 0U)
				position = last - 1U;

			/* Run the all-pole reconstruction in the region's signalled frequency direction. */
			for (index = 0U; index < count; index++) {
				sample = channel->spectrum[window * size + position];

				/* Previously reconstructed coefficients supply this region's prediction. */
				for (order = 0U; order < filter->order; order++)
					sample -= prediction[order] * history[order];

				/* Shift only this filter's history, keeping the newest value at the front. */
				for (order = filter->order - 1U; order != 0U; order--)
					history[order] = history[order - 1U];
				history[0] = sample;
				channel->spectrum[window * size + position] = (float)sample;

				/* Advance in the selected frequency direction without indexing past the final sample. */
				if (filter->direction != 0U) {
					position--;
				} else {
					position++;
				}
			}
		}
	}

	/* Succeeded: the reconstructed channel is ready for its inverse transform. */
	return;
}

/* Compute a DCT-IV through a twice-length complex FFT, then apply IMDCT's exact symmetry. */
static void
synth_imdct(
	const float *spectrum,
	unsigned size,
	struct media_aac_transform *transform,
	float *output)
{
	unsigned length;
	unsigned index;
	float scale;

	/* Long and short phase tables were independently generated from the analytical DCT-IV formula. */
	length = 0U;
	if (size == 128U)
		length = 1U;
	memset(transform->real, 0, sizeof(transform->real));
	memset(transform->imaginary, 0, sizeof(transform->imaginary));

	/* Pre-rotate the real coefficients, with an equally long zero tail. */
	for (index = 0U; index < size; index++) {
		transform->real[index] = spectrum[index] * synth_pre_cos[length][index];
		transform->imaginary[index] = -spectrum[index] * synth_pre_sin[length][index];
	}

	/* The original FFT supplies all frequency bins needed for the DCT-IV. */
	synth_fft(transform, size * 2U);

	/* Post-rotation extracts the real DCT-IV, independent of IMDCT reordering. */
	for (index = 0U; index < size; index++)
		transform->cosine[index] = transform->real[index] * synth_post_cos[length][index] + transform->imaginary[index] * synth_post_sin[length][index];

	/* IMDCT symmetry maps two coefficient-counts of output from the single DCT-IV. */
	scale = 1.0f / ((float)size * 32768.0f);
	for (index = 0U; index < size / 2U; index++)
		output[index] = transform->cosine[size / 2U + index] * scale;

	/* The middle half is the negated reverse of the whole DCT-IV. */
	for (index = size / 2U; index < 3U * size / 2U; index++)
		output[index] = -transform->cosine[3U * size / 2U - 1U - index] * scale;

	/* The final quarter is the negated first half of the DCT-IV. */
	for (index = 3U * size / 2U; index < 2U * size; index++)
		output[index] = -transform->cosine[index - 3U * size / 2U] * scale;

	/* Succeeded: output holds the complete normalized time-domain transform. */
	return;
}

/* Evaluate an in-place forward radix-two complex FFT using immutable maximum-size phases. */
static void
synth_fft(
	struct media_aac_transform *transform,
	unsigned length)
{
	unsigned index;
	unsigned reversed;
	unsigned bit;
	unsigned span;
	unsigned start;
	unsigned offset;
	unsigned phase;
	unsigned first;
	unsigned second;
	float temporary;
	float real;
	float imaginary;
	float cosine;
	float sine;

	/* Permute input once by incremental binary reversal. */
	reversed = 0U;
	for (index = 1U; index < length; index++) {
		bit = length / 2U;

		/* Carry through the reversed binary digits until the next zero is reached. */
		while ((reversed & bit) != 0U) {
			reversed ^= bit;
			bit /= 2U;
		}

		/* Set the next uncarrried reversed bit to finish the permutation index. */
		reversed ^= bit;

		/* Each symmetric pair is swapped only once. */
		if (index < reversed) {
			temporary = transform->real[index];
			transform->real[index] = transform->real[reversed];
			transform->real[reversed] = temporary;
			temporary = transform->imaginary[index];
			transform->imaginary[index] = transform->imaginary[reversed];
			transform->imaginary[reversed] = temporary;
		}
	}

	/* Successive butterflies combine blocks at doubling transform lengths. */
	for (span = 2U; span <= length; span *= 2U) {
		/* Each block uses the same immutable phases at the current span's stride. */
		for (start = 0U; start < length; start += span) {
			/* Pair low and high halves with a negative-exponent complex rotation. */
			for (offset = 0U; offset < span / 2U; offset++) {
				phase = offset * (2048U / span);
				cosine = synth_fft_cos[phase];
				sine = -synth_fft_sin[phase];
				first = start + offset;
				second = first + span / 2U;
				real = cosine * transform->real[second] - sine * transform->imaginary[second];
				imaginary = cosine * transform->imaginary[second] + sine * transform->real[second];
				transform->real[second] = transform->real[first] - real;
				transform->imaginary[second] = transform->imaginary[first] - imaginary;
				transform->real[first] += real;
				transform->imaginary[first] += imaginary;
			}
		}
	}

	/* Succeeded: the unnormalized forward FFT remains in decoder-owned scratch. */
	return;
}
