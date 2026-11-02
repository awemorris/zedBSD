/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts sshd's pre-authentication and session processes share: the
 * start of a child, the frames from the monitor, the settings, the host
 * key operations through the monitor, and the blocking flush.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "sshd-child.h"

/* The most bytes one read of the monitor's socket asks for. */
#define CHILD_READ_CHUNK	4096U

/* The largest SIGNATURE body taken: a string of the 83-byte blob, with room. */
#define CHILD_SIGNATURE_MAX	128U

/* The bytes of a RESULT body. */
#define CHILD_RESULT_SIZE	4U

static int child_write_all(int descriptor, const uint8_t *data, size_t size);

/*
 * Sends one frame of type with size bytes of body to the monitor, for a
 * message that has no reply.
 */
int
sshd_child_send(
	enum ssh_mon_type type,
	const uint8_t *body,
	size_t size)
{
	struct ssh_buf frame;
	int framed;
	int written;

	/* Builds the frame in a secret buffer: AUTH_DONE carries the session keys. */
	ssh_buf_init(&frame, 4U + (size_t)SSH_MON_STATE_MAX, 1);
	framed = ssh_mon_put_frame(type, body, size, &frame);
	if (framed != 0) {
		ssh_buf_free(&frame);
		return EIO;
	}

	/* Sends it whole. */
	written = child_write_all(SSHD_FD_MONITOR, frame.data, frame.size);
	ssh_buf_free(&frame);
	if (written != 0)
		return EIO;

	/* Succeeded: the frame is sent. */
	return 0;
}

/*
 * Starts a child of the monitor.
 */
void
sshd_child_start(void)
{
	int flags;

	/* Nothing of the monitor's but the two descriptors belongs here. */
	(void)closefrom(SSHD_FD_MONITOR + 1);

	/* A peer that closes its end makes a write fail, not the process end. */
	(void)signal(SIGPIPE, SIG_IGN);

	/* The connection is driven with poll. */
	flags = fcntl(SSHD_FD_CLIENT, F_GETFL);
	if (flags >= 0)
		(void)fcntl(SSHD_FD_CLIENT, F_SETFL, flags | O_NONBLOCK);

	/* Opens the log now: once the root is changed there is no log socket to find. */
	openlog("sshd", LOG_PID | LOG_NDELAY, LOG_AUTH);
}

/*
 * Reads one whole frame from the monitor's socket.
 */
int
sshd_child_read_frame(
	struct ssh_buf *input,
	enum ssh_mon_type *type,
	struct ssh_buf *body)
{
	uint8_t chunk[CHILD_READ_CHUNK];
	ssize_t got;
	int taken;
	int added;

	/* Reads until a frame is whole. */
	for (;;) {
		taken = ssh_mon_next_frame(input, type, body);
		if (taken != EAGAIN)
			break;

		/* Takes more bytes; an end of file is the monitor gone. */
		got = read(SSHD_FD_MONITOR, chunk, sizeof(chunk));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0) {
			taken = EIO;
			break;
		}
		ssh_buf_put_bytes(input, chunk, (size_t)got);
		added = ssh_buf_error(input);
		if (added != 0) {
			taken = EIO;
			break;
		}
	}

	/* Wipes what was read: a frame may carry the session keys. */
	ssh_wipe(chunk, sizeof(chunk));

	/* Reports why no frame came. */
	if (taken != 0)
		return taken;

	/* Succeeded: one frame is in *type and body. */
	return 0;
}

/*
 * Reads CONFIG into child.
 */
int
sshd_child_read_config(
	struct ssh_buf *input,
	struct sshd_child *child)
{
	struct ssh_buf body;
	struct ssh_reader reader;
	enum ssh_mon_type type;
	char error_text[SSH_CONFIG_ERROR_MAX];
	uint8_t public_key[SSH_ED25519_PUBLIC];
	const uint8_t *text;
	const uint8_t *blob;
	size_t text_size;
	size_t blob_size;
	uint32_t privileged;
	int status;

	/* Takes the first frame, which must be CONFIG. */
	memset(child, 0, sizeof(*child));
	ssh_buf_init(&body, SSH_MON_STATE_MAX, 1);
	status = sshd_child_read_frame(input, &type, &body);
	if (status != 0) {
		ssh_buf_free(&body);
		return EIO;
	}
	if (type != SSH_MON_CONFIG) {
		ssh_buf_free(&body);
		return EPROTO;
	}

	/* Reads the four fields, with nothing after them. */
	ssh_reader_init(&reader, body.data, body.size);
	ssh_reader_get_string(&reader, &text, &text_size);
	ssh_reader_get_string(&reader, &blob, &blob_size);
	ssh_reader_get_cstring(&reader, child->peer, sizeof(child->peer));
	privileged = ssh_reader_get_u32(&reader);
	status = ssh_reader_finish(&reader);
	if (status != 0 ||
	    blob_size != SSH_KEY_BLOB_SIZE ||
	    privileged > 1) {
		ssh_buf_free(&body);
		return EPROTO;
	}

	/* The blob must be a key of the one type. */
	status = ssh_key_parse_public_blob(blob, blob_size, public_key);
	if (status != 0) {
		ssh_buf_free(&body);
		return EPROTO;
	}
	memcpy(child->host_key_blob, blob, SSH_KEY_BLOB_SIZE);
	child->privileged = (int)privileged;

