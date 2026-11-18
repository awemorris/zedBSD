/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * audio-loop (ws204-p003, test images only): checks recording through
 * audiod by playing a tone and recording it at once.
 *
 *   audio-loop [--seconds=S] [--frequency=HZ] [--level=DB] [--rate=R]
 *              [--margin=DB] [--device-volume=N] [--wav=PATH]
 *   audio-loop --record=S --wav=PATH [--rate=R] [--device-volume=N]
 *
 * The first form records one second of quiet, then plays a sine (1 kHz at
 * -12 dBFS for 3 seconds by default) while it goes on recording, and
 * compares the tone's level in the recording with the quiet's noise.  On a
 * laptop the speaker's sound reaches the microphone.  It prints one line:
 *
 *   AUDIOLOOP rate= frames= expected= ms= overruns= underruns= noise_rms=
 *       noise_tone= tone= tone_rms= peak= margin= result=PASS|FAIL|SILENT
 *
 * (levels in dBFS against a full-scale sine; margin is the tone less the
 * noise's RMS; PASS when it is at least --margin, 20 dB by default; SILENT
 * when every recorded sample was zero).  The second form only records, for
 * a person to listen to.  --wav saves what was recorded (16-bit stereo).
 * --device-volume sets the output's volume (0..100) first and leaves it.
 *
 * Exits 0 on PASS (or a finished recording), 1 on FAIL or SILENT, 2 on bad
 * arguments, 3 when audiod or the file failed.
 */

#include "userland/base/audiod/protocol.h"
#include "userland/tests/audio-loop/measure.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* The streams' numbers on the connection. */
#define LOOP_CAPTURE_ID		1U
#define LOOP_PLAYBACK_ID	2U

/* The streams' channels, and their rings and periods (frames). */
#define LOOP_CHANNELS		2U
#define LOOP_PERIOD_FRAMES	1024U

/* The numeric options, in loop_parse's table. */
#define LOOP_SECONDS		0U
#define LOOP_FREQUENCY		1U
#define LOOP_LEVEL		2U
#define LOOP_RATE		3U
#define LOOP_MARGIN		4U
#define LOOP_DEVICE_VOLUME	5U
#define LOOP_RECORD		6U
#define LOOP_NUMBERS		7U

/* How long a recording may stand still, and the time allowed beyond the expected (milliseconds). */
#define LOOP_STALL_MS		2000L
#define LOOP_SLACK_MS		5000L

/*
 * The options: the tone's length, frequency and level, the rate, the
 * margin asked, the output volume to set (-1: leave it), the WAV file, and
 * the recording's length in the second form (0: the first form).
 */
struct loop_options {
	double seconds;
	double frequency;
	double level;
	unsigned rate;
	double margin;
	int device_volume;
	const char *wav;
	double record;
};

/* The connection to audiod and the serial of its last request. */
struct loop_link {
	int fd;
	uint32_t serial;
};

/* One stream: its number and its shared memory (the header, then the ring). */
struct loop_stream {
	uint32_t id;
	int fd;
	size_t bytes;
	struct audiod_shm_header *shm;
	uint8_t *ring;
};

/* What was recorded: stereo frames, their room and their number. */
struct loop_take {
	int16_t *frames;
	size_t capacity;
	size_t count;
};

