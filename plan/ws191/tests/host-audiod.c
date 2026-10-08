/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws191-p002: the host test of zedBSD's backend of the sound's playback
 * streams (userland/desktop/libkeiland-backend-zedbsd/audio-stream-zedbsd.c)
 * against a stand-in audiod on a socket in the work directory: audiod not
 * there, no device, the stream made and its ring handed over, the controls
 * answered by serial, a drain answered at once and its DRAINED, the flush
 * of a drain sent as STOP and FLUSH with one answer, underruns gathered,
 * and audiod gone.
 */

#define _GNU_SOURCE 1

#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include "userland/base/audiod/protocol.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
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

/* The stand-in audiod: what it answers and what it saw, under lock. */
static struct {
	pthread_mutex_t lock;
	int listener;
	int client;
	unsigned device;
	unsigned close_at_accept;
	uint32_t types[64];
	unsigned type_count;
	uint32_t drain_serial;
	unsigned send_drained;
	unsigned send_underruns;
	unsigned hang_up;
	unsigned stop;
} audiod = { PTHREAD_MUTEX_INITIALIZER, -1, -1, 1U, 0U, { 0 }, 0U, 0U, 0U, 0U, 0U, 0U };

static void
pause_ms(unsigned ms)
{
	struct timespec wait;

	wait.tv_sec = ms / 1000U;
	wait.tv_nsec = (long)(ms % 1000U) * 1000000L;
	nanosleep(&wait, NULL);
}

/* Sends one message of audiod's, with a descriptor when there is one. */
static void
audiod_send(int fd, const void *message, size_t length, int descriptor)
{
	union {
		struct cmsghdr header;
		unsigned char bytes[CMSG_SPACE(sizeof(int))];
	} control;
	struct cmsghdr *item;
	struct msghdr header;
	struct iovec vector;

	memset(&header, 0, sizeof(header));
	vector.iov_base = (void *)message;
	vector.iov_len = length;
	header.msg_iov = &vector;
	header.msg_iovlen = 1;
	if (descriptor >= 0) {
		memset(&control, 0, sizeof(control));
		header.msg_control = control.bytes;
		header.msg_controllen = sizeof(control.bytes);
		item = CMSG_FIRSTHDR(&header);
		item->cmsg_level = SOL_SOCKET;
		item->cmsg_type = SCM_RIGHTS;
		item->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(item), &descriptor, sizeof(int));
	}
	(void)sendmsg(fd, &header, MSG_NOSIGNAL);
}

/* Answers one request as audiod does. */
static void
audiod_answer(int fd, const struct audiod_header *request)
{
	struct audiod_welcome welcome;
	struct audiod_stream_created created;
	struct audiod_result result;
	struct audiod_shm_header *shm;
	int memory;

	pthread_mutex_lock(&audiod.lock);
	if (audiod.type_count < 64U)
		audiod.types[audiod.type_count++] = request->type;
	pthread_mutex_unlock(&audiod.lock);

	switch (request->type) {
	case AUDIOD_HELLO:
		memset(&welcome, 0, sizeof(welcome));
		welcome.header.type = AUDIOD_WELCOME;
		welcome.header.length = sizeof(welcome);
		welcome.header.serial = request->serial;
		welcome.device = audiod.device;
		audiod_send(fd, &welcome, sizeof(welcome), -1);
		break;
	case AUDIOD_STREAM_CREATE:
		memory = memfd_create("host-audiod", MFD_CLOEXEC);
		(void)ftruncate(memory, AUDIOD_SHM_HEADER + 8192);
		shm = mmap(NULL, AUDIOD_SHM_HEADER, PROT_READ | PROT_WRITE, MAP_SHARED, memory, 0);
		shm->magic = AUDIOD_SHM_MAGIC;
		shm->version = AUDIOD_VERSION;
		munmap(shm, AUDIOD_SHM_HEADER);
		memset(&created, 0, sizeof(created));
		created.header.type = AUDIOD_STREAM_CREATED;
		created.header.length = sizeof(created);
		created.header.serial = request->serial;
		created.header.stream = request->stream;
		created.shm_bytes = AUDIOD_SHM_HEADER + 8192;
		created.capacity_frames = 2048U;
		audiod_send(fd, &created, sizeof(created), memory);
		close(memory);
		break;
	case AUDIOD_STREAM_DRAIN:
		/* No DONE for a drain; DRAINED comes later with its serial. */
		pthread_mutex_lock(&audiod.lock);
		audiod.drain_serial = request->serial;
		pthread_mutex_unlock(&audiod.lock);
		break;
	default:
		memset(&result, 0, sizeof(result));
		result.header.type = AUDIOD_DONE;
		result.header.length = sizeof(result);
		result.header.serial = request->serial;
		audiod_send(fd, &result, sizeof(result), -1);
		break;
	}
}

