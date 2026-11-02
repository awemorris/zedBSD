/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The encoding and syntax checks of handshake messages.
 */

#include <string.h>

#include "sslc-msg.h"

/*
 * The largest numbers a length prefix of 1, 2 and 3 bytes holds.
 *
 * Values referenced: RFC 8446 section 3.4 (a vector's length prefix is as
 * wide as its ceiling needs: 2^8-1, 2^16-1, 2^24-1).
 */
#define SSLC_MSG_MAX_U8		255U
#define SSLC_MSG_MAX_U16	65535U
#define SSLC_MSG_MAX_U24	16777215U

/*
 * The most entries one list of a ClientHello is given: two bytes each
 * (and the one signalling suite after the suites) must fit the 2-byte
 * length of the list, whose ceiling is 2^16-2 for the suites and the
 * signature schemes (RFC 8446 section 4.1.2 and 4.2.3).
 */
#define SSLC_MSG_LIST_MAX	32766U

/*
 * The longest legacy_session_id.
 *
 * Values referenced: RFC 8446 section 4.1.2 (opaque legacy_session_id<0..32>).
 */
#define SSLC_MSG_SESSION_ID_MAX	32U

/*
 * The one compression method, "null".
 *
 * Values referenced: RFC 8446 section 4.1.2 (legacy_compression_methods
 * contains exactly one byte, set to zero).
 */
#define SSLC_MSG_COMPRESSION_NULL	0U

/*
 * The one name type of server_name, "host_name".
 *
 * Values referenced: RFC 6066 section 3 (NameType host_name(0)).
 */
#define SSLC_MSG_NAME_TYPE_HOST_NAME	0U

/*
 * The one point format libssl-compat speaks, "uncompressed".
 *
 * Values referenced: RFC 8422 section 5.1.2 (ECPointFormat uncompressed(0)).
 */
#define SSLC_MSG_POINT_UNCOMPRESSED	0U

/*
 * The one curve type of a ServerKeyExchange, "named_curve".
 *
 * Values referenced: RFC 8422 section 5.4 (ECCurveType named_curve(3)).
 */
#define SSLC_MSG_CURVE_NAMED		3U

/*
 * The two values of a KeyUpdate's request_update.
 *
 * Values referenced: RFC 8446 section 4.6.3 (update_not_requested(0),
 * update_requested(1)).
 */
#define SSLC_MSG_UPDATE_NOT_REQUESTED	0U
#define SSLC_MSG_UPDATE_REQUESTED	1U

/* Sets the alert to send and returns the errno value that goes with it. */
static int sslc_msg_fail(unsigned *alert, unsigned description, int error);

/* Reads one extension (its type and its data as a reader) from an extensions block. */
static int sslc_msg_next_extension(struct sslc_reader *block, uint32_t *type, struct sslc_reader *data);

/* Checks the syntax of an extensions block whose extensions are not used. */
static int sslc_msg_skip_extensions(const struct sslc_reader *block, unsigned *alert);

/* Checks a ClientHello's fields for the inconsistencies its writer refuses. */
static int sslc_msg_check_client_hello(const struct sslc_client_hello *hello);

/* Writes the extensions of a ClientHello into the open extensions block. */
static void sslc_msg_write_extensions(struct sslc_writer *writer, const struct sslc_client_hello *hello, size_t name_size);

/* Takes in one extension of a ServerHello or HelloRetryRequest. */
static int sslc_msg_hello_extension(struct sslc_server_hello *hello, uint32_t type, struct sslc_reader *data, unsigned *alert);

/* Ends a message: reports the writer's state and how many bytes were written. */
static int sslc_msg_end_write(const struct sslc_writer *writer, size_t *written);

