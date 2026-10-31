/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The TLS 1.3 states of the client handshake.
 */

#include "sslc-hs.h"

/*
 * TLS 1.3 after its ServerHello: the handshake secrets and keys (sslc-
 * hs13.c).
 */
int
sslc_hs13_server_hello(
	struct sslc_hs *hs,
	const struct sslc_server_hello *hello,
	struct sslc_hs_actions *actions)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hs);
	UNUSED_PARAMETER(hello);
	UNUSED_PARAMETER(actions);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * TLS 1.3 messages after the ServerHello, and after the handshake (sslc-
 * hs13.c); message is the whole message.
 */
int
sslc_hs13_message(
	struct sslc_hs *hs,
	unsigned type,
	const uint8_t *message,
	size_t size,
	struct sslc_hs_actions *actions)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hs);
	UNUSED_PARAMETER(type);
	UNUSED_PARAMETER(message);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(actions);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}