/* The stand-in audiod's thread: one client at a time. */
static void *
audiod_serve(void *argument)
{
	unsigned char buffer[4096];
	struct audiod_header header;
	struct audiod_event event;
	struct pollfd descriptor;
	size_t filled;
	size_t at;
	ssize_t got;
	unsigned stop;
	unsigned drained;
	unsigned underruns;
	unsigned hang_up;
	unsigned index;
	int fd;

	(void)argument;
	filled = 0U;
	fd = -1;
	for (;;) {
		pthread_mutex_lock(&audiod.lock);
		stop = audiod.stop;
		drained = audiod.send_drained;
		audiod.send_drained = 0U;
		underruns = audiod.send_underruns;
		audiod.send_underruns = 0U;
		hang_up = audiod.hang_up;
		audiod.hang_up = 0U;
		pthread_mutex_unlock(&audiod.lock);
		if (stop)
			break;

		/* A connection, or its messages. */
		descriptor.fd = fd >= 0 ? fd : audiod.listener;
		descriptor.events = POLLIN;
		descriptor.revents = 0;
		if (poll(&descriptor, 1, 5) > 0) {
			if (fd < 0) {
				fd = accept(audiod.listener, NULL, NULL);
				filled = 0U;
				if (audiod.close_at_accept && fd >= 0) {
					close(fd);
					fd = -1;
				}
			} else {
				got = read(fd, buffer + filled, sizeof(buffer) - filled);
				if (got <= 0) {
					close(fd);
					fd = -1;
					continue;
				}
				filled += (size_t)got;
				at = 0U;
				while (filled - at >= sizeof(header)) {
					memcpy(&header, buffer + at, sizeof(header));
					if (filled - at < header.length)
						break;
					audiod_answer(fd, &header);
					at += header.length;
				}
				memmove(buffer, buffer + at, filled - at);
				filled -= at;
			}
		}

		/* The test's commands to the connection. */
		if (fd >= 0 && drained) {
			memset(&event, 0, sizeof(event));
			event.header.type = AUDIOD_DRAINED;
			event.header.length = sizeof(event);
			event.header.serial = audiod.drain_serial;
			event.header.stream = 1U;
			audiod_send(fd, &event, sizeof(event), -1);
		}
		for (index = 0U; fd >= 0 && index < underruns; index++) {
			memset(&event, 0, sizeof(event));
			event.header.type = AUDIOD_UNDERRUN;
			event.header.length = sizeof(event);
			event.header.stream = 1U;
			event.count = index + 1U;
			audiod_send(fd, &event, sizeof(event), -1);
		}
		if (fd >= 0 && hang_up) {
			close(fd);
			fd = -1;
		}
	}
	if (fd >= 0)
		close(fd);
	return NULL;
}

/* Takes reports until one comes or the time is up; 1 when one came. */
static int
next_report(struct kl_backend_audio_stream *stream, struct kl_backend_audio_stream_report *report, unsigned ms)
{
	unsigned waited;
	int taken;

	for (waited = 0U; waited < ms; waited += 5U) {
		taken = kl_backend_audio_stream_next(stream, report);
		if (taken)
			return 1;
		pause_ms(5);
	}
	return 0;
}

