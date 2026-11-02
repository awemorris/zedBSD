/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the handshake messages (sslc-msg.h): the ClientHello writer, the
 * parsers of the messages a client reads, and the small writers.
 *
 * The expected values come from RFC 8446 section 4 and Appendix B.3, RFC
 * 5246 section 7.4, RFC 8422 section 5, RFC 6066 section 3 and the traces
 * of RFC 8448 (the messages are read from its labelled values; the fields
 * the tests expect are decoded from the RFC's dumps by hand and written
 * out here).  Each parser is also compared with a second reading of the
 * same rules written in this file (the models), on tables of bad inputs,
 * on every prefix of good messages, and on mutated and random inputs, all
 * in heap blocks of the exact size so that a read past the end is caught.
 *
 * The ClientHello writer is compared with the RFC 8448 ClientHellos
 * extension by extension: the extensions libssl-compat sends are the same
 * bytes; the extensions it does not send are listed in the checks.
 */

#include "check.h"
#include "vecfile.h"

#include "userland/base/libssl-compat/sslc-msg.h"

/* The state of the deterministic generator of the test inputs. */
static uint32_t random_state = 0x6b43a9b5U;

/* Returns the next pseudo-random 32-bit value (xorshift). */
static uint32_t
random_next(void)
{
	/* The three xorshift steps. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

/* Returns a pseudo-random value below limit (limit above 0). */
static uint32_t
random_below(
	uint32_t limit)
{
	/* The top bits are the better ones. */
	return (random_next() >> 8) % limit;
}

/* The alerts under their short names. */
#define DEC	SSLC_ALERT_DECODE_ERROR
#define UNS	SSLC_ALERT_UNSUPPORTED_EXTENSION
#define ILL	SSLC_ALERT_ILLEGAL_PARAMETER
#define BADC	SSLC_ALERT_BAD_CERTIFICATE

/* Returns a heap block of exactly size bytes holding a copy of data (a block of 1 byte for size 0). */
static uint8_t *
exact_copy(
	const uint8_t *data,
	size_t size)
{
	uint8_t *block;

	/* Exact size, so that AddressSanitizer sees every read past the end. */
	block = malloc(size == 0U ? 1U : size);
	if (block == NULL) {
		printf("FAIL out of memory\n");
		exit(EXIT_FAILURE);
	}
	if (size != 0U)
		memcpy(block, data, size);
	return block;
}

/* A growing message under construction (the tests build messages with this, not with the code under test). */
struct buffer {
	uint8_t *bytes;
	size_t size;
};

/* Largest message a test builds. */
#define BUFFER_MAX	(1U << 17)

/* Starts an empty buffer. */
static void
buffer_init(
	struct buffer *buffer)
{
	/* A block big enough for every test message. */
	buffer->bytes = malloc(BUFFER_MAX);
	if (buffer->bytes == NULL) {
		printf("FAIL out of memory\n");
		exit(EXIT_FAILURE);
	}
	buffer->size = 0;
}

/* Frees a buffer. */
static void
buffer_free(
	struct buffer *buffer)
{
	/* The block, once. */
	free(buffer->bytes);
	buffer->bytes = NULL;
	buffer->size = 0;
}

/* Appends one byte. */
static void
put8(
	struct buffer *buffer,
	unsigned value)
{
	/* One byte, low 8 bits. */
	buffer->bytes[buffer->size++] = (uint8_t)(value & 0xffU);
}

/* Appends a 2-byte big-endian value. */
static void
put16(
	struct buffer *buffer,
	unsigned value)
{
	/* High byte first. */
	put8(buffer, (value >> 8) & 0xffU);
	put8(buffer, value & 0xffU);
}

/* Appends a 3-byte big-endian value. */
static void
put24(
	struct buffer *buffer,
	unsigned long value)
{
	/* High byte first. */
	put8(buffer, (unsigned)((value >> 16) & 0xffU));
	put8(buffer, (unsigned)((value >> 8) & 0xffU));
	put8(buffer, (unsigned)(value & 0xffU));
}

/* Appends size bytes. */
static void
put_bytes(
	struct buffer *buffer,
	const uint8_t *data,
	size_t size)
{
	/* A copy. */
	if (size != 0U)
		memcpy(buffer->bytes + buffer->size, data, size);
	buffer->size += size;
}

/* Appends size copies of one byte. */
static void
put_fill(
	struct buffer *buffer,
	unsigned value,
	size_t size)
{
	size_t index;

	/* One at a time. */
	for (index = 0; index < size; index++)
		put8(buffer, value);
}

/* Appends an extension: its type, its length and its data. */
static void
put_extension(
	struct buffer *buffer,
	unsigned type,
	const uint8_t *data,
	size_t size)
{
	/* The header, then the data. */
	put16(buffer, type);
	put16(buffer, (unsigned)size);
	put_bytes(buffer, data, size);
}

/* Appends the decoded bytes of a hex string; returns 0, or -1 for a bad string. */
static int
put_hex(
	struct buffer *buffer,
	const char *text)
{
	size_t size;

	/* The decoder of check.h. */
	size = hex_decode(text, buffer->bytes + buffer->size, BUFFER_MAX - buffer->size);
	if (size == (size_t)-1)
		return -1;
	buffer->size += size;
	return 0;
}

/* Reads a big-endian 16-bit value at p. */
static unsigned
get16(
	const uint8_t *p)
{
	/* High byte first. */
	return (unsigned)p[0] * 256U + (unsigned)p[1];
}

/* Reads a big-endian 24-bit value at p. */
static unsigned long
get24(
	const uint8_t *p)
{
	/* High byte first. */
	return ((unsigned long)p[0] << 16) | ((unsigned long)p[1] << 8) | (unsigned long)p[2];
}

/* Returns 1 when the status and the alert agree with each other as the header says. */
static int
alert_agrees(
	int status,
	unsigned alert)
{
	/* Success writes alert 0; EINVAL is decode_error; EPROTO is one of the three. */
	if (status == 0)
		return alert == 0U;
	if (status == EINVAL)
		return alert == DEC;
	if (status == EPROTO)
		return alert == UNS || alert == ILL || alert == BADC;
	return 0;
}

/* Returns 1 when size bytes at view lie inside the body (a view of size 0 may be NULL). */
static int
inside(
	const uint8_t *view,
	size_t size,
	const uint8_t *body,
	size_t body_size)
{
	/* An empty view needs no place. */
	if (size == 0U && view == NULL)
		return 1;
	if (view < body)
		return 0;
	if ((size_t)(view - body) > body_size)
		return 0;
	return size <= body_size - (size_t)(view - body);
}

/*
 * The RFC 8448 values.
 */

/* The section titles of the RFC's trace, as vectors.py writes them. */
#define SEC3	"3 Simple 1-RTT Handshake"
#define SEC4	"4 Resumed 0-RTT Handshake"
#define SEC5	"5 HelloRetryRequest"
#define SEC6	"6 Client Authentication"
#define SEC7	"7 Compatibility Mode"

/*
 * Reads the nth labelled value of the RFC 8448 trace whose section is
 * section, whose step contains step and whose label is label; returns its
 * size, or (size_t)-1.
 */
static size_t
rfc_value(
	const char *section,
	const char *step,
	const char *label,
	size_t nth,
	uint8_t *out,
	size_t room)
{
	struct vec_file file;
	size_t found;
	size_t size;

	/* The trace file. */
	size = (size_t)-1;
	if (vec_open(&file, "rfc8448") != 0) {
		CHECK(0);
		return size;
	}

	/* The nth record that matches. */
	found = 0;
	while (vec_next(&file)) {
		if (strcmp(vec_get(&file, "section"), section) != 0)
			continue;
		if (strstr(vec_get(&file, "step"), step) == NULL)
			continue;
		if (strcmp(vec_get(&file, "label"), label) != 0)
			continue;
		if (found == nth) {
			size = vec_hex(&file, "hex", out, room);
			break;
		}
		found++;
	}
	vec_close(&file);
	CHECK(size != (size_t)-1);
	return size;
}

/*
 * A ClientHello taken apart by this file's own reader: offsets, not the
 * code under test.
 */
struct decoded_hello {
	size_t random;
	size_t session_size;
	size_t session;
	size_t suites;
	size_t suite_bytes;
	size_t compression;
	size_t ext_block;
	size_t ext_count;
	unsigned ext_type[32];
	size_t ext_start[32];
	size_t ext_data[32];
	size_t ext_size[32];
};

/* Takes a ClientHello apart; returns 0, or -1 when its framing is wrong. */
static int
decode_hello(
	const uint8_t *message,
	size_t size,
	struct decoded_hello *hello)
{
	size_t position;
	size_t end;
	size_t length;

	/* The handshake header: type 1 and a length that covers the rest. */
	memset(hello, 0, sizeof(*hello));
	if (size < 4U || message[0] != 1U || get24(message + 1) != (unsigned long)(size - 4U))
		return -1;

	/* The version and the random. */
	if (size < 4U + 2U + 32U + 1U)
		return -1;
	if (get16(message + 4) != 0x0303U)
		return -1;
	hello->random = 6;
	position = 38;

	/* The session id. */
	hello->session_size = message[position];
	hello->session = position + 1U;
	position += 1U + hello->session_size;

	/* The suites. */
	if (position + 2U > size)
		return -1;
	length = get16(message + position);
	hello->suites = position + 2U;
	hello->suite_bytes = length;
	position += 2U + length;

	/* The compression methods: one, null. */
	if (position + 2U > size)
		return -1;
	if (message[position] != 1U || message[position + 1U] != 0U)
		return -1;
	hello->compression = position;
	position += 2U;

	/* The extensions block, which ends the message. */
	if (position + 2U > size)
		return -1;
	length = get16(message + position);
	hello->ext_block = position;
	position += 2U;
	if (position + length != size)
		return -1;
	end = size;

	/* Each extension. */
	while (position < end) {
		if (position + 4U > end || hello->ext_count >= 32U)
			return -1;
		hello->ext_type[hello->ext_count] = get16(message + position);
		hello->ext_start[hello->ext_count] = position;
		hello->ext_data[hello->ext_count] = position + 4U;
		hello->ext_size[hello->ext_count] = get16(message + position + 2);
		position += 4U + hello->ext_size[hello->ext_count];
		if (position > end)
			return -1;
		hello->ext_count++;
	}
	return 0;
}

/* Returns the index of the extension of the type, or -1. */
static int
find_extension(
	const struct decoded_hello *hello,
	unsigned type)
{
	size_t index;

	/* The first of the type. */
	for (index = 0; index < hello->ext_count; index++) {
		if (hello->ext_type[index] == type)
			return (int)index;
	}
	return -1;
}

/*
 * The ClientHello writer.
 */

/* The inputs of one writer run and the arrays they point to. */
struct hello_input {
	struct sslc_client_hello hello;
	uint16_t suites[64];
	uint16_t groups[64];
	uint16_t schemes[64];
	uint8_t shares[SSLC_MSG_MAX_SHARES][80];
	uint8_t cookie[SSLC_MSG_COOKIE_MAX + 8U];
	char name[400];
};

/* Fills an input with a typical ClientHello: both versions, one suite of each, one share. */
static void
input_typical(
	struct hello_input *input,
	uint16_t min_version,
	uint16_t max_version)
{
	size_t index;

	/* A known random and no session id. */
	memset(input, 0, sizeof(*input));
	for (index = 0; index < 32U; index++)
		input->hello.random[index] = (uint8_t)index;

	/* The versions. */
	input->hello.min_version = min_version;
	input->hello.max_version = max_version;

	/* The lists. */
	input->suites[0] = SSLC_SUITE_AES_128_GCM_SHA256;
	input->suites[1] = SSLC_SUITE_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256;
	input->hello.suites = input->suites;
	input->hello.suite_count = 2;
	input->groups[0] = SSLC_GROUP_X25519;
	input->groups[1] = SSLC_GROUP_SECP256R1;
	input->hello.groups = input->groups;
	input->hello.group_count = 2;
	input->schemes[0] = SSLC_SCHEME_ECDSA_SECP256R1_SHA256;
	input->schemes[1] = SSLC_SCHEME_RSA_PSS_RSAE_SHA256;
	input->schemes[2] = SSLC_SCHEME_RSA_PKCS1_SHA256;
	input->hello.schemes = input->schemes;
	input->hello.scheme_count = 3;

	/* One x25519 share when 1.3 is offered. */
	if (max_version == SSLC_TLS13) {
		for (index = 0; index < 32U; index++)
			input->shares[0][index] = (uint8_t)(0xa0U + index);
		input->hello.shares[0].group = SSLC_GROUP_X25519;
		input->hello.shares[0].key = input->shares[0];
		input->hello.shares[0].key_size = 32;
		input->hello.share_count = 1;
	}
}

/*
 * Builds the bytes the header says a ClientHello is, with this file's
 * builder, for comparison with the writer.
 */
static void
model_client_hello(
	const struct sslc_client_hello *hello,
	struct buffer *out)
{
	struct buffer body;
	struct buffer exts;
	struct buffer data;
	size_t index;
	int offer12;
	int offer13;
	size_t name_size;

	/* Which versions are offered. */
	offer12 = hello->min_version == SSLC_TLS12;
	offer13 = hello->max_version == SSLC_TLS13;
	buffer_init(&body);
	buffer_init(&exts);
	buffer_init(&data);

	/* server_name. */
	if (hello->server_name != NULL) {
		name_size = strlen(hello->server_name);
		data.size = 0;
		put16(&data, (unsigned)(name_size + 3U));
		put8(&data, 0);
		put16(&data, (unsigned)name_size);
		put_bytes(&data, (const uint8_t *)hello->server_name, name_size);
		put_extension(&exts, 0, data.bytes, data.size);
	}

	/* supported_groups. */
	data.size = 0;
	put16(&data, (unsigned)(hello->group_count * 2U));
	for (index = 0; index < hello->group_count; index++)
		put16(&data, hello->groups[index]);
	put_extension(&exts, 10, data.bytes, data.size);

	/* ec_point_formats and extended_master_secret. */
	if (offer12) {
		data.size = 0;
		put8(&data, 1);
		put8(&data, 0);
		put_extension(&exts, 11, data.bytes, data.size);
		put_extension(&exts, 23, NULL, 0);
	}

	/* signature_algorithms. */
	data.size = 0;
	put16(&data, (unsigned)(hello->scheme_count * 2U));
	for (index = 0; index < hello->scheme_count; index++)
		put16(&data, hello->schemes[index]);
	put_extension(&exts, 13, data.bytes, data.size);

	/* supported_versions. */
	if (offer13) {
		data.size = 0;
		put8(&data, offer12 ? 4U : 2U);
		put16(&data, 0x0304U);
		if (offer12)
			put16(&data, 0x0303U);
		put_extension(&exts, 43, data.bytes, data.size);
	}

	/* cookie. */
	if (hello->cookie_size > 0U) {
		data.size = 0;
		put16(&data, (unsigned)hello->cookie_size);
		put_bytes(&data, hello->cookie, hello->cookie_size);
		put_extension(&exts, 44, data.bytes, data.size);
	}

	/* key_share. */
	if (offer13) {
		data.size = 0;
		put16(&data, 0);
		for (index = 0; index < hello->share_count; index++) {
			put16(&data, hello->shares[index].group);
			put16(&data, (unsigned)hello->shares[index].key_size);
			put_bytes(&data, hello->shares[index].key, hello->shares[index].key_size);
		}
		data.bytes[0] = (uint8_t)((data.size - 2U) >> 8);
		data.bytes[1] = (uint8_t)((data.size - 2U) & 0xffU);
		put_extension(&exts, 51, data.bytes, data.size);
	}

	/* The body. */
	put16(&body, 0x0303U);
	put_bytes(&body, hello->random, 32);
	put8(&body, (unsigned)hello->session_id_size);
	put_bytes(&body, hello->session_id, hello->session_id_size);
	put16(&body, (unsigned)((hello->suite_count + (offer12 ? 1U : 0U)) * 2U));
	for (index = 0; index < hello->suite_count; index++)
		put16(&body, hello->suites[index]);
	if (offer12)
		put16(&body, 0x00ffU);
	put8(&body, 1);
	put8(&body, 0);
	put16(&body, (unsigned)exts.size);
	put_bytes(&body, exts.bytes, exts.size);

	/* The message. */
	out->size = 0;
	put8(out, 1);
	put24(out, (unsigned long)body.size);
	put_bytes(out, body.bytes, body.size);
	buffer_free(&body);
	buffer_free(&exts);
	buffer_free(&data);
}

/* Runs the writer into an exact-size block and compares with the model; returns the writer's size or 0. */
static size_t
check_writer(
	const struct sslc_client_hello *hello)
{
	struct buffer model;
	uint8_t *out;
	size_t written;
	int status;

	/* The model's message. */
	buffer_init(&model);
	model_client_hello(hello, &model);

	/* The writer, into a block of exactly the model's size. */
	out = malloc(model.size);
	written = 12345;
	status = sslc_msg_write_client_hello(hello, out, model.size, &written);
	CHECK(status == 0);
	CHECK(written == model.size);
	if (status == 0 && written == model.size)
		CHECK_BYTES(out, model.bytes, model.size);
	free(out);
	status = written == model.size ? (int)model.size : 0;
	buffer_free(&model);
	return (size_t)status;
}

/* A literal TLS 1.3 ClientHello, written out by hand from RFC 8446 4.1.2 and 4.2. */
static const char literal13_hex[] =
	"0100006c" "0303"
	"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
	"00" "0002" "1301" "0100" "0041"
	"000a" "0004" "0002" "001d"
	"000d" "0004" "0002" "0804"
	"002b" "0003" "02" "0304"
	"0033" "0026" "0024" "001d" "0020"
	"a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf";

/* A literal TLS 1.2 ClientHello with a host name, written out by hand from RFC 5246 7.4.1.2, RFC 8422 5.1 and RFC 6066 3. */
static const char literal12_hex[] =
	"01000059" "0303"
	"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
	"00" "0006" "c02b" "c02f" "00ff" "0100" "002a"
	"0000" "0008" "0006" "00" "0003" "612e62"
	"000a" "0006" "0004" "0017" "001d"
	"000b" "0002" "01" "00"
	"0017" "0000"
	"000d" "0006" "0004" "0403" "0401";

/* Checks the writer against the two literal messages. */
static void
test_write_literal(void)
{
	struct hello_input input;
	struct buffer expected;
	uint8_t out[256];
	size_t written;
	int status;

	/* TLS 1.3 only, no name: the literal bytes. */
	buffer_init(&expected);
	input_typical(&input, SSLC_TLS13, SSLC_TLS13);
	input.suites[0] = 0x1301U;
	input.hello.suite_count = 1;
	input.groups[0] = 0x001dU;
	input.hello.group_count = 1;
	input.schemes[0] = 0x0804U;
	input.hello.scheme_count = 1;
	CHECK(put_hex(&expected, literal13_hex) == 0);
	CHECK(expected.size == 112U);
	status = sslc_msg_write_client_hello(&input.hello, out, sizeof(out), &written);
	CHECK(status == 0);
	CHECK(written == expected.size);
	CHECK_BYTES(out, expected.bytes, expected.size);

	/* TLS 1.2 only with a name: the literal bytes. */
	expected.size = 0;
	input_typical(&input, SSLC_TLS12, SSLC_TLS12);
	input.suites[0] = 0xc02bU;
	input.suites[1] = 0xc02fU;
	input.groups[0] = 0x0017U;
	input.groups[1] = 0x001dU;
	input.schemes[0] = 0x0403U;
	input.schemes[1] = 0x0401U;
	input.hello.scheme_count = 2;
	input.hello.server_name = "a.b";
	CHECK(put_hex(&expected, literal12_hex) == 0);
	CHECK(expected.size == 93U);
	status = sslc_msg_write_client_hello(&input.hello, out, sizeof(out), &written);
	CHECK(status == 0);
	CHECK(written == expected.size);
	CHECK_BYTES(out, expected.bytes, expected.size);
	buffer_free(&expected);
}

/* Checks the writer over many combinations of its fields against the model. */
static void
test_write_models(void)
{
	static const uint16_t versions[3][2] = {
		{ SSLC_TLS12, SSLC_TLS12 }, { SSLC_TLS12, SSLC_TLS13 }, { SSLC_TLS13, SSLC_TLS13 }
	};
	static const size_t session_sizes[] = { 0, 1, 31, 32 };
	static const size_t cookie_sizes[] = { 0, 1, 2, 255, 256, 1024 };
	struct hello_input *input;
	struct decoded_hello decoded;
	struct buffer model;
	size_t version;
	size_t name_case;
	size_t session_case;
	size_t cookie_case;
	size_t share_count;
	size_t index;
	size_t written;
	size_t size;
	int slot;

	/* One big input, reused. */
	input = malloc(sizeof(*input));
	buffer_init(&model);

	/* Every combination of versions, name, session id, cookie and share count. */
	for (version = 0; version < 3U; version++) {
		for (name_case = 0; name_case < 5U; name_case++) {
			for (session_case = 0; session_case < 4U; session_case++) {
				for (cookie_case = 0; cookie_case < 6U; cookie_case++) {
					for (share_count = 0; share_count <= SSLC_MSG_MAX_SHARES; share_count++) {
						input_typical(input, versions[version][0], versions[version][1]);
						if (versions[version][1] != SSLC_TLS13) {
							if (cookie_case != 0U || share_count != 0U)
								continue;
						}
						input->hello.share_count = 0;
						for (index = 0; index < share_count; index++) {
							memset(input->shares[index], (int)(0x40U + index), sizeof(input->shares[index]));
							input->hello.shares[index].group = index == 0U ? SSLC_GROUP_X25519 : SSLC_GROUP_SECP256R1;
							input->hello.shares[index].key = input->shares[index];
							input->hello.shares[index].key_size = index == 0U ? 32U : 65U;
						}
						input->hello.share_count = share_count;
						input->hello.session_id_size = session_sizes[session_case];
						memset(input->hello.session_id, 0x5a, sizeof(input->hello.session_id));
						input->hello.cookie_size = cookie_sizes[cookie_case];
						memset(input->cookie, 0xc7, sizeof(input->cookie));
						input->hello.cookie = cookie_sizes[cookie_case] != 0U ? input->cookie : NULL;
						input->hello.server_name = NULL;
						if (name_case == 1U) {
							strcpy(input->name, "a");
							input->hello.server_name = input->name;
						} else if (name_case == 2U) {
							strcpy(input->name, "www.example.com");
							input->hello.server_name = input->name;
						} else if (name_case == 3U) {
							memset(input->name, 'x', CRYC_NAME_HOST_MAX);
							input->name[CRYC_NAME_HOST_MAX] = '\0';
							input->hello.server_name = input->name;
						} else if (name_case == 4U) {
							strcpy(input->name, "1.2.3.4");
							input->hello.server_name = input->name;
						}
						size = check_writer(&input->hello);

						/* The first of each is also taken apart by the file's reader. */
						if (size != 0U && cookie_case == 3U && share_count == 1U) {
							model.size = 0;
							model_client_hello(&input->hello, &model);
							CHECK(decode_hello(model.bytes, model.size, &decoded) == 0);
							slot = find_extension(&decoded, 0);
							CHECK((slot >= 0) == (input->hello.server_name != NULL));
							CHECK(find_extension(&decoded, 10) >= 0);
							CHECK(find_extension(&decoded, 13) >= 0);
							CHECK((find_extension(&decoded, 43) >= 0) == (versions[version][1] == SSLC_TLS13));
							CHECK((find_extension(&decoded, 51) >= 0) == (versions[version][1] == SSLC_TLS13));
							CHECK((find_extension(&decoded, 11) >= 0) == (versions[version][0] == SSLC_TLS12));
							CHECK((find_extension(&decoded, 23) >= 0) == (versions[version][0] == SSLC_TLS12));
							CHECK((find_extension(&decoded, 44) >= 0) == (versions[version][1] == SSLC_TLS13));
							CHECK(find_extension(&decoded, 21) < 0);
							CHECK(decoded.session_size == input->hello.session_id_size);
						}
					}
				}
			}
		}
	}

	/* The longest lists the length fields allow, with and without the signalling suite. */
	input_typical(input, SSLC_TLS12, SSLC_TLS12);
	for (index = 0; index < 64U; index++) {
		input->suites[index] = (uint16_t)(0x1300U + index);
		input->groups[index] = (uint16_t)(0x0100U + index);
		input->schemes[index] = (uint16_t)(0x0800U + index);
	}
	input->hello.suite_count = 64;
	input->hello.group_count = 64;
	input->hello.scheme_count = 64;
	size = check_writer(&input->hello);
	CHECK(size != 0U);
	written = 0;

	/* A ClientHello with as many suites as a 2-byte list holds (32766, plus the signalling suite in 1.2). */
	{
		uint16_t *many;
		uint8_t *out;
		size_t room;
		int status;

		many = malloc(32767U * sizeof(*many));
		for (index = 0; index < 32767U; index++)
			many[index] = (uint16_t)(0x1000U + index);
		input->hello.suites = many;
		input->hello.suite_count = 32766;
		room = 4U + 2U + 32U + 1U + 2U + 32767U * 2U + 2U + 2U + 1000U;
		out = malloc(room);
		status = sslc_msg_write_client_hello(&input->hello, out, room, &written);
		CHECK(status == 0);
		CHECK(written >= 65534U);
		CHECK(get16(out + 4 + 2 + 32 + 1) == 65534U);
		input->hello.suite_count = 32767;
		status = sslc_msg_write_client_hello(&input->hello, out, room, &written);
		CHECK(status == EINVAL);
		CHECK(written == 0U);
		input->hello.min_version = SSLC_TLS13;
		input->hello.max_version = SSLC_TLS13;
		input->hello.suite_count = 32766;
		status = sslc_msg_write_client_hello(&input->hello, out, room, &written);
		CHECK(status == 0);
		CHECK(get16(out + 4 + 2 + 32 + 1) == 65532U);
		free(out);
		free(many);
	}

	/* Long lists of groups and schemes: 30000 fit; 32766 and 32767 do not (the extensions block, 2^16-1 bytes, is full first). */
	{
		uint16_t *many;
		uint8_t *out;
		size_t room;
		int status;

		many = malloc(32767U * sizeof(*many));
		for (index = 0; index < 32767U; index++)
			many[index] = (uint16_t)(0x0100U + (index & 0xffU));
		input_typical(input, SSLC_TLS13, SSLC_TLS13);
		room = 70000U + 2000U;
		out = malloc(room);

		/* Groups. */
		input->hello.groups = many;
		input->hello.group_count = 30000;
		status = sslc_msg_write_client_hello(&input->hello, out, room, &written);
		CHECK(status == 0 && written > 60000U);
		input->hello.group_count = 32766;
		status = sslc_msg_write_client_hello(&input->hello, out, room, &written);
		CHECK(status == EINVAL && written == 0U);
		input->hello.group_count = 32767;
		status = sslc_msg_write_client_hello(&input->hello, out, room, &written);
		CHECK(status == EINVAL && written == 0U);
		input->hello.groups = input->groups;
		input->hello.group_count = 2;

		/* Schemes. */
		input->hello.schemes = many;
		input->hello.scheme_count = 30000;
		status = sslc_msg_write_client_hello(&input->hello, out, room, &written);
		CHECK(status == 0 && written > 60000U);
		input->hello.scheme_count = 32766;
		status = sslc_msg_write_client_hello(&input->hello, out, room, &written);
		CHECK(status == EINVAL && written == 0U);
		input->hello.scheme_count = 32767;
		status = sslc_msg_write_client_hello(&input->hello, out, room, &written);
		CHECK(status == EINVAL && written == 0U);
		free(out);
		free(many);
	}

	/* Key shares of 1 byte and of 60000 bytes; 65529 and above fill the extensions block (2^16-1 bytes) past its end. */
	{
		uint8_t *key;
		uint8_t *out;
		size_t room;
		int status;

		key = calloc(65536U, 1U);
		room = 70000U;
		out = malloc(room);
		input_typical(input, SSLC_TLS13, SSLC_TLS13);
		input->hello.shares[0].key = key;
		input->hello.shares[0].key_size = 1;
		CHECK(check_writer(&input->hello) != 0U);
		input->hello.shares[0].key_size = 60000;
		CHECK(check_writer(&input->hello) != 0U);
		input->hello.shares[0].key_size = 65529;
		status = sslc_msg_write_client_hello(&input->hello, out, room, &written);
		CHECK(status == EINVAL && written == 0U);
		input->hello.shares[0].key_size = 65530;
		status = sslc_msg_write_client_hello(&input->hello, out, room, &written);
		CHECK(status == EINVAL && written == 0U);
		input->hello.shares[0].key_size = 65535;
		status = sslc_msg_write_client_hello(&input->hello, out, room, &written);
		CHECK(status == EINVAL && written == 0U);
		free(out);
		free(key);
	}
	buffer_free(&model);
	free(input);
}

/* Checks every condition the writer refuses, and that nothing is claimed written. */
static void
test_write_errors(void)
{
	struct hello_input *input;
	uint8_t out[1024];
	size_t written;
	int status;

	/* One input, changed one field at a time. */
	input = malloc(sizeof(*input));

	/* The baseline works. */
	input_typical(input, SSLC_TLS12, SSLC_TLS13);
	status = sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written);
	CHECK(status == 0);

	/* Versions outside the two spoken, or in the wrong order. */
	input_typical(input, SSLC_TLS13, SSLC_TLS12);
	written = 99;
	status = sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written);
	CHECK(status == EINVAL);
	CHECK(written == 0U);
	input_typical(input, 0x0302U, SSLC_TLS13);
	status = sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written);
	CHECK(status == EINVAL);
	input_typical(input, SSLC_TLS12, 0x0305U);
	status = sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written);
	CHECK(status == EINVAL);
	input_typical(input, 0, 0);
	status = sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written);
	CHECK(status == EINVAL);

	/* A session id over 32 bytes. */
	input_typical(input, SSLC_TLS12, SSLC_TLS13);
	input->hello.session_id_size = 33;
	status = sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written);
	CHECK(status == EINVAL);
	CHECK(written == 0U);

	/* No suite, no group, no scheme, a list too long for its field, a NULL list. */
	input_typical(input, SSLC_TLS12, SSLC_TLS13);
	input->hello.suite_count = 0;
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);
	input_typical(input, SSLC_TLS12, SSLC_TLS13);
	input->hello.group_count = 0;
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);
	input_typical(input, SSLC_TLS12, SSLC_TLS13);
	input->hello.scheme_count = 0;
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);
	input_typical(input, SSLC_TLS12, SSLC_TLS13);
	input->hello.suites = NULL;
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);
	input_typical(input, SSLC_TLS12, SSLC_TLS13);
	input->hello.groups = NULL;
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);
	input_typical(input, SSLC_TLS12, SSLC_TLS13);
	input->hello.schemes = NULL;
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);
	input_typical(input, SSLC_TLS12, SSLC_TLS13);
	input->hello.suite_count = 32767;
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);
	input->hello.suite_count = (size_t)-1;
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);
	input_typical(input, SSLC_TLS12, SSLC_TLS13);
	input->hello.group_count = 32767;
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);
	input_typical(input, SSLC_TLS12, SSLC_TLS13);
	input->hello.scheme_count = 32767;
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);

	/* A share without TLS 1.3, too many shares, an empty or missing key. */
	input_typical(input, SSLC_TLS12, SSLC_TLS12);
	input->hello.share_count = 1;
	input->hello.shares[0].group = SSLC_GROUP_X25519;
	input->hello.shares[0].key = input->shares[0];
	input->hello.shares[0].key_size = 32;
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);
	input_typical(input, SSLC_TLS13, SSLC_TLS13);
	input->hello.share_count = SSLC_MSG_MAX_SHARES + 1U;
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);
	input_typical(input, SSLC_TLS13, SSLC_TLS13);
	input->hello.shares[0].key_size = 0;
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);
	input_typical(input, SSLC_TLS13, SSLC_TLS13);
	input->hello.shares[0].key = NULL;
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);
	input_typical(input, SSLC_TLS13, SSLC_TLS13);
	input->hello.shares[0].key_size = 65536U;
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);

	/* A cookie without TLS 1.3, over its limit, or with no bytes. */
	input_typical(input, SSLC_TLS12, SSLC_TLS12);
	input->hello.cookie = input->cookie;
	input->hello.cookie_size = 4;
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);
	input_typical(input, SSLC_TLS13, SSLC_TLS13);
	input->hello.cookie = input->cookie;
	input->hello.cookie_size = SSLC_MSG_COOKIE_MAX + 1U;
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);
	input_typical(input, SSLC_TLS13, SSLC_TLS13);
	input->hello.cookie = NULL;
	input->hello.cookie_size = 4;
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);

	/* A host name over CRYC_NAME_HOST_MAX, or empty. */
	input_typical(input, SSLC_TLS12, SSLC_TLS13);
	memset(input->name, 'y', CRYC_NAME_HOST_MAX + 1U);
	input->name[CRYC_NAME_HOST_MAX + 1U] = '\0';
	input->hello.server_name = input->name;
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);
	input->name[0] = '\0';
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);
	memset(input->name, 'y', sizeof(input->name) - 1U);
	input->name[sizeof(input->name) - 1U] = '\0';
	CHECK(sslc_msg_write_client_hello(&input->hello, out, sizeof(out), &written) == EINVAL);

	/* NULL hello. */
	CHECK(sslc_msg_write_client_hello(NULL, out, sizeof(out), &written) == EINVAL);
	CHECK(written == 0U);

	/* An error is reported before room is looked at: a bad hello with no room is EINVAL. */
	input_typical(input, SSLC_TLS13, SSLC_TLS12);
	CHECK(sslc_msg_write_client_hello(&input->hello, out, 0, &written) == EINVAL);
	free(input);
}

