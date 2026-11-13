/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The public X509, X509_STORE and X509_VERIFY_PARAM functions, and the
 * private functions libssl-compat uses on them.
 *
 * A function that fails queues exactly one error (the X509 library for a
 * null or invalid argument, a store at its limit and no memory; the ASN.1
 * library for a certificate that does not parse) and returns the failure
 * value of its interface; a function that succeeds queues nothing.  A
 * failed call changes neither the objects it was given nor the caller's
 * output, except that a failed d2i_X509 frees the object *out names, as
 * its interface says.
 *
 * Everything handled here is public: certificates, host names and
 * addresses.  Nothing is secret, so nothing is wiped and nothing is
 * compared in constant time.
 */

#include <stdlib.h>
#include <string.h>

#include "cryc-api.h"
#include "cryc-der.h"
#include "cryc-os.h"

/* The longest text a host name setting reads: a host name, a trailing dot, and the NUL that ends it. */
#define HOST_TEXT_ROOM		(CRYC_NAME_HOST_MAX + 2U)

/* The largest DER a certificate can have: a header of at most five octets (SEQUENCE and a length of three octets) and contents of at most CRYC_DER_MAX_LENGTH. */
#define CERTIFICATE_DER_MAX	(CRYC_DER_MAX_LENGTH + 5UL)

/* The number of 16-bit groups of an IPv6 address (RFC 4291 2.2). */
#define IPV6_GROUPS		8U

/*
 * One verification verdict of the library paired with the X509_V_ code
 * a program sees for it.
 */
struct verdict_code {
	enum cryc_verify_code verdict;
	long code;
};

/*
 * One X509_V_ code paired with its constant text.
 */
struct code_text {
	long code;
	const char *text;
};

/*
 * The X509_V_ code of every verdict the validation reports.
 *
 * A verdict that has no code of its own in the subset (no peer
 * certificate, malformed input, the work bound) is X509_V_ERR_UNSPECIFIED.
 * Two verdicts stand for more than one code and take the one named:
 * CRYC_VERIFY_KEY_USAGE (a missing keyCertSign or a missing
 * digitalSignature) is X509_V_ERR_KEYUSAGE_NO_CERTSIGN, and
 * CRYC_VERIFY_NAME_CONSTRAINT_VIOLATION (a name outside a permitted
 * subtree or inside an excluded one) is X509_V_ERR_PERMITTED_VIOLATION.
 * A verdict not in this table is X509_V_ERR_UNSPECIFIED.
 * Values referenced: OpenSSL 3.5.8 include/openssl/x509_vfy.h.in
 * (X509_V_OK, X509_V_ERR_*).
 */
