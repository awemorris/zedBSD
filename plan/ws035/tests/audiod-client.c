/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Guest test client for audiod (ws035-p009), speaking the protocol of
 * userland/base/audiod/protocol.h directly.
 *
 *   audiod-client info
 *   audiod-client play FRAMES [RATE [CHANNELS]]   counting pattern, S16
 *   audiod-client constant FRAMES VALUE [VOLUME]  one stream of a constant
 *   audiod-client sum FRAMES A B                  two streams at once
 *   audiod-client capture FRAMES                  reads FRAMES, reports time
 *   audiod-client shrink FRAMES                   shrinks a playing stream's
 *                                                 memory, then plays FRAMES on
 *                                                 a new connection (audiod lives)
 *
 * Each mode prints one "AUDIODTEST ..." line.
 */

#include "userland/base/audiod/protocol.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct connection {
	int fd;
	uint32_t serial;
};

struct stream {
	struct connection *connection;
	int fd;
	uint32_t id;
	struct audiod_shm_header *shm;
	uint8_t *ring;
};

static int
fail(const char *what)
{
	printf("AUDIODTEST FAIL %s errno=%d\n", what, errno);
	exit(1);
}

static long
elapsed_ms(const struct timespec *start)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - start->tv_sec) * 1000L +
	    (now.tv_nsec - start->tv_nsec) / 1000000L;
}

/*
 * Receives one message, and the descriptor that came with it if any.  The
 * header is read first with the control buffer, which is where a passed
 * descriptor arrives (peeking at such data is refused), then the rest.
 */
static int
receive(struct connection *c, void *buffer, int *fd)
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

	header = buffer;
	if (fd != NULL)
		*fd = -1;
	have = 0;
	while (have < sizeof(*header)) {
		memset(&message, 0, sizeof(message));
		vector.iov_base = (uint8_t *)buffer + have;
		vector.iov_len = sizeof(*header) - have;
		message.msg_iov = &vector;
		message.msg_iovlen = 1;
		message.msg_control = control.space;
		message.msg_controllen = sizeof(control.space);
		got = recvmsg(c->fd, &message, 0);
		if (got <= 0)
			return -1;
		cmsg = CMSG_FIRSTHDR(&message);
		if (fd != NULL && cmsg != NULL && cmsg->cmsg_type == SCM_RIGHTS)
			memcpy(fd, CMSG_DATA(cmsg), sizeof(int));
		have += (size_t)got;
	}
	if (header->length < sizeof(*header) || header->length > AUDIOD_MESSAGE_MAX)
		return -1;
	while (have < header->length) {
		got = recv(c->fd, (uint8_t *)buffer + have, header->length - have, 0);
		if (got <= 0)
			return -1;
		have += (size_t)got;
	}
	return 0;
}

/* Sends a request and waits for its answer, skipping events. */
static int
request(struct connection *c, void *message, uint32_t length, void *reply, int *fd)
{
	struct audiod_header *header;
	uint8_t buffer[AUDIOD_MESSAGE_MAX];

	header = message;
	header->length = length;
	header->serial = ++c->serial;
	if (send(c->fd, message, length, 0) != (ssize_t)length)
		fail("send");
	for (;;) {
		if (receive(c, buffer, fd) != 0)
			fail("receive");
		if (((struct audiod_header *)buffer)->serial == c->serial &&
		    ((struct audiod_header *)buffer)->type != AUDIOD_REQUEST) {
			memcpy(reply, buffer, ((struct audiod_header *)buffer)->length);
			return ((struct audiod_header *)buffer)->type == AUDIOD_ERROR ?
			    (int)((struct audiod_result *)buffer)->error : 0;
		}
	}
}

static void
connect_audiod(struct connection *c)
{
	struct sockaddr_un address;
	struct audiod_hello hello;
	uint8_t reply[AUDIOD_MESSAGE_MAX];

	memset(c, 0, sizeof(*c));
	c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, AUDIOD_SOCKET_PATH);
	if (c->fd < 0 || connect(c->fd, (struct sockaddr *)&address, sizeof(address)) != 0)
		fail("connect");
	memset(&hello, 0, sizeof(hello));
	hello.header.type = AUDIOD_HELLO;
	hello.version = AUDIOD_VERSION;
	if (request(c, &hello, sizeof(hello), reply, NULL) != 0 ||
	    ((struct audiod_header *)reply)->type != AUDIOD_WELCOME)
		fail("hello");
}