/* Gives the alert for an extension the message does not allow: illegal_parameter for one the client sends, else unsupported_extension. */
static unsigned sslc_msg_misplaced_alert(uint32_t type);

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
	size_t value;

	/* The type is the first byte. */
	*type = in[0];

	/* The length is the next three bytes, big-endian. */
	value = (size_t)in[1];
	value = value * 256U + (size_t)in[2];
	value = value * 256U + (size_t)in[3];
	*length = value;
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
	struct sslc_writer writer;
	size_t name_size;
	size_t index;
	int error;

	/* Nothing is written until the fields are checked. */
	*written = 0;
	error = sslc_msg_check_client_hello(hello);
	if (error != 0)
		return error;

	/* The length of the host name, 0 when there is none. */
	name_size = 0;
	if (hello->server_name != NULL)
		name_size = strlen(hello->server_name);

	/* Opens the message: its type and its 3-byte length. */
	sslc_writer_init(&writer, out, room);
	sslc_writer_u8(&writer, SSLC_HS_CLIENT_HELLO);
	sslc_writer_begin(&writer, 3);

	/* The legacy version and the random. */
	sslc_writer_u16(&writer, SSLC_TLS12);
	sslc_writer_bytes(&writer, hello->random, sizeof(hello->random));

	/* The legacy session id. */
	sslc_writer_begin(&writer, 1);
	sslc_writer_bytes(&writer, hello->session_id, hello->session_id_size);
	sslc_writer_end(&writer);

	/* The cipher suites, then the signalling value that stands for an empty renegotiation_info. */
	sslc_writer_begin(&writer, 2);
	for (index = 0; index < hello->suite_count; index++)
		sslc_writer_u16(&writer, hello->suites[index]);
	if (hello->min_version == SSLC_TLS12)
		sslc_writer_u16(&writer, SSLC_SUITE_EMPTY_RENEGOTIATION_INFO_SCSV);
	sslc_writer_end(&writer);

	/* The compression methods: only null. */
	sslc_writer_begin(&writer, 1);
	sslc_writer_u8(&writer, SSLC_MSG_COMPRESSION_NULL);
	sslc_writer_end(&writer);

	/* The extensions. */
	sslc_writer_begin(&writer, 2);
	sslc_msg_write_extensions(&writer, hello, name_size);
	sslc_writer_end(&writer);

	/* Closes the message and reports its size. */
	sslc_writer_end(&writer);
	return sslc_msg_end_write(&writer, written);
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
	struct sslc_reader reader;
	struct sslc_reader session;
	struct sslc_reader block;
	struct sslc_reader data;
	const uint8_t *random;
	uint32_t type;
	size_t left;
	int same;
	int error;

	/* Starts with nothing found and no alert. */
	memset(hello, 0, sizeof(*hello));
	*alert = 0;
	sslc_reader_init(&reader, body, size);

	/* The version and the random. */
	hello->legacy_version = (uint16_t)sslc_reader_u16(&reader);
	random = sslc_reader_bytes(&reader, sizeof(hello->random));
	if (reader.error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
	memcpy(hello->random, random, sizeof(hello->random));

	/* The session id echoed, at most 32 bytes. */
	error = sslc_reader_vector(&reader, 1, 0, SSLC_MSG_SESSION_ID_MAX, &session);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
	hello->session_id = session.data;
	hello->session_id_size = session.size;

	/* The suite and the compression method. */
	hello->suite = (uint16_t)sslc_reader_u16(&reader);
	hello->compression = sslc_reader_u8(&reader);
	if (reader.error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);

	/* A HelloRetryRequest is a ServerHello with one fixed random. */
	same = memcmp(hello->random, sslc_tls_hrr_random, sizeof(hello->random));
	if (same == 0)
		hello->is_retry = 1;

	/* No extensions block at all is a valid TLS 1.2 ServerHello. */
	left = sslc_reader_left(&reader);
	if (left == 0U)
		return 0;

	/* The extensions block ends the body. */
	error = sslc_reader_vector(&reader, 2, 0, SSLC_MSG_MAX_U16, &block);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
	error = sslc_reader_finish(&reader);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);

	/* Takes in each extension in turn, stopping at the first that is not acceptable. */
	left = sslc_reader_left(&block);
	while (left > 0U) {
		error = sslc_msg_next_extension(&block, &type, &data);
		if (error != 0)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
		error = sslc_msg_hello_extension(hello, type, &data, alert);
		if (error != 0)
			return error;
		left = sslc_reader_left(&block);
	}

	/* Every extension was well formed and allowed. */
	return 0;
}

/*
 * Parses an EncryptedExtensions body (4.3.1): an extensions block, each
 * well formed, no type twice; allowed: server_name (empty) and
 * supported_groups (a list, ignored, 4.2.7); another extension the client
 * sends in its ClientHello is illegal_parameter (4.2: recognized, not
 * specified for this message), any other unsupported_extension.
 */
