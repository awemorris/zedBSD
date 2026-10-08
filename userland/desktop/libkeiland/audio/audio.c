/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sound's playback streams, kl_audio_stream_* (WS191,
 * plan/ws191/design.md sections 4 and 5).
 *
 * A stream has its own connection to the compositor (WAYLAND_DISPLAY):
 * kl_audio_v1's create_stream makes it, and its ready event hands over the
 * ring, a shared memory of a page of positions and the frames, which is
 * mapped here.  The frames are written into the ring and the write position
 * advanced; the device side advances the read position and reckons the
 * position heard.  The connection carries only the controls and what comes
 * of them.  A stream whose service went becomes a silent sink: its
 * positions advance by the monotonic clock while it runs.
 */

#include <keiland/keiland.h>

#include <wayland-client.h>

#include "audio-protocol.h"
#include "userland/desktop/libkeiland/audio/kl-audio-protocol.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The ring's positions are 8-byte atomics; a system without lock-free ones is not one the streams run on. */
_Static_assert(__GCC_ATOMIC_LLONG_LOCK_FREE == 2, "8-byte atomics must be lock-free");

/* How long an open or a control waits for the compositor (ms). */
#define AUDIO_WAIT_MS		2000

/* The nanoseconds of a second. */
#define AUDIO_NS		1000000000LL

/*
 * One stream.
 *
 * lock orders the connection (the controls, the dispatch) and the fields
 * the events fill; the writing thread uses the ring and written without
 * it.  The sink's fields are written under lock with sink_sequence odd,
 * and read by any thread without lock through that sequence.  lost and
 * written are read by any thread with atomic loads.
 */
struct kl_audio_stream {
	pthread_mutex_t lock;
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_proxy *audio;
	struct wl_proxy *stream;

	/* The ring, and what its frames are. */
	unsigned char *ring;
	size_t ring_bytes;
	unsigned frame_bytes;
	unsigned capacity;
	unsigned rate;

	/* The frames written (the writer's; others read it atomically), and the floor of the position after a flush. */
	uint64_t written;
	uint64_t flush_floor;

	/* The registry's kl_audio_v1, and the round trip's end. */
	uint32_t audio_name;
	unsigned synced;

	/* What the events told: ready (with its ring), failed, the result awaited, the events for dispatch. */
	unsigned ready;
	int ready_fd;
	uint32_t ready_bytes;
	uint32_t ready_capacity;
	unsigned failed;
	uint32_t failed_error;
	uint32_t next_request;
	uint32_t awaited;
	unsigned answered;
	uint32_t answer_error;
	unsigned events;

	/* Whether the stream runs (as the last control left it), for the sink. */
	unsigned running;

	/* The silent sink: lost (atomic), and its anchors under sink_sequence. */
	unsigned lost;
	uint32_t sink_sequence;
	uint64_t sink_consumed;
	uint64_t sink_played;
	int64_t sink_ns;
	unsigned sink_running;
};

static int audio_connect(struct kl_audio_stream *stream, const struct kl_audio_format *format);
static int audio_map(struct kl_audio_stream *stream, const struct kl_audio_format *format);
static int audio_control(struct kl_audio_stream *stream, uint32_t opcode, unsigned starts, unsigned stops);
static int audio_wait(struct kl_audio_stream *stream, int (*done)(const struct kl_audio_stream *stream));
static int audio_read(struct kl_audio_stream *stream, int timeout_ms);
static void audio_lose(struct kl_audio_stream *stream);
static void audio_sink_anchor(struct kl_audio_stream *stream, unsigned running);
static void audio_sink_read(const struct kl_audio_stream *stream, uint64_t *consumed, uint64_t *played, int64_t *time_ns);
static uint64_t audio_ring_load(const struct kl_audio_stream *stream, unsigned offset);
static void audio_ring_played(const struct kl_audio_stream *stream, uint64_t *played, int64_t *time_ns);
static int64_t audio_now_ns(void);
static int audio_errno(uint32_t error);
static int audio_synced_done(const struct kl_audio_stream *stream);
static int audio_ready_done(const struct kl_audio_stream *stream);
static int audio_answer_done(const struct kl_audio_stream *stream);
static void audio_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void audio_global_remove(void *data, struct wl_registry *registry, uint32_t name);
static void audio_sync_done(void *data, struct wl_callback *callback, uint32_t serial);
static void audio_on_ready(void *data, struct wl_proxy *proxy, int32_t fd, uint32_t bytes, uint32_t capacity, uint32_t period);
static void audio_on_failed(void *data, struct wl_proxy *proxy, uint32_t error);
static void audio_on_result(void *data, struct wl_proxy *proxy, uint32_t request, uint32_t error);
static void audio_on_drained(void *data, struct wl_proxy *proxy, uint32_t request);
static void audio_on_underrun(void *data, struct wl_proxy *proxy, uint32_t count);
static void audio_on_lost(void *data, struct wl_proxy *proxy, uint32_t error);

