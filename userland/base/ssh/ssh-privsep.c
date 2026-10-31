/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The framing of the messages between sshd's monitor and its unprivileged
 * processes: the frame writer, the frame parser, and the blocking
 * request-and-reply call of the asking side.
 *
 * The frame is a uint32 length (of the type byte and the body that follow
 * it), a type byte, and the body.  A length counts the type byte, so a
 * length of 0 has no type and is refused.  The limit of a type is a limit
 * on that length: SSH_MON_STATE_MAX for CONFIG, AUTH_DONE and
 * SESSION_START, SSH_MON_FRAME_MAX for every other type.
 *
 * The parser decides as early as the bytes allow, so a peer that announces
 * a frame it may not send is refused before its body is waited for.
 */

#include <string.h>
#include <unistd.h>

#include "ssh-crypto.h"
#include "ssh-privsep.h"

/* The bytes before the body: the length and the type. */
#define MON_HEADER_SIZE		5U

/* The bytes of the length field. */
#define MON_LENGTH_SIZE		4U

/* The most bytes one read system call is asked for. */
#define MON_READ_CHUNK		4096U

static uint32_t mon_length_limit(unsigned type_byte);
static int mon_write_all(int descriptor, const uint8_t *data, size_t size);
static int mon_read_some(int descriptor, uint8_t *data, size_t size, size_t *count);
static size_t mon_bytes_wanted(const struct ssh_buf *input);

/*
 * Appends one frame (length, type, size bytes of body) to out.
 *
 * Room for the whole frame is made first, so a frame that does not fit
 * leaves nothing of itself in out.  A body over the limit of its type, a
 * type outside the enumeration, or a body that is missing returns the
 * error without recording it in out: out stays usable.
 */
int
ssh_mon_put_frame(
	enum ssh_mon_type type,
	const uint8_t *body,
	size_t size,
	struct ssh_buf *out)
{
	uint32_t limit;
	int error;

	/* Does nothing after an earlier failure of out. */
	if (out->error != 0)
		return out->error;

	/* Refuses a body that is missing. */
	if (body == NULL && size != 0)
		return EINVAL;

	/* Refuses a type outside the enumeration, a negative value included (its limit is 0). */
	if ((unsigned long)type > 255UL)
		return EINVAL;
	limit = mon_length_limit((unsigned)type);
	if (limit == 0)
		return EINVAL;

	/* Refuses a body whose length with the type byte passes the limit of its type. */
	if (size > (size_t)limit - 1)
		return EMSGSIZE;

	/* Makes room for the whole frame at once. */
	error = ssh_buf_reserve(out, MON_HEADER_SIZE + size);
	if (error != 0)
		return error;

	/* Writes the length, the type, and the body. */
	ssh_buf_put_u32(out, (uint32_t)(size + 1));
	ssh_buf_put_u8(out, (uint8_t)type);
	ssh_buf_put_bytes(out, body, size);

	/* Reports a failure the writes recorded. */
	error = ssh_buf_error(out);
	if (error != 0)
		return error;

	/* Succeeded: the frame is at the end of out. */
	return 0;
}

/*
 * Takes one whole frame from the front of input (bytes read from the
 * socketpair): *type and the body (copied into body, which the caller
 * reset; secret when the type carries a secret).
 *
 * The body buffer is made secret and emptied first, whatever the caller
 * passed, because the type is not known until the frame is read.  On any
 * return other than 0 input is unchanged and body is empty.  A body that
 * does not fit the limit of the caller's buffer returns that buffer's error
 * (EMSGSIZE or ENOMEM).
 */
int
ssh_mon_next_frame(
	struct ssh_buf *input,
	enum ssh_mon_type *type,
	struct ssh_buf *body)
{
	uint32_t length;
	uint32_t limit;
	unsigned type_byte;
	size_t body_size;
	int error;

