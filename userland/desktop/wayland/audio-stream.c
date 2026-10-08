/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sound's playback streams on the wire (WS191, plan/ws191/design.md):
 * kl_audio_v1 and its kl_audio_stream_v1, shown to the clients of the
 * compositor's own user while the backend makes streams.
 *
 * The compositor only carries a stream's making, its controls and what
 * comes of them between the client and libkeiland-backend; the sound
 * itself goes through the ring the backend makes, which the client gets as
 * the ready event's descriptor and writes into directly.  Nothing here
 * waits: the backend's answers are taken each pass (kwl_audio_tick, from
 * the system extension's tick).  A stream's state moves when the backend
 * answers a control without an error (the state machine of the design's
 * section 3).  The streams a process holds, and all of them, are limited;
 * the process is the one the kernel recorded at the connection.
 */

#include "kwl.h"

#include "userland/desktop/libkeiland/audio/kl-audio-protocol.h"
#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* The ring's layout libkeiland reads is the one the backends write. */
_Static_assert(offsetof(struct kl_backend_audio_ring, version) == KL_AUDIO_RING_VERSION, "version");
_Static_assert(offsetof(struct kl_backend_audio_ring, format) == KL_AUDIO_RING_FORMAT, "format");
_Static_assert(offsetof(struct kl_backend_audio_ring, channels) == KL_AUDIO_RING_CHANNELS, "channels");
_Static_assert(offsetof(struct kl_backend_audio_ring, rate) == KL_AUDIO_RING_RATE, "rate");
_Static_assert(offsetof(struct kl_backend_audio_ring, frame_bytes) == KL_AUDIO_RING_FRAME_BYTES, "frame_bytes");
_Static_assert(offsetof(struct kl_backend_audio_ring, capacity_frames) == KL_AUDIO_RING_CAPACITY, "capacity");
_Static_assert(offsetof(struct kl_backend_audio_ring, period_frames) == KL_AUDIO_RING_PERIOD, "period");
_Static_assert(offsetof(struct kl_backend_audio_ring, write_position) == KL_AUDIO_RING_WRITE_POSITION, "write");
_Static_assert(offsetof(struct kl_backend_audio_ring, read_position) == KL_AUDIO_RING_READ_POSITION, "read");
_Static_assert(offsetof(struct kl_backend_audio_ring, write_sequence) == KL_AUDIO_RING_WRITE_SEQUENCE, "write sequence");
_Static_assert(offsetof(struct kl_backend_audio_ring, read_sequence) == KL_AUDIO_RING_READ_SEQUENCE, "read sequence");
_Static_assert(offsetof(struct kl_backend_audio_ring, played_position) == KL_AUDIO_RING_PLAYED_POSITION, "played");
_Static_assert(offsetof(struct kl_backend_audio_ring, played_time_ns) == KL_AUDIO_RING_PLAYED_TIME, "played time");
_Static_assert(offsetof(struct kl_backend_audio_ring, played_sequence) == KL_AUDIO_RING_PLAYED_SEQUENCE, "played sequence");
_Static_assert(offsetof(struct kl_backend_audio_ring, underruns) == KL_AUDIO_RING_UNDERRUNS, "underruns");
_Static_assert(offsetof(struct kl_backend_audio_ring, overruns) == KL_AUDIO_RING_OVERRUNS, "overruns");
_Static_assert(offsetof(struct kl_backend_audio_ring, state) == KL_AUDIO_RING_STATE, "state");
_Static_assert(KL_BACKEND_AUDIO_RING_HEADER == KL_AUDIO_RING_HEADER, "the ring's page");
_Static_assert(KL_BACKEND_AUDIO_RING_VERSION == KL_AUDIO_RING_VERSION_VALUE, "the ring's version");

