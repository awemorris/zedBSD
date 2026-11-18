/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arithmetic of audio-loop (ws204-p003): the test tone, the levels of
 * what was recorded and the WAV file's header.  Without system calls; the
 * host test builds it.
 *
 * Levels are in dBFS against a full-scale sine: a sine whose peak is the
 * largest sample reads 0 dB, both as a tone level and as an RMS level.
 */

#ifndef AUDIO_LOOP_MEASURE_H
#define AUDIO_LOOP_MEASURE_H

#include <stddef.h>
#include <stdint.h>

/* The level reported for silence (and anything quieter). */
#define LOOP_FLOOR_DB		(-150.0)

/* The bytes of a canonical PCM WAV header. */
#define LOOP_WAV_HEADER		44U

void loop_tone(int16_t *frames, size_t count, unsigned channels, uint64_t first, double frequency, unsigned rate, double amplitude);
void loop_mono(const int16_t *frames, size_t count, unsigned channels, double *out);
double loop_tone_level(const double *samples, size_t count, double frequency, unsigned rate);
double loop_rms_level(const double *samples, size_t count);
double loop_peak(const double *samples, size_t count);
double loop_db(double amplitude);
void loop_wav_header(uint8_t header[LOOP_WAV_HEADER], unsigned rate, unsigned channels, uint32_t frames);

#endif