int
sslc_msg_parse_encrypted_extensions(
	const uint8_t *body,
	size_t size,
	int *server_name_ack,
	unsigned *alert)
{
	struct sslc_reader reader;
	struct sslc_reader block;
	struct sslc_reader data;
	struct sslc_reader groups;
	uint32_t type;
	size_t left;
	int seen_name;
	int seen_groups;
	int error;

	/* Starts with nothing found and no alert. */
	*server_name_ack = 0;
	*alert = 0;
	seen_name = 0;
	seen_groups = 0;

	/* The extensions block is the whole body. */
	sslc_reader_init(&reader, body, size);
	error = sslc_reader_vector(&reader, 2, 0, SSLC_MSG_MAX_U16, &block);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
	error = sslc_reader_finish(&reader);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);

	/* Takes in each extension in turn, stopping at the first that is not acceptable. */
	left = sslc_reader_left(&block);
	while (left > 0U) {
		error = sslc_msg_next_extension(&block, &type, &data);
		if (error != 0)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);

		/* An extension that has no place here (one the client offered) or that the client did not offer. */
		if (type != SSLC_EXT_SERVER_NAME && type != SSLC_EXT_SUPPORTED_GROUPS)
			return sslc_msg_fail(alert, sslc_msg_misplaced_alert(type), EPROTO);

		/* The server_name acknowledgement carries no data. */
		if (type == SSLC_EXT_SERVER_NAME) {
			if (seen_name)
				return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
			seen_name = 1;
			error = sslc_reader_finish(&data);
			if (error != 0)
				return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
			*server_name_ack = 1;
			left = sslc_reader_left(&block);
			continue;
		}

		/* The supported_groups list: two bytes a group, at least one group. */
		if (seen_groups)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
		seen_groups = 1;
		error = sslc_reader_vector(&data, 2, 2, SSLC_MSG_MAX_U16, &groups);
		if (error != 0)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
		error = sslc_reader_finish(&data);
		if (error != 0)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
		if (groups.size % 2U != 0U)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
		left = sslc_reader_left(&block);
	}

	/* Every extension was well formed and allowed. */
	return 0;
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
	struct sslc_reader reader;
	struct sslc_reader context;
	struct sslc_reader list;
	struct sslc_reader entry;
	struct sslc_reader extensions;
	size_t left;
	int error;

	/* Starts with nothing found and no alert. */
	memset(message, 0, sizeof(*message));
	*alert = 0;
	sslc_reader_init(&reader, body, size);

	/* TLS 1.3 begins with the request context, which is empty for a server's Certificate. */
	if (tls13) {
		error = sslc_reader_vector(&reader, 1, 0, SSLC_MSG_MAX_U8, &context);
		if (error != 0)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
		message->context = context.data;
		message->context_size = context.size;
	}

	/* The certificate list is the rest of the body. */
	error = sslc_reader_vector(&reader, 3, 0, SSLC_MSG_MAX_U24, &list);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
	error = sslc_reader_finish(&reader);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);

	/* A server that sends no certificate breaks the protocol. */
	if (list.size == 0U)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);

	/* Takes in each certificate in turn. */
	left = sslc_reader_left(&list);
	while (left > 0U) {
		/* A chain longer than the verifier keeps. */
		if (message->count >= CRYC_CHAIN_MAX_PEER)
			return sslc_msg_fail(alert, SSLC_ALERT_BAD_CERTIFICATE, EPROTO);

		/* The certificate's DER, at least one byte. */
		error = sslc_reader_vector(&list, 3, 1, SSLC_MSG_MAX_U24, &entry);
		if (error != 0)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
		message->der[message->count] = entry.data;
		message->der_size[message->count] = entry.size;
		message->count++;

		/* A TLS 1.3 entry has an extensions block, which must be empty here. */
		if (tls13) {
			error = sslc_reader_vector(&list, 2, 0, SSLC_MSG_MAX_U16, &extensions);
			if (error != 0)
				return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
			error = sslc_msg_skip_extensions(&extensions, alert);
			if (error != 0)
				return error;
			if (extensions.size != 0U)
				return sslc_msg_fail(alert, SSLC_ALERT_UNSUPPORTED_EXTENSION, EPROTO);
		}
		left = sslc_reader_left(&list);
	}

	/* The whole list was read. */
	return 0;
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
	struct sslc_reader reader;
	struct sslc_reader inner;
	uint32_t code;
	int error;

	/* Starts with nothing found and no alert. */
	*scheme = 0;
	*signature = NULL;
	*signature_size = 0;
	*alert = 0;
	sslc_reader_init(&reader, body, size);

	/* The signature scheme. */
	code = sslc_reader_u16(&reader);
	if (reader.error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);

	/* The signature, which ends the body. */
	error = sslc_reader_vector(&reader, 2, 0, SSLC_MSG_MAX_U16, &inner);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
	error = sslc_reader_finish(&reader);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);

	/* Hands the fields to the caller. */
	*scheme = (uint16_t)code;
	*signature = inner.data;
	*signature_size = inner.size;
	return 0;
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
	struct sslc_reader reader;
	struct sslc_reader point;
	struct sslc_reader signature;
	uint32_t curve_type;
	int error;

	/* Starts with nothing found and no alert. */
	memset(exchange, 0, sizeof(*exchange));
	*alert = 0;
	sslc_reader_init(&reader, body, size);

	/* The curve type: only a named curve is spoken. */
	curve_type = sslc_reader_u8(&reader);
	if (reader.error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
	if (curve_type != SSLC_MSG_CURVE_NAMED)
		return sslc_msg_fail(alert, SSLC_ALERT_ILLEGAL_PARAMETER, EPROTO);

	/* The group and the server's public point. */
	exchange->group = (uint16_t)sslc_reader_u16(&reader);
	if (reader.error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
	error = sslc_reader_vector(&reader, 1, 1, SSLC_MSG_MAX_U8, &point);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
	exchange->point = point.data;
	exchange->point_size = point.size;

	/* The bytes of the ServerECDHParams, which the signature covers. */
	exchange->params = body;
	exchange->params_size = reader.position;

	/* The scheme and the signature, which end the body. */
	exchange->scheme = (uint16_t)sslc_reader_u16(&reader);
	if (reader.error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
	error = sslc_reader_vector(&reader, 2, 0, SSLC_MSG_MAX_U16, &signature);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
	exchange->signature = signature.data;
	exchange->signature_size = signature.size;
	error = sslc_reader_finish(&reader);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);

	/* The whole body was read. */
	return 0;
}

/*
 * Checks a CertificateRequest body (4.3.2 when tls13 is 1: context<0..
 * 2^8-1>, extensions<2..2^16-1>; RFC 5246 7.4.4 otherwise) for syntax
 * only and gives the TLS 1.3 context (a view) to echo in the empty
 * Certificate (4.4.2).
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
	struct sslc_reader reader;
	struct sslc_reader inner;
	struct sslc_reader names;
	struct sslc_reader name;
	size_t left;
	int error;

	/* Starts with nothing found and no alert. */
	*context = NULL;
	*context_size = 0;
	*alert = 0;
	sslc_reader_init(&reader, body, size);

	/* TLS 1.3: the context, then the extensions, which are only checked for their syntax. */
	if (tls13) {
		error = sslc_reader_vector(&reader, 1, 0, SSLC_MSG_MAX_U8, &inner);
		if (error != 0)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
		*context = inner.data;
		*context_size = inner.size;
		error = sslc_reader_vector(&reader, 2, 2, SSLC_MSG_MAX_U16, &inner);
		if (error != 0)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
		error = sslc_msg_skip_extensions(&inner, alert);
		if (error != 0)
			return error;
		error = sslc_reader_finish(&reader);
		if (error != 0)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
		return 0;
	}

	/* TLS 1.2: the certificate types, at least one. */
	error = sslc_reader_vector(&reader, 1, 1, SSLC_MSG_MAX_U8, &inner);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);

	/* The signature and hash algorithm pairs: two bytes each, at least one. */
	error = sslc_reader_vector(&reader, 2, 2, SSLC_MSG_MAX_U16, &inner);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
	if (inner.size % 2U != 0U)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);

	/* The acceptable authorities: a list of names of at least one byte each. */
	error = sslc_reader_vector(&reader, 2, 0, SSLC_MSG_MAX_U16, &names);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
	left = sslc_reader_left(&names);
	while (left > 0U) {
		error = sslc_reader_vector(&names, 2, 1, SSLC_MSG_MAX_U16, &name);
		if (error != 0)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
		left = sslc_reader_left(&names);
	}

	/* The body ends there. */
	error = sslc_reader_finish(&reader);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
	return 0;
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
	struct sslc_reader reader;
	struct sslc_reader inner;
	int error;

	/* Starts with no alert. */
	*alert = 0;
	sslc_reader_init(&reader, body, size);

	/* The lifetime and the age add, which are not used. */
	(void)sslc_reader_u32(&reader);
	(void)sslc_reader_u32(&reader);
	if (reader.error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);

	/* The nonce and the ticket, whose bytes are not kept. */
	error = sslc_reader_vector(&reader, 1, 0, SSLC_MSG_MAX_U8, &inner);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
	error = sslc_reader_vector(&reader, 2, 1, SSLC_MSG_MAX_U16, &inner);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);

	/* The extensions, whose ceiling is one less than their prefix allows. */
	error = sslc_reader_vector(&reader, 2, 0, SSLC_MSG_MAX_U16 - 1U, &inner);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
	error = sslc_msg_skip_extensions(&inner, alert);
	if (error != 0)
		return error;

	/* The body ends there. */
	error = sslc_reader_finish(&reader);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
	return 0;
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
	/* Starts with nothing found and no alert. */
	*update_requested = 0;
	*alert = 0;

	/* The body is exactly one byte. */
	if (size != 1U)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);

	/* Only the two defined values are legal. */
	if (body[0] != SSLC_MSG_UPDATE_NOT_REQUESTED && body[0] != SSLC_MSG_UPDATE_REQUESTED)
		return sslc_msg_fail(alert, SSLC_ALERT_ILLEGAL_PARAMETER, EPROTO);

	/* Tells the caller whether the server wants the client's keys updated too. */
	*update_requested = (int)body[0];
	return 0;
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
	struct sslc_writer writer;

	/* Refuses data that cannot be a message body. */
	*written = 0;
	if (verify_size > SSLC_MSG_MAX_U24)
		return EINVAL;
	if (verify_data == NULL && verify_size != 0U)
		return EINVAL;

	/* The header and the verify_data. */
	sslc_writer_init(&writer, out, room);
	sslc_writer_u8(&writer, SSLC_HS_FINISHED);
	sslc_writer_begin(&writer, 3);
	sslc_writer_bytes(&writer, verify_data, verify_size);
	sslc_writer_end(&writer);

	/* Reports the size. */
	return sslc_msg_end_write(&writer, written);
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
	struct sslc_writer writer;

	/* Refuses a context that does not fit its 1-byte length. */
	*written = 0;
	if (tls13 && context_size > SSLC_MSG_MAX_U8)
		return EINVAL;
	if (tls13 && context == NULL && context_size != 0U)
		return EINVAL;

	/* The header. */
	sslc_writer_init(&writer, out, room);
	sslc_writer_u8(&writer, SSLC_HS_CERTIFICATE);
	sslc_writer_begin(&writer, 3);

	/* TLS 1.3 echoes the request context first. */
	if (tls13) {
		sslc_writer_begin(&writer, 1);
		sslc_writer_bytes(&writer, context, context_size);
		sslc_writer_end(&writer);
	}

	/* The certificate list, empty. */
	sslc_writer_begin(&writer, 3);
	sslc_writer_end(&writer);

	/* Closes the message and reports its size. */
	sslc_writer_end(&writer);
	return sslc_msg_end_write(&writer, written);
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
	struct sslc_writer writer;

	/* Refuses a point that cannot be written. */
	*written = 0;
	if (point == NULL)
		return EINVAL;
	if (point_size < 1U || point_size > SSLC_MSG_MAX_U8)
		return EINVAL;

	/* The header and the point. */
	sslc_writer_init(&writer, out, room);
	sslc_writer_u8(&writer, SSLC_HS_CLIENT_KEY_EXCHANGE);
	sslc_writer_begin(&writer, 3);
	sslc_writer_begin(&writer, 1);
	sslc_writer_bytes(&writer, point, point_size);
	sslc_writer_end(&writer);
	sslc_writer_end(&writer);

	/* Reports the size. */
	return sslc_msg_end_write(&writer, written);
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
	struct sslc_writer writer;

	/* Refuses a value that is neither defined one. */
	*written = 0;
	if (update_requested != 0 && update_requested != 1)
		return EINVAL;

	/* The header and the one byte. */
	sslc_writer_init(&writer, out, room);
	sslc_writer_u8(&writer, SSLC_HS_KEY_UPDATE);
	sslc_writer_begin(&writer, 3);
	sslc_writer_u8(&writer, (uint32_t)update_requested);
	sslc_writer_end(&writer);

	/* Reports the size. */
	return sslc_msg_end_write(&writer, written);
}

