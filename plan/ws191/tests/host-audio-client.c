/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws191-p002: the host test of the sound's playback streams, both ends.
 * libkeiland's kl_audio_stream_* (userland/desktop/libkeiland/audio/) over
 * the tree's libwayland, against the compositor's audio-stream.c served by
 * the stand-in of host-audio-server.c: the open and its refusals, the ring
 * written and read, the controls' results and the state they leave, the
 * flush that keeps a stream running and the position never behind it, the
 * drain, the underrun told once after a control, the limit of a process's
 * streams, the stream lost and its silent sink, and the backend's stream
 * closed with the object.
 *
 * Exit 0 when every check passes; each failure is printed.
 */

#include <keiland/keiland.h>

#include "host-audio.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Sleeps for some milliseconds (the server thread's passes are 5 ms apart). */
static void
pause_ms(unsigned ms)
{
	struct timespec wait;

	wait.tv_sec = ms / 1000U;
	wait.tv_nsec = (long)(ms % 1000U) * 1000000L;
	nanosleep(&wait, NULL);
}

/* Sets a command for the server's next pass. */
static void
command(uint64_t play, unsigned underrun, unsigned drained, unsigned lose)
{
	pthread_mutex_lock(&host_audio.lock);
	host_audio.play_frames = play;
	host_audio.underrun = underrun;
	host_audio.drained = drained;
	host_audio.lose = lose;
	pthread_mutex_unlock(&host_audio.lock);
	pause_ms(30);
}

/* Dispatches for a while and gathers the events. */
static unsigned
gather(struct kl_audio_stream *stream, unsigned ms)
{
	unsigned all;
	unsigned events;
	unsigned waited;

	all = 0U;
	for (waited = 0U; waited < ms; waited += 5U) {
		events = 0U;
		(void)kl_audio_stream_dispatch(stream, &events);
		all |= events;
		pause_ms(5);
	}
	return all;
}