/* Checks that every smaller room is ENOBUFS with nothing claimed written, and the exact room works. */
static void
test_write_room(void)
{
	struct hello_input *input;
	struct buffer model;
	uint8_t *out;
	uint8_t canary[64];
	size_t room;
	size_t written;
	size_t size;
	int status;
	int variant;

	/* Three shapes: 1.3 with a name and a cookie, 1.2 with a name, both versions with two shares. */
	input = malloc(sizeof(*input));
	buffer_init(&model);
	for (variant = 0; variant < 3; variant++) {
		if (variant == 0) {
			input_typical(input, SSLC_TLS13, SSLC_TLS13);
			input->hello.cookie = input->cookie;
			input->hello.cookie_size = 20;
			memset(input->cookie, 0x33, 20);
			strcpy(input->name, "example.org");
			input->hello.server_name = input->name;
		} else if (variant == 1) {
			input_typical(input, SSLC_TLS12, SSLC_TLS12);
			strcpy(input->name, "example.org");
			input->hello.server_name = input->name;
		} else {
			input_typical(input, SSLC_TLS12, SSLC_TLS13);
			memset(input->shares[1], 0x77, 65);
			input->hello.shares[1].group = SSLC_GROUP_SECP256R1;
			input->hello.shares[1].key = input->shares[1];
			input->hello.shares[1].key_size = 65;
			input->hello.share_count = 2;
			input->hello.session_id_size = 32;
		}
		model.size = 0;
		model_client_hello(&input->hello, &model);
		size = model.size;

		/* Every room below the size. */
		for (room = 0; room < size; room++) {
			out = exact_copy(model.bytes, room);
			memset(out, 0xee, room);
			written = 4242;
			status = sslc_msg_write_client_hello(&input->hello, out, room, &written);
			CHECK(status == ENOBUFS);
			CHECK(written == 0U);
			free(out);
		}

		/* The exact room works; a bigger room leaves what follows alone. */
		out = malloc(size + sizeof(canary));
		memset(out, 0xee, size + sizeof(canary));
		status = sslc_msg_write_client_hello(&input->hello, out, size + sizeof(canary), &written);
		CHECK(status == 0);
		CHECK(written == size);
		CHECK_BYTES(out, model.bytes, size);
		memset(canary, 0xee, sizeof(canary));
		CHECK_BYTES(out + size, canary, sizeof(canary));
		free(out);
	}
	buffer_free(&model);
	free(input);
}

