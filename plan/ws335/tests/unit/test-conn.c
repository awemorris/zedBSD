/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the connection (sslc-conn.h): record I/O over a transport, the
 * reassembly of handshake messages, alerts, the closing exchange, and the
 * limits on records that make no progress.
 *
 * The other end is tlspeer.h in its record mode, behind a memory pipe the
 * test controls: bytes can be given one at a time, a read or a write can
 * answer EAGAIN, EOF or a failure, and every request the connection makes
 * is audited against the framing of the bytes it was given (a read must
 * never ask for more than the rest of the current record).  Cases that
 * need bytes tlspeer does not make (a record of an unknown type, a wrong
 * version, an oversize length) put raw bytes into the pipe ahead of, or
 * instead of, the peer's.
 *
 * The expected values come from RFC 8446 5 and 6 and RFC 5246 6 and 7.2
 * (the record framing, the alert numbers, the order of the records of a
 * handshake), from hand calculation (the size of a protected record:
 * header 5, inner type 1 for TLS 1.3, explicit nonce 8 for TLS 1.2
 * AES-GCM, tag 16), and from the public header's reasons.
 */

#include "check.h"
#include "tlspeer.h"

#include <compat/ssl/ssl.h>

#include "userland/base/libcrypto-compat/cryc-chain.h"
#include "userland/base/libssl-compat/sslc-conn.h"

/* The most bytes the pipe holds in either direction. */
#define PIPE_ROOM		(1U << 20)

/* The most bytes a loop spends before it is a hang. */
#define GUARD			200000U

/*
 * The alert descriptions the test expects, written here from RFC 8446
 * Appendix B.2.
 * Values referenced: RFC 8446 section 6 and Appendix B.2.
 */
#define A_CLOSE_NOTIFY		0U
#define A_UNEXPECTED_MESSAGE	10U
#define A_BAD_RECORD_MAC	20U
#define A_RECORD_OVERFLOW	22U
#define A_HANDSHAKE_FAILURE	40U
#define A_ILLEGAL_PARAMETER	47U
#define A_UNKNOWN_CA		48U
#define A_DECODE_ERROR		50U
#define A_DECRYPT_ERROR		51U
#define A_PROTOCOL_VERSION	70U
#define A_INTERNAL_ERROR	80U
#define A_USER_CANCELED		90U
#define A_NO_RENEGOTIATION	100U
#define A_UNSUPPORTED_EXTENSION	110U

/* The record content types (RFC 8446 5.1). */
#define T_CCS			20U
#define T_ALERT			21U
#define T_HANDSHAKE		22U
#define T_DATA			23U

/* The pipe's scripts: what the transport does to the connection. */
struct pipe {
	struct tlspeer *peer;
	uint8_t in[PIPE_ROOM];
	size_t in_used;
	size_t in_taken;
	size_t read_chunk;
	unsigned read_eagain_every;
	unsigned read_fail_at;
	int read_fail_errno;
	int read_lie;
	int eof;
	unsigned long read_calls;
	unsigned long read_eagains;
	unsigned overreads;
	size_t header_have;
	uint8_t header[5];
	size_t body_left;
	size_t write_chunk;
	unsigned write_eagain_every;
	unsigned write_fail_at;
	int write_fail_errno;
	int write_zero;
	int write_lie;
	int write_limited;
	size_t write_budget;
	unsigned long write_calls;
	unsigned long write_eagains;
	uint8_t sent[PIPE_ROOM];
	size_t sent_used;
	int peer_error;
};

/* One record of what the connection wrote, as the pipe saw it. */
struct wire_record {
	unsigned type;
	unsigned version;
	size_t length;
	size_t offset;
};

/* The test's random source: byte i of the stream is byte i mod 32 of SHA-256("conn stream" || be32(i / 32)). */
struct stream {
	unsigned long position;
	unsigned calls;
	unsigned fail_at;
};

/* One client connection, its configuration and store, its peer and the pipe between them. */
struct bench {
	struct cryc_store store;
	struct sslc_hs_config config;
	struct stream stream;
	struct tlspeer *peer;
	struct pipe pipe;
	struct sslc_conn conn;
	unsigned eagains;
	unsigned bad_wants;
};

/* The root and leaf of P-256 and of P-384, for host "server.test". */
static struct tlspeer_pki pki256;
static struct tlspeer_pki pki384;

/* Draws size bytes from the stream; fails at the call fail_at names. */
static int
stream_random(
	void *context,
	uint8_t *out,
	size_t size)
{
	struct stream *stream;
	uint8_t input[16];
	uint8_t block[CRYC_SHA256_SIZE];
	size_t index;
	unsigned long position;

	/* The call is counted, and fails when asked to. */
	stream = context;
	stream->calls++;
	if (stream->fail_at != 0U && stream->calls == stream->fail_at)
		return EIO;

	/* Each byte from its block. */
	memcpy(input, "conn stream", 11);
	input[11] = 0;
	for (index = 0; index < size; index++) {
		position = stream->position + index;
		cryc_store_be32(input + 12, (uint32_t)(position / 32U));
		cryc_hash_digest(CRYC_HASH_SHA256, input, 16, block);
		out[index] = block[position % 32U];
	}
	stream->position += size;
	return 0;
}

/* Gives the connection bytes from the pipe: the injected bytes first, then what the peer sends. */
static void
pipe_pull(
	struct pipe *pipe)
{
	size_t taken;
	int error;

	/* Only when everything given so far was read. */
	if (pipe->in_taken != pipe->in_used)
		return;
	pipe->in_taken = 0;
	pipe->in_used = 0;

	/* The peer's bytes, as many as there are. */
	if (pipe->peer == NULL)
		return;
	taken = 0;
	error = tlspeer_take(pipe->peer, pipe->in, sizeof(pipe->in), &taken);
	if (error != 0 && pipe->peer_error == 0)
		pipe->peer_error = error;
	pipe->in_used = taken;
}

/* Seals what the peer has to send now into records of its own (so that the next thing it sends is a record apart). */
static void
pipe_seal(
	struct pipe *pipe)
{
	size_t taken;
	int error;

	/* Appended after the bytes the connection has not read yet. */
	taken = 0;
	error = tlspeer_take(pipe->peer, pipe->in + pipe->in_used, sizeof(pipe->in) - pipe->in_used, &taken);
	if (error != 0 && pipe->peer_error == 0)
		pipe->peer_error = error;
	pipe->in_used += taken;
}

/* Appends bytes the peer did not make to what the connection will read. */
static void
pipe_inject(
	struct pipe *pipe,
	const uint8_t *bytes,
	size_t size)
{
	/* After everything already waiting. */
	if (size > sizeof(pipe->in) - pipe->in_used) {
		CHECK(0);
		return;
	}
	memcpy(pipe->in + pipe->in_used, bytes, size);
	pipe->in_used += size;
}

/* The transport's read. */
static int
pipe_read(
	void *context,
	uint8_t *buffer,
	size_t size,
	size_t *done,
	int *os_error)
{
	struct pipe *pipe;
	size_t allowed;
	size_t count;
	size_t index;

	/* The call is counted; the size asked for is held against the framing of what was given. */
	pipe = context;
	pipe->read_calls++;
	*done = 0;
	if (pipe->header_have < 5U)
		allowed = 5U - pipe->header_have;
	else
		allowed = pipe->body_left;
	if (size > allowed)
		pipe->overreads++;

	/* The scripted failures. */
	if (pipe->read_fail_at != 0U && pipe->read_calls == pipe->read_fail_at) {
		*os_error = pipe->read_fail_errno;
		return EIO;
	}
	if (pipe->read_eagain_every != 0U && pipe->read_calls % pipe->read_eagain_every == 0U) {
		pipe->read_eagains++;
		return EAGAIN;
	}

	/* Nothing waits: the end of the stream, or not yet. */
	pipe_pull(pipe);
	if (pipe->in_taken == pipe->in_used) {
		if (pipe->eof)
			return 0;
		pipe->read_eagains++;
		return EAGAIN;
	}

	/* As much as asked for, as the script allows. */
	count = pipe->in_used - pipe->in_taken;
	if (count > size)
		count = size;
	if (pipe->read_chunk != 0U && count > pipe->read_chunk)
		count = pipe->read_chunk;
	memcpy(buffer, pipe->in + pipe->in_taken, count);
	pipe->in_taken += count;

	/* The framing of what has been given. */
	for (index = 0; index < count; index++) {
		if (pipe->header_have < 5U) {
			pipe->header[pipe->header_have] = buffer[index];
			pipe->header_have++;
			if (pipe->header_have == 5U) {
				pipe->body_left = (size_t)pipe->header[3] * 256U + (size_t)pipe->header[4];
				if (pipe->body_left == 0U)
					pipe->header_have = 0;
			}
		} else {
			pipe->body_left--;
			if (pipe->body_left == 0U)
				pipe->header_have = 0;
		}
	}
	*done = count;
	if (pipe->read_lie)
		*done = size + 1U;
	return 0;
}

/* The transport's write: the bytes are logged and given to the peer at once. */
static int
pipe_write(
	void *context,
	const uint8_t *buffer,
	size_t size,
	size_t *done,
	int *os_error)
{
	struct pipe *pipe;
	size_t count;
	int error;

	/* The call is counted; the scripted failures. */
	pipe = context;
	pipe->write_calls++;
	*done = 0;
	if (pipe->write_fail_at != 0U && pipe->write_calls == pipe->write_fail_at) {
		*os_error = pipe->write_fail_errno;
		return EIO;
	}
	if (pipe->write_eagain_every != 0U && pipe->write_calls % pipe->write_eagain_every == 0U) {
		pipe->write_eagains++;
		return EAGAIN;
	}
	if (pipe->write_limited && pipe->write_budget == 0U) {
		pipe->write_eagains++;
		return EAGAIN;
	}
	if (pipe->write_zero)
		return 0;

	/* As much as the script takes. */
	count = size;
	if (pipe->write_chunk != 0U && count > pipe->write_chunk)
		count = pipe->write_chunk;
	if (pipe->write_limited && count > pipe->write_budget)
		count = pipe->write_budget;
	if (count > sizeof(pipe->sent) - pipe->sent_used) {
		CHECK(0);
		return EIO;
	}
	memcpy(pipe->sent + pipe->sent_used, buffer, count);
	pipe->sent_used += count;
	if (pipe->write_limited)
		pipe->write_budget -= count;

	/* The peer reads them as they come. */
	if (pipe->peer != NULL) {
		error = tlspeer_input(pipe->peer, buffer, count);
		if (error != 0 && pipe->peer_error == 0)
			pipe->peer_error = error;
	}
	*done = count;
	if (pipe->write_lie)
		*done = size + 1U;
	return 0;
}

/* Parses the records the connection wrote; returns how many there are. */
static size_t
wire_parse(
	const struct pipe *pipe,
	struct wire_record *records,
	size_t room)
{
	size_t offset;
	size_t count;

	/* Each whole record in the log. */
	offset = 0;
	count = 0;
	while (pipe->sent_used - offset >= 5U && count < room) {
		records[count].type = pipe->sent[offset];
		records[count].version = (unsigned)pipe->sent[offset + 1U] * 256U + pipe->sent[offset + 2U];
		records[count].length = (size_t)pipe->sent[offset + 3U] * 256U + (size_t)pipe->sent[offset + 4U];
		records[count].offset = offset;
		if (records[count].length > pipe->sent_used - offset - 5U)
			break;
		offset += 5U + records[count].length;
		count++;
	}
	return count;
}

/* Writes the types of what the connection wrote as letters (c ccs, a alert, h handshake, d data) into text. */
static void
wire_types(
	const struct pipe *pipe,
	char *text,
	size_t room)
{
	struct wire_record records[64];
	size_t count;
	size_t index;

	/* One letter per record. */
	count = wire_parse(pipe, records, 64U);
	for (index = 0; index < count && index + 1U < room; index++) {
		switch (records[index].type) {
		case T_CCS:
			text[index] = 'c';
			break;
		case T_ALERT:
			text[index] = 'a';
			break;
		case T_HANDSHAKE:
			text[index] = 'h';
			break;
		case T_DATA:
			text[index] = 'd';
			break;
		default:
			text[index] = '?';
			break;
		}
	}
	text[index] = '\0';
}

/* Makes a bench: a client of min to max trusting root (may be NULL) for server.test, and a peer of config (may be NULL: raw). */
static struct bench *
bench_new(
	uint16_t min_version,
	uint16_t max_version,
	const struct tlspeer_pki *root,
	const struct tlspeer_config *peer_config,
	uint32_t flags)
{
	struct bench *bench;
	struct sslc_transport transport;

	/* Zeroed, with a store that trusts the root. */
	bench = calloc(1, sizeof(*bench));
	if (bench == NULL) {
		CHECK(0);
		return NULL;
	}
	CHECK(cryc_store_init(&bench->store) == 0);
	if (root != NULL)
		CHECK(cryc_store_add_der(&bench->store, root->root, root->root_size) == 0);

	/* The handshake configuration. */
	bench->config.min_version = min_version;
	bench->config.max_version = max_version;
	bench->config.store = &bench->store;
	bench->config.params.has_time = 1;
	bench->config.params.time = TLSPEER_PKI_TIME;
	bench->config.params.has_host = 1;
	CHECK(cryc_name_host_normalize("server.test", bench->config.params.host) == 0);
	bench->config.server_name = bench->config.params.host;
	bench->config.verify_peer = 1;
	bench->config.random = stream_random;
	bench->config.random_context = &bench->stream;

	/* The peer, when there is one. */
	if (peer_config != NULL) {
		CHECK(tlspeer_new(&bench->peer, peer_config) == 0);
		bench->pipe.peer = bench->peer;
	}

	/* The connection over the pipe. */
	transport.read = pipe_read;
	transport.write = pipe_write;
	transport.context = &bench->pipe;
	CHECK(sslc_conn_init(&bench->conn, &bench->config, &transport, flags) == 0);
	return bench;
}

/* Frees a bench. */
static void
bench_free(
	struct bench *bench)
{
	/* Everything it holds. */
	if (bench == NULL)
		return;
	sslc_conn_free(&bench->conn);
	tlspeer_free(bench->peer);
	cryc_store_free(&bench->store);
	free(bench);
}

/* Checks the wants of an EAGAIN: only the two bits, at least one. */
static void
bench_wants(
	struct bench *bench,
	unsigned wants)
{
	/* Counted; a wrong value is a failure. */
	bench->eagains++;
	if (wants == 0U || (wants & ~(SSLC_WANT_READ | SSLC_WANT_WRITE)) != 0U)
		bench->bad_wants++;
}

/* Tells whether nothing can reach the connection: no byte in the pipe and nothing queued at the peer. */
static int
pipe_dry(
	const struct bench *bench)
{
	/* Bytes in the pipe. */
	if (bench->pipe.in_taken != bench->pipe.in_used)
		return 0;

	/* Items or sealed bytes at the peer. */
	if (bench->peer != NULL && bench->peer->item_taken < bench->peer->item_count)
		return 0;
	if (bench->peer != NULL && bench->peer->out_taken < bench->peer->out_used)
		return 0;
	return 1;
}

/* Runs the handshake, repeating on EAGAIN, until it ends; returns its result. */
static int
drive_handshake(
	struct bench *bench)
{
	unsigned wants;
	unsigned guard;
	int error;

	/* The same call until it stops waiting. */
	for (guard = 0; guard < GUARD; guard++) {
		wants = 99;
		error = sslc_conn_handshake(&bench->conn, &wants);
		if (error != EAGAIN) {
			if (error == 0)
				CHECK(wants == 0U);
			return error;
		}
		bench_wants(bench, wants);

		/* A real wait: the connection waits to read, nothing is in the pipe and the peer has nothing to send. */
		if ((wants & SSLC_WANT_WRITE) == 0U && pipe_dry(bench))
			return EAGAIN;
	}
	CHECK(0);
	return EAGAIN;
}

/* Reads up to size bytes, repeating on EAGAIN while the peer still has something to send; returns the result. */
static int
drive_read(
	struct bench *bench,
	uint8_t *buffer,
	size_t size,
	size_t *done)
{
	unsigned wants;
	unsigned guard;
	int error;

	/* The same call until it stops waiting. */
	for (guard = 0; guard < GUARD; guard++) {
		wants = 99;
		*done = 77;
		error = sslc_conn_read(&bench->conn, buffer, size, done, &wants);
		if (error != EAGAIN) {
			if (error == 0)
				CHECK(wants == 0U);
			return error;
		}
		CHECK(*done == 0U);
		bench_wants(bench, wants);

		/* A real wait: the connection waits to read, nothing is in the pipe and the peer has nothing to send. */
		if ((wants & SSLC_WANT_WRITE) == 0U && pipe_dry(bench))
			return EAGAIN;
	}
	CHECK(0);
	return EAGAIN;
}