static const struct verdict_code verdict_codes[] = {
	{ CRYC_VERIFY_OK, X509_V_OK },
	{ CRYC_VERIFY_NO_PEER_CERTIFICATE, X509_V_ERR_UNSPECIFIED },
	{ CRYC_VERIFY_MALFORMED, X509_V_ERR_UNSPECIFIED },
	{ CRYC_VERIFY_UNABLE_TO_GET_ISSUER, X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY },
	{ CRYC_VERIFY_DEPTH_ZERO_SELF_SIGNED, X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT },
	{ CRYC_VERIFY_SELF_SIGNED_IN_CHAIN, X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN },
	{ CRYC_VERIFY_CHAIN_TOO_LONG, X509_V_ERR_CERT_CHAIN_TOO_LONG },
	{ CRYC_VERIFY_SIGNATURE_FAILURE, X509_V_ERR_CERT_SIGNATURE_FAILURE },
	{ CRYC_VERIFY_UNSUPPORTED_ALGORITHM, X509_V_ERR_UNSUPPORTED_SIGNATURE_ALGORITHM },
	{ CRYC_VERIFY_KEY_TOO_SMALL, X509_V_ERR_EE_KEY_TOO_SMALL },
	{ CRYC_VERIFY_NOT_YET_VALID, X509_V_ERR_CERT_NOT_YET_VALID },
	{ CRYC_VERIFY_EXPIRED, X509_V_ERR_CERT_HAS_EXPIRED },
	{ CRYC_VERIFY_INVALID_CA, X509_V_ERR_INVALID_CA },
	{ CRYC_VERIFY_PATH_LENGTH_EXCEEDED, X509_V_ERR_PATH_LENGTH_EXCEEDED },
	{ CRYC_VERIFY_KEY_USAGE, X509_V_ERR_KEYUSAGE_NO_CERTSIGN },
	{ CRYC_VERIFY_INVALID_PURPOSE, X509_V_ERR_INVALID_PURPOSE },
	{ CRYC_VERIFY_UNHANDLED_CRITICAL_EXTENSION, X509_V_ERR_UNHANDLED_CRITICAL_EXTENSION },
	{ CRYC_VERIFY_NAME_CONSTRAINT_VIOLATION, X509_V_ERR_PERMITTED_VIOLATION },
	{ CRYC_VERIFY_UNSUPPORTED_NAME_CONSTRAINT, X509_V_ERR_UNSUPPORTED_NAME_SYNTAX },
	{ CRYC_VERIFY_HOSTNAME_MISMATCH, X509_V_ERR_HOSTNAME_MISMATCH },
	{ CRYC_VERIFY_IP_ADDRESS_MISMATCH, X509_V_ERR_IP_ADDRESS_MISMATCH },
	{ CRYC_VERIFY_WORK_EXCEEDED, X509_V_ERR_UNSPECIFIED },
	{ CRYC_VERIFY_REJECTED, X509_V_ERR_CERT_REJECTED }
};

/*
 * The constant text of every X509_V_ code of the subset.
 *
 * Values referenced: OpenSSL 3.5.8 crypto/x509/x509_txt.c
 * (X509_verify_cert_error_string, the text of each X509_V_ code), used as
 * data.
 */
static const struct code_text code_texts[] = {
	{ X509_V_OK, "ok" },
	{ X509_V_ERR_UNSPECIFIED, "unspecified certificate verification error" },
	{ X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT, "unable to get issuer certificate" },
	{ X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY, "unable to decode issuer public key" },
	{ X509_V_ERR_CERT_SIGNATURE_FAILURE, "certificate signature failure" },
	{ X509_V_ERR_CERT_NOT_YET_VALID, "certificate is not yet valid" },
	{ X509_V_ERR_CERT_HAS_EXPIRED, "certificate has expired" },
	{ X509_V_ERR_OUT_OF_MEM, "out of memory" },
	{ X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT, "self-signed certificate" },
	{ X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN, "self-signed certificate in certificate chain" },
	{ X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY, "unable to get local issuer certificate" },
	{ X509_V_ERR_CERT_CHAIN_TOO_LONG, "certificate chain too long" },
	{ X509_V_ERR_PATH_LENGTH_EXCEEDED, "path length constraint exceeded" },
	{ X509_V_ERR_INVALID_PURPOSE, "unsuitable certificate purpose" },
	{ X509_V_ERR_CERT_REJECTED, "certificate rejected" },
	{ X509_V_ERR_KEYUSAGE_NO_CERTSIGN, "key usage does not include certificate signing" },
	{ X509_V_ERR_UNHANDLED_CRITICAL_EXTENSION, "unhandled critical extension" },
	{ X509_V_ERR_INVALID_CA, "invalid CA certificate" },
	{ X509_V_ERR_KEYUSAGE_NO_DIGITAL_SIGNATURE, "key usage does not include digital signature" },
	{ X509_V_ERR_PERMITTED_VIOLATION, "permitted subtree violation" },
	{ X509_V_ERR_EXCLUDED_VIOLATION, "excluded subtree violation" },
	{ X509_V_ERR_UNSUPPORTED_NAME_SYNTAX, "unsupported or invalid name syntax" },
	{ X509_V_ERR_HOSTNAME_MISMATCH, "hostname mismatch" },
	{ X509_V_ERR_IP_ADDRESS_MISMATCH, "IP address mismatch" },
	{ X509_V_ERR_EE_KEY_TOO_SMALL, "EE certificate key too weak" },
	{ X509_V_ERR_UNSUPPORTED_SIGNATURE_ALGORITHM, "Cannot find certificate signature algorithm" }
};