static int loop_parse(int argc, char **argv, struct loop_options *options);
static int loop_number(const char *text, double *value);
static int loop_connect(struct loop_link *link);
static int loop_receive(struct loop_link *link, void *buffer);
static int loop_request(struct loop_link *link, void *message, uint32_t length, void *reply, int *fd);
static int loop_open(struct loop_link *link, struct loop_stream *stream, uint32_t id, uint32_t direction, unsigned rate, uint32_t buffer_frames);
static int loop_simple(struct loop_link *link, const struct loop_stream *stream, uint32_t type);
static int loop_device_volume(struct loop_link *link, unsigned volume);
static int loop_send_drain(struct loop_link *link, const struct loop_stream *stream);
static int loop_wait(struct loop_link *link, int timeout_ms);
static size_t loop_collect(struct loop_stream *stream, struct loop_take *take);
static uint64_t loop_feed(struct loop_stream *stream, uint64_t written, uint64_t total, const struct loop_options *options);
static int loop_record(struct loop_link *link, struct loop_stream *capture, struct loop_take *take, size_t frames);
static int loop_play(struct loop_link *link, struct loop_stream *capture, struct loop_stream *playback, struct loop_take *take, const struct loop_options *options, size_t *tone_start);
static int loop_save(const char *path, const struct loop_take *take, unsigned rate);
static int loop_judge(const struct loop_take *take, size_t tone_start, const struct loop_options *options, long elapsed, unsigned overruns, unsigned underruns);
static long loop_ms_since(const struct timespec *start);

/*
 * Plays and records (or only records), measures, saves the WAV when asked
 * and prints the result line.
 */
int
main(
	int argc,
	char **argv)
{
	struct loop_options options;
	struct loop_link link;
	struct loop_stream capture;
	struct loop_stream playback;
	struct loop_take take;
	struct timespec start;
	double seconds;
	size_t tone_start;
	long elapsed;
	int status;
	int error;

	/* The options. */
	error = loop_parse(argc, argv, &options);
	if (error != 0) {
		fprintf(stderr, "usage: audio-loop [--seconds=S] [--frequency=HZ] [--level=DB] [--rate=R] [--margin=DB] [--device-volume=N] [--wav=PATH]\n"
		    "       audio-loop --record=S --wav=PATH [--rate=R] [--device-volume=N]\n");
		return 2;
	}

	/* The room for the whole recording: the quiet, the tone and half a second after it. */
	seconds = options.record;
	if (seconds == 0.0)
		seconds = 1.0 + options.seconds + 0.5;
	memset(&take, 0, sizeof(take));
	take.capacity = (size_t)(seconds * (double)options.rate);
	take.frames = calloc(take.capacity, LOOP_CHANNELS * sizeof(int16_t));
	if (take.frames == NULL) {
		fprintf(stderr, "audio-loop: no memory for %zu frames\n", take.capacity);
		return 3;
	}

	/* The connection. */
	error = loop_connect(&link);
	if (error != 0) {
		fprintf(stderr, "audio-loop: audiod: %s\n", strerror(error));
		return 3;
	}

	/* The output's volume, when asked. */
	if (options.device_volume >= 0) {
		error = loop_device_volume(&link, (unsigned)options.device_volume);
		if (error != 0) {
			fprintf(stderr, "audio-loop: device volume: %s\n", strerror(error));
			return 3;
		}
	}

	/* The recording stream, a second of room so that a slow reader does not lose frames. */
	error = loop_open(&link, &capture, LOOP_CAPTURE_ID, AUDIOD_CAPTURE, options.rate, options.rate);
	if (error != 0) {
		fprintf(stderr, "audio-loop: capture stream: %s\n", strerror(error));
		return 3;
	}

	/* The playing stream, in the first form. */
	memset(&playback, 0, sizeof(playback));
	if (options.record == 0.0) {
		error = loop_open(&link, &playback, LOOP_PLAYBACK_ID, AUDIOD_PLAYBACK, options.rate, 4U * LOOP_PERIOD_FRAMES);
		if (error != 0) {
			fprintf(stderr, "audio-loop: playback stream: %s\n", strerror(error));
			return 3;
		}
	}

	/* The recording, with the tone in the first form. */
	clock_gettime(CLOCK_MONOTONIC, &start);
	tone_start = 0U;
	if (options.record != 0.0) {
		error = loop_record(&link, &capture, &take, take.capacity);
	} else {
		error = loop_play(&link, &capture, &playback, &take, &options, &tone_start);
	}

	/* How long it took; a recording that stopped short is reported. */
	elapsed = loop_ms_since(&start);
	if (error != 0) {
		fprintf(stderr, "audio-loop: recording stopped after %zu frames: %s\n", take.count, strerror(error));
		return 3;
	}

	/* The WAV file, when asked. */
	if (options.wav != NULL) {
		error = loop_save(options.wav, &take, options.rate);
		if (error != 0) {
			fprintf(stderr, "audio-loop: %s: %s\n", options.wav, strerror(error));
			return 3;
		}
	}

	/* The second form's line: what was recorded. */
	if (options.record != 0.0) {
		printf("AUDIOLOOP record rate=%u frames=%zu ms=%ld overruns=%u\n",
		    options.rate, take.count, elapsed, capture.shm->overruns);
		return 0;
	}

	/* The first form's measures and verdict. */
	status = loop_judge(&take, tone_start, &options, elapsed, capture.shm->overruns, playback.shm->underruns);
	if (status != 0)
		return 1;

	/* Succeeded: the tone was heard above the noise. */
	return 0;
}