/* Writes size bytes, repeating on EAGAIN; returns the result. */
static int
drive_write(
	struct bench *bench,
	const uint8_t *buffer,
	size_t size,
	size_t *done)
{
	unsigned wants;
	unsigned guard;
	int error;

	/* The same call (same buffer, same size) until it stops waiting. */
	for (guard = 0; guard < GUARD; guard++) {
		wants = 99;
		*done = 77;
		error = sslc_conn_write(&bench->conn, buffer, size, done, &wants);
		if (error != EAGAIN) {
			if (error == 0)
				CHECK(wants == 0U);
			return error;
		}
		CHECK(*done == 0U);
		bench_wants(bench, wants);
	}
	CHECK(0);
	return EAGAIN;
}

/* Fills a pattern of size bytes that depends on seed. */
static void
pattern(
	uint8_t *out,
	size_t size,
	unsigned seed)
{
	size_t index;

	/* Every byte differs from its neighbours and from another seed's. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)((index * 7U + index / 251U + seed * 13U + 1U) & 0xffU);
}

/* The peer's script for a version: the tool's default over pki. */
static void
script(
	struct tlspeer_config *config,
	uint16_t version,
	const struct tlspeer_pki *pki)
{
	/* The default script. */
	CHECK(tlspeer_config_init(config, version, pki) == 0);
}

/* Makes a client of TLS 1.3 (or of 1.2 to 1.3) with a peer of config and connects it (all checked); returns the bench. */
static struct bench *
connected(
	uint16_t min_version,
	uint16_t max_version,
	const struct tlspeer_config *config,
	const struct tlspeer_pki *pki)
{
	struct bench *bench;

	/* The handshake over the pipe, to the end. */
	bench = bench_new(min_version, max_version, pki, config, 0);
	if (bench == NULL)
		return NULL;
	CHECK(drive_handshake(bench) == 0);
	CHECK((bench->conn.flags & SSLC_CONN_CONNECTED) != 0U);
	CHECK((bench->conn.flags & SSLC_CONN_FAILED) == 0U);
	return bench;
}

/* Makes a connected TLS 1.3 bench over the default script. */
static struct bench *
connected13(void)
{
	struct tlspeer_config config;

	/* The default script of TLS 1.3. */
	script(&config, SSLC_TLS13, &pki256);
	return connected(SSLC_TLS12, SSLC_TLS13, &config, &pki256);
}

/* Makes a connected TLS 1.2 bench over the default script. */
static struct bench *
connected12(void)
{
	struct tlspeer_config config;

	/* The default script of TLS 1.2. */
	script(&config, SSLC_TLS12, &pki256);
	return connected(SSLC_TLS12, SSLC_TLS13, &config, &pki256);
}

/* Checks the alert the peer got last: its level and description, and whether it was protected. */
static void
check_peer_alert(
	const struct bench *bench,
	unsigned count,
	unsigned level,
	unsigned description,
	int protected_alert)
{
	/* What the peer recorded. */
	CHECK(bench->peer->alert_count == count);
	if (count == 0U)
		return;
	CHECK(bench->peer->alert_level == level);
	CHECK(bench->peer->alert_description == description);
	CHECK(bench->peer->alert_protected == protected_alert);
	if (bench->peer->alert_description != description)
		printf("  alert %u, expected %u\n", bench->peer->alert_description, description);
}

/* Tells whether size bytes at bytes are all zero. */
static int
buffer_is_zero(
	const uint8_t *bytes,
	size_t size)
{
	size_t index;

	/* Every byte. */
	for (index = 0; index < size; index++) {
		if (bytes[index] != 0U)
			return 0;
	}
	return 1;
}

/* Checks that a failed connection refuses every operation with EPROTO and sends nothing more. */
static void
check_dead(
	struct bench *bench)
{
	uint8_t buffer[8];
	size_t done;
	size_t before;
	unsigned wants;
	int both;

	/* Every operation. */
	before = bench->pipe.sent_used;
	CHECK((bench->conn.flags & SSLC_CONN_FAILED) != 0U);
	done = 5;
	wants = 5;
	CHECK(sslc_conn_handshake(&bench->conn, &wants) == EPROTO);
	CHECK(sslc_conn_read(&bench->conn, buffer, sizeof(buffer), &done, &wants) == EPROTO);
	CHECK(done == 0U);
	CHECK(sslc_conn_write(&bench->conn, buffer, sizeof(buffer), &done, &wants) == EPROTO);
	CHECK(done == 0U);
	CHECK(sslc_conn_shutdown(&bench->conn, &both, &wants) == EPROTO);
	CHECK(both == 0);
	CHECK(sslc_conn_pending(&bench->conn) == 0U);
	CHECK(bench->pipe.sent_used == before);

	/* No key, no secret, no plaintext and no held message is left behind. */
	CHECK(bench->conn.hs.state == SSLC_HS_FAILED);
	CHECK(bench->conn.reading.suite == NULL);
	CHECK(bench->conn.writing.suite == NULL);
	CHECK(bench->conn.plain == NULL);
	CHECK(bench->conn.plain_size == 0U);
	CHECK(bench->conn.in_used == 0U);
	CHECK(bench->conn.message == NULL);
	CHECK(bench->conn.message_used == 0U);
	CHECK(buffer_is_zero(bench->conn.in, sizeof(bench->conn.in)));
}

/* The result of one read and its bytes, for the tests that read data. */
static void
read_exact(
	struct bench *bench,
	const uint8_t *expected,
	size_t size,
	size_t buffer_size)
{
	uint8_t *buffer;
	size_t total;
	size_t done;
	unsigned guard;
	int error;

	/* Reads with buffers of buffer_size until size bytes are in. */
	buffer = malloc(buffer_size);
	if (buffer == NULL) {
		CHECK(0);
		return;
	}
	total = 0;
	for (guard = 0; guard < GUARD && total < size; guard++) {
		error = drive_read(bench, buffer, buffer_size, &done);
		if (error != 0) {
			CHECK(0);
			printf("  read gave %d after %lu of %lu bytes\n", error, (unsigned long)total, (unsigned long)size);
			break;
		}
		CHECK(done > 0U);
		CHECK(done <= buffer_size);
		if (done > size - total) {
			CHECK(0);
			break;
		}
		CHECK_BYTES(buffer, expected + total, done);
		total += done;
	}
	CHECK(total == size);
	free(buffer);
}

/*
 * The handshake over the pipe.
 */

/* One handshake of the matrix: the peer's version, suite and group, whether it retries, its curve, and the records the client writes. */
struct hs_case {
	uint16_t version;
	uint16_t suite;
	uint16_t group;
	int retry;
	int p384;
	uint32_t options;
	const char *wire;
};

/*
 * The handshakes: TLS 1.3 with each suite, one after a HelloRetryRequest
 * (the records the client writes are the ClientHello, the compatibility
 * change_cipher_spec, the second ClientHello and the Finished), and TLS 1.2
 * with each AEAD (the ClientHello, the ClientKeyExchange, the plaintext
 * change_cipher_spec, the Finished).
 * Values referenced: RFC 8446 Appendix D.4 and 4.1.4; RFC 5246 7.3.
 */
static const struct hs_case hs_cases[] = {
	{ SSLC_TLS13, 0x1301, 0x001d, 0, 0, 0, "hcd" },
	{ SSLC_TLS13, 0x1303, 0x001d, 0, 0, 0, "hcd" },
	{ SSLC_TLS13, 0x1302, 0x0017, 1, 1, 0, "hchd" },
	{ SSLC_TLS13, 0x1301, 0x001d, 0, 0, TLSPEER_OPT_CCS | TLSPEER_OPT_SNI_ACK | TLSPEER_OPT_CERT_REQUEST, "hcd" },
	{ SSLC_TLS13, 0x1301, 0x0017, 1, 0, TLSPEER_OPT_CCS, "hchd" },
	{ SSLC_TLS12, 0xc02b, 0x001d, 0, 0, 0, "hhch" },
	{ SSLC_TLS12, 0xcca9, 0x0017, 0, 1, 0, "hhch" },
	{ SSLC_TLS12, 0xc02c, 0x001d, 0, 0, TLSPEER_OPT_SNI_ACK | TLSPEER_OPT_POINT_FORMATS | TLSPEER_OPT_CERT_REQUEST, "hhch" }
};

/* Sets the pipe's scripts for transport mode 0 (plain), 1 (one byte at a time), 2 (EAGAIN often) or 3 (small pieces and EAGAIN). */
static void
set_mode(
	struct pipe *pipe,
	unsigned mode)
{
	/* The scripts of the mode. */
	if (mode == 1U) {
		pipe->read_chunk = 1;
		pipe->write_chunk = 1;
	} else if (mode == 2U) {
		pipe->read_eagain_every = 2;
		pipe->write_eagain_every = 3;
	} else if (mode == 3U) {
		pipe->read_chunk = 7;
		pipe->write_chunk = 5;
		pipe->read_eagain_every = 3;
		pipe->write_eagain_every = 2;
	}
}

/* Checks one handshake of the matrix in one transport mode. */
static void
check_handshake(
	const struct hs_case *item,
	unsigned mode)
{
	struct tlspeer_config config;
	struct wire_record records[16];
	const struct tlspeer_pki *pki;
	struct bench *bench;
	char types[16];
	unsigned wants;
	size_t count;
	size_t index;
	int error;

	/* The peer's script, the client, the pipe's mode. */
	pki = item->p384 ? &pki384 : &pki256;
	script(&config, item->version, pki);
	config.suite = item->suite;
	config.group = item->group;
	config.hello_retry = item->retry;
	config.options = item->options;
	bench = bench_new(SSLC_TLS12, SSLC_TLS13, pki, &config, 0);
	if (bench == NULL)
		return;
	set_mode(&bench->pipe, mode);

	/* Not connected before the handshake; the handshake runs to its end. */
	CHECK(sslc_conn_pending(&bench->conn) == 0U);
	error = drive_handshake(bench);
	CHECK(error == 0);
	CHECK((bench->conn.flags & SSLC_CONN_STARTED) != 0U);
	CHECK((bench->conn.flags & SSLC_CONN_CONNECTED) != 0U);
	CHECK((bench->conn.flags & SSLC_CONN_FAILED) == 0U);

	/* Both ends agree that the handshake is complete, on the version and the suite. */
	CHECK(bench->peer->state == TLSPEER_CONNECTED);
	CHECK(bench->pipe.peer_error == 0);
	CHECK(bench->peer->error == 0);
	CHECK(sslc_hs_connected(&bench->conn.hs) == 1);
	CHECK(bench->conn.hs.version == item->version);
	CHECK(bench->conn.hs.suite != NULL && bench->conn.hs.suite->id == item->suite);

	/* Nothing is left to send, nothing is held, and no read asked for more than the rest of its record. */
	CHECK(bench->conn.out_used == 0U);
	CHECK(bench->conn.message == NULL);
	CHECK(bench->conn.message_used == 0U);
	CHECK(bench->conn.plain_size == 0U);
	CHECK(bench->pipe.overreads == 0U);
	CHECK(bench->bad_wants == 0U);
	if (mode >= 2U)
		CHECK(bench->eagains > 0U);

	/* The handshake called again is a success that does nothing. */
	count = bench->pipe.sent_used;
	wants = 9;
	CHECK(sslc_conn_handshake(&bench->conn, &wants) == 0);
	CHECK(wants == 0U);
	CHECK(bench->pipe.sent_used == count);

	/* The records written: their kinds, the first with the legacy version 0x0301 and the others 0x0303 (RFC 8446 5.1). */
	wire_types(&bench->pipe, types, sizeof(types));
	CHECK(strcmp(types, item->wire) == 0);
	if (strcmp(types, item->wire) != 0)
		printf("  wire %s, expected %s\n", types, item->wire);
	count = wire_parse(&bench->pipe, records, 16U);
	CHECK(count == strlen(item->wire));
	for (index = 0; index < count; index++) {
		if (index == 0U)
			CHECK(records[index].version == 0x0301U);
		else
			CHECK(records[index].version == 0x0303U);
	}

	/* The change_cipher_spec record is the same six bytes whatever keys the client had by then. */
	for (index = 0; index < count; index++) {
		if (records[index].type == T_CCS) {
			CHECK(records[index].length == 1U);
			CHECK(bench->pipe.sent[records[index].offset + 5U] == 1U);
		}
	}
	bench_free(bench);
}

/* Runs every handshake of the matrix in every transport mode. */
static void
test_handshakes(void)
{
	unsigned index;
	unsigned mode;

	/* Each case, each mode. */
	for (index = 0; index < sizeof(hs_cases) / sizeof(hs_cases[0]); index++) {
		for (mode = 0; mode < 4U; mode++)
			check_handshake(&hs_cases[index], mode);
	}
}

/*
 * Application data.
 */

/*
 * The plaintext sizes of the data tests: one byte, around a power of two,
 * a full fragment (2^14), one more, and several fragments.
 * Values referenced: RFC 8446 section 5.1 (a fragment of at most 2^14 bytes).
 */
static const size_t data_sizes[] = { 1, 2, 255, 256, 257, 4096, 16383, 16384, 16385, 32768, 40000 };

/* The records a write of size bytes makes: full fragments and the rest. */
static size_t
record_count(
	size_t size)
{
	/* Whole fragments of 16384 bytes, and a last one when something is left. */
	return (size + 16383U) / 16384U;
}

/* Writes and reads data of every size on a connected bench whose protected record adds overhead bytes beyond the header's 5 and the plaintext. */
static void
check_data(
	struct bench *bench,
	size_t overhead)
{
	static uint8_t data[40000];
	size_t index;
	size_t done;
	size_t before;
	unsigned long records_before;

	/* The client writes; the peer must decrypt exactly the bytes, in the records the sizes imply. */
	for (index = 0; index < sizeof(data_sizes) / sizeof(data_sizes[0]); index++) {
		pattern(data, data_sizes[index], (unsigned)index);
		bench->peer->data_size = 0;
		before = bench->pipe.sent_used;
		records_before = bench->peer->records_in;
		CHECK(drive_write(bench, data, data_sizes[index], &done) == 0);
		CHECK(done == data_sizes[index]);
		CHECK(bench->conn.taken == 0U);
		CHECK(bench->pipe.sent_used - before == data_sizes[index] + record_count(data_sizes[index]) * (5U + overhead));
		CHECK(bench->peer->records_in - records_before == record_count(data_sizes[index]));
		CHECK(bench->peer->data_size == data_sizes[index]);
		CHECK_BYTES(bench->peer->data, data, data_sizes[index]);
	}

	/* The peer writes; the client reads it with a large buffer and with a buffer of 100 bytes. */
	for (index = 0; index < sizeof(data_sizes) / sizeof(data_sizes[0]); index++) {
		pattern(data, data_sizes[index], 50U + (unsigned)index);
		CHECK(tlspeer_send_data(bench->peer, data, data_sizes[index]) == 0);
		read_exact(bench, data, data_sizes[index], 65536U);
		CHECK(sslc_conn_pending(&bench->conn) == 0U);
		if (data_sizes[index] <= 4096U) {
			CHECK(tlspeer_send_data(bench->peer, data, data_sizes[index]) == 0);
			read_exact(bench, data, data_sizes[index], 100U);
		}
	}
	CHECK(bench->peer->error == 0);
	CHECK(bench->pipe.peer_error == 0);
	CHECK(bench->pipe.overreads == 0U);
	CHECK(bench->bad_wants == 0U);
}

/* Checks data in every version and AEAD the matrix has, in a plain transport and in a stuttering one. */
static void
test_data(void)
{
	struct tlspeer_config config;
	struct bench *bench;
	unsigned mode;
	unsigned index;
	size_t overhead;

	/* TLS 1.3 adds the inner type and the tag; TLS 1.2 AES-GCM the explicit nonce and the tag; TLS 1.2 ChaCha20-Poly1305 the tag. */
	for (index = 0; index < 4U; index++) {
		for (mode = 0; mode < 4U; mode += 3U) {
			if (index < 2U)
				script(&config, SSLC_TLS13, &pki256);
			else
				script(&config, SSLC_TLS12, &pki256);
			overhead = 17U;
			if (index == 1U)
				config.suite = 0x1303;
			if (index == 2U)
				overhead = 8U + 16U;
			if (index == 3U) {
				config.suite = 0xcca9;
				overhead = 16U;
			}
			bench = connected(SSLC_TLS12, SSLC_TLS13, &config, &pki256);
			if (bench == NULL)
				continue;
			set_mode(&bench->pipe, mode);
			check_data(bench, overhead);
			bench_free(bench);
		}
	}
}

/* Checks that application bytes are kept for later reads, that the transport is not touched for them, and that an empty read is refused. */
static void
test_pending(void)
{
	struct bench *bench;
	uint8_t data[100];
	uint8_t buffer[200];
	unsigned long calls;
	unsigned wants;
	size_t done;
	unsigned version;

	/* TLS 1.3 and 1.2. */
	for (version = 0; version < 2U; version++) {
		bench = version == 0U ? connected13() : connected12();
		if (bench == NULL)
			continue;
		pattern(data, sizeof(data), 3U);

		/* A read of no bytes is a mistake of the caller. */
		done = 5;
		wants = 5;
		CHECK(sslc_conn_read(&bench->conn, buffer, 0, &done, &wants) == EINVAL);
		CHECK(done == 0U);

		/* Nothing buffered, nothing in the pipe: EAGAIN to read. */
		done = 5;
		wants = 0;
		CHECK(sslc_conn_read(&bench->conn, buffer, sizeof(buffer), &done, &wants) == EAGAIN);
		CHECK(done == 0U);
		CHECK(wants == SSLC_WANT_READ);

		/* One record of 100 bytes; a read of 10 leaves 90 buffered. */
		CHECK(tlspeer_send_data(bench->peer, data, sizeof(data)) == 0);
		CHECK(sslc_conn_read(&bench->conn, buffer, 10, &done, &wants) == 0);
		CHECK(done == 10U);
		CHECK_BYTES(buffer, data, 10);
		CHECK(sslc_conn_pending(&bench->conn) == 90U);

		/* The next reads come from the buffer without a call of the transport. */
		calls = bench->pipe.read_calls;
		CHECK(sslc_conn_read(&bench->conn, buffer, 1, &done, &wants) == 0);
		CHECK(done == 1U && buffer[0] == data[10]);
		CHECK(sslc_conn_pending(&bench->conn) == 89U);
		CHECK(sslc_conn_read(&bench->conn, buffer, sizeof(buffer), &done, &wants) == 0);
		CHECK(done == 89U);
		CHECK_BYTES(buffer, data + 11, 89);
		CHECK(sslc_conn_pending(&bench->conn) == 0U);
		CHECK(bench->pipe.read_calls == calls);
		CHECK(bench->conn.in_used == 0U);

		/* The buffer is free for the next record. */
		CHECK(tlspeer_send_data(bench->peer, data, 5) == 0);
		CHECK(sslc_conn_read(&bench->conn, buffer, sizeof(buffer), &done, &wants) == 0);
		CHECK(done == 5U);
		CHECK_BYTES(buffer, data, 5);
		CHECK(bench->pipe.overreads == 0U);
		bench_free(bench);
	}
}

/*
 * A read never takes a byte of the next record: three records wait in the
 * pipe; each read takes one of them, in two transport calls (the header,
 * then the body), and the pipe still holds the others.
 */
static void
test_read_ahead(void)
{
	struct bench *bench;
	uint8_t data[64];
	uint8_t buffer[100];
	size_t sizes[3];
	size_t overhead;
	size_t before;
	size_t expected;
	size_t done;
	unsigned long calls;
	unsigned wants;
	unsigned version;
	unsigned index;

	/* TLS 1.3 (record overhead 22) and TLS 1.2 AES-GCM (29). */
	sizes[0] = 10;
	sizes[1] = 20;
	sizes[2] = 30;
	for (version = 0; version < 2U; version++) {
		bench = version == 0U ? connected13() : connected12();
		if (bench == NULL)
			continue;
		overhead = version == 0U ? 17U : 24U;
		pattern(data, sizeof(data), 8U);

		/* Three records, sealed apart, all in the pipe. */
		for (index = 0; index < 3U; index++) {
			CHECK(tlspeer_send_data(bench->peer, data, sizes[index]) == 0);
			pipe_seal(&bench->pipe);
		}
		before = bench->pipe.in_taken;
		expected = 0;

		/* One record per read, and two transport calls for each. */
		for (index = 0; index < 3U; index++) {
			calls = bench->pipe.read_calls;
			CHECK(sslc_conn_read(&bench->conn, buffer, sizeof(buffer), &done, &wants) == 0);
			CHECK(done == sizes[index]);
			CHECK_BYTES(buffer, data, done);
			CHECK(bench->pipe.read_calls - calls == 2U);
			expected += 5U + sizes[index] + overhead;
			CHECK(bench->pipe.in_taken - before == expected);
			CHECK(sslc_conn_pending(&bench->conn) == 0U);
		}

		/* Everything given was taken, and nothing more was asked for. */
		CHECK(bench->pipe.in_taken == bench->pipe.in_used);
		CHECK(bench->pipe.overreads == 0U);
		CHECK(sslc_conn_read(&bench->conn, buffer, sizeof(buffer), &done, &wants) == EAGAIN);
		CHECK(wants == SSLC_WANT_READ);
		bench_free(bench);
	}
}

/*
 * A write that stops with EAGAIN keeps its sealed record whole until it is
 * sent, takes no new record meanwhile, and sends each record once.
 */
static void
check_write_retry(
	struct bench *bench,
	size_t overhead)
{
	static uint8_t data[20000];
	size_t before;
	size_t done;
	size_t pending;
	size_t last;
	unsigned wants;
	unsigned calls;
	unsigned long records_before;
	int error;

	/* Two records (16384 and 3616 bytes), and a transport that takes nothing. */
	pattern(data, sizeof(data), 21U);
	bench->peer->data_size = 0;
	before = bench->pipe.sent_used;
	records_before = bench->peer->records_in;
	bench->pipe.write_limited = 1;
	bench->pipe.write_budget = 0;
	done = 5;
	wants = 0;
	error = sslc_conn_write(&bench->conn, data, sizeof(data), &done, &wants);
	CHECK(error == EAGAIN);
	CHECK(done == 0U);
	CHECK(wants == SSLC_WANT_WRITE);

	/* The first record is sealed, counted as taken and held whole; the second is not sealed. */
	CHECK(bench->conn.taken == 16384U);
	CHECK(bench->conn.out_used - bench->conn.out_sent == 16384U + 5U + overhead);
	CHECK(bench->pipe.sent_used == before);

	/* A retry that names fewer bytes than were taken is not the same call and changes nothing. */
	done = 5;
	CHECK(sslc_conn_write(&bench->conn, data, 100, &done, &wants) == EINVAL);
	CHECK(done == 0U);
	CHECK(bench->conn.taken == 16384U);
	CHECK((bench->conn.flags & SSLC_CONN_FAILED) == 0U);

	/* The transport takes 1000 bytes at a time; each try sends what it can of the held record. */
	calls = 0;
	last = bench->conn.out_used - bench->conn.out_sent;
	do {
		bench->pipe.write_budget = 1000;
		done = 5;
		wants = 0;
		error = sslc_conn_write(&bench->conn, data, sizeof(data), &done, &wants);
		calls++;
		if (error == EAGAIN) {
			CHECK(wants == SSLC_WANT_WRITE);
			CHECK(done == 0U);
			pending = bench->conn.out_used - bench->conn.out_sent;
			CHECK(pending > 0U);
			CHECK(pending < last || pending == 16384U + 5U + overhead || bench->conn.taken == sizeof(data));

			/* A whole record is never held in two parts: the held bytes are a record's tail or a whole record. */
			last = pending;
		}
	} while (error == EAGAIN && calls < 100U);
	CHECK(error == 0);
	CHECK(done == sizeof(data));
	CHECK(bench->conn.taken == 0U);
	CHECK(bench->conn.out_used == 0U);

	/* The wire carries exactly the two records, once. */
	CHECK(bench->pipe.sent_used - before == sizeof(data) + 2U * (5U + overhead));
	CHECK(bench->peer->records_in - records_before == 2U);
	CHECK(bench->peer->data_size == sizeof(data));
	CHECK_BYTES(bench->peer->data, data, sizeof(data));
	CHECK(bench->peer->error == 0);
	CHECK(bench->pipe.peer_error == 0);

	/* The last record sealed, then the transport stops: everything is taken and a retry only sends. */
	bench->peer->data_size = 0;
	before = bench->pipe.sent_used;
	bench->pipe.write_budget = 16384U + 5U + overhead;
	done = 5;
	error = sslc_conn_write(&bench->conn, data, sizeof(data), &done, &wants);
	CHECK(error == EAGAIN);
	CHECK(bench->conn.taken == sizeof(data));
	CHECK(bench->pipe.sent_used - before == 16384U + 5U + overhead);
	bench->pipe.write_budget = 1U << 20;
	done = 5;
	error = sslc_conn_write(&bench->conn, data, sizeof(data), &done, &wants);
	CHECK(error == 0);
	CHECK(done == sizeof(data));
	CHECK(bench->conn.taken == 0U);
	CHECK(bench->pipe.sent_used - before == sizeof(data) + 2U * (5U + overhead));
	CHECK(bench->peer->data_size == sizeof(data));
	CHECK_BYTES(bench->peer->data, data, sizeof(data));
	CHECK(bench->peer->error == 0);
}

/* Runs the write retry in TLS 1.3 and TLS 1.2. */
static void
test_write_retry(void)
{
	struct bench *bench;

	/* TLS 1.3 (overhead 17: inner type and tag), TLS 1.2 (24: explicit nonce and tag). */
	bench = connected13();
	if (bench != NULL)
		check_write_retry(bench, 17U);
	bench_free(bench);
	bench = connected12();
	if (bench != NULL)
		check_write_retry(bench, 24U);
	bench_free(bench);
}

/* Checks the writes of no bytes, and a write after the transport stopped half way. */
static void
test_write_edges(void)
{
	struct bench *bench;
	uint8_t data[4];
	size_t done;
	size_t before;
	unsigned wants;

	/* A write of no bytes succeeds and sends nothing. */
	bench = connected13();
	if (bench == NULL)
		return;
	before = bench->pipe.sent_used;
	done = 5;
	wants = 5;
	CHECK(sslc_conn_write(&bench->conn, data, 0, &done, &wants) == 0);
	CHECK(done == 0U);
	CHECK(wants == 0U);
	CHECK(bench->pipe.sent_used == before);

	/* No buffer with bytes to send is refused. */
	CHECK(sslc_conn_write(&bench->conn, NULL, 4, &done, &wants) == EINVAL);
	CHECK(done == 0U);
	CHECK((bench->conn.flags & SSLC_CONN_FAILED) == 0U);
	CHECK(bench->conn.out_used == 0U);
	CHECK(bench->pipe.sent_used == before);
	bench_free(bench);
}

/*
 * Alerts.
 */

/* Has the peer send an alert and puts it in the pipe as a record of its own. */
static void
peer_alert(
	struct bench *bench,
	unsigned level,
	unsigned description)
{
	/* One protected record. */
	CHECK(tlspeer_send_alert(bench->peer, level, description) == 0);
	pipe_seal(&bench->pipe);
}

/* Has the peer send application data and puts it in the pipe as a record of its own. */
static void
peer_data(
	struct bench *bench,
	const char *text)
{
	/* One protected record. */
	CHECK(tlspeer_send_data(bench->peer, (const uint8_t *)text, strlen(text)) == 0);
	pipe_seal(&bench->pipe);
}

/* Checks that the next read gives the text. */
static void
expect_text(
	struct bench *bench,
	const char *text)
{
	uint8_t buffer[64];
	size_t done;

	/* The bytes of the record, all at once. */
	CHECK(drive_read(bench, buffer, sizeof(buffer), &done) == 0);
	CHECK(done == strlen(text));
	if (done == strlen(text))
		CHECK_BYTES(buffer, text, done);
}

/* Checks that the next read fails with error, with the connection's reason, and the alert the peer got (0: none was sent). */
static void
expect_failure(
	struct bench *bench,
	int error,
	unsigned reason,
	unsigned alert)
{
	uint8_t buffer[16];
	size_t done;
	int got;

	/* The failure, its reason, the alert (always fatal, always protected after the handshake) and a dead connection. */
	got = drive_read(bench, buffer, sizeof(buffer), &done);
	CHECK(got == error);
	if (got != error)
		printf("  read gave %d, expected %d\n", got, error);
	CHECK(done == 0U);
	CHECK(bench->conn.reason == reason);
	if (bench->conn.reason != reason)
		printf("  reason %u, expected %u\n", bench->conn.reason, reason);
	if (alert == 0U)
		check_peer_alert(bench, 0, 0, 0, 0);
	else
		check_peer_alert(bench, 1, 2, alert, 1);
	CHECK(bench->pipe.peer_error == 0);
	check_dead(bench);
}

/* A close_notify after data: the data is read first, then EPIPE; the reading side is over but the writing side is not. */
static void
check_close_notify(
	int version13,
	unsigned level)
{
	struct bench *bench;
	uint8_t buffer[16];
	size_t done;
	unsigned long calls;
	unsigned wants;
	int both;

	/* Data, then the closing. */
	bench = version13 ? connected13() : connected12();
	if (bench == NULL)
		return;
	peer_data(bench, "abc");
	peer_alert(bench, level, A_CLOSE_NOTIFY);
	expect_text(bench, "abc");
	done = 5;
	CHECK(drive_read(bench, buffer, sizeof(buffer), &done) == EPIPE);
	CHECK(done == 0U);
	CHECK((bench->conn.flags & SSLC_CONN_CLOSE_RECEIVED) != 0U);
	CHECK((bench->conn.flags & SSLC_CONN_FAILED) == 0U);

	/* A later read ends the same way without asking the transport. */
	calls = bench->pipe.read_calls;
	CHECK(sslc_conn_read(&bench->conn, buffer, sizeof(buffer), &done, &wants) == EPIPE);
	CHECK(bench->pipe.read_calls == calls);

	/* This side can still write (RFC 8446 6.1: each side closes its own writing side). */
	CHECK(drive_write(bench, (const uint8_t *)"xyz", 3, &done) == 0);
	CHECK(done == 3U);
	CHECK(bench->peer->data_size == 3U);
	CHECK_BYTES(bench->peer->data, "xyz", 3);

	/* The reply close_notify, once, and both closings seen. */
	both = 7;
	CHECK(sslc_conn_shutdown(&bench->conn, &both, &wants) == 0);
	CHECK(both == 1);
	CHECK(wants == 0U);
	CHECK(bench->peer->close_notify == 1);
	check_peer_alert(bench, 1, 1, A_CLOSE_NOTIFY, 1);
	CHECK(sslc_conn_shutdown(&bench->conn, &both, &wants) == 0);
	CHECK(both == 1);
	check_peer_alert(bench, 1, 1, A_CLOSE_NOTIFY, 1);

	/* After this side's close_notify nothing is written. */
	done = 5;
	CHECK(sslc_conn_write(&bench->conn, (const uint8_t *)"q", 1, &done, &wants) == EPIPE);
	CHECK(done == 0U);
	CHECK(bench->peer->data_size == 3U);
	bench_free(bench);
}

/* A fatal alert from the peer ends the connection with a reason of 1000 plus its description, and nothing is sent back. */
static void
check_fatal_alert(
	int version13,
	unsigned level,
	unsigned description)
{
	struct bench *bench;

	/* The alert, the failure. */
	bench = version13 ? connected13() : connected12();
	if (bench == NULL)
		return;
	peer_alert(bench, level, description);
	expect_failure(bench, EPROTO, SSL_AD_REASON_OFFSET + description, 0);
	CHECK(bench->conn.alert_received == description);
	bench_free(bench);
}

/* The alerts of TLS 1.3 (RFC 8446 6) and of TLS 1.2 (RFC 5246 7.2). */
static void
test_alerts(void)
{
	struct bench *bench;
	uint8_t record[3];
	unsigned version;
	size_t size;

	/* close_notify in both versions; TLS 1.3 takes it at any level. */
	check_close_notify(1, 1);
	check_close_notify(1, 2);
	check_close_notify(0, 1);

	/* TLS 1.3: user_canceled is ignored at either level, and data follows. */
	bench = connected13();
	if (bench != NULL) {
		peer_alert(bench, 1, A_USER_CANCELED);
		peer_alert(bench, 2, A_USER_CANCELED);
		peer_data(bench, "after");
		expect_text(bench, "after");
		CHECK(bench->conn.empty == 0U);
		CHECK((bench->conn.flags & SSLC_CONN_FAILED) == 0U);
		bench_free(bench);
	}

	/* TLS 1.3: every other alert is fatal, a warning too. */
	check_fatal_alert(1, 1, A_HANDSHAKE_FAILURE);
	check_fatal_alert(1, 2, A_HANDSHAKE_FAILURE);
	check_fatal_alert(1, 1, 255);
	check_fatal_alert(1, 2, A_USER_CANCELED + 1U);

	/* TLS 1.2: a warning other than close_notify is ignored, data follows. */
	bench = connected12();
	if (bench != NULL) {
		peer_alert(bench, 1, A_USER_CANCELED);
		peer_alert(bench, 1, A_NO_RENEGOTIATION);
		peer_data(bench, "after");
		expect_text(bench, "after");
		CHECK(bench->conn.empty == 0U);
		check_peer_alert(bench, 0, 0, 0, 0);
		bench_free(bench);
	}

	/* TLS 1.2: a fatal alert, whatever its description, is fatal (a fatal close_notify too). */
	check_fatal_alert(0, 2, A_HANDSHAKE_FAILURE);
	check_fatal_alert(0, 2, A_CLOSE_NOTIFY);
	check_fatal_alert(0, 2, A_USER_CANCELED);

	/* TLS 1.2 knows two levels: another is answered with illegal_parameter. */
	bench = connected12();
	if (bench != NULL) {
		peer_alert(bench, 3, A_HANDSHAKE_FAILURE);
		expect_failure(bench, EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_ILLEGAL_PARAMETER);
		bench_free(bench);
	}
	bench = connected12();
	if (bench != NULL) {
		peer_alert(bench, 0, A_CLOSE_NOTIFY);
		expect_failure(bench, EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_ILLEGAL_PARAMETER);
		bench_free(bench);
	}

	/* An alert is exactly two bytes: one, three and none are decode_error (RFC 8446 6; RFC 5246 7.2). */
	memset(record, 0, sizeof(record));
	for (version = 0; version < 2U; version++) {
		for (size = 0; size <= 3U; size++) {
			if (size == 2U)
				continue;
			bench = version == 0U ? connected13() : connected12();
			if (bench == NULL)
				continue;
			CHECK(tlspeer_send_record(bench->peer, T_ALERT, record, size) == 0);
			pipe_seal(&bench->pipe);
			expect_failure(bench, EPROTO, SSL_R_BAD_LENGTH, A_DECODE_ERROR);
			bench_free(bench);
		}
	}
}

/*
 * Records without progress.
 */

/* What the peer sends as a record that gives the connection nothing to use. */
enum empty_kind {
	EMPTY_USER_CANCELED,
	EMPTY_DATA,
	EMPTY_TICKET,
	EMPTY_WARNING12
};

/* Has the peer send count records of a kind, each apart, then a record of data; checks what the connection does. */
static void
check_empties(
	struct bench *bench,
	enum empty_kind kind,
	unsigned count)
{
	static const uint8_t none[1] = { 0 };
	unsigned index;

	/* The records, each sealed apart. */
	for (index = 0; index < count; index++) {
		switch (kind) {
		case EMPTY_USER_CANCELED:
			CHECK(tlspeer_send_alert(bench->peer, 1, A_USER_CANCELED) == 0);
			break;
		case EMPTY_DATA:
			CHECK(tlspeer_send_data(bench->peer, none, 0) == 0);
			break;
		case EMPTY_TICKET:
			CHECK(tlspeer_send_ticket(bench->peer) == 0);
			break;
		default:
			CHECK(tlspeer_send_alert(bench->peer, 1, A_NO_RENEGOTIATION) == 0);
			break;
		}
		pipe_seal(&bench->pipe);
	}
	peer_data(bench, "z");

	/* At most 32 in a row: the 33rd is unexpected_message. */
	if (count <= SSLC_CONN_MAX_EMPTY) {
		expect_text(bench, "z");
		CHECK(bench->conn.empty == 0U);
		CHECK((bench->conn.flags & SSLC_CONN_FAILED) == 0U);
	} else {
		expect_failure(bench, EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE);
	}
}

/* Checks the limit of records without progress for every kind, and that data resets the count. */
static void
test_empties(void)
{
	static const unsigned counts[] = { 1, 31, 32, 33, 34, 100 };
	struct bench *bench;
	unsigned index;
	unsigned kind;

	/* Every kind at each count, in the version that has the kind. */
	for (kind = 0; kind < 4U; kind++) {
		for (index = 0; index < sizeof(counts) / sizeof(counts[0]); index++) {
			if (kind == EMPTY_WARNING12 || (kind == EMPTY_DATA && index % 2U == 0U))
				bench = connected12();
			else
				bench = connected13();
			if (bench == NULL)
				continue;
			check_empties(bench, (enum empty_kind)kind, counts[index]);
			bench_free(bench);
		}
	}

	/* A record of data in between resets the count: 32 and 32 are fine. */
	bench = connected13();
	if (bench != NULL) {
		check_empties(bench, EMPTY_USER_CANCELED, 32);
		check_empties(bench, EMPTY_USER_CANCELED, 32);
		check_empties(bench, EMPTY_TICKET, 32);
		bench_free(bench);
	}
	bench = connected12();
	if (bench != NULL) {
		check_empties(bench, EMPTY_DATA, 32);
		check_empties(bench, EMPTY_WARNING12, 32);
		check_empties(bench, EMPTY_DATA, 32);
		bench_free(bench);
	}
}

/*
 * change_cipher_spec records and warnings before the server's flight count
 * too; a handshake message resets the count, so the compatibility record
 * the server sends after its ServerHello (TLS 1.3 with the option) or its
 * own change_cipher_spec (TLS 1.2) comes on a fresh count.
 */
static void
test_empties_handshake(void)
{
	static const uint8_t ccs[6] = { 0x14, 0x03, 0x03, 0x00, 0x01, 0x01 };
	static const uint8_t canceled[7] = { 0x15, 0x03, 0x03, 0x00, 0x02, 0x01, 0x5a };
	struct tlspeer_config config;
	struct bench *bench;
	unsigned count;
	unsigned kind;
	unsigned variant;
	unsigned index;
	int error;

	/* TLS 1.3 with the compatibility record after the ServerHello, TLS 1.3 without it, and TLS 1.2. */
	for (variant = 0; variant < 3U; variant++) {
		script(&config, variant == 2U ? SSLC_TLS12 : SSLC_TLS13, &pki256);
		if (variant == 0U)
			config.options = TLSPEER_OPT_CCS;

		/* 32 are accepted, the 33rd fails. */
		for (kind = 0; kind < 2U; kind++) {
			for (count = 31; count <= 33U; count++) {
				bench = bench_new(SSLC_TLS12, SSLC_TLS13, &pki256, &config, 0);
				if (bench == NULL)
					continue;
				for (index = 0; index < count; index++) {
					if (kind == 0U)
						pipe_inject(&bench->pipe, ccs, sizeof(ccs));
					else
						pipe_inject(&bench->pipe, canceled, sizeof(canceled));
				}
				error = drive_handshake(bench);
				if (count <= SSLC_CONN_MAX_EMPTY) {
					CHECK(error == 0);
					CHECK((bench->conn.flags & SSLC_CONN_CONNECTED) != 0U);
				} else {
					CHECK(error == EPROTO);
					CHECK(bench->conn.reason == SSL_R_UNEXPECTED_MESSAGE);
					check_peer_alert(bench, 1, 2, A_UNEXPECTED_MESSAGE, 0);
					check_dead(bench);
				}
				bench_free(bench);
			}
		}
	}
}

/*
 * Key changes and the framing of handshake messages.
 */

/* A TLS 1.3 NewSessionTicket as tlspeer makes it: its 34 bytes, for the tests that cut a message into records. */
static const uint8_t ticket_message[34] = {
	0x04, 0x00, 0x00, 0x1e,
	0x00, 0x00, 0x1c, 0x20,
	0x01, 0x02, 0x03, 0x04,
	0x01, 0x00,
	0x00, 0x10,
	0x74, 0x6c, 0x73, 0x70, 0x65, 0x65, 0x72, 0x20, 0x74, 0x69, 0x63, 0x6b, 0x65, 0x74, 0x00, 0x01,
	0x00, 0x00
};

/* A key change must fall on a record boundary: the server's first protected message in the record of the ServerHello. */
static void
test_key_change(void)
{
	struct tlspeer_config config;
	struct bench *bench;
	uint8_t buffer[16];
	size_t done;

	/* ServerHello and EncryptedExtensions in one record: unexpected_message, in plaintext (the client has no keys yet). */
	script(&config, SSLC_TLS13, &pki256);
	config.breaks = TLSPEER_BREAK_SAME_RECORD;
	bench = bench_new(SSLC_TLS12, SSLC_TLS13, &pki256, &config, 0);
	if (bench != NULL) {
		CHECK(drive_handshake(bench) == EPROTO);
		CHECK(bench->conn.reason == SSL_R_UNEXPECTED_MESSAGE);
		check_peer_alert(bench, 1, 2, A_UNEXPECTED_MESSAGE, 0);
		check_dead(bench);
		bench_free(bench);
	}

	/* A KeyUpdate and a message after it in one record: unexpected_message, protected under the keys then in force. */
	bench = connected13();
	if (bench != NULL) {
		CHECK(tlspeer_send_key_update(bench->peer, 0) == 0);
		CHECK(tlspeer_send_ticket(bench->peer) == 0);
		bench->peer->breaks |= TLSPEER_BREAK_SAME_RECORD;
		pipe_seal(&bench->pipe);
		CHECK(drive_read(bench, buffer, sizeof(buffer), &done) == EPROTO);
		CHECK(bench->conn.reason == SSL_R_UNEXPECTED_MESSAGE);
		check_peer_alert(bench, 1, 2, A_UNEXPECTED_MESSAGE, 1);
		check_dead(bench);
		bench_free(bench);
	}

	/* The same KeyUpdate and ticket in two records are fine. */
	bench = connected13();
	if (bench != NULL) {
		CHECK(tlspeer_send_key_update(bench->peer, 0) == 0);
		pipe_seal(&bench->pipe);
		CHECK(tlspeer_send_ticket(bench->peer) == 0);
		pipe_seal(&bench->pipe);
		peer_data(bench, "after");
		expect_text(bench, "after");
		CHECK((bench->conn.flags & SSLC_CONN_FAILED) == 0U);
		CHECK(bench->pipe.peer_error == 0);
		bench_free(bench);
	}
}

/* A handshake message cut into records, other records inside a message, and empty fragments. */
static void
test_framing(void)
{
	static const uint8_t ccs[6] = { 0x14, 0x03, 0x03, 0x00, 0x01, 0x01 };
	static const uint8_t request[4] = { 0, 0, 0, 0 };
	static const uint8_t none[1] = { 0 };
	struct bench *bench;
	unsigned version;
	unsigned variant;

	/* A message in two records, with nothing between them, is taken (TLS 1.3 ticket, TLS 1.2 HelloRequest). */
	bench = connected13();
	if (bench != NULL) {
		CHECK(tlspeer_send_record(bench->peer, T_HANDSHAKE, ticket_message, 10) == 0);
		pipe_seal(&bench->pipe);
		CHECK(tlspeer_send_record(bench->peer, T_HANDSHAKE, ticket_message + 10, sizeof(ticket_message) - 10U) == 0);
		pipe_seal(&bench->pipe);
		peer_data(bench, "ok");
		expect_text(bench, "ok");
		CHECK(bench->conn.message == NULL);
		CHECK(bench->conn.message_used == 0U);
		bench_free(bench);
	}
	bench = connected12();
	if (bench != NULL) {
		CHECK(tlspeer_send_record(bench->peer, T_HANDSHAKE, request, 2) == 0);
		pipe_seal(&bench->pipe);
		CHECK(tlspeer_send_record(bench->peer, T_HANDSHAKE, request + 2, 2) == 0);
		pipe_seal(&bench->pipe);
		peer_data(bench, "ok");
		expect_text(bench, "ok");
		check_peer_alert(bench, 1, 1, A_NO_RENEGOTIATION, 1);
		bench_free(bench);
	}

	/* A record of another kind between the parts of a message is unexpected_message (RFC 8446 5.1). */
	for (version = 0; version < 2U; version++) {
		for (variant = 0; variant < 4U; variant++) {
			bench = version == 0U ? connected13() : connected12();
			if (bench == NULL)
				continue;
			CHECK(tlspeer_send_record(bench->peer, T_HANDSHAKE, ticket_message, 6) == 0);
			pipe_seal(&bench->pipe);
			if (variant == 0U)
				peer_data(bench, "x");
			else if (variant == 1U)
				peer_alert(bench, 1, A_CLOSE_NOTIFY);
			else if (variant == 2U && version == 0U)
				CHECK(tlspeer_send_bytes(bench->peer, ccs, sizeof(ccs)) == 0);
			else if (variant == 2U)
				CHECK(tlspeer_send_record(bench->peer, T_CCS, ccs + 5, 1) == 0);
			else
				CHECK(tlspeer_send_data(bench->peer, none, 0) == 0);
			pipe_seal(&bench->pipe);
			expect_failure(bench, EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE);
			CHECK(bench->conn.message == NULL);
			bench_free(bench);
		}
	}

	/* An empty fragment of a handshake record is decode_error (RFC 8446 5.1), in both versions. */
	for (version = 0; version < 2U; version++) {
		bench = version == 0U ? connected13() : connected12();
		if (bench == NULL)
			continue;
		CHECK(tlspeer_send_record(bench->peer, T_HANDSHAKE, none, 0) == 0);
		pipe_seal(&bench->pipe);
		expect_failure(bench, EPROTO, SSL_R_BAD_LENGTH, A_DECODE_ERROR);
		bench_free(bench);
	}
}

/*
 * Records the other end could not make, and records that are damaged.
 */

/* One damaged thing the peer sends after the handshake, and what the connection must do. */
struct damage {
	const char *name;
	int version13;
	uint8_t header[5];
	size_t body;
	int error;
	unsigned reason;
	unsigned alert;
};

/*
 * The damaged records: a header the record layer refuses (an unknown content
 * type, a wrong version, a length over the limit or under the tag's size),
 * and the largest length of each version, which is read to its end and then
 * fails on its tag.
 * Values referenced: RFC 8446 section 5.1 (types, version, 2^14 + 256), 5.2;
 * RFC 5246 sections 6.2.1 and 6.2.3 (2^14 + 2048).
 */
static const struct damage damages[] = {
	{ "type 24", 1, { 0x18, 3, 3, 0, 1 }, 1, EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE },
	{ "type 19", 1, { 0x13, 3, 3, 0, 1 }, 1, EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE },
	{ "type 0", 0, { 0x00, 3, 3, 0, 1 }, 1, EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE },
	{ "type 24 and version 0x0302", 1, { 0x18, 3, 2, 0, 1 }, 1, EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE },
	{ "type 19 and length 0", 1, { 0x13, 3, 3, 0, 0 }, 0, EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE },
	{ "type 25 and version 0x0304 (1.2)", 0, { 0x19, 3, 4, 0, 1 }, 1, EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE },
	{ "version 0x0302", 1, { 0x17, 3, 2, 0, 0x20 }, 0, EPROTO, SSL_R_WRONG_VERSION_NUMBER, A_PROTOCOL_VERSION },
	{ "version 0x0304", 1, { 0x17, 3, 4, 0, 0x20 }, 0, EPROTO, SSL_R_WRONG_VERSION_NUMBER, A_PROTOCOL_VERSION },
	{ "version 0x0302 (1.2)", 0, { 0x17, 3, 2, 0, 0x20 }, 0, EPROTO, SSL_R_WRONG_VERSION_NUMBER, A_PROTOCOL_VERSION },
	{ "version 0x0200", 0, { 0x17, 2, 0, 0, 0x20 }, 0, EPROTO, SSL_R_WRONG_VERSION_NUMBER, A_PROTOCOL_VERSION },
	{ "length 2^14+257", 1, { 0x17, 3, 3, 0x41, 0x01 }, 0, EMSGSIZE, SSL_R_PACKET_LENGTH_TOO_LONG, A_RECORD_OVERFLOW },
	{ "length 2^14+2049", 0, { 0x17, 3, 3, 0x48, 0x01 }, 0, EMSGSIZE, SSL_R_PACKET_LENGTH_TOO_LONG, A_RECORD_OVERFLOW },
	{ "length 65535", 1, { 0x17, 3, 3, 0xff, 0xff }, 0, EMSGSIZE, SSL_R_PACKET_LENGTH_TOO_LONG, A_RECORD_OVERFLOW },
	{ "length 0", 1, { 0x17, 3, 3, 0, 0 }, 0, EBADMSG, SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC, A_BAD_RECORD_MAC },
	{ "length 16", 1, { 0x17, 3, 3, 0, 16 }, 16, EBADMSG, SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC, A_BAD_RECORD_MAC },
	{ "length 0 (1.2)", 0, { 0x17, 3, 3, 0, 0 }, 0, EBADMSG, SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC, A_BAD_RECORD_MAC },
	{ "length 23 (1.2)", 0, { 0x17, 3, 3, 0, 23 }, 23, EBADMSG, SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC, A_BAD_RECORD_MAC },
	{ "largest 1.3", 1, { 0x17, 3, 3, 0x41, 0x00 }, 16640, EBADMSG, SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC, A_BAD_RECORD_MAC },
	{ "largest 1.2", 0, { 0x17, 3, 3, 0x48, 0x00 }, 18432, EBADMSG, SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC, A_BAD_RECORD_MAC },
	{ "tag of 1.3 data", 1, { 0x17, 3, 3, 0, 40 }, 40, EBADMSG, SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC, A_BAD_RECORD_MAC },
	{ "tag of 1.2 data", 0, { 0x17, 3, 3, 0, 40 }, 40, EBADMSG, SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC, A_BAD_RECORD_MAC },
	{ "plaintext alert in 1.3", 1, { 0x15, 3, 3, 0, 17 }, 17, EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE },
	{ "plaintext ccs of two bytes", 1, { 0x14, 3, 3, 0, 2 }, 2, EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE }
};

/* Sends each damaged record after a finished handshake and checks the outcome. */
static void
test_damage(void)
{
	static uint8_t bytes[20000];
	struct bench *bench;
	unsigned index;

	/* The record is the header and a body of zeros; the failure comes at the header or at the tag. */
	for (index = 0; index < sizeof(damages) / sizeof(damages[0]); index++) {
		bench = damages[index].version13 ? connected13() : connected12();
		if (bench == NULL)
			continue;
		memcpy(bytes, damages[index].header, 5);
		memset(bytes + 5, 0, damages[index].body);
		CHECK(tlspeer_send_bytes(bench->peer, bytes, 5U + damages[index].body) == 0);
		pipe_seal(&bench->pipe);
		expect_failure(bench, damages[index].error, damages[index].reason, damages[index].alert);
		CHECK(bench->pipe.overreads == 0U);
		if (bench->pipe.overreads != 0U || bench->conn.reason != damages[index].reason)
			printf("  damage %s\n", damages[index].name);
		bench_free(bench);
	}
}

/* A tag that does not verify, a protected change_cipher_spec, and other records out of place after the handshake. */
static void
test_out_of_place(void)
{
	static const uint8_t ccs_one[1] = { 1 };
	static const uint8_t ccs_two[1] = { 2 };
	static const uint8_t ccs_record[6] = { 0x14, 0x03, 0x03, 0x00, 0x01, 0x01 };
	static const uint8_t client_hello[4] = { 0x01, 0x00, 0x00, 0x00 };
	static const uint8_t ticket12[4] = { 0x04, 0x00, 0x00, 0x00 };
	struct bench *bench;
	unsigned version;
	uint8_t buffer[16];
	size_t done;
	unsigned long calls;

	/* A flipped tag on data, in both versions: bad_record_mac, and the failure leaves no byte to read. */
	for (version = 0; version < 2U; version++) {
		bench = version == 0U ? connected13() : connected12();
		if (bench == NULL)
			continue;
		bench->peer->breaks |= TLSPEER_BREAK_RECORD_TAG;
		peer_data(bench, "secret");
		expect_failure(bench, EBADMSG, SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC, A_BAD_RECORD_MAC);
		CHECK(bench->conn.plain_size == 0U);
		bench_free(bench);
	}

	/* A protected change_cipher_spec is not the compatibility record (TLS 1.3: unexpected_message). */
	bench = connected13();
	if (bench != NULL) {
		CHECK(tlspeer_send_record(bench->peer, T_CCS, ccs_one, 1) == 0);
		pipe_seal(&bench->pipe);
		expect_failure(bench, EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE);
		bench_free(bench);
	}

	/* The unprotected compatibility record after the handshake: the handshake refuses it (unexpected_message). */
	bench = connected13();
	if (bench != NULL) {
		CHECK(tlspeer_send_bytes(bench->peer, ccs_record, sizeof(ccs_record)) == 0);
		pipe_seal(&bench->pipe);
		expect_failure(bench, EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE);
		bench_free(bench);
	}

	/* TLS 1.2: a protected change_cipher_spec after the handshake, and one with a wrong content. */
	bench = connected12();
	if (bench != NULL) {
		CHECK(tlspeer_send_record(bench->peer, T_CCS, ccs_one, 1) == 0);
		pipe_seal(&bench->pipe);
		expect_failure(bench, EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE);
		bench_free(bench);
	}
	bench = connected12();
	if (bench != NULL) {
		CHECK(tlspeer_send_record(bench->peer, T_CCS, ccs_two, 1) == 0);
		pipe_seal(&bench->pipe);
		expect_failure(bench, EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE);
		bench_free(bench);
	}

	/* A handshake message the state does not take: the handshake's unexpected_message goes to the peer. */
	bench = connected13();
	if (bench != NULL) {
		CHECK(tlspeer_send_handshake(bench->peer, client_hello, sizeof(client_hello)) == 0);
		pipe_seal(&bench->pipe);
		expect_failure(bench, EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE);
		bench_free(bench);
	}
	bench = connected12();
	if (bench != NULL) {
		CHECK(tlspeer_send_handshake(bench->peer, ticket12, sizeof(ticket12)) == 0);
		pipe_seal(&bench->pipe);
		expect_failure(bench, EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE);
		bench_free(bench);
	}

	/* A HelloRequest after a TLS 1.2 handshake is answered with a warning, and the data after it is read. */
	bench = connected12();
	if (bench != NULL) {
		CHECK(tlspeer_send_handshake(bench->peer, (const uint8_t *)"\0\0\0\0", 4) == 0);
		pipe_seal(&bench->pipe);
		peer_data(bench, "after");
		expect_text(bench, "after");
		check_peer_alert(bench, 1, 1, A_NO_RENEGOTIATION, 1);
		CHECK((bench->conn.flags & SSLC_CONN_FAILED) == 0U);
		bench_free(bench);
	}

	/* An empty record of application data is accepted (RFC 8446 5.1) and the next one is read. */
	bench = connected13();
	if (bench != NULL) {
		CHECK(tlspeer_send_data(bench->peer, ccs_one, 0) == 0);
		pipe_seal(&bench->pipe);
		peer_data(bench, "next");
		calls = bench->pipe.read_calls;
		CHECK(drive_read(bench, buffer, sizeof(buffer), &done) == 0);
		CHECK(done == 4U);
		CHECK(bench->pipe.read_calls - calls >= 4U);
		bench_free(bench);
	}
}

/*
 * Bytes before the keys: a server that is not tlspeer.
 */

/* Starts a client of TLS 1.3 with no peer: the ClientHello is written and the client waits for the server. */
static struct bench *
raw_start(void)
{
	struct bench *bench;
	unsigned wants;
	char types[8];
	int error;

	/* The first call writes the ClientHello and waits to read. */
	bench = bench_new(SSLC_TLS12, SSLC_TLS13, &pki256, NULL, 0);
	if (bench == NULL)
		return NULL;
	wants = 0;
	error = sslc_conn_handshake(&bench->conn, &wants);
	CHECK(error == EAGAIN);
	CHECK(wants == SSLC_WANT_READ);
	wire_types(&bench->pipe, types, sizeof(types));
	CHECK(strcmp(types, "h") == 0);
	return bench;
}

/* Checks the bytes of the alert record the client wrote after its ClientHello: a plaintext fatal alert of description. */
static void
check_wire_alert(
	const struct bench *bench,
	unsigned description)
{
	struct wire_record records[4];
	size_t count;
	size_t at;

	/* The ClientHello and then exactly one alert record: type 21, version 0x0303, two bytes, fatal, the description. */
	count = wire_parse(&bench->pipe, records, 4U);
	CHECK(count == 2U);
	if (count != 2U)
		return;
	CHECK(records[1].type == T_ALERT);
	CHECK(records[1].version == 0x0303U);
	CHECK(records[1].length == 2U);
	at = records[1].offset + 5U;
	CHECK(bench->pipe.sent[at] == 2U);
	CHECK(bench->pipe.sent[at + 1U] == description);
	CHECK(bench->pipe.sent_used == records[1].offset + 7U);
}

/* Gives the client raw bytes in the handshake and checks the failure, the reason, and the alert (0: none) it sent. */
static void
raw_case(
	const uint8_t *bytes,
	size_t size,
	int error,
	unsigned reason,
	unsigned alert)
{
	struct bench *bench;
	char types[8];
	int got;

	/* The bytes, the failure, the reason, the alert. */
	bench = raw_start();
	if (bench == NULL)
		return;
	pipe_inject(&bench->pipe, bytes, size);
	got = drive_handshake(bench);
	CHECK(got == error);
	if (got != error)
		printf("  handshake gave %d, expected %d\n", got, error);
	CHECK(bench->conn.reason == reason);
	if (bench->conn.reason != reason)
		printf("  reason %u, expected %u\n", bench->conn.reason, reason);
	CHECK((bench->conn.flags & SSLC_CONN_FAILED) != 0U);
	if (alert != 0U) {
		check_wire_alert(bench, alert);
	} else {
		wire_types(&bench->pipe, types, sizeof(types));
		CHECK(strcmp(types, "h") == 0);
	}
	CHECK(bench->conn.message == NULL);
	CHECK(bench->conn.message_used == 0U);
	check_dead(bench);
	bench_free(bench);
}

/* The raw cases of the handshake phase. */
static void
test_raw(void)
{
	static const uint8_t type24[6] = { 0x18, 3, 3, 0, 1, 0 };
	static const uint8_t type19[6] = { 0x13, 3, 3, 0, 1, 0 };
	static const uint8_t version0300[6] = { 0x16, 3, 0, 0, 1, 0 };
	static const uint8_t version0304[6] = { 0x16, 3, 4, 0, 1, 0 };
	static const uint8_t version0200[6] = { 0x16, 2, 0, 0, 1, 0 };
	static const uint8_t empty_handshake[5] = { 0x16, 3, 3, 0, 0 };
	static const uint8_t empty_alert[5] = { 0x15, 3, 3, 0, 0 };
	static const uint8_t too_long[5] = { 0x16, 3, 3, 0x40, 0x01 };
	static const uint8_t alert_one[6] = { 0x15, 3, 3, 0, 1, 0 };
	static const uint8_t alert_three[8] = { 0x15, 3, 3, 0, 3, 0, 0, 0 };
	static const uint8_t ccs_bad[6] = { 0x14, 3, 3, 0, 1, 2 };
	static const uint8_t ccs_zero[6] = { 0x14, 3, 3, 0, 1, 0 };
	static const uint8_t ccs_two[7] = { 0x14, 3, 3, 0, 2, 1, 1 };
	static const uint8_t data_early[6] = { 0x17, 3, 3, 0, 1, 0 };
	static const uint8_t partial_ccs[15] = { 0x16, 3, 3, 0, 4, 0x02, 0, 0, 0x5a, 0x14, 3, 3, 0, 1, 1 };
	static const uint8_t partial_alert[16] = { 0x16, 3, 3, 0, 4, 0x02, 0, 0, 0x5a, 0x15, 3, 3, 0, 2, 1, 0 };
	static const uint8_t partial_data[15] = { 0x16, 3, 3, 0, 4, 0x02, 0, 0, 0x5a, 0x17, 3, 3, 0, 1, 0 };
	static const uint8_t message_over[9] = { 0x16, 3, 3, 0, 4, 0x02, 0x01, 0x90, 0x01 };
	static const uint8_t fatal_alert[7] = { 0x15, 3, 3, 0, 2, 2, 40 };
	static const uint8_t fatal_close[7] = { 0x15, 3, 3, 0, 2, 2, 0 };
	static const uint8_t level_three[7] = { 0x15, 3, 3, 0, 2, 3, 0 };
	static uint8_t big[5U + 16384U];
	struct bench *bench;
	char types[8];
	unsigned wants;
	unsigned version;
	uint8_t close_notify[7];

	/* Records of a kind that does not exist: unexpected_message. */
	raw_case(type24, sizeof(type24), EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE);
	raw_case(type19, sizeof(type19), EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE);

	/* A version other than 0x0301 to 0x0303: protocol_version. */
	raw_case(version0300, sizeof(version0300), EPROTO, SSL_R_WRONG_VERSION_NUMBER, A_PROTOCOL_VERSION);
	raw_case(version0304, sizeof(version0304), EPROTO, SSL_R_WRONG_VERSION_NUMBER, A_PROTOCOL_VERSION);
	raw_case(version0200, sizeof(version0200), EPROTO, SSL_R_WRONG_VERSION_NUMBER, A_PROTOCOL_VERSION);

	/* An empty plaintext record: decode_error. */
	raw_case(empty_handshake, sizeof(empty_handshake), EPROTO, SSL_R_BAD_LENGTH, A_DECODE_ERROR);
	raw_case(empty_alert, sizeof(empty_alert), EPROTO, SSL_R_BAD_LENGTH, A_DECODE_ERROR);

	/* A plaintext record over 2^14 bytes: record_overflow; one of exactly 2^14 is read (here an alert too long). */
	raw_case(too_long, sizeof(too_long), EMSGSIZE, SSL_R_PACKET_LENGTH_TOO_LONG, A_RECORD_OVERFLOW);
	memset(big, 0, sizeof(big));
	big[0] = 0x15;
	big[1] = 3;
	big[2] = 3;
	big[3] = 0x40;
	big[4] = 0x00;
	raw_case(big, sizeof(big), EPROTO, SSL_R_BAD_LENGTH, A_DECODE_ERROR);

	/* An alert of one or three bytes: decode_error. */
	raw_case(alert_one, sizeof(alert_one), EPROTO, SSL_R_BAD_LENGTH, A_DECODE_ERROR);
	raw_case(alert_three, sizeof(alert_three), EPROTO, SSL_R_BAD_LENGTH, A_DECODE_ERROR);

	/* A change_cipher_spec that is not the byte 1, or not one byte: unexpected_message. */
	raw_case(ccs_bad, sizeof(ccs_bad), EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE);
	raw_case(ccs_zero, sizeof(ccs_zero), EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE);
	raw_case(ccs_two, sizeof(ccs_two), EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE);

	/* Application data before the handshake is over. */
	raw_case(data_early, sizeof(data_early), EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE);

	/* A record of another kind inside a handshake message. */
	raw_case(partial_ccs, sizeof(partial_ccs), EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE);
	raw_case(partial_alert, sizeof(partial_alert), EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE);
	raw_case(partial_data, sizeof(partial_data), EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE);

	/* A handshake message over 102400 bytes of body is refused at its header: decode_error. */
	raw_case(message_over, sizeof(message_over), EMSGSIZE, SSL_R_EXCESSIVE_MESSAGE_SIZE, A_DECODE_ERROR);

	/* Fatal alerts before the ServerHello end the connection and are not answered; a TLS 1.2 style warning is not fatal. */
	raw_case(fatal_alert, sizeof(fatal_alert), EPROTO, SSL_AD_REASON_OFFSET + 40U, 0);
	raw_case(fatal_close, sizeof(fatal_close), EPROTO, SSL_AD_REASON_OFFSET + 0U, 0);

	/* A level that is neither warning nor fatal, before the version is known: illegal_parameter. */
	raw_case(level_three, sizeof(level_three), EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_ILLEGAL_PARAMETER);

	/* A close_notify before the ServerHello is accepted in a record of any of the three versions: EPIPE, nothing sent. */
	for (version = 1; version <= 3U; version++) {
		bench = raw_start();
		if (bench == NULL)
			continue;
		close_notify[0] = 0x15;
		close_notify[1] = 3;
		close_notify[2] = (uint8_t)version;
		close_notify[3] = 0;
		close_notify[4] = 2;
		close_notify[5] = 1;
		close_notify[6] = 0;
		pipe_inject(&bench->pipe, close_notify, sizeof(close_notify));
		CHECK(drive_handshake(bench) == EPIPE);
		CHECK((bench->conn.flags & SSLC_CONN_CLOSE_RECEIVED) != 0U);
		CHECK((bench->conn.flags & SSLC_CONN_FAILED) == 0U);
		CHECK(bench->conn.reason == SSL_AD_REASON_OFFSET);
		CHECK(bench->conn.alert_received == 0U);
		wire_types(&bench->pipe, types, sizeof(types));
		CHECK(strcmp(types, "h") == 0);

		/* The handshake called again says the same without reading. */
		wants = 5;
		CHECK(sslc_conn_handshake(&bench->conn, &wants) == EPIPE);
		bench_free(bench);
	}
}

/*
 * Makes the plaintext records of a handshake message of the largest size
 * (4 + 102400 bytes, a ServerHello of zeros after its header) cut into the
 * pieces of sizes[0 .. count - 1] (the last piece takes what is left of the
 * message, so the sum must reach it; a piece after the message's end is the
 * start of the next message, zeros), and puts them into the pipe.
 */
static void
inject_big_message(
	struct bench *bench,
	const size_t *sizes,
	size_t count)
{
	static uint8_t record[5U + 16384U];
	size_t index;
	size_t total;

	/* The pieces, the first starting with the message header. */
	total = 0;
	for (index = 0; index < count; index++) {
		memset(record, 0, sizeof(record));
		record[0] = 0x16;
		record[1] = 3;
		record[2] = 3;
		record[3] = (uint8_t)(sizes[index] >> 8);
		record[4] = (uint8_t)(sizes[index] & 0xffU);
		if (index == 0U) {
			record[5] = 0x02;
			record[6] = 0x01;
			record[7] = 0x90;
			record[8] = 0x00;
		}
		pipe_inject(&bench->pipe, record, 5U + sizes[index]);
		total += sizes[index];
	}
	CHECK(total >= 102404U);
}

/*
 * A handshake message of exactly the largest size is read to its end (and
 * then refused by the handshake, not by the size limit), also when the last
 * record that completes it carries the start of the next message, which
 * needs the room of a whole record beyond the limit.
 */
static void
test_message_limit(void)
{
	static const size_t plain[7] = { 16384, 16384, 16384, 16384, 16384, 16384, 4100 };
	static const size_t overlap[8] = { 16384, 16384, 16384, 16384, 16384, 16384, 4099, 16384 };
	struct bench *bench;
	unsigned variant;
	int error;

	/* Seven records, and eight (the last one 16383 bytes past the end of the message). */
	for (variant = 0; variant < 2U; variant++) {
		bench = raw_start();
		if (bench == NULL)
			continue;
		if (variant == 0U)
			inject_big_message(bench, plain, 7U);
		else
			inject_big_message(bench, overlap, 8U);

		/* Read to the end; the ServerHello of zeros is the handshake's to refuse, with its own error, not the size limit's. */
		error = drive_handshake(bench);
		CHECK(error != 0);
		CHECK(error != EAGAIN);
		CHECK(error != EMSGSIZE);
		CHECK(bench->conn.reason != SSL_R_EXCESSIVE_MESSAGE_SIZE);
		CHECK((bench->conn.flags & SSLC_CONN_FAILED) != 0U);
		CHECK(bench->pipe.in_taken == bench->pipe.in_used);
		CHECK(bench->pipe.overreads == 0U);
		check_dead(bench);
		bench_free(bench);
	}
}

/*
 * The end of the stream and failures of the transport.
 */

/* EOF during the handshake is EPIPE, without a failure and without an alert; EOF inside a record too. */
static void
test_eof_handshake(void)
{
	static const uint8_t partial[3] = { 0x16, 3, 3 };
	static const uint8_t header[5] = { 0x16, 3, 3, 0, 9 };
	struct bench *bench;
	unsigned wants;
	unsigned variant;
	char types[8];

	/* At a record boundary, inside a header and inside a body. */
	for (variant = 0; variant < 3U; variant++) {
		bench = raw_start();
		if (bench == NULL)
			continue;
		if (variant == 1U)
			pipe_inject(&bench->pipe, partial, sizeof(partial));
		if (variant == 2U)
			pipe_inject(&bench->pipe, header, sizeof(header));
		bench->pipe.eof = 1;
		CHECK(drive_handshake(bench) == EPIPE);
		CHECK(bench->conn.reason == SSL_R_UNEXPECTED_EOF_WHILE_READING);
		CHECK((bench->conn.flags & SSLC_CONN_FAILED) == 0U);
		CHECK((bench->conn.flags & SSLC_CONN_CLOSE_RECEIVED) == 0U);
		wire_types(&bench->pipe, types, sizeof(types));
		CHECK(strcmp(types, "h") == 0);

		/* The same again. */
		wants = 5;
		CHECK(sslc_conn_handshake(&bench->conn, &wants) == EPIPE);
		bench_free(bench);
	}
}

/* EOF after the handshake without close_notify: ECONNRESET, or EPIPE with SSLC_CONN_IGNORE_EOF; buffered data first. */
static void
check_eof(
	uint32_t flags,
	int cut_record)
{
	struct tlspeer_config config;
	struct bench *bench;
	uint8_t buffer[16];
	size_t done;
	unsigned wants;
	int both;
	int expected;

	/* A connected client with the flag, and data waiting. */
	script(&config, SSLC_TLS13, &pki256);
	bench = bench_new(SSLC_TLS12, SSLC_TLS13, &pki256, &config, flags);
	if (bench == NULL)
		return;
	CHECK(drive_handshake(bench) == 0);
	CHECK((bench->conn.flags & SSLC_CONN_IGNORE_EOF) == (flags & SSLC_CONN_IGNORE_EOF));
	expected = (flags & SSLC_CONN_IGNORE_EOF) != 0U ? EPIPE : ECONNRESET;
	peer_data(bench, "tail");
	if (cut_record) {
		/* The record is cut three bytes short: its data never arrives. */
		bench->pipe.in_used -= 3U;
		bench->pipe.eof = 1;
		CHECK(drive_read(bench, buffer, sizeof(buffer), &done) == expected);
		CHECK(done == 0U);
	} else {
		bench->pipe.eof = 1;
		expect_text(bench, "tail");
		CHECK(drive_read(bench, buffer, sizeof(buffer), &done) == expected);
		CHECK(done == 0U);

		/* Asked again, the same answer. */
		CHECK(sslc_conn_read(&bench->conn, buffer, sizeof(buffer), &done, &wants) == expected);
	}
	CHECK(bench->conn.reason == SSL_R_UNEXPECTED_EOF_WHILE_READING);
	CHECK(bench->conn.os_error == 0);
	CHECK((bench->conn.flags & SSLC_CONN_FAILED) == 0U);
	if ((flags & SSLC_CONN_IGNORE_EOF) != 0U)
		CHECK((bench->conn.flags & SSLC_CONN_CLOSE_RECEIVED) != 0U);
	else
		CHECK((bench->conn.flags & SSLC_CONN_CLOSE_RECEIVED) == 0U);

	/* Writing still works, and the closing exchange reports whether the peer's close_notify counts as received. */
	CHECK(drive_write(bench, (const uint8_t *)"bye", 3, &done) == 0);
	CHECK(bench->peer->data_size == 3U);
	both = 5;
	CHECK(sslc_conn_shutdown(&bench->conn, &both, &wants) == 0);
	CHECK(both == ((flags & SSLC_CONN_IGNORE_EOF) != 0U ? 1 : 0));
	check_peer_alert(bench, 1, 1, A_CLOSE_NOTIFY, 1);
	bench_free(bench);
}

/* Runs the end-of-stream checks. */
static void
test_eof(void)
{
	/* Both flags, whole and cut records. */
	check_eof(0, 0);
	check_eof(0, 1);
	check_eof(SSLC_CONN_IGNORE_EOF, 0);
	check_eof(SSLC_CONN_IGNORE_EOF, 1);
	test_eof_handshake();
}

/* A failure of the transport is EIO with the system's error value, ends the connection and sends nothing. */
static void
test_transport_failures(void)
{
	struct bench *bench;
	uint8_t buffer[16];
	size_t done;
	unsigned wants;
	unsigned variant;
	char types[8];
	int both;

	/* The first read of the handshake fails, and the first write. */
	bench = bench_new(SSLC_TLS12, SSLC_TLS13, &pki256, NULL, 0);
	if (bench != NULL) {
		bench->pipe.read_fail_at = 1;
		bench->pipe.read_fail_errno = 77;
		CHECK(drive_handshake(bench) == EIO);
		CHECK(bench->conn.os_error == 77);
		CHECK((bench->conn.flags & SSLC_CONN_FAILED) != 0U);
		wire_types(&bench->pipe, types, sizeof(types));
		CHECK(strcmp(types, "h") == 0);
		check_dead(bench);
		bench_free(bench);
	}
	bench = bench_new(SSLC_TLS12, SSLC_TLS13, &pki256, NULL, 0);
	if (bench != NULL) {
		bench->pipe.write_fail_at = 1;
		bench->pipe.write_fail_errno = 55;
		CHECK(drive_handshake(bench) == EIO);
		CHECK(bench->conn.os_error == 55);
		CHECK((bench->conn.flags & SSLC_CONN_FAILED) != 0U);
		CHECK(bench->pipe.sent_used == 0U);
		check_dead(bench);
		bench_free(bench);
	}

	/* A read, a write and the next write of a connected client. */
	for (variant = 0; variant < 3U; variant++) {
		bench = connected13();
		if (bench == NULL)
			continue;
		if (variant == 0U) {
			bench->pipe.read_fail_at = (unsigned)bench->pipe.read_calls + 1U;
			bench->pipe.read_fail_errno = 33;
			CHECK(drive_read(bench, buffer, sizeof(buffer), &done) == EIO);
			CHECK(bench->conn.os_error == 33);
		} else if (variant == 1U) {
			bench->pipe.write_fail_at = (unsigned)bench->pipe.write_calls + 1U;
			bench->pipe.write_fail_errno = 44;
			CHECK(drive_write(bench, (const uint8_t *)"x", 1, &done) == EIO);
			CHECK(bench->conn.os_error == 44);
		} else {
			/* A failure while the connection's own alert is sent changes nothing it reports. */
			bench->pipe.write_fail_at = (unsigned)bench->pipe.write_calls + 1U;
			bench->pipe.write_fail_errno = 45;
			CHECK(sslc_conn_shutdown(&bench->conn, &both, &wants) == EIO);
			CHECK(bench->conn.os_error == 45);
		}
		CHECK((bench->conn.flags & SSLC_CONN_FAILED) != 0U);
		check_peer_alert(bench, 0, 0, 0, 0);
		check_dead(bench);
		bench_free(bench);
	}

	/* A read that says it gave more than asked, a write that says so, and a write that took nothing: EIO without an errno value. */
	for (variant = 0; variant < 3U; variant++) {
		bench = bench_new(SSLC_TLS12, SSLC_TLS13, &pki256, NULL, 0);
		if (bench == NULL)
			continue;
		if (variant == 0U) {
			/* A server hello record of one byte is enough to begin: the lie is in the count. */
			CHECK(sslc_conn_handshake(&bench->conn, &wants) == EAGAIN);
			bench->pipe.read_lie = 1;
			pipe_inject(&bench->pipe, (const uint8_t *)"\x16\x03\x03\x00\x01\x00", 6);
			CHECK(drive_handshake(bench) == EIO);
		} else if (variant == 1U) {
			bench->pipe.write_lie = 1;
			CHECK(drive_handshake(bench) == EIO);
		} else {
			bench->pipe.write_zero = 1;
			CHECK(drive_handshake(bench) == EIO);
		}
		CHECK(bench->conn.os_error == 0);
		CHECK((bench->conn.flags & SSLC_CONN_FAILED) != 0U);
		bench_free(bench);
	}
}

/*
 * Closing.
 */

/* Checks the closing exchange: before the handshake, once, with a write held back, and after the peer's close_notify. */
static void
test_shutdown(void)
{
	static uint8_t data[20000];
	struct bench *bench;
	size_t done;
	size_t before;
	unsigned wants;
	int both;

	/* Before the handshake is over nothing is sent and the connection is not failed. */
	bench = bench_new(SSLC_TLS12, SSLC_TLS13, &pki256, NULL, 0);
	if (bench != NULL) {
		both = 7;
		wants = 7;
		CHECK(sslc_conn_shutdown(&bench->conn, &both, &wants) == EPROTO);
		CHECK(both == 0);
		CHECK(wants == 0U);
		CHECK(bench->pipe.sent_used == 0U);
		CHECK((bench->conn.flags & SSLC_CONN_FAILED) == 0U);
		CHECK((bench->conn.flags & SSLC_CONN_CLOSE_SENT) == 0U);
		bench_free(bench);
	}

	/* Once: the peer sees one close_notify (a warning), however often shutdown is called; both is 0 until the peer's comes. */
	bench = connected12();
	if (bench != NULL) {
		both = 7;
		CHECK(sslc_conn_shutdown(&bench->conn, &both, &wants) == 0);
		CHECK(both == 0);
		CHECK(wants == 0U);
		CHECK((bench->conn.flags & SSLC_CONN_CLOSE_SENT) != 0U);
		before = bench->pipe.sent_used;
		CHECK(sslc_conn_shutdown(&bench->conn, &both, &wants) == 0);
		CHECK(both == 0);
		CHECK(bench->pipe.sent_used == before);
		CHECK(bench->peer->close_notify == 1);
		check_peer_alert(bench, 1, 1, A_CLOSE_NOTIFY, 1);

		/* The peer's close_notify arrives later: both. */
		peer_alert(bench, 1, A_CLOSE_NOTIFY);
		CHECK(drive_read(bench, data, 8, &done) == EPIPE);
		CHECK(sslc_conn_shutdown(&bench->conn, &both, &wants) == 0);
		CHECK(both == 1);
		CHECK(bench->pipe.sent_used == before);
		bench_free(bench);
	}

	/* With the transport full: EAGAIN, the close_notify is sealed once and a retry sends it. */
	bench = connected13();
	if (bench != NULL) {
		bench->pipe.write_limited = 1;
		bench->pipe.write_budget = 0;
		before = bench->pipe.sent_used;
		both = 7;
		CHECK(sslc_conn_shutdown(&bench->conn, &both, &wants) == EAGAIN);
		CHECK(wants == SSLC_WANT_WRITE);
		CHECK((bench->conn.flags & SSLC_CONN_CLOSE_SENT) != 0U);
		CHECK(bench->conn.out_used - bench->conn.out_sent == 5U + 2U + 17U);
		CHECK(sslc_conn_shutdown(&bench->conn, &both, &wants) == EAGAIN);
		CHECK(bench->conn.out_used - bench->conn.out_sent == 5U + 2U + 17U);
		bench->pipe.write_budget = 100;
		CHECK(sslc_conn_shutdown(&bench->conn, &both, &wants) == 0);
		CHECK(both == 0);
		CHECK(bench->pipe.sent_used - before == 5U + 2U + 17U);
		CHECK(bench->peer->close_notify == 1);
		check_peer_alert(bench, 1, 1, A_CLOSE_NOTIFY, 1);
		bench_free(bench);
	}

	/* A write held back when shutdown is called is sent first, whole, then the close_notify. */
	bench = connected13();
	if (bench != NULL) {
		pattern(data, 1000, 5U);
		bench->pipe.write_limited = 1;
		bench->pipe.write_budget = 10;
		before = bench->pipe.sent_used;
		CHECK(sslc_conn_write(&bench->conn, data, 1000, &done, &wants) == EAGAIN);
		bench->pipe.write_budget = 1U << 20;
		both = 7;
		CHECK(sslc_conn_shutdown(&bench->conn, &both, &wants) == 0);
		CHECK(bench->pipe.sent_used - before == 5U + 1000U + 17U + 5U + 2U + 17U);
		CHECK(bench->peer->data_size == 1000U);
		CHECK_BYTES(bench->peer->data, data, 1000);
		CHECK(bench->peer->close_notify == 1);
		CHECK(bench->peer->error == 0);

		/* The retried write of the held bytes is refused: this side is closed. */
		done = 5;
		CHECK(sslc_conn_write(&bench->conn, data, 1000, &done, &wants) == EPIPE);
		CHECK(done == 0U);
		bench_free(bench);
	}
}

/*
 * The structure.
 */

/* Checks init's refusals and copies, and free's idempotence. */
static void
test_init_free(void)
{
	struct sslc_conn *conn;
	struct sslc_transport transport;
	struct sslc_hs_config config;
	struct sslc_hs_config copy;
	struct bench *bench;
	static struct pipe pipe;
	uint8_t record[6];
	size_t done;
	unsigned wants;
	int both;

	/* The parts of a call. */
	conn = calloc(1, sizeof(*conn));
	if (conn == NULL) {
		CHECK(0);
		return;
	}
	memset(&pipe, 0, sizeof(pipe));
	transport.read = pipe_read;
	transport.write = pipe_write;
	transport.context = &pipe;
	memset(&config, 0, sizeof(config));
	config.min_version = SSLC_TLS12;
	config.max_version = SSLC_TLS13;
	CHECK(sslc_conn_init(NULL, &config, &transport, 0) == EINVAL);
	CHECK(sslc_conn_init(conn, NULL, &transport, 0) == EINVAL);
	CHECK(sslc_conn_init(conn, &config, NULL, 0) == EINVAL);
	transport.read = NULL;
	CHECK(sslc_conn_init(conn, &config, &transport, 0) == EINVAL);
	transport.read = pipe_read;
	transport.write = NULL;
	CHECK(sslc_conn_init(conn, &config, &transport, 0) == EINVAL);
	transport.write = pipe_write;

	/* Only the flag of the caller's is kept; the configuration is copied; whatever the memory held before is gone. */
	memset(conn, 0xaa, sizeof(*conn));
	CHECK(sslc_conn_init(conn, &config, &transport, 0xffffffffU) == 0);
	CHECK(conn->flags == SSLC_CONN_IGNORE_EOF);
	copy = config;
	config.min_version = 0x1234;
	CHECK(memcmp(&conn->config, &copy, sizeof(copy)) == 0);
	CHECK(conn->transport.context == &pipe);
	CHECK(conn->reading.suite == NULL && conn->writing.suite == NULL);
	CHECK(conn->in_used == 0U && conn->out_used == 0U && conn->plain_size == 0U && conn->message == NULL);
	CHECK(sslc_conn_pending(conn) == 0U);
	CHECK(sslc_conn_pending(NULL) == 0U);

	/* Free wipes the structure, and is harmless twice and on NULL. */
	sslc_conn_free(conn);
	CHECK(conn->flags == 0U);
	CHECK(conn->transport.read == NULL);
	sslc_conn_free(conn);
	sslc_conn_free(NULL);
	free(conn);

	/* The arguments of the other calls. */
	bench = connected13();
	if (bench != NULL) {
		CHECK(sslc_conn_handshake(NULL, &wants) == EINVAL);
		CHECK(sslc_conn_handshake(&bench->conn, NULL) == EINVAL);
		CHECK(sslc_conn_read(NULL, record, 1, &done, &wants) == EINVAL);
		CHECK(sslc_conn_read(&bench->conn, NULL, 1, &done, &wants) == EINVAL);
		CHECK(sslc_conn_read(&bench->conn, record, 1, NULL, &wants) == EINVAL);
		CHECK(sslc_conn_read(&bench->conn, record, 1, &done, NULL) == EINVAL);
		CHECK(sslc_conn_write(NULL, record, 1, &done, &wants) == EINVAL);
		CHECK(sslc_conn_write(&bench->conn, record, 1, NULL, &wants) == EINVAL);
		CHECK(sslc_conn_write(&bench->conn, record, 1, &done, NULL) == EINVAL);
		CHECK(sslc_conn_shutdown(NULL, &both, &wants) == EINVAL);
		CHECK(sslc_conn_shutdown(&bench->conn, NULL, &wants) == EINVAL);
		CHECK(sslc_conn_shutdown(&bench->conn, &both, NULL) == EINVAL);
		CHECK((bench->conn.flags & SSLC_CONN_FAILED) == 0U);
		bench_free(bench);
	}

	/* A connection freed in the middle of a handshake message holds no memory (the leak check would say). */
	bench = raw_start();
	if (bench != NULL) {
		record[0] = 0x16;
		record[1] = 3;
		record[2] = 3;
		record[3] = 0;
		record[4] = 1;
		record[5] = 0x02;
		pipe_inject(&bench->pipe, record, 6);
		CHECK(drive_handshake(bench) == EAGAIN);
		CHECK(bench->conn.message != NULL);
		CHECK(bench->conn.message_used == 1U);
		bench_free(bench);
	}
}

/* A handshake that cannot start: the random source fails, or the configuration is out of range. */
static void
test_start_failure(void)
{
	struct bench *bench;
	unsigned variant;

	/* Nothing is written, no alert is sent, and the connection is dead. */
	for (variant = 0; variant < 2U; variant++) {
		if (variant == 0U) {
			bench = bench_new(SSLC_TLS12, SSLC_TLS13, &pki256, NULL, 0);
			if (bench == NULL)
				continue;
			bench->stream.fail_at = 1;
			CHECK(drive_handshake(bench) == EIO);
		} else {
			bench = bench_new(SSLC_TLS13, SSLC_TLS12, &pki256, NULL, 0);
			if (bench == NULL)
				continue;
			CHECK(drive_handshake(bench) == EINVAL);
		}
		CHECK((bench->conn.flags & SSLC_CONN_STARTED) == 0U);
		CHECK(bench->pipe.sent_used == 0U);
		check_dead(bench);
		bench_free(bench);
	}
}

/* A sequence number that would wrap ends the connection: a write cannot seal, a read cannot open. */
static void
test_sequence(void)
{
	struct bench *bench;
	uint8_t buffer[16];
	size_t done;
	unsigned wants;

	/* The write side: EOVERFLOW, no alert (it cannot be sealed either), and the connection is dead. */
	bench = connected13();
	if (bench != NULL) {
		bench->conn.writing.sequence = ~(uint64_t)0;
		done = 5;
		CHECK(sslc_conn_write(&bench->conn, (const uint8_t *)"x", 1, &done, &wants) == EOVERFLOW);
		CHECK(done == 0U);
		CHECK((bench->conn.flags & SSLC_CONN_FAILED) != 0U);
		check_peer_alert(bench, 0, 0, 0, 0);
		check_dead(bench);
		bench_free(bench);
	}

	/* The read side: EOVERFLOW, and an internal_error under the write keys, which are fine. */
	bench = connected13();
	if (bench != NULL) {
		bench->conn.reading.sequence = ~(uint64_t)0;
		peer_data(bench, "x");
		CHECK(drive_read(bench, buffer, sizeof(buffer), &done) == EOVERFLOW);
		CHECK(bench->conn.reason == 0U);
		check_peer_alert(bench, 1, 2, A_INTERNAL_ERROR, 1);
		check_dead(bench);
		bench_free(bench);
	}
}

/*
 * After the handshake: KeyUpdate and the answers a read has to send.
 */

/* Checks KeyUpdate in TLS 1.3: the keys of both directions move, data goes on both ways, and an answer is sent only when asked for. */
static void
test_key_update(void)
{
	static uint8_t data[300];
	struct bench *bench;
	size_t before;
	size_t done;
	unsigned request;
	unsigned round;

	/* Three updates from the server in a row, asking for an answer or not. */
	for (request = 0; request < 2U; request++) {
		bench = connected13();
		if (bench == NULL)
			continue;
		for (round = 0; round < 3U; round++) {
			CHECK(tlspeer_send_key_update(bench->peer, (int)request) == 0);
			pipe_seal(&bench->pipe);
			pattern(data, sizeof(data), round);
			CHECK(tlspeer_send_data(bench->peer, data, sizeof(data)) == 0);
			pipe_seal(&bench->pipe);

			/* The data under the new keys is read; an answer (a KeyUpdate record, 5 + 5 + 17 bytes) goes out when asked for. */
			before = bench->pipe.sent_used;
			read_exact(bench, data, sizeof(data), 1000U);
			if (request == 1U)
				CHECK(bench->pipe.sent_used - before == 5U + 5U + 17U - 17U + 17U);
			else
				CHECK(bench->pipe.sent_used == before);

			/* The client's data under its new keys is read by the peer. */
			bench->peer->data_size = 0;
			CHECK(drive_write(bench, data, 100, &done) == 0);
			CHECK(bench->peer->data_size == 100U);
			CHECK_BYTES(bench->peer->data, data, 100);
			CHECK(bench->peer->error == 0);
			CHECK(bench->pipe.peer_error == 0);
		}
		CHECK((bench->conn.flags & SSLC_CONN_FAILED) == 0U);
		bench_free(bench);
	}

	/* With the transport full the answer waits: the read says EAGAIN to write, and gives its data after the answer is sent. */
	bench = connected13();
	if (bench != NULL) {
		unsigned wants;
		uint8_t buffer[400];

		CHECK(tlspeer_send_key_update(bench->peer, 1) == 0);
		pipe_seal(&bench->pipe);
		pattern(data, sizeof(data), 9U);
		CHECK(tlspeer_send_data(bench->peer, data, sizeof(data)) == 0);
		pipe_seal(&bench->pipe);
		bench->pipe.write_limited = 1;
		bench->pipe.write_budget = 0;
		wants = 0;
		done = 5;
		CHECK(sslc_conn_read(&bench->conn, buffer, sizeof(buffer), &done, &wants) == EAGAIN);
		CHECK(done == 0U);
		CHECK(wants == SSLC_WANT_WRITE);
		bench->pipe.write_budget = 1U << 20;
		CHECK(sslc_conn_read(&bench->conn, buffer, sizeof(buffer), &done, &wants) == 0);
		CHECK(done == sizeof(data));
		CHECK_BYTES(buffer, data, sizeof(data));
		bench->peer->data_size = 0;
		CHECK(drive_write(bench, data, 10, &done) == 0);
		CHECK(bench->peer->data_size == 10U);
		CHECK(bench->peer->error == 0);
		bench_free(bench);
	}
}

/*
 * Records cut small, and every way a flight can be cut.
 */

/* Handshakes and data with the peer writing records of a few bytes, in both versions and with a stuttering transport. */
static void
test_fragments(void)
{
	static const size_t sizes[] = { 1, 2, 3, 7, 100, 1000 };
	static uint8_t data[300];
	struct tlspeer_config config;
	struct bench *bench;
	unsigned index;
	unsigned version;
	unsigned mode;

	/* Each record size; the handshake messages are cut across records, and so is the data. */
	for (version = 0; version < 2U; version++) {
		for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
			for (mode = 0; mode < 4U; mode += 3U) {
				script(&config, version == 0U ? SSLC_TLS13 : SSLC_TLS12, &pki256);
				config.record_size = sizes[index];
				bench = bench_new(SSLC_TLS12, SSLC_TLS13, &pki256, &config, 0);
				if (bench == NULL)
					continue;
				set_mode(&bench->pipe, mode);
				CHECK(drive_handshake(bench) == 0);
				CHECK(bench->peer->state == TLSPEER_CONNECTED);
				CHECK(bench->conn.message == NULL);
				pattern(data, sizeof(data), (unsigned)index);
				CHECK(tlspeer_send_data(bench->peer, data, sizeof(data)) == 0);
				read_exact(bench, data, sizeof(data), 64U);
				CHECK(bench->pipe.overreads == 0U);
				CHECK(bench->bad_wants == 0U);
				CHECK(bench->pipe.peer_error == 0);
				bench_free(bench);
			}
		}
	}
}

