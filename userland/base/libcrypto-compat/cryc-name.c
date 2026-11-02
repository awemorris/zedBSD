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

#include "cryc-der.h"

#include <string.h>

/* The longest label of a host name (RFC 1035 2.3.4). */
#define NAME_LABEL_MAX		63U

/* The longest mailbox or mail constraint accepted (RFC 5321 4.5.3.1.3 allows a path of 256 octets; the bound only keeps each comparison cheap). */
#define NAME_MAIL_MAX		512U

/* The octets of an IPv4 and of an IPv6 address (RFC 5280 4.2.1.6). */
#define NAME_IPV4_SIZE		4U
#define NAME_IPV6_SIZE		16U

/* The first octet that is not ASCII. */
#define NAME_NON_ASCII		0x80U

/* The printable ASCII range without the space: the octets a name of a certificate may be made of here. */
#define NAME_GRAPHIC_FIRST	0x21U
#define NAME_GRAPHIC_LAST	0x7eU

/* The class bits and the tag number bits of an identifier octet (X.690 8.1.2). */
#define NAME_TAG_CLASS_MASK	0xc0U
#define NAME_TAG_NUMBER_MASK	0x1fU

/*
 * The GeneralName choices by tag number (RFC 5280 4.2.1.6, Appendix A.1:
 * GeneralName ::= CHOICE { otherName [0], rfc822Name [1], dNSName [2],
 * x400Address [3], directoryName [4], ediPartyName [5],
 * uniformResourceIdentifier [6], iPAddress [7], registeredID [8] }).
 */
#define NAME_FORM_OTHER		0U
#define NAME_FORM_RFC822	1U
#define NAME_FORM_DNS		2U
#define NAME_FORM_X400		3U
#define NAME_FORM_DIRECTORY	4U
#define NAME_FORM_EDI		5U
#define NAME_FORM_URI		6U
#define NAME_FORM_IP		7U
#define NAME_FORM_REGISTERED_ID	8U
#define NAME_FORM_COUNT		9U

/*
 * The forms this module evaluates against a constraint (RFC 5280
 * 4.2.1.10): rfc822Name, dNSName, directoryName and iPAddress.  For the
 * others a check fails when a constraint and a name both use the form.
 */
#define NAME_FORMS_EVALUATED	((1U << NAME_FORM_RFC822) | (1U << NAME_FORM_DNS) | (1U << NAME_FORM_DIRECTORY) | (1U << NAME_FORM_IP))

/*
 * Whether the context-specific tag of each GeneralName choice is
 * constructed, by tag number.
 *
 * Values referenced: RFC 5280 (May 2008) Appendix A.1 (the module is
 * IMPLICIT TAGS: otherName, x400Address and ediPartyName are SEQUENCEs and
 * so constructed; directoryName is an explicitly tagged Name and so
 * constructed; rfc822Name, dNSName and uniformResourceIdentifier are
 * IA5Strings, iPAddress an OCTET STRING and registeredID an OBJECT
 * IDENTIFIER, all primitive).
 */
static const uint8_t name_form_constructed[NAME_FORM_COUNT] = {
	1, 0, 0, 1, 1, 1, 0, 0, 0
};

/*
 * The contents octets of the DER encoding of the emailAddress attribute
 * type, 1.2.840.113549.1.9.1 (X.690 8.19: the first two arcs as 40 * a +
 * b, the other arcs in base 128).
 *
 * Values referenced: RFC 5280 (May 2008) Appendix A.1: pkcs-9 OBJECT
 * IDENTIFIER ::= { iso(1) member-body(2) us(840) rsadsi(113549) pkcs(1) 9
 * } and id-emailAddress ::= { pkcs-9 1 }.
 */
static const uint8_t name_oid_email_address[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x01 };

/*
 * The bounded state of one name constraints check: which forms the
 * constraining certificate's two lists mention, and how many comparisons
 * of a name against a subtree may still be made.  It lives for one call
 * of cryc_name_constraints_check.
 */
struct name_limits {
	const struct cryc_x509 *ca;
	uint32_t permitted_forms;
	uint32_t excluded_forms;
	size_t budget;
};

/*
 * A text value read as RFC 5280 7.1 compares it: the leading and trailing
 * spaces dropped (position and end bound the rest), each run of internal
 * spaces read as one space and each ASCII letter as lower case.  It reads
 * the bytes of a value of a Name and owns nothing.
 */
struct name_folder {
	const uint8_t *text;
	size_t position;
	size_t end;
};

/* Forward declarations of the static functions. */
static unsigned name_lower(unsigned character);
static int name_equal_nocase(const uint8_t *left, const uint8_t *right, size_t size);
static int name_ends_with_nocase(const uint8_t *text, size_t size, const uint8_t *suffix, size_t suffix_size);
static void name_folder_init(struct name_folder *folder, const uint8_t *text, size_t size);
static int name_folder_next(struct name_folder *folder);
static int name_text_equal(const uint8_t *left, size_t left_size, const uint8_t *right, size_t right_size);
static int name_is_ascii_text(const struct cryc_der_element *value);
static int name_value_equal(const struct cryc_der_element *left, const struct cryc_der_element *right);
static int name_read_attribute(struct cryc_der *set, struct cryc_der_element *type, struct cryc_der_element *value);
static int name_check_structure(const struct cryc_der_element *name, size_t *rdn_count);
static int name_enter(const uint8_t *data, size_t size, struct cryc_der_element *name);
static int name_open(const uint8_t *data, size_t size, struct cryc_der_element *name);
static int name_rdn_equal(const struct cryc_der_element *left, const struct cryc_der_element *right);
static int name_rdns_equal(const struct cryc_der_element *left, const struct cryc_der_element *right, int whole);
static int name_graphic(const uint8_t *text, size_t size);
static int name_dns_valid(const uint8_t *text, size_t size, int is_constraint);
static int name_dns_within(const uint8_t *name, size_t name_size, const uint8_t *constraint, size_t constraint_size);
static int name_dns_wildcard_overlaps(const uint8_t *name, size_t name_size, const uint8_t *constraint, size_t constraint_size);
static int name_label_count(const uint8_t *text, size_t size);
static int name_presented_matches(const uint8_t *presented, size_t presented_size, const char *reference, size_t reference_size);
static int name_mail_find_at(const uint8_t *text, size_t size, size_t *at);
static int name_mail_valid(const uint8_t *text, size_t size, int is_constraint);
static int name_mail_within(const uint8_t *mail, size_t mail_size, const uint8_t *constraint, size_t constraint_size);
static int name_ip_constraint_valid(const uint8_t *constraint, size_t size);
static int name_ip_within(const uint8_t *address, size_t address_size, const uint8_t *constraint, size_t constraint_size);
static int name_base_form(const struct cryc_der_element *base, unsigned *form);
static int name_constraint_valid(unsigned form, const struct cryc_der_element *base);
static int name_subtree_next(struct cryc_der *list, struct cryc_der_element *base);
static int name_scan_subtrees(const uint8_t *list, size_t size, uint32_t *forms);
static int name_within(unsigned form, const uint8_t *name, size_t name_size, const struct cryc_der_element *base, int overlap);
static int name_match_list(struct name_limits *limits, const uint8_t *list, size_t list_size, unsigned form, const uint8_t *name, size_t name_size, int overlap, int *matched);
static int name_valid_for_form(unsigned form, const uint8_t *name, size_t name_size);
static int name_check_one(struct name_limits *limits, unsigned form, const uint8_t *name, size_t name_size);
static int name_check_email_attributes(struct name_limits *limits, const struct cryc_der_element *name);
static int name_check_subject(struct name_limits *limits, const struct cryc_x509 *subject);
static int name_check_san(struct name_limits *limits, const struct cryc_x509 *subject);
static int name_san_match(const struct cryc_x509 *cert, unsigned tag, const uint8_t *wanted, size_t wanted_size, const char *reference, size_t reference_size);