	/* Marks the body as holding a secret, and empties it of anything an earlier frame left. */
	body->secret = 1;
	ssh_buf_reset(body);

	/* Waits for the four bytes of the length. */
	if (input->size < MON_LENGTH_SIZE)
		return EAGAIN;

	/* Refuses a length with no type byte. */
	length = ssh_load_be32(input->data);
	if (length == 0)
		return EINVAL;

	/* Refuses a length over the largest limit of any type. */
	if (length > SSH_MON_STATE_MAX)
		return EINVAL;

	/* Waits for the type byte. */
	if (input->size < MON_HEADER_SIZE)
		return EAGAIN;

	/* Refuses a type outside the enumeration (its limit is 0). */
	type_byte = input->data[MON_LENGTH_SIZE];
	limit = mon_length_limit(type_byte);
	if (limit == 0)
		return EINVAL;

	/* Refuses a length over the limit of this type before its body arrives. */
	if (length > limit)
		return EINVAL;

	/* Waits for the rest of the frame. */
	if (input->size - MON_LENGTH_SIZE < (size_t)length)
		return EAGAIN;

	/* Copies the body out, leaving the input as it was when the body does not fit. */
	body_size = (size_t)length - 1;
	ssh_buf_put_bytes(body, input->data + MON_HEADER_SIZE, body_size);
	error = ssh_buf_error(body);
	if (error != 0) {
		ssh_buf_reset(body);
		return error;
	}

	/* Takes the frame off the front of the input (the removed bytes are wiped when input is secret). */
	*type = (enum ssh_mon_type)type_byte;
	ssh_buf_consume(input, MON_LENGTH_SIZE + (size_t)length);

	/* Succeeded: *type and body hold one frame. */
	return 0;
}

/*
 * Writes a whole frame to descriptor and reads the whole reply frame,
 * blocking (the asking side of one request).
 *
 * Exactly the bytes of one reply frame are read, never more, so the bytes
 * of a frame that follows stay on the descriptor.  The frame and the
 * partial input are kept in secret buffers, since a request may carry a
 * password.  Writing to a descriptor whose other side has closed raises
 * SIGPIPE unless the process ignores it; a process that calls this
 * ignores it, and the write then fails with EIO.
 */
int
ssh_mon_call(
	int descriptor,
	enum ssh_mon_type type,
	const uint8_t *request,
	size_t request_size,
	enum ssh_mon_type *reply_type,
	struct ssh_buf *reply)
{
	struct ssh_buf frame;
	struct ssh_buf input;
	uint8_t chunk[MON_READ_CHUNK];
	size_t wanted;
	size_t count;
	int error;

	/* Starts the buffers of the request frame and of the reply bytes read so far. */
	ssh_buf_init(&frame, MON_LENGTH_SIZE + (size_t)SSH_MON_STATE_MAX, 1);
	ssh_buf_init(&input, MON_LENGTH_SIZE + (size_t)SSH_MON_STATE_MAX, 1);

	/* Builds the request frame. */
	error = ssh_mon_put_frame(type, request, request_size, &frame);
	if (error != 0)
		goto done;

	/* Sends the whole frame. */
	error = mon_write_all(descriptor, frame.data, frame.size);
	if (error != 0)
		goto done;

	/* Reads the reply a piece at a time, asking each time for only the bytes still missing. */
	error = ssh_mon_next_frame(&input, reply_type, reply);
	while (error == EAGAIN) {
		wanted = mon_bytes_wanted(&input);
		if (wanted > sizeof(chunk))
			wanted = sizeof(chunk);

		/* Takes the next bytes of the reply from the descriptor. */
		error = mon_read_some(descriptor, chunk, wanted, &count);
		if (error != 0)
			goto done;

		/* Adds them to the reply bytes read so far. */
		ssh_buf_put_bytes(&input, chunk, count);
		error = ssh_buf_error(&input);
		if (error != 0)
			goto done;

		/* Tries to take a whole frame from what has arrived. */
		error = ssh_mon_next_frame(&input, reply_type, reply);
	}

done:
	/* Wipes the request frame, the reply bytes and the read piece, which may hold a password. */
	ssh_buf_free(&frame);
	ssh_buf_free(&input);
	ssh_wipe(chunk, sizeof(chunk));