/* Counts the stand-in audiod's requests of a type. */
static unsigned
seen(uint32_t type)
{
	unsigned index;
	unsigned count;

	count = 0U;
	pthread_mutex_lock(&audiod.lock);
	for (index = 0U; index < audiod.type_count; index++) {
		if (audiod.types[index] == type)
			count++;
	}
	pthread_mutex_unlock(&audiod.lock);
	return count;
}

int
main(void)
{
	struct kl_backend_audio_stream_format format;
	struct kl_backend_audio_stream_report report;
	struct kl_backend_audio_stream *stream;
	struct sockaddr_un address;
	pthread_t thread;
	int taken;
	int error;

	memset(&format, 0, sizeof(format));
	format.format = KL_BACKEND_AUDIO_FORMAT_S16_LE;
	format.channels = 2U;
	format.rate = 48000U;

	/* 1. audiod not there: FAILED, UNAVAILABLE. */
	unlink(HOST_AUDIOD_PATH);
	stream = kl_backend_audio_stream_open(&format);
	taken = next_report(stream, &report, 100U);
	CHECK(taken && report.what == KL_BACKEND_AUDIO_FAILED && report.error == KL_BACKEND_AUDIO_ERROR_UNAVAILABLE, "no audiod: %u %u", report.what, report.error);
	kl_backend_audio_stream_close(stream);

	/* The stand-in audiod. */
	audiod.listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", HOST_AUDIOD_PATH);
	if (bind(audiod.listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(audiod.listener, 4) != 0) {
		perror("host-audiod: listen");
		return 2;
	}
	pthread_create(&thread, NULL, audiod_serve, NULL);

	/* 2. No sound device: FAILED, NO_DEVICE. */
	audiod.device = 0U;
	stream = kl_backend_audio_stream_open(&format);
	taken = next_report(stream, &report, 500U);
	CHECK(taken && report.what == KL_BACKEND_AUDIO_FAILED && report.error == KL_BACKEND_AUDIO_ERROR_NO_DEVICE, "no device: %u %u", report.what, report.error);
	kl_backend_audio_stream_close(stream);
	pause_ms(20);

	/* 3. audiod full (it closes at once): FAILED, UNAVAILABLE. */
	audiod.device = 1U;
	audiod.close_at_accept = 1U;
	stream = kl_backend_audio_stream_open(&format);
	taken = next_report(stream, &report, 500U);
	CHECK(taken && report.what == KL_BACKEND_AUDIO_FAILED && report.error == KL_BACKEND_AUDIO_ERROR_UNAVAILABLE, "closed at accept: %u %u", report.what, report.error);
	kl_backend_audio_stream_close(stream);
	audiod.close_at_accept = 0U;
	pause_ms(20);

	/* 4. Made: READY with the ring. */
	stream = kl_backend_audio_stream_open(&format);
	taken = next_report(stream, &report, 500U);
	CHECK(taken && report.what == KL_BACKEND_AUDIO_READY && report.fd >= 0 && report.capacity_frames == 2048U, "ready: %u fd %d", report.what, report.fd);
	if (report.fd >= 0)
		close(report.fd);

	/* 5. START, then FLUSH: each answered by its own DONE. */
	error = kl_backend_audio_stream_control(stream, KL_BACKEND_AUDIO_START, 11U);
	CHECK(error == 0, "start: %d", error);
	taken = next_report(stream, &report, 500U);
	CHECK(taken && report.what == KL_BACKEND_AUDIO_RESULT && report.request == 11U && report.error == 0U, "start's result");
	error = kl_backend_audio_stream_control(stream, KL_BACKEND_AUDIO_FLUSH, 12U);
	CHECK(error == 0, "flush: %d", error);
	taken = next_report(stream, &report, 500U);
	CHECK(taken && report.what == KL_BACKEND_AUDIO_RESULT && report.request == 12U, "flush's result");
	CHECK(seen(AUDIOD_STREAM_STOP) == 0U, "a running flush sends no STOP");

	/* 6. DRAIN: answered at once; DRAINED with its serial. */
	error = kl_backend_audio_stream_control(stream, KL_BACKEND_AUDIO_DRAIN, 13U);
	CHECK(error == 0, "drain: %d", error);
	taken = next_report(stream, &report, 100U);
	CHECK(taken && report.what == KL_BACKEND_AUDIO_RESULT && report.request == 13U, "drain answered at once");
	pause_ms(30);
	pthread_mutex_lock(&audiod.lock);
	audiod.send_drained = 1U;
	pthread_mutex_unlock(&audiod.lock);
	taken = next_report(stream, &report, 500U);
	CHECK(taken && report.what == KL_BACKEND_AUDIO_DRAINED && report.request == 13U, "drained: %u %u", report.what, report.request);

	/* 7. The flush of a drain: STOP and FLUSH, one answer after both. */
	error = kl_backend_audio_stream_control(stream, KL_BACKEND_AUDIO_DRAIN, 14U);
	taken = next_report(stream, &report, 100U);
	error = kl_backend_audio_stream_control(stream, KL_BACKEND_AUDIO_FLUSH, 15U);
	CHECK(error == 0, "the drain's flush: %d", error);
	taken = next_report(stream, &report, 500U);
	CHECK(taken && report.what == KL_BACKEND_AUDIO_RESULT && report.request == 15U, "one answer for STOP and FLUSH");
	taken = next_report(stream, &report, 50U);
	CHECK(!taken, "no second answer");
	CHECK(seen(AUDIOD_STREAM_STOP) == 1U && seen(AUDIOD_STREAM_FLUSH) == 2U, "STOP %u FLUSH %u", seen(AUDIOD_STREAM_STOP), seen(AUDIOD_STREAM_FLUSH));
	pthread_mutex_lock(&audiod.lock);
	audiod.send_drained = 1U;
	pthread_mutex_unlock(&audiod.lock);
	taken = next_report(stream, &report, 100U);
	CHECK(!taken, "the ended drain's DRAINED is passed over");

	/* 8. Underruns gathered: one report of the total. */
	pthread_mutex_lock(&audiod.lock);
	audiod.send_underruns = 3U;
	pthread_mutex_unlock(&audiod.lock);
	pause_ms(50);
	taken = next_report(stream, &report, 100U);
	CHECK(taken && report.what == KL_BACKEND_AUDIO_UNDERRUN && report.count == 3U, "underruns: %u %u", report.what, report.count);
	taken = next_report(stream, &report, 50U);
	CHECK(!taken, "one report for the three");

	/* 9. audiod gone: LOST, GONE; a control then finds no stream. */
	pthread_mutex_lock(&audiod.lock);
	audiod.hang_up = 1U;
	pthread_mutex_unlock(&audiod.lock);
	taken = next_report(stream, &report, 500U);
	CHECK(taken && report.what == KL_BACKEND_AUDIO_LOST && report.error == KL_BACKEND_AUDIO_ERROR_GONE, "lost: %u %u", report.what, report.error);
	error = kl_backend_audio_stream_control(stream, KL_BACKEND_AUDIO_START, 16U);
	CHECK(error == ENOTCONN, "a lost stream's control: %d", error);
	kl_backend_audio_stream_close(stream);

	pthread_mutex_lock(&audiod.lock);
	audiod.stop = 1U;
	pthread_mutex_unlock(&audiod.lock);
	pthread_join(thread, NULL);
	close(audiod.listener);
	unlink(HOST_AUDIOD_PATH);
	if (failures != 0) {
		printf("host-audiod: %d checks failed\n", failures);
		return 1;
	}
	printf("host-audiod: PASS\n");
	return 0;
}