/*
 * Sets the alert to send and returns the errno value that goes with it.
 */
static int
sslc_msg_fail(
	unsigned *alert,
	unsigned description,
	int error)
{
	/* Tells the caller which alert to send. */
	*alert = description;

	/* The errno value stands for the kind of the error. */
	return error;
}

/*
 * Reads one extension (its type and its data as a reader) from an
 * extensions block.
 */
static int
sslc_msg_next_extension(
	struct sslc_reader *block,
	uint32_t *type,
	struct sslc_reader *data)
{
	int error;

	/* The type, then the data with its 2-byte length; a cut-off type fails the data's read too. */
	*type = sslc_reader_u16(block);
	error = sslc_reader_vector(block, 2, 0, SSLC_MSG_MAX_U16, data);
	if (error != 0)
		return EINVAL;
	return 0;
}

/*
 * Checks the syntax of an extensions block whose extensions are not used:
 * each is a type and data that fits.
 */
static int
sslc_msg_skip_extensions(
	const struct sslc_reader *block,
	unsigned *alert)
{
	struct sslc_reader copy;
	struct sslc_reader data;
	uint32_t type;
	size_t left;
	int error;

	/* Walks a copy, so that the block itself is left whole. */
	copy = *block;
	left = sslc_reader_left(&copy);
	while (left > 0U) {
		error = sslc_msg_next_extension(&copy, &type, &data);
		if (error != 0)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
		left = sslc_reader_left(&copy);
	}

	/* Every extension fits its length. */
	return 0;
}