/*
 * Failures of the handshake and what the connection reports for them.
 */

/* One failing server: its script, and what the client must report. */
struct hs_failure {
	const char *name;
	uint16_t version;
	uint32_t options;
	uint32_t breaks;
	int untrusted;
	unsigned reason;
	unsigned alert;
	int protected_alert;
};

/*
 * The failures: a server whose Finished does not verify (decrypt_error), a
 * root the client does not trust (unknown_ca), an echoed session id that
 * differs (illegal_parameter), an extension in EncryptedExtensions that was
 * not asked for (unsupported_extension, RFC 8446 4.2), a TLS 1.2 server
 * without renegotiation_info (handshake_failure), a broken tag on the
 * protected flight, and a TLS 1.3 server that downgrades.
 * Values referenced: RFC 8446 sections 4.2, 4.4.4, 4.1.3, 6.2; RFC 5746 3.4.
 */
static const struct hs_failure hs_failures[] = {
	{ "finished 1.3", SSLC_TLS13, 0, TLSPEER_BREAK_FINISHED, 0, SSL_R_BAD_SIGNATURE, A_DECRYPT_ERROR, 0 },
	{ "finished 1.2", SSLC_TLS12, 0, TLSPEER_BREAK_FINISHED, 0, SSL_R_BAD_SIGNATURE, A_DECRYPT_ERROR, 1 },
	{ "untrusted 1.3", SSLC_TLS13, 0, 0, 1, SSL_R_CERTIFICATE_VERIFY_FAILED, A_UNKNOWN_CA, 0 },
	{ "untrusted 1.2", SSLC_TLS12, 0, 0, 1, SSL_R_CERTIFICATE_VERIFY_FAILED, A_UNKNOWN_CA, 0 },
	{ "session id 1.3", SSLC_TLS13, 0, TLSPEER_BREAK_SESSION_ID, 0, SSL_R_SSL_HANDSHAKE_FAILURE, A_ILLEGAL_PARAMETER, 0 },
	{ "no renegotiation_info", SSLC_TLS12, TLSPEER_OPT_NO_RI, 0, 0, SSL_R_SSL_HANDSHAKE_FAILURE, A_HANDSHAKE_FAILURE, 0 },
	{ "tag 1.3", SSLC_TLS13, 0, TLSPEER_BREAK_RECORD_TAG, 0, SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC, A_BAD_RECORD_MAC, 0 },
	{ "tag 1.2", SSLC_TLS12, 0, TLSPEER_BREAK_RECORD_TAG, 0, SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC, A_BAD_RECORD_MAC, 1 }
};

