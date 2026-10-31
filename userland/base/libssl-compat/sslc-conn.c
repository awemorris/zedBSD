/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * One TLS client connection over a transport.
 */

#include "sslc-conn.h"

/*
 * Makes a connection (no I/O yet).
 */
int
sslc_conn_init(
	struct sslc_conn *conn,
	const struct sslc_hs_config *config,
	const struct sslc_transport *transport,
	uint32_t flags)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(conn);
	UNUSED_PARAMETER(config);
	UNUSED_PARAMETER(transport);
	UNUSED_PARAMETER(flags);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Runs the handshake until it is done or must wait.
 */
int
sslc_conn_handshake(
	struct sslc_conn *conn,
	unsigned *wants)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(conn);
	UNUSED_PARAMETER(wants);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Reads up to size bytes of application data into buffer, running the
 * handshake first when needed.
 */
int
sslc_conn_read(
	struct sslc_conn *conn,
	uint8_t *buffer,
	size_t size,
	size_t *done,
	unsigned *wants)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(conn);
	UNUSED_PARAMETER(buffer);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(done);
	UNUSED_PARAMETER(wants);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Writes size bytes of application data (in records of at most
 * SSLC_RECORD_PLAINTEXT_MAX), running the handshake first when needed.
 */
int
sslc_conn_write(
	struct sslc_conn *conn,
	const uint8_t *buffer,
	size_t size,
	size_t *done,
	unsigned *wants)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(conn);
	UNUSED_PARAMETER(buffer);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(done);
	UNUSED_PARAMETER(wants);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Returns the decrypted application bytes buffered (conn->plain_size).
 */
size_t
sslc_conn_pending(
	const struct sslc_conn *conn)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(conn);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Sends close_notify once (after flushing what is pending) and reports
 * whether the peer's was received: *both is 1 when it was.
 */
int
sslc_conn_shutdown(
	struct sslc_conn *conn,
	int *both,
	unsigned *wants)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(conn);
	UNUSED_PARAMETER(both);
	UNUSED_PARAMETER(wants);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Wipes and frees everything the connection holds (the transport's context
 * is the caller's).
 */
void
sslc_conn_free(
	struct sslc_conn *conn)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(conn);
}