/* The registry's callbacks: kl_audio_v1 is looked for. */
static const struct wl_registry_listener audio_registry_listener = {
	audio_global,
	audio_global_remove
};

/* The round trip's callback. */
static const struct wl_callback_listener audio_sync_listener = {
	audio_sync_done
};

/* A stream's events, in kl_audio_stream_v1's order. */
static const struct {
	void (*ready)(void *data, struct wl_proxy *proxy, int32_t fd, uint32_t bytes, uint32_t capacity, uint32_t period);
	void (*failed)(void *data, struct wl_proxy *proxy, uint32_t error);
	void (*result)(void *data, struct wl_proxy *proxy, uint32_t request, uint32_t error);
	void (*drained)(void *data, struct wl_proxy *proxy, uint32_t request);
	void (*underrun)(void *data, struct wl_proxy *proxy, uint32_t count);
	void (*lost)(void *data, struct wl_proxy *proxy, uint32_t error);
} audio_stream_listener = {
	audio_on_ready,
	audio_on_failed,
	audio_on_result,
	audio_on_drained,
	audio_on_underrun,
	audio_on_lost
};

/*
 * Opens a stream and waits until the desktop made it.
 */
int
kl_audio_stream_open(
	const struct kl_audio_format *format,
	struct kl_audio_stream **result)
{
	struct kl_audio_stream *stream;
	const char *variable;
	int error;

	/* A format and a place for the stream. */
	if (format == NULL || result == NULL)
		return EINVAL;
	*result = NULL;

	/* A connection made by name only: an inherited socket is the program's own, and an unnamed display may be another. */
	variable = getenv("WAYLAND_SOCKET");
	if (variable != NULL)
		return ENOTSUP;
	variable = getenv("WAYLAND_DISPLAY");
	if (variable == NULL || variable[0] == '\0')
		return ENOTSUP;

	/* The record. */
	stream = calloc(1, sizeof(*stream));
	if (stream == NULL)
		return ENOMEM;
	stream->ready_fd = -1;
	stream->next_request = 1U;
	error = pthread_mutex_init(&stream->lock, NULL);
	if (error != 0) {
		free(stream);
		return error;
	}

	/* The connection, the stream made, and its ring mapped. */
	error = audio_connect(stream, format);
	if (error == 0)
		error = audio_map(stream, format);
	if (error != 0) {
		kl_audio_stream_close(stream);
		return error;
	}

	/* Succeeded: the stream is the caller's, stopped. */
	*result = stream;
	return 0;
}

/*
 * Closes a stream: its object, its connection and its ring.
 */
void
kl_audio_stream_close(
	struct kl_audio_stream *stream)
{
	/* Nothing to close. */
	if (stream == NULL)
		return;

	/* The objects, then the connection (the compositor closes the stream's backend side with the object). */
	if (stream->stream != NULL)
		(void)wl_proxy_marshal_flags(stream->stream, KL_AUDIO_STREAM_DESTROY, NULL, KL_AUDIO_VERSION, WL_MARSHAL_FLAG_DESTROY);
	if (stream->audio != NULL)
		(void)wl_proxy_marshal_flags(stream->audio, KL_AUDIO_DESTROY, NULL, KL_AUDIO_VERSION, WL_MARSHAL_FLAG_DESTROY);
	if (stream->registry != NULL)
		wl_registry_destroy(stream->registry);
	if (stream->display != NULL) {
		(void)wl_display_flush(stream->display);
		wl_display_disconnect(stream->display);
	}

	/* A ring handed over and not mapped, and the mapping. */
	if (stream->ready_fd >= 0)
		(void)close(stream->ready_fd);
	if (stream->ring != NULL)
		(void)munmap(stream->ring, stream->ring_bytes);

	/* The record. */
	(void)pthread_mutex_destroy(&stream->lock);
	free(stream);
}

/*
 * Starts the device reading the ring.
 */
int
kl_audio_stream_start(
	struct kl_audio_stream *stream)
{
	int error;

