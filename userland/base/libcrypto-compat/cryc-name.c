/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Name comparison, host and address matching, and name constraints.
 */

#include "cryc-name.h"

/*
 * Compares two Name encodings (whole SEQUENCEs) as RFC 5280 7.1 asks for
 * path validation.
 */
int
cryc_name_equal(
	const uint8_t *left,
	size_t left_size,
	const uint8_t *right,
	size_t right_size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(left);
	UNUSED_PARAMETER(left_size);
	UNUSED_PARAMETER(right);
	UNUSED_PARAMETER(right_size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Checks a reference host name (RFC 9525 6.1, a DNS domain name of
 * A-labels) and puts it in the form the matching uses: one trailing dot
 * removed, letters folded to lower case, written to out (room for
 * CRYC_NAME_HOST_MAX + 1 bytes, NUL-terminated).
 */
int
cryc_name_host_normalize(
	const char *host,
	char *out)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(host);
	UNUSED_PARAMETER(out);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Matches a normalized reference host name against the dNSName entries of
 * cert's subjectAltName (RFC 9525 6.3 and 6.4).
 */
int
cryc_name_match_host(
	const struct cryc_x509 *cert,
	const char *host)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(cert);
	UNUSED_PARAMETER(host);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Matches an IP address (4 or 16 bytes, network order) against the
 * iPAddress entries of cert's subjectAltName (RFC 9525 6.2: exact equality
 * of the octets).
 */
int
cryc_name_match_ip(
	const struct cryc_x509 *cert,
	const uint8_t *address,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(cert);
	UNUSED_PARAMETER(address);
	UNUSED_PARAMETER(size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Checks the names of subject against the name constraints of ca (which
 * has CRYC_X509_HAS_NAME_CONSTRAINTS), RFC 5280 4.2.1.10 and 6.1.3 (b),
 * (c), (applied to every certificate below the constraining one, 6.1.4
 * (g)).
 */
int
cryc_name_constraints_check(
	const struct cryc_x509 *ca,
	const struct cryc_x509 *subject,
	int is_leaf)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ca);
	UNUSED_PARAMETER(subject);
	UNUSED_PARAMETER(is_leaf);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}