int
main(void)
{
	struct kl_audio_format format;
	struct kl_audio_stream *stream;
	struct kl_audio_stream *many[9];
	int16_t frames[2 * 1000];
	char path[256];
	uint64_t position;
	uint64_t before;
	unsigned events;
	unsigned index;
	unsigned closes;
	size_t written;
	int error;

	/* The stand-in compositor, named by WAYLAND_DISPLAY. */
	snprintf(path, sizeof(path), "%s/host-audio.%ld.sock", getenv("HOST_AUDIO_DIR") != NULL ? getenv("HOST_AUDIO_DIR") : "/tmp", (long)getpid());
	host_audio_server_start(path);
	memset(frames, 0, sizeof(frames));
	memset(&format, 0, sizeof(format));
	format.format = KL_AUDIO_FORMAT_S16_LE;
	format.channels = 2U;
	format.rate = 48000U;
	format.buffer_frames = 24000U;
	format.period_frames = 960U;

	/* 1. No display named, or an inherited socket: not opened. */
	unsetenv("WAYLAND_DISPLAY");
	error = kl_audio_stream_open(&format, &stream);
	CHECK(error == ENOTSUP, "no WAYLAND_DISPLAY: %d", error);
	setenv("WAYLAND_DISPLAY", path, 1);
	setenv("WAYLAND_SOCKET", "99", 1);
	error = kl_audio_stream_open(&format, &stream);
	CHECK(error == ENOTSUP, "WAYLAND_SOCKET set: %d", error);
	unsetenv("WAYLAND_SOCKET");

	/* 2. What the streams carry: three channels and a ring under 10 ms are refused. */
	format.channels = 3U;
	error = kl_audio_stream_open(&format, &stream);
	CHECK(error == EINVAL, "three channels: %d", error);
	format.channels = 2U;
	format.buffer_frames = 100U;
	error = kl_audio_stream_open(&format, &stream);
	CHECK(error == EINVAL, "a ring under 10 ms: %d", error);
	format.buffer_frames = 24000U;

	/* 3. A backend that refuses (no device). */
	pthread_mutex_lock(&host_audio.lock);
	host_audio.fail_next_open = 2U;
	pthread_mutex_unlock(&host_audio.lock);
	error = kl_audio_stream_open(&format, &stream);
	CHECK(error == ENODEV, "no device: %d", error);

	/* 4. Opened: the ring as asked, nothing written. */
	error = kl_audio_stream_open(&format, &stream);
	CHECK(error == 0, "open: %d", error);
	if (error != 0)
		return 1;
	CHECK(kl_audio_stream_capacity(stream) == 24000U, "capacity %u", kl_audio_stream_capacity(stream));
	CHECK(kl_audio_stream_written(stream) == 0U && kl_audio_stream_consumed(stream) == 0U, "fresh positions");

	/* 5. Written, then a full ring takes no more. */
	written = 0U;
	for (index = 0U; index < 30U; index++)
		written += kl_audio_stream_write(stream, frames, 1000U);
	CHECK(written == 24000U, "the ring holds 24000 frames: %zu", written);
	CHECK(kl_audio_stream_written(stream) == 24000U, "written 24000");

	/* 6. Stopped, nothing plays; started, frames are consumed and heard. */
	command(1000U, 0U, 0U, 0U);
	CHECK(kl_audio_stream_consumed(stream) == 0U, "a stopped stream consumes nothing");
	error = kl_audio_stream_start(stream);
	CHECK(error == 0, "start: %d", error);
	command(1000U, 0U, 0U, 0U);
	CHECK(kl_audio_stream_consumed(stream) == 1000U, "consumed 1000: %llu", (unsigned long long)kl_audio_stream_consumed(stream));
	CHECK(kl_audio_stream_position(stream, NULL) == 1000U, "heard 1000");

	/* 7. A running flush keeps the stream running, and the position is never behind it. */
	error = kl_audio_stream_flush(stream);
	CHECK(error == 0, "flush: %d", error);
	CHECK(kl_audio_stream_consumed(stream) == 24000U, "the flush consumed the ring");
	CHECK(kl_audio_stream_position(stream, NULL) >= 24000U, "the position is not behind the flush: %llu", (unsigned long long)kl_audio_stream_position(stream, NULL));
	(void)kl_audio_stream_write(stream, frames, 1000U);
	command(500U, 0U, 0U, 0U);
	CHECK(kl_audio_stream_consumed(stream) == 24500U, "still running after the flush: %llu", (unsigned long long)kl_audio_stream_consumed(stream));

	/* 8. Stopped, flushed and started again: the position moves by what is newly heard (BL-1). */
	error = kl_audio_stream_stop(stream);
	CHECK(error == 0, "stop: %d", error);
	(void)kl_audio_stream_write(stream, frames, 1000U);
	error = kl_audio_stream_flush(stream);
	CHECK(error == 0, "stopped flush: %d", error);
	before = kl_audio_stream_position(stream, NULL);
	CHECK(before == 26000U, "the position after a stopped flush is the flush's: %llu", (unsigned long long)before);
	(void)kl_audio_stream_write(stream, frames, 1000U);
	error = kl_audio_stream_start(stream);
	CHECK(error == 0, "start again: %d", error);
	command(300U, 0U, 0U, 0U);
	position = kl_audio_stream_position(stream, NULL);
	CHECK(position - before == 300U, "the position moved by what was heard: %llu", (unsigned long long)(position - before));

	/* 9. Underruns: told once after a control, however many come. */
	command(0U, 5U, 0U, 0U);
	command(0U, 6U, 0U, 0U);
	events = gather(stream, 50U);
	CHECK((events & KL_AUDIO_EVENT_UNDERRUN) != 0U, "an underrun is told");
	events = gather(stream, 20U);
	command(0U, 7U, 0U, 0U);
	events = gather(stream, 50U);
	CHECK((events & KL_AUDIO_EVENT_UNDERRUN) == 0U, "only once until the next control");

	/* 10. A drain: answered, then drained told. */
	error = kl_audio_stream_drain(stream);
	CHECK(error == 0, "drain: %d", error);
	command(0U, 0U, 1U, 0U);
	events = gather(stream, 50U);
	CHECK((events & KL_AUDIO_EVENT_DRAINED) != 0U, "drained is told");

	/* 11. A control the backend cannot take now: EAGAIN. */
	pthread_mutex_lock(&host_audio.lock);
	host_audio.refuse_next_control = EAGAIN;
	pthread_mutex_unlock(&host_audio.lock);
	error = kl_audio_stream_start(stream);
	CHECK(error == EAGAIN, "a refused control: %d", error);

	/* 12. A process holds eight streams; the ninth is refused. */
	for (index = 0U; index < 9U; index++)
		many[index] = NULL;
	for (index = 0U; index < 7U; index++) {
		error = kl_audio_stream_open(&format, &many[index]);
		CHECK(error == 0, "stream %u: %d", index + 2U, error);
	}
	error = kl_audio_stream_open(&format, &many[7]);
	CHECK(error == EMFILE, "the ninth stream: %d", error);
	pthread_mutex_lock(&host_audio.lock);
	closes = host_audio.closes;
	pthread_mutex_unlock(&host_audio.lock);
	for (index = 0U; index < 9U; index++)
		kl_audio_stream_close(many[index]);
	pause_ms(50);
	pthread_mutex_lock(&host_audio.lock);
	CHECK(host_audio.closes == closes + 7U, "the closed streams' backends are closed: %u", host_audio.closes - closes);
	pthread_mutex_unlock(&host_audio.lock);

	/* 13. Lost: the controls answer EPIPE, and the sink plays on while running. */
	error = kl_audio_stream_start(stream);
	CHECK(error == 0, "running before the loss: %d", error);
	command(0U, 0U, 0U, 1U);
	events = gather(stream, 50U);
	CHECK((events & KL_AUDIO_EVENT_LOST) != 0U, "lost is told");
	for (index = 0U; index < 20U; index++)
		(void)kl_audio_stream_write(stream, frames, 1000U);
	before = kl_audio_stream_position(stream, NULL);
	pause_ms(20);
	position = kl_audio_stream_position(stream, NULL);
	CHECK(position > before, "the sink plays on: %llu -> %llu", (unsigned long long)before, (unsigned long long)position);
	CHECK(kl_audio_stream_consumed(stream) <= kl_audio_stream_written(stream), "never past what is written");
	error = kl_audio_stream_stop(stream);
	CHECK(error == EPIPE, "a lost stream's stop: %d", error);
	before = kl_audio_stream_position(stream, NULL);
	pause_ms(20);
	CHECK(kl_audio_stream_position(stream, NULL) == before, "the stopped sink stands still");

	/* 14. Closed: the stream's backend side went at the loss. */
	kl_audio_stream_close(stream);
	pause_ms(30);

	/* The outcome. */
	pthread_mutex_lock(&host_audio.lock);
	CHECK(host_audio.protocol_errors == 0U, "protocol errors: %u", host_audio.protocol_errors);
	pthread_mutex_unlock(&host_audio.lock);
	host_audio_server_stop();
	unlink(path);
	if (failures != 0) {
		printf("host-audio-stream: %d checks failed\n", failures);
		return 1;
	}
	printf("host-audio-stream: PASS\n");
	return 0;
}