/*
 * Reads the options into *options, the defaults first.  Returns 0, or
 * EINVAL for anything unknown or out of range.
 */
static int
loop_parse(
	int argc,
	char **argv,
	struct loop_options *options)
{
	static const char *const names[LOOP_NUMBERS] = {
		"--seconds=", "--frequency=", "--level=", "--rate=", "--margin=", "--device-volume=", "--record="
	};
	double numbers[LOOP_NUMBERS] = {3.0, 1000.0, -12.0, 48000.0, 20.0, -1.0, 0.0};
	const char *argument;
	size_t length;
	size_t name;
	int index;
	int same;
	int error;

	/* No WAV file unless one is named. */
	options->wav = NULL;

	/* Each argument, a --name=value. */
	for (index = 1; index < argc; index++) {
		argument = argv[index];

		/* The WAV file's path, as it is. */
		same = strncmp(argument, "--wav=", 6U);
		if (same == 0) {
			options->wav = argument + 6;
			if (options->wav[0] == '\0')
				return EINVAL;
			continue;
		}

		/* Every other option is a number, by its name. */
		for (name = 0U; name < LOOP_NUMBERS; name++) {
			length = strlen(names[name]);
			same = strncmp(argument, names[name], length);
			if (same == 0)
				break;
		}

		/* An unknown name, or a value that is not a number. */
		if (name == LOOP_NUMBERS)
			return EINVAL;
		error = loop_number(argument + length, &numbers[name]);
		if (error != 0)
			return EINVAL;
	}

	/* The numbers in their places. */
	options->seconds = numbers[LOOP_SECONDS];
	options->frequency = numbers[LOOP_FREQUENCY];
	options->level = numbers[LOOP_LEVEL];
	options->rate = (unsigned)numbers[LOOP_RATE];
	options->margin = numbers[LOOP_MARGIN];
	options->device_volume = (int)numbers[LOOP_DEVICE_VOLUME];
	options->record = numbers[LOOP_RECORD];

	/* The rate audiod takes, a tone below half of it, at most full scale, long enough to measure. */
	if (numbers[LOOP_RATE] < 8000.0 || numbers[LOOP_RATE] > 192000.0)
		return EINVAL;
	if (options->frequency <= 0.0 || options->frequency >= (double)options->rate / 2.0)
		return EINVAL;
	if (options->level > 0.0)
		return EINVAL;
	if (options->seconds < 1.0 || options->seconds > 60.0)
		return EINVAL;
	if (options->device_volume > 100)
		return EINVAL;

	/* The second form: a length, and a file to put it in. */
	if (options->record < 0.0 || options->record > 600.0)
		return EINVAL;
	if (options->record != 0.0 && options->wav == NULL)
		return EINVAL;

	/* Succeeded: the options. */
	return 0;
}

/* Reads a whole decimal number.  Returns 0, or EINVAL. */
static int
loop_number(
	const char *text,
	double *value)
{
	char *end;
	double number;

	/* The number, and nothing after it. */
	if (text[0] == '\0')
		return EINVAL;
	number = strtod(text, &end);
	if (*end != '\0')
		return EINVAL;

	/* Succeeded: the number. */
	*value = number;
	return 0;
}