	/* start: the stream runs from now. */
	error = audio_control(stream, KL_AUDIO_STREAM_START, 1U, 0U);
	if (error != 0)
		return error;

	/* Succeeded: running. */
	return 0;
}

/*
 * Stops the device reading the ring (a pause).
 */
int
kl_audio_stream_stop(
	struct kl_audio_stream *stream)
{
	int error;

	/* stop: what is written stays. */
	error = audio_control(stream, KL_AUDIO_STREAM_STOP, 0U, 1U);
	if (error != 0)
		return error;

	/* Succeeded: stopped. */
	return 0;
}

/*
 * Drops what is written and not read.
 */
int
kl_audio_stream_flush(
	struct kl_audio_stream *stream)
{
	int error;

	/* flush: a running stream keeps running. */
	error = audio_control(stream, KL_AUDIO_STREAM_FLUSH, 0U, 0U);
	if (error != 0)
		return error;

	/* Succeeded: the ring is empty. */
	return 0;
}

/*
 * Plays what is written, then stops; KL_AUDIO_EVENT_DRAINED tells the end.
 */
int
kl_audio_stream_drain(
	struct kl_audio_stream *stream)
{
	int error;

	/* drain: the stream runs until the ring is played out. */
	error = audio_control(stream, KL_AUDIO_STREAM_DRAIN, 1U, 0U);
	if (error != 0)
		return error;

	/* Succeeded: draining. */
	return 0;
}

/*
 * Writes frames into the ring, as many as there is room for.
 */
size_t
kl_audio_stream_write(
	struct kl_audio_stream *stream,
	const void *frames,
	size_t count)
{
	const unsigned char *source;
	unsigned char *place;
	uint64_t consumed;
	uint64_t held;
	size_t room;
	size_t first;
	size_t index;
	int status;

	/* The room: the ring less what is written and not consumed. */
	consumed = kl_audio_stream_consumed(stream);
	held = stream->written - consumed;
	room = 0U;
	if (held < stream->capacity)
		room = (size_t)(stream->capacity - held);

	/*
	 * A full ring: what the desktop told is read meanwhile (kept for the
	 * next dispatch), so that the compositor never waits for a reader and a
	 * lost stream is known; another thread holding the stream does it.
	 */
	if (room == 0U) {
		status = pthread_mutex_trylock(&stream->lock);
		if (status != 0)
			return 0U;
		if (!stream->lost)
			(void)audio_read(stream, 0);
		pthread_mutex_unlock(&stream->lock);
		return 0U;
	}

	/* The frames that fit, in at most two parts (the ring wraps). */
	if (count > room)
		count = room;
	source = frames;
	index = (size_t)(stream->written % stream->capacity);
	first = stream->capacity - index;
	if (first > count)
		first = count;
	place = stream->ring + KL_AUDIO_RING_HEADER;
	memcpy(place + index * stream->frame_bytes, source, first * stream->frame_bytes);
	if (count > first)
		memcpy(place, source + first * stream->frame_bytes, (count - first) * stream->frame_bytes);

	/* The frames published: the device side may read them now. */
	__atomic_store_n(&stream->written, stream->written + count, __ATOMIC_RELEASE);
	__atomic_store_n((uint64_t *)(void *)(stream->ring + KL_AUDIO_RING_WRITE_POSITION), stream->written, __ATOMIC_RELEASE);

	/* Succeeded: the frames written. */
	return count;
}

/*
 * Gives the frames written.
 */
uint64_t
kl_audio_stream_written(
	const struct kl_audio_stream *stream)
{
	uint64_t written;

	/* The writer's count. */
	written = __atomic_load_n(&stream->written, __ATOMIC_ACQUIRE);

	/* Succeeded: the frames written. */
	return written;
}

/*
 * Gives the frames the device side took from the ring.
 */
uint64_t
kl_audio_stream_consumed(
	const struct kl_audio_stream *stream)
{
	uint64_t consumed;
	uint64_t played;
	unsigned lost;

	/* A lost stream's sink. */
	lost = __atomic_load_n(&stream->lost, __ATOMIC_ACQUIRE);
	if (lost) {
		audio_sink_read(stream, &consumed, &played, NULL);
		return consumed;
	}

	/* The ring's read position. */
	consumed = audio_ring_load(stream, KL_AUDIO_RING_READ_POSITION);

	/* Succeeded: the frames consumed. */
	return consumed;
}

/*
 * Gives the position heard, and when it was reckoned.
 */