/*
 * Compares two Name encodings (whole SEQUENCEs) as RFC 5280 7.1 asks for
 * path validation.
 *
 * Both encodings are checked first, so two identical malformed encodings
 * are not equal.  Two Names are equal when they have the same RDNs, in
 * the same order, each RDN with the same attributes in the same order,
 * each attribute with the same type and an equal value.  Values are equal
 * when both are PrintableString or UTF8String of ASCII after the folding
 * RFC 5280 7.1 names (spaces trimmed and folded, ASCII letters lower
 * cased); every other pair of values (a non-ASCII byte, another string
 * type, another tag) when the encodings are byte-equal.
 */
int
cryc_name_equal(
	const uint8_t *left,
	size_t left_size,
	const uint8_t *right,
	size_t right_size)
{
	struct cryc_der_element left_name;
	struct cryc_der_element right_name;
	int equal;
	int error;

	/* Reads both Names; a malformed one is never equal to anything. */
	error = name_open(left, left_size, &left_name);
	if (error != 0)
		return 0;
	error = name_open(right, right_size, &right_name);
	if (error != 0)
		return 0;

	/* Compares every RDN of the two; they must have the same number. */
	equal = name_rdns_equal(&left_name, &right_name, 1);

	/* Succeeded: reports whether the Names are equal. */
	return equal;
}

/*
 * Checks a reference host name (RFC 9525 6.1, a DNS domain name of
 * A-labels) and puts it in the form the matching uses: one trailing dot
 * removed, letters folded to lower case, written to out (room for
 * CRYC_NAME_HOST_MAX + 1 bytes, NUL-terminated).
 *
 * On an error out holds the empty string.
 */
int
cryc_name_host_normalize(
	const char *host,
	char *out)
{
	size_t length;
	size_t index;
	size_t label_length;
	unsigned character;

	/* Without a place to write there is nothing to return. */
	if (out == NULL)
		return EINVAL;
	out[0] = '\0';
	if (host == NULL)
		return EINVAL;

	/* Measures the name, reading no further than a name this long could reach. */
	length = 0;
	while (length <= CRYC_NAME_HOST_MAX + 1U && host[length] != '\0')
		length++;
	if (length > CRYC_NAME_HOST_MAX + 1U)
		return EINVAL;

	/* Drops one trailing dot (the root label). */
	if (length > 0 && host[length - 1U] == '.')
		length--;

	/* An empty name, and a name over the limit, are refused. */
	if (length == 0)
		return EINVAL;
	if (length > CRYC_NAME_HOST_MAX)
		return EINVAL;

	/* Copies each character in lower case, refusing a character a host name cannot hold and an empty or over-long label. */
	label_length = 0;
	for (index = 0;
	     index < length;
	     index++) {
		character = (unsigned char)host[index];
		if (character == '.') {
			if (label_length == 0) {
				out[0] = '\0';
				return EINVAL;
			}

			/* Starts the next label. */
			label_length = 0;
		} else {
			if (!((character >= 'a' && character <= 'z') ||
			      (character >= 'A' && character <= 'Z') ||
			      (character >= '0' && character <= '9') ||
			      character == '-' || character == '_')) {
				out[0] = '\0';
				return EINVAL;
			}

			/* Counts the character in its label and refuses a label that is too long. */
			label_length++;
			if (label_length > NAME_LABEL_MAX) {
				out[0] = '\0';
				return EINVAL;
			}
		}

		/* Stores the character in lower case. */
		out[index] = (char)name_lower(character);
	}

	/* The last label must not be empty (the name did not end with two dots). */
	if (label_length == 0) {
		out[0] = '\0';
		return EINVAL;
	}

	/* Ends the string. */
	out[length] = '\0';

	/* Succeeded: out holds the normalized name. */
	return 0;
}

/*
 * Matches a normalized reference host name against the dNSName entries of
 * cert's subjectAltName (RFC 9525 6.3 and 6.4).
 *
 * The reference is normalized again here (so a name that was not is still
 * compared in its normal form); a name that does not normalize matches
 * nothing.
 */
int
cryc_name_match_host(
	const struct cryc_x509 *cert,
	const char *host)
{
	char reference[CRYC_NAME_HOST_MAX + 1U];
	size_t reference_size;
	int matched;
	int error;

	/* Puts the reference in its normal form. */
	error = cryc_name_host_normalize(host, reference);
	if (error != 0)
		return 0;
	reference_size = strlen(reference);

	/* A certificate without a subjectAltName names no host (RFC 9525 6.3: the common name is not used). */
	if (cert == NULL)
		return 0;
	if ((cert->flags & CRYC_X509_HAS_SAN) == 0U)
		return 0;

	/* Looks for a dNSName that matches the reference. */
	matched = name_san_match(cert, (CRYC_DER_CONTEXT | NAME_FORM_DNS), NULL, 0, reference, reference_size);

	/* Succeeded: reports whether a presented identifier matched. */
	return matched;
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
	int matched;

	/* Only the two address sizes can match, and only a certificate with names. */
	if (cert == NULL || address == NULL)
		return 0;
	if (size != NAME_IPV4_SIZE && size != NAME_IPV6_SIZE)
		return 0;
	if ((cert->flags & CRYC_X509_HAS_SAN) == 0U)
		return 0;

	/* Looks for an iPAddress entry with the same octets. */
	matched = name_san_match(cert, (CRYC_DER_CONTEXT | NAME_FORM_IP), address, size, NULL, 0);

	/* Succeeded: reports whether an iPAddress entry matched. */
	return matched;
}

/*
 * Checks the names of subject against the name constraints of ca (which
 * has CRYC_X509_HAS_NAME_CONSTRAINTS), RFC 5280 4.2.1.10 and 6.1.3 (b),
 * (c), (applied to every certificate below the constraining one, 6.1.4
 * (g)).
 *
 * A name is checked only when ca constrains its form.  A name of a
 * constrained form that is malformed is EINVAL.  The constraint lists of
 * ca are checked in full first, so a malformed constraint is EINVAL
 * whatever names the subject has.  A dNSName that is a wildcard ("*.x"
 * as the whole first label) is compared as the names it could stand for:
 * it is inside a permitted subtree when "x" is, and it hits an excluded
 * subtree when "x" is inside it or when it is one label below "x".
 * is_leaf does not change the check.
 */