/* Connects to audiod and says hello.  Returns 0 or an errno value. */
static int
loop_connect(
	struct loop_link *link)
{
	struct sockaddr_un address;
	struct audiod_hello hello;
	uint8_t reply[AUDIOD_MESSAGE_MAX];
	int result;
	int error;

	/* The socket. */
	memset(link, 0, sizeof(*link));
	link->fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (link->fd < 0)
		return errno;

	/* Audiod's socket. */
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, AUDIOD_SOCKET_PATH);
	result = connect(link->fd, (struct sockaddr *)&address, sizeof(address));
	if (result != 0)
		return errno;

	/* The greeting, answered WELCOME. */
	memset(&hello, 0, sizeof(hello));
	hello.header.type = AUDIOD_HELLO;
	hello.version = AUDIOD_PROTOCOL_VERSION_1;
	error = loop_request(link, &hello, sizeof(hello), reply, NULL);
	if (error != 0)
		return error;

	/* Succeeded: connected. */
	return 0;
}

/*
 * Receives one message into buffer (AUDIOD_MESSAGE_MAX bytes) and returns
 * the descriptor that came with it, or -1.  The header is read first with
 * the control buffer, where a passed descriptor arrives, then the rest.
 * Returns -2 when the connection failed.
 */
static int
loop_receive(
	struct loop_link *link,
	void *buffer)
{
	union {
		struct cmsghdr header;
		char space[CMSG_SPACE(sizeof(int))];
	} control;
	struct audiod_header *header;
	struct msghdr message;
	struct iovec vector;
	struct cmsghdr *cmsg;
	size_t have;
	ssize_t got;
	int fd;

	/* The header, and a descriptor with it. */
	header = buffer;
	fd = -1;
	have = 0U;
	while (have < sizeof(*header)) {
		memset(&message, 0, sizeof(message));
		vector.iov_base = (uint8_t *)buffer + have;
		vector.iov_len = sizeof(*header) - have;
		message.msg_iov = &vector;
		message.msg_iovlen = 1;
		message.msg_control = control.space;
		message.msg_controllen = sizeof(control.space);
		got = recvmsg(link->fd, &message, 0);
		if (got <= 0)
			return -2;
		cmsg = CMSG_FIRSTHDR(&message);
		if (cmsg != NULL && cmsg->cmsg_type == SCM_RIGHTS)
			memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
		have += (size_t)got;
	}

	/* A length audiod sends. */
	if (header->length < sizeof(*header) || header->length > AUDIOD_MESSAGE_MAX)
		return -2;

	/* The rest. */
	while (have < header->length) {
		got = recv(link->fd, (uint8_t *)buffer + have, header->length - have, 0);
		if (got <= 0)
			return -2;
		have += (size_t)got;
	}

	/* Succeeded: the message, and its descriptor if any. */
	return fd;
}

/*
 * Sends a request and waits for its answer (WELCOME, STREAM_CREATED, DONE
 * or ERROR with its serial), passing over the events.  The answer goes to
 * reply and its descriptor to *fd when fd is not NULL.  Returns 0, the
 * ERROR's errno value, or EIO when the connection failed.
 */
static int
loop_request(
	struct loop_link *link,
	void *message,
	uint32_t length,
	void *reply,
	int *fd)
{
	struct audiod_header *header;
	struct audiod_header *answer;
	uint8_t buffer[AUDIOD_MESSAGE_MAX];
	ssize_t sent;
	int received;

	/* The request, numbered. */
	header = message;
	header->length = length;
	link->serial++;
	header->serial = link->serial;
	sent = send(link->fd, message, length, 0);
	if (sent != (ssize_t)length)
		return EIO;

	/* Messages until its answer. */
	answer = (struct audiod_header *)(void *)buffer;
	for (;;) {
		received = loop_receive(link, buffer);
		if (received == -2)
			return EIO;

		/* An event, or another request's answer, passed over. */
		if (answer->serial != link->serial ||
		    (answer->type != AUDIOD_WELCOME &&
		     answer->type != AUDIOD_STREAM_CREATED &&
		     answer->type != AUDIOD_DONE &&
		     answer->type != AUDIOD_ERROR)) {
			if (received >= 0)
				close(received);
			continue;
		}

		/* The answer. */
		memcpy(reply, buffer, answer->length);
		if (fd != NULL) {
			*fd = received;
		} else if (received >= 0) {
			close(received);
		}

		/* Found. */
		break;
	}

	/* A refusal. */
	if (answer->type == AUDIOD_ERROR)
		return (int)((struct audiod_result *)(void *)buffer)->error;

	/* Succeeded: answered. */
	return 0;
}

