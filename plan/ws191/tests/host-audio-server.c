/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws191-p002: the server half of the host test of the sound's playback
 * streams.  A stand-in of the compositor's event loop on a Unix socket
 * (wl_display's sync and get_registry, the registry's bind, then
 * userland/desktop/wayland/audio-stream.c's requests and its tick each
 * pass), and a stand-in backend the test drives: rings in memfds, the
 * controls answered at the next pass, and ready, failed, underrun, drained
 * and lost made on command.  The backend plays like audiod: the read
 * position moves only while running, a flush moves it to the write
 * position, and the played position is written only while playing.
 */

#define _GNU_SOURCE 1

#include "userland/desktop/wayland/kwl.h"
#include "userland/desktop/libkeiland/audio/kl-audio-protocol.h"
#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include "host-audio.h"

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

/* The connections the stand-in serves at once. */
#define SERVER_CLIENTS		16U

/* One backend stream of the stand-in. */
struct kl_backend_audio_stream {
	struct kl_backend_audio_stream_format format;
	int fd;
	struct kl_backend_audio_ring *ring;
	size_t bytes;
	unsigned capacity;
	unsigned ready_told;
	unsigned running;
	unsigned draining;
	uint32_t drain_request;
	unsigned ended;
	/* The answers to give at the next look. */
	struct kl_backend_audio_stream_report reports[8];
	unsigned report_count;
	struct kl_backend_audio_stream *next;
};

/* One client connection, with the bytes read and not handled yet. */
struct server_client {
	struct kwl_client client;
	unsigned char input[65536];
	size_t filled;
	uint32_t registry;
};

/* The test's world: guarded by host_audio.lock. */
struct host_audio_world host_audio = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* The stand-in's compositor and its clients (the server thread's). */
static struct kwl_server server;
static struct server_client clients[SERVER_CLIENTS];
static struct kl_backend_audio_stream *streams;
static int listener = -1;
static pthread_t server_thread;

static void *serve(void *argument);
static void serve_client(struct server_client *client);
static void serve_message(struct server_client *client, uint32_t id, uint32_t opcode, const unsigned char *bytes, size_t size);
static void serve_commands(void);
static void emit_raw(int fd, uint32_t object, uint32_t opcode, const void *payload, size_t size, int descriptor);
static void stream_report(struct kl_backend_audio_stream *stream, unsigned what, unsigned error, uint32_t request, uint32_t count);
static uint32_t word_at(const unsigned char *bytes, size_t offset);

/* ------------------------------------------------- the compositor's services */

uint64_t
kwl_milliseconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

int
kwl_emit(struct kwl_client *client, uint32_t object, uint32_t opcode, const void *payload, size_t size)
{
	emit_raw(client->fd, object, opcode, payload, size, -1);
	return 0;
}

int
kwl_emit_fd(struct kwl_client *client, uint32_t object, uint32_t opcode, const void *payload, size_t size, int descriptor)
{
	emit_raw(client->fd, object, opcode, payload, size, descriptor);
	if (descriptor >= 0)
		close(descriptor);
	return 0;
}

struct kwl_object *
kwl_find(struct kwl_client *client, uint32_t id)
{
	struct kwl_object *object;

	for (object = client->objects; object != NULL; object = object->next) {
		if (object->id == id && !object->dead)
			return object;
	}
	return NULL;
}

struct kwl_object *
kwl_create(struct kwl_client *client, uint32_t id, enum kwl_kind kind, uint32_t version)
{
	struct kwl_object *object;

	if (id == 0U || kwl_find(client, id) != NULL)
		return NULL;
	object = calloc(1, sizeof(*object));
	if (object == NULL)
		return NULL;
	object->client = client;
	object->id = id;
	object->kind = kind;
	object->version = version;
	object->next = client->objects;
	client->objects = object;
	return object;
}