/*
 * Checks a ClientHello's fields for the inconsistencies its writer
 * refuses: a version outside the two spoken, no suite, group or scheme,
 * a list too long for its length field, a session id over 32 bytes, a
 * share or a cookie without TLS 1.3, an empty or oversize key, and a
 * server name that is empty or over CRYC_NAME_HOST_MAX.
 */
static int
sslc_msg_check_client_hello(
	const struct sslc_client_hello *hello)
{
	size_t index;
	size_t length;

	/* The versions: each of the two, the lower not above the higher. */
	if (hello == NULL)
		return EINVAL;
	if (hello->min_version != SSLC_TLS12 && hello->min_version != SSLC_TLS13)
		return EINVAL;
	if (hello->max_version != SSLC_TLS12 && hello->max_version != SSLC_TLS13)
		return EINVAL;
	if (hello->min_version > hello->max_version)
		return EINVAL;

	/* The session id. */
	if (hello->session_id_size > SSLC_MSG_SESSION_ID_MAX)
		return EINVAL;

	/* The suites: at least one, as many as the length field holds. */
	if (hello->suites == NULL || hello->suite_count == 0U || hello->suite_count > SSLC_MSG_LIST_MAX)
		return EINVAL;

	/* The groups. */
	if (hello->groups == NULL || hello->group_count == 0U || hello->group_count > SSLC_MSG_LIST_MAX)
		return EINVAL;

	/* The signature schemes. */
	if (hello->schemes == NULL || hello->scheme_count == 0U || hello->scheme_count > SSLC_MSG_LIST_MAX)
		return EINVAL;

	/* The key shares need TLS 1.3 and a non-empty key each. */
	if (hello->share_count > SSLC_MSG_MAX_SHARES)
		return EINVAL;
	if (hello->share_count > 0U && hello->max_version != SSLC_TLS13)
		return EINVAL;
	for (index = 0; index < hello->share_count; index++) {
		if (hello->shares[index].key == NULL)
			return EINVAL;
		if (hello->shares[index].key_size < 1U || hello->shares[index].key_size > SSLC_MSG_MAX_U16)
			return EINVAL;
	}

	/* The cookie needs TLS 1.3 and is bounded like the one a server may send. */
	if (hello->cookie_size > 0U) {
		if (hello->cookie == NULL || hello->max_version != SSLC_TLS13)
			return EINVAL;
		if (hello->cookie_size > SSLC_MSG_COOKIE_MAX)
			return EINVAL;
	}

	/* The host name: counted one byte at a time up to one past the limit. */
	if (hello->server_name != NULL) {
		length = 0;
		while (length <= CRYC_NAME_HOST_MAX && hello->server_name[length] != '\0')
			length++;
		if (length < 1U || length > CRYC_NAME_HOST_MAX)
			return EINVAL;
	}

	/* The fields agree. */
	return 0;
}

