/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arithmetic of audio-loop (ws204-p003): the sine it plays, the mono
 * mix of what it records, the level of one frequency (a Goertzel filter
 * over a Hann window), the RMS level, and the WAV header.
 */

#include "userland/tests/audio-loop/measure.h"

#include <math.h>
#include <string.h>

/* The constant pi, which C11's <math.h> does not name. */
#define LOOP_PI			3.14159265358979323846

static double loop_mean(const double *samples, size_t count);
static void loop_put16(uint8_t *at, unsigned value);
static void loop_put32(uint8_t *at, uint32_t value);

/*
 * Writes count frames of a sine of the given frequency and amplitude (1.0
 * is full scale) into every channel; first is the frame's number since
 * the tone began, so that the calls continue one sine.
 */
void
loop_tone(
	int16_t *frames,
	size_t count,
	unsigned channels,
	uint64_t first,
	double frequency,
	unsigned rate,
	double amplitude)
{
	double cycles;
	double value;
	size_t index;
	unsigned channel;
	int16_t sample;

	/* Each frame: the sine at its time, its cycles' whole part dropped to keep the precision. */
	for (index = 0U; index < count; index++) {
		cycles = (double)(first + index) * frequency / (double)rate;
		cycles -= floor(cycles);
		value = amplitude * 32767.0 * sin(2.0 * LOOP_PI * cycles);
		sample = (int16_t)lrint(value);
		for (channel = 0U; channel < channels; channel++)
			frames[index * channels + channel] = sample;
	}
}

/*
 * Mixes count frames of channels samples into one channel scaled to -1..1
 * (the channels' average).
 */
void
loop_mono(
	const int16_t *frames,
	size_t count,
	unsigned channels,
	double *out)
{
	double sum;
	size_t index;
	unsigned channel;

	/* Each frame's average. */
	for (index = 0U; index < count; index++) {
		sum = 0.0;
		for (channel = 0U; channel < channels; channel++)
			sum += (double)frames[index * channels + channel];
		out[index] = sum / ((double)channels * 32768.0);
	}
}

/*
 * Measures the level of one frequency in count samples (dBFS): the samples
 * less their mean, through a Hann window, into a Goertzel filter; the
 * filter's magnitude over half the window's sum is the sine's amplitude.
 * LOOP_FLOOR_DB for fewer than two samples or silence.
 */
double
loop_tone_level(
	const double *samples,
	size_t count,
	double frequency,
	unsigned rate)
{
	double coefficient;
	double window_sum;
	double magnitude;
	double previous;
	double before;
	double weight;
	double power;
	double mean;
	double state;
	size_t index;

	/* Too few samples for a window. */
	if (count < 2U)
		return LOOP_FLOOR_DB;

	/* The filter's coefficient for the frequency, and the mean to take away. */
	coefficient = 2.0 * cos(2.0 * LOOP_PI * frequency / (double)rate);
	mean = loop_mean(samples, count);

	/* The windowed samples through the filter. */
	previous = 0.0;
	before = 0.0;
	window_sum = 0.0;
	for (index = 0U; index < count; index++) {
		weight = 0.5 - 0.5 * cos(2.0 * LOOP_PI * (double)index / (double)(count - 1U));
		window_sum += weight;
		state = (samples[index] - mean) * weight + coefficient * previous - before;
		before = previous;
		previous = state;
	}

	/* The magnitude, as the amplitude of a sine. */
	power = previous * previous + before * before - coefficient * previous * before;
	if (power < 0.0)
		power = 0.0;
	magnitude = sqrt(power);

	/* Succeeded: the frequency's level. */
	return loop_db(2.0 * magnitude / window_sum);
}

/*
 * Measures the RMS level of count samples less their mean (dBFS against a
 * full-scale sine, so the RMS times the square root of two).
 */
double
loop_rms_level(
	const double *samples,
	size_t count)
{
	double mean;
	double sum;
	double value;
	size_t index;

	/* Nothing to measure. */
	if (count == 0U)
		return LOOP_FLOOR_DB;

	/* The squares' sum about the mean. */
	mean = loop_mean(samples, count);
	sum = 0.0;
	for (index = 0U; index < count; index++) {
		value = samples[index] - mean;
		sum += value * value;
	}

	/* Succeeded: the level. */
	return loop_db(sqrt(2.0 * sum / (double)count));
}

/*
 * Finds the largest magnitude among count samples (1.0 is full scale), to
 * tell a clipped recording.
 */
double
loop_peak(
	const double *samples,
	size_t count)
{
	double magnitude;
	double peak;
	size_t index;

	/* The largest. */
	peak = 0.0;
	for (index = 0U; index < count; index++) {
		magnitude = fabs(samples[index]);
		if (magnitude > peak)
			peak = magnitude;
	}

	/* Succeeded: the peak. */
	return peak;
}

/*
 * Converts an amplitude (1.0 full scale) to decibels, LOOP_FLOOR_DB at the
 * quietest.
 */
double
loop_db(
	double amplitude)
{
	double level;

	/* Nothing at all. */
	if (amplitude <= 0.0)
		return LOOP_FLOOR_DB;

	/* The decibels, no quieter than the floor. */
	level = 20.0 * log10(amplitude);
	if (level < LOOP_FLOOR_DB)
		return LOOP_FLOOR_DB;

	/* Succeeded: the level. */
	return level;
}

/*
 * Fills the 44 bytes of a PCM WAV header for frames frames of 16-bit
 * samples (little-endian, as the RIFF format is).
 */
void
loop_wav_header(
	uint8_t header[LOOP_WAV_HEADER],
	unsigned rate,
	unsigned channels,
	uint32_t frames)
{
	uint32_t data_bytes;

	/* The sizes. */
	data_bytes = frames * channels * 2U;

	/* The RIFF chunk. */
	memcpy(header, "RIFF", 4U);
	loop_put32(header + 4, 36U + data_bytes);
	memcpy(header + 8, "WAVE", 4U);

	/* The format: PCM, the channels, the rate, the bytes per second and per frame, 16 bits. */
	memcpy(header + 12, "fmt ", 4U);
	loop_put32(header + 16, 16U);
	loop_put16(header + 20, 1U);
	loop_put16(header + 22, channels);
	loop_put32(header + 24, rate);
	loop_put32(header + 28, rate * channels * 2U);
	loop_put16(header + 32, channels * 2U);
	loop_put16(header + 34, 16U);

	/* The data chunk's head. */
	memcpy(header + 36, "data", 4U);
	loop_put32(header + 40, data_bytes);
}

/* Computes the mean of count samples (count is not zero). */
static double
loop_mean(
	const double *samples,
	size_t count)
{
	double sum;
	size_t index;

	/* The sum. */
	sum = 0.0;
	for (index = 0U; index < count; index++)
		sum += samples[index];

	/* Succeeded: the mean. */
	return sum / (double)count;
}

/* Stores a 16-bit little-endian value. */
static void
loop_put16(
	uint8_t *at,
	unsigned value)
{
	/* The low byte first. */
	at[0] = (uint8_t)(value & 0xffU);
	at[1] = (uint8_t)((value >> 8) & 0xffU);
}

/* Stores a 32-bit little-endian value. */
static void
loop_put32(
	uint8_t *at,
	uint32_t value)
{
	/* The low byte first. */
	at[0] = (uint8_t)(value & 0xffU);
	at[1] = (uint8_t)((value >> 8) & 0xffU);
	at[2] = (uint8_t)((value >> 16) & 0xffU);
	at[3] = (uint8_t)((value >> 24) & 0xffU);
}
