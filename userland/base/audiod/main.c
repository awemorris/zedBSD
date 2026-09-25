/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * audiod: mixes the sound of its clients and hands out what the device
 * records (plan/ws035/audiod-design.md).
 *
 * One thread, one poll loop: the listening socket, each client, and the
 * sound device.  Sound is exchanged in shared memory; the socket carries
 * requests, their answers and a few events.
 */

#include "userland/base/audiod/audiod.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define AUDIOD_POLL_MAX		66U
#define AUDIOD_CLIENT_MAX	64U

struct audiod_client *audiod_clients;
struct audiod_device audiod_device;
sigjmp_buf audiod_bus_jump;
volatile int audiod_bus_armed;

static int listener = -1;
static unsigned client_count;
static unsigned shm_counter;

static void bus_error(int signal_number);
static int listen_socket(void);
static void accept_client(void);
static void read_client(struct audiod_client *client);
static void handle_message(struct audiod_client *client, const uint8_t *bytes, uint32_t length);
static void send_message(struct audiod_client *client, void *message, uint32_t length, int fd);
static void send_result(struct audiod_client *client, const struct audiod_header *request, int error);
static struct audiod_stream *find_stream(struct audiod_client *client, uint32_t id);
static int create_stream(struct audiod_client *client, const struct audiod_stream_create *request, int *fd);
static void destroy_stream(struct audiod_client *client, struct audiod_stream *stream);
static void drop_client(struct audiod_client *client);
static void reap_clients(void);
static void broadcast_volume(void);
static void send_volume(struct audiod_client *client);

/* Reports the monotonic time in nanoseconds. */
int64_t
audiod_now_ns(
	void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

/* Sends an event about a stream to its client. */
void
audiod_send_event(
	struct audiod_stream *stream,
	uint32_t type,
	uint32_t count)
{
	struct audiod_event event;

	memset(&event, 0, sizeof(event));
	event.header.type = type;
	event.header.length = sizeof(event);
	event.header.stream = stream->id;
	event.header.serial = type == AUDIOD_DRAINED ? count : 0U;
	event.count = count;
	send_message(stream->client, &event, sizeof(event), -1);
}

int
main(
	void)
{
	struct pollfd fds[AUDIOD_POLL_MAX];
	struct audiod_client *clients[AUDIOD_POLL_MAX];
	struct audiod_client *client;
	struct sigaction action;
	unsigned count;
	unsigned index;
	short events;
	int device_slot;
	int device_fd;

	/* A client that goes away must not end audiod with SIGPIPE. */
	signal(SIGPIPE, SIG_IGN);

	/* A client that shrinks its shared memory faults the stream, not audiod. */
	memset(&action, 0, sizeof(action));
	action.sa_handler = bus_error;
	sigemptyset(&action.sa_mask);
	sigaction(SIGBUS, &action, NULL);

	if (audiod_device_open(&audiod_device) != 0) {
		fprintf(stderr, "audiod: cannot allocate the device buffers\n");
		return 1;
	}
	if (listen_socket() != 0)
		return 1;
	fprintf(stderr, "audiod: %s, %u Hz, %u frames a period, %s\n",
	    audiod_device.dsp >= 0 ? "/dev/dsp0" : "no device",
	    audiod_device.rate, audiod_device.period_frames,
	    audiod_device.map != NULL ? "mmap" : "write");

	for (;;) {
		/* Waits on the listener, the device and every client. */
		count = 0;
		fds[count].fd = listener;
		fds[count].events = POLLIN;
		clients[count++] = NULL;
		device_slot = -1;
		device_fd = audiod_device_fd(&audiod_device, &events);
		if (device_fd >= 0) {
			device_slot = (int)count;
			fds[count].fd = device_fd;
			fds[count].events = events;
			clients[count++] = NULL;
		}
		for (client = audiod_clients; client != NULL && count < AUDIOD_POLL_MAX;
		     client = client->next) {
			fds[count].fd = client->fd;
			fds[count].events = POLLIN;
			clients[count++] = client;
		}
		for (index = 0; index < count; index++)
			fds[index].revents = 0;

		if (poll(fds, count, audiod_device_timeout_ms(&audiod_device)) < 0 &&
		    errno != EINTR)
			return 1;

		/* The device first, so that a client's new request sees current positions. */
		audiod_device_service(&audiod_device,
		    device_slot >= 0 ? fds[device_slot].revents : 0);
		if ((fds[0].revents & POLLIN) != 0)
			accept_client();
		for (index = 0; index < count; index++) {
			if (clients[index] != NULL && fds[index].revents != 0)
				read_client(clients[index]);
		}
		reap_clients();
	}
}

/* Leaves the stream being touched when its shared memory faults. */
static void
bus_error(
	int signal_number)
{
	(void)signal_number;
	if (audiod_bus_armed)
		siglongjmp(audiod_bus_jump, 1);
	signal(SIGBUS, SIG_DFL);
	raise(SIGBUS);
}

/* Listens on /run/audiod.sock, which everyone may use. */
static int
listen_socket(
	void)
{
	struct sockaddr_un address;

	listener = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listener < 0) {
		perror("audiod: socket");
		return -1;
	}
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, AUDIOD_SOCKET_PATH);
	unlink(AUDIOD_SOCKET_PATH);
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
	    listen(listener, 16) != 0) {
		perror("audiod: " AUDIOD_SOCKET_PATH);
		return -1;
	}
	(void)chmod(AUDIOD_SOCKET_PATH, 0666);
	(void)fcntl(listener, F_SETFL, O_NONBLOCK);
	return 0;
}