/*
 * Writes the extensions of a ClientHello into the open extensions block,
 * in the order the header gives.
 */
static void
sslc_msg_write_extensions(
	struct sslc_writer *writer,
	const struct sslc_client_hello *hello,
	size_t name_size)
{
	size_t index;
	int offer12;
	int offer13;

	/* Which versions are offered decides which extensions are sent. */
	offer12 = hello->min_version == SSLC_TLS12;
	offer13 = hello->max_version == SSLC_TLS13;

	/* server_name: one host_name entry (an address sends none). */
	if (hello->server_name != NULL) {
		sslc_writer_u16(writer, SSLC_EXT_SERVER_NAME);
		sslc_writer_begin(writer, 2);
		sslc_writer_begin(writer, 2);
		sslc_writer_u8(writer, SSLC_MSG_NAME_TYPE_HOST_NAME);
		sslc_writer_begin(writer, 2);
		sslc_writer_bytes(writer, (const uint8_t *)hello->server_name, name_size);
		sslc_writer_end(writer);
		sslc_writer_end(writer);
		sslc_writer_end(writer);
	}

	/* supported_groups. */
	sslc_writer_u16(writer, SSLC_EXT_SUPPORTED_GROUPS);
	sslc_writer_begin(writer, 2);
	sslc_writer_begin(writer, 2);
	for (index = 0; index < hello->group_count; index++)
		sslc_writer_u16(writer, hello->groups[index]);
	sslc_writer_end(writer);
	sslc_writer_end(writer);

	/* TLS 1.2: the point format list with uncompressed alone, and the extended master secret. */
	if (offer12) {
		sslc_writer_u16(writer, SSLC_EXT_EC_POINT_FORMATS);
		sslc_writer_begin(writer, 2);
		sslc_writer_begin(writer, 1);
		sslc_writer_u8(writer, SSLC_MSG_POINT_UNCOMPRESSED);
		sslc_writer_end(writer);
		sslc_writer_end(writer);
		sslc_writer_u16(writer, SSLC_EXT_EXTENDED_MASTER_SECRET);
		sslc_writer_begin(writer, 2);
		sslc_writer_end(writer);
	}

	/* signature_algorithms. */
	sslc_writer_u16(writer, SSLC_EXT_SIGNATURE_ALGORITHMS);
	sslc_writer_begin(writer, 2);
	sslc_writer_begin(writer, 2);
	for (index = 0; index < hello->scheme_count; index++)
		sslc_writer_u16(writer, hello->schemes[index]);
	sslc_writer_end(writer);
	sslc_writer_end(writer);

	/* TLS 1.3: supported_versions with the highest first. */
	if (offer13) {
		sslc_writer_u16(writer, SSLC_EXT_SUPPORTED_VERSIONS);
		sslc_writer_begin(writer, 2);
		sslc_writer_begin(writer, 1);
		sslc_writer_u16(writer, SSLC_TLS13);
		if (offer12)
			sslc_writer_u16(writer, SSLC_TLS12);
		sslc_writer_end(writer);
		sslc_writer_end(writer);
	}

	/* The cookie of a HelloRetryRequest, echoed. */
	if (hello->cookie_size > 0U) {
		sslc_writer_u16(writer, SSLC_EXT_COOKIE);
		sslc_writer_begin(writer, 2);
		sslc_writer_begin(writer, 2);
		sslc_writer_bytes(writer, hello->cookie, hello->cookie_size);
		sslc_writer_end(writer);
		sslc_writer_end(writer);
	}

	/* TLS 1.3: key_share with the shares offered (none when a retry is wanted). */
	if (offer13) {
		sslc_writer_u16(writer, SSLC_EXT_KEY_SHARE);
		sslc_writer_begin(writer, 2);
		sslc_writer_begin(writer, 2);
		for (index = 0; index < hello->share_count; index++) {
			sslc_writer_u16(writer, hello->shares[index].group);
			sslc_writer_begin(writer, 2);
			sslc_writer_bytes(writer, hello->shares[index].key, hello->shares[index].key_size);
			sslc_writer_end(writer);
		}
		sslc_writer_end(writer);
		sslc_writer_end(writer);
	}
}