/*
 * The text of a code the subset does not have.
 *
 * Values referenced: OpenSSL 3.5.8 crypto/x509/x509_txt.c
 * (X509_verify_cert_error_string, the default case), used as data.
 */
static const char unknown_code_text[] = "unknown certificate verification error";

/* Queues one error of the X509 library. */
static void x509_fail(unsigned reason);

/* Frees the object *out names and sets *out to NULL when out is not NULL (the failed d2i convention). */
static void d2i_forget(X509 **out);

/* Reads the value of one hexadecimal digit; -1 when the character is not one. */
static int hex_value(char digit);

/* Reads a dotted IPv4 address (four decimal fields of 0 to 255, no leading zeros) from [begin, end); 1 when it is one. */
static int ip_parse_v4(const char *begin, const char *end, uint8_t *out);

/* Reads an IPv6 address in the forms of RFC 4291 2.2 from a C string; 1 when it is one. */
static int ip_parse_v6(const char *text, uint8_t *out);

/*
 * Makes a store with one reference (for SSL_CTX_new).
 */
X509_STORE *
cryc_x509_store_new(void)
{
	X509_STORE *store;
	int error;

	/* Allocates the store object. */
	store = calloc(1, sizeof(*store));
	if (store == NULL) {
		x509_fail(CRYC_R_MALLOC_FAILURE);
		return NULL;
	}

	/* Starts the internal store (its lock and its empty arrays). */
	error = cryc_store_init(&store->store);
	if (error != 0) {
		free(store);
		x509_fail(CRYC_R_MALLOC_FAILURE);
		return NULL;
	}

	/* The creator holds the first reference. */
	store->references = 1L;

	/* Succeeded: a store with one reference, owned by the caller. */
	return store;
}

/*
 * Drops one reference to a store, freeing it at the last; NULL is ignored.
 */
void
cryc_x509_store_free(
	X509_STORE *store)
{
	long count;

	/* Nothing to free. */
	if (store == NULL)
		return;

	/* Drops this holder's reference; the call that brings the count to zero frees the store. */
	count = cryc_os_atomic_add(&store->references, -1L);
	if (count != 0L)
		return;

	/* Frees the certificates, the marks and the lock, then the object. */
	cryc_store_free(&store->store);
	free(store);
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
	X509 *certificate;
	int error;

	/* Refuses a missing encoding. */
	if (der == NULL) {
		x509_fail(CRYC_R_PASSED_NULL_PARAMETER);
		return NULL;
	}

	/* Refuses an empty encoding and one longer than any certificate can be. */
	if (size == 0U || size > CERTIFICATE_DER_MAX) {
		x509_fail(CRYC_R_PASSED_INVALID_ARGUMENT);
		return NULL;
	}

	/* Allocates the certificate object. */
	certificate = calloc(1, sizeof(*certificate));
	if (certificate == NULL) {
		x509_fail(CRYC_R_MALLOC_FAILURE);
		return NULL;
	}

	/* Allocates the object's own copy of the DER that the parsed form points into. */
	certificate->der = malloc(size);
	if (certificate->der == NULL) {
		free(certificate);
		x509_fail(CRYC_R_MALLOC_FAILURE);
		return NULL;
	}

	/* Copies the DER so that the object does not depend on the caller's buffer. */
	memcpy(certificate->der, der, size);
	certificate->der_size = size;

	/* Parses the copy; the parsed form is a set of views into it. */
	error = cryc_x509_parse(&certificate->cert, certificate->der, certificate->der_size);
	if (error != 0) {
		free(certificate->der);
		free(certificate);
		cryc_err_put(CRYC_LIB_ASN1, CRYC_R_ASN1_WRONG_TAG);
		return NULL;
	}

	/* The creator holds the first reference. */
	certificate->references = 1L;

	/* Succeeded: a certificate with one reference, owned by the caller. */
	return certificate;
}