/* Accepts one client. */
static void
accept_client(
	void)
{
	struct audiod_client *client;
	int fd;

	fd = accept(listener, NULL, NULL);
	if (fd < 0)
		return;
	if (client_count >= AUDIOD_CLIENT_MAX) {
		close(fd);
		return;
	}
	client = calloc(1, sizeof(*client));
	if (client == NULL) {
		close(fd);
		return;
	}
	(void)fcntl(fd, F_SETFL, O_NONBLOCK);
	client->fd = fd;
	client->next = audiod_clients;
	audiod_clients = client;
	client_count++;
}

/* Reads what a client sent and handles each whole message. */
static void
read_client(
	struct audiod_client *client)
{
	struct audiod_header header;
	ssize_t count;
	size_t offset;

	count = recv(client->fd, client->input + client->input_used,
	    sizeof(client->input) - client->input_used, 0);
	if (count <= 0) {
		if (count == 0 || (errno != EAGAIN && errno != EINTR))
			client->dead = 1;
		return;
	}
	client->input_used += (size_t)count;

	/* A message that claims an impossible length ends the client. */
	offset = 0;
	while (client->input_used - offset >= sizeof(header)) {
		memcpy(&header, client->input + offset, sizeof(header));
		if (header.length < sizeof(header) ||
		    header.length > AUDIOD_MESSAGE_MAX) {
			client->dead = 1;
			return;
		}
		if (client->input_used - offset < header.length)
			break;
		handle_message(client, client->input + offset, header.length);
		offset += header.length;
	}
	memmove(client->input, client->input + offset, client->input_used - offset);
	client->input_used -= offset;
}