/*
 * The RFC 8448 ClientHellos against the writer.
 */

/* Checks one RFC 8448 ClientHello: the same fields given to the writer give the same bytes in every extension the writer sends. */
static void
check_rfc_client_hello(
	const char *section,
	size_t nth,
	const unsigned *missing,
	size_t missing_count)
{
	struct hello_input *input;
	struct decoded_hello rfc;
	struct decoded_hello ours;
	uint8_t message[1024];
	uint8_t written_bytes[1024];
	size_t size;
	size_t written;
	size_t position;
	size_t index;
	size_t listed;
	size_t end;
	int slot;
	int status;
	unsigned type;

	/* The RFC's message, taken apart by this file's reader. */
	input = malloc(sizeof(*input));
	size = rfc_value(section, "{client} construct a ClientHello", "ClientHello", nth, message, sizeof(message));
	CHECK(size != (size_t)-1);
	CHECK(decode_hello(message, size, &rfc) == 0);
	input_typical(input, SSLC_TLS13, SSLC_TLS13);
	memcpy(input->hello.random, message + rfc.random, 32);
	input->hello.session_id_size = rfc.session_size;
	memcpy(input->hello.session_id, message + rfc.session, rfc.session_size);

	/* The suites. */
	input->hello.suite_count = rfc.suite_bytes / 2U;
	for (index = 0; index < input->hello.suite_count; index++)
		input->suites[index] = (uint16_t)get16(message + rfc.suites + index * 2U);

	/* The groups. */
	slot = find_extension(&rfc, 10);
	CHECK(slot >= 0);
	input->hello.group_count = (rfc.ext_size[slot] - 2U) / 2U;
	for (index = 0; index < input->hello.group_count; index++)
		input->groups[index] = (uint16_t)get16(message + rfc.ext_data[slot] + 2U + index * 2U);

	/* The schemes. */
	slot = find_extension(&rfc, 13);
	CHECK(slot >= 0);
	input->hello.scheme_count = (rfc.ext_size[slot] - 2U) / 2U;
	for (index = 0; index < input->hello.scheme_count; index++)
		input->schemes[index] = (uint16_t)get16(message + rfc.ext_data[slot] + 2U + index * 2U);

	/* The key shares. */
	slot = find_extension(&rfc, 51);
	CHECK(slot >= 0);
	position = rfc.ext_data[slot] + 2U;
	end = rfc.ext_data[slot] + rfc.ext_size[slot];
	input->hello.share_count = 0;
	while (position < end) {
		index = input->hello.share_count;
		input->hello.shares[index].group = (uint16_t)get16(message + position);
		input->hello.shares[index].key_size = get16(message + position + 2U);
		memcpy(input->shares[index], message + position + 4U, input->hello.shares[index].key_size);
		input->hello.shares[index].key = input->shares[index];
		position += 4U + input->hello.shares[index].key_size;
		input->hello.share_count++;
	}

	/* The name. */
	slot = find_extension(&rfc, 0);
	CHECK(slot >= 0);
	memcpy(input->name, message + rfc.ext_data[slot] + 5U, rfc.ext_size[slot] - 5U);
	input->name[rfc.ext_size[slot] - 5U] = '\0';
	input->hello.server_name = input->name;

	/* The cookie, when the RFC's message has one. */
	slot = find_extension(&rfc, 44);
	if (slot >= 0) {
		input->hello.cookie_size = rfc.ext_size[slot] - 2U;
		memcpy(input->cookie, message + rfc.ext_data[slot] + 2U, input->hello.cookie_size);
		input->hello.cookie = input->cookie;
	}

	/* The writer's message. */
	written = 0;
	status = sslc_msg_write_client_hello(&input->hello, written_bytes, sizeof(written_bytes), &written);
	CHECK(status == 0);
	CHECK(decode_hello(written_bytes, written, &ours) == 0);

	/* Everything before the extensions is the same bytes: random, session id, suites, compression. */
	CHECK(rfc.ext_block == ours.ext_block);
	CHECK_BYTES(written_bytes + 4, message + 4, rfc.ext_block - 4U);

	/* Each extension the writer sends is the RFC's bytes, and it sends it only if the RFC does. */
	listed = 0;
	for (index = 0; index < rfc.ext_count; index++) {
		type = rfc.ext_type[index];
		slot = find_extension(&ours, type);
		if (slot >= 0) {
			CHECK(rfc.ext_size[index] == ours.ext_size[slot]);
			CHECK_BYTES(written_bytes + ours.ext_start[slot], message + rfc.ext_start[index], 4U + rfc.ext_size[index]);
		} else {
			CHECK(listed < missing_count && missing[listed] == type);
			listed++;
		}
	}
	CHECK(listed == missing_count);

	/* Nothing the RFC lacks is sent. */
	for (index = 0; index < ours.ext_count; index++)
		CHECK(find_extension(&rfc, ours.ext_type[index]) >= 0);
	free(input);
}

/* Checks the writer against the five RFC 8448 ClientHellos that carry no pre-shared key. */
static void
test_write_rfc8448(void)
{
	static const unsigned missing3[] = { 0xff01U, 0x0023U, 0x002dU, 0x001cU };
	static const unsigned missing5a[] = { 0xff01U, 0x002dU, 0x001cU };
	static const unsigned missing5b[] = { 0xff01U, 0x002dU, 0x001cU, 0x0015U };

	/* Section 3 (session ticket too), 5 twice (the second has a cookie, a P-256 share and padding), 6 and 7 (a 32-byte session id). */
	check_rfc_client_hello(SEC3, 0, missing3, 4);
	check_rfc_client_hello(SEC5, 0, missing5a, 3);
	check_rfc_client_hello(SEC5, 1, missing5b, 4);
	check_rfc_client_hello(SEC6, 0, missing5a, 3);
	check_rfc_client_hello(SEC7, 0, missing5a, 3);
}

/*
 * The small functions.
 */

/* Checks the message header reader. */
static void
test_header(void)
{
	static const uint8_t example[4] = { 0x01, 0x00, 0x00, 0xc0 };
	static const uint8_t finished[4] = { 0x14, 0x00, 0x00, 0x20 };
	static const uint8_t maximum[4] = { 0xff, 0xff, 0xff, 0xff };
	static const uint8_t ordered[4] = { 0x00, 0x01, 0x02, 0x03 };
	uint8_t bytes[4];
	unsigned type;
	size_t length;
	unsigned position;
	unsigned value;

	/* The first message of RFC 8448: a ClientHello of 192 bytes. */
	sslc_msg_header(example, &type, &length);
	CHECK(type == 1U);
	CHECK(length == 192U);

	/* A Finished of 32 bytes. */
	sslc_msg_header(finished, &type, &length);
	CHECK(type == 20U);
	CHECK(length == 32U);

	/* The largest length. */
	sslc_msg_header(maximum, &type, &length);
	CHECK(type == 255U);
	CHECK(length == 16777215U);

	/* Each length byte in its own place. */
	sslc_msg_header(ordered, &type, &length);
	CHECK(type == 0U);
	CHECK(length == 0x010203U);
	for (position = 1; position < 4U; position++) {
		for (value = 0; value < 256U; value++) {
			memset(bytes, 0, sizeof(bytes));
			bytes[position] = (uint8_t)value;
			sslc_msg_header(bytes, &type, &length);
			CHECK(type == 0U);
			CHECK(length == (size_t)value << (8U * (3U - position)));
		}
	}

	/* The type byte alone. */
	for (value = 0; value < 256U; value++) {
		memset(bytes, 0, sizeof(bytes));
		bytes[0] = (uint8_t)value;
		sslc_msg_header(bytes, &type, &length);
		CHECK(type == value);
		CHECK(length == 0U);
	}
}

/* Checks the writers of Finished, the empty Certificate, ClientKeyExchange and KeyUpdate. */
static void
test_small_writers(void)
{
	static const uint8_t context[3] = { 0xaa, 0xbb, 0xcc };
	uint8_t data[300];
	uint8_t out[400];
	uint8_t *block;
	size_t written;
	size_t index;
	size_t room;
	int status;
	int update;
	unsigned type;
	size_t length;
	int requested;
	unsigned alert;

	/* Finished of 32 bytes: 14 00 00 20 and the data; of 48; of 12 (TLS 1.2); of 0 and 1. */
	for (index = 0; index < sizeof(data); index++)
		data[index] = (uint8_t)(index * 7U + 1U);
	status = sslc_msg_write_finished(data, 32, out, sizeof(out), &written);
	CHECK(status == 0);
	CHECK(written == 36U);
	CHECK(out[0] == 0x14U && out[1] == 0U && out[2] == 0U && out[3] == 0x20U);
	CHECK_BYTES(out + 4, data, 32);
	status = sslc_msg_write_finished(data, 48, out, sizeof(out), &written);
	CHECK(status == 0 && written == 52U && out[3] == 48U);
	CHECK_BYTES(out + 4, data, 48);
	status = sslc_msg_write_finished(data, 12, out, sizeof(out), &written);
	CHECK(status == 0 && written == 16U && out[3] == 12U);
	status = sslc_msg_write_finished(data, 1, out, sizeof(out), &written);
	CHECK(status == 0 && written == 5U && out[3] == 1U && out[4] == data[0]);
	status = sslc_msg_write_finished(NULL, 0, out, sizeof(out), &written);
	CHECK(status == 0 && written == 4U && out[3] == 0U);
	status = sslc_msg_write_finished(data, 0, out, sizeof(out), &written);
	CHECK(status == 0 && written == 4U);

	/* A Finished with a length over 24 bits, or NULL data with a size, is refused. */
	status = sslc_msg_write_finished(data, 16777216U, out, sizeof(out), &written);
	CHECK(status == EINVAL && written == 0U);
	status = sslc_msg_write_finished(NULL, 5, out, sizeof(out), &written);
	CHECK(status == EINVAL && written == 0U);

	/* Every smaller room is ENOBUFS and writes nothing out of its block. */
	for (room = 0; room < 36U; room++) {
		block = exact_copy(out, room);
		written = 9;
		status = sslc_msg_write_finished(data, 32, block, room, &written);
		CHECK(status == ENOBUFS && written == 0U);
		free(block);
	}
	block = exact_copy(out, 36);
	status = sslc_msg_write_finished(data, 32, block, 36, &written);
	CHECK(status == 0 && written == 36U);
	free(block);

	/* The empty Certificate of TLS 1.3 with no context: 0b 00 00 04 00 00 00 00. */
	status = sslc_msg_write_empty_certificate(1, NULL, 0, out, sizeof(out), &written);
	CHECK(status == 0 && written == 8U);
	CHECK_BYTES(out, "\x0b\x00\x00\x04\x00\x00\x00\x00", 8);

	/* With a 3-byte context: 0b 00 00 07 03 aa bb cc 00 00 00. */
	status = sslc_msg_write_empty_certificate(1, context, 3, out, sizeof(out), &written);
	CHECK(status == 0 && written == 11U);
	CHECK_BYTES(out, "\x0b\x00\x00\x07\x03\xaa\xbb\xcc\x00\x00\x00", 11);

	/* The largest context, 255 bytes, and 256 refused. */
	status = sslc_msg_write_empty_certificate(1, data, 255, out, sizeof(out), &written);
	CHECK(status == 0 && written == 4U + 1U + 255U + 3U);
	CHECK(get24(out + 1) == 259UL);
	CHECK(out[4] == 255U);
	status = sslc_msg_write_empty_certificate(1, data, 256, out, sizeof(out), &written);
	CHECK(status == EINVAL && written == 0U);
	status = sslc_msg_write_empty_certificate(1, NULL, 1, out, sizeof(out), &written);
	CHECK(status == EINVAL);

	/* TLS 1.2: an empty list: 0b 00 00 03 00 00 00; the context is ignored. */
	status = sslc_msg_write_empty_certificate(0, NULL, 0, out, sizeof(out), &written);
	CHECK(status == 0 && written == 7U);
	CHECK_BYTES(out, "\x0b\x00\x00\x03\x00\x00\x00", 7);
	status = sslc_msg_write_empty_certificate(0, context, 3, out, sizeof(out), &written);
	CHECK(status == 0 && written == 7U);
	CHECK_BYTES(out, "\x0b\x00\x00\x03\x00\x00\x00", 7);

	/* Every smaller room is ENOBUFS. */
	for (room = 0; room < 11U; room++) {
		block = exact_copy(out, room);
		status = sslc_msg_write_empty_certificate(1, context, 3, block, room, &written);
		CHECK(status == ENOBUFS && written == 0U);
		free(block);
	}
	for (room = 0; room < 7U; room++) {
		block = exact_copy(out, room);
		status = sslc_msg_write_empty_certificate(0, NULL, 0, block, room, &written);
		CHECK(status == ENOBUFS && written == 0U);
		free(block);
	}

	/* The ClientKeyExchange with a 65-byte point: 10 00 00 42 41 and the point. */
	status = sslc_msg_write_client_key_exchange(data, 65, out, sizeof(out), &written);
	CHECK(status == 0 && written == 70U);
	CHECK(out[0] == 0x10U && out[1] == 0U && out[2] == 0U && out[3] == 0x42U && out[4] == 0x41U);
	CHECK_BYTES(out + 5, data, 65);
	status = sslc_msg_write_client_key_exchange(data, 1, out, sizeof(out), &written);
	CHECK(status == 0 && written == 6U && out[3] == 2U && out[4] == 1U && out[5] == data[0]);
	status = sslc_msg_write_client_key_exchange(data, 255, out, sizeof(out), &written);
	CHECK(status == 0 && written == 260U && out[4] == 255U && get24(out + 1) == 256UL);
	status = sslc_msg_write_client_key_exchange(data, 0, out, sizeof(out), &written);
	CHECK(status == EINVAL && written == 0U);
	status = sslc_msg_write_client_key_exchange(data, 256, out, sizeof(out), &written);
	CHECK(status == EINVAL && written == 0U);
	status = sslc_msg_write_client_key_exchange(NULL, 5, out, sizeof(out), &written);
	CHECK(status == EINVAL);
	for (room = 0; room < 70U; room++) {
		block = exact_copy(out, room);
		status = sslc_msg_write_client_key_exchange(data, 65, block, room, &written);
		CHECK(status == ENOBUFS && written == 0U);
		free(block);
	}

	/* KeyUpdate: 18 00 00 01 00 or 01 (RFC 8446 4.6.3), and round trip through the parser. */
	for (update = 0; update < 2; update++) {
		status = sslc_msg_write_key_update(update, out, sizeof(out), &written);
		CHECK(status == 0 && written == 5U);
		CHECK(out[0] == 0x18U && out[1] == 0U && out[2] == 0U && out[3] == 1U && out[4] == (uint8_t)update);
		sslc_msg_header(out, &type, &length);
		CHECK(type == 24U && length == 1U);
		status = sslc_msg_parse_key_update(out + 4, 1, &requested, &alert);
		CHECK(status == 0 && requested == update && alert == 0U);
	}
	status = sslc_msg_write_key_update(2, out, sizeof(out), &written);
	CHECK(status == EINVAL && written == 0U);
	status = sslc_msg_write_key_update(-1, out, sizeof(out), &written);
	CHECK(status == EINVAL && written == 0U);
	status = sslc_msg_write_key_update(256, out, sizeof(out), &written);
	CHECK(status == EINVAL);
	for (room = 0; room < 5U; room++) {
		block = exact_copy(out, room);
		status = sslc_msg_write_key_update(1, block, room, &written);
		CHECK(status == ENOBUFS && written == 0U);
		free(block);
	}
}

