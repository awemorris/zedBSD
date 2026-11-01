/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The protection of TLS records.
 */

#include <string.h>

#include "sslc-record.h"

/* The length of the additional data of a TLS 1.2 record: sequence number, type, version and length. */
#define SSLC_RECORD12_AAD	13U

/* The longest TLSInnerPlaintext of TLS 1.3: the content, its type byte and the zero padding together. */
#define SSLC_RECORD13_INNER_MAX	(SSLC_RECORD_PLAINTEXT_MAX + 1U)

/* The record header version of the first ClientHello (5.1 allows it as a compatibility value). */
#define SSLC_RECORD_LEGACY_VERSION_FIRST	0x0301U

/* The record header version of every other record (5.1: legacy_record_version). */
#define SSLC_RECORD_LEGACY_VERSION		0x0303U

/* Tells whether the direction is protected by TLS 1.2 AES-GCM (the only form with an explicit nonce). */
static int sslc_record_is_gcm12(const struct sslc_record_dir *dir);

/* Returns the smallest length field a protected record of the direction may have. */
static size_t sslc_record_protected_min(const struct sslc_record_dir *dir);

/* Makes the 12-byte AEAD nonce of one record from the 8 sequence bytes and, for TLS 1.2 AES-GCM, the 8 explicit bytes. */
static void sslc_record_make_nonce(const struct sslc_record_dir *dir, const uint8_t *sequence_bytes, const uint8_t *explicit_nonce, uint8_t *nonce);

/* Makes the 13-byte additional data of a TLS 1.2 record. */
static void sslc_record_make_aad12(const uint8_t *sequence_bytes, unsigned type, uint32_t version, size_t size, uint8_t *aad);

/* Seals size bytes in place with the direction's AEAD; returns 0 or an error. */
static int sslc_record_aead_seal(const struct sslc_record_dir *dir, const uint8_t *nonce, const uint8_t *aad, size_t aad_size, uint8_t *data, size_t size, uint8_t *tag);

/* Opens size bytes in place with the direction's AEAD; returns 0 or EBADMSG without a branch on the tag's result. */
static int sslc_record_aead_open(const struct sslc_record_dir *dir, const uint8_t *nonce, const uint8_t *aad, size_t aad_size, uint8_t *data, size_t size, const uint8_t *tag);

/* Returns first when it is not 0 and second otherwise, without a branch. */
static uint32_t sslc_record_first_error(uint32_t first, uint32_t second);

/*
 * Makes a direction without keys (plaintext records, sequence 0).
 */
void
sslc_record_init(
	struct sslc_record_dir *dir)
{
	/* No suite, no keys, sequence 0: all zero bytes. */
	memset(dir, 0, sizeof(*dir));
}

/*
 * Sets new keys (wiping the old) and resets the sequence number to 0.
 *
 * The new state is made on the side and replaces the old one only when
 * everything was accepted, so a refused call leaves the direction as it
 * was.
 */
int
sslc_record_set_keys(
	struct sslc_record_dir *dir,
	int version13,
	const struct sslc_traffic_keys *keys)
{
	struct sslc_record_dir next;
	const struct sslc_suite *suite;
	uint16_t version;
	int error;

	/* A suite is required: plaintext is made by sslc_record_clear. */
	suite = keys->suite;
	if (suite == NULL)
		return EINVAL;

	/* The suite must carry an AEAD. */
	if (suite->aead == SSLC_AEAD_NONE)
		return EINVAL;

	/* The record format and the suite's protocol version must agree. */
	version = version13 != 0 ? SSLC_TLS13 : SSLC_TLS12;
	if (suite->version != version)
		return EINVAL;

	/* The key and IV sizes must be the suite's, and must fit the structure. */
	if (keys->key_size != (size_t)suite->key_size)
		return EINVAL;
	if (keys->iv_size != (size_t)suite->fixed_iv_size)
		return EINVAL;
	if (keys->key_size > sizeof(keys->key))
		return EINVAL;
	if (keys->iv_size > sizeof(keys->iv))
		return EINVAL;

	/* The new direction: the suite, the format, the IV and the expanded key. */
	sslc_record_init(&next);
	next.suite = suite;
	next.version13 = version13 != 0 ? 1 : 0;
	memcpy(next.iv, keys->iv, keys->iv_size);
	next.iv_size = keys->iv_size;
	error = 0;
	if (suite->aead == SSLC_AEAD_CHACHA20_POLY1305) {
		if (keys->key_size != sizeof(next.chacha_key))
			error = EINVAL;
		else
			memcpy(next.chacha_key, keys->key, keys->key_size);
	} else {
		if (cryc_gcm_init(&next.gcm, keys->key, keys->key_size) != 0)
			error = EINVAL;
	}
	if (error != 0) {
		cryc_wipe(&next, sizeof(next));
		return error;
	}

	/* Wipes the old keys and takes the new state, with sequence 0. */
	sslc_record_clear(dir);
	memcpy(dir, &next, sizeof(*dir));
	cryc_wipe(&next, sizeof(next));
	return 0;
}