uint64_t
kl_audio_stream_position(
	const struct kl_audio_stream *stream,
	int64_t *time_ns)
{
	uint64_t consumed;
	uint64_t played;
	uint64_t floor;
	int64_t when;
	unsigned lost;

	/* A lost stream's sink, or the ring's reckoning. */
	lost = __atomic_load_n(&stream->lost, __ATOMIC_ACQUIRE);
	if (lost) {
		audio_sink_read(stream, &consumed, &played, &when);
	} else {
		audio_ring_played(stream, &played, &when);
	}

	/* Never behind what a flush dropped (the device side reckons the position only while it plays). */
	floor = __atomic_load_n(&stream->flush_floor, __ATOMIC_ACQUIRE);
	if (played < floor)
		played = floor;

	/* The time, when asked for. */
	if (time_ns != NULL)
		*time_ns = when;

	/* Succeeded: the position heard. */
	return played;
}

/*
 * Gives the ring's frames.
 */
unsigned
kl_audio_stream_capacity(
	const struct kl_audio_stream *stream)
{
	/* Succeeded: the ring's frames. */
	return stream->capacity;
}

/*
 * Takes what the desktop told, without waiting.
 */
int
kl_audio_stream_dispatch(
	struct kl_audio_stream *stream,
	unsigned *events)
{
	unsigned lost;
	int status;

	/* Another thread holds the stream: nothing is taken now. */
	if (events != NULL)
		*events = 0U;
	status = pthread_mutex_trylock(&stream->lock);
	if (status != 0)
		return 0;

	/* What came, without waiting. */
	if (!stream->lost)
		(void)audio_read(stream, 0);

	/* The events gathered, given once. */
	if (events != NULL)
		*events = stream->events;
	stream->events = 0U;
	lost = stream->lost;

	pthread_mutex_unlock(&stream->lock);

	/* A lost stream says so. */
	if (lost)
		return EPIPE;

	/* Succeeded: the events taken. */
	return 0;
}

/* Connects, finds kl_audio_v1, makes the stream and waits for ready or failed. Returns 0 or an errno value. */
static int
audio_connect(
	struct kl_audio_stream *stream,
	const struct kl_audio_format *format)
{
	struct wl_callback *callback;
	int error;

	/* The connection to the compositor named by WAYLAND_DISPLAY. */
	stream->display = wl_display_connect(NULL);
	if (stream->display == NULL)
		return ENOTSUP;

	/* The globals, and a round trip so that all are announced. */
	stream->registry = wl_display_get_registry(stream->display);
	if (stream->registry == NULL)
		return ENOMEM;
	(void)wl_registry_add_listener(stream->registry, &audio_registry_listener, stream);
	callback = wl_display_sync(stream->display);
	if (callback == NULL)
		return ENOMEM;
	(void)wl_callback_add_listener(callback, &audio_sync_listener, stream);
	error = audio_wait(stream, audio_synced_done);
	if (error != 0)
		return error;

	/* No kl_audio_v1: not Keiland, another user's, or a backend without streams. */
	if (stream->audio_name == 0U)
		return ENOTSUP;

	/* kl_audio_v1, then the stream. */
	stream->audio = wl_registry_bind(stream->registry, stream->audio_name, &kl_audio_v1_interface, KL_AUDIO_VERSION);
	if (stream->audio == NULL)
		return ENOMEM;
	stream->stream = wl_proxy_marshal_flags(stream->audio, KL_AUDIO_CREATE_STREAM, &kl_audio_stream_v1_interface, KL_AUDIO_VERSION, 0U,
	    NULL,
	    format->format,
	    format->channels,
	    format->rate,
	    format->buffer_frames,
	    format->period_frames);
	if (stream->stream == NULL)
		return ENOMEM;
	(void)wl_proxy_add_listener(stream->stream, (void (**)(void))&audio_stream_listener, stream);

	/* ready, or failed. */
	error = audio_wait(stream, audio_ready_done);
	if (error != 0)
		return error;

	/* Refused. */
	if (stream->failed) {
		error = audio_errno(stream->failed_error);
		return error;
	}

	/* Succeeded: the ring came. */
	return 0;
}