/*
 * The parsers: models.
 */

/* One parse result as the models and the code under test give it. */
struct outcome {
	int status;
	unsigned alert;
};

/* Walks an extensions block for its syntax alone; returns 1 when well formed (the model's own walker). */
static int
model_extensions_ok(
	const uint8_t *data,
	size_t size)
{
	size_t position;
	size_t length;

	/* Each: four bytes of header and the data. */
	position = 0;
	while (position < size) {
		if (size - position < 4U)
			return 0;
		length = get16(data + position + 2U);
		if (size - position - 4U < length)
			return 0;
		position += 4U + length;
	}
	return 1;
}

/* The model of the KeyUpdate parser. */
static struct outcome
model_key_update(
	const uint8_t *body,
	size_t size,
	int *requested)
{
	struct outcome outcome;

	/* Exactly one byte; 0 or 1. */
	outcome.status = 0;
	outcome.alert = 0;
	*requested = 0;
	if (size != 1U) {
		outcome.status = EINVAL;
		outcome.alert = DEC;
	} else if (body[0] > 1U) {
		outcome.status = EPROTO;
		outcome.alert = ILL;
	} else {
		*requested = body[0];
	}
	return outcome;
}

/* Runs the KeyUpdate parser and its model on the same bytes and compares. */
static void
compare_key_update(
	const uint8_t *data,
	size_t size)
{
	struct outcome want;
	uint8_t *block;
	unsigned alert;
	int requested;
	int want_requested;
	int status;

	/* The model first, on the original bytes. */
	want = model_key_update(data, size, &want_requested);

	/* The parser, on an exact-size copy, with poisoned outputs. */
	block = exact_copy(data, size);
	alert = 777;
	requested = 555;
	status = sslc_msg_parse_key_update(block, size, &requested, &alert);
	CHECK(status == want.status);
	CHECK(alert == want.alert);
	CHECK(requested == want_requested);
	free(block);
}

/* The model of the CertificateVerify parser: fields in want_*; returns the outcome. */
static struct outcome
model_certificate_verify(
	const uint8_t *body,
	size_t size,
	unsigned *scheme,
	size_t *signature_offset,
	size_t *signature_size)
{
	struct outcome outcome;

	/* Two bytes of scheme, two of length, then exactly that many bytes. */
	outcome.status = 0;
	outcome.alert = 0;
	*scheme = 0;
	*signature_offset = 0;
	*signature_size = 0;
	if (size < 4U || get16(body + 2) != size - 4U) {
		outcome.status = EINVAL;
		outcome.alert = DEC;
		return outcome;
	}
	*scheme = get16(body);
	*signature_offset = 4;
	*signature_size = size - 4U;
	return outcome;
}

/* Runs the CertificateVerify parser and its model on the same bytes and compares. */
static void
compare_certificate_verify(
	const uint8_t *data,
	size_t size)
{
	struct outcome want;
	uint8_t *block;
	const uint8_t *signature;
	size_t signature_size;
	size_t want_offset;
	size_t want_size;
	unsigned want_scheme;
	unsigned alert;
	uint16_t scheme;
	int status;

	/* The model and the parser. */
	want = model_certificate_verify(data, size, &want_scheme, &want_offset, &want_size);
	block = exact_copy(data, size);
	alert = 777;
	scheme = 0x5555U;
	signature = (const uint8_t *)1;
	signature_size = 555;
	status = sslc_msg_parse_certificate_verify(block, size, &scheme, &signature, &signature_size, &alert);
	CHECK(status == want.status);
	CHECK(alert == want.alert);
	if (status == 0 && want.status == 0) {
		CHECK(scheme == want_scheme);
		CHECK(signature_size == want_size);
		CHECK(signature == block + want_offset);
	}
	free(block);
}

/* The fields of a parsed ServerKeyExchange that the model fills. */
struct ske_model {
	unsigned group;
	size_t point;
	size_t point_size;
	unsigned scheme;
	size_t signature;
	size_t signature_size;
	size_t params_size;
};

/* The model of the ServerKeyExchange parser. */
static struct outcome
model_server_key_exchange(
	const uint8_t *body,
	size_t size,
	struct ske_model *fields)
{
	struct outcome outcome;
	size_t point_size;
	size_t position;
	size_t signature_size;

	/* Reads in the order of RFC 8422 5.4. */
	outcome.status = EINVAL;
	outcome.alert = DEC;
	memset(fields, 0, sizeof(*fields));
	if (size < 1U)
		return outcome;
	if (body[0] != 3U) {
		outcome.status = EPROTO;
		outcome.alert = ILL;
		return outcome;
	}
	if (size < 4U)
		return outcome;
	fields->group = get16(body + 1);
	point_size = body[3];
	if (point_size < 1U || size - 4U < point_size)
		return outcome;
	fields->point = 4;
	fields->point_size = point_size;
	position = 4U + point_size;
	fields->params_size = position;
	if (size - position < 4U)
		return outcome;
	fields->scheme = get16(body + position);
	signature_size = get16(body + position + 2U);
	position += 4U;
	if (size - position != signature_size)
		return outcome;
	fields->signature = position;
	fields->signature_size = signature_size;
	outcome.status = 0;
	outcome.alert = 0;
	return outcome;
}

/* Runs the ServerKeyExchange parser and its model on the same bytes and compares. */
static void
compare_server_key_exchange(
	const uint8_t *data,
	size_t size)
{
	struct sslc_server_key_exchange exchange;
	struct ske_model want_fields;
	struct outcome want;
	uint8_t *block;
	unsigned alert;
	int status;

	/* The model and the parser. */
	want = model_server_key_exchange(data, size, &want_fields);
	block = exact_copy(data, size);
	alert = 777;
	memset(&exchange, 0xa5, sizeof(exchange));
	status = sslc_msg_parse_server_key_exchange(block, size, &exchange, &alert);
	CHECK(status == want.status);
	CHECK(alert == want.alert);
	if (status == 0 && want.status == 0) {
		CHECK(exchange.group == want_fields.group);
		CHECK(exchange.point == block + want_fields.point);
		CHECK(exchange.point_size == want_fields.point_size);
		CHECK(exchange.scheme == want_fields.scheme);
		CHECK(exchange.signature == block + want_fields.signature);
		CHECK(exchange.signature_size == want_fields.signature_size);
		CHECK(exchange.params == block);
		CHECK(exchange.params_size == want_fields.params_size);
	}
	free(block);
}

/* The fields of a parsed Certificate that the model fills. */
struct cert_model {
	size_t context;
	size_t context_size;
	size_t count;
	size_t der[CRYC_CHAIN_MAX_PEER];
	size_t der_size[CRYC_CHAIN_MAX_PEER];
};

/* The model of the Certificate parser (both versions). */
static struct outcome
model_certificate(
	const uint8_t *body,
	size_t size,
	int tls13,
	struct cert_model *fields)
{
	struct outcome outcome;
	size_t offset;
	size_t context_size;
	size_t list_size;
	size_t position;
	size_t entry_size;
	size_t ext_size;

	/* The result of a failure to read the syntax. */
	outcome.status = EINVAL;
	outcome.alert = DEC;
	memset(fields, 0, sizeof(*fields));
	offset = 0;

	/* The context of TLS 1.3. */
	if (tls13) {
		if (size < 1U)
			return outcome;
		context_size = body[0];
		if (size - 1U < context_size)
			return outcome;
		fields->context = 1;
		fields->context_size = context_size;
		offset = 1U + context_size;
	}

	/* The list covers the rest of the body exactly. */
	if (size - offset < 3U)
		return outcome;
	list_size = (size_t)get24(body + offset);
	if (size - offset - 3U != list_size)
		return outcome;
	if (list_size == 0U)
		return outcome;

	/* The entries. */
	position = offset + 3U;
	while (position < size) {
		if (fields->count >= CRYC_CHAIN_MAX_PEER) {
			outcome.status = EPROTO;
			outcome.alert = BADC;
			return outcome;
		}
		if (size - position < 3U)
			return outcome;
		entry_size = (size_t)get24(body + position);
		if (entry_size < 1U || size - position - 3U < entry_size)
			return outcome;
		fields->der[fields->count] = position + 3U;
		fields->der_size[fields->count] = entry_size;
		fields->count++;
		position += 3U + entry_size;
		if (tls13) {
			if (size - position < 2U)
				return outcome;
			ext_size = get16(body + position);
			if (size - position - 2U < ext_size)
				return outcome;
			if (!model_extensions_ok(body + position + 2U, ext_size))
				return outcome;
			if (ext_size != 0U) {
				outcome.status = EPROTO;
				outcome.alert = UNS;
				return outcome;
			}
			position += 2U + ext_size;
		}
	}
	outcome.status = 0;
	outcome.alert = 0;
	return outcome;
}

/* Runs the Certificate parser and its model on the same bytes and compares. */
static void
compare_certificate(
	const uint8_t *data,
	size_t size,
	int tls13)
{
	struct sslc_certificate_msg message;
	struct cert_model want_fields;
	struct outcome want;
	uint8_t *block;
	unsigned alert;
	size_t index;
	int status;

	/* The model and the parser. */
	want = model_certificate(data, size, tls13, &want_fields);
	block = exact_copy(data, size);
	alert = 777;
	memset(&message, 0xa5, sizeof(message));
	status = sslc_msg_parse_certificate(block, size, tls13, &message, &alert);
	CHECK(status == want.status);
	CHECK(alert == want.alert);
	CHECK(alert_agrees(status, alert));
	if (status == 0 && want.status == 0) {
		CHECK(message.count == want_fields.count);
		CHECK(message.context_size == want_fields.context_size);
		if (tls13)
			CHECK(message.context == block + want_fields.context);
		else
			CHECK(message.context == NULL);
		for (index = 0; index < message.count && index < want_fields.count; index++) {
			CHECK(message.der[index] == block + want_fields.der[index]);
			CHECK(message.der_size[index] == want_fields.der_size[index]);
		}
	}
	free(block);
}

/* The fields of a parsed EncryptedExtensions that the model fills. */
struct ee_model {
	int ack;
};

/* The model of the EncryptedExtensions parser. */
static struct outcome
model_encrypted_extensions(
	const uint8_t *body,
	size_t size,
	struct ee_model *fields)
{
	struct outcome outcome;
	size_t position;
	size_t length;
	size_t type;
	size_t list;
	int seen_name;
	int seen_groups;

	/* The block covers the body. */
	outcome.status = EINVAL;
	outcome.alert = DEC;
	fields->ack = 0;
	seen_name = 0;
	seen_groups = 0;
	if (size < 2U || get16(body) != size - 2U)
		return outcome;

	/* Each extension. */
	position = 2;
	while (position < size) {
		if (size - position < 4U)
			return outcome;
		type = get16(body + position);
		length = get16(body + position + 2U);
		if (size - position - 4U < length)
			return outcome;
		if (type != 0U && type != 10U) {
			/* RFC 8446 4.2: an extension the client offers is illegal here, one it never offers unsupported. */
			outcome.status = EPROTO;
			outcome.alert = UNS;
			if (type == 11U || type == 13U || type == 23U || type == 43U || type == 44U || type == 51U || type == 0xff01U)
				outcome.alert = ILL;
			return outcome;
		}
		if (type == 0U) {
			if (seen_name || length != 0U)
				return outcome;
			seen_name = 1;
			fields->ack = 1;
		} else {
			if (seen_groups || length < 2U)
				return outcome;
			seen_groups = 1;
			list = get16(body + position + 4U);
			if (list != length - 2U || list < 2U || list % 2U != 0U)
				return outcome;
		}
		position += 4U + length;
	}
	outcome.status = 0;
	outcome.alert = 0;
	return outcome;
}

/* Runs the EncryptedExtensions parser and its model on the same bytes and compares. */
static void
compare_encrypted_extensions(
	const uint8_t *data,
	size_t size)
{
	struct ee_model want_fields;
	struct outcome want;
	uint8_t *block;
	unsigned alert;
	int ack;
	int status;

	/* The model and the parser. */
	want = model_encrypted_extensions(data, size, &want_fields);
	block = exact_copy(data, size);
	alert = 777;
	ack = 555;
	status = sslc_msg_parse_encrypted_extensions(block, size, &ack, &alert);
	CHECK(status == want.status);
	CHECK(alert == want.alert);
	if (status == 0)
		CHECK(ack == want_fields.ack);
	free(block);
}

/*
 * The ServerHello parser: tables.
 */

/* The pieces of a ServerHello body written as hex: the version and a random of 32 bytes. */
#define R8	"1122334455667788"
#define R32	R8 R8 R8 R8

/* The special random of a HelloRetryRequest as RFC 8446 4.1.3 prints it. */
#define HRR32	"cf21ad74e59a6111be1d8c021e65b891c2a211167abb8c5e079e09e2c8a8339c"

/* A ServerHello body up to its extensions: TLS 1.2 version field, random, an empty session id, suite 0x1301, compression 0. */
#define SH_HEAD		"0303" R32 "00" "1301" "00"

/* The same with the HelloRetryRequest random. */
#define HRR_HEAD	"0303" HRR32 "00" "1301" "00"

/* One bad or unusual body and what the parser must say. */
struct hex_case {
	const char *name;
	const char *hex;
	int status;
	unsigned alert;
};

/* Runs one parser over a table of hex bodies, checking the status and the alert, and the invariants. */
static void
run_server_hello_cases(
	const struct hex_case *cases,
	size_t count)
{
	struct sslc_server_hello hello;
	struct buffer body;
	uint8_t *block;
	unsigned alert;
	size_t index;
	int status;

	/* Each case. */
	buffer_init(&body);
	for (index = 0; index < count; index++) {
		body.size = 0;
		if (put_hex(&body, cases[index].hex) != 0) {
			printf("FAIL bad hex in case %s\n", cases[index].name);
			CHECK(0);
			continue;
		}
		block = exact_copy(body.bytes, body.size);
		alert = 777;
		status = sslc_msg_parse_server_hello(block, body.size, &hello, &alert);
		if (status != cases[index].status || alert != cases[index].alert)
			printf("  case %s: status %d alert %u\n", cases[index].name, status, alert);
		CHECK(status == cases[index].status);
		CHECK(alert == cases[index].alert);
		free(block);
	}
	buffer_free(&body);
}

/* Builds a ServerHello body from the head and up to four extension hex pieces; returns the buffer's size. */
static void
make_server_hello(
	struct buffer *body,
	const char *head,
	const char *e1,
	const char *e2,
	const char *e3,
	const char *e4)
{
	const char *pieces[4];
	struct buffer exts;
	size_t index;

	/* The head, then the block of the pieces that exist. */
	pieces[0] = e1;
	pieces[1] = e2;
	pieces[2] = e3;
	pieces[3] = e4;
	buffer_init(&exts);
	for (index = 0; index < 4U; index++) {
		if (pieces[index] != NULL)
			CHECK(put_hex(&exts, pieces[index]) == 0);
	}
	body->size = 0;
	CHECK(put_hex(body, head) == 0);
	put16(body, (unsigned)exts.size);
	put_bytes(body, exts.bytes, exts.size);
	buffer_free(&exts);
}

/* The extension pieces used below, as hex. */
#define E_SV13		"002b" "0002" "0304"
#define E_KS_X25519	"0033" "0024" "001d" "0020" R32
#define E_KS_RETRY	"0033" "0002" "0017"
#define E_COOKIE2	"002c" "0004" "0002" "abcd"
#define E_EMS		"0017" "0000"
#define E_RENEG		"ff01" "0001" "00"
#define E_ECPF		"000b" "0002" "01" "00"
#define E_SNI		"0000" "0000"