/* The backend's formats and errors are the wire's values. */
_Static_assert(KL_BACKEND_AUDIO_FORMAT_S16_LE == KL_AUDIO_WIRE_FORMAT_S16_LE, "S16_LE");
_Static_assert(KL_BACKEND_AUDIO_FORMAT_S32_LE == KL_AUDIO_WIRE_FORMAT_S32_LE, "S32_LE");
_Static_assert(KL_BACKEND_AUDIO_FORMAT_F32_LE == KL_AUDIO_WIRE_FORMAT_F32_LE, "F32_LE");
_Static_assert(KL_BACKEND_AUDIO_ERROR_NONE == KL_AUDIO_ERROR_NONE, "NONE");
_Static_assert(KL_BACKEND_AUDIO_ERROR_INVALID == KL_AUDIO_ERROR_INVALID, "INVALID");
_Static_assert(KL_BACKEND_AUDIO_ERROR_NO_DEVICE == KL_AUDIO_ERROR_NO_DEVICE, "NO_DEVICE");
_Static_assert(KL_BACKEND_AUDIO_ERROR_UNAVAILABLE == KL_AUDIO_ERROR_UNAVAILABLE, "UNAVAILABLE");
_Static_assert(KL_BACKEND_AUDIO_ERROR_UNSUPPORTED == KL_AUDIO_ERROR_UNSUPPORTED, "UNSUPPORTED");
_Static_assert(KL_BACKEND_AUDIO_ERROR_NO_MEMORY == KL_AUDIO_ERROR_NO_MEMORY, "NO_MEMORY");
_Static_assert(KL_BACKEND_AUDIO_ERROR_TOO_MANY == KL_AUDIO_ERROR_TOO_MANY, "TOO_MANY");
_Static_assert(KL_BACKEND_AUDIO_ERROR_STATE == KL_AUDIO_ERROR_STATE, "STATE");
_Static_assert(KL_BACKEND_AUDIO_ERROR_GONE == KL_AUDIO_ERROR_GONE, "GONE");
_Static_assert(KL_BACKEND_AUDIO_ERROR_BROKEN == KL_AUDIO_ERROR_BROKEN, "BROKEN");
_Static_assert(KL_BACKEND_AUDIO_ERROR_FAILED == KL_AUDIO_ERROR_FAILED, "FAILED");

/*
 * The table's slots: twice the streams that may hold a backend's stream,
 * since a failed or lost stream keeps its slot until its client destroys
 * it (it is not counted against the limits).
 */
#define AUDIO_SLOTS		(2U * KL_AUDIO_STREAMS_MAX)

/* The controls a stream may have with the backend at once: one, so that the backend and the compositor see one state. */
#define AUDIO_WAITING		1U

/* Marks a parameter a function does not use. */
#define UNUSED_PARAMETER(name)	((void)(name))

/* Where a stream is (the design's section 3). */
enum audio_state {
	AUDIO_PENDING,		/* made; the backend's ready or failed awaited */
	AUDIO_STOPPED,
	AUDIO_RUNNING,
	AUDIO_DRAINING,
	AUDIO_FAILED,		/* failed was sent; nothing more */
	AUDIO_LOST		/* lost was sent; nothing more */
};

/* A control given to the backend and not answered yet: its request and which control. used 0 is a free slot. */
struct audio_waiting {
	unsigned used;
	uint32_t request;
	unsigned what;
};

/*
 * One stream: its object (NULL for a free slot), the process holding it,
 * where it is, the backend's stream (NULL once failed or lost), the
 * controls waiting, and whether an underrun may be told (once after each
 * start or other control, so that a stream left running at the end of a
 * song does not fill a client that does not read).  The slot is the object's from its create_stream until its
 * destroy or its client's end (kwl_audio_object_gone).
 */
struct audio_stream {
	struct kwl_object *object;
	pid_t pid;
	enum audio_state state;
	struct kl_backend_audio_stream *backend;
	struct audio_waiting waiting[AUDIO_WAITING];
	unsigned underrun_armed;
};

/*
 * The streams, and whether the backend makes streams (asked once, at the
 * first look; offered decides whether kl_audio_v1 is shown).  Only the
 * compositor's thread touches them.
 */
static struct {
	struct audio_stream streams[AUDIO_SLOTS];
	unsigned asked;
	unsigned offered;
} audio_state;