/*
 * Takes in one extension of a ServerHello or HelloRetryRequest: checks
 * that it is one of the allowed ones, that it is not repeated, and that its
 * data is well formed, and records it.
 */
static int
sslc_msg_hello_extension(
	struct sslc_server_hello *hello,
	uint32_t type,
	struct sslc_reader *data,
	unsigned *alert)
{
	struct sslc_reader inner;
	uint32_t bit;
	size_t index;
	int found;
	int error;

	/* Which extension this is; the cookie is recorded in either message, and refused by the handshake outside a HelloRetryRequest. */
	bit = 0;
	if (type == SSLC_EXT_SUPPORTED_VERSIONS)
		bit = SSLC_SEEN_SUPPORTED_VERSIONS;
	else if (type == SSLC_EXT_KEY_SHARE)
		bit = SSLC_SEEN_KEY_SHARE;
	else if (type == SSLC_EXT_COOKIE)
		bit = SSLC_SEEN_COOKIE;
	else if (type == SSLC_EXT_EXTENDED_MASTER_SECRET)
		bit = SSLC_SEEN_EXTENDED_MASTER;
	else if (type == SSLC_EXT_RENEGOTIATION_INFO)
		bit = SSLC_SEEN_RENEGOTIATION_INFO;
	else if (type == SSLC_EXT_EC_POINT_FORMATS)
		bit = SSLC_SEEN_EC_POINT_FORMATS;
	else if (type == SSLC_EXT_SERVER_NAME)
		bit = SSLC_SEEN_SERVER_NAME;

	/* Any other extension has no place in this message (one the client offered) or was not asked for. */
	if (bit == 0U)
		return sslc_msg_fail(alert, sslc_msg_misplaced_alert(type), EPROTO);

	/* An extension may appear once. */
	if ((hello->seen & bit) != 0U)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
	hello->seen |= bit;

	/* supported_versions: the one version the server chose. */
	if (type == SSLC_EXT_SUPPORTED_VERSIONS) {
		hello->selected_version = (uint16_t)sslc_reader_u16(data);
		error = sslc_reader_finish(data);
		if (error != 0)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
		return 0;
	}

	/* key_share: the group alone in a HelloRetryRequest, else the group and the server's key. */
	if (type == SSLC_EXT_KEY_SHARE) {
		hello->share_group = (uint16_t)sslc_reader_u16(data);
		if (!hello->is_retry) {
			error = sslc_reader_vector(data, 2, 1, SSLC_MSG_MAX_U16, &inner);
			if (error != 0)
				return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
			hello->share_key = inner.data;
			hello->share_size = inner.size;
		}
		error = sslc_reader_finish(data);
		if (error != 0)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
		return 0;
	}

	/* cookie: one or more bytes, not longer than a client will echo. */
	if (type == SSLC_EXT_COOKIE) {
		error = sslc_reader_vector(data, 2, 1, SSLC_MSG_MAX_U16, &inner);
		if (error != 0)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
		error = sslc_reader_finish(data);
		if (error != 0)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
		if (inner.size > SSLC_MSG_COOKIE_MAX)
			return sslc_msg_fail(alert, SSLC_ALERT_ILLEGAL_PARAMETER, EPROTO);
		hello->cookie = inner.data;
		hello->cookie_size = inner.size;
		return 0;
	}

	/* renegotiation_info: the renegotiated_connection bytes. */
	if (type == SSLC_EXT_RENEGOTIATION_INFO) {
		error = sslc_reader_vector(data, 1, 0, SSLC_MSG_MAX_U8, &inner);
		if (error != 0)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
		error = sslc_reader_finish(data);
		if (error != 0)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
		hello->renegotiation_info = inner.data;
		hello->renegotiation_info_size = inner.size;
		return 0;
	}

	/* ec_point_formats: a list of one or more formats that includes uncompressed. */
	if (type == SSLC_EXT_EC_POINT_FORMATS) {
		error = sslc_reader_vector(data, 1, 1, SSLC_MSG_MAX_U8, &inner);
		if (error != 0)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
		error = sslc_reader_finish(data);
		if (error != 0)
			return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
		found = 0;
		for (index = 0; index < inner.size; index++) {
			if (inner.data[index] == SSLC_MSG_POINT_UNCOMPRESSED)
				found = 1;
		}
		if (!found)
			return sslc_msg_fail(alert, SSLC_ALERT_ILLEGAL_PARAMETER, EPROTO);
		return 0;
	}

	/* extended_master_secret and server_name: the server's answer carries no data. */
	error = sslc_reader_finish(data);
	if (error != 0)
		return sslc_msg_fail(alert, SSLC_ALERT_DECODE_ERROR, EINVAL);
	return 0;
}