/*
 * Maps a validation verdict to its X509_V_ code.
 */
long
cryc_verify_code_to_x509(
	enum cryc_verify_code code)
{
	size_t index;

	/* Looks the verdict up in the table. */
	for (index = 0; index < sizeof(verdict_codes) / sizeof(verdict_codes[0]); index++) {
		if (verdict_codes[index].verdict == code)
			return verdict_codes[index].code;
	}

	/* A verdict the table does not know has no more specific code. */
	return X509_V_ERR_UNSPECIFIED;
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
	struct cryc_der reader;
	struct cryc_der_element element;
	X509 *certificate;
	X509 *old;
	unsigned reason;
	int error;

	/* Refuses a missing input pointer. */
	if (in == NULL || *in == NULL) {
		x509_fail(CRYC_R_PASSED_NULL_PARAMETER);
		d2i_forget(out);
		return NULL;
	}

	/* Refuses a length that is not positive. */
	if (length <= 0L) {
		x509_fail(CRYC_R_PASSED_INVALID_ARGUMENT);
		d2i_forget(out);
		return NULL;
	}

	/*
	 * Finds where the Certificate ends: the first element of the input is
	 * its SEQUENCE, and the bytes after it are left unread.
	 */
	cryc_der_init(&reader, *in, (size_t)length);
	error = cryc_der_expect(&reader, CRYC_DER_SEQUENCE, &element);
	if (error != 0) {
		/* A first octet that is not SEQUENCE is the wrong tag; otherwise the length does not fit. */
		reason = CRYC_R_ASN1_HEADER_TOO_LONG;
		if ((*in)[0] != CRYC_DER_SEQUENCE)
			reason = CRYC_R_ASN1_WRONG_TAG;
		cryc_err_put(CRYC_LIB_ASN1, reason);
		d2i_forget(out);
		return NULL;
	}

	/* Makes the certificate object from exactly the Certificate's bytes (it queues its own error). */
	certificate = cryc_x509_from_der(element.encoding, element.encoding_size);
	if (certificate == NULL) {
		d2i_forget(out);
		return NULL;
	}

	/* Moves the caller's pointer past the certificate. */
	*in += element.encoding_size;

	/* Hands the object over in *out as well, after dropping the one it held. */
	if (out != NULL) {
		old = *out;
		*out = certificate;
		X509_free(old);
	}

	/* Succeeded: a certificate with one reference, owned by the caller. */
	return certificate;
}

/*
 * Drops one reference to a certificate and frees it at the last; NULL is
 * ignored.
 */
void
X509_free(
	X509 *certificate)
{
	long count;

	/* Nothing to free. */
	if (certificate == NULL)
		return;

	/* Drops this holder's reference; the call that brings the count to zero frees the certificate. */
	count = cryc_os_atomic_add(&certificate->references, -1L);
	if (count != 0L)
		return;

	/* Frees the copy of the DER the parsed form pointed into, then the object. */
	free(certificate->der);
	free(certificate);
}

/*
 * Writes the digest of a certificate's whole DER encoding with type (only
 * SHA-256) into md, and its size into *length when length is not NULL.
 */