/* Carries out one request. */
static void
handle_message(
	struct audiod_client *client,
	const uint8_t *bytes,
	uint32_t length)
{
	union {
		struct audiod_header header;
		struct audiod_hello hello;
		struct audiod_stream_create create;
		struct audiod_volume volume;
		struct audiod_subscribe subscribe;
	} request;
	struct audiod_welcome welcome;
	struct audiod_stream_created created;
	struct audiod_stream *stream;
	int error;
	int fd;

	memset(&request, 0, sizeof(request));
	memcpy(&request, bytes, length < sizeof(request) ? length : sizeof(request));
	stream = find_stream(client, request.header.stream);
	error = 0;

	switch (request.header.type) {
	case AUDIOD_HELLO:
		memset(&welcome, 0, sizeof(welcome));
		welcome.header.type = AUDIOD_WELCOME;
		welcome.header.length = sizeof(welcome);
		welcome.header.serial = request.header.serial;
		welcome.version = AUDIOD_VERSION;
		welcome.device = audiod_device.dsp >= 0;
		welcome.format = audiod_device.format;
		welcome.channels = audiod_device.channels;
		welcome.rate = audiod_device.rate;
		welcome.period_frames = audiod_device.period_frames;
		send_message(client, &welcome, sizeof(welcome), -1);
		return;

	case AUDIOD_STREAM_CREATE:
		if (length != sizeof(request.create)) {
			error = EINVAL;
			break;
		}
		if (stream != NULL) {
			error = EEXIST;
			break;
		}
		error = create_stream(client, &request.create, &fd);
		if (error != 0)
			break;
		stream = find_stream(client, request.header.stream);
		memset(&created, 0, sizeof(created));
		created.header.type = AUDIOD_STREAM_CREATED;
		created.header.length = sizeof(created);
		created.header.serial = request.header.serial;
		created.header.stream = request.header.stream;
		created.shm_bytes = (uint32_t)stream->shm_bytes;
		created.capacity_frames = stream->shm->capacity_frames;
		send_message(client, &created, sizeof(created), fd);
		close(fd);
		return;

	case AUDIOD_STREAM_START:
		if (stream == NULL) {
			error = ENOENT;
			break;
		}
		if (!stream->running) {
			audiod_stream_rates(stream, &audiod_device);
			stream->last_request = stream->direction == AUDIOD_PLAYBACK ?
			    stream->shm->read_position : stream->shm->write_position;
		}
		stream->running = 1;
		stream->draining = 0;
		stream->shm->state = AUDIOD_STATE_RUNNING;
		if (stream->direction == AUDIOD_CAPTURE)
			audiod_device_start_capture(&audiod_device);
		break;

	case AUDIOD_STREAM_STOP:
		if (stream == NULL) {
			error = ENOENT;
			break;
		}
		stream->running = 0;
		stream->draining = 0;
		stream->shm->state = AUDIOD_STATE_STOPPED;
		break;

	case AUDIOD_STREAM_DRAIN:
		if (stream == NULL || stream->direction != AUDIOD_PLAYBACK) {
			error = stream == NULL ? ENOENT : EINVAL;
			break;
		}

		/* Answered with DRAINED once the device has played everything. */
		if (!stream->running) {
			audiod_stream_rates(stream, &audiod_device);
			stream->running = 1;
		}
		stream->draining = 1;
		stream->drain_target = 0;
		stream->drain_serial = request.header.serial;
		stream->shm->state = AUDIOD_STATE_DRAINING;
		return;

	case AUDIOD_STREAM_FLUSH:
		if (stream == NULL) {
			error = ENOENT;
			break;
		}
		/*
		 * Playback drops what is queued.  The read position of a capture
		 * stream belongs to the client, which drops what it has not read
		 * by moving it; audiod only starts its conversion afresh.
		 */
		if (stream->direction == AUDIOD_PLAYBACK) {
			audiod_position_store(&stream->shm->read_position,
			    &stream->shm->read_sequence,
			    audiod_position_load(&stream->shm->write_position,
			    &stream->shm->write_sequence));
			stream->last_request = stream->shm->read_position;
		}
		audiod_stream_rates(stream, &audiod_device);
		break;

	case AUDIOD_STREAM_VOLUME:
		if (stream == NULL || length != sizeof(request.volume)) {
			error = stream == NULL ? ENOENT : EINVAL;
			break;
		}
		if (request.volume.left > AUDIOD_VOLUME_UNITY * 4U ||
		    request.volume.right > AUDIOD_VOLUME_UNITY * 4U) {
			error = EINVAL;
			break;
		}
		stream->volume_left = request.volume.left;
		stream->volume_right = request.volume.right;
		stream->muted = request.volume.muted != 0;
		break;

	case AUDIOD_STREAM_DESTROY:
		if (stream == NULL) {
			error = ENOENT;
			break;
		}
		destroy_stream(client, stream);
		break;

	case AUDIOD_DEVICE_VOLUME:
		if (length != sizeof(request.volume) ||
		    request.volume.left > 100U || request.volume.right > 100U) {
			error = EINVAL;
			break;
		}
		audiod_device_set_volume(&audiod_device, request.volume.left,
		    request.volume.right, request.volume.muted != 0);
		broadcast_volume();
		break;

	case AUDIOD_SUBSCRIBE:
		/* A new subscriber is told the volume as it stands. */
		client->subscribed = request.subscribe.mask;
		send_result(client, &request.header, 0);
		if (client->subscribed)
			send_volume(client);
		return;

	default:
		error = EINVAL;
		break;
	}

	send_result(client, &request.header, error);
}