/* Checks and maps the ring the ready event handed over. Returns 0, EPROTO or an errno value. */
static int
audio_map(
	struct kl_audio_stream *stream,
	const struct kl_audio_format *format)
{
	struct stat status;
	const uint32_t *words;
	uint64_t needed;
	unsigned sample_bytes;
	void *memory;
	int result;

	/* What a frame is. */
	sample_bytes = 4U;
	if (format->format == KL_AUDIO_FORMAT_S16_LE)
		sample_bytes = 2U;
	stream->frame_bytes = sample_bytes * format->channels;

	/* The memory must hold the page and the frames it claims. */
	needed = KL_AUDIO_RING_HEADER + (uint64_t)stream->ready_capacity * stream->frame_bytes;
	if (stream->ready_capacity == 0U || stream->ready_bytes < needed)
		return EPROTO;
	result = fstat(stream->ready_fd, &status);
	if (result != 0)
		return errno;
	if ((uint64_t)status.st_size < stream->ready_bytes)
		return EPROTO;

	/* Mapped, and the descriptor closed. */
	memory = mmap(NULL, stream->ready_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, stream->ready_fd, 0);
	if (memory == MAP_FAILED)
		return errno;
	(void)close(stream->ready_fd);
	stream->ready_fd = -1;
	stream->ring = memory;
	stream->ring_bytes = stream->ready_bytes;

	/* The page says what was asked for. */
	words = (const uint32_t *)(const void *)stream->ring;
	if (words[KL_AUDIO_RING_VERSION / 4U] != KL_AUDIO_RING_VERSION_VALUE)
		return EPROTO;
	if (words[KL_AUDIO_RING_FORMAT / 4U] != format->format)
		return EPROTO;
	if (words[KL_AUDIO_RING_CHANNELS / 4U] != format->channels)
		return EPROTO;
	if (words[KL_AUDIO_RING_RATE / 4U] != format->rate)
		return EPROTO;
	if (words[KL_AUDIO_RING_FRAME_BYTES / 4U] != stream->frame_bytes)
		return EPROTO;
	if (words[KL_AUDIO_RING_CAPACITY / 4U] != stream->ready_capacity)
		return EPROTO;
	if (format->buffer_frames != 0U && stream->ready_capacity != format->buffer_frames)
		return EPROTO;

	/* The stream's measures; nothing is written yet. */
	stream->capacity = stream->ready_capacity;
	stream->rate = format->rate;
	stream->written = audio_ring_load(stream, KL_AUDIO_RING_WRITE_POSITION);

	/* Succeeded: the ring is the stream's. */
	return 0;
}

/*
 * Sends a control and waits for its result.  starts and stops say what a
 * success does to the running the sink keeps.  Returns 0 or an errno value.
 */
static int
audio_control(
	struct kl_audio_stream *stream,
	uint32_t opcode,
	unsigned starts,
	unsigned stops)
{
	uint64_t consumed;
	int error;

	/* One control at a time on the connection. */
	pthread_mutex_lock(&stream->lock);

	/* A lost stream: the sink takes the control. */
	if (stream->lost) {
		if (starts)
			stream->running = 1U;
		if (stops)
			stream->running = 0U;
		audio_sink_anchor(stream, stream->running);
		if (opcode == KL_AUDIO_STREAM_DRAIN)
			stream->events |= KL_AUDIO_EVENT_DRAINED;
		pthread_mutex_unlock(&stream->lock);
		return EPIPE;
	}

	/* The request, and its result awaited. */
	stream->awaited = stream->next_request++;
	stream->answered = 0U;
	(void)wl_proxy_marshal_flags(stream->stream, opcode, NULL, KL_AUDIO_VERSION, 0U, stream->awaited);
	error = audio_wait(stream, audio_answer_done);
	if (error == 0)
		error = audio_errno(stream->answer_error);

	/* The service went meanwhile: the sink takes the control. */
	if (error == EPIPE) {
		if (!stream->lost)
			audio_lose(stream);
		if (starts)
			stream->running = 1U;
		if (stops)
			stream->running = 0U;
		audio_sink_anchor(stream, stream->running);
	}

	/* A success: the running, and after a flush the floor of the position. */
	if (error == 0) {
		if (starts)
			stream->running = 1U;
		if (stops)
			stream->running = 0U;
		if (opcode == KL_AUDIO_STREAM_FLUSH) {
			consumed = audio_ring_load(stream, KL_AUDIO_RING_READ_POSITION);
			__atomic_store_n(&stream->flush_floor, consumed, __ATOMIC_RELEASE);
		}
	}

	pthread_mutex_unlock(&stream->lock);

	/* Refused, or lost. */
	if (error != 0)
		return error;

	/* Succeeded: the control was carried out. */
	return 0;
}

