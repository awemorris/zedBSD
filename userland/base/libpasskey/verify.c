/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libpasskey's verification of an assertion (verify.h; ws161-p004).
 *
 * In order (WebAuthn section 7.2, the steps that do not involve a
 * browser's client data): the credential the answer names must be one the
 * verifier allows; the authenticator data must be well formed, without
 * attested credential data and with an extensions map only when its flag
 * says so; its relying party hash must be the verifier's; the required
 * flags must be set; the signature over the authenticator data and the
 * verifier's client data hash must verify under the stored key; and the
 * signature count must grow unless it and the stored one are both 0.
 */

#include "verify.h"

#include "cbor.h"

#include <string.h>

/* The COSE key's labels and the values ES256 on P-256 needs (RFC 9053). */
#define COSE_KTY		1
#define COSE_ALG		3
#define COSE_CRV		(-1)
#define COSE_X			(-2)
#define COSE_Y			(-3)
#define COSE_KTY_EC2		2
#define COSE_ALG_ES256		(-7)
#define COSE_CRV_P256		1

static int verify_auth_data(const uint8_t *data, size_t size, uint8_t *flags, uint32_t *count);
static uint32_t verify_be32(const uint8_t *bytes);

/*
 * Verifies an answer against what the verifier expects.  Returns 0 with
 * the index of the credential that signed in *matched and the count to
 * store in *sign_count; or EINVAL, or one of the PK_VERIFY_* values.
 */
int
pk_verify_assertion(
	const struct pk_expectation *expectation,
	const struct pk_assertion *assertion,
	size_t *matched,
	uint32_t *sign_count)
{
	const struct pk_credential *credential;
	struct pk_crypto_part parts[2];
	struct pk_crypto_part party;
	uint8_t party_hash[PK_SHA256_SIZE];
	uint8_t signed_hash[PK_SHA256_SIZE];
	uint8_t x[PK_P256_SIZE];
	uint8_t y[PK_P256_SIZE];
	uint32_t count;
	uint8_t flags;
	size_t index;
	int same;
	int error;

	/* Everything is given. */
	if (expectation == NULL || assertion == NULL || matched == NULL || sign_count == NULL)
		return EINVAL;
	if (expectation->rp_id == NULL || assertion->credential_id == NULL || assertion->auth_data == NULL ||
	    assertion->signature == NULL)
		return EINVAL;

	/* The credential the answer names, among the verifier's own. */
	credential = NULL;
	for (index = 0U; index < expectation->credential_count; index++) {
		if (expectation->credentials[index].id_size != assertion->credential_id_size)
			continue;
		same = pk_crypto_equal(expectation->credentials[index].id, assertion->credential_id,
		    assertion->credential_id_size);
		if (same) {
			credential = &expectation->credentials[index];
			break;
		}
	}

	/* A credential that is not one of them. */
	if (credential == NULL)
		return PK_VERIFY_NOT_ALLOWED;

	/* The authenticator data, its flags and its count. */
	error = verify_auth_data(assertion->auth_data, assertion->auth_data_size, &flags, &count);
	if (error != 0)
		return error;

	/* The relying party's hash is the verifier's. */
	party.data = (const uint8_t *)expectation->rp_id;
	party.size = strlen(expectation->rp_id);
	error = pk_crypto_sha256(&party, 1U, party_hash);
	if (error != 0)
		return error;
	same = pk_crypto_equal(party_hash, assertion->auth_data, PK_SHA256_SIZE);
	if (!same)
		return PK_VERIFY_WRONG_PARTY;

	/*
	 * The user was present and verified as the verifier requires: a
	 * login's answer requires both, the unlock of an account that asks no
	 * touch neither (ws199-p001 section 4.3), and a key's silent answer
	 * that only names its owner nothing (section 4.2).
	 */
	if ((flags & expectation->required_flags) != expectation->required_flags)
		return PK_VERIFY_FLAGS;

	/* The stored key. */
	error = pk_cose_p256(credential->cose_key, credential->cose_key_size, x, y);
	if (error != 0)
		return PK_VERIFY_MALFORMED;

	/* The signature over the authenticator data and the verifier's client data hash. */
	parts[0].data = assertion->auth_data;
	parts[0].size = assertion->auth_data_size;
	parts[1].data = expectation->client_data_hash;
	parts[1].size = sizeof(expectation->client_data_hash);
	error = pk_crypto_sha256(parts, 2U, signed_hash);
	if (error != 0)
		return error;
	error = pk_crypto_p256_verify(x, y, signed_hash, assertion->signature, assertion->signature_size);
	if (error == EACCES)
		return PK_VERIFY_SIGNATURE;
	if (error != 0)
		return PK_VERIFY_MALFORMED;

	/* The count grows, unless both it and the stored one are 0 (a key that keeps no count). */
	if ((count != 0U || credential->sign_count != 0U) && count <= credential->sign_count)
		return PK_VERIFY_REPLAY;

	/* Succeeded: the credential that signed, and the count to store. */
	*matched = (size_t)(credential - expectation->credentials);
	*sign_count = count;
	return 0;
}

/*
 * Reads a COSE key that must be ES256 on P-256 (kty 2, alg -7, crv 1, x
 * and y of 32 bytes, nothing else, in canonical order).  Returns 0 with
 * the coordinates, or EBADMSG.
 */
int
pk_cose_p256(
	const uint8_t *cose_key,
	size_t size,
	uint8_t *x,
	uint8_t *y)
{
	int error;

	/* An EC2 key of ES256. */
	error = pk_cose_ec2(cose_key, size, COSE_ALG_ES256, x, y);
	if (error != 0)
		return error;

	/* Succeeded: the coordinates. */
	return 0;
}

/*
 * Reads a COSE EC2 key on P-256 of one algorithm (kty 2, alg, crv 1, x and
 * y of 32 bytes, nothing else, in canonical order): ES256 (-7) for a
 * credential, ECDH-ES+HKDF-256 (-25) for the PIN/UV key agreement.
 * Returns 0 with the coordinates, or EBADMSG.
 */
int
pk_cose_ec2(
	const uint8_t *cose_key,
	size_t size,
	int64_t algorithm,
	uint8_t *x,
	uint8_t *y)
{
	static const int64_t labels[5] = { COSE_KTY, COSE_ALG, COSE_CRV, COSE_X, COSE_Y };
	struct pk_cbor_reader reader;
	struct pk_cbor_item item;
	size_t length;
	unsigned index;
	int64_t label;
	int64_t value;
	int error;

	/* One canonical map that is the whole key. */
	if (cose_key == NULL)
		return EBADMSG;
	error = pk_cbor_check(cose_key, size, PK_CBOR_STRICT_MAPS, &length);
	if (error != 0 || length != size)
		return EBADMSG;
	pk_cbor_reader_init(&reader, cose_key, size);
	error = pk_cbor_read(&reader, &item);
	if (error != 0 || item.kind != PK_CBOR_MAP || item.value != 5U)
		return EBADMSG;

	/* The five pairs, in canonical order: 1, 3, -1, -2, -3. */
	for (index = 0U; index < 5U; index++) {
		/* The label. */
		error = pk_cbor_read(&reader, &item);
		if (error != 0)
			return EBADMSG;
		error = pk_cbor_integer(&item, &label);
		if (error != 0 || label != labels[index])
			return EBADMSG;

		/* Its value: a number for the first three, 32 bytes for the coordinates. */
		error = pk_cbor_read(&reader, &item);
		if (error != 0)
			return EBADMSG;
		if (index >= 3U) {
			if (item.kind != PK_CBOR_BYTES || item.value != PK_P256_SIZE)
				return EBADMSG;
			if (index == 3U)
				memcpy(x, item.bytes, PK_P256_SIZE);
			else
				memcpy(y, item.bytes, PK_P256_SIZE);
			continue;
		}

		/* The numbers: EC2, ES256, P-256. */
		error = pk_cbor_integer(&item, &value);
		if (error != 0)
			return EBADMSG;
		if ((index == 0U && value != COSE_KTY_EC2) ||
		    (index == 1U && value != algorithm) ||
		    (index == 2U && value != COSE_CRV_P256))
			return EBADMSG;
	}

	/* Succeeded: the coordinates. */
	return 0;
}

/*
 * Reads authenticator data of an assertion: the relying party's hash, the
 * flags, the count, no attested credential data, and an extensions map
 * exactly when the flag says so, with nothing after it.  Returns 0 or
 * PK_VERIFY_MALFORMED.
 */
static int
verify_auth_data(
	const uint8_t *data,
	size_t size,
	uint8_t *flags,
	uint32_t *count)
{
	size_t length;
	int error;

	/* The fixed part. */
	if (size < PK_AUTH_DATA_MIN)
		return PK_VERIFY_MALFORMED;
	*flags = data[PK_SHA256_SIZE];
	*count = verify_be32(data + PK_SHA256_SIZE + 1U);

	/* An assertion carries no attested credential data. */
	if ((*flags & PK_FLAG_AT) != 0U)
		return PK_VERIFY_MALFORMED;

	/* Without extensions, nothing follows. */
	if ((*flags & PK_FLAG_ED) == 0U) {
		if (size != PK_AUTH_DATA_MIN)
			return PK_VERIFY_MALFORMED;
		return 0;
	}

	/* With them, one canonical map that is the rest. */
	error = pk_cbor_check(data + PK_AUTH_DATA_MIN, size - PK_AUTH_DATA_MIN, PK_CBOR_STRICT_MAPS, &length);
	if (error != 0 || length != size - PK_AUTH_DATA_MIN || data[PK_AUTH_DATA_MIN] >> 5U != 5U)
		return PK_VERIFY_MALFORMED;

	/* Succeeded: well formed. */
	return 0;
}

/* Reads a 32-bit big-endian number (the signature count). */
static uint32_t
verify_be32(
	const uint8_t *bytes)
{
	/* The most significant byte first. */
	return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) | ((uint32_t)bytes[2] << 8U) |
	    (uint32_t)bytes[3];
}
