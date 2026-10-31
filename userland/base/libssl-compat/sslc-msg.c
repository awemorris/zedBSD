/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The encoding and syntax checks of handshake messages.
 */

#include "sslc-msg.h"

/*
 * Reads a handshake message header (4; RFC 5246 7.4): type and body length
 * (24 bits).
 */
void
sslc_msg_header(
	const uint8_t *in,
	unsigned *type,
	size_t *length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(in);
	UNUSED_PARAMETER(type);
	UNUSED_PARAMETER(length);
}

/*
 * Writes a ClientHello (above).
 */
int
sslc_msg_write_client_hello(
	const struct sslc_client_hello *hello,
	uint8_t *out,
	size_t room,
	size_t *written)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hello);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(room);
	UNUSED_PARAMETER(written);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Parses a ServerHello or HelloRetryRequest body.
 */
int
sslc_msg_parse_server_hello(
	const uint8_t *body,
	size_t size,
	struct sslc_server_hello *hello,
	unsigned *alert)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(body);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(hello);
	UNUSED_PARAMETER(alert);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Parses an EncryptedExtensions body (4.3.1): an extensions block, each
 * well formed, no type twice; allowed: server_name (empty) and
 * supported_groups (a list, ignored, 4.2.7); anything else is
 * unsupported_extension (4.2: libssl-compat offers nothing else that a
 * server answers here).
 */
int
sslc_msg_parse_encrypted_extensions(
	const uint8_t *body,
	size_t size,
	int *server_name_ack,
	unsigned *alert)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(body);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(server_name_ack);
	UNUSED_PARAMETER(alert);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Parses a Certificate body: TLS 1.3 (4.4.2) when tls13 is 1, else TLS 1.2
 * (RFC 5246 7.4.2).
 */
int
sslc_msg_parse_certificate(
	const uint8_t *body,
	size_t size,
	int tls13,
	struct sslc_certificate_msg *message,
	unsigned *alert)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(body);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(tls13);
	UNUSED_PARAMETER(message);
	UNUSED_PARAMETER(alert);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Parses a CertificateVerify body (4.4.3): the scheme and a view of the
 * signature<0..2^16-1>.
 */
int
sslc_msg_parse_certificate_verify(
	const uint8_t *body,
	size_t size,
	uint16_t *scheme,
	const uint8_t **signature,
	size_t *signature_size,
	unsigned *alert)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(body);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(scheme);
	UNUSED_PARAMETER(signature);
	UNUSED_PARAMETER(signature_size);
	UNUSED_PARAMETER(alert);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Parses a ServerKeyExchange body for ECDHE (RFC 8422 5.4 with RFC 5246
 * 7.4.1.4.1's SignatureAndHashAlgorithm): curve_type must be named_curve
 * (3; else illegal_parameter), then the group, the point<1..2^8-1>, the
 * scheme and the signature<0..2^16-1>, nothing after.
 */
int
sslc_msg_parse_server_key_exchange(
	const uint8_t *body,
	size_t size,
	struct sslc_server_key_exchange *exchange,
	unsigned *alert)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(body);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(exchange);
	UNUSED_PARAMETER(alert);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Checks a CertificateRequest body (4.3.2 when tls13 is 1: context<0..
 */
int
sslc_msg_parse_certificate_request(
	const uint8_t *body,
	size_t size,
	int tls13,
	const uint8_t **context,
	size_t *context_size,
	unsigned *alert)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(body);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(tls13);
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(context_size);
	UNUSED_PARAMETER(alert);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Checks a TLS 1.3 NewSessionTicket body (4.6.1) for syntax: lifetime,
 * age_add, nonce<0..255>, ticket<1..2^16-1>, extensions<0..2^16-2>.
 */
int
sslc_msg_parse_new_session_ticket(
	const uint8_t *body,
	size_t size,
	unsigned *alert)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(body);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(alert);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Parses a KeyUpdate body (4.6.3): one byte, 0 (update_not_requested) or 1
 * (update_requested), else illegal_parameter (EPROTO); another length is
 * decode_error.
 */
int
sslc_msg_parse_key_update(
	const uint8_t *body,
	size_t size,
	int *update_requested,
	unsigned *alert)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(body);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(update_requested);
	UNUSED_PARAMETER(alert);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Writes a Finished (4.4.4; RFC 5246 7.4.9) carrying verify_data
 * (verify_size bytes).
 */
int
sslc_msg_write_finished(
	const uint8_t *verify_data,
	size_t verify_size,
	uint8_t *out,
	size_t room,
	size_t *written)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(verify_data);
	UNUSED_PARAMETER(verify_size);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(room);
	UNUSED_PARAMETER(written);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Writes an empty client Certificate: TLS 1.3 with the request's context
 * echoed and no entries (4.4.2.4); TLS 1.2 with an empty list (RFC 5246
 * 7.4.6).
 */
int
sslc_msg_write_empty_certificate(
	int tls13,
	const uint8_t *context,
	size_t context_size,
	uint8_t *out,
	size_t room,
	size_t *written)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(tls13);
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(context_size);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(room);
	UNUSED_PARAMETER(written);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Writes a TLS 1.2 ECDHE ClientKeyExchange (RFC 8422 5.7): the
 * point<1..2^8-1>.
 */
int
sslc_msg_write_client_key_exchange(
	const uint8_t *point,
	size_t point_size,
	uint8_t *out,
	size_t room,
	size_t *written)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(point);
	UNUSED_PARAMETER(point_size);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(room);
	UNUSED_PARAMETER(written);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Writes a TLS 1.3 KeyUpdate (4.6.3) with request_update 0 or 1.
 */
int
sslc_msg_write_key_update(
	int update_requested,
	uint8_t *out,
	size_t room,
	size_t *written)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(update_requested);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(room);
	UNUSED_PARAMETER(written);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}