	/* Reports why the call failed. */
	if (error != 0)
		return error;

	/* Succeeded: the reply frame is in *reply_type and reply. */
	return 0;
}

/*
 * Returns the limit on the length of a frame of the type with the byte
 * value type_byte, or 0 when the byte is not a type of the enumeration.
 */
static uint32_t
mon_length_limit(
	unsigned type_byte)
{
	/* Gives the exported transport and the configuration their larger limit, every other type the frame limit. */
	switch (type_byte) {
	case SSH_MON_CONFIG:
	case SSH_MON_AUTH_DONE:
	case SSH_MON_SESSION_START:
		return SSH_MON_STATE_MAX;
	case SSH_MON_SIGN:
	case SSH_MON_SIGNATURE:
	case SSH_MON_PUBKEY_QUERY:
	case SSH_MON_PUBKEY_LOGIN:
	case SSH_MON_PASSWORD_LOGIN:
	case SSH_MON_LOGIN_RECORD:
	case SSH_MON_LOGOUT_RECORD:
	case SSH_MON_RESULT:
		return SSH_MON_FRAME_MAX;
	default:
		break;
	}

	/* Reports a byte that is no type. */
	return 0;
}

/*
 * Writes all size bytes to descriptor, retrying after a signal and after a
 * short write.  Returns 0 or EIO.
 */
static int
mon_write_all(
	int descriptor,
	const uint8_t *data,
	size_t size)
{
	size_t done;
	ssize_t written;

	/* Writes until every byte has gone, a short write continuing from where it stopped. */
	done = 0;
	while (done < size) {
		written = write(descriptor, data + done, size - done);
		if (written < 0) {
			/* A signal interrupted the write before any byte went: tries again. */
			if (errno == EINTR)
				continue;

			/* The descriptor failed or the other side closed. */
			return EIO;
		}
		if (written == 0)
			return EIO;

		done += (size_t)written;
	}

	/* Succeeded: every byte was written. */
	return 0;
}

/*
 * Reads at least one and at most size bytes from descriptor into data,
 * retrying after a signal; *count receives how many.  Returns 0 or EIO
 * (the other side closed, or the descriptor failed).
 */
static int
mon_read_some(
	int descriptor,
	uint8_t *data,
	size_t size,
	size_t *count)
{
	ssize_t got;

	/* Reads until a read returns bytes, an end of file, or a failure other than a signal. */
	for (;;) {
		got = read(descriptor, data, size);
		if (got < 0) {
			/* A signal interrupted the read before any byte came: tries again. */
			if (errno == EINTR)
				continue;

			/* The descriptor failed. */
			return EIO;
		}

		/* The other side closed before the frame was complete. */
		if (got == 0)
			return EIO;

		break;
	}

	/* Succeeded: reports how many bytes arrived. */
	*count = (size_t)got;
	return 0;
}

/*
 * Returns how many more bytes a frame being read needs, given that
 * ssh_mon_next_frame answered EAGAIN for the input: the rest of the
 * header while it is short, then the rest of the announced frame.  The
 * announced length is within its limit, since the parser would have
 * refused it otherwise.
 */
static size_t
mon_bytes_wanted(
	const struct ssh_buf *input)
{
	size_t length;

	/* Asks for the rest of the length and the type byte while they are not all there. */
	if (input->size < MON_HEADER_SIZE)
		return MON_HEADER_SIZE - input->size;

	/* Asks for the rest of the announced frame. */
	length = (size_t)ssh_load_be32(input->data);
	return MON_LENGTH_SIZE + length - input->size;
}
