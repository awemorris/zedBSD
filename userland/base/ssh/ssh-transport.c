/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The transport layer of one connection.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "ssh-transport.h"

/*
 * Starts a transport for role with ops (copied) and queues this side's
 * identification line (SSH_VERSION_STRING and CR LF) in the output.
 */
int
ssh_transport_init(
	struct ssh_transport *transport,
	enum ssh_role role,
	const struct ssh_transport_ops *ops,
	uint64_t now)
{
	/* Marks the arguments used until the function is implemented. */
	(void)transport;
	(void)role;
	(void)ops;
	(void)now;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Wipes every secret (keys, k, h, the client's half, held payloads) and
 * frees the memory.
 */
void
ssh_transport_free(
	struct ssh_transport *transport)
{
	/* Marks the arguments used until the function is implemented. */
	(void)transport;
}

/*
 * Appends size received bytes.
 */
int
ssh_transport_input(
	struct ssh_transport *transport,
	const uint8_t *data,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)transport;
	(void)data;
	(void)size;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Processes received input until one upper-layer message is ready or the
 * input is used up.
 */
int
ssh_transport_next(
	struct ssh_transport *transport,
	struct ssh_buf *payload)
{
	/* Marks the arguments used until the function is implemented. */
	(void)transport;
	(void)payload;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Sends one upper-layer message (message number first; 5, 6, or 50 and
 * above).
 */
int
ssh_transport_send(
	struct ssh_transport *transport,
	const uint8_t *payload,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)transport;
	(void)payload;
	(void)size;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Gives the current time (seconds, monotonic) and starts a re-exchange
 * when a limit is reached and none is in progress (state RUNNING).
 */
int
ssh_transport_tick(
	struct ssh_transport *transport,
	uint64_t now)
{
	/* Marks the arguments used until the function is implemented. */
	(void)transport;
	(void)now;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Queues SSH_MSG_DISCONNECT with code and description (NUL-terminated,
 * cut to SSH_TRANSPORT_REASON_MAX) and sets the state CLOSED.
 */
void
ssh_transport_disconnect(
	struct ssh_transport *transport,
	uint32_t code,
	const char *description)
{
	/* Marks the arguments used until the function is implemented. */
	(void)transport;
	(void)code;
	(void)description;
}

/*
 * Returns the bytes waiting to be written (as ssh_packet_pending).
 */
const uint8_t *
ssh_transport_pending(
	const struct ssh_transport *transport,
	size_t *size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)transport;
	(void)size;

	/* Reports that the function is not implemented yet. */
	return NULL;
}

/*
 * Removes size written bytes from the output.
 */
void
ssh_transport_written(
	struct ssh_transport *transport,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)transport;
	(void)size;
}

/*
 * Appends everything another process needs to go on with this connection
 * after the initial exchange and authentication (a privilege-separated
 * server hands its connection from the pre-authentication process to the
 * session process): role, both identification lines, the session
 * identifier, the strict flag, both directions' keys, sequence numbers
 * and counters, the time of the last key, and the received bytes not yet
 * processed.
 */
int
ssh_transport_export(
	const struct ssh_transport *transport,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)transport;
	(void)out;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Rebuilds a transport from size bytes made by ssh_transport_export,
 * with new ops.
 */
int
ssh_transport_import(
	struct ssh_transport *transport,
	const uint8_t *data,
	size_t size,
	const struct ssh_transport_ops *ops,
	uint64_t now)
{
	/* Marks the arguments used until the function is implemented. */
	(void)transport;
	(void)data;
	(void)size;
	(void)ops;
	(void)now;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