/*
 * Creates a stream and its shared memory.  The memory object is unlinked
 * the moment it exists, so only this process and the client it is sent to
 * can reach it.
 */
static int
create_stream(
	struct audiod_client *client,
	const struct audiod_stream_create *request,
	int *fd)
{
	struct audiod_stream *stream;
	struct audiod_shm_header *shm;
	uint32_t frame_bytes;
	uint32_t capacity;
	size_t ring_bytes;
	size_t bytes;
	char name[64];
	void *memory;

	/* Refuses what audiod cannot convert. */
	if (request->direction > AUDIOD_CAPTURE ||
	    request->format < AUDIOD_FORMAT_S16_LE ||
	    request->format > AUDIOD_FORMAT_F32_LE ||
	    request->channels < 1U || request->channels > 2U ||
	    request->rate < 8000U || request->rate > 192000U)
		return EINVAL;
	capacity = request->buffer_frames != 0U ? request->buffer_frames :
	    4U * audiod_device.period_frames;
	if (capacity > AUDIOD_BUFFER_MAX || request->period_frames > capacity)
		return EINVAL;
	frame_bytes = audiod_frame_bytes(request->format, request->channels);
	ring_bytes = ((size_t)capacity * frame_bytes + 4095U) & ~(size_t)4095U;
	bytes = AUDIOD_SHM_HEADER + ring_bytes;

	/* Makes the anonymous shared memory. */
	snprintf(name, sizeof(name), "/audiod-%d-%u", (int)getpid(), shm_counter++);
	*fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (*fd < 0)
		return errno;
	(void)shm_unlink(name);
	if (ftruncate(*fd, (off_t)bytes) != 0) {
		close(*fd);
		return errno;
	}
	memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
	if (memory == MAP_FAILED) {
		close(*fd);
		return errno;
	}

	stream = calloc(1, sizeof(*stream));
	if (stream == NULL) {
		munmap(memory, bytes);
		close(*fd);
		return ENOMEM;
	}
	shm = memory;
	memset(shm, 0, sizeof(*shm));
	shm->magic = AUDIOD_SHM_MAGIC;
	shm->version = AUDIOD_VERSION;
	shm->format = request->format;
	shm->channels = request->channels;
	shm->rate = request->rate;
	shm->frame_bytes = frame_bytes;
	shm->capacity_frames = capacity;
	shm->period_frames = request->period_frames != 0U ? request->period_frames :
	    audiod_device.period_frames;
	shm->state = AUDIOD_STATE_STOPPED;

	stream->client = client;
	stream->id = request->header.stream;
	stream->direction = request->direction;
	stream->shm = shm;
	stream->ring = (uint8_t *)memory + AUDIOD_SHM_HEADER;
	stream->shm_bytes = bytes;
	stream->volume_left = AUDIOD_VOLUME_UNITY;
	stream->volume_right = AUDIOD_VOLUME_UNITY;
	audiod_stream_rates(stream, &audiod_device);
	stream->next = client->streams;
	client->streams = stream;
	return 0;
}

