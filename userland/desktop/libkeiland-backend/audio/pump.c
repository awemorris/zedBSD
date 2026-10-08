/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sound's playback streams on Linux and FreeBSD (keiland-backend.h,
 * WS191 p004, plan/ws191/design.md section 6.4): a stream's ring is a
 * memfd sealed against shrinking and growing (a client that shrank it
 * would fault the compositor), and a pump thread of its own carries the
 * frames from the ring to the system's sound device (pump.h).
 *
 * The compositor's thread and the pump meet only under the stream's lock:
 * the controls go to the pump as commands (with a byte on the wake pipe),
 * and what comes of them comes back as reports that
 * kl_backend_audio_stream_next takes.  The read and played positions and
 * the ring's state are written by the pump alone; from the shared memory
 * it reads only the client's write position, and checks it.  A closed
 * stream's pump is told to end and is joined later, when it has (reap), so
 * that no call of the compositor's thread waits for a device.
 */

#include "pump.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* The ring's positions are 8-byte atomics; a system without lock-free ones is not one the streams run on. */
_Static_assert(__GCC_ATOMIC_LLONG_LOCK_FREE == 2, "8-byte atomics must be lock-free");

/* The commands and the reports a stream holds at once. */
#define PUMP_COMMANDS		8U
#define PUMP_REPORTS		32U

/* The frames the pump carries at once at most, and the most bytes a frame of the ring has (two channels of 4 bytes). */
#define PUMP_CHUNK		1024U
#define PUMP_FRAME_MAX		8U

/* How long a running pump waits for the device before it looks at the ring again (ms). */
#define PUMP_WAIT_MS		10

/* The descriptors of a device the pump waits on at most. */
#define PUMP_DESCRIPTORS	8

/* The ring's tag ("KAUD"), the server's own. */
#define PUMP_TAG		0x4455414bU

/* A control the compositor's thread gave the pump. */
struct pump_command {
	unsigned what;
	uint32_t request;
};

/*
 * One stream: the device's calls and the format asked for; the thread;
 * under lock, the commands, the reports, and whether the stream was closed,
 * ended (FAILED or LOST was reported) or its pump has finished; the wake
 * pipe; the ring.  The rest is the pump's own: the device, the format it
 * takes, its period, and where the stream is.
 *
 * The compositor owns the record until kl_backend_audio_stream_close; then
 * the closed list does, until the pump finished and reap frees it.
 */
struct kl_backend_audio_stream {
	const struct pump_device_ops *ops;
	struct kl_backend_audio_stream_format format;
	pthread_t thread;
	unsigned thread_started;

	pthread_mutex_t lock;
	struct pump_command commands[PUMP_COMMANDS];
	unsigned command_count;
	struct kl_backend_audio_stream_report reports[PUMP_REPORTS];
	unsigned report_first;
	unsigned report_count;
	unsigned closing;
	unsigned ended;
	unsigned finished;

	int wake[2];
	int ring_fd;
	struct kl_backend_audio_ring *ring;
	size_t ring_bytes;
	unsigned capacity;
	unsigned frame_bytes;
	struct kl_backend_audio_stream *next_closed;

	struct pump_device *device;
	unsigned device_format;
	unsigned device_frame_bytes;
	unsigned period;
	unsigned running;
	unsigned draining;
	unsigned drain_started;
	uint32_t drain_request;
	uint64_t read;
	uint64_t played;
	uint64_t silence_tail;
	unsigned empty_told;
	uint32_t underruns;
	unsigned char chunk[PUMP_CHUNK * PUMP_FRAME_MAX];
};

/*
 * The closed streams whose pumps have not been joined yet, linked by
 * next_closed; pump_closed_lock guards the list.  Streams join it at their
 * close and leave it at reap.
 */
static pthread_mutex_t pump_closed_lock = PTHREAD_MUTEX_INITIALIZER;
static struct kl_backend_audio_stream *pump_closed;