int
X509_digest(
	const X509 *certificate,
	const EVP_MD *type,
	unsigned char *md,
	unsigned int *length)
{
	int result;

	/* Refuses a missing certificate (EVP_Digest checks the rest). */
	if (certificate == NULL) {
		x509_fail(CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Hashes the DER the object holds, as received; EVP_Digest refuses a missing output and every digest but SHA-256. */
	result = EVP_Digest(certificate->der, certificate->der_size, md, length, type, NULL);
	if (result != 1)
		return 0;

	/* Succeeded: md holds the fingerprint. */
	return 1;
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
	int error;

	/* Refuses a missing store or certificate. */
	if (store == NULL || certificate == NULL) {
		x509_fail(CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Adds a copy of the DER (a certificate already there is not added again and is not an error). */
	error = cryc_store_add_der(&store->store, certificate->der, certificate->der_size);
	if (error == ENOMEM) {
		/* No memory for the copy. */
		x509_fail(CRYC_R_MALLOC_FAILURE);
		return 0;
	}

	/* Reports a store that is full as an unsupported request. */
	if (error == ENOBUFS) {
		/* The store holds as many certificates as this library allows. */
		x509_fail(CRYC_R_UNSUPPORTED);
		return 0;
	}

	/* Reports any other refusal as an internal fault. */
	if (error != 0) {
		/* The certificate parsed when it was made, so the store refusing it is an internal fault. */
		x509_fail(CRYC_R_INTERNAL_ERROR);
		return 0;
	}

	/* Succeeded: the store trusts the certificate. */
	return 1;
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
	char text[HOST_TEXT_ROOM];
	char normalized[CRYC_NAME_HOST_MAX + 1U];
	size_t count;
	size_t text_length;
	int error;

	/* Refuses a missing parameter object. */
	if (param == NULL) {
		x509_fail(CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* A NULL name clears the host setting; the address setting stays. */
	if (name == NULL) {
		param->params.has_host = 0;
		memset(param->params.host, 0, sizeof(param->params.host));
		return 1;
	}

	/*
	 * Finds the name's length: given, or for 0 the length of the C
	 * string, looked for no further than the longest name plus its
	 * trailing dot (a longer string is not a host name).
	 */
	count = length;
	if (length == 0U) {
		while (count < HOST_TEXT_ROOM && name[count] != '\0')
			count++;
	}

	/* Refuses a name too long to be a host name, before anything is copied. */
	if (count > CRYC_NAME_HOST_MAX + 1U) {
		x509_fail(CRYC_R_PASSED_INVALID_ARGUMENT);
		return 0;
	}

	/* An empty name clears the host setting, as OpenSSL's does; the address setting stays. */
	if (count == 0U) {
		param->params.has_host = 0;
		memset(param->params.host, 0, sizeof(param->params.host));
		return 1;
	}

	/* Copies the name and ends it, so that the check works on a C string. */
	memcpy(text, name, count);
	text[count] = '\0';

	/* Refuses a name with a NUL inside it: a host name cannot have one, and the check would stop at it. */
	text_length = strlen(text);
	if (text_length != count) {
		x509_fail(CRYC_R_PASSED_INVALID_ARGUMENT);
		return 0;
	}

	/* Checks the name and puts it in the form the matching uses (no trailing dot, lower case). */
	error = cryc_name_host_normalize(text, normalized);
	if (error != 0) {
		x509_fail(CRYC_R_PASSED_INVALID_ARGUMENT);
		return 0;
	}

	/* Records the host; one identity is checked, so an address set before is dropped. */
	memset(param->params.host, 0, sizeof(param->params.host));
	memcpy(param->params.host, normalized, strlen(normalized));
	param->params.has_host = 1;
	memset(param->params.ip, 0, sizeof(param->params.ip));
	param->params.ip_size = 0U;

	/* Succeeded: the validation will match this host name. */
	return 1;
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
	uint8_t bytes[16];
	const char *end;
	const char *colon;
	size_t size;
	int good;

	/* Refuses a missing parameter object or text. */
	if (param == NULL || address == NULL) {
		x509_fail(CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* An address with a colon is IPv6, any other is IPv4. */
	memset(bytes, 0, sizeof(bytes));
	size = 4U;
	colon = strchr(address, ':');
	if (colon != NULL) {
		size = 16U;
		good = ip_parse_v6(address, bytes);
	} else {
		end = address + strlen(address);
		good = ip_parse_v4(address, end, bytes);
	}

	/* Refuses a text that is not an address. */
	if (!good) {
		x509_fail(CRYC_R_PASSED_INVALID_ARGUMENT);
		return 0;
	}

	/* Records the address; one identity is checked, so a host name set before is dropped. */
	memset(param->params.ip, 0, sizeof(param->params.ip));
	memcpy(param->params.ip, bytes, size);
	param->params.ip_size = size;
	param->params.has_host = 0;
	memset(param->params.host, 0, sizeof(param->params.host));

	/* Succeeded: the validation will match this address. */
	return 1;
}

/*
 * Returns a constant English text for a verification result (an unknown
 * value gives a generic text, never NULL).
 */
const char *
X509_verify_cert_error_string(
	long result)
{
	size_t index;

	/* Looks the code up in the table of the subset. */
	for (index = 0; index < sizeof(code_texts) / sizeof(code_texts[0]); index++) {
		if (code_texts[index].code == result)
			return code_texts[index].text;
	}

	/* A code outside the subset gets the generic text. */
	return unknown_code_text;
}

/*
 * Queues one error of the X509 library.
 */
static void
x509_fail(
	unsigned reason)
{
	/* Queues the reason under the X509 library's number. */
	cryc_err_put(CRYC_LIB_X509, reason);
}

/*
 * Frees the object *out names and sets *out to NULL when out is not NULL.
 */
static void
d2i_forget(
	X509 **out)
{
	X509 *old;

	/* A caller that did not pass out has nothing to forget. */
	if (out == NULL)
		return;

	/* Drops the reference *out held and clears it, as a failed d2i call must. */
	old = *out;
	*out = NULL;
	X509_free(old);
}

/*
 * Reads the value of one hexadecimal digit; -1 when the character is not
 * one.
 */
static int
hex_value(
	char digit)
{
	/* The decimal digits. */
	if (digit >= '0' && digit <= '9')
		return digit - '0';

	/* The lower-case letters a to f. */
	if (digit >= 'a' && digit <= 'f')
		return digit - 'a' + 10;

	/* The upper-case letters A to F. */
	if (digit >= 'A' && digit <= 'F')
		return digit - 'A' + 10;

	/* Not a hexadecimal digit. */
	return -1;
}

/*
 * Reads a dotted IPv4 address from [begin, end).
 */
static int
ip_parse_v4(
	const char *begin,
	const char *end,
	uint8_t *out)
{
	const char *cursor;
	unsigned field_count;
	unsigned digit_count;
	unsigned value;
	unsigned digit;

	/* Walks the text one character at a time, counting the fields (three dots make four) and the value of the field being read. */
	field_count = 0U;
	digit_count = 0U;
	value = 0U;
	for (cursor = begin;
	     cursor < end;
	     cursor++) {
		if (*cursor >= '0' && *cursor <= '9') {
			/* A leading zero is refused (a field of two or more digits may not start with 0: it could mean octal). */
			if (digit_count == 1U && value == 0U)
				return 0;

			/* Adds the digit; a field has at most three digits and at most the value 255. */
			digit = (unsigned)(*cursor - '0');
			value = value * 10U + digit;
			digit_count++;
			if (digit_count > 3U || value > 255U)
				return 0;
		} else if (*cursor == '.') {
			/* A dot ends a field, which must have digits; there are three dots at most. */
			if (digit_count == 0U || field_count >= 3U)
				return 0;
			out[field_count] = (uint8_t)value;
			field_count++;
			digit_count = 0U;
			value = 0U;
		} else {
			/* Any other character is not part of an address. */
			return 0;
		}
	}

	/* The last field ends at the end of the text and there are exactly four. */
	if (digit_count == 0U || field_count != 3U)
		return 0;
	out[3] = (uint8_t)value;

	/* Succeeded: the four octets are in out. */
	return 1;
}

/*
 * Reads an IPv6 address in the forms of RFC 4291 2.2 from a C string.
 */
static int
ip_parse_v6(
	const char *text,
	uint8_t *out)
{
	unsigned groups[IPV6_GROUPS];
	unsigned group_count;
	unsigned tail_count;
	unsigned digit_count;
	unsigned value;
	int compress_at;
	int finished;
	int has_dot;
	int digit;
	int good;
	const char *cursor;
	const char *token_end;
	const char *scan;
	uint8_t v4[4];
	size_t index;
	size_t position;

	/* Starts with no groups read and no "::" seen. */
	group_count = 0U;
	compress_at = -1;
	finished = 0;
	cursor = text;

	/* An address may begin with "::" (the zero groups come first); a single leading colon is not an address. */
	if (cursor[0] == ':') {
		if (cursor[1] != ':')
			return 0;
		compress_at = 0;
		cursor += 2;
		if (*cursor == '\0')
			finished = 1;
	}

	/* Reads the groups: each token is up to the next colon, and a "::" between two tokens marks the zero groups. */
	while (!finished) {
		/* Finds the end of this token and whether it holds a dot (the last two groups written as an IPv4 address). */
		has_dot = 0;
		for (token_end = cursor;
		     *token_end != '\0' && *token_end != ':';
		     token_end++) {
			if (*token_end == '.')
				has_dot = 1;
		}

		/* A token must not be empty (this refuses ":::", a trailing single colon and the like). */
		if (token_end == cursor)
			return 0;

		/* A token with a dot is the IPv4 tail; any other is a group of hexadecimal digits. */
		if (has_dot) {
			/* An IPv4 tail is the last token and takes two groups. */
			if (*token_end != '\0')
				return 0;
			if (group_count > IPV6_GROUPS - 2U)
				return 0;
			good = ip_parse_v4(cursor, token_end, v4);
			if (!good)
				return 0;
			groups[group_count] = (unsigned)v4[0] * 256U + (unsigned)v4[1];
			group_count++;
			groups[group_count] = (unsigned)v4[2] * 256U + (unsigned)v4[3];
			group_count++;
			finished = 1;
		} else {
			/* A group is one to four hexadecimal digits. */
			if (group_count >= IPV6_GROUPS)
				return 0;
			value = 0U;
			digit_count = 0U;
			for (scan = cursor;
			     scan < token_end;
			     scan++) {
				digit = hex_value(*scan);
				if (digit < 0)
					return 0;
				digit_count++;
				if (digit_count > 4U)
					return 0;
				value = value * 16U + (unsigned)digit;
			}

			/* Records the group just read. */
			groups[group_count] = value;
			group_count++;

			/* The token is followed by the end, a "::" (once) or a single colon. */
			cursor = token_end;
			if (*cursor == '\0') {
				finished = 1;
			} else if (cursor[1] == ':') {
				if (compress_at >= 0)
					return 0;
				compress_at = (int)group_count;
				cursor += 2;
				if (*cursor == '\0')
					finished = 1;
			} else {
				cursor++;
			}
		}
	}

	/* Without "::" there are exactly eight groups; with it the zero groups stand for at least one. */
	if (compress_at < 0 && group_count != IPV6_GROUPS)
		return 0;
	if (compress_at >= 0 && group_count > IPV6_GROUPS - 1U)
		return 0;

	/* Writes the address: the groups before the "::", the zeros, the groups after it, each group big endian. */
	memset(out, 0, 16U);
	if (compress_at < 0)
		compress_at = (int)group_count;
	for (index = 0U;
	     index < (size_t)compress_at;
	     index++) {
		out[index * 2U] = (uint8_t)(groups[index] >> 8);
		out[index * 2U + 1U] = (uint8_t)(groups[index] & 0xffU);
	}

	/* Writes the groups that follow the zero run. */
	tail_count = group_count - (unsigned)compress_at;
	for (index = 0U;
	     index < tail_count;
	     index++) {
		position = (IPV6_GROUPS - tail_count + index) * 2U;
		out[position] = (uint8_t)(groups[(size_t)compress_at + index] >> 8);
		out[position + 1U] = (uint8_t)(groups[(size_t)compress_at + index] & 0xffU);
	}

	/* Succeeded: the sixteen octets are in out. */
	return 1;
}
