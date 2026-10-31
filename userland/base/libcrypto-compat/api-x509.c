/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The public X509, X509_STORE and X509_VERIFY_PARAM functions.
 */

#include "cryc-api.h"

/*
 * Makes a store with one reference (for SSL_CTX_new).
 */
X509_STORE *
cryc_x509_store_new(void)
{
	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Drops one reference to a store, freeing it at the last; NULL is ignored.
 */
void
cryc_x509_store_free(
	X509_STORE *store)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(store);
}

/*
 * Makes a certificate object from a parsed peer certificate's DER (a
 * copy), with one reference: libssl-compat keeps the server's chain this
 * way.
 */
X509 *
cryc_x509_from_der(
	const uint8_t *der,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(der);
	UNUSED_PARAMETER(size);

	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Maps a validation verdict to its X509_V_ code.
 */
long
cryc_verify_code_to_x509(
	enum cryc_verify_code code)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(code);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Makes a certificate from DER (OpenSSL's d2i convention): reads *in for
 * at most length bytes; on success advances *in past the certificate and
 * returns a new X509 with one reference (stored also in *out when out is
 * not NULL; an existing *out is freed first).
 */
X509 *
d2i_X509(
	X509 **out,
	const unsigned char **in,
	long length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(in);
	UNUSED_PARAMETER(length);

	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Drops one reference to a certificate and frees it at the last; NULL is
 * ignored.
 */
void
X509_free(
	X509 *certificate)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(certificate);
}

/*
 * Adds a certificate to a store as a trust anchor (the store takes its own
 * copy; the caller still frees certificate).
 */
int
X509_STORE_add_cert(
	X509_STORE *store,
	X509 *certificate)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(store);
	UNUSED_PARAMETER(certificate);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Sets the host name a validation must match (RFC 9525 DNS-ID; no common-
 * name fallback).
 */
int
X509_VERIFY_PARAM_set1_host(
	X509_VERIFY_PARAM *param,
	const char *name,
	size_t length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(name);
	UNUSED_PARAMETER(length);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Sets the IP address a validation must match, from its text: a dotted
 * IPv4 address or an IPv6 address (RFC 4291 2.2 forms, without brackets or
 * zone).
 */
int
X509_VERIFY_PARAM_set1_ip_asc(
	X509_VERIFY_PARAM *param,
	const char *address)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(address);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Returns a constant English text for a verification result (an unknown
 * value gives a generic text, never NULL).
 */
const char *
X509_verify_cert_error_string(
	long result)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(result);

	/* Not implemented: the failure value. */
	return NULL;
}