/* Checks the fields of good ServerHellos and HelloRetryRequests. */
static void
test_server_hello_good(void)
{
	static const char *const orders[6][3] = {
		{ E_SV13, E_KS_RETRY, E_COOKIE2 }, { E_SV13, E_COOKIE2, E_KS_RETRY },
		{ E_KS_RETRY, E_SV13, E_COOKIE2 }, { E_KS_RETRY, E_COOKIE2, E_SV13 },
		{ E_COOKIE2, E_SV13, E_KS_RETRY }, { E_COOKIE2, E_KS_RETRY, E_SV13 }
	};
	struct sslc_server_hello hello;
	struct buffer body;
	uint8_t *block;
	uint8_t random[32];
	unsigned alert;
	size_t index;
	size_t size;
	int status;
	char hex[8192];

	/* No extensions block: a TLS 1.2 ServerHello with only its fixed fields. */
	buffer_init(&body);
	CHECK(put_hex(&body, SH_HEAD) == 0);
	CHECK(hex_decode(R32, random, sizeof(random)) == 32U);
	block = exact_copy(body.bytes, body.size);
	alert = 777;
	status = sslc_msg_parse_server_hello(block, body.size, &hello, &alert);
	CHECK(status == 0 && alert == 0U);
	CHECK(hello.legacy_version == 0x0303U);
	CHECK_BYTES(hello.random, random, 32);
	CHECK(hello.session_id_size == 0U);
	CHECK(hello.suite == 0x1301U);
	CHECK(hello.compression == 0U);
	CHECK(hello.is_retry == 0);
	CHECK(hello.seen == 0U);
	CHECK(hello.selected_version == 0U);
	CHECK(hello.share_size == 0U && hello.share_key == NULL);
	CHECK(hello.cookie_size == 0U);
	free(block);

	/* An empty extensions block. */
	make_server_hello(&body, SH_HEAD, NULL, NULL, NULL, NULL);
	CHECK(body.size == 2U + 32U + 1U + 2U + 1U + 2U);
	block = exact_copy(body.bytes, body.size);
	status = sslc_msg_parse_server_hello(block, body.size, &hello, &alert);
	CHECK(status == 0 && hello.seen == 0U);
	free(block);

	/* A TLS 1.3 ServerHello with supported_versions and key_share: both orders. */
	make_server_hello(&body, SH_HEAD, E_SV13, E_KS_X25519, NULL, NULL);
	block = exact_copy(body.bytes, body.size);
	status = sslc_msg_parse_server_hello(block, body.size, &hello, &alert);
	CHECK(status == 0 && alert == 0U);
	CHECK(hello.seen == (SSLC_SEEN_SUPPORTED_VERSIONS | SSLC_SEEN_KEY_SHARE));
	CHECK(hello.selected_version == 0x0304U);
	CHECK(hello.share_group == 0x001dU);
	CHECK(hello.share_size == 32U);
	CHECK(hello.share_key == block + body.size - 32U);
	CHECK_BYTES(hello.share_key, random, 8);
	CHECK(hello.is_retry == 0);
	free(block);
	make_server_hello(&body, SH_HEAD, E_KS_X25519, E_SV13, NULL, NULL);
	block = exact_copy(body.bytes, body.size);
	status = sslc_msg_parse_server_hello(block, body.size, &hello, &alert);
	CHECK(status == 0 && hello.seen == (SSLC_SEEN_SUPPORTED_VERSIONS | SSLC_SEEN_KEY_SHARE));
	CHECK(hello.selected_version == 0x0304U && hello.share_group == 0x001dU && hello.share_size == 32U);
	free(block);

	/* A HelloRetryRequest in all six orders of its three extensions. */
	for (index = 0; index < 6U; index++) {
		make_server_hello(&body, HRR_HEAD, orders[index][0], orders[index][1], orders[index][2], NULL);
		block = exact_copy(body.bytes, body.size);
		alert = 777;
		status = sslc_msg_parse_server_hello(block, body.size, &hello, &alert);
		CHECK(status == 0 && alert == 0U);
		CHECK(hello.is_retry == 1);
		CHECK(hello.seen == (SSLC_SEEN_SUPPORTED_VERSIONS | SSLC_SEEN_KEY_SHARE | SSLC_SEEN_COOKIE));
		CHECK(hello.selected_version == 0x0304U);
		CHECK(hello.share_group == 0x0017U);
		CHECK(hello.share_key == NULL && hello.share_size == 0U);
		CHECK(hello.cookie_size == 2U);
		CHECK(hello.cookie != NULL && hello.cookie[0] == 0xabU && hello.cookie[1] == 0xcdU);
		free(block);
	}

	/* A TLS 1.2 ServerHello with every extension that is allowed. */
	make_server_hello(&body, "0303" R32 "00" "c02f" "00", E_EMS, E_RENEG, E_ECPF, E_SNI);
	block = exact_copy(body.bytes, body.size);
	status = sslc_msg_parse_server_hello(block, body.size, &hello, &alert);
	CHECK(status == 0 && alert == 0U);
	CHECK(hello.suite == 0xc02fU);
	CHECK(hello.seen == (SSLC_SEEN_EXTENDED_MASTER | SSLC_SEEN_RENEGOTIATION_INFO | SSLC_SEEN_EC_POINT_FORMATS | SSLC_SEEN_SERVER_NAME));
	CHECK(hello.renegotiation_info_size == 0U && hello.renegotiation_info != NULL);
	CHECK(hello.is_retry == 0);
	free(block);

	/* The session id echoed, every length from 0 to 32. */
	for (index = 0; index <= 32U; index++) {
		struct buffer built;

		buffer_init(&built);
		put16(&built, 0x0303U);
		put_fill(&built, 0x11, 32);
		put8(&built, (unsigned)index);
		put_fill(&built, 0xd0U + (unsigned)index, index);
		put16(&built, 0x1301U);
		put8(&built, 0);
		block = exact_copy(built.bytes, built.size);
		status = sslc_msg_parse_server_hello(block, built.size, &hello, &alert);
		CHECK(status == 0);
		CHECK(hello.session_id_size == index);
		if (index > 0U) {
			CHECK(hello.session_id == block + 35U);
			CHECK(hello.session_id[0] == (uint8_t)(0xd0U + index));
		}
		free(block);
		buffer_free(&built);
	}

	/* A cookie of 1 and of the limit, 1024 bytes. */
	size = 1;
	for (index = 0; index < 2U; index++) {
		struct buffer built;

		buffer_init(&built);
		put16(&built, 0x0303U);
		put_bytes(&built, (const uint8_t *)"\xcf\x21\xad\x74\xe5\x9a\x61\x11\xbe\x1d\x8c\x02\x1e\x65\xb8\x91\xc2\xa2\x11\x16\x7a\xbb\x8c\x5e\x07\x9e\x09\xe2\xc8\xa8\x33\x9c", 32);
		put8(&built, 0);
		put16(&built, 0x1301U);
		put8(&built, 0);
		put16(&built, (unsigned)(4U + 2U + size));
		put16(&built, 44);
		put16(&built, (unsigned)(2U + size));
		put16(&built, (unsigned)size);
		put_fill(&built, 0x99, size);
		block = exact_copy(built.bytes, built.size);
		status = sslc_msg_parse_server_hello(block, built.size, &hello, &alert);
		CHECK(status == 0 && hello.is_retry == 1);
		CHECK(hello.cookie_size == size);
		CHECK(hello.cookie == block + built.size - size);
		free(block);
		buffer_free(&built);
		size = SSLC_MSG_COOKIE_MAX;
	}

	/* A cookie of 1025 bytes is illegal_parameter. */
	snprintf(hex, sizeof(hex), HRR_HEAD "%04x" "002c" "%04x" "%04x", 4U + 2U + 1025U, 2U + 1025U, 1025U);
	size = strlen(hex);
	for (index = 0; index < 1025U; index++) {
		hex[size++] = '9';
		hex[size++] = '9';
	}
	hex[size] = '\0';
	body.size = 0;
	CHECK(put_hex(&body, hex) == 0);
	block = exact_copy(body.bytes, body.size);
	status = sslc_msg_parse_server_hello(block, body.size, &hello, &alert);
	CHECK(status == EPROTO && alert == ILL);
	free(block);

	/* A random that differs from the HelloRetryRequest's in any one byte is an ordinary ServerHello. */
	for (index = 0; index < 32U; index++) {
		struct buffer built;

		buffer_init(&built);
		put16(&built, 0x0303U);
		put_bytes(&built, (const uint8_t *)"\xcf\x21\xad\x74\xe5\x9a\x61\x11\xbe\x1d\x8c\x02\x1e\x65\xb8\x91\xc2\xa2\x11\x16\x7a\xbb\x8c\x5e\x07\x9e\x09\xe2\xc8\xa8\x33\x9c", 32);
		built.bytes[2U + index] ^= 0x01U;
		put8(&built, 0);
		put16(&built, 0x1301U);
		put8(&built, 0);
		block = exact_copy(built.bytes, built.size);
		status = sslc_msg_parse_server_hello(block, built.size, &hello, &alert);
		CHECK(status == 0 && hello.is_retry == 0);
		free(block);
		buffer_free(&built);
	}

	/* ec_point_formats: lists (a length byte, then formats) that contain uncompressed are good, those without are illegal_parameter. */
	{
		static const char *const lists[] = { "0100", "020001", "020100", "03010200", "03000102", "0300ff00" };
		static const char *const bad_lists[] = { "0101", "020102", "02ffee", "03010203" };

		for (index = 0; index < sizeof(lists) / sizeof(lists[0]); index++) {
			snprintf(hex, sizeof(hex), "000b" "%04x" "%s", (unsigned)(strlen(lists[index]) / 2U), lists[index]);
			make_server_hello(&body, SH_HEAD, hex, NULL, NULL, NULL);
			block = exact_copy(body.bytes, body.size);
			alert = 777;
			status = sslc_msg_parse_server_hello(block, body.size, &hello, &alert);
			CHECK(status == 0 && alert == 0U);
			CHECK(hello.seen == SSLC_SEEN_EC_POINT_FORMATS);
			free(block);
		}
		for (index = 0; index < sizeof(bad_lists) / sizeof(bad_lists[0]); index++) {
			snprintf(hex, sizeof(hex), "000b" "%04x" "%s", (unsigned)(strlen(bad_lists[index]) / 2U), bad_lists[index]);
			make_server_hello(&body, SH_HEAD, hex, NULL, NULL, NULL);
			block = exact_copy(body.bytes, body.size);
			alert = 777;
			status = sslc_msg_parse_server_hello(block, body.size, &hello, &alert);
			CHECK(status == EPROTO && alert == ILL);
			free(block);
		}
	}

	buffer_free(&body);
}

/* The table of bad ServerHello bodies. */
static void
test_server_hello_bad(void)
{
	static const struct hex_case cases[] = {
		/* Cut inside the fixed fields. */
		{ "empty", "", EINVAL, DEC },
		{ "one byte", "03", EINVAL, DEC },
		{ "version only", "0303", EINVAL, DEC },
		{ "short random", "0303" R8 R8 R8 "11223344556677", EINVAL, DEC },
		{ "random only", "0303" R32, EINVAL, DEC },
		{ "session length missing", "0303" R32, EINVAL, DEC },
		{ "session over 32", "0303" R32 "21" R32 "11" "1301" "00", EINVAL, DEC },
		{ "session 255", "0303" R32 "ff" "1301" "00", EINVAL, DEC },
		{ "session longer than body", "0303" R32 "20" R8, EINVAL, DEC },
		{ "no suite", "0303" R32 "00", EINVAL, DEC },
		{ "half a suite", "0303" R32 "00" "13", EINVAL, DEC },
		{ "no compression", "0303" R32 "00" "1301", EINVAL, DEC },

		/* The extensions block. */
		{ "block length half", SH_HEAD "00", EINVAL, DEC },
		{ "block longer than body", SH_HEAD "0004" "002b", EINVAL, DEC },
		{ "block longer by one", SH_HEAD "0007" E_SV13, EINVAL, DEC },
		{ "bytes after the block", SH_HEAD "0006" E_SV13 "ff", EINVAL, DEC },
		{ "block shorter than its extension", SH_HEAD "0005" E_SV13, EINVAL, DEC },
		{ "extension type cut", SH_HEAD "0001" "00", EINVAL, DEC },
		{ "extension length cut", SH_HEAD "0003" "002b" "00", EINVAL, DEC },
		{ "extension data cut", SH_HEAD "0005" "002b" "0002" "03", EINVAL, DEC },

		/* A repeated extension, for each that is allowed. */
		{ "twice supported_versions", SH_HEAD "000c" E_SV13 E_SV13, EINVAL, DEC },
		{ "twice key_share", SH_HEAD "0050" E_KS_X25519 E_KS_X25519, EINVAL, DEC },
		{ "twice extended_master_secret", SH_HEAD "0008" E_EMS E_EMS, EINVAL, DEC },
		{ "twice renegotiation_info", SH_HEAD "000a" E_RENEG E_RENEG, EINVAL, DEC },
		{ "twice ec_point_formats", SH_HEAD "000c" E_ECPF E_ECPF, EINVAL, DEC },
		{ "twice server_name", SH_HEAD "0008" E_SNI E_SNI, EINVAL, DEC },
		{ "twice cookie", HRR_HEAD "0010" E_COOKIE2 E_COOKIE2, EINVAL, DEC },

		/* supported_versions. */
		{ "supported_versions empty", SH_HEAD "0004" "002b0000", EINVAL, DEC },
		{ "supported_versions one byte", SH_HEAD "0005" "002b000103", EINVAL, DEC },
		{ "supported_versions three bytes", SH_HEAD "0007" "002b0003" "030400", EINVAL, DEC },
		{ "supported_versions list form", SH_HEAD "0007" "002b0003" "020304", EINVAL, DEC },

		/* key_share of a ServerHello. */
		{ "key_share empty", SH_HEAD "0004" "00330000", EINVAL, DEC },
		{ "key_share group only", SH_HEAD "0006" "00330002" "001d", EINVAL, DEC },
		{ "key_share key length cut", SH_HEAD "0007" "00330003" "001d00", EINVAL, DEC },
		{ "key_share key empty", SH_HEAD "0008" "00330004" "001d" "0000", EINVAL, DEC },
		{ "key_share key longer than data", SH_HEAD "000a" "00330006" "001d" "0020" "1122", EINVAL, DEC },
		{ "key_share bytes after key", SH_HEAD "000b" "00330007" "001d" "0001" "11" "22", EINVAL, DEC },

		/* key_share of a HelloRetryRequest is the group alone. */
		{ "retry key_share empty", HRR_HEAD "0004" "00330000", EINVAL, DEC },
		{ "retry key_share one byte", HRR_HEAD "0005" "00330001" "00", EINVAL, DEC },
		{ "retry key_share with a key", HRR_HEAD "0009" "00330005" "0017" "0001" "11", EINVAL, DEC },
		{ "retry key_share three bytes", HRR_HEAD "0007" "00330003" "001700", EINVAL, DEC },

		/* cookie. */
		{ "cookie empty data", HRR_HEAD "0004" "002c0000", EINVAL, DEC },
		{ "cookie length only", HRR_HEAD "0006" "002c0002" "0000", EINVAL, DEC },
		{ "cookie length zero", HRR_HEAD "0008" "002c0004" "0000" "1122", EINVAL, DEC },
		{ "cookie truncated", HRR_HEAD "0008" "002c0004" "0003" "1122", EINVAL, DEC },
		{ "cookie bytes after", HRR_HEAD "0009" "002c0005" "0001" "11" "2233", EINVAL, DEC },

		/* The empty extensions. */
		{ "extended_master_secret with data", SH_HEAD "0005" "00170001" "00", EINVAL, DEC },
		{ "server_name with data", SH_HEAD "0005" "00000001" "00", EINVAL, DEC },

		/* renegotiation_info. */
		{ "renegotiation_info empty data", SH_HEAD "0004" "ff010000", EINVAL, DEC },
		{ "renegotiation_info length too big", SH_HEAD "0006" "ff010002" "0500", EINVAL, DEC },
		{ "renegotiation_info bytes after", SH_HEAD "0007" "ff010003" "00" "0000", EINVAL, DEC },
		{ "renegotiation_info length short", SH_HEAD "0007" "ff010003" "01" "0000", EINVAL, DEC },

		/* ec_point_formats. */
		{ "ec_point_formats empty data", SH_HEAD "0004" "000b0000", EINVAL, DEC },
		{ "ec_point_formats empty list", SH_HEAD "0005" "000b0001" "00", EINVAL, DEC },
		{ "ec_point_formats list too long", SH_HEAD "0007" "000b0003" "05" "0000", EINVAL, DEC },
		{ "ec_point_formats bytes after", SH_HEAD "0008" "000b0004" "01" "00" "00" "00", EINVAL, DEC },
		{ "ec_point_formats no uncompressed", SH_HEAD "0006" "000b0002" "01" "01", EPROTO, ILL },
		{ "ec_point_formats two without", SH_HEAD "0007" "000b0003" "02" "0102", EPROTO, ILL },

		/* Extensions the client did not offer, or that have no place here. */
		{ "supported_groups", SH_HEAD "0008" "000a0004" "0002" "001d", EPROTO, ILL },
		{ "signature_algorithms", SH_HEAD "0008" "000d0004" "0002" "0804", EPROTO, ILL },
		{ "alpn", SH_HEAD "0008" "00100004" "0002" "0100", EPROTO, UNS },
		{ "session_ticket", SH_HEAD "0004" "00230000", EPROTO, UNS },
		{ "pre_shared_key", SH_HEAD "0006" "00290002" "0000", EPROTO, UNS },
		{ "early_data", SH_HEAD "0004" "002a0000", EPROTO, UNS },
		{ "psk_key_exchange_modes", SH_HEAD "0006" "002d0002" "0101", EPROTO, UNS },
		{ "record_size_limit", SH_HEAD "0006" "001c0002" "4001", EPROTO, UNS },
		{ "padding", SH_HEAD "0004" "00150000", EPROTO, UNS },
		{ "unknown", SH_HEAD "0004" "99990000", EPROTO, UNS },
		{ "grease", SH_HEAD "0004" "0a0a0000", EPROTO, UNS },
		{ "cookie in a ServerHello (recorded; the handshake refuses it)", SH_HEAD "0008" E_COOKIE2, 0, 0 },

		/* The order of the findings: the first extension decides. */
		{ "unsupported before bad syntax", SH_HEAD "0009" "00290000" "002b0001" "03", EPROTO, UNS },
		{ "bad syntax before unsupported", SH_HEAD "0009" "002b0001" "03" "00290000", EINVAL, DEC },
		{ "good then unsupported", SH_HEAD "000a" E_SV13 "00290000", EPROTO, UNS }
	};

	/* The table. */
	run_server_hello_cases(cases, sizeof(cases) / sizeof(cases[0]));
}