	/* Reads the settings as the monitor applies them. */
	sshd_config_defaults(&child->config);
	status = sshd_config_parse(&child->config, (const char *)text, text_size, error_text);
	ssh_buf_free(&body);
	if (status != 0)
		return EPROTO;

	/* Succeeded: child holds the connection's settings. */
	return 0;
}

/*
 * Appends the host's public key blob.
 */
int
sshd_child_host_key(
	void *context,
	struct ssh_buf *out)
{
	const struct sshd_child *child;
	int written;

	/* The blob CONFIG brought. */
	child = context;
	ssh_buf_put_bytes(out, child->host_key_blob, SSH_KEY_BLOB_SIZE);
	written = ssh_buf_error(out);
	if (written != 0)
		return written;

	/* Succeeded: the blob is appended. */
	return 0;
}

/*
 * Asks the monitor for the signature of h and appends its blob.
 */
int
sshd_child_sign(
	void *context,
	const uint8_t *h,
	struct ssh_buf *out)
{
	struct ssh_buf request;
	struct ssh_buf reply;
	struct ssh_reader reader;
	enum ssh_mon_type type;
	const uint8_t *blob;
	size_t blob_size;
	int called;
	int finished;
	int written;

	/* The request: string h. */
	(void)context;
	ssh_buf_init(&request, 64, 0);
	ssh_buf_init(&reply, CHILD_SIGNATURE_MAX, 0);
	ssh_buf_put_string(&request, h, SSH_KEX_HASH_SIZE);
	called = ssh_buf_error(&request);
	if (called == 0)
		called = ssh_mon_call(SSHD_FD_MONITOR, SSH_MON_SIGN, request.data, request.size, &type, &reply);
	ssh_buf_free(&request);
	if (called != 0 || type != SSH_MON_SIGNATURE) {
		ssh_buf_free(&reply);
		return EIO;
	}

	/* The reply: string blob, with nothing after it. */
	ssh_reader_init(&reader, reply.data, reply.size);
	ssh_reader_get_string(&reader, &blob, &blob_size);
	finished = ssh_reader_finish(&reader);
	if (finished != 0) {
		ssh_buf_free(&reply);
		return EIO;
	}
	ssh_buf_put_bytes(out, blob, blob_size);
	ssh_buf_free(&reply);
	written = ssh_buf_error(out);
	if (written != 0)
		return EIO;

	/* Succeeded: the signature blob is appended. */
	return 0;
}

/*
 * Asks the monitor one request and takes its RESULT.
 */
int
sshd_child_ask(
	enum ssh_mon_type type,
	const uint8_t *body,
	size_t size,
	uint32_t *result)
{
	struct ssh_buf reply;
	enum ssh_mon_type reply_type;
	int called;

	/* A RESULT is four bytes; the buffer is secret since the request's call keeps nothing else here. */
	ssh_buf_init(&reply, 64, 1);
	called = ssh_mon_call(SSHD_FD_MONITOR, type, body, size, &reply_type, &reply);
	if (called != 0 ||
	    reply_type != SSH_MON_RESULT ||
	    reply.size != CHILD_RESULT_SIZE) {
		ssh_buf_free(&reply);
		return EIO;
	}
	*result = ssh_load_be32(reply.data);
	ssh_buf_free(&reply);

	/* Succeeded: *result is the monitor's answer. */
	return 0;
}

/*
 * Writes the transport's pending output until none is left.
 */
int
sshd_child_flush(
	struct ssh_transport *transport)
{
	struct pollfd waiting;
	const uint8_t *pending;
	size_t size;
	ssize_t written;
	int ready;

	/* Writes, waiting while the connection is full, until nothing is pending. */
	for (;;) {
		pending = ssh_transport_pending(transport, &size);
		if (size == 0)
			break;
		written = write(SSHD_FD_CLIENT, pending, size);
		if (written > 0) {
			ssh_transport_written(transport, (size_t)written);
			continue;
		}
		if (written < 0 && errno == EINTR)
			continue;
		if (written < 0 &&
		    errno != EAGAIN &&
		    errno != EWOULDBLOCK)
			return EIO;

		/* Waits until the connection takes more. */
		waiting.fd = SSHD_FD_CLIENT;
		waiting.events = POLLOUT;
		waiting.revents = 0;
		ready = poll(&waiting, 1, -1);
		if (ready < 0 && errno != EINTR)
			return EIO;
	}

	/* Succeeded: everything pending is written. */
	return 0;
}

/*
 * Returns the seconds of the monotonic clock.
 */
uint64_t
sshd_child_now(void)
{
	struct timespec now;

	/* Reads the clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Reports the whole seconds. */
	return (uint64_t)now.tv_sec;
}

/* Writes all size bytes to a blocking descriptor; 0 or EIO. */
static int
child_write_all(
	int descriptor,
	const uint8_t *data,
	size_t size)
{
	size_t done;
	ssize_t written;

	/* Writes until every byte has gone. */
	done = 0;
	while (done < size) {
		written = write(descriptor, data + done, size - done);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return EIO;
		done += (size_t)written;
	}

	/* Succeeded: every byte was written. */
	return 0;
}