void
kwl_object_destroy(struct kwl_object *object)
{
	uint32_t id;

	if (object->dead)
		return;
	if (object->kind == KWL_AUDIO_STREAM)
		kwl_audio_object_gone(object);
	object->dead = 1U;
	id = object->id;
	if (object->client->fd >= 0)
		emit_raw(object->client->fd, 1U, 1U, &id, sizeof(id), -1);
}

/* ------------------------------------------------- the stand-in backend */

int
kl_backend_audio_stream_supported(void)
{
	return 1;
}

struct kl_backend_audio_stream *
kl_backend_audio_stream_open(const struct kl_backend_audio_stream_format *format)
{
	struct kl_backend_audio_stream *stream;
	unsigned frame_bytes;
	unsigned fail;

	pthread_mutex_lock(&host_audio.lock);
	host_audio.opens++;
	fail = host_audio.fail_next_open;
	host_audio.fail_next_open = 0U;
	pthread_mutex_unlock(&host_audio.lock);

	stream = calloc(1, sizeof(*stream));
	if (stream == NULL)
		return NULL;
	stream->format = *format;
	stream->fd = -1;
	if (fail != 0U) {
		stream_report(stream, KL_BACKEND_AUDIO_FAILED, fail, 0U, 0U);
		stream->ended = 1U;
	} else {
		frame_bytes = 4U * format->channels;
		if (format->format == KL_BACKEND_AUDIO_FORMAT_S16_LE)
			frame_bytes = 2U * format->channels;
		stream->capacity = format->buffer_frames != 0U ? format->buffer_frames : 4800U;
		stream->bytes = KL_BACKEND_AUDIO_RING_HEADER + (((size_t)stream->capacity * frame_bytes + 4095U) & ~(size_t)4095U);
		stream->fd = memfd_create("host-audio", MFD_CLOEXEC);
		if (stream->fd < 0 || ftruncate(stream->fd, (off_t)stream->bytes) != 0) {
			free(stream);
			return NULL;
		}
		stream->ring = mmap(NULL, stream->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, stream->fd, 0);
		stream->ring->version = KL_BACKEND_AUDIO_RING_VERSION;
		stream->ring->format = format->format;
		stream->ring->channels = format->channels;
		stream->ring->rate = format->rate;
		stream->ring->frame_bytes = frame_bytes;
		stream->ring->capacity_frames = stream->capacity;
		stream->ring->period_frames = 480U;
		stream_report(stream, KL_BACKEND_AUDIO_READY, 0U, 0U, 0U);
	}
	stream->next = streams;
	streams = stream;
	return stream;
}