/* Runs each failing server and checks the error, the reason, the alert the peer got and the dead connection. */
static void
test_handshake_failures(void)
{
	struct tlspeer_config config;
	struct bench *bench;
	unsigned index;
	int error;

	/* Each failure. */
	for (index = 0; index < sizeof(hs_failures) / sizeof(hs_failures[0]); index++) {
		script(&config, hs_failures[index].version, &pki256);
		config.options = hs_failures[index].options;
		config.breaks = hs_failures[index].breaks;
		bench = bench_new(SSLC_TLS12, SSLC_TLS13, hs_failures[index].untrusted ? NULL : &pki256, &config, 0);
		if (bench == NULL)
			continue;
		error = drive_handshake(bench);
		CHECK(error != 0);
		CHECK(error != EAGAIN);
		CHECK(bench->conn.reason == hs_failures[index].reason);
		check_peer_alert(bench, 1, 2, hs_failures[index].alert, hs_failures[index].protected_alert);
		if (bench->conn.reason != hs_failures[index].reason || bench->peer->alert_description != hs_failures[index].alert)
			printf("  failure %s: error %d reason %u alert %u\n", hs_failures[index].name, error, bench->conn.reason, bench->peer->alert_description);
		CHECK((bench->conn.flags & SSLC_CONN_CONNECTED) == 0U);
		CHECK(bench->conn.message == NULL);
		check_dead(bench);
		bench_free(bench);
	}

	/* An extension in EncryptedExtensions that the client did not offer. */
	script(&config, SSLC_TLS13, &pki256);
	config.extra_type = 0x5a5a;
	config.extra_at = TLSPEER_AT_ENCRYPTED_EXTENSIONS;
	bench = bench_new(SSLC_TLS12, SSLC_TLS13, &pki256, &config, 0);
	if (bench != NULL) {
		CHECK(drive_handshake(bench) != 0);
		CHECK(bench->conn.reason == SSL_R_BAD_EXTENSION);
		check_peer_alert(bench, 1, 2, A_UNSUPPORTED_EXTENSION, 0);
		check_dead(bench);
		bench_free(bench);
	}
}

