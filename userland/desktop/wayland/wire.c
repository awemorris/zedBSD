/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Wayland framing with independent stream-byte and SCM_RIGHTS ownership.
 */

#include "kwl.h"
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__riscv)
/* How long kwl_log_us measures the time counter's rate before using it, and the step its rate is rounded to (Hz). */
#define LOG_CALIBRATE_NS	2000000000ULL
#define LOG_RATE_STEP		100000ULL

/*
 * kwl_log_us's state: the first readings of the time counter and of the
 * monotonic clock, the counter's rate (Hz, 0 until measured) and its
 * offset in nanoseconds (INT64_MIN until the first fine reading).
 */
static uint64_t log_first_counter;
static uint64_t log_first_monotonic;
static uint64_t log_rate;
static int64_t log_offset = INT64_MIN;
#endif

static int receive_rights(struct kwl_client *client, const struct msghdr *message);
static int decode_messages(struct kwl_client *client);
static ssize_t send_packet(struct kwl_client *client, struct kwl_packet *packet);

/*
 * Reads a monotonic time for frame callbacks and finite service deadlines.
 */
uint64_t
kwl_milliseconds(
	void)
{
	struct timespec now;
	int error;

	/* Clock failure cannot turn a finite service lifetime into an endless loop. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return UINT64_MAX;

	/* Succeeded: callbacks use the wrapping low 32 bits of this clock. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/*
 * Reads the monotonic clock in microseconds, for the per-frame lines that
 * time the pen's input to the frame that shows it (ws099-p015).
 */
uint64_t
kwl_microseconds(
	void)
{
	struct timespec now;
	int error;

	/* A clock that fails reads as zero. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0;

	/* Succeeded: the time. */
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

/*
 * The per-frame lines' clock in microseconds: the monotonic clock, which
 * advances once a scheduler tick, made fine with the time counter where
 * user mode may read it (riscv64's rdtime), as libvulkan's VULKAN TIMING
 * is, so that the compositor's and the clients' lines can be set side by
 * side.  The counter's rate is measured against the monotonic clock over
 * the first LOG_CALIBRATE_NS and taken as the round rate within 0.1 % of
 * it, as timebases are (the tick makes the measure up to 0.05 % off, which
 * would otherwise drift the lines apart over a run); the offset is the
 * largest by which a monotonic reading was ahead of the counter.  Until
 * the rate is known, and elsewhere, the monotonic clock as it is.  Only a
 * diagnostic: relaxed atomics, a race only refines it later.
 */
uint64_t
kwl_log_us(
	void)
{
	struct timespec now;
	uint64_t monotonic;
#if defined(__riscv)
	uint64_t counter;
	uint64_t first_monotonic;
	uint64_t first_counter;
	uint64_t rate;
	uint64_t round;
	uint64_t scaled;
	int64_t offset;
	int64_t ahead;
#endif

	/* The monotonic clock; one that fails reads as zero. */
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	monotonic = (uint64_t)now.tv_sec * 1000000000U + (uint64_t)now.tv_nsec;
#if defined(__riscv)
	/* The counter now. */
	__asm__ volatile("rdtime %0" : "=r"(counter));

	/* The first reading starts the rate's measurement. */
	first_monotonic = __atomic_load_n(&log_first_monotonic, __ATOMIC_RELAXED);
	if (first_monotonic == 0) {
		__atomic_store_n(&log_first_counter, counter, __ATOMIC_RELAXED);
		__atomic_store_n(&log_first_monotonic, monotonic, __ATOMIC_RELAXED);
		return monotonic / 1000U;
	}

	/* The rate, once the span is long enough, rounded to the 100 kHz near it when that is within 0.1 %. */
	rate = __atomic_load_n(&log_rate, __ATOMIC_RELAXED);
	if (rate == 0) {
		first_counter = __atomic_load_n(&log_first_counter, __ATOMIC_RELAXED);
		if (monotonic - first_monotonic < LOG_CALIBRATE_NS || counter <= first_counter)
			return monotonic / 1000U;
		rate = (counter - first_counter) * 1000000000U / (monotonic - first_monotonic);
		round = (rate + LOG_RATE_STEP / 2U) / LOG_RATE_STEP * LOG_RATE_STEP;
		if (round != 0 && (rate > round ? rate - round : round - rate) * 1000U <= round)
			rate = round;
		if (rate == 0)
			return monotonic / 1000U;
		__atomic_store_n(&log_rate, rate, __ATOMIC_RELAXED);
	}

	/* The counter in nanoseconds, and the largest amount the monotonic clock was ahead of it. */
	scaled = (counter / rate) * 1000000000U + (counter % rate) * 1000000000U / rate;
	offset = __atomic_load_n(&log_offset, __ATOMIC_RELAXED);
	ahead = (int64_t)monotonic - (int64_t)scaled;
	if (ahead > offset) {
		__atomic_store_n(&log_offset, ahead, __ATOMIC_RELAXED);
		offset = ahead;
	}

	/* Succeeded: the counter's time on the monotonic clock's scale. */
	return (uint64_t)((int64_t)scaled + offset) / 1000U;
#else
	/* No counter user mode may read: the monotonic clock. */
	return monotonic / 1000U;
#endif
}

/*
 * Queues one aligned protocol event without blocking the other clients.
 */
int
kwl_emit(
	struct kwl_client *client,
	uint32_t object,
	uint32_t opcode,
	const void *payload,
	size_t size)
{
	int error;

	/* An ordinary event carries no descriptor. */
	error = kwl_emit_fd(client, object, opcode, payload, size, -1);
	if (error != 0)
		return error;

	/* Succeeded: the event is retained for nonblocking delivery. */
	return 0;
}

/*
 * Queues one protocol event whose descriptor argument travels as SCM_RIGHTS.
 *
 * The packet takes ownership of a descriptor that is not -1: it is closed
 * once sent, when the client is destroyed, or here when queuing fails.
 */
int
kwl_emit_fd(
	struct kwl_client *client,
	uint32_t object,
	uint32_t opcode,
	const void *payload,
	size_t size,
	int descriptor)
{
	struct kwl_packet *packet;
	uint32_t header[2];
	size_t total;

	/* Refuse malformed or excessive event storage before allocating a packet. */
	if (size > KWL_WIRE_MAX - 8U || (size & 3U) != 0) {
		if (descriptor >= 0)
			close(descriptor);

		/* Refuses the malformed event. */
		return EPROTO;
	}

	/* A client that never reads cannot consume unbounded compositor memory. */
	total = size + 8U;
	if (client->output_bytes > KWL_OUTPUT_MAX - total) {
		if (descriptor >= 0)
			close(descriptor);

		/* Refuses the event: the client reads too little. */
		return ENOBUFS;
	}

	/* Each packet owns its unsent suffix until flush or client destruction. */
	packet = calloc(1, sizeof(*packet) + total);
	if (packet == NULL) {
		if (descriptor >= 0)
			close(descriptor);

		/* Refuses the event: no memory for it. */
		return ENOMEM;
	}

	/* Wayland uses native-endian words on its local Unix stream. */
	packet->size = total;
	packet->descriptor = descriptor;
	header[0] = object;
	header[1] = (uint32_t)(total << 16) | opcode;
	memcpy(packet->bytes, header, sizeof(header));

	/* Empty events have no payload pointer to read. */
	if (size != 0)
		memcpy(packet->bytes + 8U, payload, size);

	/* Preserve event order across short writes and descriptor reuse. */
	if (client->output_tail != NULL)
		client->output_tail->next = packet;
	else
		client->output_head = packet;

	/* Queue accounting remains charged until the entire packet is sent. */
	client->output_tail = packet;
	client->output_bytes += total;

	/* Succeeded: the event is retained for nonblocking delivery. */
	return 0;
}

/*
 * Drains queued events while retaining every unsent suffix after EAGAIN.
 */
int
kwl_flush(
	struct kwl_client *client)
{
	struct kwl_packet *packet;
	ssize_t sent;

	/* A positive stream write may consume only part of a protocol message. */
	while (client->output_head != NULL) {
		/* Resume this packet at exactly the suffix the previous write left unsent. */
		packet = client->output_head;
		sent = send_packet(client, packet);
		if (sent < 0) {
			/* Retry an interrupted write without losing the queued suffix. */
			if (errno == EINTR)
				continue;

			/* Backpressure leaves this client runnable on the next writable event. */
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;

			/* Other socket failures terminate this connection. */
			return errno;
		}

		/* A zero-byte write cannot make progress on a nonempty event. */
		if (sent == 0)
			return EPIPE;

		/* A partial packet remains owned by the queue. */
		packet->sent += (size_t)sent;
		if (packet->sent != packet->size)
			continue;

		/* Complete packets release their memory charge and list ownership. */
		client->output_head = packet->next;
		client->output_bytes -= packet->size;
		kwl_packet_free(packet);
	}

	/* An empty queue has no last packet. */
	client->output_tail = NULL;

	/* Succeeded: all currently queued events were accepted by the socket. */
	return 0;
}

/*
 * Releases one queued event and any descriptor it has not yet sent.
 */
void
kwl_packet_free(
	struct kwl_packet *packet)
{
	/* An unsent descriptor still belongs to the packet. */
	if (packet->descriptor >= 0)
		close(packet->descriptor);

	/* The packet storage holds nothing else that needs releasing. */
	free(packet);

	/* Succeeded: the packet and its descriptor are gone. */
	return;
}

/*
 * Reads one available stream fragment and decodes every complete request.
 */
int
kwl_read(
	struct kwl_client *client)
{
	struct msghdr message;
	struct iovec vector;
	union {
		struct cmsghdr alignment;
		unsigned char bytes[CMSG_SPACE(8U * sizeof(int))];
	} control;
	ssize_t received;
	int error;

	/* A maximal incomplete request cannot accept another byte indefinitely. */
	if (client->input_size == KWL_WIRE_MAX) {
		error = kwl_error(client, 1, "request exceeds wire limit");
		return error;
	}

	/* Rights travel independently of stream-message boundaries. */
	memset(&message, 0, sizeof(message));
	memset(&control, 0, sizeof(control));
	vector.iov_base = client->input + client->input_size;
	vector.iov_len = KWL_WIRE_MAX - client->input_size;
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = control.bytes;
	message.msg_controllen = sizeof(control.bytes);
	received = recvmsg(client->fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
	if (received < 0) {
		/* Poll readiness may be consumed or interrupted before this read. */
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return 0;

		/* Other socket errors withdraw this client's objects. */
		return errno;
	}

	/* Capture or close received rights even when the payload later fails validation. */
	error = receive_rights(client, &message);
	if (error != 0)
		return error;

	/* The peer cannot complete any pending protocol state after end of stream. */
	if (received == 0)
		return EPIPE;

	/* Complete messages are consumed; a partial suffix remains for the next read. */
	client->input_size += (size_t)received;
	error = decode_messages(client);
	if (error != 0)
		return error;

	/* Succeeded: all newly complete requests have been dispatched. */
	return 0;
}

/*
 * Records a protocol error and queues the standard wl_display.error event.
 */
int
kwl_error(
	struct kwl_client *client,
	uint32_t object,
	const char *reason)
{
	unsigned char payload[268];
	uint32_t word;
	size_t bytes;
	int error;

	/* Only the first defect owns this connection's final protocol error. */
	if (client->fatal)
		return EPROTO;

	/* A display error names the surviving display object even for an invalid request ID. */
	memset(payload, 0, sizeof(payload));
	word = 1;
	memcpy(payload, &word, 4);
	memcpy(payload + 4, &word, 4);
	bytes = strlen(reason);
	if (bytes > 255U)
		bytes = 255U;

	/* Strings include one terminator and have aligned wire storage. */
	word = (uint32_t)bytes + 1U;
	memcpy(payload + 8, &word, 4);
	memcpy(payload + 12, reason, bytes);
	error = kwl_emit(client, 1, 0, payload, 12U + ((bytes + 4U) & ~(size_t)3U));
	kwl_client_fail(client);
	printf("KWL ERROR client=%llu object=%u reason=%s emit=%d\n", (unsigned long long)client->number, object, reason, error);

	/* Succeeded: EPROTO directs the loop to flush this terminal error before disconnect. */
	return EPROTO;
}

/*
 * Queues wl_display.error naming the object at fault and its interface's error code.
 *
 * kwl_error names the display with code 1 because the request it reports may
 * have named no live object; a request that failed on a live object of an
 * interface with its own error enum uses this instead, so that the client
 * learns which object and which error.  Returns EPROTO, like kwl_error.
 */
int
kwl_error_code(
	struct kwl_client *client,
	uint32_t object,
	uint32_t code,
	const char *reason)
{
	unsigned char payload[268];
	uint32_t word;
	size_t bytes;
	int error;

	/* Only the first defect owns the connection's final protocol error. */
	if (client->fatal)
		return EPROTO;

	/* The object at fault and the code of its interface's error. */
	memset(payload, 0, sizeof(payload));
	memcpy(payload, &object, 4);
	memcpy(payload + 4, &code, 4);

	/* The message, cut to 255 bytes, with its terminator and aligned storage. */
	bytes = strlen(reason);
	if (bytes > 255U)
		bytes = 255U;
	word = (uint32_t)bytes + 1U;
	memcpy(payload + 8, &word, 4);
	memcpy(payload + 12, reason, bytes);

	/* The event goes out before the connection is closed; nothing more is taken from it. */
	error = kwl_emit(client, 1, 0, payload, 12U + ((bytes + 4U) & ~(size_t)3U));
	kwl_client_fail(client);
	printf("KWL ERROR client=%llu object=%u code=%u reason=%s emit=%d\n", (unsigned long long)client->number, object, code, reason, error);

	/* Succeeded: EPROTO makes the loop flush the error before it disconnects. */
	return EPROTO;
}

/*
 * Transfers the next ancillary descriptor from connection ownership to a request.
 */
int
kwl_take_fd(
	struct kwl_client *client)
{
	int descriptor;

	/* A handle-bearing request is invalid until its ancillary reference has arrived. */
	if (client->right_count == 0)
		return -1;

	/* FIFO order is independent from arbitrary Unix stream fragment boundaries. */
	descriptor = client->rights[0];
	client->right_count--;
	memmove(client->rights, client->rights + 1, client->right_count * sizeof(int));

	/* Succeeded: the request now owns this descriptor and must close it. */
	return descriptor;
}

/*
 * Returns a retired client object ID through the canonical display event.
 */
void
kwl_delete_id(
	struct kwl_client *client,
	uint32_t id)
{
	int error;

	/* Fatal connections no longer allocate or recycle protocol identities. */
	if (client->fatal)
		return;

	/* The client may reuse the ID only after this ordered event is received. */
	error = kwl_emit(client, 1, 1, &id, sizeof(id));
	if (error != 0) {
		kwl_client_fail(client);
	}

	/* Succeeded: the retired identity is queued for ordered client reuse. */
	return;
}

/* Captures all received descriptors before reporting malformed ancillary state. */
static int
receive_rights(
	struct kwl_client *client,
	const struct msghdr *message)
{
	struct cmsghdr *header;
	size_t offset;
	size_t length;
	size_t least;
	size_t bytes;
	size_t index;
	int descriptor;
	int error;

	/* Kernel-produced ancillary records must fit the returned control span. */
	offset = 0;
	error = 0;
	while (offset + sizeof(*header) <= message->msg_controllen) {
		/* A record may be read only within the kernel-returned ancillary span. */
		header = (struct cmsghdr *)((unsigned char *)message->msg_control + offset);
		length = header->cmsg_len;
		least = CMSG_LEN(0);
		if (length < least || length > message->msg_controllen - offset)
			return EPROTO;

		/* This protocol uses only ordinary SCM_RIGHTS capability transfer. */
		if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS)
			return EPROTO;

		/* Close excess received references instead of losing them on a parse error. */
		bytes = length - CMSG_LEN(0);
		for (index = 0; index + sizeof(int) <= bytes; index += sizeof(int)) {
			/* Every delivered descriptor is either retained or closed before rejecting overflow. */
			memcpy(&descriptor, (unsigned char *)CMSG_DATA(header) + index, sizeof(descriptor));
			if (client->right_count == KWL_RIGHTS_MAX) {
				close(descriptor);
				error = EPROTO;
			} else {
				client->rights[client->right_count++] = descriptor;
			}
		}

		/* A partial descriptor encoding is never a valid kernel message. */
		if ((bytes % sizeof(int)) != 0)
			error = EPROTO;

		/* Control headers are aligned independently from Wayland message words. */
		offset += CMSG_SPACE(bytes);
	}

	/* Truncation would make the byte stream disagree with its descriptor FIFO. */
	if ((message->msg_flags & MSG_CTRUNC) != 0)
		error = EPROTO;

	/* Report ancillary errors after every complete delivered fd was retained or closed. */
	if (error != 0)
		return error;

	/* Succeeded: connection ownership includes every received descriptor. */
	return 0;
}

/*
 * Writes the unsent suffix of one packet, attaching its descriptor to the first byte.
 *
 * The descriptor is closed as soon as any byte of the packet has been accepted,
 * because the kernel then holds its own reference in the receive queue.
 */
static ssize_t
send_packet(
	struct kwl_client *client,
	struct kwl_packet *packet)
{
	struct msghdr message;
	struct iovec vector;
	struct cmsghdr *header;
	union {
		struct cmsghdr alignment;
		unsigned char bytes[CMSG_SPACE(sizeof(int))];
	} control;
	ssize_t sent;

	/* A packet without a pending descriptor is plain stream bytes. */
	if (packet->descriptor < 0 || packet->sent != 0) {
		sent = send(client->fd, packet->bytes + packet->sent, packet->size - packet->sent, MSG_DONTWAIT | MSG_NOSIGNAL);
		return sent;
	}

	/* The descriptor rides on the first byte of its own event. */
	memset(&message, 0, sizeof(message));
	memset(&control, 0, sizeof(control));
	vector.iov_base = packet->bytes;
	vector.iov_len = packet->size;
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = control.bytes;
	message.msg_controllen = sizeof(control.bytes);
	header = CMSG_FIRSTHDR(&message);
	header->cmsg_len = CMSG_LEN(sizeof(int));
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	memcpy(CMSG_DATA(header), &packet->descriptor, sizeof(int));
	sent = sendmsg(client->fd, &message, MSG_DONTWAIT | MSG_NOSIGNAL);

	/* Once any byte is accepted the kernel owns the transferred reference. */
	if (sent > 0) {
		close(packet->descriptor);
		packet->descriptor = -1;
	}

	/* Reports the byte count or the failure exactly as sendmsg did. */
	return sent;
}

/* Decodes complete native-endian requests without reading beyond an incomplete suffix. */
static int
decode_messages(
	struct kwl_client *client)
{
	uint32_t header[2];
	size_t size;
	int error;

	/* Arbitrary reads can contain several requests followed by a partial header. */
	while (client->input_size >= sizeof(header)) {
		/* Refuse malformed framing before an interface can inspect its payload. */
		memcpy(header, client->input, sizeof(header));
		size = header[1] >> 16;
		if (size < 8U ||
		    (size & 3U) != 0 ||
		    size > KWL_WIRE_MAX) {
			error = kwl_error(client, header[0], "invalid request size");
			return error;
		}

		/* Retain a complete header until all its declared payload bytes arrive. */
		if (size > client->input_size)
			return 0;

		/* Each request consumes only the descriptors specified by its interface signature. */
		error = kwl_dispatch(client, header[0], header[1] & 65535U, client->input + 8U, size - 8U);
		if (error == EAGAIN)
			return 0;

		/* A malformed complete request terminates the connection, unlike delayed rights. */
		if (error != 0)
			return error;

		/* Retire the completed request while preserving any partial successor. */
		client->input_size -= size;
		memmove(client->input, client->input + size, client->input_size);
	}

	/* Succeeded: no complete request remains buffered. */
	return 0;
}