/*
 * Wipes the direction's keys and makes it plaintext again.
 */
void
sslc_record_clear(
	struct sslc_record_dir *dir)
{
	/* The keys first, in a way the compiler keeps, then the plain state. */
	cryc_wipe(dir, sizeof(*dir));
	sslc_record_init(dir);
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
	unsigned header_type;
	uint32_t header_version;
	size_t header_length;
	size_t maximum;
	size_t minimum;

	/* The three fields. */
	header_type = header[0];
	header_version = cryc_load_be16(header + 1);
	header_length = (size_t)cryc_load_be16(header + 3);

	/* The type must be one of the four. */
	if (header_type < SSLC_CT_CHANGE_CIPHER_SPEC)
		return EPROTO;
	if (header_type > SSLC_CT_APPLICATION_DATA)
		return EPROTO;

	/* The version: the one asked for, or any of 0x0301 to 0x0303 when none is. */
	if (version_check != 0) {
		if (header_version != (uint32_t)version_check)
			return EPROTO;
	} else {
		if (header_version < 0x0301U)
			return EPROTO;
		if (header_version > SSLC_TLS12)
			return EPROTO;
	}

	/* The largest length the direction accepts, and the smallest. */
	if (dir->suite == NULL) {
		maximum = SSLC_RECORD_PLAINTEXT_MAX;
		minimum = 1;
	} else if (dir->version13 != 0) {
		maximum = SSLC_RECORD13_CIPHERTEXT_MAX;
		minimum = sslc_record_protected_min(dir);
	} else {
		maximum = SSLC_RECORD12_CIPHERTEXT_MAX;
		minimum = sslc_record_protected_min(dir);
	}

	/* A TLS 1.3 compatibility change_cipher_spec record is not protected and is exactly one byte long (5). */
	if (dir->suite != NULL) {
		if (dir->version13 != 0) {
			if (header_type == SSLC_CT_CHANGE_CIPHER_SPEC) {
				if (header_length != 1U)
					return EPROTO;
				minimum = 1;
			}
		}
	}

	/* Too long is a record_overflow. */
	if (header_length > maximum)
		return EMSGSIZE;

	/* Too short: an empty plaintext record is a decode_error, a short protected one a bad_record_mac. */
	if (header_length < minimum) {
		if (dir->suite == NULL)
			return EPROTO;
		return EBADMSG;
	}

	/* The accepted header. */
	*type = header_type;
	*length = header_length;
	return 0;
}

/*
 * Returns the bytes of a whole protected record (header, nonce, payload,
 * tag) that carries size bytes of plaintext on this direction (for TLS 1.3
 * one byte more for the inner content type, no padding).
 *
 * A size over SSLC_RECORD_PLAINTEXT_MAX gives 0 (no such record exists).
 */
size_t
sslc_record_overhead(
	const struct sslc_record_dir *dir,
	size_t size)
{
	size_t total;

	/* No record carries more than a full fragment. */
	if (size > SSLC_RECORD_PLAINTEXT_MAX)
		return 0;

	/* The header and the payload. */
	total = SSLC_RECORD_HEADER + size;

	/* Plaintext adds nothing more. */
	if (dir->suite == NULL)
		return total;

	/* The tag, the inner content type of TLS 1.3, or the explicit nonce of TLS 1.2 AES-GCM. */
	total += SSLC_RECORD_TAG;
	if (dir->version13 != 0)
		total += 1;
	if (sslc_record_is_gcm12(dir) != 0)
		total += SSLC_RECORD12_EXPLICIT;
	return total;
}

