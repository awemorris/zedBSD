/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Signature verification by algorithm.
 */

#include "cryc-sigver.h"

#include "cryc-der.h"
#include "cryc-hash.h"
#include "cryc-rsa.h"

#include <string.h>

/* Forward declarations. */
static int sigver_signature_integer(const struct cryc_der_element *element, size_t size, const uint8_t **magnitude, size_t *magnitude_size);
static int sigver_verify_rsa(const struct cryc_pubkey *key, const struct cryc_sig_params *params, enum cryc_hash_id hash_id, const uint8_t *digest, size_t digest_size, const uint8_t *signature, size_t signature_size);
static int sigver_verify_ecdsa(const struct cryc_pubkey *key, const uint8_t *digest, size_t digest_size, const uint8_t *signature, size_t signature_size);

/*
 * Decodes an ECDSA signature in its DER form, Ecdsa-Sig-Value (RFC 5480
 * 2.2 via RFC 3279 2.2.3; the same form TLS uses, RFC 8446 4.2.3 and RFC
 * 8422 5.4): SEQUENCE { r INTEGER, s INTEGER }, nothing after it.
 *
 * Each INTEGER goes through the strict DER reader, which refuses a
 * negative value and a non-minimal encoding; a value of zero or wider than
 * size bytes is refused here.  r and s are written only when the whole
 * signature is accepted.
 */
int
cryc_ecdsa_signature_decode(
	const uint8_t *der,
	size_t der_size,
	size_t size,
	uint8_t *r,
	uint8_t *s)
{
	struct cryc_der outer;
	struct cryc_der inner;
	struct cryc_der_element sequence;
	struct cryc_der_element r_element;
	struct cryc_der_element s_element;
	const uint8_t *r_magnitude;
	const uint8_t *s_magnitude;
	size_t r_size;
	size_t s_size;
	int error;

	/* Reads the outer SEQUENCE, which must be the whole input. */
	cryc_der_init(&outer, der, der_size);
	error = cryc_der_expect(&outer, CRYC_DER_SEQUENCE, &sequence);
	if (error != 0)
		return EBADMSG;
	if (cryc_der_empty(&outer) == 0)
		return EBADMSG;

	/* Reads r and s, which must be the whole contents of the SEQUENCE. */
	error = cryc_der_enter(&sequence, &inner);
	if (error != 0)
		return EBADMSG;
	error = cryc_der_expect(&inner, CRYC_DER_INTEGER, &r_element);
	if (error != 0)
		return EBADMSG;
	error = cryc_der_expect(&inner, CRYC_DER_INTEGER, &s_element);
	if (error != 0)
		return EBADMSG;
	if (cryc_der_empty(&inner) == 0)
		return EBADMSG;

	/* Checks each value without writing anything. */
	error = sigver_signature_integer(&r_element, size, &r_magnitude, &r_size);
	if (error != 0)
		return EBADMSG;
	error = sigver_signature_integer(&s_element, size, &s_magnitude, &s_size);
	if (error != 0)
		return EBADMSG;

	/* Writes both right-aligned in their fields, now that both are accepted. */
	memset(r, 0, size);
	memcpy(r + (size - r_size), r_magnitude, r_size);
	memset(s, 0, size);
	memcpy(s + (size - s_size), s_magnitude, s_size);

	/* Succeeded: r and s hold the two values, size bytes each. */
	return 0;
}

/*
 * Verifies signature over message with key under params.
 *
 * The checks come in a fixed order, so that a caller's mistake is not
 * hidden behind a malformed signature: missing arguments (EINVAL), an
 * unset algorithm or key (EINVAL), an unsupported algorithm or key
 * (ENOTSUP), an algorithm that the key does not suit (EINVAL); then the
 * key's own checks, then the signature.
 */
int
cryc_signature_verify(
	const struct cryc_pubkey *key,
	const struct cryc_sig_params *params,
	const uint8_t *message,
	size_t message_size,
	const uint8_t *signature,
	size_t signature_size)
{
	uint8_t digest[CRYC_HASH_MAX_SIZE];
	uint8_t no_signature[1];
	enum cryc_hash_id hash_id;
	size_t digest_size;
	int rsa_alg;
	int error;

	/* Refuses missing arguments (a NULL buffer is allowed only when its size is 0). */
	if (key == NULL || params == NULL)
		return EINVAL;
	if (message == NULL && message_size != 0U)
		return EINVAL;
	if (signature == NULL && signature_size != 0U)
		return EINVAL;

	/* Refuses an algorithm or a key that was never set. */
	if (params->alg == CRYC_SIG_NONE || key->type == CRYC_KEY_NONE)
		return EINVAL;

	/* Refuses an algorithm or a key outside libssl-compat's range, and any value that is not one of the enumerators. */
	if (params->alg == CRYC_SIG_UNSUPPORTED || key->type == CRYC_KEY_UNSUPPORTED)
		return ENOTSUP;
	if ((int)params->alg < (int)CRYC_SIG_RSA_PKCS1_SHA256 || (int)params->alg > (int)CRYC_SIG_ECDSA_SHA512)
		return ENOTSUP;
	if (key->type != CRYC_KEY_RSA && key->type != CRYC_KEY_EC)
		return ENOTSUP;

	/* Sorts the algorithm by the kind of key it needs: the RSA algorithms come first in the enumeration. */
	rsa_alg = (int)params->alg <= (int)CRYC_SIG_RSA_PSS_SHA512;

	/* Refuses an algorithm that the key does not suit. */
	if (rsa_alg && key->type != CRYC_KEY_RSA)
		return EINVAL;
	if (!rsa_alg && key->type != CRYC_KEY_EC)
		return EINVAL;

	/* An empty signature given as NULL is a signature of no bytes: the primitives refuse it as a bad signature, after their checks of the key. */
	if (signature == NULL) {
		no_signature[0] = 0U;
		signature = no_signature;
	}

	/* Hashes the message with the algorithm's hash. */
	hash_id = cryc_sig_hash(params->alg);
	digest_size = cryc_hash_size(hash_id);
	if (digest_size == 0U)
		return ENOTSUP;
	error = cryc_hash_digest(hash_id, message, message_size, digest);
	if (error != 0)
		return error;

	/* Verifies by the kind of key. */
	if (rsa_alg) {
		error = sigver_verify_rsa(key, params, hash_id, digest, digest_size, signature, signature_size);
		if (error != 0)
			return error;
	} else {
		error = sigver_verify_ecdsa(key, digest, digest_size, signature, signature_size);
		if (error != 0)
			return error;
	}

	/* Succeeded: the signature is valid for the message under the key. */
	return 0;
}