int
cryc_name_constraints_check(
	const struct cryc_x509 *ca,
	const struct cryc_x509 *subject,
	int is_leaf)
{
	struct name_limits limits;
	int error;

	UNUSED_PARAMETER(is_leaf);

	/* The constraining certificate must carry name constraints. */
	if (ca == NULL || subject == NULL)
		return EINVAL;
	if ((ca->flags & CRYC_X509_HAS_NAME_CONSTRAINTS) == 0U)
		return EINVAL;
	if (ca->permitted == NULL && ca->permitted_size != 0)
		return EINVAL;
	if (ca->excluded == NULL && ca->excluded_size != 0)
		return EINVAL;

	/* Reads both lists once, checking each subtree and noting which forms they use. */
	limits.ca = ca;
	limits.permitted_forms = 0;
	limits.excluded_forms = 0;
	limits.budget = CRYC_NAME_MAX_CHECKS;
	error = name_scan_subtrees(ca->permitted, ca->permitted_size, &limits.permitted_forms);
	if (error != 0)
		return error;
	error = name_scan_subtrees(ca->excluded, ca->excluded_size, &limits.excluded_forms);
	if (error != 0)
		return error;

	/* Checks the subject Name (and its emailAddress attributes when there is no subjectAltName). */
	error = name_check_subject(&limits, subject);
	if (error != 0)
		return error;

	/* Checks every name of the subjectAltName. */
	if ((subject->flags & CRYC_X509_HAS_SAN) != 0U) {
		error = name_check_san(&limits, subject);
		if (error != 0)
			return error;
	}

	/* Succeeded: every name is allowed. */
	return 0;
}

/* Folds an ASCII letter to lower case; any other value is unchanged. */
static unsigned
name_lower(
	unsigned character)
{
	/* Only the capital letters change. */
	if (character >= 'A' && character <= 'Z')
		return character + ('a' - 'A');

	/* Succeeded: not a capital letter, so unchanged. */
	return character;
}

/* Compares size bytes of two texts as ASCII ignoring case; returns 1 when equal. */
static int
name_equal_nocase(
	const uint8_t *left,
	const uint8_t *right,
	size_t size)
{
	size_t index;
	unsigned left_character;
	unsigned right_character;

	/* Compares each pair of bytes after folding. */
	for (index = 0;
	     index < size;
	     index++) {
		left_character = name_lower(left[index]);
		right_character = name_lower(right[index]);
		if (left_character != right_character)
			return 0;
	}

	/* Succeeded: every byte was equal. */
	return 1;
}

/* Tells whether text ends with suffix, ignoring ASCII case (a suffix longer than text does not). */
static int
name_ends_with_nocase(
	const uint8_t *text,
	size_t size,
	const uint8_t *suffix,
	size_t suffix_size)
{
	int equal;

	/* A suffix longer than the text cannot end it. */
	if (suffix_size > size)
		return 0;

	/* Compares the last suffix_size bytes. */
	equal = name_equal_nocase(text + (size - suffix_size), suffix, suffix_size);

	/* Succeeded: reports whether the ends are equal. */
	return equal;
}

/* Starts reading a text value in its folded form: the outer spaces are left out. */
static void
name_folder_init(
	struct name_folder *folder,
	const uint8_t *text,
	size_t size)
{
	size_t start;
	size_t end;

	/* Skips the leading spaces. */
	start = 0;
	while (start < size && text[start] == ' ')
		start++;

	/* Drops the trailing spaces. */
	end = size;
	while (end > start && text[end - 1U] == ' ')
		end--;

	/* Publishes the bounds. */
	folder->text = text;
	folder->position = start;
	folder->end = end;
}

/* Reads the next character of a folded value (a run of spaces as one space, letters in lower case); returns -1 at the end. */
static int
name_folder_next(
	struct name_folder *folder)
{
	unsigned character;
	int folded;

	/* Nothing is left. */
	if (folder->position >= folder->end)
		return -1;

	/* A run of spaces is read as one space, so the rest of the run is skipped. */
	character = folder->text[folder->position];
	if (character == ' ') {
		while (folder->position < folder->end && folder->text[folder->position] == ' ')
			folder->position++;
		return ' ';
	}

	/* Any other character is read in lower case. */
	folder->position++;
	folded = (int)name_lower(character);

	/* Succeeded: the next folded character. */
	return folded;
}

/* Compares two text values after the folding of RFC 5280 7.1; returns 1 when equal. */
static int
name_text_equal(
	const uint8_t *left,
	size_t left_size,
	const uint8_t *right,
	size_t right_size)
{
	struct name_folder left_folder;
	struct name_folder right_folder;
	int left_character;
	int right_character;
	int equal;

	/* Reads both values in their folded form. */
	name_folder_init(&left_folder, left, left_size);
	name_folder_init(&right_folder, right, right_size);

	/* Compares them character by character until both end together. */
	equal = 1;
	for (;;) {
		left_character = name_folder_next(&left_folder);
		right_character = name_folder_next(&right_folder);
		if (left_character != right_character) {
			equal = 0;
			break;
		}

		/* Stops when the left name has no more characters. */
		if (left_character < 0)
			break;
	}

	/* Succeeded: reports whether the folded values are equal. */
	return equal;
}

/* Tells whether a value is a PrintableString or UTF8String holding only ASCII, the values RFC 5280 7.1 folds. */
static int
name_is_ascii_text(
	const struct cryc_der_element *value)
{
	size_t index;

	/* Only the two string types are folded. */
	if (value->tag != CRYC_DER_PRINTABLE_STRING && value->tag != CRYC_DER_UTF8_STRING)
		return 0;

	/* A non-ASCII byte makes the value compare byte for byte. */
	for (index = 0;
	     index < value->length;
	     index++) {
		if (value->contents[index] >= NAME_NON_ASCII)
			return 0;
	}

	/* Succeeded: the value is ASCII text. */
	return 1;
}

/* Compares two attribute values: folded when both are ASCII text, else byte for byte; returns 1 when equal. */
static int
name_value_equal(
	const struct cryc_der_element *left,
	const struct cryc_der_element *right)
{
	int left_text;
	int right_text;
	int equal;
	int difference;

	/* Two ASCII text values compare after the folding. */
	left_text = name_is_ascii_text(left);
	right_text = name_is_ascii_text(right);
	if (left_text && right_text) {
		equal = name_text_equal(left->contents, left->length, right->contents, right->length);
		return equal;
	}

	/* Every other pair is equal only when the whole encodings are. */
	if (left->encoding_size != right->encoding_size)
		return 0;
	difference = memcmp(left->encoding, right->encoding, left->encoding_size);
	if (difference != 0)
		return 0;

	/* Succeeded: the encodings are byte-equal. */
	return 1;
}

/* Reads the next AttributeTypeAndValue of an RDN: its type OID and its value. */
static int
name_read_attribute(
	struct cryc_der *set,
	struct cryc_der_element *type,
	struct cryc_der_element *value)
{
	struct cryc_der_element pair;
	struct cryc_der attribute;
	int error;

