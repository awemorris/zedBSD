/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws191-p004: the sine tone through libkeiland's sound stream
 * (kl_audio_stream_*), for the Linux guest's Keiland: two seconds of 440 Hz,
 * 16-bit stereo at 48 kHz, then a drain.  It prints the positions every
 * half second and "TONE done written=W heard=H drained=D"; the test reads
 * the line and the WAV QEMU wrote.
 *   tone [seconds]
 */

#include <keiland/keiland.h>

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define TONE_RATE	48000U
#define TONE_HZ		440.0

static void
pause_ms(unsigned ms)
{
	struct timespec wait;

	wait.tv_sec = ms / 1000U;
	wait.tv_nsec = (long)(ms % 1000U) * 1000000L;
	nanosleep(&wait, NULL);
}

int
main(int argc, char **argv)
{
	struct kl_audio_format format;
	struct kl_audio_stream *stream;
	int16_t frames[2 * 480];
	uint64_t total;
	uint64_t made;
	unsigned events;
	unsigned drained;
	unsigned index;
	unsigned waited;
	double seconds;
	double phase;
	size_t written;
	size_t done;
	int error;

	/* Two seconds, or as asked. */
	seconds = 2.0;
	if (argc > 1)
		seconds = atof(argv[1]);
	total = (uint64_t)(seconds * TONE_RATE);

	/* The stream. */
	format.format = KL_AUDIO_FORMAT_S16_LE;
	format.channels = 2U;
	format.rate = TONE_RATE;
	format.buffer_frames = TONE_RATE / 4U;
	format.period_frames = TONE_RATE / 100U;
	error = kl_audio_stream_open(&format, &stream);
	printf("TONE open error=%d\n", error);
	if (error != 0)
		return 1;
	error = kl_audio_stream_start(stream);
	printf("TONE start error=%d\n", error);

	/* The tone, written as room opens; the positions every half second. */
	made = 0U;
	waited = 0U;
	phase = 0.0;
	while (made < total) {
		for (index = 0U; index < 480U; index++) {
			frames[2U * index] = (int16_t)(sin(phase) * 12000.0);
			frames[2U * index + 1U] = frames[2U * index];
			phase += 2.0 * M_PI * TONE_HZ / TONE_RATE;
		}

		done = 0U;
		for (;;) {
			written = kl_audio_stream_write(stream, frames + 2U * done, 480U - done);
			done += written;
			if (done == 480U)
				break;
			pause_ms(5);
			waited += 5U;
			if (waited % 500U == 0U)
				printf("TONE at written=%llu heard=%llu\n", (unsigned long long)kl_audio_stream_written(stream), (unsigned long long)kl_audio_stream_position(stream, NULL));
		}

		made += 480U;
	}

	/* Played out. */
	error = kl_audio_stream_drain(stream);
	printf("TONE drain error=%d\n", error);
	drained = 0U;
	for (waited = 0U; waited < 3000U && !drained; waited += 10U) {
		events = 0U;
		(void)kl_audio_stream_dispatch(stream, &events);
		if ((events & KL_AUDIO_EVENT_DRAINED) != 0U)
			drained = 1U;
		pause_ms(10);
	}

	printf("TONE done written=%llu heard=%llu drained=%u\n", (unsigned long long)kl_audio_stream_written(stream), (unsigned long long)kl_audio_stream_position(stream, NULL), drained);
	kl_audio_stream_close(stream);
	return 0;
}
