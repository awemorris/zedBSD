/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Names in certificates: the comparison of distinguished names for
 * chaining (RFC 5280 7.1), the matching of a server's name or address
 * against its certificate (RFC 9525, November 2023, which replaces RFC
 * 6125), and name constraints (RFC 5280 4.2.1.10 and 6.1.3 (b), (c)).
 *
 * Host names are compared as ASCII, case-insensitively: a caller passes
 * an internationalized name as its A-labels (the browser's URL parser
 * does).  The common name of the subject is never used as a host name
 * (RFC 9525 6.3: no CN-ID); a certificate without a matching dNSName or
 * iPAddress in its subjectAltName does not match.
 *
 * All inputs are public; certificate bytes are untrusted.
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_NAME_H
#define LIBCRYPTO_COMPAT_CRYC_NAME_H

#include "cryc-x509.h"

/* The longest host name accepted (RFC 1035 2.3.4: 253 characters without the root's dot). */
#define CRYC_NAME_HOST_MAX	253U

/*
 * Compares two Name encodings (whole SEQUENCEs) as RFC 5280 7.1 asks for
 * path validation.
 *
 * Equal when byte-equal; otherwise equal when both have the same number
 * of RDNs, each RDN the same number of attributes in the same order, each
 * pair the same attribute type OID, and values equal: two PrintableString
 * or UTF8String values (either kind against either) compare after
 * removing leading and trailing spaces, folding internal runs of spaces to
 * one, and folding ASCII letters to lower case (the subset of RFC 4518
 * that RFC 5280 7.1 requires conforming implementations to support, for
 * ASCII); a value containing a non-ASCII byte, and any other string type,
 * compares byte for byte.  (Set ordering within a multi-valued RDN is not
 * reordered; such RDNs are very rare and must then match in order.)
 *
 * Returns 1 when equal, 0 when not, and 0 for a malformed encoding.
 */
int cryc_name_equal(const uint8_t *left, size_t left_size, const uint8_t *right, size_t right_size);

/*
 * Checks a reference host name (RFC 9525 6.1, a DNS domain name of
 * A-labels) and puts it in the form the matching uses: one trailing dot
 * removed, letters folded to lower case, written to out (room for
 * CRYC_NAME_HOST_MAX + 1 bytes, NUL-terminated).
 *
 * Returns 0, or EINVAL for an empty name, a name over the limit, an empty
 * label, a label over 63 characters, or a character other than letters,
 * digits, '-' and '.' (and '_', accepted in labels as some servers use
 * it).  An IP address literal is not a host name (the caller decides with
 * its own parser and calls cryc_name_match_ip).
 */
int cryc_name_host_normalize(const char *host, char *out);

/*
 * Matches a normalized reference host name against the dNSName entries
 * of cert's subjectAltName (RFC 9525 6.3 and 6.4).
 *
 * A presented identifier matches when equal ignoring ASCII case, or when
 * its first label is exactly "*" and the rest equal the reference's
 * labels after the first, the reference having the same number of labels
 * (a wildcard matches one whole label, never a part, never across dots;
 * an identifier of fewer than three labels with a wildcard, such as
 * "*.com", never matches; no public suffix list is consulted).  A
 * presented identifier that is malformed (empty labels, '*' anywhere but
 * as the whole first label) never matches.  Returns 1 or 0.  A
 * subjectAltName that does not parse matches nothing.
 */
int cryc_name_match_host(const struct cryc_x509 *cert, const char *host);

/*
 * Matches an IP address (4 or 16 bytes, network order) against the
 * iPAddress entries of cert's subjectAltName (RFC 9525 6.2: exact
 * equality of the octets).  Returns 1 or 0.
 */
int cryc_name_match_ip(const struct cryc_x509 *cert, const uint8_t *address, size_t size);

/*
 * Checks the names of subject against the name constraints of ca (which
 * has CRYC_X509_HAS_NAME_CONSTRAINTS), RFC 5280 4.2.1.10 and 6.1.3 (b),
 * (c), (applied to every certificate below the constraining one, 6.1.4
 * (g)).
 *
 * Name forms constrained: dNSName (a constraint "example.com" permits the
 * name itself and every name ending in ".example.com"; a constraint
 * starting with "." permits only names below it), iPAddress (address and
 * mask of 8 or 32 bytes; an address of the other family is not matched by
 * the constraint), directoryName (the subject's Name, and directoryName
 * entries of its subjectAltName, must begin with the constraint's RDNs,
 * compared as cryc_name_equal compares an RDN; an empty subject is not
 * constrained, 6.1.3 (b)), and rfc822Name (the three forms of 4.2.1.10).
 * A name must be within at least one permitted subtree of its form when
 * the permitted list has any subtree of that form, and within no
 * excluded subtree.  A constraint of a form not listed above (URI,
 * otherName, ...) fails the check when subject has a name of that form,
 * and is ignored otherwise.  is_leaf is 1 for the end-entity certificate
 * (only it carries the names the check matters most for, but the check
 * is the same for intermediates, which are not self-issued, 6.1.4 (g)).
 *
 * Returns 0 when the names are allowed; EACCES when a name is outside a
 * permitted subtree or inside an excluded one; ENOTSUP for a constrained
 * name form the subject uses that this check cannot evaluate; EINVAL for
 * malformed constraints or names.
 */
int cryc_name_constraints_check(const struct cryc_x509 *ca, const struct cryc_x509 *subject, int is_leaf);

#endif