	/* The attribute is a SEQUENCE of an OID and one value. */
	error = cryc_der_expect(set, CRYC_DER_SEQUENCE, &pair);
	if (error != 0)
		return EINVAL;
	error = cryc_der_enter(&pair, &attribute);
	if (error != 0)
		return EINVAL;
	error = cryc_der_expect(&attribute, CRYC_DER_OID, type);
	if (error != 0)
		return EINVAL;
	error = cryc_der_oid(type);
	if (error != 0)
		return EINVAL;
	error = cryc_der_next(&attribute, value);
	if (error != 0)
		return EINVAL;

	/* Nothing may follow the value. */
	if (attribute.size != 0)
		return EINVAL;

	/* Succeeded: type and value are views of the attribute. */
	return 0;
}

/*
 * Checks the structure of a Name (RFC 5280 4.1.2.4: a SEQUENCE of RDNs,
 * each a non-empty SET of attributes) and counts its RDNs; an empty Name
 * is well formed here.
 */
static int
name_check_structure(
	const struct cryc_der_element *name,
	size_t *rdn_count)
{
	struct cryc_der names;
	struct cryc_der set;
	struct cryc_der_element rdn;
	struct cryc_der_element type;
	struct cryc_der_element value;
	size_t count;
	int error;

	/* Enters the Name's SEQUENCE. */
	error = cryc_der_enter(name, &names);
	if (error != 0)
		return EINVAL;

	/* Checks every RDN and every attribute in it. */
	count = 0;
	while (names.size != 0) {
		error = cryc_der_expect(&names, CRYC_DER_SET, &rdn);
		if (error != 0)
			return EINVAL;
		error = cryc_der_enter(&rdn, &set);
		if (error != 0)
			return EINVAL;
		if (set.size == 0)
			return EINVAL;
		while (set.size != 0) {
			error = name_read_attribute(&set, &type, &value);
			if (error != 0)
				return EINVAL;
		}

		/* Counts the element. */
		count++;
	}

	/* Reports the count. */
	*rdn_count = count;

	/* Succeeded: the Name is well formed. */
	return 0;
}

/* Reads the one SEQUENCE a Name encoding is, with nothing after it, without checking inside it. */
static int
name_enter(
	const uint8_t *data,
	size_t size,
	struct cryc_der_element *name)
{
	struct cryc_der reader;
	int error;

	/* Bytes are needed to read anything. */
	if (data == NULL)
		return EINVAL;

	/* The encoding is exactly one SEQUENCE. */
	cryc_der_init(&reader, data, size);
	error = cryc_der_expect(&reader, CRYC_DER_SEQUENCE, name);
	if (error != 0)
		return EINVAL;
	if (reader.size != 0)
		return EINVAL;

	/* Succeeded: name is the SEQUENCE. */
	return 0;
}

/* Reads a Name encoding and checks its structure in full. */
static int
name_open(
	const uint8_t *data,
	size_t size,
	struct cryc_der_element *name)
{
	size_t count;
	int error;

	/* The one SEQUENCE. */
	error = name_enter(data, size, name);
	if (error != 0)
		return EINVAL;

	/* Its RDNs and attributes. */
	error = name_check_structure(name, &count);
	if (error != 0)
		return EINVAL;

	/* Succeeded: name is a well-formed Name. */
	return 0;
}

/* Compares two RDNs (SET elements): the same attributes in the same order, with equal types and values; returns 1 when equal. */
static int
name_rdn_equal(
	const struct cryc_der_element *left,
	const struct cryc_der_element *right)
{
	struct cryc_der left_set;
	struct cryc_der right_set;
	struct cryc_der_element left_type;
	struct cryc_der_element left_value;
	struct cryc_der_element right_type;
	struct cryc_der_element right_value;
	int left_end;
	int right_end;
	int same;
	int equal;
	int difference;
	int error;

	/* Enters both sets. */
	error = cryc_der_enter(left, &left_set);
	if (error != 0)
		return 0;
	error = cryc_der_enter(right, &right_set);
	if (error != 0)
		return 0;

	/* Compares the attributes pair by pair until both sets end together. */
	equal = 1;
	for (;;) {
		left_end = cryc_der_empty(&left_set);
		right_end = cryc_der_empty(&right_set);
		if (left_end && right_end)
			break;
		if (left_end || right_end) {
			equal = 0;
			break;
		}

		/* Reads the next attribute of each. */
		error = name_read_attribute(&left_set, &left_type, &left_value);
		if (error != 0) {
			equal = 0;
			break;
		}

		/* Reads the next attribute of the right set. */
		error = name_read_attribute(&right_set, &right_type, &right_value);
		if (error != 0) {
			equal = 0;
			break;
		}

		/* The types are equal by their OID octets. */
		if (left_type.length != right_type.length) {
			equal = 0;
			break;
		}

		/* Compares the attribute types. */
		difference = memcmp(left_type.contents, right_type.contents, left_type.length);
		if (difference != 0) {
			equal = 0;
			break;
		}

		/* The values are equal by the rule of RFC 5280 7.1. */
		same = name_value_equal(&left_value, &right_value);
		if (!same) {
			equal = 0;
			break;
		}
	}

	/* Succeeded: reports whether every attribute pair was equal. */
	return equal;
}

/*
 * Compares the RDNs of two Name SEQUENCEs: with whole set, the two must
 * have the same RDNs; without it, the RDNs of right must be the first
 * RDNs of left (right is a directoryName constraint, left the name).
 * Returns 1 when they are.
 */
static int
name_rdns_equal(
	const struct cryc_der_element *left,
	const struct cryc_der_element *right,
	int whole)
{
	struct cryc_der left_names;
	struct cryc_der right_names;
	struct cryc_der_element left_rdn;
	struct cryc_der_element right_rdn;
	int left_end;
	int right_end;
	int same;
	int equal;
	int error;

	/* Enters both Names. */
	error = cryc_der_enter(left, &left_names);
	if (error != 0)
		return 0;
	error = cryc_der_enter(right, &right_names);
	if (error != 0)
		return 0;

	/* Compares the RDNs in order. */
	equal = 1;
	for (;;) {
		left_end = cryc_der_empty(&left_names);
		right_end = cryc_der_empty(&right_names);

		/* The right Name is used up: a prefix is enough, a whole comparison also needs the left one used up. */
		if (right_end) {
			if (whole)
				equal = left_end;
			break;
		}

		/* The left Name ended first: it is shorter than the right one. */
		if (left_end) {
			equal = 0;
			break;
		}

		/* Compares the next RDN of each. */
		error = cryc_der_expect(&left_names, CRYC_DER_SET, &left_rdn);
		if (error != 0) {
			equal = 0;
			break;
		}

		/* Reads the next RDN of the right name. */
		error = cryc_der_expect(&right_names, CRYC_DER_SET, &right_rdn);
		if (error != 0) {
			equal = 0;
			break;
		}

		/* Compares the two RDNs. */
		same = name_rdn_equal(&left_rdn, &right_rdn);
		if (!same) {
			equal = 0;
			break;
		}
	}

	/* Succeeded: reports whether the RDNs are equal. */
	return equal;
}