/*
 * Writes one record carrying size bytes of type (at most
 * SSLC_RECORD_PLAINTEXT_MAX) into out (room bytes, at least
 * sslc_record_overhead(dir, size)).
 *
 * A type other than the four content types, or a NULL in with a size over
 * 0, is EINVAL.  Plaintext directions have no sequence number: it stays 0
 * and never overflows.
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
	uint8_t sequence_bytes[8];
	uint8_t nonce[12];
	uint8_t aad[SSLC_RECORD12_AAD];
	size_t total;
	size_t payload;
	size_t body;
	size_t explicit_size;
	uint32_t version;
	int error;

	/* The caller's mistakes: a bad type, a fragment too large, no bytes to read. */
	if (type < SSLC_CT_CHANGE_CIPHER_SPEC)
		return EINVAL;
	if (type > SSLC_CT_APPLICATION_DATA)
		return EINVAL;
	if (size > SSLC_RECORD_PLAINTEXT_MAX)
		return EINVAL;
	if (in == NULL && size != 0)
		return EINVAL;

	/* The room the whole record needs. */
	total = sslc_record_overhead(dir, size);
	if (room < total)
		return ENOBUFS;

	/* A plaintext record: header and the bytes. */
	if (dir->suite == NULL) {
		version = SSLC_RECORD_LEGACY_VERSION;
		if (first_hello != 0)
			version = SSLC_RECORD_LEGACY_VERSION_FIRST;
		out[0] = (uint8_t)type;
		cryc_store_be16(out + 1, version);
		cryc_store_be16(out + 3, (uint32_t)size);
		if (size > 0)
			memcpy(out + SSLC_RECORD_HEADER, in, size);
		*written = total;
		return 0;
	}

	/* A sequence number about to wrap ends the connection's use of the keys (5.3). */
	if (dir->sequence == (uint64_t)0 - (uint64_t)1)
		return EOVERFLOW;
	cryc_store_be64(sequence_bytes, dir->sequence);

	/* The explicit nonce (TLS 1.2 AES-GCM only) goes between the header and the payload. */
	explicit_size = 0;
	if (sslc_record_is_gcm12(dir) != 0)
		explicit_size = SSLC_RECORD12_EXPLICIT;

	/* The payload: the plaintext, and for TLS 1.3 its content type; the tag follows. */
	payload = size;
	if (dir->version13 != 0)
		payload = size + 1;
	body = explicit_size + payload + SSLC_RECORD_TAG;

	/* The header: TLS 1.3 shows application_data, TLS 1.2 the real type. */
	if (dir->version13 != 0)
		out[0] = (uint8_t)SSLC_CT_APPLICATION_DATA;
	else
		out[0] = (uint8_t)type;
	cryc_store_be16(out + 1, SSLC_RECORD_LEGACY_VERSION);
	cryc_store_be16(out + 3, (uint32_t)body);

	/* The explicit nonce is the sequence number, which never repeats on a key. */
	if (explicit_size > 0)
		memcpy(out + SSLC_RECORD_HEADER, sequence_bytes, explicit_size);

	/* The plaintext, and the inner content type after it in TLS 1.3. */
	if (size > 0)
		memcpy(out + SSLC_RECORD_HEADER + explicit_size, in, size);
	if (dir->version13 != 0)
		out[SSLC_RECORD_HEADER + size] = (uint8_t)type;

	/* The nonce and the additional data (the header itself in TLS 1.3). */
	sslc_record_make_nonce(dir, sequence_bytes, sequence_bytes, nonce);
	if (dir->version13 != 0)
		memcpy(aad, out, SSLC_RECORD_HEADER);
	else
		sslc_record_make_aad12(sequence_bytes, type, SSLC_RECORD_LEGACY_VERSION, size, aad);

	/* The payload is encrypted where it stands and the tag written after it. */
	error = sslc_record_aead_seal(dir, nonce, aad, dir->version13 != 0 ? SSLC_RECORD_HEADER : SSLC_RECORD12_AAD,
	    out + SSLC_RECORD_HEADER + explicit_size, payload, out + SSLC_RECORD_HEADER + explicit_size + payload);
	cryc_wipe(nonce, sizeof(nonce));
	if (error != 0)
		return EINVAL;

	/* The next record uses the next sequence number. */
	dir->sequence++;
	*written = total;
	return 0;
}