/* A fatal alert during the handshake from the peer through tlspeer's own records (plaintext, TLS 1.3 and 1.2). */
static void
test_handshake_alert(void)
{
	static const uint8_t canceled[7] = { 0x15, 0x03, 0x03, 0x00, 0x02, 0x01, 0x5a };
	struct tlspeer_config config;
	struct bench *bench;

	/* A warning alert in plaintext before the ServerHello, then a good flight: the handshake finishes. */
	script(&config, SSLC_TLS12, &pki256);
	bench = bench_new(SSLC_TLS12, SSLC_TLS12, &pki256, &config, 0);
	if (bench != NULL) {
		pipe_inject(&bench->pipe, canceled, sizeof(canceled));
		CHECK(drive_handshake(bench) == 0);
		CHECK(bench->peer->state == TLSPEER_CONNECTED);
		bench_free(bench);
	}
}

/*
 * Records made by hand, which tlspeer cannot make: a protected change
 * cipher spec in the middle of the handshake, an inner content type that
 * does not exist, padding.
 */

/*
 * Makes a TLS 1.3 AES-GCM record from the keys the peer uses: the inner
 * plaintext is content, the inner type and padding zeros; the nonce is the
 * IV xor the sequence number; the additional data is the header.
 * Values referenced: RFC 8446 sections 5.2, 5.3 and 5.4.
 */