/* Tells whether every byte is printable ASCII without the space. */
static int
name_graphic(
	const uint8_t *text,
	size_t size)
{
	size_t index;

	/* Looks for a byte outside the range. */
	for (index = 0;
	     index < size;
	     index++) {
		if (text[index] < NAME_GRAPHIC_FIRST || text[index] > NAME_GRAPHIC_LAST)
			return 0;
	}

	/* Succeeded: every byte is in the range. */
	return 1;
}

/*
 * Checks a dNSName of a certificate or of a constraint (RFC 5280
 * 4.2.1.6: the preferred name syntax of RFC 1034 3.5 and RFC 1123 2.1;
 * the leading period of a constraint is the form of 4.2.1.10 that
 * permits only names below it).  Labels are 1 to 63 octets without a
 * dot; octets are printable ASCII.  A certificate's name may start with
 * "*" as a whole label; a constraint has no wildcard and may be empty or
 * start with one period.  Returns 0 or EINVAL.
 */
static int
name_dns_valid(
	const uint8_t *text,
	size_t size,
	int is_constraint)
{
	size_t index;
	size_t start;
	size_t label_length;
	unsigned character;
	int graphic;

	/* An empty constraint is the whole name space; an empty name is malformed. */
	if (size == 0) {
		if (is_constraint)
			return 0;
		return EINVAL;
	}

	/* A name or constraint longer than a host name (and one period) is malformed, as is one with an octet outside printable ASCII. */
	if (size > CRYC_NAME_HOST_MAX + 1U)
		return EINVAL;
	graphic = name_graphic(text, size);
	if (!graphic)
		return EINVAL;

	/* A constraint may begin with one period; the labels follow it. */
	start = 0;
	if (is_constraint && text[0] == '.')
		start = 1;
	if (size - start > CRYC_NAME_HOST_MAX)
		return EINVAL;

	/* Checks each label: not empty, not over 63 octets, and a wildcard only as the whole first label of a name. */
	label_length = 0;
	for (index = start;
	     index < size;
	     index++) {
		character = text[index];
		if (character == '.') {
			if (label_length == 0)
				return EINVAL;
			label_length = 0;
		} else {
			if (character == '*') {
				if (is_constraint)
					return EINVAL;
				if (index != 0)
					return EINVAL;
				if (size > 1U && text[1] != '.')
					return EINVAL;
			}

			/* Counts the character in its label and refuses a label that is too long. */
			label_length++;
			if (label_length > NAME_LABEL_MAX)
				return EINVAL;
		}
	}

	/* The last label is not empty either (no trailing period). */
	if (label_length == 0)
		return EINVAL;

	/* Succeeded: the name or constraint is well formed. */
	return 0;
}

/*
 * Tells whether a dNSName is inside a dNSName constraint (RFC 5280
 * 4.2.1.10: the names made by adding labels on the left of the
 * constraint); a constraint with a leading period permits only names
 * below it and the empty constraint permits every name.
 */
static int
name_dns_within(
	const uint8_t *name,
	size_t name_size,
	const uint8_t *constraint,
	size_t constraint_size)
{
	int equal;

	/* The empty constraint is every name. */
	if (constraint_size == 0)
		return 1;

	/* A constraint with a leading period permits the names that end with it and have a label before it. */
	if (constraint[0] == '.') {
		if (name_size <= constraint_size)
			return 0;
		equal = name_ends_with_nocase(name, name_size, constraint, constraint_size);
		return equal;
	}

	/* The name itself is inside. */
	if (name_size == constraint_size) {
		equal = name_equal_nocase(name, constraint, constraint_size);
		return equal;
	}

	/* A longer name is inside when it ends with a dot and the constraint. */
	if (name_size < constraint_size)
		return 0;
	if (name[name_size - constraint_size - 1U] != '.')
		return 0;
	equal = name_ends_with_nocase(name, name_size, constraint, constraint_size);

	/* Succeeded: reports whether the name is below the constraint. */
	return equal;
}

/*
 * Tells whether a wildcard dNSName ("*" or "*.x") could stand for a name
 * that is inside an excluded dNSName constraint without being inside it
 * as written: the constraint is exactly one label below "x" (or one label
 * when the name is "*").
 */
static int
name_dns_wildcard_overlaps(
	const uint8_t *name,
	size_t name_size,
	const uint8_t *constraint,
	size_t constraint_size)
{
	const uint8_t *dot;
	size_t first_label;
	size_t rest_size;
	int equal;

	/* Only a name of the form "*" or "*.x" and a constraint without a leading period are compared. */
	if (constraint_size == 0 || constraint[0] == '.')
		return 0;
	if (name_size == 0 || name[0] != '*')
		return 0;

	/* The first dot of the constraint ends its first label. */
	dot = (const uint8_t *)memchr(constraint, '.', constraint_size);

	/* The name "*" stands for any single label: the constraint must be one label. */
	if (name_size == 1U) {
		if (dot == NULL)
			return 1;
		return 0;
	}

	/* "*.x" stands for a label before x: the constraint must be one label before x. */
	if (dot == NULL)
		return 0;
	first_label = (size_t)(dot - constraint);
	rest_size = constraint_size - first_label - 1U;
	if (rest_size != name_size - 2U)
		return 0;
	equal = name_equal_nocase(constraint + first_label + 1U, name + 2U, rest_size);

	/* Succeeded: reports whether the constraint is one label below x. */
	return equal;
}

/* Counts the labels of a non-empty dotted name (the dots plus one). */
static int
name_label_count(
	const uint8_t *text,
	size_t size)
{
	size_t index;
	int count;

	/* One label, and one more for each dot. */
	count = 1;
	for (index = 0;
	     index < size;
	     index++) {
		if (text[index] == '.')
			count++;
	}

	/* Succeeded: the number of labels. */
	return count;
}

/*
 * Tells whether a presented dNSName matches the reference host name
 * (RFC 9525 6.3): equal ignoring case, or a wildcard "*" as the whole
 * first label of at least three labels standing for exactly one label of
 * the reference.
 */