/* Reads and dispatches until done says so, for at most AUDIO_WAIT_MS; the lock is held. Returns 0, ETIMEDOUT or EPIPE. */
static int
audio_wait(
	struct kl_audio_stream *stream,
	int (*done)(const struct kl_audio_stream *stream))
{
	int64_t deadline;
	int64_t left;
	int finished;
	int error;

	/* Until done, the time is up, or the connection ends. */
	deadline = audio_now_ns() + (int64_t)AUDIO_WAIT_MS * 1000000LL;
	for (;;) {
		finished = done(stream);
		if (finished)
			break;

		/* A lost stream has nothing more to wait for. */
		if (stream->lost)
			return EPIPE;

		/* The time left. */
		left = deadline - audio_now_ns();
		if (left <= 0)
			return ETIMEDOUT;

		/* What comes in that time. */
		error = audio_read(stream, (int)(left / 1000000LL) + 1);
		if (error != 0)
			return error;
	}

	/* Succeeded: done. */
	return 0;
}

/* Sends what waits, reads what came within timeout_ms, and dispatches it; the lock is held. Returns 0, or EPIPE when the connection ended. */
static int
audio_read(
	struct kl_audio_stream *stream,
	int timeout_ms)
{
	struct pollfd descriptor;
	int status;

	/* What waits to be sent (a full socket is sent later). */
	status = wl_display_flush(stream->display);
	if (status < 0 && errno != EAGAIN) {
		audio_lose(stream);
		return EPIPE;
	}

	/* Events read before are dispatched first. */
	status = wl_display_prepare_read(stream->display);
	if (status != 0) {
		status = wl_display_dispatch_pending(stream->display);
		if (status < 0) {
			audio_lose(stream);
			return EPIPE;
		}

		/* Succeeded: the events read before are dispatched. */
		return 0;
	}

	/* The socket, for at most the time given. */
	descriptor.fd = wl_display_get_fd(stream->display);
	descriptor.events = POLLIN;
	descriptor.revents = 0;
	status = poll(&descriptor, 1, timeout_ms);
	if (status <= 0) {
		wl_display_cancel_read(stream->display);
		return 0;
	}

	/* Read, then dispatched; an end of the connection is the stream lost. */
	status = wl_display_read_events(stream->display);
	if (status < 0) {
		audio_lose(stream);
		return EPIPE;
	}

	/* The events read, to their callbacks. */
	status = wl_display_dispatch_pending(stream->display);
	if (status < 0) {
		audio_lose(stream);
		return EPIPE;
	}

	/* Succeeded: what came is dispatched. */
	return 0;
}

/* Makes the stream a silent sink from the ring's positions now; the lock is held. */
static void
audio_lose(
	struct kl_audio_stream *stream)
{
	/* Once. */
	if (stream->lost)
		return;

	/* The sink starts where the ring is, running as the stream was. */
	audio_sink_anchor(stream, stream->running);
	stream->events |= KL_AUDIO_EVENT_LOST;

	/*
	 * lost tells every reader to take the sink's positions from now; it is
	 * published after the anchors, which audio_sink_anchor wrote.
	 */
	__atomic_store_n(&stream->lost, 1U, __ATOMIC_RELEASE);
}

/* Sets the sink's anchors to its positions now, with the running from now on; the lock is held. */
static void
audio_sink_anchor(
	struct kl_audio_stream *stream,
	unsigned running)
{
	uint64_t consumed;
	uint64_t played;
	int64_t when;
	uint32_t sequence;
	unsigned lost;

	/* The positions now: the sink's own, or the ring's when the stream is just lost. */
	lost = __atomic_load_n(&stream->lost, __ATOMIC_ACQUIRE);
	if (lost) {
		audio_sink_read(stream, &consumed, &played, &when);
	} else {
		consumed = audio_ring_load(stream, KL_AUDIO_RING_READ_POSITION);
		audio_ring_played(stream, &played, &when);
	}

	/* Written as one: the sequence is odd meanwhile. */
	sequence = stream->sink_sequence;
	__atomic_store_n(&stream->sink_sequence, sequence + 1U, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&stream->sink_consumed, consumed, __ATOMIC_RELAXED);
	__atomic_store_n(&stream->sink_played, played, __ATOMIC_RELAXED);
	__atomic_store_n(&stream->sink_ns, audio_now_ns(), __ATOMIC_RELAXED);
	__atomic_store_n(&stream->sink_running, running, __ATOMIC_RELAXED);
	__atomic_store_n(&stream->sink_sequence, sequence + 2U, __ATOMIC_RELEASE);
}