/*
 * Ends a message: reports the writer's state and how many bytes were
 * written.
 */
static int
sslc_msg_end_write(
	const struct sslc_writer *writer,
	size_t *written)
{
	int error;

	/* A message is whole only when nothing failed and every vector is closed. */
	error = sslc_writer_finish(writer);
	if (error != 0) {
		*written = 0;
		return error;
	}

	/* The size of the message, header included. */
	*written = writer->used;
	return 0;
}

/*
 * Gives the alert for an extension a server message may not carry (RFC
 * 8446 4.2): one this client sends in a ClientHello is recognized and not
 * specified for the message (illegal_parameter); any other was never
 * offered (unsupported_extension).
 */
static unsigned
sslc_msg_misplaced_alert(
	uint32_t type)
{
	/* The extensions the ClientHello carries (sslc_msg_write_client_hello). */
	switch (type) {
	case SSLC_EXT_SERVER_NAME:
	case SSLC_EXT_SUPPORTED_GROUPS:
	case SSLC_EXT_EC_POINT_FORMATS:
	case SSLC_EXT_SIGNATURE_ALGORITHMS:
	case SSLC_EXT_EXTENDED_MASTER_SECRET:
	case SSLC_EXT_SUPPORTED_VERSIONS:
	case SSLC_EXT_COOKIE:
	case SSLC_EXT_KEY_SHARE:
	case SSLC_EXT_RENEGOTIATION_INFO:
		return SSLC_ALERT_ILLEGAL_PARAMETER;
	default:
		break;
	}

	/* Not offered. */
	return SSLC_ALERT_UNSUPPORTED_EXTENSION;
}