/*
 * Creates a stream of 16-bit stereo frames at rate with a ring of
 * buffer_frames and maps its shared memory.  Returns 0 or an errno value.
 */
static int
loop_open(
	struct loop_link *link,
	struct loop_stream *stream,
	uint32_t id,
	uint32_t direction,
	unsigned rate,
	uint32_t buffer_frames)
{
	struct audiod_stream_create create;
	struct audiod_stream_created created;
	void *memory;
	int error;
	int fd;

	/* The stream asked for, answered with its memory. */
	memset(&create, 0, sizeof(create));
	create.header.type = AUDIOD_STREAM_CREATE;
	create.header.stream = id;
	create.direction = direction;
	create.format = AUDIOD_FORMAT_S16_LE;
	create.channels = LOOP_CHANNELS;
	create.rate = rate;
	create.buffer_frames = buffer_frames;
	create.period_frames = LOOP_PERIOD_FRAMES;
	fd = -1;
	error = loop_request(link, &create, sizeof(create), &created, &fd);
	if (error != 0)
		return error;
	if (fd < 0)
		return EPROTO;

	/* Its memory mapped. */
	memory = mmap(NULL, created.shm_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (memory == MAP_FAILED)
		return errno;

	/* The stream. */
	memset(stream, 0, sizeof(*stream));
	stream->id = id;
	stream->fd = fd;
	stream->bytes = created.shm_bytes;
	stream->shm = memory;
	stream->ring = (uint8_t *)memory + AUDIOD_SHM_HEADER;

	/* Memory that is audiod's ring. */
	if (stream->shm->magic != AUDIOD_SHM_MAGIC)
		return EPROTO;

	/* Succeeded: the stream. */
	return 0;
}

/* Sends a request of the header alone about a stream (start, stop).  Returns 0 or an errno value. */
static int
loop_simple(
	struct loop_link *link,
	const struct loop_stream *stream,
	uint32_t type)
{
	struct audiod_header header;
	uint8_t reply[AUDIOD_MESSAGE_MAX];
	int error;

	/* The request. */
	memset(&header, 0, sizeof(header));
	header.type = type;
	header.stream = stream->id;
	error = loop_request(link, &header, sizeof(header), reply, NULL);
	if (error != 0)
		return error;

	/* Succeeded: done. */
	return 0;
}

/* Sets the output's volume (0..100, both channels, not muted).  Returns 0 or an errno value. */
static int
loop_device_volume(
	struct loop_link *link,
	unsigned volume)
{
	struct audiod_volume message;
	uint8_t reply[AUDIOD_MESSAGE_MAX];
	int error;

	/* The request. */
	memset(&message, 0, sizeof(message));
	message.header.type = AUDIOD_DEVICE_VOLUME;
	message.left = volume;
	message.right = volume;
	message.muted = 0U;
	error = loop_request(link, &message, sizeof(message), reply, NULL);
	if (error != 0)
		return error;

	/* Succeeded: set. */
	return 0;
}

/*
 * Asks audiod to play out what the stream holds and stop, without waiting
 * for DRAINED (the recording goes on meanwhile).  Returns 0 or EIO.
 */
static int
loop_send_drain(
	struct loop_link *link,
	const struct loop_stream *stream)
{
	struct audiod_header header;
	ssize_t sent;

	/* The request, its answers left to loop_wait. */
	memset(&header, 0, sizeof(header));
	header.type = AUDIOD_STREAM_DRAIN;
	header.stream = stream->id;
	header.length = sizeof(header);
	link->serial++;
	header.serial = link->serial;
	sent = send(link->fd, &header, sizeof(header), 0);
	if (sent != (ssize_t)sizeof(header))
		return EIO;

	/* Succeeded: asked. */
	return 0;
}

/*
 * Waits up to timeout_ms for audiod's next message (a period recorded or
 * wanted) and reads it; what it says is in the rings.  Returns 0, or EIO
 * when the connection failed.
 */
static int
loop_wait(
	struct loop_link *link,
	int timeout_ms)
{
	struct pollfd poller;
	uint8_t buffer[AUDIOD_MESSAGE_MAX];
	int ready;
	int received;

	/* A message, or the time. */
	poller.fd = link->fd;
	poller.events = POLLIN;
	poller.revents = 0;
	ready = poll(&poller, 1, timeout_ms);
	if (ready < 0 && errno == EINTR)
		return 0;
	if (ready < 0)
		return EIO;
	if (ready == 0)
		return 0;

	/* The message read, any descriptor with it closed. */
	received = loop_receive(link, buffer);
	if (received == -2)
		return EIO;
	if (received >= 0)
		close(received);

	/* Succeeded: waited. */
	return 0;
}

/*
 * Moves what the capture ring holds into the take (frames past its room
 * are read and dropped) and gives the ring back.  Returns the frames kept.
 */
static size_t
loop_collect(
	struct loop_stream *stream,
	struct loop_take *take)
{
	struct audiod_shm_header *shm;
	uint64_t read_position;
	uint64_t write_position;
	size_t kept;
	size_t slot;

	/* What audiod has written since the last read. */
	shm = stream->shm;
	read_position = shm->read_position;
	write_position = audiod_position_load(&shm->write_position, &shm->write_sequence);

	/* Each frame, kept while there is room. */
	kept = 0U;
	while (read_position < write_position) {
		slot = (size_t)(read_position % shm->capacity_frames);
		if (take->count < take->capacity) {
			memcpy(&take->frames[take->count * LOOP_CHANNELS],
			    stream->ring + slot * shm->frame_bytes,
			    LOOP_CHANNELS * sizeof(int16_t));
			take->count++;
			kept++;
		}

		/* Past it, kept or not. */
		read_position++;
	}

	/* The ring given back. */
	audiod_position_store(&shm->read_position, &shm->read_sequence, read_position);

	/* Succeeded: the frames kept. */
	return kept;
}

/*
 * Writes the tone into the playback ring as far as there is room, up to
 * total frames since it began.  Returns the frames written so far.
 */
static uint64_t
loop_feed(
	struct loop_stream *stream,
	uint64_t written,
	uint64_t total,
	const struct loop_options *options)
{
	struct audiod_shm_header *shm;
	uint64_t read_position;
	uint64_t room;
	uint64_t count;
	size_t slot;
	double amplitude;

	/* The room the reader has left. */
	shm = stream->shm;
	read_position = audiod_position_load(&shm->read_position, &shm->read_sequence);
	room = shm->capacity_frames - (written - read_position);
	amplitude = pow(10.0, options->level / 20.0);

	/* The tone, a run at a time up to the ring's end. */
	while (room != 0U && written < total) {
		slot = (size_t)(written % shm->capacity_frames);
		count = shm->capacity_frames - slot;
		if (count > room)
			count = room;
		if (count > total - written)
			count = total - written;
		loop_tone((int16_t *)(void *)(stream->ring + slot * shm->frame_bytes),
		    (size_t)count, LOOP_CHANNELS, written, options->frequency,
		    options->rate, amplitude);
		written += count;
		room -= count;
	}

	/* The new end published. */
	audiod_position_store(&shm->write_position, &shm->write_sequence, written);

	/* Succeeded: the frames written. */
	return written;
}

/*
 * Records frames frames (the second form).  Returns 0, or ETIMEDOUT when
 * the recording stood still, or an errno value of audiod.
 */
static int
loop_record(
	struct loop_link *link,
	struct loop_stream *capture,
	struct loop_take *take,
	size_t frames)
{
	struct timespec moved;
	size_t kept;
	long still;
	int error;

	/* The recording started. */
	error = loop_simple(link, capture, AUDIOD_STREAM_START);
	if (error != 0)
		return error;

	/* The frames as they come, while they keep coming. */
	clock_gettime(CLOCK_MONOTONIC, &moved);
	while (take->count < frames) {
		kept = loop_collect(capture, take);
		if (kept != 0U)
			clock_gettime(CLOCK_MONOTONIC, &moved);
		still = loop_ms_since(&moved);
		if (still > LOOP_STALL_MS)
			return ETIMEDOUT;
		error = loop_wait(link, 50);
		if (error != 0)
			return error;
	}

	/* The recording stopped. */
	error = loop_simple(link, capture, AUDIOD_STREAM_STOP);
	if (error != 0)
		return error;

	/* Succeeded: recorded. */
	return 0;
}

/*
 * Records a second of quiet, then plays the tone while recording until
 * the take is full; *tone_start is the take's frame at which the tone was
 * started.  Returns 0, ETIMEDOUT when the recording stood still or took
 * far longer than it should, or an errno value of audiod.
 */
static int
loop_play(
	struct loop_link *link,
	struct loop_stream *capture,
	struct loop_stream *playback,
	struct loop_take *take,
	const struct loop_options *options,
	size_t *tone_start)
{
	struct timespec moved;
	struct timespec began;
	uint64_t tone_frames;
	uint64_t written;
	long limit;
	long still;
	size_t kept;
	int playing;
	int drained;
	int error;

	/* The recording started; the tone's length; the time it all may take. */
	error = loop_simple(link, capture, AUDIOD_STREAM_START);
	if (error != 0)
		return error;
	tone_frames = (uint64_t)(options->seconds * (double)options->rate);
	limit = (long)((double)take->capacity * 1000.0 / (double)options->rate) + LOOP_SLACK_MS;
	clock_gettime(CLOCK_MONOTONIC, &moved);
	clock_gettime(CLOCK_MONOTONIC, &began);

	/* The quiet, then the tone, until the take is full. */
	written = 0U;
	playing = 0;
	drained = 0;
	while (take->count < take->capacity) {
		/* What was recorded, while it keeps coming. */
		kept = loop_collect(capture, take);
		if (kept != 0U)
			clock_gettime(CLOCK_MONOTONIC, &moved);
		still = loop_ms_since(&moved);
		if (still > LOOP_STALL_MS)
			return ETIMEDOUT;
		still = loop_ms_since(&began);
		if (still > limit)
			return ETIMEDOUT;

		/* The tone started after the quiet's second. */
		if (!playing && take->count >= options->rate) {
			*tone_start = take->count;
			written = loop_feed(playback, written, tone_frames, options);
			error = loop_simple(link, playback, AUDIOD_STREAM_START);
			if (error != 0)
				return error;
			playing = 1;
		}

		/* The tone kept fed, and played out at its end. */
		if (playing && !drained) {
			written = loop_feed(playback, written, tone_frames, options);
			if (written >= tone_frames) {
				error = loop_send_drain(link, playback);
				if (error != 0)
					return error;
				drained = 1;
			}
		}

		/* The next period. */
		error = loop_wait(link, 20);
		if (error != 0)
			return error;
	}

	/* The recording stopped. */
	error = loop_simple(link, capture, AUDIOD_STREAM_STOP);
	if (error != 0)
		return error;

	/* Succeeded: the quiet and the tone recorded. */
	return 0;
}

/* Writes the take as a 16-bit stereo WAV file.  Returns 0 or an errno value. */
static int
loop_save(
	const char *path,
	const struct loop_take *take,
	unsigned rate)
{
	uint8_t header[LOOP_WAV_HEADER];
	size_t done;
	FILE *file;
	int result;

	/* The file. */
	file = fopen(path, "wb");
	if (file == NULL)
		return errno;

	/* The header, then the frames (little-endian, as this machine is). */
	loop_wav_header(header, rate, LOOP_CHANNELS, (uint32_t)take->count);
	done = fwrite(header, 1U, sizeof(header), file);
	if (done != sizeof(header)) {
		fclose(file);
		return EIO;
	}

	/* The frames. */
	done = fwrite(take->frames, LOOP_CHANNELS * sizeof(int16_t), take->count, file);
	if (done != take->count) {
		fclose(file);
		return EIO;
	}

	/* Closed, its writes finished. */
	result = fclose(file);
	if (result != 0)
		return EIO;

	/* Succeeded: saved. */
	return 0;
}

/*
 * Measures the quiet (from 0.2 s to the tone's start) and the tone (from
 * 0.4 s after its start, past the playing's delay, to its length) and
 * prints the result line.  Returns 0 on PASS, 1 on FAIL or SILENT.
 */
static int
loop_judge(
	const struct loop_take *take,
	size_t tone_start,
	const struct loop_options *options,
	long elapsed,
	unsigned overruns,
	unsigned underruns)
{
	const char *result;
	double *mono;
	double noise_rms;
	double noise_tone;
	double tone;
	double tone_rms;
	double peak;
	double margin;
	size_t room;
	size_t noise_from;
	size_t tone_from;
	size_t tone_to;
	int same;

	/* The take in one channel (room for one sample at least). */
	room = take->count;
	if (room == 0U)
		room = 1U;
	mono = calloc(room, sizeof(double));
	if (mono == NULL) {
		fprintf(stderr, "audio-loop: no memory to measure\n");
		return 1;
	}

	/* Mixed. */
	loop_mono(take->frames, take->count, LOOP_CHANNELS, mono);

	/* The windows. */
	noise_from = options->rate / 5U;
	tone_from = tone_start + 2U * options->rate / 5U;
	tone_to = tone_start + (size_t)(options->seconds * (double)options->rate);
	if (tone_to > take->count)
		tone_to = take->count;
	if (tone_from > tone_to)
		tone_from = tone_to;
	if (noise_from > tone_start)
		noise_from = tone_start;

	/* The levels. */
	noise_rms = loop_rms_level(mono + noise_from, tone_start - noise_from);
	noise_tone = loop_tone_level(mono + noise_from, tone_start - noise_from, options->frequency, options->rate);
	tone = loop_tone_level(mono + tone_from, tone_to - tone_from, options->frequency, options->rate);
	tone_rms = loop_rms_level(mono + tone_from, tone_to - tone_from);
	peak = loop_peak(mono, take->count);
	margin = tone - noise_rms;
	free(mono);

	/* The verdict: nothing recorded at all, the tone well above the noise, or not. */
	if (peak == 0.0) {
		result = "SILENT";
	} else if (margin >= options->margin) {
		result = "PASS";
	} else {
		result = "FAIL";
	}

	/* The line. */
	printf("AUDIOLOOP rate=%u frames=%zu expected=%zu ms=%ld overruns=%u underruns=%u "
	    "noise_rms=%.1f noise_tone=%.1f tone=%.1f tone_rms=%.1f peak=%.3f margin=%.1f result=%s\n",
	    options->rate, take->count, take->capacity, elapsed, overruns, underruns,
	    noise_rms, noise_tone, tone, tone_rms, peak, margin, result);

	/* Only a pass is a pass. */
	same = strcmp(result, "PASS");
	if (same != 0)
		return 1;

	/* Succeeded: passed. */
	return 0;
}

/* Measures the milliseconds since start (CLOCK_MONOTONIC). */
static long
loop_ms_since(
	const struct timespec *start)
{
	struct timespec now;

	/* Now, less the start. */
	clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: the milliseconds. */
	return (long)(now.tv_sec - start->tv_sec) * 1000L +
	    (long)(now.tv_nsec - start->tv_nsec) / 1000000L;
}
