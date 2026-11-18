/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * audiod's adaptive rate converter: a polyphase FIR (a
 * Kaiser-windowed sinc, stop band below -60 dB, pass band to 0.425 of
 * the lower rate, 256 phases with linear interpolation between them)
 * between two rates whose ratio a PI controller corrects by at most
 * +-0.2% to keep a ring's fill at its target.  Pure: no audiod state,
 * built alone by the host test.
 */

#ifndef AUDIOD_ASRC_H
#define AUDIOD_ASRC_H

#include <stddef.h>
#include <stdint.h>

/* The phases of the table, the most taps, and the channels. */
#define ASRC_PHASES		256U
#define ASRC_TAPS_MAX		512U
#define ASRC_CHANNELS_MAX	2U

/* The most the PI controller corrects the ratio by (0.2%). */
#define ASRC_CORRECTION_MAX	0.002

/*
 * One converter: the rates, the filter (taps a phase, the table of
 * ASRC_PHASES + 1 rows), the input history of each channel (a window of
 * twice the taps), where the next output falls in it (in input samples),
 * the nominal and the corrected step, and the PI controller's state.
 */
struct asrc {
	uint32_t rate_in;
	uint32_t rate_out;
	unsigned channels;
	unsigned taps;
	float *table;
	float history[ASRC_CHANNELS_MAX][2U * ASRC_TAPS_MAX];
	unsigned held;
	double position;
	double step_nominal;
	double step;
	double integral;
	double correction;
};

int asrc_init(struct asrc *converter, uint32_t rate_in, uint32_t rate_out, unsigned channels);
void asrc_free(struct asrc *converter);
void asrc_reset(struct asrc *converter);
size_t asrc_process(struct asrc *converter, const float *input, size_t frames_in, float *output, size_t room_out, size_t *used_in);
void asrc_steer(struct asrc *converter, double fill_error);
unsigned asrc_taps(uint32_t rate_in, uint32_t rate_out);

#endif