/* Reads the sink's positions now: the anchors, advanced at the rate while it runs, never past what is written. */
static void
audio_sink_read(
	const struct kl_audio_stream *stream,
	uint64_t *consumed,
	uint64_t *played,
	int64_t *time_ns)
{
	uint64_t base_consumed;
	uint64_t base_played;
	uint64_t advanced;
	uint64_t written;
	uint64_t elapsed;
	int64_t base_ns;
	int64_t now;
	uint32_t before;
	uint32_t after;
	unsigned running;

	/* The anchors, read again until no writer was in the middle. */
	do {
		before = __atomic_load_n(&stream->sink_sequence, __ATOMIC_ACQUIRE);
		base_consumed = __atomic_load_n(&stream->sink_consumed, __ATOMIC_RELAXED);
		base_played = __atomic_load_n(&stream->sink_played, __ATOMIC_RELAXED);
		base_ns = __atomic_load_n(&stream->sink_ns, __ATOMIC_RELAXED);
		running = __atomic_load_n(&stream->sink_running, __ATOMIC_RELAXED);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		after = __atomic_load_n(&stream->sink_sequence, __ATOMIC_RELAXED);
	} while ((before & 1U) != 0U || before != after);

	/* The frames the sink took since, while running (whole seconds apart, so that the product does not overflow). */
	now = audio_now_ns();
	advanced = 0U;
	if (running && now > base_ns) {
		elapsed = (uint64_t)(now - base_ns);
		advanced = elapsed / (uint64_t)AUDIO_NS * stream->rate;
		advanced += elapsed % (uint64_t)AUDIO_NS * stream->rate / (uint64_t)AUDIO_NS;
	}

	/* Never past what is written. */
	written = __atomic_load_n(&stream->written, __ATOMIC_ACQUIRE);
	*consumed = base_consumed + advanced;
	if (*consumed > written)
		*consumed = written;
	*played = base_played + advanced;
	if (*played > written)
		*played = written;
	if (time_ns != NULL)
		*time_ns = now;
}

/* Reads one of the ring's 8-byte positions (acquire). */
static uint64_t
audio_ring_load(
	const struct kl_audio_stream *stream,
	unsigned offset)
{
	uint64_t value;

	/* The position. */
	value = __atomic_load_n((const uint64_t *)(const void *)(stream->ring + offset), __ATOMIC_ACQUIRE);

	/* Succeeded: the position. */
	return value;
}

/* Reads the position heard and its time as one, under the ring's played sequence. */
static void
audio_ring_played(
	const struct kl_audio_stream *stream,
	uint64_t *played,
	int64_t *time_ns)
{
	const uint32_t *sequence;
	const uint64_t *position;
	const int64_t *when;
	uint32_t before;
	uint32_t after;

	/* The pair and its sequence in the ring's page. */
	sequence = (const uint32_t *)(const void *)(stream->ring + KL_AUDIO_RING_PLAYED_SEQUENCE);
	position = (const uint64_t *)(const void *)(stream->ring + KL_AUDIO_RING_PLAYED_POSITION);
	when = (const int64_t *)(const void *)(stream->ring + KL_AUDIO_RING_PLAYED_TIME);

	/* Read again until the server was not in the middle of writing it. */
	do {
		before = __atomic_load_n(sequence, __ATOMIC_ACQUIRE);
		*played = __atomic_load_n(position, __ATOMIC_RELAXED);
		*time_ns = __atomic_load_n(when, __ATOMIC_RELAXED);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		after = __atomic_load_n(sequence, __ATOMIC_RELAXED);
	} while ((before & 1U) != 0U || before != after);
}

/* Gives the monotonic clock in nanoseconds. */
static int64_t
audio_now_ns(
	void)
{
	struct timespec now;

	/* The clock (it does not fail with a valid clock and place). */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: the time. */
	return (int64_t)now.tv_sec * AUDIO_NS + now.tv_nsec;
}

/* Gives the errno value of a wire error. */
static int
audio_errno(
	uint32_t error)
{
	/* Each error of the wire (kl-audio-protocol.h). */
	switch (error) {
	case KL_AUDIO_ERROR_NONE:
		return 0;
	case KL_AUDIO_ERROR_INVALID:
		return EINVAL;
	case KL_AUDIO_ERROR_NO_DEVICE:
		return ENODEV;
	case KL_AUDIO_ERROR_UNAVAILABLE:
		return EAGAIN;
	case KL_AUDIO_ERROR_UNSUPPORTED:
		return ENOTSUP;
	case KL_AUDIO_ERROR_NO_MEMORY:
		return ENOMEM;
	case KL_AUDIO_ERROR_TOO_MANY:
		return EMFILE;
	case KL_AUDIO_ERROR_STATE:
		return EBUSY;
	case KL_AUDIO_ERROR_GONE:
		return EPIPE;
	case KL_AUDIO_ERROR_BROKEN:
		return EPROTO;
	default:
		break;
	}

	/* Succeeded: any other failure. */
	return EIO;
}