/* Unlinks and frees a stream; the memory goes when the client unmaps it too. */
static void
destroy_stream(
	struct audiod_client *client,
	struct audiod_stream *stream)
{
	struct audiod_stream **link;

	for (link = &client->streams; *link != NULL; link = &(*link)->next) {
		if (*link == stream) {
			*link = stream->next;
			break;
		}
	}
	munmap(stream->shm, stream->shm_bytes);
	free(stream);
}

/* Finds a client's stream by its id. */
static struct audiod_stream *
find_stream(
	struct audiod_client *client,
	uint32_t id)
{
	struct audiod_stream *stream;

	for (stream = client->streams; stream != NULL; stream = stream->next) {
		if (stream->id == id)
			return stream;
	}
	return NULL;
}

/* Answers a request with DONE, or ERROR and the error number. */
static void
send_result(
	struct audiod_client *client,
	const struct audiod_header *request,
	int error)
{
	struct audiod_result result;

	memset(&result, 0, sizeof(result));
	result.header.type = error != 0 ? AUDIOD_ERROR : AUDIOD_DONE;
	result.header.length = sizeof(result);
	result.header.serial = request->serial;
	result.header.stream = request->stream;
	result.error = (uint32_t)error;
	send_message(client, &result, sizeof(result), -1);
}

/*
 * Sends one message, with a descriptor when fd is not -1.  A client that
 * cannot take a message now is too slow to keep, and is dropped.
 */
static void
send_message(
	struct audiod_client *client,
	void *message,
	uint32_t length,
	int fd)
{
	union {
		struct cmsghdr header;
		char space[CMSG_SPACE(sizeof(int))];
	} control;
	struct msghdr header;
	struct iovec vector;
	struct cmsghdr *cmsg;

	if (client->dead)
		return;
	memset(&header, 0, sizeof(header));
	vector.iov_base = message;
	vector.iov_len = length;
	header.msg_iov = &vector;
	header.msg_iovlen = 1;
	if (fd >= 0) {
		memset(&control, 0, sizeof(control));
		header.msg_control = control.space;
		header.msg_controllen = sizeof(control.space);
		cmsg = CMSG_FIRSTHDR(&header);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
	}
	if (sendmsg(client->fd, &header, MSG_DONTWAIT) != (ssize_t)length)
		client->dead = 1;
}

/* Tells a client the device volume. */
static void
send_volume(
	struct audiod_client *client)
{
	struct audiod_volume volume;

	memset(&volume, 0, sizeof(volume));
	volume.header.type = AUDIOD_VOLUME_CHANGED;
	volume.header.length = sizeof(volume);
	audiod_device_get_volume(&audiod_device, &volume.left, &volume.right,
	    &volume.muted);
	send_message(client, &volume, sizeof(volume), -1);
}

/* Tells every subscribed client the device volume. */
static void
broadcast_volume(
	void)
{
	struct audiod_client *client;

	for (client = audiod_clients; client != NULL; client = client->next) {
		if (client->subscribed)
			send_volume(client);
	}
}

/* Frees a client and its streams. */
static void
drop_client(
	struct audiod_client *client)
{
	while (client->streams != NULL)
		destroy_stream(client, client->streams);
	close(client->fd);
	free(client);
	client_count--;
}

/* Frees every client that went away or fell too far behind. */
static void
reap_clients(
	void)
{
	struct audiod_client **link;
	struct audiod_client *client;

	link = &audiod_clients;
	while (*link != NULL) {
		client = *link;
		if (client->dead) {
			*link = client->next;
			drop_client(client);
		} else {
			link = &client->next;
		}
	}
}