/*
 * Checks one INTEGER of an Ecdsa-Sig-Value and gives its magnitude;
 * returns 0, or EBADMSG when it is malformed, zero, or wider than size
 * bytes.
 */
static int
sigver_signature_integer(
	const struct cryc_der_element *element,
	size_t size,
	const uint8_t **magnitude,
	size_t *magnitude_size)
{
	int error;

	/* The strict reader refuses a negative value and a redundant leading zero byte. */
	error = cryc_der_integer(element, magnitude, magnitude_size);
	if (error != 0)
		return EBADMSG;

	/* A value of zero is the one-byte magnitude 0x00; r and s must be at least 1 (FIPS 186-5 6.4.2 step 1). */
	if (*magnitude_size == 1U && (*magnitude)[0] == 0U)
		return EBADMSG;

	/* A value with more significant bytes than the field's width cannot be below n. */
	if (*magnitude_size > size)
		return EBADMSG;

	/* Succeeded: the magnitude fits the field. */
	return 0;
}

/*
 * Verifies an RSA signature (PKCS#1 v1.5 or PSS) over a message whose
 * digest is given; the primitives' errors are returned as they are.
 */
static int
sigver_verify_rsa(
	const struct cryc_pubkey *key,
	const struct cryc_sig_params *params,
	enum cryc_hash_id hash_id,
	const uint8_t *digest,
	size_t digest_size,
	const uint8_t *signature,
	size_t signature_size)
{
	int error;

	/* RSASSA-PSS names its salt length; the PKCS#1 v1.5 schemes have none. */
	if (params->alg == CRYC_SIG_RSA_PSS_SHA256 || params->alg == CRYC_SIG_RSA_PSS_SHA384 || params->alg == CRYC_SIG_RSA_PSS_SHA512) {
		error = cryc_rsa_pss_verify(&key->rsa, hash_id, digest, digest_size, params->salt_size, signature, signature_size);
		if (error != 0)
			return error;
	} else {
		error = cryc_rsa_pkcs1_verify(&key->rsa, hash_id, digest, digest_size, signature, signature_size);
		if (error != 0)
			return error;
	}

	/* Succeeded: the RSA signature verifies. */
	return 0;
}

/*
 * Verifies an ECDSA signature in DER form over a message whose digest is
 * given.  The key is checked before the signature, so that a malformed key
 * is reported as such whatever the signature is.  The digest may be longer
 * or shorter than the curve's order: the verification takes its leftmost
 * bits (FIPS 186-5 6.4.2 step 3).
 */
static int
sigver_verify_ecdsa(
	const struct cryc_pubkey *key,
	const uint8_t *digest,
	size_t digest_size,
	const uint8_t *signature,
	size_t signature_size)
{
	struct cryc_ecp ec;
	struct cryc_ecp_point point;
	uint8_t r[CRYC_ECP_MAX_BYTES];
	uint8_t s[CRYC_ECP_MAX_BYTES];
	int error;

	/* A key of this type names one of the two curves; anything else is a malformed key. */
	if (key->curve != CRYC_CURVE_P256 && key->curve != CRYC_CURVE_P384)
		return EINVAL;

	/* Prepares the curve. */
	memset(&ec, 0, sizeof(ec));
	error = cryc_ecp_init(&ec, key->curve);
	if (error != 0)
		return EINVAL;

	/* Validates the point: every way it can be wrong (size, form, coordinates, not on the curve) is a malformed key. */
	memset(&point, 0, sizeof(point));
	error = cryc_ecp_decode(&ec, &point, key->point, key->point_size);
	if (error != 0)
		return EINVAL;

	/* Decodes the DER signature. */
	error = cryc_ecdsa_signature_decode(signature, signature_size, ec.size, r, s);
	if (error != 0)
		return EBADMSG;

	/* Verifies against the public point (decoded again by the primitive; the cost is small beside the multiplication). */
	error = cryc_ecdsa_verify(&ec, key->point, key->point_size, digest, digest_size, r, s);
	if (error != 0)
		return error;

	/* Succeeded: the ECDSA signature verifies. */
	return 0;
}