static int pump_ring(struct kl_backend_audio_stream *stream);
static void *pump_run(void *argument);
static unsigned pump_take(struct kl_backend_audio_stream *stream, struct pump_command *commands);
static int pump_command(struct kl_backend_audio_stream *stream, const struct pump_command *command);
static int pump_step(struct kl_backend_audio_stream *stream);
static int pump_carry(struct kl_backend_audio_stream *stream, uint64_t frames);
static void pump_convert(struct kl_backend_audio_stream *stream, const unsigned char *source, unsigned frames, unsigned char *target);
static void pump_played(struct kl_backend_audio_stream *stream, int exact);
static void pump_wait(struct kl_backend_audio_stream *stream);
static void pump_wake(struct kl_backend_audio_stream *stream);
static void pump_report(struct kl_backend_audio_stream *stream, unsigned what, unsigned error, uint32_t request, uint32_t count);
static void pump_end(struct kl_backend_audio_stream *stream, unsigned what, unsigned error);
static void pump_free(struct kl_backend_audio_stream *stream);
static int64_t pump_now_ns(void);

/*
 * Makes a stream on a device: the ring, and the pump's thread.
 */
struct kl_backend_audio_stream *
kl_backend_audio_pump_open(
	const struct pump_device_ops *ops,
	const struct kl_backend_audio_stream_format *format)
{
	struct kl_backend_audio_stream *stream;
	sigset_t all;
	sigset_t previous;
	int error;

	/* The record. */
	stream = calloc(1, sizeof(*stream));
	if (stream == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	/* What it is asked to be; nothing open yet. */
	stream->ops = ops;
	stream->format = *format;
	stream->wake[0] = -1;
	stream->wake[1] = -1;
	stream->ring_fd = -1;
	error = pthread_mutex_init(&stream->lock, NULL);
	if (error != 0) {
		free(stream);
		errno = error;
		return NULL;
	}

	/* The wake pipe, and the ring. */
	error = pipe2(stream->wake, O_CLOEXEC | O_NONBLOCK);
	if (error != 0)
		error = errno;
	if (error == 0)
		error = pump_ring(stream);
	if (error != 0) {
		pump_free(stream);
		errno = error;
		return NULL;
	}

	/*
	 * The pump's thread, with every signal blocked: the compositor's own
	 * thread takes them, and the threads a sound library starts from the
	 * pump inherit the mask.
	 */
	(void)sigfillset(&all);
	(void)pthread_sigmask(SIG_SETMASK, &all, &previous);
	error = pthread_create(&stream->thread, NULL, pump_run, stream);
	(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
	if (error != 0) {
		pump_free(stream);
		errno = error;
		return NULL;
	}

	/* Succeeded: READY or FAILED comes from the pump. */
	stream->thread_started = 1U;
	return stream;
}

/*
 * Gives the pump a control; its RESULT comes as a report.
 */
int
kl_backend_audio_stream_control(
	struct kl_backend_audio_stream *stream,
	unsigned what,
	uint32_t request)
{
	int error;

	/* Queued, unless the stream ended or the queue is full. */
	pthread_mutex_lock(&stream->lock);

	error = 0;
	if (stream->ended) {
		error = ENOTCONN;
	} else if (stream->command_count == PUMP_COMMANDS) {
		error = EAGAIN;
	} else {
		stream->commands[stream->command_count].what = what;
		stream->commands[stream->command_count].request = request;
		stream->command_count++;
	}

	pthread_mutex_unlock(&stream->lock);

	/* Not queued. */
	if (error != 0)
		return error;

	/* The pump is woken for it. */
	pump_wake(stream);

	/* Succeeded: the pump carries it out. */
	return 0;
}

/*
 * Takes the next thing that came of the stream without waiting.
 */
int
kl_backend_audio_stream_next(
	struct kl_backend_audio_stream *stream,
	struct kl_backend_audio_stream_report *report)
{
	int taken;

	/* The oldest report, when there is one. */
	pthread_mutex_lock(&stream->lock);

	taken = 0;
	if (stream->report_count != 0U) {
		*report = stream->reports[stream->report_first];
		stream->report_first = (stream->report_first + 1U) % PUMP_REPORTS;
		stream->report_count--;
		taken = 1;
	}

	pthread_mutex_unlock(&stream->lock);

	/* Succeeded: whether a report was taken. */
	return taken;
}

/*
 * Ends a stream without waiting: its pump is told to end, and the stream
 * waits on the closed list until reap joins it.
 */
void
kl_backend_audio_stream_close(
	struct kl_backend_audio_stream *stream)
{
	/* Nothing to close. */
	if (stream == NULL)
		return;

	/* The pump ends at its next look. */
	pthread_mutex_lock(&stream->lock);

	stream->closing = 1U;

	pthread_mutex_unlock(&stream->lock);

	/* Woken for it. */
	pump_wake(stream);

	/* On the closed list until its pump finished. */
	pthread_mutex_lock(&pump_closed_lock);

	stream->next_closed = pump_closed;
	pump_closed = stream;

	pthread_mutex_unlock(&pump_closed_lock);
}

/*
 * Joins and frees every closed stream whose pump has finished.
 */
void
kl_backend_audio_stream_reap(
	void)
{
	struct kl_backend_audio_stream **link;
	struct kl_backend_audio_stream *stream;
	unsigned finished;

	/* Each closed stream; a pump still running stays for a later pass. */
	pthread_mutex_lock(&pump_closed_lock);

	link = &pump_closed;
	while (*link != NULL) {
		stream = *link;
		pthread_mutex_lock(&stream->lock);
		finished = stream->finished;
		pthread_mutex_unlock(&stream->lock);
		if (!finished) {
			link = &stream->next_closed;
			continue;
		}

		/* Finished: off the list, joined and freed. */
		*link = stream->next_closed;
		pump_free(stream);
	}

	pthread_mutex_unlock(&pump_closed_lock);
}

/*
 * Waits for every closed stream's pump to finish, and frees them.
 */
void
kl_backend_audio_stream_reap_all(
	void)
{
	struct kl_backend_audio_stream *stream;

	/* Each closed stream, its pump joined (it was told to end). */
	pthread_mutex_lock(&pump_closed_lock);

	while (pump_closed != NULL) {
		stream = pump_closed;
		pump_closed = stream->next_closed;
		pump_free(stream);
	}

	pthread_mutex_unlock(&pump_closed_lock);
}

/* Makes the ring: a sealed memfd of a page of positions and the frames, mapped. Returns 0 or an errno value. */
static int
pump_ring(
	struct kl_backend_audio_stream *stream)
{
	struct kl_backend_audio_ring *ring;
	size_t frames_bytes;
	unsigned sample_bytes;
	void *memory;
	int status;

	/* What a frame is, and the ring's frames (a quarter of a second when the client leaves it to the backend). */
	sample_bytes = 4U;
	if (stream->format.format == KL_BACKEND_AUDIO_FORMAT_S16_LE)
		sample_bytes = 2U;
	stream->frame_bytes = sample_bytes * stream->format.channels;
	stream->capacity = stream->format.buffer_frames;
	if (stream->capacity == 0U)
		stream->capacity = stream->format.rate / 4U;
	frames_bytes = ((size_t)stream->capacity * stream->frame_bytes + 4095U) & ~(size_t)4095U;
	stream->ring_bytes = KL_BACKEND_AUDIO_RING_HEADER + frames_bytes;

	/* The memory, of its size for good: sealed against shrinking and growing, and the seals themselves. */
	stream->ring_fd = memfd_create("keiland-audio", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (stream->ring_fd < 0)
		return errno;
	status = ftruncate(stream->ring_fd, (off_t)stream->ring_bytes);
	if (status != 0)
		return errno;
	status = fcntl(stream->ring_fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
	if (status != 0)
		return errno;

	/* Mapped for the pump. */
	memory = mmap(NULL, stream->ring_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, stream->ring_fd, 0);
	if (memory == MAP_FAILED)
		return errno;
	stream->ring = memory;

	/* Its page: what the frames are; the positions start at 0 (the memory is zero). */
	ring = stream->ring;
	ring->tag = PUMP_TAG;
	ring->version = KL_BACKEND_AUDIO_RING_VERSION;
	ring->format = stream->format.format;
	ring->channels = stream->format.channels;
	ring->rate = stream->format.rate;
	ring->frame_bytes = stream->frame_bytes;
	ring->capacity_frames = stream->capacity;
	ring->period_frames = stream->format.period_frames;
	ring->state = KL_BACKEND_AUDIO_STATE_STOPPED;

	/* Succeeded: the ring is made. */
	return 0;
}

/* The pump: opens the device, then carries the commands out and the frames over until the stream closes or ends. */
static void *
pump_run(
	void *argument)
{
	struct kl_backend_audio_stream *stream;
	struct pump_command commands[PUMP_COMMANDS];
	unsigned count;
	unsigned index;
	unsigned error;
	unsigned sample_bytes;
	int status;
	int fd;

	/* The device, for the format asked. */
	stream = argument;
	error = stream->ops->open(&stream->format, &stream->device, &stream->device_format, &stream->period);
	if (error != KL_BACKEND_AUDIO_ERROR_NONE) {
		pump_end(stream, KL_BACKEND_AUDIO_FAILED, error);
		pthread_mutex_lock(&stream->lock);
		stream->finished = 1U;
		pthread_mutex_unlock(&stream->lock);
		return NULL;
	}

	/* What a frame is on the device. */
	sample_bytes = 4U;
	if (stream->device_format == KL_BACKEND_AUDIO_FORMAT_S16_LE)
		sample_bytes = 2U;
	stream->device_frame_bytes = sample_bytes * stream->format.channels;
	stream->ring->period_frames = stream->period;

	/* READY: a descriptor of the ring for the client. */
	fd = fcntl(stream->ring_fd, F_DUPFD_CLOEXEC, 0);
	if (fd < 0) {
		pump_end(stream, KL_BACKEND_AUDIO_FAILED, KL_BACKEND_AUDIO_ERROR_NO_MEMORY);
	} else {
		pthread_mutex_lock(&stream->lock);
		pump_report(stream, KL_BACKEND_AUDIO_READY, KL_BACKEND_AUDIO_ERROR_NONE, 0U, 0U);
		stream->reports[(stream->report_first + stream->report_count - 1U) % PUMP_REPORTS].fd = fd;
		pthread_mutex_unlock(&stream->lock);
	}

	/* Until the stream closes or ends. */
	for (;;) {
		/* The commands, and whether the stream closes. */
		count = pump_take(stream, commands);
		if (count == PUMP_COMMANDS + 1U)
			break;

		/* Each command; one that ended the stream ends the pump's work. */
		status = 0;
		for (index = 0U; index < count && status == 0; index++)
			status = pump_command(stream, &commands[index]);

		/* The frames over, while running. */
		if (status == 0)
			status = pump_step(stream);

		/* Ended: only the close is waited for now. */
		if (status != 0) {
			stream->running = 0U;
			stream->draining = 0U;
		}

		/* Until the device takes frames, a command comes, or a while has passed. */
		pump_wait(stream);
	}

	/* The device (an ended stream's is closed already), then the pump is done. */
	if (stream->device != NULL)
		stream->ops->close(stream->device);
	stream->device = NULL;
	pthread_mutex_lock(&stream->lock);
	stream->finished = 1U;
	pthread_mutex_unlock(&stream->lock);
	return NULL;
}

/* Takes the commands queued; gives how many, or PUMP_COMMANDS + 1 when the stream closes. */
static unsigned
pump_take(
	struct kl_backend_audio_stream *stream,
	struct pump_command *commands)
{
	unsigned count;
	unsigned closing;

	/* The queue emptied into the pump's copy. */
	pthread_mutex_lock(&stream->lock);

	closing = stream->closing;
	count = stream->command_count;
	memcpy(commands, stream->commands, count * sizeof(commands[0]));
	stream->command_count = 0U;

	pthread_mutex_unlock(&stream->lock);

	/* A closed stream's commands are dropped. */
	if (closing)
		return PUMP_COMMANDS + 1U;

	/* Succeeded: the commands. */
	return count;
}

/* Carries out one command and reports its result; returns nonzero when the stream ended. */
static int
pump_command(
	struct kl_backend_audio_stream *stream,
	const struct pump_command *command)
{
	uint64_t write;

	/* An ended stream answers nothing more (its LOST was the last report). */
	if (stream->device == NULL)
		return 1;

	/* Which control. */
	switch (command->what) {
	case KL_BACKEND_AUDIO_START:
		/* Running from now: a paused device resumes; a drain under way ends. */
		stream->running = 1U;
		stream->draining = 0U;
		stream->empty_told = 0U;
		stream->ops->pause(stream->device, 0);
		break;
	case KL_BACKEND_AUDIO_STOP:
		/* A pause: what is written stays in the ring; the position heard stands. */
		stream->running = 0U;
		stream->draining = 0U;
		stream->ops->pause(stream->device, 1);
		pump_played(stream, 0);
		break;
	case KL_BACKEND_AUDIO_FLUSH:
		/* What is written and not read is dropped, the device's too; a running stream keeps running, a drain ends. */
		write = __atomic_load_n(&stream->ring->write_position, __ATOMIC_ACQUIRE);
		if (write < stream->read || write - stream->read > stream->capacity) {
			pump_end(stream, KL_BACKEND_AUDIO_LOST, KL_BACKEND_AUDIO_ERROR_BROKEN);
			return 1;
		}

		/* The device's frames too, and the ring's from the write position on. */
		stream->ops->drop(stream->device);
		stream->read = write;
		__atomic_store_n(&stream->ring->read_position, stream->read, __ATOMIC_RELEASE);
		stream->silence_tail = 0U;
		pump_played(stream, 1);
		if (stream->draining) {
			stream->draining = 0U;
			stream->running = 0U;
		}

		/* A drain ends with the flush. */
		break;
	case KL_BACKEND_AUDIO_DRAIN:
		/* What is written plays out, then the stream stops. */
		stream->running = 1U;
		stream->draining = 1U;
		stream->drain_started = 0U;
		stream->drain_request = command->request;
		stream->ops->pause(stream->device, 0);
		break;
	default:
		break;
	}

	/* The ring's state, for the client. */
	stream->ring->state = KL_BACKEND_AUDIO_STATE_STOPPED;
	if (stream->running)
		stream->ring->state = KL_BACKEND_AUDIO_STATE_RUNNING;
	if (stream->draining)
		stream->ring->state = KL_BACKEND_AUDIO_STATE_DRAINING;

	/* The result. */
	pthread_mutex_lock(&stream->lock);
	pump_report(stream, KL_BACKEND_AUDIO_RESULT, KL_BACKEND_AUDIO_ERROR_NONE, command->request, 0U);
	pthread_mutex_unlock(&stream->lock);

	/* Succeeded: the stream goes on. */
	return 0;
}

/* Carries frames over while running, or silence when the ring ran dry, or finishes a drain; returns nonzero when the stream ended. */
static int
pump_step(
	struct kl_backend_audio_stream *stream)
{
	uint64_t write;
	uint64_t frames;
	long avail;
	long written;
	int idle;
	int status;

	/* Only a running stream. */
	if (!stream->running || stream->device == NULL)
		return 0;

	/* The client's position, checked: never behind what was read, never more than the ring ahead. */
	write = __atomic_load_n(&stream->ring->write_position, __ATOMIC_ACQUIRE);
	if (write < stream->read || write - stream->read > stream->capacity) {
		pump_end(stream, KL_BACKEND_AUDIO_LOST, KL_BACKEND_AUDIO_ERROR_BROKEN);
		return 1;
	}

	/* The frames waiting, carried over. */
	frames = write - stream->read;
	if (frames != 0U) {
		status = pump_carry(stream, frames);
		if (status != 0)
			return status;
		pump_played(stream, 0);
		return 0;
	}

	/* A drain: the device starts what it holds, and the drain ends once it played it out. */
	if (stream->draining) {
		if (!stream->drain_started) {
			stream->drain_started = 1U;
			stream->ops->start(stream->device);
		}

		/* Whether the device played out what it held. */
		idle = stream->ops->idle(stream->device);
		if (idle) {
			stream->running = 0U;
			stream->draining = 0U;
			stream->ring->state = KL_BACKEND_AUDIO_STATE_STOPPED;
			pump_played(stream, 1);
			pthread_mutex_lock(&stream->lock);
			pump_report(stream, KL_BACKEND_AUDIO_DRAINED, KL_BACKEND_AUDIO_ERROR_NONE, stream->drain_request, 0U);
			pthread_mutex_unlock(&stream->lock);
		}

		/* Draining until then. */
		return 0;
	}

	/* The ring ran dry while running: an underrun, told when it begins. */
	if (!stream->empty_told) {
		stream->empty_told = 1U;
		stream->underruns++;
		stream->ring->underruns = stream->underruns;
		pthread_mutex_lock(&stream->lock);
		pump_report(stream, KL_BACKEND_AUDIO_UNDERRUN, KL_BACKEND_AUDIO_ERROR_NONE, 0U, stream->underruns);
		pthread_mutex_unlock(&stream->lock);
	}

	/* A period of silence keeps the device going (counted, so that the position heard leaves it out). */
	avail = stream->ops->avail(stream->device);
	if (avail < 0) {
		pump_end(stream, KL_BACKEND_AUDIO_LOST, KL_BACKEND_AUDIO_ERROR_GONE);
		return 1;
	}

	/* The silence, when the device has room for a period. */
	if ((unsigned long)avail >= stream->period && stream->period <= PUMP_CHUNK) {
		memset(stream->chunk, 0, (size_t)stream->period * stream->device_frame_bytes);
		written = stream->ops->write(stream->device, stream->chunk, stream->period);
		if (written < 0) {
			pump_end(stream, KL_BACKEND_AUDIO_LOST, KL_BACKEND_AUDIO_ERROR_GONE);
			return 1;
		}

		/* Counted at the tail. */
		stream->silence_tail += (uint64_t)written;
	}

	/* Succeeded: the device has something to play. */
	pump_played(stream, 0);
	return 0;
}

/* Carries up to the frames waiting (as many as the device takes now) from the ring to the device; returns nonzero when the stream ended. */
static int
pump_carry(
	struct kl_backend_audio_stream *stream,
	uint64_t frames)
{
	const unsigned char *place;
	unsigned long count;
	unsigned long index;
	unsigned long first;
	long avail;
	long written;

	/* As many as the device takes now, at most a chunk. */
	avail = stream->ops->avail(stream->device);
	if (avail < 0) {
		pump_end(stream, KL_BACKEND_AUDIO_LOST, KL_BACKEND_AUDIO_ERROR_GONE);
		return 1;
	}

	/* The frames, as many as fit. */
	count = (unsigned long)avail;
	if (count > frames)
		count = (unsigned long)frames;
	if (count > PUMP_CHUNK)
		count = PUMP_CHUNK;
	if (count == 0U)
		return 0;

	/* The frames, in at most two parts around the ring's end, in the device's format. */
	place = (const unsigned char *)stream->ring + KL_BACKEND_AUDIO_RING_HEADER;
	index = (unsigned long)(stream->read % stream->capacity);
	first = stream->capacity - index;
	if (first > count)
		first = count;
	pump_convert(stream, place + index * stream->frame_bytes, (unsigned)first, stream->chunk);
	if (count > first)
		pump_convert(stream, place, (unsigned)(count - first), stream->chunk + first * stream->device_frame_bytes);

	/* Written; what the device took is read. */
	written = stream->ops->write(stream->device, stream->chunk, count);
	if (written < 0) {
		pump_end(stream, KL_BACKEND_AUDIO_LOST, KL_BACKEND_AUDIO_ERROR_GONE);
		return 1;
	}

	/* What the device took is read. */
	stream->read += (uint64_t)written;
	__atomic_store_n(&stream->ring->read_position, stream->read, __ATOMIC_RELEASE);

	/* Real frames again: the silence before them is no longer at the tail, and a new dry spell is a new underrun. */
	if (written > 0) {
		stream->silence_tail = 0U;
		stream->empty_told = 0U;
	}

	/* Succeeded: carried over. */
	return 0;
}

/* Copies frames of the ring into the device's format (16-bit when the device does not take the ring's). */
static void
pump_convert(
	struct kl_backend_audio_stream *stream,
	const unsigned char *source,
	unsigned frames,
	unsigned char *target)
{
	int16_t *out;
	int32_t whole;
	float fraction;
	unsigned samples;
	unsigned index;

	/* The same format: copied as it is. */
	if (stream->device_format == stream->format.format) {
		memcpy(target, source, (size_t)frames * stream->frame_bytes);
		return;
	}

	/* Each sample to 16 bits: a 32-bit one's high half, a float's scaled and held to the range. */
	out = (int16_t *)(void *)target;
	samples = frames * stream->format.channels;
	for (index = 0U; index < samples; index++) {
		if (stream->format.format == KL_BACKEND_AUDIO_FORMAT_S32_LE) {
			memcpy(&whole, source + (size_t)index * 4U, sizeof(whole));
			out[index] = (int16_t)(whole >> 16);
		} else {
			memcpy(&fraction, source + (size_t)index * 4U, sizeof(fraction));
			if (fraction > 1.0f)
				fraction = 1.0f;
			if (fraction < -1.0f)
				fraction = -1.0f;
			out[index] = (int16_t)(fraction * 32767.0f);
		}
	}
}

/*
 * Reckons the position heard and writes it with its time under the ring's
 * played sequence: what was read less what the device holds and has not
 * played, the silence at its tail left out; exact (after a flush or a
 * drain) when the device holds nothing.  It never goes back.
 */
static void
pump_played(
	struct kl_backend_audio_stream *stream,
	int exact)
{
	struct kl_backend_audio_ring *ring;
	uint64_t unheard;
	uint64_t estimate;
	uint32_t sequence;
	long delay;

	/* What the device holds of the frames read. */
	unheard = 0U;
	if (!exact && stream->device != NULL) {
		delay = stream->ops->delay(stream->device);
		if (delay > 0 && (uint64_t)delay > stream->silence_tail)
			unheard = (uint64_t)delay - stream->silence_tail;
	}

	/* The estimate, never back. */
	estimate = 0U;
	if (stream->read > unheard)
		estimate = stream->read - unheard;
	if (estimate < stream->played)
		estimate = stream->played;
	stream->played = estimate;

	/* Written as one: the sequence is odd meanwhile. */
	ring = stream->ring;
	sequence = ring->played_sequence;
	__atomic_store_n(&ring->played_sequence, sequence + 1U, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&ring->played_position, estimate, __ATOMIC_RELAXED);
	__atomic_store_n(&ring->played_time_ns, pump_now_ns(), __ATOMIC_RELAXED);
	__atomic_store_n(&ring->played_sequence, sequence + 2U, __ATOMIC_RELEASE);
}

/* Waits until the device takes frames (while running), a command comes on the wake pipe, or a while has passed. */
static void
pump_wait(
	struct kl_backend_audio_stream *stream)
{
	struct pollfd descriptors[1 + PUMP_DESCRIPTORS];
	unsigned char bytes[64];
	ssize_t got;
	int count;
	int timeout;

	/* The wake pipe; the device's descriptors only while running (a stopped pump waits for a command alone). */
	descriptors[0].fd = stream->wake[0];
	descriptors[0].events = POLLIN;
	descriptors[0].revents = 0;
	count = 1;
	timeout = -1;
	if (stream->running && stream->device != NULL) {
		count += stream->ops->descriptors(stream->device, descriptors + 1, PUMP_DESCRIPTORS);
		timeout = PUMP_WAIT_MS;
	}

	/* The wait; the frames a client writes wake nothing, so a running pump looks again after a while. */
	(void)poll(descriptors, (nfds_t)count, timeout);

	/* The wake pipe's bytes are taken (they only woke the pump). */
	do {
		got = read(stream->wake[0], bytes, sizeof(bytes));
	} while (got > 0);
}

/* Wakes the pump (a full pipe already wakes it). */
static void
pump_wake(
	struct kl_backend_audio_stream *stream)
{
	unsigned char byte;

	/* One byte. */
	byte = 1U;
	(void)write(stream->wake[1], &byte, 1U);
}

/* Keeps a report for kl_backend_audio_stream_next; the lock is held.  A full queue drops it (a READY's descriptor is closed by its taker). */
static void
pump_report(
	struct kl_backend_audio_stream *stream,
	unsigned what,
	unsigned error,
	uint32_t request,
	uint32_t count)
{
	struct kl_backend_audio_stream_report *report;

	/* A full queue cannot happen (one command a time, one READY, one FAILED or LOST, underruns told when they begin). */
	if (stream->report_count == PUMP_REPORTS)
		return;

	/* After the reports kept. */
	report = &stream->reports[(stream->report_first + stream->report_count) % PUMP_REPORTS];
	memset(report, 0, sizeof(*report));
	report->what = what;
	report->error = error;
	report->request = request;
	report->count = count;
	report->fd = -1;
	if (what == KL_BACKEND_AUDIO_READY) {
		report->bytes = (uint32_t)stream->ring_bytes;
		report->capacity_frames = stream->capacity;
		report->period_frames = stream->period;
	}

	/* Kept. */
	stream->report_count++;
}

/* Ends the stream with FAILED or LOST: nothing more is reported, and controls are refused. */
static void
pump_end(
	struct kl_backend_audio_stream *stream,
	unsigned what,
	unsigned error)
{
	/* The last report. */
	pthread_mutex_lock(&stream->lock);

	if (!stream->ended) {
		stream->ended = 1U;
		pump_report(stream, what, error, 0U, 0U);
	}

	pthread_mutex_unlock(&stream->lock);

	/* The device goes now (the pump only waits for the close from here). */
	if (stream->device != NULL) {
		stream->ops->close(stream->device);
		stream->device = NULL;
	}
}

/* Joins a stream's pump (it was told to end) and frees what it holds. */
static void
pump_free(
	struct kl_backend_audio_stream *stream)
{
	unsigned index;
	int fd;

	/* The pump, which ends at its next look. */
	if (stream->thread_started)
		(void)pthread_join(stream->thread, NULL);

	/* A ring handed over in a report nobody took. */
	for (index = 0U; index < stream->report_count; index++) {
		fd = stream->reports[(stream->report_first + index) % PUMP_REPORTS].fd;
		if (fd >= 0)
			(void)close(fd);
	}

	/* The ring, the pipe and the record. */
	if (stream->ring != NULL)
		(void)munmap(stream->ring, stream->ring_bytes);
	if (stream->ring_fd >= 0)
		(void)close(stream->ring_fd);
	if (stream->wake[0] >= 0)
		(void)close(stream->wake[0]);
	if (stream->wake[1] >= 0)
		(void)close(stream->wake[1]);
	(void)pthread_mutex_destroy(&stream->lock);
	free(stream);
}

/* Gives the monotonic clock in nanoseconds. */
static int64_t
pump_now_ns(
	void)
{
	struct timespec now;

	/* The clock (it does not fail with a valid clock and place). */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: the time. */
	return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}