static int
name_presented_matches(
	const uint8_t *presented,
	size_t presented_size,
	const char *reference,
	size_t reference_size)
{
	const uint8_t *reference_bytes;
	const uint8_t *dot;
	size_t index;
	size_t label_length;
	size_t wildcards;
	size_t presented_rest;
	size_t reference_rest;
	int presented_labels;
	int reference_labels;
	int equal;

	/* An empty identifier or one longer than a host name matches nothing. */
	if (presented_size == 0 || presented_size > CRYC_NAME_HOST_MAX)
		return 0;

	/* Checks the labels: none empty, and "*" only as the whole first label (a presented identifier that breaks this is invalid and ignored). */
	reference_bytes = (const uint8_t *)reference;
	wildcards = 0;
	label_length = 0;
	for (index = 0;
	     index < presented_size;
	     index++) {
		if (presented[index] == '.') {
			if (label_length == 0)
				return 0;
			label_length = 0;
		} else {
			if (presented[index] == '*') {
				wildcards++;
				if (index != 0)
					return 0;
				if (presented_size > 1U && presented[1] != '.')
					return 0;
			}

			/* Counts the character in its label. */
			label_length++;
		}
	}

	/* Refuses a name without any label. */
	if (label_length == 0)
		return 0;

	/* Without a wildcard the whole identifier is compared with the whole reference. */
	if (wildcards == 0) {
		if (presented_size != reference_size)
			return 0;
		equal = name_equal_nocase(presented, reference_bytes, reference_size);
		return equal;
	}

	/* A wildcard identifier has at least three labels, "*" and two more. */
	presented_labels = name_label_count(presented, presented_size);
	if (presented_labels < 3)
		return 0;

	/* The reference has the same number of labels: the wildcard stands for exactly one. */
	reference_labels = name_label_count(reference_bytes, reference_size);
	if (reference_labels != presented_labels)
		return 0;

	/* The labels after the first are equal. */
	dot = (const uint8_t *)memchr(reference_bytes, '.', reference_size);
	if (dot == NULL)
		return 0;
	reference_rest = reference_size - (size_t)(dot - reference_bytes) - 1U;
	presented_rest = presented_size - 2U;
	if (reference_rest != presented_rest)
		return 0;
	equal = name_equal_nocase(dot + 1U, presented + 2U, presented_rest);

	/* Succeeded: reports whether the other labels are equal. */
	return equal;
}

/* Finds the last '@' of a mail address or constraint; returns 1 and its index when there is one. */
static int
name_mail_find_at(
	const uint8_t *text,
	size_t size,
	size_t *at)
{
	size_t index;

	/* Walks back from the end, so a quoted local part with an '@' stays whole. */
	for (index = size;
	     index > 0;
	     index--) {
		if (text[index - 1U] == '@') {
			*at = index - 1U;
			return 1;
		}
	}

	/* Succeeded: there is no '@'. */
	return 0;
}

/*
 * Checks an rfc822Name of a certificate (a mailbox: a local part, '@' and
 * a host) or of a constraint (a mailbox, a host, or a domain with a
 * leading period, RFC 5280 4.2.1.10).  Returns 0 or EINVAL.
 */
static int
name_mail_valid(
	const uint8_t *text,
	size_t size,
	int is_constraint)
{
	size_t at;
	int found;
	int graphic;

	/* Nothing to compare, or too long to be a mail address. */
	if (size == 0 || size > NAME_MAIL_MAX)
		return EINVAL;
	graphic = name_graphic(text, size);
	if (!graphic)
		return EINVAL;

	/* A mailbox has a local part and a host on either side of the last '@'. */
	found = name_mail_find_at(text, size, &at);
	if (found) {
		if (at == 0 || at + 1U >= size)
			return EINVAL;
		return 0;
	}

	/* A name of a certificate must be a mailbox. */
	if (!is_constraint)
		return EINVAL;

	/* A constraint without '@' is a host or a domain: a lone period is no domain. */
	if (size == 1U && text[0] == '.')
		return EINVAL;

	/* Succeeded: the host or domain constraint is well formed. */
	return 0;
}

/* Tells whether a mailbox is inside an rfc822Name constraint: a mailbox, a host, or a domain below it (RFC 5280 4.2.1.10). */
static int
name_mail_within(
	const uint8_t *mail,
	size_t mail_size,
	const uint8_t *constraint,
	size_t constraint_size)
{
	size_t mail_at;
	size_t constraint_at;
	const uint8_t *domain;
	size_t domain_size;
	int found;
	int equal;
	int difference;

	/* The host part of the mailbox. */
	found = name_mail_find_at(mail, mail_size, &mail_at);
	if (!found)
		return 0;
	domain = mail + mail_at + 1U;
	domain_size = mail_size - mail_at - 1U;

	/* A constraint that is a mailbox: the local part compared as written, the host ignoring case. */
	found = name_mail_find_at(constraint, constraint_size, &constraint_at);
	if (found) {
		if (mail_at != constraint_at)
			return 0;
		difference = memcmp(mail, constraint, mail_at);
		if (difference != 0)
			return 0;
		if (domain_size != constraint_size - constraint_at - 1U)
			return 0;
		equal = name_equal_nocase(domain, constraint + constraint_at + 1U, domain_size);
		return equal;
	}

	/* A constraint with a leading period: any host below the domain, not the domain itself. */
	if (constraint[0] == '.') {
		if (domain_size <= constraint_size)
			return 0;
		equal = name_ends_with_nocase(domain, domain_size, constraint, constraint_size);
		return equal;
	}

	/* A constraint that is a host: the mailboxes on exactly that host. */
	if (domain_size != constraint_size)
		return 0;
	equal = name_equal_nocase(domain, constraint, constraint_size);

	/* Succeeded: reports whether the host is the constraint. */
	return equal;
}

/*
 * Checks an iPAddress constraint (RFC 5280 4.2.1.10: an address and a
 * mask of the same size, 8 octets for IPv4 and 32 for IPv6, the mask in
 * the CIDR style of RFC 4632: ones followed by zeros).  Returns 0 or
 * EINVAL.
 */
static int
name_ip_constraint_valid(
	const uint8_t *constraint,
	size_t size)
{
	const uint8_t *mask;
	size_t half;
	size_t index;
	unsigned inverse;
	int ended;

	/* The two sizes. */
	if (size != 2U * NAME_IPV4_SIZE && size != 2U * NAME_IPV6_SIZE)
		return EINVAL;

	/* The mask is the second half. */
	half = size / 2U;
	mask = constraint + half;

	/* Walks the mask: whole bytes of ones, then at most one byte of ones then zeros, then bytes of zeros. */
	ended = 0;
	for (index = 0;
	     index < half;
	     index++) {
		if (mask[index] == 0xff) {
			if (ended)
				return EINVAL;
		} else {
			/* After the partial byte every byte is zero. */
			if (ended) {
				if (mask[index] != 0)
					return EINVAL;
				continue;
			}

			/* Refuses a mask whose set bits are not one run from the top. */
			inverse = (unsigned)(mask[index] ^ 0xff);
			if ((inverse & (inverse + 1U)) != 0U)
				return EINVAL;
			ended = 1;
		}
	}

	/* Succeeded: the mask is ones followed by zeros. */
	return 0;
}

/* Tells whether an address is inside an iPAddress constraint of the same family: the bits under the mask equal. */
static int
name_ip_within(
	const uint8_t *address,
	size_t address_size,
	const uint8_t *constraint,
	size_t constraint_size)
{
	const uint8_t *mask;
	size_t index;

	/* An address of the other family is not inside. */
	if (constraint_size != 2U * address_size)
		return 0;

	/* Compares the bits the mask selects. */
	mask = constraint + address_size;
	for (index = 0;
	     index < address_size;
	     index++) {
		if (((address[index] ^ constraint[index]) & mask[index]) != 0)
			return 0;
	}

	/* Succeeded: every selected bit is equal. */
	return 1;
}

/*
 * Reads which GeneralName choice a base or a name is: a context-specific
 * tag with a number below 9 whose constructed bit is the one RFC 5280
 * A.1 gives that choice.  Returns 0 or EINVAL.
 */
