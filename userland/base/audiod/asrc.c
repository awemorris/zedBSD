/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The adaptive rate converter (asrc.h, ws209-p003).
 *
 * Each output sample falls at a position in the input (in input samples):
 * the taps around it are weighed by the windowed sinc at their distance,
 * read from the table's two phases nearest the fraction and interpolated
 * between them.  The filter is designed in the input's samples: its cut
 * lies between 0.425 and 0.5 of the lower rate, so that downsampling
 * leaves no alias above the lower rate's half (8 kHz: 3.4 kHz passed, 4 kHz
 * and up 60 dB down) and upsampling no image.  The step between outputs is
 * the rates' ratio times (1 + the PI controller's correction).
 */

#include "userland/base/audiod/asrc.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The stop band's attenuation the taps are counted for (dB), and the Kaiser window's beta for it. */
#define ASRC_ATTENUATION	60.0
#define ASRC_BETA		5.653

/* The pass band's edge and the stop band's start, as fractions of the lower rate. */
#define ASRC_PASS		0.425
#define ASRC_STOP		0.5

/* The PI controller's gains: of the error (a fraction of the target), and of its sum each step. */
#define ASRC_GAIN_PROPORTIONAL	0.0005
#define ASRC_GAIN_INTEGRAL	0.00002

static double asrc_bessel_i0(double x);
static double asrc_tap(double t, double cutoff, double half);
static void asrc_compact(struct asrc *converter);

/*
 * Prepares a converter between two rates for channels (1 or 2): its taps
 * and table.  Returns 0, EINVAL or ENOMEM.
 */
int
asrc_init(
	struct asrc *converter,
	uint32_t rate_in,
	uint32_t rate_out,
	unsigned channels)
{
	double lower;
	double pass;
	double stop;
	double cutoff;
	double half;
	double t;
	unsigned phase;
	unsigned tap;
	unsigned taps;

	/* Rates audiod uses, one or two channels. */
	memset(converter, 0, sizeof(*converter));
	if (rate_in < 4000U || rate_out < 4000U || channels == 0U || channels > ASRC_CHANNELS_MAX)
		return EINVAL;

	/* The cut, in cycles of an input sample, and the taps it needs. */
	lower = (double)rate_in;
	if (rate_out < rate_in)
		lower = (double)rate_out;
	pass = ASRC_PASS * lower / (double)rate_in;
	stop = ASRC_STOP * lower / (double)rate_in;
	cutoff = (pass + stop) / 2.0;
	taps = asrc_taps(rate_in, rate_out);

	/* The table: a row a phase, and one more for the interpolation past the last. */
	converter->table = malloc(sizeof(float) * (ASRC_PHASES + 1U) * taps);
	if (converter->table == NULL)
		return ENOMEM;
	half = (double)taps / 2.0;
	for (phase = 0U; phase <= ASRC_PHASES; phase++) {
		for (tap = 0U; tap < taps; tap++) {
			t = (double)tap - (half - 1.0) - (double)phase / (double)ASRC_PHASES;
			converter->table[phase * taps + tap] = (float)asrc_tap(t, cutoff, half);
		}
	}

	/* The converter, its history silent. */
	converter->rate_in = rate_in;
	converter->rate_out = rate_out;
	converter->channels = channels;
	converter->taps = taps;
	converter->step_nominal = (double)rate_in / (double)rate_out;
	asrc_reset(converter);

	/* Succeeded: ready. */
	return 0;
}

/* Frees a converter's table. */
void
asrc_free(
	struct asrc *converter)
{
	/* The table. */
	free(converter->table);
	converter->table = NULL;
}

/* Starts a converter afresh: silent history, the nominal step, no correction (after an XRUN, design C3). */
void
asrc_reset(
	struct asrc *converter)
{
	/* A window of silence before the first output. */
	memset(converter->history, 0, sizeof(converter->history));
	converter->held = converter->taps;
	converter->position = (double)converter->taps / 2.0 - 1.0;

	/* The step and the controller. */
	converter->step = converter->step_nominal;
	converter->integral = 0.0;
	converter->correction = 0.0;
}

/*
 * Converts: takes input frames (interleaved floats) into the history while
 * there is room, and makes output frames while the history holds their
 * taps and the output has room.  Returns the output frames made; *used_in
 * says how many input frames were taken (the rest is offered again).
 */