static void
open_stream(struct connection *c, struct stream *s, uint32_t id, uint32_t direction,
    uint32_t rate, uint32_t channels)
{
	struct audiod_stream_create create;
	struct audiod_stream_created created;
	void *memory;
	int fd;

	memset(&create, 0, sizeof(create));
	create.header.type = AUDIOD_STREAM_CREATE;
	create.header.stream = id;
	create.direction = direction;
	create.format = AUDIOD_FORMAT_S16_LE;
	create.channels = channels;
	create.rate = rate;
	create.buffer_frames = 8192;
	create.period_frames = 1024;
	if (request(c, &create, sizeof(create), &created, &fd) != 0 || fd < 0)
		fail("create");
	memory = mmap(NULL, created.shm_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (memory == MAP_FAILED)
		fail("mmap");
	s->fd = fd;
	s->connection = c;
	s->id = id;
	s->shm = memory;
	s->ring = (uint8_t *)memory + AUDIOD_SHM_HEADER;
	if (s->shm->magic != AUDIOD_SHM_MAGIC)
		fail("magic");
}

static int
simple(struct stream *s, uint32_t type)
{
	struct audiod_header header;
	uint8_t reply[AUDIOD_MESSAGE_MAX];

	memset(&header, 0, sizeof(header));
	header.type = type;
	header.stream = s->id;
	return request(s->connection, &header, sizeof(header), reply, NULL);
}

static void
set_volume(struct stream *s, uint32_t volume)
{
	struct audiod_volume message;
	uint8_t reply[AUDIOD_MESSAGE_MAX];

	memset(&message, 0, sizeof(message));
	message.header.type = AUDIOD_STREAM_VOLUME;
	message.header.stream = s->id;
	message.left = volume;
	message.right = volume;
	if (request(s->connection, &message, sizeof(message), reply, NULL) != 0)
		fail("volume");
}

/* One sample for position n of a stream: the counting pattern or a constant. */
static int16_t
sample(uint64_t n, uint32_t channel, int constant, int value)
{
	if (constant)
		return (int16_t)value;
	return channel == 0 ? (int16_t)(uint16_t)n : (int16_t)(uint16_t)~n;
}

/* Writes into a playback ring as far as there is room, up to total frames. */
static uint64_t
fill(struct stream *s, uint64_t total, int constant, int value)
{
	uint64_t write_position;
	uint64_t read_position;
	uint32_t channel;
	int16_t *frame;

	write_position = s->shm->write_position;
	read_position = audiod_position_load(&s->shm->read_position, &s->shm->read_sequence);
	while (write_position < total &&
	    write_position - read_position < s->shm->capacity_frames) {
		frame = (int16_t *)(void *)(s->ring + (write_position % s->shm->capacity_frames) *
		    s->shm->frame_bytes);
		for (channel = 0; channel < s->shm->channels; channel++)
			frame[channel] = sample(write_position, channel, constant, value);
		write_position++;
	}
	audiod_position_store(&s->shm->write_position, &s->shm->write_sequence, write_position);
	return write_position;
}

/*
 * Plays total frames on each of count streams (each on its own connection),
 * refilling on REQUEST, then drains them all.
 */
static void
play(struct stream *streams, unsigned count, uint64_t total, int constant, const int *values)
{
	struct pollfd fds[2];
	struct timespec start;
	uint8_t buffer[AUDIOD_MESSAGE_MAX];
	unsigned index;
	unsigned done;
	uint32_t drained[2] = {0, 0};

	for (index = 0; index < count; index++) {
		fill(&streams[index], total, constant, values[index]);
		if (simple(&streams[index], AUDIOD_STREAM_START) != 0)
			fail("start");
	}
	clock_gettime(CLOCK_MONOTONIC, &start);

	/* Keeps each ring filled until its whole sound is written. */
	done = 0;
	while (done < count) {
		for (index = 0; index < count; index++) {
			fds[index].fd = streams[index].connection->fd;
			fds[index].events = POLLIN;
		}
		if (poll(fds, count, 2000) <= 0)
			fail("poll");
		done = 0;
		for (index = 0; index < count; index++) {
			if ((fds[index].revents & POLLIN) != 0 &&
			    receive(streams[index].connection, buffer, NULL) != 0)
				fail("event");
			if (fill(&streams[index], total, constant, values[index]) >= total)
				done++;
		}
	}

	/* Drains: each is answered with DRAINED once the device has played it. */
	for (index = 0; index < count; index++) {
		struct audiod_header header;

		memset(&header, 0, sizeof(header));
		header.type = AUDIOD_STREAM_DRAIN;
		header.stream = streams[index].id;
		header.length = sizeof(header);
		header.serial = ++streams[index].connection->serial;
		drained[index] = header.serial;
		if (send(streams[index].connection->fd, &header, sizeof(header), 0) !=
		    (ssize_t)sizeof(header))
			fail("drain");
	}
	for (index = 0; index < count; index++) {
		for (;;) {
			if (receive(streams[index].connection, buffer, NULL) != 0)
				fail("drained");
			if (((struct audiod_header *)buffer)->type == AUDIOD_DRAINED &&
			    ((struct audiod_header *)buffer)->serial == drained[index])
				break;
		}
	}
	printf("AUDIODTEST PLAY streams=%u frames=%llu ms=%ld underruns=%u\n",
	    count, (unsigned long long)total, elapsed_ms(&start), streams[0].shm->underruns);
}

static int
capture(uint64_t total)
{
	struct connection c;
	struct stream s;
	struct pollfd fd;
	struct timespec start;
	uint8_t buffer[AUDIOD_MESSAGE_MAX];
	uint64_t available;

	connect_audiod(&c);
	open_stream(&c, &s, 1, AUDIOD_CAPTURE, 48000, 2);
	if (simple(&s, AUDIOD_STREAM_START) != 0)
		fail("start");
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		available = audiod_position_load(&s.shm->write_position, &s.shm->write_sequence);
		/* Consumes everything recorded, like a reader would. */
		audiod_position_store(&s.shm->read_position, &s.shm->read_sequence, available);
		if (available >= total)
			break;
		fd.fd = c.fd;
		fd.events = POLLIN;
		if (poll(&fd, 1, 2000) <= 0)
			fail("poll");
		if (receive(&c, buffer, NULL) != 0)
			fail("event");
	}
	printf("AUDIODTEST CAPTURE frames=%llu ms=%ld overruns=%u\n",
	    (unsigned long long)available, elapsed_ms(&start), s.shm->overruns);
	return 0;
}