static int
name_base_form(
	const struct cryc_der_element *base,
	unsigned *form)
{
	unsigned number;
	unsigned constructed;

	/* Only the context-specific class carries a GeneralName. */
	if ((base->tag & NAME_TAG_CLASS_MASK) != CRYC_DER_CONTEXT)
		return EINVAL;

	/* A tag number of a choice that exists. */
	number = base->tag & NAME_TAG_NUMBER_MASK;
	if (number >= NAME_FORM_COUNT)
		return EINVAL;

	/* The constructed bit of the choice. */
	constructed = 0;
	if ((base->tag & CRYC_DER_CONSTRUCTED) != 0U)
		constructed = 1;
	if (constructed != name_form_constructed[number])
		return EINVAL;

	/* Succeeded: the choice. */
	*form = number;
	return 0;
}

/* Checks the base of a subtree of its form: a well-formed constraint of that form (the forms not evaluated are not looked into). */
static int
name_constraint_valid(
	unsigned form,
	const struct cryc_der_element *base)
{
	struct cryc_der_element name;
	int error;

	/* The check by form. */
	error = 0;
	switch (form) {
	case NAME_FORM_DNS:
		error = name_dns_valid(base->contents, base->length, 1);
		break;
	case NAME_FORM_RFC822:
		error = name_mail_valid(base->contents, base->length, 1);
		break;
	case NAME_FORM_IP:
		error = name_ip_constraint_valid(base->contents, base->length);
		break;
	case NAME_FORM_DIRECTORY:
		error = name_open(base->contents, base->length, &name);
		break;
	default:
		break;
	}

	/* Reports a malformed constraint. */
	if (error != 0)
		return EINVAL;

	/* Succeeded: the constraint is well formed. */
	return 0;
}

/* Reads the next GeneralSubtree of a list (SEQUENCE { base GeneralName, ... }) and gives its base. */
static int
name_subtree_next(
	struct cryc_der *list,
	struct cryc_der_element *base)
{
	struct cryc_der_element subtree;
	struct cryc_der item;
	int error;

	/* The subtree is a SEQUENCE whose first element is the base. */
	error = cryc_der_expect(list, CRYC_DER_SEQUENCE, &subtree);
	if (error != 0)
		return EINVAL;
	error = cryc_der_enter(&subtree, &item);
	if (error != 0)
		return EINVAL;
	error = cryc_der_next(&item, base);
	if (error != 0)
		return EINVAL;

	/* Succeeded: base is the subtree's base. */
	return 0;
}

/* Checks every subtree of a list and notes in forms the bit of each GeneralName choice the list uses. */
static int
name_scan_subtrees(
	const uint8_t *list,
	size_t size,
	uint32_t *forms)
{
	struct cryc_der reader;
	struct cryc_der_element base;
	unsigned form;
	int error;

	/* Reads the list. */
	cryc_der_init(&reader, list, size);
	*forms = 0;

	/* Checks each subtree's base and records its form. */
	while (reader.size != 0) {
		error = name_subtree_next(&reader, &base);
		if (error != 0)
			return EINVAL;
		error = name_base_form(&base, &form);
		if (error != 0)
			return EINVAL;
		error = name_constraint_valid(form, &base);
		if (error != 0)
			return EINVAL;
		*forms |= 1U << form;
	}

	/* Succeeded: every subtree is well formed. */
	return 0;
}

/*
 * Tells whether a name of a form is inside the subtree whose base is
 * given.  With overlap set (the subtree is an excluded one), a wildcard
 * dNSName also counts when it could stand for a name inside the subtree.
 */
static int
name_within(
	unsigned form,
	const uint8_t *name,
	size_t name_size,
	const struct cryc_der_element *base,
	int overlap)
{
	struct cryc_der_element name_element;
	struct cryc_der_element base_element;
	int within;
	int error;

	/* The comparison by form. */
	within = 0;
	switch (form) {
	case NAME_FORM_DNS:
		within = name_dns_within(name, name_size, base->contents, base->length);
		if (!within && overlap)
			within = name_dns_wildcard_overlaps(name, name_size, base->contents, base->length);
		break;
	case NAME_FORM_RFC822:
		within = name_mail_within(name, name_size, base->contents, base->length);
		break;
	case NAME_FORM_IP:
		within = name_ip_within(name, name_size, base->contents, base->length);
		break;
	case NAME_FORM_DIRECTORY:
		/* The subtree's RDNs must begin the name's RDNs. */
		error = name_enter(name, name_size, &name_element);
		if (error != 0)
			break;
		error = name_enter(base->contents, base->length, &base_element);
		if (error != 0)
			break;
		within = name_rdns_equal(&name_element, &base_element, 0);
		break;
	default:
		break;
	}

	/* Succeeded: reports whether the name is inside. */
	return within;
}

/*
 * Looks through a list of subtrees for one of the form that contains the
 * name, spending one comparison of the budget on each subtree of that
 * form it compares with; stops at the first.
 */
static int
name_match_list(
	struct name_limits *limits,
	const uint8_t *list,
	size_t list_size,
	unsigned form,
	const uint8_t *name,
	size_t name_size,
	int overlap,
	int *matched)
{
	struct cryc_der reader;
	struct cryc_der_element base;
	unsigned base_form;
	int within;
	int error;

	/* Walks the subtrees. */
	cryc_der_init(&reader, list, list_size);
	*matched = 0;
	while (reader.size != 0) {
		error = name_subtree_next(&reader, &base);
		if (error != 0)
			return EINVAL;
		error = name_base_form(&base, &base_form);
		if (error != 0)
			return EINVAL;

		/* Only the subtrees of the name's form are compared. */
		if (base_form == form) {
			/* One comparison of the budget: more than the bound is refused (a hostile certificate must not cost unbounded time). */
			if (limits->budget == 0)
				return ENOTSUP;
			limits->budget--;
			within = name_within(form, name, name_size, &base, overlap);
			if (within) {
				*matched = 1;
				return 0;
			}
		}
	}

	/* Succeeded: no subtree of the form contains the name. */
	return 0;
}

/* Checks that a name of a form that the constraints mention is well formed. */
static int
name_valid_for_form(
	unsigned form,
	const uint8_t *name,
	size_t name_size)
{
	struct cryc_der_element element;
	int error;

	/* The check by form. */
	error = 0;
	switch (form) {
	case NAME_FORM_DNS:
		error = name_dns_valid(name, name_size, 0);
		break;
	case NAME_FORM_RFC822:
		error = name_mail_valid(name, name_size, 0);
		break;
	case NAME_FORM_IP:
		if (name_size != NAME_IPV4_SIZE && name_size != NAME_IPV6_SIZE)
			error = EINVAL;
		break;
	case NAME_FORM_DIRECTORY:
		error = name_open(name, name_size, &element);
		break;
	default:
		break;
	}

	/* Reports a malformed name. */
	if (error != 0)
		return EINVAL;

	/* Succeeded: the name is well formed. */
	return 0;
}

