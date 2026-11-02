/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the protection of TLS records (sslc-record.h).
 *
 * The official values are the records of RFC 8448 section 3 to 7
 * (TLS 1.3 with AES-128-GCM, read from the vector file): every record is
 * sealed again with the same key, IV and sequence number and must give the
 * same bytes, and opened to give the same plaintext.  RFC 8448 has no
 * TLS 1.2 record, so the other suites are checked against a reference
 * written here from the formulas of the RFCs (RFC 8446 5.2 and 5.3, RFC
 * 5246 6.2.3.3, RFC 5288 3, RFC 7905 2), which builds the nonce and the
 * additional data in its own way and calls the AEAD of libcrypto-compat
 * directly.  Inputs the sender would never make (padding, oversized inner
 * plaintext, all-zero inner plaintext) are made by the reference.
 */

#include "check.h"
#include "vecfile.h"

#include "userland/base/libssl-compat/sslc-record.h"

/* The state of the pseudo-random generator (xorshift; not a secret). */
static uint32_t random_state = 0x2f6b1c97U;

/* Returns the next pseudo-random 32-bit value. */
static uint32_t
random_next(void)
{
	/* The three xorshift steps. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

/* Returns a pseudo-random number below limit (limit above 0). */
static uint32_t
random_below(
	uint32_t limit)
{
	/* The remainder is good enough for a test. */
	return random_next() % limit;
}

/* Fills size bytes with pseudo-random bytes. */
static void
random_fill(
	uint8_t *out,
	size_t size)
{
	size_t index;

	/* One byte at a time. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)(random_next() >> 11);
}

/* Allocates size bytes (at least 1) in a block of exactly that size, so that ASan sees any overrun. */
static uint8_t *
block_new(
	size_t size)
{
	uint8_t *block;

	/* A zero size still gets a block. */
	block = malloc(size == 0U ? 1U : size);
	if (block == NULL) {
		printf("FAIL out of memory\n");
		exit(EXIT_FAILURE);
	}
	memset(block, 0xa5, size == 0U ? 1U : size);
	return block;
}

/* Makes a 64-bit value from its two halves. */
static uint64_t
make64(
	uint32_t high,
	uint32_t low)
{
	/* The high half shifted up. */
	return ((uint64_t)high << 32) | (uint64_t)low;
}

/* The largest sequence number, which can never be used (5.3). */
static uint64_t
sequence_max(void)
{
	/* All 64 bits set. */
	return make64(0xffffffffU, 0xffffffffU);
}

/* The nine suites of the table, in the order the tests use them. */
static const unsigned suite_ids[9] = {
	0x1301U, 0x1302U, 0x1303U, 0xc02bU, 0xc02cU, 0xc02fU, 0xc030U, 0xcca8U, 0xcca9U
};

/* Fills the keys of a direction for a suite from a seed. */
static void
fill_keys(
	struct sslc_traffic_keys *keys,
	const struct sslc_suite *suite,
	unsigned seed)
{
	size_t index;

	/* The suite's sizes, and distinct bytes for the key and the IV. */
	memset(keys, 0, sizeof(*keys));
	keys->suite = suite;
	keys->key_size = suite->key_size;
	keys->iv_size = suite->fixed_iv_size;
	for (index = 0; index < keys->key_size; index++)
		keys->key[index] = (uint8_t)(seed * 7U + index * 5U + 1U);
	for (index = 0; index < keys->iv_size; index++)
		keys->iv[index] = (uint8_t)(seed * 11U + index * 3U + 0x80U);
}

/* Returns 1 when the suite belongs to TLS 1.3. */
static int
is13(
	const struct sslc_suite *suite)
{
	/* The suite's version says so. */
	return suite->version == SSLC_TLS13;
}

/*
 * The reference: writes the record the RFCs describe for the keys, the
 * sequence number, the type, size bytes of plaintext and (TLS 1.3 only)
 * padding zero bytes after the inner content type.  Returns the record's
 * size.  The nonce is built from the sequence number's bits by shifting.
 */
static size_t
ref_seal(
	const struct sslc_traffic_keys *keys,
	uint64_t sequence,
	unsigned type,
	const uint8_t *plain,
	size_t size,
	size_t padding,
	uint8_t *out)
{
	struct cryc_gcm gcm;
	uint8_t nonce[12];
	uint8_t aad[13];
	uint8_t tag[16];
	uint8_t *body;
	size_t body_size;
	size_t total;
	size_t index;
	int version13;
	int chacha;

	/* Which form. */
	version13 = is13(keys->suite);
	chacha = keys->suite->aead == SSLC_AEAD_CHACHA20_POLY1305;
	if (!chacha)
		CHECK(cryc_gcm_init(&gcm, keys->key, keys->key_size) == 0);

	/* The nonce: TLS 1.2 AES-GCM is salt then the sequence number; the others the IV xor the padded sequence number. */
	for (index = 0; index < 12U; index++)
		nonce[index] = keys->iv[index];
	if (!version13 && !chacha) {
		for (index = 0; index < 8U; index++)
			nonce[11U - index] = (uint8_t)(sequence >> (8U * index));
	} else {
		for (index = 0; index < 8U; index++)
			nonce[11U - index] ^= (uint8_t)(sequence >> (8U * index));
	}

	/* TLS 1.3: header application_data, then the encrypted content, type and padding. */
	if (version13) {
		body_size = size + 1U + padding;
		out[0] = 23;
		out[1] = 3;
		out[2] = 3;
		out[3] = (uint8_t)((body_size + 16U) >> 8);
		out[4] = (uint8_t)(body_size + 16U);
		body = out + 5;
		if (size > 0U)
			memcpy(body, plain, size);
		body[size] = (uint8_t)type;
		memset(body + size + 1U, 0, padding);
		memcpy(aad, out, 5);
		if (chacha)
			cryc_chacha20_poly1305_seal(keys->key, nonce, aad, 5, body, body_size, body, tag);
		else
			cryc_gcm_seal(&gcm, nonce, 12, aad, 5, body, body_size, body, tag);
		memcpy(body + body_size, tag, 16);
		return 5U + body_size + 16U;
	}

	/* TLS 1.2: the additional data is the sequence number, type, version and plaintext length. */
	for (index = 0; index < 8U; index++)
		aad[index] = (uint8_t)(sequence >> (8U * (7U - index)));
	aad[8] = (uint8_t)type;
	aad[9] = 3;
	aad[10] = 3;
	aad[11] = (uint8_t)(size >> 8);
	aad[12] = (uint8_t)size;

	/* AES-GCM sends the 8 explicit bytes first; ChaCha20-Poly1305 sends none. */
	out[0] = (uint8_t)type;
	out[1] = 3;
	out[2] = 3;
	if (chacha) {
		total = 5U + size + 16U;
		out[3] = (uint8_t)((size + 16U) >> 8);
		out[4] = (uint8_t)(size + 16U);
		body = out + 5;
	} else {
		total = 5U + 8U + size + 16U;
		out[3] = (uint8_t)((8U + size + 16U) >> 8);
		out[4] = (uint8_t)(8U + size + 16U);
		for (index = 0; index < 8U; index++)
			out[5U + index] = nonce[4U + index];
		body = out + 13;
	}
	if (size > 0U)
		memcpy(body, plain, size);
	if (chacha)
		cryc_chacha20_poly1305_seal(keys->key, nonce, aad, 13, body, size, body, tag);
	else
		cryc_gcm_seal(&gcm, nonce, 12, aad, 13, body, size, body, tag);
	memcpy(body + size, tag, 16);
	return total;
}

/* Sets a direction's keys and checks that the call succeeded. */
static void
make_dir(
	struct sslc_record_dir *dir,
	const struct sslc_traffic_keys *keys)
{
	/* The format follows the suite. */
	sslc_record_init(dir);
	CHECK(sslc_record_set_keys(dir, is13(keys->suite), keys) == 0);
}

/* Returns the offset of the payload in a protected record of the direction. */
static size_t
payload_offset(
	const struct sslc_suite *suite)
{
	/* TLS 1.2 AES-GCM has 8 explicit bytes. */
	if (!is13(suite))
		if (suite->aead != SSLC_AEAD_CHACHA20_POLY1305)
			return 13;
	return 5;
}

/* One record of RFC 8448 split into the sender, the type of its content, its plaintext and its wire bytes. */
struct rfc_state {
	struct sslc_record_dir send[2];
	struct sslc_record_dir recv[2];
	uint8_t key[2][16];
	uint8_t iv[2][12];
	int have_key[2];
	int keyed[2];
	unsigned long sent[2];
	unsigned long protected_records;
	unsigned long plain_records;
};

/* Sets the keys of one sender's direction (0 client, 1 server) from the key and IV just read. */
static void
rfc_set_keys(
	struct rfc_state *state,
	int role)
{
	struct sslc_traffic_keys keys;
	const struct sslc_suite *suite;

	/* TLS_AES_128_GCM_SHA256 is the suite of RFC 8448. */
	suite = sslc_tls_suite(SSLC_SUITE_AES_128_GCM_SHA256);
	CHECK(suite != NULL);
	if (suite == NULL)
		return;
	memset(&keys, 0, sizeof(keys));
	keys.suite = suite;
	memcpy(keys.key, state->key[role], 16);
	keys.key_size = 16;
	memcpy(keys.iv, state->iv[role], 12);
	keys.iv_size = 12;

	/* The sender seals with one copy and the receiver opens with the other. */
	sslc_record_init(&state->send[role]);
	sslc_record_init(&state->recv[role]);
	CHECK(sslc_record_set_keys(&state->send[role], 1, &keys) == 0);
	CHECK(sslc_record_set_keys(&state->recv[role], 1, &keys) == 0);
	state->keyed[role] = 1;
	state->sent[role] = 0;
}

/* Checks one record of RFC 8448: sealed again and opened. */
static void
rfc_check_record(
	struct rfc_state *state,
	int role,
	unsigned type,
	const uint8_t *payload,
	size_t payload_size,
	const uint8_t *wire,
	size_t wire_size)
{
	struct sslc_record_dir plain;
	struct sslc_record_dir saved;
	struct sslc_record record;
	uint8_t out[2048];
	uint8_t copy[2048];
	size_t written;
	size_t length;
	size_t index;
	unsigned header_type;
	int is_plain;

	/* A plaintext record is the header and the payload. */
	is_plain = wire_size == payload_size + 5U && memcmp(wire + 5, payload, payload_size) == 0;
	if (is_plain) {
		state->plain_records++;
		sslc_record_init(&plain);

		/* Sealing gives the wire bytes (version 0x0301 for the first ClientHello). */
		CHECK(sslc_record_seal(&plain, type, wire[2] == 1 ? 1 : 0, payload, payload_size, out, sizeof(out), &written) == 0);
		CHECK(written == wire_size);
		CHECK_BYTES(out, wire, wire_size);
		CHECK(sslc_record_overhead(&plain, payload_size) == wire_size);

		/* The header and the opened record. */
		CHECK(sslc_record_header(&plain, wire, 0, &header_type, &length) == 0);
		CHECK(header_type == type);
		CHECK(length == payload_size);
		memcpy(copy, wire, wire_size);
		CHECK(sslc_record_open(&plain, copy, wire_size, &record) == 0);
		CHECK(record.type == type);
		CHECK(record.size == payload_size);
		CHECK_BYTES(record.data, payload, payload_size);
		CHECK(plain.sequence == 0);
		return;
	}

	/* A protected record: the sender's keys must be set and its sequence number is the count so far. */
	state->protected_records++;
	CHECK(state->keyed[role]);
	if (!state->keyed[role])
		return;
	CHECK(state->send[role].sequence == state->sent[role]);
	CHECK(state->recv[role].sequence == state->sent[role]);

	/* Sealing gives the wire bytes. */
	CHECK(sslc_record_seal(&state->send[role], type, 0, payload, payload_size, out, sizeof(out), &written) == 0);
	CHECK(written == wire_size);
	CHECK_BYTES(out, wire, wire_size);
	CHECK(sslc_record_overhead(&state->send[role], payload_size) == wire_size);

	/* A forged copy (every byte of the record changed in turn) is refused and leaves no plaintext. */
	for (index = 0; index < wire_size; index++) {
		saved = state->recv[role];
		memcpy(copy, wire, wire_size);
		copy[index] ^= 0x01;
		if (index == 3U || index == 4U) {
			CHECK(sslc_record_open(&saved, copy, wire_size, &record) == EPROTO);
		} else if (index == 0U) {
			CHECK(sslc_record_open(&saved, copy, wire_size, &record) == EPROTO);
		} else {
			CHECK(sslc_record_open(&saved, copy, wire_size, &record) == EBADMSG);
		}
		CHECK(saved.sequence == state->sent[role]);
	}

	/* The header is accepted and the record opened to the payload and its type. */
	CHECK(sslc_record_header(&state->recv[role], wire, 0x0303, &header_type, &length) == 0);
	CHECK(header_type == 23);
	CHECK(length == wire_size - 5U);
	memcpy(copy, wire, wire_size);
	CHECK(sslc_record_open(&state->recv[role], copy, wire_size, &record) == 0);
	CHECK(record.type == type);
	CHECK(record.size == payload_size);
	CHECK_BYTES(record.data, payload, payload_size);

	/* Both directions moved to the next sequence number. */
	state->sent[role]++;
	CHECK(state->send[role].sequence == state->sent[role]);
	CHECK(state->recv[role].sequence == state->sent[role]);
}

/* Returns 1 when text contains word. */
static int
contains(
	const char *text,
	const char *word)
{
	/* A substring search. */
	return strstr(text, word) != NULL;
}

/* Walks the labelled values of RFC 8448 and checks every record. */
static void
test_rfc8448(void)
{
	static struct rfc_state state;
	struct vec_file file;
	uint8_t payload[2048];
	uint8_t wire[2048];
	uint8_t value[2048];
	char section[160];
	const char *step;
	const char *label;
	const char *text;
	size_t payload_size;
	size_t size;
	unsigned type;
	int role;
	int has_payload;

	/* The vector file of RFC 8448. */
	if (vec_open(&file, "rfc8448") != 0) {
		CHECK(0);
		return;
	}
	memset(&state, 0, sizeof(state));
	section[0] = '\0';
	payload_size = 0;
	type = 0;
	has_payload = 0;

	/* Each labelled value in order. */
	while (vec_next(&file)) {
		text = vec_get(&file, "section");
		step = vec_get(&file, "step");
		label = vec_get(&file, "label");
		if (text == NULL || step == NULL || label == NULL)
			continue;

		/* A new section starts without keys. */
		if (strcmp(text, section) != 0) {
			strncpy(section, text, sizeof(section) - 1U);
			section[sizeof(section) - 1U] = '\0';
			memset(state.keyed, 0, sizeof(state.keyed));
			has_payload = 0;
		}
		role = strncmp(step, "{client}", 8) == 0 ? 0 : 1;

		/* New traffic keys: a sender's own write keys, or the server's read keys which are the client's write keys. */
		if (contains(step, "derive write traffic keys") || strncmp(step, "{server} derive read traffic keys", 33) == 0) {
			if (strncmp(step, "{server} derive read traffic keys", 33) == 0)
				role = 0;
			if (strcmp(label, "key expanded") == 0) {
				size = vec_hex(&file, "hex", value, sizeof(value));
				CHECK(size == 16U);
				if (size == 16U)
					memcpy(state.key[role], value, 16);
				state.have_key[role] = 1;
			}
			if (strcmp(label, "iv expanded") == 0) {
				size = vec_hex(&file, "hex", value, sizeof(value));
				CHECK(size == 12U);
				if (size == 12U && state.have_key[role] != 0) {
					memcpy(state.iv[role], value, 12);
					rfc_set_keys(&state, role);
				}
			}
			continue;
		}

		/* A record's plaintext, then the record. */
		if (!contains(step, " record:"))
			continue;
		if (strcmp(label, "payload") == 0) {
			payload_size = vec_hex(&file, "hex", payload, sizeof(payload));
			CHECK(payload_size != (size_t)-1);
			has_payload = payload_size != (size_t)-1;
			type = 0;
			if (contains(step, "handshake record"))
				type = 22;
			if (contains(step, "application_data record"))
				type = 23;
			if (contains(step, "alert record"))
				type = 21;
			if (contains(step, "change_cipher_spec record"))
				type = 20;
			CHECK(type != 0);
			continue;
		}
		if (strcmp(label, "complete record") == 0) {
			size = vec_hex(&file, "hex", wire, sizeof(wire));
			CHECK(size != (size_t)-1);
			CHECK(has_payload);
			if (size != (size_t)-1 && has_payload)
				rfc_check_record(&state, role, type, payload, payload_size, wire, size);
			has_payload = 0;
		}
	}
	vec_close(&file);

	/* Every record of the file was seen: 27 protected and 14 plaintext. */
	CHECK(state.protected_records == 27UL);
	CHECK(state.plain_records == 14UL);
}

/* The directions the header tests use: plaintext, TLS 1.3, TLS 1.2 AES-GCM and TLS 1.2 ChaCha20-Poly1305. */
static void
header_dirs(
	struct sslc_record_dir *plain,
	struct sslc_record_dir *tls13,
	struct sslc_record_dir *gcm12,
	struct sslc_record_dir *chacha12)
{
	struct sslc_traffic_keys keys;

	/* Plaintext has no keys. */
	sslc_record_init(plain);

	/* The others with the first suite of each kind. */
	fill_keys(&keys, sslc_tls_suite(0x1301U), 1);
	make_dir(tls13, &keys);
	fill_keys(&keys, sslc_tls_suite(0xc02bU), 2);
	make_dir(gcm12, &keys);
	fill_keys(&keys, sslc_tls_suite(0xcca9U), 3);
	make_dir(chacha12, &keys);
}

/* Makes a record header of the type, version and length. */
static void
make_header(
	uint8_t *header,
	unsigned type,
	unsigned version,
	unsigned length)
{
	/* Type, version, length, each big-endian. */
	header[0] = (uint8_t)type;
	header[1] = (uint8_t)(version >> 8);
	header[2] = (uint8_t)version;
	header[3] = (uint8_t)(length >> 8);
	header[4] = (uint8_t)length;
}

/* Checks sslc_record_header. */
static void
test_header(void)
{
	struct sslc_record_dir dirs[4];
	uint8_t header[5];
	unsigned type;
	size_t length;
	unsigned value;
	unsigned kind;
	unsigned minimum;
	unsigned maximum;
	unsigned version;
	unsigned index;
	int result;

	/* The four kinds of direction. */
	header_dirs(&dirs[0], &dirs[1], &dirs[2], &dirs[3]);

	/* The type: 20 to 23 pass and every other byte is EPROTO, on every direction. */
	for (kind = 0; kind < 4U; kind++) {
		for (value = 0; value < 256U; value++) {
			make_header(header, value, 0x0303U, 100U);
			result = sslc_record_header(&dirs[kind], header, 0, &type, &length);
			if (kind == 1U && value >= 20U && value <= 22U) {
				/* A protected TLS 1.3 direction takes application_data only (and a 1-byte change_cipher_spec). */
				CHECK(result == EPROTO);
			} else if (value >= 20U && value <= 23U) {
				CHECK(result == 0);
				CHECK(type == value);
				CHECK(length == 100U);
			} else {
				CHECK(result == EPROTO);
			}
		}
	}

	/* The version, when none is demanded: 0x0301 to 0x0303 only. */
	for (kind = 0; kind < 4U; kind++) {
		for (version = 0; version < 0x10000U; version += (version < 0x400U ? 1U : 0x101U)) {
			make_header(header, 23, version, 100U);
			result = sslc_record_header(&dirs[kind], header, 0, &type, &length);
			if (version >= 0x0301U && version <= 0x0303U)
				CHECK(result == 0);
			else
				CHECK(result == EPROTO);
		}
	}

	/* The version, when 0x0303 is demanded: that one only. */
	for (kind = 0; kind < 4U; kind++) {
		for (version = 0x0300U; version <= 0x0305U; version++) {
			make_header(header, 23, version, 100U);
			result = sslc_record_header(&dirs[kind], header, 0x0303U, &type, &length);
			if (version == 0x0303U)
				CHECK(result == 0);
			else
				CHECK(result == EPROTO);
		}
	}

	/* The version demanded may be another one too. */
	make_header(header, 22, 0x0304U, 100U);
	CHECK(sslc_record_header(&dirs[0], header, 0x0304U, &type, &length) == 0);
	CHECK(sslc_record_header(&dirs[0], header, 0x0303U, &type, &length) == EPROTO);

	/* The length: the smallest and the largest of each direction, and the ones around. */
	for (kind = 0; kind < 4U; kind++) {
		if (kind == 0U) {
			minimum = 1;
			maximum = 16384U;
		} else if (kind == 1U) {
			minimum = 17;
			maximum = 16384U + 256U;
		} else if (kind == 2U) {
			minimum = 24;
			maximum = 16384U + 2048U;
		} else {
			minimum = 16;
			maximum = 16384U + 2048U;
		}
		for (index = 0; index < 140U; index++) {
			/* 0 to 39 one by one, then five lengths around each limit, then spread-out lengths. */
			if (index < 40U)
				value = index;
			else if (index < 45U)
				value = minimum + index - 42U;
			else if (index < 50U)
				value = maximum + index - 47U;
			else
				value = (index - 50U) * 727U;
			if (value > 65535U)
				continue;
			make_header(header, 23, 0x0303U, value);
			result = sslc_record_header(&dirs[kind], header, 0x0303U, &type, &length);
			if (value > maximum) {
				CHECK(result == EMSGSIZE);
			} else if (value < minimum) {
				CHECK(result == (kind == 0U ? EPROTO : EBADMSG));
			} else {
				CHECK(result == 0);
				CHECK(length == value);
			}
		}
		make_header(header, 23, 0x0303U, 65535U);
		CHECK(sslc_record_header(&dirs[kind], header, 0x0303U, &type, &length) == EMSGSIZE);
	}

	/* On TLS 1.3 a change_cipher_spec record is exactly 1 byte (5): other lengths are protocol errors. */
	make_header(header, 20, 0x0303U, 1);
	CHECK(sslc_record_header(&dirs[1], header, 0x0303U, &type, &length) == 0);
	CHECK(type == 20 && length == 1U);
	for (value = 0; value < 70000U; value += (value < 40U ? 1U : 1009U)) {
		if (value == 1U)
			continue;
		if (value > 65535U)
			break;
		make_header(header, 20, 0x0303U, value);
		CHECK(sslc_record_header(&dirs[1], header, 0x0303U, &type, &length) == EPROTO);
	}

	/* On TLS 1.3 application_data below the protected minimum is short; another type is unexpected whatever its length. */
	make_header(header, 23, 0x0303U, 16);
	CHECK(sslc_record_header(&dirs[1], header, 0x0303U, &type, &length) == EBADMSG);
	make_header(header, 22, 0x0303U, 16);
	CHECK(sslc_record_header(&dirs[1], header, 0x0303U, &type, &length) == EPROTO);
	make_header(header, 21, 0x0303U, 2);
	CHECK(sslc_record_header(&dirs[1], header, 0x0303U, &type, &length) == EPROTO);

	/* On TLS 1.2 a change_cipher_spec record follows the protected minimum (it is the last plaintext one, before keys). */
	make_header(header, 20, 0x0303U, 1);
	CHECK(sslc_record_header(&dirs[2], header, 0x0303U, &type, &length) == EBADMSG);
	CHECK(sslc_record_header(&dirs[0], header, 0x0303U, &type, &length) == 0);

	/* A failed header leaves the outputs alone. */
	type = 77;
	length = 88;
	make_header(header, 99, 0x0303U, 5);
	CHECK(sslc_record_header(&dirs[0], header, 0, &type, &length) == EPROTO);
	CHECK(type == 77U && length == 88U);
}

/* Checks the overhead of every kind of direction. */
static void
test_overhead(void)
{
	struct sslc_record_dir dirs[4];
	size_t sizes[8];
	size_t index;

	/* The sizes to ask about. */
	sizes[0] = 0;
	sizes[1] = 1;
	sizes[2] = 16;
	sizes[3] = 255;
	sizes[4] = 1000;
	sizes[5] = 16383;
	sizes[6] = 16384;
	sizes[7] = 16385;
	header_dirs(&dirs[0], &dirs[1], &dirs[2], &dirs[3]);

	/* Plaintext: the header. TLS 1.3: header, inner type, tag. TLS 1.2 GCM: header, explicit nonce, tag. ChaCha20: header, tag. */
	for (index = 0; index < 7U; index++) {
		CHECK(sslc_record_overhead(&dirs[0], sizes[index]) == 5U + sizes[index]);
		CHECK(sslc_record_overhead(&dirs[1], sizes[index]) == 5U + sizes[index] + 1U + 16U);
		CHECK(sslc_record_overhead(&dirs[2], sizes[index]) == 5U + 8U + sizes[index] + 16U);
		CHECK(sslc_record_overhead(&dirs[3], sizes[index]) == 5U + sizes[index] + 16U);
	}

	/* No record carries more than 2^14 bytes. */
	CHECK(sslc_record_overhead(&dirs[0], sizes[7]) == 0);
	CHECK(sslc_record_overhead(&dirs[1], sizes[7]) == 0);
	CHECK(sslc_record_overhead(&dirs[2], sizes[7]) == 0);
	CHECK(sslc_record_overhead(&dirs[3], (size_t)-1) == 0);
}

/* Seals and opens one record on a suite and checks it against the reference. */
static void
check_seal_open(
	const struct sslc_traffic_keys *keys,
	uint64_t sequence,
	unsigned type,
	const uint8_t *plain,
	size_t size)
{
	struct sslc_record_dir send;
	struct sslc_record_dir recv;
	struct sslc_record record;
	uint8_t *expected;
	uint8_t *wire;
	uint8_t *copy;
	size_t total;
	size_t written;
	size_t reference_size;
	size_t offset;
	unsigned header_type;
	size_t length;

	/* Two directions with the same keys, at the same sequence number. */
	make_dir(&send, keys);
	make_dir(&recv, keys);
	send.sequence = sequence;
	recv.sequence = sequence;
	total = sslc_record_overhead(&send, size);
	CHECK(total > 0U);
	wire = block_new(total);
	expected = block_new(total + 64U);
	copy = block_new(total);

	/* Sealing into a block of exactly the size, compared with the reference. */
	if (!CHECK(sslc_record_seal(&send, type, 0, plain, size, wire, total, &written) == 0)) {
		free(copy);
		free(expected);
		free(wire);
		return;
	}
	CHECK(written == total);
	reference_size = ref_seal(keys, sequence, type, plain, size, 0, expected);
	CHECK(reference_size == total);
	CHECK_BYTES(wire, expected, total);
	CHECK(send.sequence == sequence + 1U);

	/* The header says the right type and length (TLS 1.3 hides the type). */
	CHECK(sslc_record_header(&recv, wire, 0x0303U, &header_type, &length) == 0);
	CHECK(header_type == (is13(keys->suite) ? 23U : type));
	CHECK(length == total - 5U);

	/* TLS 1.2 AES-GCM sends the sequence number as its explicit nonce. */
	offset = payload_offset(keys->suite);
	if (offset == 13U) {
		CHECK(wire[5] == (uint8_t)(sequence >> 56));
		CHECK(wire[8] == (uint8_t)(sequence >> 32));
		CHECK(wire[12] == (uint8_t)sequence);
	}

	/* Opening gives the type and the plaintext as a view into the record. */
	memcpy(copy, wire, total);
	CHECK(sslc_record_open(&recv, copy, total, &record) == 0);
	CHECK(record.type == type);
	CHECK(record.size == size);
	CHECK(record.data == copy + offset);
	CHECK_BYTES(record.data, plain, size);
	CHECK(recv.sequence == sequence + 1U);

	/* Sealing again gives another record (the sequence number moved). */
	if (sequence + 1U != sequence_max()) {
		CHECK(sslc_record_seal(&send, type, 0, plain, size, expected, total, &written) == 0);
		CHECK(memcmp(expected + 5, wire + 5, total - 5U) != 0);
	}

	free(copy);
	free(expected);
	free(wire);
}

/* Seals and opens many sizes, sequence numbers and types on every suite. */
static void
test_suites(void)
{
	static const size_t sizes[16] = { 0, 1, 2, 15, 16, 17, 31, 32, 33, 255, 256, 257, 1000, 4096, 16383, 16384 };
	uint64_t sequences[10];
	struct sslc_traffic_keys keys;
	uint8_t *plain;
	unsigned suite_index;
	unsigned size_index;
	unsigned sequence_index;
	unsigned type;
	unsigned count;

	/* The sequence numbers around the places where bytes carry. */
	sequences[0] = 0;
	sequences[1] = 1;
	sequences[2] = 255;
	sequences[3] = 256;
	sequences[4] = 65536;
	sequences[5] = make64(0, 0xffffffffU);
	sequences[6] = make64(1, 0);
	sequences[7] = make64(0x01020304U, 0x05060708U);
	sequences[8] = make64(0x80000000U, 0);
	sequences[9] = sequence_max() - 1U;
	plain = block_new(16384U);
	count = 0;

	/* Each suite, size and type; the large sizes use only a few sequence numbers. */
	for (suite_index = 0; suite_index < 9U; suite_index++) {
		fill_keys(&keys, sslc_tls_suite(suite_ids[suite_index]), suite_index + 5U);
		for (size_index = 0; size_index < 16U; size_index++) {
			if (size_index % 3U == 0U)
				memset(plain, 0, sizes[size_index]);
			else
				random_fill(plain, sizes[size_index]);
			for (sequence_index = 0; sequence_index < 10U; sequence_index++) {
				if (sizes[size_index] > 300U && sequence_index % 4U != 0U)
					continue;
				for (type = 20; type <= 23U; type++) {
					if (sizes[size_index] > 2U && (type + count) % 4U != 0U)
						continue;
					check_seal_open(&keys, sequences[sequence_index], type, plain, sizes[size_index]);
					count++;
				}
			}
		}
	}
	free(plain);
	CHECK(count > 1000U);
}

/* Opens a reference record, returns the result and the record. */
static int
open_reference(
	const struct sslc_traffic_keys *keys,
	uint64_t sequence,
	unsigned type,
	const uint8_t *plain,
	size_t size,
	size_t padding,
	struct sslc_record *record,
	uint8_t **block)
{
	struct sslc_record_dir recv;
	uint8_t *wire;
	size_t total;
	int result;

	/* The reference record in a block of exactly its size. */
	wire = block_new(5U + size + 1U + padding + 16U);
	total = ref_seal(keys, sequence, type, plain, size, padding, wire);
	make_dir(&recv, keys);
	recv.sequence = sequence;

	/* Opens it in place. */
	result = sslc_record_open(&recv, wire, total, record);
	if (result == 0)
		CHECK(recv.sequence == sequence + 1U);
	else
		CHECK(recv.sequence == sequence || record->size == 0U);
	*block = wire;
	return result;
}

/* Checks the padding, the inner content type and the limits of TLS 1.3. */
static void
test_inner_plaintext(void)
{
	static const size_t sizes[6] = { 0, 1, 100, 16000, 16383, 16384 };
	static const size_t paddings[10] = { 0, 1, 2, 17, 255, 256, 385, 386, 1000, 5000 };
	static const unsigned types[3] = { 21, 22, 23 };
	static const uint8_t content[3] = { 5, 6, 7 };
	struct sslc_traffic_keys keys;
	struct sslc_record record;
	uint8_t *plain;
	uint8_t *block;
	size_t inner;
	unsigned suite_index;
	unsigned size_index;
	unsigned padding_index;
	unsigned type_index;
	int result;

	/* Plaintext of a pattern that ends in a non-zero byte. */
	plain = block_new(16384U);
	random_fill(plain, 16384U);

	/* The three TLS 1.3 suites. */
	for (suite_index = 0; suite_index < 3U; suite_index++) {
		fill_keys(&keys, sslc_tls_suite(suite_ids[suite_index]), suite_index + 40U);
		for (size_index = 0; size_index < 6U; size_index++) {
			for (padding_index = 0; padding_index < 10U; padding_index++) {
				for (type_index = 0; type_index < 3U; type_index++) {
					inner = sizes[size_index] + 1U + paddings[padding_index];

					/* Up to 2^14 + 1 bytes of inner plaintext is allowed, more is record_overflow. */
					result = open_reference(&keys, 3, types[type_index], plain, sizes[size_index], paddings[padding_index], &record, &block);
					if (inner > 16385U) {
						CHECK(result == EMSGSIZE);
						CHECK(record.size == 0U);
					} else {
						CHECK(result == 0);
						CHECK(record.type == types[type_index]);
						CHECK(record.size == sizes[size_index]);
						CHECK_BYTES(record.data, plain, sizes[size_index]);
					}
					free(block);
				}
			}
		}

		/* An inner plaintext of only zeros has no content type (5.4). */
		for (padding_index = 0; padding_index < 10U; padding_index++) {
			result = open_reference(&keys, 9, 0, NULL, 0, paddings[padding_index], &record, &block);
			if (paddings[padding_index] + 1U > 16385U)
				CHECK(result == EMSGSIZE);
			else
				CHECK(result == EPROTO);
			free(block);
		}

		/* Zero bytes in the content are content, and the last non-zero byte is the type: the type byte 0 makes the last content byte the type. */
		result = open_reference(&keys, 4, 0, content, 3, 2, &record, &block);
		CHECK(result == 0);
		CHECK(record.type == 7U);
		CHECK(record.size == 2U);
		CHECK(record.data[0] == 5 && record.data[1] == 6);
		free(block);

		/* Content with zero bytes inside it and before the type. */
		memset(plain, 0, 50);
		plain[10] = 9;
		result = open_reference(&keys, 5, 22, plain, 50, 3, &record, &block);
		CHECK(result == 0);
		CHECK(record.type == 22U);
		CHECK(record.size == 50U);
		CHECK_BYTES(record.data, plain, 50);
		free(block);

		/* A forged record of too long an inner plaintext is a bad tag first. */
		{
			struct sslc_record_dir recv;
			uint8_t *wire;
			size_t total;

			wire = block_new(5U + 16000U + 1U + 400U + 16U);
			total = ref_seal(&keys, 6, 23, plain, 16000, 400, wire);
			make_dir(&recv, &keys);
			recv.sequence = 6;
			wire[total - 1U] ^= 0x80;
			CHECK(sslc_record_open(&recv, wire, total, &record) == EBADMSG);
			CHECK(recv.sequence == 6U);
			free(wire);
		}

		/* A record over the limit of the whole ciphertext (2^14 + 256) is record_overflow before anything else. */
		{
			struct sslc_record_dir recv;
			uint8_t *wire;
			size_t total;

			wire = block_new(5U + 16000U + 1U + 700U + 16U);
			total = ref_seal(&keys, 7, 23, plain, 16000, 700, wire);
			make_dir(&recv, &keys);
			recv.sequence = 7;
			CHECK(total - 5U > 16640U);
			CHECK(sslc_record_open(&recv, wire, total, &record) == EMSGSIZE);
			CHECK(recv.sequence == 7U);
			free(wire);
		}
	}
	free(plain);
}

/* Checks the records that TLS 1.3 accepts or refuses by their outer type. */
static void
test_outer_types(void)
{
	static const uint8_t ccs[6] = { 20, 3, 3, 0, 1, 1 };
	struct sslc_traffic_keys keys;
	struct sslc_record_dir dir;
	struct sslc_record record;
	uint8_t wire[64];
	unsigned type;

	/* A TLS 1.3 direction. */
	fill_keys(&keys, sslc_tls_suite(0x1301U), 3);
	make_dir(&dir, &keys);

	/* A compatibility change_cipher_spec record (one byte, value 1) is given back as it is and does not use a sequence number. */
	memcpy(wire, ccs, 6);
	CHECK(sslc_record_open(&dir, wire, 6, &record) == 0);
	CHECK(record.type == 20U);
	CHECK(record.size == 1U);
	CHECK(record.data == wire + 5);
	CHECK(record.data[0] == 1);
	CHECK(dir.sequence == 0U);

	/* An empty one, one of 2 bytes (1 and 1), a value other than 1, and a long one are protocol errors. */
	wire[0] = 20;
	wire[3] = 0;
	wire[4] = 0;
	CHECK(sslc_record_open(&dir, wire, 5, &record) == EPROTO);
	memcpy(wire, ccs, 6);
	wire[3] = 0;
	wire[4] = 2;
	wire[6] = 1;
	CHECK(sslc_record_open(&dir, wire, 7, &record) == EPROTO);
	for (type = 0; type < 256U; type++) {
		memcpy(wire, ccs, 6);
		wire[5] = (uint8_t)type;
		if (type == 1U) {
			CHECK(sslc_record_open(&dir, wire, 6, &record) == 0);
		} else {
			CHECK(sslc_record_open(&dir, wire, 6, &record) == EPROTO);
		}
	}
	memset(wire, 1, sizeof(wire));
	wire[0] = 20;
	wire[1] = 3;
	wire[2] = 3;
	wire[3] = 0;
	wire[4] = 40;
	CHECK(sslc_record_open(&dir, wire, 45, &record) == EPROTO);
	CHECK(dir.sequence == 0U);

	/* Handshake and alert records cannot be unprotected once keys are set; other types never are. */
	for (type = 0; type < 256U; type++) {
		memset(wire, 0x11, sizeof(wire));
		wire[0] = (uint8_t)type;
		wire[1] = 3;
		wire[2] = 3;
		wire[3] = 0;
		wire[4] = 40;
		if (type == 23U || type == 20U)
			continue;
		CHECK(sslc_record_open(&dir, wire, 45, &record) == EPROTO);
		CHECK(dir.sequence == 0U);
	}

	/* On a TLS 1.2 direction the type is part of the additional data and the same record is a bad tag. */
	fill_keys(&keys, sslc_tls_suite(0xc02bU), 3);
	make_dir(&dir, &keys);
	for (type = 20; type <= 23U; type++) {
		memset(wire, 0x11, sizeof(wire));
		wire[0] = (uint8_t)type;
		wire[1] = 3;
		wire[2] = 3;
		wire[3] = 0;
		wire[4] = 40;
		CHECK(sslc_record_open(&dir, wire, 45, &record) == EBADMSG);
		CHECK(dir.sequence == 0U);
	}
}

/* Checks the refusals of a forged record on every suite. */
static void
test_forgery(void)
{
	struct sslc_traffic_keys keys;
	struct sslc_traffic_keys other;
	struct sslc_record_dir recv;
	struct sslc_record_dir send;
	struct sslc_record record;
	uint8_t plain[40];
	uint8_t wire[128];
	uint8_t copy[128];
	size_t total;
	size_t offset;
	size_t index;
	size_t zero;
	unsigned suite_index;
	unsigned bit;
	int version13;
	int result;

	/* Each suite. */
	random_fill(plain, sizeof(plain));
	for (suite_index = 0; suite_index < 9U; suite_index++) {
		fill_keys(&keys, sslc_tls_suite(suite_ids[suite_index]), suite_index + 70U);
		version13 = is13(keys.suite);
		make_dir(&send, &keys);
		make_dir(&recv, &keys);
		offset = payload_offset(keys.suite);
		CHECK(sslc_record_seal(&send, 23, 0, plain, sizeof(plain), wire, sizeof(wire), &total) == 0);

		/* Every bit of the record changed in turn: the header's type and length are protocol errors, the rest a bad tag. */
		for (index = 0; index < total; index++) {
			for (bit = 0; bit < 8U; bit++) {
				memcpy(copy, wire, total);
				copy[index] ^= (uint8_t)(1U << bit);
				result = sslc_record_open(&recv, copy, total, &record);
				if (index == 3U || index == 4U)
					CHECK(result == EPROTO);
				else if (index == 0U && version13)
					CHECK(result == EPROTO);
				else
					CHECK(result == EBADMSG);
				CHECK(recv.sequence == 0U);
				CHECK(record.size == 0U);
				CHECK(record.type == 0U);

				/* A bad tag leaves no plaintext behind: the bytes the plaintext would have had are zero. */
				if (result == EBADMSG && index >= 5U) {
					for (zero = 0; zero < total - offset - 16U; zero++)
						if (copy[offset + zero] != 0)
							break;
					CHECK(zero == total - offset - 16U);
				}
			}
		}

		/* The record is still good afterwards: the failures did not move the sequence number. */
		memcpy(copy, wire, total);
		CHECK(sslc_record_open(&recv, copy, total, &record) == 0);
		CHECK_BYTES(record.data, plain, sizeof(plain));

		/* A wrong sequence number, key or IV is a bad tag. */
		make_dir(&recv, &keys);
		recv.sequence = 1;
		memcpy(copy, wire, total);
		CHECK(sslc_record_open(&recv, copy, total, &record) == EBADMSG);
		other = keys;
		other.key[0] ^= 0x01;
		make_dir(&recv, &other);
		memcpy(copy, wire, total);
		CHECK(sslc_record_open(&recv, copy, total, &record) == EBADMSG);
		other = keys;
		other.iv[other.iv_size - 1U] ^= 0x01;
		make_dir(&recv, &other);
		memcpy(copy, wire, total);
		CHECK(sslc_record_open(&recv, copy, total, &record) == EBADMSG);

		/* The iv's salt bytes of TLS 1.2 AES-GCM and the first bytes of the other IVs matter as well. */
		other = keys;
		other.iv[0] ^= 0x80;
		make_dir(&recv, &other);
		memcpy(copy, wire, total);
		CHECK(sslc_record_open(&recv, copy, total, &record) == EBADMSG);
	}
}

/* Checks truncated and mismatched sizes with blocks of exactly the size. */
static void
test_truncated(void)
{
	struct sslc_traffic_keys keys;
	struct sslc_record_dir send;
	struct sslc_record_dir recv;
	struct sslc_record record;
	uint8_t plain[30];
	uint8_t wire[128];
	uint8_t *copy;
	size_t total;
	size_t length;
	unsigned suite_index;
	int result;

	/* Each suite, and the plaintext direction. */
	random_fill(plain, sizeof(plain));
	for (suite_index = 0; suite_index < 10U; suite_index++) {
		if (suite_index == 9U) {
			sslc_record_init(&send);
			sslc_record_init(&recv);
		} else {
			fill_keys(&keys, sslc_tls_suite(suite_ids[suite_index]), suite_index + 90U);
			make_dir(&send, &keys);
			make_dir(&recv, &keys);
		}
		CHECK(sslc_record_seal(&send, 23, 0, plain, sizeof(plain), wire, sizeof(wire), &total) == 0);

		/* Every shorter size with the header untouched: the length field does not match. */
		for (length = 0; length < total; length++) {
			copy = block_new(length);
			if (length > 0U)
				memcpy(copy, wire, length);
			result = sslc_record_open(&recv, copy, length, &record);
			CHECK(result == EPROTO);
			CHECK(record.size == 0U);
			free(copy);
		}

		/* Every shorter size with the header's length patched to match: a bad tag, or on a plaintext direction a shorter record. */
		for (length = 5; length < total; length++) {
			copy = block_new(length);
			memcpy(copy, wire, length);
			copy[3] = (uint8_t)((length - 5U) >> 8);
			copy[4] = (uint8_t)(length - 5U);
			result = sslc_record_open(&recv, copy, length, &record);
			if (suite_index == 9U) {
				if (length == 5U)
					CHECK(result == EPROTO);
				else
					CHECK(result == 0);
			} else {
				CHECK(result == EBADMSG);
			}
			CHECK(recv.sequence == 0U);
			free(copy);
		}

		/* One byte too long. */
		copy = block_new(total + 1U);
		memcpy(copy, wire, total);
		copy[total] = 0;
		CHECK(sslc_record_open(&recv, copy, total + 1U, &record) == EPROTO);
		free(copy);
	}
}

/* Checks the sequence number: lock-step runs, the key change reset and the wrap. */
static void
test_sequence(void)
{
	struct sslc_traffic_keys keys;
	struct sslc_record_dir send;
	struct sslc_record_dir recv;
	struct sslc_record_dir plain;
	struct sslc_record record;
	uint8_t content[300];
	uint8_t wire[400];
	uint8_t guard[400];
	size_t total;
	size_t written;
	size_t size;
	unsigned suite_index;
	unsigned count;
	unsigned type;

	/* Each suite: 300 records, the two directions always at the same sequence number. */
	for (suite_index = 0; suite_index < 9U; suite_index++) {
		fill_keys(&keys, sslc_tls_suite(suite_ids[suite_index]), suite_index + 120U);
		make_dir(&send, &keys);
		make_dir(&recv, &keys);
		for (count = 0; count < 300U; count++) {
			size = random_below(280U);
			type = 20U + random_below(4U);
			random_fill(content, size);
			CHECK(sslc_record_seal(&send, type, 0, content, size, wire, sizeof(wire), &total) == 0);
			CHECK(send.sequence == (uint64_t)count + 1U);
			CHECK(sslc_record_open(&recv, wire, total, &record) == 0);
			CHECK(recv.sequence == (uint64_t)count + 1U);
			CHECK(record.type == type);
			CHECK(record.size == size);
			CHECK_BYTES(record.data, content, size);
		}

		/* A record opened out of order is a bad tag, and the order restored opens it. */
		CHECK(sslc_record_seal(&send, 23, 0, content, 10, wire, sizeof(wire), &total) == 0);
		CHECK(sslc_record_seal(&send, 23, 0, content, 11, guard, sizeof(guard), &total) == 0);
		CHECK(sslc_record_open(&recv, guard, total, &record) == EBADMSG);

		/* New keys reset the sequence number to 0, and records of the old keys are no longer opened. */
		CHECK(send.sequence == 302U);
		fill_keys(&keys, sslc_tls_suite(suite_ids[suite_index]), suite_index + 130U);
		CHECK(sslc_record_set_keys(&send, is13(keys.suite), &keys) == 0);
		CHECK(send.sequence == 0U);
		CHECK(sslc_record_set_keys(&recv, is13(keys.suite), &keys) == 0);
		CHECK(recv.sequence == 0U);
		CHECK(sslc_record_seal(&send, 23, 0, content, 5, wire, sizeof(wire), &total) == 0);
		CHECK(sslc_record_open(&recv, wire, total, &record) == 0);

		/* The wrap: the largest sequence number is never used, in either direction. */
		send.sequence = sequence_max() - 1U;
		recv.sequence = sequence_max() - 1U;
		CHECK(sslc_record_seal(&send, 23, 0, content, 5, wire, sizeof(wire), &total) == 0);
		CHECK(send.sequence == sequence_max());
		CHECK(sslc_record_open(&recv, wire, total, &record) == 0);
		CHECK(recv.sequence == sequence_max());
		memset(guard, 0x3c, sizeof(guard));
		CHECK(sslc_record_seal(&send, 23, 0, content, 5, guard, sizeof(guard), &written) == EOVERFLOW);
		CHECK(guard[0] == 0x3c && guard[5] == 0x3c);
		CHECK(send.sequence == sequence_max());
		CHECK(sslc_record_open(&recv, wire, total, &record) == EOVERFLOW);
		CHECK(recv.sequence == sequence_max());
	}

	/* A plaintext direction has no sequence number: it stays 0 and never overflows. */
	sslc_record_init(&plain);
	plain.sequence = 0;
	for (count = 0; count < 20U; count++)
		CHECK(sslc_record_seal(&plain, 22, 0, content, 7, wire, sizeof(wire), &total) == 0);
	CHECK(plain.sequence == 0U);
}

/* Checks the open of the largest sequence number returns EOVERFLOW without touching the record. */
static void
test_open_overflow(void)
{
	struct sslc_traffic_keys keys;
	struct sslc_record_dir send;
	struct sslc_record_dir recv;
	struct sslc_record record;
	uint8_t content[20];
	uint8_t wire[100];
	uint8_t copy[100];
	size_t total;
	unsigned suite_index;

	/* Each suite. */
	random_fill(content, sizeof(content));
	for (suite_index = 0; suite_index < 9U; suite_index++) {
		fill_keys(&keys, sslc_tls_suite(suite_ids[suite_index]), suite_index + 150U);
		make_dir(&send, &keys);
		make_dir(&recv, &keys);
		CHECK(sslc_record_seal(&send, 23, 0, content, sizeof(content), wire, sizeof(wire), &total) == 0);
		recv.sequence = sequence_max();
		memcpy(copy, wire, total);
		CHECK(sslc_record_open(&recv, copy, total, &record) == EOVERFLOW);
		CHECK_BYTES(copy, wire, total);
		CHECK(recv.sequence == sequence_max());
	}
}

/* Checks the refusals of seal, and that a refused call writes nothing and keeps the sequence number. */
static void
test_seal_errors(void)
{
	struct sslc_traffic_keys keys;
	struct sslc_record_dir dir;
	struct sslc_record_dir plain;
	uint8_t content[16];
	uint8_t *out;
	uint8_t *big;
	size_t total;
	size_t written;
	size_t room;
	unsigned suite_index;
	unsigned type;
	int kind;

	/* The plaintext direction and the nine suites. */
	random_fill(content, sizeof(content));
	big = block_new(16385U);
	memset(big, 0x42, 16385U);
	for (kind = 0; kind < 10; kind++) {
		if (kind == 9) {
			sslc_record_init(&dir);
		} else {
			suite_index = (unsigned)kind;
			fill_keys(&keys, sslc_tls_suite(suite_ids[suite_index]), suite_index + 160U);
			make_dir(&dir, &keys);
		}
		total = sslc_record_overhead(&dir, sizeof(content));
		out = block_new(total + 32U);
		written = 12345;

		/* A type that is not one of the four. */
		for (type = 0; type < 256U; type++) {
			if (type >= 20U && type <= 23U)
				continue;
			CHECK(sslc_record_seal(&dir, type, 0, content, sizeof(content), out, total + 32U, &written) == EINVAL);
		}
		CHECK(written == 12345U);
		CHECK(dir.sequence == 0U);

		/* A fragment over 2^14 bytes, and NULL input with a size. */
		CHECK(sslc_record_seal(&dir, 23, 0, big, 16385U, out, total + 32U, &written) == EINVAL);
		CHECK(sslc_record_seal(&dir, 23, 0, NULL, 1, out, total + 32U, &written) == EINVAL);
		CHECK(sslc_record_seal(&dir, 23, 0, big, (size_t)-1, out, total + 32U, &written) == EINVAL);
		CHECK(dir.sequence == 0U);

		/* Too little room writes nothing and does not advance the sequence number. */
		for (room = 0; room < total; room++) {
			memset(out, 0x6e, total + 32U);
			CHECK(sslc_record_seal(&dir, 23, 0, content, sizeof(content), out, room, &written) == ENOBUFS);
			CHECK(out[0] == 0x6e && out[total - 1U] == 0x6e);
		}
		CHECK(dir.sequence == 0U);
		CHECK(written == 12345U);

		/* Exactly enough room works and writes nothing past the record. */
		memset(out, 0x6e, total + 32U);
		CHECK(sslc_record_seal(&dir, 23, 0, content, sizeof(content), out, total, &written) == 0);
		CHECK(written == total);
		CHECK(out[total] == 0x6e && out[total + 31U] == 0x6e);

		/* Empty content works (NULL input with size 0). */
		CHECK(sslc_record_seal(&dir, 23, 0, NULL, 0, out, total + 32U, &written) == 0);
		CHECK(written == sslc_record_overhead(&dir, 0));
		free(out);

		/* A full fragment fits in the largest buffer. */
		out = block_new(sslc_record_overhead(&dir, 16384U));
		CHECK(sslc_record_seal(&dir, 22, 0, big, 16384U, out, sslc_record_overhead(&dir, 16384U), &written) == 0);
		CHECK(written == sslc_record_overhead(&dir, 16384U));
		CHECK(written <= SSLC_RECORD_BUFFER);
		free(out);
	}
	free(big);

	/* A plaintext record's first ClientHello carries the legacy version 0x0301, the rest 0x0303; a protected one ignores the flag. */
	sslc_record_init(&plain);
	out = block_new(5U + sizeof(content));
	CHECK(sslc_record_seal(&plain, 22, 1, content, sizeof(content), out, 5U + sizeof(content), &written) == 0);
	CHECK(out[0] == 22 && out[1] == 3 && out[2] == 1 && out[3] == 0 && out[4] == sizeof(content));
	CHECK(sslc_record_seal(&plain, 22, 0, content, sizeof(content), out, 5U + sizeof(content), &written) == 0);
	CHECK(out[1] == 3 && out[2] == 3);
	free(out);
	fill_keys(&keys, sslc_tls_suite(0x1301U), 1);
	make_dir(&dir, &keys);
	out = block_new(sslc_record_overhead(&dir, 4));
	CHECK(sslc_record_seal(&dir, 22, 1, content, 4, out, sslc_record_overhead(&dir, 4), &written) == 0);
	CHECK(out[0] == 23 && out[1] == 3 && out[2] == 3);
	free(out);
}

/* Checks set_keys, clear and init. */
static void
test_keys(void)
{
	struct sslc_traffic_keys keys;
	struct sslc_traffic_keys other;
	struct sslc_record_dir dir;
	struct sslc_record_dir before;
	struct sslc_record_dir fresh;
	struct sslc_record_dir peer;
	struct sslc_suite odd;
	struct sslc_record record;
	uint8_t wire[100];
	uint8_t content[10];
	uint8_t zero[sizeof(struct sslc_record_dir)];
	size_t total;
	unsigned suite_index;
	unsigned version13;

	/* init makes a plaintext direction. */
	memset(&dir, 0xee, sizeof(dir));
	sslc_record_init(&dir);
	CHECK(dir.suite == NULL);
	CHECK(dir.sequence == 0U);
	CHECK(dir.version13 == 0);

	/* Every suite accepts its own keys in its own format, and no other format. */
	random_fill(content, sizeof(content));
	for (suite_index = 0; suite_index < 9U; suite_index++) {
		fill_keys(&keys, sslc_tls_suite(suite_ids[suite_index]), suite_index);
		for (version13 = 0; version13 < 2U; version13++) {
			sslc_record_init(&dir);
			dir.sequence = 99;
			if ((int)version13 == is13(keys.suite)) {
				CHECK(sslc_record_set_keys(&dir, (int)version13, &keys) == 0);
				CHECK(dir.suite == keys.suite);
				CHECK(dir.version13 == (int)version13);
				CHECK(dir.sequence == 0U);
				CHECK(dir.iv_size == keys.iv_size);
				CHECK_BYTES(dir.iv, keys.iv, keys.iv_size);
			} else {
				before = dir;
				CHECK(sslc_record_set_keys(&dir, (int)version13, &keys) == EINVAL);
				CHECK(memcmp(&before, &dir, sizeof(dir)) == 0);
			}
		}
	}

	/* A refusal leaves a keyed direction as it was and still working. */
	fill_keys(&keys, sslc_tls_suite(0x1301U), 1);
	make_dir(&dir, &keys);
	dir.sequence = 5;
	before = dir;
	other = keys;
	other.suite = NULL;
	CHECK(sslc_record_set_keys(&dir, 1, &other) == EINVAL);
	other = keys;
	other.key_size = 32;
	CHECK(sslc_record_set_keys(&dir, 1, &other) == EINVAL);
	other = keys;
	other.key_size = 15;
	CHECK(sslc_record_set_keys(&dir, 1, &other) == EINVAL);
	other = keys;
	other.iv_size = 11;
	CHECK(sslc_record_set_keys(&dir, 1, &other) == EINVAL);
	other = keys;
	other.iv_size = 13;
	CHECK(sslc_record_set_keys(&dir, 1, &other) == EINVAL);
	other = keys;
	other.iv_size = 0;
	CHECK(sslc_record_set_keys(&dir, 1, &other) == EINVAL);
	CHECK(memcmp(&before, &dir, sizeof(dir)) == 0);

	/* A suite without an AEAD, and a suite that claims sizes beyond the structure's arrays. */
	odd = *sslc_tls_suite(0x1301U);
	odd.aead = SSLC_AEAD_NONE;
	other = keys;
	other.suite = &odd;
	CHECK(sslc_record_set_keys(&dir, 1, &other) == EINVAL);
	odd = *sslc_tls_suite(0x1301U);
	odd.key_size = 64;
	other = keys;
	other.suite = &odd;
	other.key_size = 64;
	CHECK(sslc_record_set_keys(&dir, 1, &other) == EINVAL);
	odd = *sslc_tls_suite(0x1301U);
	odd.fixed_iv_size = 32;
	other = keys;
	other.suite = &odd;
	other.iv_size = 32;
	CHECK(sslc_record_set_keys(&dir, 1, &other) == EINVAL);
	odd = *sslc_tls_suite(0x1303U);
	odd.key_size = 16;
	other = keys;
	other.suite = &odd;
	other.key_size = 16;
	CHECK(sslc_record_set_keys(&dir, 1, &other) == EINVAL);
	CHECK(memcmp(&before, &dir, sizeof(dir)) == 0);

	/* New keys replace the old: the record now follows the new keys and the sequence number restarts. */
	make_dir(&peer, &keys);
	CHECK(sslc_record_seal(&dir, 23, 0, content, sizeof(content), wire, sizeof(wire), &total) == 0);
	fill_keys(&other, sslc_tls_suite(0x1301U), 2);
	CHECK(sslc_record_set_keys(&dir, 1, &other) == 0);
	CHECK(dir.sequence == 0U);
	peer.sequence = 0;
	CHECK(sslc_record_open(&peer, wire, total, &record) == EBADMSG);
	CHECK(sslc_record_seal(&dir, 23, 0, content, sizeof(content), wire, sizeof(wire), &total) == 0);
	CHECK(sslc_record_open(&peer, wire, total, &record) == EBADMSG);

	/* Changing to another suite is allowed, and the old key bytes are not left in the structure. */
	fill_keys(&keys, sslc_tls_suite(0x1303U), 3);
	CHECK(sslc_record_set_keys(&dir, 1, &keys) == 0);
	CHECK(dir.suite == keys.suite);
	CHECK_BYTES(dir.chacha_key, keys.key, 32);

	/* clear wipes everything and makes the direction plaintext again. */
	sslc_record_init(&fresh);
	dir.sequence = 1234;
	sslc_record_clear(&dir);
	CHECK(memcmp(&dir, &fresh, sizeof(dir)) == 0);
	memset(zero, 0, sizeof(zero));
	CHECK(memcmp(&dir, zero, sizeof(dir)) == 0);
	CHECK(dir.suite == NULL);

	/* After clear a record is plaintext. */
	CHECK(sslc_record_seal(&dir, 22, 0, content, sizeof(content), wire, sizeof(wire), &total) == 0);
	CHECK(total == 5U + sizeof(content));
	CHECK(memcmp(wire + 5, content, sizeof(content)) == 0);

	/* Clearing a direction that is already plain is harmless. */
	sslc_record_clear(&dir);
	CHECK(memcmp(&dir, &fresh, sizeof(dir)) == 0);
}

/* Checks the plaintext direction's open and seal round trip. */
static void
test_plaintext(void)
{
	struct sslc_record_dir dir;
	struct sslc_record record;
	uint8_t *content;
	uint8_t *wire;
	size_t sizes[7];
	size_t total;
	size_t index;
	unsigned type;

	/* The sizes of a plaintext fragment. */
	sizes[0] = 1;
	sizes[1] = 2;
	sizes[2] = 255;
	sizes[3] = 256;
	sizes[4] = 1000;
	sizes[5] = 16383;
	sizes[6] = 16384;
	sslc_record_init(&dir);
	content = block_new(16384U);
	random_fill(content, 16384U);
	for (index = 0; index < 7U; index++) {
		for (type = 20; type <= 23U; type++) {
			wire = block_new(5U + sizes[index]);
			CHECK(sslc_record_seal(&dir, type, 0, content, sizes[index], wire, 5U + sizes[index], &total) == 0);
			CHECK(total == 5U + sizes[index]);
			CHECK(wire[0] == type);
			CHECK(wire[3] == (uint8_t)(sizes[index] >> 8));
			CHECK(wire[4] == (uint8_t)sizes[index]);
			CHECK_BYTES(wire + 5, content, sizes[index]);
			CHECK(sslc_record_open(&dir, wire, total, &record) == 0);
			CHECK(record.type == type);
			CHECK(record.size == sizes[index]);
			CHECK(record.data == wire + 5);
			free(wire);
		}
	}
	free(content);

	/* An empty plaintext record is a protocol error (a decode_error), whichever way it is made. */
	wire = block_new(5);
	wire[0] = 22;
	wire[1] = 3;
	wire[2] = 3;
	wire[3] = 0;
	wire[4] = 0;
	CHECK(sslc_record_open(&dir, wire, 5, &record) == EPROTO);
	free(wire);

	/* One over the limit is record_overflow. */
	wire = block_new(5U + 16385U);
	memset(wire, 0, 5U + 16385U);
	wire[0] = 22;
	wire[1] = 3;
	wire[2] = 3;
	wire[3] = (uint8_t)(16385U >> 8);
	wire[4] = (uint8_t)16385U;
	CHECK(sslc_record_open(&dir, wire, 5U + 16385U, &record) == EMSGSIZE);
	free(wire);

	/* Sizes of 0 to 4 bytes are not even a header. */
	for (index = 0; index < 5U; index++) {
		wire = block_new(index);
		CHECK(sslc_record_open(&dir, wire, index, &record) == EPROTO);
		free(wire);
	}
}

/* Checks that random bytes in a record never crash and give one of the documented errors. */
static void
test_random(void)
{
	struct sslc_traffic_keys keys;
	struct sslc_record_dir dirs[10];
	struct sslc_record_dir copy;
	struct sslc_record record;
	uint8_t header[5];
	uint8_t *wire;
	size_t size;
	size_t length;
	unsigned type;
	unsigned kind;
	unsigned count;
	int result;

	/* The nine suites and the plaintext direction. */
	for (kind = 0; kind < 9U; kind++) {
		fill_keys(&keys, sslc_tls_suite(suite_ids[kind]), kind + 170U);
		make_dir(&dirs[kind], &keys);
	}
	sslc_record_init(&dirs[9]);

	/* Random whole records with a header made to fit (or not) and random bytes. */
	for (kind = 0; kind < 10U; kind++) {
		for (count = 0; count < 3000U; count++) {
			size = random_below(8U) == 0U ? random_below(18500U) : random_below(300U);
			wire = block_new(size);
			random_fill(wire, size);
			if (size >= 5U && random_below(4U) != 0U) {
				wire[0] = (uint8_t)(20U + random_below(4U));
				wire[1] = 3;
				wire[2] = (uint8_t)(1U + random_below(3U));
				wire[3] = (uint8_t)((size - 5U) >> 8);
				wire[4] = (uint8_t)(size - 5U);
			}
			copy = dirs[kind];
			result = sslc_record_open(&copy, wire, size, &record);
			CHECK(result == 0 || result == EBADMSG || result == EPROTO || result == EMSGSIZE);
			if (result == 0) {
				CHECK(record.data >= wire);
				CHECK(record.data + record.size <= wire + size);
			} else {
				CHECK(copy.sequence == dirs[kind].sequence);
			}
			free(wire);
		}
	}

	/* Random headers, checked on every direction: a success gives a type 20 to 23 and a length in range. */
	for (kind = 0; kind < 10U; kind++) {
		for (count = 0; count < 5000U; count++) {
			random_fill(header, sizeof(header));
			if (random_below(2U) == 0U)
				header[0] = (uint8_t)(20U + random_below(4U));
			if (random_below(2U) == 0U) {
				header[1] = 3;
				header[2] = (uint8_t)random_below(5U);
			}
			result = sslc_record_header(&dirs[kind], header, random_below(2U) == 0U ? 0U : 0x0303U, &type, &length);
			CHECK(result == 0 || result == EBADMSG || result == EPROTO || result == EMSGSIZE);
			if (result == 0) {
				CHECK(type >= 20U && type <= 23U);
				CHECK(length >= 1U && length <= SSLC_RECORD12_CIPHERTEXT_MAX);
				CHECK(length == (size_t)header[3] * 256U + header[4]);
			}
		}
	}
}

/* Checks a long run of random seal and open pairs, each in a block of exactly its size. */
static void
test_round_trips(void)
{
	struct sslc_traffic_keys keys;
	struct sslc_record_dir send;
	struct sslc_record_dir recv;
	struct sslc_record record;
	uint8_t *content;
	uint8_t *wire;
	size_t size;
	size_t total;
	size_t written;
	unsigned suite_index;
	unsigned count;
	unsigned type;
	uint64_t sequence;

	/* Each suite, with random sizes up to a full fragment, types and a random starting sequence number. */
	content = block_new(16384U);
	for (suite_index = 0; suite_index < 9U; suite_index++) {
		fill_keys(&keys, sslc_tls_suite(suite_ids[suite_index]), suite_index + 190U);
		make_dir(&send, &keys);
		make_dir(&recv, &keys);
		sequence = make64(random_next(), random_next());
		if (sequence > sequence_max() - 1000U)
			sequence = 77;
		send.sequence = sequence;
		recv.sequence = sequence;
		for (count = 0; count < 120U; count++) {
			size = random_below(4U) == 0U ? random_below(16385U) : random_below(200U);
			type = 20U + random_below(4U);
			random_fill(content, size);
			total = sslc_record_overhead(&send, size);
			wire = block_new(total);
			CHECK(sslc_record_seal(&send, type, 0, content, size, wire, total, &written) == 0);
			CHECK(written == total);
			CHECK(sslc_record_open(&recv, wire, written, &record) == 0);
			CHECK(record.type == type);
			CHECK(record.size == size);
			CHECK_BYTES(record.data, content, size);
			CHECK(send.sequence == recv.sequence);
			free(wire);
		}
		CHECK(send.sequence == sequence + 120U);
	}
	free(content);
}

/* Runs the checks. */
int
main(void)
{
	/* The official records first. */
	test_rfc8448();

	/* The shape of the headers and the sizes. */
	test_header();
	test_overhead();

	/* Every suite against the reference. */
	test_suites();
	test_inner_plaintext();
	test_outer_types();

	/* Refusals. */
	test_forgery();
	test_truncated();
	test_sequence();
	test_open_overflow();
	test_seal_errors();

	/* Keys and the plaintext direction. */
	test_keys();
	test_plaintext();

	/* Random inputs and long round trips. */
	test_random();
	test_round_trips();

	/* The result. */
	return check_finish("test-record");
}