int
main(int argc, char **argv)
{
	struct connection connections[2];
	struct stream streams[2];
	int values[2] = {0, 0};
	uint64_t frames;

	if (argc >= 2 && strcmp(argv[1], "info") == 0) {
		connect_audiod(&connections[0]);
		printf("AUDIODTEST INFO connected\n");
		return 0;
	}
	if (argc < 3) {
		fprintf(stderr, "usage: audiod-client info|play|constant|sum|capture ...\n");
		return 2;
	}
	frames = strtoull(argv[2], NULL, 10);

	if (strcmp(argv[1], "play") == 0) {
		connect_audiod(&connections[0]);
		open_stream(&connections[0], &streams[0], 1, AUDIOD_PLAYBACK,
		    argc > 3 ? (uint32_t)atoi(argv[3]) : 48000,
		    argc > 4 ? (uint32_t)atoi(argv[4]) : 2);
		play(streams, 1, frames, 0, values);
		return 0;
	}
	if (strcmp(argv[1], "constant") == 0 && argc >= 4) {
		values[0] = atoi(argv[3]);
		connect_audiod(&connections[0]);
		open_stream(&connections[0], &streams[0], 1, AUDIOD_PLAYBACK, 48000, 2);
		if (argc >= 5)
			set_volume(&streams[0], (uint32_t)atoi(argv[4]));
		play(streams, 1, frames, 1, values);
		return 0;
	}
	if (strcmp(argv[1], "sum") == 0 && argc >= 5) {
		values[0] = atoi(argv[3]);
		values[1] = atoi(argv[4]);
		connect_audiod(&connections[0]);
		connect_audiod(&connections[1]);
		open_stream(&connections[0], &streams[0], 1, AUDIOD_PLAYBACK, 48000, 2);
		open_stream(&connections[1], &streams[1], 1, AUDIOD_PLAYBACK, 48000, 2);
		play(streams, 2, frames, 1, values);
		return 0;
	}
	if (strcmp(argv[1], "capture") == 0)
		return capture(frames);
	if (strcmp(argv[1], "shrink") == 0) {
		/* A client that shrinks its memory must lose its stream, not audiod. */
		connect_audiod(&connections[0]);
		open_stream(&connections[0], &streams[0], 1, AUDIOD_PLAYBACK, 48000, 2);
		fill(&streams[0], 8192, 1, 1000);
		if (simple(&streams[0], AUDIOD_STREAM_START) != 0)
			fail("start");
		if (ftruncate(streams[0].fd, 0) != 0)
			fail("ftruncate");
		usleep(500000);
		connect_audiod(&connections[1]);
		open_stream(&connections[1], &streams[1], 1, AUDIOD_PLAYBACK, 48000, 2);
		play(&streams[1], 1, frames, 0, values);
		printf("AUDIODTEST SHRINK survived\n");
		return 0;
	}
	fprintf(stderr, "audiod-client: unknown mode\n");
	return 2;
}