size_t
asrc_process(
	struct asrc *converter,
	const float *input,
	size_t frames_in,
	float *output,
	size_t room_out,
	size_t *used_in)
{
	const float *row;
	const float *next;
	const float *history;
	double fraction;
	double weight;
	double sum;
	size_t made;
	size_t taken;
	size_t center;
	size_t first;
	unsigned channel;
	unsigned phase;
	unsigned tap;
	float coefficient;

	/* Until the input is taken or the output full. */
	made = 0U;
	taken = 0U;
	for (;;) {
		/* An output whose taps the history holds. */
		center = (size_t)converter->position;
		if (center + converter->taps / 2U + 1U <= converter->held && made < room_out) {
			/* The phase rows around the fraction. */
			fraction = converter->position - (double)center;
			weight = fraction * (double)ASRC_PHASES;
			phase = (unsigned)weight;
			if (phase >= ASRC_PHASES)
				phase = ASRC_PHASES - 1U;
			weight -= (double)phase;
			row = converter->table + (size_t)phase * converter->taps;
			next = row + converter->taps;
			first = center + 1U - converter->taps / 2U;

			/* Each channel: the taps weighed. */
			for (channel = 0U; channel < converter->channels; channel++) {
				history = converter->history[channel] + first;
				sum = 0.0;
				for (tap = 0U; tap < converter->taps; tap++) {
					coefficient = row[tap] + (float)weight * (next[tap] - row[tap]);
					sum += (double)coefficient * (double)history[tap];
				}

				/* The channel's sample. */
				output[made * converter->channels + channel] = (float)sum;
			}

			/* The next output's position. */
			made++;
			converter->position += converter->step;
			continue;
		}

		/* No more input, or no room for output. */
		if (taken == frames_in || made == room_out)
			break;

		/* The history full: what is no longer needed goes. */
		if (converter->held == 2U * converter->taps)
			asrc_compact(converter);

		/* One input frame into the history. */
		for (channel = 0U; channel < converter->channels; channel++)
			converter->history[channel][converter->held] = input[taken * converter->channels + channel];
		converter->held++;
		taken++;
	}

	/* Succeeded: the counts. */
	*used_in = taken;
	return made;
}

/*
 * Steers the step by a ring's fill: fill_error is (fill - target) / target.
 * A ring that fills (the reader slower) takes the step up, one that empties
 * down; the correction is held within ASRC_CORRECTION_MAX.
 */
void
asrc_steer(
	struct asrc *converter,
	double fill_error)
{
	double correction;

	/* Proportional and integral, the integral bounded as the correction is. */
	converter->integral += ASRC_GAIN_INTEGRAL * fill_error;
	if (converter->integral > ASRC_CORRECTION_MAX)
		converter->integral = ASRC_CORRECTION_MAX;
	if (converter->integral < -ASRC_CORRECTION_MAX)
		converter->integral = -ASRC_CORRECTION_MAX;
	correction = ASRC_GAIN_PROPORTIONAL * fill_error + converter->integral;
	if (correction > ASRC_CORRECTION_MAX)
		correction = ASRC_CORRECTION_MAX;
	if (correction < -ASRC_CORRECTION_MAX)
		correction = -ASRC_CORRECTION_MAX;

	/* The step. */
	converter->correction = correction;
	converter->step = converter->step_nominal * (1.0 + correction);
}

/*
 * Counts the taps a conversion needs: the Kaiser estimate for the stop
 * band's attenuation over the transition between 0.425 and 0.5 of the
 * lower rate, even, at most ASRC_TAPS_MAX.
 */
unsigned
asrc_taps(
	uint32_t rate_in,
	uint32_t rate_out)
{
	double lower;
	double width;
	double estimate;
	unsigned taps;

	/* The transition's width in cycles of an input sample. */
	lower = (double)rate_in;
	if (rate_out < rate_in)
		lower = (double)rate_out;
	width = (ASRC_STOP - ASRC_PASS) * lower / (double)rate_in;

	/* Kaiser's estimate, made even. */
	estimate = (ASRC_ATTENUATION - 7.95) / (2.285 * 2.0 * M_PI * width) + 1.0;
	taps = (unsigned)ceil(estimate);
	taps = (taps + 1U) & ~1U;
	if (taps > ASRC_TAPS_MAX)
		taps = ASRC_TAPS_MAX;

	/* Succeeded: the taps. */
	return taps;
}

/* The modified Bessel function of order 0, by its series. */
static double
asrc_bessel_i0(
	double x)
{
	double sum;
	double term;
	double half;
	unsigned k;

	/* Until the terms no longer count. */
	sum = 1.0;
	term = 1.0;
	half = x / 2.0;
	for (k = 1U; k < 50U; k++) {
		term *= (half / (double)k) * (half / (double)k);
		sum += term;
		if (term < sum * 1e-12)
			break;
	}

	/* Succeeded: the sum. */
	return sum;
}

/* One tap: the sinc of a cut (cycles of an input sample) at a distance t, under the Kaiser window of half width half. */
static double
asrc_tap(
	double t,
	double cutoff,
	double half)
{
	double sinc;
	double ratio;
	double window;
	double distance;
	double x;

	/* The sinc, its gain 1 at DC. */
	x = 2.0 * cutoff * t;
	sinc = 2.0 * cutoff;
	distance = fabs(x);
	if (distance > 1e-12)
		sinc = 2.0 * cutoff * sin(M_PI * x) / (M_PI * x);

	/* The window, nothing outside it. */
	ratio = t / half;
	if (ratio <= -1.0 || ratio >= 1.0)
		return 0.0;
	window = asrc_bessel_i0(ASRC_BETA * sqrt(1.0 - ratio * ratio)) / asrc_bessel_i0(ASRC_BETA);

	/* Succeeded: the tap. */
	return sinc * window;
}

/* Moves the history's last taps to its start; the position follows. */
static void
asrc_compact(
	struct asrc *converter)
{
	size_t keep;
	size_t drop;
	unsigned channel;

	/* What the next outputs still need: from a tap's half before the position. */
	keep = converter->taps;
	drop = converter->held - keep;
	for (channel = 0U; channel < converter->channels; channel++)
		memmove(converter->history[channel], converter->history[channel] + drop, keep * sizeof(float));

	/* Moved. */
	converter->held = keep;
	converter->position -= (double)drop;
}
