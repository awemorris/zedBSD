/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parsing of X.509 certificates, public keys and signature algorithms
 * (RFC 5280).
 */

#include "cryc-x509.h"

/*
 * Parses an AlgorithmIdentifier encoding (the whole SEQUENCE) naming a
 * signature algorithm.
 */
int
cryc_x509_signature_algorithm(
	const uint8_t *encoding,
	size_t size,
	struct cryc_sig_params *params)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(encoding);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(params);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Parses a SubjectPublicKeyInfo encoding (the whole SEQUENCE).
 */
int
cryc_x509_public_key(
	const uint8_t *encoding,
	size_t size,
	struct cryc_pubkey *key)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(encoding);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(key);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Parses a certificate (RFC 5280 4.1) from size DER bytes.
 */
int
cryc_x509_parse(
	struct cryc_x509 *cert,
	const uint8_t *der,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(cert);
	UNUSED_PARAMETER(der);
	UNUSED_PARAMETER(size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Returns the hash of a signature algorithm (SHA-256 for
 * CRYC_SIG_RSA_PKCS1_SHA256 and so on), or CRYC_HASH_NONE for
 * CRYC_SIG_NONE and CRYC_SIG_UNSUPPORTED.
 */
enum cryc_hash_id
cryc_sig_hash(
	enum cryc_sig_alg alg)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(alg);

	/* Not implemented: the failure value. */
	return 0;
}
