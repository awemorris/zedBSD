/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The framing of the messages between sshd's monitor and its unprivileged processes.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "ssh-privsep.h"

/*
 * Appends one frame (length, type, size bytes of body) to out.
 */
int
ssh_mon_put_frame(
	enum ssh_mon_type type,
	const uint8_t *body,
	size_t size,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)type;
	(void)body;
	(void)size;
	(void)out;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Takes one whole frame from the front of input (bytes read from the
 * socketpair): *type and the body (copied into body, which the caller
 * reset; secret when the type carries a secret).
 */
int
ssh_mon_next_frame(
	struct ssh_buf *input,
	enum ssh_mon_type *type,
	struct ssh_buf *body)
{
	/* Marks the arguments used until the function is implemented. */
	(void)input;
	(void)type;
	(void)body;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Writes a whole frame to descriptor and reads the whole reply frame,
 * blocking (the asking side of one request).
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
	/* Marks the arguments used until the function is implemented. */
	(void)descriptor;
	(void)type;
	(void)request;
	(void)request_size;
	(void)reply_type;
	(void)reply;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