/* A good ServerHello cut at every length. */
static void
test_server_hello_prefixes(void)
{
	struct sslc_server_hello hello;
	struct buffer body;
	uint8_t *block;
	unsigned alert;
	size_t fixed;
	size_t cut;
	int status;
	int variant;

	/* A TLS 1.3 ServerHello with a session id of 32, and a HelloRetryRequest. */
	buffer_init(&body);
	for (variant = 0; variant < 3; variant++) {
		if (variant == 0) {
			make_server_hello(&body, "0303" R32 "20" R32 "1301" "00", E_SV13, E_KS_X25519, NULL, NULL);
			fixed = 2U + 32U + 1U + 32U + 2U + 1U;
		} else if (variant == 1) {
			make_server_hello(&body, HRR_HEAD, E_KS_RETRY, E_COOKIE2, E_SV13, NULL);
			fixed = 2U + 32U + 1U + 2U + 1U;
		} else {
			make_server_hello(&body, "0303" R32 "00" "c02f" "00", E_EMS, E_RENEG, E_ECPF, E_SNI);
			fixed = 2U + 32U + 1U + 2U + 1U;
		}

		/* Only the cut right after the fixed fields (no extensions block) is a ServerHello. */
		for (cut = 0; cut < body.size; cut++) {
			block = exact_copy(body.bytes, cut);
			alert = 777;
			status = sslc_msg_parse_server_hello(block, cut, &hello, &alert);
			if (cut == fixed) {
				CHECK(status == 0 && alert == 0U);
			} else {
				CHECK(status == EINVAL && alert == DEC);
			}
			free(block);
		}
		block = exact_copy(body.bytes, body.size);
		status = sslc_msg_parse_server_hello(block, body.size, &hello, &alert);
		CHECK(status == 0);
		free(block);
	}
	buffer_free(&body);
}

/*
 * The RFC 8448 messages against the parsers.
 */

/* Loads an RFC 8448 handshake message and checks its header against its length; returns the size. */
static size_t
rfc_message(
	const char *section,
	const char *step,
	const char *label,
	size_t nth,
	unsigned type,
	uint8_t *out,
	size_t room)
{
	unsigned header_type;
	size_t length;
	size_t size;

	/* The bytes, then the 4-byte header. */
	size = rfc_value(section, step, label, nth, out, room);
	if (size == (size_t)-1 || size < 4U) {
		CHECK(0);
		return 0;
	}
	sslc_msg_header(out, &header_type, &length);
	CHECK(header_type == type);
	CHECK(length == size - 4U);
	return size;
}

/* Parses the ServerHello (or HelloRetryRequest) of an RFC 8448 section and checks its fields. */
static void
check_rfc_server_hello(
	const char *section,
	size_t nth,
	const char *random_hex,
	const char *session_hex,
	int retry,
	unsigned group,
	size_t key_size,
	const char *key_label,
	const char *key_step)
{
	struct sslc_server_hello hello;
	uint8_t message[256];
	uint8_t expected[256];
	uint8_t *block;
	size_t size;
	size_t expected_size;
	unsigned alert;
	int status;

	/* The message and its parse. */
	size = rfc_message(section, "{server} construct a ServerHello", "ServerHello", nth, SSLC_HS_SERVER_HELLO, message, sizeof(message));
	block = exact_copy(message + 4, size - 4U);
	alert = 777;
	status = sslc_msg_parse_server_hello(block, size - 4U, &hello, &alert);
	CHECK(status == 0);
	CHECK(alert == 0U);
	CHECK(hello.legacy_version == 0x0303U);
	CHECK(hello.suite == 0x1301U);
	CHECK(hello.compression == 0U);
	CHECK(hello.is_retry == retry);
	CHECK(hello.selected_version == 0x0304U);
	CHECK(hello.share_group == group);

	/* The random and the session id, as the RFC's dump has them. */
	CHECK(hex_decode(random_hex, expected, sizeof(expected)) == 32U);
	CHECK_BYTES(hello.random, expected, 32);
	expected_size = hex_decode(session_hex, expected, sizeof(expected));
	CHECK(hello.session_id_size == expected_size);
	if (expected_size > 0U)
		CHECK_BYTES(hello.session_id, expected, expected_size);

	/* The server's key equals the key the RFC prints for the key pair it made (a HelloRetryRequest has none). */
	if (key_size != 0U) {
		CHECK(hello.share_size == key_size);
		expected_size = rfc_value(section, key_step, key_label, 0, expected, sizeof(expected));
		CHECK(expected_size == key_size);
		if (hello.share_size == key_size && expected_size == key_size)
			CHECK_BYTES(hello.share_key, expected, key_size);
	} else {
		CHECK(hello.share_size == 0U);
	}
	free(block);
}

/* Parses the ServerHellos of RFC 8448. */
static void
test_rfc_server_hello(void)
{
	struct sslc_server_hello hello;
	uint8_t message[256];
	uint8_t *block;
	size_t size;
	unsigned alert;
	int status;

	/* Section 3: x25519, no session id. */
	check_rfc_server_hello(SEC3, 0, "a6af06a4121860dc5e6e60249cd34c95930c8ac5cb1434dac155772ed3e26928", "", 0, 0x001dU, 32,
	    "public key", "{server} create an ephemeral x25519 key pair");

	/* Section 5: the HelloRetryRequest (cookie, group secp256r1) and the ServerHello that follows (a P-256 key of 65 bytes). */
	check_rfc_server_hello(SEC5, 0, "cf21ad74e59a6111be1d8c021e65b891c2a211167abb8c5e079e09e2c8a8339c", "", 1, 0x0017U, 0,
	    "", "");
	check_rfc_server_hello(SEC5, 1, "bb341d847fd789c47c387172dc0c9bf147fccacb5043d86ca4c598d3ff571b98", "", 0, 0x0017U, 65,
	    "public key", "{server} create an ephemeral P-256 key pair");

	/* Section 6: another x25519 exchange. */
	check_rfc_server_hello(SEC6, 0, "3b50fdf1c3d572e40e68953e7fff4e2758459c59afa0582c0ea032874255fe6e", "", 0, 0x001dU, 32,
	    "public key", "{server} create an ephemeral x25519 key pair");

	/* Section 7: the compatibility mode echoes the client's 32-byte session id. */
	check_rfc_server_hello(SEC7, 0, "e5dd5948c435f7a38f0f0130708dc322d9df09abd4838117c183a7bb6d994f2c",
	    "a80c165581a8e0d06c0018d54d3a06dd32cfd4051eb026fad3fd0ba99269e6ef", 0, 0x001dU, 32,
	    "public key", "{server} create an ephemeral x25519 key pair");

	/* The HelloRetryRequest's cookie is the 116 bytes of the RFC, starting 00 72 71 dc (a 2-byte length 114 then 114 bytes). */
	size = rfc_message(SEC5, "{server} construct a ServerHello", "ServerHello", 0, SSLC_HS_SERVER_HELLO, message, sizeof(message));
	block = exact_copy(message + 4, size - 4U);
	status = sslc_msg_parse_server_hello(block, size - 4U, &hello, &alert);
	CHECK(status == 0);
	CHECK(hello.cookie_size == 114U);
	CHECK(hello.cookie != NULL && hello.cookie[0] == 0x71U && hello.cookie[1] == 0xdcU);
	CHECK(hello.seen == (SSLC_SEEN_KEY_SHARE | SSLC_SEEN_COOKIE | SSLC_SEEN_SUPPORTED_VERSIONS));
	free(block);

	/* Section 4's ServerHello carries a pre_shared_key, which this client never offered. */
	size = rfc_message(SEC4, "{server} construct a ServerHello", "ServerHello", 0, SSLC_HS_SERVER_HELLO, message, sizeof(message));
	block = exact_copy(message + 4, size - 4U);
	alert = 777;
	status = sslc_msg_parse_server_hello(block, size - 4U, &hello, &alert);
	CHECK(status == EPROTO && alert == UNS);
	free(block);
}

/* The extensions of an RFC 8448 EncryptedExtensions without one type, as a new body (the model walker's view of the bytes). */
static size_t
without_extension(
	const uint8_t *body,
	size_t size,
	unsigned type,
	uint8_t *out)
{
	size_t position;
	size_t length;
	size_t used;

	/* Everything except the extensions of the type; the block's length is rewritten. */
	used = 2;
	position = 2;
	while (position + 4U <= size) {
		length = get16(body + position + 2U);
		if (get16(body + position) != type) {
			memcpy(out + used, body + position, 4U + length);
			used += 4U + length;
		}
		position += 4U + length;
	}
	out[0] = (uint8_t)((used - 2U) >> 8);
	out[1] = (uint8_t)((used - 2U) & 0xffU);
	return used;
}

/* Parses the EncryptedExtensions, Certificate, CertificateVerify, Finished and the others of RFC 8448. */
static void
test_rfc_messages(void)
{
	static const char *const sections[5] = { SEC3, SEC5, SEC6, SEC6, SEC7 };
	static const char *const sides[5] = { "{server}", "{server}", "{server}", "{client}", "{server}" };
	static const size_t cert_sizes[5] = { 432U, 432U, 306U, 438U, 432U };
	static const unsigned schemes[5] = { 0x0804U, 0x0804U, 0x0403U, 0x0804U, 0x0804U };
	static const size_t sig_sizes[5] = { 128U, 128U, 71U, 128U, 128U };
	uint8_t message[1024];
	uint8_t cut[1024];
	uint8_t finished[64];
	uint8_t written_bytes[64];
	struct sslc_certificate_msg certificate;
	uint8_t *block;
	const uint8_t *signature;
	size_t signature_size;
	size_t size;
	size_t index;
	size_t cut_size;
	size_t written;
	size_t room;
	uint16_t scheme;
	unsigned alert;
	unsigned type;
	size_t length;
	int ack;
	int status;
	char step[96];

	/* The EncryptedExtensions of five sections: each has record_size_limit, which this client never offers. */
	for (index = 0; index < 5U; index++) {
		if (index == 3U)
			continue;
		size = rfc_message(sections[index], "{server} construct an EncryptedExtensions", "EncryptedExtensions", 0, SSLC_HS_ENCRYPTED_EXTENSIONS, message, sizeof(message));
		block = exact_copy(message + 4, size - 4U);
		ack = 555;
		alert = 777;
		status = sslc_msg_parse_encrypted_extensions(block, size - 4U, &ack, &alert);
		CHECK(status == EPROTO && alert == UNS);
		free(block);

		/* Without record_size_limit the server_name acknowledgement and the group list parse. */
		cut_size = without_extension(message + 4, size - 4U, 0x001cU, cut);
		block = exact_copy(cut, cut_size);
		status = sslc_msg_parse_encrypted_extensions(block, cut_size, &ack, &alert);
		CHECK(status == 0 && alert == 0U && ack == 1);
		free(block);

		/* Without server_name too: no acknowledgement. */
		size = cut_size;
		memcpy(message + 4, cut, size);
		cut_size = without_extension(message + 4, size, 0x0000U, cut);
		block = exact_copy(cut, cut_size);
		status = sslc_msg_parse_encrypted_extensions(block, cut_size, &ack, &alert);
		CHECK(status == 0 && ack == 0);
		free(block);

		/* Without the group list: an empty block. */
		cut_size = without_extension(cut, cut_size, 0x000aU, message);
		block = exact_copy(message, cut_size);
		status = sslc_msg_parse_encrypted_extensions(block, cut_size, &ack, &alert);
		CHECK(status == 0 && ack == 0 && cut_size == 2U);
		free(block);
	}

	/* Section 4's EncryptedExtensions also has early_data, which was not offered either. */
	size = rfc_message(SEC4, "{server} construct an EncryptedExtensions", "EncryptedExtensions", 0, SSLC_HS_ENCRYPTED_EXTENSIONS, message, sizeof(message));
	cut_size = without_extension(message + 4, size - 4U, 0x001cU, cut);
	block = exact_copy(cut, cut_size);
	status = sslc_msg_parse_encrypted_extensions(block, cut_size, &ack, &alert);
	CHECK(status == EPROTO && alert == UNS);
	free(block);

	/* The Certificate messages: one certificate each, whose DER begins with a SEQUENCE of the length the DER itself gives. */
	for (index = 0; index < 5U; index++) {
		snprintf(step, sizeof(step), "%s construct a Certificate", sides[index]);
		size = rfc_message(sections[index], step, "Certificate", 0, SSLC_HS_CERTIFICATE, message, sizeof(message));
		block = exact_copy(message + 4, size - 4U);
		alert = 777;
		status = sslc_msg_parse_certificate(block, size - 4U, 1, &certificate, &alert);
		CHECK(status == 0 && alert == 0U);
		CHECK(certificate.count == 1U);
		CHECK(certificate.context_size == 0U);
		CHECK(certificate.der_size[0] == cert_sizes[index]);
		if (status == 0) {
			CHECK(certificate.der[0] == block + 1U + 3U + 3U);
			CHECK(certificate.der[0][0] == 0x30U);
			CHECK(certificate.der[0][1] == 0x82U);
			CHECK(get16(certificate.der[0] + 2) + 4U == certificate.der_size[0]);
		}
		compare_certificate(message + 4, size - 4U, 1);
		free(block);

		/* Every shorter body is refused and agrees with the model. */
		for (cut_size = 0; cut_size < size - 4U; cut_size += (cut_size < 16U ? 1U : 13U))
			compare_certificate(message + 4, cut_size, 1);
	}

	/* The CertificateVerify messages. */
	for (index = 0; index < 5U; index++) {
		snprintf(step, sizeof(step), "%s construct a CertificateVerify", sides[index]);
		size = rfc_message(sections[index], step, "CertificateVerify", 0, SSLC_HS_CERTIFICATE_VERIFY, message, sizeof(message));
		block = exact_copy(message + 4, size - 4U);
		alert = 777;
		status = sslc_msg_parse_certificate_verify(block, size - 4U, &scheme, &signature, &signature_size, &alert);
		CHECK(status == 0 && alert == 0U);
		CHECK(scheme == schemes[index]);
		CHECK(signature_size == sig_sizes[index]);
		CHECK(signature == block + 4U);
		free(block);
		compare_certificate_verify(message + 4, size - 4U);
		for (cut_size = 0; cut_size < size - 4U; cut_size++)
			compare_certificate_verify(message + 4, cut_size);
	}

	/* The Finished messages: the writer rebuilds each from its verify_data, byte for byte. */
	for (index = 0; index < 5U; index++) {
		snprintf(step, sizeof(step), "%s construct a Finished", sides[index]);
		size = rfc_message(sections[index], step, "Finished", 0, SSLC_HS_FINISHED, message, sizeof(message));
		CHECK(size == 36U);
		memcpy(finished, message + 4, 32);
		written = 0;
		status = sslc_msg_write_finished(finished, 32, written_bytes, sizeof(written_bytes), &written);
		CHECK(status == 0 && written == 36U);
		CHECK_BYTES(written_bytes, message, 36);
		for (room = 0; room < 36U; room++) {
			block = exact_copy(message, room);
			status = sslc_msg_write_finished(finished, 32, block, room, &written);
			CHECK(status == ENOBUFS);
			free(block);
		}
	}

	/* The NewSessionTicket of section 3 (ticket, an early_data extension). */
	size = rfc_message(SEC3, "{server} construct a NewSessionTicket", "NewSessionTicket", 0, SSLC_HS_NEW_SESSION_TICKET, message, sizeof(message));
	CHECK(size == 205U);
	block = exact_copy(message + 4, size - 4U);
	alert = 777;
	status = sslc_msg_parse_new_session_ticket(block, size - 4U, &alert);
	CHECK(status == 0 && alert == 0U);
	free(block);
	for (cut_size = 0; cut_size < size - 4U; cut_size++) {
		block = exact_copy(message + 4, cut_size);
		status = sslc_msg_parse_new_session_ticket(block, cut_size, &alert);
		CHECK(status == EINVAL && alert == DEC);
		free(block);
	}

	/* The CertificateRequest of section 6: an empty context and a signature_algorithms extension. */
	size = rfc_message(SEC6, "{server} construct a CertificateRequest", "CertificateRequest", 0, SSLC_HS_CERTIFICATE_REQUEST, message, sizeof(message));
	CHECK(size == 43U);
	{
		const uint8_t *context;
		size_t context_size;

		block = exact_copy(message + 4, size - 4U);
		alert = 777;
		context = (const uint8_t *)1;
		context_size = 555;
		status = sslc_msg_parse_certificate_request(block, size - 4U, 1, &context, &context_size, &alert);
		CHECK(status == 0 && alert == 0U);
		CHECK(context_size == 0U);
		free(block);
		for (cut_size = 0; cut_size < size - 4U; cut_size++) {
			block = exact_copy(message + 4, cut_size);
			status = sslc_msg_parse_certificate_request(block, cut_size, 1, &context, &context_size, &alert);
			CHECK(status == EINVAL && alert == DEC);
			free(block);
		}

		/* The client's reply to it: an empty Certificate echoing the context (nothing, here). */
		status = sslc_msg_write_empty_certificate(1, context, 0, written_bytes, sizeof(written_bytes), &written);
		CHECK(status == 0 && written == 8U);
	}

	/* Message headers of the first handshake message of each section. */
	size = rfc_message(SEC3, "{server} construct a Finished", "Finished", 0, SSLC_HS_FINISHED, message, sizeof(message));
	sslc_msg_header(message, &type, &length);
	CHECK(type == SSLC_HS_FINISHED && length == 32U);
}

