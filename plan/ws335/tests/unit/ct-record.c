/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant-time checks of the record protection: the keys, the IVs and
 * the plaintext are marked secret, so memcheck reports any branch or
 * address that depends on their bytes in setting keys, sealing and opening
 * (the search for the TLS 1.3 inner content type included).  The sizes,
 * the sequence number and the record's ciphertext are public, as they are
 * on the wire.  The verdict of an open, the type and size it gives, the
 * sequence number it advances and the plaintext are made public only after
 * the call, since the caller learns them (and sends an alert on a bad
 * tag).
 */

#include "check.h"
#include "ct.h"

#include "userland/base/libssl-compat/sslc-record.h"

/* Folds a public output into one value, to keep the call from being dropped. */
static unsigned
fold(
	const uint8_t *bytes,
	size_t size)
{
	unsigned value;
	size_t index;

	/* Exclusive-or of the bytes, rotated. */
	value = 0;
	for (index = 0; index < size; index++)
		value = ((value << 1) | (value >> 7)) ^ bytes[index];
	return value & 0xffU;
}

/* Runs seal and open for one suite. */
static unsigned
run_case(
	unsigned suite_id,
	unsigned seed)
{
	struct sslc_traffic_keys keys;
	struct sslc_record_dir send;
	struct sslc_record_dir recv;
	struct sslc_record record;
	uint8_t plain[200];
	uint8_t wire[300];
	uint8_t copy[300];
	size_t total;
	size_t index;
	unsigned total_fold;
	int version13;
	int error;

	/* The suite and its keys. */
	total_fold = 0;
	memset(&keys, 0, sizeof(keys));
	keys.suite = sslc_tls_suite((uint16_t)suite_id);
	CHECK(keys.suite != NULL);
	version13 = keys.suite->version == SSLC_TLS13;
	keys.key_size = keys.suite->key_size;
	keys.iv_size = keys.suite->fixed_iv_size;
	for (index = 0; index < keys.key_size; index++)
		keys.key[index] = (uint8_t)(seed + index * 5U);
	for (index = 0; index < keys.iv_size; index++)
		keys.iv[index] = (uint8_t)(seed + index * 3U + 77U);
	for (index = 0; index < sizeof(plain); index++)
		plain[index] = (uint8_t)(seed + index * 7U + (index >> 2));

	/* Setting secret keys in both directions. */
	CT_SECRET(keys.key, keys.key_size);
	CT_SECRET(keys.iv, keys.iv_size);
	sslc_record_init(&send);
	sslc_record_init(&recv);
	error = sslc_record_set_keys(&send, version13, &keys);
	CT_PUBLIC(&error, sizeof(error));
	CHECK(error == 0);
	error = sslc_record_set_keys(&recv, version13, &keys);
	CT_PUBLIC(&error, sizeof(error));
	CHECK(error == 0);
	CT_PUBLIC(keys.key, keys.key_size);
	CT_PUBLIC(keys.iv, keys.iv_size);

	/* Sealing a secret plaintext. */
	CT_SECRET(plain, sizeof(plain));
	error = sslc_record_seal(&send, 23, 0, plain, sizeof(plain), wire, sizeof(wire), &total);
	CT_PUBLIC(&error, sizeof(error));
	CHECK(error == 0);
	CT_PUBLIC(wire, total);
	CT_PUBLIC(plain, sizeof(plain));
	total_fold += fold(wire, total);

	/* Opening the record with the secret keys: a good one. */
	memcpy(copy, wire, total);
	error = sslc_record_open(&recv, copy, total, &record);
	CT_PUBLIC(&error, sizeof(error));
	CHECK(error == 0);
	CT_PUBLIC(&record.type, sizeof(record.type));
	CT_PUBLIC(&record.size, sizeof(record.size));
	CT_PUBLIC(&recv.sequence, sizeof(recv.sequence));
	CHECK(record.type == 23U);
	CHECK(record.size == sizeof(plain));
	CT_PUBLIC(copy, total);
	total_fold += fold(record.data, record.size);

	/* A forged record: the same code runs, the verdict is a bad tag and the sequence number stays. */
	recv.sequence = send.sequence - 1U;
	memcpy(copy, wire, total);
	copy[total - 1U] ^= 0x01;
	error = sslc_record_open(&recv, copy, total, &record);
	CT_PUBLIC(&error, sizeof(error));
	CHECK(error == EBADMSG);
	CT_PUBLIC(&record.type, sizeof(record.type));
	CT_PUBLIC(&record.size, sizeof(record.size));
	CT_PUBLIC(&recv.sequence, sizeof(recv.sequence));
	CT_PUBLIC(copy, total);
	total_fold += fold(copy, total);

	/* The keys are wiped. */
	sslc_record_clear(&send);
	sslc_record_clear(&recv);
	return total_fold;
}

/* Runs the checks. */
int
main(void)
{
	unsigned total;

	/* One suite of each kind: TLS 1.3 with AES-GCM and ChaCha20-Poly1305, TLS 1.2 with the same two. */
	total = run_case(0x1301U, 3U);
	total += run_case(0x1302U, 5U);
	total += run_case(0x1303U, 7U);
	total += run_case(0xc02bU, 11U);
	total += run_case(0xc030U, 13U);
	total += run_case(0xcca8U, 17U);

	/* The folded value is used so that no call can be dropped. */
	CHECK(total < 1000000U);
	return check_finish("ct-record");
}
