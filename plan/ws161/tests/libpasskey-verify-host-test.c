/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of libpasskey's verification (ws161-p004,
 * userland/base/libpasskey/verify.c): a P-256 key made here signs
 * authenticator data the way a security key does, and the verifier takes
 * the good answer and refuses the bad ones: a credential not allowed,
 * changed authenticator data, another relying party, no user presence, no
 * user verification when required, a signature of other data, a count that
 * did not grow, attested credential data in an assertion, trailing bytes, a
 * stored key that is not ES256.  A high-S signature (the same signature
 * with s replaced by n - s) is accepted, as plain ECDSA does.
 */

#include "userland/base/libpasskey/cbor.h"
#include "userland/base/libpasskey/verify.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>

#include <stdio.h>
#include <string.h>

/* The checks that failed. */
static unsigned failures;

/* The test key, its COSE form, and the client data hash. */
static EVP_PKEY *key;
static uint8_t cose[128];
static size_t cose_size;
static uint8_t client_hash[32];
static const uint8_t credential_id[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

/* Counts and reports a check that does not hold. */
static void
expect(
	int condition,
	const char *what)
{
	if (condition)
		return;
	failures++;
	printf("FAIL: %s\n", what);
}

/* Builds authenticator data: the hash of rp, flags, count, and extra bytes. */
static size_t
make_auth_data(
	uint8_t *data,
	const char *rp,
	uint8_t flags,
	uint32_t count,
	const uint8_t *extra,
	size_t extra_size)
{
	struct pk_crypto_part part;

	part.data = (const uint8_t *)rp;
	part.size = strlen(rp);
	(void)pk_crypto_sha256(&part, 1U, data);
	data[32] = flags;
	data[33] = (uint8_t)(count >> 24);
	data[34] = (uint8_t)(count >> 16);
	data[35] = (uint8_t)(count >> 8);
	data[36] = (uint8_t)count;
	if (extra_size != 0U)
		memcpy(data + 37, extra, extra_size);
	return 37U + extra_size;
}

/* Signs authenticator data and a client data hash with the test key (DER). */
static size_t
sign(
	const uint8_t *data,
	size_t size,
	const uint8_t *hash,
	uint8_t *signature)
{
	EVP_MD_CTX *context;
	size_t length;

	context = EVP_MD_CTX_new();
	length = 80U;
	(void)EVP_DigestSignInit(context, NULL, EVP_sha256(), NULL, key);
	(void)EVP_DigestSignUpdate(context, data, size);
	(void)EVP_DigestSignUpdate(context, hash, 32U);
	(void)EVP_DigestSignFinal(context, signature, &length);
	EVP_MD_CTX_free(context);
	return length;
}

/* Replaces s by n - s in a DER signature. */
static size_t
high_s(
	const uint8_t *signature,
	size_t size,
	uint8_t *out)
{
	const unsigned char *in;
	unsigned char *write;
	ECDSA_SIG *parsed;
	const BIGNUM *r;
	const BIGNUM *s;
	BIGNUM *order;
	BIGNUM *flipped;
	int length;

	in = signature;
	parsed = d2i_ECDSA_SIG(NULL, &in, (long)size);
	ECDSA_SIG_get0(parsed, &r, &s);
	order = NULL;
	(void)BN_hex2bn(&order, "FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551");
	flipped = BN_new();
	(void)BN_sub(flipped, order, s);
	(void)ECDSA_SIG_set0(parsed, BN_dup(r), flipped);
	write = out;
	length = i2d_ECDSA_SIG(parsed, &write);
	ECDSA_SIG_free(parsed);
	BN_free(order);
	return (size_t)length;
}

/* Verifies one answer against the test credential; returns what the verifier said. */
static int
check(
	const char *rp,
	unsigned required,
	uint32_t stored,
	const uint8_t *id,
	const uint8_t *data,
	size_t data_size,
	const uint8_t *signature,
	size_t signature_size,
	uint32_t *count)
{
	struct pk_expectation expectation;
	struct pk_assertion assertion;
	struct pk_credential credential;
	size_t matched;

	credential.id = credential_id;
	credential.id_size = sizeof(credential_id);
	credential.cose_key = cose;
	credential.cose_key_size = cose_size;
	credential.sign_count = stored;
	memset(&expectation, 0, sizeof(expectation));
	expectation.rp_id = rp;
	memcpy(expectation.client_data_hash, client_hash, 32U);
	expectation.required_flags = required;
	expectation.credentials = &credential;
	expectation.credential_count = 1U;
	assertion.credential_id = id;
	assertion.credential_id_size = 16U;
	assertion.auth_data = data;
	assertion.auth_data_size = data_size;
	assertion.signature = signature;
	assertion.signature_size = signature_size;
	matched = 99U;
	return pk_verify_assertion(&expectation, &assertion, &matched, count);
}

int
main(void)
{
	struct pk_cbor_writer writer;
	uint8_t point[65];
	uint8_t data[128];
	uint8_t other[128];
	uint8_t signature[80];
	uint8_t flipped[80];
	uint8_t wrong_id[16];
	uint8_t x[32];
	uint8_t y[32];
	static const uint8_t extensions[] = { 0xa1, 0x61, 0x61, 0x01 };
	size_t point_size;
	size_t data_size;
	size_t other_size;
	size_t signature_size;
	size_t flipped_size;
	uint32_t count;
	int error;

	/* The key, and its COSE form. */
	key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
	point_size = 0U;
	(void)EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point), &point_size);
	expect(point_size == 65U, "the key's point");
	pk_cbor_writer_init(&writer, cose, sizeof(cose));
	pk_cbor_put_map(&writer, 5U);
	pk_cbor_put_integer(&writer, 1);
	pk_cbor_put_integer(&writer, 2);
	pk_cbor_put_integer(&writer, 3);
	pk_cbor_put_integer(&writer, -7);
	pk_cbor_put_integer(&writer, -1);
	pk_cbor_put_integer(&writer, 1);
	pk_cbor_put_integer(&writer, -2);
	pk_cbor_put_bytes(&writer, point + 1, 32U);
	pk_cbor_put_integer(&writer, -3);
	pk_cbor_put_bytes(&writer, point + 33, 32U);
	cose_size = writer.length;
	error = pk_cose_p256(cose, cose_size, x, y);
	expect(error == 0 && memcmp(x, point + 1, 32U) == 0 && memcmp(y, point + 33, 32U) == 0, "COSE key read back");
	memset(client_hash, 0x5a, sizeof(client_hash));

	/* A good answer: UP and UV, count 7 over a stored 6. */
	data_size = make_auth_data(data, "zedbsd.login", PK_FLAG_UP | PK_FLAG_UV, 7U, NULL, 0U);
	signature_size = sign(data, data_size, client_hash, signature);
	count = 0U;
	error = check("zedbsd.login", PK_FLAG_UV, 6U, credential_id, data, data_size, signature, signature_size, &count);
	expect(error == 0 && count == 7U, "good answer");

	/* A key that keeps no count: 0 over a stored 0. */
	data_size = make_auth_data(data, "zedbsd.login", PK_FLAG_UP | PK_FLAG_UV, 0U, NULL, 0U);
	signature_size = sign(data, data_size, client_hash, signature);
	error = check("zedbsd.login", PK_FLAG_UV, 0U, credential_id, data, data_size, signature, signature_size, &count);
	expect(error == 0 && count == 0U, "count 0 over 0");

	/* The same signature, high-S. */
	flipped_size = high_s(signature, signature_size, flipped);
	error = check("zedbsd.login", PK_FLAG_UV, 0U, credential_id, data, data_size, flipped, flipped_size, &count);
	expect(error == 0, "high-S accepted");

	/* A count that did not grow, and 0 under a stored count. */
	data_size = make_auth_data(data, "zedbsd.login", PK_FLAG_UP | PK_FLAG_UV, 6U, NULL, 0U);
	signature_size = sign(data, data_size, client_hash, signature);
	error = check("zedbsd.login", PK_FLAG_UV, 6U, credential_id, data, data_size, signature, signature_size, &count);
	expect(error == PK_VERIFY_REPLAY, "count did not grow");
	data_size = make_auth_data(data, "zedbsd.login", PK_FLAG_UP | PK_FLAG_UV, 0U, NULL, 0U);
	signature_size = sign(data, data_size, client_hash, signature);
	error = check("zedbsd.login", PK_FLAG_UV, 6U, credential_id, data, data_size, signature, signature_size, &count);
	expect(error == PK_VERIFY_REPLAY, "count 0 under a stored count");

	/* A credential the verifier does not allow. */
	data_size = make_auth_data(data, "zedbsd.login", PK_FLAG_UP | PK_FLAG_UV, 9U, NULL, 0U);
	signature_size = sign(data, data_size, client_hash, signature);
	memcpy(wrong_id, credential_id, sizeof(wrong_id));
	wrong_id[15] ^= 1U;
	error = check("zedbsd.login", PK_FLAG_UV, 0U, wrong_id, data, data_size, signature, signature_size, &count);
	expect(error == PK_VERIFY_NOT_ALLOWED, "credential not allowed");

	/* Changed authenticator data (the count), and another relying party. */
	memcpy(other, data, data_size);
	other[36] ^= 1U;
	error = check("zedbsd.login", PK_FLAG_UV, 0U, credential_id, other, data_size, signature, signature_size, &count);
	expect(error == PK_VERIFY_SIGNATURE, "changed authenticator data");
	other_size = make_auth_data(other, "example.com", PK_FLAG_UP | PK_FLAG_UV, 9U, NULL, 0U);
	signature_size = sign(other, other_size, client_hash, signature);
	error = check("zedbsd.login", PK_FLAG_UV, 0U, credential_id, other, other_size, signature, signature_size, &count);
	expect(error == PK_VERIFY_WRONG_PARTY, "another relying party");

	/* No presence; no verification when it is required (and accepted when it is not). */
	other_size = make_auth_data(other, "zedbsd.login", PK_FLAG_UV, 9U, NULL, 0U);
	signature_size = sign(other, other_size, client_hash, signature);
	error = check("zedbsd.login", PK_FLAG_UP | PK_FLAG_UV, 0U, credential_id, other, other_size, signature, signature_size, &count);
	expect(error == PK_VERIFY_FLAGS, "no presence when it is required");
	error = check("zedbsd.login", PK_FLAG_UV, 0U, credential_id, other, other_size, signature, signature_size, &count);
	expect(error == 0, "no presence when it is not required (an unlock without the touch, ws199)");
	error = check("zedbsd.login", 0U, 0U, credential_id, other, other_size, signature, signature_size, &count);
	expect(error == 0, "nothing required (a key's owner, ws199)");
	other_size = make_auth_data(other, "zedbsd.login", PK_FLAG_UP, 9U, NULL, 0U);
	signature_size = sign(other, other_size, client_hash, signature);
	error = check("zedbsd.login", PK_FLAG_UV, 0U, credential_id, other, other_size, signature, signature_size, &count);
	expect(error == PK_VERIFY_FLAGS, "no verification when required");
	error = check("zedbsd.login", 0U, 0U, credential_id, other, other_size, signature, signature_size, &count);
	expect(error == 0, "presence alone when verification is not required");

	/* A signature over another client data hash. */
	client_hash[0] ^= 1U;
	signature_size = sign(data, data_size, client_hash, signature);
	client_hash[0] ^= 1U;
	error = check("zedbsd.login", PK_FLAG_UV, 0U, credential_id, data, data_size, signature, signature_size, &count);
	expect(error == PK_VERIFY_SIGNATURE, "signature of another client data hash");

	/* Extensions: a map that is the rest is good; attested data, trailing bytes and a missing map are not. */
	other_size = make_auth_data(other, "zedbsd.login", PK_FLAG_UP | PK_FLAG_UV | PK_FLAG_ED, 9U, extensions,
	    sizeof(extensions));
	signature_size = sign(other, other_size, client_hash, signature);
	error = check("zedbsd.login", PK_FLAG_UV, 0U, credential_id, other, other_size, signature, signature_size, &count);
	expect(error == 0, "extensions map");
	other_size = make_auth_data(other, "zedbsd.login", PK_FLAG_UP | PK_FLAG_UV | PK_FLAG_AT, 9U, NULL, 0U);
	error = check("zedbsd.login", PK_FLAG_UV, 0U, credential_id, other, other_size, signature, signature_size, &count);
	expect(error == PK_VERIFY_MALFORMED, "attested data in an assertion");
	other_size = make_auth_data(other, "zedbsd.login", PK_FLAG_UP | PK_FLAG_UV, 9U, extensions, 1U);
	error = check("zedbsd.login", PK_FLAG_UV, 0U, credential_id, other, other_size, signature, signature_size, &count);
	expect(error == PK_VERIFY_MALFORMED, "trailing bytes");
	other_size = make_auth_data(other, "zedbsd.login", PK_FLAG_UP | PK_FLAG_UV | PK_FLAG_ED, 9U, NULL, 0U);
	error = check("zedbsd.login", PK_FLAG_UV, 0U, credential_id, other, other_size, signature, signature_size, &count);
	expect(error == PK_VERIFY_MALFORMED, "extension flag without a map");
	error = check("zedbsd.login", PK_FLAG_UV, 0U, credential_id, other, 20U, signature, signature_size, &count);
	expect(error == PK_VERIFY_MALFORMED, "authenticator data too short");

	/* A stored key that is not ES256 (alg -8). */
	cose[4] = 0x27U;
	data_size = make_auth_data(data, "zedbsd.login", PK_FLAG_UP | PK_FLAG_UV, 11U, NULL, 0U);
	signature_size = sign(data, data_size, client_hash, signature);
	error = check("zedbsd.login", PK_FLAG_UV, 0U, credential_id, data, data_size, signature, signature_size, &count);
	expect(error == PK_VERIFY_MALFORMED, "stored key not ES256");
	cose[4] = 0x26U;

	/* A malformed DER signature. */
	signature[0] ^= 0xffU;
	error = check("zedbsd.login", PK_FLAG_UV, 0U, credential_id, data, data_size, signature, signature_size, &count);
	expect(error == PK_VERIFY_SIGNATURE, "malformed signature");

	EVP_PKEY_free(key);

	/* The verdict. */
	if (failures != 0U) {
		printf("libpasskey-verify-host-test: FAIL (%u)\n", failures);
		return 1;
	}
	printf("libpasskey-verify-host-test: PASS\n");
	return 0;
}