/*
 * The other parsers: tables.
 */

/* Builds a Certificate body of count certificates of the given size each, with TLS 1.3's context and extensions or TLS 1.2's plain list. */
static void
make_certificate(
	struct buffer *body,
	int tls13,
	size_t context_size,
	size_t count,
	size_t cert_size,
	size_t ext_size)
{
	size_t index;
	size_t entry;

	/* The context, the list length, then each entry. */
	body->size = 0;
	entry = 3U + cert_size + (tls13 ? 2U + ext_size : 0U);
	if (tls13) {
		put8(body, (unsigned)context_size);
		put_fill(body, 0x55, context_size);
	}
	put24(body, (unsigned long)(entry * count));
	for (index = 0; index < count; index++) {
		put24(body, (unsigned long)cert_size);
		put_fill(body, 0x30, cert_size);
		if (tls13) {
			put16(body, (unsigned)ext_size);
			if (ext_size >= 4U) {
				put16(body, 0x0005U);
				put16(body, (unsigned)(ext_size - 4U));
				put_fill(body, 0x01, ext_size - 4U);
			} else {
				put_fill(body, 0x77, ext_size);
			}
		}
	}
}

/* Checks the Certificate parser on built and malformed bodies, both versions, against the model. */
static void
test_certificate(void)
{
	struct sslc_certificate_msg certificate;
	struct buffer body;
	uint8_t *block;
	unsigned alert;
	size_t count;
	size_t cert_size;
	size_t index;
	int tls13;
	int status;

	/* Each version, from 0 to 17 certificates of a few sizes. */
	buffer_init(&body);
	for (tls13 = 0; tls13 < 2; tls13++) {
		for (count = 0; count <= 17U; count++) {
			for (cert_size = 1; cert_size < 600U; cert_size = cert_size * 3U + 1U) {
				make_certificate(&body, tls13, tls13 ? 4U : 0U, count, cert_size, 0);
				compare_certificate(body.bytes, body.size, tls13);
				block = exact_copy(body.bytes, body.size);
				status = sslc_msg_parse_certificate(block, body.size, tls13, &certificate, &alert);
				if (count == 0U) {
					CHECK(status == EINVAL && alert == DEC);
				} else if (count <= CRYC_CHAIN_MAX_PEER) {
					CHECK(status == 0 && alert == 0U);
					CHECK(certificate.count == count);
					for (index = 0; index < count; index++)
						CHECK(certificate.der_size[index] == cert_size);
					CHECK(certificate.context_size == (tls13 ? 4U : 0U));
				} else {
					CHECK(status == EPROTO && alert == BADC);
				}
				free(block);
			}
		}
	}

	/* A TLS 1.3 entry with extensions: well formed ones are unsupported_extension, a cut one is decode_error. */
	for (index = 1; index < 12U; index++) {
		make_certificate(&body, 1, 0, 1, 20, index);
		compare_certificate(body.bytes, body.size, 1);
		block = exact_copy(body.bytes, body.size);
		status = sslc_msg_parse_certificate(block, body.size, 1, &certificate, &alert);
		if (index >= 4U) {
			CHECK(status == EPROTO && alert == UNS);
		} else {
			CHECK(status == EINVAL && alert == DEC);
		}
		free(block);
	}

	/* Contexts of 0, 1 and 255 bytes. */
	for (index = 0; index < 3U; index++) {
		static const size_t contexts[3] = { 0, 1, 255 };

		make_certificate(&body, 1, contexts[index], 2, 10, 0);
		compare_certificate(body.bytes, body.size, 1);
		block = exact_copy(body.bytes, body.size);
		status = sslc_msg_parse_certificate(block, body.size, 1, &certificate, &alert);
		CHECK(status == 0 && certificate.context_size == contexts[index] && certificate.count == 2U);
		free(block);
	}

	/* A TLS 1.3 body read as TLS 1.2 and a TLS 1.2 body read as TLS 1.3. */
	make_certificate(&body, 1, 0, 1, 10, 0);
	compare_certificate(body.bytes, body.size, 0);
	make_certificate(&body, 0, 0, 1, 10, 0);
	compare_certificate(body.bytes, body.size, 1);

	/* Malformed: an empty body, a list length of 1 or 2 bytes, trailing bytes, a zero-length certificate, a too-long list. */
	body.size = 0;
	compare_certificate(body.bytes, 0, 1);
	compare_certificate(body.bytes, 0, 0);
	{
		static const char *const bad12[] = {
			"00", "0000", "000000", "000001", "000001ff", "00000300000000", "000004" "000001" "aa" "bb",
			"000007" "000003aabbcc" "00", "000006" "000003aabbcc" "00", "000003" "000003",
			"000009" "000003aabbcc" "000001", "000003" "ffffff"
		};

		for (index = 0; index < sizeof(bad12) / sizeof(bad12[0]); index++) {
			body.size = 0;
			CHECK(put_hex(&body, bad12[index]) == 0);
			compare_certificate(body.bytes, body.size, 0);
			block = exact_copy(body.bytes, body.size);
			status = sslc_msg_parse_certificate(block, body.size, 0, &certificate, &alert);
			CHECK(status == EINVAL && alert == DEC);
			free(block);
		}
	}

	/* Trailing bytes after the list. */
	make_certificate(&body, 0, 0, 2, 12, 0);
	put8(&body, 0);
	compare_certificate(body.bytes, body.size, 0);
	block = exact_copy(body.bytes, body.size);
	status = sslc_msg_parse_certificate(block, body.size, 0, &certificate, &alert);
	CHECK(status == EINVAL && alert == DEC);
	free(block);
	make_certificate(&body, 1, 0, 2, 12, 0);
	put8(&body, 0);
	compare_certificate(body.bytes, body.size, 1);

	/* A big certificate, 2^24-1 bytes, is a view of the body (the list's own limit allows it). */
	make_certificate(&body, 0, 0, 1, 100000U, 0);
	compare_certificate(body.bytes, body.size, 0);
	buffer_free(&body);
}

/* Checks the largest messages a 3-byte length field holds: a Finished and a TLS 1.2 Certificate of 2^24-1 bytes. */
static void
test_largest_messages(void)
{
	struct sslc_certificate_msg certificate;
	uint8_t *data;
	uint8_t *out;
	uint8_t *body;
	size_t written;
	size_t size;
	unsigned alert;
	int status;

	/* A Finished whose verify_data is 16777215 bytes: the header says ff ff ff. */
	size = 16777215U;
	data = calloc(size, 1U);
	out = malloc(size + 4U);
	data[0] = 0x11U;
	data[size - 1U] = 0x22U;
	status = sslc_msg_write_finished(data, size, out, size + 4U, &written);
	CHECK(status == 0 && written == size + 4U);
	CHECK(out[0] == 0x14U && out[1] == 0xffU && out[2] == 0xffU && out[3] == 0xffU);
	CHECK(out[4] == 0x11U && out[size + 3U] == 0x22U);
	status = sslc_msg_write_finished(data, size, out, size + 3U, &written);
	CHECK(status == ENOBUFS && written == 0U);
	free(out);
	free(data);

	/* A TLS 1.2 Certificate: the list is 2^24-1 bytes, one certificate of 2^24-4 bytes. */
	size = 3U + 16777215U;
	body = malloc(size);
	body[0] = 0xffU;
	body[1] = 0xffU;
	body[2] = 0xffU;
	body[3] = 0xffU;
	body[4] = 0xffU;
	body[5] = 0xfcU;
	memset(body + 6, 0x30, size - 6U);
	alert = 777;
	status = sslc_msg_parse_certificate(body, size, 0, &certificate, &alert);
	CHECK(status == 0 && alert == 0U);
	CHECK(certificate.count == 1U && certificate.der_size[0] == 16777212U && certificate.der[0] == body + 6U);
	free(body);
}

/* Checks the CertificateVerify parser over boundary lengths. */
static void
test_certificate_verify(void)
{
	struct buffer body;
	size_t length;
	size_t extra;

	/* The signature length against the body: equal, short and long, at the 8-bit and 16-bit boundaries. */
	buffer_init(&body);
	for (length = 0; length <= 300U; length += (length < 8U ? 1U : 37U)) {
		for (extra = 0; extra < 3U; extra++) {
			body.size = 0;
			put16(&body, 0x0804U);
			put16(&body, (unsigned)(length + extra - 1U));
			put_fill(&body, 0x42, length);
			compare_certificate_verify(body.bytes, body.size);
			body.size = 0;
			put16(&body, 0x0804U);
			put16(&body, (unsigned)length);
			put_fill(&body, 0x42, length + extra);
			compare_certificate_verify(body.bytes, body.size);
		}
	}

	/* The largest signature, 65535 bytes, and one byte more as the body that holds a longer one. */
	body.size = 0;
	put16(&body, 0x0403U);
	put16(&body, 65535U);
	put_fill(&body, 0x11, 65535U);
	compare_certificate_verify(body.bytes, body.size);
	put8(&body, 0x11);
	compare_certificate_verify(body.bytes, body.size);

	/* The empty signature is accepted by the syntax (the verifier refuses it). */
	body.size = 0;
	put16(&body, 0x0403U);
	put16(&body, 0);
	compare_certificate_verify(body.bytes, body.size);
	buffer_free(&body);
}

/* Checks the ServerKeyExchange parser. */
static void
test_server_key_exchange(void)
{
	struct sslc_server_key_exchange exchange;
	struct buffer body;
	uint8_t *block;
	unsigned alert;
	size_t point_size;
	size_t signature_size;
	size_t curve;
	int status;

	/* A good one: curve_type 3, secp256r1, a 65-byte point, ecdsa_secp256r1_sha256, a 70-byte signature. */
	buffer_init(&body);
	put8(&body, 3);
	put16(&body, 0x0017U);
	put8(&body, 65);
	put8(&body, 0x04);
	put_fill(&body, 0x66, 64);
	put16(&body, 0x0403U);
	put16(&body, 70);
	put_fill(&body, 0x77, 70);
	block = exact_copy(body.bytes, body.size);
	alert = 777;
	status = sslc_msg_parse_server_key_exchange(block, body.size, &exchange, &alert);
	CHECK(status == 0 && alert == 0U);
	CHECK(exchange.group == 0x0017U);
	CHECK(exchange.point == block + 4U && exchange.point_size == 65U && exchange.point[0] == 0x04U);
	CHECK(exchange.params == block && exchange.params_size == 1U + 2U + 1U + 65U);
	CHECK(exchange.scheme == 0x0403U);
	CHECK(exchange.signature == block + 4U + 65U + 4U && exchange.signature_size == 70U);
	free(block);
	compare_server_key_exchange(body.bytes, body.size);

	/* Every cut. */
	for (curve = 0; curve < body.size; curve++)
		compare_server_key_exchange(body.bytes, curve);

	/* One more byte at the end. */
	put8(&body, 0);
	compare_server_key_exchange(body.bytes, body.size);

	/* The curve type: 0 to 255; only 3 is accepted, the rest (with a body that is otherwise good) are illegal_parameter. */
	for (curve = 0; curve < 256U; curve++) {
		body.size = 0;
		put8(&body, (unsigned)curve);
		put16(&body, 0x001dU);
		put8(&body, 32);
		put_fill(&body, 0x66, 32);
		put16(&body, 0x0403U);
		put16(&body, 4);
		put_fill(&body, 0x77, 4);
		compare_server_key_exchange(body.bytes, body.size);
		block = exact_copy(body.bytes, body.size);
		status = sslc_msg_parse_server_key_exchange(block, body.size, &exchange, &alert);
		if (curve == 3U) {
			CHECK(status == 0 && alert == 0U);
		} else {
			CHECK(status == EPROTO && alert == ILL);
		}
		free(block);
	}

	/* Point and signature sizes at their ends: 0 (refused for the point), 1, 255; 0, 1, 256, 65535 for the signature. */
	for (point_size = 0; point_size <= 255U; point_size += (point_size < 2U ? 1U : 127U)) {
		for (signature_size = 0; signature_size <= 65535U; signature_size = (signature_size < 2U ? signature_size + 1U : signature_size * 16U + 1U)) {
			body.size = 0;
			put8(&body, 3);
			put16(&body, 0x0018U);
			put8(&body, (unsigned)point_size);
			put_fill(&body, 0x66, point_size);
			put16(&body, 0x0503U);
			put16(&body, (unsigned)signature_size);
			put_fill(&body, 0x77, signature_size);
			compare_server_key_exchange(body.bytes, body.size);
		}
	}
	body.size = 0;
	put8(&body, 3);
	put16(&body, 0x0018U);
	put8(&body, 255);
	put_fill(&body, 0x66, 255);
	put16(&body, 0x0503U);
	put16(&body, 65535U);
	put_fill(&body, 0x77, 65535U);
	compare_server_key_exchange(body.bytes, body.size);
	block = exact_copy(body.bytes, body.size);
	status = sslc_msg_parse_server_key_exchange(block, body.size, &exchange, &alert);
	CHECK(status == 0 && exchange.point_size == 255U && exchange.signature_size == 65535U);
	free(block);
	buffer_free(&body);
}

/* Checks the EncryptedExtensions parser on tables. */
static void
test_encrypted_extensions(void)
{
	static const char *const hexes[] = {
		"0000", "0004" "00000000", "0004" "000a0000", "000a" "000a0006" "0004" "001d0017",
		"000a" "000a0006" "0004" "001d0017", "0006" "000a0002" "0000", "0008" "000a0004" "0002" "001d",
		"0008" "000a0004" "0001" "1d", "000c" "00000000" "000a0004" "0002" "001d", "000c" "000a0004" "0002" "001d" "00000000",
		"0008" "00000000" "00000000", "0001" "00", "0003" "000a00", "00", "", "0004" "00000001",
		"0005" "00000001" "00", "0004" "001c0000", "0006" "001c0002" "4001", "0008" "00000000" "001c0000",
		"0008" "001c0000" "00000000", "000c" "000a0004" "0002" "001d" "0000000000", "0009" "000a0005" "0003" "001d0017",
		"0006" "000a0002" "0002", "0007" "000a0003" "0000" "00", "0008" "000a0004" "0000" "0000",
		"0004" "00150000", "0004" "00290000", "0004" "002a0000", "0004" "0a0a0000", "0004" "ff010000", "0004" "00100000"
	};
	struct buffer body;
	size_t index;
	size_t cut;

	/* Each, against the model. */
	buffer_init(&body);
	for (index = 0; index < sizeof(hexes) / sizeof(hexes[0]); index++) {
		body.size = 0;
		CHECK(put_hex(&body, hexes[index]) == 0);
		compare_encrypted_extensions(body.bytes, body.size);
		for (cut = 0; cut < body.size; cut++)
			compare_encrypted_extensions(body.bytes, cut);
	}

	/* Explicit results. */
	{
		struct {
			const char *hex;
			int status;
			unsigned alert;
			int ack;
		} const cases[] = {
			{ "0000", 0, 0, 0 },
			{ "0004" "00000000", 0, 0, 1 },
			{ "0008" "000a0004" "0002" "001d", 0, 0, 0 },
			{ "000c" "000a0004" "0002" "001d" "00000000", 0, 0, 1 },
			{ "0004" "00000001", EINVAL, DEC, 0 },
			{ "0008" "00000000" "00000000", EINVAL, DEC, 0 },
			{ "0010" "000a0004" "0002" "001d" "000a0004" "0002" "0017", EINVAL, DEC, 0 },
			{ "0008" "000a0004" "0001" "1d", EINVAL, DEC, 0 },
			{ "0006" "000a0002" "0000", EINVAL, DEC, 0 },
			{ "0009" "000a0005" "0003" "001d0017", EINVAL, DEC, 0 },
			{ "0004" "001c0000", EPROTO, UNS, 0 },
			{ "0004" "00290000", EPROTO, UNS, 0 },
			{ "0004" "002a0000", EPROTO, UNS, 0 },
			{ "0004" "00100000", EPROTO, UNS, 0 },
			{ "0004" "00170000", EPROTO, ILL, 0 },
			{ "0006" "002b0002" "0304", EPROTO, ILL, 0 },
			{ "0004" "002c0000", EPROTO, ILL, 0 },
			{ "0006" "000d0002" "0403", EPROTO, ILL, 0 },
			{ "0005" "ff010001" "00", EPROTO, ILL, 0 },
			{ "0001" "00", EINVAL, DEC, 0 },
			{ "0003" "000a00", EINVAL, DEC, 0 },
			{ "", EINVAL, DEC, 0 },
			{ "00", EINVAL, DEC, 0 },
			{ "0000" "00", EINVAL, DEC, 0 }
		};
		uint8_t *block;
		unsigned alert;
		int ack;
		int status;

		for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
			body.size = 0;
			CHECK(put_hex(&body, cases[index].hex) == 0);
			block = exact_copy(body.bytes, body.size);
			alert = 777;
			ack = 555;
			status = sslc_msg_parse_encrypted_extensions(block, body.size, &ack, &alert);
			CHECK(status == cases[index].status);
			CHECK(alert == cases[index].alert);
			if (status == 0)
				CHECK(ack == cases[index].ack);
			free(block);
		}
	}

	/* A group list of 1 to 40 groups (the list is parsed and ignored), then 32767 groups at the top of a 2-byte list. */
	for (index = 1; index <= 40U; index++) {
		body.size = 0;
		put16(&body, (unsigned)(4U + 2U + index * 2U));
		put16(&body, 10);
		put16(&body, (unsigned)(2U + index * 2U));
		put16(&body, (unsigned)(index * 2U));
		put_fill(&body, 0x1d, index * 2U);
		compare_encrypted_extensions(body.bytes, body.size);
	}
	body.size = 0;
	put16(&body, (unsigned)(4U + 2U + 65532U));
	put16(&body, 10);
	put16(&body, 2U + 65532U);
	put16(&body, 65532U);
	put_fill(&body, 0x1d, 65532U);
	compare_encrypted_extensions(body.bytes, body.size);
	buffer_free(&body);
}

