/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constructors of OpenSSL 3's parameters (<compat/crypto/params.h>),
 * and the comparisons of names the public functions that take parameters
 * share (cryc-api.h).
 *
 * The constructors fill a structure and copy nothing; they cannot fail
 * and queue nothing.
 */

#include <string.h>

#include "cryc-api.h"

/* Folds a lowercase ASCII letter to uppercase; any other byte is returned as it is. */
static unsigned char param_upper(unsigned char letter);

/*
 * Makes a UTF-8 string parameter named key pointing to buf: its size is
 * bsize bytes, or strlen(buf) when bsize is 0 and buf is not NULL.
 */
OSSL_PARAM
OSSL_PARAM_construct_utf8_string(
	const char *key,
	char *buf,
	size_t bsize)
{
	OSSL_PARAM param;

	/* A size of 0 with a string means the string's own length, without its NUL. */
	if (buf != NULL && bsize == 0U)
		bsize = strlen(buf);

	/* The parameter, not written yet. */
	param.key = key;
	param.data_type = OSSL_PARAM_UTF8_STRING;
	param.data = buf;
	param.data_size = bsize;
	param.return_size = OSSL_PARAM_UNMODIFIED;

	/* Succeeded: the parameter. */
	return param;
}

/*
 * Makes an octet string parameter named key: bsize bytes at buf.
 */
OSSL_PARAM
OSSL_PARAM_construct_octet_string(
	const char *key,
	void *buf,
	size_t bsize)
{
	OSSL_PARAM param;

	/* The parameter, not written yet. */
	param.key = key;
	param.data_type = OSSL_PARAM_OCTET_STRING;
	param.data = buf;
	param.data_size = bsize;
	param.return_size = OSSL_PARAM_UNMODIFIED;

	/* Succeeded: the parameter. */
	return param;
}

/*
 * Makes the parameter that ends an array.
 */
OSSL_PARAM
OSSL_PARAM_construct_end(void)
{
	OSSL_PARAM param;

	/* Every member zero: a NULL key ends the array. */
	param.key = NULL;
	param.data_type = 0U;
	param.data = NULL;
	param.data_size = 0U;
	param.return_size = 0U;

	/* Succeeded: the end marker. */
	return param;
}

/*
 * Compares a NUL-terminated name with an uppercase ASCII name, ignoring
 * the case of the letters of name.
 */
int
cryc_param_name_equal(
	const char *name,
	const char *expected)
{
	unsigned char letter;
	size_t index;

	/* Compares letter by letter until name ends. */
	for (index = 0U; name[index] != '\0'; index++) {
		letter = param_upper((unsigned char)name[index]);
		if (letter != (unsigned char)expected[index])
			return 0;
	}

	/* The names are equal when expected ends where name does. */
	if (expected[index] != '\0')
		return 0;

	/* Equal. */
	return 1;
}

/*
 * Compares a UTF-8 string parameter's value with an uppercase ASCII name,
 * ignoring the case of its letters.
 */
int
cryc_param_string_equal(
	const OSSL_PARAM *param,
	const char *expected)
{
	const unsigned char *value;
	unsigned char letter;
	size_t length;
	size_t index;

	/* Only a UTF-8 string is compared. */
	if (param->data_type != OSSL_PARAM_UTF8_STRING)
		return 0;

	/* A string with no bytes has none to read. */
	value = (const unsigned char *)param->data;
	length = param->data_size;
	if (value == NULL && length != 0U)
		return 0;

	/* A NUL counted in the size ends the string there. */
	if (length != 0U && value[length - 1U] == 0U)
		length--;

	/* Compares letter by letter; expected must not end before the value does. */
	for (index = 0U; index < length; index++) {
		if (expected[index] == '\0')
			return 0;
		letter = param_upper(value[index]);
		if (letter != (unsigned char)expected[index])
			return 0;
	}

	/* The strings are equal when expected ends where the value does. */
	if (expected[length] != '\0')
		return 0;

	/* Equal. */
	return 1;
}

/*
 * Tells whether a group name is one of P-256's names: NIST's "P-256", and
 * the SEC 2 and ANSI X9.62 names OpenSSL also takes.
 */
int
cryc_param_p256_name(
	const char *name)
{
	int equal;

	/* NIST's name. */
	equal = cryc_param_name_equal(name, "P-256");
	if (equal)
		return 1;

	/* ANSI X9.62's name, OpenSSL's own. */
	equal = cryc_param_name_equal(name, "PRIME256V1");
	if (equal)
		return 1;

	/* SEC 2's name. */
	equal = cryc_param_name_equal(name, "SECP256R1");
	if (equal)
		return 1;

	/* Not P-256. */
	return 0;
}

/*
 * Folds a lowercase ASCII letter to uppercase.
 */
static unsigned char
param_upper(
	unsigned char letter)
{
	/* Only a to z change. */
	if (letter >= (unsigned char)'a' && letter <= (unsigned char)'z')
		return (unsigned char)(letter - (unsigned char)'a' + (unsigned char)'A');

	/* Any other byte stays. */
	return letter;
}