/*
 * Opens one whole record (its 5-byte header, already checked by
 * sslc_record_header, followed by length bytes) in place.
 *
 * size is the whole record, header included.  On TLS 1.3 a change_cipher_spec
 * record must be one byte of value 1 (5): it is given back as type 20 and the
 * caller drops it; any other length or value, like a protected one, is
 * EPROTO.  A size that is not the header's length field plus 5 is EPROTO.
 * After the tag is checked, the
 * work that depends on the plaintext (the search for the inner content
 * type, the verdict and the advance of the sequence number) is done
 * without a branch on it, so the time does not depend on the padding.
 * After a failure that came from the AEAD or the lengths, out->type and
 * out->size are 0 and out->data is not to be used.
 */
int
sslc_record_open(
	struct sslc_record_dir *dir,
	uint8_t *record,
	size_t size,
	struct sslc_record *out)
{
	uint8_t sequence_bytes[8];
	uint8_t nonce[12];
	uint8_t aad[SSLC_RECORD12_AAD];
	uint8_t *payload;
	const uint8_t *tag;
	size_t length;
	size_t explicit_size;
	size_t plain_size;
	size_t index;
	uint32_t status;
	uint32_t tag_error;
	uint32_t size_error;
	uint32_t found;
	uint32_t found_index;
	uint32_t found_type;
	uint32_t not_zero;
	uint32_t ok;
	unsigned outer;
	int result;

	/* The result is empty until the record is accepted. */
	out->type = 0;
	out->data = NULL;
	out->size = 0;

	/* The header's length field must describe exactly these bytes. */
	if (size < SSLC_RECORD_HEADER)
		return EPROTO;
	length = size - SSLC_RECORD_HEADER;
	if (length != (size_t)cryc_load_be16(record + 3))
		return EPROTO;
	outer = record[0];

	/* A plaintext direction: the record is its plaintext. */
	if (dir->suite == NULL) {
		if (length == 0)
			return EPROTO;
		if (length > SSLC_RECORD_PLAINTEXT_MAX)
			return EMSGSIZE;
		out->type = outer;
		out->data = record + SSLC_RECORD_HEADER;
		out->size = length;
		return 0;
	}

	/* The record must fit the direction's largest protected record. */
	if (dir->version13 != 0) {
		if (length > SSLC_RECORD13_CIPHERTEXT_MAX)
			return EMSGSIZE;
	} else {
		if (length > SSLC_RECORD12_CIPHERTEXT_MAX)
			return EMSGSIZE;
	}

	/* TLS 1.3: a compatibility change_cipher_spec record is unprotected, one byte of value 1, and given back as it is (5, D.4). */
	if (dir->version13 != 0) {
		if (outer == SSLC_CT_CHANGE_CIPHER_SPEC) {
			if (length != 1U)
				return EPROTO;
			if (record[SSLC_RECORD_HEADER] != 1)
				return EPROTO;
			out->type = outer;
			out->data = record + SSLC_RECORD_HEADER;
			out->size = length;
			return 0;
		}

		/* Every other protected TLS 1.3 record is application_data on the wire (5.2). */
		if (outer != SSLC_CT_APPLICATION_DATA)
			return EPROTO;
	}

	/* Too short to hold the nonce, the tag and the inner type. */
	if (length < sslc_record_protected_min(dir))
		return EBADMSG;

	/* The sequence number must not wrap (5.3). */
	if (dir->sequence == (uint64_t)0 - (uint64_t)1)
		return EOVERFLOW;
	cryc_store_be64(sequence_bytes, dir->sequence);

	/* The explicit nonce (TLS 1.2 AES-GCM) follows the header; the payload, then the tag, after it. */
	explicit_size = 0;
	if (sslc_record_is_gcm12(dir) != 0)
		explicit_size = SSLC_RECORD12_EXPLICIT;
	payload = record + SSLC_RECORD_HEADER + explicit_size;
	plain_size = length - explicit_size - SSLC_RECORD_TAG;
	tag = payload + plain_size;

