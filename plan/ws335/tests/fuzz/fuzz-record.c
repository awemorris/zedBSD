/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libFuzzer target: record protection (sslc-record.h).  The first input
 * byte picks the case, the rest is its data:
 *
 *   0  a plaintext direction: the data is a whole record (header and
 *      body), checked with sslc_record_header and opened.
 *   1  a TLS 1.3 AES-128-GCM direction: the data is the TLSInnerPlaintext
 *      (content, type and padding as the fuzzer chooses), sealed here
 *      with the direction's key and nonce (RFC 8446 5.2, 5.3) so that the
 *      tag verifies and the inner type and padding are parsed.
 *   2  the same direction: the data is a protected record as it is (the
 *      header's checks and a tag that does not verify).
 *   3  a TLS 1.2 AES-128-GCM direction: the data after one type byte is
 *      sealed with sslc_record_seal and opened again, and must come back
 *      the same.
 *
 * The key and IV are fixed (the target looks for faults of parsing, not
 * of the keys).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "userland/base/libcrypto-compat/cryc-gcm.h"
#include "userland/base/libssl-compat/sslc-record.h"

/* The libFuzzer entry point, run once for every generated input. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Fills in the fixed traffic keys of a cipher suite. */
static void fuzz_keys(struct sslc_traffic_keys *keys, uint16_t suite_id);

/* Checks the header and opens a whole record of size bytes (a copy). */
static void fuzz_open(struct sslc_record_dir *dir, const uint8_t *data, size_t size);

/* Fills in the fixed traffic keys of a cipher suite. */
static void
fuzz_keys(
	struct sslc_traffic_keys *keys,
	uint16_t suite_id)
{
	size_t index;

	/* Names the suite and the fixed key. */
	memset(keys, 0, sizeof(*keys));
	keys->suite = sslc_tls_suite(suite_id);
	keys->key_size = 16;
	for (index = 0; index < 16U; index++)
		keys->key[index] = (uint8_t)(0x10U + index);

	/* Takes the suite's IV size, or 12 bytes when the suite is unknown. */
	keys->iv_size = 12U;
	if (keys->suite != NULL)
		keys->iv_size = (size_t)keys->suite->fixed_iv_size;

	/* Fills the fixed IV. */
	for (index = 0; index < keys->iv_size; index++)
		keys->iv[index] = (uint8_t)(0xa0U + index);
}

/* Checks the header and opens a whole record of size bytes (a copy). */
static void
fuzz_open(
	struct sslc_record_dir *dir,
	const uint8_t *data,
	size_t size)
{
	struct sslc_record record;
	uint8_t *copy;
	unsigned type;
	size_t length;
	int status;

	/* Refuses an input too short for a header. */
	if (size < SSLC_RECORD_HEADER)
		return;

	/* Refuses an input whose header is bad. */
	status = sslc_record_header(dir, data, 0, &type, &length);
	if (status != 0)
		return;

	/* Refuses an input whose length field does not match its size. */
	if (length != size - SSLC_RECORD_HEADER)
		return;

	/* Makes the copy of exactly size bytes, so a read past the end is caught. */
	copy = malloc(size);
	if (copy == NULL)
		return;
	memcpy(copy, data, size);

	/* Opens the record and reads both ends of what it returns. */
	status = sslc_record_open(dir, copy, size, &record);
	if (status == 0 && record.size != 0U)
		(void)(record.data[0] + record.data[record.size - 1U]);

	/* Releases the copy. */
	free(copy);
}

/*
 * Runs the record case picked by the first input byte.
 *
 * Always reports 0, because a refusal is not a failure; a fault of the
 * library itself, or a round trip that does not come back the same, aborts.
 */
int
LLVMFuzzerTestOneInput(
	const uint8_t *data,
	size_t size)
{
	static uint8_t record[SSLC_RECORD_BUFFER + 64U];
	static uint8_t back[SSLC_RECORD_BUFFER];
	struct sslc_record_dir dir;
	struct sslc_record_dir reader;
	struct sslc_traffic_keys keys;
	struct sslc_record opened;
	struct cryc_gcm gcm;
	size_t written;
	unsigned mode;
	int status;
	int cmp;

	/* Needs the byte that picks the case. */
	if (size == 0U)
		return 0;

	/* Takes the case from the first byte and the data from the rest. */
	mode = data[0] % 4U;
	data++;
	size--;
	sslc_record_init(&dir);

	/* Case 0: a plaintext direction opens the data as a whole record. */
	if (mode == 0U) {
		fuzz_open(&dir, data, size);
		return 0;
	}

	/* Cases 1 and 2: a TLS 1.3 AES-128-GCM direction. */
	if (mode == 1U || mode == 2U) {
		fuzz_keys(&keys, 0x1301);
		status = sslc_record_set_keys(&dir, 1, &keys);
		if (status != 0)
			abort();

		/* Case 2: the data is a protected record as it is. */
		if (mode == 2U) {
			fuzz_open(&dir, data, size);
			sslc_record_clear(&dir);
			return 0;
		}

		/* header || AEAD(inner plaintext) || tag with nonce = iv (sequence 0) and the header as the additional data. */
		if (size + 16U > SSLC_RECORD13_CIPHERTEXT_MAX + 64U)
			size = SSLC_RECORD13_CIPHERTEXT_MAX + 64U - 16U;
		record[0] = 23;
		record[1] = 3;
		record[2] = 3;
		record[3] = (uint8_t)((size + 16U) >> 8);
		record[4] = (uint8_t)(size + 16U);
		status = cryc_gcm_init(&gcm, keys.key, 16);
		if (status != 0)
			abort();

		/* Seals the inner plaintext after the header, the tag after the ciphertext. */
		status = cryc_gcm_seal(&gcm, keys.iv, 12, record, 5, data, size, record + 5, record + 5 + size);
		if (status != 0)
			abort();

		/* Wipes the cipher state. */
		cryc_gcm_clear(&gcm);

		/* Opens the record sealed above, so the tag verifies and the inner type is parsed. */
		fuzz_open(&dir, record, 5U + size + 16U);
		sslc_record_clear(&dir);
		return 0;
	}

	/* TLS 1.2: seal what the fuzzer gives (its first byte the type), open it again, compare. */
	if (size == 0U)
		return 0;

	/* Sets up a sealing direction and an opening direction with the same keys. */
	fuzz_keys(&keys, 0xc02b);
	sslc_record_init(&reader);
	status = sslc_record_set_keys(&dir, 0, &keys);
	if (status != 0)
		abort();
	status = sslc_record_set_keys(&reader, 0, &keys);
	if (status != 0)
		abort();

	/* Seals the data when it is not too large, then opens it again and compares. */
	status = -1;
	if (size - 1U <= SSLC_RECORD_PLAINTEXT_MAX)
		status = sslc_record_seal(&dir, 20U + data[0] % 4U, 0, data + 1, size - 1U, record, sizeof(record), &written);
	if (status == 0) {
		memcpy(back, record, written);
		status = sslc_record_open(&reader, back, written, &opened);
		if (status != 0)
			abort();
		if (opened.size != size - 1U)
			abort();
		if (opened.size != 0U) {
			cmp = memcmp(opened.data, data + 1, opened.size);
			if (cmp != 0)
				abort();
		}
	}

	/* Wipes both directions. */
	sslc_record_clear(&dir);
	sslc_record_clear(&reader);

	/* The input never fails the run on its own. */
	return 0;
}