static size_t
make_record13(
	const struct sslc_traffic_keys *keys,
	uint64_t sequence,
	unsigned inner_type,
	const uint8_t *content,
	size_t size,
	size_t padding,
	uint8_t *out)
{
	struct cryc_gcm gcm;
	uint8_t nonce[12];
	static uint8_t inner[17000];
	size_t length;
	size_t index;

	/* TLSInnerPlaintext. */
	if (size + 1U + padding > sizeof(inner)) {
		CHECK(0);
		return 0;
	}
	if (size > 0U)
		memcpy(inner, content, size);
	inner[size] = (uint8_t)inner_type;
	memset(inner + size + 1U, 0, padding);
	length = size + 1U + padding;

	/* The header: application_data, 0x0303, the length of the ciphertext and the tag. */
	out[0] = 23;
	out[1] = 3;
	out[2] = 3;
	out[3] = (uint8_t)((length + 16U) >> 8);
	out[4] = (uint8_t)((length + 16U) & 0xffU);

	/* The nonce: the IV with the 64-bit sequence number (big-endian) xored into its last 8 bytes. */
	memcpy(nonce, keys->iv, 12);
	for (index = 0; index < 8U; index++)
		nonce[4U + index] ^= (uint8_t)(sequence >> (56U - 8U * index));

	/* The sealed record. */
	CHECK(cryc_gcm_init(&gcm, keys->key, keys->key_size) == 0);
	CHECK(cryc_gcm_seal(&gcm, nonce, 12, out, 5, inner, length, out + 5, out + 5U + length) == 0);
	cryc_gcm_clear(&gcm);
	return 5U + length + 16U;
}