	/* The nonce and the additional data (TLS 1.2's uses the plaintext length). */
	sslc_record_make_nonce(dir, sequence_bytes, record + SSLC_RECORD_HEADER, nonce);
	if (dir->version13 != 0)
		memcpy(aad, record, SSLC_RECORD_HEADER);
	else
		sslc_record_make_aad12(sequence_bytes, outer, cryc_load_be16(record + 1), plain_size, aad);

	/* The tag is checked and the plaintext released (zeroed on a bad tag), in place. */
	result = sslc_record_aead_open(dir, nonce, aad, dir->version13 != 0 ? SSLC_RECORD_HEADER : SSLC_RECORD12_AAD,
	    payload, plain_size, tag);
	cryc_wipe(nonce, sizeof(nonce));
	tag_error = (uint32_t)result;

	/* The length rules, on public lengths: TLS 1.3's inner plaintext and TLS 1.2's plaintext. */
	size_error = 0;
	if (dir->version13 != 0) {
		if (plain_size > SSLC_RECORD13_INNER_MAX)
			size_error = (uint32_t)EMSGSIZE;
	} else {
		if (plain_size > SSLC_RECORD_PLAINTEXT_MAX)
			size_error = (uint32_t)EMSGSIZE;
	}

	/* TLS 1.3: the inner content type is the last byte that is not zero, found by looking at every byte. */
	found = 0;
	found_index = 0;
	found_type = 0;
	if (dir->version13 != 0) {
		for (index = 0; index < plain_size; index++) {
			not_zero = ~cryc_ct_mask_zero32((uint32_t)payload[index]);
			found |= not_zero;
			found_index = (found_index & ~not_zero) | ((uint32_t)index & not_zero);
			found_type = (found_type & ~not_zero) | ((uint32_t)payload[index] & not_zero);
		}
	}

	/* The verdict: a bad tag first, then a length error, then (TLS 1.3) an inner plaintext of only zeros (5.4). */
	status = size_error;
	if (dir->version13 != 0)
		status = sslc_record_first_error(size_error, (uint32_t)EPROTO & ~found);
	status = sslc_record_first_error(tag_error, status);
	ok = cryc_ct_mask_zero32(status);

	/* The plaintext is given back only when everything was accepted. */
	out->data = payload;
	if (dir->version13 != 0) {
		out->type = (unsigned)(found_type & ok);
		out->size = (size_t)(found_index & ok);
	} else {
		out->type = (unsigned)((uint32_t)outer & ok);
		out->size = (size_t)((uint32_t)plain_size & ok);
	}

	/* The sequence number advances after a record that was accepted. */
	dir->sequence += (uint64_t)(ok & 1U);
	return (int)status;
}

/*
 * Tells whether the direction is protected by TLS 1.2 AES-GCM (the only form
 * with an explicit nonce).
 */
static int
sslc_record_is_gcm12(
	const struct sslc_record_dir *dir)
{
	/* A direction without keys has no AEAD. */
	if (dir->suite == NULL)
		return 0;

	/* TLS 1.3 derives the whole nonce from the IV. */
	if (dir->version13 != 0)
		return 0;

	/* ChaCha20-Poly1305 does the same in TLS 1.2 (RFC 7905 2). */
	if (dir->suite->aead == SSLC_AEAD_CHACHA20_POLY1305)
		return 0;
	return 1;
}

/*
 * Returns the smallest length field a protected record of the direction may
 * have: the tag, the inner content type (TLS 1.3) or the explicit nonce
 * (TLS 1.2 AES-GCM).
 */
static size_t
sslc_record_protected_min(
	const struct sslc_record_dir *dir)
{
	size_t minimum;

	/* Every protected record carries a tag. */
	minimum = SSLC_RECORD_TAG;

	/* TLS 1.3 adds the inner content type, TLS 1.2 AES-GCM the explicit nonce. */
	if (dir->version13 != 0)
		minimum += 1;
	if (sslc_record_is_gcm12(dir) != 0)
		minimum += SSLC_RECORD12_EXPLICIT;
	return minimum;
}