/* Tells whether the round trip ended. */
static int
audio_synced_done(
	const struct kl_audio_stream *stream)
{
	/* Succeeded: whether it ended. */
	return (int)stream->synced;
}

/* Tells whether the stream was made or refused. */
static int
audio_ready_done(
	const struct kl_audio_stream *stream)
{
	/* Made. */
	if (stream->ready)
		return 1;

	/* Succeeded: refused, or still awaited. */
	return (int)stream->failed;
}

/* Tells whether the control awaited was answered. */
static int
audio_answer_done(
	const struct kl_audio_stream *stream)
{
	/* Succeeded: whether it was answered. */
	return (int)stream->answered;
}

/* Notes kl_audio_v1 among the globals. */
static void
audio_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct kl_audio_stream *stream;
	int differs;

	/* Only kl_audio_v1. */
	(void)registry;
	(void)version;
	stream = data;
	differs = strcmp(interface, KL_AUDIO_NAME);
	if (differs != 0)
		return;

	/* Its name, to bind. */
	stream->audio_name = name;
}

/* A global that goes: kl_audio_v1 does not go while the compositor runs. */
static void
audio_global_remove(
	void *data,
	struct wl_registry *registry,
	uint32_t name)
{
	/* Nothing to do. */
	(void)data;
	(void)registry;
	(void)name;
}

/* The round trip's end. */
static void
audio_sync_done(
	void *data,
	struct wl_callback *callback,
	uint32_t serial)
{
	struct kl_audio_stream *stream;

	/* Every global before it is announced. */
	(void)serial;
	stream = data;
	stream->synced = 1U;
	wl_callback_destroy(callback);
}

/* ready: the ring, kept until it is mapped. */
static void
audio_on_ready(
	void *data,
	struct wl_proxy *proxy,
	int32_t fd,
	uint32_t bytes,
	uint32_t capacity,
	uint32_t period)
{
	struct kl_audio_stream *stream;

	/* Only the first ring. */
	(void)proxy;
	(void)period;
	stream = data;
	if (stream->ready) {
		(void)close(fd);
		return;
	}

	/* Kept for the open. */
	stream->ready = 1U;
	stream->ready_fd = fd;
	stream->ready_bytes = bytes;
	stream->ready_capacity = capacity;
}

/* failed: the stream was not made. */
static void
audio_on_failed(
	void *data,
	struct wl_proxy *proxy,
	uint32_t error)
{
	struct kl_audio_stream *stream;

	/* Why. */
	(void)proxy;
	stream = data;
	stream->failed = 1U;
	stream->failed_error = error;
}

/* result: the answer to the control awaited (others are passed over). */
static void
audio_on_result(
	void *data,
	struct wl_proxy *proxy,
	uint32_t request,
	uint32_t error)
{
	struct kl_audio_stream *stream;

	/* Only the control awaited. */
	(void)proxy;
	stream = data;
	if (request != stream->awaited)
		return;

	/* Answered. */
	stream->answered = 1U;
	stream->answer_error = error;
}

/* drained: a drain is complete, and the stream stopped. */
static void
audio_on_drained(
	void *data,
	struct wl_proxy *proxy,
	uint32_t request)
{
	struct kl_audio_stream *stream;

	/* Told by the next dispatch. */
	(void)proxy;
	(void)request;
	stream = data;
	stream->running = 0U;
	stream->events |= KL_AUDIO_EVENT_DRAINED;
}

/* underrun: the device found the ring empty. */
static void
audio_on_underrun(
	void *data,
	struct wl_proxy *proxy,
	uint32_t count)
{
	struct kl_audio_stream *stream;

	/* Told by the next dispatch. */
	(void)proxy;
	(void)count;
	stream = data;
	stream->events |= KL_AUDIO_EVENT_UNDERRUN;
}

/* lost: the stream is a silent sink from now. */
static void
audio_on_lost(
	void *data,
	struct wl_proxy *proxy,
	uint32_t error)
{
	struct kl_audio_stream *stream;

	/* The sink (under the lock the dispatching holds). */
	(void)proxy;
	(void)error;
	stream = data;
	audio_lose(stream);
}
