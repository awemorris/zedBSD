/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The protection of TLS records.
 */

#include "sslc-record.h"

/*
 * Makes a direction without keys (plaintext records, sequence 0).
 */
void
sslc_record_init(
	struct sslc_record_dir *dir)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(dir);
}

/*
 * Sets new keys (wiping the old) and resets the sequence number to 0.
 */
int
sslc_record_set_keys(
	struct sslc_record_dir *dir,
	int version13,
	const struct sslc_traffic_keys *keys)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(dir);
	UNUSED_PARAMETER(version13);
	UNUSED_PARAMETER(keys);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Wipes the direction's keys and makes it plaintext again.
 */
void
sslc_record_clear(
	struct sslc_record_dir *dir)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(dir);
}

/*
 * Parses a record header (5.1; RFC 5246 6.2.1): type, legacy version,
 * length.
 */
int
sslc_record_header(
	const struct sslc_record_dir *dir,
	const uint8_t *header,
	uint16_t version_check,
	unsigned *type,
	size_t *length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(dir);
	UNUSED_PARAMETER(header);
	UNUSED_PARAMETER(version_check);
	UNUSED_PARAMETER(type);
	UNUSED_PARAMETER(length);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Returns the bytes of a whole protected record (header, nonce, payload,
 * tag) that carries size bytes of plaintext on this direction (for TLS 1.3
 * one byte more for the inner content type, no padding).
 */
size_t
sslc_record_overhead(
	const struct sslc_record_dir *dir,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(dir);
	UNUSED_PARAMETER(size);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Writes one record carrying size bytes of type (at most
 * SSLC_RECORD_PLAINTEXT_MAX) into out (room bytes, at least
 * sslc_record_overhead(dir, size)).
 */
int
sslc_record_seal(
	struct sslc_record_dir *dir,
	unsigned type,
	int first_hello,
	const uint8_t *in,
	size_t size,
	uint8_t *out,
	size_t room,
	size_t *written)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(dir);
	UNUSED_PARAMETER(type);
	UNUSED_PARAMETER(first_hello);
	UNUSED_PARAMETER(in);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(room);
	UNUSED_PARAMETER(written);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Opens one whole record (its 5-byte header, already checked by
 * sslc_record_header, followed by length bytes) in place.
 */
int
sslc_record_open(
	struct sslc_record_dir *dir,
	uint8_t *record,
	size_t size,
	struct sslc_record *out)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(dir);
	UNUSED_PARAMETER(record);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(out);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}
