/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws191-p004: the sine tone through the backend's playback stream alone,
 * without the compositor (FreeBSD's test guest has no GPU to run it): the
 * stream opened as the compositor opens it, its ring mapped as libkeiland
 * maps it, two seconds of 440 Hz written, started, drained.  It prints
 * "TONE done written=W read=R heard=H drained=D".
 *   cc -I. plan/ws191/tests/tone-backend.c userland/desktop/libkeiland-backend-freebsd/audio-stream-freebsd.c \
 *      userland/desktop/libkeiland-backend/audio/pump.c -lpthread -lm -o tone-backend
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define TONE_RATE	48000U

static void
pause_ms(unsigned ms)
{
	struct timespec wait;

	wait.tv_sec = ms / 1000U;
	wait.tv_nsec = (long)(ms % 1000U) * 1000000L;
	nanosleep(&wait, NULL);
}

/* Waits for a report of a kind (others printed), at most ms. */
static int
expect(struct kl_backend_audio_stream *stream, unsigned what, struct kl_backend_audio_stream_report *report, unsigned ms)
{
	unsigned waited;

	for (waited = 0U; waited < ms; waited += 5U) {
		while (kl_backend_audio_stream_next(stream, report)) {
			printf("TONE report what=%u error=%u request=%u count=%u\n", report->what, report->error, report->request, report->count);
			if (report->what == what)
				return 1;
			if (report->what == KL_BACKEND_AUDIO_FAILED || report->what == KL_BACKEND_AUDIO_LOST)
				return 0;
		}
		pause_ms(5);
	}
	return 0;
}

int
main(void)
{
	struct kl_backend_audio_stream_format format;
	struct kl_backend_audio_stream_report report;
	struct kl_backend_audio_stream *stream;
	struct kl_backend_audio_ring *ring;
	int16_t *frames;
	uint64_t total;
	uint64_t written;
	uint64_t read;
	unsigned capacity;
	unsigned index;
	double phase;
	int drained;

	/* The stream, as the compositor makes it. */
	memset(&format, 0, sizeof(format));
	format.format = KL_BACKEND_AUDIO_FORMAT_S16_LE;
	format.channels = 2U;
	format.rate = TONE_RATE;
	format.buffer_frames = TONE_RATE / 4U;
	format.period_frames = TONE_RATE / 100U;
	printf("TONE supported=%d\n", kl_backend_audio_stream_supported());
	stream = kl_backend_audio_stream_open(&format);
	if (stream == NULL || !expect(stream, KL_BACKEND_AUDIO_READY, &report, 3000U)) {
		printf("TONE done failed\n");
		return 1;
	}

	/* The ring, as libkeiland maps it. */
	ring = mmap(NULL, report.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, report.fd, 0);
	close(report.fd);
	if (ring == MAP_FAILED) {
		printf("TONE done failed map\n");
		return 1;
	}
	capacity = ring->capacity_frames;
	frames = (int16_t *)(void *)((unsigned char *)ring + KL_BACKEND_AUDIO_RING_HEADER);

	/* Started, then two seconds of the tone written as room opens. */
	(void)kl_backend_audio_stream_control(stream, KL_BACKEND_AUDIO_START, 1U);
	(void)expect(stream, KL_BACKEND_AUDIO_RESULT, &report, 1000U);
	total = 2U * TONE_RATE;
	written = 0U;
	phase = 0.0;
	while (written < total) {
		read = __atomic_load_n(&ring->read_position, __ATOMIC_ACQUIRE);
		while (written < total && written - read < capacity) {
			index = (unsigned)(written % capacity);
			frames[2U * index] = (int16_t)(sin(phase) * 12000.0);
			frames[2U * index + 1U] = frames[2U * index];
			phase += 2.0 * M_PI * 440.0 / TONE_RATE;
			written++;
		}
		__atomic_store_n(&ring->write_position, written, __ATOMIC_RELEASE);
		while (kl_backend_audio_stream_next(stream, &report))
			printf("TONE report what=%u error=%u count=%u\n", report.what, report.error, report.count);
		pause_ms(10);
	}

	/* Drained. */
	(void)kl_backend_audio_stream_control(stream, KL_BACKEND_AUDIO_DRAIN, 2U);
	drained = expect(stream, KL_BACKEND_AUDIO_DRAINED, &report, 3000U);
	printf("TONE done written=%llu read=%llu heard=%llu drained=%d\n",
	    (unsigned long long)written,
	    (unsigned long long)__atomic_load_n(&ring->read_position, __ATOMIC_ACQUIRE),
	    (unsigned long long)__atomic_load_n(&ring->played_position, __ATOMIC_ACQUIRE),
	    drained);
	kl_backend_audio_stream_close(stream);
	kl_backend_audio_stream_reap_all();
	return 0;
}
