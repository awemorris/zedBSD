/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws191-p004: the host test of the Linux backend of the sound's playback
 * streams (audio-stream-linux.c and the pump, pump.c) over a stand-in
 * alsa-lib (fake-alsa.c, the library the backend opens with dlopen): the
 * ring handed over and sealed, the frames carried to the PCM, the position
 * heard, a running flush that keeps running, a stop, a drain that ends
 * once the PCM played out, an underrun told once, a client's position out
 * of the ring (BROKEN), a PCM that cannot be opened, and a close that does
 * not wait.
 */

#define _GNU_SOURCE 1

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(condition, ...) do { \
	if (!(condition)) { \
		failures++; \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

static void (*fake_play)(unsigned long frames);
static void (*fake_fail_open)(int error);
static unsigned long (*fake_written)(void);
static int (*fake_opened)(void);

static void
pause_ms(unsigned ms)
{
	struct timespec wait;

	wait.tv_sec = ms / 1000U;
	wait.tv_nsec = (long)(ms % 1000U) * 1000000L;
	nanosleep(&wait, NULL);
}

/* Takes reports until one of a kind comes (others passed over), or the time is up. */
static int
expect(struct kl_backend_audio_stream *stream, unsigned what, struct kl_backend_audio_stream_report *report, unsigned ms)
{
	unsigned waited;

	for (waited = 0U; waited < ms; waited += 5U) {
		while (kl_backend_audio_stream_next(stream, report)) {
			if (report->what == what)
				return 1;
			if (report->what == KL_BACKEND_AUDIO_READY && report->fd >= 0)
				close(report->fd);
		}
		pause_ms(5);
	}
	return 0;
}

/* Reads the position heard. */
static uint64_t
played(struct kl_backend_audio_ring *ring)
{
	return __atomic_load_n(&ring->played_position, __ATOMIC_ACQUIRE);
}

int
main(void)
{
	struct kl_backend_audio_stream_format format;
	struct kl_backend_audio_stream_report report;
	struct kl_backend_audio_stream *stream;
	struct kl_backend_audio_ring *ring;
	void *library;
	uint64_t read;
	uint64_t before;
	int drained;
	int round;
	int fd;
	int status;

	/* The stand-in's own controls, from the same library the backend opens. */
	library = dlopen(KL_BACKEND_ALSA_LIBRARY, RTLD_NOW | RTLD_LOCAL);
	CHECK(library != NULL, "the stand-in alsa-lib: %s", dlerror());
	if (library == NULL)
		return 1;
	fake_play = (void (*)(unsigned long))dlsym(library, "fake_alsa_play");
	fake_fail_open = (void (*)(int))dlsym(library, "fake_alsa_fail_open");
	fake_written = (unsigned long (*)(void))dlsym(library, "fake_alsa_written");
	fake_opened = (int (*)(void))dlsym(library, "fake_alsa_opened");

	/* 1. alsa-lib found whole: streams are made. */
	CHECK(kl_backend_audio_stream_supported() == 1, "supported");

	/* 2. A PCM that cannot be opened: FAILED, NO_DEVICE. */
	memset(&format, 0, sizeof(format));
	format.format = KL_BACKEND_AUDIO_FORMAT_S16_LE;
	format.channels = 2U;
	format.rate = 48000U;
	format.buffer_frames = 4800U;
	fake_fail_open(ENODEV);
	stream = kl_backend_audio_stream_open(&format);
	CHECK(stream != NULL, "a stream record");
	CHECK(expect(stream, KL_BACKEND_AUDIO_FAILED, &report, 500U) && report.error == KL_BACKEND_AUDIO_ERROR_NO_DEVICE, "no device: %u", report.error);
	kl_backend_audio_stream_close(stream);

	/* 3. Made: READY with the ring, sealed against a change of size. */
	stream = kl_backend_audio_stream_open(&format);
	CHECK(expect(stream, KL_BACKEND_AUDIO_READY, &report, 500U), "ready");
	fd = report.fd;
	CHECK(fd >= 0 && report.capacity_frames == 4800U && report.period_frames == 480U, "ring %d capacity %u period %u", fd, report.capacity_frames, report.period_frames);
	status = ftruncate(fd, 4096);
	CHECK(status != 0 && errno == EPERM, "the ring cannot shrink: %d %d", status, errno);
	ring = mmap(NULL, report.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(ring != MAP_FAILED && ring->version == KL_BACKEND_AUDIO_RING_VERSION && ring->capacity_frames == 4800U, "the ring's page");
	close(fd);

	/* 4. Written and started: the frames go to the PCM, as many as it holds. */
	__atomic_store_n(&ring->write_position, 3000U, __ATOMIC_RELEASE);
	CHECK(kl_backend_audio_stream_control(stream, KL_BACKEND_AUDIO_START, 1U) == 0, "start");
	CHECK(expect(stream, KL_BACKEND_AUDIO_RESULT, &report, 500U) && report.request == 1U, "start's result");
	pause_ms(50);
	read = __atomic_load_n(&ring->read_position, __ATOMIC_ACQUIRE);
	CHECK(read == 1920U && fake_written() == 1920U, "the PCM holds its buffer: read %llu", (unsigned long long)read);

	/* 5. Played: the position heard is what was read less what the PCM holds. */
	fake_play(960U);
	pause_ms(50);
	read = __atomic_load_n(&ring->read_position, __ATOMIC_ACQUIRE);
	CHECK(read == 2880U, "more carried as room opened: %llu", (unsigned long long)read);
	CHECK(played(ring) == 960U, "heard 960: %llu", (unsigned long long)played(ring));

	/* 6. A running flush: the ring and the PCM emptied, the position heard is the flush's, and it keeps running. */
	CHECK(kl_backend_audio_stream_control(stream, KL_BACKEND_AUDIO_FLUSH, 2U) == 0, "flush");
	CHECK(expect(stream, KL_BACKEND_AUDIO_RESULT, &report, 500U) && report.request == 2U, "flush's result");
	CHECK(__atomic_load_n(&ring->read_position, __ATOMIC_ACQUIRE) == 3000U && played(ring) == 3000U, "read and heard at the write position");
	__atomic_store_n(&ring->write_position, 3500U, __ATOMIC_RELEASE);
	pause_ms(50);
	CHECK(__atomic_load_n(&ring->read_position, __ATOMIC_ACQUIRE) == 3500U, "still running after the flush");

	/* 7. Dry while running: one underrun, and silence to keep the PCM going. */
	CHECK(expect(stream, KL_BACKEND_AUDIO_UNDERRUN, &report, 200U) && report.count >= 1U, "an underrun: %u", report.count);
	while (expect(stream, KL_BACKEND_AUDIO_UNDERRUN, &report, 60U))
		;
	before = fake_written();
	pause_ms(30);
	CHECK(fake_written() >= before, "silence written");
	CHECK(!expect(stream, KL_BACKEND_AUDIO_UNDERRUN, &report, 60U), "told once while the ring stays dry");

	/* 8. A drain: what is written plays out, then DRAINED. */
	__atomic_store_n(&ring->write_position, 3800U, __ATOMIC_RELEASE);
	CHECK(kl_backend_audio_stream_control(stream, KL_BACKEND_AUDIO_DRAIN, 3U) == 0, "drain");
	CHECK(expect(stream, KL_BACKEND_AUDIO_RESULT, &report, 500U) && report.request == 3U, "drain's result");
	drained = 0;
	for (round = 0; round < 20 && !drained; round++) {
		pause_ms(30);
		fake_play(4000U);
		drained = expect(stream, KL_BACKEND_AUDIO_DRAINED, &report, 30U);
	}
	CHECK(drained && report.request == 3U, "drained");
	CHECK(played(ring) == 3800U, "all heard: %llu", (unsigned long long)played(ring));

	/* 9. A position out of the ring: LOST, BROKEN; then no controls. */
	CHECK(kl_backend_audio_stream_control(stream, KL_BACKEND_AUDIO_START, 4U) == 0, "start again");
	CHECK(expect(stream, KL_BACKEND_AUDIO_RESULT, &report, 500U), "its result");
	__atomic_store_n(&ring->write_position, 3800U + 4800U + 10U, __ATOMIC_RELEASE);
	CHECK(expect(stream, KL_BACKEND_AUDIO_LOST, &report, 500U) && report.error == KL_BACKEND_AUDIO_ERROR_BROKEN, "broken: %u", report.error);
	CHECK(kl_backend_audio_stream_control(stream, KL_BACKEND_AUDIO_STOP, 5U) == ENOTCONN, "no controls after");

	/* 10. Closed without waiting; reaped once its pump ended; the PCM closed. */
	kl_backend_audio_stream_close(stream);
	pause_ms(50);
	kl_backend_audio_stream_reap();
	CHECK(fake_opened() == 0, "the PCM is closed: %d", fake_opened());
	munmap(ring, 4096U + 19200U);

	/* 11. A stream closed before it is ready, then everything joined. */
	stream = kl_backend_audio_stream_open(&format);
	kl_backend_audio_stream_close(stream);
	kl_backend_audio_stream_reap_all();
	CHECK(fake_opened() == 0, "all PCMs closed");

	if (failures != 0) {
		printf("host-pump: %d checks failed\n", failures);
		return 1;
	}
	printf("host-pump: PASS\n");
	return 0;
}
