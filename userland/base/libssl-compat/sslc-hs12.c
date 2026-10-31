/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The TLS 1.2 states of the client handshake.
 */

#include "sslc-hs.h"

/*
 * TLS 1.2 after its ServerHello (sslc-hs12.c).
 */
int
sslc_hs12_server_hello(
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
 * TLS 1.2 messages after the ServerHello, and HelloRequest after the
 * handshake (sslc-hs12.c).
 */
int
sslc_hs12_message(
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

/*
 * TLS 1.2 ChangeCipherSpec in its state (sslc-hs12.c).
 */
int
sslc_hs12_change_cipher_spec(
	struct sslc_hs *hs,
	struct sslc_hs_actions *actions)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hs);
	UNUSED_PARAMETER(actions);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}