static int audio_create(struct kwl_object *manager, const unsigned char *bytes, size_t size);
static int audio_control(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
static struct audio_stream *audio_find(const struct kwl_object *object);
static unsigned audio_check(uint32_t format, uint32_t channels, uint32_t rate, uint32_t buffer, uint32_t period);
static unsigned audio_count(pid_t pid, unsigned *all);
static void audio_take(struct audio_stream *stream, const struct kl_backend_audio_stream_report *report);
static void audio_answered(struct audio_stream *stream, uint32_t request, unsigned error);
static void audio_end(struct audio_stream *stream, enum audio_state state, uint32_t opcode, unsigned error);
static void audio_close_backend(struct audio_stream *stream);
static void audio_event(struct audio_stream *stream, uint32_t opcode, const uint32_t *words, size_t count);
static void audio_drop_client(struct audio_stream *stream, int error);
static const char *audio_control_name(unsigned what);
static uint32_t audio_word(const unsigned char *bytes, size_t offset);

/*
 * Tells whether kl_audio_v1 is shown: only while the backend makes streams
 * (the user's own clients only; settings.c decides that).
 */
int
kwl_audio_offered(
	void)
{
	int supported;

	/* Asked once. */
	if (!audio_state.asked) {
		audio_state.asked = 1U;
		supported = kl_backend_audio_stream_supported();
		audio_state.offered = 0U;
		if (supported)
			audio_state.offered = 1U;
		printf("KWL AUDIO streams offered=%u\n", audio_state.offered);
	}

	/* Succeeded: whether streams are offered. */
	return (int)audio_state.offered;
}

/*
 * Carries out a request of kl_audio_v1 or of a stream.  Returns 0, or
 * EPROTO for a malformed one.
 */
int
kwl_audio_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	int error;

	/* kl_audio_v1: its end (the streams stay), or a stream made. */
	if (object->kind == KWL_AUDIO) {
		if (opcode == KL_AUDIO_DESTROY) {
			if (size != 0U)
				return EPROTO;
			kwl_object_destroy(object);
			return 0;
		}

		/* Nothing else but create_stream. */
		if (opcode != KL_AUDIO_CREATE_STREAM)
			return EPROTO;
		error = audio_create(object, bytes, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* A stream's end (its backend's stream closes with it, kwl_audio_object_gone). */
	if (opcode == KL_AUDIO_STREAM_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(object);
		return 0;
	}

	/* A control. */
	error = audio_control(object, opcode, bytes, size);
	if (error != 0)
		return error;

	/* Succeeded: carried out. */
	return 0;
}

/*
 * Takes what came of every stream from the backend, tells the clients, and
 * lets the ended pump threads go (each pass of the event loop).
 */
void
kwl_audio_tick(
	struct kwl_server *server)
{
	struct kl_backend_audio_stream_report report;
	struct audio_stream *stream;
	unsigned index;
	int taken;

	UNUSED_PARAMETER(server);

	/* Each stream with a backend's stream, until nothing more came. */
	for (index = 0U; index < AUDIO_SLOTS; index++) {
		stream = &audio_state.streams[index];
		for (;;) {
			if (stream->object == NULL || stream->backend == NULL)
				break;
			taken = kl_backend_audio_stream_next(stream->backend, &report);
			if (!taken)
				break;
			audio_take(stream, &report);
		}
	}

	/* The pump threads of closed streams. */
	kl_backend_audio_stream_reap();
}

/*
 * Closes a stream's backend side when its object goes (its destroy, or its
 * client's end; objects.c calls it for every stream object).
 */
void
kwl_audio_object_gone(
	struct kwl_object *object)
{
	struct audio_stream *stream;

	/* The stream of the object. */
	stream = audio_find(object);
	if (stream == NULL)
		return;

	/* Its backend's stream, then the slot. */
	audio_close_backend(stream);
	printf("KWL AUDIO stream client=%llu id=%u closed\n", (unsigned long long)object->client->number, object->id);
	memset(stream, 0, sizeof(*stream));
}

/*
 * Waits for every pump thread at the compositor's end, after the clients
 * (and with them every stream) went.
 */
void
kwl_audio_close(
	struct kwl_server *server)
{
	UNUSED_PARAMETER(server);

	/* Nothing was offered, so nothing ran. */
	if (!audio_state.offered)
		return;

	/* Every pump ends and is let go before the process exits. */
	kl_backend_audio_stream_reap_all();
}

/* Makes a stream for create_stream(new_id, format, channels, rate, buffer_frames, period_frames). Returns 0 or EPROTO. */
static int
audio_create(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct kl_backend_audio_stream_format format;
	struct kwl_object *created;
	struct audio_stream *stream;
	uint32_t error_word;
	unsigned index;
	unsigned held;
	unsigned all;
	unsigned error;
	uint32_t id;
	pid_t pid;
	int peer_error;

	/* The request's words. */
	if (size != 24U)
		return EPROTO;
	id = audio_word(bytes, 0U);
	memset(&format, 0, sizeof(format));
	format.format = audio_word(bytes, 4U);
	format.channels = audio_word(bytes, 8U);
	format.rate = audio_word(bytes, 12U);
	format.buffer_frames = audio_word(bytes, 16U);
	format.period_frames = audio_word(bytes, 20U);

	/* The object, under the ID the client chose. */
	created = kwl_create(manager->client, id, KWL_AUDIO_STREAM, manager->version);
	if (created == NULL)
		return EPROTO;

	/* The process at the other end of the connection (0 when it cannot be told: one group for all such). */
	pid = 0;
	peer_error = kl_backend_peer_pid(manager->client->fd, &pid);
	if (peer_error != 0)
		pid = 0;
	printf("KWL AUDIO stream client=%llu id=%u pid=%ld create format=%u channels=%u rate=%u buffer=%u period=%u\n",
	    (unsigned long long)manager->client->number,
	    id,
	    (long)pid,
	    format.format,
	    format.channels,
	    format.rate,
	    format.buffer_frames,
	    format.period_frames);

	/* A free slot; the limits make sure there is one while the total is under it. */
	stream = NULL;
	for (index = 0U; index < AUDIO_SLOTS && stream == NULL; index++) {
		if (audio_state.streams[index].object == NULL)
			stream = &audio_state.streams[index];
	}

	/* No slot at all: the stream is refused at once (its object is not one of the table's). */
	if (stream == NULL) {
		error_word = KL_AUDIO_ERROR_TOO_MANY;
		(void)kwl_emit(created->client, created->id, KL_AUDIO_STREAM_EVENT_FAILED, &error_word, sizeof(error_word));
		printf("KWL AUDIO stream client=%llu id=%u failed error=%u\n", (unsigned long long)created->client->number, id, error_word);
		return 0;
	}

	/* The slot is the object's from now. */
	memset(stream, 0, sizeof(*stream));
	stream->object = created;
	stream->pid = pid;
	stream->state = AUDIO_PENDING;

	/* What is asked for, checked. */
	error = audio_check(format.format, format.channels, format.rate, format.buffer_frames, format.period_frames);
	if (error != KL_AUDIO_ERROR_NONE) {
		audio_end(stream, AUDIO_FAILED, KL_AUDIO_STREAM_EVENT_FAILED, error);
		return 0;
	}

	/* The process's streams and all of them (this one not counted yet: it has no backend's stream). */
	held = audio_count(pid, &all);
	if (held >= KL_AUDIO_STREAMS_PER_PROCESS || all >= KL_AUDIO_STREAMS_MAX) {
		audio_end(stream, AUDIO_FAILED, KL_AUDIO_STREAM_EVENT_FAILED, KL_AUDIO_ERROR_TOO_MANY);
		return 0;
	}

	/* The backend's stream; its ready or failed comes at a later tick. */
	stream->backend = kl_backend_audio_stream_open(&format);
	if (stream->backend == NULL) {
		error = KL_AUDIO_ERROR_NO_MEMORY;
		if (errno == ENOTSUP)
			error = KL_AUDIO_ERROR_UNSUPPORTED;
		audio_end(stream, AUDIO_FAILED, KL_AUDIO_STREAM_EVENT_FAILED, error);
		return 0;
	}

	/* Succeeded: the stream waits for the backend. */
	return 0;
}

/* Carries out start, stop, flush or drain(request). Returns 0 or EPROTO. */
static int
audio_control(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct audio_stream *stream;
	uint32_t words[2];
	uint32_t request;
	unsigned what;
	unsigned slot;
	int error;

	/* Which control. */
	switch (opcode) {
	case KL_AUDIO_STREAM_START:
		what = KL_BACKEND_AUDIO_START;
		break;
	case KL_AUDIO_STREAM_STOP:
		what = KL_BACKEND_AUDIO_STOP;
		break;
	case KL_AUDIO_STREAM_FLUSH:
		what = KL_BACKEND_AUDIO_FLUSH;
		break;
	case KL_AUDIO_STREAM_DRAIN:
		what = KL_BACKEND_AUDIO_DRAIN;
		break;
	default:
		return EPROTO;
	}

	/* The request's number. */
	if (size != 4U)
		return EPROTO;
	request = audio_word(bytes, 0U);

	/* The object's stream; one the table had no slot for has failed, and is answered directly. */
	stream = audio_find(object);
	if (stream == NULL) {
		words[0] = request;
		words[1] = KL_AUDIO_ERROR_STATE;
		(void)kwl_emit(object->client, object->id, KL_AUDIO_STREAM_EVENT_RESULT, words, sizeof(words));
		return 0;
	}

	/* Before ready, or after failed or lost, nothing is controlled. */
	if (stream->state == AUDIO_LOST) {
		audio_answered(stream, request, KL_AUDIO_ERROR_GONE);
		return 0;
	}

	/* Not ready yet, or failed. */
	if (stream->state == AUDIO_PENDING || stream->state == AUDIO_FAILED || stream->backend == NULL) {
		audio_answered(stream, request, KL_AUDIO_ERROR_STATE);
		return 0;
	}

	/* A free slot for the answer; none is a client asking faster than the backend answers. */
	for (slot = 0U; slot < AUDIO_WAITING; slot++) {
		if (!stream->waiting[slot].used)
			break;
	}

	/* A control not answered yet: one at a time. */
	if (slot == AUDIO_WAITING) {
		audio_answered(stream, request, KL_AUDIO_ERROR_STATE);
		return 0;
	}

	/* The control to the backend; its answer comes at a later tick. */
	error = kl_backend_audio_stream_control(stream->backend, what, request);
	if (error == EAGAIN) {
		audio_answered(stream, request, KL_AUDIO_ERROR_UNAVAILABLE);
		return 0;
	}

	/* The backend's stream ended (its LOST comes at a later tick). */
	if (error == ENOTCONN) {
		audio_answered(stream, request, KL_AUDIO_ERROR_GONE);
		return 0;
	}

	/* Any other refusal. */
	if (error != 0) {
		audio_answered(stream, request, KL_AUDIO_ERROR_FAILED);
		return 0;
	}

	/* Succeeded: the answer is awaited. */
	stream->waiting[slot].used = 1U;
	stream->waiting[slot].request = request;
	stream->waiting[slot].what = what;
	return 0;
}

/* Finds an object's stream, or NULL. */
static struct audio_stream *
audio_find(
	const struct kwl_object *object)
{
	unsigned index;

	/* The slot that names the object. */
	for (index = 0U; index < AUDIO_SLOTS; index++) {
		if (audio_state.streams[index].object == object)
			return &audio_state.streams[index];
	}

	/* None. */
	return NULL;
}

/* Checks what a stream asks for; gives KL_AUDIO_ERROR_NONE or _INVALID. */
static unsigned
audio_check(
	uint32_t format,
	uint32_t channels,
	uint32_t rate,
	uint32_t buffer,
	uint32_t period)
{
	uint64_t bytes;
	uint32_t sample_bytes;

	/* A format, channels and a rate the streams carry. */
	if (format < KL_AUDIO_WIRE_FORMAT_S16_LE || format > KL_AUDIO_WIRE_FORMAT_F32_LE)
		return KL_AUDIO_ERROR_INVALID;
	if (channels < 1U || channels > KL_AUDIO_CHANNELS_MAX)
		return KL_AUDIO_ERROR_INVALID;
	if (rate < KL_AUDIO_RATE_MIN || rate > KL_AUDIO_RATE_MAX)
		return KL_AUDIO_ERROR_INVALID;

	/* The backend's choice of the ring. */
	if (buffer == 0U)
		return KL_AUDIO_ERROR_NONE;

	/* A ring of at least 10 ms, and of two periods when a period is asked for. */
	if (buffer < rate / 100U)
		return KL_AUDIO_ERROR_INVALID;
	if (period != 0U && buffer / 2U < period)
		return KL_AUDIO_ERROR_INVALID;

	/* At most a mebibyte of frames. */
	sample_bytes = 4U;
	if (format == KL_AUDIO_WIRE_FORMAT_S16_LE)
		sample_bytes = 2U;
	bytes = (uint64_t)buffer * sample_bytes * channels;
	if (bytes > KL_AUDIO_RING_BYTES_MAX)
		return KL_AUDIO_ERROR_INVALID;

	/* Succeeded: the stream may be made. */
	return KL_AUDIO_ERROR_NONE;
}

/* Counts the streams that hold a backend's stream: a process's (returned) and all of them (*all). */
static unsigned
audio_count(
	pid_t pid,
	unsigned *all)
{
	struct audio_stream *stream;
	unsigned index;
	unsigned held;

	/* Each stream that is not failed or lost (their backends are closed). */
	held = 0U;
	*all = 0U;
	for (index = 0U; index < AUDIO_SLOTS; index++) {
		stream = &audio_state.streams[index];
		if (stream->object == NULL || stream->backend == NULL)
			continue;
		(*all)++;
		if (stream->pid == pid)
			held++;
	}

	/* Succeeded: the process's count. */
	return held;
}

/* Takes one thing that came of a stream's backend and tells the client, as the state machine says. */
static void
audio_take(
	struct audio_stream *stream,
	const struct kl_backend_audio_stream_report *report)
{
	struct kwl_client *client;
	uint32_t words[3];
	int error;

	/* Which thing. */
	client = stream->object->client;
	switch (report->what) {
	case KL_BACKEND_AUDIO_READY:
		/* The ring, once, to a stream that waits for it (a stray one's descriptor is closed). */
		if (stream->state != AUDIO_PENDING) {
			(void)close(report->fd);
			break;
		}

		/* The ring's size and frames, with its descriptor (sent, or closed on failure). */
		words[0] = report->bytes;
		words[1] = report->capacity_frames;
		words[2] = report->period_frames;
		error = kwl_emit_fd(client, stream->object->id, KL_AUDIO_STREAM_EVENT_READY, words, sizeof(words), report->fd);
		if (error != 0) {
			audio_drop_client(stream, error);
			break;
		}

		/* Ready: stopped until started. */
		stream->state = AUDIO_STOPPED;
		printf("KWL AUDIO stream client=%llu id=%u ready capacity=%u bytes=%u\n", (unsigned long long)client->number, stream->object->id, report->capacity_frames, report->bytes);
		break;
	case KL_BACKEND_AUDIO_FAILED:
		/* The stream could not be made. */
		audio_end(stream, AUDIO_FAILED, KL_AUDIO_STREAM_EVENT_FAILED, report->error);
		break;
	case KL_BACKEND_AUDIO_RESULT:
		/* A control's answer. */
		audio_answered(stream, report->request, report->error);
		break;
	case KL_BACKEND_AUDIO_DRAINED:
		/* The drain of a draining stream is complete; any other is stale. */
		if (stream->state != AUDIO_DRAINING)
			break;
		stream->state = AUDIO_STOPPED;
		words[0] = report->request;
		audio_event(stream, KL_AUDIO_STREAM_EVENT_DRAINED, words, 1U);
		printf("KWL AUDIO stream client=%llu id=%u drained request=%u\n", (unsigned long long)client->number, stream->object->id, report->request);
		break;
	case KL_BACKEND_AUDIO_UNDERRUN:
		/* Told once after each start or other control, while running. */
		if (stream->state != AUDIO_RUNNING || !stream->underrun_armed)
			break;
		stream->underrun_armed = 0U;
		words[0] = report->count;
		audio_event(stream, KL_AUDIO_STREAM_EVENT_UNDERRUN, words, 1U);
		printf("KWL AUDIO stream client=%llu id=%u underrun count=%u\n", (unsigned long long)client->number, stream->object->id, report->count);
		break;
	case KL_BACKEND_AUDIO_LOST:
		/* The stream is gone. */
		audio_end(stream, AUDIO_LOST, KL_AUDIO_STREAM_EVENT_LOST, report->error);
		break;
	default:
		break;
	}
}

/* Sends a control's result and moves the stream when it succeeded. */
static void
audio_answered(
	struct audio_stream *stream,
	uint32_t request,
	unsigned error)
{
	struct audio_waiting *waiting;
	uint32_t words[2];
	unsigned slot;

	/* The control the request names, and its slot free again. */
	waiting = NULL;
	for (slot = 0U; slot < AUDIO_WAITING && waiting == NULL; slot++) {
		if (stream->waiting[slot].used && stream->waiting[slot].request == request)
			waiting = &stream->waiting[slot];
	}

	/* Its slot is free again. */
	if (waiting != NULL)
		waiting->used = 0U;

	/* A control that succeeded moves the stream (the design's section 3). */
	if (waiting != NULL && error == KL_AUDIO_ERROR_NONE) {
		switch (waiting->what) {
		case KL_BACKEND_AUDIO_START:
			stream->state = AUDIO_RUNNING;
			break;
		case KL_BACKEND_AUDIO_STOP:
			stream->state = AUDIO_STOPPED;
			break;
		case KL_BACKEND_AUDIO_FLUSH:
			/* A flush keeps a running or stopped stream as it is (a drain's is below). */
			break;
		case KL_BACKEND_AUDIO_DRAIN:
			stream->state = AUDIO_DRAINING;
			break;
		default:
			break;
		}
	}

	/* A drain's flush ends the drain however it was answered: audiod stops it before it flushes. */
	if (waiting != NULL && waiting->what == KL_BACKEND_AUDIO_FLUSH && stream->state == AUDIO_DRAINING)
		stream->state = AUDIO_STOPPED;

	/* An underrun may be told once again after a control. */
	if (waiting != NULL)
		stream->underrun_armed = 1U;

	/* The result. */
	words[0] = request;
	words[1] = error;
	audio_event(stream, KL_AUDIO_STREAM_EVENT_RESULT, words, 2U);
	if (waiting != NULL) {
		printf("KWL AUDIO stream client=%llu id=%u %s request=%u error=%u\n",
		    (unsigned long long)stream->object->client->number,
		    stream->object->id,
		    audio_control_name(waiting->what),
		    request,
		    error);
	}
}

/* Ends a stream with failed or lost: its backend closed, the event sent, nothing more after it. */
static void
audio_end(
	struct audio_stream *stream,
	enum audio_state state,
	uint32_t opcode,
	unsigned error)
{
	uint32_t word;
	const char *what;
	unsigned slot;

	/* A control not answered yet is answered first: its stream is gone (one result for each control). */
	for (slot = 0U; slot < AUDIO_WAITING; slot++) {
		if (stream->waiting[slot].used)
			audio_answered(stream, stream->waiting[slot].request, KL_AUDIO_ERROR_GONE);
	}

	/* The backend's stream, and where the stream is now. */
	audio_close_backend(stream);
	stream->state = state;

	/* The event. */
	word = error;
	audio_event(stream, opcode, &word, 1U);
	what = "failed";
	if (state == AUDIO_LOST)
		what = "lost";
	printf("KWL AUDIO stream client=%llu id=%u %s error=%u\n", (unsigned long long)stream->object->client->number, stream->object->id, what, error);
}

/* Closes a stream's backend side, once. */
static void
audio_close_backend(
	struct audio_stream *stream)
{
	/* Already closed. */
	if (stream->backend == NULL)
		return;

	/* The backend's stream (a ring it handed over and nobody took is closed with it). */
	kl_backend_audio_stream_close(stream->backend);
	stream->backend = NULL;
}

/* Sends a stream an event of words; one that cannot be sent ends the client's connection. */
static void
audio_event(
	struct audio_stream *stream,
	uint32_t opcode,
	const uint32_t *words,
	size_t count)
{
	struct kwl_client *client;
	int error;

	/* A connection being retired hears nothing more. */
	client = stream->object->client;
	if (client->fatal)
		return;

	/* The event. */
	error = kwl_emit(client, stream->object->id, opcode, words, count * sizeof(words[0]));
	if (error != 0)
		audio_drop_client(stream, error);
}

/*
 * Ends a stream whose client does not take its events: its backend closes
 * and the connection goes (a stream has its own connection, so no window
 * goes with it); libkeiland takes the end as the stream lost.
 */
static void
audio_drop_client(
	struct audio_stream *stream,
	int error)
{
	struct kwl_client *client;

	/* The backend's stream; no event can reach the client any more. */
	audio_close_backend(stream);
	stream->state = AUDIO_LOST;

	/* The connection, retired on the next pass. */
	client = stream->object->client;
	printf("KWL AUDIO stream client=%llu id=%u dropped errno=%d\n", (unsigned long long)client->number, stream->object->id, error);
	if (!client->fatal) {
		client->fatal = 1;
		client->fatal_time = kwl_milliseconds();
	}
}

/* Names a control for the log. */
static const char *
audio_control_name(
	unsigned what)
{
	/* Which control. */
	switch (what) {
	case KL_BACKEND_AUDIO_START:
		return "start";
	case KL_BACKEND_AUDIO_STOP:
		return "stop";
	case KL_BACKEND_AUDIO_FLUSH:
		return "flush";
	case KL_BACKEND_AUDIO_DRAIN:
		return "drain";
	default:
		break;
	}

	/* Succeeded: not a control. */
	return "control";
}

/* Reads a native-endian word of a request. */
static uint32_t
audio_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The word. */
	memcpy(&word, bytes + offset, sizeof(word));
	return word;
}