int
kl_backend_audio_stream_control(struct kl_backend_audio_stream *stream, unsigned what, uint32_t request)
{
	int refuse;

	if (stream->ended)
		return ENOTCONN;
	pthread_mutex_lock(&host_audio.lock);
	refuse = host_audio.refuse_next_control;
	host_audio.refuse_next_control = 0;
	host_audio.controls++;
	pthread_mutex_unlock(&host_audio.lock);
	if (refuse != 0)
		return refuse;

	switch (what) {
	case KL_BACKEND_AUDIO_START:
		stream->running = 1U;
		stream->draining = 0U;
		break;
	case KL_BACKEND_AUDIO_STOP:
		stream->running = 0U;
		stream->draining = 0U;
		break;
	case KL_BACKEND_AUDIO_FLUSH:
		/* As audiod: the read position takes the write position's; played is not touched. */
		__atomic_store_n(&stream->ring->read_position, __atomic_load_n(&stream->ring->write_position, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
		if (stream->draining) {
			stream->draining = 0U;
			stream->running = 0U;
		}
		break;
	case KL_BACKEND_AUDIO_DRAIN:
		stream->running = 1U;
		stream->draining = 1U;
		stream->drain_request = request;
		break;
	default:
		return EINVAL;
	}
	stream_report(stream, KL_BACKEND_AUDIO_RESULT, KL_BACKEND_AUDIO_ERROR_NONE, request, 0U);
	return 0;
}

int
kl_backend_audio_stream_next(struct kl_backend_audio_stream *stream, struct kl_backend_audio_stream_report *report)
{
	if (stream->report_count == 0U)
		return 0;
	*report = stream->reports[0];
	stream->report_count--;
	memmove(stream->reports, stream->reports + 1, stream->report_count * sizeof(stream->reports[0]));
	if (report->what == KL_BACKEND_AUDIO_READY) {
		report->fd = dup(stream->fd);
		report->bytes = (uint32_t)stream->bytes;
		report->capacity_frames = stream->capacity;
		report->period_frames = 480U;
	}
	return 1;
}

void
kl_backend_audio_stream_close(struct kl_backend_audio_stream *stream)
{
	struct kl_backend_audio_stream **link;

	pthread_mutex_lock(&host_audio.lock);
	host_audio.closes++;
	pthread_mutex_unlock(&host_audio.lock);
	for (link = &streams; *link != NULL; link = &(*link)->next) {
		if (*link == stream) {
			*link = stream->next;
			break;
		}
	}
	if (stream->ring != NULL)
		munmap(stream->ring, stream->bytes);
	if (stream->fd >= 0)
		close(stream->fd);
	free(stream);
}

void
kl_backend_audio_stream_reap(void)
{
}

void
kl_backend_audio_stream_reap_all(void)
{
}

/* ------------------------------------------------- the test's controls */

/* Starts the stand-in on a socket at path. */
void
host_audio_server_start(const char *path)
{
	struct sockaddr_un address;

	listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
	unlink(path);
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(listener, 16) != 0) {
		perror("host-audio: listen");
		exit(2);
	}
	pthread_create(&server_thread, NULL, serve, NULL);
}

/* Stops the stand-in. */
void
host_audio_server_stop(void)
{
	pthread_mutex_lock(&host_audio.lock);
	host_audio.stop = 1U;
	pthread_mutex_unlock(&host_audio.lock);
	pthread_join(server_thread, NULL);
}

/* ------------------------------------------------- the server thread */

static void *
serve(void *argument)
{
	struct pollfd descriptors[1U + SERVER_CLIENTS];
	unsigned index;
	unsigned count;
	unsigned stop;
	int accepted;

	(void)argument;
	for (;;) {
		pthread_mutex_lock(&host_audio.lock);
		stop = host_audio.stop;
		pthread_mutex_unlock(&host_audio.lock);
		if (stop)
			break;

		descriptors[0].fd = listener;
		descriptors[0].events = POLLIN;
		descriptors[0].revents = 0;
		count = 1U;
		for (index = 0U; index < SERVER_CLIENTS; index++) {
			descriptors[count].fd = clients[index].client.fd > 0 ? clients[index].client.fd : -1;
			descriptors[count].events = POLLIN;
			descriptors[count].revents = 0;
			count++;
		}
		(void)poll(descriptors, count, 5);

		/* A new connection takes a free slot. */
		if ((descriptors[0].revents & POLLIN) != 0) {
			accepted = accept(listener, NULL, NULL);
			for (index = 0U; index < SERVER_CLIENTS && accepted >= 0; index++) {
				if (clients[index].client.fd <= 0) {
					memset(&clients[index], 0, sizeof(clients[index]));
					clients[index].client.fd = accepted;
					clients[index].client.server = &server;
					clients[index].client.number = index + 1U;
					accepted = -1;
				}
			}
			if (accepted >= 0)
				close(accepted);
		}

		/* Each connection's requests. */
		for (index = 0U; index < SERVER_CLIENTS; index++) {
			if (descriptors[1U + index].fd > 0 && (descriptors[1U + index].revents & (POLLIN | POLLHUP)) != 0)
				serve_client(&clients[index]);
		}

		/* The test's commands to the backend, then the tick. */
		serve_commands();
		kwl_audio_tick(&server);
	}
	return NULL;
}

/* Reads a connection's bytes and carries out each whole message; its end destroys its objects. */
static void
serve_client(struct server_client *client)
{
	struct kwl_object *object;
	ssize_t got;
	size_t at;
	uint32_t size;
	int fd;

	got = read(client->client.fd, client->input + client->filled, sizeof(client->input) - client->filled);
	if (got <= 0) {
		/* The connection's end: its objects go, as kwl_client_destroy does, telling nobody. */
		fd = client->client.fd;
		client->client.fd = -1;
		client->client.fatal = 1U;
		for (object = client->client.objects; object != NULL; object = object->next)
			kwl_object_destroy(object);
		while (client->client.objects != NULL) {
			object = client->client.objects;
			client->client.objects = object->next;
			free(object);
		}
		close(fd);
		client->client.fd = 0;
		return;
	}
	client->filled += (size_t)got;
	at = 0U;
	while (client->filled - at >= 8U) {
		size = word_at(client->input, at + 4U) >> 16;
		if (size < 8U || client->filled - at < size)
			break;
		serve_message(client, word_at(client->input, at), word_at(client->input, at + 4U) & 0xffffU, client->input + at + 8U, size - 8U);
		at += size;
	}
	memmove(client->input, client->input + at, client->filled - at);
	client->filled -= at;
}

/* Carries out one request, as the compositor's dispatch would. */
static void
serve_message(struct server_client *client, uint32_t id, uint32_t opcode, const unsigned char *bytes, size_t size)
{
	unsigned char payload[64];
	struct kwl_object *object;
	uint32_t word;
	uint32_t length;
	size_t offset;
	int error;

	/* wl_display: sync (done, then delete_id), get_registry (kl_audio_v1 announced). */
	if (id == 1U) {
		if (opcode == 0U) {
			word = 7U;
			emit_raw(client->client.fd, word_at(bytes, 0), 0U, &word, 4, -1);
			word = word_at(bytes, 0);
			emit_raw(client->client.fd, 1U, 1U, &word, 4, -1);
		} else {
			client->registry = word_at(bytes, 0);
			word = 29U;
			memcpy(payload, &word, 4);
			length = (uint32_t)strlen(KL_AUDIO_NAME) + 1U;
			memcpy(payload + 4, &length, 4);
			memset(payload + 8, 0, 16);
			memcpy(payload + 8, KL_AUDIO_NAME, length);
			offset = 8U + ((length + 3U) & ~3U);
			word = KL_AUDIO_VERSION;
			memcpy(payload + offset, &word, 4);
			emit_raw(client->client.fd, client->registry, 0U, payload, offset + 4U, -1);
		}
		return;
	}

	/* wl_registry.bind(name, interface, version, new_id). */
	if (id == client->registry) {
		length = word_at(bytes, 4);
		offset = 8U + ((length + 3U) & ~3U);
		object = kwl_create(&client->client, word_at(bytes, offset + 4U), KWL_AUDIO, word_at(bytes, offset));
		if (object == NULL) {
			pthread_mutex_lock(&host_audio.lock);
			host_audio.protocol_errors++;
			pthread_mutex_unlock(&host_audio.lock);
		}
		return;
	}

	/* kl_audio_v1 and its streams. */
	object = kwl_find(&client->client, id);
	if (object == NULL)
		return;
	error = kwl_audio_request(object, opcode, bytes, size);
	if (error != 0) {
		printf("FAIL serve: object %u opcode %u error %d\n", id, opcode, error);
		pthread_mutex_lock(&host_audio.lock);
		host_audio.protocol_errors++;
		pthread_mutex_unlock(&host_audio.lock);
	}
}

/* Carries out the test's commands to every backend stream: play frames, underrun, drained, lost. */
static void
serve_commands(void)
{
	struct kl_backend_audio_stream *stream;
	uint64_t read;
	uint64_t written;
	uint64_t frames;
	uint32_t sequence;
	unsigned underrun;
	unsigned lose;
	unsigned drained;

	pthread_mutex_lock(&host_audio.lock);
	frames = host_audio.play_frames;
	host_audio.play_frames = 0U;
	underrun = host_audio.underrun;
	host_audio.underrun = 0U;
	lose = host_audio.lose;
	host_audio.lose = 0U;
	drained = host_audio.drained;
	host_audio.drained = 0U;
	pthread_mutex_unlock(&host_audio.lock);

	for (stream = streams; stream != NULL; stream = stream->next) {
		if (stream->ended || stream->ring == NULL)
			continue;

		/* Played as audiod plays: only while running; played follows read, under its sequence. */
		if (frames != 0U && stream->running) {
			read = __atomic_load_n(&stream->ring->read_position, __ATOMIC_ACQUIRE);
			written = __atomic_load_n(&stream->ring->write_position, __ATOMIC_ACQUIRE);
			read += frames;
			if (read > written)
				read = written;
			__atomic_store_n(&stream->ring->read_position, read, __ATOMIC_RELEASE);
			sequence = stream->ring->played_sequence;
			__atomic_store_n(&stream->ring->played_sequence, sequence + 1U, __ATOMIC_RELAXED);
			__atomic_thread_fence(__ATOMIC_RELEASE);
			stream->ring->played_position = read;
			stream->ring->played_time_ns = 1;
			__atomic_store_n(&stream->ring->played_sequence, sequence + 2U, __ATOMIC_RELEASE);
		}
		if (underrun != 0U)
			stream_report(stream, KL_BACKEND_AUDIO_UNDERRUN, 0U, 0U, underrun);
		if (drained != 0U && stream->draining) {
			stream->draining = 0U;
			stream->running = 0U;
			stream_report(stream, KL_BACKEND_AUDIO_DRAINED, 0U, stream->drain_request, 0U);
		}
		if (lose != 0U) {
			stream->ended = 1U;
			stream_report(stream, KL_BACKEND_AUDIO_LOST, KL_BACKEND_AUDIO_ERROR_GONE, 0U, 0U);
		}
	}
}

/* Sends one event, with a descriptor when there is one. */
static void
emit_raw(int fd, uint32_t object, uint32_t opcode, const void *payload, size_t size, int descriptor)
{
	union {
		struct cmsghdr header;
		unsigned char bytes[CMSG_SPACE(sizeof(int))];
	} control;
	unsigned char message[128];
	struct cmsghdr *item;
	struct msghdr header;
	struct iovec vector;
	uint32_t words[2];
	ssize_t written;

	words[0] = object;
	words[1] = (uint32_t)((size + 8U) << 16) | opcode;
	memcpy(message, words, sizeof(words));
	if (size > 0U)
		memcpy(message + 8, payload, size);
	memset(&header, 0, sizeof(header));
	vector.iov_base = message;
	vector.iov_len = size + 8U;
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
	/* A client that already went (it closes right after its destroy requests) is not a failure. */
	written = sendmsg(fd, &header, MSG_NOSIGNAL);
	if (written != (ssize_t)(size + 8U) && errno != EPIPE && errno != ECONNRESET)
		printf("FAIL emit_raw: sendmsg %zd errno %d\n", written, errno);
}

/* Keeps a report for the next look. */
static void
stream_report(struct kl_backend_audio_stream *stream, unsigned what, unsigned error, uint32_t request, uint32_t count)
{
	struct kl_backend_audio_stream_report *report;

	if (stream->report_count == 8U)
		return;
	report = &stream->reports[stream->report_count++];
	memset(report, 0, sizeof(*report));
	report->what = what;
	report->error = error;
	report->request = request;
	report->count = count;
	report->fd = -1;
}

static uint32_t
word_at(const unsigned char *bytes, size_t offset)
{
	uint32_t word;

	memcpy(&word, bytes + offset, 4);
	return word;
}
