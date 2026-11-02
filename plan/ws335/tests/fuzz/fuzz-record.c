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

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* The fixed traffic keys. */
static void
fuzz_keys(
	struct sslc_traffic_keys *keys,
	uint16_t suite_id)
{
	size_t index;

	memset(keys, 0, sizeof(*keys));
	keys->suite = sslc_tls_suite(suite_id);
	keys->key_size = 16;
	for (index = 0; index < 16U; index++)
		keys->key[index] = (uint8_t)(0x10U + index);
	keys->iv_size = keys->suite != NULL ? (size_t)keys->suite->fixed_iv_size : 12U;
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

	if (size < SSLC_RECORD_HEADER)
		return;
	if (sslc_record_header(dir, data, 0, &type, &length) != 0)
		return;
	if (length != size - SSLC_RECORD_HEADER)
		return;
	copy = malloc(size);
	if (copy == NULL)
		return;
	memcpy(copy, data, size);
	if (sslc_record_open(dir, copy, size, &record) == 0 && record.size != 0U)
		(void)(record.data[0] + record.data[record.size - 1U]);
	free(copy);
}

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

	if (size == 0U)
		return 0;
	mode = data[0] % 4U;
	data++;
	size--;
	sslc_record_init(&dir);

	if (mode == 0U) {
		fuzz_open(&dir, data, size);
		return 0;
	}

	if (mode == 1U || mode == 2U) {
		fuzz_keys(&keys, 0x1301);
		if (sslc_record_set_keys(&dir, 1, &keys) != 0)
			abort();
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
		if (cryc_gcm_init(&gcm, keys.key, 16) != 0)
			abort();
		if (cryc_gcm_seal(&gcm, keys.iv, 12, record, 5, data, size, record + 5, record + 5 + size) != 0)
			abort();
		cryc_gcm_clear(&gcm);
		fuzz_open(&dir, record, 5U + size + 16U);
		sslc_record_clear(&dir);
		return 0;
	}

	/* TLS 1.2: seal what the fuzzer gives (its first byte the type), open it again, compare. */
	if (size == 0U)
		return 0;
	fuzz_keys(&keys, 0xc02b);
	sslc_record_init(&reader);
	if (sslc_record_set_keys(&dir, 0, &keys) != 0 || sslc_record_set_keys(&reader, 0, &keys) != 0)
		abort();
	if (size - 1U <= SSLC_RECORD_PLAINTEXT_MAX &&
	    sslc_record_seal(&dir, 20U + data[0] % 4U, 0, data + 1, size - 1U, record, sizeof(record), &written) == 0) {
		memcpy(back, record, written);
		if (sslc_record_open(&reader, back, written, &opened) != 0)
			abort();
		if (opened.size != size - 1U || (opened.size != 0U && memcmp(opened.data, data + 1, opened.size) != 0))
			abort();
	}
	sslc_record_clear(&dir);
	sslc_record_clear(&reader);
	return 0;
}