/*
 * Makes the 12-byte AEAD nonce of one record from the 8 sequence bytes and,
 * for TLS 1.2 AES-GCM, the 8 explicit bytes.
 *
 * TLS 1.3 and TLS 1.2 ChaCha20-Poly1305: the IV exclusive-ored with the
 * sequence number left-padded with zeros (RFC 8446 5.3, RFC 7905 2).  TLS
 * 1.2 AES-GCM: the 4-byte salt followed by the explicit bytes (RFC 5288 3).
 */
static void
sslc_record_make_nonce(
	const struct sslc_record_dir *dir,
	const uint8_t *sequence_bytes,
	const uint8_t *explicit_nonce,
	uint8_t *nonce)
{
	size_t index;

	/* The salt and the explicit bytes. */
	if (sslc_record_is_gcm12(dir) != 0) {
		memcpy(nonce, dir->iv, 4);
		memcpy(nonce + 4, explicit_nonce, SSLC_RECORD12_EXPLICIT);
		return;
	}

	/* The IV, with the sequence number exclusive-ored into its last 8 bytes. */
	memcpy(nonce, dir->iv, 12);
	for (index = 0; index < 8U; index++)
		nonce[4U + index] ^= sequence_bytes[index];
}

/*
 * Makes the 13-byte additional data of a TLS 1.2 record: the sequence
 * number, the type, the version of the record header and the length of the
 * plaintext (RFC 5246 6.2.3.3).
 */
static void
sslc_record_make_aad12(
	const uint8_t *sequence_bytes,
	unsigned type,
	uint32_t version,
	size_t size,
	uint8_t *aad)
{
	/* The sequence number, type, version and plaintext length, in this order. */
	memcpy(aad, sequence_bytes, 8);
	aad[8] = (uint8_t)type;
	cryc_store_be16(aad + 9, version);
	cryc_store_be16(aad + 11, (uint32_t)size);
}

/*
 * Seals size bytes in place with the direction's AEAD.
 */
static int
sslc_record_aead_seal(
	const struct sslc_record_dir *dir,
	const uint8_t *nonce,
	const uint8_t *aad,
	size_t aad_size,
	uint8_t *data,
	size_t size,
	uint8_t *tag)
{
	/* ChaCha20-Poly1305 has its own key. */
	if (dir->suite->aead == SSLC_AEAD_CHACHA20_POLY1305)
		return cryc_chacha20_poly1305_seal(dir->chacha_key, nonce, aad, aad_size, data, size, data, tag);

	/* AES-GCM with the 12-byte nonce. */
	return cryc_gcm_seal(&dir->gcm, nonce, CRYC_GCM_IV, aad, aad_size, data, size, data, tag);
}

/*
 * Opens size bytes in place with the direction's AEAD.  The result is 0 or
 * EBADMSG, made without a branch on whether the tag was good.
 */
static int
sslc_record_aead_open(
	const struct sslc_record_dir *dir,
	const uint8_t *nonce,
	const uint8_t *aad,
	size_t aad_size,
	uint8_t *data,
	size_t size,
	const uint8_t *tag)
{
	uint32_t error;

	/* ChaCha20-Poly1305 has its own key; AES-GCM uses the 12-byte nonce. */
	if (dir->suite->aead == SSLC_AEAD_CHACHA20_POLY1305)
		error = (uint32_t)cryc_chacha20_poly1305_open(dir->chacha_key, nonce, aad, aad_size, data, size, data, tag);
	else
		error = (uint32_t)cryc_gcm_open(&dir->gcm, nonce, CRYC_GCM_IV, aad, aad_size, data, size, data, tag);

	/* Whatever the cause, the answer is bad_record_mac's EBADMSG (or 0). */
	return (int)((uint32_t)EBADMSG & ~cryc_ct_mask_zero32(error));
}

/*
 * Returns first when it is not 0 and second otherwise, without a branch.
 */
static uint32_t
sslc_record_first_error(
	uint32_t first,
	uint32_t second)
{
	uint32_t first_is_error;

	/* All ones when first is an error. */
	first_is_error = ~cryc_ct_mask_zero32(first);

	/* The chosen one. */
	return (first & first_is_error) | (second & ~first_is_error);
}