/*
 * Checks one name of a form against the constraints: when the form is not
 * constrained the name is allowed; a constrained form this module does
 * not evaluate is ENOTSUP; otherwise the name must be inside no excluded
 * subtree and, when the permitted list has subtrees of the form, inside
 * one of them.
 */
static int
name_check_one(
	struct name_limits *limits,
	unsigned form,
	const uint8_t *name,
	size_t name_size)
{
	uint32_t bit;
	int matched;
	int error;

	/* A form no constraint mentions is not constrained. */
	bit = 1U << form;
	if (((limits->permitted_forms | limits->excluded_forms) & bit) == 0U)
		return 0;

	/* A constrained form this module cannot evaluate. */
	if ((NAME_FORMS_EVALUATED & bit) == 0U)
		return ENOTSUP;

	/* The name itself must be well formed before it is compared. */
	error = name_valid_for_form(form, name, name_size);
	if (error != 0)
		return error;

	/* A name inside an excluded subtree is refused whatever the permitted list says. */
	if ((limits->excluded_forms & bit) != 0U) {
		error = name_match_list(limits, limits->ca->excluded, limits->ca->excluded_size, form, name, name_size, 1, &matched);
		if (error != 0)
			return error;
		if (matched)
			return EACCES;
	}

	/* With permitted subtrees of the form, the name must be inside one. */
	if ((limits->permitted_forms & bit) != 0U) {
		error = name_match_list(limits, limits->ca->permitted, limits->ca->permitted_size, form, name, name_size, 0, &matched);
		if (error != 0)
			return error;
		if (!matched)
			return EACCES;
	}

	/* Succeeded: the name is allowed. */
	return 0;
}

/* Checks each emailAddress attribute of a subject Name as an rfc822Name (RFC 5280 4.2.1.10, a subject without a subjectAltName). */
static int
name_check_email_attributes(
	struct name_limits *limits,
	const struct cryc_der_element *name)
{
	struct cryc_der names;
	struct cryc_der set;
	struct cryc_der_element rdn;
	struct cryc_der_element type;
	struct cryc_der_element value;
	int is_email;
	int error;
	int difference;

	/* Only constraints on mail addresses make these attributes matter. */
	if (((limits->permitted_forms | limits->excluded_forms) & (1U << NAME_FORM_RFC822)) == 0U)
		return 0;

	/* Walks every attribute of every RDN. */
	error = cryc_der_enter(name, &names);
	if (error != 0)
		return EINVAL;
	while (names.size != 0) {
		error = cryc_der_expect(&names, CRYC_DER_SET, &rdn);
		if (error != 0)
			return EINVAL;
		error = cryc_der_enter(&rdn, &set);
		if (error != 0)
			return EINVAL;
		while (set.size != 0) {
			error = name_read_attribute(&set, &type, &value);
			if (error != 0)
				return EINVAL;

			/* Tells whether the attribute is an emailAddress. */
			is_email = 0;
			if (type.length == sizeof(name_oid_email_address)) {
				difference = memcmp(type.contents, name_oid_email_address, type.length);
				if (difference == 0)
					is_email = 1;
			}

			/* An emailAddress attribute is a mail address like any other. */
			if (is_email) {
				if (value.tag != CRYC_DER_IA5_STRING && value.tag != CRYC_DER_UTF8_STRING && value.tag != CRYC_DER_PRINTABLE_STRING)
					return EINVAL;
				error = name_check_one(limits, NAME_FORM_RFC822, value.contents, value.length);
				if (error != 0)
					return error;
			}
		}
	}

	/* Succeeded: every emailAddress attribute is allowed. */
	return 0;
}

/* Checks the subject Name of a certificate as a directoryName (not when empty, RFC 5280 6.1.3 (b)) and its emailAddress attributes when there is no subjectAltName. */
static int
name_check_subject(
	struct name_limits *limits,
	const struct cryc_x509 *subject)
{
	struct cryc_der_element name;
	size_t count;
	int error;

	/* A certificate without a subject has no name to check. */
	if (subject->subject == NULL || subject->subject_size == 0)
		return 0;

	/* Reads the subject Name; an empty one is not constrained. */
	error = name_enter(subject->subject, subject->subject_size, &name);
	if (error != 0)
		return EINVAL;
	error = name_check_structure(&name, &count);
	if (error != 0)
		return EINVAL;
	if (count == 0)
		return 0;

	/* The subject as a directoryName. */
	error = name_check_one(limits, NAME_FORM_DIRECTORY, subject->subject, subject->subject_size);
	if (error != 0)
		return error;

	/* Without a subjectAltName the mail addresses in the subject are the mail names. */
	if ((subject->flags & CRYC_X509_HAS_SAN) == 0U) {
		error = name_check_email_attributes(limits, &name);
		if (error != 0)
			return error;
	}

	/* Succeeded: the subject is allowed. */
	return 0;
}

/* Checks every GeneralName of a certificate's subjectAltName. */
static int
name_check_san(
	struct name_limits *limits,
	const struct cryc_x509 *subject)
{
	struct cryc_der reader;
	struct cryc_der_element name;
	unsigned form;
	int error;

	/* Walks the names. */
	cryc_der_init(&reader, subject->san, subject->san_size);
	while (reader.size != 0) {
		error = cryc_der_next(&reader, &name);
		if (error != 0)
			return EINVAL;
		error = name_base_form(&name, &form);
		if (error != 0)
			return EINVAL;

		/* The name's contents are the name (for a directoryName, the Name encoding). */
		error = name_check_one(limits, form, name.contents, name.length);
		if (error != 0)
			return error;
	}

	/* Succeeded: every name is allowed. */
	return 0;
}

/*
 * Looks through the subjectAltName of cert for an entry with the given
 * tag that matches: a dNSName against a reference host name, or an
 * iPAddress against wanted octets (the other argument pair is unused).
 * A subjectAltName that does not parse matches nothing.
 */
static int
name_san_match(
	const struct cryc_x509 *cert,
	unsigned tag,
	const uint8_t *wanted,
	size_t wanted_size,
	const char *reference,
	size_t reference_size)
{
	struct cryc_der reader;
	struct cryc_der_element name;
	int matched;
	int equal;
	int difference;
	int error;

	/* Walks every entry, to the end even after a match, so that a malformed one anywhere means no match. */
	cryc_der_init(&reader, cert->san, cert->san_size);
	matched = 0;
	while (reader.size != 0) {
		error = cryc_der_next(&reader, &name);
		if (error != 0)
			return 0;

		/* A dNSName is compared with the reference host name. */
		if (name.tag == tag && tag == (CRYC_DER_CONTEXT | NAME_FORM_DNS)) {
			equal = name_presented_matches(name.contents, name.length, reference, reference_size);
			if (equal)
				matched = 1;
		}

		/* An iPAddress is equal when its octets are. */
		if (name.tag == tag && tag == (CRYC_DER_CONTEXT | NAME_FORM_IP) && name.length == wanted_size) {
			difference = memcmp(name.contents, wanted, wanted_size);
			if (difference == 0)
				matched = 1;
		}
	}

	/* Succeeded: reports whether an entry matched. */
	return matched;
}