/* Checks the KeyUpdate parser over every length 0 to 3 and every byte value. */
static void
test_key_update(void)
{
	uint8_t data[3];
	size_t size;
	unsigned value;

	/* Size 1: every value. */
	for (value = 0; value < 256U; value++) {
		data[0] = (uint8_t)value;
		compare_key_update(data, 1);
	}

	/* The other sizes. */
	data[0] = 0;
	data[1] = 0;
	data[2] = 0;
	for (size = 0; size <= 3U; size++)
		compare_key_update(data, size);
	data[0] = 1;
	compare_key_update(data, 2);
	compare_key_update(data, 3);
}

/* Checks the CertificateRequest parser of both versions on tables. */
static void
test_certificate_request(void)
{
	struct {
		const char *name;
		int tls13;
		const char *hex;
		int status;
		unsigned alert;
		size_t context_size;
	} const cases[] = {
		/* TLS 1.3: context, then extensions of at least two bytes. */
		{ "13 rfc8448", 1, "00" "0024" "000d0020" "001e040305030603020308040805080604010501060102010402050206020202", 0, 0, 0 },
		{ "13 context 3", 1, "03" "aabbcc" "0006" "000d0002" "0000", 0, 0, 3 },
		{ "13 two extensions", 1, "00" "000a" "000d0002" "0000" "00130000", 0, 0, 0 },
		{ "13 empty body", 1, "", EINVAL, DEC, 0 },
		{ "13 no extensions", 1, "00", EINVAL, DEC, 0 },
		{ "13 empty extensions", 1, "00" "0000", EINVAL, DEC, 0 },
		{ "13 extensions one byte", 1, "00" "0001" "00", EINVAL, DEC, 0 },
		{ "13 extensions length two", 1, "00" "0002" "0000", EINVAL, DEC, 0 },
		{ "13 extensions cut", 1, "00" "0004" "000d00", EINVAL, DEC, 0 },
		{ "13 extension data cut", 1, "00" "0006" "000d0003" "00", EINVAL, DEC, 0 },
		{ "13 extension header cut", 1, "00" "0003" "000d00", EINVAL, DEC, 0 },
		{ "13 trailing byte", 1, "00" "0006" "000d0002" "0000" "00", EINVAL, DEC, 0 },
		{ "13 context longer than body", 1, "05" "aabb", EINVAL, DEC, 0 },
		{ "13 extension inside is odd", 1, "00" "0005" "000d0001" "00", 0, 0, 0 },
		/* TLS 1.2: types, algorithms, authorities. */
		{ "12 minimal", 0, "01" "01" "0002" "0401" "0000", 0, 0, 0 },
		{ "12 authorities", 0, "02" "0102" "0004" "0401" "0503" "0008" "0003" "aabbcc" "0001" "dd", 0, 0, 0 },
		{ "12 no types", 0, "00" "0002" "0401" "0000", EINVAL, DEC, 0 },
		{ "12 no algorithms", 0, "01" "01" "0000" "0000", EINVAL, DEC, 0 },
		{ "12 odd algorithms", 0, "01" "01" "0003" "040105" "0000", EINVAL, DEC, 0 },
		{ "12 no authorities field", 0, "01" "01" "0002" "0401", EINVAL, DEC, 0 },
		{ "12 authority empty", 0, "01" "01" "0002" "0401" "0002" "0000", EINVAL, DEC, 0 },
		{ "12 authority cut", 0, "01" "01" "0002" "0401" "0003" "0002" "aa", EINVAL, DEC, 0 },
		{ "12 authority length half", 0, "01" "01" "0002" "0401" "0001" "00", EINVAL, DEC, 0 },
		{ "12 trailing byte", 0, "01" "01" "0002" "0401" "0000" "00", EINVAL, DEC, 0 },
		{ "12 empty body", 0, "", EINVAL, DEC, 0 },
		{ "12 types longer than body", 0, "05" "0102", EINVAL, DEC, 0 }
	};
	struct buffer body;
	const uint8_t *context;
	size_t context_size;
	uint8_t *block;
	unsigned alert;
	size_t index;
	size_t cut;
	int status;

	/* Each case: explicit result, the view of the context, and the invariants. */
	buffer_init(&body);
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		body.size = 0;
		CHECK(put_hex(&body, cases[index].hex) == 0);
		block = exact_copy(body.bytes, body.size);
		alert = 777;
		context = (const uint8_t *)1;
		context_size = 555;
		status = sslc_msg_parse_certificate_request(block, body.size, cases[index].tls13, &context, &context_size, &alert);
		if (status != cases[index].status || alert != cases[index].alert)
			printf("  case %s: status %d alert %u\n", cases[index].name, status, alert);
		CHECK(status == cases[index].status);
		CHECK(alert == cases[index].alert);
		if (status == 0) {
			CHECK(context_size == cases[index].context_size);
			if (cases[index].tls13)
				CHECK(inside(context, context_size, block, body.size));
		}
		free(block);

		/* The cuts of the good ones are all errors. */
		if (status == 0) {
			for (cut = 0; cut < body.size; cut++) {
				block = exact_copy(body.bytes, cut);
				status = sslc_msg_parse_certificate_request(block, cut, cases[index].tls13, &context, &context_size, &alert);
				CHECK(status == EINVAL && alert == DEC);
				free(block);
			}
		}
	}
	buffer_free(&body);
}

/* Checks the NewSessionTicket parser on tables. */
static void
test_new_session_ticket(void)
{
	struct {
		const char *name;
		const char *hex;
		int status;
		unsigned alert;
	} const cases[] = {
		{ "minimal", "00000000" "00000000" "00" "0001" "aa" "0000", 0, 0 },
		{ "nonce 255 cut", "00000e10" "01020304" "ff", EINVAL, DEC },
		{ "with extension", "00000000" "00000000" "02" "aabb" "0001" "cc" "0008" "002a0004" "00000400", 0, 0 },
		{ "empty body", "", EINVAL, DEC },
		{ "seven bytes", "00000000" "000000", EINVAL, DEC },
		{ "no nonce", "00000000" "00000000", EINVAL, DEC },
		{ "nonce longer than body", "00000000" "00000000" "05" "aabb", EINVAL, DEC },
		{ "no ticket", "00000000" "00000000" "00", EINVAL, DEC },
		{ "ticket empty", "00000000" "00000000" "00" "0000" "0000", EINVAL, DEC },
		{ "ticket longer than body", "00000000" "00000000" "00" "0005" "aabb", EINVAL, DEC },
		{ "no extensions", "00000000" "00000000" "00" "0001" "aa", EINVAL, DEC },
		{ "extensions cut", "00000000" "00000000" "00" "0001" "aa" "0004" "002a00", EINVAL, DEC },
		{ "extension data cut", "00000000" "00000000" "00" "0001" "aa" "0005" "002a0002" "00", EINVAL, DEC },
		{ "extension header cut", "00000000" "00000000" "00" "0001" "aa" "0003" "002a00", EINVAL, DEC },
		{ "trailing byte", "00000000" "00000000" "00" "0001" "aa" "0000" "00", EINVAL, DEC }
	};
	struct buffer body;
	uint8_t *block;
	unsigned alert;
	size_t index;
	size_t cut;
	size_t big;
	int status;

	/* Each case. */
	buffer_init(&body);
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		body.size = 0;
		CHECK(put_hex(&body, cases[index].hex) == 0);
		block = exact_copy(body.bytes, body.size);
		alert = 777;
		status = sslc_msg_parse_new_session_ticket(block, body.size, &alert);
		if (status != cases[index].status || alert != cases[index].alert)
			printf("  case %s: status %d alert %u\n", cases[index].name, status, alert);
		CHECK(status == cases[index].status);
		CHECK(alert == cases[index].alert);
		free(block);
		if (cases[index].status == 0) {
			for (cut = 0; cut < body.size; cut++) {
				block = exact_copy(body.bytes, cut);
				status = sslc_msg_parse_new_session_ticket(block, cut, &alert);
				CHECK(status == EINVAL && alert == DEC);
				free(block);
			}
		}
	}

	/* The ceilings: a ticket of 65535 bytes is allowed; extensions of 65534 are allowed and 65535 are not (2^16-2). */
	for (big = 0; big < 3U; big++) {
		body.size = 0;
		put16(&body, 0);
		put16(&body, 0);
		put16(&body, 0);
		put16(&body, 0);
		put8(&body, 0);
		put16(&body, big == 0U ? 65535U : 1U);
		put_fill(&body, 0xaa, big == 0U ? 65535U : 1U);
		if (big == 0U) {
			put16(&body, 0);
		} else {
			/* One extension (type 5) that fills the block exactly. */
			put16(&body, big == 1U ? 65534U : 65535U);
			put16(&body, 5);
			put16(&body, big == 1U ? 65530U : 65531U);
			put_fill(&body, 0x00, big == 1U ? 65530U : 65531U);
		}
		block = exact_copy(body.bytes, body.size);
		alert = 777;
		status = sslc_msg_parse_new_session_ticket(block, body.size, &alert);
		if (big < 2U) {
			CHECK(status == 0 && alert == 0U);
		} else {
			CHECK(status == EINVAL && alert == DEC);
		}
		free(block);
	}
	buffer_free(&body);
}

/*
 * Mutated and random inputs.
 */

/* Checks the invariants of every parser's answer on one input, and the models where there are some. */
static void
check_every_parser(
	const uint8_t *data,
	size_t size)
{
	struct sslc_server_hello hello;
	struct sslc_certificate_msg certificate;
	const uint8_t *context;
	size_t context_size;
	uint8_t *block;
	unsigned alert;
	int status;
	size_t index;

	/* ServerHello: the answer is one of the three, its views are inside the body, its alert agrees. */
	block = exact_copy(data, size);
	alert = 777;
	status = sslc_msg_parse_server_hello(block, size, &hello, &alert);
	CHECK(alert_agrees(status, alert));
	if (status == 0) {
		CHECK(inside(hello.session_id, hello.session_id_size, block, size));
		CHECK(inside(hello.share_key, hello.share_size, block, size));
		CHECK(inside(hello.cookie, hello.cookie_size, block, size));
		CHECK(inside(hello.renegotiation_info, hello.renegotiation_info_size, block, size));
		CHECK(hello.session_id_size <= 32U);
		CHECK(hello.cookie_size <= SSLC_MSG_COOKIE_MAX);
		CHECK(hello.compression <= 255U);
	}
	free(block);

	/* Certificate, both versions: against the model. */
	compare_certificate(data, size, 0);
	compare_certificate(data, size, 1);
	block = exact_copy(data, size);
	status = sslc_msg_parse_certificate(block, size, 1, &certificate, &alert);
	if (status == 0) {
		CHECK(certificate.count >= 1U && certificate.count <= CRYC_CHAIN_MAX_PEER);
		for (index = 0; index < certificate.count; index++)
			CHECK(inside(certificate.der[index], certificate.der_size[index], block, size));
		CHECK(inside(certificate.context, certificate.context_size, block, size));
	}
	free(block);

	/* The parsers with models. */
	compare_certificate_verify(data, size);
	compare_server_key_exchange(data, size);
	compare_encrypted_extensions(data, size);
	compare_key_update(data, size);

	/* The two without models: the invariants. */
	block = exact_copy(data, size);
	alert = 777;
	context = NULL;
	context_size = 0;
	status = sslc_msg_parse_certificate_request(block, size, 1, &context, &context_size, &alert);
	CHECK(status == 0 || status == EINVAL);
	CHECK(alert_agrees(status, alert));
	if (status == 0)
		CHECK(inside(context, context_size, block, size));
	alert = 777;
	status = sslc_msg_parse_certificate_request(block, size, 0, &context, &context_size, &alert);
	CHECK(status == 0 || status == EINVAL);
	CHECK(alert_agrees(status, alert));
	alert = 777;
	status = sslc_msg_parse_new_session_ticket(block, size, &alert);
	CHECK(status == 0 || status == EINVAL);
	CHECK(alert_agrees(status, alert));
	free(block);
}

/* Applies one random change to a message in place (a flip, a set, a cut, an insertion or a deletion); returns the new size. */
static size_t
mutate(
	uint8_t *data,
	size_t size,
	size_t room)
{
	size_t position;
	unsigned kind;

	/* Which change. */
	kind = random_below(7U);
	if (size == 0U)
		kind = 3U;
	if (kind == 0U) {
		position = random_below((uint32_t)size);
		data[position] ^= (uint8_t)(1U << random_below(8U));
	} else if (kind == 1U) {
		position = random_below((uint32_t)size);
		data[position] = (uint8_t)random_next();
	} else if (kind == 2U) {
		size = random_below((uint32_t)size + 1U);
	} else if (kind == 3U) {
		if (size < room) {
			data[size] = (uint8_t)random_next();
			size++;
		}
	} else if (kind == 4U) {
		position = random_below((uint32_t)size);
		memmove(data + position, data + position + 1U, size - position - 1U);
		size--;
	} else if (kind == 5U) {
		if (size < room) {
			position = random_below((uint32_t)size + 1U);
			memmove(data + position + 1U, data + position, size - position);
			data[position] = (uint8_t)random_next();
			size++;
		}
	} else {
		position = random_below((uint32_t)size);
		data[position] = (uint8_t)(random_below(2U) == 0U ? 0x00U : 0xffU);
	}
	return size;
}

/* Runs the mutated seeds and random bytes through every parser. */
static void
test_random(void)
{
	static const char *const seed_hex[] = {
		SH_HEAD "002e" E_KS_X25519 E_SV13,
		HRR_HEAD "0014" E_KS_RETRY E_COOKIE2 E_SV13,
		"0303" R32 "00" "c02f" "00" "0013" E_EMS E_RENEG E_ECPF E_SNI,
		"0010" "000a0004" "0002" "001d" "00000000" "00000000",
		"0017" "0000000000" "0000000000000000" "0000" "000000" "000000" "0000" "00",
		"03" "0017" "04" "00000000" "0403" "0004" "11223344",
		"01" "0017" "04" "aabbccdd" "0403" "0004" "11223344",
		"00" "0006" "000d0002" "0000",
		"01" "01" "0002" "0401" "0003" "0001" "aa",
		"00000000" "00000000" "00" "0001" "aa" "0008" "002a0004" "00000400",
		"00000b" "000003aabbcc" "000003ddeeff",
		"00" "00000b" "000003aabbcc" "0000" "000003ddeeff" "0000",
		"0403" "0004" "aabbccdd",
		"01"
	};
	struct buffer body;
	uint8_t data[8192];
	size_t seed;
	size_t iteration;
	size_t size;
	size_t changes;
	size_t index;

	/* Mutations of each seed. */
	buffer_init(&body);
	for (seed = 0; seed < sizeof(seed_hex) / sizeof(seed_hex[0]); seed++) {
		body.size = 0;
		CHECK(put_hex(&body, seed_hex[seed]) == 0);
		check_every_parser(body.bytes, body.size);
		for (iteration = 0; iteration < 2500U; iteration++) {
			memcpy(data, body.bytes, body.size);
			size = body.size;
			changes = 1U + random_below(3U);
			for (index = 0; index < changes; index++)
				size = mutate(data, size, sizeof(data));
			check_every_parser(data, size);
		}
	}

	/* Random bytes of random lengths, and random bytes that begin like a message. */
	for (iteration = 0; iteration < 6000U; iteration++) {
		size = random_below(200U);
		for (index = 0; index < size; index++)
			data[index] = (uint8_t)random_next();
		if (random_below(2U) == 0U && size >= 4U) {
			data[0] = 0;
			data[1] = (uint8_t)(random_below(2U) == 0U ? 0x00 : 0x03);
			data[2] = (uint8_t)(random_below(3U));
			data[3] = (uint8_t)(random_below(4U));
		}
		check_every_parser(data, size);
	}
	buffer_free(&body);
}

/* Runs every check. */
int
main(void)
{
	/* The header, the writer and its literal bytes, the models and errors, the room, the RFC ClientHellos. */
	test_header();
	test_write_literal();
	test_write_models();
	test_write_errors();
	test_write_room();
	test_write_rfc8448();
	test_small_writers();

	/* The parsers: the ServerHello, the RFC 8448 messages, and the rest. */
	test_server_hello_good();
	test_server_hello_bad();
	test_server_hello_prefixes();
	test_rfc_server_hello();
	test_rfc_messages();
	test_certificate();
	test_largest_messages();
	test_certificate_verify();
	test_server_key_exchange();
	test_encrypted_extensions();
	test_key_update();
	test_certificate_request();
	test_new_session_ticket();
	test_random();
	return check_finish("test-msg");
}