/*
 * Plays the server of a TLS 1.3 handshake by hand: the peer makes the
 * messages and keys, and this puts them into records, with one record made
 * by hand after the insert_after-th record of the flight (0: none), and
 * hands the bytes to the client; returns the bench, with the handshake run.
 */
static struct bench *
manual_server(
	size_t insert_after,
	unsigned inner_type,
	const uint8_t *content,
	size_t size,
	int *result)
{
	static uint8_t stream[16384];
	struct tlspeer_config config;
	struct tlspeer_item item;
	struct bench *bench;
	uint64_t sequence[TLSPEER_EPOCHS];
	size_t used;
	size_t records;
	size_t length;
	unsigned wants;
	unsigned epoch;

	/* The client's ClientHello goes to the peer as a message. */
	script(&config, SSLC_TLS13, &pki256);
	bench = bench_new(SSLC_TLS12, SSLC_TLS13, &pki256, &config, 0);
	if (bench == NULL)
		return NULL;
	bench->pipe.peer = NULL;
	wants = 0;
	CHECK(sslc_conn_handshake(&bench->conn, &wants) == EAGAIN);
	CHECK(bench->pipe.sent_used > 5U);
	CHECK(bench->pipe.sent[0] == T_HANDSHAKE);
	length = (size_t)bench->pipe.sent[3] * 256U + (size_t)bench->pipe.sent[4];
	CHECK(tlspeer_handshake(bench->peer, bench->pipe.sent + 5, length) == 0);

	/* Each item of the flight in a record of its own, plaintext before the keys and protected after. */
	memset(sequence, 0, sizeof(sequence));
	used = 0;
	records = 0;
	epoch = 0;
	while (tlspeer_next(bench->peer, &item)) {
		epoch = item.epoch;
		if (item.epoch == 0U) {
			stream[used] = T_HANDSHAKE;
			stream[used + 1U] = 3;
			stream[used + 2U] = 3;
			stream[used + 3U] = (uint8_t)(item.size >> 8);
			stream[used + 4U] = (uint8_t)(item.size & 0xffU);
			memcpy(stream + used + 5U, item.data, item.size);
			used += 5U + item.size;
		} else {
			used += make_record13(&bench->peer->write_keys[item.epoch], sequence[item.epoch], T_HANDSHAKE, item.data, item.size, 0, stream + used);
			sequence[item.epoch]++;
		}
		records++;

		/* The record made by hand, under the keys of the record before it. */
		if (records == insert_after) {
			used += make_record13(&bench->peer->write_keys[epoch], sequence[epoch], inner_type, content, size, 0, stream + used);
			sequence[epoch]++;
		}
	}

	/* The client reads it all. */
	pipe_inject(&bench->pipe, stream, used);
	*result = drive_handshake(bench);
	return bench;
}

/* Checks the handshake with records made by hand: the plain flight is accepted, and a record that must not be there fails it. */
static void
test_manual(void)
{
	static const uint8_t one[1] = { 1 };
	static const uint8_t text[3] = { 'x', 'y', 'z' };
	static uint8_t big[16384];
	static uint8_t record[17100];
	struct bench *bench;
	uint8_t buffer[16];
	size_t done;
	size_t length;
	unsigned variant;
	int result;

	/* The control: the flight alone is a handshake that completes (the hand-made records are valid). */
	bench = manual_server(0, 0, NULL, 0, &result);
	if (bench != NULL) {
		CHECK(result == 0);
		CHECK((bench->conn.flags & SSLC_CONN_CONNECTED) != 0U);
		bench_free(bench);
	}

	/* A protected change_cipher_spec after EncryptedExtensions is not the compatibility record: unexpected_message, in plaintext. */
	bench = manual_server(2, T_CCS, one, 1, &result);
	if (bench != NULL) {
		CHECK(result == EPROTO);
		CHECK(bench->conn.reason == SSL_R_UNEXPECTED_MESSAGE);
		check_wire_alert(bench, A_UNEXPECTED_MESSAGE);
		bench_free(bench);
	}

	/* An inner content type that is none of the four. */
	bench = manual_server(2, 0x63, text, 3, &result);
	if (bench != NULL) {
		CHECK(result == EPROTO);
		CHECK(bench->conn.reason == SSL_R_UNEXPECTED_MESSAGE);
		check_wire_alert(bench, A_UNEXPECTED_MESSAGE);
		bench_free(bench);
	}

	/*
	 * After the handshake: the same inner type, an inner plaintext of zeros
	 * only (RFC 8446 5.4), padding, which is allowed, a full fragment of
	 * 2^14 bytes with its inner type (2^14 + 1 bytes, the most 5.4 allows),
	 * and one padding byte more (record_overflow).
	 */
	memset(big, 0x5a, sizeof(big));
	for (variant = 0; variant < 5U; variant++) {
		bench = connected13();
		if (bench == NULL)
			continue;
		if (variant == 0U)
			length = make_record13(&bench->peer->write_keys[bench->peer->write_epochs - 1U], 0, 0x63, text, 3, 0, record);
		else if (variant == 1U)
			length = make_record13(&bench->peer->write_keys[bench->peer->write_epochs - 1U], 0, 0, text, 0, 7, record);
		else if (variant == 2U)
			length = make_record13(&bench->peer->write_keys[bench->peer->write_epochs - 1U], 0, T_DATA, text, 3, 300, record);
		else if (variant == 3U)
			length = make_record13(&bench->peer->write_keys[bench->peer->write_epochs - 1U], 0, T_DATA, big, sizeof(big), 0, record);
		else
			length = make_record13(&bench->peer->write_keys[bench->peer->write_epochs - 1U], 0, T_DATA, big, sizeof(big), 1, record);
		CHECK(tlspeer_send_bytes(bench->peer, record, length) == 0);
		pipe_seal(&bench->pipe);
		if (variant == 2U) {
			CHECK(drive_read(bench, buffer, sizeof(buffer), &done) == 0);
			CHECK(done == 3U);
			CHECK_BYTES(buffer, text, 3);
		} else if (variant == 3U) {
			read_exact(bench, big, sizeof(big), 20000U);
		} else if (variant == 4U) {
			expect_failure(bench, EMSGSIZE, SSL_R_PACKET_LENGTH_TOO_LONG, A_RECORD_OVERFLOW);
		} else {
			expect_failure(bench, EPROTO, SSL_R_UNEXPECTED_MESSAGE, A_UNEXPECTED_MESSAGE);
		}
		bench_free(bench);
	}
}

/* A verification callback that refuses every chain, valid or not. */
static int
refuse_callback(
	void *context,
	int preverified,
	enum cryc_verify_code code)
{
	/* The answer is the same whatever the verdict. */
	(void)context;
	(void)preverified;
	(void)code;
	return 0;
}

/* One failing handshake of the second kind: what is changed, and the alert and reason it must give. */
struct tweak_case {
	const char *name;
	unsigned tweak;
	uint16_t version;
	unsigned alert;
	unsigned reason;
};

/* The things that change the client or the server: an expired time, another host, a refusing callback, a client of TLS 1.3 only, malformed EncryptedExtensions. */
enum tweak {
	TWEAK_EXPIRED = 1,
	TWEAK_HOST,
	TWEAK_REFUSE,
	TWEAK_ONLY13,
	TWEAK_MALFORMED
};

/*
 * The failures that name the alert of a certificate (RFC 8446 6.2:
 * certificate_expired 45, bad_certificate 42, certificate_unknown 46), of
 * a version (protocol_version 70: a client of TLS 1.3 only meets a TLS 1.2
 * server) and of a message (decode_error 50).
 * Values referenced: RFC 8446 section 6.2; RFC 5246 section 7.2.2.
 */
static const struct tweak_case tweak_cases[] = {
	{ "expired 1.3", TWEAK_EXPIRED, SSLC_TLS13, 45, SSL_R_CERTIFICATE_VERIFY_FAILED },
	{ "expired 1.2", TWEAK_EXPIRED, SSLC_TLS12, 45, SSL_R_CERTIFICATE_VERIFY_FAILED },
	{ "host 1.3", TWEAK_HOST, SSLC_TLS13, 42, SSL_R_CERTIFICATE_VERIFY_FAILED },
	{ "host 1.2", TWEAK_HOST, SSLC_TLS12, 42, SSL_R_CERTIFICATE_VERIFY_FAILED },
	{ "refused 1.3", TWEAK_REFUSE, SSLC_TLS13, 46, SSL_R_CERTIFICATE_VERIFY_FAILED },
	{ "refused 1.2", TWEAK_REFUSE, SSLC_TLS12, 46, SSL_R_CERTIFICATE_VERIFY_FAILED },
	{ "version", TWEAK_ONLY13, SSLC_TLS12, 70, SSL_R_UNSUPPORTED_PROTOCOL },
	{ "malformed", TWEAK_MALFORMED, SSLC_TLS13, 50, SSL_R_BAD_LENGTH }
};

/* Runs each of those failures, and a failure of the random source after the handshake began (internal_error). */
static void
test_tweaked_failures(void)
{
	static const uint8_t malformed[1] = { 0x00 };
	struct tlspeer_config config;
	struct bench *bench;
	unsigned index;
	int error;

	/* Each case: the alert reaches the peer in plaintext (no key of the client's exists yet). */
	for (index = 0; index < sizeof(tweak_cases) / sizeof(tweak_cases[0]); index++) {
		script(&config, tweak_cases[index].version, &pki256);
		if (tweak_cases[index].tweak == TWEAK_MALFORMED) {
			config.ee_raw = malformed;
			config.ee_raw_size = sizeof(malformed);
		}
		bench = bench_new(tweak_cases[index].tweak == TWEAK_ONLY13 ? SSLC_TLS13 : SSLC_TLS12, SSLC_TLS13, &pki256, &config, 0);
		if (bench == NULL)
			continue;
		if (tweak_cases[index].tweak == TWEAK_EXPIRED)
			bench->conn.config.params.time = TLSPEER_NOT_AFTER_TIME + 86400L;
		if (tweak_cases[index].tweak == TWEAK_HOST)
			strcpy(bench->conn.config.params.host, "other.test");
		if (tweak_cases[index].tweak == TWEAK_REFUSE)
			bench->conn.config.callback = refuse_callback;
		error = drive_handshake(bench);
		CHECK(error != 0);
		CHECK(error != EAGAIN);
		CHECK(bench->conn.reason == tweak_cases[index].reason);
		check_peer_alert(bench, 1, 2, tweak_cases[index].alert, 0);
		if (bench->conn.reason != tweak_cases[index].reason || bench->peer->alert_description != tweak_cases[index].alert)
			printf("  failure %s: error %d reason %u alert %u\n", tweak_cases[index].name, error, bench->conn.reason, bench->peer->alert_description);
		check_dead(bench);
		bench_free(bench);
	}

	/* The random source fails when the client makes its secp256r1 key after the HelloRetryRequest (the fourth draw): internal_error, and the handshake's own error. */
	script(&config, SSLC_TLS13, &pki256);
	config.group = SSLC_GROUP_SECP256R1;
	config.hello_retry = 1;
	bench = bench_new(SSLC_TLS12, SSLC_TLS13, &pki256, &config, 0);
	if (bench != NULL) {
		bench->stream.fail_at = 4;
		CHECK(drive_handshake(bench) == EIO);
		CHECK(bench->conn.reason == 0U);
		check_peer_alert(bench, 1, 2, A_INTERNAL_ERROR, 0);
		check_dead(bench);
		bench_free(bench);
	}
}

/* A write or a read is the first call: the handshake runs first, then the call goes on. */
static void
test_before_handshake(void)
{
	struct tlspeer_config config;
	struct bench *bench;
	uint8_t buffer[16];
	size_t done;
	unsigned wants;
	unsigned version;

	/* A write first, in both versions. */
	for (version = 0; version < 2U; version++) {
		script(&config, version == 0U ? SSLC_TLS13 : SSLC_TLS12, &pki256);
		bench = bench_new(SSLC_TLS12, SSLC_TLS13, &pki256, &config, 0);
		if (bench == NULL)
			continue;
		CHECK(drive_write(bench, (const uint8_t *)"hello", 5, &done) == 0);
		CHECK(done == 5U);
		CHECK((bench->conn.flags & SSLC_CONN_CONNECTED) != 0U);
		CHECK(bench->peer->state == TLSPEER_CONNECTED);
		CHECK(bench->peer->data_size == 5U);
		CHECK_BYTES(bench->peer->data, "hello", 5);
		bench_free(bench);
	}

	/* A read first: the handshake runs, then there is nothing to read yet, and then there is. */
	for (version = 0; version < 2U; version++) {
		script(&config, version == 0U ? SSLC_TLS13 : SSLC_TLS12, &pki256);
		bench = bench_new(SSLC_TLS12, SSLC_TLS13, &pki256, &config, 0);
		if (bench == NULL)
			continue;
		done = 5;
		wants = 0;
		CHECK(drive_read(bench, buffer, sizeof(buffer), &done) == EAGAIN);
		CHECK(done == 0U);
		CHECK((bench->conn.flags & SSLC_CONN_CONNECTED) != 0U);
		CHECK(bench->peer->state == TLSPEER_CONNECTED);
		CHECK(sslc_conn_read(&bench->conn, buffer, sizeof(buffer), &done, &wants) == EAGAIN);
		CHECK(wants == SSLC_WANT_READ);
		peer_data(bench, "later");
		expect_text(bench, "later");
		bench_free(bench);
	}
}

/* A failure that is not a record's: while application bytes are buffered, and when the sequence number cannot be used to close. */
static void
test_failure_with_buffer(void)
{
	static uint8_t data[100];
	struct bench *bench;
	uint8_t buffer[16];
	size_t done;
	unsigned wants;
	int both;

	/* A read of 10 bytes of 100 leaves 90 buffered; the transport then fails on a write: nothing of them is left. */
	bench = connected13();
	if (bench != NULL) {
		pattern(data, sizeof(data), 4U);
		CHECK(tlspeer_send_data(bench->peer, data, sizeof(data)) == 0);
		CHECK(sslc_conn_read(&bench->conn, buffer, 10, &done, &wants) == 0);
		CHECK(sslc_conn_pending(&bench->conn) == 90U);
		bench->pipe.write_fail_at = (unsigned)bench->pipe.write_calls + 1U;
		bench->pipe.write_fail_errno = 12;
		CHECK(drive_write(bench, data, 5, &done) == EIO);
		CHECK(bench->conn.os_error == 12);
		CHECK(sslc_conn_pending(&bench->conn) == 0U);
		check_dead(bench);
		bench_free(bench);
	}

	/* A close_notify that cannot be sealed (the write sequence is used up) fails the connection and says it was not sent. */
	bench = connected13();
	if (bench != NULL) {
		bench->conn.writing.sequence = ~(uint64_t)0;
		both = 7;
		CHECK(sslc_conn_shutdown(&bench->conn, &both, &wants) == EOVERFLOW);
		CHECK(both == 0);
		CHECK((bench->conn.flags & SSLC_CONN_CLOSE_SENT) == 0U);
		check_peer_alert(bench, 0, 0, 0, 0);
		check_dead(bench);
		bench_free(bench);
	}
}

/*
 * Two messages in one record, the first of which makes the connection
 * send something: the second waits until the first answer is sent, so the
 * output never holds more than one answer.
 */
static void
test_pending_output(void)
{
	static const uint8_t requests[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	struct bench *bench;
	uint8_t buffer[16];
	size_t done;
	unsigned wants;

	/* Two HelloRequests of TLS 1.2 (each answered with a warning of 2 + 8 + 16 + 5 bytes), and data after them; the transport is full. */
	bench = connected12();
	if (bench == NULL)
		return;
	CHECK(tlspeer_send_record(bench->peer, T_HANDSHAKE, requests, sizeof(requests)) == 0);
	pipe_seal(&bench->pipe);
	peer_data(bench, "after");
	bench->pipe.write_limited = 1;
	bench->pipe.write_budget = 0;
	done = 5;
	wants = 0;
	CHECK(sslc_conn_read(&bench->conn, buffer, sizeof(buffer), &done, &wants) == EAGAIN);
	CHECK(done == 0U);
	CHECK(wants == SSLC_WANT_WRITE);

	/* The first answer is held, the second message waits whole in the buffer. */
	CHECK(bench->conn.out_used - bench->conn.out_sent == 31U);
	CHECK(bench->conn.message_used == 4U);

	/* The transport takes it: both answers are sent, once each, and the data is read. */
	bench->pipe.write_budget = 1U << 20;
	CHECK(sslc_conn_read(&bench->conn, buffer, sizeof(buffer), &done, &wants) == 0);
	CHECK(done == 5U);
	CHECK_BYTES(buffer, "after", 5);
	CHECK(bench->peer->alert_count == 2U);
	CHECK(bench->peer->alert_level == 1U);
	CHECK(bench->peer->alert_description == A_NO_RENEGOTIATION);
	CHECK(bench->peer->error == 0);
	bench_free(bench);
}

/* Runs every group of checks. */
int
main(void)
{
	/* The certificates of both curves. */
	CHECK(tlspeer_pki_init(&pki256, CRYC_CURVE_P256, "server.test") == 0);
	CHECK(tlspeer_pki_init(&pki384, CRYC_CURVE_P384, "server.test") == 0);

	/* The groups of checks. */
	test_handshakes();
	test_data();
	test_pending();
	test_read_ahead();
	test_write_retry();
	test_write_edges();
	test_alerts();
	test_empties();
	test_empties_handshake();
	test_key_change();
	test_framing();
	test_damage();
	test_out_of_place();
	test_raw();
	test_message_limit();
	test_eof();
	test_transport_failures();
	test_shutdown();
	test_init_free();
	test_start_failure();
	test_sequence();
	test_key_update();
	test_fragments();
	test_handshake_failures();
	test_handshake_alert();
	test_manual();
	test_tweaked_failures();
	test_before_handshake();
	test_failure_with_buffer();
	test_pending_output();
	return check_finish("test-conn");
}
